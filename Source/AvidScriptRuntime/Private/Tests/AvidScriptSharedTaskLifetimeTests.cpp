#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGeneratedTypeSessionTestTypes.h"
#include "AvidScriptTypedCancellationTestSupport.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

namespace AvidScript::Tests::SharedTaskLifetime
{
struct FCompiledFixture
{
    TArray<uint8> Bytes;
    FAvidScriptWasmReloadManifest Manifest;
    TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types;
    int32 CompletedOffset = 0;
    uint32 EntryOrdinal = 0;

    bool Load(FAutomationTestBase& Test, const FString& Directory, int32 Offset)
    {
        const FString Stem = Directory / FString::Printf(TEXT("shared-task-%d"), Offset);
        FString Json, Error;
        TSharedPtr<FJsonObject> Metadata;
        if (!FFileHelper::LoadFileToArray(Bytes, *(Stem + TEXT(".wasm")))
            || !FFileHelper::LoadFileToString(Json, *(Stem + TEXT(".json")))
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Metadata)) return false;
        if (!Test.TestEqual(TEXT("Executable bytes match compiler metadata"),
                FAvidScriptHash::Sha256Hex(Bytes), Metadata->GetStringField(TEXT("wasm_sha256")))) return false;
        auto Registry = MakeShared<FJsonObject>();
        Registry->SetNumberField(TEXT("schema_version"), 6);
        Registry->SetStringField(TEXT("generator_version"), TEXT("1.8"));
        Registry->SetStringField(TEXT("module_name"), TEXT("AvidScriptRuntime"));
        Registry->SetStringField(TEXT("generation_key_sha256"), FString::ChrN(64, TEXT('a')));
        auto Type = MakeShared<FJsonObject>();
        Type->SetNumberField(TEXT("type_ordinal"), 0);
        Type->SetStringField(TEXT("stable_type_id"), Metadata->GetStringField(TEXT("type_id")));
        Type->SetStringField(TEXT("engine_name"), TEXT("AvidScriptGeneratedTypeSessionTestObject"));
        Type->SetStringField(TEXT("class_path"), UAvidScriptGeneratedTypeSessionTestObject::StaticClass()->GetPathName());
        Type->SetArrayField(TEXT("properties"), Metadata->GetArrayField(TEXT("properties")));
        auto Function = MakeShared<FJsonObject>();
        EntryOrdinal = Metadata->GetIntegerField(TEXT("member_ordinal"));
        Function->SetNumberField(TEXT("member_ordinal"), EntryOrdinal);
        Function->SetStringField(TEXT("stable_member_id"), Metadata->GetStringField(TEXT("method_id")));
        Function->SetStringField(TEXT("native_name"), TEXT("GetScriptValue"));
        Function->SetStringField(TEXT("export_name"), Metadata->GetStringField(TEXT("export_name")));
        Function->SetArrayField(TEXT("flags"), {});
        Type->SetArrayField(TEXT("functions"), {MakeShared<FJsonValueObject>(Function)});
        Registry->SetArrayField(TEXT("types"), {MakeShared<FJsonValueObject>(Type)});
        FString RegistryJson;
        if (!FJsonSerializer::Serialize(Registry, TJsonWriterFactory<>::Create(&RegistryJson))
            || !FAvidScriptGeneratedTypeRegistry::BuildFromJson(RegistryJson, Types, Error))
        { Test.AddError(Error); return false; }
        Manifest.ModuleId = Metadata->GetStringField(TEXT("module_id"));
        Manifest.Language = TEXT("csharp");
        Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
        Manifest.RequiredExports = {TEXT("avid_on_begin_play"), TEXT("avid_on_continuation_v2"),
            Metadata->GetStringField(TEXT("export_name"))};
        for (const auto& Import : Metadata->GetArrayField(TEXT("imports")))
            Manifest.RequiredImports.Add({Import->AsObject()->GetStringField(TEXT("module")),
                Import->AsObject()->GetStringField(TEXT("name"))});
        CompletedOffset = Metadata->GetIntegerField(TEXT("completed_offset"));
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptSharedTaskLifetimeTest,
    "AvidScript.Runtime.GeneratedTypes.SharedTaskLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSharedTaskLifetimeTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests;
    if (!GEngine) return false;
    const FString Directory = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_SHARED_TASK_FIXTURE_DIR"));
    SharedTaskLifetime::FCompiledFixture Initial, Next;
    if (!Initial.Load(*this, Directory, 0) || !Next.Load(*this, Directory, 16)) return false;
    TestEqual(TEXT("Different code generations retain module identity"), Initial.Manifest.ModuleId, Next.Manifest.ModuleId);
    TestTrue(TEXT("Reload executes different WASM"), Initial.Bytes != Next.Bytes);
    int32 Cases = 0;
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (int32 Mode = 0; Mode < 8; ++Mode)
    for (const bool Cancel : {false, true})
    {
        // Complete, retire one owner, collect it, World teardown, commit,
        // fail preparation after one/two instances, and fault the shared domain.
        if (Mode == 7 && Cancel) continue;
        const FString Label = FString::Printf(TEXT("backend=%d mode=%d cancel=%d"), static_cast<int32>(Backend), Mode, Cancel);
        AddInfo(TEXT("shared Task start ") + Label);
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        UWorld* OtherWorld = UWorld::CreateWorld(EWorldType::Game, false);
        if (!World || !OtherWorld) return false;
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(OtherWorld);
        World->InitializeActorsForPlay(FURL()); OtherWorld->InitializeActorsForPlay(FURL());
        ON_SCOPE_EXIT {
            if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
            GEngine->DestroyWorldContext(OtherWorld); OtherWorld->DestroyWorld(false);
        };
        auto Host = FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
        ON_SCOPE_EXIT { Host->Shutdown(); };
        TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(World));
        TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> B(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(World));
        TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Other(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(OtherWorld));
        A->Value = Mode == 7 ? -2 : Cancel ? -1 : 5;
        B->Value = 10; Other->Value = 20;
        const auto Selection = TypedCancellation::Selection(Backend);
        const auto Artifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Initial.Manifest, Initial.Bytes, Selection);
        const auto Candidate = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Next.Manifest, Next.Bytes, Selection);
        FString Error;
        if (!Host->InstallPackage(Initial.Types, Artifact, Error) || !Host->BeginInstance(*A, 0, Error)
            || !Host->BeginInstance(*B, 0, Error) || !Host->BeginInstance(*Other, 0, Error))
        { AddError(Label + TEXT(": ") + Error); return false; }
        auto* SA = Host->GetInstanceSessionForTesting(*A);
        auto* SB = Host->GetInstanceSessionForTesting(*B);
        auto* SO = Host->GetInstanceSessionForTesting(*Other);
        auto* Original = SA->GetLiveRuntimeForTesting();
        auto Lease = SA->GetRuntimeLeaseForTesting();
        auto OtherLease = SO->GetRuntimeLeaseForTesting();
        const auto OldState = SA->GetTestSnapshot().HostContext.InstanceExecutionState;
        auto ReadCompleted = [&](const FAvidScriptRuntimeSession& Session) {
            int32 Value = -1;
            FString ReadError;
            if (!Session.GetLiveRuntimeForTesting()->ReadStateBytes(Initial.CompletedOffset,
                    MakeArrayView(reinterpret_cast<uint8*>(&Value), sizeof(Value)), ReadError)) AddError(ReadError);
            return Value;
        };
        TestTrue(TEXT("Same World instances share VM and heap"), Original == SB->GetLiveRuntimeForTesting());
        TestTrue(TEXT("Other World has separate VM and heap"), Original != SO->GetLiveRuntimeForTesting());
        auto Start = [&](UObject* Object) {
            int32 Result = 0;
            return TestTrue(*Label, FAvidScriptGeneratedTypeDispatcher::Invoke(Object, 0, Initial.EntryOrdinal, {}, &Result));
        };
        if (!Start(A.Get()) || !Start(B.Get()) || !Start(Other.Get())) return false;
        TestEqual(TEXT("Each instance owns its three distinct Task records"), SA->GetContinuationOwnerForTesting()->GetTaskResultsForTesting().GetCount(), 3);
        TestEqual(*Label, SB->GetContinuationOwnerForTesting()->GetTaskResultsForTesting().GetCount(), 3);
        TestEqual(*Label, SO->GetContinuationOwnerForTesting()->GetTaskResultsForTesting().GetCount(), 3);
        FAvidScriptWasmSmokeResult Tick;
        if (Cancel)
        {
            // Dispatch the two child cancellations but leave the parent queued.
            for (int32 Step = 0; Step < 2; ++Step)
                if (!SA->TickLive(0.0f, Tick)) { AddError(Tick.ErrorMessage); return false; }
            auto* Heap = Original->GetManagedHeapForTesting();
            TestTrue(TEXT("Unhandled child cancellations own typed language errors"), Heap->GetStats().LiveRoots >= 2);
            const auto Objects = Heap->GetStats().LiveObjects;
            TestTrue(*Label, Heap->Collect() == AvidScript::Managed::EHeapError::Ok);
            TestEqual(TEXT("Both cancelled Task errors survive GC"), Heap->GetStats().LiveObjects, Objects);
        }
        const int32 Frames = SA->GetContinuationOwnerForTesting()->GetStateFrameByteCountForTesting();
        const int32 Ready = SA->GetContinuationOwnerForTesting()->GetReadyCountForTesting(EAvidScriptContinuationLane::Active);
        const uint32 Roots = Original->GetManagedHeapForTesting()->GetStats().LiveRoots;
        if (Mode == 1)
        {
            if (!Host->EndInstance(*A, Error)) { AddError(Error); return false; }
            SA = nullptr;
        }
        else if (Mode == 2)
        {
            TWeakObjectPtr<UObject> WeakA(A.Get());
            A.Reset(); CollectGarbage(RF_NoFlags);
            TestFalse(TEXT("Suspended Task does not retain its UObject"), WeakA.IsValid());
            FTSTicker::GetCoreTicker().Tick(0.0f);
            SA = nullptr;
        }
        else if (Mode == 3)
        {
            World->BeginTearingDown();
            // BeginTearingDown marks the World unavailable; the dispatch gate
            // cancels its pending work. Cleanup later retires the Session/VM,
            // leaving UE terminal lifecycle callbacks their normal ordering.
            for (auto* Session : {SA, SB})
            {
                TArray<FAvidScriptContinuationCompletion> ReadyAfterTeardown;
                Session->GetContinuationOwnerForTesting()->DrainReady(ReadyAfterTeardown);
                TestEqual(TEXT("Tearing-down World cannot dispatch queued callbacks"), ReadyAfterTeardown.Num(), 0);
            }
            TypedCancellation::CheckEmpty(*this, *SA->GetContinuationOwnerForTesting(), 0);
            TypedCancellation::CheckEmpty(*this, *SB->GetContinuationOwnerForTesting(), 0);
            TypedCancellation::CheckHeapReleased(*this, *Original);
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
            World = nullptr;
            TestFalse(TEXT("World cleanup unloads both shared Sessions"), SA->IsLiveLoaded() || SB->IsLiveLoaded());
            TestFalse(TEXT("World cleanup releases shared VM"), Lease.IsValid());
            TestTrue(TEXT("World cleanup preserves other-World VM"), OtherLease.IsValid());
        }
        else if (Mode >= 4 && Mode <= 6)
        {
            if (Mode != 4) Host->SetReloadFailureAfterInstanceCountForTesting(Mode - 4);
            FAvidScriptGeneratedTypePackageReloadResult Reload;
            const bool Applied = Host->ReloadPackage(Next.Types, Candidate, Reload, Error);
            if (!TestEqual(*Label, Applied, Mode == 4)) { AddError(Error); return false; }
            if (Mode == 4)
            {
                TestEqual(TEXT("Package publishes every instance across Worlds"), Reload.ReloadedInstanceCount, 3);
                TestFalse(TEXT("Commit releases old shared VM"), Lease.IsValid());
                TestFalse(TEXT("Commit releases old other-World VM"), OtherLease.IsValid());
                TypedCancellation::CheckEmpty(*this, *SA->GetContinuationOwnerForTesting(), 0);
                TypedCancellation::CheckEmpty(*this, *SB->GetContinuationOwnerForTesting(), 0);
                TypedCancellation::CheckEmpty(*this, *SO->GetContinuationOwnerForTesting(), 0);
                TestTrue(TEXT("Published peers still share one VM"), SA->GetLiveRuntimeForTesting() == SB->GetLiveRuntimeForTesting());
                if (!Start(A.Get()) || !Start(B.Get()) || !Start(Other.Get())) return false;
            }
            else
            {
                TestEqual(*Label, Reload.PreparedInstanceCount, Mode - 4);
                TestEqual(*Label, Reload.RolledBackInstanceCount, Mode - 4);
                TestTrue(TEXT("Rollback preserves original package"), Reload.bRollbackPreservedLivePackage);
                TestTrue(TEXT("Rollback keeps exact VM identity"), SA->GetLiveRuntimeForTesting() == Original);
                TestTrue(TEXT("Rollback keeps exact instance state"), SA->GetTestSnapshot().HostContext.InstanceExecutionState == OldState);
                TestEqual(*Label, SA->GetContinuationOwnerForTesting()->GetStateFrameByteCountForTesting(), Frames);
                TestEqual(*Label, SA->GetContinuationOwnerForTesting()->GetReadyCountForTesting(EAvidScriptContinuationLane::Active), Ready);
                TestEqual(*Label, Original->GetManagedHeapForTesting()->GetStats().LiveRoots, Roots);
                TestTrue(*Label, Lease.IsValid() && OtherLease.IsValid());
            }
        }
        if (Mode == 1 || Mode == 2)
        {
            TestTrue(TEXT("Retiring one owner keeps peer VM alive"), Lease.IsValid());
            TestTrue(TEXT("Retired owner state is marked"), OldState->IsRetired());
            TestEqual(TEXT("Peer retains its own replacement Tasks"), SB->GetContinuationOwnerForTesting()->GetTaskResultsForTesting().GetCount(), 3);
            TypedCancellation::CheckHeapReleased(*this, *SB->GetLiveRuntimeForTesting());
        }
        bool FaultObserved = false;
        for (int32 Round = 0; Round < 24; ++Round)
        {
            if (Mode != 3) { World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter; }
            OtherWorld->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
            if (Mode != 3 && !FaultObserved)
            {
                if (SA && !SA->TickLive(0.02f, Tick))
                {
                    if (Mode != 7) { AddError(Label + TEXT(": ") + Tick.ErrorMessage); return false; }
                    FaultObserved = true;
                }
                if (!FaultObserved && !SB->TickLive(0.02f, Tick)) { AddError(Tick.ErrorMessage); return false; }
            }
            if (!SO->TickLive(0.02f, Tick)) { AddError(Tick.ErrorMessage); return false; }
        }
        const int32 Delta = Mode == 4 ? 32 : 0;
        TestEqual(TEXT("Other World keeps independent tasks and code generation"), Other->Value, 41 + Delta);
        TestEqual(TEXT("Other World has its own static completion count"), ReadCompleted(*SO), 1);
        TypedCancellation::CheckEmpty(*this, *SO->GetContinuationOwnerForTesting(), 0);
        TypedCancellation::CheckHeapReleased(*this, *SO->GetLiveRuntimeForTesting());
        if (Mode == 7)
        {
            TestTrue(TEXT("Actual resumed Guest trap occurred"), FaultObserved);
            TestTrue(TEXT("Shared fault quarantines both peers"), SA->GetSnapshot().bFaultQuarantined && SB->GetSnapshot().bFaultQuarantined);
            TestFalse(TEXT("Fault releases shared VM"), Lease.IsValid());
            TypedCancellation::CheckEmpty(*this, *SA->GetContinuationOwnerForTesting(), 0);
            TypedCancellation::CheckEmpty(*this, *SB->GetContinuationOwnerForTesting(), 0);
            int32 Rejected = 0;
            TestFalse(TEXT("Peer cannot reenter quarantined domain"),
                FAvidScriptGeneratedTypeDispatcher::Invoke(B.Get(), 0, Initial.EntryOrdinal, {}, &Rejected));
        }
        else if (Mode == 3)
        {
            TestEqual(TEXT("World teardown prevents peer completion"), B->Value, 10);
            TestEqual(TEXT("World teardown prevents owner completion"), A->Value, Cancel ? -1 : 5);
        }
        else
        {
            TestEqual(TEXT("Peer completes from its own state"), B->Value, 21 + Delta);
            if (SA) TestEqual(TEXT("Owner cancellation never cancels peer"), A->Value, Cancel ? 90 : 11 + Delta);
            else if (A) TestEqual(TEXT("Retired owner never resumes"), A->Value, Cancel ? -1 : 5);
            TypedCancellation::CheckEmpty(*this, *SB->GetContinuationOwnerForTesting(), 0);
            if (SA) TypedCancellation::CheckEmpty(*this, *SA->GetContinuationOwnerForTesting(), 0);
            TypedCancellation::CheckHeapReleased(*this, *SB->GetLiveRuntimeForTesting());
            TestEqual(TEXT("Shared statics count only surviving instance completions"), ReadCompleted(*SB), SA ? 2 : 1);
        }
        auto FinalShared = SB->GetRuntimeLeaseForTesting();
        auto FinalOther = SO->GetRuntimeLeaseForTesting();
        Host->Shutdown();
        TestFalse(TEXT("Final shared VM lease released"), FinalShared.IsValid());
        TestFalse(TEXT("Final other World VM lease released"), FinalOther.IsValid());
        TestEqual(TEXT("All generated instances removed"), Host->GetActiveInstanceCount(), 0);
        TestEqual(TEXT("All object handles removed"), Host->GetRegisteredHandleCount(), 0);
        if (HasAnyErrors()) return false;
        ++Cases;
        AddInfo(TEXT("shared Task passed ") + Label);
    }
    TestEqual(TEXT("Shared Task scenario count"), Cases, 30);
    AddInfo(TEXT("SharedTaskLifetime: 30/30 passed"));
    return true;
}

#endif
