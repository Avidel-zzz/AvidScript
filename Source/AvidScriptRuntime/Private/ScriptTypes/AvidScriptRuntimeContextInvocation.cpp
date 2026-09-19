#include "AvidScriptWasmRuntime.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

bool FAvidScriptWasmRuntimeInstance::PrepareContextualExportCall(
	const FString& ExportName, FAvidScriptContextualExportCall& OutCall, FString& OutError)
{
	OutCall = {};
	OutError.Reset();
	if (!IsInGameThread() || !IsLoaded() || ContextInvocationDepth != 0 || ManagedHeapInvocationDepth != 0)
	{
		OutError = TEXT("contextual exports require an idle loaded GameThread Runtime");
		return false;
	}
	if (!PrepareNamedExportCall(ExportName, OutCall.Call, OutError)) return false;
	if (!ContextCallCodeIdentity) ContextCallCodeIdentity = MakeShared<const uint8>(0);
	OutCall.CodeIdentity = ContextCallCodeIdentity;
	OutCall.ExportName = ExportName;
	return true;
}

bool FAvidScriptWasmRuntimeInstance::PrepareContextualContinuationCall(FAvidScriptContextualExportCall& OutCall, FString& OutError)
{
	OutCall = {};
	OutError.Reset();
	if (!IsInGameThread() || !IsLoaded() || ContextInvocationDepth != 0 || ManagedHeapInvocationDepth != 0)
	{
		OutError = TEXT("contextual continuation preparation requires an idle loaded GameThread Runtime");
		return false;
	}
	if (ContinuationV2Export.Handle.IsValid()) return PrepareContextualExportCall(TEXT("avid_on_continuation_v2"), OutCall, OutError);
	if (ContinuationExport.Handle.IsValid()) return PrepareContextualExportCall(TEXT("avid_on_continuation"), OutCall, OutError);
	return true;
}

bool FAvidScriptWasmRuntimeInstance::ValidateInvocationContext(const FAvidScriptWasmHostContext& Context) const
{
	if (!IsInGameThread() || Context.ObjectRegistry == nullptr || !Context.OwnerHandle.IsValid()
		|| Context.World.IsStale() || (Context.World.IsValid() && Context.World->bIsTearingDown)
		|| (Context.DebugProbes != nullptr && Context.DebugProbes->IsExecutionSuspended())) return false;
	FAvidScriptObjectHandleResult Result;
	UObject* Owner = Context.ObjectRegistry->ResolveObject(Context.OwnerHandle, Result, false);
	return Owner != nullptr && Owner->GetWorld() == Context.World.Get()
		&& !Owner->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed);
}

void FAvidScriptWasmRuntimeInstance::LatchContextInvocationFailure(const FAvidScriptVmError& Error)
{
	if (ContextInvocationFailure.Category.IsEmpty()) ContextInvocationFailure = Error;
}

