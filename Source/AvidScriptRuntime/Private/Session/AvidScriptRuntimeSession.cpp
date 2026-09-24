#include "AvidScriptRuntimeSession.h"
#include "Session/AvidScriptRuntimeExecutionDomain.h"

#include "AvidScriptRuntimeArtifact.h"

#include "AvidScriptRuntimeEventRouter.h"
#include "AvidScriptRuntimeScheduler.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Debugging/AvidScriptSessionDebugger.h"
#include "Diagnostics/AvidScriptWasmDebugMap.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "HostEffects/AvidScriptHostEffectTransaction.h"
#include "Lifecycle/AvidScriptRuntimeLifecycleCoordinator.h"
#include "Misc/ScopeExit.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
#include "Session/AvidScriptSessionDelegateSubscriptions.h"
#include "Session/AvidScriptSessionInboundHandlers.h"
#include "ScriptTypes/AvidScriptGeneratedTypeSessionPrivate.h"
#include "StateMigration/AvidScriptRuntimeStateMigration.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "Validation/AvidScriptWasmImportPolicy.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptRuntimeSession, Log, All);

struct FAvidScriptPreparedRuntimeActivation
{
	TSharedPtr<FAvidScriptWasmRuntimeInstance> Runtime;
	TSharedPtr<FAvidScriptRuntimeExecutionDomain> Domain;
	FAvidScriptWasmHostContext Context;
	FAvidScriptWasmReloadManifest Manifest;
	FAvidScriptWasmSmokeResult BeginPlayResult;
	TArray<FAvidScriptGeneratedPreparedTypeRoute> GeneratedRoutes;
	FAvidScriptContextualExportCall ContinuationCall;
	TMap<FString, FAvidScriptContextualExportCall> DelegateCalls;
	TOptional<FAvidScriptHostEffectTransaction> HostEffectTransaction;
	int32 BorrowedHandleCheckpoint = 0;
};

FAvidScriptRuntimeExecutionDomain::FAvidScriptRuntimeExecutionDomain(
	TSharedPtr<FAvidScriptWasmRuntimeInstance> InRuntime, const FAvidScriptWasmHostContext& Context,
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> InTypes)
	: Runtime(MoveTemp(InRuntime)), World(Context.World), Registry(Context.ObjectRegistry), Types(MoveTemp(InTypes))
{
	check(IsInGameThread() && Runtime && Registry && Types);
}

FAvidScriptRuntimeExecutionDomain::~FAvidScriptRuntimeExecutionDomain()
{
	check(IsInGameThread() && Members.IsEmpty());
	Runtime->Unload();
}

bool FAvidScriptRuntimeExecutionDomain::Matches(const FAvidScriptWasmHostContext& Context,
	const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& InTypes) const
{
	return !bFaulted && Runtime->IsLoaded() && !World.IsStale()
		&& World == Context.World && Registry == Context.ObjectRegistry && Types == InTypes;
}

bool FAvidScriptRuntimeExecutionDomain::HasActiveCalls() const
{
	return Runtime->IsContextInvocationActive();
}

void FAvidScriptRuntimeExecutionDomain::Attach(FAvidScriptRuntimeSession& Session)
{
	check(IsInGameThread() && !bFaulted && !Members.Contains(&Session));
	check(Session.HostContext.OwnerHandle.IsValid() && !MembersByHandle.Contains(Session.HostContext.OwnerHandle.ToUInt64()));
	Members.Add(&Session);
	MembersByHandle.Add(Session.HostContext.OwnerHandle.ToUInt64(), &Session);
}

void FAvidScriptRuntimeExecutionDomain::Detach(FAvidScriptRuntimeSession& Session)
{
	check(IsInGameThread() && !HasActiveCalls());
	const int32 Removed = Members.RemoveSingle(&Session);
	check(Removed == 1);
	const int32 RemovedHandle = MembersByHandle.Remove(Session.HostContext.OwnerHandle.ToUInt64());
	check(RemovedHandle == 1);
	if (Members.IsEmpty()) Runtime->Unload();
}

FAvidScriptRuntimeSession* FAvidScriptRuntimeExecutionDomain::ResolveInvocationTarget(
	const FAvidScriptRuntimeSession& Source, const FAvidScriptObjectHandle& TargetHandle, FAvidScriptVmError& OutError) const
{
	OutError.Reset();
	const auto Reject = [&](const TCHAR* Category, const TCHAR* Details) -> FAvidScriptRuntimeSession*
	{
		OutError.Category = Category; OutError.Details = Details; return nullptr;
	};
	if (!IsInGameThread() || bFaulted || !HasActiveCalls() || Source.LiveDomain.Get() != this
		|| MembersByHandle.FindRef(Source.HostContext.OwnerHandle.ToUInt64()) != &Source)
		return Reject(TEXT("generated_invocation_domain"), TEXT("source is not active in this published execution domain"));
	FAvidScriptRuntimeSession* Target = MembersByHandle.FindRef(TargetHandle.ToUInt64());
	if (!Target || Target->LiveDomain.Get() != this || Target->LiveRuntime != Runtime)
		return Reject(TEXT("generated_invocation_target"), TEXT("target handle is not a live member of the source execution domain"));
	const auto CanInvoke = [](const FAvidScriptRuntimeSession& Session)
	{
		return !Session.bMutationInProgress && !Session.bPackageReloadBarrier && !Session.PreparedActivation
			&& !Session.bApplicationSuspended && !Session.bLifecycleInvalidated && !Session.bFaultQuarantined
			&& !Session.IsDebugExecutionSuspended() && Session.GeneratedTypeInstance
			&& Session.GeneratedTypeInstance->Registration.IsValid()
			&& Session.GetLiveLifecycleState() == EAvidScriptLifecycleState::Running;
	};
	if (!CanInvoke(Source) || !CanInvoke(*Target))
		return Reject(TEXT("generated_invocation_state"), TEXT("source or target is mutating, suspended, retired or faulted"));
	return Target;
}

bool FAvidScriptRuntimeExecutionDomain::ResolveTypeOrdinal(const FAvidScriptRuntimeSession& Source,
	const FAvidScriptObjectHandle& TargetHandle, uint32& OutOrdinal, FAvidScriptVmError& OutError) const
{
	const auto* Target = ResolveInvocationTarget(Source, TargetHandle, OutError);
	if (!Target) return false;
	OutOrdinal = Target->GeneratedTypeInstance->TypeOrdinal;
	return true;
}

bool FAvidScriptRuntimeExecutionDomain::Invoke(FAvidScriptRuntimeSession& Source,
	const FAvidScriptObjectHandle& TargetHandle, const FAvidScriptContextualExportCall& Call,
	const FAvidScriptVmCallFrame& Frame, FAvidScriptVmError& OutError, FAvidScriptVmCallResult* OutResult)
{
	if (OutResult) *OutResult = {};
	FAvidScriptRuntimeSession* Target = ResolveInvocationTarget(Source, TargetHandle, OutError);
	if (!Target) return false;
	bool bCalled;
	{
		TGuardValue<int32> CallGuard(Target->ActiveGuestCallDepth, Target->ActiveGuestCallDepth + 1);
		bCalled = Runtime->InvokeInContext(Call, Target->HostContext, Frame, OutError, OutResult);
	}
	if (!bCalled)
	{
		FAvidScriptWasmSmokeResult Failure;
		Runtime->RecordContextualFailure(Target->HostContext, Call.GetExportName(), OutError, Failure);
		Poison(Failure); // Runtime still owns an outer invocation; cleanup waits for it.
	}
	return bCalled;
}

void FAvidScriptRuntimeExecutionDomain::Poison(const FAvidScriptWasmSmokeResult& Failure)
{
	check(IsInGameThread());
	if (!bFaulted)
	{
		bFaulted = true;
		RootFailure = Failure;
		for (auto* Session : Members) Session->RecordDomainFault(RootFailure);
	}
	DrainFault();
}

void FAvidScriptRuntimeExecutionDomain::DrainFault()
{
	if (!bFaulted || bDraining) return;
	for (const auto* Session : Members)
		if (Session->IsOperationActive()) return;
	TGuardValue<bool> DrainGuard(bDraining, true);
	const auto Snapshot = Members;
	for (auto* Session : Snapshot)
	{
		const int32 PreviousFaultCount = Session->FaultCount;
		FAvidScriptWasmSmokeResult Stopped;
		Session->StopAndUnload(Stopped); // Quarantine suppresses Guest EndPlay.
		Session->RecordDomainFault(RootFailure);
		Session->FaultCount = PreviousFaultCount;
	}
}

void FAvidScriptRuntimeSession::RecordDomainFault(const FAvidScriptWasmSmokeResult& Failure)
{
	if (!bFaultQuarantined) ++FaultCount;
	bFaultQuarantined = true;
	FaultedModuleId = Failure.ModuleId;
	FaultCategory = Failure.ErrorCategory.IsEmpty() ? TEXT("execution_domain_faulted") : Failure.ErrorCategory;
	FaultExportName = Failure.ExportName;
	FaultDiagnostic = Failure.ErrorMessage.Left(4096);
}

bool FAvidScriptRuntimeSession::IsOperationActive() const
{
	return bMutationInProgress || ActiveGuestCallDepth > 0 || bPackageReloadBarrier || PreparedActivation.IsValid()
		|| (LiveDomain && LiveDomain->HasActiveCalls());
}

void FAvidScriptRuntimeSession::ReleaseLiveRuntime(FAvidScriptWasmSmokeResult& OutResult)
{
	if (!LiveRuntime) return;
	if (LiveDomain)
	{
		FString RetireError;
		if (HostContext.InstanceExecutionState && !HostContext.InstanceExecutionState->IsRetired())
		{
			const bool bRetired = LiveRuntime->RetireInstanceExecutionState(HostContext.InstanceExecutionState, RetireError);
			checkf(bRetired, TEXT("idle execution-domain instance could not retire: %s"), *RetireError);
		}
		LiveDomain->Detach(*this);
		LiveDomain.Reset();
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.bUnloaded = true; // This Session has released its VM lease.
	}
	else LiveRuntime->Unload(OutResult);
	LiveRuntime.Reset();
}

namespace
{
void ResetReloadResult(
	FAvidScriptWasmReloadResult& OutResult,
	const FString& PreviousModuleId,
	const FString& CandidateModuleId,
	const FString& ActiveModuleId)
{
	OutResult = FAvidScriptWasmReloadResult();
	OutResult.PreviousModuleId = PreviousModuleId;
	OutResult.CandidateModuleId = CandidateModuleId;
	OutResult.ActiveModuleId = ActiveModuleId;
}

void CopyRuntimeFailure(
	const FAvidScriptWasmSmokeResult& RuntimeResult,
	FAvidScriptWasmReloadResult& OutResult)
{
	OutResult.RuntimeResult = RuntimeResult;
	OutResult.ExportName = RuntimeResult.ExportName;
	OutResult.ErrorCategory = RuntimeResult.ErrorCategory;
	OutResult.NextAction = RuntimeResult.NextAction;
	OutResult.ErrorMessage = RuntimeResult.ErrorMessage;
}

void SetReloadFailure(
	FAvidScriptWasmReloadResult& OutResult,
	const FString& ExportName,
	const FString& Category,
	const FString& Details,
	const FString& NextAction)
{
	OutResult.ExportName = ExportName;
	OutResult.ErrorCategory = Category;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript reload rejected | previous=%s | candidate=%s | active=%s | export=%s | category=%s | details=%s | next=%s"),
		OutResult.PreviousModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.PreviousModuleId,
		OutResult.CandidateModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.CandidateModuleId,
		OutResult.ActiveModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.ActiveModuleId,
		ExportName.IsEmpty() ? TEXT("<none>") : *ExportName,
		*Category,
		*Details,
		*NextAction);

	UE_LOG(LogAvidScriptRuntimeSession, Warning, TEXT("%s"), *OutResult.ErrorMessage);
}

void MarkRejectedReloadWithRollback(
	const FString& ActiveModuleId,
	FAvidScriptWasmReloadResult& OutResult)
{
	OutResult.ActiveModuleId = ActiveModuleId;
	OutResult.bRollbackPreservedLiveRuntime = !ActiveModuleId.IsEmpty();
}

void CopyHostEffectResult(
	const FAvidScriptHostEffectTransactionResult& HostEffectResult,
	FAvidScriptWasmReloadResult& OutResult)
{
	OutResult.HostEffectCapturedObjectCount = HostEffectResult.CapturedObjectCount;
	OutResult.HostEffectRestoredObjectCount = HostEffectResult.RestoredObjectCount;
	OutResult.HostEffectFailedObjectCount = HostEffectResult.FailedObjectCount;
	OutResult.HostEffectErrorSource = HostEffectResult.ErrorSource;
}

void SetSessionExecutionFailure(
	const FString& ModuleId,
	const FString& ExportName,
	const FString& Details,
	FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = ModuleId;
	OutResult.ExportName = ExportName;
	OutResult.ErrorCategory = TEXT("reentrant_operation");
	OutResult.NextAction = TEXT("defer the requested operation until the active guest call returns");
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript runtime operation rejected | module=%s | export=%s | category=reentrant_operation | details=%s"),
		ModuleId.IsEmpty() ? TEXT("<none>") : *ModuleId,
		ExportName.IsEmpty() ? TEXT("<none>") : *ExportName,
		*Details);
}

void SetSessionDebugSuspendedFailure(
	const FString& ModuleId,
	const FString& ExportName,
	FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = ModuleId;
	OutResult.ExportName = ExportName;
	OutResult.ErrorCategory = TEXT("debug_execution_suspended");
	OutResult.NextAction = TEXT("continue, step, reload, or unload the paused Runtime Session before dispatching another guest entry");
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript runtime operation rejected | module=%s | export=%s | category=debug_execution_suspended | details=the Session owns a suspended guest frame"),
		ModuleId.IsEmpty() ? TEXT("<none>") : *ModuleId,
		ExportName.IsEmpty() ? TEXT("<none>") : *ExportName);
}

void SetSessionFaultedFailure(
	const FString& ModuleId,
	const FString& ExportName,
	const FString& FaultCategory,
	FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = ModuleId;
	OutResult.ExportName = ExportName;
	OutResult.ErrorCategory = TEXT("session_faulted");
	OutResult.NextAction = TEXT("load a new validated module or explicitly unload the quarantined Session");
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript guest entry rejected | module=%s | export=%s | category=session_faulted | root=%s | details=the previous guest failure quarantined this Session"),
		ModuleId.IsEmpty() ? TEXT("<none>") : *ModuleId,
		ExportName.IsEmpty() ? TEXT("<none>") : *ExportName,
		FaultCategory.IsEmpty() ? TEXT("unknown") : *FaultCategory);
}

