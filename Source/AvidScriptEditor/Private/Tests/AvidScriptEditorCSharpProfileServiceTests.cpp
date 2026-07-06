#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FString NormalizeAvidScriptCSharpProfileTestPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString GetAvidScriptCSharpProfileServiceTestRoot()
{
	return NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptEditorTests"),
		TEXT("CSharpProfileService")));
}

FString MakeAvidScriptCSharpProfileSourceText()
{
	return TEXT(
		"using AvidScript;\n"
		"\n"
		"public static class ProfileDrivenMover\n"
		"{\n"
		"    public static void BeginPlay()\n"
		"    {\n"
		"        Actor.SetLocation(31.0f, 32.0f, 33.0f);\n"
		"    }\n"
		"\n"
		"    public static void Tick(float deltaSeconds)\n"
		"    {\n"
		"        Actor.SetLocation(deltaSeconds * 8.0f, 9.0f, 10.0f);\n"
		"    }\n"
		"}\n");
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpProfileServiceMissingProfileTest,
	"AvidScript.Editor.CSharpProfileService.MissingProfileFailsSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpProfileServiceMissingProfileTest::RunTest(const FString& Parameters)
{
	const FString MissingProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		GetAvidScriptCSharpProfileServiceTestRoot(),
		TEXT("Missing"),
		TEXT("missing.csharp-profile.json")));

	FAvidScriptEditorCSharpProfileLoadResult Result;
	TestFalse(TEXT("Missing C# profile fails"), FAvidScriptEditorCSharpProfileService::LoadProfile(MissingProfilePath, Result));
	TestFalse(TEXT("Missing C# profile result fails"), Result.bSucceeded);
	TestEqual(TEXT("Missing profile category"), Result.ErrorCategory, FString(TEXT("profile_missing")));
	TestEqual(TEXT("Missing profile path normalized"), Result.NormalizedProfilePath, MissingProfilePath);
	TestFalse(TEXT("Missing profile next action is present"), Result.NextAction.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpProfileServiceLoadProfileTest,
	"AvidScript.Editor.CSharpProfileService.LoadProfileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpProfileServiceLoadProfileTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		GetAvidScriptCSharpProfileServiceTestRoot(),
		TEXT("ProfileDriven")));
	TestTrue(TEXT("C# profile test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString SourcePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(TestRoot, TEXT("ProfileDrivenMover.cs")));
	TestTrue(TEXT("C# profile source can be written"), FFileHelper::SaveStringToFile(MakeAvidScriptCSharpProfileSourceText(), *SourcePath));

	const FString OutputRoot = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ProfileDriven")));
	const FString ProfilePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(TestRoot, TEXT("profile_driven.csharp-profile.json")));
	const FString ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	const FString ProfileText = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"language\": \"csharp\",\n")
		TEXT("  \"source_path\": \"%s\",\n")
		TEXT("  \"project_path\": \"%s\",\n")
		TEXT("  \"module_id\": \"csharp_profile_driven\",\n")
		TEXT("  \"artifact_stem\": \"profile_driven\",\n")
		TEXT("  \"output_root\": \"%s\",\n")
		TEXT("  \"configuration\": \"Release\"\n")
		TEXT("}\n"),
		*SourcePath,
		*ProjectPath,
		*OutputRoot);
	TestTrue(TEXT("C# profile can be written"), FFileHelper::SaveStringToFile(ProfileText, *ProfilePath));

	FAvidScriptEditorCSharpProfileLoadResult Result;
	TestTrue(TEXT("C# profile loads"), FAvidScriptEditorCSharpProfileService::LoadProfile(ProfilePath, Result));
	TestTrue(TEXT("C# profile load result succeeds"), Result.bSucceeded);
	TestEqual(TEXT("C# profile path normalized"), Result.NormalizedProfilePath, ProfilePath);
	TestEqual(TEXT("C# profile source"), Result.BuildConfig.SourcePath, SourcePath);
	TestEqual(TEXT("C# profile project"), Result.BuildConfig.ProjectPath, ProjectPath);
	TestEqual(TEXT("C# profile module id"), Result.BuildConfig.ModuleId, FString(TEXT("csharp_profile_driven")));
	TestEqual(TEXT("C# profile artifact stem"), Result.BuildConfig.ArtifactStem, FString(TEXT("profile_driven")));
	TestEqual(TEXT("C# profile output root"), Result.BuildConfig.OutputRoot, OutputRoot);
	TestEqual(TEXT("C# profile report default"), Result.BuildConfig.ReportPath, FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(OutputRoot, TEXT("profile_driven")));
	TestEqual(TEXT("C# profile manifest default"), Result.BuildConfig.ManifestPath, FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(OutputRoot, TEXT("profile_driven")));
	TestEqual(TEXT("C# profile build script default"), Result.BuildConfig.BuildScriptPath, FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath());
	TestEqual(TEXT("C# profile configuration"), Result.BuildConfig.Configuration, FString(TEXT("Release")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
