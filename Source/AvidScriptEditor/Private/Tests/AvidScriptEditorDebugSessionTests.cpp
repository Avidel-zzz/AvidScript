#include "Debugging/AvidScriptEditorDebugSession.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
class FAvidScriptEditorDebugRuntimeFake final : public IAvidScriptEditorDebugRuntime
{
public:
	virtual bool GetBreakpointCatalog(
		TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
		FString& OutError) const override
	{
		OutBreakpoints = Catalog;
		OutError.Reset();
		return bCatalogAvailable;
	}

	virtual bool AttachDebugger(const TConstArrayView<uint64> ProbeIds) override
	{
		if (Snapshot.State != EAvidScriptDebugSessionState::Detached)
		{
			return false;
		}
		LastProbeIds.Reset(ProbeIds.Num());
		LastProbeIds.Append(ProbeIds.GetData(), ProbeIds.Num());
		Snapshot.State = EAvidScriptDebugSessionState::Running;
		Snapshot.BreakpointCount = LastProbeIds.Num();
		return true;
	}

	virtual bool DetachDebugger() override
	{
		if (Snapshot.State == EAvidScriptDebugSessionState::Paused)
		{
			return false;
		}
		Snapshot = FAvidScriptDebugSessionSnapshot();
		LastProbeIds.Reset();
		return true;
	}

	virtual bool SetBreakpoints(const TConstArrayView<uint64> ProbeIds) override
	{
		if (Snapshot.State == EAvidScriptDebugSessionState::Detached)
		{
			return false;
		}
		LastProbeIds.Reset(ProbeIds.Num());
		LastProbeIds.Append(ProbeIds.GetData(), ProbeIds.Num());
		Snapshot.BreakpointCount = LastProbeIds.Num();
		++SetBreakpointCallCount;
		return true;
	}

	virtual bool RequestPause() override
	{
		if (Snapshot.State != EAvidScriptDebugSessionState::Running)
		{
			return false;
		}
		Snapshot.State = EAvidScriptDebugSessionState::Suspending;
		Snapshot.RunMode = EAvidScriptDebugRunMode::PauseNext;
		return true;
	}

	virtual bool ContinueExecution() override
	{
		if (Snapshot.State != EAvidScriptDebugSessionState::Paused)
		{
			return false;
		}
		Snapshot.State = EAvidScriptDebugSessionState::Running;
		Snapshot.RunMode = EAvidScriptDebugRunMode::Continue;
		Snapshot.ActiveProbeId = 0;
		return true;
	}

	virtual bool StepInto() override
	{
		if (Snapshot.State != EAvidScriptDebugSessionState::Paused)
		{
			return false;
		}
		Snapshot.State = EAvidScriptDebugSessionState::Running;
		Snapshot.RunMode = EAvidScriptDebugRunMode::StepInto;
		Snapshot.ActiveProbeId = 0;
		return true;
	}

	virtual FAvidScriptDebugSessionSnapshot GetSnapshot() const override
	{
		return Snapshot;
	}

	virtual bool GetVariables(
		FAvidScriptDebugVariablesSnapshot& OutVariables,
		FString& OutError) const override
	{
		OutVariables = Variables;
		OutError.Reset();
		return bVariablesAvailable;
	}

	void PauseAt(const uint64 ProbeId, const uint64 PauseSequence)
	{
		Snapshot.State = EAvidScriptDebugSessionState::Paused;
		Snapshot.ActiveProbeId = ProbeId;
		Snapshot.PauseSequence = PauseSequence;
		Variables.Epoch = Snapshot.Epoch;
		Variables.ActiveProbeId = ProbeId;
		Variables.PauseSequence = PauseSequence;
	}

	TArray<FAvidScriptDebugBreakpoint> Catalog;
	TArray<uint64> LastProbeIds;
	FAvidScriptDebugSessionSnapshot Snapshot;
	FAvidScriptDebugVariablesSnapshot Variables;
	int32 SetBreakpointCallCount = 0;
	bool bCatalogAvailable = true;
	bool bVariablesAvailable = true;
};

