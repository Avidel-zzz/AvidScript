#if WITH_DEV_AUTOMATION_TESTS

#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadCoordinator.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
FString MakeAvidScriptLiveReloadCoordinatorTestRoot(const FString& CaseName)
{
	FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript"),
		TEXT("Tests"),
		TEXT("P45"),
		TEXT("Coordinator"),
		CaseName));
	FPaths::NormalizeFilename(Root);
	IFileManager::Get().MakeDirectory(*Root, true);
	return Root;
}

FAvidScriptEditorCSharpLiveReloadCoordinatorConfig MakeAvidScriptLiveReloadCoordinatorTestConfig(
	const FString& CaseName)
{
	FAvidScriptEditorCSharpLiveReloadCoordinatorConfig Config;
	Config.WorkspaceRoot = MakeAvidScriptLiveReloadCoordinatorTestRoot(CaseName);
	Config.DebounceSeconds = 0.35;
	return Config;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadCoordinatorFilteringDebounceTest,
	"AvidScript.Editor.CSharpLiveReload.Coordinator.FilteringAndDebounce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadCoordinatorFilteringDebounceTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpLiveReloadCoordinator Coordinator;
	const FAvidScriptEditorCSharpLiveReloadCoordinatorConfig Config =
		MakeAvidScriptLiveReloadCoordinatorTestConfig(TEXT("FilteringDebounce"));
	FString ErrorCategory;
	FString ErrorMessage;
	if (!TestTrue(TEXT("Coordinator starts"), Coordinator.Start(Config, ErrorCategory, ErrorMessage)))
	{
		AddError(ErrorCategory + TEXT(": ") + ErrorMessage);
		return false;
	}

	const FString FirstSource = FPaths::Combine(Config.WorkspaceRoot, TEXT("GameplayScript.cs"));
	const FString SecondSource = FPaths::Combine(Config.WorkspaceRoot, TEXT("Player"), TEXT("Movement.cs"));
	const FString IgnoredText = FPaths::Combine(Config.WorkspaceRoot, TEXT("notes.txt"));
	const FString OutsideSource = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Outside.cs"));

	TestFalse(
		TEXT("Outside and unrelated paths are ignored"),
		Coordinator.NotifyFileChanges({OutsideSource, IgnoredText}, 1.0));
	TestTrue(TEXT("First source schedules build"), Coordinator.NotifyFileChanges({FirstSource}, 1.0));
	TestFalse(TEXT("Build does not start before first deadline"), Coordinator.TryBeginBuild(1.349).IsSet());
	TestTrue(TEXT("Second save extends debounce"), Coordinator.NotifyFileChanges({SecondSource}, 1.2));
	TestFalse(TEXT("Build does not start at stale deadline"), Coordinator.TryBeginBuild(1.35).IsSet());
	TestFalse(TEXT("Build does not start before extended deadline"), Coordinator.TryBeginBuild(1.549).IsSet());

	const TOptional<FAvidScriptEditorCSharpLiveReloadBuildRequest> Request = Coordinator.TryBeginBuild(1.55);
	if (!TestTrue(TEXT("Build starts at extended deadline"), Request.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("Request contains both changed sources"), Request->ChangedFiles.Num(), 2);
	TestEqual(TEXT("Observed path count"), Coordinator.GetStats().ObservedFileCount, 4);
	TestEqual(TEXT("Ignored path count"), Coordinator.GetStats().IgnoredFileCount, 2);
	TestEqual(TEXT("Relevant batch count"), Coordinator.GetStats().RelevantChangeBatchCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadCoordinatorSingleFlightTest,
	"AvidScript.Editor.CSharpLiveReload.Coordinator.SingleFlightTrailingBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadCoordinatorSingleFlightTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpLiveReloadCoordinator Coordinator;
	const FAvidScriptEditorCSharpLiveReloadCoordinatorConfig Config =
		MakeAvidScriptLiveReloadCoordinatorTestConfig(TEXT("SingleFlight"));
	FString ErrorCategory;
	FString ErrorMessage;
	TestTrue(TEXT("Coordinator starts"), Coordinator.Start(Config, ErrorCategory, ErrorMessage));

	const FString FirstSource = FPaths::Combine(Config.WorkspaceRoot, TEXT("First.cs"));
	const FString SecondSource = FPaths::Combine(Config.WorkspaceRoot, TEXT("Second.cs"));
	const FString ProjectFile = FPaths::Combine(Config.WorkspaceRoot, TEXT("AvidScript.Gameplay.csproj"));
	Coordinator.NotifyFileChanges({FirstSource}, 2.0);
	const TOptional<FAvidScriptEditorCSharpLiveReloadBuildRequest> FirstRequest = Coordinator.TryBeginBuild(2.35);
	if (!TestTrue(TEXT("First request starts"), FirstRequest.IsSet()))
	{
		return false;
	}

	TestTrue(TEXT("Change during build is accepted"), Coordinator.NotifyFileChanges({SecondSource}, 2.4));
	TestTrue(TEXT("Another change during build is accepted"), Coordinator.NotifyFileChanges({ProjectFile}, 2.5));
	TestFalse(TEXT("No concurrent request starts"), Coordinator.TryBeginBuild(10.0).IsSet());
	TestTrue(TEXT("First request completes"), Coordinator.CompleteBuild(*FirstRequest, true));
	TestFalse(TEXT("Trailing build still honors debounce"), Coordinator.TryBeginBuild(2.849).IsSet());

	const TOptional<FAvidScriptEditorCSharpLiveReloadBuildRequest> TrailingRequest = Coordinator.TryBeginBuild(2.85);
	if (!TestTrue(TEXT("One trailing request starts"), TrailingRequest.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("Trailing request merges build-time changes"), TrailingRequest->ChangedFiles.Num(), 2);
	TestTrue(TEXT("Trailing request completes as failure"), Coordinator.CompleteBuild(*TrailingRequest, false));
	TestEqual(TEXT("Two builds started"), Coordinator.GetStats().BuildStartedCount, 2);
	TestEqual(TEXT("One build succeeded"), Coordinator.GetStats().BuildSucceededCount, 1);
	TestEqual(TEXT("One build failed"), Coordinator.GetStats().BuildFailedCount, 1);
	TestEqual(TEXT("Two build-time batches coalesced"), Coordinator.GetStats().CoalescedChangeBatchCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadCoordinatorSessionTest,
	"AvidScript.Editor.CSharpLiveReload.Coordinator.StopInvalidatesCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadCoordinatorSessionTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpLiveReloadCoordinator Coordinator;
	const FAvidScriptEditorCSharpLiveReloadCoordinatorConfig Config =
		MakeAvidScriptLiveReloadCoordinatorTestConfig(TEXT("Session"));
	FString ErrorCategory;
	FString ErrorMessage;
	TestTrue(TEXT("First session starts"), Coordinator.Start(Config, ErrorCategory, ErrorMessage));
	const FString Source = FPaths::Combine(Config.WorkspaceRoot, TEXT("GameplayScript.cs"));
	Coordinator.NotifyFileChanges({Source}, 3.0);
	const TOptional<FAvidScriptEditorCSharpLiveReloadBuildRequest> OldRequest = Coordinator.TryBeginBuild(3.35);
	if (!TestTrue(TEXT("Old request starts"), OldRequest.IsSet()))
	{
		return false;
	}

	Coordinator.Stop();
	TestFalse(TEXT("Coordinator stops"), Coordinator.IsRunning());
	TestEqual(TEXT("Active build is counted canceled"), Coordinator.GetStats().BuildCanceledCount, 1);
	TestFalse(TEXT("Stopped session rejects old completion"), Coordinator.CompleteBuild(*OldRequest, true));

	TestTrue(TEXT("Second session starts"), Coordinator.Start(Config, ErrorCategory, ErrorMessage));
	Coordinator.NotifyFileChanges({Source}, 4.0);
	const TOptional<FAvidScriptEditorCSharpLiveReloadBuildRequest> NewRequest = Coordinator.TryBeginBuild(4.35);
	if (!TestTrue(TEXT("New request starts"), NewRequest.IsSet()))
	{
		return false;
	}
	TestNotEqual(
		TEXT("New session generation differs"),
		NewRequest->SessionGeneration,
		OldRequest->SessionGeneration);
	TestFalse(TEXT("New session still rejects old completion"), Coordinator.CompleteBuild(*OldRequest, true));
	TestTrue(TEXT("New request completion is accepted"), Coordinator.CompleteBuild(*NewRequest, true));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
