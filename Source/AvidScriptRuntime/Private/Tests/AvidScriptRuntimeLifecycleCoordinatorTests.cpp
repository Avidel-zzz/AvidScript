#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptRuntimeSession.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmRuntime.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Lifecycle/AvidScriptRuntimeLifecycleCoordinator.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeApplicationLifecycleTest,
	"AvidScript.Runtime.Lifecycle.ApplicationPauseResume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeApplicationLifecycleTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptRuntimeLifecycleCoordinator& Coordinator =
		FAvidScriptRuntimeLifecycleCoordinator::Get();
	Coordinator.EnterForegroundForTesting();
	const FAvidScriptRuntimeLifecycleSnapshot Baseline =
		Coordinator.GetSnapshot();
	TestTrue(TEXT("Lifecycle coordinator is started"), Baseline.bStarted);

	FAvidScriptRuntimeSession Session;
	TestEqual(
		TEXT("Session registers with the lifecycle coordinator"),
		Coordinator.GetSnapshot().RegisteredSessionCount,
		Baseline.RegisteredSessionCount + 1);

	FAvidScriptWasmReloadResult LoadResult;
	if (!TestTrue(
		TEXT("Embedded Runtime loads before suspension"),
		Session.LoadEmbeddedSmoke(LoadResult)))
	{
		return false;
	}

	FAvidScriptWasmSmokeResult TickResult;
	if (!TestTrue(
		TEXT("Initial guest Tick succeeds"),
		Session.Tick(1.0f / 60.0f, TickResult)))
	{
		return false;
	}
	const int32 TickCountBeforeSuspend =
		Session.GetSnapshot().TickCallCount;

	Coordinator.EnterBackgroundForTesting();
	ON_SCOPE_EXIT
	{
		Coordinator.EnterForegroundForTesting();
	};
	const FAvidScriptRuntimeSessionSnapshot Suspended = Session.GetSnapshot();
	TestTrue(TEXT("Session records application suspension"), Suspended.bApplicationSuspended);
	TestTrue(
		TEXT("Application lifecycle generation advances"),
		Suspended.ApplicationLifecycleGeneration > Baseline.ApplicationGeneration);

	FAvidScriptWasmSmokeResult SuppressedTick;
	TestTrue(
		TEXT("Suspended Tick is an expected no-op"),
		Session.Tick(1.0f / 60.0f, SuppressedTick));
	const FAvidScriptRuntimeSessionSnapshot AfterSuppressedTick =
		Session.GetSnapshot();
	TestEqual(
		TEXT("Suspended Tick does not enter guest code"),
		AfterSuppressedTick.TickCallCount,
		TickCountBeforeSuspend);
	TestEqual(
		TEXT("Suppressed lifecycle entry is counted"),
		AfterSuppressedTick.SuppressedLifecycleEntryCount,
		Suspended.SuppressedLifecycleEntryCount + 1);

	FAvidScriptWasmReloadResult SuspendedLoad;
	TestFalse(
		TEXT("Suspended application rejects module replacement"),
		Session.LoadEmbeddedSmoke(SuspendedLoad));
	TestEqual(
		TEXT("Suspended module replacement reports a stable category"),
		SuspendedLoad.ErrorCategory,
		FString(TEXT("application_suspended")));

	Coordinator.HandleLowMemoryForTesting();
	TestEqual(
		TEXT("Live Session observes low-memory notification"),
		Session.GetSnapshot().LowMemoryNotificationCount,
		Suspended.LowMemoryNotificationCount + 1);

	Coordinator.EnterForegroundForTesting();
	const FAvidScriptRuntimeSessionSnapshot Resumed = Session.GetSnapshot();
	TestFalse(TEXT("Valid Session resumes"), Resumed.bApplicationSuspended);
	TestFalse(TEXT("Valid Session is not invalidated"), Resumed.bLifecycleInvalidated);
	TestTrue(TEXT("Valid Session remains loaded"), Resumed.bHasActiveRuntime);

	FAvidScriptWasmSmokeResult ResumedTick;
	TestTrue(
		TEXT("Resumed Session re-enters guest code"),
		Session.Tick(1.0f / 60.0f, ResumedTick));
	TestEqual(
		TEXT("Resumed Tick advances exactly once"),
		Session.GetSnapshot().TickCallCount,
		TickCountBeforeSuspend + 1);

	FAvidScriptWasmSmokeResult UnloadResult;
	TestTrue(TEXT("Session unload succeeds"), Session.StopAndUnload(UnloadResult));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeWorldTeardownLifecycleTest,
	"AvidScript.Runtime.Lifecycle.WorldTeardownGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeWorldTeardownLifecycleTest::RunTest(
	const FString& Parameters)
{
	if (!TestNotNull(TEXT("Engine is available"), GEngine))
	{
		return false;
	}
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptLifecycleTeardownWorld"));
	if (!TestNotNull(TEXT("Lifecycle test World is created"), World))
	{
		return false;
	}
	FWorldContext& WorldContext =
		GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};

	FAvidScriptRuntimeLifecycleCoordinator& Coordinator =
		FAvidScriptRuntimeLifecycleCoordinator::Get();
	Coordinator.EnterForegroundForTesting();
	const int32 InvalidatedBefore =
		Coordinator.GetSnapshot().InvalidatedWorldSessionCount;
	{
		FAvidScriptObjectRegistry Registry;
		FAvidScriptRuntimeSession Session;
		FAvidScriptWasmHostContext HostContext;
		HostContext.World = World;
		HostContext.ObjectRegistry = &Registry;
		Session.SetHostContext(HostContext);

		FAvidScriptWasmReloadResult LoadResult;
		if (!TestTrue(
			TEXT("World-bound Runtime loads"),
			Session.LoadEmbeddedSmoke(LoadResult)))
		{
			return false;
		}
		UObject* Dead = NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
		FAvidScriptObjectHandleResult BorrowResult;
		Session.GetTestSnapshot().HostContext.ObjectOwnership->Borrow(Registry, *Dead, BorrowResult);
		Dead = nullptr;
		bool bObservedGc = false;
		Session.SetLiveExecutionObserverForTesting([&]()
		{
			bObservedGc = true;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			TestEqual(TEXT("World Session defers GC while guest call is active"), Session.GetSnapshot().BorrowedHandleEntryCount, 1);
		});
		FAvidScriptWasmSmokeResult TickResult;
		TestTrue(TEXT("World Session executes the GC observer"), Session.TickHot(0.01f, TickResult));
		TestTrue(TEXT("World fixture reached actual GC"), bObservedGc);
		TestEqual(TEXT("Borrowed cleanup remains pending before World teardown"), Session.GetSnapshot().BorrowedHandleEntryCount, 1);
		FAvidScriptWasmRuntimeInstance* Runtime =
			Session.GetLiveRuntimeForTesting();
		if (!TestNotNull(TEXT("World-bound Runtime is accessible"), Runtime))
		{
			return false;
		}
		TestTrue(
			TEXT("World-bound continuation is scheduled"),
			Runtime->HandleContinuationDelayImport(30.0f, 901) > 0);
		TestEqual(
			TEXT("World-bound continuation is pending"),
			Session.GetLivePendingContinuationCount(),
			1);

		Coordinator.EnterBackgroundForTesting();
		TestTrue(
			TEXT("World-bound Session suspends"),
			Session.GetSnapshot().bApplicationSuspended);
		Coordinator.CleanupWorldForTesting(*World);
		const FAvidScriptRuntimeSessionSnapshot Invalidated =
			Session.GetSnapshot();
		TestTrue(
			TEXT("World teardown invalidates the Session generation"),
			Invalidated.bLifecycleInvalidated);
		TestFalse(
			TEXT("World teardown unloads without retaining guest runtime"),
			Invalidated.bHasActiveRuntime);
		TestEqual(
			TEXT("World teardown cancels pending continuations"),
			Invalidated.PendingContinuationCount,
			0);
		TestEqual(TEXT("World teardown clears deferred borrowed entries"), Invalidated.BorrowedHandleEntryCount, 0);
		TestEqual(TEXT("World teardown releases deferred borrowed leases"), Registry.GetLiveHandleCount(), 0);
		TestEqual(
			TEXT("World teardown is counted once"),
			Invalidated.LifecycleInvalidationCount,
			1);

		Coordinator.EnterForegroundForTesting();
		const FAvidScriptRuntimeSessionSnapshot AfterForeground =
			Session.GetSnapshot();
		TestFalse(
			TEXT("Foreground does not revive the old World Session"),
			AfterForeground.bHasActiveRuntime);
		TestTrue(
			TEXT("Invalidation remains explicit after foreground"),
			AfterForeground.bLifecycleInvalidated);
	}

	TestTrue(
		TEXT("Coordinator records the test Session and any World-owned peers"),
		Coordinator.GetSnapshot().InvalidatedWorldSessionCount
			>= InvalidatedBefore + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeOwnerGenerationLifecycleTest,
	"AvidScript.Runtime.Lifecycle.OwnerGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeOwnerGenerationLifecycleTest::RunTest(
	const FString& Parameters)
{
	if (!TestNotNull(TEXT("Engine is available"), GEngine))
	{
		return false;
	}
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptLifecycleOwnerWorld"));
	if (!TestNotNull(TEXT("Owner lifecycle World is created"), World))
	{
		return false;
	}
	FWorldContext& WorldContext =
		GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};

	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Lifecycle owner is spawned"), Owner))
	{
		return false;
	}
	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle OwnerHandle =
		Registry.RegisterObject(Owner, RegisterResult, true);
	if (!TestTrue(
		TEXT("Lifecycle owner receives a handle"),
		OwnerHandle.IsValid()))
	{
		return false;
	}

	FAvidScriptRuntimeLifecycleCoordinator& Coordinator =
		FAvidScriptRuntimeLifecycleCoordinator::Get();
	Coordinator.EnterForegroundForTesting();
	{
		FAvidScriptRuntimeSession Session;
		FAvidScriptWasmHostContext HostContext;
		HostContext.ObjectRegistry = &Registry;
		HostContext.OwnerHandle = OwnerHandle;
		HostContext.World = World;
		Session.SetHostContext(HostContext);
		FAvidScriptWasmReloadResult LoadResult;
		if (!TestTrue(
			TEXT("Owner-bound Runtime loads"),
			Session.LoadEmbeddedSmoke(LoadResult)))
		{
			return false;
		}

		Coordinator.EnterBackgroundForTesting();
		FAvidScriptObjectHandleResult ReleaseResult;
		TestTrue(
			TEXT("Owner generation is retired while suspended"),
			Registry.ReleaseHandle(OwnerHandle, ReleaseResult));
		Coordinator.EnterForegroundForTesting();

		const FAvidScriptRuntimeSessionSnapshot Invalidated =
			Session.GetSnapshot();
		TestTrue(
			TEXT("Stale owner generation invalidates resume"),
			Invalidated.bLifecycleInvalidated);
		TestFalse(
			TEXT("Stale owner generation cannot revive Runtime"),
			Invalidated.bHasActiveRuntime);
		TestEqual(
			TEXT("Owner generation invalidation is counted once"),
			Invalidated.LifecycleInvalidationCount,
			1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeGcBorrowedLeasesTest,
	"AvidScript.Runtime.Lifecycle.GarbageCollectBorrowedLeases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeGcBorrowedLeasesTest::RunTest(const FString& Parameters)
{
	FAvidScriptRuntimeLifecycleCoordinator& Coordinator = FAvidScriptRuntimeLifecycleCoordinator::Get();
	if (!TestTrue(TEXT("GC lifecycle hook is started"), Coordinator.GetSnapshot().bStarted))
	{
		return false;
	}
	FAvidScriptObjectRegistry Registry;
	FAvidScriptRuntimeSession First;
	FAvidScriptRuntimeSession Second;
	FAvidScriptWasmHostContext Context;
	Context.ObjectRegistry = &Registry;
	First.SetHostContext(Context);
	Second.SetHostContext(Context);
	IAvidScriptObjectOwnershipDomain* FirstOwnership = First.GetTestSnapshot().HostContext.ObjectOwnership;
	IAvidScriptObjectOwnershipDomain* SecondOwnership = Second.GetTestSnapshot().HostContext.ObjectOwnership;
	TStrongObjectPtr<UObject> Live(NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage()));
	UObject* Dead = NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	TWeakObjectPtr<UObject> WeakDead(Dead);
	FAvidScriptObjectHandleResult Result;
	FirstOwnership->Borrow(Registry, *Live.Get(), Result);
	const FAvidScriptObjectHandle LiveHandle = Result.Handle;
	SecondOwnership->Borrow(Registry, *Live.Get(), Result);
	FirstOwnership->Borrow(Registry, *Dead, Result);
	const FAvidScriptObjectHandle DeadHandle = Result.Handle;
	SecondOwnership->Borrow(Registry, *Dead, Result);
	TestTrue(TEXT("Dead object shares the same registry identity"), DeadHandle == Result.Handle);
	Registry.AcquireBorrowedObject(Dead, Result, false);
	Dead = nullptr;

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	TestFalse(TEXT("Borrowed leases do not retain the UObject"), WeakDead.IsValid());
	TestEqual(TEXT("GC complete prunes the first Session immediately"), First.GetSnapshot().BorrowedHandleEntryCount, 1);
	TestEqual(TEXT("GC complete prunes the second Session immediately"), Second.GetSnapshot().BorrowedHandleEntryCount, 1);
	TestFalse(TEXT("First dead capability is removed"), FirstOwnership->HasCapability(DeadHandle));
	TestFalse(TEXT("Second dead capability is removed"), SecondOwnership->HasCapability(DeadHandle));
	TestTrue(TEXT("Live capability survives in both Sessions"),
		FirstOwnership->HasCapability(LiveHandle, Live.Get()) && SecondOwnership->HasCapability(LiveHandle, Live.Get()));
	TestEqual(TEXT("An independent lease still occupies the dead slot"), Registry.GetLiveHandleCount(), 2);
	TestTrue(TEXT("Independent dead lease was not consumed by Session pruning"),
		Registry.ReleaseBorrowedHandle(DeadHandle, Result, false));
	TestEqual(TEXT("Dead slot retires only after its last lease"), Registry.GetLiveHandleCount(), 1);

	FAvidScriptWasmSmokeResult StopResult;
	TestTrue(TEXT("First Session stops"), First.StopAndUnload(StopResult));
	TestEqual(TEXT("Stopping first Session preserves second live lease"),
		Registry.ResolveObject(LiveHandle, Result, false), Live.Get());
	Second.ClearHostContext();
	TestEqual(TEXT("Host detach releases the last Session lease"), Registry.GetLiveHandleCount(), 0);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	TestEqual(TEXT("GC after stop remains empty"), First.GetSnapshot().BorrowedHandleEntryCount, 0);
	TestEqual(TEXT("GC after host detach remains empty"), Second.GetSnapshot().BorrowedHandleEntryCount, 0);
	return true;
}

#endif
