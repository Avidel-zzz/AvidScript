#include "AvidScriptWasmRuntime.h"
#include "ScriptTypes/AvidScriptGeneratedTypeAuthority.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "Memory/AvidScriptManagedHeapProtocol.h"
#include "Memory/AvidScriptManagedRootTransfer.h"

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

bool FAvidScriptWasmRuntimeInstance::ValidateInvocationContext(const FAvidScriptWasmHostContext& Context, bool bAllowSuspended) const
{
	if (!IsInGameThread() || Context.ObjectRegistry == nullptr || !Context.OwnerHandle.IsValid()
		|| !ValidateInstanceExecutionState(Context)
		|| Context.World.IsStale() || (Context.World.IsValid() && Context.World->bIsTearingDown)
		|| (!bAllowSuspended && Context.DebugProbes != nullptr && Context.DebugProbes->IsExecutionSuspended())) return false;
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
		[&](FAvidScriptVmError& Error)
		{
			if (Context.InstanceExecutionState && GetLifecycleState() != EAvidScriptLifecycleState::Running)
			{
				Error.Category = TEXT("context_instance_not_running");
				Error.Details = TEXT("instance exports require a successful BeginPlay and an active owner");
				return false;
			}
			return Prepared.Call.Call(Frame, Error, OutResult);
		}, OutError);
	if (!bSucceeded && OutResult) *OutResult = {};
	return bSucceeded;
}

bool FAvidScriptWasmRuntimeInstance::InvokeGeneratedInstanceExport(const FAvidScriptObjectHandle& Target,
	const FAvidScriptContextualExportCall& Call, const FAvidScriptVmCallFrame& Frame,
	FAvidScriptVmError& OutError, FAvidScriptVmCallResult* OutResult, TConstArrayView<uint64> ReturnedRoots)
{
	OutError.Reset();
	if (OutResult) *OutResult = {};
	if (!IsInGameThread()) { OutError.Category = TEXT("context_invocation_thread"); return false; }
	if (!ContextInvocationFailure.Category.IsEmpty()) { OutError = ContextInvocationFailure; return false; }
	const auto Authority = HostContext.GeneratedTypeAuthority.Pin();
	if (!ContextInvocationDepth || !ValidateInvocationContext(HostContext)
		|| GetLifecycleState() != EAvidScriptLifecycleState::Running || !Authority)
	{
		OutError.Category = TEXT("generated_invocation_source");
		OutError.Details = TEXT("instance routing requires an active running generated owner context");
	}
	else
	{
		const auto RootStatus = ReturnedRoots.IsEmpty() ? AvidScript::Managed::EHeapError::Ok
			: ManagedHeap && ManagedHeapInvocationDepth != 0 ? ManagedHeap->ValidateRootTransfer(
				{ReturnedRoots.GetData(), static_cast<size_t>(ReturnedRoots.Num())}, ManagedHeapFrameFloor) : AvidScript::Managed::EHeapError::RootAuthority;
		if (RootStatus != AvidScript::Managed::EHeapError::Ok)
		{
			OutError.Category = TEXT("generated_invocation_roots");
			OutError.Details = FString::Printf(TEXT("returned roots require the caller's current frame: %s"),
				UTF8_TO_TCHAR(AvidScript::Managed::HeapErrorName(RootStatus)));
		}
		else
		{
			FAvidScriptManagedRootTransfer Transfer;
			Transfer.InvocationDepth = ManagedHeapInvocationDepth + 1;
			Transfer.FrameFloor = ManagedHeap ? ManagedHeap->GetStats().ActiveFrames : 0;
			Transfer.Roots.Append(ReturnedRoots.GetData(), ReturnedRoots.Num());
			TGuardValue<const FAvidScriptManagedRootTransfer*> TransferGuard(ActiveRootTransfer, &Transfer);
			if (Authority->InvokeInstanceExport(*this, Target, Call, Frame, OutError, OutResult)) return true;
		}
	}
	if (OutError.Category.IsEmpty()) OutError.Category = TEXT("generated_invocation_rejected");
	if (ContextInvocationDepth) LatchContextInvocationFailure(OutError);
	if (OutResult) *OutResult = {};
	return false;
}

