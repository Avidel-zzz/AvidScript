#include "StateMigration/AvidScriptRuntimeStateMigration.h"

namespace
{
struct FAvidScriptPendingStateWrite
{
	FString StableId;
	uint32 CandidateOffset = 0;
	TArray<uint8> Bytes;
};

bool FailMigration(
	FAvidScriptRuntimeStateMigrationResult& OutResult,
	const FString& StableId,
	const FString& Category,
	const FString& Details)
{
	OutResult.bSucceeded = false;
	OutResult.StableId = StableId;
	OutResult.ErrorCategory = Category;
	OutResult.ErrorDetails = Details;
	return false;
}
} // namespace

bool FAvidScriptRuntimeStateMigration::Migrate(
	FAvidScriptWasmRuntimeInstance& PreviousRuntime,
	const FAvidScriptWasmReloadManifest& PreviousManifest,
	FAvidScriptWasmRuntimeInstance& CandidateRuntime,
	const FAvidScriptWasmReloadManifest& CandidateManifest,
	FAvidScriptRuntimeStateMigrationResult& OutResult)
{
	OutResult = FAvidScriptRuntimeStateMigrationResult();
	if (!PreviousManifest.StateMigration.IsEnabled()
		|| !CandidateManifest.StateMigration.IsEnabled()
		|| PreviousManifest.ModuleId != CandidateManifest.ModuleId
		|| PreviousManifest.StateMigration.OwnerTypeId != CandidateManifest.StateMigration.OwnerTypeId)
	{
		OutResult.bSucceeded = true;
		return true;
	}

	OutResult.bAttempted = true;
	TMap<FString, const FAvidScriptWasmStateSlot*> CandidateSlots;
	for (const FAvidScriptWasmStateSlot& Slot : CandidateManifest.StateMigration.Slots)
	{
		CandidateSlots.Add(Slot.StableId, &Slot);
	}

	TArray<FAvidScriptPendingStateWrite> PendingWrites;
	PendingWrites.Reserve(FMath::Min(
		PreviousManifest.StateMigration.Slots.Num(),
		CandidateManifest.StateMigration.Slots.Num()));
	TSet<FString> MatchedStableIds;
	for (const FAvidScriptWasmStateSlot& PreviousSlot : PreviousManifest.StateMigration.Slots)
	{
		const FAvidScriptWasmStateSlot* const* CandidateSlotPtr = CandidateSlots.Find(PreviousSlot.StableId);
		if (CandidateSlotPtr == nullptr)
		{
			++OutResult.SkippedSlotCount;
			continue;
		}

		const FAvidScriptWasmStateSlot& CandidateSlot = **CandidateSlotPtr;
		MatchedStableIds.Add(PreviousSlot.StableId);
		if (PreviousSlot.TypeFingerprint != CandidateSlot.TypeFingerprint
			|| PreviousSlot.Size != CandidateSlot.Size)
		{
			return FailMigration(
				OutResult,
				PreviousSlot.StableId,
				TEXT("state_migration_incompatible"),
				TEXT("stable state slot changed type fingerprint or byte size"));
		}

		FAvidScriptPendingStateWrite& PendingWrite = PendingWrites.AddDefaulted_GetRef();
		PendingWrite.StableId = PreviousSlot.StableId;
		PendingWrite.CandidateOffset = CandidateSlot.Offset;
		PendingWrite.Bytes.SetNumUninitialized(static_cast<int32>(PreviousSlot.Size));
		FString ReadError;
		if (!PreviousRuntime.ReadStateBytes(PreviousSlot.Offset, PendingWrite.Bytes, ReadError))
		{
			return FailMigration(
				OutResult,
				PreviousSlot.StableId,
				TEXT("state_migration_read_failed"),
				ReadError);
		}
	}

	OutResult.SkippedSlotCount += CandidateManifest.StateMigration.Slots.Num() - MatchedStableIds.Num();
	for (const FAvidScriptPendingStateWrite& PendingWrite : PendingWrites)
	{
		FString WriteError;
		if (!CandidateRuntime.WriteStateBytes(
			PendingWrite.CandidateOffset,
			PendingWrite.Bytes,
			WriteError))
		{
			return FailMigration(
				OutResult,
				PendingWrite.StableId,
				TEXT("state_migration_write_failed"),
				WriteError);
		}
		++OutResult.MigratedSlotCount;
		OutResult.MigratedByteCount += PendingWrite.Bytes.Num();
	}

	OutResult.bSucceeded = true;
	return true;
}
