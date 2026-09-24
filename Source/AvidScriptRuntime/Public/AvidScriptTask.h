#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptTaskResultState : uint8
{
	Running,
	Succeeded,
	Faulted,
	Cancelled
};

enum class EAvidScriptTaskWaitRegistration : uint8
{
	Invalid,
	Queued,
	Ready
};

struct FAvidScriptTaskResultSnapshot
{
	EAvidScriptTaskResultState State = EAvidScriptTaskResultState::Running;
	FString TypeId;
	TArray<uint8> Value;
	FString ErrorCode;
};

// Native Session capability. Guest bytes must be validated against a versioned
// task value codec before an import can call this interface.
class AVIDSCRIPTRUNTIME_API IAvidScriptTaskHost
{
public:
	virtual ~IAvidScriptTaskHost() = default;

	virtual int64 CreateTaskResult(FString TypeId) = 0;
	virtual bool RetainTaskResult(int64 Token) = 0;
	virtual bool ReleaseTaskResult(int64 Token) = 0;
	virtual bool HasTaskResultType(int64 Token, const FString& TypeId) const = 0;
	// Returns a Session-owned continuation token only when the task is pending.
	virtual EAvidScriptTaskWaitRegistration AwaitTaskResult(
		int64 Token, int32 CallbackId, int64& OutContinuationToken) = 0;
	// The Session retains one producer reference while this continuation is pending.
	// A dispatch may transfer that reference to its next continuation.
	virtual bool BindTaskProducer(int64 TaskToken, int64 ContinuationToken) = 0;
	virtual bool SucceedTaskResult(int64 Token, TConstArrayView<uint8> Value,
		TArray<int64>& OutWaiters) = 0;
	virtual bool FaultTaskResult(int64 Token, FString ErrorCode,
		TArray<int64>& OutWaiters) = 0;
	virtual bool CancelTaskResult(int64 Token, TArray<int64>& OutWaiters) = 0;
	virtual bool ReadTaskResult(int64 Token,
		FAvidScriptTaskResultSnapshot& OutSnapshot) const = 0;
};