bool FAvidScriptWasmRuntimeInstance::InvokeContextOperation(
	const FAvidScriptContextualExportCall& Prepared, const FAvidScriptWasmHostContext& Context,
	TFunctionRef<bool(FAvidScriptVmError&)> Operation, FAvidScriptVmError& OutError, bool bResumeSuspended)
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
	if ((!bRoot && !ValidateInvocationContext(HostContext)) || !ValidateInvocationContext(Context, bResumeSuspended)
		|| Context.ObjectRegistry != HostContext.ObjectRegistry || Context.World != HostContext.World
		|| Context.HostEffectJournal != HostContext.HostEffectJournal)
		return Reject(TEXT("context_invocation_authority"), TEXT("owner, registry, World or effect transaction does not match the execution domain"));

	++ContextInvocationEntries;
	TGuardValue<uint32> DepthGuard(ContextInvocationDepth, ContextInvocationDepth + 1);
	// Pin the state for the complete call. Switching states preserves timer queues,
	// lifecycle flags and observations without copying any VM memory or heap data.
	const auto InstanceState = Context.InstanceExecutionState;
	FAvidScriptWasmInstanceExecutionState* NextState = InstanceState ? InstanceState.Get() : &DefaultInstanceState;
	TGuardValue<FAvidScriptWasmInstanceExecutionState*> InstanceGuard(ActiveInstanceState, NextState);
	TGuardValue<uint32> InstanceCallsGuard(NextState->ActiveCalls, NextState->ActiveCalls + 1);
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
	else if (!ValidateInvocationContext(Context, true))
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
	if (!ValidateInvocationContext(HostContext, true))
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
		if (Context.InstanceExecutionState && GetLifecycleState() != EAvidScriptLifecycleState::Running)
		{
			Failure.Category = TEXT("context_instance_not_running");
			Failure.Details = TEXT("continuation requires a running instance");
			return false;
		}
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
	if (!bSucceeded) RecordContextualFailure(Context, Prepared.ExportName, Error, OutResult);
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
		if (Context.InstanceExecutionState && GetLifecycleState() != EAvidScriptLifecycleState::Running)
		{
			Failure.Category = TEXT("context_instance_not_running");
			Failure.Details = TEXT("delegate callback requires a running instance");
			return false;
		}
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
	if (!bSucceeded) RecordContextualFailure(Context, Event.ExportName, Error, OutResult);
	return bSucceeded;
}

bool FAvidScriptWasmRuntimeInstance::ValidateInstanceExecutionState(const FAvidScriptWasmHostContext& Context) const
{
	const auto& State = Context.InstanceExecutionState;
	return !State || (!State->bRetired && State->CodeIdentity.IsValid()
		&& State->CodeIdentity == ContextCallCodeIdentity
		&& State->ObjectRegistry == Context.ObjectRegistry && State->OwnerHandle == Context.OwnerHandle
		&& State->World == Context.World);
}

bool FAvidScriptWasmRuntimeInstance::CreateInstanceExecutionState(
	const FAvidScriptWasmHostContext& Context, TSharedPtr<FAvidScriptWasmInstanceExecutionState>& OutState, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || !IsLoaded() || ContextInvocationDepth != 0 || ManagedHeapInvocationDepth != 0
		|| OutState || Context.InstanceExecutionState || !ValidateInvocationContext(Context)
		|| Context.ObjectRegistry != HostContext.ObjectRegistry || Context.World != HostContext.World)
	{
		OutError = TEXT("instance execution state requires an idle loaded Runtime and a fresh owner context in its World/registry");
		return false;
	}
	FAvidScriptContextualExportCall Begin;
	if (!PrepareContextualExportCall(TEXT("avid_on_begin_play"), Begin, OutError)) return false;
	InstanceExecutionStates.RemoveAll([](const auto& Entry) { return !Entry.IsValid(); });
	for (const auto& Entry : InstanceExecutionStates)
	{
		const auto Existing = Entry.Pin();
		if (Existing && !Existing->bRetired && Existing->OwnerHandle == Context.OwnerHandle
			&& Existing->ObjectRegistry == Context.ObjectRegistry)
		{
			OutError = TEXT("the owner already has an instance execution state in this code generation");
			return false;
		}
	}
	TSharedPtr<FAvidScriptWasmInstanceExecutionState> State = MakeShareable(new FAvidScriptWasmInstanceExecutionState());
	State->CodeIdentity = ContextCallCodeIdentity;
	State->LifecycleScopeCall = MoveTemp(Begin);
	State->ObjectRegistry = Context.ObjectRegistry;
	State->OwnerHandle = Context.OwnerHandle;
	State->World = Context.World;
	FAvidScriptLifecycleTransitionResult Transition;
	if (!State->LifecycleState.TryTransition(EAvidScriptLifecycleState::Loaded, Transition)) return false;
	OutState = MoveTemp(State);
	InstanceExecutionStates.Add(OutState);
	return true;
}

