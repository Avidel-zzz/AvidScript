#pragma once

#include "AvidScriptEditorCSharpBuildService.h"

#include "CoreMinimal.h"

struct FAvidScriptEditorCSharpBuildArtifactBackup
{
	FString CommittedPath;
	FString BackupPath;
	bool bExisted = false;
};

struct FAvidScriptEditorCSharpBuildPlan
{
	FAvidScriptEditorCSharpBuildConfig FinalConfig;
	FAvidScriptEditorCSharpBuildConfig BootstrapConfig;
	FAvidScriptEditorCSharpBuildResult BootstrapResult;
	FString AuthorizationBindingPackagePath;
	FString RuntimeBindingPackagePath;
	FAvidScriptBindingSelectionProfile AuthorizationBindingProfile;
	FString BindingSelectionHash;
	FString BootstrapRoot;
	FString ArtifactBackupRoot;
	TArray<FAvidScriptEditorCSharpBuildArtifactBackup> ArtifactBackups;
	bool bAutomaticBindingSlice = false;
	bool bUsesEngineGameplayBindingProfile = true;
	bool bReusedAuthorizationBindingPackage = false;
	bool bBootstrapCompleted = false;
	bool bArtifactTransactionActive = false;
};

class FAvidScriptEditorCSharpBuildPipeline
{
public:
	static bool Prepare(
		const FAvidScriptEditorCSharpBuildConfig& Config,
		FAvidScriptEditorCSharpBuildPlan& OutPlan,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static bool Prepare(
		const FAvidScriptEditorCSharpBuildRequest& Request,
		FAvidScriptEditorCSharpBuildPlan& OutPlan,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static bool CompleteBootstrap(
		FAvidScriptEditorCSharpBuildPlan& Plan,
		const FAvidScriptEditorCSharpBuildResult& BootstrapResult,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static bool CompleteFinal(
		FAvidScriptEditorCSharpBuildPlan& Plan,
		const FAvidScriptEditorCSharpBuildResult& FinalResult,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static void Cleanup(FAvidScriptEditorCSharpBuildPlan& Plan);
};
