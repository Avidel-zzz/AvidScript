#include "AvidScriptVmDiagnostics.h"

#include "AvidScriptVmBackend.h"
#include "AvidScriptVmDiagnosticsInternal.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace AvidScriptVmDiagnosticsInternal
{
struct FBackendLifecycleCounters
{
	FCriticalSection CriticalSection;
	uint64 CreatedCount = 0;
	uint64 DestroyedCount = 0;
};

static FBackendLifecycleCounters& GetBackendLifecycleCounters()
{
	static FBackendLifecycleCounters Counters;
	return Counters;
}
}

IAvidScriptVmBackend::IAvidScriptVmBackend()
{
	AvidScriptVmDiagnosticsInternal::FBackendLifecycleCounters& Counters =
		AvidScriptVmDiagnosticsInternal::GetBackendLifecycleCounters();
	FScopeLock Lock(&Counters.CriticalSection);
	++Counters.CreatedCount;
}

IAvidScriptVmBackend::~IAvidScriptVmBackend()
{
	AvidScriptVmDiagnosticsInternal::FBackendLifecycleCounters& Counters =
		AvidScriptVmDiagnosticsInternal::GetBackendLifecycleCounters();
	FScopeLock Lock(&Counters.CriticalSection);
	++Counters.DestroyedCount;
}

FAvidScriptVmMemorySnapshot CaptureAvidScriptVmMemorySnapshot()
{
	FAvidScriptVmMemorySnapshot Snapshot;
	{
		AvidScriptVmDiagnosticsInternal::FBackendLifecycleCounters& Counters =
			AvidScriptVmDiagnosticsInternal::GetBackendLifecycleCounters();
		FScopeLock Lock(&Counters.CriticalSection);
		Snapshot.BackendCreatedCount = Counters.CreatedCount;
		Snapshot.BackendDestroyedCount = Counters.DestroyedCount;
		Snapshot.BackendLiveCount = Counters.CreatedCount - Counters.DestroyedCount;
	}
	// Do not nest the lifecycle and compiler owner locks.
	AvidScriptVmDiagnosticsInternal::CaptureArtifactMemory(Snapshot);
	return Snapshot;
}
