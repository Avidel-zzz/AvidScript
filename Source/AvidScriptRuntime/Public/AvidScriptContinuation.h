#pragma once

#include "AvidScriptBindingLatent.h"
#include "AvidScriptManagedStateLease.h"
#include "CoreMinimal.h"

class FAvidScriptWasmRuntimeInstance;

// Native-only ownership. A lease must not extend the lifetime of its Runtime.
class AVIDSCRIPTRUNTIME_API IAvidScriptContinuationStateLease : public IAvidScriptManagedStateLease
{
public:
	virtual ~IAvidScriptContinuationStateLease() = default;
	virtual bool IsValidForRuntime(const FAvidScriptWasmRuntimeInstance& Runtime) const = 0;
};

enum class EAvidScriptContinuationStatus : int32
{
	Completed = 1,
	Failed = 2,
	Cancelled = 3
};

struct FAvidScriptContinuationCompletion
{
	int32 CallbackId = 0;
	int64 Token = 0;
	EAvidScriptContinuationStatus Status =
		EAvidScriptContinuationStatus::Completed;
	int32 ObjectSlot = 0;
	int32 ObjectGeneration = 0;
	uint64 RegistrationSerial = 0;
};

class AVIDSCRIPTRUNTIME_API IAvidScriptContinuationHost
{
public:
	virtual ~IAvidScriptContinuationHost() = default;

	virtual int64 ScheduleDelay(float DelaySeconds, int32 CallbackId) = 0;
	// A compiler-owned await may resume on active cancellation to run its
	// cleanup. Older hosts fail closed instead of silently dropping cleanup.
	virtual int64 ScheduleDelayWithCancelResume(float, int32) { return 0; }
	virtual int64 ScheduleObjectLoad(FString ObjectPath, int32 CallbackId) = 0;
	virtual bool Cancel(int64 Token) = 0;
	virtual int64 CreateCancellationSource() = 0;
	virtual bool CancelCancellationSource(int64 SourceToken) = 0;
	virtual bool ReleaseCancellationSource(int64 SourceToken) = 0;
	virtual bool BindCancellationSource(
		int64 SourceToken,
		int64 ContinuationToken) = 0;
	virtual bool StoreState(
		int64 ContinuationToken,
		TConstArrayView<uint8> StateBytes)
	{
		return false;
	}
	virtual bool ReadState(
		int64 ContinuationToken,
		TArrayView<uint8> OutStateBytes)
	{
		return false;
	}
	virtual bool ConsumeResult(
		int64 ContinuationToken,
		int32 Slot,
		int32 Generation,
		const FString& ExpectedTypeId,
		FAvidScriptBindingLatentCompletionPayload& OutPayload)
	{
		return false;
	}
	// Ownership transfers only on success. Bytes must come from a validated
	// native state layout; these methods are not an arbitrary Guest root ABI.
	virtual bool StoreManagedState(
		int64 ContinuationToken,
		const FAvidScriptWasmRuntimeInstance& Runtime,
		TConstArrayView<uint8> StateBytes,
		TUniquePtr<IAvidScriptContinuationStateLease>&& Lease)
	{
		return false;
	}
	virtual bool ReadManagedState(
		int64 ContinuationToken,
		const FAvidScriptWasmRuntimeInstance& Runtime,
		TArrayView<uint8> OutStateBytes)
	{
		return false;
	}
};
