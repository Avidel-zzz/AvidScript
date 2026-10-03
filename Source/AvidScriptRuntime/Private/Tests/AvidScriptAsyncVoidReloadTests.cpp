#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptRuntimeSession.h"
#include "AvidScriptHash.h"
#include "AvidScriptObjectRegistry.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"

namespace AvidScript::Tests::AsyncVoidReload
{
struct FFixture
{
    FAvidScriptWasmReloadManifest Manifest;
    TArray<uint8> Bytes;

    bool Load(FAutomationTestBase& Test, const FString& Directory, const TCHAR* Name)
    {
        const FString Path = FPaths::Combine(Directory, FString(Name) + TEXT(".avidscript.json"));
        FAvidScriptWasmReloadManifestLoadResult Result;
        if (!Test.TestTrue(TEXT("Formal manifest checks executed WASM bytes"),
            FAvidScriptWasmReloadManifestLoader::LoadFromFile(Path, Manifest, Bytes, Result)))
        { Test.AddError(Result.ErrorMessage); return false; }
        TArray<uint8> IrBytes;
        FString ManifestText, IrText;
        TSharedPtr<FJsonObject> ManifestJson, Ir;
        if (!Test.TestTrue(TEXT("IR provenance and migration are available"),
            FFileHelper::LoadFileToString(ManifestText, *Path)
            && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestText), ManifestJson)
            && ManifestJson.IsValid()
            && FFileHelper::LoadFileToArray(IrBytes, *FPaths::Combine(Directory, FString(Name) + TEXT(".guestir.json")))
            && FAvidScriptHash::Sha256Hex(IrBytes) == ManifestJson->GetObjectField(TEXT("guest_ir"))->GetStringField(TEXT("sha256")))) return false;
        FFileHelper::BufferToString(IrText, IrBytes.GetData(), IrBytes.Num());
        if (!Test.TestTrue(TEXT("IR36 and actual migration owner match"),
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(IrText), Ir) && Ir.IsValid()
            && Ir->GetIntegerField(TEXT("schema_version")) == 36
            && Ir->GetStringField(TEXT("ir_version")) == TEXT("1.35")
            && Ir->GetStringField(TEXT("module_id")) == Manifest.ModuleId
            && Manifest.StateMigration.IsEnabled()
            && Manifest.StateMigration.OwnerTypeId == TEXT("type:global::Script")
            && Manifest.StateMigration.Slots.Num() == 5)) return false;
        for (const auto& Slot : Manifest.StateMigration.Slots)
        {
            const FString Field = Slot.StableId.RightChop(FString(TEXT("state:type:global::Script:")).Len());
            const FString Global = TEXT("global:symbol:field:global::Script.") + Field + TEXT(":int32");
            const auto* IrSlot = Ir->GetObjectField(TEXT("memory_layout"))->GetArrayField(TEXT("state_slots"))
                .FindByPredicate([&](const TSharedPtr<FJsonValue>& Item) { return Item->AsObject()->GetStringField(TEXT("global_id")) == Global; });
            if (!Test.TestTrue(TEXT("Readback slot comes from hashed compiler layout"), IrSlot
                && (*IrSlot)->AsObject()->GetIntegerField(TEXT("offset")) == Slot.Offset
                && Slot.Size == sizeof(int32) && Slot.Offset % 4 == 0)) return false;
        }
        return true;
    }

    int32 Read(FAutomationTestBase& Test, const FAvidScriptWasmRuntimeInstance& Runtime, const TCHAR* Field) const
    {
        const FString Id = FString(TEXT("state:type:global::Script:")) + Field;
        const auto* Slot = Manifest.StateMigration.Slots.FindByPredicate([&](const FAvidScriptWasmStateSlot& Item) { return Item.StableId == Id; });
        int32 Value = MIN_int32;
        FString Error;
        if (!Slot || !Runtime.ReadStateBytes(Slot->Offset,
            MakeArrayView(reinterpret_cast<uint8*>(&Value), sizeof(Value)), Error)) Test.AddError(TEXT("Readback failed: ") + Id + TEXT(" ") + Error);
        return Value;
    }
};

