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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpProfileServiceDefaultTemplateTest,
	"AvidScript.Editor.CSharpProfileService.DefaultTemplateSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpProfileServiceDefaultTemplateTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(
		GetAvidScriptCSharpProfileServiceTestRoot(),
		TEXT("DefaultTemplate")));
	TestTrue(TEXT("C# profile template test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString TemplatePath = NormalizeAvidScriptCSharpProfileTestPath(FPaths::Combine(TestRoot, TEXT("default_template.csharp-profile.json")));
	IFileManager::Get().Delete(*TemplatePath, false, true, true);

	FAvidScriptEditorCSharpProfileTemplateResult TemplateResult;
	TestTrue(TEXT("Default C# profile template writes"), FAvidScriptEditorCSharpProfileService::WriteProfileTemplate(TemplatePath, TemplateResult, false));
	TestTrue(TEXT("Default C# profile template result succeeds"), TemplateResult.bSucceeded);
	TestTrue(TEXT("Default C# profile template is created"), TemplateResult.bCreated);
	TestEqual(TEXT("Default C# profile template path"), TemplateResult.NormalizedProfilePath, TemplatePath);
	TestEqual(TEXT("Default C# profile template source"), TemplateResult.SourcePath, FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath());
	TestEqual(TEXT("Default C# profile template module id"), TemplateResult.ModuleId, FString(TEXT("csharp_profile_actor_lifecycle")));
	TestEqual(TEXT("Default C# profile template artifact stem"), TemplateResult.ArtifactStem, FString(TEXT("profile_actor_lifecycle")));
	TestTrue(TEXT("Default C# profile template file exists"), FPaths::FileExists(TemplatePath));

	FAvidScriptEditorCSharpProfileLoadResult LoadResult;
	TestTrue(TEXT("Generated default C# profile loads"), FAvidScriptEditorCSharpProfileService::LoadProfile(TemplatePath, LoadResult));
	TestTrue(TEXT("Generated default C# profile load succeeds"), LoadResult.bSucceeded);
	TestEqual(TEXT("Generated default C# profile source"), LoadResult.BuildConfig.SourcePath, FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath());
	TestEqual(TEXT("Generated default C# profile project"), LoadResult.BuildConfig.ProjectPath, FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath());
	TestEqual(TEXT("Generated default C# profile module"), LoadResult.BuildConfig.ModuleId, FString(TEXT("csharp_profile_actor_lifecycle")));
	TestEqual(TEXT("Generated default C# profile artifact"), LoadResult.BuildConfig.ArtifactStem, FString(TEXT("profile_actor_lifecycle")));
	TestTrue(TEXT("Generated default C# profile output root uses profile folder"), LoadResult.BuildConfig.OutputRoot.EndsWith(TEXT("Saved/AvidScriptCSharpGuest/Profiles/profile_actor_lifecycle")));

	FString OriginalProfileText;
	TestTrue(TEXT("Generated default C# profile can be read back"), FFileHelper::LoadFileToString(OriginalProfileText, *TemplatePath));

	FAvidScriptEditorCSharpProfileTemplateResult ExistingResult;
	TestTrue(TEXT("Existing default C# profile template is accepted"), FAvidScriptEditorCSharpProfileService::WriteProfileTemplate(TemplatePath, ExistingResult, false));
	TestTrue(TEXT("Existing default C# profile template result succeeds"), ExistingResult.bSucceeded);
	TestFalse(TEXT("Existing default C# profile template is not overwritten"), ExistingResult.bCreated);

	FString ExistingProfileText;
	TestTrue(TEXT("Existing default C# profile can be read back"), FFileHelper::LoadFileToString(ExistingProfileText, *TemplatePath));
	TestEqual(TEXT("Existing default C# profile text is unchanged"), ExistingProfileText, OriginalProfileText);
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
