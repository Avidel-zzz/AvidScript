#include "AvidScriptComponent.h"


#include "GameFramework/Actor.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptComponent, Log, All);

namespace
{
void SetComponentManifestLoadFailure(
	const FAvidScriptWasmReloadManifestLoadResult& LoadResult,
	FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = TEXT("<component_manifest>");
	OutResult.ExportName = TEXT("<manifest>");
	OutResult.ErrorCategory = LoadResult.ErrorCategory;
	OutResult.NextAction = LoadResult.NextAction;
	OutResult.ErrorMessage = LoadResult.ErrorMessage.IsEmpty()
		? FString::Printf(TEXT("Failed to load AvidScript manifest: %s"), *LoadResult.ManifestPath)
		: LoadResult.ErrorMessage;
}

void CopySessionLoadResult(
	const FAvidScriptWasmReloadResult& ReloadResult,
	FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = ReloadResult.RuntimeResult;
	if (OutResult.ModuleId.IsEmpty())
	{
		OutResult.ModuleId = ReloadResult.CandidateModuleId;
	}

	if (!ReloadResult.bSucceeded)
	{
		OutResult.ExportName = ReloadResult.ExportName;
		OutResult.ErrorCategory = ReloadResult.ErrorCategory;
		OutResult.NextAction = ReloadResult.NextAction;
		OutResult.ErrorMessage = ReloadResult.ErrorMessage;
	}
}

void SetComponentReloadManifestLoadFailure(
	const FAvidScriptWasmReloadManifestLoadResult& LoadResult,
	const FString& ActiveModuleId,
	FAvidScriptWasmReloadResult& OutResult)
{
	OutResult = FAvidScriptWasmReloadResult();
	OutResult.PreviousModuleId = ActiveModuleId;
	OutResult.CandidateModuleId = TEXT("<component_manifest>");
	OutResult.ActiveModuleId = ActiveModuleId;
	OutResult.ExportName = TEXT("<manifest>");
	OutResult.ErrorCategory = LoadResult.ErrorCategory;
	OutResult.NextAction = LoadResult.NextAction;
	OutResult.ErrorMessage = LoadResult.ErrorMessage.IsEmpty()
		? FString::Printf(TEXT("Failed to load AvidScript manifest: %s"), *LoadResult.ManifestPath)
		: LoadResult.ErrorMessage;
	OutResult.bRollbackPreservedLiveRuntime = !ActiveModuleId.IsEmpty();
	OutResult.RuntimeResult.ModuleId = ActiveModuleId;
	OutResult.RuntimeResult.ExportName = OutResult.ExportName;
	OutResult.RuntimeResult.ErrorCategory = OutResult.ErrorCategory;
	OutResult.RuntimeResult.NextAction = OutResult.NextAction;
	OutResult.RuntimeResult.ErrorMessage = OutResult.ErrorMessage;
}

void CopyComponentEventStats(
	const FAvidScriptWasmSmokeResult& Result,
	FAvidScriptComponentRuntimeStats& Stats)
{
	Stats.EventCallbackCount = FMath::Max(Stats.EventCallbackCount, Result.EventCallbackCount);
	if (Result.bEventCallbackCalled)
	{
		Stats.LastEventId = Result.LastEventId;
		Stats.LastEventValue = Result.LastEventValue;
	}
}

} // namespace

UAvidScriptComponent::UAvidScriptComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UAvidScriptComponent::SetScriptManifestPath(const FString& InScriptManifestPath)
{
	ScriptManifestFile.FilePath = InScriptManifestPath;
	FPaths::NormalizeFilename(ScriptManifestFile.FilePath);
}

FString UAvidScriptComponent::GetScriptManifestPath() const
{
	return ScriptManifestFile.FilePath;
}

FString UAvidScriptComponent::ResolveScriptManifestPath() const
{
	FString ManifestPath = ScriptManifestFile.FilePath;
	if (ManifestPath.IsEmpty())
	{
		return FString();
	}

	FPaths::NormalizeFilename(ManifestPath);
	if (FPaths::IsRelative(ManifestPath))
	{
		ManifestPath = FPaths::Combine(FPaths::ProjectDir(), ManifestPath);
	}

	ManifestPath = FPaths::ConvertRelativePathToFull(ManifestPath);
	FPaths::NormalizeFilename(ManifestPath);
	return ManifestPath;
}

