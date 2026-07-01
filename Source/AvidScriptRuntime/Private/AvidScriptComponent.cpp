#include "AvidScriptComponent.h"

#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptComponent, Log, All);

UAvidScriptComponent::UAvidScriptComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

bool UAvidScriptComponent::ResolveOwnerActor(AActor*& OutOwner, FAvidScriptObjectHandleResult& OutResult) const
{
	OutOwner = ObjectRegistry.ResolveObject<AActor>(OwnerHandle, OutResult);
	return OutOwner != nullptr;
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
	if (!Runtime->LoadEmbeddedSmokeModule(Result) || !Runtime->BeginPlay(Result))
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
	RuntimeStats.Metrics = Result.Metrics;

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

	ReleaseRuntime(&UnloadResult);
	ReleaseOwner();
	RuntimeStats.bEndPlayCalled = true;

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
			RuntimeStats.Metrics = Result.Metrics;

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
	RuntimeStats.Metrics = Result.Metrics;

	UE_LOG(LogAvidScriptComponent, Warning, TEXT("%s"), *Result.ErrorMessage);
}

void UAvidScriptComponent::ReleaseRuntime(FAvidScriptWasmSmokeResult* OutUnloadResult)
{
	FAvidScriptWasmSmokeResult LocalUnloadResult;
	FAvidScriptWasmSmokeResult& UnloadResult = OutUnloadResult != nullptr ? *OutUnloadResult : LocalUnloadResult;

	if (Runtime.IsValid())
	{
		Runtime->Unload(UnloadResult);
		Runtime.Reset();
	}
	else
	{
		UnloadResult = FAvidScriptWasmSmokeResult();
	}

	RuntimeStats.Metrics = UnloadResult.Metrics;
	RuntimeStats.TickCallCount = FMath::Max(RuntimeStats.TickCallCount, UnloadResult.TickCallCount);
	bPlayActive = false;
	RuntimeStats.bRuntimeLoaded = false;
}
