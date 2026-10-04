#if WITH_DEV_AUTOMATION_TESTS && AVIDSCRIPT_WITH_GENERATED_TYPES && AVIDSCRIPT_WITH_GENERATED_NATURAL_RELOAD_TESTS

#include "AvidScriptGeneratedTypes.h"
#include "AvidScriptHash.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace AvidScript::Tests::GeneratedNaturalReload
{
UWorld* CreateWorld()
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (World) GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    return World;
}

void DestroyWorld(UWorld*& World)
{
    if (!World) return;
    if (World->HasBegunPlay()) World->EndPlay(EEndPlayReason::Quit);
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    World = nullptr;
}

struct FOwner
{
    UObject* Object = nullptr;
    FAvidScriptRuntimeSession* Session = nullptr;
    int32 Seed = 0;
    int32 Property(const FName Name) const
    {
        const auto* Field = FindFProperty<FIntProperty>(Object->GetClass(), Name);
        return Field ? Field->GetPropertyValue_InContainer(Object) : MIN_int32;
    }
};

struct FPackage
{
    TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types;
    FAvidScriptRuntimeArtifact Artifact;

    bool Load(FAutomationTestBase& Test, FAvidScriptGeneratedTypeRuntimeHost& Host, const FString& Descriptor)
    {
        FString Error;
        if (!Host.LoadPackageForTesting(Descriptor, Types, Artifact, Error)) { Test.AddError(Error); return false; }
        return Test.TestFalse(TEXT("Formal Development package has no backend-specific replacement bytes"), Artifact.bUsesPrecompiledArtifact)
            && Test.TestEqual(TEXT("Loaded canonical bytes match the production manifest"),
                FAvidScriptHash::Sha256Hex(Artifact.VmArtifact.CanonicalWasmBytes), Artifact.Manifest.WasmSha256)
            && Test.TestEqual(TEXT("Shared domain migrates the original three static fields once"), Artifact.Manifest.StateMigration.Slots.Num(), 3)
            && Test.TestEqual(TEXT("Migration belongs to the execution domain"), Artifact.Manifest.StateMigration.OwnerTypeId,
                TEXT("execution_domain:") + Artifact.Manifest.ModuleId);
    }

    FAvidScriptRuntimeArtifact Select(EAvidScriptVmBackendKind Backend) const
    {
        FAvidScriptVmBackendSelection Selection;
        Selection.BackendKind = Backend;
        Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
            ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
        Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
        Selection.bAllowFallback = false;
        return FAvidScriptRuntimeArtifact::FromCanonicalWasm(Artifact.Manifest, Artifact.VmArtifact.CanonicalWasmBytes, Selection);
    }

