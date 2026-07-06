#pragma once

#include "AvidScriptEditorCSharpBuildService.h"

#include "CoreMinimal.h"

struct FAvidScriptEditorCSharpProfileLoadResult
{
	bool bSucceeded = false;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString NormalizedProfilePath;
	FAvidScriptEditorCSharpBuildConfig BuildConfig;
};

struct FAvidScriptEditorCSharpProfileTemplateResult
{
	bool bSucceeded = false;
	bool bCreated = false;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString NormalizedProfilePath;
	FString SourcePath;
	FString ProjectPath;
	FString BuildScriptPath;
	FString OutputRoot;
	FString ReportPath;
	FString ManifestPath;
	FString ModuleId;
	FString ArtifactStem;
	FString Configuration = TEXT("Release");
};

class FAvidScriptEditorCSharpProfileService
{
public:
	static FString GetDefaultProfilePath();

	static bool WriteDefaultProfileTemplate(
		FAvidScriptEditorCSharpProfileTemplateResult& OutResult,
		bool bOverwrite = false);

	static bool WriteProfileTemplate(
		const FString& ProfilePath,
		FAvidScriptEditorCSharpProfileTemplateResult& OutResult,
		bool bOverwrite = false);

	static bool LoadProfile(
		const FString& ProfilePath,
		FAvidScriptEditorCSharpProfileLoadResult& OutResult);
};
