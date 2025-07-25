#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/core/ytree/convert.h>
#include <yt/yt/core/ytree/ephemeral_node_factory.h>
#include <yt/yt/core/ytree/fluent.h>
#include <yt/yt/core/ytree/tree_builder.h>
#include <yt/yt/core/ytree/yson_struct.h>

#include <yt/yt/core/ytree/unittests/proto/test.pb.h>

namespace NYT::NYTree {
namespace {

using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETestEnum,
    (Value0)
    (Value1)
);

////////////////////////////////////////////////////////////////////////////////

struct TTestSubStruct
    : public TYsonStruct
{
    ui32 MyUint;

    REGISTER_YSON_STRUCT(TTestSubStruct);

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("my_uint", &TThis::MyUint)
            .Default();
    }
};

using TTestSubStructPtr = TIntrusivePtr<TTestSubStruct>;

struct TTestSubStructLite
    : public TYsonStructLite
{
    i32 MyInt;

    REGISTER_YSON_STRUCT_LITE(TTestSubStructLite);

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("my_int", &TThis::MyInt)
            .Default();
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TTestYsonStruct
    : public TYsonStruct
{
    TString MyString;
    std::string MyStdString;
    TTestSubStructPtr Sub;
    std::vector<TTestSubStructLite> SubList;
    std::vector<TString> MyStringList;
    std::unordered_map<TString, int> IntMap;
    std::optional<i64> NullableInt;
    ETestEnum MyEnum;
    unsigned int MyUint;
    bool MyBool;
    char MyChar;
    i8 MyByte;
    ui8 MyUbyte;
    i16 MyShort;
    ui16 MyUshort;

    REGISTER_YSON_STRUCT(TTestYsonStruct);

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("my_string", &TThis::MyString);
        registrar.Parameter("my_std_string", &TThis::MyStdString);
        registrar.Parameter("sub", &TThis::Sub)
            .DefaultNew();
        registrar.Parameter("sub_list", &TThis::SubList)
            .Default();
        registrar.Parameter("int_map", &TThis::IntMap)
            .Default();
        registrar.Parameter("my_string_list", &TThis::MyStringList)
            .Default();
        registrar.Parameter("nullable_int", &TThis::NullableInt)
            .Default();
        registrar.Parameter("my_uint", &TThis::MyUint)
            .Default();
        registrar.Parameter("my_bool", &TThis::MyBool)
            .Default();
        registrar.Parameter("my_char", &TThis::MyChar)
            .Default();
        registrar.Parameter("my_byte", &TThis::MyByte)
            .Default();
        registrar.Parameter("my_ubyte", &TThis::MyUbyte)
            .Default();
        registrar.Parameter("my_short", &TThis::MyShort)
            .Default();
        registrar.Parameter("my_ushort", &TThis::MyUshort)
            .Default();
        registrar.Parameter("my_enum", &TThis::MyEnum)
            .Default(ETestEnum::Value1);
    }
};

using TTestYsonStructPtr = TIntrusivePtr<TTestYsonStruct>;

////////////////////////////////////////////////////////////////////////////////

struct TTestStructWithProtobuf
    : public TYsonStruct
{
    std::optional<NProto::TTestMessage> MyMessage;

    REGISTER_YSON_STRUCT(TTestStructWithProtobuf);

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("my_message", &TThis::MyMessage)
            .Optional();
    }
};

struct TTestStructWithProtobufAsYson
    : public TYsonStruct
{
    std::optional<TProtoSerializedAsYson<NProto::TTestMessage>> MyMessage;

    REGISTER_YSON_STRUCT(TTestStructWithProtobufAsYson);

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("my_message", &TThis::MyMessage)
            .Optional();
    }
};

struct TTestStructWithProtobufAsString
    : public TYsonStruct
{
    std::optional<TProtoSerializedAsString<NProto::TTestMessage>> MyMessage;

    REGISTER_YSON_STRUCT(TTestStructWithProtobufAsString);

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("my_message", &TThis::MyMessage)
            .Optional();
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TCustomType
{
    int Value;
};

void Serialize(TCustomType value, NYT::NYson::IYsonConsumer* consumer)
{
    consumer->OnInt64Scalar(value.Value);
}

void Deserialize(TCustomType& value, NYT::NYTree::INodePtr node)
{
    value.Value = node->GetValue<int>();
}

struct TTestStructWithCustomType
    : public TYsonStruct
{
    TCustomType MyType;

    REGISTER_YSON_STRUCT(TTestStructWithCustomType);

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("my_type", &TThis::MyType)
            .Optional();
    }
};

struct TTestStructWithUndefinedType
    : public TYsonStruct
{
    NYT::NYTree::TYsonStructPtr UndefinedTypeField;

    REGISTER_YSON_STRUCT(TTestStructWithUndefinedType);

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("undefined_type_field", &TThis::UndefinedTypeField)
            .Optional();
    }
};

struct TTestStructWithTuples
    : public TYsonStruct
{
    std::tuple<TString, ui64, double> Tuple;
    std::pair<TString, TString> Pair;

    REGISTER_YSON_STRUCT(TTestStructWithTuples)

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("tuple", &TThis::Tuple);
        registrar.Parameter("pair", &TThis::Pair);
    }
};

struct TTestStructWithArray
    : public TYsonStruct
{
    std::array<TString, 3> StringArray;

    REGISTER_YSON_STRUCT(TTestStructWithArray)

    static void Register(TRegistrar registrar)
    {
        registrar.Parameter("string_array", &TThis::StringArray);
    }
};

