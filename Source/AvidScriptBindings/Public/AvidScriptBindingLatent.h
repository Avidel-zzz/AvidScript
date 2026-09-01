#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "UObject/StrongObjectPtr.h"

class UObject;
class UClass;
class FMulticastDelegateProperty;

enum class EAvidScriptBindingLatentPayloadKind : uint8
{
	AbiCells,
	FixedWire,
	Object,
	Utf8,
	Array,
	Composite
};

struct FAvidScriptBindingLatentCompletionField
{
	int32 WireOffset = INDEX_NONE;
	FString TypeId;
	EAvidScriptBindingLatentPayloadKind Kind =
		EAvidScriptBindingLatentPayloadKind::FixedWire;
	TArray<uint8> Bytes;
	TStrongObjectPtr<UObject> ObjectValue;
};

struct FAvidScriptBindingLatentCompletionPayload
{
	FString TypeId;
	EAvidScriptBindingLatentPayloadKind Kind =
		EAvidScriptBindingLatentPayloadKind::AbiCells;
	TArray<uint64> AbiCells;
	TArray<uint8> Bytes;
	TStrongObjectPtr<UObject> ObjectValue;
	FString ElementTypeId;
	int32 ElementCount = 0;
	int32 ElementStride = 0;
	int32 ElementAlignment = 1;
	TArray<FAvidScriptBindingLatentCompletionField> Fields;
};

class IAvidScriptBindingAsyncActionPayloadEncoder
{
public:
	virtual ~IAvidScriptBindingAsyncActionPayloadEncoder() = default;

	virtual bool Capture(
		void* Parameters,
		FAvidScriptBindingLatentCompletionPayload& OutPayload,
		FString& OutError) const = 0;
};

class IAvidScriptLatentCompletionProvider
{
public:
	virtual ~IAvidScriptLatentCompletionProvider() = default;

	virtual FString GetProviderId() const = 0;
	virtual FString GetFunctionPath() const = 0;
	virtual FString GetPayloadTypeId() const = 0;
	virtual bool ConsumePayload(
		UObject* CallbackTarget,
		int32 UUID,
		FAvidScriptBindingLatentCompletionPayload& OutPayload) = 0;
	virtual void AbandonPayload(UObject* CallbackTarget, int32 UUID) = 0;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptLatentCompletionProviderRegistry
{
public:
	static bool Register(
		const TSharedRef<IAvidScriptLatentCompletionProvider>& Provider,
		FString& OutError);
	static bool Unregister(
		const TSharedRef<IAvidScriptLatentCompletionProvider>& Provider);
	static TSharedPtr<IAvidScriptLatentCompletionProvider> FindByProviderId(
		const FString& ProviderId);
	static TSharedPtr<IAvidScriptLatentCompletionProvider> FindByFunctionPath(
		const FString& FunctionPath);
};

struct FAvidScriptBindingLatentCompletionContract
{
	FString Mode = TEXT("none");
	FString ProviderId;
	FString PayloadTypeId;
	FString StatusPolicy = TEXT("abandon_on_cancel");
	bool bCancellable = false;
	TSharedPtr<IAvidScriptLatentCompletionProvider> Provider;

	bool IsProvider() const
	{
		return Mode == TEXT("provider")
			&& !ProviderId.IsEmpty()
			&& !PayloadTypeId.IsEmpty()
			&& Provider.IsValid();
	}

	bool ResumesOutcomeOnCancel() const
	{
		return IsProvider()
			&& StatusPolicy == TEXT("resume_outcome_on_cancel")
			&& bCancellable;
	}
};

struct FAvidScriptBindingLatentReservation
{
	int64 Token = 0;
	UObject* CallbackTarget = nullptr;
	FName ExecutionFunction;
	int32 UUID = 0;
	int32 Linkage = INDEX_NONE;

	bool IsValid() const
	{
		return Token != 0
			&& CallbackTarget != nullptr
			&& !ExecutionFunction.IsNone()
			&& UUID > 0
			&& Linkage >= 0;
	}
};

struct FAvidScriptBindingAsyncActionOutcomeContract
{
	FString StableId;
	int32 Ordinal = INDEX_NONE;
	FMulticastDelegateProperty* DelegateProperty = nullptr;
	TSharedPtr<const IAvidScriptBindingAsyncActionPayloadEncoder>
		PayloadEncoder;
};

struct FAvidScriptBindingAsyncActionContract
{
	UClass* ActionClass = nullptr;
	FString PayloadTypeId;
	TArray<FAvidScriptBindingAsyncActionOutcomeContract> Outcomes;

	bool IsValid() const
	{
		if (ActionClass == nullptr
			|| PayloadTypeId.IsEmpty()
			|| Outcomes.IsEmpty())
		{
			return false;
		}
		const bool bUsesPayloadEncoders =
			Outcomes[0].PayloadEncoder.IsValid();
		return !Outcomes.ContainsByPredicate(
			[bUsesPayloadEncoders](
				const FAvidScriptBindingAsyncActionOutcomeContract& Outcome)
			{
				return Outcome.PayloadEncoder.IsValid()
					!= bUsesPayloadEncoders;
			});
	}
};

class IAvidScriptBindingLatentHost
{
public:
	virtual ~IAvidScriptBindingLatentHost() = default;

	virtual bool BeginLatent(
		int32 CallbackId,
		FAvidScriptBindingLatentReservation& OutReservation) = 0;
	virtual bool BeginLatentWithCompletion(
		int32 CallbackId,
		const FAvidScriptBindingLatentCompletionContract& Completion,
		FAvidScriptBindingLatentReservation& OutReservation)
	{
		OutReservation = {};
		return false;
	}
	virtual bool CommitLatent(int64 Token) = 0;
	virtual bool AbortLatent(int64 Token) = 0;
	virtual int64 BeginAsyncAction(
		int32 CallbackId,
		UObject& Action,
		const FAvidScriptBindingAsyncActionContract& Contract,
		FString& OutError)
	{
		OutError = TEXT("async_action_host_unsupported");
		return 0;
	}
};
