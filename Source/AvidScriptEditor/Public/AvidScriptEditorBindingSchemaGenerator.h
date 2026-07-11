#pragma once

#include "CoreMinimal.h"

struct FAvidScriptReflectedBindingSpec
{
	FString OwnerClassPath;
	FName FunctionName;
	FString ImportModule;
	FString ImportName;
	FString AbiSignature;
	FString Projection;
};

struct FAvidScriptBindingSchemaGenerateResult
{
	bool bSucceeded = false;
	int32 BindingCount = 0;
	int32 IntrinsicCount = 0;
	FString OutputPath;
	FString ErrorCategory;
	FString ErrorSource;
	FString NextAction;
	FString ErrorMessage;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorBindingSchemaGenerator
{
public:
	static TArray<FAvidScriptReflectedBindingSpec> MakeDefaultSpecs();

	static bool Generate(
		const TArray<FAvidScriptReflectedBindingSpec>& Specs,
		FString& OutJson,
		FAvidScriptBindingSchemaGenerateResult& OutResult);

	static bool GenerateDefault(
		FString& OutJson,
		FAvidScriptBindingSchemaGenerateResult& OutResult);

	static bool WriteDefault(
		const FString& OutputPath,
		FAvidScriptBindingSchemaGenerateResult& OutResult);

	static bool ValidateManifestImports(
		const FString& ManifestPath,
		FAvidScriptBindingSchemaGenerateResult& OutResult);
};

