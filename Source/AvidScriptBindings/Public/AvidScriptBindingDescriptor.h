#pragma once

#include "AvidScriptBindingReloadEffect.h"
#include "AvidScriptObjectFactoryPolicy.h"
#include "CoreMinimal.h"

struct FAvidScriptBindingEnumValue
{
	FString Name;
	int64 Value = 0;
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

class AVIDSCRIPTBINDINGS_API FAvidScriptBindingDescriptorIdentity
{
public:
	static FString MakeTypeIdentity(
		const FString& CanonicalType,
		const TArray<FAvidScriptBindingEnumValue>& EnumValues);

	static FString MakeTypeStableId(
		const FString& CanonicalType,
		const TArray<FAvidScriptBindingEnumValue>& EnumValues);

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