bool UAvidScriptComponent::LoadConfiguredScriptModule(FAvidScriptWasmSmokeResult& OutResult)
{
	if (!RuntimeSession.IsValid())
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.ErrorCategory = TEXT("invalid_state");
		OutResult.ErrorMessage = TEXT("AvidScript component session has not been allocated.");
		OutResult.NextAction = TEXT("create a runtime session before loading script modules");
		return false;
	}

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &ObjectRegistry;
	HostContext.OwnerHandle = OwnerHandle;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	RuntimeSession->SetHostContext(HostContext);

	RuntimeStats.ScriptManifestPath = ResolveScriptManifestPath();
	FAvidScriptWasmReloadResult SessionResult;
	bool bLoaded = false;
	if (RuntimeStats.ScriptManifestPath.IsEmpty())
	{
		bLoaded = RuntimeSession->LoadEmbeddedSmoke(SessionResult);
	}
	else
	{
		FAvidScriptWasmReloadManifest Manifest;
		TArray<uint8> Bytecode;
		FAvidScriptWasmReloadManifestLoadResult LoadResult;
		if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(RuntimeStats.ScriptManifestPath, Manifest, Bytecode, LoadResult))
		{
			SetComponentManifestLoadFailure(LoadResult, OutResult);
			return false;
		}

		bLoaded = RuntimeSession->LoadInitialModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			Manifest,
			SessionResult);
	}

	CopySessionLoadResult(SessionResult, OutResult);
	if (bLoaded)
	{
		RuntimeStats.ModuleId = SessionResult.ActiveModuleId;
	}
	return bLoaded;
}

