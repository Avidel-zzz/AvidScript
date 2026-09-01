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
	const uint8 FrameBytes[] = { 1, 3, 5, 7 };
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
		TEXT("Pending suspension retains the probe"),
		Debugger.GetSnapshot().ActiveProbeId,
		BreakpointProbe);
	TestEqual(
		TEXT("Probe decision waits for a frame commit"),
		Debugger.GetSnapshot().State,
		EAvidScriptDebugSessionState::Suspending);
	const int64 FirstToken = Debugger.CommitSuspension(
		BreakpointProbe,
		1,
		FrameBytes);
	TestTrue(TEXT("Frame commit returns a capability token"), FirstToken > 0);
	TestEqual(
		TEXT("Frame commit enters paused state"),
		Debugger.GetSnapshot().State,
		EAvidScriptDebugSessionState::Paused);

	TestTrue(TEXT("Paused execution continues"), Debugger.ContinueExecution());
	uint8 RestoredFrame[UE_ARRAY_COUNT(FrameBytes)] = {};
	TestTrue(
		TEXT("Resume consumes the exact suspension frame"),
		Debugger.ReadSuspensionFrame(FirstToken, RestoredFrame));
	TestTrue(
		TEXT("Suspension frame roundtrips"),
		FMemory::Memcmp(FrameBytes, RestoredFrame, sizeof(FrameBytes)) == 0);
	TestEqual(
		TEXT("Continue suppresses the resumed probe once"),
		Debugger.EvaluateProbe(BreakpointProbe),
		EAvidScriptDebugProbeAction::Continue);
	TestEqual(
		TEXT("A later visit to the breakpoint pauses again"),
		Debugger.EvaluateProbe(BreakpointProbe),
		EAvidScriptDebugProbeAction::Pause);
	TestTrue(
		TEXT("Second pause commits"),
		Debugger.CommitSuspension(BreakpointProbe, 2, FrameBytes) > 0);

	TestTrue(TEXT("Paused execution accepts step into"), Debugger.StepInto());
	const int64 SecondToken = Debugger.GetSnapshot().SuspensionToken;
	TestTrue(TEXT("Step resume has a suspension token"), SecondToken > 0);
	TestTrue(
		TEXT("Step resume restores its frame"),
		Debugger.ReadSuspensionFrame(SecondToken, RestoredFrame));
	TestEqual(
		TEXT("Step suppresses the resumed probe once"),
		Debugger.EvaluateProbe(BreakpointProbe),
		EAvidScriptDebugProbeAction::Continue);
	TestEqual(
		TEXT("Step pauses on the next probe"),
		Debugger.EvaluateProbe(NextProbe),
		EAvidScriptDebugProbeAction::Pause);
	TestTrue(
		TEXT("Step pause commits"),
		Debugger.CommitSuspension(NextProbe, 3, FrameBytes) > 0);

	const uint64 PreviousEpoch = Debugger.GetSnapshot().Epoch;
	Debugger.OnRuntimeGenerationChanged();
	TestTrue(TEXT("Runtime replacement advances the debug epoch"), Debugger.GetSnapshot().Epoch > PreviousEpoch);
	TestEqual(
		TEXT("Runtime replacement clears a stale suspension"),
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

	const uint8 FrameBytes[] = { 2, 4, 6, 8 };
	Call = FAvidScriptHostCall();
	Call.BindingId = EAvidScriptHostBindingId::DebugSuspend;
	Call.Int64Args[0] = static_cast<int64>(BreakpointProbe);
	Call.IntArgs[0] = 7;
	Call.IntArgs[1] = UE_ARRAY_COUNT(FrameBytes);
	Call.InputBytes = FrameBytes;
	TestTrue(TEXT("Runtime commits the suspension frame"), Runtime.DispatchHostCall(Call, Result));
	const int64 SuspensionToken = Result.ReturnValueI64;
	TestTrue(TEXT("Runtime returns an opaque suspension token"), SuspensionToken > 0);
	TestEqual(
		TEXT("Committed suspension is paused"),
		Debugger.GetSnapshot().State,
		EAvidScriptDebugSessionState::Paused);
	TestTrue(TEXT("Runtime route can continue"), Debugger.ContinueExecution());

	uint8 RestoredFrame[UE_ARRAY_COUNT(FrameBytes)] = {};
	Call = FAvidScriptHostCall();
	Call.BindingId = EAvidScriptHostBindingId::DebugFrameRead;
	Call.Int64Args[0] = SuspensionToken;
	Call.IntArgs[1] = UE_ARRAY_COUNT(RestoredFrame);
	Call.OutputBytes = RestoredFrame;
	TestTrue(TEXT("Runtime restores the suspension frame"), Runtime.DispatchHostCall(Call, Result));
	TestTrue(
		TEXT("Runtime frame route preserves bytes"),
		FMemory::Memcmp(FrameBytes, RestoredFrame, sizeof(FrameBytes)) == 0);
	return true;
}

#endif
