#pragma once

#include "CoreMinimal.h"

struct FAvidScriptEditorCSharpBuildConfig
{
	FString SourcePath;
	FString ProjectPath;
	FString BuildScriptPath;
	FString OutputRoot;
	FString ReportPath;
	FString ManifestPath;
	FString BindingPackagePath;
	FString RuntimeBindingPackagePath;
	FString DotNetPath;
	FString ModuleId;
	FString ArtifactStem;
	FString Configuration = TEXT("Release");
	bool bOmitRuntimeBindingPackage = false;
};

struct FAvidScriptEditorCSharpBuildResult
{
	bool bSucceeded = false;
	int32 ProcessExitCode = INDEX_NONE;
	FString Stdout;
	FString Stderr;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString SourcePath;
	FString ProjectPath;
	FString BuildScriptPath;
	FString OutputRoot;
	FString ReportPath;
	FString ManifestPath;
	FString AuthorizationBindingPackagePath;
	FString BindingPackagePath;
	FString ModuleId;
	FString ArtifactStem;
	int32 BuildInvocationCount = 0;
};

class FAvidScriptEditorCSharpBuildService
{
public:
	static FString GetDefaultActorLifecycleBuildScriptPath();
	static FString GetDefaultActorLifecycleSourcePath();
	static FString GetDefaultActorLifecycleProjectPath();
	static FString GetDefaultActorLifecycleOutputRoot();
	static FString GetDefaultActorLifecycleReportPath();
	static FString GetDefaultActorLifecycleManifestPath();
	static FString GetDefaultActorLifecycleModuleId();
	static FString GetDefaultActorLifecycleArtifactStem();

	static FString MakeReportPathForOutputRoot(const FString& OutputRoot, const FString& ArtifactStem);
	static FString MakeManifestPathForOutputRoot(const FString& OutputRoot, const FString& ArtifactStem);

	static bool BuildProfile(
		const FAvidScriptEditorCSharpBuildConfig& Config,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static bool BuildActorLifecycle(
		const FAvidScriptEditorCSharpBuildConfig& Config,
		FAvidScriptEditorCSharpBuildResult& OutResult);
};
