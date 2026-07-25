#include "AvidScriptWorldSubsystem.h"

#include "Engine/World.h"
#include "Misc/ScopeExit.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWorldSubsystem, Log, All);

namespace
{
void CopyRuntimeResultToStats(const FAvidScriptWasmSmokeResult& Result, FAvidScriptWorldRuntimeStats& Stats)
{
	Stats.bBeginPlayCalled = Result.bBeginPlayCalled;
	Stats.TickCallCount = Result.TickCallCount;
	Stats.Metrics = Result.Metrics;
}

void CopyWorldSessionLoadResult(
	const FAvidScriptWasmReloadResult& ReloadResult,
	FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = ReloadResult.RuntimeResult;
	if (!ReloadResult.bSucceeded)
	{
		OutResult.ExportName = ReloadResult.ExportName;
		OutResult.ErrorCategory = ReloadResult.ErrorCategory;
		OutResult.NextAction = ReloadResult.NextAction;
		OutResult.ErrorMessage = ReloadResult.ErrorMessage;
	}
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
	bRuntimeReleaseDeferred = false;
	bRuntimeReleaseInProgress = false;
	RuntimeSession = MakeUnique<FAvidScriptRuntimeSession>();

	FAvidScriptWasmReloadResult ReloadResult;
	if (!RuntimeSession->LoadEmbeddedSmoke(ReloadResult))
	{
		FAvidScriptWasmSmokeResult Result;
		CopyWorldSessionLoadResult(ReloadResult, Result);
		RecordFailure(Result);
		ReleaseRuntime();
		return;
	}

	const FAvidScriptWasmSmokeResult& Result = ReloadResult.RuntimeResult;
	const FAvidScriptRuntimeSessionSnapshot Snapshot = RuntimeSession->GetSnapshot();
	RuntimeStats.bRuntimeLoaded = Snapshot.bHasActiveRuntime;
	CopyRuntimeResultToStats(Result, RuntimeStats);
	RuntimeStats.TickCallCount = Snapshot.TickCallCount;

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
	const bool bHadRuntime = RuntimeSession.IsValid() && RuntimeSession->GetSnapshot().bHasActiveRuntime;
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
	ON_SCOPE_EXIT
	{
		FlushDeferredRuntimeRelease();
	};

	if (RuntimeSession.IsValid() &&
		RuntimeSession->GetSnapshot().LifecycleState == EAvidScriptLifecycleState::Running)
	{
		FAvidScriptWasmSmokeResult Result;
		const int32 PreviousTickCallCount = RuntimeStats.TickCallCount;
		if (RuntimeSession->Tick(DeltaTime, Result))
		{
			const FAvidScriptRuntimeSessionSnapshot Snapshot = RuntimeSession->GetSnapshot();
			RuntimeStats.bRuntimeLoaded = Snapshot.bHasActiveRuntime;
			CopyRuntimeResultToStats(Result, RuntimeStats);
			RuntimeStats.TickCallCount = Snapshot.TickCallCount;

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
	return RuntimeSession.IsValid() &&
		RuntimeSession->GetSnapshot().LifecycleState == EAvidScriptLifecycleState::Running;
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
	const FAvidScriptRuntimeSessionSnapshot Snapshot = RuntimeSession.IsValid()
		? RuntimeSession->GetSnapshot()
		: FAvidScriptRuntimeSessionSnapshot();
	RuntimeStats.LastErrorMessage = Result.ErrorMessage;
	RuntimeStats.bRuntimeLoaded = Snapshot.bHasActiveRuntime;
	CopyRuntimeResultToStats(Result, RuntimeStats);
	RuntimeStats.TickCallCount = FMath::Max(Snapshot.TickCallCount, Result.TickCallCount);

	UE_LOG(LogAvidScriptWorldSubsystem, Warning, TEXT("%s"), *Result.ErrorMessage);
}

void UAvidScriptWorldSubsystem::ReleaseRuntime(FAvidScriptWasmSmokeResult* OutUnloadResult)
{
	FAvidScriptWasmSmokeResult LocalUnloadResult;
	FAvidScriptWasmSmokeResult& UnloadResult = OutUnloadResult != nullptr ? *OutUnloadResult : LocalUnloadResult;

	if (bRuntimeReleaseInProgress
		|| (RuntimeSession.IsValid() && RuntimeSession->IsOperationActive()))
	{
		bRuntimeReleaseDeferred = true;
		UnloadResult = FAvidScriptWasmSmokeResult();
		UnloadResult.ModuleId = RuntimeSession.IsValid() ? RuntimeSession->GetLiveModuleId() : FString();
		UnloadResult.ExportName = TEXT("<unload>");
		UnloadResult.ErrorCategory = TEXT("reentrant_operation");
		UnloadResult.NextAction = TEXT("defer world Runtime release until the active script callback returns");
		UnloadResult.ErrorMessage = TEXT("AvidScript world subsystem deferred Runtime release while a script operation was active.");
		return;
	}

	bRuntimeReleaseInProgress = true;
	ON_SCOPE_EXIT
	{
		bRuntimeReleaseInProgress = false;
	};

	if (RuntimeSession.IsValid())
	{
		if (!RuntimeSession->StopAndUnload(UnloadResult))
		{
			RecordFailure(UnloadResult);
			if (RuntimeSession->IsOperationActive())
			{
				bRuntimeReleaseDeferred = true;
				return;
			}
		}
		RuntimeSession.Reset();
	}
	else
	{
		UnloadResult = FAvidScriptWasmSmokeResult();
	}

	RuntimeStats.Metrics = UnloadResult.Metrics;
	RuntimeStats.TickCallCount = FMath::Max(RuntimeStats.TickCallCount, UnloadResult.TickCallCount);
	RuntimeStats.bRuntimeLoaded = false;
}

void UAvidScriptWorldSubsystem::FlushDeferredRuntimeRelease()
{
	if (!bRuntimeReleaseDeferred
		|| bRuntimeReleaseInProgress
		|| (RuntimeSession.IsValid() && RuntimeSession->IsOperationActive()))
	{
		return;
	}

	bRuntimeReleaseDeferred = false;
	ReleaseRuntime();
}
