#include "update.h"
#include <ydb/core/tx/schemeshard/schemeshard_info_types.h>
#include <ydb/core/tx/schemeshard/schemeshard_utils.h>
#include <yql/essentials/minikql/mkql_type_ops.h>
#include <ydb/core/scheme/scheme_types_proto.h>
#include <ydb/core/scheme_types/scheme_type_registry.h>
#include <ydb/core/formats/arrow/serializer/abstract.h>
#include <ydb/core/formats/arrow/arrow_helpers.h>

extern "C" {
#include <yql/essentials/parser/pg_wrapper/postgresql/src/include/catalog/pg_type_d.h>
}

namespace NKikimr::NSchemeShard {

namespace {

bool IsValidColumnNameForColumnTable(const TString& name) {
    if (IsValidColumnName(name, false)) {
        return true;
    }

    if (!AppDataVerified().ColumnShardConfig.GetAllowExtraSymbolsForColumnTableColumns()) {
        return false;
    }

    return std::all_of(name.begin(), name.end(),
            [](char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '@'; });
}

}

bool TOlapColumnDiff::ParseFromRequest(const NKikimrSchemeOp::TOlapColumnDiff& columnSchema, IErrorCollector& errors) {
    Name = columnSchema.GetName();
    if (!!columnSchema.GetStorageId()) {
        StorageId = columnSchema.GetStorageId();
    }
    if (!Name) {
        errors.AddError("empty field name");
        return false;
    }
    if (columnSchema.HasDefaultValue()) {
        DefaultValue = columnSchema.GetDefaultValue();
    }
    if (columnSchema.HasDataAccessorConstructor()) {
        if (!AccessorConstructor.DeserializeFromProto(columnSchema.GetDataAccessorConstructor())) {
            errors.AddError("cannot parse accessor constructor from proto");
            return false;
        }
    }

    if (columnSchema.HasColumnFamilyName()) {
        ColumnFamilyName = columnSchema.GetColumnFamilyName();
    }
    if (columnSchema.HasSerializer()) {
        if (!Serializer.DeserializeFromProto(columnSchema.GetSerializer())) {
            errors.AddError("cannot parse serializer diff from proto");
            return false;
        }
    }
    if (!DictionaryEncoding.DeserializeFromProto(columnSchema.GetDictionaryEncoding())) {
        errors.AddError("cannot parse dictionary encoding diff from proto");
        return false;
    }
    return true;
}

bool TOlapColumnBase::ParseFromRequest(const NKikimrSchemeOp::TOlapColumnDescription& columnSchema, IErrorCollector& errors) {
    if (!columnSchema.GetName()) {
        errors.AddError("Columns cannot have an empty name");
        return false;
    }
    Name = columnSchema.GetName();
    if (!IsValidColumnNameForColumnTable(Name)) {
        errors.AddError(Sprintf("Invalid name for column '%s'", Name.data()));
        return false;
    }
    NotNullFlag = columnSchema.GetNotNull();
    TypeName = columnSchema.GetType();
    StorageId = columnSchema.GetStorageId();
    if (columnSchema.HasColumnFamilyId()) {
        ColumnFamilyId = columnSchema.GetColumnFamilyId();
    }
    if (columnSchema.HasSerializer()) {
        NArrow::NSerialization::TSerializerContainer serializer;
        if (!serializer.DeserializeFromProto(columnSchema.GetSerializer())) {
            errors.AddError("Cannot parse serializer info");
            return false;
        }
        Serializer = serializer;
    }
    if (columnSchema.HasDictionaryEncoding()) {
        auto settings = NArrow::NDictionary::TEncodingSettings::BuildFromProto(columnSchema.GetDictionaryEncoding());
        if (!settings) {
            errors.AddError("Cannot parse dictionary compression info: " + settings.GetErrorMessage());
            return false;
        }
        DictionaryEncoding = *settings;
    }

    if (columnSchema.HasTypeId()) {
        errors.AddError(TStringBuilder() << "Cannot set TypeId for column '" << Name << ", use Type");
        return false;
    }

    if (!columnSchema.HasType()) {
        errors.AddError(TStringBuilder() << "Missing Type for column '" << Name);
        return false;
    }

    TString errStr;
    Y_ABORT_UNLESS(AppData()->TypeRegistry);
    if (!GetTypeInfo(AppData()->TypeRegistry->GetType(TypeName), columnSchema.GetTypeInfo(), TypeName, Name, Type, errStr)) {
        errors.AddError(errStr);
        return false;
    }

    if (Type.GetTypeId() == NScheme::NTypeIds::Pg) {
        if (!IsAllowedPgType(NPg::PgTypeIdFromTypeDesc(Type.GetPgTypeDesc()))) {
            errors.AddError(TStringBuilder() << "Type '" << TypeName << "' specified for column '" << Name << "' is not supported");
            return false;
        }
    } else {
        if (!IsAllowedType(Type.GetTypeId())) {
            errors.AddError(TStringBuilder() << "Type '" << TypeName << "' specified for column '" << Name << "' is not supported");
            return false;
        }
    }

    auto arrowTypeResult = NArrow::GetArrowType(Type);
    const auto arrowTypeStatus = arrowTypeResult.status();
    if (!arrowTypeStatus.ok()) {
        errors.AddError(TStringBuilder() << "Column '" << Name << "': " << arrowTypeStatus.ToString());
        return false;
    }
    if (columnSchema.HasDefaultValue()) {
        auto conclusion = DefaultValue.DeserializeFromProto(columnSchema.GetDefaultValue());
        if (conclusion.IsFail()) {
            errors.AddError(conclusion.GetErrorMessage());
            return false;
        }
        if (!DefaultValue.IsCompatibleType(*arrowTypeResult)) {
            errors.AddError(
                "incompatible types for default write: def" + DefaultValue.DebugString() + ", col:" + (*arrowTypeResult)->ToString());
            return false;
        }
    }
    return true;
}

void TOlapColumnBase::ParseFromLocalDB(const NKikimrSchemeOp::TOlapColumnDescription& columnSchema) {
    Name = columnSchema.GetName();
    TypeName = columnSchema.GetType();
    StorageId = columnSchema.GetStorageId();

    if (columnSchema.HasTypeInfo()) {
        Type = NScheme::TypeInfoModFromProtoColumnType(columnSchema.GetTypeId(), &columnSchema.GetTypeInfo()).TypeInfo;
    } else {
        Type = NScheme::TypeInfoModFromProtoColumnType(columnSchema.GetTypeId(), nullptr).TypeInfo;
    }
    auto arrowType = NArrow::TStatusValidator::GetValid(NArrow::GetArrowType(Type));
    if (columnSchema.HasDefaultValue()) {
        DefaultValue.DeserializeFromProto(columnSchema.GetDefaultValue()).Validate();
        AFL_VERIFY(DefaultValue.IsCompatibleType(arrowType));
    }
    if (columnSchema.HasColumnFamilyId()) {
        ColumnFamilyId = columnSchema.GetColumnFamilyId();
    }
    if (columnSchema.HasSerializer()) {
        NArrow::NSerialization::TSerializerContainer serializer;
        AFL_VERIFY(serializer.DeserializeFromProto(columnSchema.GetSerializer()));
        Serializer = serializer;
    } else if (columnSchema.HasCompression()) {
        NArrow::NSerialization::TSerializerContainer serializer;
        serializer.DeserializeFromProto(columnSchema.GetCompression()).Validate();
        Serializer = serializer;
    }
    if (columnSchema.HasDataAccessorConstructor()) {
        NArrow::NAccessor::TConstructorContainer container;
        AFL_VERIFY(container.DeserializeFromProto(columnSchema.GetDataAccessorConstructor()));
        AccessorConstructor = container;
    }
    if (columnSchema.HasDictionaryEncoding()) {
        auto settings = NArrow::NDictionary::TEncodingSettings::BuildFromProto(columnSchema.GetDictionaryEncoding());
        Y_ABORT_UNLESS(settings.IsSuccess());
        DictionaryEncoding = *settings;
    }
    if (columnSchema.HasNotNull()) {
        NotNullFlag = columnSchema.GetNotNull();
    } else {
        NotNullFlag = false;
    }
}

void TOlapColumnBase::Serialize(NKikimrSchemeOp::TOlapColumnDescription& columnSchema) const {
    columnSchema.SetName(Name);
    columnSchema.SetType(TypeName);
    columnSchema.SetNotNull(NotNullFlag);
    columnSchema.SetStorageId(StorageId);
    *columnSchema.MutableDefaultValue() = DefaultValue.SerializeToProto();
    if (ColumnFamilyId.has_value()) {
        columnSchema.SetColumnFamilyId(ColumnFamilyId.value());
    }
    if (Serializer) {
        Serializer.SerializeToProto(*columnSchema.MutableSerializer());
    }
    if (AccessorConstructor) {
        *columnSchema.MutableDataAccessorConstructor() = AccessorConstructor.SerializeToProto();
    }
    if (DictionaryEncoding) {
        *columnSchema.MutableDictionaryEncoding() = DictionaryEncoding->SerializeToProto();
    }

    auto columnType = NScheme::ProtoColumnTypeFromTypeInfoMod(Type, "");
    columnSchema.SetTypeId(columnType.TypeId);
    if (columnType.TypeInfo) {
        *columnSchema.MutableTypeInfo() = *columnType.TypeInfo;
    }
}

bool TOlapColumnBase::ApplySerializerFromColumnFamily(const TOlapColumnFamiliesDescription& columnFamilies, IErrorCollector& errors) {
    if (GetColumnFamilyId().has_value()) {
        SetSerializer(columnFamilies.GetByIdVerified(GetColumnFamilyId().value())->GetSerializerContainer());
    } else {
        TString familyName = "default";
        const TOlapColumnFamily* columnFamily = columnFamilies.GetByName(familyName);

        if (!columnFamily) {
            errors.AddError(NKikimrScheme::StatusSchemeError,
                TStringBuilder() << "Cannot set column family `" << familyName << "` for column `" << GetName() << "`. Family not found");
            return false;
        }

        ColumnFamilyId = columnFamily->GetId();
        SetSerializer(columnFamilies.GetByIdVerified(columnFamily->GetId())->GetSerializerContainer());
    }
    return true;
}

bool TOlapColumnBase::ApplyDiff(
    const TOlapColumnDiff& diffColumn, const TOlapColumnFamiliesDescription& columnFamilies, IErrorCollector& errors) {
    Y_ABORT_UNLESS(GetName() == diffColumn.GetName());
    if (diffColumn.GetDefaultValue()) {
        auto conclusion = DefaultValue.ParseFromString(*diffColumn.GetDefaultValue(), Type);
        if (conclusion.IsFail()) {
            errors.AddError(conclusion.GetErrorMessage());
            return false;
        }
    }
    if (!!diffColumn.GetAccessorConstructor()) {
        auto conclusion = diffColumn.GetAccessorConstructor()->BuildConstructor();
        if (conclusion.IsFail()) {
            errors.AddError(conclusion.GetErrorMessage());
            return false;
        }
        AccessorConstructor = conclusion.DetachResult();
    }
    if (diffColumn.GetStorageId()) {
        StorageId = *diffColumn.GetStorageId();
    }
    if (diffColumn.GetColumnFamilyName().has_value()) {
        TString columnFamilyName = diffColumn.GetColumnFamilyName().value();
        const TOlapColumnFamily* columnFamily = columnFamilies.GetByName(columnFamilyName);
        if (!columnFamily) {
            errors.AddError(NKikimrScheme::StatusSchemeError, TStringBuilder() << "Cannot alter column family `" << columnFamilyName
                                                                               << "` for column `" << GetName() << "`. Family not found");
            return false;
        }
        ColumnFamilyId = columnFamily->GetId();
    }

    if (diffColumn.GetSerializer()) {
        Serializer = diffColumn.GetSerializer();
    } else {
        if (!columnFamilies.GetColumnFamilies().empty() && !ApplySerializerFromColumnFamily(columnFamilies, errors)) {
            return false;
        }
    }
    {
        auto result = diffColumn.GetDictionaryEncoding().Apply(DictionaryEncoding);
        if (!result) {
            errors.AddError("Cannot merge dictionary encoding info: " + result.GetErrorMessage());
            return false;
        }
    }
    return true;
}

bool TOlapColumnBase::IsAllowedType(ui32 typeId) {
    if (!NScheme::NTypeIds::IsYqlType(typeId)) {
        return false;
    }

    switch (typeId) {
        case NYql::NProto::Bool:
        case NYql::NProto::Interval:
        case NYql::NProto::DyNumber:
            return false;
        default:
            break;
    }
    return true;
}

bool TOlapColumnBase::IsAllowedPgType(ui32 pgTypeId) {
    switch (pgTypeId) {
        case INT2OID:
        case INT4OID:
        case INT8OID:
        case FLOAT4OID:
        case FLOAT8OID:
            return true;
        default:
            break;
    }
    return false;
}

bool TOlapColumnBase::IsAllowedPkType(ui32 typeId) {
    switch (typeId) {
        case NYql::NProto::Int8:
        case NYql::NProto::Uint8:  // Byte
        case NYql::NProto::Int16:
        case NYql::NProto::Uint16:
        case NYql::NProto::Int32:
        case NYql::NProto::Uint32:
        case NYql::NProto::Int64:
        case NYql::NProto::Uint64:
        case NYql::NProto::String:
        case NYql::NProto::Utf8:
        case NYql::NProto::Date:
        case NYql::NProto::Datetime:
        case NYql::NProto::Timestamp:
        case NYql::NProto::Date32:
        case NYql::NProto::Datetime64:
        case NYql::NProto::Timestamp64:
        case NYql::NProto::Interval64:
        case NYql::NProto::Decimal:
            return true;
        default:
            return false;
    }
}

bool TOlapColumnAdd::ParseFromRequest(const NKikimrSchemeOp::TOlapColumnDescription& columnSchema, IErrorCollector& errors) {
    if (columnSchema.HasColumnFamilyName()) {
        ColumnFamilyName = columnSchema.GetColumnFamilyName();
    }
    return TBase::ParseFromRequest(columnSchema, errors);
}

void TOlapColumnAdd::ParseFromLocalDB(const NKikimrSchemeOp::TOlapColumnDescription& columnSchema) {
    if (columnSchema.HasColumnFamilyName()) {
        ColumnFamilyName = columnSchema.GetColumnFamilyName();
    }
    TBase::ParseFromLocalDB(columnSchema);
}

