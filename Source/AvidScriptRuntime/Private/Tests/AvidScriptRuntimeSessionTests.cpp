#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGameplayEvent.h"
#include "AvidScriptRuntimeSession.h"
#include "Session/AvidScriptRuntimeEventRouter.h"
#include "Session/AvidScriptRuntimeScheduler.h"
#include "StateMigration/AvidScriptRuntimeStateMigration.h"

#include "Misc/AutomationTest.h"

namespace
{
const uint8 GSessionCompatibleModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01,
	0x05, 0x03, 0x01, 0x00, 0x01, 0x07,
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

FAvidScriptWasmStateSlot MakeSessionStateSlot(
	const TCHAR* StableId,
	const TCHAR* Fingerprint,
	uint32 Offset,
	TArray<FString> Aliases = {})
{
	FAvidScriptWasmStateSlot Slot;
	Slot.StableId = StableId;
	Slot.Aliases = MoveTemp(Aliases);
	Slot.TypeFingerprint = Fingerprint;
	Slot.Offset = Offset;
	Slot.Size = 4;
	Slot.Alignment = 4;
	return Slot;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeStateMigrationServiceTest,
	"AvidScript.Architecture.Session.StateMigrationService",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeStateMigrationServiceTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance PreviousRuntime;
	FAvidScriptWasmRuntimeInstance CandidateRuntime;
	FAvidScriptWasmSmokeResult RuntimeResult;
	TestTrue(TEXT("Previous state runtime loads"), PreviousRuntime.LoadModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		TEXT("state_service"),
		RuntimeResult));
	TestTrue(TEXT("Candidate state runtime loads"), CandidateRuntime.LoadModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		TEXT("state_service"),
		RuntimeResult));

	const TArray<uint8> ScoreBytes = { 0x00, 0x00, 0x60, 0x40 };
	const TArray<uint8> RemovedBytes = { 0x01, 0x02, 0x03, 0x04 };
	FString MemoryError;
	TestTrue(TEXT("Previous score writes"), PreviousRuntime.WriteStateBytes(16, ScoreBytes, MemoryError));
	TestTrue(TEXT("Previous removed state writes"), PreviousRuntime.WriteStateBytes(20, RemovedBytes, MemoryError));

	const TCHAR* FloatFingerprint = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	FAvidScriptWasmReloadManifest PreviousManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_service"));
	PreviousManifest.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	PreviousManifest.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	PreviousManifest.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:removed"), FloatFingerprint, 20),
		MakeSessionStateSlot(TEXT("global:score"), FloatFingerprint, 16),
	};
	FAvidScriptWasmReloadManifest CandidateManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_service"));
	CandidateManifest.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	CandidateManifest.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	CandidateManifest.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:added"), FloatFingerprint, 36),
		MakeSessionStateSlot(TEXT("global:score"), FloatFingerprint, 32),
	};

	FAvidScriptRuntimeStateMigrationResult MigrationResult;
	TestTrue(TEXT("Compatible state migrates"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		PreviousManifest,
		CandidateRuntime,
		CandidateManifest,
		MigrationResult));
	TestTrue(TEXT("Migration is attempted"), MigrationResult.bAttempted);
	TestEqual(TEXT("One compatible slot migrates"), MigrationResult.MigratedSlotCount, 1);
	TestEqual(TEXT("Four bytes migrate"), MigrationResult.MigratedByteCount, 4);
	TestEqual(TEXT("Added and removed slots are skipped"), MigrationResult.SkippedSlotCount, 2);
	TArray<uint8> MigratedScore;
	MigratedScore.SetNumZeroed(4);
	TestTrue(TEXT("Candidate migrated score reads"), CandidateRuntime.ReadStateBytes(32, MigratedScore, MemoryError));
	TestEqual(TEXT("Candidate receives exact score bytes"), MigratedScore, ScoreBytes);

	FAvidScriptWasmReloadManifest IncompatibleManifest = CandidateManifest;
	IncompatibleManifest.StateMigration.Slots[1].TypeFingerprint =
		TEXT("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	TestFalse(TEXT("Incompatible stable type is rejected"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		PreviousManifest,
		CandidateRuntime,
		IncompatibleManifest,
		MigrationResult));
	TestEqual(TEXT("Incompatible state category"), MigrationResult.ErrorCategory, FString(TEXT("state_migration_incompatible")));
	TestEqual(TEXT("Incompatible state identifies stable id"), MigrationResult.StableId, FString(TEXT("global:score")));

	FAvidScriptWasmReloadManifest ExactPreferredCandidate = CandidateManifest;
	ExactPreferredCandidate.StateMigration.ContractVersion = 2;
	ExactPreferredCandidate.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), FloatFingerprint, 40, { TEXT("global:removed") }),
		MakeSessionStateSlot(TEXT("global:renamed"), FloatFingerprint, 44, { TEXT("global:score") }),
	};
	FAvidScriptWasmReloadManifest ExactPreferredPrevious = PreviousManifest;
	ExactPreferredPrevious.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), FloatFingerprint, 16),
	};
	TestTrue(TEXT("Exact primary match takes priority over alias"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		ExactPreferredPrevious,
		CandidateRuntime,
		ExactPreferredCandidate,
		MigrationResult));
	TestEqual(TEXT("Exact primary does not report alias match"), MigrationResult.AliasedSlotCount, 0);
	TArray<uint8> ExactBytes;
	ExactBytes.SetNumZeroed(4);
	TestTrue(TEXT("Exact primary destination reads"), CandidateRuntime.ReadStateBytes(40, ExactBytes, MemoryError));
	TestEqual(TEXT("Exact primary destination receives score"), ExactBytes, ScoreBytes);

	FAvidScriptWasmReloadManifest RenameAliasCandidate = CandidateManifest;
	RenameAliasCandidate.StateMigration.ContractVersion = 2;
	RenameAliasCandidate.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:renamed_score"), FloatFingerprint, 48, { TEXT("global:score") }),
	};
	TestTrue(TEXT("Rename alias migrates state"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		PreviousManifest,
		CandidateRuntime,
		RenameAliasCandidate,
		MigrationResult));
	TestEqual(TEXT("Rename alias count"), MigrationResult.AliasedSlotCount, 1);
	TArray<uint8> RenamedBytes;
	RenamedBytes.SetNumZeroed(4);
	TestTrue(TEXT("Rename alias destination reads"), CandidateRuntime.ReadStateBytes(48, RenamedBytes, MemoryError));
	TestEqual(TEXT("Rename alias destination receives score"), RenamedBytes, ScoreBytes);

	FAvidScriptWasmReloadManifest VersionRegressionCandidate = CandidateManifest;
	PreviousManifest.StateMigration.ContractVersion = 2;
	VersionRegressionCandidate.StateMigration.ContractVersion = 1;
	TestFalse(TEXT("Candidate contract version regression is rejected"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		PreviousManifest,
		CandidateRuntime,
		VersionRegressionCandidate,
		MigrationResult));
	TestEqual(TEXT("Version regression category"), MigrationResult.ErrorCategory, FString(TEXT("state_migration_version_regression")));
	PreviousManifest.StateMigration.ContractVersion = 1;

	const TArray<uint8> CandidateFirstOriginal = { 0x10, 0x11, 0x12, 0x13 };
	const TArray<uint8> CandidateSecondOriginal = { 0x20, 0x21, 0x22, 0x23 };
	const TArray<uint8> PreviousSecondBytes = { 0x30, 0x31, 0x32, 0x33 };
	TestTrue(TEXT("Previous second state writes"), PreviousRuntime.WriteStateBytes(24, PreviousSecondBytes, MemoryError));
	TestTrue(TEXT("Candidate first original writes"), CandidateRuntime.WriteStateBytes(52, CandidateFirstOriginal, MemoryError));
	TestTrue(TEXT("Candidate second original writes"), CandidateRuntime.WriteStateBytes(56, CandidateSecondOriginal, MemoryError));
	FAvidScriptWasmReloadManifest TransactionPrevious = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_service"));
	TransactionPrevious.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	TransactionPrevious.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	TransactionPrevious.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:first"), FloatFingerprint, 16),
		MakeSessionStateSlot(TEXT("global:second"), FloatFingerprint, 24),
	};
	FAvidScriptWasmReloadManifest TransactionCandidate = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_service"));
	TransactionCandidate.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	TransactionCandidate.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	TransactionCandidate.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:first"), FloatFingerprint, 52),
		MakeSessionStateSlot(TEXT("global:second"), FloatFingerprint, 56),
	};
	CandidateRuntime.SetStateWriteFailureForTesting(2);
	TestFalse(TEXT("Second candidate state write fails transaction"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		TransactionPrevious,
		CandidateRuntime,
		TransactionCandidate,
		MigrationResult));
	CandidateRuntime.ClearStateWriteFailureForTesting();
	TestEqual(TEXT("Write failure category"), MigrationResult.ErrorCategory, FString(TEXT("state_migration_write_failed")));
	TArray<uint8> CandidateFirstAfterFailure;
	CandidateFirstAfterFailure.SetNumZeroed(4);
	TestTrue(TEXT("Candidate first state reads after rollback"), CandidateRuntime.ReadStateBytes(52, CandidateFirstAfterFailure, MemoryError));
	TestEqual(TEXT("Candidate first state is restored after second write failure"), CandidateFirstAfterFailure, CandidateFirstOriginal);
	TArray<uint8> CandidateSecondAfterFailure;
	CandidateSecondAfterFailure.SetNumZeroed(4);
	TestTrue(TEXT("Candidate second state reads after failed write"), CandidateRuntime.ReadStateBytes(56, CandidateSecondAfterFailure, MemoryError));
	TestEqual(TEXT("Candidate second state remains original after failed write"), CandidateSecondAfterFailure, CandidateSecondOriginal);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeSessionStateMigrationRollbackTest,
	"AvidScript.Architecture.Session.StateMigrationRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeSessionStateMigrationRollbackTest::RunTest(const FString& Parameters)
{
	const TCHAR* PreviousFingerprint = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	const TCHAR* CandidateFingerprint = TEXT("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	FAvidScriptWasmReloadManifest PreviousManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_session"));
	PreviousManifest.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	PreviousManifest.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	PreviousManifest.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), PreviousFingerprint, 16),
	};
	FAvidScriptWasmReloadManifest CandidateManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_session"));
	CandidateManifest.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	CandidateManifest.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	CandidateManifest.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), CandidateFingerprint, 32),
	};

	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(TEXT("State session starts"), Session.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		PreviousManifest,
		ReloadResult));

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("State session ticks before rejected reload"), Session.Tick(1.0f / 60.0f, TickResult));
	TestFalse(TEXT("Incompatible state candidate is rejected"), Session.ReloadModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		CandidateManifest,
		ReloadResult));
	TestTrue(TEXT("State migration was attempted"), ReloadResult.bStateMigrationAttempted);
	TestTrue(TEXT("Rejected state candidate preserves old runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("State rejection category"), ReloadResult.ErrorCategory, FString(TEXT("state_migration_incompatible")));
	TestEqual(TEXT("State rejection identifies stable id"), ReloadResult.StateMigrationStableId, FString(TEXT("global:score")));
	TestEqual(TEXT("Old state module remains active"), Session.GetSnapshot().ModuleId, FString(TEXT("state_session")));
	TestTrue(TEXT("Old state runtime continues ticking"), Session.Tick(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Old state runtime tick count continues"), Session.GetSnapshot().TickCallCount, 2);

	FAvidScriptWasmSmokeResult StopResult;
	Session.StopAndUnload(StopResult);
	return true;
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