void FAvidScriptWasmRuntimeInstance::RecordContextualFailure(const FAvidScriptWasmHostContext& Context,
	const FString& ExportName, const FAvidScriptVmError& Error, FAvidScriptWasmSmokeResult& OutResult)
{
	if (!IsInGameThread())
	{
		OutResult = {}; OutResult.ErrorCategory = TEXT("context_invocation_thread");
		return;
	}
	if (!Context.InstanceExecutionState)
	{
		RecordPreparedVmFailure(ExportName, Error, OutResult);
		return;
	}
	// A rejected foreign/stale state must never be mutated through this Runtime.
	if (ValidateInstanceExecutionState(Context))
	{
		TGuardValue<FAvidScriptWasmInstanceExecutionState*> Guard(ActiveInstanceState, Context.InstanceExecutionState.Get());
		RecordPreparedVmFailure(ExportName, Error, OutResult);
		return;
	}
	OutResult = {};
	OutResult.ModuleId = ModuleId;
	OutResult.ExportName = ExportName;
	OutResult.ErrorCategory = Error.Category;
	OutResult.ErrorMessage = Error.Details;
}

bool FAvidScriptWasmRuntimeInstance::InvokeInstanceLifecycle(const FAvidScriptWasmHostContext& Context,
	const EInstanceLifecycleOperation Operation, const float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult,
	const EAvidScriptWasmResultDetail ResultDetail)
{
	if (ResultDetail != EAvidScriptWasmResultDetail::HotFailureOnly) OutResult = {};
	if (!IsInGameThread() || !Context.InstanceExecutionState || !ValidateInstanceExecutionState(Context)
		|| Context.InstanceExecutionState->ActiveCalls != 0)
	{
		OutResult = {};
		OutResult.ErrorCategory = TEXT("context_instance_lifecycle");
		OutResult.ErrorMessage = TEXT("instance lifecycle requires an idle state belonging to this owner and code generation");
		if (IsInGameThread() && ContextInvocationDepth != 0)
		{
			FAvidScriptVmError Failure;
			Failure.Category = OutResult.ErrorCategory; Failure.Details = OutResult.ErrorMessage;
			LatchContextInvocationFailure(Failure);
		}
		return false;
	}
	FAvidScriptVmError Error;
	bool bEntered = false;
	bool bOperationSucceeded = false;
	const bool bSucceeded = InvokeContextOperation(Context.InstanceExecutionState->LifecycleScopeCall, Context,
		[&](FAvidScriptVmError& Failure)
		{
			bEntered = true;
			bool bCalled = false;
			switch (Operation)
			{
			case EInstanceLifecycleOperation::Begin: bCalled = BeginPlayInternal(OutResult); break;
			case EInstanceLifecycleOperation::Tick: bCalled = TickInternal(DeltaSeconds, OutResult, ResultDetail); break;
			case EInstanceLifecycleOperation::End:
				bCalled = EndPlayInternal(OutResult);
				GetInstanceState().ActiveTimers.Reset(); GetInstanceState().TimerHeap.Reset(); GetInstanceState().DueTimerScratch.Reset();
				GetInstanceState().StaleTimerHeapEntryCount = 0;
				break;
			}
			if (!bCalled) { Failure.Category = OutResult.ErrorCategory; Failure.Details = OutResult.ErrorMessage; }
			bOperationSucceeded = bCalled;
			return bCalled;
		}, Error);
	if (!bSucceeded)
	{
		const TCHAR* ExportName = Operation == EInstanceLifecycleOperation::Begin ? TEXT("avid_on_begin_play")
			: Operation == EInstanceLifecycleOperation::Tick ? TEXT("avid_on_tick") : TEXT("avid_on_end_play");
		if (!bEntered || bOperationSucceeded) RecordContextualFailure(Context, ExportName, Error, OutResult);
		if (Operation == EInstanceLifecycleOperation::End && ValidateInstanceExecutionState(Context))
		{
			Context.InstanceExecutionState->bEndPlaySucceeded = false;
			Context.InstanceExecutionState->CachedEndPlayResult = OutResult;
		}
	}
	return bSucceeded;
}

bool FAvidScriptWasmRuntimeInstance::BeginPlayInContext(const FAvidScriptWasmHostContext& Context, FAvidScriptWasmSmokeResult& OutResult)
{
	return InvokeInstanceLifecycle(Context, EInstanceLifecycleOperation::Begin, 0.0f, OutResult);
}

bool FAvidScriptWasmRuntimeInstance::TickInContext(const FAvidScriptWasmHostContext& Context, float DeltaSeconds,
	FAvidScriptWasmSmokeResult& OutResult, EAvidScriptWasmResultDetail ResultDetail)
{
	return InvokeInstanceLifecycle(Context, EInstanceLifecycleOperation::Tick, DeltaSeconds, OutResult, ResultDetail);
}

bool FAvidScriptWasmRuntimeInstance::EndPlayInContext(const FAvidScriptWasmHostContext& Context, FAvidScriptWasmSmokeResult& OutResult)
{
	return InvokeInstanceLifecycle(Context, EInstanceLifecycleOperation::End, 0.0f, OutResult);
}

