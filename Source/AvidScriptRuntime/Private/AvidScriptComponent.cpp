#include "AvidScriptComponent.h"

#include "AvidScriptWasmReload.h"

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
	if (!Runtime.IsValid())
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.ErrorCategory = TEXT("invalid_state");
		OutResult.ErrorMessage = TEXT("AvidScript component runtime has not been allocated.");
		OutResult.NextAction = TEXT("create a runtime before loading script modules");
		return false;
	}

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &ObjectRegistry;
	HostContext.OwnerHandle = OwnerHandle;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	Runtime->SetHostContext(HostContext);

	RuntimeStats.ScriptManifestPath = ResolveScriptManifestPath();
	if (RuntimeStats.ScriptManifestPath.IsEmpty())
	{
		return Runtime->LoadEmbeddedSmokeModule(OutResult);
	}

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(RuntimeStats.ScriptManifestPath, Manifest, Bytecode, LoadResult))
	{
		SetComponentManifestLoadFailure(LoadResult, OutResult);
		return false;
	}

	if (!Runtime->LoadModule(Bytecode.GetData(), Bytecode.Num(), Manifest.ModuleId, OutResult))
	{
		return false;
	}

	if (!Runtime->ValidateRequiredExports(Manifest.RequiredExports, OutResult))
	{
		Runtime->Unload();
		return false;
	}

	RuntimeStats.ModuleId = Manifest.ModuleId;
	return true;
}

bool UAvidScriptComponent::ResolveOwnerActor(AActor*& OutOwner, FAvidScriptObjectHandleResult& OutResult) const
{
	OutOwner = ObjectRegistry.ResolveObject<AActor>(OwnerHandle, OutResult);
	return OutOwner != nullptr;
}

bool UAvidScriptComponent::DispatchScriptEvent(int32 EventId, float Value)
{
	if (!bPlayActive || !Runtime.IsValid())
	{
		RuntimeStats.LastErrorMessage = TEXT("AvidScript gameplay event rejected because the component runtime is not active.");
		return false;
	}

	FAvidScriptWasmSmokeResult Result;
	if (!Runtime->DispatchEvent(EventId, Value, Result))
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

	RuntimeStats.Metrics = Result.Metrics;
	RuntimeStats.ModuleId = Result.ModuleId;
	CopyComponentEventStats(Result, RuntimeStats);
	return true;
}

void UAvidScriptComponent::BeginPlay()
{
	Super::BeginPlay();

	RuntimeStats = FAvidScriptComponentRuntimeStats();
	bPlayActive = false;

	if (!RegisterOwner())
	{
		SetComponentTickEnabled(false);
		return;
	}

	Runtime = MakeUnique<FAvidScriptWasmRuntimeInstance>();

	FAvidScriptWasmSmokeResult Result;
	if (!LoadConfiguredScriptModule(Result) || !Runtime->BeginPlay(Result))
	{
		RecordRuntimeFailure(Result);
		ReleaseRuntime();
		SetComponentTickEnabled(false);
		return;
	}

	bPlayActive = true;
	RuntimeStats.bRuntimeLoaded = Runtime->IsLoaded();
	RuntimeStats.bBeginPlayCalled = Result.bBeginPlayCalled;
	RuntimeStats.TickCallCount = Result.TickCallCount;
	RuntimeStats.TimerCallbackCount = Result.TimerCallbackCount;
	RuntimeStats.LastTimerCallbackId = Result.LastTimerCallbackId;
	RuntimeStats.LastTimerHandle = Result.LastTimerHandle;
	RuntimeStats.Metrics = Result.Metrics;
	RuntimeStats.ModuleId = Result.ModuleId;
	CopyComponentEventStats(Result, RuntimeStats);

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
	const bool bHadRuntime = bPlayActive || Runtime.IsValid();
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
	if (bPlayActive && Runtime.IsValid())
	{
		FAvidScriptWasmSmokeResult Result;
		const int32 PreviousTickCallCount = RuntimeStats.TickCallCount;
		if (Runtime->Tick(DeltaTime, Result))
		{
			RuntimeStats.bRuntimeLoaded = Runtime->IsLoaded();
			RuntimeStats.bBeginPlayCalled = Result.bBeginPlayCalled;
			RuntimeStats.TickCallCount = Result.TickCallCount;
			RuntimeStats.TimerCallbackCount = Result.TimerCallbackCount;
			RuntimeStats.LastTimerCallbackId = Result.LastTimerCallbackId;
			RuntimeStats.LastTimerHandle = Result.LastTimerHandle;
			RuntimeStats.Metrics = Result.Metrics;
			RuntimeStats.ModuleId = Result.ModuleId;
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

void UAvidScriptComponent::RecordRuntimeFailure(const FAvidScriptWasmSmokeResult& Result)
{
	RuntimeStats.LastErrorMessage = Result.ErrorMessage;
	RuntimeStats.bRuntimeLoaded = Runtime.IsValid() && Runtime->IsLoaded();
	RuntimeStats.bBeginPlayCalled = Result.bBeginPlayCalled;
	RuntimeStats.TickCallCount = Runtime.IsValid() ? Runtime->GetTickCallCount() : Result.TickCallCount;
	RuntimeStats.TimerCallbackCount = FMath::Max(RuntimeStats.TimerCallbackCount, Result.TimerCallbackCount);
	if (Result.LastTimerHandle > 0)
	{
		RuntimeStats.LastTimerCallbackId = Result.LastTimerCallbackId;
		RuntimeStats.LastTimerHandle = Result.LastTimerHandle;
	}
	RuntimeStats.Metrics = Result.Metrics;
	RuntimeStats.ModuleId = Result.ModuleId;
	CopyComponentEventStats(Result, RuntimeStats);

	UE_LOG(LogAvidScriptComponent, Warning, TEXT("%s"), *Result.ErrorMessage);
}

void UAvidScriptComponent::ReleaseRuntime(FAvidScriptWasmSmokeResult* OutUnloadResult)
{
	FAvidScriptWasmSmokeResult LocalUnloadResult;
	FAvidScriptWasmSmokeResult& UnloadResult = OutUnloadResult != nullptr ? *OutUnloadResult : LocalUnloadResult;

	if (Runtime.IsValid())
	{
		if (Runtime->HasBegunPlay())
		{
			FAvidScriptWasmSmokeResult EndPlayResult;
			if (Runtime->EndPlay(EndPlayResult))
			{
				RuntimeStats.bEndPlayCalled = RuntimeStats.bEndPlayCalled || EndPlayResult.bEndPlayCalled;
				RuntimeStats.Metrics = EndPlayResult.Metrics;
			}
			else
			{
				RecordRuntimeFailure(EndPlayResult);
			}
		}

		Runtime->Unload(UnloadResult);
		Runtime.Reset();
	}
	else
	{
		UnloadResult = FAvidScriptWasmSmokeResult();
	}

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
	bPlayActive = false;
	RuntimeStats.bRuntimeLoaded = false;
}
