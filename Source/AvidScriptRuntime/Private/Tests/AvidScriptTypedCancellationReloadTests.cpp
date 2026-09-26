#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptTypedCancellationTestSupport.h"
#include "AvidScriptRuntimeSession.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTypedCancellationReloadTest,
	"AvidScript.Runtime.Continuation.TypedCancellationReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTypedCancellationReloadTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Tests::TypedCancellation;
	if (!GEngine) return false;
	FFixture Initial, Next;
	if (!Initial.Load(*this, FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_TYPED_CANCELLATION_MANIFEST")))
		|| !Next.Load(*this, FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_TYPED_CANCELLATION_NEXT_MANIFEST")))) return false;
	if (!TestEqual(TEXT("Code update retains module identity"), Initial.Manifest.ModuleId, Next.Manifest.ModuleId)
		|| !TestTrue(TEXT("Code update changes executable bytes"), Initial.Bytes != Next.Bytes)
		|| !TestNotEqual(TEXT("Verified executable hashes differ"), Initial.Manifest.WasmSha256, Next.Manifest.WasmSha256)) return false;
	if (!TestEqual(TEXT("Readback slot count unchanged"), Initial.Offsets.Num(), Next.Offsets.Num())) return false;
	for (const auto& Slot : Initial.Offsets)
	{
		const int32* Updated = Next.Offsets.Find(Slot.Key);
		if (!TestTrue(TEXT("Both generations use the verified readback layout"), Updated && *Updated == Slot.Value)) return false;
	}
	for (const auto& Slot : Initial.Manifest.StateMigration.Slots)
	{
		const auto* Updated = Next.Manifest.StateMigration.Slots.FindByPredicate(
			[&](const FAvidScriptWasmStateSlot& Item) { return Item.StableId == Slot.StableId; });
		if (!TestTrue(TEXT("Constant-only update retains state layout"), Updated && Updated->Offset == Slot.Offset
			&& Updated->Size == Slot.Size && Updated->TypeFingerprint == Slot.TypeFingerprint)) return false;
	}
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (int32 Stage = -1; Stage < 3; ++Stage)
	for (int32 Mode = 0; Mode < 3; ++Mode)
	{
		// -1 leaves the old Task pending normally; 0 queues direct cancellation;
		// 1 retains the cancelled child Task; 2 holds it across a handled wait.
		// Modes: publish normal, publish cancelled, trap after prepared work exists.
		const bool bReject = Mode == 2;
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		if (!TestNotNull(TEXT("Reload world created"), World)) return false;
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
		Session.SetBackendSelectionForTesting(Selection(Backend));
		FAvidScriptWasmHostContext Context;
		Context.World = World; Context.ObjectRegistry = &Registry; Context.OwnerHandle = Handle;
		Session.SetHostContext(Context);
		FAvidScriptWasmReloadResult ReloadResult;
		if (!Session.LoadInitialModule(Initial.Bytes.GetData(), Initial.Bytes.Num(), Initial.Manifest, ReloadResult))
		{ AddError(ReloadResult.ErrorMessage); return false; }
		auto* Owner = Session.GetContinuationOwnerForTesting();
		auto* Original = Session.GetLiveRuntimeForTesting();
		const auto OriginalLease = Session.GetRuntimeLeaseForTesting();
		auto Read = [&](const FAvidScriptWasmRuntimeInstance& Runtime, const TCHAR* Field)
		{ return Initial.Read(*this, Runtime, TEXT("CancellationScript"), Field); };
		auto ReadEntry = [&](const FAvidScriptWasmRuntimeInstance& Runtime, const TCHAR* Field)
		{ return Initial.Read(*this, Runtime, TEXT("CancellationLifecycleEntry"), Field); };
		FAvidScriptWasmSmokeResult CallResult;
		if (!Session.DispatchEventLive(Mode, Stage >= 0 ? -1.0f : 0.0f, CallResult))
		{ AddError(CallResult.ErrorMessage); return false; }
		for (int32 Step = 0; Step < Stage; ++Step)
			if (!Session.TickLive(0.0f, CallResult)) { AddError(CallResult.ErrorMessage); return false; }
		TestEqual(TEXT("Old BeginPlay executed once"), ReadEntry(*Original, TEXT("BeginCount")), 1);
		TestEqual(TEXT("Old inner cancellation stage matches"), Read(*Original, TEXT("InnerCatch")), Stage > 0 ? 1 : 0);
		TestEqual(TEXT("Old outer cancellation stage matches"), Read(*Original, TEXT("OuterCatch")), Stage == 2 ? 1 : 0);
		TestEqual(TEXT("Old Tasks have not completed the entry"), ReadEntry(*Original, TEXT("Result")), 0);
		const int32 OldTrace = Read(*Original, TEXT("Trace"));
		const int32 OldInner = Read(*Original, TEXT("InnerCatch"));
		const int32 OldOuter = Read(*Original, TEXT("OuterCatch"));
		const int32 OldConditional = Read(*Original, TEXT("ConditionalTrace"));
		const int32 OldFrames = Owner->GetStateFrameByteCountForTesting();
		const int32 OldReady = Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Active);
		const uint32 OldRoots = Original->GetManagedHeapForTesting()->GetStats().LiveRoots;
		if (Stage > 0) TestTrue(TEXT("Actual typed exception remains rooted before reload"), OldRoots > 0);
		TestTrue(TEXT("Old state is suspended before reload"), OldFrames > 0);
		TestEqual(TEXT("Old source owns two Task records"), Owner->GetTaskResultsForTesting().GetCount(), 2);
		TWeakPtr<FAvidScriptWasmRuntimeInstance> CandidateLease;
		int32 Observed = 0;
		Session.SetCandidateBeginPlayCompletionObserverForTesting(
			[&](TWeakPtr<FAvidScriptWasmRuntimeInstance> Candidate, bool bBegan)
			{
				++Observed; CandidateLease = Candidate;
				const auto Runtime = Candidate.Pin();
				if (!TestTrue(TEXT("Observer sees actual candidate"), Runtime.IsValid())) return;
				TestTrue(TEXT("Candidate has a distinct VM"), Runtime.Get() != Original);
				TestEqual(TEXT("Candidate BeginPlay outcome"), bBegan, !bReject);
				TestEqual(TEXT("BeginCount migrates before candidate execution"), ReadEntry(*Runtime, TEXT("BeginCount")), 2);
				TestEqual(TEXT("Reload mode migrates"), ReadEntry(*Runtime, TEXT("ReloadMode")), Mode);
				TestEqual(TEXT("Helper cleanup state is fresh in the candidate"), Read(*Runtime, TEXT("Trace")), 0);
				TestEqual(TEXT("Helper inner handler state is fresh"), Read(*Runtime, TEXT("InnerCatch")), 0);
				TestEqual(TEXT("Helper outer handler state is fresh"), Read(*Runtime, TEXT("OuterCatch")), 0);
				TestEqual(TEXT("Candidate has not resumed its suspended method"), Read(*Runtime, TEXT("ConditionalTrace")), 0);
				TestEqual(TEXT("Old typed roots remain untouched during preparation"), Original->GetManagedHeapForTesting()->GetStats().LiveRoots, OldRoots);
				TestEqual(TEXT("Both lanes own their Task records"), Owner->GetTaskResultsForTesting().GetCount(), 4);
				TestTrue(TEXT("Even rejected candidate established suspended work"), Owner->GetPreparedCount() > 0);
				TestEqual(TEXT("Prepared cancellation remains unpublished"), Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Prepared), Mode == 0 ? 0 : 1);
			});
		const bool bReloaded = Session.ReloadModule(Next.Bytes.GetData(), Next.Bytes.Num(), Next.Manifest, ReloadResult);
		if (!TestEqual(TEXT("Public reload transaction outcome"), bReloaded, !bReject))
		{ AddError(ReloadResult.ErrorMessage); return false; }
		TestEqual(TEXT("Candidate executes once"), Observed, 1);
		TestTrue(TEXT("Host transaction attempted"), ReloadResult.bHostEffectTransactionAttempted);
		TestTrue(TEXT("Formal state migration applied"), ReloadResult.bStateMigrationApplied);
		TestEqual(TEXT("Only published candidate commits effects"), ReloadResult.bHostEffectTransactionCommitted, !bReject);
		TestEqual(TEXT("Failed candidate rolls back effects"), ReloadResult.bHostEffectRollbackSucceeded, bReject);
		TestEqual(TEXT("Rollback preserves live Runtime"), ReloadResult.bRollbackPreservedLiveRuntime, bReject);
		TestEqual(TEXT("Old VM lives only after rollback"), OriginalLease.IsValid(), bReject);
		TestEqual(TEXT("Candidate VM lives only after commit"), CandidateLease.IsValid(), !bReject);
		TestEqual(TEXT("Successful reload count"), Session.GetSuccessfulReloadCount(), bReject ? 0 : 1);
		TestEqual(TEXT("Rejected reload count"), Session.GetRejectedReloadCount(), bReject ? 1 : 0);
		if (bReject)
		{
			TestEqual(TEXT("Candidate source division remains a VM trap"), ReloadResult.ErrorCategory,
				Backend == EAvidScriptVmBackendKind::Wasmtime ? FString(TEXT("guest_trap")) : FString(TEXT("trap")));
			TestTrue(TEXT("Rollback preserves exact VM identity"), Session.GetLiveRuntimeForTesting() == Original);
			TestEqual(TEXT("Rollback preserves suspended state bytes"), Owner->GetStateFrameByteCountForTesting(), OldFrames);
			TestEqual(TEXT("Rollback preserves typed roots"), Original->GetManagedHeapForTesting()->GetStats().LiveRoots, OldRoots);
			TestEqual(TEXT("Rollback preserves old queued cancellation"), Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Active), OldReady);
		}
		TestFalse(TEXT("Candidate trap does not quarantine survivor"), Session.GetSnapshot().bFaultQuarantined);
		auto* Survivor = Session.GetLiveRuntimeForTesting();
		TestEqual(TEXT("Transaction preserves only the surviving helper state"), Read(*Survivor, TEXT("ConditionalTrace")), bReject ? OldConditional : 0);
		TestEqual(TEXT("Transaction does not rerun inner cleanup"), Read(*Survivor, TEXT("Trace")), bReject ? OldTrace : 0);
		TestEqual(TEXT("Transaction does not rerun inner handler"), Read(*Survivor, TEXT("InnerCatch")), bReject ? OldInner : 0);
		TestEqual(TEXT("Transaction does not rerun outer handler"), Read(*Survivor, TEXT("OuterCatch")), bReject ? OldOuter : 0);
		TestEqual(TEXT("Rollback does not rerun BeginPlay"), ReadEntry(*Survivor, TEXT("BeginCount")), bReject ? 1 : 2);
		TestEqual(TEXT("Displaced Task records released"), Owner->GetTaskResultsForTesting().GetCount(), 2);
		TestEqual(TEXT("Displaced cancellation source released"), Owner->GetCancellationSourceCountForTesting(), 1);
		TestEqual(TEXT("Prepared lane retired"), Owner->GetPreparedCount(), 0);
		TestEqual(TEXT("Prepared ready queue retired"), Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Prepared), 0);
		for (int32 Round = 0; Round < 12 && Owner->GetActiveCount() > 0; ++Round)
		{
			World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
			if (!Session.TickLive(0.02f, CallResult)) { AddError(CallResult.ErrorMessage); return false; }
			TestTrue(TEXT("Survivor remains valid across GC"), Survivor->GetManagedHeapForTesting()->Collect() == AvidScript::Managed::EHeapError::Ok);
		}
		const bool bCancelled = bReject ? Stage >= 0 : Mode == 1;
		const int32 Expected = bCancelled ? 30 : bReject ? 16 : 32;
		TestEqual(TEXT("Correct surviving code generation executes"), ReadEntry(*Survivor, TEXT("Result")), Expected);
		TestEqual(TEXT("Inner finally executes once per surviving generation"), Read(*Survivor, TEXT("Trace")), 1);
		TestEqual(TEXT("Inner cancellation handler count"), Read(*Survivor, TEXT("InnerCatch")), bCancelled ? 1 : 0);
		TestEqual(TEXT("Outer cancellation handler count"), Read(*Survivor, TEXT("OuterCatch")), bCancelled ? 1 : 0);
		TestEqual(TEXT("Survivor completes its post-handler wait"), Read(*Survivor, TEXT("ConditionalTrace")), 12);
		CheckHeapReleased(*this, *Survivor);
		CheckEmpty(*this, *Owner, 1);
		TestTrue(TEXT("Public Session stop succeeds"), Session.StopAndUnload(CallResult));
		TestTrue(TEXT("Repeated stop succeeds"), Session.StopAndUnload(CallResult));
		TestFalse(TEXT("Old VM released"), OriginalLease.IsValid());
		TestFalse(TEXT("Candidate VM released"), CandidateLease.IsValid());
		CheckEmpty(*this, *Owner, 0);
		if (HasAnyErrors()) return false;
		AddInfo(FString::Printf(TEXT("typed cancellation reload backend=%d stage=%d mode=%d committed=%d result=%d resources=0 runtimes=0"),
			static_cast<int32>(Backend), Stage, Mode, bReloaded ? 1 : 0, Expected));
	}
	AddInfo(TEXT("TypedCancellationReload: 24/24 passed"));
	return true;
}

#endif
