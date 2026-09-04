#pragma once

#include "CoreMinimal.h"

struct AVIDSCRIPTVM_API FAvidScriptVmMemorySnapshot
{
	// C++ backend object lifetimes, not the lifetime of every native runtime resource.
	uint64 BackendCreatedCount = 0;
	uint64 BackendDestroyedCount = 0;
	uint64 BackendLiveCount = 0;

	int32 ArtifactCacheEntries = 0;
	int32 ArtifactCacheCapacity = 0;
	int32 AttestationEntries = 0;
	int32 AttestationCapacity = 0;

	// UE-owned container allocations only, including owned arrays and strings.
	// Excludes Wasmtime heaps, code pages, linear memory and OS address reservations.
	uint64 ArtifactCacheAllocatedBytes = 0;
	uint64 AttestationAllocatedBytes = 0;
};

// Each owner is read under its own lock; the lifecycle triple is consistent.
// Does not copy artifacts, change LRU order, issue attestations or clear caches.
AVIDSCRIPTVM_API FAvidScriptVmMemorySnapshot CaptureAvidScriptVmMemorySnapshot();
