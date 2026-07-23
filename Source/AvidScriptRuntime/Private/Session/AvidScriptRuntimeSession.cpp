#include "AvidScriptRuntimeSession.h"

#include "AvidScriptRuntimeEventRouter.h"
#include "AvidScriptRuntimeScheduler.h"
#include "GameFramework/Actor.h"
#include "HostEffects/AvidScriptHostEffectTransaction.h"
#include "StateMigration/AvidScriptRuntimeStateMigration.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

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
} // namespace

FAvidScriptRuntimeSession::FAvidScriptRuntimeSession()
	: Scheduler(MakeUnique<FAvidScriptRuntimeScheduler>())
	, EventRouter(MakeUnique<FAvidScriptRuntimeEventRouter>(*Scheduler))
{
}

FAvidScriptRuntimeSession::~FAvidScriptRuntimeSession()
{
	UnloadLive();
}

bool FAvidScriptRuntimeSession::LoadEmbeddedSmoke(FAvidScriptWasmReloadResult& OutResult)
{
	const FString ModuleId = TEXT("embedded_smoke");
	const FString PreviousModuleId = GetLiveModuleId();
	ResetReloadResult(OutResult, PreviousModuleId, ModuleId, PreviousModuleId);

	const FAvidScriptWasmReloadManifest Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(ModuleId);
	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime = MakeUnique<FAvidScriptWasmRuntimeInstance>();
	FAvidScriptWasmSmokeResult RuntimeResult;
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
	return true;
}

bool FAvidScriptRuntimeSession::LoadInitialModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadResult& OutResult)
{
	const FString PreviousModuleId = GetLiveModuleId();
	ResetReloadResult(OutResult, PreviousModuleId, Manifest.ModuleId, PreviousModuleId);

	if (!ValidateManifest(Manifest, PreviousModuleId, OutResult))
	{
		return false;
	}

	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime;
	if (!BuildValidatedRuntime(Bytecode, BytecodeSize, Manifest, CandidateRuntime, OutResult))
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
	return true;
}

bool FAvidScriptRuntimeSession::ReloadModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadResult& OutResult)
{
	const FString PreviousModuleId = GetLiveModuleId();
	ResetReloadResult(OutResult, PreviousModuleId, Manifest.ModuleId, PreviousModuleId);

	if (!ValidateManifest(Manifest, PreviousModuleId, OutResult))
	{
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}

	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime;
	if (!BuildValidatedRuntime(Bytecode, BytecodeSize, Manifest, CandidateRuntime, OutResult))
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
	return true;
}
void FAvidScriptRuntimeSession::SetHostContext(const FAvidScriptWasmHostContext& InHostContext)
{
	HostContext = InHostContext;
	HostContext.HostEffectJournal = nullptr;
	if (!HostContext.World.IsValid()
		&& HostContext.ObjectRegistry != nullptr
		&& HostContext.OwnerHandle.IsValid())
	{
		FAvidScriptObjectHandleResult ResolveResult;
		if (const AActor* Owner = HostContext.ObjectRegistry->ResolveObject<AActor>(
			HostContext.OwnerHandle,
			ResolveResult))
		{
			HostContext.World = Owner->GetWorld();
		}
	}
	if (LiveRuntime)
	{
		LiveRuntime->SetHostContext(HostContext);
	}
}

void FAvidScriptRuntimeSession::ClearHostContext()
{
	HostContext = FAvidScriptWasmHostContext();
	if (LiveRuntime)
	{
		LiveRuntime->ClearHostContext();
	}
}

bool FAvidScriptRuntimeSession::Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult)
{
	return TickLive(DeltaSeconds, OutResult);
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
	return Scheduler->Tick(DeltaSeconds, OutResult);
}

bool FAvidScriptRuntimeSession::DispatchEventLive(
	int32 EventId,
	float Value,
	FAvidScriptWasmSmokeResult& OutResult)
{
	return EventRouter->Dispatch(EventId, Value, OutResult);
}

bool FAvidScriptRuntimeSession::DispatchGameplayEventLive(
	const FAvidScriptGameplayEvent& Event,
	FAvidScriptWasmSmokeResult& OutResult)
{
	return EventRouter->Dispatch(Event, OutResult);
}

bool FAvidScriptRuntimeSession::EndPlayLive(FAvidScriptWasmSmokeResult& OutResult)
{
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

	return LiveRuntime->EndPlay(OutResult);
}

