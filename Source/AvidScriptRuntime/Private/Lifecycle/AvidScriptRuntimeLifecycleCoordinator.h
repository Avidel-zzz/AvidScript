#pragma once

#include "Containers/Set.h"
#include "Delegates/Delegate.h"

class FAvidScriptRuntimeSession;
class UWorld;

struct FAvidScriptRuntimeLifecycleSnapshot
{
	bool bStarted = false;
	bool bApplicationSuspended = false;
	uint64 ApplicationGeneration = 0;
	int32 RegisteredSessionCount = 0;
	int32 BackgroundCount = 0;
	int32 ForegroundCount = 0;
	int32 LowMemoryCount = 0;
	int32 ReleasedArtifactCacheEntryCount = 0;
	int32 InvalidatedWorldSessionCount = 0;
};

class FAvidScriptRuntimeLifecycleCoordinator final
{
public:
	static FAvidScriptRuntimeLifecycleCoordinator& Get();

	void Startup();
	void Shutdown();
	void RegisterSession(FAvidScriptRuntimeSession& Session);
	void UnregisterSession(FAvidScriptRuntimeSession& Session);
	FAvidScriptRuntimeLifecycleSnapshot GetSnapshot() const;

#if WITH_DEV_AUTOMATION_TESTS
	void EnterBackgroundForTesting();
	void EnterForegroundForTesting();
	void HandleLowMemoryForTesting();
	void CleanupWorldForTesting(UWorld& World);
#endif

private:
	void HandleEnterBackground();
	void HandleEnterForeground();
	void HandleLowMemory();
	void HandleGarbageCollectComplete();
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	TArray<FAvidScriptRuntimeSession*> SnapshotSessions() const;

	TSet<FAvidScriptRuntimeSession*> Sessions;
	FDelegateHandle EnterBackgroundHandle;
	FDelegateHandle EnterForegroundHandle;
	FDelegateHandle LowMemoryHandle;
	FDelegateHandle WorldCleanupHandle;
	FDelegateHandle GarbageCollectCompleteHandle;
	uint64 ApplicationGeneration = 0;
	int32 BackgroundCount = 0;
	int32 ForegroundCount = 0;
	int32 LowMemoryCount = 0;
	int32 ReleasedArtifactCacheEntryCount = 0;
	int32 InvalidatedWorldSessionCount = 0;
	bool bStarted = false;
	bool bApplicationSuspended = false;
};
