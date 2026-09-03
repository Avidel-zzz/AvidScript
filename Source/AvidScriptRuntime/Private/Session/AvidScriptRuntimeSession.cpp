#include "AvidScriptRuntimeSession.h"

#include "AvidScriptRuntimeArtifact.h"

#include "AvidScriptRuntimeEventRouter.h"
#include "AvidScriptRuntimeScheduler.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Debugging/AvidScriptSessionDebugger.h"
#include "Diagnostics/AvidScriptWasmDebugMap.h"
#include "GameFramework/Actor.h"
#include "HostEffects/AvidScriptHostEffectTransaction.h"
#include "Lifecycle/AvidScriptRuntimeLifecycleCoordinator.h"
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
	const FAvidScriptVmBackendSelection& Selection)
{
	FAvidScriptVmLoadConfig::FExecutionBudget Budget;
	Budget.MaxHostCallsPerEntry = 100000;
	if (Selection.BackendKind == EAvidScriptVmBackendKind::Wasmtime)
	{
		Budget.FuelPerEntry = 50000000;
		Budget.EpochDeadlineTicks = 1;
		Budget.EpochTimeoutMilliseconds = 100;
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
	checkf(!IsOperationActive(), TEXT("AvidScript RuntimeSession cannot be destroyed during an active guest call or mutation."));
	FAvidScriptRuntimeLifecycleCoordinator::Get().UnregisterSession(*this);
	UnloadLive();
	FString ClearError;
	ensureMsgf(
		ClearGeneratedTypeInstance(ClearError),
		TEXT("Generated type instance teardown failed: %s"),
		ClearError.IsEmpty() ? TEXT("unknown") : *ClearError);
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
		&& LiveRuntime->GetLifecycleState() == EAvidScriptLifecycleState::Running)
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
	const FString& ExportName,
	FAvidScriptWasmSmokeResult& OutResult)
{
	if (!bApplicationSuspended)
	{
		return false;
	}

	++SuppressedLifecycleEntryCount;
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = GetLiveModuleId();
	OutResult.ExportName = ExportName;
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
		LiveRuntime->Unload(IgnoredUnloadResult);
		LiveRuntime.Reset();
	}
	if (GeneratedTypeInstance)
	{
		GeneratedTypeInstance->PreparedTypeRoutes.Reset();
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
	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime =
		MakeUnique<FAvidScriptWasmRuntimeInstance>(BackendSelection);
	FAvidScriptWasmSmokeResult RuntimeResult;
	FString BudgetError;
	if (!CandidateRuntime->ConfigureExecutionBudget(
			MakeSessionExecutionBudget(BackendSelection),
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

	CandidateRuntime->SetHostContext(HostContext);
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

	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime;
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
	if (IsOperationActive())
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

	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime;
	if (!BuildValidatedRuntime(Artifact, CandidateRuntime, OutResult))
	{
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}

	FAvidScriptRuntimeStateMigrationResult MigrationResult;
	if (LiveRuntime && !FAvidScriptRuntimeStateMigration::Migrate(
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

	if (!ActivateValidatedRuntime(CandidateRuntime, Manifest, true, OutResult))
	{
		++RejectedReloadCount;
		const FString ActiveModuleId = GetLiveModuleId();
		MarkRejectedReloadWithRollback(ActiveModuleId, OutResult);
		return false;
	}

	++SuccessfulReloadCount;
	OutResult.bSucceeded = true;
	OutResult.bReloadApplied = true;
	OutResult.ActiveModuleId = Manifest.ModuleId;
	ProfileScope.SetSucceeded(true);
	return true;
}
void FAvidScriptRuntimeSession::SetHostContext(const FAvidScriptWasmHostContext& InHostContext)
{
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
	NextHostContext.ObjectOwnership = ObjectOwnership.Get();
	NextHostContext.HostEffectJournal = nullptr;
	NextHostContext.EventSubscriptions = DelegateSubscriptions.Get();
	NextHostContext.Continuations = HostContext.Continuations;
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
			NextHostContext.LatentHost = &ActiveContinuationHost;
		}
		else
		{
			NextHostContext.Continuations = nullptr;
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
	HostContext = MoveTemp(NextHostContext);
	if (LiveRuntime)
	{
		LiveRuntime->SetHostContext(HostContext);
		Continuations->ReleaseRetiredEndpoint();
		if (bDelegateSourceChanged
			&& LiveRuntime->GetLifecycleState()
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
					&& !DelegateSubscriptions->Prepare(Source, Events, Error))
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
	if (IsOperationActive())
	{
		UE_LOG(
			LogAvidScriptRuntimeSession,
			Verbose,
			TEXT("AvidScript host context clear rejected during an active guest call or mutation."));
		return;
	}
	TGuardValue<bool> MutationGuard(bMutationInProgress, true);
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
	if (LiveRuntime)
	{
		LiveRuntime->SetHostContext(HostContext);
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
		if (bSucceeded && !IsDebugExecutionSuspended())
		{
			bSucceeded = PumpReadyContinuations(OutResult);
		}
	}
	const bool bCompleted = bSucceeded
		&& (IsDebugExecutionSuspended()
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
		if (bSucceeded && !IsDebugExecutionSuspended())
		{
			bSucceeded = PumpReadyContinuations(OutResult);
		}
	}
	const bool bCompleted = bSucceeded
		&& (IsDebugExecutionSuspended()
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
		if (bSucceeded && !IsDebugExecutionSuspended())
		{
			bSucceeded = PumpReadyContinuations(OutFailure);
		}
	}
	const bool bCompleted = bSucceeded
		&& (IsDebugExecutionSuspended()
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
			&& LiveRuntime->DispatchContinuation(Completion, OutResult);
		const bool bFinalized = Continuations->FinalizeDispatched(
			Completion.Token,
			bDispatched);
		if (!bDispatched || !bFinalized)
		{
			Continuations->Teardown();
			HostContext.Continuations = nullptr;
			if (LiveRuntime)
			{
				LiveRuntime->SetHostContext(HostContext);
			}
			Continuations->ReleaseRetiredEndpoint();
			return false;
		}
		ProfileScope.SetSucceeded(true);
	}
	return true;
}

bool FAvidScriptRuntimeSession::CanEnterGuest(
	const FString& ExportName,
	FAvidScriptWasmSmokeResult& OutResult)
{
	if (bApplicationSuspended || bLifecycleInvalidated)
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.ModuleId = GetLiveModuleId();
		OutResult.ExportName = ExportName;
		OutResult.ErrorCategory = bApplicationSuspended
			? TEXT("application_suspended")
			: TEXT("lifecycle_invalidated");
		OutResult.NextAction = bApplicationSuspended
			? TEXT("resume the application before entering guest code")
			: TEXT("bind a live World and load a new validated module");
		OutResult.ErrorMessage = FString::Printf(
			TEXT("AvidScript guest entry rejected | module=%s | export=%s | category=%s"),
			OutResult.ModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.ModuleId,
			ExportName.IsEmpty() ? TEXT("<none>") : *ExportName,
			*OutResult.ErrorCategory);
		return false;
	}
	if (bFaultQuarantined)
	{
		++FaultedEntryRejectCount;
		++SuppressedFaultDiagnosticCount;
		SetSessionFaultedFailure(
			FaultedModuleId,
			ExportName,
			FaultCategory,
			OutResult);
		return false;
	}
	if (IsOperationActive())
	{
		SetSessionExecutionFailure(
			GetLiveModuleId(),
			ExportName,
			TEXT("guest entry was requested while another guest call or Runtime mutation is active"),
			OutResult);
		return false;
	}
	if (IsDebugExecutionSuspended())
	{
		SetSessionDebugSuspendedFailure(
			GetLiveModuleId(),
			ExportName,
			OutResult);
		return false;
	}
	return true;
}

void FAvidScriptRuntimeSession::QuarantineFaultedRuntime(
	const FAvidScriptWasmSmokeResult& Failure)
{
	if (!LiveRuntime
		|| LiveRuntime->GetLifecycleState() !=
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
	FAvidScriptWasmSmokeResult& OutResult)
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
	if (!CanEnterGuest(Event.ExportName, OutResult))
	{
		return false;
	}
	bool bSucceeded = false;
	{
		TGuardValue<int32> GuestCallGuard(
			ActiveGuestCallDepth,
			ActiveGuestCallDepth + 1);
		bSucceeded = EventRouter->Dispatch(Event, NativeParameters, OutResult);
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
	LiveRuntime->CaptureSnapshot(OutResult);
	return true;
}

bool FAvidScriptRuntimeSession::EndPlayLive(FAvidScriptWasmSmokeResult& OutResult)
{
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
		bSucceeded = LiveRuntime->EndPlay(OutResult);
	}
	HostContext.Continuations = nullptr;
	LiveRuntime->SetHostContext(HostContext);
	Continuations->ReleaseRetiredEndpoint();
	if (!bSucceeded)
	{
		QuarantineFaultedRuntime(OutResult);
	}
	else if (HostContext.ObjectRegistry != nullptr)
	{
		ObjectOwnership->Cleanup(*HostContext.ObjectRegistry);
	}
	ProfileScope.SetSucceeded(bSucceeded);
	return bSucceeded;
}

bool FAvidScriptRuntimeSession::StopAndUnload(FAvidScriptWasmSmokeResult& OutResult)
{
	if (IsOperationActive())
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
		if (LiveRuntime->GetLifecycleState() == EAvidScriptLifecycleState::Running &&
			!LiveRuntime->EndPlay(EndPlayFailure))
		{
			bSucceeded = false;
		}

		FAvidScriptWasmSmokeResult UnloadResult;
		LiveRuntime->Unload(UnloadResult);
		OutResult = bSucceeded ? UnloadResult : EndPlayFailure;
		LiveRuntime.Reset();
	}
	else
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.bUnloaded = true;
	}
	if (GeneratedTypeInstance)
	{
		GeneratedTypeInstance->PreparedTypeRoutes.Reset();
	}
	Continuations->ReleaseRetiredEndpoint();
	HostContext.Continuations = nullptr;
	if (HostContext.ObjectRegistry != nullptr)
	{
		ObjectOwnership->Cleanup(*HostContext.ObjectRegistry);
	}

	LiveManifest = FAvidScriptWasmReloadManifest();
	Debugger->OnRuntimeGenerationChanged();
	ClearFaultQuarantine();
	bLifecycleInvalidated = false;
	bApplicationSuspended = false;
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
		bDispatched = LiveRuntime->DispatchDebugResume(
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
		? LiveRuntime->GetHotSnapshot()
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
	return DelegateSubscriptions->Prepare(Source, Events, OutError);
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
	TUniquePtr<FAvidScriptWasmRuntimeInstance>& OutRuntime,
	FAvidScriptWasmReloadResult& OutResult) const
{
	const FAvidScriptWasmReloadManifest& Manifest = Artifact.Manifest;
	TArray<FAvidScriptVmTypedHostImport> GeneratedPropertyImports;
	if (GeneratedTypeInstance)
	{
		for (const FAvidScriptVmTypedHostImport& Import
			: GeneratedTypeInstance->PropertyImports)
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

	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime =
		MakeUnique<FAvidScriptWasmRuntimeInstance>(Artifact.BackendSelection);
	FAvidScriptWasmSmokeResult RuntimeResult;
	FString SupplementalImportError;
	FString BudgetError;
	if (!CandidateRuntime->ConfigureExecutionBudget(
			MakeSessionExecutionBudget(Artifact.BackendSelection),
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

	CandidateRuntime->SetHostContext(HostContext);
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
	TUniquePtr<FAvidScriptWasmRuntimeInstance>& CandidateRuntime,
	const FAvidScriptWasmReloadManifest& Manifest,
	bool bUseHostEffectTransaction,
	FAvidScriptWasmReloadResult& OutResult)
{
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
		CandidateRuntime->Unload();
		return false;
	}
	TArray<FAvidScriptPreparedDelegateEvent> CandidateDelegateEvents;
	TArray<FAvidScriptPreparedDelegateEvent> CandidateInboundHandlers;
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
		CandidateRuntime->Unload();
		return false;
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
				DelegatePrepareError)
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
			CandidateRuntime->Unload();
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
	CandidateHostContext.DebugProbes = nullptr;
	CandidateHostContext.Continuations = &PreparedContinuationHost;
	CandidateHostContext.LatentHost = &PreparedContinuationHost;
	CandidateRuntime->SetHostContext(CandidateHostContext);

	TOptional<FAvidScriptHostEffectTransaction> HostEffectTransaction;
	if (bUseHostEffectTransaction)
	{
		HostEffectTransaction.Emplace();
		OutResult.bHostEffectTransactionAttempted = true;
		CandidateHostContext.HostEffectJournal = &HostEffectTransaction.GetValue();
		CandidateRuntime->SetHostContext(CandidateHostContext);
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
	if (!CandidateRuntime->BeginPlay(BeginPlayResult))
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
		CandidateRuntime->SetHostContext(HostContext);
		Continuations->DiscardPrepared();
		DiscardPreparedCallbacks();
		CandidateRuntime->Unload();
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
		CandidateRuntime->SetHostContext(HostContext);
		Continuations->DiscardPrepared();
		DiscardPreparedCallbacks();
		CandidateRuntime->Unload();
		return false;
	}

	if (bUseHostEffectTransaction)
	{
		FAvidScriptHostEffectTransactionResult CommitResult;
		if (!HostEffectTransaction->Commit(CommitResult))
		{
			RollbackBorrowedHandles();
			CopyHostEffectResult(CommitResult, OutResult);
			SetReloadFailure(
				OutResult,
				TEXT("avid_on_begin_play"),
				TEXT("host_effect_transaction_invalid_state"),
				CommitResult.ErrorDetails,
				TEXT("keep the previous runtime active and report the candidate transaction state"));
			CandidateRuntime->SetHostContext(HostContext);
			Continuations->DiscardPrepared();
			DiscardPreparedCallbacks();
			CandidateRuntime->Unload();
			return false;
		}
		OutResult.bHostEffectTransactionCommitted = true;
		CopyHostEffectResult(CommitResult, OutResult);
		CandidateHostContext.HostEffectJournal = nullptr;
		CandidateRuntime->SetHostContext(CandidateHostContext);
	}
	Continuations->CommitPrepared();

	if (LiveRuntime)
	{
		DelegateSubscriptions->SetDispatchEnabled(false);
		DelegateSubscriptions->UnbindActive();
		InboundHandlers->SetDispatchEnabled(false);
		InboundHandlers->UnbindActive();
		Scheduler->Detach();
		LiveRuntime->Unload();
		LiveRuntime.Reset();
		LiveManifest = FAvidScriptWasmReloadManifest();
	}
	Continuations->ReleaseRetiredEndpoint();

	Debugger->OnRuntimeGenerationChanged();
	CandidateHostContext.DebugProbes = Debugger.Get();
	CandidateHostContext.Profiler = Profiler.Get();
	CandidateRuntime->SetHostContext(CandidateHostContext);
	OutResult.RuntimeResult = BeginPlayResult;
	LiveRuntime = MoveTemp(CandidateRuntime);
	if (GeneratedTypeInstance)
	{
		GeneratedTypeInstance->PreparedTypeRoutes = MoveTemp(CandidateGeneratedTypeRoutes);
	}
	Scheduler->Attach(*LiveRuntime);
	LiveManifest = Manifest;
	HostContext.Continuations = CandidateHostContext.Continuations;
	HostContext.DebugProbes = Debugger.Get();
	HostContext.Profiler = Profiler.Get();
	DelegateSubscriptions->CommitPrepared();
	DelegateSubscriptions->SetDispatchEnabled(true);
	InboundCommitError.Reset();
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
