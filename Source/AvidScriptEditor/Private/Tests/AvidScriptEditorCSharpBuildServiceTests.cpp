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
	const FString SourceText = TEXT(
		"using AvidScript;\n"
		"\n"
		"public static class CustomMoverScript\n"
		"{\n"
		"    public static void BeginPlay()\n"
		"    {\n"
		"        Actor.SetLocation(11.0f, 22.0f, 33.0f);\n"
		"    }\n"
		"}\n");
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
	TestFalse(
		TEXT("Custom C# profile waits for generated Phase 42 bindings"),
		bBuildSucceeded);
	TestFalse(TEXT("Custom C# profile build result does not succeed"), BuildResult.bSucceeded);
	TestEqual(TEXT("Custom C# profile process exit code"), BuildResult.ProcessExitCode, 1);
	TestEqual(
		TEXT("Custom C# profile exposes the structured failure category"),
		BuildResult.ErrorCategory,
		FString(TEXT("phase42_binding_required")));
	TestTrue(
		TEXT("Custom C# profile exposes the actionable diagnostic"),
		BuildResult.ErrorMessage.Contains(TEXT("ASBI4201")));
	TestTrue(TEXT("Custom C# profile report exists"), FPaths::FileExists(Config.ReportPath));
	TestFalse(TEXT("Blocked custom profile leaves no manifest"), FPaths::FileExists(Config.ManifestPath));
	TestFalse(
		TEXT("Blocked custom profile leaves no formal WASM"),
		FPaths::FileExists(FPaths::Combine(Config.OutputRoot, TEXT("custom_mover.wasm"))));

	TSharedPtr<FJsonObject> ReportObject;
	TestTrue(TEXT("Custom C# profile report is valid JSON"), LoadAvidScriptCSharpBuildTestJsonObject(Config.ReportPath, ReportObject));
	if (!ReportObject.IsValid())
	{
		return true;
	}

	TestEqual(
		TEXT("Custom report declares the Phase 42 binding gate"),
		ReportObject->GetStringField(TEXT("result")),
		FString(TEXT("phase42_binding_required")));
	TestFalse(TEXT("Custom report records failure"), ReportObject->GetBoolField(TEXT("succeeded")));
	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
	if (!TestTrue(TEXT("Custom report declares diagnostics"), ReportObject->TryGetArrayField(TEXT("diagnostics"), Diagnostics)) || Diagnostics == nullptr)
	{
		return true;
	}
	const bool bHasBindingDiagnostic = Diagnostics->ContainsByPredicate(
		[](const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject> Diagnostic = Value.IsValid() ? Value->AsObject() : nullptr;
			return Diagnostic.IsValid() && Diagnostic->GetStringField(TEXT("code")) == TEXT("ASBI4201");
		});
	TestTrue(TEXT("Custom report includes ASBI4201"), bHasBindingDiagnostic);

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
