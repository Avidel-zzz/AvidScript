#if WITH_DEV_AUTOMATION_TESTS && AVIDSCRIPT_WITH_GENERATED_TYPES && AVIDSCRIPT_WITH_GENERATED_ASYNC_THROW_TESTS

#include "AvidScriptGeneratedTypes.h"
#include "AvidScriptRuntimeSession.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

namespace AvidScript::Tests::GeneratedAsyncThrow
{
UWorld* CreateWorld()
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    if (World) GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    return World;
}

void BeginWorld(UWorld& World)
{
    World.InitializeActorsForPlay(FURL());
    World.BeginPlay();
    World.SetBegunPlay(true);
}

void DestroyWorld(UWorld*& World)
{
    if (!World) return;
    if (World->HasBegunPlay()) World->EndPlay(EEndPlayReason::Quit);
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    World = nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedAsyncThrowLifecycleTest,
    "AvidScript.GeneratedTypes.AsyncThrowLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedAsyncThrowLifecycleTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::GeneratedAsyncThrow;
    if (!GEngine) return false;
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
    const FString Candidate = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_GENERATED_TASK_CANDIDATE"));
    if (!Plugin || !TestTrue(TEXT("Formal candidate package exists"), FPaths::FileExists(Candidate))) return false;
    const FString Initial = Plugin->GetBaseDir() / TEXT("Source/AvidScriptGenerated/AvidScriptGeneratedPackage.json");
    auto& Host = FAvidScriptGeneratedTypeRuntimeHost::Get();
    if (!TestTrue(TEXT("Cold startup installed the generated package"), Host.HasInstalledPackage())) return false;
    TestEqual(TEXT("Real UHT class"), AAsyncThrowActor::StaticClass()->GetPathName(),
        FString(TEXT("/Script/AvidScriptGenerated.AsyncThrowActor")));
    // Same-source .NET checks each encoded result AND the cleanup order.
    const int32 Expected[] = {700024, 8001234, 9000254, 9001254, 8001234, 8000234};
    int32 Cases = 0;
    for (int32 Scenario = 0; Scenario < UE_ARRAY_COUNT(Expected); ++Scenario)
    for (int32 Mode = 0; Mode < 6; ++Mode)
    for (int32 Point = 0; Point < 3; ++Point)
    {
        const FString Label = FString::Printf(TEXT("scenario=%d mode=%d point=%d"), Scenario, Mode, Point);
        FString Error;
        if (Cases && !Host.InstallPackageFromDescriptorFile(Initial, Error)) { AddError(Error); return false; }
        UWorld* World = CreateWorld();
        UWorld* OtherWorld = CreateWorld();
        ON_SCOPE_EXIT { DestroyWorld(World); DestroyWorld(OtherWorld); };
        if (!World || !OtherWorld) return false;
        TStrongObjectPtr<AAsyncThrowActor> Owner(World->SpawnActor<AAsyncThrowActor>());
        TStrongObjectPtr<AAsyncThrowActor> Peer(World->SpawnActor<AAsyncThrowActor>());
        TStrongObjectPtr<AAsyncThrowActor> Other(OtherWorld->SpawnActor<AAsyncThrowActor>());
        if (!Owner || !Peer || !Other) return false;
        const int32 PeerScenario = (Scenario + 1) % UE_ARRAY_COUNT(Expected);
        const int32 OtherScenario = (Scenario + 2) % UE_ARRAY_COUNT(Expected);
        Owner->Value = Scenario;
        Peer->Value = PeerScenario;
        Other->Value = OtherScenario;
        BeginWorld(*World);
        BeginWorld(*OtherWorld);
        for (AAsyncThrowActor* Actor : {Owner.Get(), Peer.Get(), Other.Get()})
            if (!Actor->HasActorBegunPlay()) Actor->DispatchBeginPlay();
        auto* OwnerSession = Host.GetInstanceSessionForTesting(*Owner);
        auto* PeerSession = Host.GetInstanceSessionForTesting(*Peer);
        auto* OtherSession = Host.GetInstanceSessionForTesting(*Other);
        if (!TestNotNull(*Label, OwnerSession) || !TestNotNull(*Label, PeerSession) || !TestNotNull(*Label, OtherSession)) return false;
        auto OwnerLease = OwnerSession->GetRuntimeLeaseForTesting();
        auto OtherLease = OtherSession->GetRuntimeLeaseForTesting();
        TestTrue(TEXT("Peers share VM"), OwnerSession->GetLiveRuntimeForTesting() == PeerSession->GetLiveRuntimeForTesting());
        TestTrue(TEXT("Worlds isolate VM"), OwnerSession->GetLiveRuntimeForTesting() != OtherSession->GetLiveRuntimeForTesting());
        TestFalse(TEXT("No synthesized Actor Tick override"), Owner->PrimaryActorTick.bCanEverTick);
        auto Start = [&](AAsyncThrowActor& Actor, bool Reflected) {
            const int32 Seed = Actor.Value;
            int32 Result = MIN_int32;
            if (Reflected)
            {
                struct FParameters { int32 ReturnValue = MIN_int32; } Args;
                Actor.ProcessEvent(Actor.FindFunctionChecked(GET_FUNCTION_NAME_CHECKED(AAsyncThrowActor, GetScriptValue)), &Args);
                Result = Args.ReturnValue;
            }
            else Result = Actor.GetScriptValue();
            TestEqual(TEXT("Entry returns before async result"), Result, Seed);
        };
        auto Tick = [&]() {
            if (World) World->Tick(LEVELTICK_All, 0.02f);
            if (OtherWorld) OtherWorld->Tick(LEVELTICK_All, 0.02f);
            ++GFrameCounter;
        };
        Start(*Owner, true);
        Start(*Peer, false);
        Start(*Other, true);
        TestTrue(TEXT("Real UFunction created Task ownership"), OwnerSession->GetTestSnapshot().TaskCount > 0);
        if (Point == 1)
        {
            Tick();
            TestEqual(TEXT("Parent has not published the result after first resume"), Owner->Stage, 0);
            if (Scenario == 1 || Scenario == 3 || Scenario == 4)
                TestTrue(TEXT("Cancellation errors are actually rooted before lifecycle mutation"),
                    OwnerSession->GetTestSnapshot().ManagedLiveRoots > 0);
        }
        else if (Point == 2)
        {
            for (int32 Frame = 0; Frame < 16 && (Owner->Stage != 1 || Peer->Stage != 1 || Other->Stage != 1); ++Frame) Tick();
            for (auto* Actor : {Owner.Get(), Peer.Get(), Other.Get()})
                TestEqual(TEXT("Exception replacement finished; final write still suspended"), Actor->Stage, 1);
        }
        TestEqual(TEXT("No early result write"), Owner->Value, Scenario);
        const int32 StoppedStage = Owner->Stage;
        for (auto* Session : {OwnerSession, PeerSession, OtherSession})
        {
            TestTrue(TEXT("Suspended work is real"), Session->GetLivePendingContinuationCount() > 0);
            TestTrue(TEXT("Suspended state is owned"), Session->GetTestSnapshot().ContinuationStateBytes > 0);
        }
        if (HasAnyErrors()) return false;
        if (Mode == 1)
        {
            const uint32 RootsBefore = OwnerSession->GetTestSnapshot().ManagedLiveRoots;
            TestTrue(TEXT("Actor::Destroy invokes generated EndPlay"), Owner->Destroy());
            OwnerSession = nullptr;
            TestFalse(TEXT("Destroyed owner retired"), Host.IsInstanceActive(*Owner));
            TestTrue(TEXT("Peer retains shared execution"), OwnerLease.IsValid());
            TestEqual(TEXT("Only owner retired"), Host.GetActiveInstanceCount(), 2);
            if (Point == 1 && (Scenario == 1 || Scenario == 3 || Scenario == 4))
            {
                const uint32 RootsAfter = PeerSession->GetTestSnapshot().ManagedLiveRoots;
                TestTrue(TEXT("Destroying owner releases its live exception roots"), RootsAfter < RootsBefore);
                if (PeerScenario == 4)
                    TestTrue(TEXT("Peer keeps its own cancellation error roots"), RootsAfter > 0);
            }
        }
        else if (Mode == 2)
        {
            DestroyWorld(World);
            OwnerSession = PeerSession = nullptr;
            TestFalse(TEXT("World shutdown releases its VM"), OwnerLease.IsValid());
            TestTrue(TEXT("Other World survives"), OtherLease.IsValid());
            TestEqual(TEXT("Other World is the sole live instance"), Host.GetActiveInstanceCount(), 1);
        }
        else if (Mode >= 3)
        {
            TArray<FAvidScriptRuntimeSessionTestSnapshot> Before;
            for (auto* Session : {OwnerSession, PeerSession, OtherSession}) Before.Add(Session->GetTestSnapshot());
            if (Mode < 5) Host.SetReloadFailureAfterInstanceCountForTesting(Mode - 2);
            FAvidScriptGeneratedTypePackageReloadResult Reload;
            const bool Applied = Host.ReloadPackageFromDescriptorFile(Candidate, Reload, Error);
            if (!TestEqual(*Label, Applied, Mode == 5)) { AddError(Error); return false; }
            if (Mode == 5)
            {
                TestEqual(TEXT("Commit publishes all instances"), Reload.ReloadedInstanceCount, 3);
                TestFalse(TEXT("Old shared VM retired"), OwnerLease.IsValid());
                TestFalse(TEXT("Old other-World VM retired"), OtherLease.IsValid());
                for (auto* Session : {OwnerSession, PeerSession, OtherSession})
                {
                    const auto Snapshot = Session->GetTestSnapshot();
                    TestEqual(TEXT("Old tasks cancelled by commit"), Snapshot.TaskCount, 0);
                    TestEqual(TEXT("Old waits cancelled by commit"), Snapshot.Runtime.PendingContinuationCount, 0);
                    TestEqual(TEXT("Old frames cancelled by commit"), Snapshot.ContinuationStateBytes, 0);
                }
                Start(*Owner, true);
                Start(*Peer, false);
                Start(*Other, true);
            }
            else
            {
                TestEqual(TEXT("Partial prepare reached requested count"), Reload.PreparedInstanceCount, Mode - 2);
                TestEqual(TEXT("Partial prepare rolled back"), Reload.RolledBackInstanceCount, Mode - 2);
                TestEqual(TEXT("No candidate published"), Reload.ReloadedInstanceCount, 0);
                TestTrue(TEXT("Old package preserved"), Reload.bRollbackPreservedLivePackage);
                int32 Index = 0;
                for (auto* Session : {OwnerSession, PeerSession, OtherSession})
                {
                    const auto After = Session->GetTestSnapshot();
                    const auto& Prior = Before[Index++];
                    TestTrue(TEXT("Exact VM preserved"), After.LiveRuntimeIdentity == Prior.LiveRuntimeIdentity);
                    TestTrue(TEXT("Exact instance state preserved"), After.HostContext.InstanceExecutionState == Prior.HostContext.InstanceExecutionState);
                    TestEqual(TEXT("Tasks preserved"), After.TaskCount, Prior.TaskCount);
                    TestEqual(TEXT("Waiters preserved"), After.TaskWaiterCount, Prior.TaskWaiterCount);
                    TestEqual(TEXT("Frames preserved"), After.ContinuationStateBytes, Prior.ContinuationStateBytes);
                    TestEqual(TEXT("Ready queue preserved"), After.ReadyContinuationCount, Prior.ReadyContinuationCount);
                    TestEqual(TEXT("Error roots preserved"), After.ManagedLiveRoots, Prior.ManagedLiveRoots);
                }
            }
        }
        for (int32 Frame = 0; Frame < 80; ++Frame) Tick();
        const int32 Offset = Mode == 5 ? 16 : 0;
        TestEqual(*Label, Other->Value, Expected[OtherScenario] + Offset);
        TestEqual(*Label, Peer->Value, Mode == 2 ? PeerScenario : Expected[PeerScenario] + Offset);
        TestEqual(*Label, Owner->Value, (Mode == 1 || Mode == 2) ? Scenario : Expected[Scenario] + Offset);
        TestEqual(TEXT("Retired continuation cannot write Stage"), Owner->Stage, (Mode == 1 || Mode == 2) ? StoppedStage : 2);
        for (auto* Session : {OwnerSession, PeerSession, OtherSession})
            if (Session)
            {
                const auto Snapshot = Session->GetTestSnapshot();
                TestFalse(*Label, Snapshot.Runtime.bFaultQuarantined);
                TestEqual(TEXT("No Task leaks"), Snapshot.TaskCount, 0);
                TestEqual(TEXT("No waiter leaks"), Snapshot.TaskWaiterCount, 0);
                TestEqual(TEXT("No continuation leaks"), Snapshot.Runtime.PendingContinuationCount, 0);
                TestEqual(TEXT("No queued resumption"), Snapshot.ReadyContinuationCount, 0);
                TestEqual(TEXT("No state-frame leaks"), Snapshot.ContinuationStateBytes, 0);
                TestEqual(TEXT("No language-error roots"), Snapshot.ManagedLiveRoots, uint32(0));
                TestEqual(TEXT("No synthetic Tick entry"), Snapshot.Runtime.TickCallCount, 0);
                TestTrue(TEXT("Unreachable exceptions collected"), Session->CollectManagedHeapForTesting());
                TestEqual(TEXT("No retained exception objects"), Session->GetTestSnapshot().ManagedLiveObjects, uint32(0));
            }
        auto FinalLease = OtherSession->GetRuntimeLeaseForTesting();
        DestroyWorld(World);
        DestroyWorld(OtherWorld);
        TestFalse(TEXT("Last World releases final runtime"), FinalLease.IsValid());
        TestEqual(TEXT("All generated instances retired"), Host.GetActiveInstanceCount(), 0);
        if (HasAnyErrors()) return false;
        ++Cases;
        AddInfo(TEXT("generated async throw passed ") + Label);
    }
    AddInfo(FString::Printf(TEXT("GeneratedAsyncThrowLifecycle: %d/108 passed"), Cases));
    return true;
}

#endif