FAvidScriptVmLoadConfig::FExecutionBudget MakeSessionExecutionBudget(
	const FAvidScriptVmBackendSelection& Selection,
	const EAvidScriptVmArtifactTrust ArtifactTrust,
	const bool bCooperativeSafepointProofVerified,
	const bool bSerializedArtifactProcessAuthorized)
{
	FAvidScriptVmLoadConfig::FExecutionBudget Budget;
	Budget.MaxHostCallsPerEntry = 100000;
	if (Selection.BackendKind == EAvidScriptVmBackendKind::Wasmtime)
	{
		const bool bUseVerifiedPackageFastContainment =
#if PLATFORM_WINDOWS
			ArtifactTrust == EAvidScriptVmArtifactTrust::VerifiedPackage
			&& Selection.ExecutionMode == EAvidScriptVmExecutionMode::Aot
			&& Selection.ArtifactFormat ==
				EAvidScriptVmArtifactFormat::WasmtimeSerialized;
#else
			false;
#endif
		const bool bUseCooperativeContainment =
			bCooperativeSafepointProofVerified
			&& Selection.ExecutionMode == EAvidScriptVmExecutionMode::Aot
			&& Selection.ArtifactFormat ==
				EAvidScriptVmArtifactFormat::WasmtimeSerialized
			&& (bUseVerifiedPackageFastContainment
				|| bSerializedArtifactProcessAuthorized);
		const bool bUseFastContainment =
			bUseVerifiedPackageFastContainment
			|| bUseCooperativeContainment;
		Budget.MaxHostCallsPerEntry = bUseFastContainment
			? 0
			: 100000;
		Budget.FuelPerEntry = bUseFastContainment
			? 0
			: 50000000;
		Budget.EpochDeadlineTicks = bUseCooperativeContainment ? 0 : 1;
		Budget.EpochTimeoutMilliseconds =
			bUseCooperativeContainment ? 0 : 100;
		Budget.CooperativeTimeoutMilliseconds =
			bUseCooperativeContainment ? 100 : 0;
		Budget.bEpochInterruptionIsTerminal =
			bUseVerifiedPackageFastContainment
			&& !bUseCooperativeContainment;
		Budget.MaxLinearMemoryBytes = UINT64_C(64) << 20;
	}
	return Budget;
}
} // namespace

FAvidScriptRuntimeSession::FAvidScriptRuntimeSession()
	: ObjectOwnership(MakeUnique<FAvidScriptSessionObjectOwnership>())
	, DelegateSubscriptions(
		MakeUnique<FAvidScriptSessionDelegateSubscriptions>(*this))
	, InboundHandlers(MakeUnique<FAvidScriptSessionInboundHandlers>(*this))
	, Continuations(MakeShared<FAvidScriptSessionContinuations>())
	, Profiler(MakeUnique<FAvidScriptProfilerEventBuffer>())
	, Debugger(MakeUnique<FAvidScriptSessionDebugger>())
	, Scheduler(MakeUnique<FAvidScriptRuntimeScheduler>())
	, EventRouter(MakeUnique<FAvidScriptRuntimeEventRouter>(*Scheduler))
{
	HostContext.DebugProbes = Debugger.Get();
	HostContext.Profiler = Profiler.Get();
	BackendSelection.BackendKind = EAvidScriptVmBackendKind::Wamr;
	BackendSelection.ExecutionMode = EAvidScriptVmExecutionMode::Auto;
	BackendSelection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	BackendSelection.bAllowFallback = true;
	FAvidScriptRuntimeLifecycleCoordinator::Get().RegisterSession(*this);
}

FAvidScriptRuntimeSession::~FAvidScriptRuntimeSession()
{
	check(IsInGameThread());
	if (PreparedActivation && !bMutationInProgress && ActiveGuestCallDepth == 0)
	{
		FAvidScriptWasmReloadResult DiscardResult;
		DiscardPreparedActivation(DiscardResult);
	}
	checkf(!IsOperationActive(), TEXT("AvidScript RuntimeSession cannot be destroyed during an active guest call or mutation."));
	FAvidScriptRuntimeLifecycleCoordinator::Get().UnregisterSession(*this);
	UnloadLive();
	FString ClearError;
	ensureMsgf(
		ClearGeneratedTypeInstance(ClearError),
		TEXT("Generated type instance teardown failed: %s"),
		ClearError.IsEmpty() ? TEXT("unknown") : *ClearError);
}

FAvidScriptVmLoadConfig::FExecutionBudget
FAvidScriptRuntimeSession::ResolveExecutionBudget(
	const FAvidScriptVmBackendSelection& Selection,
	const EAvidScriptVmArtifactTrust ArtifactTrust,
	const bool bCooperativeSafepointProofVerified,
	const bool bSerializedArtifactProcessAuthorized) const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ExecutionBudgetOverrideForTesting.IsSet())
	{
		return ExecutionBudgetOverrideForTesting.GetValue();
	}
#endif
	return MakeSessionExecutionBudget(
		Selection,
		ArtifactTrust,
		bCooperativeSafepointProofVerified,
		bSerializedArtifactProcessAuthorized);
}

void FAvidScriptRuntimeSession::SuspendForApplicationLifecycle(
	const uint64 Generation)
{
	check(IsInGameThread());
	if (bApplicationSuspended || bLifecycleInvalidated)
	{
		return;
	}

	bApplicationSuspended = true;
	ApplicationLifecycleGeneration = Generation;
	SuspendedWorld = HostContext.World;
	SuspendedOwnerHandle = HostContext.OwnerHandle;
	SuspendedRuntimeIdentity = LiveRuntime.Get();
	SuspendedModuleId = LiveManifest.ModuleId;
	DelegateSubscriptions->SetDispatchEnabled(false);
	InboundHandlers->SetDispatchEnabled(false);
}

bool FAvidScriptRuntimeSession::ResumeFromApplicationLifecycle(
	const uint64 Generation)
{
	check(IsInGameThread());
	PrunePendingBorrowedHandles();
	if (!bApplicationSuspended)
	{
		return !bLifecycleInvalidated;
	}
	if (!IsSuspendedContextCurrent(Generation))
	{
		AbortRuntimeForLifecycleInvalidation();
		bLifecycleInvalidated = true;
		++LifecycleInvalidationCount;
		return false;
	}

	bApplicationSuspended = false;
	ResetSuspendedContext();
	if (LiveRuntime
		&& GetLiveLifecycleState() == EAvidScriptLifecycleState::Running)
	{
		DelegateSubscriptions->SetDispatchEnabled(true);
		InboundHandlers->SetDispatchEnabled(true);
	}
	return true;
}

void FAvidScriptRuntimeSession::HandleApplicationLowMemory()
{
	check(IsInGameThread());
	++LowMemoryNotificationCount;
	Profiler->Reset();
}

void FAvidScriptRuntimeSession::HandleGarbageCollectComplete()
{
	check(IsInGameThread());
	bBorrowedHandlePrunePending = true;
	PrunePendingBorrowedHandles();
}

void FAvidScriptRuntimeSession::PrunePendingBorrowedHandles()
{
	if (!bBorrowedHandlePrunePending || IsOperationActive())
	{
		return;
	}
	check(IsInGameThread());
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);
	bBorrowedHandlePrunePending = false;
	if (HostContext.ObjectRegistry != nullptr)
	{
		ObjectOwnership->PruneInvalidBorrowedHandles(*HostContext.ObjectRegistry);
	}
}

bool FAvidScriptRuntimeSession::InvalidateForWorldTeardown(UWorld& World)
{
	check(IsInGameThread());
	if (HostContext.World.Get() != &World)
	{
		return false;
	}

	// UWorld broadcasts cleanup before its subsystems receive Deinitialize.
	// Their generated terminal route owns orderly Session teardown.
	if (GeneratedTypeInstance
		&& GeneratedTypeInstance->Receiver.IsValid()
		&& GeneratedTypeInstance->Receiver->IsA<UWorldSubsystem>())
	{
		return false;
	}

	AbortRuntimeForLifecycleInvalidation();
	bLifecycleInvalidated = true;
	++LifecycleInvalidationCount;
	return true;
}

bool FAvidScriptRuntimeSession::SuppressApplicationLifecycleEntry(
	const FStringView ExportName,
	FAvidScriptWasmSmokeResult& OutResult)
{
	if (!bApplicationSuspended)
	{
		return false;
	}

	++SuppressedLifecycleEntryCount;
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = GetLiveModuleId();
	OutResult.ExportName = FString(ExportName.Len(), ExportName.GetData());
	const FAvidScriptWasmHotSnapshot Snapshot = GetLiveHotSnapshot();
	OutResult.bModuleLoaded = Snapshot.bRuntimeLoaded;
	OutResult.bBeginPlayCalled = Snapshot.bBeginPlayCalled;
	OutResult.bEndPlayCalled = Snapshot.bEndPlayCalled;
	OutResult.TickCallCount = Snapshot.TickCallCount;
	OutResult.TimerCallbackCount = Snapshot.TimerCallbackCount;
	OutResult.LastTimerCallbackId = Snapshot.LastTimerCallbackId;
	OutResult.LastTimerHandle = Snapshot.LastTimerHandle;
	OutResult.EventCallbackCount = Snapshot.EventCallbackCount;
	OutResult.LastEventId = Snapshot.LastEventId;
	OutResult.LastEventValue = Snapshot.LastEventValue;
	OutResult.Metrics = Snapshot.Metrics;
	return true;
}

bool FAvidScriptRuntimeSession::IsSuspendedContextCurrent(
	const uint64 Generation) const
{
	if (Generation == 0
		|| Generation != ApplicationLifecycleGeneration
		|| bLifecycleInvalidated
		|| LiveRuntime.Get() != SuspendedRuntimeIdentity
		|| LiveManifest.ModuleId != SuspendedModuleId
		|| SuspendedWorld.IsStale()
		|| HostContext.World != SuspendedWorld
		|| HostContext.OwnerHandle != SuspendedOwnerHandle)
	{
		return false;
	}

	if (!SuspendedOwnerHandle.IsValid())
	{
		return true;
	}
	if (HostContext.ObjectRegistry == nullptr)
	{
		return false;
	}
	FAvidScriptObjectHandleResult ResolveResult;
	const UObject* Owner = HostContext.ObjectRegistry->ResolveObject(
		SuspendedOwnerHandle,
		ResolveResult,
		false);
	return Owner != nullptr && Owner->GetWorld() == SuspendedWorld.Get();
}

void FAvidScriptRuntimeSession::AbortRuntimeForLifecycleInvalidation()
{
	check(IsInGameThread());
	checkf(
		!IsOperationActive(),
		TEXT("AvidScript lifecycle invalidation cannot interrupt an active Runtime operation."));
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);
	DelegateSubscriptions->SetDispatchEnabled(false);
	DelegateSubscriptions->UnbindActive();
	DelegateSubscriptions->DiscardPrepared();
	InboundHandlers->SetDispatchEnabled(false);
	InboundHandlers->UnbindActive();
	InboundHandlers->DiscardPrepared();
	Continuations->Teardown();
	Scheduler->Detach();
	if (LiveRuntime)
	{
		FAvidScriptWasmSmokeResult IgnoredUnloadResult;
		ReleaseLiveRuntime(IgnoredUnloadResult);
	}
	if (GeneratedTypeInstance)
	{
		GeneratedTypeInstance->PreparedTypeRoutes.Reset();
		GeneratedTypeInstance->ContinuationCall = {};
		GeneratedTypeInstance->DelegateCalls.Reset();
	}
	Continuations->ReleaseRetiredEndpoint();
	if (HostContext.ObjectRegistry != nullptr)
	{
		ObjectOwnership->Cleanup(*HostContext.ObjectRegistry);
	}
	HostContext = FAvidScriptWasmHostContext();
	HostContext.DebugProbes = Debugger.Get();
	HostContext.Profiler = Profiler.Get();
	LiveManifest = FAvidScriptWasmReloadManifest();
	Debugger->OnRuntimeGenerationChanged();
	ClearFaultQuarantine();
	bApplicationSuspended = false;
	bBorrowedHandlePrunePending = false;
	ResetSuspendedContext();
}

void FAvidScriptRuntimeSession::ResetSuspendedContext()
{
	SuspendedWorld.Reset();
	SuspendedOwnerHandle = FAvidScriptObjectHandle();
	SuspendedRuntimeIdentity = nullptr;
	SuspendedModuleId.Reset();
}

bool FAvidScriptRuntimeSession::LoadEmbeddedSmoke(FAvidScriptWasmReloadResult& OutResult)
{
	if (LiveDomain)
	{
		SetReloadFailure(OutResult, TEXT("<domain>"), TEXT("execution_domain_package_required"),
			TEXT("a generated domain cannot be replaced by an embedded module"), TEXT("reload the generated package"));
		return false;
	}
	PrunePendingBorrowedHandles();
	const FString ModuleId = TEXT("embedded_smoke");
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::RuntimeLoad,
		static_cast<uint32>(EAvidScriptProfilerOperation::LoadEmbedded),
		0,
		0,
		GetTypeHash(ModuleId));
	ProfileScope.SetSucceeded(false);
	const FString PreviousModuleId = GetLiveModuleId();
	ResetReloadResult(OutResult, PreviousModuleId, ModuleId, PreviousModuleId);
	if (bApplicationSuspended)
	{
		SetReloadFailure(
			OutResult,
			TEXT("<session>"),
			TEXT("application_suspended"),
			TEXT("embedded module load was requested while the application is suspended"),
			TEXT("resume the application before loading a Runtime module"));
		return false;
	}
	if (IsOperationActive())
	{
		SetReloadFailure(
			OutResult,
			TEXT("<session>"),
			TEXT("reentrant_operation"),
			TEXT("embedded module load was requested while another Runtime operation is active"),
			TEXT("defer the load until the active guest call or Runtime mutation returns"));
		return false;
	}
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);

	const FAvidScriptWasmReloadManifest Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(ModuleId);
	TSharedPtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime =
		MakeShared<FAvidScriptWasmRuntimeInstance>(BackendSelection);
	FAvidScriptWasmSmokeResult RuntimeResult;
	FString BudgetError;
	if (!CandidateRuntime->ConfigureExecutionBudget(
			ResolveExecutionBudget(BackendSelection),
			BudgetError))
	{
		SetReloadFailure(
			OutResult,
			TEXT("<runtime>"),
			TEXT("execution_budget_invalid"),
			BudgetError,
			TEXT("fix the Session execution policy before loading the script"));
		return false;
	}
	if (!CandidateRuntime->LoadEmbeddedSmokeModule(RuntimeResult) ||
		!CandidateRuntime->ValidateRequiredExports(Manifest.RequiredExports, RuntimeResult))
	{
		CopyRuntimeFailure(RuntimeResult, OutResult);
		CandidateRuntime->Unload();
		return false;
	}

	SetRuntimeBaseContext(*CandidateRuntime, HostContext);
	if (!ActivateValidatedRuntime(CandidateRuntime, Manifest, false, OutResult))
	{
		OutResult.ActiveModuleId = GetLiveModuleId();
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ActiveModuleId = ModuleId;
	ProfileScope.SetSucceeded(true);
	return true;
}

