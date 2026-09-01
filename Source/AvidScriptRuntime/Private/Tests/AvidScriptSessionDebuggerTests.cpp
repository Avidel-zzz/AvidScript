#include "Debugging/AvidScriptSessionDebugger.h"
#include "AvidScriptWasmRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptSessionDebuggerStateMachineTest,
	"AvidScript.Runtime.Diagnostics.SessionDebuggerStateMachine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSessionDebuggerStateMachineTest::RunTest(const FString& Parameters)
{
	constexpr uint64 BreakpointProbe = 0x123456789abcdef0ULL;
	constexpr uint64 NextProbe = 0x0fedcba987654321ULL;
	const uint64 Breakpoints[] = { BreakpointProbe, BreakpointProbe };
	FAvidScriptSessionDebugger Debugger;

	TestTrue(TEXT("Debugger attaches"), Debugger.Attach(Breakpoints));
	TestEqual(TEXT("Breakpoints are deduplicated"), Debugger.GetSnapshot().BreakpointCount, 1);
	TestEqual(
		TEXT("Unmatched probe continues"),
		Debugger.EvaluateProbe(NextProbe),
		EAvidScriptDebugProbeAction::Continue);
	TestEqual(
		TEXT("Breakpoint probe pauses"),
		Debugger.EvaluateProbe(BreakpointProbe),
		EAvidScriptDebugProbeAction::Pause);
	TestEqual(
		TEXT("Pause snapshot retains the probe"),
		Debugger.GetSnapshot().ActiveProbeId,
		BreakpointProbe);

	TestTrue(TEXT("Paused execution continues"), Debugger.ContinueExecution());
	TestEqual(
		TEXT("Continue suppresses the resumed probe once"),
		Debugger.EvaluateProbe(BreakpointProbe),
		EAvidScriptDebugProbeAction::Continue);
	TestEqual(
		TEXT("A later visit to the breakpoint pauses again"),
		Debugger.EvaluateProbe(BreakpointProbe),
		EAvidScriptDebugProbeAction::Pause);

	TestTrue(TEXT("Paused execution accepts step into"), Debugger.StepInto());
	TestEqual(
		TEXT("Step suppresses the resumed probe once"),
		Debugger.EvaluateProbe(BreakpointProbe),
		EAvidScriptDebugProbeAction::Continue);
	TestEqual(
		TEXT("Step pauses on the next probe"),
		Debugger.EvaluateProbe(NextProbe),
		EAvidScriptDebugProbeAction::Pause);

	const uint64 PreviousEpoch = Debugger.GetSnapshot().Epoch;
	Debugger.OnRuntimeGenerationChanged();
	TestTrue(TEXT("Runtime replacement advances the debug epoch"), Debugger.GetSnapshot().Epoch > PreviousEpoch);
	TestEqual(
		TEXT("Runtime replacement clears a stale pause"),
		Debugger.GetSnapshot().State,
		EAvidScriptDebugSessionState::Running);
	TestEqual(
		TEXT("Breakpoints survive runtime replacement"),
		Debugger.EvaluateProbe(BreakpointProbe),
		EAvidScriptDebugProbeAction::Pause);

	Debugger.Detach();
	TestEqual(
		TEXT("Detached probes continue"),
		Debugger.EvaluateProbe(BreakpointProbe),
		EAvidScriptDebugProbeAction::Continue);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeDebugProbeRouteTest,
	"AvidScript.Runtime.Diagnostics.DebugProbeSessionRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeDebugProbeRouteTest::RunTest(const FString& Parameters)
{
	constexpr uint64 BreakpointProbe = 0xfedcba9876543210ULL;
	const uint64 Breakpoints[] = { BreakpointProbe };
	FAvidScriptSessionDebugger Debugger;
	TestTrue(TEXT("Debugger attaches for Runtime routing"), Debugger.Attach(Breakpoints));

	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext HostContext;
	HostContext.DebugProbes = &Debugger;
	Runtime.SetHostContext(HostContext);
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::DebugProbe;
	Call.Int64Args[0] = static_cast<int64>(BreakpointProbe);
	FAvidScriptHostCallResult Result;
	TestTrue(TEXT("Runtime routes an active debug probe"), Runtime.DispatchHostCall(Call, Result));
	TestEqual(
		TEXT("Runtime returns the Session pause action"),
		Result.ReturnValue,
		static_cast<int32>(EAvidScriptDebugProbeAction::Pause));
	TestEqual(
		TEXT("Session records the routed probe"),
		Debugger.GetSnapshot().ActiveProbeId,
		BreakpointProbe);
	return true;
}

#endif
