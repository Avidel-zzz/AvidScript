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
	bool PrepareReflectedProperty(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& Handle,
		UObject& Target,
		FProperty& Property,
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
		const FProperty* Property = nullptr;

		bool operator==(const FEntryKey&) const = default;

		friend uint32 GetTypeHash(const FEntryKey& Key)
		{
			return HashCombineFast(
				HashCombineFast(
					::GetTypeHash(Key.HandleValue),
					::GetTypeHash(static_cast<uint8>(Key.Effect))),
				PointerHash(Key.Property));
		}
	};

	struct FPropertySnapshot
	{
		FPropertySnapshot() = default;
		~FPropertySnapshot();
		FPropertySnapshot(const FPropertySnapshot&) = delete;
		FPropertySnapshot& operator=(const FPropertySnapshot&) = delete;
		FPropertySnapshot(FPropertySnapshot&& Other) noexcept;
		FPropertySnapshot& operator=(FPropertySnapshot&& Other) noexcept;

		bool Capture(FProperty& InProperty, UObject& Source);
		bool Restore(UObject& Target) const;
		bool IsValid() const { return Property != nullptr && Data != nullptr; }

	private:
		void Reset();

		FProperty* Property = nullptr;
		void* Data = nullptr;
	};

	struct FEntry
	{
		FAvidScriptObjectHandle Handle;
		TWeakObjectPtr<UObject> Object;
		EAvidScriptBindingReloadEffect Effect = EAvidScriptBindingReloadEffect::Unsupported;
		FTransform OriginalTransform = FTransform::Identity;
		FPropertySnapshot OriginalProperty;
	};

	static FString FormatHandle(const FAvidScriptObjectHandle& Handle);
	void SetPrepareFailure(
		FAvidScriptBindingHostEffectPrepareResult& OutResult,
		const FString& Category,
		const FString& Source,
		const FString& Details);
	void CopyFirstPrepareFailure(FAvidScriptHostEffectTransactionResult& OutResult) const;
	static void SetClosedFailure(FAvidScriptHostEffectTransactionResult& OutResult);
	static void RecordRestoreFailure(
		FAvidScriptHostEffectTransactionResult& OutResult,
		const FEntry& Entry,
		const FString& Details);

	EAvidScriptHostEffectTransactionState State = EAvidScriptHostEffectTransactionState::Open;
	TSet<FEntryKey> CapturedKeys;
	TArray<FEntry> Entries;
	FString FirstPrepareErrorCategory;
	FString FirstPrepareErrorSource;
	FString FirstPrepareErrorDetails;
};