bool FAvidScriptRuntimeSession::LoadInitialModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadResult& OutResult)
{
	TConstArrayView<uint8> BytecodeView;
	if (Bytecode != nullptr && BytecodeSize > 0)
	{
		BytecodeView = MakeArrayView(Bytecode, BytecodeSize);
	}
	const FAvidScriptRuntimeArtifact Artifact =
		FAvidScriptRuntimeArtifact::FromCanonicalWasm(
			Manifest,
			BytecodeView,
			BackendSelection);
	return LoadInitialArtifact(Artifact, OutResult);
}

bool FAvidScriptRuntimeSession::LoadInitialArtifact(
	const FAvidScriptRuntimeArtifact& Artifact,
	FAvidScriptWasmReloadResult& OutResult)
{
	PrunePendingBorrowedHandles();
	const FAvidScriptWasmReloadManifest& Manifest = Artifact.Manifest;
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::RuntimeLoad,
		static_cast<uint32>(EAvidScriptProfilerOperation::LoadInitial),
		0,
		0,
		GetTypeHash(Manifest.ModuleId));
	ProfileScope.SetSucceeded(false);
	const FString PreviousModuleId = GetLiveModuleId();
	ResetReloadResult(OutResult, PreviousModuleId, Manifest.ModuleId, PreviousModuleId);
	if (LiveDomain)
	{
		SetReloadFailure(OutResult, TEXT("<domain>"), TEXT("execution_domain_package_required"),
			TEXT("a generated execution domain must be replaced through its package"), TEXT("reload the generated package"));
		return false;
	}
	if (bApplicationSuspended)
	{
		SetReloadFailure(
			OutResult,
			TEXT("<session>"),
			TEXT("application_suspended"),
			TEXT("initial module load was requested while the application is suspended"),
			TEXT("resume the application before loading a Runtime module"));
		return false;
	}
	if (IsOperationActive())
	{
		SetReloadFailure(
			OutResult,
			TEXT("<session>"),
			TEXT("reentrant_operation"),
			TEXT("initial module load was requested while another Runtime operation is active"),
			TEXT("defer the load until the active guest call or Runtime mutation returns"));
		return false;
	}
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);

	if (!ValidateManifest(Manifest, PreviousModuleId, OutResult))
	{
		return false;
	}

	TSharedPtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime;
	if (!BuildValidatedRuntime(Artifact, CandidateRuntime, OutResult))
	{
		OutResult.ActiveModuleId = PreviousModuleId;
		return false;
	}

	if (!ActivateValidatedRuntime(CandidateRuntime, Manifest, false, OutResult))
	{
		OutResult.ActiveModuleId = GetLiveModuleId();
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ActiveModuleId = Manifest.ModuleId;
	ProfileScope.SetSucceeded(true);
	return true;
}

bool FAvidScriptRuntimeSession::LoadGeneratedDomainArtifact(
	const FAvidScriptRuntimeArtifact& Artifact, const TSharedPtr<FAvidScriptRuntimeExecutionDomain>& ExistingDomain,
	FAvidScriptWasmReloadResult& OutResult)
{
	ResetReloadResult(OutResult, {}, Artifact.Manifest.ModuleId, {});
	if (!GeneratedTypeInstance || LiveRuntime || IsOperationActive() || bApplicationSuspended
		|| (ExistingDomain && (!ExistingDomain->Matches(HostContext, GeneratedTypeInstance->Registry)
			|| ExistingDomain->HasActiveCalls())))
	{
		SetReloadFailure(OutResult, TEXT("<domain>"), TEXT("execution_domain_join_rejected"),
			TEXT("generated owner cannot join this execution domain"), TEXT("use the current package, World and registry"));
		return false;
	}
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);
	if (!ValidateManifest(Artifact.Manifest, {}, OutResult) || !ValidateExpectedOwner(Artifact.Manifest, OutResult)) return false;
	auto Domain = ExistingDomain;
	TSharedPtr<FAvidScriptWasmRuntimeInstance> Runtime = Domain ? Domain->GetRuntime() : nullptr;
	if (!Runtime)
	{
		if (!BuildValidatedRuntime(Artifact, Runtime, OutResult)) return false;
		Domain = MakeShared<FAvidScriptRuntimeExecutionDomain>(Runtime, HostContext, GeneratedTypeInstance->Registry);
	}
	if (!ActivateValidatedRuntime(Runtime, Artifact.Manifest, false, OutResult, false, Domain))
	{
		auto Failure = OutResult.RuntimeResult;
		Failure.ModuleId = Artifact.Manifest.ModuleId;
		Failure.ErrorCategory = OutResult.ErrorCategory;
		Failure.ErrorMessage = OutResult.ErrorMessage;
		Domain->Poison(Failure);
		return false;
	}
	OutResult.bSucceeded = true;
	OutResult.ActiveModuleId = Artifact.Manifest.ModuleId;
	return true;
}

bool FAvidScriptRuntimeSession::ReloadModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadResult& OutResult)
{
	TConstArrayView<uint8> BytecodeView;
	if (Bytecode != nullptr && BytecodeSize > 0)
	{
		BytecodeView = MakeArrayView(Bytecode, BytecodeSize);
	}
	const FAvidScriptRuntimeArtifact Artifact =
		FAvidScriptRuntimeArtifact::FromCanonicalWasm(
			Manifest,
			BytecodeView,
			BackendSelection);
	return ReloadArtifact(Artifact, OutResult);
}

bool FAvidScriptRuntimeSession::ReloadArtifact(
	const FAvidScriptRuntimeArtifact& Artifact,
	FAvidScriptWasmReloadResult& OutResult)
{
	return ReloadArtifactInternal(Artifact, OutResult, false);
}

bool FAvidScriptRuntimeSession::ReloadArtifactInternal(
	const FAvidScriptRuntimeArtifact& Artifact,
	FAvidScriptWasmReloadResult& OutResult, bool bDeferCommit,
	TSharedPtr<FAvidScriptRuntimeExecutionDomain>* SharedCandidate)
{
	PrunePendingBorrowedHandles();
	const FAvidScriptWasmReloadManifest& Manifest = Artifact.Manifest;
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::Reload,
		static_cast<uint32>(EAvidScriptProfilerOperation::Reload),
		Debugger->GetSnapshot().Epoch,
		0,
		GetTypeHash(Manifest.ModuleId));
	ProfileScope.SetSucceeded(false);
	const FString PreviousModuleId = GetLiveModuleId();
	ResetReloadResult(OutResult, PreviousModuleId, Manifest.ModuleId, PreviousModuleId);
	if (LiveDomain && (!bDeferCommit || !SharedCandidate))
	{
		SetReloadFailure(OutResult, TEXT("<domain>"), TEXT("execution_domain_package_required"),
			TEXT("shared code must be reloaded as a complete generated package"), TEXT("reload the generated package"));
		return false;
	}
	if (bApplicationSuspended)
	{
		SetReloadFailure(
			OutResult,
			TEXT("<session>"),
			TEXT("application_suspended"),
			TEXT("reload was requested while the application is suspended"),
			TEXT("resume the application before replacing the Runtime module"));
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}
	if (bMutationInProgress || ActiveGuestCallDepth > 0 || PreparedActivation
		|| (bPackageReloadBarrier && !bDeferCommit)
		|| (bDeferCommit && (!bPackageReloadBarrier || !GeneratedTypeInstance)))
	{
		SetReloadFailure(
			OutResult,
			TEXT("<session>"),
			TEXT("reentrant_operation"),
			TEXT("reload was requested while another Runtime operation is active"),
			TEXT("defer the reload until the active guest call or Runtime mutation returns"));
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);

	if (!ValidateManifest(Manifest, PreviousModuleId, OutResult))
	{
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}

	auto CandidateDomain = SharedCandidate ? *SharedCandidate : TSharedPtr<FAvidScriptRuntimeExecutionDomain>();
	const bool bReuseCandidate = CandidateDomain.IsValid();
	TSharedPtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime = CandidateDomain ? CandidateDomain->GetRuntime() : nullptr;
	if ((CandidateDomain && !CandidateDomain->Matches(HostContext, GeneratedTypeInstance->Registry))
		|| (!CandidateRuntime && !BuildValidatedRuntime(Artifact, CandidateRuntime, OutResult)))
	{
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}

	FAvidScriptRuntimeStateMigrationResult MigrationResult;
	if (!bReuseCandidate && LiveRuntime && !FAvidScriptRuntimeStateMigration::Migrate(
		*LiveRuntime,
		LiveManifest,
		*CandidateRuntime,
		Manifest,
		MigrationResult))
	{
		OutResult.bStateMigrationAttempted = MigrationResult.bAttempted;
		OutResult.StateMigrationStableId = MigrationResult.StableId;
		SetReloadFailure(
			OutResult,
			TEXT("<state_migration>"),
			MigrationResult.ErrorCategory,
			FString::Printf(
				TEXT("stable_id=%s; %s"),
				MigrationResult.StableId.IsEmpty() ? TEXT("<none>") : *MigrationResult.StableId,
				*MigrationResult.ErrorDetails),
			TEXT("keep the previous runtime active and make the changed field transient or provide a compatible type"));
		CandidateRuntime->Unload();
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}

	OutResult.bStateMigrationAttempted = MigrationResult.bAttempted;
	OutResult.bStateMigrationApplied = MigrationResult.bAttempted && MigrationResult.bSucceeded;
	OutResult.StateMigrationMigratedSlotCount = MigrationResult.MigratedSlotCount;
	OutResult.StateMigrationMigratedByteCount = MigrationResult.MigratedByteCount;
	OutResult.StateMigrationSkippedSlotCount = MigrationResult.SkippedSlotCount;
	OutResult.StateMigrationAliasedSlotCount = MigrationResult.AliasedSlotCount;

	if (SharedCandidate && !CandidateDomain)
	{
		CandidateDomain = MakeShared<FAvidScriptRuntimeExecutionDomain>(CandidateRuntime, HostContext, GeneratedTypeInstance->Registry);
		*SharedCandidate = CandidateDomain;
	}
	if (!ActivateValidatedRuntime(CandidateRuntime, Manifest, true, OutResult, bDeferCommit, CandidateDomain))
	{
		++RejectedReloadCount;
		const FString ActiveModuleId = GetLiveModuleId();
		MarkRejectedReloadWithRollback(ActiveModuleId, OutResult);
		return false;
	}
	if (bDeferCommit)
	{
		ProfileScope.SetSucceeded(true);
		return true;
	}

	++SuccessfulReloadCount;
	OutResult.bSucceeded = true;
	OutResult.bReloadApplied = true;
	OutResult.ActiveModuleId = Manifest.ModuleId;
	ProfileScope.SetSucceeded(true);
	return true;
}
void FAvidScriptRuntimeSession::SetRuntimeBaseContext(
	FAvidScriptWasmRuntimeInstance& Runtime, const FAvidScriptWasmHostContext& Context) const
{
	if (GeneratedTypeInstance || Context.InstanceExecutionState)
	{
		// No instance-owned endpoint may survive in a shared module's base context.
		FAvidScriptWasmHostContext Base;
		Base.ObjectRegistry = Context.ObjectRegistry;
		Base.World = Context.World;
		Base.HostEffectJournal = Context.HostEffectJournal;
		Runtime.SetHostContext(Base);
	}
	else Runtime.SetHostContext(Context);
}

void FAvidScriptRuntimeSession::SetHostContext(const FAvidScriptWasmHostContext& InHostContext)
{
	PrunePendingBorrowedHandles();
	if (IsOperationActive())
	{
		UE_LOG(
			LogAvidScriptRuntimeSession,
			Verbose,
			TEXT("AvidScript host context change rejected during an active guest call or mutation."));
		return;
	}
	if (GeneratedTypeInstance
		&& InHostContext.OwnerHandle.IsValid()
		&& InHostContext.OwnerHandle != GeneratedTypeInstance->ReceiverHandle)
	{
		UE_LOG(
			LogAvidScriptRuntimeSession,
			Warning,
			TEXT("AvidScript host context rejected because its owner handle does not match the generated type instance."));
		return;
	}
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);
	FAvidScriptWasmHostContext NextHostContext = InHostContext;
	NextHostContext.InstanceExecutionState = HostContext.InstanceExecutionState;
	NextHostContext.GeneratedTypeAuthority = GeneratedTypeInstance
		? GeneratedTypeInstance->Authority : TWeakPtr<IAvidScriptGeneratedTypeAuthority>();
	NextHostContext.ObjectOwnership = ObjectOwnership.Get();
	NextHostContext.HostEffectJournal = nullptr;
	NextHostContext.EventSubscriptions = DelegateSubscriptions.Get();
	NextHostContext.Continuations = HostContext.Continuations;
	NextHostContext.Tasks = HostContext.Tasks;
	NextHostContext.DebugProbes = Debugger.Get();
	NextHostContext.Profiler = Profiler.Get();
	NextHostContext.LatentHost = HostContext.LatentHost;
	if (NextHostContext.ObjectRegistry != nullptr
		&& NextHostContext.OwnerHandle.IsValid())
	{
		FAvidScriptObjectHandleResult ResolveResult;
		if (const UObject* Owner = NextHostContext.ObjectRegistry->ResolveObject(
				NextHostContext.OwnerHandle,
				ResolveResult,
				false))
		{
			if (UWorld* const OwnerWorld = Owner->GetWorld())
			{
				NextHostContext.World = OwnerWorld;
			}
		}
	}

	const bool bDelegateSourceChanged =
		HostContext.ObjectRegistry != NextHostContext.ObjectRegistry
		|| HostContext.OwnerHandle != NextHostContext.OwnerHandle
		|| HostContext.World != NextHostContext.World;
	if (HostContext.InstanceExecutionState && bDelegateSourceChanged)
	{
		UE_LOG(LogAvidScriptRuntimeSession, Warning,
			TEXT("Generated Session context rebinding requires stopping its owner-bound execution state first."));
		return;
	}
	if (bDelegateSourceChanged)
	{
		DelegateSubscriptions->UnbindActive();
		DelegateSubscriptions->DiscardPrepared();
		InboundHandlers->UnbindActive();
		InboundHandlers->DiscardPrepared();
		if (LiveRuntime)
		{
			FAvidScriptContinuationHostEndpoint& ActiveContinuationHost =
				Continuations->ResetActive(
					NextHostContext.World.Get(),
					NextHostContext.ObjectRegistry,
					ObjectOwnership.Get(),
					NextHostContext.OwnerHandle);
			NextHostContext.Continuations = &ActiveContinuationHost;
			NextHostContext.Tasks = &ActiveContinuationHost;
			NextHostContext.LatentHost = &ActiveContinuationHost;
		}
		else
		{
			NextHostContext.Continuations = nullptr;
			NextHostContext.Tasks = nullptr;
			NextHostContext.LatentHost = nullptr;
		}
		if (!LiveRuntime)
		{
			Continuations->Teardown();
		}
	}
	if (bDelegateSourceChanged && HostContext.ObjectRegistry != nullptr)
	{
		ObjectOwnership->Cleanup(*HostContext.ObjectRegistry);
	}
	if (bDelegateSourceChanged)
	{
		bBorrowedHandlePrunePending = false;
	}
	HostContext = MoveTemp(NextHostContext);
	if (LiveRuntime)
	{
		SetRuntimeBaseContext(*LiveRuntime, HostContext);
		Continuations->ReleaseRetiredEndpoint();
		if (bDelegateSourceChanged
			&& GetLiveLifecycleState()
				== EAvidScriptLifecycleState::Running)
		{
			TArray<FAvidScriptPreparedDelegateEvent> Events;
			TArray<FAvidScriptPreparedDelegateEvent> Handlers;
			FString Error;
			UObject* Source = nullptr;
			if (HostContext.ObjectRegistry != nullptr)
			{
				FAvidScriptObjectHandleResult ResolveResult;
				Source = HostContext.ObjectRegistry->ResolveObject(
					HostContext.OwnerHandle,
					ResolveResult,
					false);
			}
			if (!LiveRuntime->BuildPreparedCallbacks(Events, Handlers, Error)
				|| (!Events.IsEmpty()
					&& !DelegateSubscriptions->Prepare(Source, Events, Error, LiveRuntime.Get()))
				|| (!Handlers.IsEmpty()
					&& !InboundHandlers->Prepare(Source, Handlers, Error)))
			{
				DelegateSubscriptions->DiscardPrepared();
				InboundHandlers->DiscardPrepared();
				UE_LOG(
					LogAvidScriptRuntimeSession,
					Warning,
					TEXT("AvidScript delegate rebind rejected after host context change: %s"),
					Error.IsEmpty() ? TEXT("delegate_source_unavailable") : *Error);
				return;
			}
			DelegateSubscriptions->CommitPrepared();
			DelegateSubscriptions->SetDispatchEnabled(true);
			FString CommitError;
			if (!InboundHandlers->CommitPrepared(CommitError))
			{
				UE_LOG(
					LogAvidScriptRuntimeSession,
					Error,
					TEXT("AvidScript inbound handler rebind commit failed: %s"),
					CommitError.IsEmpty() ? TEXT("unknown") : *CommitError);
				return;
			}
			InboundHandlers->SetDispatchEnabled(true);
		}
	}
}