bool FAvidScriptWasmRuntimeInstance::RetireInstanceExecutionState(
	const TSharedPtr<FAvidScriptWasmInstanceExecutionState>& State, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || !State || State->CodeIdentity != ContextCallCodeIdentity || State->ActiveCalls != 0)
	{
		OutError = TEXT("instance retirement requires its owning Runtime and zero active calls");
		return false;
	}
	State->bRetired = true;
	State->ActiveTimers.Reset(); State->TimerHeap.Reset(); State->DueTimerScratch.Reset();
	State->StaleTimerHeapEntryCount = 0;
	return true;
}

bool FAvidScriptWasmRuntimeInstance::InvokeInstanceCallback(const FAvidScriptWasmHostContext& Context,
	const FString& ExportName, TFunctionRef<bool()> Operation, FAvidScriptWasmSmokeResult& OutResult, bool bResumeSuspended)
{
	if (!IsInGameThread() || !Context.InstanceExecutionState)
	{
		OutResult = {}; OutResult.ErrorCategory = TEXT("context_instance_callback");
		OutResult.ErrorMessage = TEXT("instance callback requires the Game Thread and an explicit state");
		return false;
	}
	FAvidScriptVmError Error;
	bool bEntered = false, bOperationSucceeded = false;
	const bool bSucceeded = InvokeContextOperation(Context.InstanceExecutionState->LifecycleScopeCall, Context,
		[&](FAvidScriptVmError& Failure)
		{
			if (GetLifecycleState() != EAvidScriptLifecycleState::Running)
			{
				Failure.Category = TEXT("context_instance_not_running");
				Failure.Details = TEXT("instance callback requires a running owner");
				return false;
			}
			bEntered = true;
			bOperationSucceeded = Operation();
			if (!bOperationSucceeded) { Failure.Category = OutResult.ErrorCategory; Failure.Details = OutResult.ErrorMessage; }
			return bOperationSucceeded;
		}, Error, bResumeSuspended);
	if (!bSucceeded && (!bEntered || bOperationSucceeded)) RecordContextualFailure(Context, ExportName, Error, OutResult);
	return bSucceeded;
}

bool FAvidScriptWasmRuntimeInstance::DispatchEventInContext(const FAvidScriptWasmHostContext& Context,
	int32 EventId, float Value, FAvidScriptWasmSmokeResult& OutResult, EAvidScriptWasmResultDetail Detail)
{
	return InvokeInstanceCallback(Context, TEXT("avid_on_event"),
		[&] { return DispatchEventInternal(EventId, Value, OutResult, Detail); }, OutResult);
}

bool FAvidScriptWasmRuntimeInstance::DispatchGameplayEventInContext(const FAvidScriptWasmHostContext& Context,
	const FAvidScriptGameplayEvent& Event, FAvidScriptWasmSmokeResult& OutResult, EAvidScriptWasmResultDetail Detail)
{
	return InvokeInstanceCallback(Context, TEXT("avid_on_gameplay_event"),
		[&] { return DispatchGameplayEventInternal(Event, OutResult, Detail); }, OutResult);
}

bool FAvidScriptWasmRuntimeInstance::DispatchDebugResumeInContext(const FAvidScriptWasmHostContext& Context,
	int64 Token, uint32 Route, FAvidScriptWasmSmokeResult& OutResult)
{
	return InvokeInstanceCallback(Context, TEXT("avid_on_debug_resume"),
		[&] { return DispatchDebugResumeInternal(Token, Route, OutResult); }, OutResult, true);
}

bool FAvidScriptWasmRuntimeInstance::CaptureSnapshotInContext(const FAvidScriptWasmHostContext& Context, FAvidScriptWasmSmokeResult& OutResult)
{
	if (!IsInGameThread() || !Context.InstanceExecutionState || !ValidateInstanceExecutionState(Context))
	{
		OutResult = {}; OutResult.ErrorCategory = TEXT("context_instance_snapshot");
		return false;
	}
	TGuardValue<FAvidScriptWasmInstanceExecutionState*> Guard(ActiveInstanceState, Context.InstanceExecutionState.Get());
	CaptureSnapshot(OutResult);
	OutResult.BindingInstrumentation = Context.BindingInvocationInstrumentation
		? *Context.BindingInvocationInstrumentation : FAvidScriptBindingInvocationInstrumentation();
	return true;
}

FAvidScriptWasmHotSnapshot FAvidScriptWasmRuntimeInstance::GetHotSnapshotInContext(const FAvidScriptWasmHostContext& Context)
{
	if (!IsInGameThread() || !Context.InstanceExecutionState || !ValidateInstanceExecutionState(Context)) return {};
	TGuardValue<FAvidScriptWasmInstanceExecutionState*> Guard(ActiveInstanceState, Context.InstanceExecutionState.Get());
	return GetHotSnapshot();
}