    int32 Read(FAutomationTestBase& Test, const FAvidScriptWasmRuntimeInstance& Runtime, const TCHAR* Field) const
    {
        const FString Suffix = FString(TEXT(":type:global::ReloadFlow:")) + Field;
        const auto* Slot = Artifact.Manifest.StateMigration.Slots.FindByPredicate(
            [&](const FAvidScriptWasmStateSlot& Value) { return Value.StableId.EndsWith(Suffix); });
        int32 Result = MIN_int32;
        FString Error;
        if (!Slot || Slot->Size != sizeof(Result)) Test.AddError(TEXT("No exact compiler state slot for ") + Suffix);
        else if (!Runtime.ReadStateBytes(Slot->Offset, MakeArrayView(reinterpret_cast<uint8*>(&Result), sizeof(Result)), Error)) Test.AddError(Error);
        return Result;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedNaturalReloadTest,
    "AvidScript.GeneratedTypes.NaturalLanguageRollback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedNaturalReloadTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::GeneratedNaturalReload;
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
    const FString CandidateRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_GENERATED_NATURAL_RELOAD_ROOT"));
    if (!GEngine || !Plugin || !TestTrue(TEXT("Formal candidate directory exists"), FPaths::DirectoryExists(CandidateRoot))) return false;
    auto& Host = FAvidScriptGeneratedTypeRuntimeHost::Get();
    if (!TestTrue(TEXT("Normal module startup installed the actual UHT package"), Host.HasInstalledPackage())) return false;
    TestEqual(TEXT("Real Actor class"), AReloadFlowActor::StaticClass()->GetPathName(), FString(TEXT("/Script/AvidScriptGenerated.ReloadFlowActor")));
    TestEqual(TEXT("Real Component class"), UReloadFlowComponent::StaticClass()->GetPathName(), FString(TEXT("/Script/AvidScriptGenerated.ReloadFlowComponent")));
    FPackage Initial, Next, Reject[3], AsyncFailure;
    const FString InitialPath = Plugin->GetBaseDir() / TEXT("Source/AvidScriptGenerated/AvidScriptGeneratedPackage.json");
    if (!Initial.Load(*this, Host, InitialPath)
        || !Next.Load(*this, Host, CandidateRoot / TEXT("Next/AvidScriptGeneratedPackage.json"))
        || !AsyncFailure.Load(*this, Host, CandidateRoot / TEXT("AsyncFailure/AvidScriptGeneratedPackage.json"))) return false;
    for (int32 Index = 0; Index < 3; ++Index)
        if (!Reject[Index].Load(*this, Host, CandidateRoot / FString::Printf(TEXT("Reject%d/AvidScriptGeneratedPackage.json"), Index + 1))) return false;

    int32 Cases = 0;
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (int32 Mode = 0; Mode < 9; ++Mode)
    for (int32 Point = 0; Point < 2; ++Point)
    {
        const FString Label = FString::Printf(TEXT("backend=%d mode=%d point=%d"), static_cast<int32>(Backend), Mode, Point);
        AddInfo(TEXT("natural generated reload start ") + Label);
        FString Error;
        if (!Host.InstallPackage(Initial.Types, Initial.Select(Backend), Error)) { AddError(Error); return false; }
        UWorld* World = CreateWorld();
        UWorld* OtherWorld = CreateWorld();
        ON_SCOPE_EXIT { DestroyWorld(World); DestroyWorld(OtherWorld); };
        if (!World || !OtherWorld) return false;
        TStrongObjectPtr<AReloadFlowActor> A(World->SpawnActor<AReloadFlowActor>());
        TStrongObjectPtr<AReloadFlowActor> B(World->SpawnActor<AReloadFlowActor>());
        TStrongObjectPtr<AReloadFlowActor> Other(OtherWorld->SpawnActor<AReloadFlowActor>());
        if (!A || !B || !Other) return false;
        TStrongObjectPtr<UReloadFlowComponent> Component(NewObject<UReloadFlowComponent>(A.Get()));
        A->AddInstanceComponent(Component.Get());
        Component->RegisterComponent();
        A->Value = 10; B->Value = 20; Component->Value = 30; Other->Value = 40;
        for (auto* CurrentWorld : {World, OtherWorld})
        {
            CurrentWorld->InitializeActorsForPlay(FURL());
            CurrentWorld->BeginPlay();
            CurrentWorld->SetBegunPlay(true);
        }
        for (auto* Actor : {A.Get(), B.Get(), Other.Get()}) if (!Actor->HasActorBegunPlay()) Actor->DispatchBeginPlay();
        TArray<FOwner> Owners{{A.Get(), nullptr, 10}, {B.Get(), nullptr, 20}, {Component.Get(), nullptr, 30}, {Other.Get(), nullptr, 40}};
        for (auto& Owner : Owners)
        {
            Owner.Session = Host.GetInstanceSessionForTesting(*Owner.Object);
            if (!TestNotNull(*Label, Owner.Session)) return false;
            TestEqual(TEXT("Actual BeginPlay writes and suspends"), Owner.Property(TEXT("Stage")), 1);
            TestEqual(TEXT("No initial result published"), Owner.Property(TEXT("Value")), Owner.Seed);
            TestEqual(TEXT("Original ReloadFlow initialization owns exactly one domain slot"), Owner.Session->GetTestSnapshot().ManagedStaticRoots, uint32(1));
        }
        auto SharedLease = Owners[0].Session->GetRuntimeLeaseForTesting();
        auto OtherLease = Owners[3].Session->GetRuntimeLeaseForTesting();
        auto* Original = Owners[0].Session->GetLiveRuntimeForTesting();
        TestTrue(TEXT("Two Actors and a Component share the production domain"), Original == Owners[1].Session->GetLiveRuntimeForTesting()
            && Original == Owners[2].Session->GetLiveRuntimeForTesting());
        TestTrue(TEXT("Another World has a separate production domain"), Original != Owners[3].Session->GetLiveRuntimeForTesting());
        TestEqual(TEXT("Actor reflected read observes shared state"), A->GetBeginCount(), 3);
        TestEqual(TEXT("Component reflected read observes the same shared state"), Component->GetBeginCount(), 3);
        TestEqual(TEXT("Other World initializes its own state"), Other->GetBeginCount(), 1);
        auto Tick = [&]()
        {
            if (World) World->Tick(LEVELTICK_All, 0.01f);
            if (OtherWorld) OtherWorld->Tick(LEVELTICK_All, 0.01f);
            ++GFrameCounter;
        };
        if (Point == 1)
        {
            for (int32 Frame = 0; Frame < 8 && A->GetCleanedCount() != 3; ++Frame) Tick();
            TestEqual(TEXT("First Task result has resumed; final timer remains pending"), A->GetCleanedCount(), 3);
            TestEqual(TEXT("No final timer result yet"), A->GetCompletedCount(), 0);
        }
        for (const auto& Owner : Owners)
        {
            const auto Snapshot = Owner.Session->GetTestSnapshot();
            TestTrue(TEXT("Real suspended frames and work"), Snapshot.ContinuationStateBytes > 0 && Snapshot.Runtime.PendingContinuationCount > 0);
        }
        if (HasAnyErrors()) return false;

        if (Mode >= 1 && Mode <= 4 || Mode == 8)
        {
            const FPackage& Candidate = Mode == 8 ? AsyncFailure : Mode == 4 ? Next : Reject[Mode - 1];
            TArray<FAvidScriptRuntimeSessionTestSnapshot> Before;
            TArray<TWeakPtr<FAvidScriptWasmRuntimeInstance>> CandidateLeases;
            TMap<UWorld*, TWeakPtr<FAvidScriptWasmRuntimeInstance>> CandidateWorlds;
            int32 Succeeded = 0, Failed = 0;
            for (const auto& Owner : Owners)
            {
                Before.Add(Owner.Session->GetTestSnapshot());
                Owner.Session->SetCandidateBeginPlayCompletionObserverForTesting([&, Object = Owner.Object, Session = Owner.Session, Seed = Owner.Seed]
                    (TWeakPtr<FAvidScriptWasmRuntimeInstance> Lease, bool Began)
                {
                    CandidateLeases.Add(Lease);
                    auto Runtime = Lease.Pin();
                    TestTrue(TEXT("Observer sees an actual candidate VM"), Runtime.IsValid());
                    if (!Runtime) return;
                    TestTrue(TEXT("Candidate has its own VM"), Runtime.Get() != Session->GetLiveRuntimeForTesting());
                    if (const auto* Peer = CandidateWorlds.Find(Object->GetWorld()))
                        TestTrue(TEXT("Every candidate in a World shares one VM"), Peer->Pin().Get() == Runtime.Get());
                    else CandidateWorlds.Add(Object->GetWorld(), Lease);
                    const FOwner Actual{Object, Session, Seed};
                    TestEqual(TEXT("C# wrote the real UProperty before natural failure"), Actual.Property(TEXT("Value")), Seed + 16);
                    TestTrue(TEXT("C# created a real candidate continuation before natural failure"), Session->GetPreparedContinuationCountForTesting() > 0);
                    if (Began) ++Succeeded; else ++Failed;
                });
            }
            const int32 PriorCleaned = Point ? 3 : 0;
            FAvidScriptGeneratedTypePackageReloadResult Reload;
            const bool Applied = Host.ReloadPackage(Candidate.Types, Candidate.Select(Backend), Reload, Error);
            for (auto& Owner : Owners) Owner.Session->SetCandidateBeginPlayCompletionObserverForTesting({});
            const bool ShouldPublish = Mode == 4 || Mode == 8;
            if (!TestEqual(*Label, Applied, ShouldPublish)) { AddError(Error); return false; }
            TestEqual(TEXT("Prepared count comes from observed C# execution"), Reload.PreparedInstanceCount, Succeeded);
            if (!ShouldPublish)
            {
                TestEqual(TEXT("Exactly one ordinary C# BeginPlay throws"), Failed, 1);
                TestTrue(TEXT("Natural failure retains original source and exception"), Error.Contains(TEXT("InvalidOperationException"))
                    && Error.Contains(TEXT("Fixtures/Phase66/GeneratedNaturalReload.cs")));
                TestTrue(TEXT("Whole package preserved after rollback"), Reload.bRollbackPreservedLivePackage);
                TestEqual(TEXT("Every prepared peer rolled back"), Reload.RolledBackInstanceCount, Succeeded);
                TestEqual(TEXT("No partial publication"), Reload.ReloadedInstanceCount, 0);
                for (int32 Index = 0; Index < Owners.Num(); ++Index)
                {
                    const auto& Owner = Owners[Index];
                    const auto After = Owner.Session->GetTestSnapshot();
                    const auto& Prior = Before[Index];
                    TestTrue(TEXT("Exact old VM and instance preserved"), After.LiveRuntimeIdentity == Prior.LiveRuntimeIdentity
                        && After.HostContext.InstanceExecutionState == Prior.HostContext.InstanceExecutionState);
                    TestEqual(TEXT("Exact code generation preserved"), After.LiveManifest.WasmSha256, Prior.LiveManifest.WasmSha256);
                    TestEqual(TEXT("Exact lifecycle generation preserved"), After.Runtime.ApplicationLifecycleGeneration, Prior.Runtime.ApplicationLifecycleGeneration);
                    TestEqual(TEXT("Exact property restored"), Owner.Property(TEXT("Value")), Owner.Seed);
                    TestEqual(TEXT("Exact Stage restored"), Owner.Property(TEXT("Stage")), 1);
                    TestEqual(TEXT("Tasks preserved"), After.TaskCount, Prior.TaskCount);
                    TestEqual(TEXT("Waiters preserved"), After.TaskWaiterCount, Prior.TaskWaiterCount);
                    TestEqual(TEXT("Frames preserved"), After.ContinuationStateBytes, Prior.ContinuationStateBytes);
                    TestEqual(TEXT("Pending continuations preserved"), After.Runtime.PendingContinuationCount, Prior.Runtime.PendingContinuationCount);
                    TestEqual(TEXT("Ready queue preserved"), After.ReadyContinuationCount, Prior.ReadyContinuationCount);
                    TestEqual(TEXT("Roots preserved"), After.ManagedLiveRoots, Prior.ManagedLiveRoots);
                    TestEqual(TEXT("Domain slots preserved"), After.ManagedStaticRoots, Prior.ManagedStaticRoots);
                    TestEqual(TEXT("Managed frame scopes preserved"), After.ManagedActiveFrames, Prior.ManagedActiveFrames);
                }
                for (const auto& Lease : CandidateLeases) TestFalse(TEXT("Failed candidates release their VM, heap and captured work"), Lease.IsValid());
                TestEqual(TEXT("Original shared static count restored"), A->GetBeginCount(), 3);
                TestEqual(TEXT("Original cleanup count preserved"), A->GetCleanedCount(), PriorCleaned);
                TestEqual(TEXT("Original other World static state preserved"), Other->GetBeginCount(), 1);
            }
            else
            {
                TestEqual(TEXT("Publish all four real instances"), Reload.ReloadedInstanceCount, 4);
                TestEqual(TEXT("No BeginPlay failed during preparation"), Failed, 0);
                TestFalse(TEXT("Old shared VM and work retired"), SharedLease.IsValid());
                TestFalse(TEXT("Old other World VM and work retired"), OtherLease.IsValid());
                TestEqual(TEXT("Shared state migrated once then three BeginPlay calls"), A->GetBeginCount(), 6);
                TestEqual(TEXT("Other World state migrated once"), Other->GetBeginCount(), 2);
                TestEqual(TEXT("Migrated first-generation cleanup retained"), A->GetCleanedCount(), PriorCleaned);
                TestTrue(TEXT("Published Actors and Component still share one VM"), Owners[0].Session->GetLiveRuntimeForTesting()
                    == Owners[1].Session->GetLiveRuntimeForTesting() && Owners[0].Session->GetLiveRuntimeForTesting() == Owners[2].Session->GetLiveRuntimeForTesting());
            }
        }
        else if (Mode == 5)
        {
            TestTrue(TEXT("Destroy only the Actor without the Component"), B->Destroy());
            TestFalse(TEXT("Destroyed owner retired"), Host.IsInstanceActive(*B));
            Owners[1].Session = nullptr;
            TestTrue(TEXT("Other owners keep the shared VM"), SharedLease.IsValid());
        }
        else if (Mode == 6)
        {
            TWeakObjectPtr<UReloadFlowComponent> Weak(Component.Get());
            Component->DestroyComponent();
            Component.Reset();
            Owners[2].Object = nullptr; Owners[2].Session = nullptr;
            CollectGarbage(RF_NoFlags);
            FTSTicker::GetCoreTicker().Tick(0.0f);
            TestFalse(TEXT("Suspended work cannot retain the destroyed Component"), Weak.IsValid());
            TestTrue(TEXT("Actors survive Component teardown and GC"), SharedLease.IsValid());
            TestTrue(TEXT("Managed heap can collect while peer Tasks remain suspended"), Owners[0].Session->CollectManagedHeapForTesting());
        }
        else if (Mode == 7)
        {
            DestroyWorld(World);
            for (int32 Index = 0; Index < 3; ++Index) Owners[Index].Session = nullptr;
            TestFalse(TEXT("World teardown retires its whole execution domain"), SharedLease.IsValid());
            TestTrue(TEXT("World teardown preserves the other World"), OtherLease.IsValid());
        }

        for (int32 Frame = 0; Frame < 80; ++Frame) Tick();
        const int32 Delta = Mode == 4 || Mode == 8 ? 32 : 0;
        for (int32 Index = 0; Index < Owners.Num(); ++Index)
        {
            const auto& Owner = Owners[Index];
            if (Mode == 8 && Index < 3)
            {
                const auto Snapshot = Owner.Session->GetTestSnapshot();
                TestTrue(TEXT("Published async void fault quarantines all shared owners"), Snapshot.Runtime.bFaultQuarantined);
                TestFalse(TEXT("Faulted shared domain releases VM"), Owner.Session->GetRuntimeLeaseForTesting().IsValid());
                TestTrue(TEXT("Fault is a natural source exception"), Snapshot.Runtime.FaultDiagnostic.Contains(TEXT("InvalidOperationException"))
                    && Snapshot.Runtime.FaultDiagnostic.Contains(TEXT("Fixtures/Phase66/GeneratedNaturalReload.cs")));
            }
            else if (Owner.Object)
                TestEqual(TEXT("Surviving old/new task completes; retired task never writes"), Owner.Property(TEXT("Value")),
                    Owner.Seed + (Owner.Session ? 7 + Delta : 0));
            if (Owner.Session)
            {
                const auto Snapshot = Owner.Session->GetTestSnapshot();
                TestEqual(TEXT("No Task leaks"), Snapshot.TaskCount, 0);
                TestEqual(TEXT("No waiter leaks"), Snapshot.TaskWaiterCount, 0);
                TestEqual(TEXT("No pending continuation leaks"), Snapshot.Runtime.PendingContinuationCount, 0);
                TestEqual(TEXT("No queued resume leaks"), Snapshot.ReadyContinuationCount, 0);
                TestEqual(TEXT("No frame leaks"), Snapshot.ContinuationStateBytes, 0);
                // The original IR declares one static initialization slot for
                // ReloadFlow. Its VM owns that slot even while it is empty.
                TestEqual(TEXT("Only the declared domain slot remains"), Snapshot.ManagedStaticRoots,
                    Mode == 8 && Index < 3 ? uint32(0) : uint32(1));
                TestEqual(TEXT("No Task, callback or language-error roots remain"), Snapshot.ManagedLiveRoots, Snapshot.ManagedStaticRoots);
                TestEqual(TEXT("No managed call frame leaks"), Snapshot.ManagedActiveFrames, uint32(0));
                if (Mode != 8 || Index == 3)
                {
                    TestFalse(TEXT("Surviving domain is not quarantined"), Snapshot.Runtime.bFaultQuarantined);
                    TestTrue(TEXT("Completed domain can collect"), Owner.Session->CollectManagedHeapForTesting());
                    TestEqual(TEXT("No retained managed objects"), Owner.Session->GetTestSnapshot().ManagedLiveObjects, uint32(0));
                }
            }
        }
        TestEqual(TEXT("Other World has independent completions"), Other->GetCompletedCount(), 1);
        if (Mode != 7 && Mode != 8) TestEqual(TEXT("Only surviving shared owners complete"), A->GetCompletedCount(), Mode == 5 || Mode == 6 ? 2 : 3);
        auto FinalOther = Owners[3].Session->GetRuntimeLeaseForTesting();
        DestroyWorld(World); DestroyWorld(OtherWorld);
        TestFalse(TEXT("Last owner releases final VM"), FinalOther.IsValid());
        TestEqual(TEXT("All generated owners retired"), Host.GetActiveInstanceCount(), 0);
        TestEqual(TEXT("All generated handles released"), Host.GetRegisteredHandleCount(), 0);
        if (HasAnyErrors()) return false;
        ++Cases;
        AddInfo(TEXT("natural generated reload passed ") + Label);
    }
    AddInfo(FString::Printf(TEXT("GeneratedNaturalLanguageRollback: %d/36 passed"), Cases));
    return TestEqual(TEXT("Whole real-type dual-backend matrix executed"), Cases, 36);
}

#endif
