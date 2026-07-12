#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGameplayEvent.h"
#include "AvidScriptRuntimeSession.h"
#include "Session/AvidScriptRuntimeEventRouter.h"
#include "Session/AvidScriptRuntimeScheduler.h"

#include "Misc/AutomationTest.h"

namespace
{
const uint8 GSessionCompatibleModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x07,
	0x02, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

const uint8 GSessionBeginTrapModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x08,
	0x02, 0x03, 0x00, 0x00, 0x0b, 0x02, 0x00, 0x0b
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeSessionCandidateBeginRollbackTest,
	"AvidScript.Architecture.Session.CandidateBeginRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeSessionCandidateBeginRollbackTest::RunTest(const FString& Parameters)
{
	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(
		TEXT("initial session starts"),
		Session.LoadInitialModule(
			GSessionCompatibleModule,
			UE_ARRAY_COUNT(GSessionCompatibleModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("session_live")),
			ReloadResult));

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("initial live runtime ticks"), Session.Tick(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("initial tick count"), Session.GetSnapshot().TickCallCount, 1);

	TestFalse(
		TEXT("candidate BeginPlay trap rejects reload"),
		Session.ReloadModule(
			GSessionBeginTrapModule,
			UE_ARRAY_COUNT(GSessionBeginTrapModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("session_trap")),
			ReloadResult));
	TestTrue(TEXT("rollback preserves old runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("old module remains active"), Session.GetSnapshot().ModuleId, FString(TEXT("session_live")));
	TestEqual(TEXT("one reload is rejected"), Session.GetSnapshot().RejectedReloadCount, 1);
	TestEqual(TEXT("session remains running"), Session.GetSnapshot().LifecycleState, EAvidScriptLifecycleState::Running);

	TestTrue(TEXT("old runtime continues ticking"), Session.Tick(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("old tick count continues"), Session.GetSnapshot().TickCallCount, 2);

	FAvidScriptWasmSmokeResult StopResult;
	Session.StopAndUnload(StopResult);
	TestEqual(TEXT("session returns to empty"), Session.GetSnapshot().LifecycleState, EAvidScriptLifecycleState::Empty);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeServicesAttachDetachTest,
	"AvidScript.Architecture.Session.RuntimeServicesAttachDetach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeServicesAttachDetachTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("service fixture loads"), Runtime.LoadEmbeddedSmokeModule(Result));
	TestTrue(TEXT("service fixture begins"), Runtime.BeginPlay(Result));

	FAvidScriptRuntimeScheduler Scheduler;
	FAvidScriptRuntimeEventRouter EventRouter(Scheduler);
	Scheduler.Attach(Runtime);
	TestTrue(TEXT("attached scheduler ticks"), Scheduler.Tick(1.0f / 60.0f, Result));
	TestEqual(TEXT("scheduler exposes tick count"), Scheduler.GetTickCallCount(), 1);

	TestFalse(TEXT("invalid event is rejected"), EventRouter.Dispatch(-1, 1.0f, Result));
	TestEqual(TEXT("invalid event category"), Result.ErrorCategory, FString(TEXT("invalid_argument")));
	TestEqual(TEXT("invalid event leaves runtime running"), Scheduler.GetLifecycleState(), EAvidScriptLifecycleState::Running);
	FAvidScriptGameplayEvent TypedInputEvent;
	TypedInputEvent.Type = EAvidScriptGameplayEventType::Input;
	TypedInputEvent.PrimaryId = 1;
	TestTrue(TEXT("attached router accepts optional typed event"), EventRouter.Dispatch(TypedInputEvent, Result));

	TestTrue(TEXT("attached runtime stops"), Runtime.EndPlay(Result));
	TestFalse(TEXT("stopped runtime rejects scheduler tick"), Scheduler.Tick(1.0f / 60.0f, Result));
	TestEqual(TEXT("stopped tick category"), Result.ErrorCategory, FString(TEXT("invalid_state")));
	TestFalse(TEXT("stopped runtime rejects routed event"), EventRouter.Dispatch(1, 1.0f, Result));
	TestEqual(TEXT("stopped event category"), Result.ErrorCategory, FString(TEXT("invalid_state")));
	TestFalse(TEXT("stopped runtime rejects typed event"), EventRouter.Dispatch(TypedInputEvent, Result));
	TestEqual(TEXT("stopped typed event category"), Result.ErrorCategory, FString(TEXT("invalid_state")));

	Scheduler.Detach();
	TestFalse(TEXT("detached scheduler rejects tick"), Scheduler.Tick(1.0f / 60.0f, Result));
	TestEqual(TEXT("detached tick category"), Result.ErrorCategory, FString(TEXT("invalid_state")));
	TestFalse(TEXT("detached event router rejects dispatch"), EventRouter.Dispatch(1, 1.0f, Result));
	TestEqual(TEXT("detached event category"), Result.ErrorCategory, FString(TEXT("invalid_state")));
	TestFalse(TEXT("detached event router rejects typed dispatch"), EventRouter.Dispatch(TypedInputEvent, Result));
	TestEqual(TEXT("detached typed event category"), Result.ErrorCategory, FString(TEXT("invalid_state")));

	Runtime.Unload(Result);
	return true;
}
#endif
