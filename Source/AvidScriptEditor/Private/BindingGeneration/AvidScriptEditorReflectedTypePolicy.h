#pragma once

#include "BindingGeneration/AvidScriptEditorBindingDescriptorIdentity.h"
#include "CoreMinimal.h"

class FProperty;
class UClass;
class UFunction;

struct FAvidScriptProjectedBindingType
{
	FString CanonicalType;
	FString StableId;
	FString Kind;
	FString CppType;
	int32 Size = 0;
	int32 Alignment = 1;
	TArray<FString> AbiValueTypes;
	TArray<FAvidScriptBindingEnumValue> EnumValues;
	bool bVoid = false;
};

struct FAvidScriptProjectedBindingValue
{
	FString Name;
	FString Direction;
	bool bHasDefaultValue = false;
	FString DefaultValue;
	FAvidScriptProjectedBindingType Type;
};

struct FAvidScriptProjectedFunction
{
	FAvidScriptProjectedBindingValue ReturnValue;
	TArray<FAvidScriptProjectedBindingValue> Parameters;
	FString AbiSignature;
};

class FAvidScriptEditorReflectedTypePolicy
{
public:
	static FAvidScriptProjectedBindingType MakeObjectType(const UClass* ObjectClass);
	static bool ProjectFunction(
		const UFunction* Function,
		bool bIsStatic,
		FAvidScriptProjectedFunction& OutProjection,
		FString& OutErrorSource);
	static bool ProjectReadableProperty(
		const FProperty* Property,
		FAvidScriptProjectedBindingValue& OutValue,
		FString& OutErrorSource);

private:
	static bool ProjectProperty(
		const FProperty* Property,
		FAvidScriptProjectedBindingValue& OutValue,
		FString& OutErrorSource);
};
