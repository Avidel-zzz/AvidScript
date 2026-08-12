#pragma once

#include "AvidScriptEditorProjectBindingProfile.h"
#include "AvidScriptEditorBindingSelectionTypes.h"
#include "CoreMinimal.h"

struct FAvidScriptBindingDescriptorGenerateResult
{
	bool bSucceeded = false;
	int32 BindingCount = 0;
	int32 DelegateEventCount = 0;
	int32 TypeCount = 0;
	int32 ClassReferenceCount = 0;
	int32 ObjectFactoryCount = 0;
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
	static FAvidScriptBindingSelectionProfile MakeEngineGameplayProfile();

	static bool Generate(
		const FString& PackageName,
		const TArray<FAvidScriptReflectedFunctionSelection>& Selections,
		FString& OutJson,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);
	static bool GenerateWithReadableProperties(
		const FString& PackageName,
		const TArray<FAvidScriptReflectedFunctionSelection>& FunctionSelections,
		const TArray<FAvidScriptReflectedPropertySelection>& PropertySelections,
		FString& OutJson,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);
	static bool GenerateWithClassReferences(
		const FString& PackageName,
		const TArray<FAvidScriptReflectedFunctionSelection>& FunctionSelections,
		const TArray<FAvidScriptReflectedPropertySelection>& PropertySelections,
		const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
		FString& OutJson,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);
	static bool GenerateWithObjectFactories(
		const FString& PackageName,
		const TArray<FAvidScriptReflectedFunctionSelection>& FunctionSelections,
		const TArray<FAvidScriptReflectedPropertySelection>& PropertySelections,
		const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
		const TArray<FAvidScriptProjectObjectFactorySpec>& ObjectFactories,
		FString& OutJson,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);

	static bool GenerateFromProfile(
		const FAvidScriptBindingSelectionProfile& Profile,
		FString& OutJson,
		FAvidScriptBindingSelectionResolveResult& OutSelectionResult,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);
	static bool GenerateFromProfile(
		const FAvidScriptBindingSelectionProfile& Profile,
		const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
		FString& OutJson,
		FAvidScriptBindingSelectionResolveResult& OutSelectionResult,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);
	static bool GenerateFromProfile(
		const FAvidScriptBindingSelectionProfile& Profile,
		const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
		const TArray<FAvidScriptProjectObjectFactorySpec>& ObjectFactories,
		FString& OutJson,
		FAvidScriptBindingSelectionResolveResult& OutSelectionResult,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);

	static bool GenerateDefault(
		FString& OutJson,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);

	static bool WriteDefault(
		const FString& OutputPath,
		FAvidScriptBindingDescriptorGenerateResult& OutResult);
};
