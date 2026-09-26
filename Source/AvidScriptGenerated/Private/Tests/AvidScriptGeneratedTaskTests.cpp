#if WITH_DEV_AUTOMATION_TESTS && AVIDSCRIPT_WITH_GENERATED_TYPES && AVIDSCRIPT_WITH_GENERATED_TASK_TESTS

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

namespace AvidScript::Tests::GeneratedTask
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptGeneratedTaskUhtLifecycleTest,
    "AvidScript.GeneratedTypes.TaskUhtLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedTaskUhtLifecycleTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::GeneratedTask;
    if (!GEngine) return false;
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
    const FString Candidate = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_GENERATED_TASK_CANDIDATE"));
    if (!Plugin || !TestTrue(TEXT("Candidate package exists"), FPaths::FileExists(Candidate))) return false;
    const FString Initial = Plugin->GetBaseDir() / TEXT("Source/AvidScriptGenerated/AvidScriptGeneratedPackage.json");
    auto& Host = FAvidScriptGeneratedTypeRuntimeHost::Get();
    if (!TestTrue(TEXT("Cold Editor module startup installed the formal package"), Host.HasInstalledPackage())) return false;
    TestEqual(TEXT("The real generated UHT class is installed"), ASharedTaskActor::StaticClass()->GetPathName(),
        FString(TEXT("/Script/AvidScriptGenerated.SharedTaskActor")));
    int32 Cases = 0;
    for (int32 Mode = 0; Mode < 6; ++Mode)
    for (bool Cancel : {false, true})
    {
        const FString Label = FString::Printf(TEXT("mode=%d cancel=%d"), Mode, Cancel);
        FString Error;
        // The first case uses module startup; subsequent cases restore the original
        // generation through the production package loader after retiring all owners.
        if (Cases && !Host.InstallPackageFromDescriptorFile(Initial, Error)) { AddError(Error); return false; }
        UWorld* World = CreateWorld();
        UWorld* OtherWorld = CreateWorld();
        ON_SCOPE_EXIT { DestroyWorld(World); DestroyWorld(OtherWorld); };
        if (!World || !OtherWorld) return false;
        TStrongObjectPtr<ASharedTaskActor> Owner(World->SpawnActor<ASharedTaskActor>());
        TStrongObjectPtr<ASharedTaskActor> Peer(World->SpawnActor<ASharedTaskActor>());
        TStrongObjectPtr<ASharedTaskActor> Other(OtherWorld->SpawnActor<ASharedTaskActor>());
        if (!Owner || !Peer || !Other) return false;
        Owner->Value = Cancel ? -1 : 5;
        Peer->Value = 10;
        Other->Value = 20;
        BeginWorld(*World);
        BeginWorld(*OtherWorld);
        // A test World has no GameMode to dispatch these actors automatically.
        for (ASharedTaskActor* Actor : {Owner.Get(), Peer.Get(), Other.Get()})
            if (!Actor->HasActorBegunPlay()) Actor->DispatchBeginPlay();
        auto* OwnerSession = Host.GetInstanceSessionForTesting(*Owner);
        auto* PeerSession = Host.GetInstanceSessionForTesting(*Peer);
        auto* OtherSession = Host.GetInstanceSessionForTesting(*Other);
        if (!TestNotNull(TEXT("Generated BeginPlay creates owner Session"), OwnerSession)
            || !TestNotNull(TEXT("Generated BeginPlay creates peer Session"), PeerSession)
            || !TestNotNull(TEXT("Other World creates a Session"), OtherSession)) return false;
        const auto OwnerLease = OwnerSession->GetRuntimeLeaseForTesting();
        const auto OtherLease = OtherSession->GetRuntimeLeaseForTesting();
        auto* OriginalRuntime = OwnerSession->GetLiveRuntimeForTesting();
        const auto OriginalState = OwnerSession->GetTestSnapshot().HostContext.InstanceExecutionState;
        TestTrue(TEXT("Same-World generated instances share execution"), OriginalRuntime == PeerSession->GetLiveRuntimeForTesting());
        TestTrue(TEXT("Other World has its own execution"), OriginalRuntime != OtherSession->GetLiveRuntimeForTesting());
        TestFalse(TEXT("Await works without an Actor Tick override"), Owner->PrimaryActorTick.bCanEverTick);
        auto Start = [&](ASharedTaskActor& Actor, bool Reflected) {
            const int32 Seed = Actor.Value;
            int32 Result = MIN_int32;
            if (Reflected)
            {
                struct FParameters { int32 ReturnValue = MIN_int32; } Args;
                Actor.ProcessEvent(Actor.FindFunctionChecked(GET_FUNCTION_NAME_CHECKED(ASharedTaskActor, GetScriptValue)), &Args);
                Result = Args.ReturnValue;
            }
            else Result = Actor.GetScriptValue();
            TestEqual(TEXT("Native/UHT entry returns the pre-await value"), Result, Seed);
        };
        Start(*Owner, true);
        Start(*Peer, false);
        Start(*Other, true);
        const int32 Pending = OwnerSession->GetLivePendingContinuationCount();
        TestTrue(TEXT("UHT call created suspended work"), Pending > 0);
        if (Mode == 1)
        {
            TestTrue(TEXT("Real Actor destruction runs generated EndPlay"), Owner->Destroy());
            OwnerSession = nullptr;
            TestFalse(TEXT("Destroyed owner has no active instance"), Host.IsInstanceActive(*Owner));
            TestTrue(TEXT("Peer retains shared VM after owner destruction"), OwnerLease.IsValid());
        }
        else if (Mode == 2)
        {
            DestroyWorld(World);
            OwnerSession = nullptr;
            PeerSession = nullptr;
            TestFalse(TEXT("World cleanup retires owner"), Host.IsInstanceActive(*Owner));
            TestFalse(TEXT("World cleanup retires peer"), Host.IsInstanceActive(*Peer));
            TestFalse(TEXT("World cleanup releases VM"), OwnerLease.IsValid());
            TestTrue(TEXT("Other World survives cleanup"), OtherLease.IsValid());
        }
        else if (Mode >= 3)
        {
            if (Mode < 5) Host.SetReloadFailureAfterInstanceCountForTesting(Mode - 2);
            FAvidScriptGeneratedTypePackageReloadResult Reload;
            const bool Applied = Host.ReloadPackageFromDescriptorFile(Candidate, Reload, Error);
            if (!TestEqual(*Label, Applied, Mode == 5)) { AddError(Error); return false; }
            if (Mode == 5)
            {
                TestEqual(TEXT("Package publishes all three owners"), Reload.ReloadedInstanceCount, 3);
                TestFalse(TEXT("Commit releases old shared VM"), OwnerLease.IsValid());
                TestFalse(TEXT("Commit releases old other-World VM"), OtherLease.IsValid());
                for (auto* Session : {OwnerSession, PeerSession, OtherSession})
                    TestEqual(TEXT("Commit cancels old pending work"), Session->GetLivePendingContinuationCount(), 0);
                Start(*Owner, true);
                Start(*Peer, false);
                Start(*Other, true);
            }
            else
            {
                TestEqual(TEXT("Requested partial preparation occurred"), Reload.PreparedInstanceCount, Mode - 2);
                TestEqual(TEXT("Prepared instances rolled back"), Reload.RolledBackInstanceCount, Mode - 2);
                TestEqual(TEXT("Rollback published no instance"), Reload.ReloadedInstanceCount, 0);
                TestTrue(TEXT("Rollback retains original package"), Reload.bRollbackPreservedLivePackage);
                TestTrue(TEXT("Rollback retains exact VM"), OriginalRuntime == OwnerSession->GetLiveRuntimeForTesting());
                TestTrue(TEXT("Rollback retains exact instance state"), OriginalState == OwnerSession->GetTestSnapshot().HostContext.InstanceExecutionState);
                TestEqual(TEXT("Rollback retains suspended work"), OwnerSession->GetLivePendingContinuationCount(), Pending);
            }
        }
        for (int32 Frame = 0; Frame < 24; ++Frame)
        {
            // Use normal UE World ticks. Calling Session::TickLive here would hide
            // missing production scheduling for generated actors without Tick.
            if (World) World->Tick(LEVELTICK_All, 0.02f);
            OtherWorld->Tick(LEVELTICK_All, 0.02f);
            ++GFrameCounter;
        }
        const int32 Delta = Mode == 5 ? 32 : 0;
        TestEqual(*Label, Other->Value, 41 + Delta);
        TestEqual(*Label, Peer->Value, Mode == 2 ? 10 : 21 + Delta);
        TestEqual(*Label, Owner->Value, (Mode == 1 || Mode == 2) ? (Cancel ? -1 : 5) : (Cancel ? 90 : 11 + Delta));
        for (auto* Session : {OwnerSession, PeerSession, OtherSession})
            if (Session)
            {
                TestEqual(TEXT("Completed instance releases its pending work"), Session->GetLivePendingContinuationCount(), 0);
                TestEqual(TEXT("Async scheduling does not synthesize script Tick calls"), Session->GetLiveTickCallCount(), 0);
            }
        DestroyWorld(World);
        DestroyWorld(OtherWorld);
        TestEqual(TEXT("All generated instances retire on World shutdown"), Host.GetActiveInstanceCount(), 0);
        if (HasAnyErrors()) return false;
        ++Cases;
        AddInfo(TEXT("generated Task UHT passed ") + Label);
    }
    AddInfo(FString::Printf(TEXT("GeneratedTaskUhtLifecycle: %d/12 passed"), Cases));
    return true;
}

#endif
