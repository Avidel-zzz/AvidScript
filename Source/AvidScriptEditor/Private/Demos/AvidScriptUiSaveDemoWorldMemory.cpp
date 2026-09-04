#include "Demos/AvidScriptUiSaveDemoWorld.h"

#include "AvidScriptVmDiagnostics.h"
#include "CoreGlobals.h"
#include "HAL/LowLevelMemTracker.h"
#include "HAL/PlatformMemory.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "Trace/Trace.h"
#include "UObject/NameTypes.h"
#include "UObject/UObjectArray.h"

namespace AvidScript::UiSaveDemo
{
namespace
{
TSharedRef<FJsonObject> CaptureWorldEngineMemory()
{
	const TSharedRef<FJsonObject> Engine = MakeShared<FJsonObject>();
	Engine->SetNumberField(TEXT("schema_version"), 1);
	Engine->SetNumberField(TEXT("sample_frame"), static_cast<double>(GFrameCounter));
	Engine->SetStringField(TEXT("sample_consistency"), TEXT("owner_snapshots_not_atomic"));
	const auto Allocator = FPlatformMemory::AllocatorToUse;
	Engine->SetStringField(TEXT("allocator_name"), Allocator == FPlatformMemory::Mimalloc
		? FString(TEXT("Mimalloc")) : FString::Printf(TEXT("platform_allocator_%d"), static_cast<int32>(Allocator)));

	const TSharedRef<FJsonObject> Trace = MakeShared<FJsonObject>();
	const TCHAR* TraceFields[] = { TEXT("memory_used_bytes"), TEXT("block_pool_bytes"), TEXT("fixed_buffer_bytes"),
		TEXT("shared_buffer_bytes"), TEXT("important_cache_allocated_bytes"), TEXT("important_cache_used_bytes"),
		TEXT("thread_registry_bytes") };
#if UE_TRACE_ENABLED
	UE::Trace::FStatistics Stats;
	UE::Trace::GetStatistics(Stats);
	const uint64 TraceValues[] = { Stats.MemoryUsed, Stats.BlockPoolAllocated, Stats.FixedBufferAllocated,
		Stats.SharedBufferAllocated, Stats.CacheAllocated, Stats.CacheUsed, Stats.ThreadRegistryAllocated };
	static_assert(UE_ARRAY_COUNT(TraceFields) == UE_ARRAY_COUNT(TraceValues));
	Trace->SetBoolField(TEXT("available"), true);
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(TraceFields); ++Index)
	{
		Trace->SetNumberField(TraceFields[Index], static_cast<double>(TraceValues[Index]));
	}
#else
	Trace->SetBoolField(TEXT("available"), false);
	for (const TCHAR* Field : TraceFields) { Trace->SetField(Field, MakeShared<FJsonValueNull>()); }
#endif
	Engine->SetObjectField(TEXT("trace"), Trace);

	const TSharedRef<FJsonObject> Names = MakeShared<FJsonObject>();
	Names->SetNumberField(TEXT("entry_bytes"), static_cast<double>(FName::GetNameEntryMemorySize()));
	Names->SetNumberField(TEXT("table_bytes"), static_cast<double>(FName::GetNameTableMemorySize()));
	Names->SetNumberField(TEXT("ansi_count"), FName::GetNumAnsiNames());
	Names->SetNumberField(TEXT("wide_count"), FName::GetNumWideNames());
	Engine->SetObjectField(TEXT("names"), Names);

	const TSharedRef<FJsonObject> Llm = MakeShared<FJsonObject>();
	const bool bLlmEnabled = LLM_IS_ENABLED();
	Llm->SetBoolField(TEXT("enabled"), bLlmEnabled);
	Llm->SetStringField(TEXT("sample_origin"), TEXT("live_totals_and_aggregated_tags"));
	const TCHAR* LlmFields[] = { TEXT("default_total_bytes"), TEXT("platform_total_bytes"), TEXT("platform_fmalloc_bytes"),
		TEXT("platform_overhead_bytes"), TEXT("default_fmalloc_unused_bytes"), TEXT("default_uobject_bytes"),
		TEXT("default_fname_bytes"), TEXT("default_untagged_bytes"), TEXT("default_engine_misc_bytes") };
#if ENABLE_LOW_LEVEL_MEM_TRACKER
	if (bLlmEnabled)
	{
		FLowLevelMemTracker& Tracker = FLowLevelMemTracker::Get();
		Llm->SetNumberField(LlmFields[0], static_cast<double>(Tracker.GetTotalTrackedMemory(ELLMTracker::Default)));
		Llm->SetNumberField(LlmFields[1], static_cast<double>(Tracker.GetTotalTrackedMemory(ELLMTracker::Platform)));
		const ELLMTag Tags[] = { ELLMTag::FMalloc,
			ELLMTag::PlatformOverhead, ELLMTag::FMallocUnused, ELLMTag::UObject,
			ELLMTag::FName, ELLMTag::Untagged, ELLMTag::EngineMisc };
		static_assert(UE_ARRAY_COUNT(LlmFields) == UE_ARRAY_COUNT(Tags) + 2);
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Tags); ++Index)
		{
			const ELLMTracker Owner = Index < 2 ? ELLMTracker::Platform : ELLMTracker::Default;
			Llm->SetNumberField(LlmFields[Index + 2], static_cast<double>(Tracker.GetTagAmountForTracker(Owner, Tags[Index])));
		}
	}
	else
