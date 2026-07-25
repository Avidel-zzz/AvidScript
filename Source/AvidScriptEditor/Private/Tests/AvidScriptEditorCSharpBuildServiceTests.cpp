#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "CSharpBuild/AvidScriptEditorCSharpBuildInvoker.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
FString NormalizeAvidScriptCSharpBuildTestPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

bool LoadAvidScriptCSharpBuildTestJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *Path))
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

FString MakeAvidScriptZeroBindingLifecycleSource()
{
	return TEXT(
		"using System.Runtime.InteropServices;\n"
		"\n"
		"namespace AvidScript;\n"
		"\n"
		"public static class ZeroBindingLifecycleScript\n"
		"{\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")]\n"
		"    public static void BeginPlay() {}\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_tick\")]\n"
		"    public static void Tick(float deltaSeconds) {}\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_end_play\")]\n"
		"    public static void EndPlay() {}\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_timer\")]\n"
		"    public static void OnTimer(int callbackId, int timerHandle) {}\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_event\")]\n"
		"    public static void OnEvent(int eventId, float value) {}\n"
		"\n"
		"    [UnmanagedCallersOnly(EntryPoint = \"avid_on_gameplay_event\")]\n"
		"    public static void OnGameplayEvent(\n"
		"        int eventType, int primaryId, int secondaryId, int objectSlot,\n"
		"        int objectGeneration, float x, float y, float z) {}\n"
		"}\n");
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildServiceCustomProfileTest,
	"AvidScript.Editor.CSharpBuildService.CustomProfileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildServiceCustomProfileTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests"),
		TEXT("CSharpProfiles"),
		TEXT("CustomMover")));
	TestTrue(TEXT("Custom C# profile test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString SourcePath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(TestRoot, TEXT("CustomMoverScript.cs")));
	const FString GeneratedLifecycleSamplePath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Samples/CSharp/GeneratedBindingLifecycle/GeneratedBindingLifecycleScript.cs")));
	FString SourceText;
	if (!TestTrue(
		TEXT("Generated binding lifecycle sample can be read"),
		FFileHelper::LoadFileToString(SourceText, *GeneratedLifecycleSamplePath)))
	{
		return false;
	}
	const int32 ClosingBraceIndex = SourceText.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (!TestTrue(TEXT("Generated binding lifecycle sample has a closing type brace"), ClosingBraceIndex != INDEX_NONE))
	{
		return false;
	}
	SourceText = SourceText.Left(ClosingBraceIndex)
		+ TEXT("    private static void UnreachableBindingHelper()\n    {\n        _ = UE.Self.GetActorLocation();\n    }\n")
		+ SourceText.Mid(ClosingBraceIndex);
	TestTrue(TEXT("Custom C# source can be written"), FFileHelper::SaveStringToFile(SourceText, *SourcePath));

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.SourcePath = SourcePath;
	Config.ModuleId = TEXT("csharp_custom_mover");
	Config.ArtifactStem = TEXT("custom_mover");
	Config.OutputRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("CustomMover")));
	Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);
	Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);
	Config.SemanticCacheRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P43_5/CustomMover/CSharpSemanticCache/v1")));
	IFileManager::Get().DeleteDirectory(*Config.SemanticCacheRoot, false, true);

	FAvidScriptEditorCSharpBuildResult BuildResult;
	const bool bBuildSucceeded = FAvidScriptEditorCSharpBuildService::BuildProfile(Config, BuildResult);
	TestTrue(
		TEXT("Custom C# profile automatically publishes the engine gameplay bindings"),
		bBuildSucceeded);
	TestTrue(TEXT("Custom C# profile build result succeeds"), BuildResult.bSucceeded);
	TestEqual(TEXT("Custom C# profile process exit code"), BuildResult.ProcessExitCode, 0);
	TestEqual(TEXT("Automatic custom C# profile performs bootstrap and final builds"), BuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Automatic custom C# profile runs Frontend once"), BuildResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Automatic custom C# profile runs Semantic once"), BuildResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Automatic custom C# profile runs Guest IR twice"), BuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Automatic custom C# profile runs WASM backend twice"), BuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Cold custom C# profile records a cache miss"), BuildResult.SemanticCacheLookup, FString(TEXT("miss")));
	TestFalse(TEXT("Cold custom C# profile records a semantic cache key"), BuildResult.SemanticCacheKey.IsEmpty());
	TestTrue(TEXT("Cold custom C# profile publishes a semantic cache entry"), BuildResult.bSemanticCachePublished);
	TestTrue(
		TEXT("Custom C# profile records an authorization binding package manifest"),
		FPaths::FileExists(BuildResult.AuthorizationBindingPackagePath));
	TestTrue(
		TEXT("Custom C# profile records a runtime binding package manifest"),
		FPaths::FileExists(BuildResult.BindingPackagePath));
	TestNotEqual(
		TEXT("Automatic custom C# profile separates authorization and runtime packages"),
		BuildResult.AuthorizationBindingPackagePath,
		BuildResult.BindingPackagePath);
	TestTrue(TEXT("Custom C# profile report exists"), FPaths::FileExists(Config.ReportPath));
	TestTrue(TEXT("Custom C# profile manifest exists"), FPaths::FileExists(Config.ManifestPath));
	TestTrue(
		TEXT("Custom C# profile publishes formal WASM"),
		FPaths::FileExists(FPaths::Combine(Config.OutputRoot, TEXT("custom_mover.wasm"))));

	TSharedPtr<FJsonObject> ReportObject;
	TestTrue(TEXT("Custom C# profile report is valid JSON"), LoadAvidScriptCSharpBuildTestJsonObject(Config.ReportPath, ReportObject));
	if (!ReportObject.IsValid())
	{
		return true;
	}

	TestEqual(
		TEXT("Custom report declares direct ABI success"),
		ReportObject->GetStringField(TEXT("result")),
		FString(TEXT("direct_abi_built")));
	TestTrue(TEXT("Custom report records success"), ReportObject->GetBoolField(TEXT("succeeded")));
	const TSharedPtr<FJsonObject>* BindingAuthorizationObject = nullptr;
	if (!TestTrue(
		TEXT("Custom report contains binding authorization provenance"),
		ReportObject->TryGetObjectField(TEXT("binding_authorization"), BindingAuthorizationObject))
		|| BindingAuthorizationObject == nullptr
		|| !BindingAuthorizationObject->IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("Custom report keeps the gameplay profile and shared capabilities as its authorization ceiling"),
		static_cast<int32>((*BindingAuthorizationObject)->GetIntegerField(TEXT("profile_import_count"))),
		344);
	TestEqual(
		TEXT("Custom authorization records five reflected bindings and packed owner access"),
		static_cast<int32>((*BindingAuthorizationObject)->GetIntegerField(TEXT("used_import_count"))),
		6);
	TestEqual(
		TEXT("Custom authorization exposes six used stable identities"),
		(*BindingAuthorizationObject)->GetArrayField(TEXT("used_imports")).Num(),
		6);
	const TSharedPtr<FJsonObject>* BindingPackageObject = nullptr;
	if (!TestTrue(
		TEXT("Custom report contains binding package provenance"),
		ReportObject->TryGetObjectField(TEXT("binding_package"), BindingPackageObject))
		|| BindingPackageObject == nullptr
		|| !BindingPackageObject->IsValid())
	{
		return false;
	}
	TestNotEqual(
		TEXT("Custom report separates authorization and runtime manifests"),
		(*BindingAuthorizationObject)->GetStringField(TEXT("manifest_file")),
		(*BindingPackageObject)->GetStringField(TEXT("manifest_file")));
	TestTrue(
		TEXT("Custom report marks generated bindings required"),
		(*BindingPackageObject)->GetBoolField(TEXT("required")));
	TestEqual(
		TEXT("Custom report records the engine gameplay package"),
		(*BindingPackageObject)->GetStringField(TEXT("package_name")),
		FString(TEXT("avidscript.engine.gameplay")));
	TestFalse(
		TEXT("Custom report records a content-addressed package hash"),
		(*BindingPackageObject)->GetStringField(TEXT("package_hash")).IsEmpty());
	TestEqual(
		TEXT("Custom report publishes five bindings, object-type support, and packed owner access"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("profile_import_count"))),
		7);
	TestEqual(
		TEXT("Custom runtime package records five reflected bindings and packed owner access"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("used_import_count"))),
		6);
	TestEqual(TEXT("Custom report exposes six used stable identities"), (*BindingPackageObject)->GetArrayField(TEXT("used_imports")).Num(), 6);

	TSharedPtr<FJsonObject> ManifestObject;
	TestTrue(TEXT("Custom C# profile manifest is valid JSON"), LoadAvidScriptCSharpBuildTestJsonObject(Config.ManifestPath, ManifestObject));
	if (ManifestObject.IsValid())
	{
		const TSharedPtr<FJsonObject>* ManifestBindingPackage = nullptr;
		if (TestTrue(
			TEXT("Custom manifest contains runtime binding package provenance"),
			ManifestObject->TryGetObjectField(TEXT("binding_package"), ManifestBindingPackage))
			&& ManifestBindingPackage != nullptr
			&& ManifestBindingPackage->IsValid())
		{
			TestEqual(
				TEXT("Custom manifest publishes five bindings, object-type support, and packed owner access"),
				static_cast<int32>((*ManifestBindingPackage)->GetIntegerField(TEXT("profile_import_count"))),
				7);
		}
	}

	FAvidScriptCSharpBindingEmitResult ExplicitPackage;
	if (!TestTrue(
		TEXT("Explicit gameplay binding package publishes"),
		FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(ExplicitPackage)))
	{
		AddError(ExplicitPackage.ErrorMessage);
		return false;
	}
	FAvidScriptEditorCSharpBuildConfig ExplicitConfig = Config;
	ExplicitConfig.OutputRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(TestRoot, TEXT("ExplicitPackage")));
	ExplicitConfig.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(ExplicitConfig.OutputRoot, ExplicitConfig.ArtifactStem);
	ExplicitConfig.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(ExplicitConfig.OutputRoot, ExplicitConfig.ArtifactStem);
	ExplicitConfig.BindingPackagePath = ExplicitPackage.ManifestPath;
	ExplicitConfig.PreparedBuildReportPath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		TestRoot,
		TEXT("CallerOwnedPreparedReport"),
		TEXT("missing.csharp.report.json")));
	ExplicitConfig.bDisableSemanticCache = true;
	FAvidScriptEditorCSharpBuildResult ExplicitResult;
	TestTrue(TEXT("Explicit package custom C# profile builds"), FAvidScriptEditorCSharpBuildService::BuildProfile(ExplicitConfig, ExplicitResult));
	TestEqual(TEXT("Explicit package uses one build invocation"), ExplicitResult.BuildInvocationCount, 1);
	TestEqual(TEXT("Explicit package runs Frontend once"), ExplicitResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Explicit package runs Semantic once"), ExplicitResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Explicit package runs Guest IR once"), ExplicitResult.GuestIrInvocationCount, 1);
	TestEqual(TEXT("Explicit package runs WASM backend once"), ExplicitResult.WasmBackendInvocationCount, 1);
	TestEqual(TEXT("Explicit diagnostic build disables semantic cache"), ExplicitResult.SemanticCacheLookup, FString(TEXT("disabled")));
	TestEqual(
		TEXT("Explicit package remains both authorization and runtime package"),
		ExplicitResult.AuthorizationBindingPackagePath,
		ExplicitResult.BindingPackagePath);

	FAvidScriptEditorCSharpBuildInvocation Invocation;
	FAvidScriptEditorCSharpBuildResult PreparedResult;
	TestTrue(
		TEXT("Explicit build invocation can be prepared independently"),
		FAvidScriptEditorCSharpBuildInvoker::Prepare(ExplicitConfig, Invocation, PreparedResult));
	TestEqual(
		TEXT("Prepared invocation uses PowerShell"),
		Invocation.ExecutablePath,
		FString(TEXT("powershell.exe")));
	TestFalse(TEXT("Prepared invocation contains parameters"), Invocation.Parameters.IsEmpty());

	FAvidScriptEditorCSharpBuildResult FinalizedResult;
	TestTrue(
		TEXT("Existing explicit artifacts finalize through the shared contract"),
		FAvidScriptEditorCSharpBuildInvoker::Finalize(
			Invocation,
			ExplicitResult.ProcessExitCode,
			ExplicitResult.Stdout,
			ExplicitResult.Stderr,
			FinalizedResult));
	TestEqual(TEXT("Shared finalizer preserves report path"), FinalizedResult.ReportPath, ExplicitResult.ReportPath);
	TestEqual(TEXT("Shared finalizer preserves manifest path"), FinalizedResult.ManifestPath, ExplicitResult.ManifestPath);
	TestEqual(TEXT("Shared finalizer preserves Frontend count"), FinalizedResult.FrontendInvocationCount, ExplicitResult.FrontendInvocationCount);
	TestEqual(TEXT("Shared finalizer preserves Semantic count"), FinalizedResult.SemanticInvocationCount, ExplicitResult.SemanticInvocationCount);
	TestEqual(TEXT("Shared finalizer preserves Guest IR count"), FinalizedResult.GuestIrInvocationCount, ExplicitResult.GuestIrInvocationCount);
	TestEqual(TEXT("Shared finalizer preserves WASM backend count"), FinalizedResult.WasmBackendInvocationCount, ExplicitResult.WasmBackendInvocationCount);
	TestEqual(TEXT("Shared finalizer preserves cache lookup"), FinalizedResult.SemanticCacheLookup, ExplicitResult.SemanticCacheLookup);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildServiceZeroBindingProfileTest,
	"AvidScript.Editor.CSharpBuildService.ZeroBindingProfileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildServiceZeroBindingProfileTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests/CSharpProfiles/ZeroBinding")));
	TestTrue(TEXT("Zero-binding profile root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));
	const FString SourcePath = FPaths::Combine(TestRoot, TEXT("ZeroBindingLifecycleScript.cs"));
	TestTrue(
		TEXT("Zero-binding C# source can be written"),
		FFileHelper::SaveStringToFile(MakeAvidScriptZeroBindingLifecycleSource(), *SourcePath));

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.SourcePath = SourcePath;
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.ModuleId = TEXT("csharp_zero_binding");
	Config.ArtifactStem = TEXT("zero_binding");
	Config.OutputRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(TestRoot, TEXT("Output")));
	Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);
	Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);
	Config.SemanticCacheRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P43_5/ZeroBinding/CSharpSemanticCache/v1")));
	IFileManager::Get().DeleteDirectory(*Config.SemanticCacheRoot, false, true);

	FAvidScriptEditorCSharpBuildResult BuildResult;
	TestTrue(TEXT("Zero-binding custom C# profile builds"), FAvidScriptEditorCSharpBuildService::BuildProfile(Config, BuildResult));
	TestEqual(TEXT("Zero-binding profile performs bootstrap and final builds"), BuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Zero-binding profile runs Frontend once"), BuildResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Zero-binding profile runs Semantic once"), BuildResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Zero-binding profile runs Guest IR twice"), BuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Zero-binding profile runs WASM backend twice"), BuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Cold zero-binding profile records a cache miss"), BuildResult.SemanticCacheLookup, FString(TEXT("miss")));
	TestTrue(TEXT("Zero-binding profile keeps authorization package"), FPaths::FileExists(BuildResult.AuthorizationBindingPackagePath));
	TestTrue(TEXT("Zero-binding profile omits runtime package path"), BuildResult.BindingPackagePath.IsEmpty());

	TSharedPtr<FJsonObject> ManifestObject;
	TestTrue(TEXT("Zero-binding manifest is valid JSON"), LoadAvidScriptCSharpBuildTestJsonObject(Config.ManifestPath, ManifestObject));
	if (ManifestObject.IsValid())
	{
		TestFalse(TEXT("Zero-binding manifest omits binding_package"), ManifestObject->HasField(TEXT("binding_package")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildServiceSourceMissingNextActionTest,
	"AvidScript.Editor.CSharpBuildService.SourceMissingNextActionSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildServiceSourceMissingNextActionTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpBuildConfig Config;
	Config.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.SourcePath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests"),
		TEXT("CSharpProfiles"),
		TEXT("MissingSource"),
		TEXT("MissingMover.cs")));

	FAvidScriptEditorCSharpBuildResult BuildResult;
	TestFalse(TEXT("Missing C# source build fails"), FAvidScriptEditorCSharpBuildService::BuildProfile(Config, BuildResult));
	TestFalse(TEXT("Missing C# source build result does not succeed"), BuildResult.bSucceeded);
	TestEqual(TEXT("Missing C# source error category"), BuildResult.ErrorCategory, FString(TEXT("source_missing")));
	TestFalse(TEXT("Missing C# source next action is set"), BuildResult.NextAction.IsEmpty());
	TestTrue(TEXT("Missing C# source next action mentions source or profile"), BuildResult.NextAction.Contains(TEXT("source")) || BuildResult.NextAction.Contains(TEXT("profile")));

	const FString BlockedOutputRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests"),
		TEXT("CSharpProfiles"),
		TEXT("Blocked*Output")));

	FAvidScriptEditorCSharpBuildConfig BlockedConfig;
	BlockedConfig.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	BlockedConfig.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	BlockedConfig.SourcePath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath();
	BlockedConfig.OutputRoot = BlockedOutputRoot;
	BlockedConfig.ReportPath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(BlockedOutputRoot, TEXT("blocked.csharp.report.json")));
	BlockedConfig.ManifestPath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(BlockedOutputRoot, TEXT("blocked.avidscript.json")));

	FAvidScriptEditorCSharpBuildResult BlockedResult;
	TestFalse(TEXT("Blocked output prevents process launch"), FAvidScriptEditorCSharpBuildService::BuildProfile(BlockedConfig, BlockedResult));
	TestEqual(TEXT("Blocked output has stable error category"), BlockedResult.ErrorCategory, FString(TEXT("output_directory_failed")));
	TestEqual(TEXT("Blocked output launches no build process"), BlockedResult.BuildInvocationCount, 0);
	TestEqual(TEXT("Blocked output launches no Frontend"), BlockedResult.FrontendInvocationCount, 0);
	TestEqual(TEXT("Blocked output launches no Semantic"), BlockedResult.SemanticInvocationCount, 0);
	TestEqual(TEXT("Blocked output launches no Guest IR"), BlockedResult.GuestIrInvocationCount, 0);
	TestEqual(TEXT("Blocked output launches no WASM backend"), BlockedResult.WasmBackendInvocationCount, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