void FAvidScriptRuntimeSession::ClearHostContext()
{
	if (IsOperationActive() || LiveDomain)
	{
		UE_LOG(
			LogAvidScriptRuntimeSession,
			Verbose,
			TEXT("AvidScript host context clear rejected during an active guest call or mutation."));
		return;
	}
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);
	if (LiveRuntime && HostContext.InstanceExecutionState && !HostContext.InstanceExecutionState->IsRetired())
	{
		FString RetireError;
		if (!LiveRuntime->RetireInstanceExecutionState(HostContext.InstanceExecutionState, RetireError)) return;
	}
	DelegateSubscriptions->UnbindActive();
	DelegateSubscriptions->DiscardPrepared();
	InboundHandlers->UnbindActive();
	InboundHandlers->DiscardPrepared();
	Continuations->Teardown();
	if (HostContext.ObjectRegistry != nullptr)
	{
		ObjectOwnership->Cleanup(*HostContext.ObjectRegistry);
	}
	HostContext = FAvidScriptWasmHostContext();
	HostContext.DebugProbes = Debugger.Get();
	HostContext.Profiler = Profiler.Get();
	bBorrowedHandlePrunePending = false;
	if (LiveRuntime)
	{
		SetRuntimeBaseContext(*LiveRuntime, HostContext);
		Continuations->ReleaseRetiredEndpoint();
	}
}

bool FAvidScriptRuntimeSession::Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult)
{
	if (SuppressApplicationLifecycleEntry(TEXT("avid_on_tick"), OutResult))
	{
		return true;
	}
	if (!CanEnterGuest(TEXT("avid_on_tick"), OutResult))
	{
		return false;
	}
	bool bSucceeded = false;
	bool bDebugExecutionSuspended = false;
	{
		TGuardValue<int32> GuestCallGuard(ActiveGuestCallDepth, ActiveGuestCallDepth + 1);
#if WITH_DEV_AUTOMATION_TESTS
		if (LiveExecutionObserverForTesting)
		{
			TFunction<void()> Observer =
				MoveTemp(LiveExecutionObserverForTesting);
			Observer();
		}
#endif
		bSucceeded = Scheduler->Tick(DeltaSeconds, OutResult);
		if (bSucceeded)
		{
			bDebugExecutionSuspended = IsDebugExecutionSuspended();
			if (!bDebugExecutionSuspended && Continuations->NeedsTickPump())
			{
				bSucceeded = PumpReadyContinuations(OutResult);
				if (bSucceeded)
				{
					bDebugExecutionSuspended = IsDebugExecutionSuspended();
				}
			}
		}
	}
	const bool bCompleted = bSucceeded
		&& (bDebugExecutionSuspended
			|| !InboundHandlers->NeedsDeferredPump()
			|| InboundHandlers->PumpDeferred(OutResult));
	if (!bCompleted)
	{
		QuarantineFaultedRuntime(OutResult);
	}
	return bCompleted;
}

bool FAvidScriptRuntimeSession::DispatchEvent(
	int32 EventId,
	float Value,
	FAvidScriptWasmSmokeResult& OutResult)
{
	return DispatchEventLive(EventId, Value, OutResult);
}

bool FAvidScriptRuntimeSession::DispatchGameplayEvent(
	const FAvidScriptGameplayEvent& Event,
	FAvidScriptWasmSmokeResult& OutResult)
{
	return DispatchGameplayEventLive(Event, OutResult);
}

bool FAvidScriptRuntimeSession::TickLive(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult)
{
	if (SuppressApplicationLifecycleEntry(TEXT("avid_on_tick"), OutResult))
	{
		return true;
	}
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::GuestCall,
		static_cast<uint32>(EAvidScriptProfilerOperation::Tick));
	ProfileScope.SetSucceeded(false);
	if (!CanEnterGuest(TEXT("avid_on_tick"), OutResult))
	{
		return false;
	}
	bool bSucceeded = false;
	bool bDebugExecutionSuspended = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
#if WITH_DEV_AUTOMATION_TESTS
		if (LiveExecutionObserverForTesting)
		{
			TFunction<void()> Observer =
				MoveTemp(LiveExecutionObserverForTesting);
			Observer();
		}
#endif
		bSucceeded = Scheduler->Tick(
			DeltaSeconds,
			OutResult,
			EAvidScriptWasmResultDetail::FailureOnly);
		if (bSucceeded)
		{
			bDebugExecutionSuspended = IsDebugExecutionSuspended();
			if (!bDebugExecutionSuspended && Continuations->NeedsTickPump())
			{
				bSucceeded = PumpReadyContinuations(OutResult);
				if (bSucceeded)
				{
					bDebugExecutionSuspended = IsDebugExecutionSuspended();
				}
			}
		}
	}
	const bool bCompleted = bSucceeded
		&& (bDebugExecutionSuspended
			|| !InboundHandlers->NeedsDeferredPump()
			|| InboundHandlers->PumpDeferred(OutResult));
	if (!bCompleted)
	{
		QuarantineFaultedRuntime(OutResult);
	}
	ProfileScope.SetSucceeded(bCompleted);
	return bCompleted;
}

bool FAvidScriptRuntimeSession::TickHot(
	const float DeltaSeconds,
	FAvidScriptWasmSmokeResult& OutFailure)
{
	if (SuppressApplicationLifecycleEntry(TEXT("avid_on_tick"), OutFailure))
	{
		return true;
	}
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::GuestCall,
		static_cast<uint32>(EAvidScriptProfilerOperation::Tick));
	ProfileScope.SetSucceeded(false);
	if (!CanEnterGuest(TEXT("avid_on_tick"), OutFailure))
	{
		return false;
	}
	bool bSucceeded = false;
	bool bDebugExecutionSuspended = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
#if WITH_DEV_AUTOMATION_TESTS
		if (LiveExecutionObserverForTesting)
		{
			TFunction<void()> Observer =
				MoveTemp(LiveExecutionObserverForTesting);
			Observer();
		}
#endif
		bSucceeded = Scheduler->TickHot(DeltaSeconds, OutFailure);
		if (bSucceeded)
		{
			bDebugExecutionSuspended = IsDebugExecutionSuspended();
			if (!bDebugExecutionSuspended && Continuations->NeedsTickPump())
			{
				bSucceeded = PumpReadyContinuations(OutFailure);
				if (bSucceeded)
				{
					bDebugExecutionSuspended = IsDebugExecutionSuspended();
				}
			}
		}
	}
	const bool bCompleted = bSucceeded
		&& (bDebugExecutionSuspended
			|| !InboundHandlers->NeedsDeferredPump()
			|| InboundHandlers->PumpDeferred(OutFailure));
	if (!bCompleted)
	{
		QuarantineFaultedRuntime(OutFailure);
	}
	ProfileScope.SetSucceeded(bCompleted);
	return bCompleted;
}

bool FAvidScriptRuntimeSession::PumpReadyContinuations(
	FAvidScriptWasmSmokeResult& OutResult)
{
	TArray<FAvidScriptContinuationCompletion> Completions;
	Continuations->DrainReady(Completions);
	for (const FAvidScriptContinuationCompletion& Completion : Completions)
	{
		FAvidScriptProfilerScope ProfileScope(
			Profiler.Get(),
			EAvidScriptProfilerEventKind::Continuation,
			static_cast<uint32>(EAvidScriptProfilerOperation::ContinuationDispatch),
			0,
			0,
			static_cast<uint64>(Completion.Token));
		ProfileScope.SetSucceeded(false);
		const bool bDispatched = LiveRuntime
			&& (GeneratedTypeInstance
				? LiveRuntime->DispatchContinuationInContext(GeneratedTypeInstance->ContinuationCall, HostContext, Completion, OutResult)
				: LiveRuntime->DispatchContinuation(Completion, OutResult));
		const bool bFinalized = Continuations->FinalizeDispatched(
			Completion.Token,
			bDispatched);
		if (!bDispatched || !bFinalized)
		{
			Continuations->Teardown();
			HostContext.Continuations = nullptr;
			HostContext.Tasks = nullptr;
			HostContext.LatentHost = nullptr;
			if (LiveRuntime)
			{
				SetRuntimeBaseContext(*LiveRuntime, HostContext);
			}
			Continuations->ReleaseRetiredEndpoint();
			return false;
		}
		ProfileScope.SetSucceeded(true);
	}
	return true;
}

bool FAvidScriptRuntimeSession::CanEnterGuest(
	const FStringView ExportName,
	FAvidScriptWasmSmokeResult& OutResult,
	const bool bAllowNestedDelegate)
{
	PrunePendingBorrowedHandles();
	if (LiveDomain && LiveDomain->IsFaulted())
	{
		auto Domain = LiveDomain;
		Domain->DrainFault();
	}
	if (LiveRuntime && ((GeneratedTypeInstance && !HostContext.InstanceExecutionState)
		|| (HostContext.InstanceExecutionState && HostContext.InstanceExecutionState->IsRetired())))
	{
		SetSessionExecutionFailure(GetLiveModuleId(), FString(ExportName.Len(), ExportName.GetData()),
			TEXT("generated instance execution state is missing or retired"), OutResult);
		return false;
	}
	if (bApplicationSuspended || bLifecycleInvalidated)
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.ModuleId = GetLiveModuleId();
		OutResult.ExportName = FString(ExportName.Len(), ExportName.GetData());
		OutResult.ErrorCategory = bApplicationSuspended
			? TEXT("application_suspended")
			: TEXT("lifecycle_invalidated");
		OutResult.NextAction = bApplicationSuspended
			? TEXT("resume the application before entering guest code")
			: TEXT("bind a live World and load a new validated module");
		OutResult.ErrorMessage = FString::Printf(
			TEXT("AvidScript guest entry rejected | module=%s | export=%s | category=%s"),
			OutResult.ModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.ModuleId,
			OutResult.ExportName.IsEmpty() ? TEXT("<none>") : *OutResult.ExportName,
			*OutResult.ErrorCategory);
		return false;
	}
	if (bFaultQuarantined)
	{
		++FaultedEntryRejectCount;
		++SuppressedFaultDiagnosticCount;
		SetSessionFaultedFailure(
			FaultedModuleId,
			FString(ExportName.Len(), ExportName.GetData()),
			FaultCategory,
			OutResult);
		return false;
	}
	const bool bSafeNestedDelegate = bAllowNestedDelegate
		&& ActiveGuestCallDepth > 0 && ActiveGuestCallDepth < 64
		&& !bMutationInProgress && !bPackageReloadBarrier && !PreparedActivation
		&& LiveRuntime && (!LiveDomain || LiveDomain->HasActiveCalls());
	if (IsOperationActive() && !bSafeNestedDelegate)
	{
		SetSessionExecutionFailure(
			GetLiveModuleId(),
			FString(ExportName.Len(), ExportName.GetData()),
			TEXT("guest entry was requested while another guest call or Runtime mutation is active"),
			OutResult);
		return false;
	}
	if (IsDebugExecutionSuspended())
	{
		SetSessionDebugSuspendedFailure(
			GetLiveModuleId(),
			FString(ExportName.Len(), ExportName.GetData()),
			OutResult);
		return false;
	}
	return true;
}

void FAvidScriptRuntimeSession::QuarantineFaultedRuntime(
	const FAvidScriptWasmSmokeResult& Failure)
{
	if (LiveDomain && (LiveDomain->IsFaulted() || GetLiveLifecycleState() == EAvidScriptLifecycleState::Faulted))
	{
		auto Domain = LiveDomain;
		Domain->Poison(Failure);
		return;
	}
	if (!LiveRuntime
		|| GetLiveLifecycleState() !=
			EAvidScriptLifecycleState::Faulted)
	{
		return;
	}

	const FString FailedModuleId = LiveManifest.ModuleId;
	const FString RootCategory = Failure.ErrorCategory;
	const FString FailedExportName = Failure.ExportName;
	const FString FailedDiagnostic = Failure.ErrorMessage.Left(4096);
	FAvidScriptWasmSmokeResult IgnoredUnloadResult;
	StopAndUnload(IgnoredUnloadResult);
	bFaultQuarantined = true;
	FaultedModuleId = FailedModuleId;
	FaultCategory = RootCategory;
	FaultExportName = FailedExportName;
	FaultDiagnostic = FailedDiagnostic;
	++FaultCount;
}

