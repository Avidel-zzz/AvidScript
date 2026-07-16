#pragma once

#include "BindingGeneration/AvidScriptEditorBindingDescriptorIdentity.h"
#include "CoreMinimal.h"

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

class FAvidScriptEditorBindingDescriptorModelParser
{
public:
	static bool Parse(
		const FString& Json,
		FAvidScriptBindingPackageModel& OutPackage,
		FString& OutErrorCategory,
		FString& OutErrorSource);
};
