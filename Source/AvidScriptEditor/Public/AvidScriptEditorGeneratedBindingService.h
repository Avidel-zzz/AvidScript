#pragma once

#include "AvidScriptGeneratedBindingTypes.h"
#include "CoreMinimal.h"

struct FAvidScriptEditorGeneratedBindingResult
{
	bool bSucceeded = false;
	int32 BindingCount = 0;
	FString PackageHash;
	FString OutputDirectory;
	FString ErrorCategory;
	FString ErrorSource;
	FString ErrorMessage;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorGeneratedBindingService
{
public:
	static bool BuildIr(
		const FString& DescriptorJson,
		FAvidScriptGeneratedBindingPackageIr& OutPackage,
		FAvidScriptEditorGeneratedBindingResult& OutResult);

	static bool EmitProjectModule(
		const FString& ProjectFile,
		const FAvidScriptGeneratedBindingPackageIr& Package,
		FAvidScriptEditorGeneratedBindingResult& OutResult);

	static bool GenerateProjectModule(
		const FString& ProjectFile,
		const FString& DescriptorJson,
		FAvidScriptEditorGeneratedBindingResult& OutResult);
};
