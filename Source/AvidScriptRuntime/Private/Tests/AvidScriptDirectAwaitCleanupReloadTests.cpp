#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptRuntimeSession.h"
#include "AvidScriptObjectRegistry.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCompiledDirectAwaitCleanupReloadTest,
	"AvidScript.Runtime.Continuation.CompiledDirectAwaitCleanupReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCompiledDirectAwaitCleanupReloadTest::RunTest(const FString& Parameters)
{
	if (!GEngine) return false;
	const FString ManifestPath = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_DIRECT_AWAIT_MANIFEST_PATH"));
	FAvidScriptWasmReloadManifest Manifest;
	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("Formal manifest and its verified WASM load"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(ManifestPath, Manifest, Bytes, LoadResult)))
	{ AddError(LoadResult.ErrorMessage); return false; }
	FAvidScriptWasmReloadManifest NextManifest;
	TArray<uint8> NextBytes;
	if (!TestTrue(TEXT("Next-generation manifest and verified WASM load"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_DIRECT_AWAIT_NEXT_MANIFEST_PATH")),
			NextManifest, NextBytes, LoadResult)))
	{ AddError(LoadResult.ErrorMessage); return false; }
	if (!TestEqual(TEXT("Code update keeps module identity"), NextManifest.ModuleId, Manifest.ModuleId)
		|| !TestTrue(TEXT("Code update has different executable bytes"), NextBytes != Bytes)
		|| !TestNotEqual(TEXT("Verified WASM identities differ"), NextManifest.WasmSha256, Manifest.WasmSha256)) return false;
	for (const auto& Slot : Manifest.StateMigration.Slots)
	{
		const auto* NextSlot = NextManifest.StateMigration.Slots.FindByPredicate(
			[&](const FAvidScriptWasmStateSlot& Item) { return Item.StableId == Slot.StableId; });
		if (!TestTrue(TEXT("Constant-only update retains the fixture state layout"),
			NextSlot && NextSlot->Offset == Slot.Offset && NextSlot->Size == Slot.Size
			&& NextSlot->TypeFingerprint == Slot.TypeFingerprint)) return false;
	}
	if (!TestTrue(TEXT("Formal state migration contract is present"), Manifest.StateMigration.IsEnabled())) return false;
	auto ReadInt = [&](const FAvidScriptWasmRuntimeInstance& Runtime, const TCHAR* Field)
	{
		const FString StableId = FString(TEXT("state:type:global::Script:")) + Field;
		const auto* Slot = Manifest.StateMigration.Slots.FindByPredicate(
			[&](const FAvidScriptWasmStateSlot& Item) { return Item.StableId == StableId; });
		int32 Value = MIN_int32;
		FString Error;
		if (!Slot || Slot->Size != sizeof(Value)) AddError(FString(TEXT("Missing int state slot: ")) + Field);
		else if (!Runtime.ReadStateBytes(Slot->Offset,
			MakeArrayView(reinterpret_cast<uint8*>(&Value), sizeof(Value)), Error)) AddError(Error);
		return Value;
	};
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const bool bCancelOld : {false, true})
	for (const int32 Mode : {0, 1, 2})
	{
		// Mode 0 commits a waiting candidate; 1 commits a cancelled candidate;
		// 2 traps in candidate BeginPlay after cancellation has already queued.
		const bool bReject = Mode == 2;
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		if (!TestNotNull(TEXT("Reload test world created"), World)) return false;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
		FAvidScriptObjectRegistry Registry;
		AActor* Actor = World->SpawnActor<AActor>();
		if (!TestNotNull(TEXT("Reload owner spawned"), Actor)) return false;
		FAvidScriptObjectHandleResult Registered;
		const auto Handle = Registry.RegisterObject(Actor, Registered, false);
		if (!TestTrue(TEXT("Reload owner registered"), Handle.IsValid())) return false;
		FAvidScriptRuntimeSession Session;
		ON_SCOPE_EXIT { FAvidScriptWasmSmokeResult Stopped; Session.StopAndUnload(Stopped); };
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		Session.SetBackendSelectionForTesting(Selection);
		FAvidScriptWasmHostContext Context;
		Context.World = World;
		Context.ObjectRegistry = &Registry;
		Context.OwnerHandle = Handle;
		Session.SetHostContext(Context);
		FAvidScriptWasmReloadResult ReloadResult;
		if (!TestTrue(TEXT("Initial compiled script activates through Session"),
			Session.LoadInitialModule(Bytes.GetData(), Bytes.Num(), Manifest, ReloadResult)))
		{ AddError(ReloadResult.ErrorMessage); return false; }
		auto* Owner = Session.GetContinuationOwnerForTesting();
		auto* Original = Session.GetLiveRuntimeForTesting();
		const auto OriginalLease = Session.GetRuntimeLeaseForTesting();
		const uint32 OriginalRoots = Original->GetManagedHeapForTesting()->GetStats().LiveRoots;
		TestTrue(TEXT("Original Session retains managed await state"), OriginalRoots > 0);
		TestEqual(TEXT("Original BeginPlay runs once"), ReadInt(*Original, TEXT("BeginCount")), 1);
		FAvidScriptWasmSmokeResult CallResult;
		if (!TestTrue(TEXT("Real event stages reload mode and optional active cancellation"),
			Session.DispatchEventLive(Mode, bCancelOld ? -1.0f : 0.0f, CallResult)))
		{ AddError(CallResult.ErrorMessage); return false; }
		TestEqual(TEXT("Active cancellation remains queued until Session Tick"),
			Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Active), bCancelOld ? 1 : 0);
		TestEqual(TEXT("Event has not resumed the old script"), ReadInt(*Original, TEXT("CleanupCount")), 0);
		const int32 OriginalFrames = Owner->GetStateFrameByteCountForTesting();

		TWeakPtr<FAvidScriptWasmRuntimeInstance> CandidateLease;
		int32 ObservedCandidates = 0;
		Session.SetCandidateBeginPlayCompletionObserverForTesting(
			[&](TWeakPtr<FAvidScriptWasmRuntimeInstance> Candidate, bool bBegan)
			{
				++ObservedCandidates;
				CandidateLease = Candidate;
				const auto Runtime = Candidate.Pin();
				if (!TestTrue(TEXT("Observer sees the actual candidate Runtime"), Runtime.IsValid())) return;
				TestTrue(TEXT("Candidate owns a distinct VM"), Runtime.Get() != Original);
				TestEqual(TEXT("Candidate BeginPlay outcome matches the source"), bBegan, !bReject);
				TestEqual(TEXT("State migrates before candidate BeginPlay"), ReadInt(*Runtime, TEXT("BeginCount")), 2);
				TestEqual(TEXT("Candidate mode migrates through the formal state schema"), ReadInt(*Runtime, TEXT("ReloadMode")), Mode);
				TestEqual(TEXT("Candidate cleanup has not run before publication"), ReadInt(*Runtime, TEXT("CleanupCount")), 0);
				TestEqual(TEXT("Candidate owns a prepared cancellation only when requested"),
					Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Prepared), Mode == 0 ? 0 : 1);
				TestEqual(TEXT("Candidate failure occurs after suspension was established"),
					Owner->GetPreparedCount(), bReject ? 1 : 2);
				TestTrue(TEXT("Candidate managed state is rooted before commit or rollback"),
					Runtime->GetManagedHeapForTesting()->GetStats().LiveRoots > 0);
				TestEqual(TEXT("Preparing candidate leaves old roots intact"),
					Original->GetManagedHeapForTesting()->GetStats().LiveRoots, OriginalRoots);
				TestEqual(TEXT("Both task owners exist before transaction resolution"), Owner->GetTaskResultsForTesting().GetCount(), 2);
			});
		const bool bReloaded = Session.ReloadModule(NextBytes.GetData(), NextBytes.Num(), NextManifest, ReloadResult);
		TestEqual(TEXT("Full Session transaction returns the expected result"), bReloaded, !bReject);
		if (bReloaded == bReject) { AddError(ReloadResult.ErrorMessage); return false; }
		TestEqual(TEXT("Exactly one candidate executed BeginPlay"), ObservedCandidates, 1);
		TestTrue(TEXT("Reload attempted the native host effect transaction"), ReloadResult.bHostEffectTransactionAttempted);
		TestTrue(TEXT("Reload applied the formal state migration"), ReloadResult.bStateMigrationApplied);
		TestEqual(TEXT("Only successful candidate commits host effects"), ReloadResult.bHostEffectTransactionCommitted, !bReject);
		TestEqual(TEXT("Failed candidate rolls back host effects"), ReloadResult.bHostEffectRollbackSucceeded, bReject);
		TestEqual(TEXT("Rollback preserves the old Runtime"), ReloadResult.bRollbackPreservedLiveRuntime, bReject);
		TestEqual(TEXT("Old Runtime is destroyed only on commit"), OriginalLease.IsValid(), bReject);
		TestEqual(TEXT("Candidate Runtime is destroyed on rollback"), CandidateLease.IsValid(), !bReject);
		TestEqual(TEXT("Successful reload counter"), Session.GetSuccessfulReloadCount(), bReject ? 0 : 1);
		TestEqual(TEXT("Rejected reload counter"), Session.GetRejectedReloadCount(), bReject ? 1 : 0);
		if (bReject)
		{
			TestEqual(TEXT("Candidate failure is a VM trap"), ReloadResult.ErrorCategory,
				Backend == EAvidScriptVmBackendKind::Wasmtime ? FString(TEXT("guest_trap")) : FString(TEXT("trap")));
			TestTrue(TEXT("Rollback preserves exact old VM identity"), Session.GetLiveRuntimeForTesting() == Original);
			TestEqual(TEXT("Rollback restores exact old pending state size"), Owner->GetStateFrameByteCountForTesting(), OriginalFrames);
			TestFalse(TEXT("Candidate failure does not quarantine the old Session"), Session.GetSnapshot().bFaultQuarantined);
		}
		auto* Survivor = Session.GetLiveRuntimeForTesting();
		const bool bSurvivorCancelled = bReject ? bCancelOld : Mode == 1;
		TestEqual(TEXT("Survivor has not been rerun during rollback"), ReadInt(*Survivor, TEXT("BeginCount")), bReject ? 1 : 2);
		TestEqual(TEXT("Transaction does not run cleanup callbacks"), ReadInt(*Survivor, TEXT("CleanupCount")), 0);
		TestEqual(TEXT("Surviving ready queue keeps its own cancellation"),
			Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Active), bSurvivorCancelled ? 1 : 0);
		TestEqual(TEXT("Transaction releases displaced task records"), Owner->GetTaskResultsForTesting().GetCount(), 1);
		TestEqual(TEXT("Transaction releases displaced cancellation sources"), Owner->GetCancellationSourceCountForTesting(), 1);
		TestEqual(TEXT("Transaction clears prepared entries"), Owner->GetPreparedCount(), 0);
		TestEqual(TEXT("Transaction clears prepared ready queue"), Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Prepared), 0);

		World->Tick(LEVELTICK_All, 0.02f);
		++GFrameCounter;
		if (!TestTrue(TEXT("Session dispatches the surviving direct-await cleanup"), Session.TickLive(0.02f, CallResult)))
		{ AddError(CallResult.ErrorMessage); return false; }
		TestEqual(TEXT("Surviving finally executes once"), ReadInt(*Survivor, TEXT("CleanupCount")), 1);
		TestEqual(TEXT("Cancellation does not enter catch"), ReadInt(*Survivor, TEXT("CatchCount")), 0);
		auto* Heap = Survivor->GetManagedHeapForTesting();
		TestEqual(TEXT("Session resume releases the managed capture root"), Heap->GetStats().LiveRoots, uint32(0));
		TestTrue(TEXT("Survivor heap can collect"), Heap->Collect() == AvidScript::Managed::EHeapError::Ok);
		TestEqual(TEXT("Managed capture is reclaimed after Session resume"), Heap->GetStats().LiveObjects, uint32(0));
		TestEqual(TEXT("Only the outer waiter remains after the first pump"), Owner->GetActiveCount(), 1);
		const bool bOuterSucceeded = Session.TickLive(0.02f, CallResult);
		TestEqual(TEXT("Session observes the correct outer Task terminal state"), bOuterSucceeded, !bSurvivorCancelled);
		if (bSurvivorCancelled)
		{
			TestTrue(TEXT("Uncaught outer cancellation quarantines Session"), Session.GetSnapshot().bFaultQuarantined);
			TestFalse(TEXT("Quarantine unloads the failing Runtime"), Session.IsLiveLoaded());
			TestEqual(TEXT("Outer cancellation classification is preserved"), CallResult.ErrorCategory,
				Backend == EAvidScriptVmBackendKind::Wasmtime ? FString(TEXT("guest_trap")) : FString(TEXT("trap")));
		}
		else
		{
			TestEqual(TEXT("Surviving Task executes the correct code generation"), ReadInt(*Survivor, TEXT("Result")), bReject ? 16 : 32);
			TestEqual(TEXT("Second Session pump never repeats cleanup"), ReadInt(*Survivor, TEXT("CleanupCount")), 1);
		}
		TestEqual(TEXT("Task records are released before final stop"), Owner->GetTaskResultsForTesting().GetCount(), 0);
		TestEqual(TEXT("Task waiters are released before final stop"), Owner->GetTaskResultsForTesting().GetWaiterCount(), 0);
		TestEqual(TEXT("State frames are released before final stop"), Owner->GetStateFrameByteCountForTesting(), 0);
		TestTrue(TEXT("Session stops through the public lifecycle API"), Session.StopAndUnload(CallResult));
		TestTrue(TEXT("Repeated Session stop is harmless"), Session.StopAndUnload(CallResult));
		TestFalse(TEXT("No old VM remains after stop"), OriginalLease.IsValid());
		TestFalse(TEXT("No candidate VM remains after stop"), CandidateLease.IsValid());
		TestEqual(TEXT("Stopped Session has no pending continuations"), Owner->GetActiveCount(), 0);
		TestEqual(TEXT("Stopped Session has no cancellation sources"), Owner->GetCancellationSourceCountForTesting(), 0);
		TestEqual(TEXT("Stopped Session has no cancellation bindings"), Owner->GetCancellationBindingCountForTesting(), 0);
		for (const auto Lane : {EAvidScriptContinuationLane::Active, EAvidScriptContinuationLane::Prepared})
			TestEqual(TEXT("Stopped Session has no queued callbacks"), Owner->GetReadyCountForTesting(Lane), 0);
		AddInfo(FString::Printf(TEXT("compiled direct await reload backend=%d old_cancel=%d mode=%d committed=%d survivor_cancel=%d result=%d cleanup=1 tasks=0 frames=0 sources=0 bindings=0 runtimes=0"),
			static_cast<int32>(Backend), bCancelOld ? 1 : 0, Mode, bReloaded ? 1 : 0, bSurvivorCancelled ? 1 : 0,
			bSurvivorCancelled ? 0 : bReject ? 16 : 32));
	}
	return true;
}

#endif
