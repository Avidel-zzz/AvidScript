#include "Debugging/AvidScriptEditorDebugTargetController.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Debugging/SAvidScriptEditorDebugPanel.h"
#include "Misc/AutomationTest.h"

namespace
{
class FAvidScriptEditorControllerRuntimeFake final : public IAvidScriptEditorDebugRuntime
{
public:
	virtual bool GetBreakpointCatalog(
		TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
		FString& OutError) const override
	{
		OutBreakpoints = Catalog;
		OutError.Reset();
		return true;
	}

	virtual bool AttachDebugger(const TConstArrayView<uint64> ProbeIds) override
	{
		Snapshot.State = EAvidScriptDebugSessionState::Running;
		Snapshot.BreakpointCount = ProbeIds.Num();
		return true;
	}

	virtual bool DetachDebugger() override
	{
		++DetachCallCount;
		Snapshot.State = EAvidScriptDebugSessionState::Detached;
		return true;
	}

	virtual bool SetBreakpoints(const TConstArrayView<uint64> ProbeIds) override
	{
		Snapshot.BreakpointCount = ProbeIds.Num();
		return true;
	}

	virtual bool RequestPause() override { return true; }
	virtual bool ContinueExecution() override { return true; }
	virtual bool StepInto() override { return true; }
	virtual FAvidScriptDebugSessionSnapshot GetSnapshot() const override { return Snapshot; }

	virtual bool GetVariables(
		FAvidScriptDebugVariablesSnapshot& OutVariables,
		FString& OutError) const override
	{
		OutVariables = FAvidScriptDebugVariablesSnapshot();
		OutError.Reset();
		return false;
	}