void CheckEmpty(FAutomationTestBase& Test, FAvidScriptSessionContinuations& Owner)
{
    Test.TestEqual(TEXT("Task records retired"), Owner.GetTaskResultsForTesting().GetCount(), 0);
    Test.TestEqual(TEXT("Task waiters retired"), Owner.GetTaskResultsForTesting().GetWaiterCount(), 0);
    Test.TestEqual(TEXT("State frames retired"), Owner.GetStateFrameByteCountForTesting(), 0);
    Test.TestEqual(TEXT("Active continuations retired"), Owner.GetActiveCount(), 0);
    Test.TestEqual(TEXT("Prepared continuations retired"), Owner.GetPreparedCount(), 0);
    Test.TestEqual(TEXT("Cancellation bindings retired"), Owner.GetCancellationBindingCountForTesting(), 0);
    for (const auto Lane : {EAvidScriptContinuationLane::Active, EAvidScriptContinuationLane::Prepared})
        Test.TestEqual(TEXT("Ready queue retired"), Owner.GetReadyCountForTesting(Lane), 0);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptAsyncVoidReloadTest,
    "AvidScript.Runtime.Continuation.AsyncVoidNaturalReload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptAsyncVoidReloadTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::AsyncVoidReload;
    if (!GEngine) return false;
    const FString Directory = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_ASYNC_VOID_RELOAD_FIXTURE_DIR"));
    FFixture Initial, Candidate;
    if (!Initial.Load(*this, Directory, TEXT("initial")) || !Candidate.Load(*this, Directory, TEXT("candidate"))) return false;
    if (!TestEqual(TEXT("Code update retains module identity"), Initial.Manifest.ModuleId, Candidate.Manifest.ModuleId)
        || !TestTrue(TEXT("Code update changes verified executable bytes"), Initial.Bytes != Candidate.Bytes)) return false;
    FString ReferenceText;
    TArray<TSharedPtr<FJsonValue>> References;
    if (!TestTrue(TEXT("Eight same-source .NET references exist"), FFileHelper::LoadFileToString(ReferenceText,
        *FPaths::Combine(Directory, TEXT("cases.json")))
        && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ReferenceText), References) && References.Num() == 8)) return false;
    auto Reference = [&](const TCHAR* Generation, int32 Mode) -> TSharedPtr<FJsonObject> {
        const auto* Found = References.FindByPredicate([&](const TSharedPtr<FJsonValue>& Value) {
            return Value->AsObject()->GetStringField(TEXT("generation")) == Generation && Value->AsObject()->GetIntegerField(TEXT("mode")) == Mode;
        });
        if (!Found) { AddError(TEXT("Missing .NET reference")); return nullptr; }
        return (*Found)->AsObject();
    };
    int32 Cases = 0;
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (int32 Stage = 0; Stage < 2; ++Stage)
    for (int32 Mode = 0; Mode < 4; ++Mode)
    {
        const bool Reject = Mode == 1, FaultAfterCommit = Mode == 2;
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        if (!TestNotNull(TEXT("Reload World created"), World)) return false;
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
        ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
        FAvidScriptObjectRegistry Registry;
        FAvidScriptRuntimeSession Session, Unaffected;
        FAvidScriptVmBackendSelection Selection;
        Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
            ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        auto Load = [&](FAvidScriptRuntimeSession& Target) -> AActor* {
            AActor* Actor = World->SpawnActor<AActor>();
            if (!Actor) return nullptr;
            USceneComponent* Root = NewObject<USceneComponent>(Actor);
            Actor->SetRootComponent(Root); Root->RegisterComponent();
            FAvidScriptObjectHandleResult Registered;
            FAvidScriptWasmHostContext Context;
            Context.World = World; Context.ObjectRegistry = &Registry;
            Context.OwnerHandle = Registry.RegisterObject(Actor, Registered, false);
            Context.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
            Target.SetBackendSelectionForTesting(Selection); Target.SetHostContext(Context);
            FAvidScriptWasmReloadResult Result;
            if (!Target.LoadInitialModule(Initial.Bytes.GetData(), Initial.Bytes.Num(), Initial.Manifest, Result))
            { AddError(Result.ErrorMessage); return nullptr; }
            return Actor;
        };
        AActor* Actor = Load(Session);
        AActor* OtherActor = Load(Unaffected);
        if (!TestNotNull(TEXT("Updating Actor exists"), Actor) || !TestNotNull(TEXT("Independent Actor exists"), OtherActor)) return false;
        ON_SCOPE_EXIT { FAvidScriptWasmSmokeResult Stopped; Session.StopAndUnload(Stopped); Unaffected.StopAndUnload(Stopped); };
        auto* Owner = Session.GetContinuationOwnerForTesting();
        auto* Original = Session.GetLiveRuntimeForTesting();
        const auto OriginalLease = Session.GetRuntimeLeaseForTesting();
        const auto OtherLease = Unaffected.GetRuntimeLeaseForTesting();
        FAvidScriptWasmSmokeResult Call;
        if (!Session.DispatchEventLive(Mode, 0.0f, Call)) { AddError(Call.ErrorMessage); return false; }
        if (Stage == 1)
        {
            World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
            if (!Session.TickLive(0.02f, Call)) { AddError(Call.ErrorMessage); return false; }
        }
        const int32 OldTasks = Owner->GetTaskResultsForTesting().GetCount();
        const int32 OldWaiters = Owner->GetTaskResultsForTesting().GetWaiterCount();
        const int32 OldFrames = Owner->GetStateFrameByteCountForTesting();
        const int32 OldReady = Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Active);
        const auto OldHeap = Original->GetManagedHeapForTesting()->GetStats();
        TArray<int32> OldState;
        for (const TCHAR* Field : {TEXT("BeginCount"), TEXT("Mode"), TEXT("Trace"), TEXT("Result"), TEXT("TickCount")})
            OldState.Add(Initial.Read(*this, *Original, Field));
        if (!TestTrue(TEXT("Old Task has genuine suspended state"), OldTasks > 0 && OldWaiters > 0 && OldFrames > 0)) return false;
        TWeakPtr<FAvidScriptWasmRuntimeInstance> CandidateLease;
        int32 Observed = 0;
        Session.SetCandidateBeginPlayCompletionObserverForTesting(
            [&](TWeakPtr<FAvidScriptWasmRuntimeInstance> Runtime, bool Began) {
                ++Observed; CandidateLease = Runtime;
                const auto Lease = Runtime.Pin();
                if (!TestTrue(TEXT("Observer sees an independent candidate"), Lease.IsValid() && Lease.Get() != Original)) return;
                TestEqual(TEXT("Natural C# error determines BeginPlay outcome"), Began, !Reject);
                TestEqual(TEXT("BeginCount migrates before one candidate execution"), Candidate.Read(*this, *Lease, TEXT("BeginCount")), 2);
                TestEqual(TEXT("Candidate receives source Mode"), Candidate.Read(*this, *Lease, TEXT("Mode")), Mode);
                TestEqual(TEXT("Candidate finally runs only on immediate failure"), Candidate.Read(*this, *Lease, TEXT("Trace")), Reject ? 1 : Mode == 3 ? 2 : 0);
                TestTrue(TEXT("Script Host ABI actually changed Actor before transaction decision"),
                    Actor->GetActorLocation().Equals(FVector(Reject ? 501.0 : 500.0, 600.0, 700.0), 0.01));
                TestTrue(TEXT("Candidate establishes its own suspended work"), Owner->GetPreparedCount() > 0);
                TestEqual(TEXT("Old heap roots unchanged during preparation"), Original->GetManagedHeapForTesting()->GetStats().LiveRoots, OldHeap.LiveRoots);
                if (Reject)
                {
                    TestEqual(TEXT("Guest report releases error roots before rejection"), Lease->GetManagedHeapForTesting()->GetStats().LiveRoots, 0u);
                    TestEqual(TEXT("Guest callback frames close before rejection"), Lease->GetManagedHeapForTesting()->GetStats().ActiveFrames, 0u);
                }
            });
        FAvidScriptWasmReloadResult Reload;
        if (!TestEqual(TEXT("Public reload outcome"), Session.ReloadModule(Candidate.Bytes.GetData(), Candidate.Bytes.Num(), Candidate.Manifest, Reload), !Reject))
        { AddError(Reload.ErrorMessage); return false; }
        TestEqual(TEXT("Exactly one candidate BeginPlay executes"), Observed, 1);
        TestTrue(TEXT("Formal state migration applied"), Reload.bStateMigrationApplied);
        TestTrue(TEXT("Host effect transaction opened"), Reload.bHostEffectTransactionAttempted);
        TestEqual(TEXT("Host effect commits only successful BeginPlay"), Reload.bHostEffectTransactionCommitted, !Reject);
        TestEqual(TEXT("Actor journal has one object"), Reload.HostEffectCapturedObjectCount, 1);
        TestEqual(TEXT("Actor journal restores failed candidate"), Reload.HostEffectRestoredObjectCount, Reject ? 1 : 0);
        TestEqual(TEXT("Host rollback succeeds on failed candidate"), Reload.bHostEffectRollbackSucceeded, Reject);
        TestEqual(TEXT("Old Runtime lives only when candidate rejects"), OriginalLease.IsValid(), Reject);
        TestEqual(TEXT("Candidate Runtime lives only when published"), CandidateLease.IsValid(), !Reject);
        TestEqual(TEXT("Prepared lane discarded or published"), Owner->GetPreparedCount(), 0);
        if (Reject)
        {
            TestEqual(TEXT("Natural language error reaches reload diagnostic"), Reload.ErrorCategory, FString(TEXT("language_error_uncaught")));
            TestEqual(TEXT("Report import retained in reload diagnostic"), Reload.RuntimeResult.ImportName, FString(TEXT("avid_language_error_report_v1")));
            TestTrue(TEXT("Original C# type and source retained"), Reload.ErrorMessage.Contains(TEXT("ArgumentException")) && Reload.ErrorMessage.Contains(TEXT("Scripts/AsyncVoidReload.cs")));
            TestTrue(TEXT("Rollback preserves exact old VM"), Session.GetLiveRuntimeForTesting() == Original && Reload.bRollbackPreservedLiveRuntime);
            TestTrue(TEXT("Actor Transform restored to old code result"), Actor->GetActorLocation().Equals(FVector(125, 600, 700), 0.01));
            TestEqual(TEXT("Old Task records survive exactly"), Owner->GetTaskResultsForTesting().GetCount(), OldTasks);
            TestEqual(TEXT("Old waiters survive exactly"), Owner->GetTaskResultsForTesting().GetWaiterCount(), OldWaiters);
            TestEqual(TEXT("Old state frames survive exactly"), Owner->GetStateFrameByteCountForTesting(), OldFrames);
            TestEqual(TEXT("Old ready queue survives exactly"), Owner->GetReadyCountForTesting(EAvidScriptContinuationLane::Active), OldReady);
            int32 Index = 0;
            for (const TCHAR* Field : {TEXT("BeginCount"), TEXT("Mode"), TEXT("Trace"), TEXT("Result"), TEXT("TickCount")})
                TestEqual(TEXT("Old source state untouched by candidate"), Initial.Read(*this, *Original, Field), OldState[Index++]);
            // Mode was changed in the old Session to prepare the candidate;
            // restore its normal mode via the real event entry so its pending
            // task demonstrates old code completion rather than a second fault.
            if (!Session.DispatchEventLive(0, 0.0f, Call)) return false;
        }
        TestFalse(TEXT("Preparation never quarantines survivor"), Session.GetSnapshot().bFaultQuarantined);
        bool Faulted = false;
        for (int32 Round = 0; Round < 12 && Owner->GetActiveCount() > 0; ++Round)
        {
            World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
            if (!Session.TickLive(0.02f, Call)) { Faulted = true; break; }
            if (!Session.CollectManagedHeapForTesting()) return false;
        }
        TestEqual(TEXT("Only postcommit source error quarantines"), Faulted, FaultAfterCommit);
        const auto Expected = Reference(Reject ? TEXT("initial") : TEXT("candidate"), Reject ? 0 : Mode);
        if (!Expected) return false;
        TestTrue(TEXT("Finally side effect matches identical .NET source"), Actor->GetActorLocation().Equals(
            FVector(Expected->GetIntegerField(TEXT("location")), 600, 700), 0.01));
        if (FaultAfterCommit)
        {
            TestEqual(TEXT("Postcommit fault remains language error"), Call.ErrorCategory, FString(TEXT("language_error_uncaught")));
            TestEqual(TEXT("Postcommit fault retains import"), Call.ImportName, FString(TEXT("avid_language_error_report_v1")));
            const auto Snapshot = Session.GetSnapshot();
            TestTrue(TEXT("Published generation quarantined"), Snapshot.bFaultQuarantined && Snapshot.FaultDiagnostic.Contains(TEXT("ArgumentException")));
            TestFalse(TEXT("Postcommit fault unloads candidate"), Session.IsLiveLoaded());
            TestFalse(TEXT("Postcommit fault cannot revive old VM"), OriginalLease.IsValid());
            TestFalse(TEXT("Published candidate retires on fault"), CandidateLease.IsValid());
            TestFalse(TEXT("Quarantined Session refuses reentry"), Session.TickLive(0.02f, Call));
        }
        else
        {
            auto* Survivor = Session.GetLiveRuntimeForTesting();
            TestEqual(TEXT("Surviving generation result matches .NET"), Initial.Read(*this, *Survivor, TEXT("Result")), Expected->GetIntegerField(TEXT("result")));
            TestEqual(TEXT("Finally executes exactly once"), Initial.Read(*this, *Survivor, TEXT("Trace")), Expected->GetIntegerField(TEXT("trace")));
            TestEqual(TEXT("Rollback did not rerun old BeginPlay"), Initial.Read(*this, *Survivor, TEXT("BeginCount")), Reject ? 1 : 2);
            if (!Session.CollectManagedHeapForTesting()) return false;
            const auto Heap = Survivor->GetManagedHeapForTesting()->GetStats();
            TestEqual(TEXT("Survivor error roots released before stop"), Heap.LiveRoots, 0u);
            TestEqual(TEXT("Survivor error objects collected before stop"), Heap.LiveObjects, 0u);
            TestEqual(TEXT("Survivor managed frames released before stop"), Heap.ActiveFrames, 0u);
        }
        CheckEmpty(*this, *Owner);
        TestTrue(TEXT("Independent Session retains exact VM"), Unaffected.GetLiveRuntimeForTesting() == OtherLease.Pin().Get());
        TestFalse(TEXT("Independent Session not quarantined"), Unaffected.GetSnapshot().bFaultQuarantined);
        auto* OtherOwner = Unaffected.GetContinuationOwnerForTesting();
        for (int32 Round = 0; Round < 12 && OtherOwner->GetActiveCount() > 0; ++Round)
        {
            World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
            if (!Unaffected.TickLive(0.02f, Call)) { AddError(Call.ErrorMessage); return false; }
        }
        TestEqual(TEXT("Independent source completes its original code"), Initial.Read(*this, *Unaffected.GetLiveRuntimeForTesting(), TEXT("Result")), 17);
        TestTrue(TEXT("Independent Actor unaffected by candidate"), OtherActor->GetActorLocation().Equals(FVector(126, 600, 700), 0.01));
        CheckEmpty(*this, *OtherOwner);
        TestTrue(TEXT("Public stop succeeds"), Session.StopAndUnload(Call));
        TestTrue(TEXT("Public stop remains idempotent"), Session.StopAndUnload(Call));
        TestTrue(TEXT("Independent stop succeeds"), Unaffected.StopAndUnload(Call));
        TestFalse(TEXT("No old VM remains"), OriginalLease.IsValid());
        TestFalse(TEXT("No candidate VM remains"), CandidateLease.IsValid());
        TestFalse(TEXT("No independent VM remains"), OtherLease.IsValid());
        CheckEmpty(*this, *Owner); CheckEmpty(*this, *OtherOwner);
        if (HasAnyErrors()) return false;
        ++Cases;
        AddInfo(FString::Printf(TEXT("async-void-reload backend=%d stage=%d mode=%d committed=%d fault=%d resources=0 runtimes=0"),
            static_cast<int32>(Backend), Stage, Mode, Reject ? 0 : 1, Faulted ? 1 : 0));
    }
    TestEqual(TEXT("Two VM backends, two old suspension stages, four source outcomes"), Cases, 16);
    return true;
}

#endif
