#include "kqp_metadata_loader.h"
#include "actors/kqp_ic_gateway_actors.h"

#include <ydb/core/base/path.h>
#include <ydb/core/base/table_index.h>
#include <ydb/core/external_sources/external_source_factory.h>
#include <ydb/core/kqp/federated_query/kqp_federated_query_actors.h>
#include <ydb/core/kqp/gateway/utils/scheme_helpers.h>
#include <ydb/core/statistics/events.h>
#include <ydb/core/statistics/service/service.h>

#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>
#include <yql/essentials/utils/signals/utils.h>

#include <yql/essentials/providers/common/structured_token/yql_token_builder.h>
#include <ydb/library/yql/providers/common/token_accessor/client/factory.h>

namespace NKikimr::NKqp {

namespace {

using TNavigate = NSchemeCache::TSchemeCacheNavigate;
using TTableMetadataResult = NYql::IKikimrGateway::TTableMetadataResult;
using TLoadTableMetadataSettings = NYql::IKikimrGateway::TLoadTableMetadataSettings;
using TGenericResult = NYql::IKikimrGateway::TGenericResult;
using namespace NYql::NCommon;
using namespace NThreading;
using TIssuesIds = NYql::TIssuesIds;


struct NavigateEntryResult {
    TNavigate::TEntry Entry;
    TString Path;
    std::optional<TString> QueryName;
};

NavigateEntryResult CreateNavigateEntry(const TString& path,
    const NYql::IKikimrGateway::TLoadTableMetadataSettings& settings, TKqpTempTablesState::TConstPtr tempTablesState = nullptr) {
    TNavigate::TEntry entry;
    TString currentPath = path;
    std::optional<TString> queryName = std::nullopt;
    if (tempTablesState) {
        auto tempTablesInfoIt = tempTablesState->FindInfo(currentPath, false);
        if (tempTablesInfoIt != tempTablesState->TempTables.end()) {
            queryName = currentPath;
            currentPath = GetTempTablePath(tempTablesState->Database, tempTablesState->SessionId, tempTablesInfoIt->first);
        }
    }
    entry.Path = SplitPath(currentPath);
    entry.Operation = NSchemeCache::TSchemeCacheNavigate::EOp::OpTable;
    entry.SyncVersion = true;
    entry.ShowPrivatePath = settings.WithPrivateTables_;
    return {entry, currentPath, queryName};
}

NavigateEntryResult CreateNavigateEntry(const std::pair<TIndexId, TString>& pair,
        const NYql::IKikimrGateway::TLoadTableMetadataSettings& settings, TKqpTempTablesState::TConstPtr tempTablesState = nullptr) {
    Y_UNUSED(tempTablesState);

    TNavigate::TEntry entry;

    // TODO: Right now scheme cache use TTableId for index
    // scheme cache api should be changed to use TIndexId to navigate index
    entry.TableId = TTableId(pair.first.PathId.OwnerId, pair.first.PathId.LocalPathId, pair.first.SchemaVersion);

    entry.RequestType = NSchemeCache::TSchemeCacheNavigate::TEntry::ERequestType::ByTableId;
    entry.Operation = NSchemeCache::TSchemeCacheNavigate::EOp::OpList;
    entry.SyncVersion = true;
    entry.ShowPrivatePath = settings.WithPrivateTables_;
    return {std::move(entry), pair.second, std::nullopt};
}

std::optional<NavigateEntryResult> CreateNavigateExternalEntry(const TString& path, bool externalDataSource) {
    TNavigate::TEntry entry;
    entry.Path = SplitPath(path);
    entry.Operation = NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown;
    if (externalDataSource) {
        entry.Kind = NSchemeCache::TSchemeCacheNavigate::EKind::KindExternalDataSource;
    }
    entry.SyncVersion = true;
    return {{entry, path, std::nullopt}};
}

std::optional<NavigateEntryResult> CreateNavigateExternalEntry(const std::pair<TIndexId, TString>& pair, bool externalDataSource) {
    Y_UNUSED(pair, externalDataSource);
    return {};
}

ui64 GetExpectedVersion(const std::pair<TIndexId, TString>& pathId) {
    return pathId.first.SchemaVersion;
}

ui64 GetExpectedVersion(const TString&) {
    return 0;
}

template<typename TRequest, typename TResponse, typename TResult>
TFuture<TResult> SendActorRequest(TActorSystem* actorSystem, const TActorId& actorId, TRequest* request,
    typename TActorRequestHandler<TRequest, TResponse, TResult>::TCallbackFunc callback)
{
    auto promise = NewPromise<TResult>();
    IActor* requestHandler = new TActorRequestHandler<TRequest, TResponse, TResult>(actorId, request, promise, callback);
    actorSystem->Register(requestHandler, TMailboxType::HTSwap, actorSystem->AppData<TAppData>()->UserPoolId);
    return promise.GetFuture();
}


template<typename TIndexProto>
void IndexProtoToMetadata(const TIndexProto& indexes, NYql::TKikimrTableMetadataPtr tableMeta) {
    for (const NKikimrSchemeOp::TIndexDescription& index : indexes) {
        const auto indexState = index.GetState();

        YQL_ENSURE(indexState != NKikimrSchemeOp::EIndexState::EIndexStateInvalid,
            "Unexpected index state, probably SchemeShard/SchemeCache bug!");

        // Skip index if the state is NotReady - index just has been created but mark as not
        // ready to use.
        if (indexState == NKikimrSchemeOp::EIndexState::EIndexStateNotReady) {
            continue;
        }

        tableMeta->Indexes.emplace_back(NYql::TIndexDescription(index));
    }
}

template<typename TIndexProto>
void CheckWritesAreDisabled(const TIndexProto& indexes, NYql::TKikimrTableMetadataPtr tableMeta) {
    TStringBuilder disableReason;
    for (const NKikimrSchemeOp::TIndexDescription& index : indexes) {
        if (index.GetType() == NKikimrSchemeOp::EIndexType::EIndexTypeGlobalUnique && index.GetState() != NKikimrSchemeOp::EIndexState::EIndexStateReady) {
            if (disableReason) {
                disableReason << ", ";
            }
            disableReason << "Unique index " << index.GetName() << " is under construction";
        }
    }

    if (disableReason) {
        tableMeta->WritesToTableAreDisabled = true;
        tableMeta->DisableWritesReason = disableReason;
    }
}

TString GetTypeName(const NScheme::TTypeInfoMod& typeInfoMod) {
    return NScheme::TypeName(typeInfoMod.TypeInfo, typeInfoMod.TypeMod);
}

TTableMetadataResult GetTableMetadataResult(const NSchemeCache::TSchemeCacheNavigate::TEntry& entry,
        const TString& cluster, const TString& tableName, std::optional<TString> queryName = std::nullopt) {
    using EKind = NSchemeCache::TSchemeCacheNavigate::EKind;

    TTableMetadataResult result;
    result.SetSuccess();
    result.Metadata = new NYql::TKikimrTableMetadata(cluster, tableName);
    auto tableMeta = result.Metadata;
    tableMeta->DoesExist = true;
    tableMeta->PathId = NYql::TKikimrPathId(entry.TableId.PathId.OwnerId, entry.TableId.PathId.LocalPathId);
    tableMeta->SysView = entry.TableId.SysViewInfo;
    tableMeta->SchemaVersion = entry.TableId.SchemaVersion;

    if (!tableMeta->SysView.empty()) {
        if (entry.Kind == EKind::KindColumnTable) {
            // NOTE: OLAP sys views for stats are themselves represented by OLAP tables
            tableMeta->Kind = NYql::EKikimrTableKind::Olap;
        } else {
            tableMeta->Kind = NYql::EKikimrTableKind::SysView;
        }
    } else {
        switch (entry.Kind) {
            case EKind::KindTable:
                tableMeta->Kind = NYql::EKikimrTableKind::Datashard;
                tableMeta->TableType = NYql::ETableType::Table;
                tableMeta->StoreType = NYql::EStoreType::Row;
                break;

            case EKind::KindColumnTable:
                tableMeta->Kind = NYql::EKikimrTableKind::Olap;
                tableMeta->TableType = NYql::ETableType::Table;
                tableMeta->StoreType = NYql::EStoreType::Column;
                break;

            default:
                YQL_ENSURE(false, "Unexpected entry kind: " << (ui32)entry.Kind);
                break;
        }
    }

    tableMeta->IsIndexImplTable = (entry.TableKind != NSchemeCache::ETableKind::KindRegularTable);

    tableMeta->Attributes = entry.Attributes;

    if (queryName) {
        tableMeta->Temporary = true;
        tableMeta->QueryName = queryName;
    }

    THashMap<TString, NYql::TKikimrPathId> sequences;

    for (const auto& sequenceDesc : entry.Sequences) {
        sequences[sequenceDesc.GetName()] =
            NYql::TKikimrPathId(sequenceDesc.GetPathId().GetOwnerId(), sequenceDesc.GetPathId().GetLocalId());
    }

    std::map<ui32, TString, std::less<ui32>> keyColumns;
    std::map<ui32, TString, std::less<ui32>> columnOrder;
    for (auto& pair : entry.Columns) {
        const auto& columnDesc = pair.second;
        auto notNull = entry.NotNullColumns.contains(columnDesc.Name);
        const TString typeName = GetTypeName(NScheme::TTypeInfoMod{columnDesc.PType, columnDesc.PTypeMod});
        auto defaultKind = NKikimrKqp::TKqpColumnMetadataProto::DEFAULT_KIND_UNSPECIFIED;
        NYql::TKikimrPathId defaultFromSequencePathId = {};

        if (columnDesc.IsDefaultFromSequence()) {
            defaultKind = NKikimrKqp::TKqpColumnMetadataProto::DEFAULT_KIND_SEQUENCE;
            auto sequenceIt = sequences.find(columnDesc.DefaultFromSequence);
            YQL_ENSURE(sequenceIt != sequences.end());
            defaultFromSequencePathId = sequenceIt->second;
        } else if (columnDesc.IsDefaultFromLiteral()) {
            defaultKind = NKikimrKqp::TKqpColumnMetadataProto::DEFAULT_KIND_LITERAL;
        }

        tableMeta->Columns.emplace(
            columnDesc.Name,
            NYql::TKikimrColumnMetadata(
                columnDesc.Name, columnDesc.Id, typeName, notNull, columnDesc.PType, columnDesc.PTypeMod,
                columnDesc.DefaultFromSequence,
                defaultFromSequencePathId,
                defaultKind,
                columnDesc.DefaultFromLiteral,
                columnDesc.IsBuildInProgress
            )
        );
        if (columnDesc.KeyOrder >= 0) {
            keyColumns[columnDesc.KeyOrder] = columnDesc.Name;
        }
        columnOrder[columnDesc.Id] = columnDesc.Name;
    }

    tableMeta->KeyColumnNames.reserve(keyColumns.size());
    for (const auto& pair : keyColumns) {
        tableMeta->KeyColumnNames.push_back(pair.second);
    }

    tableMeta->ColumnOrder.reserve(columnOrder.size());
    for (const auto& [_, column] : columnOrder) {
        tableMeta->ColumnOrder.push_back(column);
    }

    if (entry.ColumnTableInfo) {
        for (const auto& column: entry.ColumnTableInfo->Description.GetSharding().GetHashSharding().GetColumns()) {
            tableMeta->PartitionedByColumns.push_back(column);
        }
    }

    IndexProtoToMetadata(entry.Indexes, tableMeta);

    // Check if we have unique indexes that are not built
    CheckWritesAreDisabled(entry.Indexes, tableMeta);

    return result;
}

TTableMetadataResult GetExternalTableMetadataResult(const NSchemeCache::TSchemeCacheNavigate::TEntry& entry,
        const TString& cluster, const TString& tableName) {
    const auto& description = entry.ExternalTableInfo->Description;
    TTableMetadataResult result;
    result.SetSuccess();
    result.Metadata = new NYql::TKikimrTableMetadata(cluster, tableName);
    auto tableMeta = result.Metadata;
    tableMeta->DoesExist = true;
    tableMeta->PathId = NYql::TKikimrPathId(description.GetPathId().GetOwnerId(), description.GetPathId().GetLocalId());
    tableMeta->SchemaVersion = description.GetVersion();
    tableMeta->Kind = NYql::EKikimrTableKind::External;
    tableMeta->TableType = NYql::ETableType::ExternalTable;

    tableMeta->Attributes = entry.Attributes;

    for (auto& columnDesc : description.GetColumns()) {
        const auto typeInfoMod = NScheme::TypeInfoModFromProtoColumnType(columnDesc.GetTypeId(),
            columnDesc.HasTypeInfo() ? &columnDesc.GetTypeInfo() : nullptr);
        const TString typeName = GetTypeName(typeInfoMod);

        tableMeta->Columns.emplace(
            columnDesc.GetName(),
            NYql::TKikimrColumnMetadata(
                columnDesc.GetName(), columnDesc.GetId(), typeName, columnDesc.GetNotNull(), typeInfoMod.TypeInfo, typeInfoMod.TypeMod,
                columnDesc.GetDefaultFromSequence()
            )
        );
    }

    tableMeta->ExternalSource.SourceType = NYql::ESourceType::ExternalTable;
    tableMeta->ExternalSource.Type = description.GetSourceType();
    tableMeta->ExternalSource.TableLocation = description.GetLocation();
    tableMeta->ExternalSource.TableContent = description.GetContent();
    tableMeta->ExternalSource.DataSourcePath = description.GetDataSourcePath();
    return result;
}

TTableMetadataResult GetExternalDataSourceMetadataResult(const NSchemeCache::TSchemeCacheNavigate::TEntry& entry,
        const TString& cluster, const TString& mainCluster, const TString& tableName) {
    const auto& description = entry.ExternalDataSourceInfo->Description;
    TTableMetadataResult result;
    result.SetSuccess();
    result.Metadata = new NYql::TKikimrTableMetadata(cluster, tableName);
    auto tableMeta = result.Metadata;
    tableMeta->DoesExist = true;
    tableMeta->PathId = NYql::TKikimrPathId(description.GetPathId().GetOwnerId(), description.GetPathId().GetLocalId());
    tableMeta->SchemaVersion = description.GetVersion();
    tableMeta->Kind = NYql::EKikimrTableKind::External;
    if (cluster == mainCluster) { // resolved external data source itself
        tableMeta->TableType = NYql::ETableType::Unknown;
    } else {
        tableMeta->TableType = NYql::ETableType::Table; // wanted to resolve table in external data source
    }

    tableMeta->Attributes = entry.Attributes;

    tableMeta->ExternalSource.SourceType = NYql::ESourceType::ExternalDataSource;
    tableMeta->ExternalSource.Type = description.GetSourceType();
    tableMeta->ExternalSource.DataSourceLocation = description.GetLocation();
    tableMeta->ExternalSource.DataSourceInstallation = description.GetInstallation();
    tableMeta->ExternalSource.DataSourceAuth = description.GetAuth();
    tableMeta->ExternalSource.Properties = description.GetProperties();
    tableMeta->ExternalSource.DataSourcePath = tableName;
    return result;
}

TTableMetadataResult GetViewMetadataResult(
    const NSchemeCache::TSchemeCacheNavigate::TEntry& schemeEntry,
    const TString& cluster,
    const TString& viewName
) {
  const auto& description = schemeEntry.ViewInfo->Description;

  TTableMetadataResult builtResult;
  builtResult.SetSuccess();

  builtResult.Metadata = new NYql::TKikimrTableMetadata(cluster, viewName);
  auto metadata = builtResult.Metadata;
  metadata->DoesExist = true;
  metadata->PathId = NYql::TKikimrPathId(description.GetPathId().GetOwnerId(),
                                         description.GetPathId().GetLocalId());
  metadata->SchemaVersion = description.GetVersion();
  metadata->Kind = NYql::EKikimrTableKind::View;
  metadata->Attributes = schemeEntry.Attributes;
  metadata->ViewPersistedData = {description.GetQueryText(), description.GetCapturedContext()};

  return builtResult;
}

TTableMetadataResult GetSysViewMetadataResult(const NSchemeCache::TSchemeCacheNavigate::TEntry& entry,
                                              const TString& cluster, const TString& sysViewName) {
    TTableMetadataResult result;
    result.SetSuccess();
    result.Metadata = new NYql::TKikimrTableMetadata(cluster, sysViewName);

    auto tableMeta = result.Metadata;
    tableMeta->DoesExist = true;
    tableMeta->PathId = NYql::TKikimrPathId(entry.TableId.PathId.OwnerId, entry.TableId.PathId.LocalPathId);
    tableMeta->SysView = entry.TableId.SysViewInfo;
    tableMeta->SchemaVersion = entry.TableId.SchemaVersion;
    tableMeta->Kind = NYql::EKikimrTableKind::SysView;

    tableMeta->Attributes = entry.Attributes;

    std::map<ui32, TString, std::less<ui32>> keyColumns;
    std::map<ui32, TString, std::less<ui32>> columnOrder;
    for (const auto& [id, column] : entry.Columns) {
        const bool notNull = entry.NotNullColumns.contains(column.Name);
        const TString typeName = GetTypeName(NScheme::TTypeInfoMod{column.PType, column.PTypeMod});

        tableMeta->Columns.emplace(
            column.Name,
            NYql::TKikimrColumnMetadata(column.Name, column.Id, typeName, notNull, column.PType, column.PTypeMod)
        );

        if (column.KeyOrder >= 0) {
            keyColumns[column.KeyOrder] = column.Name;
        }

        columnOrder[column.Id] = column.Name;
    }

    tableMeta->KeyColumnNames.reserve(keyColumns.size());
    for (const auto& columnName : std::views::values(keyColumns)) {
        tableMeta->KeyColumnNames.push_back(columnName);
    }

    tableMeta->ColumnOrder.reserve(columnOrder.size());
    for (const auto& columnName : std::views::values(columnOrder)) {
        tableMeta->ColumnOrder.push_back(columnName);
    }

    YQL_ENSURE(entry.SysViewInfo);
    auto& sysViewInfo = tableMeta->SysViewInfo.ConstructInPlace();
    sysViewInfo.SetType(entry.SysViewInfo->Description.GetType());
    *sysViewInfo.MutableSourceObject() = entry.SysViewInfo->Description.GetSourceObject();

    return result;
}

TTableMetadataResult GetLoadTableMetadataResult(const NSchemeCache::TSchemeCacheNavigate::TEntry& entry,
        const TString& cluster, const TString& mainCluster, const TString& tableName, std::optional<TString> queryName = std::nullopt) {
    using TResult = NYql::IKikimrGateway::TTableMetadataResult;
    using EStatus = NSchemeCache::TSchemeCacheNavigate::EStatus;
    using EKind = NSchemeCache::TSchemeCacheNavigate::EKind;

    auto message = ToString(entry.Status);

    switch (entry.Status) {
        case EStatus::Ok:
            break;
        case EStatus::PathErrorUnknown:
        case EStatus::RootUnknown: {
            TTableMetadataResult result;
            result.SetSuccess();
            result.Metadata = new NYql::TKikimrTableMetadata(cluster, tableName);
            return result;
        }
        case EStatus::PathNotTable:
        case EStatus::TableCreationNotComplete:
            return ResultFromError<TResult>(YqlIssue({}, TIssuesIds::KIKIMR_SCHEME_ERROR, message));
        case EStatus::LookupError:
        case EStatus::RedirectLookupError:
            return ResultFromError<TResult>(YqlIssue({}, TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE, message));
        default:
            return ResultFromError<TResult>(ToString(entry.Status));
    }

    YQL_ENSURE(IsIn({EKind::KindTable,
                     EKind::KindColumnTable,
                     EKind::KindExternalTable,
                     EKind::KindExternalDataSource,
                     EKind::KindView,
                     EKind::KindSysView}, entry.Kind));

    TTableMetadataResult result;
    switch (entry.Kind) {
        case EKind::KindExternalTable:
            result = GetExternalTableMetadataResult(entry, cluster, tableName);
            break;
        case EKind::KindExternalDataSource:
            result = GetExternalDataSourceMetadataResult(entry, cluster, mainCluster, tableName);
            break;
        case EKind::KindView:
            result = GetViewMetadataResult(entry, cluster, tableName);
            break;
        case EKind::KindSysView:
            result = GetSysViewMetadataResult(entry, cluster, tableName);
            break;
        default:
            result = GetTableMetadataResult(entry, cluster, tableName, queryName);
    }
    return result;
}

TTableMetadataResult EnrichExternalTable(const TTableMetadataResult& externalTable, const TTableMetadataResult& externalDataSource) {
    TTableMetadataResult result;
    if (!externalTable.Success()) {
        result.AddIssues(externalTable.Issues());
        return result;
    }
    if (!externalDataSource.Success()) {
        result.AddIssues(externalDataSource.Issues());
        return result;
    }

    result.SetSuccess();
    result.Metadata = externalTable.Metadata;
    auto tableMeta = result.Metadata;
    tableMeta->ExternalSource.DataSourceLocation = externalDataSource.Metadata->ExternalSource.DataSourceLocation;
    tableMeta->ExternalSource.DataSourceInstallation = externalDataSource.Metadata->ExternalSource.DataSourceInstallation;
    tableMeta->ExternalSource.DataSourceAuth = externalDataSource.Metadata->ExternalSource.DataSourceAuth;
    tableMeta->ExternalSource.ServiceAccountIdSignature = externalDataSource.Metadata->ExternalSource.ServiceAccountIdSignature;
    tableMeta->ExternalSource.AwsAccessKeyId = externalDataSource.Metadata->ExternalSource.AwsAccessKeyId;
    tableMeta->ExternalSource.AwsSecretAccessKey = externalDataSource.Metadata->ExternalSource.AwsSecretAccessKey;
    return result;
}

TString GetDebugString(const TString& id) {
    return TStringBuilder() << " Path: " << id;
}

TString GetDebugString(const std::pair<NKikimr::TIndexId, TString>& id) {
    return TStringBuilder() << " Path: " << id.second  << " TableId: " << id.first;
}

void UpdateMetadataIfSuccess(NYql::TKikimrTableMetadataPtr* implTable, TTableMetadataResult& value) {
    YQL_ENSURE(implTable);
    YQL_ENSURE(value.Success());
    while (*implTable) {
        YQL_ENSURE((*implTable)->Name < value.Metadata->Name);
        implTable = &(*implTable)->Next;
    }
    *implTable = std::move(value.Metadata);
}

void SetError(TTableMetadataResult& externalDataSourceMetadata, const TString& error) {
    externalDataSourceMetadata.AddIssues({ NYql::TIssue(error) });
    externalDataSourceMetadata.SetStatus(NYql::YqlStatusFromYdbStatus(Ydb::StatusIds::BAD_REQUEST));
}

void UpdateExternalDataSourceSecretsValue(TTableMetadataResult& externalDataSourceMetadata, const TEvDescribeSecretsResponse::TDescription& objectDescription) {
    if (objectDescription.Status != Ydb::StatusIds::SUCCESS) {
        externalDataSourceMetadata.AddIssues(objectDescription.Issues);
        externalDataSourceMetadata.SetStatus(NYql::YqlStatusFromYdbStatus(objectDescription.Status));
    } else {
        const auto& authDescription = externalDataSourceMetadata.Metadata->ExternalSource.DataSourceAuth;
        switch (authDescription.identity_case()) {
            case NKikimrSchemeOp::TAuth::kServiceAccount: {
                if (objectDescription.SecretValues.size() != 1) {
                    SetError(externalDataSourceMetadata, TStringBuilder{} << "Service account auth contains invalid count of secrets: " << objectDescription.SecretValues.size() << " instead of 1");
                    return;
                }
                externalDataSourceMetadata.Metadata->ExternalSource.ServiceAccountIdSignature = objectDescription.SecretValues[0];
                return;
            }

            case NKikimrSchemeOp::TAuth::kNone: {
                if (objectDescription.SecretValues.size() != 0) {
                    SetError(externalDataSourceMetadata, TStringBuilder{} << "None auth contains invalid count of secrets: " << objectDescription.SecretValues.size() << " instead of 0");
                    return;
                }
                return;
            }

            case NKikimrSchemeOp::TAuth::kBasic: {
                if (objectDescription.SecretValues.size() != 1) {
                    SetError(externalDataSourceMetadata, TStringBuilder{} << "Basic auth contains invalid count of secrets: " << objectDescription.SecretValues.size() << " instead of 1");
                    return;
                }
                externalDataSourceMetadata.Metadata->ExternalSource.Password = objectDescription.SecretValues[0];
                return;
            }
            case NKikimrSchemeOp::TAuth::kMdbBasic: {
                if (objectDescription.SecretValues.size() != 2) {
                    SetError(externalDataSourceMetadata, TStringBuilder{} << "Mdb basic auth contains invalid count of secrets: " << objectDescription.SecretValues.size() << " instead of 2");
                    return;
                }
                externalDataSourceMetadata.Metadata->ExternalSource.ServiceAccountIdSignature = objectDescription.SecretValues[0];
                externalDataSourceMetadata.Metadata->ExternalSource.Password = objectDescription.SecretValues[1];
                return;
            }
            case NKikimrSchemeOp::TAuth::kAws: {
                if (objectDescription.SecretValues.size() != 2) {
                    SetError(externalDataSourceMetadata, TStringBuilder{} << "Aws auth contains invalid count of secrets: " << objectDescription.SecretValues.size() << " instead of 2");
                    return;
                }
                externalDataSourceMetadata.Metadata->ExternalSource.AwsAccessKeyId = objectDescription.SecretValues[0];
                externalDataSourceMetadata.Metadata->ExternalSource.AwsSecretAccessKey = objectDescription.SecretValues[1];
                return;
            }
            case NKikimrSchemeOp::TAuth::kToken: {
                if (objectDescription.SecretValues.size() != 1) {
                    SetError(externalDataSourceMetadata, TStringBuilder{} << "Token auth contains invalid count of secrets: " << objectDescription.SecretValues.size() << " instead of 1");
                    return;
                }
                externalDataSourceMetadata.Metadata->ExternalSource.Token = objectDescription.SecretValues[0];
                return;
            }
            case NKikimrSchemeOp::TAuth::IDENTITY_NOT_SET: {
                SetError(externalDataSourceMetadata, "identity case is not specified in case of update external data source secrets");
                return;
            }
        }
    }
}

NThreading::TFuture<TEvDescribeSecretsResponse::TDescription> LoadExternalDataSourceSecretValues(const NSchemeCache::TSchemeCacheNavigate::TEntry& entry, const TIntrusiveConstPtr<NACLib::TUserToken>& userToken, TActorSystem* actorSystem) {
    const auto& authDescription = entry.ExternalDataSourceInfo->Description.GetAuth();
    return DescribeExternalDataSourceSecrets(authDescription, userToken ? userToken->GetUserSID() : "", actorSystem);
}

} // anonymous namespace

NExternalSource::TAuth MakeAuth(const NYql::TExternalSource& metadata) {
    switch (metadata.DataSourceAuth.identity_case()) {
    case NKikimrSchemeOp::TAuth::IDENTITY_NOT_SET:
    case NKikimrSchemeOp::TAuth::kNone:
        return NExternalSource::NAuth::MakeNone();
    case NKikimrSchemeOp::TAuth::kServiceAccount:
        return NExternalSource::NAuth::MakeServiceAccount(metadata.DataSourceAuth.GetServiceAccount().GetId(), metadata.ServiceAccountIdSignature);
    case NKikimrSchemeOp::TAuth::kAws:
        return NExternalSource::NAuth::MakeAws(metadata.AwsAccessKeyId, metadata.AwsSecretAccessKey, metadata.DataSourceAuth.GetAws().GetAwsRegion());
    case NKikimrSchemeOp::TAuth::kBasic:
    case NKikimrSchemeOp::TAuth::kMdbBasic:
    case NKikimrSchemeOp::TAuth::kToken:
        Y_ABORT("Unimplemented external source auth: %d", metadata.DataSourceAuth.identity_case());
        break;
    }
    Y_UNREACHABLE();
}

std::shared_ptr<NExternalSource::TMetadata> ConvertToExternalSourceMetadata(const NYql::TKikimrTableMetadata& tableMetadata) {
    auto metadata = std::make_shared<NExternalSource::TMetadata>();
    metadata->TableLocation = tableMetadata.ExternalSource.TableLocation;
    metadata->DataSourceLocation = tableMetadata.ExternalSource.DataSourceLocation;
    metadata->DataSourcePath = tableMetadata.ExternalSource.DataSourcePath;
    metadata->Type = tableMetadata.ExternalSource.Type;
    metadata->Attributes = tableMetadata.Attributes;
    metadata->Auth = MakeAuth(tableMetadata.ExternalSource);
    return metadata;
}

// dynamic metadata from IExternalSource here is propagated into TKikimrTableMetadata, which will be returned as a result of LoadTableMetadata()
bool EnrichMetadata(NYql::TKikimrTableMetadata& tableMetadata, const NExternalSource::TMetadata& dynamicMetadata) {
    ui32 id = 0;
    for (const auto& column : dynamicMetadata.Schema.column()) {
        Ydb::Type::PrimitiveTypeId typeId {};
        if (column.type().has_type_id()) {
            typeId = column.type().type_id();
        } else if (column.type().has_optional_type()) {
            typeId = column.type().optional_type().item().type_id();
        } else {
            throw yexception() << "couldn't infer type for column '" << column.name() << "': " << column.type().ShortDebugString() <<
                ", make sure that the correct input format is specified";
        }
        const auto typeInfoMod = NScheme::TypeInfoModFromProtoColumnType(typeId, nullptr);
        auto typeName = GetTypeName(typeInfoMod);

        tableMetadata.Columns.emplace(
            column.name(),
            NYql::TKikimrColumnMetadata(
                column.name(), id, typeName, !column.type().has_optional_type(), typeInfoMod.TypeInfo, typeInfoMod.TypeMod
            )
        );
        ++id;
    }
    tableMetadata.Attributes = dynamicMetadata.Attributes;
    tableMetadata.ExternalSource.TableLocation = dynamicMetadata.TableLocation;
    tableMetadata.ExternalSource.DataSourceLocation = dynamicMetadata.DataSourceLocation;
    tableMetadata.ExternalSource.DataSourcePath = dynamicMetadata.DataSourcePath;
    tableMetadata.ExternalSource.Type = dynamicMetadata.Type;
    return true;
}


TVector<NKikimrKqp::TKqpTableMetadataProto> TKqpTableMetadataLoader::GetCollectedSchemeData() {
    TVector<NKikimrKqp::TKqpTableMetadataProto> result(std::move(CollectedSchemeData));
    CollectedSchemeData = TVector<NKikimrKqp::TKqpTableMetadataProto>();
    return result;
}


void TKqpTableMetadataLoader::OnLoadedTableMetadata(TTableMetadataResult& loadTableMetadataResult) {
    if (!NeedCollectSchemeData) return;
    NKikimrKqp::TKqpTableMetadataProto proto;
    loadTableMetadataResult.Metadata->ToMessage(&proto);
    with_lock(Lock) {
        CollectedSchemeData.emplace_back(std::move(proto));
    }
}

NThreading::TFuture<NYql::IKikimrGateway::TTableMetadataResult> TKqpTableMetadataLoader::LoadSysViewRewrittenMetadata(
    const NSysView::ISystemViewResolver::TSystemViewPath& sysViewPath, const TString& cluster, const TString& table
) {
    TNavigate::TEntry entry;

    auto schema = SystemViewRewrittenResolver->GetSystemViewSchema(sysViewPath.ViewName, NSysView::ISystemViewResolver::ETarget::Domain);
    entry.Kind = TNavigate::KindTable;
    entry.Columns = std::move(schema->Columns);
    entry.TableId = TTableId(TSysTables::SysSchemeShard, 0, sysViewPath.ViewName);

    auto result = GetTableMetadataResult(entry, cluster, table);

    return MakeFuture(result);
}

NThreading::TFuture<TTableMetadataResult> TKqpTableMetadataLoader::LoadTableMetadata(const TString& cluster, const TString& table,
    const NYql::IKikimrGateway::TLoadTableMetadataSettings& settings, const TString& database,
    const TIntrusiveConstPtr<NACLib::TUserToken>& userToken)
{
    using TResult = TTableMetadataResult;

    auto ptr = weak_from_base();
    try {
        NThreading::TFuture<TTableMetadataResult> tableMetaFuture;

        NSysView::ISystemViewResolver::TSystemViewPath sysViewPath;
        if (settings.SysViewRewritten_ && SystemViewRewrittenResolver->IsSystemViewPath(SplitPath(table), sysViewPath)) {
            tableMetaFuture = LoadSysViewRewrittenMetadata(sysViewPath, cluster, table);
        } else {
            tableMetaFuture = LoadTableMetadataCache(cluster, table, settings, database, userToken);
        }
        return tableMetaFuture.Apply([ptr, database, userToken](const TFuture<TTableMetadataResult>& future) mutable {
            try {
                auto result = future.GetValue();
                if (!result.Success()) {
                    return MakeFuture(result);
                }

                if (result.Metadata->Kind == NYql::EKikimrTableKind::External) {
                    return MakeFuture(result);
                }

                auto locked = ptr.lock();
                if (!locked) {
                    result.SetStatus(TIssuesIds::KIKIMR_INDEX_METADATA_LOAD_FAILED);
                    return MakeFuture(result);
                }

                if (result.Metadata->Indexes.empty()) {
                    locked->OnLoadedTableMetadata(result);
                    return MakeFuture(result);
                } else {
                    return locked->LoadIndexMetadata(result, database, userToken);
                }
            }
            catch (yexception& e) {
                return MakeFuture(ResultFromException<TResult>(e));
            }
        });
    }
    catch (yexception& e) {
        return MakeFuture(ResultFromException<TResult>(e));
    }
}

NThreading::TFuture<TTableMetadataResult> TKqpTableMetadataLoader::LoadIndexMetadata(
    TTableMetadataResult& loadTableMetadataResult, const TString& database,
    const TIntrusiveConstPtr<NACLib::TUserToken>& userToken)
{
    auto tableMetadata = loadTableMetadataResult.Metadata;
    YQL_ENSURE(tableMetadata);

    const auto& cluster = tableMetadata->Cluster;
    const auto& tableName = tableMetadata->Name;
    const size_t indexesCount = tableMetadata->Indexes.size();

    TVector<NThreading::TFuture<TTableMetadataResult>> children;
    children.reserve(indexesCount);

    const ui64 tableOwnerId = tableMetadata->PathId.OwnerId();

    for (size_t i = 0; i < indexesCount; i++) {
        const auto& index = tableMetadata->Indexes[i];
        const auto implTablePaths = NSchemeHelpers::CreateIndexTablePath(tableName, index);
        for (const auto& implTablePath : implTablePaths) {
            if (!index.SchemaVersion) {
                LOG_DEBUG_S(*ActorSystem, NKikimrServices::KQP_GATEWAY, "Load index metadata without schema version check index: " << index.Name);
                children.push_back(
                    LoadTableMetadata(cluster, implTablePath,
                        TLoadTableMetadataSettings().WithPrivateTables(true), database, userToken)
                );
            } else {
                LOG_DEBUG_S(*ActorSystem, NKikimrServices::KQP_GATEWAY, "Load index metadata with schema version check"
                    << "index: " << index.Name
                    << "pathId: " << index.LocalPathId
                    << "ownerId: " << index.PathOwnerId
                    << "schemaVersion: " << index.SchemaVersion
                    << "tableOwnerId: " << tableOwnerId);
                auto ownerId = index.PathOwnerId ? index.PathOwnerId : tableOwnerId; //for compat with 20-2
                children.push_back(
                    LoadIndexMetadataByPathId(cluster,
                        NKikimr::TIndexId(ownerId, index.LocalPathId, index.SchemaVersion), implTablePath, database, userToken)
                );

            }
        }
    }

    auto ptr = weak_from_base();
    auto loadIndexMetadataChecker =
        [ptr, result{std::move(loadTableMetadataResult)}, children](const NThreading::TFuture<void>) mutable {
            bool loadOk = true;

            const auto indexesCount = result.Metadata->Indexes.size();
            result.Metadata->ImplTables.resize(indexesCount);
            auto it = children.begin();
            for (size_t i = 0; i < indexesCount; i++) {
                for (const auto& _ : result.Metadata->Indexes[i].GetImplTables()) {
                    YQL_ENSURE(it != children.end());
                    auto value = it++->ExtractValue();
                    result.AddIssues(value.Issues());
                    if (loadOk && (loadOk = value.Success())) {
                        UpdateMetadataIfSuccess(&result.Metadata->ImplTables[i], value);
                    }
                }
            }
            YQL_ENSURE(it == children.end());

            auto locked = ptr.lock();
            if (!loadOk || !locked) {
                result.Metadata->ImplTables.clear();
                result.SetStatus(TIssuesIds::KIKIMR_INDEX_METADATA_LOAD_FAILED);
            } else {
                locked->OnLoadedTableMetadata(result);
            }

            return MakeFuture(result);
    };

    return NThreading::WaitExceptionOrAll(children).Apply(loadIndexMetadataChecker);
}

NThreading::TFuture<TTableMetadataResult> TKqpTableMetadataLoader::LoadIndexMetadataByPathId(
    const TString& cluster, const TIndexId& indexId, const TString& tableName, const TString& database,
    const TIntrusiveConstPtr<NACLib::TUserToken>& userToken)
{
    using TResult = TTableMetadataResult;

    try {
        auto ptr = weak_from_base();
        const auto settings = TLoadTableMetadataSettings().WithPrivateTables(true);
        auto tableMetaFuture = LoadTableMetadataCache(cluster, std::make_pair(indexId, tableName), settings, database, userToken);
        return tableMetaFuture.Apply([ptr, database, userToken](const TFuture<TTableMetadataResult>& future) mutable {
            try {
                auto result = future.GetValue();
                if (!result.Success()) {
                    return MakeFuture(result);
                }

                auto locked = ptr.lock();
                if (!locked) {
                    result.SetStatus(TIssuesIds::KIKIMR_INDEX_METADATA_LOAD_FAILED);
                    return MakeFuture(result);
                }

                if (result.Metadata->Indexes.empty()) {
                    return MakeFuture(result);
                } else {
                    return locked->LoadIndexMetadata(result, database, userToken);
                }
            }
            catch (yexception& e) {
                return MakeFuture(ResultFromException<TResult>(e));
            }
        });
    }
    catch (yexception& e) {
        return MakeFuture(ResultFromException<TResult>(e));
    }
}

NSchemeCache::TSchemeCacheNavigate::TEntry& InferEntry(NKikimr::NSchemeCache::TSchemeCacheNavigate::TResultSet& resultSet) {
    using EStatus = NSchemeCache::TSchemeCacheNavigate::EStatus;
    using EKind = NSchemeCache::TSchemeCacheNavigate::EKind;

    if (resultSet.size() != 2 || resultSet[1].Status != EStatus::Ok) {
        return resultSet[0];
    }

    return IsIn({EKind::KindExternalDataSource, EKind::KindExternalTable}, resultSet[1].Kind)
        ? resultSet[1]
        : resultSet[0];
}

// The type is TString or std::pair<TIndexId, TString>
template<typename TPath>
NThreading::TFuture<TTableMetadataResult> TKqpTableMetadataLoader::LoadTableMetadataCache(
    const TString& cluster, const TPath& id,
    TLoadTableMetadataSettings settings, const TString& database,
    const TIntrusiveConstPtr<NACLib::TUserToken>& userToken)
{
    using TRequest = TEvTxProxySchemeCache::TEvNavigateKeySet;
    using TResponse = TEvTxProxySchemeCache::TEvNavigateKeySetResult;
    using TResult = TTableMetadataResult;
    using EStatus = NSchemeCache::TSchemeCacheNavigate::EStatus;
    using EKind = NSchemeCache::TSchemeCacheNavigate::EKind;

    // In the case of reading from an external data source,
    // we have a construction of the form: `/Root/external_data_source`.`/path_in_external_system` WITH (...)
    // In this syntax, information about path_in_external_system is already known and we only need information about external_data_source.
    // To do this, we go to the DefaultCluster and get information about external_data_source from scheme shard
    const bool resolveEntityInsideDataSource = (cluster != Cluster);
    TMaybe<TString> externalPath;
    TPath entityName = id;
    if constexpr (std::is_same_v<TPath, TString>) {
        if (resolveEntityInsideDataSource) {
            externalPath = entityName;
            entityName = cluster;
        }
    } else {
        Y_ENSURE(!resolveEntityInsideDataSource);
    }

    const auto externalEntryItem = CreateNavigateExternalEntry(entityName, resolveEntityInsideDataSource);
    Y_ABORT_UNLESS(!resolveEntityInsideDataSource || externalEntryItem, "External data source must be resolved using path only");
    auto resNavigate = resolveEntityInsideDataSource ? *externalEntryItem : CreateNavigateEntry(entityName,
        settings, TempTablesState);
    const auto entry = resNavigate.Entry;
    const auto queryName = resNavigate.QueryName;
    const auto externalEntry = resolveEntityInsideDataSource ? std::optional<NavigateEntryResult>{} : externalEntryItem;
    const ui64 expectedSchemaVersion = GetExpectedVersion(entityName);

    LOG_DEBUG_S(*ActorSystem, NKikimrServices::KQP_GATEWAY, "Load table metadata from cache by path, request" << GetDebugString(entityName));

    auto navigate = MakeHolder<TNavigate>();
    navigate->ResultSet.emplace_back(entry);
    if (externalEntry) {
        navigate->ResultSet.emplace_back(externalEntry->Entry);
    }
    const TString& table = resNavigate.Path;

    navigate->DatabaseName = database;
    if (userToken && !userToken->GetSerializedToken().empty()) {
        navigate->UserToken = userToken;
    }

    auto ev = MakeHolder<TRequest>(navigate.Release());

    const auto schemeCacheId = MakeSchemeCacheID();

    auto future = SendActorRequest<TRequest, TResponse, TResult>(
        ActorSystem,
        schemeCacheId,
        ev.Release(),
        [userToken, database, cluster, mainCluster = Cluster, table, settings, expectedSchemaVersion, this, queryName, externalPath]
            (TPromise<TResult> promise, TResponse&& response) mutable
        {
            try {
                YQL_ENSURE(response.Request);
                auto& navigate = *response.Request;

                YQL_ENSURE(1 <= navigate.ResultSet.size() && navigate.ResultSet.size() <= 2);
                auto& entry = InferEntry(navigate.ResultSet);

                if (entry.Status != EStatus::Ok) {
                    promise.SetValue(GetLoadTableMetadataResult(entry, cluster, mainCluster, table));
                    return;
                }

                if (!IsIn({EKind::KindExternalDataSource,
                           EKind::KindExternalTable,
                           EKind::KindView}, entry.Kind) && expectedSchemaVersion && entry.TableId.SchemaVersion) {
                    if (entry.TableId.SchemaVersion != expectedSchemaVersion) {
                        const auto message = TStringBuilder()
                            << "schema version mismatch during metadata loading for: "
                            << CombinePath(entry.Path.begin(), entry.Path.end())
                            << " expected " << expectedSchemaVersion
                            << " got " << entry.TableId.SchemaVersion;

                        promise.SetValue(ResultFromError<TResult>(YqlIssue({},
                            TIssuesIds::KIKIMR_SCHEME_MISMATCH, message)));
                        return;
                    }
                }

                const bool resolveEntityInsideDataSource = (cluster != Cluster);
                // resolveEntityInsideDataSource => entry.Kind == EKind::KindExternalDataSource
                if (resolveEntityInsideDataSource && entry.Kind != EKind::KindExternalDataSource) {
                    const auto message = TStringBuilder()
                            << "\"" << CombinePath(entry.Path.begin(), entry.Path.end())
                            << "\" is expected to be external data source";

                    promise.SetValue(ResultFromError<TResult>(YqlIssue({}, TIssuesIds::KIKIMR_BAD_REQUEST, message)));
                    return;
                }

                switch (entry.Kind) {
                    case EKind::KindExternalDataSource: {
                        auto externalDataSourceMetadata = GetLoadTableMetadataResult(entry, cluster, mainCluster, table);
                        if (!externalDataSourceMetadata.Success() || !settings.RequestAuthInfo_) {
                            promise.SetValue(externalDataSourceMetadata);
                            return;
                        }
                        if (externalPath) {
                            externalDataSourceMetadata.Metadata->ExternalSource.TableLocation = *externalPath;
                        }
                        LoadExternalDataSourceSecretValues(entry, userToken, ActorSystem)
                            .Subscribe([promise, externalDataSourceMetadata, settings, table, database, externalPath, this](const TFuture<TEvDescribeSecretsResponse::TDescription>& result) mutable
                        {
                            UpdateExternalDataSourceSecretsValue(externalDataSourceMetadata, result.GetValue());
                            if (!externalDataSourceMetadata.Success()) {
                                promise.SetValue(externalDataSourceMetadata);
                                return;
                            }
                          
                            auto loadDynamicMetadata = [promise, externalDataSourceMetadata, settings, table, database, externalPath] () mutable {
                                NExternalSource::IExternalSource::TPtr externalSource;
                                if (settings.ExternalSourceFactory) {
                                    try {
                                        externalSource = settings.ExternalSourceFactory->GetOrCreate(externalDataSourceMetadata.Metadata->ExternalSource.Type);
                                    } catch (const std::exception& exception) {
                                        TTableMetadataResult wrapper;
                                        wrapper.SetException(yexception() << "couldn't get external source with type " << externalDataSourceMetadata.Metadata->ExternalSource.Type << ", " <<  exception.what());
                                        promise.SetValue(wrapper);
                                        return;
                                    }
                                }

                                if (externalSource && externalSource->CanLoadDynamicMetadata()) {
                                    auto externalSourceMeta = ConvertToExternalSourceMetadata(*externalDataSourceMetadata.Metadata);
                                    externalSourceMeta->Attributes = settings.ReadAttributes; // attributes, collected from AST
                                    externalSource->LoadDynamicMetadata(std::move(externalSourceMeta))
                                    .Subscribe([promise, externalDataSourceMetadata](const TFuture<std::shared_ptr<NExternalSource::TMetadata>>& result) mutable {
                                            TTableMetadataResult wrapper;
                                            try {
                                                auto& dynamicMetadata = result.GetValue();
                                                if (!dynamicMetadata->Changed || EnrichMetadata(*externalDataSourceMetadata.Metadata, *dynamicMetadata)) {
                                                    wrapper.SetSuccess();
                                                    wrapper.Metadata = externalDataSourceMetadata.Metadata;
                                                } else {
                                                    wrapper.SetException(yexception() << "couldn't enrich metadata with dynamically loaded part");
                                                }
                                            } catch (const std::exception& exception) {
                                                wrapper.SetException(yexception() << "couldn't load table metadata: " << exception.what());
                                            }
                                            promise.SetValue(wrapper);
                                        });
                                } else {
                                    promise.SetValue(externalDataSourceMetadata);
                                }
                            };
                            if (externalDataSourceMetadata.Metadata->ExternalSource.Type == ToString(NYql::EDatabaseType::Ydb) && externalPath) {
                                auto& source = externalDataSourceMetadata.Metadata->ExternalSource;
                                THashMap<TString, TString> properties = {source.Properties.GetProperties().begin(), source.Properties.GetProperties().end()};
                                    
                                auto token = source.Token;
                                auto secretName = source.DataSourceAuth.GetToken().GetTokenSecretName();
                                auto structuredTokenJson = NYql::ComposeStructuredTokenJsonForTokenAuthWithSecret(secretName, token);
                                auto databaseName = properties.Value("database_name", "");
                                TString useTlsStr = properties.Value("use_tls", "false");
                                useTlsStr.to_lower();
                                bool useTls = useTlsStr == "true"sv;

                                auto path = databaseName + "/" + *externalPath;

                                GetSchemeEntryType(
                                    FederatedQuerySetup,
                                    source.DataSourceLocation,
                                    databaseName,
                                    useTls,
                                    structuredTokenJson,
                                    path)
                                    .Subscribe([externalDataSourceMetadata, f = loadDynamicMetadata] (const NThreading::TFuture<TGetSchemeEntryResult>& result) mutable {
                                        TGetSchemeEntryResult type = result.GetValue();
                                        if (type == NYdb::NScheme::ESchemeEntryType::Topic) {
                                            externalDataSourceMetadata.Metadata->ExternalSource.Type = ToString(NKikimr::NExternalSource::YdbTopicsType);
                                        }
                                        f();
                                    });
                            } else {
                                loadDynamicMetadata();
                            }
                        });
                        break;
                    }
                    case EKind::KindExternalTable: {
                        YQL_ENSURE(entry.ExternalTableInfo, "expected external table info");
                        const auto& dataSourcePath = entry.ExternalTableInfo->Description.GetDataSourcePath();
                        auto externalTableMetadata = GetLoadTableMetadataResult(entry, cluster, mainCluster, table);
                        if (!externalTableMetadata.Success()) {
                            promise.SetValue(externalTableMetadata);
                            return;
                        }
                        settings.WithExternalDatasources_ = true;
                        LoadTableMetadataCache(cluster, dataSourcePath, settings, database, userToken)
                            .Apply([promise, externalTableMetadata](const TFuture<TTableMetadataResult>& result) mutable
                        {
                            auto externalDataSourceMetadata = result.GetValue();
                            auto newMetadata = EnrichExternalTable(externalTableMetadata, externalDataSourceMetadata);
                            promise.SetValue(std::move(newMetadata));
                        });
                        break;
                    }
                    case EKind::KindIndex: {
                        Y_ENSURE(entry.ListNodeEntry, "expected children list");
                        for (const auto& child : entry.ListNodeEntry->Children) {
                            if (!table.EndsWith(child.Name)) {
                                continue;
                            }
                            TIndexId pathId = TIndexId(child.PathId, child.SchemaVersion);

                            LoadTableMetadataCache(cluster, std::make_pair(pathId, table), settings, database, userToken)
                                .Apply([promise](const TFuture<TTableMetadataResult>& result) mutable
                            {
                                promise.SetValue(result.GetValue());
                            });
                            break;
                        }
                        break;
                    }
                    default: {
                        promise.SetValue(GetLoadTableMetadataResult(entry, cluster, mainCluster, table, queryName));
                    }
                }
            }
            catch (yexception& e) {
                promise.SetValue(ResultFromException<TResult>(e));
            }
        }
    );

    // Create an apply for the future that will fetch table statistics and save it in the metadata
    // This method will only run if cost based optimization is enabled

    if (!Config || !Config->FeatureFlags.GetEnableStatistics()){
        return future;
    }

    TActorSystem* actorSystem = ActorSystem;

    return future.Apply([actorSystem,table](const TFuture<TTableMetadataResult>& f) {
        auto result = f.GetValue();
        if (!result.Success()) {
            return MakeFuture(result);
        }

        if (!result.Metadata->DoesExist){
            return MakeFuture(result);
        }

        if (result.Metadata->Kind != NYql::EKikimrTableKind::Datashard &&
            result.Metadata->Kind != NYql::EKikimrTableKind::Olap) {
            return MakeFuture(result);
        }

        NKikimr::NStat::TRequest t;
        t.PathId = NKikimr::TPathId(result.Metadata->PathId.OwnerId(), result.Metadata->PathId.TableId());

        auto event = MakeHolder<NStat::TEvStatistics::TEvGetStatistics>();
        event->StatType = NKikimr::NStat::EStatType::SIMPLE;
        event->StatRequests.push_back(t);

        auto statServiceId = NStat::MakeStatServiceID(actorSystem->NodeId);

        return SendActorRequest<NStat::TEvStatistics::TEvGetStatistics, NStat::TEvStatistics::TEvGetStatisticsResult, TResult>(
            actorSystem,
            statServiceId,
            event.Release(),
            [result](TPromise<TResult> promise, NStat::TEvStatistics::TEvGetStatisticsResult&& response){
                if (!response.StatResponses.size()){
                    return;
                }
                auto resp = response.StatResponses[0];
                auto s = resp.Simple;
                result.Metadata->RecordsCount = s.RowCount;
                result.Metadata->DataSize = s.BytesSize;
                result.Metadata->StatsLoaded = response.Success;
                promise.SetValue(result);
        });

    });
}

}  // namespace NKikimr::NKqp