    bool TOlapColumnsUpdate::Parse(const NKikimrSchemeOp::TAlterColumnTableSchema& alterRequest, IErrorCollector& errors) {
        for (const auto& column : alterRequest.GetDropColumns()) {
            if (!DropColumns.emplace(column.GetName()).second) {
                errors.AddError(NKikimrScheme::StatusInvalidParameter, "Duplicated column for drop");
                return false;
            }
        }
        TSet<TString> addColumnNames;
        for (auto& columnSchema : alterRequest.GetAddColumns()) {
            TOlapColumnAdd column({});
            if (!column.ParseFromRequest(columnSchema, errors)) {
                return false;
            }
            if (addColumnNames.contains(column.GetName())) {
                errors.AddError(NKikimrScheme::StatusAlreadyExists, TStringBuilder() << "column '" << column.GetName() << "' duplication for add");
                return false;
            }
            addColumnNames.emplace(column.GetName());
            AddColumns.emplace_back(std::move(column));
        }

        TSet<TString> alterColumnNames;
        for (auto& columnSchemaDiff : alterRequest.GetAlterColumns()) {
            TOlapColumnDiff columnDiff;
            if (!columnDiff.ParseFromRequest(columnSchemaDiff, errors)) {
                return false;
            }
            if (addColumnNames.contains(columnDiff.GetName())) {
                errors.AddError(NKikimrScheme::StatusSchemeError, TStringBuilder() << "column '" << columnDiff.GetName() << "' have to be either add or update");
                return false;
            }
            if (alterColumnNames.contains(columnDiff.GetName())) {
                errors.AddError(NKikimrScheme::StatusSchemeError, TStringBuilder() << "column '" << columnDiff.GetName() << "' duplication for update");
                return false;
            }
            alterColumnNames.emplace(columnDiff.GetName());
            AlterColumns.emplace_back(std::move(columnDiff));
        }
        return true;
    }

