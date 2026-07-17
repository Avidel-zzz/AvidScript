#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCSharpBuildService.h"

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

	FAvidScriptEditorCSharpBuildResult BuildResult;
	const bool bBuildSucceeded = FAvidScriptEditorCSharpBuildService::BuildProfile(Config, BuildResult);
	TestTrue(
		TEXT("Custom C# profile automatically publishes the engine gameplay bindings"),
		bBuildSucceeded);
	TestTrue(TEXT("Custom C# profile build result succeeds"), BuildResult.bSucceeded);
	TestEqual(TEXT("Custom C# profile process exit code"), BuildResult.ProcessExitCode, 0);
	TestTrue(
		TEXT("Custom C# profile records a binding package manifest"),
		FPaths::FileExists(BuildResult.BindingPackagePath));
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
	const TSharedPtr<FJsonObject>* BindingPackageObject = nullptr;
	if (!TestTrue(
		TEXT("Custom report contains binding package provenance"),
		ReportObject->TryGetObjectField(TEXT("binding_package"), BindingPackageObject))
		|| BindingPackageObject == nullptr
		|| !BindingPackageObject->IsValid())
	{
		return false;
	}
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
		TEXT("Custom report keeps the complete gameplay profile as its authorization ceiling"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("profile_import_count"))),
		115);
	TestEqual(
		TEXT("Custom report records only the two generated bindings used by the script"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("used_import_count"))),
		2);
	TestEqual(TEXT("Custom report exposes two used stable identities"), (*BindingPackageObject)->GetArrayField(TEXT("used_imports")).Num(), 2);

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
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
