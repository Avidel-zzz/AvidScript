#pragma once

#include "AvidScriptBindingReloadEffect.h"
#include "AvidScriptObjectRegistry.h"
#include "CoreMinimal.h"

enum class EAvidScriptHostEffectTransactionState : uint8
{
	Open,
	Committed,
	RolledBack
};

struct FAvidScriptHostEffectTransactionResult
{
	bool bSucceeded = false;
	int32 CapturedObjectCount = 0;
	int32 RestoredObjectCount = 0;
	int32 FailedObjectCount = 0;
	FString ErrorCategory;
	FString ErrorSource;
	FString ErrorDetails;
};

class FAvidScriptHostEffectTransaction final : public IAvidScriptBindingHostEffectJournal
{
public:
	bool PrepareEffect(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& Handle,
		UObject& Target,
		EAvidScriptBindingReloadEffect Effect,
		FAvidScriptBindingHostEffectPrepareResult& OutResult) override;

	bool Commit(FAvidScriptHostEffectTransactionResult& OutResult);
	bool Rollback(
		FAvidScriptObjectRegistry& Registry,
		FAvidScriptHostEffectTransactionResult& OutResult);

	EAvidScriptHostEffectTransactionState GetState() const { return State; }
	int32 GetCapturedObjectCount() const { return Entries.Num(); }

private:
	struct FEntryKey
	{
		uint64 HandleValue = 0;
		EAvidScriptBindingReloadEffect Effect = EAvidScriptBindingReloadEffect::Unsupported;

		bool operator==(const FEntryKey&) const = default;

		friend uint32 GetTypeHash(const FEntryKey& Key)
		{
			return HashCombineFast(
				::GetTypeHash(Key.HandleValue),
				::GetTypeHash(static_cast<uint8>(Key.Effect)));
		}
	};

	struct FEntry
	{
		FAvidScriptObjectHandle Handle;
		TWeakObjectPtr<UObject> Object;
		EAvidScriptBindingReloadEffect Effect = EAvidScriptBindingReloadEffect::Unsupported;
		FTransform OriginalTransform = FTransform::Identity;
	};

	static FString FormatHandle(const FAvidScriptObjectHandle& Handle);
	static void SetPrepareFailure(
		FAvidScriptBindingHostEffectPrepareResult& OutResult,
		const FString& Category,
		const FString& Source,
		const FString& Details);
	static void SetClosedFailure(FAvidScriptHostEffectTransactionResult& OutResult);
	static void RecordRestoreFailure(
		FAvidScriptHostEffectTransactionResult& OutResult,
		const FEntry& Entry,
		const FString& Details);

	EAvidScriptHostEffectTransactionState State = EAvidScriptHostEffectTransactionState::Open;
	TSet<FEntryKey> CapturedKeys;
	TArray<FEntry> Entries;
};
