#pragma once

#include "AvidScriptBindingReloadEffect.h"
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
	FString UeFunction;
	FString ScriptName;
	FString DispatchMode;
	bool bStatic = false;
	bool bConst = false;
	EAvidScriptBindingReloadEffect ReloadEffect = EAvidScriptBindingReloadEffect::Unsupported;
	FAvidScriptBindingValueModel ReturnValue;
	TArray<FAvidScriptBindingValueModel> Parameters;
	FAvidScriptBindingHostImportModel HostImport;
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
	TArray<FAvidScriptBindingTypeModel> Types;
	TArray<FAvidScriptBindingFunctionModel> Bindings;
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
