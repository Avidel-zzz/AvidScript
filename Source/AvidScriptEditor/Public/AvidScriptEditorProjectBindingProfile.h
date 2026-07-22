#pragma once

#include "AvidScriptEditorBindingSelectionTypes.h"

#include "CoreMinimal.h"

struct FAvidScriptProjectBindingClassSpec
{
	FString ScriptName;
	FString ClassPath;
	FString BaseClassPath;
	FString LoadPolicy = TEXT("EditorLoad");
};

struct FAvidScriptProjectBindingProfileSpec
{
	FString PackageName;
	TArray<FString> ModulePaths;
	TArray<FAvidScriptReflectedClassSelection> Classes;
	TArray<FAvidScriptProjectBindingClassSpec> ClassReferences;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorProjectBindingProfile
{
public:
	static const TCHAR* GetResolverVersion();

	static bool Resolve(
		const FAvidScriptProjectBindingProfileSpec& Spec,
		FAvidScriptBindingSelectionProfile& OutSelection,
		TArray<FAvidScriptProjectBindingClassSpec>& OutClassReferences,
		FString& OutSelectionHash,
		FAvidScriptBindingSelectionResolveResult& OutResult);
};