void FAvidScriptRuntimeSession::ClearFaultQuarantine()
{
	bFaultQuarantined = false;
	FaultedModuleId.Reset();
	FaultCategory.Reset();
	FaultExportName.Reset();
	FaultDiagnostic.Reset();
}

bool FAvidScriptRuntimeSession::IsDebugExecutionSuspended() const
{
	return Debugger->IsExecutionSuspended();
}

bool FAvidScriptRuntimeSession::DispatchEventLive(
	int32 EventId,
	float Value,
	FAvidScriptWasmSmokeResult& OutResult)
{
	if (SuppressApplicationLifecycleEntry(TEXT("avid_on_event"), OutResult))
	{
		return true;
	}
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::GuestCall,
		static_cast<uint32>(EAvidScriptProfilerOperation::Event),
		0,
		0,
		static_cast<uint64>(static_cast<uint32>(EventId)));
	ProfileScope.SetSucceeded(false);
	if (!CanEnterGuest(TEXT("avid_on_event"), OutResult))
	{
		return false;
	}
	bool bSucceeded = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
		bSucceeded = EventRouter->Dispatch(EventId, Value, OutResult);
	}
	if (!bSucceeded)
	{
		QuarantineFaultedRuntime(OutResult);
	}
	ProfileScope.SetSucceeded(bSucceeded);
	return bSucceeded;
}

bool FAvidScriptRuntimeSession::DispatchEventHot(
	const int32 EventId,
	const float Value,
	FAvidScriptWasmSmokeResult& OutFailure)
{
	if (SuppressApplicationLifecycleEntry(TEXT("avid_on_event"), OutFailure))
	{
		return true;
	}
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::GuestCall,
		static_cast<uint32>(EAvidScriptProfilerOperation::Event),
		0,
		0,
		static_cast<uint64>(static_cast<uint32>(EventId)));
	ProfileScope.SetSucceeded(false);
	if (!CanEnterGuest(TEXT("avid_on_event"), OutFailure))
	{
		return false;
	}
	bool bSucceeded = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
		bSucceeded = EventRouter->DispatchHot(EventId, Value, OutFailure);
	}
	if (!bSucceeded)
	{
		QuarantineFaultedRuntime(OutFailure);
	}
	ProfileScope.SetSucceeded(bSucceeded);
	return bSucceeded;
}

bool FAvidScriptRuntimeSession::DispatchGameplayEventLive(
	const FAvidScriptGameplayEvent& Event,
	FAvidScriptWasmSmokeResult& OutResult)
{
	if (SuppressApplicationLifecycleEntry(
			TEXT("avid_on_gameplay_event"),
			OutResult))
	{
		return true;
	}
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::GuestCall,
		static_cast<uint32>(EAvidScriptProfilerOperation::GameplayEvent),
		0,
		0,
		static_cast<uint64>(Event.Type));
	ProfileScope.SetSucceeded(false);
	if (!CanEnterGuest(TEXT("avid_on_gameplay_event"), OutResult))
	{
		return false;
	}
	bool bSucceeded = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
		bSucceeded = EventRouter->Dispatch(Event, OutResult);
	}
	if (!bSucceeded)
	{
		QuarantineFaultedRuntime(OutResult);
	}
	ProfileScope.SetSucceeded(bSucceeded);
	return bSucceeded;
}

bool FAvidScriptRuntimeSession::DispatchGameplayEventHot(
	const FAvidScriptGameplayEvent& Event,
	FAvidScriptWasmSmokeResult& OutFailure)
{
	if (SuppressApplicationLifecycleEntry(
			TEXT("avid_on_gameplay_event"),
			OutFailure))
	{
		return true;
	}
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::GuestCall,
		static_cast<uint32>(EAvidScriptProfilerOperation::GameplayEvent),
		0,
		0,
		static_cast<uint64>(Event.Type));
	ProfileScope.SetSucceeded(false);
	if (!CanEnterGuest(TEXT("avid_on_gameplay_event"), OutFailure))
	{
		return false;
	}
	bool bSucceeded = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
		bSucceeded = EventRouter->DispatchHot(Event, OutFailure);
	}
	if (!bSucceeded)
	{
		QuarantineFaultedRuntime(OutFailure);
	}
	ProfileScope.SetSucceeded(bSucceeded);
	return bSucceeded;
}

bool FAvidScriptRuntimeSession::DispatchPreparedDelegateEvent(
	const FAvidScriptPreparedDelegateEvent& Event,
	void* NativeParameters,
	FAvidScriptWasmSmokeResult& OutResult,
	const bool bLanguageManagedCallback)
{
	if (SuppressApplicationLifecycleEntry(Event.ExportName, OutResult))
	{
		return true;
	}
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::GuestCall,
		static_cast<uint32>(EAvidScriptProfilerOperation::DelegateEvent),
		0,
		0,
		GetTypeHash(Event.ExportName));
	ProfileScope.SetSucceeded(false);
	const bool bCanNest = bLanguageManagedCallback
		&& Event.Signature.Kind == EAvidScriptPreparedDelegateKind::Multicast
		&& Event.Signature.OutputValueCount == 0;
	if (!CanEnterGuest(Event.ExportName, OutResult, bCanNest))
	{
		return false;
	}
	bool bSucceeded = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
		if (GeneratedTypeInstance)
		{
			const auto* Call = GeneratedTypeInstance->DelegateCalls.Find(Event.ExportName);
			if (Call && LiveRuntime) bSucceeded = LiveRuntime->DispatchPreparedDelegateEventInContext(*Call, HostContext, Event, NativeParameters, OutResult);
			else SetSessionExecutionFailure(GetLiveModuleId(), Event.ExportName,
				TEXT("generated instance callback was not prepared for this code generation"), OutResult);
		}
		else bSucceeded = EventRouter->Dispatch(Event, NativeParameters, OutResult);
	}
	if (!bSucceeded)
	{
		QuarantineFaultedRuntime(OutResult);
	}
	ProfileScope.SetSucceeded(bSucceeded);
	return bSucceeded;
}

bool FAvidScriptRuntimeSession::CaptureLiveSnapshot(
	FAvidScriptWasmSmokeResult& OutResult) const
{
	if (!LiveRuntime)
	{
		SetSessionExecutionFailure(
			GetLiveModuleId(),
			TEXT("<snapshot>"),
			TEXT("snapshot was requested without an active Runtime"),
			OutResult);
		return false;
	}
	if (HostContext.InstanceExecutionState) return LiveRuntime->CaptureSnapshotInContext(HostContext, OutResult);
	LiveRuntime->CaptureSnapshot(OutResult);
	return true;
}

bool FAvidScriptRuntimeSession::EndPlayLive(FAvidScriptWasmSmokeResult& OutResult)
{
	PrunePendingBorrowedHandles();
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::GuestCall,
		static_cast<uint32>(EAvidScriptProfilerOperation::EndPlay));
	ProfileScope.SetSucceeded(false);
	if (IsOperationActive())
	{
		SetSessionExecutionFailure(
			GetLiveModuleId(),
			TEXT("avid_on_end_play"),
			TEXT("EndPlay was requested while another guest call or Runtime mutation is active"),
			OutResult);
		return false;
	}
	DelegateSubscriptions->UnbindActive();
	InboundHandlers->UnbindActive();
	Continuations->Teardown();
	if (!IsLiveLoaded())
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.ModuleId = LiveManifest.ModuleId;
		OutResult.ExportName = TEXT("avid_on_end_play");
		OutResult.ErrorCategory = TEXT("invalid_state");
		OutResult.NextAction = TEXT("load a validated WASM module before ending the live script runtime");
		OutResult.ErrorMessage = FString::Printf(
			TEXT("AvidScript live runtime EndPlay rejected | module=%s | category=invalid_state | details=no live runtime is loaded"),
			LiveManifest.ModuleId.IsEmpty() ? TEXT("<none>") : *LiveManifest.ModuleId);
		return false;
	}

	bool bSucceeded = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
		bSucceeded = HostContext.InstanceExecutionState
			? LiveRuntime->EndPlayInContext(HostContext, OutResult) : LiveRuntime->EndPlay(OutResult);
	}
	HostContext.Continuations = nullptr;
	HostContext.Tasks = nullptr;
	HostContext.LatentHost = nullptr;
	SetRuntimeBaseContext(*LiveRuntime, HostContext);
	Continuations->ReleaseRetiredEndpoint();
	if (!bSucceeded)
	{
		QuarantineFaultedRuntime(OutResult);
	}
	else if (HostContext.ObjectRegistry != nullptr)
	{
		TGuardValue<bool> MutationGuard(bMutationInProgress, true);
		ObjectOwnership->Cleanup(*HostContext.ObjectRegistry);
		bBorrowedHandlePrunePending = false;
	}
	ProfileScope.SetSucceeded(bSucceeded);
	return bSucceeded;
}

bool FAvidScriptRuntimeSession::StopAndUnloadForCollectedGeneratedOwner()
{
	if (!IsInGameThread() || !GeneratedTypeInstance || GeneratedTypeInstance->Receiver.IsValid()
		|| IsOperationActive()) return false;
	AbortRuntimeForLifecycleInvalidation();
	bLifecycleInvalidated = true;
	++LifecycleInvalidationCount;
	return true;
}

bool FAvidScriptRuntimeSession::StopAndUnload(FAvidScriptWasmSmokeResult& OutResult)
{
	if (IsOperationActive() || (LiveDomain && LiveDomain->HasActiveCalls()))
	{
		SetSessionExecutionFailure(
			GetLiveModuleId(),
			TEXT("<unload>"),
			TEXT("unload was requested while another guest call or Runtime mutation is active"),
			OutResult);
		return false;
	}
	if (bApplicationSuspended)
	{
		const FString SuspendedModule = GetLiveModuleId();
		AbortRuntimeForLifecycleInvalidation();
		bLifecycleInvalidated = false;
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.ModuleId = SuspendedModule;
		OutResult.bUnloaded = true;
		return true;
	}
	auto DomainAtEntry = LiveDomain;
	ON_SCOPE_EXIT { if (DomainAtEntry) DomainAtEntry->DrainFault(); };
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);
	bool bSucceeded = true;
	FAvidScriptWasmSmokeResult EndPlayFailure;
	DelegateSubscriptions->UnbindActive();
	DelegateSubscriptions->DiscardPrepared();
	InboundHandlers->UnbindActive();
	InboundHandlers->DiscardPrepared();
	Continuations->Teardown();
	Scheduler->Detach();
	if (LiveRuntime)
	{
		const auto& State = HostContext.InstanceExecutionState;
		const bool bRunning = State
			? !State->IsRetired() && State->GetLifecycleState() == EAvidScriptLifecycleState::Running
			: LiveRuntime->GetLifecycleState() == EAvidScriptLifecycleState::Running;
		// A retired generated owner cannot safely receive the generic EndPlay export.
		// Its native terminal lifecycle route has already had its chance to run.
		bool bContextOwnerLive = true;
		if (State)
		{
			FAvidScriptObjectHandleResult ResolveResult;
			UObject* const Owner = HostContext.ObjectRegistry != nullptr
				? HostContext.ObjectRegistry->ResolveObject(HostContext.OwnerHandle, ResolveResult, false)
				: nullptr;
			bContextOwnerLive = Owner != nullptr
				&& !Owner->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
				&& !HostContext.World.IsStale()
				&& (!HostContext.World.IsValid() || !HostContext.World->bIsTearingDown)
				&& Owner->GetWorld() == HostContext.World.Get();
		}
		if (!bFaultQuarantined && bRunning && bContextOwnerLive && !(State ? LiveRuntime->EndPlayInContext(HostContext, EndPlayFailure)
			: LiveRuntime->EndPlay(EndPlayFailure)))
		{
			bSucceeded = false;
			if (LiveDomain) LiveDomain->Poison(EndPlayFailure);
		}

		FAvidScriptWasmSmokeResult UnloadResult;
		ReleaseLiveRuntime(UnloadResult);
		OutResult = bSucceeded ? UnloadResult : EndPlayFailure;
	}
	else
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.bUnloaded = true;
	}
	if (GeneratedTypeInstance)
	{
		GeneratedTypeInstance->PreparedTypeRoutes.Reset();
		GeneratedTypeInstance->ContinuationCall = {};
		GeneratedTypeInstance->DelegateCalls.Reset();
	}
	Continuations->ReleaseRetiredEndpoint();
	HostContext.Continuations = nullptr;
	HostContext.Tasks = nullptr;
	HostContext.LatentHost = nullptr;
	HostContext.InstanceExecutionState.Reset();
	if (HostContext.ObjectRegistry != nullptr)
	{
		ObjectOwnership->Cleanup(*HostContext.ObjectRegistry);
	}

	LiveManifest = FAvidScriptWasmReloadManifest();
	Debugger->OnRuntimeGenerationChanged();
	ClearFaultQuarantine();
	bLifecycleInvalidated = false;
	bApplicationSuspended = false;
	bBorrowedHandlePrunePending = false;
	ResetSuspendedContext();
	return bSucceeded;
}

void FAvidScriptRuntimeSession::UnloadLive()
{
	FAvidScriptWasmSmokeResult IgnoredResult;
	StopAndUnload(IgnoredResult);
}
bool FAvidScriptRuntimeSession::IsLiveLoaded() const
{
	return LiveRuntime.IsValid() &&
		Scheduler->IsAttachedTo(LiveRuntime.Get()) &&
		LiveRuntime->IsLoaded();
}

FString FAvidScriptRuntimeSession::GetLiveModuleId() const
{
	return Scheduler->GetModuleId();
}

int32 FAvidScriptRuntimeSession::GetLiveTickCallCount() const
{
	return Scheduler->GetTickCallCount();
}

int32 FAvidScriptRuntimeSession::GetLivePendingTimerCount() const
{
	return Scheduler->GetPendingTimerCount();
}

void FAvidScriptRuntimeSession::SetProfilerEnabled(const bool bEnabled)
{
	Profiler->SetBufferEnabled(bEnabled);
}

bool FAvidScriptRuntimeSession::IsProfilerEnabled() const
{
	return Profiler->IsBufferEnabled();
}

void FAvidScriptRuntimeSession::ResetProfiler()
{
	Profiler->Reset();
}

FAvidScriptProfilerSnapshot FAvidScriptRuntimeSession::GetProfilerSnapshot() const
{
	return Profiler->Snapshot();
}

