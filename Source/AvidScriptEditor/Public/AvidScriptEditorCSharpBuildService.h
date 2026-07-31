#pragma once

#include "AvidScriptEditorProjectBindingProfile.h"
#include "AvidScriptEditorBindingSelectionTypes.h"

#include "CoreMinimal.h"

enum class EAvidScriptEditorVmArtifactPolicy : uint8
{
	JitOnly,
	PreferPrecompiled,
	RequirePrecompiled
};

struct FAvidScriptEditorCSharpBuildConfig
{
	FString SourcePath;
	FString ProjectPath;
	FString BuildScriptPath;
	FString OutputRoot;
	FString ReportPath;
	FString ManifestPath;
	FString PreparedBuildReportPath;
	FString SemanticCacheRoot;
	FString BindingPackagePath;
	FString RuntimeBindingPackagePath;
	FString DotNetPath;
	FString ModuleId;
	FString ArtifactStem;
	FString Configuration = TEXT("Release");
	EAvidScriptEditorVmArtifactPolicy VmArtifactPolicy =
		EAvidScriptEditorVmArtifactPolicy::PreferPrecompiled;
	bool bEnableDataLaneFusion = true;
	bool bOmitRuntimeBindingPackage = false;
	bool bDisableSemanticCache = false;
};

struct FAvidScriptEditorCSharpBuildRequest
{
	FAvidScriptEditorCSharpBuildConfig Config;
	FAvidScriptBindingSelectionProfile AuthorizationBindingProfile;
	TArray<FAvidScriptProjectBindingClassSpec> AuthorizationClassReferences;
	TArray<FAvidScriptProjectObjectFactorySpec> AuthorizationObjectFactories;
	FString BindingSelectionHash;
	bool bUsesEngineGameplayBindingProfile = true;
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
	int32 FrontendInvocationCount = 0;
	int32 SemanticInvocationCount = 0;
	int32 GuestIrInvocationCount = 0;
	int32 WasmBackendInvocationCount = 0;
	int32 SemanticCacheSchemaVersion = 0;
	bool bSemanticCacheEnabled = false;
	FString SemanticCacheKey;
	FString SemanticCacheToolchainFingerprint;
	FString SemanticCacheLookup;
	FString SemanticCacheEntryReport;
	FString SemanticCacheEntryReportSha256;
	bool bSemanticCachePublished = false;
	FString SemanticCacheDiagnosticCode;
	FString SemanticCacheDiagnosticMessage;
	FString BindingSelectionHash;
	bool bReusedAuthorizationBindingPackage = false;
	bool bVmArtifactPublished = false;
	bool bVmArtifactCacheHit = false;
	double VmArtifactCompileMs = 0.0;
	FString VmArtifactPath;
	FString VmArtifactFormat;
	FString VmArtifactSha256;
	FString VmArtifactCanonicalSha256;
	FString VmArtifactCompilerBuildIdentity;
	FString VmArtifactTargetTriple;
	FString VmArtifactAttestationId;
	FString VmArtifactPolicy;
	FString VmArtifactRequestedBackend;
	FString VmArtifactSelectedBackend;
	FString VmArtifactFallbackCategory;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpBuildService
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

	static bool BuildProfile(
		const FAvidScriptEditorCSharpBuildRequest& Request,
		FAvidScriptEditorCSharpBuildResult& OutResult);

	static bool BuildActorLifecycle(
		const FAvidScriptEditorCSharpBuildConfig& Config,
		FAvidScriptEditorCSharpBuildResult& OutResult);
};