bool FAvidScriptRuntimeSession::StopAndUnload(FAvidScriptWasmSmokeResult& OutResult)
{
	bool bSucceeded = true;
	FAvidScriptWasmSmokeResult EndPlayFailure;
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

	LiveManifest = FAvidScriptWasmReloadManifest();
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

int32 FAvidScriptRuntimeSession::GetLiveTimerCallbackCount() const
{
	return Scheduler->GetTimerCallbackCount();
}

int32 FAvidScriptRuntimeSession::GetLiveEventCallbackCount() const
{
	return Scheduler->GetEventCallbackCount();
}
FAvidScriptRuntimeSessionSnapshot FAvidScriptRuntimeSession::GetSnapshot() const
{
	FAvidScriptRuntimeSessionSnapshot Snapshot;
	Snapshot.bHasActiveRuntime = IsLiveLoaded();
	Snapshot.LifecycleState = Scheduler->GetLifecycleState();
	Snapshot.ModuleId = GetLiveModuleId();
	Snapshot.TickCallCount = GetLiveTickCallCount();
	Snapshot.PendingTimerCount = GetLivePendingTimerCount();
	Snapshot.TimerCallbackCount = GetLiveTimerCallbackCount();
	Snapshot.EventCallbackCount = GetLiveEventCallbackCount();
	Snapshot.SuccessfulReloadCount = SuccessfulReloadCount;
	Snapshot.RejectedReloadCount = RejectedReloadCount;
	return Snapshot;
}

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

	const bool bRequiresDynamicBindingPackage = Manifest.RequiredImports.ContainsByPredicate(
		[](const FAvidScriptWasmRequiredImport& Import)
		{
			return Import.ModuleName == TEXT("avidscript")
				&& Import.ImportName.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive);
		});
	if (bRequiresDynamicBindingPackage && !Manifest.BindingPackage.IsValid())
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("binding_package_missing"),
			TEXT("manifest declares generated UE imports without a loaded binding package"),
			TEXT("load the script through its verified .avidscript.json manifest"));
		return false;
	}

	return true;
}

bool FAvidScriptRuntimeSession::BuildValidatedRuntime(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	TUniquePtr<FAvidScriptWasmRuntimeInstance>& OutRuntime,
	FAvidScriptWasmReloadResult& OutResult) const
{
	if (!ValidateExpectedOwner(Manifest, OutResult))
	{
		return false;
	}

	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime = MakeUnique<FAvidScriptWasmRuntimeInstance>();
	FAvidScriptWasmSmokeResult RuntimeResult;

	if (!CandidateRuntime->LoadModule(
		Bytecode,
		BytecodeSize,
		Manifest.ModuleId,
		Manifest.BindingPackage,
		Manifest.DebugMap,
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

	TOptional<FAvidScriptHostEffectTransaction> HostEffectTransaction;
	if (bUseHostEffectTransaction)
	{
		HostEffectTransaction.Emplace();
		OutResult.bHostEffectTransactionAttempted = true;
		FAvidScriptWasmHostContext CandidateHostContext = HostContext;
		CandidateHostContext.HostEffectJournal = &HostEffectTransaction.GetValue();
		CandidateRuntime->SetHostContext(CandidateHostContext);
	}

	FAvidScriptWasmSmokeResult BeginPlayResult;
	if (!CandidateRuntime->BeginPlay(BeginPlayResult))
	{
		CopyRuntimeFailure(BeginPlayResult, OutResult);
		if (bUseHostEffectTransaction)
		{
			OutResult.bHostEffectRollbackAttempted = true;
			FAvidScriptObjectRegistry EmptyRegistry;
			FAvidScriptObjectRegistry& RollbackRegistry = HostContext.ObjectRegistry != nullptr
				? *HostContext.ObjectRegistry
				: EmptyRegistry;
			FAvidScriptHostEffectTransactionResult RollbackResult;
			OutResult.bHostEffectRollbackSucceeded = HostEffectTransaction->Rollback(
				RollbackRegistry,
				RollbackResult);
			CopyHostEffectResult(RollbackResult, OutResult);
		}
		CandidateRuntime->SetHostContext(HostContext);
		CandidateRuntime->Unload();
		return false;
	}

	if (bUseHostEffectTransaction)
	{
		FAvidScriptHostEffectTransactionResult CommitResult;
		if (!HostEffectTransaction->Commit(CommitResult))
		{
			CopyHostEffectResult(CommitResult, OutResult);
			SetReloadFailure(
				OutResult,
				TEXT("avid_on_begin_play"),
				TEXT("host_effect_transaction_invalid_state"),
				CommitResult.ErrorDetails,
				TEXT("keep the previous runtime active and report the candidate transaction state"));
			CandidateRuntime->SetHostContext(HostContext);
			CandidateRuntime->Unload();
			return false;
		}
		OutResult.bHostEffectTransactionCommitted = true;
		CopyHostEffectResult(CommitResult, OutResult);
		CandidateRuntime->SetHostContext(HostContext);
	}

	if (LiveRuntime)
	{
		Scheduler->Detach();
		LiveRuntime->Unload();
		LiveRuntime.Reset();
		LiveManifest = FAvidScriptWasmReloadManifest();
	}

	OutResult.RuntimeResult = BeginPlayResult;
	LiveRuntime = MoveTemp(CandidateRuntime);
	Scheduler->Attach(*LiveRuntime);
	LiveManifest = Manifest;
	return true;
}