FAvidScriptDebugBreakpoint MakeEditorDebugBreakpoint(
	const uint64 ProbeId,
	const int32 Line,
	const int32 Column)
{
	FAvidScriptDebugBreakpoint Breakpoint;
	Breakpoint.ProbeId = ProbeId;
	Breakpoint.SourceFile = TEXT("Scripts/AvidScript/Player.cs");
	Breakpoint.SourceSha256 = FString::ChrN(64, TEXT('a'));
	Breakpoint.FunctionName = TEXT("Player.Tick(float)");
	Breakpoint.Kind = TEXT("statement");
	Breakpoint.Line = Line;
	Breakpoint.Column = Column;
	Breakpoint.EndLine = Line;
	Breakpoint.EndColumn = Column + 4;
	return Breakpoint;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorDebugSessionModelTest,
	"AvidScript.Editor.Debugging.SessionModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorDebugSessionModelTest::RunTest(const FString& Parameters)
{
	constexpr uint64 FirstProbe = 0x1111111111111111ULL;
	constexpr uint64 ReloadedProbe = 0x2222222222222222ULL;
	FAvidScriptEditorDebugSessionModel Model;
	FString Error;

	TestTrue(
		TEXT("source breakpoint can be authored before a PIE Session exists"),
		Model.SetSourceBreakpoint(TEXT("Scripts\\AvidScript\\Player.cs"), 20, true, Error));
	TestFalse(
		TEXT("absolute breakpoint source is rejected"),
		Model.SetSourceBreakpoint(TEXT("C:/Private/Player.cs"), 20, true, Error));
	TestEqual(TEXT("offline breakpoint remains pending"), Model.GetView().Breakpoints.Num(), 1);

	FAvidScriptEditorDebugRuntimeFake Runtime;
	Runtime.Snapshot.Epoch = 7;
	Runtime.Catalog.Add(MakeEditorDebugBreakpoint(FirstProbe, 20, 8));
	Runtime.Catalog.Add(MakeEditorDebugBreakpoint(0x3333333333333333ULL, 30, 4));
	TestTrue(TEXT("live Runtime Session binds"), Model.BindRuntime(Runtime, Error));
	TestTrue(TEXT("view reports a bound Runtime"), Model.GetView().bRuntimeBound);
	TestTrue(TEXT("pending source breakpoint resolves"), Model.GetView().Breakpoints[0].IsBound());
	TestEqual(TEXT("resolved breakpoint uses the validated probe"), Model.GetView().Breakpoints[0].ProbeId, FirstProbe);

	TestTrue(TEXT("debugger attaches without blocking"), Model.AttachDebugger(Error));
	TestEqual(TEXT("attach sends one enabled probe"), Runtime.LastProbeIds.Num(), 1);
	TestEqual(TEXT("attach sends the resolved probe"), Runtime.LastProbeIds[0], FirstProbe);
	TestTrue(
		TEXT("attached breakpoint can be disabled"),
		Model.SetSourceBreakpoint(TEXT("Scripts/AvidScript/Player.cs"), 20, false, Error));
	TestTrue(TEXT("disabled breakpoint is removed from Runtime"), Runtime.LastProbeIds.IsEmpty());
	TestTrue(
		TEXT("attached breakpoint can be re-enabled"),
		Model.SetSourceBreakpoint(TEXT("Scripts/AvidScript/Player.cs"), 20, true, Error));

	TestTrue(TEXT("pause-next is forwarded"), Model.RequestPause(Error));
	TestEqual(
		TEXT("view observes the non-blocking suspending state"),
		Model.GetView().Runtime.State,
		EAvidScriptDebugSessionState::Suspending);
	Runtime.PauseAt(FirstProbe, 3);
	FAvidScriptDebugVariableSnapshot& Variable = Runtime.Variables.Variables.AddDefaulted_GetRef();
	Variable.Name = TEXT("deltaSeconds");
	Variable.TypeId = TEXT("type:float32");
	Variable.Value = TEXT("0.016667");
	TestTrue(TEXT("paused Session refreshes variables"), Model.Refresh(Error));
	TestEqual(TEXT("one paused variable is visible"), Model.GetView().Variables.Variables.Num(), 1);

	Runtime.Variables.PauseSequence = 2;
	TestFalse(TEXT("stale variable snapshot is rejected"), Model.Refresh(Error));
	TestTrue(TEXT("stale variables are cleared"), Model.GetView().Variables.Variables.IsEmpty());
	Runtime.Variables.PauseSequence = 3;
	TestTrue(TEXT("matching variables recover"), Model.Refresh(Error));
	TestTrue(TEXT("continue is forwarded"), Model.ContinueExecution(Error));
	TestTrue(TEXT("continue invalidates Editor variables"), Model.GetView().Variables.Variables.IsEmpty());

	Runtime.Snapshot.Epoch = 8;
	Runtime.Catalog.Reset();
	Runtime.Catalog.Add(MakeEditorDebugBreakpoint(ReloadedProbe, 20, 6));
	const int32 SetCallsBeforeReload = Runtime.SetBreakpointCallCount;
	TestTrue(TEXT("reload epoch refreshes breakpoint catalog"), Model.Refresh(Error));
	TestEqual(TEXT("source breakpoint rebinds after reload"), Model.GetView().Breakpoints[0].ProbeId, ReloadedProbe);
	TestTrue(TEXT("attached Runtime receives reloaded probe set"), Runtime.SetBreakpointCallCount > SetCallsBeforeReload);
	TestEqual(TEXT("reloaded probe reaches Runtime"), Runtime.LastProbeIds[0], ReloadedProbe);

	TestTrue(TEXT("debugger detaches"), Model.DetachDebugger(Error));
	TestTrue(TEXT("model unbinds without owning Runtime lifetime"), Model.UnbindRuntime(Error));
	TestFalse(TEXT("view clears Runtime binding"), Model.GetView().bRuntimeBound);
	TestFalse(TEXT("user breakpoint survives Session teardown"), Model.GetView().Breakpoints.IsEmpty());
	TestFalse(TEXT("surviving breakpoint becomes unresolved"), Model.GetView().Breakpoints[0].IsBound());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
