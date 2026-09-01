#include "Debugging/AvidScriptEditorDebugTargetController.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptComponent.h"
#include "AvidScriptRuntimeSession.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
FString GetDebuggerPIEManifestPath()
{
	FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest/DebuggerPIE/actor_lifecycle.avidscript.json")));
	FPaths::NormalizeFilename(Path);
	return Path;
}

bool CreateDebuggerPIEWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(
		EWorldType::PIE,
		false,
		TEXT("AvidScriptEditorDebuggerPIEWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::PIE);
	WorldContext.SetCurrentWorld(OutWorld);
	const FURL Url;
	OutWorld->InitializeActorsForPlay(Url);
	OutWorld->BeginPlay();
	OutWorld->SetBegunPlay(true);
	return true;
}

void DestroyDebuggerPIEWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}
	if (World->HasBegunPlay())
	{
		World->EndPlay(EEndPlayReason::Quit);
	}
	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}
	World->DestroyWorld(false);
	World = nullptr;
}

UAvidScriptComponent* AddDebuggerPIEComponent(
	AActor& Actor,
	const FString& ManifestPath)
{
	USceneComponent* Root = NewObject<USceneComponent>(
		&Actor,
		TEXT("AvidScriptDebuggerPIERoot"));
	if (Root == nullptr)
	{
		return nullptr;
	}
	Actor.SetRootComponent(Root);
	Root->RegisterComponent();

	UAvidScriptComponent* Component = NewObject<UAvidScriptComponent>(
		&Actor,
		TEXT("AvidScriptDebuggerPIEComponent"));
	if (Component == nullptr)
	{
		return nullptr;
	}
	Actor.AddInstanceComponent(Component);
	Component->SetScriptManifestPath(ManifestPath);
	Component->RegisterComponent();
	return Component;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPIEDebugIntegrationTest,
	"AvidScript.Editor.Debugging.PIEIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPIEDebugIntegrationTest::RunTest(const FString& Parameters)
{
	const FString ManifestPath = GetDebuggerPIEManifestPath();
	if (!TestTrue(TEXT("debug PIE manifest exists"), FPaths::FileExists(ManifestPath)))
	{
		AddError(FString::Printf(
			TEXT("Build Debug ActorLifecycle before this integration test: %s"),
			*ManifestPath));
		return true;
	}

	UWorld* World = nullptr;
	if (!TestTrue(TEXT("PIE world is created"), CreateDebuggerPIEWorld(World)))
	{
		DestroyDebuggerPIEWorld(World);
		return true;
	}

	FAvidScriptEditorDebugTargetController Controller;
	Controller.HandleBeginPIE();

	AActor* Actor = World->SpawnActor<AActor>();
	TestNotNull(TEXT("debug PIE actor spawns"), Actor);
	UAvidScriptComponent* Component = Actor != nullptr
		? AddDebuggerPIEComponent(*Actor, ManifestPath)
		: nullptr;
	TestNotNull(TEXT("debug PIE component is created"), Component);
	if (Component == nullptr)
	{
		Controller.HandleEndPIE();
		DestroyDebuggerPIEWorld(World);
		return true;
	}

	TestTrue(
		TEXT("component loads the Debug C# Runtime"),
		Component->GetRuntimeStats().bRuntimeLoaded);
	FAvidScriptRuntimeSession* Runtime =
		Component->GetRuntimeSessionForEditorDebugging();
	TestNotNull(TEXT("component exposes its Editor debug Session"), Runtime);
	if (Runtime == nullptr)
	{
		Controller.HandleEndPIE();
		DestroyDebuggerPIEWorld(World);
		return true;
	}

	TArray<FAvidScriptDebugBreakpoint> Catalog;
	FString Error;
	TestTrue(
		TEXT("live Debug Runtime publishes a source breakpoint catalog"),
		Runtime->GetDebugBreakpointCatalog(Catalog, Error));
	const FAvidScriptDebugBreakpoint* TickBreakpoint = Catalog.FindByPredicate(
		[](const FAvidScriptDebugBreakpoint& Breakpoint)
		{
			return Breakpoint.FunctionName.Contains(TEXT("Tick"))
				&& Breakpoint.Line > 0;
		});
	TestNotNull(TEXT("catalog contains a Tick source breakpoint"), TickBreakpoint);
	if (TickBreakpoint == nullptr)
	{
		Controller.HandleEndPIE();
		DestroyDebuggerPIEWorld(World);
		return true;
	}

	TestTrue(TEXT("production discovery binds a PIE target"), Controller.Tick(Error));
	TestTrue(
		TEXT("the real Component target can be selected"),
		Controller.SelectTarget(Component->GetPathName(), Error));
	TestTrue(
		TEXT("source breakpoint resolves through the Editor model"),
		Controller.SetSourceBreakpoint(
			TickBreakpoint->SourceFile,
			TickBreakpoint->Line,
			true,
			Error));
	TestTrue(
		TEXT("resolved source breakpoint is bound"),
		!Controller.GetSessionModel().GetView().Breakpoints.IsEmpty()
			&& Controller.GetSessionModel().GetView().Breakpoints[0].IsBound());
	TestTrue(TEXT("debugger attaches to the real Session"), Controller.AttachDebugger(Error));
	TestTrue(TEXT("pause-next is accepted"), Controller.RequestPause(Error));

	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Editor model refreshes the paused Session"), Controller.Tick(Error));
	const FAvidScriptEditorDebugSessionView& PausedView =
		Controller.GetSessionModel().GetView();
	TestEqual(
		TEXT("real C# Tick pauses cooperatively"),
		PausedView.Runtime.State,
		EAvidScriptDebugSessionState::Paused);
	TestTrue(
		TEXT("paused Editor view exposes deltaSeconds"),
		PausedView.Variables.Variables.ContainsByPredicate(
			[](const FAvidScriptDebugVariableSnapshot& Variable)
			{
				return Variable.Name == TEXT("deltaSeconds")
					&& Variable.TypeId == TEXT("type:float32")
					&& !Variable.Value.IsEmpty();
			}));
	const uint64 InitialEpoch = PausedView.Runtime.Epoch;
	const uint64 InitialPauseSequence = PausedView.Runtime.PauseSequence;

	TestTrue(TEXT("step-into resumes through the Controller"), Controller.StepInto(Error));
	TestEqual(
		TEXT("step-into pauses at the next source point"),
		Controller.GetSessionModel().GetView().Runtime.State,
		EAvidScriptDebugSessionState::Paused);
	TestTrue(
		TEXT("step-into advances the pause sequence"),
		Controller.GetSessionModel().GetView().Runtime.PauseSequence
			> InitialPauseSequence);
	TestTrue(TEXT("continue completes the suspended Tick"), Controller.ContinueExecution(Error));
	TestEqual(
		TEXT("continue returns to Running"),
		Controller.GetSessionModel().GetView().Runtime.State,
		EAvidScriptDebugSessionState::Running);

	TestTrue(TEXT("same-manifest hot reload succeeds"), Component->ReloadScript());
	TestTrue(TEXT("Editor model refreshes after reload"), Controller.Tick(Error));
	const FAvidScriptEditorDebugSessionView& ReloadedView =
		Controller.GetSessionModel().GetView();
	TestTrue(TEXT("reload advances the Runtime epoch"), ReloadedView.Runtime.Epoch > InitialEpoch);
	TestTrue(
		TEXT("reload preserves and rebinds the source breakpoint"),
		!ReloadedView.Breakpoints.IsEmpty()
			&& ReloadedView.Breakpoints[0].IsBound());
	TestTrue(TEXT("reloaded Session accepts pause-next"), Controller.RequestPause(Error));
	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("reloaded Session publishes its pause"), Controller.Tick(Error));
	TestEqual(
		TEXT("reloaded C# Tick pauses"),
		Controller.GetSessionModel().GetView().Runtime.State,
		EAvidScriptDebugSessionState::Paused);
	TestTrue(TEXT("reloaded pause continues"), Controller.ContinueExecution(Error));

	TestTrue(TEXT("PIE World EndPlay succeeds"), World->EndPlay(EEndPlayReason::Quit));
	TestTrue(TEXT("target refresh tolerates Runtime teardown"), Controller.Tick(Error));
	TestTrue(TEXT("teardown removes the live target"), Controller.GetTargets().IsEmpty());
	TestFalse(
		TEXT("teardown invalidates the Runtime binding"),
		Controller.GetSessionModel().GetView().bRuntimeBound);
	TestFalse(
		TEXT("teardown preserves the user breakpoint"),
		Controller.GetSessionModel().GetView().Breakpoints.IsEmpty());

	Controller.HandleEndPIE();
	DestroyDebuggerPIEWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
