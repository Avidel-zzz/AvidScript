#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorCSharpWorkspaceService.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FString NormalizeAvidScriptWorkspaceTestPath(FString Path)
{
    Path = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Path);
    return Path;
}

FString GetAvidScriptWorkspaceTestRoot()
{
    return NormalizeAvidScriptWorkspaceTestPath(FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("AvidScript"),
        TEXT("Tests"),
        TEXT("P44"),
        TEXT("WorkspaceService")));
}

FAvidScriptEditorCSharpWorkspaceConfig MakeAvidScriptWorkspaceTestConfig(
    const FString& TestCase)
{
    const FString Root = NormalizeAvidScriptWorkspaceTestPath(FPaths::Combine(
        GetAvidScriptWorkspaceTestRoot(),
        TestCase));
    FAvidScriptEditorCSharpWorkspaceConfig Config;
    Config.WorkspaceRoot = FPaths::Combine(Root, TEXT("Workspace"));
    Config.GeneratedRoot = FPaths::Combine(Root, TEXT("Generated"));
    Config.BindingPackageRoot = FPaths::Combine(Root, TEXT("BindingPackages"));
    Config.OutputRoot = FPaths::Combine(Root, TEXT("Output"));
    return Config;
}

FString ReadAvidScriptWorkspaceTestText(
    FAutomationTestBase& Test,
    const FString& Path)
{
    FString Text;
    Test.TestTrue(
        *FString::Printf(TEXT("File can be read: %s"), *Path),
        FFileHelper::LoadFileToString(Text, *Path));
    return Text;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAvidScriptEditorCSharpWorkspaceCreateRefreshTest,
    "AvidScript.Editor.CSharpWorkspaceService.CreateRefreshPreservesUserFiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpWorkspaceCreateRefreshTest::RunTest(const FString& Parameters)
{
    const FAvidScriptEditorCSharpWorkspaceConfig Config = MakeAvidScriptWorkspaceTestConfig(TEXT("CreateRefresh"));
    IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Config.WorkspaceRoot), false, true);

    FAvidScriptEditorCSharpWorkspaceResult First;
    if (!TestTrue(
            TEXT("Project C# workspace creates"),
            FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(Config, First)))
    {
        AddError(First.ErrorCategory + TEXT(": ") + First.ErrorMessage);
        return false;
    }
    TestTrue(TEXT("Workspace result succeeds"), First.bSucceeded);
    TestEqual(TEXT("First create writes four user files"), First.CreatedUserFileCount, 4);
    TestEqual(TEXT("First create preserves no user files"), First.PreservedUserFileCount, 0);
    TestTrue(TEXT("Source is created"), First.bSourceCreated);
    TestTrue(TEXT("Project is created"), First.bProjectCreated);
    TestTrue(TEXT("Profile is created"), First.bProfileCreated);
    TestTrue(TEXT("Global json is created"), First.bGlobalJsonCreated);
    TestTrue(TEXT("Generated facade refreshes"), First.bFacadeRefreshed);
    TestTrue(TEXT("Source exists"), FPaths::FileExists(First.SourcePath));
    TestTrue(TEXT("Project exists"), FPaths::FileExists(First.ProjectPath));
    TestTrue(TEXT("Profile exists"), FPaths::FileExists(First.ProfilePath));
    TestTrue(TEXT("Global json exists"), FPaths::FileExists(First.GlobalJsonPath));
    TestTrue(TEXT("Facade exists"), FPaths::FileExists(First.FacadePath));
    TestTrue(TEXT("IDE binding package exists"), FPaths::FileExists(First.BindingPackageManifestPath));
    TestFalse(TEXT("IDE binding package hash is present"), First.BindingPackageHash.IsEmpty());

    const FString ProjectText = ReadAvidScriptWorkspaceTestText(*this, First.ProjectPath);
    TestTrue(
        TEXT("Project links generated facade"),
        ProjectText.Contains(TEXT("AvidScript.Bindings.generated.cs")));
    TestFalse(TEXT("Project has no unresolved tokens"), ProjectText.Contains(TEXT("{{")));
    const FString ProfileText = ReadAvidScriptWorkspaceTestText(*this, First.ProfilePath);
    TestFalse(TEXT("Profile has no unresolved tokens"), ProfileText.Contains(TEXT("{{")));
	const FString InitialSourceText = ReadAvidScriptWorkspaceTestText(*this, First.SourcePath);
	TestTrue(TEXT("Gameplay starter exports a zero-parameter async BeginPlay"),
		InitialSourceText.Contains(TEXT("public static async void BeginPlay()")));
	const int32 NextTickAwaitIndex = InitialSourceText.Find(
		TEXT("await AvidContinuations.NextTickAsync();"));
	const int32 ObjectAwaitIndex = InitialSourceText.Find(
		TEXT("await AvidAssets.LoadObjectAsync("));
	const int32 AsyncLoopIndex = InitialSourceText.Find(
		TEXT("for (int pass = 0; pass < scalePasses; ++pass)"));
	const int32 ConditionalDelayIndex = InitialSourceText.Find(
		TEXT("await AvidContinuations.DelayAsync(0.01f);"));
	const int32 ObjectUseIndex = InitialSourceText.Find(
		TEXT("HasAwaitedDefaultMesh = loadedObject.IsValid;"));
	TestTrue(TEXT("Gameplay starter awaits one next tick per loop iteration"),
		ObjectAwaitIndex != INDEX_NONE
		&& AsyncLoopIndex > ObjectAwaitIndex
		&& NextTickAwaitIndex > AsyncLoopIndex);
	TestTrue(TEXT("Gameplay starter supports a conditional delay continuation"),
		ConditionalDelayIndex > NextTickAwaitIndex
		&& ObjectUseIndex > ConditionalDelayIndex);
	TestTrue(TEXT("Gameplay starter declares begin overlap callback"), InitialSourceText.Contains(TEXT("public static void OnBeginOverlap(AActor otherActor, FVector location)")));
	TestTrue(TEXT("Gameplay starter declares end overlap callback"), InitialSourceText.Contains(TEXT("public static void OnEndOverlap(AActor otherActor, FVector location)")));
	TestTrue(TEXT("Gameplay starter declares hit callback"), InitialSourceText.Contains(TEXT("public static void OnHit(AActor otherActor, FVector normalImpulse)")));
	TestTrue(TEXT("Gameplay starter declares input callback"), InitialSourceText.Contains(TEXT("public static void OnInput(InputEvent input)")));
	TestFalse(TEXT("Gameplay starter does not expose the raw event router"), InitialSourceText.Contains(TEXT("OnGameplayEvent(")));

    FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
    TestTrue(
        TEXT("Workspace profile loads through ProfileService"),
        FAvidScriptEditorCSharpProfileService::LoadProfile(First.ProfilePath, ProfileResult));
    TestEqual(TEXT("Profile source is project-owned source"), ProfileResult.BuildConfig.SourcePath, First.SourcePath);
    TestEqual(TEXT("Profile project is project-owned project"), ProfileResult.BuildConfig.ProjectPath, First.ProjectPath);
    TestEqual(TEXT("Profile output is Saved-owned output"), ProfileResult.BuildConfig.OutputRoot, First.OutputRoot);
    TestEqual(TEXT("Profile report is deterministic"), ProfileResult.BuildConfig.ReportPath, First.ReportPath);
    TestEqual(TEXT("Profile manifest is deterministic"), ProfileResult.BuildConfig.ManifestPath, First.ManifestPath);
    TestTrue(TEXT("Profile uses automatic runtime binding slicing"), ProfileResult.BuildConfig.BindingPackagePath.IsEmpty());

    const FString UserMarker = TEXT("// user-owned marker\n");
    TestTrue(
        TEXT("User source marker can be written"),
        FFileHelper::SaveStringToFile(
            UserMarker,
            *First.SourcePath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    const FString FacadeMarker = TEXT("// stale generated facade\n");
    TestTrue(
        TEXT("Stale facade marker can be written"),
        FFileHelper::SaveStringToFile(
            FacadeMarker,
            *First.FacadePath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

    FAvidScriptEditorCSharpWorkspaceResult Second;
    TestTrue(
        TEXT("Project C# workspace refreshes"),
        FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(Config, Second));
    TestEqual(TEXT("Second refresh creates no user files"), Second.CreatedUserFileCount, 0);
    TestEqual(TEXT("Second refresh preserves four user files"), Second.PreservedUserFileCount, 4);
    TestEqual(
        TEXT("User source is preserved byte-for-byte"),
        ReadAvidScriptWorkspaceTestText(*this, Second.SourcePath),
        UserMarker);
    const FString RefreshedFacade = ReadAvidScriptWorkspaceTestText(*this, Second.FacadePath);
    TestTrue(TEXT("Generated facade is refreshed"), RefreshedFacade.Contains(TEXT("public static class UE")));
	TestTrue(TEXT("Generated facade retains state contract authoring surface"), RefreshedFacade.Contains(TEXT("public enum AvidStateMode")));
    TestFalse(TEXT("Stale facade marker is removed"), RefreshedFacade.Contains(TEXT("stale generated facade")));

    FAvidScriptEditorCSharpWorkspaceConfig OverwriteConfig = Config;
    OverwriteConfig.bOverwriteUserFiles = true;
    FAvidScriptEditorCSharpWorkspaceResult Overwritten;
    TestTrue(
        TEXT("Explicit overwrite refreshes user templates"),
        FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(OverwriteConfig, Overwritten));
    TestEqual(TEXT("Explicit overwrite updates four user files"), Overwritten.UpdatedUserFileCount, 4);
	const FString StarterText = ReadAvidScriptWorkspaceTestText(*this, Overwritten.SourcePath)
		.Replace(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
    TestTrue(
        TEXT("Explicit overwrite restores gameplay starter"),
        StarterText.Contains(TEXT("RotationSpeedDegreesPerSecond")));
	TestTrue(TEXT("Gameplay starter enables explicit state contracts"), StarterText.Contains(TEXT("[AvidStateContract(AvidStateMode.Explicit)]")));
	TestTrue(TEXT("Gameplay starter persists accumulated rotation"), StarterText.Contains(TEXT("[AvidPersist]\n    private static float TotalRotationDegrees;")));
	TestFalse(TEXT("Gameplay starter does not persist rotation speed configuration"), StarterText.Contains(TEXT("[AvidPersist]\n    private const float RotationSpeedDegreesPerSecond")));
	TestTrue(TEXT("Explicit overwrite restores natural gameplay callbacks"), StarterText.Contains(TEXT("public static void OnInput(InputEvent input)")));
	TestFalse(TEXT("Explicit overwrite does not restore the raw event router"), StarterText.Contains(TEXT("OnGameplayEvent(")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAvidScriptEditorCSharpWorkspaceSafetyTest,
    "AvidScript.Editor.CSharpWorkspaceService.PathAndTemplateSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpWorkspaceSafetyTest::RunTest(const FString& Parameters)
{
    FAvidScriptEditorCSharpWorkspaceConfig OutsideConfig;
    OutsideConfig.WorkspaceRoot = FPaths::Combine(
        FPaths::ProjectDir(),
        TEXT(".."),
        TEXT("AvidScriptP44OutsideProject"));
    FAvidScriptEditorCSharpWorkspaceResult OutsideResult;
    TestFalse(
        TEXT("Workspace outside project is rejected"),
        FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(OutsideConfig, OutsideResult));
    TestEqual(
        TEXT("Outside project category"),
        OutsideResult.ErrorCategory,
        FString(TEXT("workspace_path_outside_project")));

    FAvidScriptEditorCSharpWorkspaceConfig MissingTemplateConfig = MakeAvidScriptWorkspaceTestConfig(TEXT("MissingTemplate"));
    MissingTemplateConfig.TemplateRoot = FPaths::Combine(
        GetAvidScriptWorkspaceTestRoot(),
        TEXT("DoesNotExist"));
    FAvidScriptEditorCSharpWorkspaceResult MissingTemplateResult;
    TestFalse(
        TEXT("Missing template root is rejected"),
        FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(MissingTemplateConfig, MissingTemplateResult));
    TestEqual(
        TEXT("Missing template category"),
        MissingTemplateResult.ErrorCategory,
        FString(TEXT("workspace_template_missing")));
    TestFalse(
        TEXT("Missing template does not create source"),
        FPaths::FileExists(MissingTemplateResult.SourcePath));

    FAvidScriptEditorCSharpWorkspaceConfig ConflictConfig = MakeAvidScriptWorkspaceTestConfig(TEXT("DirectoryConflict"));
    const FString ConflictingSourcePath = FPaths::Combine(
        ConflictConfig.WorkspaceRoot,
        TEXT("GameplayScript.cs"));
    TestTrue(
        TEXT("Conflicting source directory can be created"),
        IFileManager::Get().MakeDirectory(*ConflictingSourcePath, true));
    FAvidScriptEditorCSharpWorkspaceResult ConflictResult;
    TestFalse(
        TEXT("Directory at source path is rejected"),
        FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(ConflictConfig, ConflictResult));
    TestEqual(
        TEXT("Directory conflict category"),
        ConflictResult.ErrorCategory,
        FString(TEXT("workspace_file_is_directory")));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