	TArray<FAvidScriptDebugBreakpoint> Catalog;
	FAvidScriptDebugSessionSnapshot Snapshot;
	int32 DetachCallCount = 0;
};

class FAvidScriptEditorControllerProfilerFake final
	: public IAvidScriptEditorProfilerRuntime
{
public:
	virtual void SetProfilerEnabled(const bool bEnabled) override
	{
		bEnabledState = bEnabled;
	}

	virtual bool IsProfilerEnabled() const override
	{
		return bEnabledState;
	}

	virtual void ResetProfiler() override
	{
		Snapshot.Events.Reset();
		++Snapshot.Revision;
	}

	virtual FAvidScriptProfilerSnapshot GetProfilerSnapshot() const override
	{
		return Snapshot;
	}

	virtual bool GetProfilerSourceCatalog(
		TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
		FString& OutError) const override
	{
		OutBreakpoints = Catalog;
		OutError.Reset();
		return true;
	}

	bool bEnabledState = false;
	FAvidScriptProfilerSnapshot Snapshot;
	TArray<FAvidScriptDebugBreakpoint> Catalog;
};

FAvidScriptDebugBreakpoint MakeControllerBreakpoint(const uint64 ProbeId)
{
	FAvidScriptDebugBreakpoint Breakpoint;
	Breakpoint.ProbeId = ProbeId;
	Breakpoint.SourceFile = TEXT("Scripts/AvidScript/Controller.cs");
	Breakpoint.FunctionName = TEXT("Controller.Tick(float)");
	Breakpoint.Kind = TEXT("statement");
	Breakpoint.Line = 12;
	Breakpoint.Column = 3;
	return Breakpoint;
}

FAvidScriptEditorDebugTarget MakeControllerTarget(
	const FString& TargetId,
	const TSharedRef<FAvidScriptEditorControllerRuntimeFake>& Runtime,
	const TSharedPtr<FAvidScriptEditorControllerProfilerFake>& Profiler = nullptr)
{
	FAvidScriptEditorDebugTarget Target;
	Target.TargetId = TargetId;
	Target.DisplayName = TargetId;
	Target.WorldName = TEXT("PIE_TestWorld");
	Target.ModuleId = TEXT("csharp:Scripts/AvidScript/Controller.cs");
	Target.RuntimeIdentity = &Runtime.Get();
	Target.CreateRuntime = [Runtime]() -> TSharedPtr<IAvidScriptEditorDebugRuntime>
	{
		return StaticCastSharedRef<IAvidScriptEditorDebugRuntime>(Runtime);
	};
	if (Profiler.IsValid())
	{
		Target.CreateProfilerRuntime = [Profiler]()
			-> TSharedPtr<IAvidScriptEditorProfilerRuntime>
		{
			return StaticCastSharedPtr<IAvidScriptEditorProfilerRuntime>(Profiler);
		};
	}
	return Target;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorDebugTargetControllerTest,
	"AvidScript.Editor.Debugging.TargetController",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorDebugTargetControllerTest::RunTest(const FString& Parameters)
{
	TSharedRef<FAvidScriptEditorControllerRuntimeFake> FirstRuntime =
		MakeShared<FAvidScriptEditorControllerRuntimeFake>();
	FirstRuntime->Snapshot.Epoch = 1;
	FirstRuntime->Catalog.Add(MakeControllerBreakpoint(0x1111111111111111ULL));
	TSharedRef<FAvidScriptEditorControllerRuntimeFake> SecondRuntime =
		MakeShared<FAvidScriptEditorControllerRuntimeFake>();
	SecondRuntime->Snapshot.Epoch = 2;
	SecondRuntime->Catalog.Add(MakeControllerBreakpoint(0x2222222222222222ULL));
	TSharedRef<FAvidScriptEditorControllerProfilerFake> FirstProfiler =
		MakeShared<FAvidScriptEditorControllerProfilerFake>();
	FirstProfiler->Snapshot.SecondsPerCycle = 1.0e-6;
	FirstProfiler->Snapshot.Events.AddDefaulted();
	FirstProfiler->Snapshot.Events[0].Kind = EAvidScriptProfilerEventKind::GuestCall;
	FirstProfiler->Snapshot.Events[0].DurationCycles = 3;
	FirstProfiler->Catalog = FirstRuntime->Catalog;
	TSharedRef<FAvidScriptEditorControllerProfilerFake> SecondProfiler =
		MakeShared<FAvidScriptEditorControllerProfilerFake>();
	SecondProfiler->Snapshot.SecondsPerCycle = 1.0e-6;
	SecondProfiler->Snapshot.Events.AddDefaulted();
	SecondProfiler->Snapshot.Events[0].Kind = EAvidScriptProfilerEventKind::HostCall;
	SecondProfiler->Snapshot.Events[0].DurationCycles = 5;
	SecondProfiler->Catalog = SecondRuntime->Catalog;

	bool bFirstAvailable = true;
	bool bSecondAvailable = false;
	FAvidScriptEditorDebugTargetController Controller(
		[&](TArray<FAvidScriptEditorDebugTarget>& OutTargets)
		{
			OutTargets.Reset();
			if (bFirstAvailable)
			{
				OutTargets.Add(MakeControllerTarget(TEXT("TargetA"), FirstRuntime, FirstProfiler));
			}
			if (bSecondAvailable)
			{
				OutTargets.Add(MakeControllerTarget(TEXT("TargetB"), SecondRuntime, SecondProfiler));
			}
		});

	FString Error;
	TestTrue(
		TEXT("breakpoint can be authored before PIE"),
		Controller.SetSourceBreakpoint(
			TEXT("Scripts/AvidScript/Controller.cs"),
			12,
			true,
			Error));
	Controller.HandleBeginPIE();
	TestTrue(TEXT("PIE tick discovers and binds first target"), Controller.Tick(Error));
	TestEqual(TEXT("one target is visible"), Controller.GetTargets().Num(), 1);
	TestEqual(TEXT("first target is auto-selected"), Controller.GetSelectedTargetId(), FString(TEXT("TargetA")));
	TestTrue(TEXT("session model is Runtime-bound"), Controller.GetSessionModel().GetView().bRuntimeBound);
	TestTrue(TEXT("profiler model is Runtime-bound"), Controller.GetProfilerModel().GetView().bRuntimeBound);
	TestEqual(TEXT("first target profiler snapshot is visible"), Controller.GetProfilerModel().GetView().SourceEventCount, 1);
	TestTrue(TEXT("profiler capture enables through the Controller"), Controller.SetProfilerCaptureEnabled(true, Error));
	TestTrue(TEXT("first target profiler receives capture state"), FirstProfiler->bEnabledState);
	TestEqual(
		TEXT("offline breakpoint resolves on selected target"),
		Controller.GetSessionModel().GetView().Breakpoints[0].ProbeId,
		0x1111111111111111ULL);
	TestTrue(TEXT("debugger attaches to selected target"), Controller.AttachDebugger(Error));

	bFirstAvailable = false;
	bSecondAvailable = true;
	TestTrue(TEXT("target replacement is handled without stale access"), Controller.Tick(Error));
	TestEqual(TEXT("replacement target is selected"), Controller.GetSelectedTargetId(), FString(TEXT("TargetB")));
	TestEqual(
		TEXT("replacement Runtime rebinds source breakpoint"),
		Controller.GetSessionModel().GetView().Breakpoints[0].ProbeId,
		0x2222222222222222ULL);
	TestTrue(TEXT("replacement profiler is rebound"), Controller.GetProfilerModel().GetView().bRuntimeBound);
	TestEqual(TEXT("replacement profiler snapshot is visible"), Controller.GetProfilerModel().GetView().SourceEventCount, 1);
	TestFalse(TEXT("replacement profiler starts with its own capture state"), SecondProfiler->bEnabledState);
	TestEqual(
		TEXT("disappearing Runtime is invalidated without a detach call"),
		FirstRuntime->DetachCallCount,
		0);

	TestTrue(TEXT("replacement debugger attaches"), Controller.AttachDebugger(Error));
	Controller.HandleEndPIE();
	TestFalse(TEXT("PIE end clears active target"), Controller.IsPIEActive());
	TestTrue(TEXT("PIE end clears target list"), Controller.GetTargets().IsEmpty());
	TestFalse(TEXT("PIE end clears Runtime binding"), Controller.GetSessionModel().GetView().bRuntimeBound);
	TestFalse(TEXT("PIE end clears profiler binding"), Controller.GetProfilerModel().GetView().bRuntimeBound);
	TestEqual(
		TEXT("PIE teardown invalidates instead of dereferencing Runtime"),
		SecondRuntime->DetachCallCount,
		0);
	TestFalse(
		TEXT("PIE end preserves user breakpoint requests"),
		Controller.GetSessionModel().GetView().Breakpoints.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorDebugPanelConstructionTest,
	"AvidScript.Editor.Debugging.PanelConstruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorDebugPanelConstructionTest::RunTest(const FString& Parameters)
{
	TSharedRef<FAvidScriptEditorDebugTargetController> Controller =
		MakeShared<FAvidScriptEditorDebugTargetController>();
	TSharedRef<SWidget> Panel = MakeAvidScriptEditorDebugPanel(Controller);

	TestTrue(TEXT("debugger panel factory returns a widget"), Panel->GetType() != NAME_None);
	TestFalse(TEXT("debugger panel construction does not activate PIE"), Controller->IsPIEActive());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
