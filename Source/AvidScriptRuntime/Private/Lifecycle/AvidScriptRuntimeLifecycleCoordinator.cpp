#include "Lifecycle/AvidScriptRuntimeLifecycleCoordinator.h"

#include "AvidScriptRuntimeSession.h"
#include "AvidScriptVmArtifact.h"
#include "Engine/World.h"
#include "Misc/CoreDelegates.h"

FAvidScriptRuntimeLifecycleCoordinator&
FAvidScriptRuntimeLifecycleCoordinator::Get()
{
	static FAvidScriptRuntimeLifecycleCoordinator Instance;
	return Instance;
}

void FAvidScriptRuntimeLifecycleCoordinator::Startup()
{
	check(IsInGameThread());
	if (bStarted)
	{
		return;
	}

	EnterBackgroundHandle =
		FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddRaw(
			this,
			&FAvidScriptRuntimeLifecycleCoordinator::HandleEnterBackground);
	EnterForegroundHandle =
		FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddRaw(
			this,
			&FAvidScriptRuntimeLifecycleCoordinator::HandleEnterForeground);
	LowMemoryHandle =
		FCoreDelegates::ApplicationShouldUnloadResourcesDelegate.AddRaw(
			this,
			&FAvidScriptRuntimeLifecycleCoordinator::HandleLowMemory);
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(
		this,
		&FAvidScriptRuntimeLifecycleCoordinator::HandleWorldCleanup);
	bStarted = true;
}

void FAvidScriptRuntimeLifecycleCoordinator::Shutdown()
{
	check(IsInGameThread());
	if (!bStarted)
	{
		return;
	}

	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(
		EnterBackgroundHandle);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(
		EnterForegroundHandle);
	FCoreDelegates::ApplicationShouldUnloadResourcesDelegate.Remove(
		LowMemoryHandle);
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	Sessions.Reset();
	bStarted = false;
	bApplicationSuspended = false;
}

void FAvidScriptRuntimeLifecycleCoordinator::RegisterSession(
	FAvidScriptRuntimeSession& Session)
{
	check(IsInGameThread());
	Sessions.Add(&Session);
	if (bApplicationSuspended)
	{
		Session.SuspendForApplicationLifecycle(ApplicationGeneration);
	}
}

void FAvidScriptRuntimeLifecycleCoordinator::UnregisterSession(
	FAvidScriptRuntimeSession& Session)
{
	check(IsInGameThread());
	Sessions.Remove(&Session);
}

FAvidScriptRuntimeLifecycleSnapshot
FAvidScriptRuntimeLifecycleCoordinator::GetSnapshot() const
{
	check(IsInGameThread());
	FAvidScriptRuntimeLifecycleSnapshot Snapshot;
	Snapshot.bStarted = bStarted;
	Snapshot.bApplicationSuspended = bApplicationSuspended;
	Snapshot.ApplicationGeneration = ApplicationGeneration;
	Snapshot.RegisteredSessionCount = Sessions.Num();
	Snapshot.BackgroundCount = BackgroundCount;
	Snapshot.ForegroundCount = ForegroundCount;
	Snapshot.LowMemoryCount = LowMemoryCount;
	Snapshot.ReleasedArtifactCacheEntryCount =
		ReleasedArtifactCacheEntryCount;
	Snapshot.InvalidatedWorldSessionCount = InvalidatedWorldSessionCount;
	return Snapshot;
}

void FAvidScriptRuntimeLifecycleCoordinator::HandleEnterBackground()
{
	check(IsInGameThread());
	if (bApplicationSuspended)
	{
		return;
	}

	bApplicationSuspended = true;
	++ApplicationGeneration;
	if (ApplicationGeneration == 0)
	{
		ApplicationGeneration = 1;
	}
	++BackgroundCount;
	for (FAvidScriptRuntimeSession* Session : SnapshotSessions())
	{
		if (Session != nullptr && Sessions.Contains(Session))
		{
			Session->SuspendForApplicationLifecycle(ApplicationGeneration);
		}
	}
}

void FAvidScriptRuntimeLifecycleCoordinator::HandleEnterForeground()
{
	check(IsInGameThread());
	if (!bApplicationSuspended)
	{
		return;
	}

	bApplicationSuspended = false;
	++ForegroundCount;
	for (FAvidScriptRuntimeSession* Session : SnapshotSessions())
	{
		if (Session != nullptr && Sessions.Contains(Session))
		{
			Session->ResumeFromApplicationLifecycle(ApplicationGeneration);
		}
	}
}

void FAvidScriptRuntimeLifecycleCoordinator::HandleLowMemory()
{
	check(IsInGameThread());
	++LowMemoryCount;
	ReleasedArtifactCacheEntryCount +=
		ReleaseAvidScriptVmArtifactMemoryCache();
	for (FAvidScriptRuntimeSession* Session : SnapshotSessions())
	{
		if (Session != nullptr && Sessions.Contains(Session))
		{
			Session->HandleApplicationLowMemory();
		}
	}
}

void FAvidScriptRuntimeLifecycleCoordinator::HandleWorldCleanup(
	UWorld* World,
	bool bSessionEnded,
	bool bCleanupResources)
{
	check(IsInGameThread());
	if (World == nullptr || (!bSessionEnded && !bCleanupResources))
	{
		return;
	}
	for (FAvidScriptRuntimeSession* Session : SnapshotSessions())
	{
		if (Session != nullptr
			&& Sessions.Contains(Session)
			&& Session->InvalidateForWorldTeardown(*World))
		{
			++InvalidatedWorldSessionCount;
		}
	}
}

TArray<FAvidScriptRuntimeSession*>
FAvidScriptRuntimeLifecycleCoordinator::SnapshotSessions() const
{
	TArray<FAvidScriptRuntimeSession*> Snapshot;
	Snapshot.Reserve(Sessions.Num());
	for (FAvidScriptRuntimeSession* Session : Sessions)
	{
		Snapshot.Add(Session);
	}
	return Snapshot;
}

#if WITH_DEV_AUTOMATION_TESTS
void FAvidScriptRuntimeLifecycleCoordinator::EnterBackgroundForTesting()
{
	HandleEnterBackground();
}

void FAvidScriptRuntimeLifecycleCoordinator::EnterForegroundForTesting()
{
	HandleEnterForeground();
}

void FAvidScriptRuntimeLifecycleCoordinator::HandleLowMemoryForTesting()
{
	HandleLowMemory();
}

void FAvidScriptRuntimeLifecycleCoordinator::CleanupWorldForTesting(
	UWorld& World)
{
	HandleWorldCleanup(&World, true, true);
}
#endif