bool FAvidScriptRuntimeSession::AttachDebugger(
	const TConstArrayView<uint64> BreakpointProbeIds)
{
	return !IsOperationActive()
		&& !IsDebugExecutionSuspended()
		&& Debugger->Attach(BreakpointProbeIds);
}

bool FAvidScriptRuntimeSession::DetachDebugger()
{
	if (IsOperationActive() || IsDebugExecutionSuspended())
	{
		return false;
	}
	Debugger->Detach();
	return true;
}

bool FAvidScriptRuntimeSession::SetDebugBreakpoints(
	const TConstArrayView<uint64> BreakpointProbeIds)
{
	return !IsOperationActive() && Debugger->SetBreakpoints(BreakpointProbeIds);
}

bool FAvidScriptRuntimeSession::RequestDebugPause()
{
	return !IsOperationActive() && Debugger->RequestPause();
}

bool FAvidScriptRuntimeSession::ContinueDebugExecution()
{
	FAvidScriptWasmSmokeResult Result;
	return ContinueDebugExecution(Result);
}

bool FAvidScriptRuntimeSession::ContinueDebugExecution(
	FAvidScriptWasmSmokeResult& OutResult)
{
	return ResumeDebugExecution(EAvidScriptDebugRunMode::Continue, OutResult);
}

bool FAvidScriptRuntimeSession::StepIntoDebugExecution()
{
	FAvidScriptWasmSmokeResult Result;
	return StepIntoDebugExecution(Result);
}

bool FAvidScriptRuntimeSession::StepIntoDebugExecution(
	FAvidScriptWasmSmokeResult& OutResult)
{
	return ResumeDebugExecution(EAvidScriptDebugRunMode::StepInto, OutResult);
}

bool FAvidScriptRuntimeSession::ResumeDebugExecution(
	const EAvidScriptDebugRunMode RunMode,
	FAvidScriptWasmSmokeResult& OutResult)
{
	PrunePendingBorrowedHandles();
	FAvidScriptProfilerScope ProfileScope(
		Profiler.Get(),
		EAvidScriptProfilerEventKind::GuestCall,
		static_cast<uint32>(EAvidScriptProfilerOperation::DebugResume));
	ProfileScope.SetSucceeded(false);
	const FString ExportName(TEXT("avid_on_debug_resume"));
	if (IsOperationActive() || !LiveRuntime)
	{
		SetSessionExecutionFailure(
			GetLiveModuleId(),
			ExportName,
			TEXT("debug resume was requested without an idle live Runtime Session"),
			OutResult);
		return false;
	}

	const FAvidScriptDebugSessionSnapshot PausedSnapshot = Debugger->GetSnapshot();
	if (PausedSnapshot.State != EAvidScriptDebugSessionState::Paused
		|| PausedSnapshot.SuspensionToken <= 0
		|| PausedSnapshot.ResumeRoute == 0)
	{
		SetSessionExecutionFailure(
			GetLiveModuleId(),
			ExportName,
			TEXT("debug resume requires a paused session with a committed suspension frame"),
			OutResult);
		return false;
	}

	const bool bPrepared = RunMode == EAvidScriptDebugRunMode::StepInto
		? Debugger->StepInto()
		: Debugger->ContinueExecution();
	if (!bPrepared)
	{
		SetSessionExecutionFailure(
			GetLiveModuleId(),
			ExportName,
			TEXT("the debugger rejected the requested resume transition"),
			OutResult);
		return false;
	}

	bool bDispatched = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
		bDispatched = HostContext.InstanceExecutionState
			? LiveRuntime->DispatchDebugResumeInContext(HostContext, PausedSnapshot.SuspensionToken,
				PausedSnapshot.ResumeRoute, OutResult)
			: LiveRuntime->DispatchDebugResume(
			PausedSnapshot.SuspensionToken,
			PausedSnapshot.ResumeRoute,
			OutResult);
	}
	if (!bDispatched)
	{
		Debugger->OnRuntimeGenerationChanged();
		QuarantineFaultedRuntime(OutResult);
		return false;
	}

	const FAvidScriptDebugSessionSnapshot ResumedSnapshot = Debugger->GetSnapshot();
	const bool bRunning =
		ResumedSnapshot.State == EAvidScriptDebugSessionState::Running;
	const bool bPausedAgain =
		ResumedSnapshot.State == EAvidScriptDebugSessionState::Paused
		&& ResumedSnapshot.PauseSequence > PausedSnapshot.PauseSequence
		&& ResumedSnapshot.SuspensionToken > 0
		&& ResumedSnapshot.SuspensionToken != PausedSnapshot.SuspensionToken;
	if (!bRunning && !bPausedAgain)
	{
		Debugger->OnRuntimeGenerationChanged();
		SetSessionExecutionFailure(
			GetLiveModuleId(),
			ExportName,
			TEXT("the debug resume export returned without consuming its suspension frame or committing a later pause"),
			OutResult);
		return false;
	}
	ProfileScope.SetSucceeded(true);
	return true;
}

FAvidScriptDebugSessionSnapshot FAvidScriptRuntimeSession::GetDebugSnapshot() const
{
	return Debugger->GetSnapshot();
}

bool FAvidScriptRuntimeSession::GetDebugBreakpointCatalog(
	TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints,
	FString& OutError) const
{
	check(IsInGameThread());
	OutBreakpoints.Reset();
	OutError.Reset();
	if (!IsLiveLoaded() || !LiveManifest.DebugMap.IsValid())
	{
		OutError = TEXT("the live Runtime Session has no validated debug map");
		return false;
	}

	LiveManifest.DebugMap->BuildBreakpointCatalog(OutBreakpoints);
	return true;
}

bool FAvidScriptRuntimeSession::GetDebugVariables(
	FAvidScriptDebugVariablesSnapshot& OutSnapshot,
	FString& OutError) const
{
	check(IsInGameThread());
	OutSnapshot = FAvidScriptDebugVariablesSnapshot();
	OutError.Reset();
	const FAvidScriptDebugSessionSnapshot DebugSnapshot = Debugger->GetSnapshot();
	if (DebugSnapshot.State != EAvidScriptDebugSessionState::Paused)
	{
		OutError = TEXT("debug variables require a paused Runtime Session");
		return false;
	}
	if (!LiveManifest.DebugMap.IsValid())
	{
		OutError = TEXT("the live Runtime Session has no validated debug map");
		return false;
	}

	TArray<uint8> FrameBytes;
	if (!Debugger->CopySuspensionFrame(FrameBytes))
	{
		OutError = TEXT("the paused Runtime Session has no readable suspension frame");
		return false;
	}
	if (!LiveManifest.DebugMap->BuildVariableSnapshot(
		DebugSnapshot.ActiveProbeId,
		FrameBytes,
		OutSnapshot,
		OutError))
	{
		OutSnapshot = FAvidScriptDebugVariablesSnapshot();
		return false;
	}
	OutSnapshot.Epoch = DebugSnapshot.Epoch;
	OutSnapshot.PauseSequence = DebugSnapshot.PauseSequence;
	return true;
}

int32 FAvidScriptRuntimeSession::GetLivePendingContinuationCount() const
{
	return Continuations->GetActiveCount();
}

int32 FAvidScriptRuntimeSession::GetLiveTimerCallbackCount() const
{
	return Scheduler->GetTimerCallbackCount();
}

int32 FAvidScriptRuntimeSession::GetLiveEventCallbackCount() const
{
	return Scheduler->GetEventCallbackCount();
}

EAvidScriptLifecycleState
FAvidScriptRuntimeSession::GetLiveLifecycleState() const
{
	return Scheduler->GetLifecycleState();
}

FAvidScriptWasmHotSnapshot
FAvidScriptRuntimeSession::GetLiveHotSnapshot() const
{
	return LiveRuntime
		? (HostContext.InstanceExecutionState ? LiveRuntime->GetHotSnapshotInContext(HostContext) : LiveRuntime->GetHotSnapshot())
		: FAvidScriptWasmHotSnapshot();
}

FAvidScriptRuntimeSessionSnapshot FAvidScriptRuntimeSession::GetSnapshot() const
{
	FAvidScriptRuntimeSessionSnapshot Snapshot;
	Snapshot.bHasActiveRuntime = IsLiveLoaded();
	Snapshot.bFaultQuarantined = bFaultQuarantined;
	Snapshot.LifecycleState = bFaultQuarantined
		? EAvidScriptLifecycleState::Faulted
		: Scheduler->GetLifecycleState();
	Snapshot.ModuleId = bFaultQuarantined
		? FaultedModuleId
		: GetLiveModuleId();
	Snapshot.FaultCategory = FaultCategory;
	Snapshot.FaultExportName = FaultExportName;
	Snapshot.FaultDiagnostic = FaultDiagnostic;
	Snapshot.FaultCount = FaultCount;
	Snapshot.FaultedEntryRejectCount = FaultedEntryRejectCount;
	Snapshot.SuppressedFaultDiagnosticCount =
		SuppressedFaultDiagnosticCount;
	Snapshot.TickCallCount = GetLiveTickCallCount();
	Snapshot.PendingTimerCount = GetLivePendingTimerCount();
	Snapshot.PendingContinuationCount = GetLivePendingContinuationCount();
	Snapshot.PreparedContinuationCount = Continuations->GetPreparedCount();
	Snapshot.ActiveDelegateSubscriptionCount = DelegateSubscriptions->NumActive();
	Snapshot.PreparedDelegateSubscriptionCount = DelegateSubscriptions->NumPrepared();
	Snapshot.OwnedObjectEntryCount = ObjectOwnership->Num();
	Snapshot.BorrowedHandleEntryCount = ObjectOwnership->GetBorrowedHandleCount();
	Snapshot.TimerCallbackCount = GetLiveTimerCallbackCount();
	Snapshot.EventCallbackCount = GetLiveEventCallbackCount();
	Snapshot.SuccessfulReloadCount = SuccessfulReloadCount;
	Snapshot.RejectedReloadCount = RejectedReloadCount;
	Snapshot.ApplicationLifecycleGeneration = ApplicationLifecycleGeneration;
	Snapshot.SuppressedLifecycleEntryCount = SuppressedLifecycleEntryCount;
	Snapshot.LowMemoryNotificationCount = LowMemoryNotificationCount;
	Snapshot.LifecycleInvalidationCount = LifecycleInvalidationCount;
	Snapshot.bApplicationSuspended = bApplicationSuspended;
	Snapshot.bLifecycleInvalidated = bLifecycleInvalidated;
	return Snapshot;
}

#if WITH_DEV_AUTOMATION_TESTS
FAvidScriptRuntimeSessionTestSnapshot FAvidScriptRuntimeSession::GetTestSnapshot() const
{
	FAvidScriptRuntimeSessionTestSnapshot Snapshot;
	Snapshot.Runtime = GetSnapshot();
	Snapshot.LiveManifest = LiveManifest;
	Snapshot.HostContext = HostContext;
	Snapshot.LiveRuntimeIdentity = LiveRuntime.Get();
	Snapshot.bSchedulerAttached = LiveRuntime.IsValid() && Scheduler->IsAttachedTo(LiveRuntime.Get());
	return Snapshot;
}

bool FAvidScriptRuntimeSession::PrepareDelegateSubscriptionsForTesting(
	UObject* Source,
	const TConstArrayView<FAvidScriptPreparedDelegateEvent> Events,
	FString& OutError)
{
	return DelegateSubscriptions->Prepare(Source, Events, OutError, LiveRuntime.Get());
}

void FAvidScriptRuntimeSession::CommitDelegateSubscriptionsForTesting()
{
	DelegateSubscriptions->CommitPrepared();
	DelegateSubscriptions->SetDispatchEnabled(true);
}

void FAvidScriptRuntimeSession::UnbindDelegateSubscriptionsForTesting()
{
	DelegateSubscriptions->UnbindActive();
	DelegateSubscriptions->DiscardPrepared();
}

int32 FAvidScriptRuntimeSession::GetDelegateSubscriptionCountForTesting() const
{
	return DelegateSubscriptions->NumActive();
}

bool FAvidScriptRuntimeSession::PrepareInboundHandlersForTesting(
	UObject* Source,
	const TConstArrayView<FAvidScriptPreparedDelegateEvent> Handlers,
	FString& OutError)
{
	return InboundHandlers->Prepare(Source, Handlers, OutError);
}

bool FAvidScriptRuntimeSession::CommitInboundHandlersForTesting(
	FString& OutError)
{
	const bool bCommitted = InboundHandlers->CommitPrepared(OutError);
	InboundHandlers->SetDispatchEnabled(bCommitted);
	return bCommitted;
}

void FAvidScriptRuntimeSession::UnbindInboundHandlersForTesting()
{
	InboundHandlers->UnbindActive();
	InboundHandlers->DiscardPrepared();
}

int32 FAvidScriptRuntimeSession::GetInboundHandlerCountForTesting() const
{
	return InboundHandlers->NumActive();
}

int32 FAvidScriptRuntimeSession::GetDeferredInboundHandlerCountForTesting() const
{
	return InboundHandlers->NumDeferred();
}

int32 FAvidScriptRuntimeSession::GetPreparedContinuationCountForTesting() const
{
	return Continuations->GetPreparedCount();
}

int64 FAvidScriptRuntimeSession::SubscribeDelegateForTesting(
	UObject& Source,
	const uint32 EventOrdinal,
	FString& OutError)
{
	return DelegateSubscriptions->Subscribe(Source, EventOrdinal, OutError);
}

bool FAvidScriptRuntimeSession::UnsubscribeDelegateForTesting(
	const int64 SubscriptionToken,
	FString& OutError)
{
	return DelegateSubscriptions->Unsubscribe(SubscriptionToken, OutError);
}
#endif

bool FAvidScriptRuntimeSession::ValidateManifest(
	const FAvidScriptWasmReloadManifest& Manifest,
	const FString& PreviousModuleId,
	FAvidScriptWasmReloadResult& OutResult) const
{
	OutResult.PreviousModuleId = PreviousModuleId;
	OutResult.CandidateModuleId = Manifest.ModuleId;

	if (Manifest.ModuleId.IsEmpty())
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("manifest_invalid"),
			TEXT("manifest ModuleId is empty"),
			TEXT("assign a stable module id before loading or reloading a script"));
		return false;
	}

	if (Manifest.AbiVersion != FAvidScriptWasmReloadManifest::SupportedAbiVersion)
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("abi_mismatch"),
			FString::Printf(
				TEXT("manifest ABI version %d does not match supported ABI version %d"),
				Manifest.AbiVersion,
				FAvidScriptWasmReloadManifest::SupportedAbiVersion),
			TEXT("rebuild the script with the runtime supported ABI or keep the previous live runtime"));
		return false;
	}

	if (Manifest.RequiredExports.IsEmpty())
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("manifest_invalid"),
			TEXT("manifest RequiredExports is empty"),
			TEXT("declare the guest exports that must be validated before activation"));
		return false;
	}

	return true;
}