bool UAvidScriptComponent::ReloadConfiguredScript(FAvidScriptWasmReloadResult& OutResult)
{
	OutResult = FAvidScriptWasmReloadResult();
	const FAvidScriptRuntimeSessionSnapshot PreviousSnapshot = RuntimeSession.IsValid()
		? RuntimeSession->GetSnapshot()
		: FAvidScriptRuntimeSessionSnapshot();
	const FString CandidateManifestPath = ResolveScriptManifestPath();

	if (!RuntimeSession.IsValid() ||
		PreviousSnapshot.LifecycleState != EAvidScriptLifecycleState::Running ||
		!PreviousSnapshot.bHasActiveRuntime)
	{
		OutResult.PreviousModuleId = PreviousSnapshot.ModuleId;
		OutResult.ActiveModuleId = PreviousSnapshot.ModuleId;
		OutResult.ExportName = TEXT("<component>");
		OutResult.ErrorCategory = TEXT("invalid_state");
		OutResult.ErrorMessage = TEXT("AvidScript component reload requires a running script runtime.");
		OutResult.NextAction = TEXT("start the component runtime before requesting a live reload");
		OutResult.bRollbackPreservedLiveRuntime = PreviousSnapshot.bHasActiveRuntime;
		++RuntimeStats.RejectedReloadCount;
		RuntimeStats.LastErrorMessage = OutResult.ErrorMessage;
		return false;
	}

	if (CandidateManifestPath.IsEmpty())
	{
		FAvidScriptWasmReloadManifestLoadResult LoadResult;
		LoadResult.ManifestPath = CandidateManifestPath;
		LoadResult.ErrorCategory = TEXT("manifest_path_invalid");
		LoadResult.ErrorMessage = TEXT("AvidScript component reload requires a non-empty manifest path.");
		LoadResult.NextAction = TEXT("assign a built .avidscript.json manifest before reloading");
		SetComponentReloadManifestLoadFailure(LoadResult, PreviousSnapshot.ModuleId, OutResult);
		++RuntimeStats.RejectedReloadCount;
		RuntimeStats.LastErrorMessage = OutResult.ErrorMessage;
		return false;
	}

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			CandidateManifestPath,
			Manifest,
			Bytecode,
			LoadResult))
	{
		SetComponentReloadManifestLoadFailure(LoadResult, PreviousSnapshot.ModuleId, OutResult);
		++RuntimeStats.RejectedReloadCount;
		RuntimeStats.LastErrorMessage = OutResult.ErrorMessage;
		RuntimeStats.bRuntimeLoaded = PreviousSnapshot.bHasActiveRuntime;
		RuntimeStats.ModuleId = PreviousSnapshot.ModuleId;
		UE_LOG(LogAvidScriptComponent, Warning, TEXT("%s"), *OutResult.ErrorMessage);
		return false;
	}

	if (!RuntimeSession->ReloadModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			Manifest,
			OutResult))
	{
		const FAvidScriptRuntimeSessionSnapshot RejectedSnapshot = RuntimeSession->GetSnapshot();
		++RuntimeStats.RejectedReloadCount;
		RuntimeStats.LastErrorMessage = OutResult.ErrorMessage;
		RuntimeStats.bRuntimeLoaded = RejectedSnapshot.bHasActiveRuntime;
		RuntimeStats.bBeginPlayCalled = RejectedSnapshot.LifecycleState == EAvidScriptLifecycleState::Running;
		RuntimeStats.TickCallCount = RejectedSnapshot.TickCallCount;
		RuntimeStats.ModuleId = RejectedSnapshot.ModuleId;
		UE_LOG(LogAvidScriptComponent, Warning, TEXT("%s"), *OutResult.ErrorMessage);
		return false;
	}

	const FAvidScriptRuntimeSessionSnapshot AppliedSnapshot = RuntimeSession->GetSnapshot();
	const FAvidScriptWasmSmokeResult& RuntimeResult = OutResult.RuntimeResult;
	++RuntimeStats.SuccessfulReloadCount;
	RuntimeStats.bRuntimeLoaded = AppliedSnapshot.bHasActiveRuntime;
	RuntimeStats.bBeginPlayCalled = RuntimeResult.bBeginPlayCalled;
	RuntimeStats.TickCallCount = AppliedSnapshot.TickCallCount;
	RuntimeStats.TimerCallbackCount = RuntimeResult.TimerCallbackCount;
	RuntimeStats.LastTimerCallbackId = RuntimeResult.LastTimerCallbackId;
	RuntimeStats.LastTimerHandle = RuntimeResult.LastTimerHandle;
	RuntimeStats.EventCallbackCount = RuntimeResult.EventCallbackCount;
	RuntimeStats.LastEventId = RuntimeResult.LastEventId;
	RuntimeStats.LastEventValue = RuntimeResult.LastEventValue;
	RuntimeStats.Metrics = RuntimeResult.Metrics;
	RuntimeStats.ModuleId = AppliedSnapshot.ModuleId;
	RuntimeStats.ScriptManifestPath = CandidateManifestPath;
	RuntimeStats.LastErrorMessage.Reset();

	UE_LOG(
		LogAvidScriptComponent,
		Log,
		TEXT("AvidScript component reload | owner=%s | previous=%s | active=%s | manifest=%s | successful=%d | rejected=%d"),
		RuntimeStats.OwnerObjectPath.IsEmpty() ? TEXT("<none>") : *RuntimeStats.OwnerObjectPath,
		OutResult.PreviousModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.PreviousModuleId,
		*RuntimeStats.ModuleId,
		*RuntimeStats.ScriptManifestPath,
		RuntimeStats.SuccessfulReloadCount,
		RuntimeStats.RejectedReloadCount);
	return true;
}

bool UAvidScriptComponent::ReloadScript()
{
	FAvidScriptWasmReloadResult Result;
	return ReloadConfiguredScript(Result);
}

bool UAvidScriptComponent::ResolveOwnerActor(AActor*& OutOwner, FAvidScriptObjectHandleResult& OutResult) const
{
	OutOwner = ObjectRegistry.ResolveObject<AActor>(OwnerHandle, OutResult);
	return OutOwner != nullptr;
}

bool UAvidScriptComponent::DispatchScriptEvent(int32 EventId, float Value)
{
	const FAvidScriptRuntimeSessionSnapshot Snapshot = RuntimeSession.IsValid()
		? RuntimeSession->GetSnapshot()
		: FAvidScriptRuntimeSessionSnapshot();
	if (Snapshot.LifecycleState != EAvidScriptLifecycleState::Running)
	{
		RuntimeStats.LastErrorMessage = TEXT("AvidScript gameplay event rejected because the component session is not running.");
		return false;
	}

	FAvidScriptWasmSmokeResult Result;
	if (!RuntimeSession->DispatchEvent(EventId, Value, Result))
	{
		RecordRuntimeFailure(Result);
		if (Result.ErrorCategory == TEXT("invalid_argument"))
		{
			return false;
		}
		ReleaseRuntime();
		SetComponentTickEnabled(false);
		return false;
	}

	RuntimeStats.bRuntimeLoaded = RuntimeSession->GetSnapshot().bHasActiveRuntime;
	RuntimeStats.Metrics = Result.Metrics;
	RuntimeStats.ModuleId = Result.ModuleId;
	CopyComponentEventStats(Result, RuntimeStats);
	return true;
}