////////////////////////////////////////////////////////////////////////////////

void CheckSchema(const TYsonStructPtr& ysonStruct, TStringBuf expected)
{
    auto* factory = GetEphemeralNodeFactory();
    auto builder = CreateBuilderFromFactory(factory);
    builder->BeginTree();
    ysonStruct->WriteSchema(builder.get());
    auto actualNode = builder->EndTree();
    auto expectedNode = ConvertToNode(TYsonStringBuf(expected), factory);
    EXPECT_TRUE(AreNodesEqual(expectedNode, actualNode))
        << "Expected: " << ConvertToYsonString(expectedNode, EYsonFormat::Text, 4).AsStringBuf() << "\n\n"
        << "Actual: " << ConvertToYsonString(actualNode, EYsonFormat::Text, 4).AsStringBuf() << "\n\n";
}

////////////////////////////////////////////////////////////////////////////////

TEST(TYsonStructSchemaTest, TestYsonStruct)
{
    CheckSchema(
        New<TTestYsonStruct>(),
        R"({type_name="struct";
            members=[
                {name="my_enum";type={type_name="enum";enum_name="ETestEnum";values=["value0";"value1";]}};
                {name="my_char";type="int8";};
                {name="my_ushort";type="uint16";};
                {name="my_std_string";type="string";required=%true;};
                {name="nullable_int";type={type_name="optional";item="int64";}};
                {
                    name="sub_list";
                    type={type_name="list";item={type_name="struct";members=[{name="my_int";type="int32";}]}}
                };
                {name="my_byte";type="int8";};
                {name="my_string";type="string";required=%true;};
                {name="int_map";type={type_name="dict";key="string";value="int32";}};
                {
                    name="sub";
                    type={type_name="optional";item={type_name="struct";members=[{name="my_uint";type="uint32";}]}};
                };
                {name="my_uint";type="uint32";};
                {name="my_ubyte";type="uint8";};
                {name="my_bool";type="bool";};
                {name="my_short";type="int16";};
                {name="my_string_list";type={type_name="list";item="string";}};
            ];})");
}

TEST(TYsonStructSchemaTest, TestSchemaForProtobuf)
{
    CheckSchema(
        New<TTestStructWithProtobuf>(),
        R"({
            type_name="struct";
            members=[
                {
                    name="my_message";
                    type={
                        type_name="optional";
                        item={
                            type_name="struct";
                            members=[
                                {name="int32_field";type="int32";};
                                {name="string_field";"type"="utf8";};
                            ];
                        };
                    };
                };
            ];})");
}

TEST(TYsonStructSchemaTest, TestSchemaForProtobufAsYson)
{
    CheckSchema(
        New<TTestStructWithProtobufAsYson>(),
        R"({
            type_name="struct";
            members=[
                {
                    name="my_message";
                    type={
                        type_name="optional";
                        item={
                            type_name="struct";
                            members=[
                                {name="int32_field";type="int32";};
                                {name="string_field";"type"="utf8";};
                            ];
                        };
                    };
                };
            ];})");
}

TEST(TYsonStructSchemaTest, TestSchemaForProtobufAsString)
{
    CheckSchema(
        New<TTestStructWithProtobufAsString>(),
        R"({
            type_name="struct";
            members=[
                {
                    name="my_message";
                    type={
                        "type_name"="optional";
                        "item"="string";
                    };
                };
            ];})");
}

TEST(TYsonStructSchemaTest, TestYsonStructWithCustomType)
{
    CheckSchema(
        New<TTestStructWithCustomType>(),
        R"({type_name="struct";
            members=[
                {name="my_type";type="int64";};
            ]})");
}

TEST(TYsonStructSchemaTest, TestYsonStructWithUndefinedType)
{
    auto ysonStruct = New<TTestStructWithUndefinedType>();
    CheckSchema(
        ysonStruct,
        R"({type_name="struct";
            members=[
                {
                    name="undefined_type_field";
                    type={type_name="optional";item={type_name="struct";members=[];};};
                };
            ]})");
    ysonStruct->UndefinedTypeField = New<TTestSubStruct>();
    CheckSchema(
        ysonStruct,
        R"({type_name="struct";
            members=[
                {
                    name="undefined_type_field";
                    type={type_name="optional";item={type_name="struct";members=[{name="my_uint";type="uint32";}]}};
                };
            ]})");
}

TEST(TYsonStructSchemaTest, TestYsonStructWithTuples)
{
    CheckSchema(
        New<TTestStructWithTuples>(),
        R"({type_name="struct";
            members=[
                {
                    name="pair";
                    required=%true;
                    type={
                        type_name="tuple";
                        elements=[{"type"="string"};{"type"="string"};];
                    };
                };
                {
                    name="tuple";
                    required=%true;
                    type={
                        type_name="tuple";
                        elements=[{"type"="string"};{"type"="uint64"};{"type"="double"};];
                    };
                };
            ]})");
}

TEST(TYsonStructSchemaTest, TestYsonStructWithArray)
{
    CheckSchema(
        New<TTestStructWithArray>(),
        R"({type_name="struct";
            members=[
                {
                    name="string_array";
                    required=%true;
                    type={
                        type_name="tuple";
                        elements=[{"type"="string"};{"type"="string"};{"type"="string"};];
                    };
                };
            ]})");
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NYTree