bool FAvidScriptRuntimeSession::BuildValidatedRuntime(
	const FAvidScriptRuntimeArtifact& Artifact,
	TSharedPtr<FAvidScriptWasmRuntimeInstance>& OutRuntime,
	FAvidScriptWasmReloadResult& OutResult) const
{
	const FAvidScriptWasmReloadManifest& Manifest = Artifact.Manifest;
	TSharedPtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime =
		MakeShared<FAvidScriptWasmRuntimeInstance>(Artifact.BackendSelection);
	TArray<FAvidScriptVmTypedHostImport> GeneratedPropertyImports;
	if (GeneratedTypeInstance)
	{
		TArray<FAvidScriptVmTypedHostImport> AvailableImports;
		FString BindingError;
		if (!CandidateRuntime->ConfigureGeneratedTypeHostBindings(GeneratedTypeInstance->Registry, AvailableImports, BindingError,
			Artifact.VmArtifact.CanonicalWasmBytes))
		{
			SetReloadFailure(OutResult, TEXT("<generated-properties>"), TEXT("generated_property_import_invalid"),
				BindingError, TEXT("rebuild the generated type package from the current registry"));
			return false;
		}
		for (const FAvidScriptVmTypedHostImport& Import : AvailableImports)
		{
			if (Manifest.RequiredImports.ContainsByPredicate(
				[&Import](const FAvidScriptWasmRequiredImport& RequiredImport)
				{
					return RequiredImport.ModuleName == Import.ModuleName
						&& RequiredImport.ImportName == Import.ImportName;
				}))
			{
				GeneratedPropertyImports.Add(Import);
			}
		}
	}
	if (Artifact.VmArtifact.CanonicalWasmBytes.IsEmpty()
		|| (Artifact.VmArtifact.ArtifactFormat !=
				EAvidScriptVmArtifactFormat::WasmBytecode
			&& Artifact.VmArtifact.ExecutionBytes.IsEmpty()))
	{
		SetReloadFailure(
			OutResult,
			TEXT("<bytecode>"),
			TEXT("invalid_bytecode"),
			TEXT("WASM bytecode pointer must be non-null and byte count must be positive"),
			TEXT("provide the complete built WASM module before loading or reloading"));
		return false;
	}

	FAvidScriptWasmImportContractResult ImportContractResult;
	if (!InspectAndValidateAvidScriptWasmImportContract(
			Artifact.VmArtifact.CanonicalWasmBytes,
			Manifest,
			ImportContractResult,
			GeneratedPropertyImports))
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			ImportContractResult.ErrorCategory,
			ImportContractResult.ErrorDetails,
			ImportContractResult.NextAction);
		return false;
	}

	const bool bRequiresPackedOwnerCapability = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& Import)
		{
			return Import.ModuleName == TEXT("avidscript")
				&& Import.ImportName == TEXT("avid_owner_get_handle");
		});
	if (bRequiresPackedOwnerCapability
		&& (!Manifest.BindingPackage.IsValid()
			|| Manifest.BindingPackage->GetExpectedSelfClass() == nullptr))
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("binding_package_import_mismatch"),
			TEXT("actual packed owner import requires a schema v6 or newer binding package with ExpectedSelfClass"),
			TEXT("rebuild the script and binding package as one transaction"));
		return false;
	}

	if (!ValidateExpectedOwner(Manifest, OutResult))
	{
		return false;
	}

	FAvidScriptWasmSmokeResult RuntimeResult;
	FString SupplementalImportError;
	FString BudgetError;
	const bool bSerializedArtifactProcessAuthorized =
		Artifact.ArtifactTrust != EAvidScriptVmArtifactTrust::VerifiedPackage
		&& Artifact.VmArtifact.bCooperativeSafepointProofVerified
		&& Artifact.VmArtifact.ArtifactFormat ==
			EAvidScriptVmArtifactFormat::WasmtimeSerialized
		&& AuthorizeAvidScriptVmArtifact(
			Artifact.VmArtifact.AttestationId,
			Artifact.VmArtifact);
	if (!CandidateRuntime->ConfigureExecutionBudget(
			ResolveExecutionBudget(
				Artifact.BackendSelection,
				Artifact.ArtifactTrust,
				Artifact.VmArtifact.bCooperativeSafepointProofVerified,
				bSerializedArtifactProcessAuthorized),
			BudgetError))
	{
		SetReloadFailure(
			OutResult,
			TEXT("<runtime>"),
			TEXT("execution_budget_invalid"),
			BudgetError,
			TEXT("fix the Session execution policy before loading the script"));
		return false;
	}
	if (!CandidateRuntime->SetSupplementalTypedHostImports(
			GeneratedPropertyImports,
			SupplementalImportError))
	{
		SetReloadFailure(
			OutResult,
			TEXT("<generated-properties>"),
			TEXT("generated_property_import_invalid"),
			SupplementalImportError,
			TEXT("rebuild the generated type package from the current registry"));
		return false;
	}

	if (!CandidateRuntime->LoadArtifact(
		Artifact.VmArtifact,
		Manifest.ModuleId,
		Manifest.BindingPackage,
		Manifest.DebugMap,
		Artifact.ArtifactTrust,
		RuntimeResult))
	{
		CopyRuntimeFailure(RuntimeResult, OutResult);
		return false;
	}

	if (!CandidateRuntime->ValidateRequiredExports(Manifest.RequiredExports, RuntimeResult))
	{
		CopyRuntimeFailure(RuntimeResult, OutResult);
		CandidateRuntime->Unload();
		return false;
	}

	if (!CandidateRuntime->PrepareGeneratedMethodHostBindings(SupplementalImportError))
	{
		SetReloadFailure(OutResult, TEXT("<generated-methods>"), TEXT("generated_method_route_invalid"),
			SupplementalImportError, TEXT("rebuild the generated method routes from the canonical module"));
		CandidateRuntime->Unload();
		return false;
	}
	SetRuntimeBaseContext(*CandidateRuntime, HostContext);
	OutResult.RuntimeResult = RuntimeResult;
	OutRuntime = MoveTemp(CandidateRuntime);
	return true;
}

bool FAvidScriptRuntimeSession::ValidateExpectedOwner(
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadResult& OutResult) const
{
	if (!Manifest.BindingPackage.IsValid())
	{
		return true;
	}
	// A generated module can contain Actors, Components and Subsystems while its
	// facade has an Actor Self. Generated receiver identity is checked against
	// the registered UClass; packed Self is checked when that import is called.
	if (GeneratedTypeInstance)
	{
		return true;
	}

	UClass* const ExpectedSelfClass = Manifest.BindingPackage->GetExpectedSelfClass();
	// Packages without typed Self retain the legacy owner contract.
	if (ExpectedSelfClass == nullptr)
	{
		return true;
	}

	auto RejectOwner = [&OutResult](const FString& Expected, const FString& Actual, const FString& Reason)
	{
		SetReloadFailure(
			OutResult,
			TEXT("<owner>"),
			TEXT("runtime_owner_type_mismatch"),
			FString::Printf(TEXT("expected=%s; actual=%s; reason=%s"), *Expected, *Actual, *Reason),
			TEXT("bind the script to a live owner that matches its generated Self type"));
		return false;
	};

	if (!IsValid(ExpectedSelfClass))
	{
		return RejectOwner(TEXT("<invalid_expected_class>"), TEXT("<unresolved>"), TEXT("the package ExpectedSelfClass is invalid"));
	}

	const FString ExpectedClassName = ExpectedSelfClass->GetName();
	if (HostContext.ObjectRegistry == nullptr)
	{
		return RejectOwner(ExpectedClassName, TEXT("<unresolved:missing_registry>"), TEXT("the host object registry is missing"));
	}

	FAvidScriptObjectHandleResult ResolveResult;
	UObject* const OwnerObject = HostContext.ObjectRegistry->ResolveObject(
		HostContext.OwnerHandle,
		ResolveResult,
		false);
	if (OwnerObject == nullptr)
	{
		const FString ResolveCategory = ResolveResult.ErrorCategory.IsEmpty()
			? TEXT("unresolved")
			: ResolveResult.ErrorCategory;
		return RejectOwner(
			ExpectedClassName,
			FString::Printf(TEXT("<unresolved:%s>"), *ResolveCategory),
			TEXT("the host owner handle cannot be resolved by the active registry"));
	}

	UClass* const ActualClass = OwnerObject->GetClass();
	const FString ActualClassName = ActualClass != nullptr ? ActualClass->GetName() : TEXT("<invalid_object_class>");
	if (ActualClass == nullptr || !OwnerObject->IsA(ExpectedSelfClass))
	{
		return RejectOwner(ExpectedClassName, ActualClassName, TEXT("the resolved owner does not satisfy ExpectedSelfClass"));
	}

	return true;
}

