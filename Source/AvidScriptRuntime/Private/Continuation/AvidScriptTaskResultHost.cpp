#include "AvidScriptWasmRuntime.h"
#include "AvidScriptTaskResultAbi.h"

bool FAvidScriptWasmRuntimeInstance::DispatchTaskBindProducerCall(
	const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult)
{
	OutResult = {};
	if (!IsInGameThread() || !IsLoaded() || ManagedHeapInvocationDepth == 0
		|| HostContext.Tasks == nullptr)
	{
		OutResult.ErrorCategory = TEXT("task_result_context");
		OutResult.Details = TEXT("Task producer binding requires a live Session and managed VM invocation.");
		return false;
	}
	const int64 TaskToken = Call.Int64Args[0];
	const int64 ContinuationToken = Call.Int64Args[1];
	if (TaskToken <= 0 || ContinuationToken <= 0
		|| !HostContext.Tasks->HasTaskResultType(TaskToken, TEXT("type:int32"))
		|| !HostContext.Tasks->BindTaskProducer(TaskToken, ContinuationToken))
	{
		OutResult.ErrorCategory = TEXT("task_result_bind_producer");
		OutResult.Details = TEXT("Session rejected Task<int> producer binding.");
		return false;
	}
	OutResult.ReturnValue = 1;
	OutResult.ReturnValueI64 = 1;
	OutResult.bSucceeded = true;
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchTaskResultInt32Call(
	const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult)
{
	using namespace AvidScript::TaskResult::Abi;
	OutResult = {};
	auto Fail = [&OutResult](const TCHAR* Category, const TCHAR* Details)
	{
		OutResult.ErrorCategory = Category;
		OutResult.Details = Details;
		return false;
	};
	if (!IsInGameThread() || !IsLoaded() || ManagedHeapInvocationDepth == 0
		|| HostContext.Tasks == nullptr)
	{
		return Fail(TEXT("task_result_context"),
			TEXT("Task result operations require a live Session and managed VM invocation."));
	}
	const ECommand Command = static_cast<ECommand>(Call.IntArgs[0]);
	const int64 Token = Call.Int64Args[0];
	const int32 Argument = Call.IntArgs[1];
	if (Call.IntArgs[2] != 0)
	{
		return Fail(TEXT("task_result_packet"), TEXT("Reserved task result argument must be zero."));
	}
	if (Command == ECommand::Create)
	{
		if (Token != 0 || Argument != 0)
		{
			return Fail(TEXT("task_result_packet"), TEXT("Task creation has unexpected arguments."));
		}
		OutResult.ReturnValueI64 = HostContext.Tasks->CreateTaskResult(TEXT("type:int32"));
		if (OutResult.ReturnValueI64 == 0)
		{
			return Fail(TEXT("task_result_create"), TEXT("Session rejected Task<int> creation."));
		}
	}
	else
	{
		if (Token <= 0 || !HostContext.Tasks->HasTaskResultType(Token, TEXT("type:int32")))
		{
			return Fail(TEXT("task_result_identity"),
				TEXT("Task token is stale, foreign or has the wrong result type."));
		}
		switch (Command)
		{
		case ECommand::Retain:
		case ECommand::Release:
		case ECommand::Cancel:
		case ECommand::Read:
			if (Argument != 0)
			{
				return Fail(TEXT("task_result_packet"), TEXT("Task operation has unexpected arguments."));
			}
			break;
		case ECommand::Await:
			if (Argument <= 0)
			{
				return Fail(TEXT("task_result_packet"), TEXT("Task await requires a callback ID."));
			}
			break;
		case ECommand::Succeed:
			break;
		default:
			return Fail(TEXT("task_result_command"), TEXT("Unknown Task<int> command."));
		}

		switch (Command)
		{
		case ECommand::Retain:
			if (!HostContext.Tasks->RetainTaskResult(Token))
				return Fail(TEXT("task_result_retain"), TEXT("Session rejected task retention."));
			OutResult.ReturnValueI64 = 1;
			break;
		case ECommand::Release:
			if (!HostContext.Tasks->ReleaseTaskResult(Token))
				return Fail(TEXT("task_result_release"), TEXT("Session rejected task release."));
			OutResult.ReturnValueI64 = 1;
			break;
		case ECommand::Await:
		{
			int64 ContinuationToken = 0;
			const EAvidScriptTaskWaitRegistration Registration =
				HostContext.Tasks->AwaitTaskResult(Token, Argument, ContinuationToken);
			if (Registration == EAvidScriptTaskWaitRegistration::Invalid
				|| (Registration == EAvidScriptTaskWaitRegistration::Queued
					&& ContinuationToken <= 0))
			{
				return Fail(TEXT("task_result_await"), TEXT("Session rejected task await."));
			}
			OutResult.ReturnValueI64 = Registration == EAvidScriptTaskWaitRegistration::Ready
				? 0 : ContinuationToken;
			break;
		}
		case ECommand::Succeed:
		{
			const uint32 Bits = static_cast<uint32>(Argument);
			const uint8 Bytes[4] = {
				static_cast<uint8>(Bits),
				static_cast<uint8>(Bits >> 8),
				static_cast<uint8>(Bits >> 16),
				static_cast<uint8>(Bits >> 24)
			};
			TArray<int64> Waiters;
			if (!HostContext.Tasks->SucceedTaskResult(Token, MakeArrayView(Bytes), Waiters))
				return Fail(TEXT("task_result_complete"), TEXT("Session rejected Task<int> completion."));
			OutResult.ReturnValueI64 = 1;
			break;
		}
		case ECommand::Cancel:
		{
			TArray<int64> Waiters;
			if (!HostContext.Tasks->CancelTaskResult(Token, Waiters))
				return Fail(TEXT("task_result_cancel"), TEXT("Session rejected task cancellation."));
			OutResult.ReturnValueI64 = 1;
			break;
		}
		case ECommand::Read:
		{
			FAvidScriptTaskResultSnapshot Snapshot;
			if (!HostContext.Tasks->ReadTaskResult(Token, Snapshot)
				|| Snapshot.TypeId != TEXT("type:int32"))
			{
				return Fail(TEXT("task_result_read"), TEXT("Task result is unavailable or has the wrong type."));
			}
			EState State;
			int32 Value = 0;
			if (Snapshot.State == EAvidScriptTaskResultState::Succeeded)
			{
				if (Snapshot.Value.Num() != sizeof(int32))
					return Fail(TEXT("task_result_codec"), TEXT("Task<int> result must contain four bytes."));
				const uint32 Bits = uint32(Snapshot.Value[0])
					| (uint32(Snapshot.Value[1]) << 8)
					| (uint32(Snapshot.Value[2]) << 16)
					| (uint32(Snapshot.Value[3]) << 24);
				Value = static_cast<int32>(Bits);
				State = EState::Succeeded;
			}
			else if (Snapshot.State == EAvidScriptTaskResultState::Faulted)
			{
				State = EState::Faulted;
			}
			else if (Snapshot.State == EAvidScriptTaskResultState::Cancelled)
			{
				State = EState::Cancelled;
			}
			else
			{
				return Fail(TEXT("task_result_read"), TEXT("Running task has no result."));
			}
			OutResult.ReturnValueI64 = static_cast<int64>(PackRead(State, Value));
			break;
		}
		default:
			checkNoEntry();
		}
	}
	OutResult.ReturnValue = static_cast<int32>(OutResult.ReturnValueI64);
	OutResult.bSucceeded = true;
	return true;
}
