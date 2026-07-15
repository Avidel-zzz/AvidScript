#pragma once

#include "CoreMinimal.h"

struct FAvidScriptReflectedFunctionSelection
{
	FString OwnerClassPath;
	FName FunctionName;
};

struct FAvidScriptBindingDescriptorGenerateResult
{
	bool bSucceeded = false;
	int32 BindingCount = 0;
	int32 TypeCount = 0;
	FString PackageHash;
	FString SelectionHash;
	FString OutputPath;
	FString ErrorCategory;
	FString ErrorSource;
	FString NextAction;
	FString ErrorMessage;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorBindingDescriptorGenerator
{
public:
	static const TCHAR* GetDefaultPackageName();
	static TArray<FAvidScriptReflectedFunctionSelection> MakeDefaultSelections();

	static bool Generate(
		const FString& PackageName,
		const TArray<FAvidScriptReflectedFunctionSelection>& Selections,
		FString& OutJson,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);

	static bool GenerateDefault(
		FString& OutJson,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);

	static bool WriteDefault(
		const FString& OutputPath,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);
};