bool UAvidScriptComponent::DispatchScriptInput(int32 ActionId, int32 TriggerEvent, FVector Value)
{
	FAvidScriptGameplayEvent Event;
	Event.Type = EAvidScriptGameplayEventType::Input;
	Event.PrimaryId = ActionId;
	Event.SecondaryId = TriggerEvent;
	Event.VectorValue = FVector3f(
		static_cast<float>(Value.X),
		static_cast<float>(Value.Y),
		static_cast<float>(Value.Z));
	return DispatchGameplayEvent(Event);
}

void UAvidScriptComponent::BeginPlay()
{
	Super::BeginPlay();

	RuntimeStats = FAvidScriptComponentRuntimeStats();

	if (!RegisterOwner())
	{
		SetComponentTickEnabled(false);
		return;
	}

	RuntimeSession = MakeUnique<FAvidScriptRuntimeSession>();

	FAvidScriptWasmSmokeResult Result;
	if (!LoadConfiguredScriptModule(Result))
	{
		RecordRuntimeFailure(Result);
		ReleaseRuntime();
		SetComponentTickEnabled(false);
		return;
	}

	const FAvidScriptRuntimeSessionSnapshot Snapshot = RuntimeSession->GetSnapshot();
	RuntimeStats.bRuntimeLoaded = Snapshot.bHasActiveRuntime;
	RuntimeStats.bBeginPlayCalled = Result.bBeginPlayCalled;
	RuntimeStats.TickCallCount = Snapshot.TickCallCount;
	RuntimeStats.TimerCallbackCount = Result.TimerCallbackCount;
	RuntimeStats.LastTimerCallbackId = Result.LastTimerCallbackId;
	RuntimeStats.LastTimerHandle = Result.LastTimerHandle;
	RuntimeStats.Metrics = Result.Metrics;
	RuntimeStats.ModuleId = Snapshot.ModuleId;
	CopyComponentEventStats(Result, RuntimeStats);
	BindOwnerGameplayDelegates();

	UE_LOG(
		LogAvidScriptComponent,
		Log,
		TEXT("AvidScript component start | owner=%s | handle=%llu | module=%s | runtime_init_ms=%.4f | load_ms=%.4f | instantiate_ms=%.4f | exec_env_ms=%.4f | begin_play_ms=%.4f"),
		*RuntimeStats.OwnerObjectPath,
		RuntimeStats.OwnerHandle.ToUInt64(),
		*Result.ModuleId,
		Result.Metrics.RuntimeInitMs,
		Result.Metrics.ModuleLoadMs,
		Result.Metrics.ModuleInstantiateMs,
		Result.Metrics.ExecEnvCreateMs,
		Result.Metrics.BeginPlayCallMs);
}
void UAvidScriptComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	FAvidScriptWasmSmokeResult UnloadResult;
	const bool bHadRuntime = RuntimeSession.IsValid() && RuntimeSession->GetSnapshot().bHasActiveRuntime;
	RuntimeStats.bComponentEndPlayObserved = true;

	ReleaseRuntime(&UnloadResult);
	ReleaseOwner();

	if (bHadRuntime)
	{
		UE_LOG(
			LogAvidScriptComponent,
			Log,
			TEXT("AvidScript component stop | owner=%s | handle=%llu | module=%s | ticks=%d | unload_ms=%.4f"),
			RuntimeStats.OwnerObjectPath.IsEmpty() ? TEXT("<none>") : *RuntimeStats.OwnerObjectPath,
			RuntimeStats.OwnerHandle.ToUInt64(),
			UnloadResult.ModuleId.IsEmpty() ? TEXT("<none>") : *UnloadResult.ModuleId,
			UnloadResult.TickCallCount,
			UnloadResult.Metrics.UnloadMs);
	}

	Super::EndPlay(EndPlayReason);
}
void UAvidScriptComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	if (RuntimeSession.IsValid() &&
		RuntimeSession->GetSnapshot().LifecycleState == EAvidScriptLifecycleState::Running)
	{
		FAvidScriptWasmSmokeResult Result;
		const int32 PreviousTickCallCount = RuntimeStats.TickCallCount;
		if (RuntimeSession->Tick(DeltaTime, Result))
		{
			const FAvidScriptRuntimeSessionSnapshot Snapshot = RuntimeSession->GetSnapshot();
			RuntimeStats.bRuntimeLoaded = Snapshot.bHasActiveRuntime;
			RuntimeStats.bBeginPlayCalled = Result.bBeginPlayCalled;
			RuntimeStats.TickCallCount = Snapshot.TickCallCount;
			RuntimeStats.TimerCallbackCount = Result.TimerCallbackCount;
			RuntimeStats.LastTimerCallbackId = Result.LastTimerCallbackId;
			RuntimeStats.LastTimerHandle = Result.LastTimerHandle;
			RuntimeStats.Metrics = Result.Metrics;
			RuntimeStats.ModuleId = Snapshot.ModuleId;
			CopyComponentEventStats(Result, RuntimeStats);

			if (PreviousTickCallCount == 0 && RuntimeStats.TickCallCount > 0)
			{
				UE_LOG(
					LogAvidScriptComponent,
					Log,
					TEXT("AvidScript component tick | owner=%s | handle=%llu | module=%s | ticks=%d | tick_ms=%.4f"),
					RuntimeStats.OwnerObjectPath.IsEmpty() ? TEXT("<none>") : *RuntimeStats.OwnerObjectPath,
					RuntimeStats.OwnerHandle.ToUInt64(),
					*Result.ModuleId,
					RuntimeStats.TickCallCount,
					Result.Metrics.TickCallMs);
			}
		}
		else
		{
			RecordRuntimeFailure(Result);
			ReleaseRuntime();
			SetComponentTickEnabled(false);
		}
	}

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}
bool UAvidScriptComponent::RegisterOwner()
{
	AActor* Owner = GetOwner();
	FAvidScriptObjectHandleResult RegisterResult;
	OwnerHandle = ObjectRegistry.RegisterObject(Owner, RegisterResult);

	RuntimeStats.bOwnerRegistered = RegisterResult.bSucceeded;
	RuntimeStats.OwnerHandle = OwnerHandle;
	RuntimeStats.OwnerObjectPath = RegisterResult.ObjectPath;

	if (!RegisterResult.bSucceeded)
	{
		RuntimeStats.LastErrorMessage = RegisterResult.ErrorMessage;
		UE_LOG(LogAvidScriptComponent, Warning, TEXT("%s"), *RegisterResult.ErrorMessage);
		return false;
	}

	return true;
}

