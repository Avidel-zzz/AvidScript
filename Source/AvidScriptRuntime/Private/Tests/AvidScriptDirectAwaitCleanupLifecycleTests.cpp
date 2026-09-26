#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCompiledDirectAwaitCleanupLifecycleTest,
	"AvidScript.Runtime.Continuation.CompiledDirectAwaitCleanupLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCompiledDirectAwaitCleanupLifecycleTest::RunTest(const FString& Parameters)
{
	if (!GEngine) return false;
	const FString Fixture = FPlatformMisc::GetEnvironmentVariable(
		TEXT("AVIDSCRIPT_DIRECT_AWAIT_WASM_PATH"));
	const FString ModuleId = FPlatformMisc::GetEnvironmentVariable(
		TEXT("AVIDSCRIPT_DIRECT_AWAIT_MODULE_ID"));
	int32 Offsets[4] = {};
	const TCHAR* OffsetNames[] = {TEXT("AVIDSCRIPT_DIRECT_AWAIT_RESULT_OFFSET"),
		TEXT("AVIDSCRIPT_DIRECT_AWAIT_CLEANUP_OFFSET"),
		TEXT("AVIDSCRIPT_DIRECT_AWAIT_CATCH_OFFSET"), TEXT("AVIDSCRIPT_DIRECT_AWAIT_MODE_OFFSET")};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Offsets); ++Index)
	{
		const FString Text = FPlatformMisc::GetEnvironmentVariable(OffsetNames[Index]);
		if (!TestTrue(OffsetNames[Index], LexTryParseString(Offsets[Index], *Text)
			&& Offsets[Index] >= 0 && Offsets[Index] < 65536)) return false;
	}
	TArray<uint8> Bytes;
	if (!TestFalse(TEXT("Lifecycle module identity is set"), ModuleId.IsEmpty())
		|| !TestTrue(TEXT("Lifecycle compiled fixture exists"),
			FFileHelper::LoadFileToArray(Bytes, *Fixture))) return false;

	enum class EScenario { Session, World, Object, Discard, Commit, CommitCancelled };
	const TCHAR* ScenarioNames[] = {TEXT("session"), TEXT("world"), TEXT("object"),
		TEXT("discard"), TEXT("commit"), TEXT("commit_cancelled")};
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const EScenario Scenario : {EScenario::Session, EScenario::World, EScenario::Object,
		EScenario::Discard, EScenario::Commit, EScenario::CommitCancelled})
	{
		const TCHAR* ScenarioName = ScenarioNames[static_cast<int32>(Scenario)];
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		if (!TestNotNull(TEXT("Lifecycle world created"), World)) return false;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
		FAvidScriptObjectRegistry Registry;
		FAvidScriptSessionObjectOwnership Ownership;
		AActor* Actor = World->SpawnActor<AActor>();
		if (!TestNotNull(TEXT("Lifecycle owner actor spawned"), Actor)) return false;
		FAvidScriptObjectHandleResult RegisterResult;
		const auto OwnerHandle = Registry.RegisterObject(Actor, RegisterResult, false);
		if (!TestTrue(TEXT("Lifecycle owner registered"), OwnerHandle.IsValid())) return false;
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Active(Selection);
		FAvidScriptWasmRuntimeInstance Candidate(Selection);
		ON_SCOPE_EXIT
		{
			Active.SetHostContext({});
			Candidate.SetHostContext({});
			Owner->Teardown();
			Ownership.Cleanup(Registry);
		};
		FAvidScriptWasmSmokeResult Result;
		auto ReadInt = [&](FAvidScriptWasmRuntimeInstance& Runtime, int32 Index)
		{
			int32 Value = MIN_int32;
			FString Error;
			if (!Runtime.ReadStateBytes(Offsets[Index],
				MakeArrayView(reinterpret_cast<uint8*>(&Value), sizeof(Value)), Error)) AddError(Error);
			return Value;
		};
		auto Start = [&](FAvidScriptWasmRuntimeInstance& Runtime,
			FAvidScriptContinuationHostEndpoint& Endpoint, int32 CleanupMode)
		{
			if (!Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), ModuleId, Result)
				|| !Runtime.ValidateRequiredExports({TEXT("avid_on_begin_play"),
					TEXT("avid_on_tick"), TEXT("avid_on_continuation_v2")}, Result))
			{ AddError(Result.ErrorMessage); return false; }
			FAvidScriptWasmHostContext Context;
			Context.Tasks = &Endpoint;
			Context.Continuations = &Endpoint;
			Context.World = World;
			Runtime.SetHostContext(Context);
			FString Error;
			if (!Runtime.WriteStateBytes(Offsets[3],
				MakeArrayView(reinterpret_cast<const uint8*>(&CleanupMode), sizeof(CleanupMode)), Error))
			{ AddError(Error); return false; }
			if (!Runtime.BeginPlay(Result)) { AddError(Result.ErrorMessage); return false; }
			const auto Stats = Runtime.GetManagedHeapForTesting()->GetStats();
			return TestTrue(TEXT("Suspended compiled script owns a live managed capture"),
				Stats.LiveRoots > 0 && Stats.LiveObjects > 0)
				&& TestEqual(TEXT("Suspension precedes finally"), ReadInt(Runtime, 1), 0);
		};
		auto QueueCancellation = [&](FAvidScriptWasmRuntimeInstance& Runtime,
			EAvidScriptContinuationLane Lane)
		{
			if (!Runtime.Tick(-1.0f, Result)) { AddError(Result.ErrorMessage); return false; }
			return TestEqual(TEXT("Cancellation is queued before lifecycle transition"),
				Owner->GetReadyCountForTesting(Lane), 1)
				&& TestTrue(TEXT("Queued cancellation retains the managed capture"),
					Runtime.GetManagedHeapForTesting()->GetStats().LiveRoots > 0)
				&& TestEqual(TEXT("Queued cancellation has not executed finally"), ReadInt(Runtime, 1), 0);
		};
		auto CheckHeapReleased = [&](FAvidScriptWasmRuntimeInstance& Runtime)
		{
			auto* Heap = Runtime.GetManagedHeapForTesting();
			TestEqual(TEXT("Lifecycle releases managed roots"), Heap->GetStats().LiveRoots, uint32(0));
			TestEqual(TEXT("Lifecycle leaves no invocation frames"), Heap->GetStats().ActiveFrames, uint32(0));
			TestTrue(TEXT("Retired managed capture can be collected"),
				Heap->Collect() == AvidScript::Managed::EHeapError::Ok);
			TestEqual(TEXT("Retired capture is actually reclaimed"), Heap->GetStats().LiveObjects, uint32(0));
		};
		auto& ActiveEndpoint = Owner->ResetActive(World, &Registry, &Ownership, OwnerHandle);
		const bool bDiscard = Scenario == EScenario::Discard;
		const bool bPrepared = bDiscard || Scenario == EScenario::Commit || Scenario == EScenario::CommitCancelled;
		if (!Start(Active, ActiveEndpoint, bDiscard ? 0 : 1)) return false;
		TestEqual(TEXT("Active script has timer and outer task waiter"), Owner->GetActiveCount(), 2);
		TestTrue(TEXT("Compiled suspension retains state bytes"), Owner->GetStateFrameByteCountForTesting() > 0);
		if (!bDiscard && !QueueCancellation(Active, EAvidScriptContinuationLane::Active)) return false;

		FAvidScriptWasmRuntimeInstance* Survivor = nullptr;
		if (bPrepared)
		{
			auto& PreparedEndpoint = Owner->BeginPrepared(World, &Registry, &Ownership, OwnerHandle);
			if (!Start(Candidate, PreparedEndpoint, bDiscard ? 1 : 0)) return false;
			TestEqual(TEXT("Prepared script has its own timer and waiter"), Owner->GetPreparedCount(), 2);
			TestEqual(TEXT("Task owners are distinct before transition"), Owner->GetTaskResultsForTesting().GetCount(), 2);
			if ((bDiscard || Scenario == EScenario::CommitCancelled)
				&& !QueueCancellation(Candidate, EAvidScriptContinuationLane::Prepared)) return false;
			if (bDiscard)
			{
				TArray<FAvidScriptContinuationCompletion> BeforeCommit;
				Owner->DrainReady(BeforeCommit);
				TestEqual(TEXT("Prepared cancellation cannot dispatch through active instance"), BeforeCommit.Num(), 0);
				// Discard destroys the candidate endpoint; detach its raw Host pointers first.
				Candidate.SetHostContext({});
				Owner->DiscardPrepared();
				CheckHeapReleased(Candidate);
				Survivor = &Active;
			}
			else
			{
				FString CommitError;
				if (!TestTrue(TEXT("Prepared state can commit"), Owner->ValidatePreparedCommit(CommitError)))
				{ AddError(CommitError); return false; }
				Owner->CommitPrepared();
				TestEqual(TEXT("Retired endpoint rejects new work"), ActiveEndpoint.ScheduleDelay(0.0f, 999), 0LL);
				Active.SetHostContext({});
				Owner->ReleaseRetiredEndpoint();
				CheckHeapReleased(Active);
				Survivor = &Candidate;
			}
			TestEqual(TEXT("Transition retires only the displaced task"), Owner->GetTaskResultsForTesting().GetCount(), 1);
			TestEqual(TEXT("Transition keeps only the survivor cancellation source"), Owner->GetCancellationSourceCountForTesting(), 1);
			TestEqual(TEXT("Prepared entries leave their lane"), Owner->GetPreparedCount(), 0);
			TestEqual(TEXT("Prepared ready queue is empty after transition"),
				Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Prepared), 0);
			TestTrue(TEXT("Survivor still owns its managed capture"),
				Survivor->GetManagedHeapForTesting()->GetStats().LiveRoots > 0);
		}
		else if (Scenario == EScenario::Session)
		{
			Owner->Teardown();
			TestEqual(TEXT("Torn-down endpoint rejects new work"), ActiveEndpoint.ScheduleDelay(0.0f, 999), 0LL);
		}
		else if (Scenario == EScenario::World)
		{
			World->BeginTearingDown();
		}
		else
		{
			if (!TestTrue(TEXT("Actual owner actor is destroyed"), World->DestroyActor(Actor))) return false;
			FAvidScriptObjectHandleResult ResolveResult;
			TestNull(TEXT("Destroyed actor no longer resolves"), Registry.ResolveObject(OwnerHandle, ResolveResult, false));
		}

		int32 Resumes = 0;
		int32 Cancelled = 0;
		const bool bCancelledSurvivor = Scenario == EScenario::CommitCancelled;
		for (int32 Round = 0; Round < 8; ++Round)
		{
			if (Scenario != EScenario::World)
			{
				World->Tick(LEVELTICK_All, 0.02f);
				++GFrameCounter;
			}
			TArray<FAvidScriptContinuationCompletion> Ready;
			Owner->DrainReady(Ready);
			for (const auto& Completion : Ready)
			{
				if (!TestNotNull(TEXT("Only a surviving instance may receive callbacks"), Survivor)) return false;
				Cancelled += Completion.Status == EAvidScriptContinuationStatus::Cancelled ? 1 : 0;
				const bool bDispatched = Survivor->DispatchContinuation(Completion, Result);
				const bool bExpectedOuterCancellation = bCancelledSurvivor && Resumes == 1;
				TestEqual(TEXT("Survivor dispatch outcome"), bDispatched, !bExpectedOuterCancellation);
				if (bExpectedOuterCancellation)
					TestEqual(TEXT("Promoted outer cancellation keeps the existing VM classification"),
						Result.ErrorCategory, Backend == EAvidScriptVmBackendKind::Wasmtime
							? FString(TEXT("guest_trap")) : FString(TEXT("trap")));
				TestTrue(TEXT("Surviving completion finalizes"), Owner->FinalizeDispatched(Completion.Token, bDispatched));
				++Resumes;
			}
		}
		TestEqual(TEXT("Only the survivor resumes"), Resumes, Survivor ? 2 : 0);
		TestEqual(TEXT("Prepared cancellation survives promotion exactly once per waiter"), Cancelled, bCancelledSurvivor ? 2 : 0);
		TestEqual(TEXT("Active result belongs to the preserved instance"), ReadInt(Active, 0), bDiscard ? 16 : 0);
		TestEqual(TEXT("Retired active cleanup never executes"), ReadInt(Active, 1), bDiscard ? 1 : 0);
		TestEqual(TEXT("Active cancellation never enters catch"), ReadInt(Active, 2), 0);
		if (bPrepared)
		{
			TestEqual(TEXT("Candidate result belongs to the committed instance"), ReadInt(Candidate, 0), Scenario == EScenario::Commit ? 16 : 0);
			TestEqual(TEXT("Discarded candidate cleanup never executes"), ReadInt(Candidate, 1), bDiscard ? 0 : 1);
			TestEqual(TEXT("Candidate cancellation never enters catch"), ReadInt(Candidate, 2), 0);
			CheckHeapReleased(Candidate);
		}
		CheckHeapReleased(Active);
		TestEqual(TEXT("All task records release before final teardown"), Owner->GetTaskResultsForTesting().GetCount(), 0);
		TestEqual(TEXT("All task waiters release before final teardown"), Owner->GetTaskResultsForTesting().GetWaiterCount(), 0);
		TestEqual(TEXT("All state frames release before final teardown"), Owner->GetStateFrameByteCountForTesting(), 0);
		TestEqual(TEXT("All cancellation bindings release"), Owner->GetCancellationBindingCountForTesting(), 0);
		TestEqual(TEXT("Invalid contexts release sources without a fallback teardown"),
			Owner->GetCancellationSourceCountForTesting(), Survivor ? 1 : 0);
		Active.SetHostContext({});
		Candidate.SetHostContext({});
		Owner->Teardown();
		Owner->Teardown();
		TestEqual(TEXT("Idempotent teardown clears active entries"), Owner->GetActiveCount(), 0);
		TestEqual(TEXT("Idempotent teardown clears prepared entries"), Owner->GetPreparedCount(), 0);
		TestEqual(TEXT("Idempotent teardown clears sources"), Owner->GetCancellationSourceCountForTesting(), 0);
		for (const auto Lane : {EAvidScriptContinuationLane::Active, EAvidScriptContinuationLane::Prepared})
			TestEqual(TEXT("No ready callback survives teardown"), Owner->GetReadyCountForTesting(Lane), 0);
		AddInfo(FString::Printf(TEXT("compiled direct await lifecycle backend=%d scenario=%s active_cleanup=%d candidate_cleanup=%d resumes=%d cancelled=%d roots=0 tasks=0 frames=0 sources=0 bindings=0"),
			static_cast<int32>(Backend), ScenarioName, ReadInt(Active, 1), bPrepared ? ReadInt(Candidate, 1) : -1, Resumes, Cancelled));
	}
	return true;
}

#endif