    bool TOlapColumnsUpdate::Parse(const NKikimrSchemeOp::TColumnTableSchema& tableSchema, IErrorCollector& errors, bool allowNullKeys) {
        TMap<TString, ui32> keyColumnNames;
        for (auto&& pkKey : tableSchema.GetKeyColumnNames()) {
            if (!keyColumnNames.emplace(pkKey, keyColumnNames.size()).second) {
                errors.AddError(NKikimrScheme::StatusSchemeError, TStringBuilder() << "Duplicate key column '" << pkKey << "'");
                return false;
            }
        }

        TSet<TString> columnNames;
        for (auto& columnSchema : tableSchema.GetColumns()) {
            std::optional<ui32> keyOrder;
            {
                auto it = keyColumnNames.find(columnSchema.GetName());
                if (it != keyColumnNames.end()) {
                    keyOrder = it->second;
                }
            }

            TOlapColumnAdd column(keyOrder);
            if (!column.ParseFromRequest(columnSchema, errors)) {
                return false;
            }
            if (column.IsKeyColumn()) {
                if (!TOlapColumnAdd::IsAllowedPkType(column.GetType().GetTypeId())) {
                    errors.AddError(NKikimrScheme::StatusSchemeError, TStringBuilder()
                        << "Type '" << column.GetTypeName() << "' specified for column '" << column.GetName()
                        << "' is not supported as primary key");
                    return false;
                }
            }
            if (columnNames.contains(column.GetName())) {
                errors.AddError(NKikimrScheme::StatusMultipleModifications, TStringBuilder() << "Duplicate column '" << column.GetName() << "'");
                return false;
            }
            if (!allowNullKeys) {
                if (keyColumnNames.contains(column.GetName()) && !column.IsNotNull()) {
                    errors.AddError(NKikimrScheme::StatusSchemeError, TStringBuilder() << "Nullable key column '" << column.GetName() << "'");
                    return false;
                }
            }
            columnNames.emplace(column.GetName());
            AddColumns.emplace_back(std::move(column));
        }

        return true;
    }

}