void UAvidScriptComponent::ReleaseOwner()
{
	if (!OwnerHandle.IsValid() || RuntimeStats.bOwnerReleased)
	{
		return;
	}

	FAvidScriptObjectHandleResult ReleaseResult;
	if (ObjectRegistry.ReleaseHandle(OwnerHandle, ReleaseResult))
	{
		RuntimeStats.bOwnerReleased = true;
		RuntimeStats.OwnerHandle = OwnerHandle;
		RuntimeStats.OwnerObjectPath = ReleaseResult.ObjectPath;
	}
	else
	{
		RuntimeStats.LastErrorMessage = ReleaseResult.ErrorMessage;
		UE_LOG(LogAvidScriptComponent, Warning, TEXT("%s"), *ReleaseResult.ErrorMessage);
	}
}

void UAvidScriptComponent::BindOwnerGameplayDelegates()
{
	AActor* Owner = GetOwner();
	if (!IsValid(Owner) || RuntimeStats.bCollisionDelegatesBound || !RuntimeSession.IsValid() ||
		RuntimeSession->GetSnapshot().LifecycleState != EAvidScriptLifecycleState::Running)
	{
		return;
	}

	Owner->OnActorBeginOverlap.AddUniqueDynamic(this, &UAvidScriptComponent::HandleOwnerBeginOverlap);
	Owner->OnActorEndOverlap.AddUniqueDynamic(this, &UAvidScriptComponent::HandleOwnerEndOverlap);
	Owner->OnActorHit.AddUniqueDynamic(this, &UAvidScriptComponent::HandleOwnerHit);
	RuntimeStats.bCollisionDelegatesBound = true;
}

