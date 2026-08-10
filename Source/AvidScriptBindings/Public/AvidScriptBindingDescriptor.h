#pragma once

#include "AvidScriptBindingReloadEffect.h"
#include "AvidScriptObjectFactoryPolicy.h"
#include "CoreMinimal.h"

struct FAvidScriptBindingEnumValue
{
	FString Name;
	int64 Value = 0;
};

struct FAvidScriptBindingStructFieldModel
{
	FString Name;
	FString TypeId;
	int32 WireOffset = 0;
};

struct FAvidScriptBindingTypeModel
{
	FString StableId;
	FString CanonicalType;
	FString Kind;
	FString CppType;
	int32 Size = 0;
	int32 Alignment = 0;
	TArray<FString> AbiTypes;
	TArray<FAvidScriptBindingEnumValue> EnumValues;
	TArray<FAvidScriptBindingStructFieldModel> StructFields;
	FString ElementTypeId;
	int32 ObjectTypeOrdinal = INDEX_NONE;
	FString ClassPath;
	FString BaseTypeId;
};

struct FAvidScriptBindingValueModel
{
	FString Name;
	FString Direction;
	bool bHasDefault = false;
	FString DefaultValue;
	FString CanonicalType;
	FString TypeId;
	FString Kind;
	FString CppType;
	TArray<FString> AbiTypes;
};

struct FAvidScriptBindingHostImportModel
{
	FString Module;
	FString Name;
	FString Signature;
};

struct FAvidScriptBindingFunctionModel
{
	FString StableId;
	FString CanonicalIdentity;
	int32 Ordinal = INDEX_NONE;
	FString OwnerClass;
	FString BindingKind = TEXT("function");
	FString UeMember;
	FString UeFunction;
	FString ScriptName;
	FString DispatchMode;
	FString GeneratedShape;
	FString GeneratedReceiverMode;
	FString GeneratedImportName;
	int32 SemanticFallbackOrdinal = INDEX_NONE;
	FString WritePolicy = TEXT("none");
	bool bStatic = false;
	bool bConst = false;
	EAvidScriptBindingReloadEffect ReloadEffect = EAvidScriptBindingReloadEffect::Unsupported;
	FAvidScriptBindingValueModel ReturnValue;
	TArray<FAvidScriptBindingValueModel> Parameters;
	FAvidScriptBindingHostImportModel HostImport;
};

struct FAvidScriptBindingClassReferenceModel
{
	FString StableId;
	int32 Ordinal = INDEX_NONE;
	FString ScriptName;
	FString ClassPath;
	FString BaseClassPath;
	FString LoadPolicy;
	FString ResultTypeId;
};

struct FAvidScriptBindingObjectFactoryModel
{
	FString StableId;
	int32 Ordinal = INDEX_NONE;
	FString ScriptName;
	FString ClassReferenceId;
	EAvidScriptObjectFactoryKind Kind = EAvidScriptObjectFactoryKind::NewObject;
	FString OuterTypeId;
	EAvidScriptObjectOwnershipPolicy Ownership =
		EAvidScriptObjectOwnershipPolicy::Session;
	EAvidScriptComponentRegistrationPolicy Registration =
		EAvidScriptComponentRegistrationPolicy::None;
};

struct FAvidScriptBindingPackageModel
{
	int32 SchemaVersion = 0;
	FString GeneratorVersion;
	FString EngineVersion;
	FString Source;
	FString PackageName;
	FString PackageHash;
	FString SelectionHash;
	FString GeneratedSourcePackageHash;
	FString SelfTypeId;
	bool bHasActiveObjectTypeOrdinals = false;
	TArray<int32> ActiveObjectTypeOrdinals;
	TArray<FAvidScriptBindingTypeModel> Types;
	TArray<FAvidScriptBindingFunctionModel> Bindings;
	TArray<FAvidScriptBindingClassReferenceModel> ClassReferences;
	TArray<FAvidScriptBindingObjectFactoryModel> ObjectFactories;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptBindingDescriptorTypeGraph
{
public:
	static bool IsDerivedFromClassPath(
		const FAvidScriptBindingPackageModel& Package,
		const FString& TypeId,
		const FString& ClassPath);
};

class AVIDSCRIPTBINDINGS_API FAvidScriptBindingDescriptorLayout
{
public:
	static bool ValidateTypeGraph(
		const TArray<FAvidScriptBindingTypeModel>& Types,
		FString& OutErrorSource);

	static bool ValidateStructWireGraph(
		const TArray<FAvidScriptBindingTypeModel>& Types,
		FString& OutErrorSource);
};

class AVIDSCRIPTBINDINGS_API FAvidScriptBindingDescriptorIdentity
{
public:
	static FString MakeTypeIdentity(
		const FString& CanonicalType,
		const TArray<FAvidScriptBindingEnumValue>& EnumValues,
		const TArray<FAvidScriptBindingStructFieldModel>& StructFields = {},
		int32 WireSize = INDEX_NONE,
		int32 WireAlignment = INDEX_NONE,
		const FString& ElementTypeId = FString());

	static FString MakeTypeStableId(
		const FString& CanonicalType,
		const TArray<FAvidScriptBindingEnumValue>& EnumValues,
		const TArray<FAvidScriptBindingStructFieldModel>& StructFields = {},
		int32 WireSize = INDEX_NONE,
		int32 WireAlignment = INDEX_NONE,
		const FString& ElementTypeId = FString());

	static FString MakeClassReferenceIdentity(
		const FString& ClassPath,
		const FString& BaseClassPath,
		const FString& LoadPolicy);

	static FString MakeClassReferenceStableId(
		const FString& ClassPath,
		const FString& BaseClassPath,
		const FString& LoadPolicy);

	static FString MakeObjectFactoryIdentity(
		const FString& ClassReferenceId,
		EAvidScriptObjectFactoryKind Kind,
		const FString& OuterTypeId,
		EAvidScriptObjectOwnershipPolicy Ownership,
		EAvidScriptComponentRegistrationPolicy Registration);

	static FString MakeObjectFactoryStableId(
		const FString& ClassReferenceId,
		EAvidScriptObjectFactoryKind Kind,
		const FString& OuterTypeId,
		EAvidScriptObjectOwnershipPolicy Ownership,
		EAvidScriptComponentRegistrationPolicy Registration);

	static bool IsFunctionDispatchModeSupported(
		int32 SchemaVersion,
		const FString& DispatchMode);

	static FString MakeFunctionCanonicalIdentity(
		const FString& BaseCanonicalIdentity,
		const FString& DispatchMode,
		const FString& GeneratedShape = FString(),
		const FString& GeneratedReceiverMode = FString(),
		const FString& GeneratedImportName = FString());

	static FString MakePropertySetCanonicalIdentity(
		const FString& OwnerClass,
		const FString& PropertyName,
		const FString& CanonicalValueType,
		const FString& BlueprintSetterFunction);

	static FString MakeSelectionHash(const FAvidScriptBindingPackageModel& Package);
	static FString MakePackageHash(const FAvidScriptBindingPackageModel& Package);
};

class AVIDSCRIPTBINDINGS_API FAvidScriptBindingDescriptorParser
{
public:
	static bool Parse(
		const FString& Json,
		FAvidScriptBindingPackageModel& OutPackage,
		FString& OutErrorCategory,
		FString& OutErrorSource);
};