bool FAvidScriptWasmRuntimeInstance::RejectActiveContextMutation(
	const TCHAR* Operation, FAvidScriptWasmSmokeResult* OutResult)
{
	if (ContextInvocationDepth == 0) return false;
	FAvidScriptVmError Error;
	Error.Category = TEXT("context_invocation_mutation");
	Error.Details = FString::Printf(TEXT("%s requires the contextual invocation chain to return"), Operation);
	LatchContextInvocationFailure(Error);
	if (OutResult != nullptr)
	{
		*OutResult = {};
		OutResult->ModuleId = ModuleId;
		OutResult->ErrorCategory = Error.Category;
		OutResult->ErrorMessage = Error.Details;
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::InvokeInContext(
	const FAvidScriptContextualExportCall& Prepared, const FAvidScriptWasmHostContext& Context,
	const FAvidScriptVmCallFrame& Frame, FAvidScriptVmError& OutError, FAvidScriptVmCallResult* OutResult)
{
	if (OutResult) *OutResult = {};
	const bool bSucceeded = InvokeContextOperation(Prepared, Context,
		[&](FAvidScriptVmError& Error) { return Prepared.Call.Call(Frame, Error, OutResult); }, OutError);
	if (!bSucceeded && OutResult) *OutResult = {};
	return bSucceeded;
}

bool FAvidScriptWasmRuntimeInstance::InvokeContextOperation(
	const FAvidScriptContextualExportCall& Prepared, const FAvidScriptWasmHostContext& Context,
	TFunctionRef<bool(FAvidScriptVmError&)> Operation, FAvidScriptVmError& OutError)
{
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError.Category = TEXT("context_invocation_thread");
		return false;
	}
	const bool bRoot = ContextInvocationDepth == 0;
	if (bRoot)
	{
		ContextInvocationEntries = 0;
		ContextInvocationFailure.Reset();
	}
	auto Reject = [&](const TCHAR* Category, const TCHAR* Details)
	{
		OutError.Category = Category;
		OutError.Details = Details;
		if (!bRoot) LatchContextInvocationFailure(OutError);
		return false;
	};
	if (!ContextInvocationFailure.Category.IsEmpty())
	{
		OutError = ContextInvocationFailure;
		return false;
	}
	if (!IsLoaded() || !Prepared.IsValid() || Prepared.CodeIdentity != ContextCallCodeIdentity)
		return Reject(TEXT("context_invocation_code"), TEXT("prepared export belongs to another or retired Runtime code generation"));
	if (ContextInvocationDepth >= 64 || ContextInvocationEntries >= 4096)
		return Reject(TEXT("context_invocation_budget"), TEXT("contextual invocation depth or entry budget exhausted"));
	if (bRoot && ManagedHeapInvocationDepth != 0)
		return Reject(TEXT("context_invocation_unscoped"), TEXT("cannot reenter an export that has no instance context scope"));
	if ((!bRoot && !ValidateInvocationContext(HostContext)) || !ValidateInvocationContext(Context)
		|| Context.ObjectRegistry != HostContext.ObjectRegistry || Context.World != HostContext.World
		|| Context.HostEffectJournal != HostContext.HostEffectJournal)
		return Reject(TEXT("context_invocation_authority"), TEXT("owner, registry, World or effect transaction does not match the execution domain"));

	++ContextInvocationEntries;
	TGuardValue<uint32> DepthGuard(ContextInvocationDepth, ContextInvocationDepth + 1);
	const FAvidScriptWasmHostContext PreviousHostContext = HostContext;
	const FAvidScriptBindingInvocationContext PreviousBindingContext = BindingInvocationContext;
	// The outer reflection call can still own pointers into this buffer. Move its
	// allocation intact and give the nested call distinct storage.
	TArray<uint8> PreviousScratch;
	if (!bRoot)
	{
		PreviousScratch = MoveTemp(BindingInvocationScratch);
		BindingInvocationScratch.SetNumUninitialized(PreviousScratch.Num());
	}
	TGuardValue<FAvidScriptPreparedDelegateOutputTransaction*> OutputGuard(ActiveDelegateOutputTransaction, nullptr);
	TGuardValue<uint32> OutputTokenGuard(ActiveDelegateOutputToken, 0);
	TGuardValue<bool> ContinuationGuard(bContinuationDispatchActive, false);
	TGuardValue<bool> ResultGuard(bContinuationResultConsumed, false);
	TGuardValue<bool> StateGuard(bContinuationStateConsumed, false);
	TGuardValue<int64> TokenGuard(ActiveContinuationToken, 0);
	TGuardValue<EAvidScriptContinuationStatus> StatusGuard(ActiveContinuationStatus, EAvidScriptContinuationStatus::Failed);
	TGuardValue<int32> SlotGuard(ActiveContinuationResultSlot, 0);
	TGuardValue<int32> GenerationGuard(ActiveContinuationResultGeneration, 0);
	TGuardValue<FAvidScriptContinuationResultCodecTransaction*> TransactionGuard(ActiveContinuationResultTransaction, nullptr);
	ApplyHostContext(Context);
	BindingInvocationContext.ScopedObjectCapabilities = {};
	BeginTypedCallbackEpoch();
	ON_SCOPE_EXIT
	{
		EndTypedCallbackEpoch();
		ApplyHostContext(PreviousHostContext);
		BindingInvocationContext = PreviousBindingContext;
		if (!bRoot) BindingInvocationScratch = MoveTemp(PreviousScratch);
	};
	const bool bCalled = Operation(OutError);
	if (!bCalled)
	{
		if (OutError.Category.IsEmpty()) OutError.Category = TEXT("context_invocation_failed");
		LatchContextInvocationFailure(OutError);
	}
	else if (!ValidateInvocationContext(Context))
	{
		OutError.Category = TEXT("context_invocation_retired");
		OutError.Details = TEXT("target owner or World retired before the invocation returned");
		LatchContextInvocationFailure(OutError);
	}
	if (!ContextInvocationFailure.Category.IsEmpty())
	{
		OutError = ContextInvocationFailure;
		return false;
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::ValidateContextCallbackCommit(FAvidScriptVmError& OutError) const
{
	if (ContextInvocationDepth == 0) return true;
	if (!ContextInvocationFailure.Category.IsEmpty())
	{
		OutError = ContextInvocationFailure;
		return false;
	}
	if (!ValidateInvocationContext(HostContext))
	{
		OutError.Category = TEXT("context_invocation_retired");
		OutError.Details = TEXT("callback owner retired before output or result commit");
		return false;
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchContinuationInContext(
	const FAvidScriptContextualExportCall& Prepared, const FAvidScriptWasmHostContext& Context,
	const FAvidScriptContinuationCompletion& Completion, FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = {};
	if (!IsInGameThread())
	{
		OutResult.ErrorCategory = TEXT("context_invocation_thread");
		OutResult.ErrorMessage = TEXT("contextual continuation dispatch requires the Game Thread");
		return false;
	}
	FAvidScriptVmError Error;
	const bool bSucceeded = InvokeContextOperation(Prepared, Context, [&](FAvidScriptVmError& Failure)
	{
		const TCHAR* ExpectedExport = ContinuationV2Export.Handle.IsValid() ? TEXT("avid_on_continuation_v2") : TEXT("avid_on_continuation");
		if (Prepared.ExportName != ExpectedExport || Context.Continuations == nullptr)
		{
			Failure.Category = TEXT("context_callback_contract");
			Failure.Details = TEXT("continuation entry and instance continuation host must match this Runtime");
			return false;
		}
		if (DispatchContinuationInternal(Completion, OutResult, &Prepared.Call)) return true;
		Failure.Category = OutResult.ErrorCategory;
		Failure.Details = OutResult.ErrorMessage;
		return false;
	}, Error);
	if (!bSucceeded) RecordPreparedVmFailure(Prepared.ExportName, Error, OutResult);
	return bSucceeded;
}

bool FAvidScriptWasmRuntimeInstance::DispatchPreparedDelegateEventInContext(
	const FAvidScriptContextualExportCall& Prepared, const FAvidScriptWasmHostContext& Context,
	const FAvidScriptPreparedDelegateEvent& Event, void* NativeParameters, FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = {};
	if (!IsInGameThread())
	{
		OutResult.ErrorCategory = TEXT("context_invocation_thread");
		OutResult.ErrorMessage = TEXT("contextual delegate dispatch requires the Game Thread");
		return false;
	}
	FAvidScriptVmError Error;
	const bool bSucceeded = InvokeContextOperation(Prepared, Context, [&](FAvidScriptVmError& Failure)
	{
		if (Prepared.ExportName != Event.ExportName)
		{
			Failure.Category = TEXT("context_callback_contract");
			Failure.Details = TEXT("delegate plan and contextual export do not identify the same callback");
			return false;
		}
		if (DispatchPreparedDelegateEventInternal(Event, NativeParameters, OutResult, &Prepared.Call)) return true;
		Failure.Category = OutResult.ErrorCategory;
		Failure.Details = OutResult.ErrorMessage;
		return false;
	}, Error);
	if (!bSucceeded) RecordPreparedVmFailure(Event.ExportName, Error, OutResult);
	return bSucceeded;
}