void UAvidScriptComponent::UnbindOwnerGameplayDelegates()
{
	if (AActor* Owner = GetOwner(); IsValid(Owner))
	{
		Owner->OnActorBeginOverlap.RemoveDynamic(this, &UAvidScriptComponent::HandleOwnerBeginOverlap);
		Owner->OnActorEndOverlap.RemoveDynamic(this, &UAvidScriptComponent::HandleOwnerEndOverlap);
		Owner->OnActorHit.RemoveDynamic(this, &UAvidScriptComponent::HandleOwnerHit);
	}
	RuntimeStats.bCollisionDelegatesBound = false;
}

bool UAvidScriptComponent::DispatchGameplayEvent(const FAvidScriptGameplayEvent& Event)
{
	if (!RuntimeSession.IsValid() ||
		RuntimeSession->GetSnapshot().LifecycleState != EAvidScriptLifecycleState::Running)
	{
		RuntimeStats.LastErrorMessage = TEXT("AvidScript typed gameplay event rejected because the component session is not running.");
		return false;
	}

	FAvidScriptWasmSmokeResult Result;
	if (!RuntimeSession->DispatchGameplayEvent(Event, Result))
	{
		RecordRuntimeFailure(Result);
		if (Result.ErrorCategory != TEXT("invalid_argument"))
		{
			ReleaseRuntime();
			SetComponentTickEnabled(false);
		}
		return false;
	}

	RuntimeStats.bRuntimeLoaded = RuntimeSession->GetSnapshot().bHasActiveRuntime;
	RuntimeStats.Metrics = Result.Metrics;
	RuntimeStats.ModuleId = Result.ModuleId;
	CopyComponentEventStats(Result, RuntimeStats);
	if (Event.Type == EAvidScriptGameplayEventType::Input)
	{
		RuntimeStats.LastInputActionId = Event.PrimaryId;
		RuntimeStats.LastInputTriggerEvent = Event.SecondaryId;
		RuntimeStats.LastInputValue = FVector(Event.VectorValue);
	}
	return true;
}

bool UAvidScriptComponent::DispatchOwnerGameplayEvent(
	EAvidScriptGameplayEventType EventType,
	AActor* OtherActor,
	const FVector& VectorValue)
{
	if (!IsValid(OtherActor) || OtherActor == GetOwner() || !RuntimeSession.IsValid() ||
		RuntimeSession->GetSnapshot().LifecycleState != EAvidScriptLifecycleState::Running)
	{
		return false;
	}

	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle OtherHandle = ObjectRegistry.RegisterObject(OtherActor, RegisterResult);
	if (!RegisterResult.bSucceeded)
	{
		RuntimeStats.LastErrorMessage = RegisterResult.ErrorMessage;
		UE_LOG(LogAvidScriptComponent, Warning, TEXT("%s"), *RegisterResult.ErrorMessage);
		return false;
	}
	if (!(OtherHandle == OwnerHandle))
	{
		GameplayObjectHandleValues.Add(OtherHandle.ToUInt64());
	}

	FAvidScriptGameplayEvent Event;
	Event.Type = EventType;
	Event.ObjectHandle = OtherHandle;
	Event.VectorValue = FVector3f(
		static_cast<float>(VectorValue.X),
		static_cast<float>(VectorValue.Y),
		static_cast<float>(VectorValue.Z));

	return DispatchGameplayEvent(Event);
}

void UAvidScriptComponent::HandleOwnerBeginOverlap(AActor* OverlappedActor, AActor* OtherActor)
{
	if (OverlappedActor == GetOwner() && IsValid(OtherActor))
	{
		DispatchOwnerGameplayEvent(
			EAvidScriptGameplayEventType::BeginOverlap,
			OtherActor,
			OtherActor->GetActorLocation());
	}
}

