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

// Catalog tokens and managed object identity belong to the task's code
// activation. Error admission validates the catalog and the producer's frame
// root; a reader must acquire its own frame root before releasing the task.
struct FAvidScriptTaskLanguageError
{
	int32 TypeToken = 0;
	int32 SourceToken = 0;
	uint64 ObjectToken = 0;
};

class AVIDSCRIPTRUNTIME_API IAvidScriptTaskLanguageErrorLease
{
public:
	virtual ~IAvidScriptTaskLanguageErrorLease() = default;
};

struct FAvidScriptTaskResultSnapshot
{
	EAvidScriptTaskResultState State = EAvidScriptTaskResultState::Running;
	FString TypeId;
	TArray<uint8> Value;
	FString ErrorCode;
	TOptional<FAvidScriptTaskLanguageError> LanguageError;
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
	// Retains a task reference until the pending continuation is dispatched or
	// retired. The caller may then release its own reference before suspension.
	virtual bool RetainTaskForContinuation(
		int64 TaskToken, int64 ContinuationToken) = 0;
	virtual bool SucceedTaskResult(int64 Token, TConstArrayView<uint8> Value,
		TArray<int64>& OutWaiters) = 0;
	virtual bool FaultTaskResult(int64 Token, FString ErrorCode,
		TArray<int64>& OutWaiters) = 0;
	virtual bool FaultTaskResultLanguageError(int64 Token,
		FAvidScriptTaskLanguageError Error,
		TSharedPtr<IAvidScriptTaskLanguageErrorLease> RootLease,
		TArray<int64>& OutWaiters) = 0;
	virtual bool PropagateTaskFailure(int64 SourceToken, int64 TargetToken,
		TArray<int64>& OutWaiters) = 0;
	virtual bool CancelTaskResult(int64 Token, TArray<int64>& OutWaiters) = 0;
	virtual bool CancelTaskResultLanguageError(int64 Token,
		FAvidScriptTaskLanguageError Error,
		TSharedPtr<IAvidScriptTaskLanguageErrorLease> RootLease,
		TArray<int64>& OutWaiters) = 0;
	virtual bool ReadTaskResult(int64 Token,
		FAvidScriptTaskResultSnapshot& OutSnapshot) const = 0;
};
