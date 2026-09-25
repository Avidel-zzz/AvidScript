#include "AvidScriptWasmRuntime.h"
#include "AvidScriptLanguageErrorCatalog.h"
#include "AvidScriptTaskResultAbi.h"
#include "Memory/AvidScriptManagedHeap.h"

namespace
{
class FAvidScriptTaskLanguageErrorHeapLease final : public IAvidScriptTaskLanguageErrorLease
{
public:
	explicit FAvidScriptTaskLanguageErrorHeapLease(AvidScript::Managed::FPersistentRoots&& InRoots)
		: Roots(MoveTemp(InRoots)) {}
private:
	AvidScript::Managed::FPersistentRoots Roots;
};
}

bool FAvidScriptWasmRuntimeInstance::AdmitTaskLanguageError(
	const int64 TaskToken, const int32 TypeToken, const int32 SourceToken,
	const uint64 ObjectToken, FAvidScriptHostCallResult& OutResult)
{
	OutResult = {};
	auto Fail = [&OutResult](const TCHAR* Category, const TCHAR* Details)
	{
		OutResult.ErrorCategory = Category;
		OutResult.Details = Details;
		return false;
	};
	if (!IsInGameThread() || !IsLoaded() || ManagedHeapInvocationDepth == 0
		|| !ManagedHeap || HostContext.Tasks == nullptr)
	{
		return Fail(TEXT("task_result_context"),
			TEXT("Task language errors require a live Session and managed VM invocation."));
	}
	if (TaskToken <= 0 || !HostContext.Tasks->HasTaskResultType(TaskToken, TEXT("type:int32")))
	{
		return Fail(TEXT("task_result_identity"),
			TEXT("Task token is stale, foreign or has the wrong result type."));
	}
	if (!LanguageErrorCatalog || !LanguageErrorCatalog->FindType(TypeToken)
		|| !LanguageErrorCatalog->FindSource(SourceToken))
	{
		return Fail(TEXT("task_language_error_catalog"),
			TEXT("Task language error tokens must belong to the loaded module catalog."));
	}
	if (!ManagedHeap->IsObjectRootedInCurrentFrame(ObjectToken, ManagedHeapFrameFloor))
	{
		return Fail(TEXT("task_language_error_root"),
			TEXT("Task language error object needs a root in the current VM invocation."));
	}
	AvidScript::Managed::FPersistentRoots Roots;
	if (ManagedHeap->RetainPersistent({&ObjectToken, 1}, Roots)
		!= AvidScript::Managed::EHeapError::Ok)
	{
		return Fail(TEXT("task_language_error_root"),
			TEXT("Task language error root could not be retained."));
	}
	TSharedPtr<IAvidScriptTaskLanguageErrorLease> Lease =
		MakeShared<FAvidScriptTaskLanguageErrorHeapLease>(MoveTemp(Roots));
	TArray<int64> Waiters;
	if (!HostContext.Tasks->FaultTaskResultLanguageError(TaskToken,
		{TypeToken, SourceToken, ObjectToken}, MoveTemp(Lease), Waiters))
	{
		return Fail(TEXT("task_result_complete"),
			TEXT("Session rejected Task<int> language-error completion."));
	}
	OutResult.ReturnValue = 1;
	OutResult.ReturnValueI64 = 1;
	OutResult.bSucceeded = true;
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchTaskFaultLanguageErrorCall(
	const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult)
{
	OutResult = {};
	if (!LanguageErrorCatalog || !LanguageErrorCatalog->SupportsTaskLanguageErrorFault())
	{
		OutResult.ErrorCategory = TEXT("task_language_error_version");
		OutResult.Details = TEXT("Task language-error fault import requires catalog-bearing Guest IR 20/1.19 or 21/1.20.");
		return false;
	}
	return AdmitTaskLanguageError(Call.Int64Args[0], Call.IntArgs[0], Call.IntArgs[1],
		static_cast<uint64>(Call.Int64Args[1]), OutResult);
}

bool FAvidScriptWasmRuntimeInstance::FindTaskLanguageError(
	const int64 TaskToken, FAvidScriptTaskLanguageError& OutError,
	FAvidScriptHostCallResult& OutResult)
{
	OutError = {};
	OutResult = {};
	auto Fail = [&OutResult](const TCHAR* Category, const TCHAR* Details)
	{
		OutResult.ErrorCategory = Category;
		OutResult.Details = Details;
		return false;
	};
	if (!LanguageErrorCatalog || !LanguageErrorCatalog->SupportsTaskLanguageErrorFault())
	{
		return Fail(TEXT("task_language_error_version"),
			TEXT("Task language-error read requires a catalog-bearing combined module."));
	}
	if (!IsInGameThread() || !IsLoaded() || ManagedHeapInvocationDepth == 0
		|| !ManagedHeap || HostContext.Tasks == nullptr)
	{
		return Fail(TEXT("task_result_context"),
			TEXT("Task language-error read requires a live Session and managed VM invocation."));
	}
	if (TaskToken <= 0 || !HostContext.Tasks->HasTaskResultType(TaskToken, TEXT("type:int32")))
	{
		return Fail(TEXT("task_result_identity"),
			TEXT("Task token is stale, foreign or has the wrong result type."));
	}
	FAvidScriptTaskResultSnapshot Snapshot;
	if (!HostContext.Tasks->ReadTaskResult(TaskToken, Snapshot)
		|| Snapshot.TypeId != TEXT("type:int32")
		|| Snapshot.State != EAvidScriptTaskResultState::Faulted
		|| !Snapshot.LanguageError.IsSet())
	{
		return Fail(TEXT("task_language_error_read"),
			TEXT("Task has no completed language-error payload."));
	}
	const FAvidScriptTaskLanguageError& Error = Snapshot.LanguageError.GetValue();
	if (!LanguageErrorCatalog->FindType(Error.TypeToken)
		|| !LanguageErrorCatalog->FindSource(Error.SourceToken))
	{
		return Fail(TEXT("task_language_error_catalog"),
			TEXT("Task language-error payload is absent from the loaded module catalog."));
	}
	OutError = Error;
	OutResult.ReturnValue = 1;
	OutResult.ReturnValueI64 = 1;
	OutResult.bSucceeded = true;
	return true;
}

bool FAvidScriptWasmRuntimeInstance::ReadTaskLanguageError(
	const int64 TaskToken, FAvidScriptTaskLanguageError& OutError,
	FAvidScriptHostCallResult& OutResult)
{
	if (!FindTaskLanguageError(TaskToken, OutError, OutResult)) return false;
	if (ManagedHeap->RootObjectInCurrentFrame(OutError.ObjectToken, ManagedHeapFrameFloor)
		!= AvidScript::Managed::EHeapError::Ok)
	{
		OutError = {};
		OutResult = {};
		OutResult.ErrorCategory = TEXT("task_language_error_root");
		OutResult.Details = TEXT("Task language-error object could not be rooted in the current invocation.");
		return false;
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchTaskLanguageErrorMetaCall(
	const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult)
{
	FAvidScriptTaskLanguageError Error;
	if (!FindTaskLanguageError(Call.Int64Args[0], Error, OutResult)) return false;
	OutResult.ReturnValueI64 = static_cast<int64>(
		(uint64(static_cast<uint32>(Error.TypeToken)) << 32)
		| static_cast<uint32>(Error.SourceToken));
	OutResult.ReturnValue = static_cast<int32>(OutResult.ReturnValueI64);
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchTaskLanguageErrorRootCall(
	const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult)
{
	FAvidScriptTaskLanguageError Error;
	if (!ReadTaskLanguageError(Call.Int64Args[0], Error, OutResult)) return false;
	OutResult.ReturnValueI64 = static_cast<int64>(Error.ObjectToken);
	OutResult.ReturnValue = static_cast<int32>(OutResult.ReturnValueI64);
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchTaskPropagateFailureCall(
	const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult)
{
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
			TEXT("Task failure propagation requires a live Session and managed VM invocation."));
	}
	const int64 SourceToken = Call.Int64Args[0];
	const int64 TargetToken = Call.Int64Args[1];
	if (SourceToken <= 0 || TargetToken <= 0 || SourceToken == TargetToken
		|| !HostContext.Tasks->HasTaskResultType(SourceToken, TEXT("type:int32"))
		|| !HostContext.Tasks->HasTaskResultType(TargetToken, TEXT("type:int32")))
	{
		return Fail(TEXT("task_result_identity"),
			TEXT("Task failure propagation needs distinct live Task<int> tokens in the current Session."));
	}
	FAvidScriptTaskResultSnapshot Source;
	if (!HostContext.Tasks->ReadTaskResult(SourceToken, Source)
		|| Source.TypeId != TEXT("type:int32"))
	{
		return Fail(TEXT("task_result_read"),
			TEXT("Source Task<int> has no readable terminal result."));
	}
	TArray<int64> Waiters;
	const bool bPropagated = HostContext.Tasks->PropagateTaskFailure(
		SourceToken, TargetToken, Waiters);
	if (!bPropagated)
	{
		return Fail(TEXT("task_result_propagate"),
			TEXT("Only a cancelled or faulted Task<int> can terminate a running target task."));
	}
	OutResult.ReturnValue = 1;
	OutResult.ReturnValueI64 = 1;
	OutResult.bSucceeded = true;
	return true;
}

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

bool FAvidScriptWasmRuntimeInstance::DispatchTaskRetainForContinuationCall(
	const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult)
{
	OutResult = {};
	if (!IsInGameThread() || !IsLoaded() || ManagedHeapInvocationDepth == 0
		|| HostContext.Tasks == nullptr)
	{
		OutResult.ErrorCategory = TEXT("task_result_context");
		OutResult.Details = TEXT("Task continuation retention requires a live Session and managed VM invocation.");
		return false;
	}
	const int64 TaskToken = Call.Int64Args[0];
	const int64 ContinuationToken = Call.Int64Args[1];
	if (TaskToken <= 0 || ContinuationToken <= 0
		|| !HostContext.Tasks->HasTaskResultType(TaskToken, TEXT("type:int32"))
		|| !HostContext.Tasks->RetainTaskForContinuation(TaskToken, ContinuationToken))
	{
		OutResult.ErrorCategory = TEXT("task_result_retain_for_continuation");
		OutResult.Details = TEXT("Session rejected Task<int> continuation retention.");
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