void UAvidScriptComponent::HandleOwnerEndOverlap(AActor* OverlappedActor, AActor* OtherActor)
{
	if (OverlappedActor == GetOwner() && IsValid(OtherActor))
	{
		DispatchOwnerGameplayEvent(
			EAvidScriptGameplayEventType::EndOverlap,
			OtherActor,
			OtherActor->GetActorLocation());
	}
}

void UAvidScriptComponent::HandleOwnerHit(
	AActor* SelfActor,
	AActor* OtherActor,
	FVector NormalImpulse,
	const FHitResult& Hit)
{
	static_cast<void>(Hit);
	if (SelfActor == GetOwner())
	{
		DispatchOwnerGameplayEvent(EAvidScriptGameplayEventType::Hit, OtherActor, NormalImpulse);
	}
}

void UAvidScriptComponent::ReleaseGameplayObjectHandles()
{
	for (const uint64 HandleValue : GameplayObjectHandleValues)
	{
		const FAvidScriptObjectHandle Handle{
			static_cast<uint32>(HandleValue),
			static_cast<uint32>(HandleValue >> 32) };
		FAvidScriptObjectHandleResult ReleaseResult;
		ObjectRegistry.ReleaseHandle(Handle, ReleaseResult);
	}
	GameplayObjectHandleValues.Reset();
}

void UAvidScriptComponent::RecordRuntimeFailure(const FAvidScriptWasmSmokeResult& Result)
{
	const FAvidScriptRuntimeSessionSnapshot Snapshot = RuntimeSession.IsValid()
		? RuntimeSession->GetSnapshot()
		: FAvidScriptRuntimeSessionSnapshot();
	RuntimeStats.LastErrorMessage = Result.ErrorMessage;
	RuntimeStats.bRuntimeLoaded = Snapshot.bHasActiveRuntime;
	RuntimeStats.bBeginPlayCalled = Result.bBeginPlayCalled;
	RuntimeStats.TickCallCount = FMath::Max(Snapshot.TickCallCount, Result.TickCallCount);
	RuntimeStats.TimerCallbackCount = FMath::Max(RuntimeStats.TimerCallbackCount, Result.TimerCallbackCount);
	if (Result.LastTimerHandle > 0)
	{
		RuntimeStats.LastTimerCallbackId = Result.LastTimerCallbackId;
		RuntimeStats.LastTimerHandle = Result.LastTimerHandle;
	}
	RuntimeStats.Metrics = Result.Metrics;
	RuntimeStats.ModuleId = Result.ModuleId.IsEmpty() ? Snapshot.ModuleId : Result.ModuleId;
	CopyComponentEventStats(Result, RuntimeStats);

	UE_LOG(LogAvidScriptComponent, Warning, TEXT("%s"), *Result.ErrorMessage);
}

void UAvidScriptComponent::ReleaseRuntime(FAvidScriptWasmSmokeResult* OutUnloadResult)
{
	FAvidScriptWasmSmokeResult LocalUnloadResult;
	FAvidScriptWasmSmokeResult& UnloadResult = OutUnloadResult != nullptr ? *OutUnloadResult : LocalUnloadResult;

	UnbindOwnerGameplayDelegates();
	if (RuntimeSession.IsValid())
	{
		if (!RuntimeSession->StopAndUnload(UnloadResult))
		{
			RecordRuntimeFailure(UnloadResult);
		}
		RuntimeSession.Reset();
	}
	else
	{
		UnloadResult = FAvidScriptWasmSmokeResult();
	}
	ReleaseGameplayObjectHandles();

	RuntimeStats.Metrics = UnloadResult.Metrics;
	RuntimeStats.bEndPlayCalled = RuntimeStats.bEndPlayCalled || UnloadResult.bEndPlayCalled;
	RuntimeStats.TickCallCount = FMath::Max(RuntimeStats.TickCallCount, UnloadResult.TickCallCount);
	RuntimeStats.TimerCallbackCount = FMath::Max(RuntimeStats.TimerCallbackCount, UnloadResult.TimerCallbackCount);
	if (UnloadResult.LastTimerHandle > 0)
	{
		RuntimeStats.LastTimerCallbackId = UnloadResult.LastTimerCallbackId;
		RuntimeStats.LastTimerHandle = UnloadResult.LastTimerHandle;
	}
	CopyComponentEventStats(UnloadResult, RuntimeStats);
	RuntimeStats.bRuntimeLoaded = false;
}
