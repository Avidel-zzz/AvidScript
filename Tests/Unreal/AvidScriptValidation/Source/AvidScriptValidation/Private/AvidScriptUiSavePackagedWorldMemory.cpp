#include "AvidScriptUiSavePackagedWorldProbe.h"

#include "AvidScriptHash.h"
#include "HAL/PlatformMemory.h"
#include "UObject/UObjectArray.h"

namespace AvidScript::Validation
{
namespace PackagedWorldMemoryPrivate
{
bool IsMilestone(int32 Cycle)
{
	static constexpr int32 Milestones[] = {
		1, 2, 3, 5, 10, 25, 50, 100, 200, 500, 1000, 2000, 5000, 10000
	};
	for (const int32 Milestone : Milestones)
	{
		if (Cycle == Milestone) { return true; }
	}
	return false;
}
}

uint64 FUiSavePackagedWorldProbe::EstimateObserverBytes() const
{
	uint64 Bytes = Samples.GetAllocatedSize() + CurrentActions.GetAllocatedSize()
		+ Steps.GetAllocatedSize() + EvidenceChain.GetAllocatedSize();
	for (const TSharedPtr<FJsonValue>& Sample : Samples)
	{
		if (Sample) { Bytes += Sample->GetMemoryFootprint(); }
	}
	for (const TSharedPtr<FJsonValue>& Action : CurrentActions)
	{
		if (Action) { Bytes += Action->GetMemoryFootprint(); }
	}
	return Bytes;
}

void FUiSavePackagedWorldProbe::AdvanceEvidenceChain(
	uint64 PhysicalBytes,
	uint64 VirtualBytes,
	int32 UObjectCount,
	const FAvidScriptVmMemorySnapshot& Vm)
{
	const FString Canonical = FString::Printf(
		TEXT("%s|%d|%s|%lld|%d|%llu|%llu|%d|%llu|%llu|%llu|%d|%llu|%d|%llu"),
		*EvidenceChain,
		CompletedCycles,
		*SaveHash,
		SaveBytes,
		CurrentActions.Num(),
		PhysicalBytes,
		VirtualBytes,
		UObjectCount,
		Vm.BackendCreatedCount,
		Vm.BackendDestroyedCount,
		Vm.BackendLiveCount,
		Vm.ArtifactCacheEntries,
		Vm.ArtifactCacheAllocatedBytes,
		Vm.AttestationEntries,
		Vm.AttestationAllocatedBytes);
	EvidenceChain = FAvidScriptHash::Sha256HexUtf8(Canonical);
}

bool FUiSavePackagedWorldProbe::CaptureMemorySample(bool bFinalCycle, FString& Error)
{
	if (CompletedCycles <= 0 || !IsHexIdentity(EvidenceChain, 64))
	{
		Error = TEXT("world_memory_sample_state_invalid"); return false;
	}
	const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
	const uint64 Physical = Memory.UsedPhysical;
	const uint64 Virtual = Memory.UsedVirtual;
	const int32 UObjectCount = GUObjectArray.GetObjectArrayNumMinusAvailable();
	int32 ActiveSessions = 0;
	if (!CountActiveSessions(ActiveSessions, Error)) { return false; }
	const FAvidScriptVmMemorySnapshot Vm = CaptureAvidScriptVmMemorySnapshot();
	if (ActiveSessions != 1 || Vm.BackendLiveCount == 0
		|| Vm.BackendCreatedCount < Vm.BackendDestroyedCount
		|| Vm.BackendCreatedCount - Vm.BackendDestroyedCount != Vm.BackendLiveCount
		|| Vm.ArtifactCacheEntries < 0 || Vm.ArtifactCacheEntries > Vm.ArtifactCacheCapacity
		|| Vm.AttestationEntries < 0 || Vm.AttestationEntries > Vm.AttestationCapacity)
	{
		Error = TEXT("world_memory_resource_invariant_failed"); return false;
	}
	if (CompletedCycles == 3)
	{
		bBaselineAvailable = true;
		BaselinePhysical = Physical;
		BaselineVirtual = Virtual;
		BaselineBackendLive = Vm.BackendLiveCount;
		BaselineArtifactEntries = Vm.ArtifactCacheEntries;
		BaselineAttestationEntries = Vm.AttestationEntries;
		BaselineArtifactBytes = Vm.ArtifactCacheAllocatedBytes;
		BaselineAttestationBytes = Vm.AttestationAllocatedBytes;
	}
	else if (CompletedCycles > 3
		&& (Vm.BackendLiveCount != BaselineBackendLive
			|| Vm.ArtifactCacheEntries > BaselineArtifactEntries
			|| Vm.AttestationEntries > BaselineAttestationEntries
			|| Vm.ArtifactCacheAllocatedBytes > BaselineArtifactBytes
			|| Vm.AttestationAllocatedBytes > BaselineAttestationBytes))
	{
		Error = TEXT("world_vm_memory_grew_after_warmup"); return false;
	}
	FinalPhysical = Physical;
	FinalVirtual = Virtual;
	PeakPhysical = FMath::Max(PeakPhysical, Physical);
	PeakVirtual = FMath::Max(PeakVirtual, Virtual);
	AdvanceEvidenceChain(Physical, Virtual, UObjectCount, Vm);

	if (!PackagedWorldMemoryPrivate::IsMilestone(CompletedCycles) && !bFinalCycle) { return true; }
	if (Samples.Num() >= MaximumSamples) { Error = TEXT("world_sample_capacity_exceeded"); return false; }
	const TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
	Sample->SetNumberField(TEXT("cycle"), CompletedCycles);
	Sample->SetBoolField(TEXT("final_cycle"), bFinalCycle);
	Sample->SetStringField(TEXT("save_sha256"), SaveHash);
	Sample->SetNumberField(TEXT("save_file_bytes"), static_cast<double>(SaveBytes));
	Sample->SetNumberField(TEXT("cycle_action_count"), CurrentActions.Num());
	Sample->SetNumberField(TEXT("physical_bytes"), static_cast<double>(Physical));
	Sample->SetNumberField(TEXT("virtual_bytes"), static_cast<double>(Virtual));
	Sample->SetNumberField(TEXT("uobject_count"), UObjectCount);
	Sample->SetNumberField(TEXT("active_sessions"), ActiveSessions);
	Sample->SetNumberField(TEXT("active_subscriptions"), Observation.Snapshot.ActiveDelegateSubscriptionCount);
	Sample->SetNumberField(TEXT("bound_buttons"), Observation.BoundButtons);
	Sample->SetNumberField(TEXT("owned_entries"), Observation.Snapshot.OwnedObjectEntryCount);
	Sample->SetNumberField(TEXT("borrowed_entries"), Observation.Snapshot.BorrowedHandleEntryCount);
	Sample->SetNumberField(TEXT("pending_timers"), Observation.Snapshot.PendingTimerCount);
	Sample->SetNumberField(TEXT("pending_continuations"), Observation.Snapshot.PendingContinuationCount);
	Sample->SetNumberField(TEXT("prepared_continuations"), Observation.Snapshot.PreparedContinuationCount);
	Sample->SetNumberField(TEXT("prepared_subscriptions"), Observation.Snapshot.PreparedDelegateSubscriptionCount);
	Sample->SetNumberField(TEXT("backend_created"), static_cast<double>(Vm.BackendCreatedCount));
	Sample->SetNumberField(TEXT("backend_destroyed"), static_cast<double>(Vm.BackendDestroyedCount));
	Sample->SetNumberField(TEXT("backend_live"), static_cast<double>(Vm.BackendLiveCount));
	Sample->SetNumberField(TEXT("artifact_cache_entries"), Vm.ArtifactCacheEntries);
	Sample->SetNumberField(TEXT("artifact_cache_capacity"), Vm.ArtifactCacheCapacity);
	Sample->SetNumberField(TEXT("artifact_cache_allocated_bytes"), static_cast<double>(Vm.ArtifactCacheAllocatedBytes));
	Sample->SetNumberField(TEXT("attestation_entries"), Vm.AttestationEntries);
	Sample->SetNumberField(TEXT("attestation_capacity"), Vm.AttestationCapacity);
	Sample->SetNumberField(TEXT("attestation_allocated_bytes"), static_cast<double>(Vm.AttestationAllocatedBytes));
	Sample->SetStringField(TEXT("evidence_chain_sha256"), EvidenceChain);
	Sample->SetNumberField(TEXT("observer_json_estimated_bytes"), 0);
	Samples.Add(MakeShared<FJsonValueObject>(Sample));
	Sample->SetNumberField(TEXT("observer_json_estimated_bytes"), static_cast<double>(EstimateObserverBytes()));
	return true;
}
}
