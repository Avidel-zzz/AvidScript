#include "StateMigration/AvidScriptRuntimeStateMigration.h"

namespace
{
struct FAvidScriptPendingStateWrite
{
	FString StableId;
	uint32 CandidateOffset = 0;
	TArray<uint8> Bytes;
	TArray<uint8> CandidateOriginalBytes;
	bool bMatchedAlias = false;
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
	if (CandidateManifest.StateMigration.ContractVersion < PreviousManifest.StateMigration.ContractVersion)
	{
		return FailMigration(
			OutResult,
			TEXT("<contract_version>"),
			TEXT("state_migration_version_regression"),
			TEXT("candidate state contract version is older than the active runtime state contract"));
	}

	TMap<FString, const FAvidScriptWasmStateSlot*> CandidatePrimarySlots;
	TMap<FString, const FAvidScriptWasmStateSlot*> CandidateAliasSlots;
	for (const FAvidScriptWasmStateSlot& Slot : CandidateManifest.StateMigration.Slots)
	{
		CandidatePrimarySlots.Add(Slot.StableId, &Slot);
		for (const FString& Alias : Slot.Aliases)
		{
			CandidateAliasSlots.Add(Alias, &Slot);
		}
	}

	TArray<FAvidScriptPendingStateWrite> PendingWrites;
	PendingWrites.Reserve(FMath::Min(
		PreviousManifest.StateMigration.Slots.Num(),
		CandidateManifest.StateMigration.Slots.Num()));
	TSet<FString> MatchedCandidateStableIds;
	TSet<FString> ClaimedCandidateStableIds;
	for (const FAvidScriptWasmStateSlot& PreviousSlot : PreviousManifest.StateMigration.Slots)
	{
		const FAvidScriptWasmStateSlot* const* CandidateSlotPtr = CandidatePrimarySlots.Find(PreviousSlot.StableId);
		bool bMatchedAlias = false;
		if (CandidateSlotPtr == nullptr)
		{
			CandidateSlotPtr = CandidateAliasSlots.Find(PreviousSlot.StableId);
			bMatchedAlias = CandidateSlotPtr != nullptr;
		}
		if (CandidateSlotPtr == nullptr)
		{
			++OutResult.SkippedSlotCount;
			continue;
		}

		const FAvidScriptWasmStateSlot& CandidateSlot = **CandidateSlotPtr;
		if (ClaimedCandidateStableIds.Contains(CandidateSlot.StableId))
		{
			return FailMigration(
				OutResult,
				PreviousSlot.StableId,
				TEXT("state_migration_incompatible"),
				FString::Printf(
					TEXT("multiple previous state slots resolved to candidate stable_id=%s"),
					*CandidateSlot.StableId));
		}
		ClaimedCandidateStableIds.Add(CandidateSlot.StableId);
		MatchedCandidateStableIds.Add(CandidateSlot.StableId);
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
		PendingWrite.bMatchedAlias = bMatchedAlias;
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

	OutResult.SkippedSlotCount += CandidateManifest.StateMigration.Slots.Num() - MatchedCandidateStableIds.Num();
	for (FAvidScriptPendingStateWrite& PendingWrite : PendingWrites)
	{
		PendingWrite.CandidateOriginalBytes.SetNumUninitialized(PendingWrite.Bytes.Num());
		FString ReadError;
		if (!CandidateRuntime.ReadStateBytes(
			PendingWrite.CandidateOffset,
			PendingWrite.CandidateOriginalBytes,
			ReadError))
		{
			return FailMigration(
				OutResult,
				PendingWrite.StableId,
				TEXT("state_migration_read_failed"),
				ReadError);
		}
	}

	int32 AppliedWriteCount = 0;
	for (const FAvidScriptPendingStateWrite& PendingWrite : PendingWrites)
	{
		FString WriteError;
		if (!CandidateRuntime.WriteStateBytes(
			PendingWrite.CandidateOffset,
			PendingWrite.Bytes,
			WriteError))
		{
			FString FirstRestoreFailureStableId;
			FString FirstRestoreFailureDetails;
			for (int32 RollbackIndex = AppliedWriteCount - 1; RollbackIndex >= 0; --RollbackIndex)
			{
				const FAvidScriptPendingStateWrite& AppliedWrite = PendingWrites[RollbackIndex];
				FString RestoreError;
				if (!CandidateRuntime.WriteStateBytes(
					AppliedWrite.CandidateOffset,
					AppliedWrite.CandidateOriginalBytes,
					RestoreError))
				{
					if (FirstRestoreFailureStableId.IsEmpty())
					{
						FirstRestoreFailureStableId = AppliedWrite.StableId;
						FirstRestoreFailureDetails = RestoreError;
					}
				}
			}
			if (!FirstRestoreFailureStableId.IsEmpty())
			{
				return FailMigration(
					OutResult,
					FirstRestoreFailureStableId,
					TEXT("state_migration_write_failed"),
					FString::Printf(
						TEXT("forward write failed for stable_id=%s: %s; candidate restore failed for stable_id=%s: %s"),
						*PendingWrite.StableId,
						*WriteError,
						*FirstRestoreFailureStableId,
						*FirstRestoreFailureDetails));
			}
			return FailMigration(
				OutResult,
				PendingWrite.StableId,
				TEXT("state_migration_write_failed"),
				WriteError);
		}
		++AppliedWriteCount;
	}

	OutResult.MigratedSlotCount = PendingWrites.Num();
	for (const FAvidScriptPendingStateWrite& PendingWrite : PendingWrites)
	{
		OutResult.MigratedByteCount += PendingWrite.Bytes.Num();
		OutResult.AliasedSlotCount += PendingWrite.bMatchedAlias ? 1 : 0;
	}
	OutResult.bSucceeded = true;
	return true;
}
