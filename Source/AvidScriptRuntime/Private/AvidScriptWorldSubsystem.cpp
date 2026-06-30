#include "AvidScriptWorldSubsystem.h"

#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWorldSubsystem, Log, All);

namespace
{
void CopyRuntimeResultToStats(const FAvidScriptWasmSmokeResult& Result, FAvidScriptWorldRuntimeStats& Stats)
{
	Stats.bBeginPlayCalled = Result.bBeginPlayCalled;
	Stats.TickCallCount = Result.TickCallCount;
	Stats.Metrics = Result.Metrics;
}
} // namespace

bool UAvidScriptWorldSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UAvidScriptWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	RuntimeStats = FAvidScriptWorldRuntimeStats();
	Runtime = MakeUnique<FAvidScriptWasmRuntimeInstance>();

	FAvidScriptWasmSmokeResult Result;
	if (!Runtime->LoadEmbeddedSmokeModule(Result) || !Runtime->BeginPlay(Result))
	{
		RecordFailure(Result);
		ReleaseRuntime();
		return;
	}

	bWorldPlayActive = true;
	RuntimeStats.bRuntimeLoaded = Runtime->IsLoaded();
	CopyRuntimeResultToStats(Result, RuntimeStats);

	UE_LOG(
		LogAvidScriptWorldSubsystem,
		Log,
		TEXT("AvidScript packaged smoke start | world=%s | module=%s | runtime_init_ms=%.4f | load_ms=%.4f | instantiate_ms=%.4f | exec_env_ms=%.4f | begin_play_ms=%.4f"),
		*InWorld.GetName(),
		*Result.ModuleId,
		Result.Metrics.RuntimeInitMs,
		Result.Metrics.ModuleLoadMs,
		Result.Metrics.ModuleInstantiateMs,
		Result.Metrics.ExecEnvCreateMs,
		Result.Metrics.BeginPlayCallMs);
}

void UAvidScriptWorldSubsystem::OnWorldEndPlay(UWorld& InWorld)
{
	FAvidScriptWasmSmokeResult UnloadResult;
	const bool bHadRuntime = bWorldPlayActive || Runtime.IsValid();
	ReleaseRuntime(&UnloadResult);
	RuntimeStats.bEndPlayCalled = true;

	if (bHadRuntime)
	{
		UE_LOG(
			LogAvidScriptWorldSubsystem,
			Log,
			TEXT("AvidScript packaged smoke stop | world=%s | module=%s | ticks=%d | unload_ms=%.4f"),
			*InWorld.GetName(),
			UnloadResult.ModuleId.IsEmpty() ? TEXT("<none>") : *UnloadResult.ModuleId,
			UnloadResult.TickCallCount,
			UnloadResult.Metrics.UnloadMs);
	}
}

void UAvidScriptWorldSubsystem::Tick(float DeltaTime)
{
	if (bWorldPlayActive && Runtime.IsValid())
	{
		FAvidScriptWasmSmokeResult Result;
		const int32 PreviousTickCallCount = RuntimeStats.TickCallCount;
		if (Runtime->Tick(DeltaTime, Result))
		{
			RuntimeStats.bRuntimeLoaded = Runtime->IsLoaded();
			CopyRuntimeResultToStats(Result, RuntimeStats);

			if (PreviousTickCallCount == 0 && RuntimeStats.TickCallCount > 0)
			{
				UE_LOG(
					LogAvidScriptWorldSubsystem,
					Log,
					TEXT("AvidScript packaged smoke tick | world=%s | module=%s | ticks=%d | tick_ms=%.4f"),
					GetWorld() != nullptr ? *GetWorld()->GetName() : TEXT("<none>"),
					*Result.ModuleId,
					RuntimeStats.TickCallCount,
					Result.Metrics.TickCallMs);
			}
		}
		else
		{
			RecordFailure(Result);
			ReleaseRuntime();
		}
	}

	Super::Tick(DeltaTime);
}

bool UAvidScriptWorldSubsystem::IsTickable() const
{
	return bWorldPlayActive && Runtime.IsValid() && Runtime->IsLoaded();
}

TStatId UAvidScriptWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UAvidScriptWorldSubsystem, STATGROUP_Tickables);
}

void UAvidScriptWorldSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		OnWorldEndPlay(*World);
	}
	else
	{
		ReleaseRuntime();
	}

	Super::Deinitialize();
}

void UAvidScriptWorldSubsystem::RecordFailure(const FAvidScriptWasmSmokeResult& Result)
{
	RuntimeStats.LastErrorMessage = Result.ErrorMessage;
	RuntimeStats.bRuntimeLoaded = Runtime.IsValid() && Runtime->IsLoaded();
	CopyRuntimeResultToStats(Result, RuntimeStats);
	RuntimeStats.TickCallCount = Runtime.IsValid() ? Runtime->GetTickCallCount() : Result.TickCallCount;

	UE_LOG(LogAvidScriptWorldSubsystem, Warning, TEXT("%s"), *Result.ErrorMessage);
}

void UAvidScriptWorldSubsystem::ReleaseRuntime(FAvidScriptWasmSmokeResult* OutUnloadResult)
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
	bWorldPlayActive = false;
	RuntimeStats.bRuntimeLoaded = false;
}