#endif
	{
		for (const TCHAR* Field : LlmFields) { Llm->SetField(Field, MakeShared<FJsonValueNull>()); }
	}
	Engine->SetObjectField(TEXT("llm"), Llm);
	return Engine;
}
}

void FUiSaveWorld::AddMemorySample(FJsonObject& Gc)
{
	const FAvidScriptVmMemorySnapshot Vm = CaptureAvidScriptVmMemorySnapshot();
	const TSharedRef<FJsonObject> Attribution = MakeShared<FJsonObject>();
	Attribution->SetNumberField(TEXT("schema_version"), 1);
	Attribution->SetNumberField(TEXT("backend_created"), static_cast<double>(Vm.BackendCreatedCount));
	Attribution->SetNumberField(TEXT("backend_destroyed"), static_cast<double>(Vm.BackendDestroyedCount));
	Attribution->SetNumberField(TEXT("backend_live"), static_cast<double>(Vm.BackendLiveCount));
	Attribution->SetNumberField(TEXT("artifact_cache_entries"), Vm.ArtifactCacheEntries);
	Attribution->SetNumberField(TEXT("artifact_cache_capacity"), Vm.ArtifactCacheCapacity);
	Attribution->SetNumberField(TEXT("artifact_cache_allocated_bytes"), static_cast<double>(Vm.ArtifactCacheAllocatedBytes));
	Attribution->SetNumberField(TEXT("attestation_entries"), Vm.AttestationEntries);
	Attribution->SetNumberField(TEXT("attestation_capacity"), Vm.AttestationCapacity);
	Attribution->SetNumberField(TEXT("attestation_allocated_bytes"), static_cast<double>(Vm.AttestationAllocatedBytes));
	Attribution->SetObjectField(TEXT("engine_memory"), CaptureWorldEngineMemory());
	RetainedActionCount += Actions.Num();
	Attribution->SetNumberField(TEXT("observer_retained_cycles"), CompletedCycles);
	Attribution->SetNumberField(TEXT("observer_retained_actions"), static_cast<double>(RetainedActionCount));
	Attribution->SetStringField(TEXT("observer_estimate_kind"), TEXT("ue_json_memory_footprint"));
	Attribution->SetNumberField(TEXT("observer_json_estimated_bytes"), 0);
	Gc.SetObjectField(TEXT("attribution"), Attribution);
	for (const TCHAR* Field : { TEXT("physical_bytes"), TEXT("virtual_bytes"), TEXT("uobject_count") })
	{
		Gc.SetNumberField(Field, 0);
	}
	// Completed cycles are immutable after this boundary. Do not walk the growing history on every sample.
	RetainedCycleJsonBytes += Cycles.Last()->GetMemoryFootprint();
	Attribution->SetNumberField(TEXT("observer_json_estimated_bytes"),
		static_cast<double>(RetainedCycleJsonBytes + Cycles.GetAllocatedSize() + Actions.GetAllocatedSize()));
	const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
	Gc.SetNumberField(TEXT("physical_bytes"), static_cast<double>(Memory.UsedPhysical));
	Gc.SetNumberField(TEXT("virtual_bytes"), static_cast<double>(Memory.UsedVirtual));
	Gc.SetNumberField(TEXT("uobject_count"), GUObjectArray.GetObjectArrayNumMinusAvailable());
	PeakPhysical = FMath::Max(PeakPhysical, static_cast<uint64>(Memory.UsedPhysical));
	PeakVirtual = FMath::Max(PeakVirtual, static_cast<uint64>(Memory.UsedVirtual));
	if (CompletedCycles == 3)
	{
		MemorySummary->SetBoolField(TEXT("baseline_available"), true);
		MemorySummary->SetNumberField(TEXT("baseline_physical_bytes"), static_cast<double>(Memory.UsedPhysical));
		MemorySummary->SetNumberField(TEXT("baseline_virtual_bytes"), static_cast<double>(Memory.UsedVirtual));
	}
	MemorySummary->SetNumberField(TEXT("final_physical_bytes"), static_cast<double>(Memory.UsedPhysical));
	MemorySummary->SetNumberField(TEXT("final_virtual_bytes"), static_cast<double>(Memory.UsedVirtual));
	MemorySummary->SetNumberField(TEXT("peak_physical_bytes"), static_cast<double>(PeakPhysical));
	MemorySummary->SetNumberField(TEXT("peak_virtual_bytes"), static_cast<double>(PeakVirtual));
	TRACE_BOOKMARK(TEXT("AvidScript.WorldGC.%d"), CompletedCycles);
}
}
