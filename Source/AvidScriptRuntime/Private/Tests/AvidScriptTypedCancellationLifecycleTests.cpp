#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptTypedCancellationTestSupport.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTypedCancellationLifecycleTest,
	"AvidScript.Runtime.Continuation.TypedCancellationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTypedCancellationLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Tests::TypedCancellation;
	if (!GEngine) return false;
	FFixture Fixture;
	if (!Fixture.Load(*this, FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_TYPED_CANCELLATION_MANIFEST")))) return false;
	enum class EScenario { Session, World, Object, Discard, Commit, CommitCancelled };
	const TCHAR* Names[] = {TEXT("session"), TEXT("world"), TEXT("object"), TEXT("discard"), TEXT("commit"), TEXT("commit_cancelled")};
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (int32 Stage = 0; Stage < 3; ++Stage)
	for (const auto Scenario : {EScenario::Session, EScenario::World, EScenario::Object,
		EScenario::Discard, EScenario::Commit, EScenario::CommitCancelled})
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		if (!TestNotNull(TEXT("Lifecycle world created"), World)) return false;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
		FAvidScriptObjectRegistry Registry;
		FAvidScriptSessionObjectOwnership Ownership;
		AActor* Actor = World->SpawnActor<AActor>();
		if (!TestNotNull(TEXT("Owner actor spawned"), Actor)) return false;
		FAvidScriptObjectHandleResult Registered;
		const auto Handle = Registry.RegisterObject(Actor, Registered, false);
		if (!TestTrue(TEXT("Owner registered"), Handle.IsValid())) return false;
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		FAvidScriptWasmRuntimeInstance Active(Selection(Backend)), Candidate(Selection(Backend));
		ON_SCOPE_EXIT
		{
			Active.SetHostContext({}); Candidate.SetHostContext({});
			Owner->Teardown(); Ownership.Cleanup(Registry);
		};
		FAvidScriptWasmSmokeResult Result;
		auto Read = [&](const FAvidScriptWasmRuntimeInstance& Runtime, const TCHAR* Field)
		{ return Fixture.Read(*this, Runtime, TEXT("CancellationScript"), Field); };
		auto Start = [&](FAvidScriptWasmRuntimeInstance& Runtime, FAvidScriptContinuationHostEndpoint& Endpoint)
		{
			if (!Runtime.LoadModule(Fixture.Bytes.GetData(), Fixture.Bytes.Num(), Fixture.Manifest.ModuleId, Result)
				|| !Runtime.ValidateRequiredExports({TEXT("avid_on_begin_play"), TEXT("avid_on_tick"),
					TEXT("avid_on_end_play"), TEXT("avid_on_continuation_v2")}, Result))
			{ AddError(Result.ErrorMessage); return false; }
			FAvidScriptWasmHostContext Context;
			Context.Tasks = &Endpoint; Context.Continuations = &Endpoint; Context.World = World;
			Context.ObjectRegistry = &Registry; Context.OwnerHandle = Handle;
			Runtime.SetHostContext(Context);
			if (!Runtime.BeginPlay(Result)) { AddError(Result.ErrorMessage); return false; }
			return true;
		};
		auto& ActiveEndpoint = Owner->ResetActive(World, &Registry, &Ownership, Handle);
		if (!Start(Active, ActiveEndpoint) || !Active.Tick(-1.0f, Result))
		{ AddError(Result.ErrorMessage); return false; }
		TestEqual(TEXT("Direct cancellation queued before any resume"), Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Active), 1);
		for (int32 Step = 0; Step < Stage; ++Step)
		{
			TArray<FAvidScriptContinuationCompletion> Ready;
			Owner->DrainReady(Ready);
			if (!TestEqual(TEXT("One cancellation advances each selected stage"), Ready.Num(), 1)) return false;
			TestTrue(TEXT("Selected stage dispatch is cancellation"), Ready[0].Status == EAvidScriptContinuationStatus::Cancelled);
			if (!Active.DispatchContinuation(Ready[0], Result)) { AddError(Result.ErrorMessage); return false; }
			TestTrue(TEXT("Selected callback finalizes"), Owner->FinalizeDispatched(Ready[0].Token, true));
		}
		TestEqual(TEXT("Inner handler has run only after the first stage"), Read(Active, TEXT("InnerCatch")), Stage > 0 ? 1 : 0);
		TestEqual(TEXT("Outer handler has run only after the second stage"), Read(Active, TEXT("OuterCatch")), Stage == 2 ? 1 : 0);
		TestEqual(TEXT("Conditional finally precedes its next wait"), Read(Active, TEXT("ConditionalTrace")), Stage == 2 ? 1 : 0);
		TestEqual(TEXT("Two Task owners exist at every retirement stage"), Owner->GetTaskResultsForTesting().GetCount(), 2);
		TestTrue(TEXT("Suspension owns state bytes"), Owner->GetStateFrameByteCountForTesting() > 0);
		if (Stage > 0) TestTrue(TEXT("Typed cancellation object remains rooted"), Active.GetManagedHeapForTesting()->GetStats().LiveRoots > 0);
		const int32 OldTrace = Read(Active, TEXT("Trace"));
		const int32 OldConditional = Read(Active, TEXT("ConditionalTrace"));
		const bool bDiscard = Scenario == EScenario::Discard;
		const bool bPrepared = bDiscard || Scenario == EScenario::Commit || Scenario == EScenario::CommitCancelled;
		FAvidScriptWasmRuntimeInstance* Survivor = nullptr;
		if (bPrepared)
		{
			auto& PreparedEndpoint = Owner->BeginPrepared(World, &Registry, &Ownership, Handle);
			if (!Start(Candidate, PreparedEndpoint)) return false;
			if (Scenario != EScenario::Commit && !Candidate.Tick(-1.0f, Result)) { AddError(Result.ErrorMessage); return false; }
			TestEqual(TEXT("Both lanes own distinct Task records"), Owner->GetTaskResultsForTesting().GetCount(), 4);
			TestEqual(TEXT("Prepared cancellation stays in its lane"), Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Prepared),
				Scenario == EScenario::Commit ? 0 : 1);
			if (bDiscard)
			{
				Candidate.SetHostContext({});
				Owner->DiscardPrepared();
				CheckHeapReleased(*this, Candidate);
				Survivor = &Active;
			}
			else
			{
				FString Error;
				if (!Owner->ValidatePreparedCommit(Error)) { AddError(Error); return false; }
				Owner->CommitPrepared();
				TestEqual(TEXT("Retired endpoint rejects new work"), ActiveEndpoint.ScheduleDelay(0.0f, 999), 0LL);
				Active.SetHostContext({}); Owner->ReleaseRetiredEndpoint();
				CheckHeapReleased(*this, Active);
				Survivor = &Candidate;
			}
			TestEqual(TEXT("Only survivor Tasks remain"), Owner->GetTaskResultsForTesting().GetCount(), 2);
			TestEqual(TEXT("Only survivor cancellation source remains"), Owner->GetCancellationSourceCountForTesting(), 1);
		}
		else if (Scenario == EScenario::Session) Owner->Teardown();
		else if (Scenario == EScenario::World) World->BeginTearingDown();
		else
		{
			if (!TestTrue(TEXT("Owner actor actually destroyed"), World->DestroyActor(Actor))) return false;
			FAvidScriptObjectHandleResult ResolveResult;
			TestNull(TEXT("Destroyed owner cannot resolve"), Registry.ResolveObject(Handle, ResolveResult, false));
		}
		int32 Resumes = 0;
		int32 Cancelled = 0;
		for (int32 Round = 0; Round < 12; ++Round)
		{
			if (Scenario != EScenario::World) { World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter; }
			TArray<FAvidScriptContinuationCompletion> Ready;
			Owner->DrainReady(Ready);
			for (const auto& Completion : Ready)
			{
				if (!TestNotNull(TEXT("Only survivor can resume"), Survivor)) return false;
				if (!Survivor->DispatchContinuation(Completion, Result)) { AddError(Result.ErrorMessage); return false; }
				TestTrue(TEXT("Surviving callback finalizes"), Owner->FinalizeDispatched(Completion.Token, true));
				TestTrue(TEXT("Survivor remains valid across collection"), Survivor->GetManagedHeapForTesting()->Collect() == AvidScript::Managed::EHeapError::Ok);
				++Resumes;
				Cancelled += Completion.Status == EAvidScriptContinuationStatus::Cancelled ? 1 : 0;
			}
		}
		TestEqual(TEXT("Only unconsumed survivor callbacks execute"), Resumes, !Survivor ? 0 : bDiscard ? 4 - Stage : 4);
		TestEqual(TEXT("Cancelled callbacks preserve exactly the surviving stage"), Cancelled,
			bDiscard ? 2 - Stage : Scenario == EScenario::CommitCancelled ? 2 : 0);
		if (!bDiscard)
		{
			TestEqual(TEXT("Retirement cannot rerun old cleanup"), Read(Active, TEXT("Trace")), OldTrace);
			TestEqual(TEXT("Retirement cannot finish old suspended code"), Read(Active, TEXT("ConditionalTrace")), OldConditional);
		}
		if (bDiscard) TestEqual(TEXT("Discarded candidate never runs cleanup"), Read(Candidate, TEXT("Trace")), 0);
		if (Survivor)
		{
			TestEqual(TEXT("Survivor returns its own result"), Fixture.Read(*this, *Survivor, TEXT("CancellationLifecycleEntry"), TEXT("Result")),
				Scenario == EScenario::Commit ? 16 : 30);
			TestEqual(TEXT("Survivor completes cleanup once"), Read(*Survivor, TEXT("ConditionalTrace")), 12);
		}
		CheckHeapReleased(*this, Active);
		if (bPrepared) CheckHeapReleased(*this, Candidate);
		CheckEmpty(*this, *Owner, Survivor ? 1 : 0);
		Active.SetHostContext({}); Candidate.SetHostContext({});
		Owner->Teardown(); Owner->Teardown();
		CheckEmpty(*this, *Owner, 0);
		if (HasAnyErrors()) return false;
		AddInfo(FString::Printf(TEXT("typed cancellation lifecycle backend=%d stage=%d scenario=%s resumes=%d cancelled=%d resources=0"),
			static_cast<int32>(Backend), Stage, Names[static_cast<int32>(Scenario)], Resumes, Cancelled));
	}
	AddInfo(TEXT("TypedCancellationLifecycle: 36/36 passed"));
	return true;
}

#endif
