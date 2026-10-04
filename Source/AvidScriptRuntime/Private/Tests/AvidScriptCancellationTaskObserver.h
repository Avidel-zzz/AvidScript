#pragma once

#include "AvidScriptTask.h"

namespace AvidScript::Tests::CompiledCancellation
{
// Forward every operation to the real Session endpoint. Snapshots contain no
// owning references, so observation cannot keep a Task or exception alive.
class FTaskObserver final : public IAvidScriptTaskHost
{
public:
	explicit FTaskObserver(IAvidScriptTaskHost& InHost) : Host(InHost) {}

	int64 CreateTaskResult(FString TypeId) override
	{
		const int64 Token = Host.CreateTaskResult(TypeId);
		if (Token != 0) CreatedTasks.Add(Token, MoveTemp(TypeId));
		return Token;
	}
	int64 CreateRootedTaskResult(FString TypeId) override
	{
		const int64 Token = Host.CreateRootedTaskResult(TypeId);
		if (Token != 0) CreatedTasks.Add(Token, MoveTemp(TypeId));
		return Token;
	}
	bool RetainTaskResult(int64 Token) override { return Host.RetainTaskResult(Token); }
	bool ReleaseTaskResult(int64 Token) override
	{
		FAvidScriptTaskResultSnapshot Snapshot;
		ReadTaskResult(Token, Snapshot);
		return Host.ReleaseTaskResult(Token);
	}
	bool HasTaskResultType(int64 Token, const FString& TypeId) const override
	{ return Host.HasTaskResultType(Token, TypeId); }
	EAvidScriptTaskWaitRegistration AwaitTaskResult(int64 Token, int32 CallbackId, int64& OutToken) override
	{
		const auto Registration = Host.AwaitTaskResult(Token, CallbackId, OutToken);
		if (Registration != EAvidScriptTaskWaitRegistration::Invalid)
		{
			AwaitedTasks.Add(Token);
			AwaitRegistrations.Add({Token, CallbackId, Registration});
		}
		FAvidScriptTaskResultSnapshot Snapshot;
		if (Registration == EAvidScriptTaskWaitRegistration::Ready && ReadTaskResult(Token, Snapshot)
			&& Snapshot.State == EAvidScriptTaskResultState::Cancelled) ++ReadyCancelledAwaits;
		return Registration;
	}
	bool BindTaskProducer(int64 TaskToken, int64 ContinuationToken) override
	{ return Host.BindTaskProducer(TaskToken, ContinuationToken); }
	bool RetainTaskForContinuation(int64 TaskToken, int64 ContinuationToken) override
	{ return Host.RetainTaskForContinuation(TaskToken, ContinuationToken); }
	bool SucceedTaskResult(int64 Token, TConstArrayView<uint8> Value, TArray<int64>& OutWaiters) override
	{ return Host.SucceedTaskResult(Token, Value, OutWaiters); }
	bool SucceedRootedTaskResult(int64 Token, TConstArrayView<uint8> Value,
		TSharedPtr<IAvidScriptTaskValueLease> Lease, TArray<int64>& OutWaiters) override
	{ return Host.SucceedRootedTaskResult(Token, Value, MoveTemp(Lease), OutWaiters); }
	bool FaultTaskResult(int64 Token, FString ErrorCode, TArray<int64>& OutWaiters) override
	{ return Host.FaultTaskResult(Token, MoveTemp(ErrorCode), OutWaiters); }
	bool FaultTaskResultLanguageError(int64 Token, FAvidScriptTaskLanguageError Error,
		TSharedPtr<IAvidScriptTaskLanguageErrorLease> Lease, TArray<int64>& OutWaiters) override
	{ return Host.FaultTaskResultLanguageError(Token, Error, MoveTemp(Lease), OutWaiters); }
	bool PropagateTaskFailure(int64 Source, int64 Target, TArray<int64>& OutWaiters) override
	{ return Host.PropagateTaskFailure(Source, Target, OutWaiters); }
	bool CancelTaskResult(int64 Token, TArray<int64>& OutWaiters) override
	{ return Host.CancelTaskResult(Token, OutWaiters); }
	bool CancelTaskResultLanguageError(int64 Token, FAvidScriptTaskLanguageError Error,
		TSharedPtr<IAvidScriptTaskLanguageErrorLease> Lease, TArray<int64>& OutWaiters) override
	{ return Host.CancelTaskResultLanguageError(Token, Error, MoveTemp(Lease), OutWaiters); }
	bool ReadTaskResult(int64 Token, FAvidScriptTaskResultSnapshot& OutSnapshot) const override
	{
		if (!Host.ReadTaskResult(Token, OutSnapshot)) return false;
		if (const auto* Previous = TerminalSnapshots.Find(Token))
		{
			bStableTerminalReads &= Previous->State == OutSnapshot.State
				&& Previous->TypeId == OutSnapshot.TypeId && Previous->Value == OutSnapshot.Value
				&& Previous->bValueRequiresLease == OutSnapshot.bValueRequiresLease
				&& Previous->ErrorCode == OutSnapshot.ErrorCode
				&& Previous->LanguageError.IsSet() == OutSnapshot.LanguageError.IsSet();
			if (Previous->LanguageError.IsSet() && OutSnapshot.LanguageError.IsSet())
				bStableTerminalReads &= Previous->LanguageError->TypeToken == OutSnapshot.LanguageError->TypeToken
					&& Previous->LanguageError->SourceToken == OutSnapshot.LanguageError->SourceToken
					&& Previous->LanguageError->ObjectToken == OutSnapshot.LanguageError->ObjectToken;
		}
		else TerminalSnapshots.Add(Token, OutSnapshot);
		return true;
	}
	int32 CountAwaited(EAvidScriptTaskResultState State) const
	{
		int32 Result = 0;
		// Compiler-owned error records also use Task storage, but are never
		// awaited by source code. Keep them out of the source Task state oracle.
		for (const auto& Entry : TerminalSnapshots)
			Result += AwaitedTasks.Contains(Entry.Key) && Entry.Value.State == State ? 1 : 0;
		return Result;
	}
	bool WasAwaited(int64 Token) const { return AwaitedTasks.Contains(Token); }
	// Called once after the entry invocation, before advancing the World. A
	// later source await identifies its Task even if it completed synchronously
	// or the guest released its last reference during that entry invocation.
	void CaptureEntryStates()
	{
		EntryStates.Reset();
		for (const auto& Entry : CreatedTasks)
		{
			FAvidScriptTaskResultSnapshot Snapshot;
			if (ReadTaskResult(Entry.Key, Snapshot)) EntryStates.Add(Entry.Key, Snapshot.State);
			else if (const auto* Terminal = TerminalSnapshots.Find(Entry.Key)) EntryStates.Add(Entry.Key, Terminal->State);
			else if (Host.HasTaskResultType(Entry.Key, Entry.Value)) EntryStates.Add(Entry.Key, EAvidScriptTaskResultState::Running);
		}
	}
	struct FAwaitRegistration
	{
		int64 Token = 0;
		int32 CallbackId = 0;
		EAvidScriptTaskWaitRegistration Registration = EAvidScriptTaskWaitRegistration::Invalid;
	};

	mutable TMap<int64, FAvidScriptTaskResultSnapshot> TerminalSnapshots;
	TMap<int64, EAvidScriptTaskResultState> EntryStates;
	TArray<FAwaitRegistration> AwaitRegistrations;
	mutable bool bStableTerminalReads = true;
	int32 ReadyCancelledAwaits = 0;
private:
	IAvidScriptTaskHost& Host;
	TSet<int64> AwaitedTasks;
	TMap<int64, FString> CreatedTasks;
};
}
