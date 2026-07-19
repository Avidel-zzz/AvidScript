#pragma once

#include "AvidScriptEditorCSharpBuildService.h"

#include "CoreMinimal.h"

struct FAvidScriptEditorCSharpBuildPlan
{
	FAvidScriptEditorCSharpBuildConfig FinalConfig;
	FAvidScriptEditorCSharpBuildConfig BootstrapConfig;
	FAvidScriptEditorCSharpBuildResult BootstrapResult;
	FString AuthorizationBindingPackagePath;
	FString RuntimeBindingPackagePath;
	FString BootstrapRoot;
	bool bAutomaticBindingSlice = false;
	bool bBootstrapCompleted = false;
};

class FAvidScriptEditorCSharpBuildPipeline
{
public:
	static bool Prepare(
		const FAvidScriptEditorCSharpBuildConfig& Config,
		FAvidScriptEditorCSharpBuildPlan& OutPlan,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static bool CompleteBootstrap(
		FAvidScriptEditorCSharpBuildPlan& Plan,
		const FAvidScriptEditorCSharpBuildResult& BootstrapResult,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static bool CompleteFinal(
		const FAvidScriptEditorCSharpBuildPlan& Plan,
		const FAvidScriptEditorCSharpBuildResult& FinalResult,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static void Cleanup(FAvidScriptEditorCSharpBuildPlan& Plan);
};