bool FAvidScriptRuntimeSession::ActivateValidatedRuntime(
	TSharedPtr<FAvidScriptWasmRuntimeInstance>& CandidateRuntime,
	const FAvidScriptWasmReloadManifest& Manifest,
	bool bUseHostEffectTransaction,
	FAvidScriptWasmReloadResult& OutResult, bool bDeferCommit,
	const TSharedPtr<FAvidScriptRuntimeExecutionDomain>& CandidateDomain)
{
	const auto RejectCandidate = [&]()
	{
		if (CandidateDomain)
		{
			auto Failure = OutResult.RuntimeResult;
			Failure.ModuleId = Manifest.ModuleId;
			Failure.ErrorCategory = OutResult.ErrorCategory;
			Failure.ErrorMessage = OutResult.ErrorMessage;
			CandidateDomain->Poison(Failure);
		}
		else if (CandidateRuntime) CandidateRuntime->Unload();
	};
	if (GeneratedExecutionGeneration == MAX_uint64)
	{
		SetReloadFailure(OutResult, TEXT("<runtime>"), TEXT("execution_generation_exhausted"),
			TEXT("Session execution generation cannot be reused"), TEXT("create a new Session"));
		return false;
	}
	if (!CandidateRuntime)
	{
		SetReloadFailure(
			OutResult,
			TEXT("<runtime>"),
			TEXT("invalid_state"),
			TEXT("validated candidate runtime is missing"),
			TEXT("rebuild and validate the candidate before activation"));
		return false;
	}
	TArray<FAvidScriptGeneratedPreparedTypeRoute> CandidateGeneratedTypeRoutes;
	FString GeneratedTypePrepareError;
	if (!PrepareGeneratedTypeExports(
		*CandidateRuntime,
		CandidateGeneratedTypeRoutes,
		GeneratedTypePrepareError))
	{
		SetReloadFailure(
			OutResult,
			TEXT("<generated_types>"),
			TEXT("generated_type_export_prepare_failed"),
			GeneratedTypePrepareError,
			TEXT("rebuild the generated type WASM exports and keep the previous runtime active"));
		RejectCandidate();
		return false;
	}
	TArray<FAvidScriptPreparedDelegateEvent> CandidateDelegateEvents;
	TArray<FAvidScriptPreparedDelegateEvent> CandidateInboundHandlers;
	FAvidScriptContextualExportCall CandidateContinuationCall;
	TMap<FString, FAvidScriptContextualExportCall> CandidateDelegateCalls;
	FString DelegatePrepareError;
	DelegateSubscriptions->DiscardPrepared();
	InboundHandlers->DiscardPrepared();
	const auto DiscardPreparedCallbacks = [this]()
	{
		DelegateSubscriptions->DiscardPrepared();
		InboundHandlers->DiscardPrepared();
	};
	if (!CandidateRuntime->BuildPreparedCallbacks(
			CandidateDelegateEvents,
			CandidateInboundHandlers,
			DelegatePrepareError))
	{
		SetReloadFailure(
			OutResult,
			TEXT("<delegate_events>"),
			TEXT("delegate_subscription_prepare_failed"),
			DelegatePrepareError.IsEmpty()
				? TEXT("the candidate delegate event plans could not be prepared")
				: DelegatePrepareError,
			TEXT("regenerate the callback descriptor and keep the previous runtime active"));
		RejectCandidate();
		return false;
	}
	if (GeneratedTypeInstance)
	{
		bool bPrepared = CandidateRuntime->PrepareContextualContinuationCall(CandidateContinuationCall, DelegatePrepareError);
		for (const auto* Plans : {&CandidateDelegateEvents, &CandidateInboundHandlers})
		{
			for (const auto& Plan : *Plans)
			{
				if (!bPrepared) break;
				if (!CandidateDelegateCalls.Contains(Plan.ExportName))
					bPrepared = CandidateRuntime->PrepareContextualExportCall(Plan.ExportName,
						CandidateDelegateCalls.Add(Plan.ExportName), DelegatePrepareError);
			}
		}
		if (!bPrepared)
		{
			DiscardPreparedCallbacks();
			SetReloadFailure(OutResult, TEXT("<contextual_callbacks>"), TEXT("callback_export_prepare_failed"),
				DelegatePrepareError, TEXT("keep the previous Runtime and rebuild the callback exports"));
			RejectCandidate();
			return false;
		}
	}
	UObject* CandidateDelegateSource = nullptr;
	if (!CandidateDelegateEvents.IsEmpty()
		|| !CandidateInboundHandlers.IsEmpty())
	{
		if (HostContext.ObjectRegistry != nullptr)
		{
			FAvidScriptObjectHandleResult ResolveResult;
			CandidateDelegateSource = HostContext.ObjectRegistry->ResolveObject(
				HostContext.OwnerHandle,
				ResolveResult,
				false);
		}
		if (!DelegateSubscriptions->Prepare(
				CandidateDelegateSource,
				CandidateDelegateEvents,
				DelegatePrepareError,
				CandidateRuntime.Get())
			|| !InboundHandlers->Prepare(
				CandidateDelegateSource,
				CandidateInboundHandlers,
				DelegatePrepareError))
		{
			DiscardPreparedCallbacks();
			SetReloadFailure(
				OutResult,
				TEXT("<delegate_events>"),
				TEXT("delegate_subscription_prepare_failed"),
				DelegatePrepareError,
				TEXT("bind the script to a compatible live self object before activation"));
			RejectCandidate();
			return false;
		}
	}
	const int32 BorrowedHandleCheckpoint = ObjectOwnership->GetBorrowedHandleCount();
	const auto RollbackBorrowedHandles = [this, BorrowedHandleCheckpoint]() -> bool
	{
		if (ObjectOwnership->GetBorrowedHandleCount() == BorrowedHandleCheckpoint)
		{
			return true;
		}
		if (HostContext.ObjectRegistry == nullptr)
		{
			return false;
		}
		FString RollbackError;
		const bool bRolledBack = ObjectOwnership->RollbackBorrowedHandles(
			*HostContext.ObjectRegistry,
			BorrowedHandleCheckpoint,
			RollbackError);
		if (!bRolledBack)
		{
			UE_LOG(
				LogAvidScriptRuntimeSession,
				Error,
				TEXT("AvidScript candidate borrowed-handle rollback failed: %s"),
				RollbackError.IsEmpty() ? TEXT("<none>") : *RollbackError);
		}
		return bRolledBack;
	};
	FAvidScriptContinuationHostEndpoint& PreparedContinuationHost =
		Continuations->BeginPrepared(
			HostContext.World.Get(),
			HostContext.ObjectRegistry,
			ObjectOwnership.Get(),
			HostContext.OwnerHandle);
	FAvidScriptWasmHostContext CandidateHostContext = HostContext;
	CandidateHostContext.InstanceExecutionState.Reset();
	CandidateHostContext.DebugProbes = nullptr;
	CandidateHostContext.Continuations = &PreparedContinuationHost;
	CandidateHostContext.Tasks = &PreparedContinuationHost;
	CandidateHostContext.LatentHost = &PreparedContinuationHost;
	SetRuntimeBaseContext(*CandidateRuntime, CandidateHostContext);

	if (GeneratedTypeInstance && !CandidateRuntime->CreateInstanceExecutionState(
		CandidateHostContext, CandidateHostContext.InstanceExecutionState, GeneratedTypePrepareError))
	{
		Continuations->DiscardPrepared();
		DiscardPreparedCallbacks();
		SetReloadFailure(OutResult, TEXT("<instance_state>"), TEXT("instance_execution_state_prepare_failed"),
			GeneratedTypePrepareError, TEXT("bind a live owner in the Runtime World and registry"));
		RejectCandidate();
		return false;
	}

	// The journal address remains stable while other package instances prepare.
	auto Pending = MakeUnique<FAvidScriptPreparedRuntimeActivation>();
	auto& HostEffectTransaction = Pending->HostEffectTransaction;
	if (bUseHostEffectTransaction)
	{
		HostEffectTransaction.Emplace();
		OutResult.bHostEffectTransactionAttempted = true;
		CandidateHostContext.HostEffectJournal = &HostEffectTransaction.GetValue();
		SetRuntimeBaseContext(*CandidateRuntime, CandidateHostContext);
	}

	FAvidScriptWasmSmokeResult BeginPlayResult;
#if WITH_DEV_AUTOMATION_TESTS
	if (CandidateBeginPlayObserverForTesting)
	{
		TFunction<void(IAvidScriptBindingHostEffectJournal*)> Observer =
			MoveTemp(CandidateBeginPlayObserverForTesting);
		Observer(
			bUseHostEffectTransaction
				? &HostEffectTransaction.GetValue()
				: nullptr);
	}
#endif
	const bool bBegan = CandidateHostContext.InstanceExecutionState
		? CandidateRuntime->BeginPlayInContext(CandidateHostContext, BeginPlayResult)
		: CandidateRuntime->BeginPlay(BeginPlayResult);
	if (!bBegan)
	{
		CopyRuntimeFailure(BeginPlayResult, OutResult);
		bool bHostEffectsRolledBack = true;
		if (bUseHostEffectTransaction)
		{
			OutResult.bHostEffectRollbackAttempted = true;
			FAvidScriptObjectRegistry EmptyRegistry;
			FAvidScriptObjectRegistry& RollbackRegistry = HostContext.ObjectRegistry != nullptr
				? *HostContext.ObjectRegistry
				: EmptyRegistry;
			FAvidScriptHostEffectTransactionResult RollbackResult;
			bHostEffectsRolledBack = HostEffectTransaction->Rollback(
				RollbackRegistry,
				RollbackResult);
			CopyHostEffectResult(RollbackResult, OutResult);
		}
		const bool bBorrowedHandlesRolledBack = RollbackBorrowedHandles();
		OutResult.bHostEffectRollbackSucceeded =
			bUseHostEffectTransaction
			&& bHostEffectsRolledBack
			&& bBorrowedHandlesRolledBack;
		SetRuntimeBaseContext(*CandidateRuntime, HostContext);
		Continuations->DiscardPrepared();
		DiscardPreparedCallbacks();
		RejectCandidate();
		return false;
	}

	FString ContinuationCommitError;
	FString InboundCommitError;
	const bool bContinuationCommitValid =
		Continuations->ValidatePreparedCommit(ContinuationCommitError);
	const bool bInboundCommitValid =
		InboundHandlers->ValidatePreparedCommit(InboundCommitError);
	if (!bContinuationCommitValid || !bInboundCommitValid)
	{
		bool bHostEffectsRolledBack = true;
		if (bUseHostEffectTransaction)
		{
			OutResult.bHostEffectRollbackAttempted = true;
			FAvidScriptObjectRegistry EmptyRegistry;
			FAvidScriptObjectRegistry& RollbackRegistry = HostContext.ObjectRegistry != nullptr
				? *HostContext.ObjectRegistry
				: EmptyRegistry;
			FAvidScriptHostEffectTransactionResult RollbackResult;
			bHostEffectsRolledBack = HostEffectTransaction->Rollback(
				RollbackRegistry,
				RollbackResult);
			CopyHostEffectResult(RollbackResult, OutResult);
		}
		const bool bBorrowedHandlesRolledBack = RollbackBorrowedHandles();
		OutResult.bHostEffectRollbackSucceeded =
			bUseHostEffectTransaction
			&& bHostEffectsRolledBack
			&& bBorrowedHandlesRolledBack;
		SetReloadFailure(
			OutResult,
			bContinuationCommitValid
				? TEXT("<inbound_handlers>")
				: TEXT("<continuations>"),
			bContinuationCommitValid
				? TEXT("inbound_handler_prepare_failed")
				: TEXT("continuation_prepare_failed"),
			bContinuationCommitValid
				? InboundCommitError
				: ContinuationCommitError,
			TEXT("keep the previous Runtime active and discard candidate callback state"));
		SetRuntimeBaseContext(*CandidateRuntime, HostContext);
		Continuations->DiscardPrepared();
		DiscardPreparedCallbacks();
		RejectCandidate();
		return false;
	}

	Pending->Runtime = MoveTemp(CandidateRuntime);
	Pending->Domain = CandidateDomain;
	Pending->Context = CandidateHostContext;
	Pending->Manifest = Manifest;
	Pending->BeginPlayResult = BeginPlayResult;
	Pending->GeneratedRoutes = MoveTemp(CandidateGeneratedTypeRoutes);
	Pending->ContinuationCall = MoveTemp(CandidateContinuationCall);
	Pending->DelegateCalls = MoveTemp(CandidateDelegateCalls);
	Pending->BorrowedHandleCheckpoint = BorrowedHandleCheckpoint;
	PreparedActivation = MoveTemp(Pending);
	return bDeferCommit || CommitPreparedActivation(OutResult);
}

bool FAvidScriptRuntimeSession::ValidatePreparedActivation(FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || ActiveGuestCallDepth != 0 || !PreparedActivation || bApplicationSuspended
		|| (PreparedActivation->Domain && PreparedActivation->Domain->IsFaulted())
		|| !PreparedActivation->Runtime || !PreparedActivation->Runtime->IsLoaded()
		|| GeneratedExecutionGeneration == MAX_uint64
		|| (PreparedActivation->HostEffectTransaction.IsSet()
			&& PreparedActivation->HostEffectTransaction->GetState() != EAvidScriptHostEffectTransactionState::Open))
	{
		OutError = TEXT("prepared Runtime activation is no longer publishable");
		return false;
	}
	if (GeneratedTypeInstance)
	{
		// Validate the candidate identity, independently of the old Session's fault
		// quarantine. A replacement must be able to recover a stopped generation.
		const auto& Context = PreparedActivation->Context;
		UObject* Receiver = GeneratedTypeInstance->Receiver.Get();
		FAvidScriptObjectHandleResult Resolved;
		if (!Receiver || !GeneratedTypeInstance->Registration.IsValid()
			|| !Context.InstanceExecutionState || Context.InstanceExecutionState->IsRetired()
			|| Context.InstanceExecutionState->GetLifecycleState() != EAvidScriptLifecycleState::Running
			|| Context.OwnerHandle != GeneratedTypeInstance->ReceiverHandle || !Context.ObjectRegistry
			|| Context.World.IsStale() || (Context.World.IsValid() && Context.World->bIsTearingDown)
			|| Receiver->GetWorld() != Context.World.Get()
			|| Receiver->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed)
			|| Context.ObjectRegistry->ResolveObject(Context.OwnerHandle, Resolved, false) != Receiver)
		{
			OutError = TEXT("prepared generated owner is no longer live");
			return false;
		}
	}
	return Continuations->ValidatePreparedCommit(OutError) && InboundHandlers->ValidatePreparedCommit(OutError);
}

bool FAvidScriptRuntimeSession::DiscardPreparedActivation(FAvidScriptWasmReloadResult& OutResult)
{
	check(IsInGameThread() && ActiveGuestCallDepth == 0);
	if (!PreparedActivation) return true;
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);
	auto Pending = MoveTemp(PreparedActivation);
	bool bRestored = true;
	if (Pending->HostEffectTransaction.IsSet())
	{
		OutResult.bHostEffectRollbackAttempted = true;
		FAvidScriptObjectRegistry EmptyRegistry;
		FAvidScriptHostEffectTransactionResult RollbackResult;
		bRestored = Pending->HostEffectTransaction->Rollback(
			HostContext.ObjectRegistry ? *HostContext.ObjectRegistry : EmptyRegistry, RollbackResult);
		CopyHostEffectResult(RollbackResult, OutResult);
	}
	if (ObjectOwnership->GetBorrowedHandleCount() != Pending->BorrowedHandleCheckpoint)
	{
		FString Error;
		bRestored = (HostContext.ObjectRegistry && ObjectOwnership->RollbackBorrowedHandles(
			*HostContext.ObjectRegistry, Pending->BorrowedHandleCheckpoint, Error)) && bRestored;
	}
	Pending->Context.HostEffectJournal = nullptr;
	SetRuntimeBaseContext(*Pending->Runtime, Pending->Context);
	if (Pending->Domain)
	{
		FString RetireError;
		bRestored = Pending->Runtime->RetireInstanceExecutionState(Pending->Context.InstanceExecutionState, RetireError) && bRestored;
	}
	else Pending->Runtime->Unload();
	Continuations->DiscardPrepared();
	DelegateSubscriptions->DiscardPrepared();
	InboundHandlers->DiscardPrepared();
	OutResult.bHostEffectRollbackSucceeded = bRestored;
	OutResult.bRollbackPreservedLiveRuntime = bRestored && LiveRuntime.IsValid();
	if (!bRestored && OutResult.ErrorMessage.IsEmpty())
	{
		OutResult.ErrorCategory = TEXT("prepared_activation_rollback_failed");
		OutResult.ErrorMessage = TEXT("prepared activation could not restore its native effects or borrowed handles");
	}
	return bRestored;
}

bool FAvidScriptRuntimeSession::CommitPreparedActivation(FAvidScriptWasmReloadResult& OutResult)
{
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);
	FString CommitError;
	if (!ValidatePreparedActivation(CommitError))
	{
		SetReloadFailure(OutResult, TEXT("<activation>"), TEXT("prepared_activation_invalid"), CommitError,
			TEXT("discard the candidate and preserve the previous Runtime"));
		DiscardPreparedActivation(OutResult);
		return false;
	}
	auto Pending = MoveTemp(PreparedActivation);
	auto& CandidateRuntime = Pending->Runtime;
	auto& CandidateHostContext = Pending->Context;
	if (Pending->HostEffectTransaction.IsSet())
	{
		FAvidScriptHostEffectTransactionResult CommitResult;
		const bool bCommitted = Pending->HostEffectTransaction->Commit(CommitResult);
		check(bCommitted); // Open journal was validated; commit executes no Guest code.
		OutResult.bHostEffectTransactionCommitted = true;
		CopyHostEffectResult(CommitResult, OutResult);
		CandidateHostContext.HostEffectJournal = nullptr;
		SetRuntimeBaseContext(*CandidateRuntime, CandidateHostContext);
	}
	Continuations->CommitPrepared();

	if (LiveRuntime)
	{
		DelegateSubscriptions->SetDispatchEnabled(false);
		DelegateSubscriptions->UnbindActive();
		InboundHandlers->SetDispatchEnabled(false);
		InboundHandlers->UnbindActive();
		Scheduler->Detach();
		FAvidScriptWasmSmokeResult Released;
		ReleaseLiveRuntime(Released);
		LiveManifest = FAvidScriptWasmReloadManifest();
	}
	Continuations->ReleaseRetiredEndpoint();

	Debugger->OnRuntimeGenerationChanged();
	CandidateHostContext.DebugProbes = Debugger.Get();
	CandidateHostContext.Profiler = Profiler.Get();
	SetRuntimeBaseContext(*CandidateRuntime, CandidateHostContext);
	OutResult.RuntimeResult = Pending->BeginPlayResult;
	LiveRuntime = MoveTemp(CandidateRuntime);
	LiveDomain = MoveTemp(Pending->Domain);
	if (LiveDomain) LiveDomain->Attach(*this);
	++GeneratedExecutionGeneration;
	if (GeneratedTypeInstance)
	{
		GeneratedTypeInstance->PreparedTypeRoutes = MoveTemp(Pending->GeneratedRoutes);
		GeneratedTypeInstance->ContinuationCall = MoveTemp(Pending->ContinuationCall);
		GeneratedTypeInstance->DelegateCalls = MoveTemp(Pending->DelegateCalls);
	}
	LiveManifest = Pending->Manifest;
	HostContext.InstanceExecutionState = CandidateHostContext.InstanceExecutionState;
	HostContext.Continuations = CandidateHostContext.Continuations;
	HostContext.Tasks = CandidateHostContext.Tasks;
	HostContext.LatentHost = CandidateHostContext.LatentHost;
	HostContext.DebugProbes = Debugger.Get();
	HostContext.Profiler = Profiler.Get();
	Scheduler->Attach(*LiveRuntime, HostContext.InstanceExecutionState ? &HostContext : nullptr);
	DelegateSubscriptions->CommitPrepared();
	DelegateSubscriptions->SetDispatchEnabled(true);
	FString InboundCommitError;
	const bool bInboundCommitted =
		InboundHandlers->CommitPrepared(InboundCommitError);
	checkf(
		bInboundCommitted,
		TEXT("Validated AvidScript inbound handler commit failed: %s"),
		InboundCommitError.IsEmpty() ? TEXT("unknown") : *InboundCommitError);
	InboundHandlers->SetDispatchEnabled(true);
	ClearFaultQuarantine();
	bLifecycleInvalidated = false;
	return true;
}
