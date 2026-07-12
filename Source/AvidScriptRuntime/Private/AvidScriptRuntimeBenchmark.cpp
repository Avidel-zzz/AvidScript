#include "AvidScriptRuntimeBenchmark.h"

#include "AvidScriptActorBinding.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmRuntime.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptRuntimeBenchmark, Log, All);

namespace
{
constexpr double AvidScriptMinimumBenchmarkMs = 0.0001;

FAvidScriptBenchmarkStats CalculateStats(TArray<double> Samples)
{
	FAvidScriptBenchmarkStats Stats;
	Stats.Count = Samples.Num();

	if (Samples.IsEmpty())
	{
		return Stats;
	}

	Samples.Sort();

	double TotalMs = 0.0;
	for (const double Sample : Samples)
	{
		TotalMs += Sample;
	}

	const int32 LastIndex = Samples.Num() - 1;
	const int32 P50Index = FMath::Clamp(FMath::RoundToInt(static_cast<double>(LastIndex) * 0.50), 0, LastIndex);
	const int32 P95Index = FMath::Clamp(FMath::CeilToInt(static_cast<double>(LastIndex) * 0.95), 0, LastIndex);

	Stats.MinMs = Samples[0];
	Stats.MaxMs = Samples[LastIndex];
	Stats.AvgMs = TotalMs / static_cast<double>(Samples.Num());
	Stats.P50Ms = Samples[P50Index];
	Stats.P95Ms = Samples[P95Index];
	return Stats;
}

double MeasureElapsedPerIterationMs(double StartSeconds, int32 Iterations)
{
	const int32 SafeIterations = FMath::Max(Iterations, 1);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	return FMath::Max(ElapsedMs / static_cast<double>(SafeIterations), AvidScriptMinimumBenchmarkMs);
}

bool CreateBenchmarkWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptHostBindingBenchmarkWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyBenchmarkWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}

	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}

	World->DestroyWorld(false);
	World = nullptr;
}

AActor* SpawnBenchmarkActor(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	AActor* Actor = World->SpawnActor<AActor>();
	if (Actor == nullptr)
	{
		return nullptr;
	}

	USceneComponent* RootComponent = NewObject<USceneComponent>(Actor, USceneComponent::StaticClass(), TEXT("AvidScriptBenchmarkRoot"));
	if (RootComponent == nullptr)
	{
		return nullptr;
	}

	Actor->SetRootComponent(RootComponent);
	Actor->AddInstanceComponent(RootComponent);
	RootComponent->RegisterComponentWithWorld(World);
	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0), false, nullptr, ETeleportType::TeleportPhysics);
	return Actor;
}

void AppendMetrics(
	const FAvidScriptWasmRuntimeMetrics& Metrics,
	TArray<double>& RuntimeInitSamples,
	TArray<double>& ModuleLoadSamples,
	TArray<double>& ModuleInstantiateSamples,
	TArray<double>& ExecEnvCreateSamples,
	TArray<double>& BeginPlayCallSamples,
	TArray<double>& TickCallSamples,
	TArray<double>& UnloadSamples)
{
	RuntimeInitSamples.Add(Metrics.RuntimeInitMs);
	ModuleLoadSamples.Add(Metrics.ModuleLoadMs);
	ModuleInstantiateSamples.Add(Metrics.ModuleInstantiateMs);
	ExecEnvCreateSamples.Add(Metrics.ExecEnvCreateMs);
	BeginPlayCallSamples.Add(Metrics.BeginPlayCallMs);
	TickCallSamples.Add(Metrics.TickCallMs);
	UnloadSamples.Add(Metrics.UnloadMs);
}

void SetFailureFromSmokeResult(
	const FAvidScriptWasmSmokeResult& SmokeResult,
	FAvidScriptRuntimeBenchmarkResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = SmokeResult.ErrorCategory;
	OutResult.ErrorMessage = SmokeResult.ErrorMessage;
	OutResult.Summary = FString::Printf(
		TEXT("runtime_microbenchmark_failed | category=%s | message=%s"),
		OutResult.ErrorCategory.IsEmpty() ? TEXT("<none>") : *OutResult.ErrorCategory,
		OutResult.ErrorMessage.IsEmpty() ? TEXT("<none>") : *OutResult.ErrorMessage);
}

void SetTimerSchedulerFailure(
	FAvidScriptTimerSchedulerBenchmarkResult& OutResult,
	const FString& ErrorCategory,
	const FString& ErrorMessage)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.Summary = FString::Printf(
		TEXT("timer_scheduler_benchmark_failed | category=%s | message=%s"),
		ErrorCategory.IsEmpty() ? TEXT("<none>") : *ErrorCategory,
		ErrorMessage.IsEmpty() ? TEXT("<none>") : *ErrorMessage);
}

void SetHostBindingFailure(
	FAvidScriptHostBindingBenchmarkResult& OutResult,
	const FString& ErrorCategory,
	const FString& Details,
	const FString& NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript host binding benchmark error | category=%s | details=%s | next=%s"),
		ErrorCategory.IsEmpty() ? TEXT("<none>") : *ErrorCategory,
		Details.IsEmpty() ? TEXT("<none>") : *Details,
		NextAction.IsEmpty() ? TEXT("<none>") : *NextAction);
	OutResult.Summary = FString::Printf(
		TEXT("host_binding_benchmark_failed | category=%s | message=%s"),
		OutResult.ErrorCategory.IsEmpty() ? TEXT("<none>") : *OutResult.ErrorCategory,
		*OutResult.ErrorMessage);
}
} // namespace

bool FAvidScriptRuntimeBenchmark::RunEmbeddedSmokeBenchmark(
	const FAvidScriptRuntimeBenchmarkOptions& Options,
	FAvidScriptRuntimeBenchmarkResult& OutResult)
{
	OutResult = FAvidScriptRuntimeBenchmarkResult();
	OutResult.WarmupCount = FMath::Max(Options.WarmupCount, 0);
	OutResult.SampleCount = FMath::Max(Options.SampleCount, 1);

	TArray<double> RuntimeInitSamples;
	TArray<double> ModuleLoadSamples;
	TArray<double> ModuleInstantiateSamples;
	TArray<double> ExecEnvCreateSamples;
	TArray<double> BeginPlayCallSamples;
	TArray<double> TickCallSamples;
	TArray<double> UnloadSamples;

	RuntimeInitSamples.Reserve(OutResult.SampleCount);
	ModuleLoadSamples.Reserve(OutResult.SampleCount);
	ModuleInstantiateSamples.Reserve(OutResult.SampleCount);
	ExecEnvCreateSamples.Reserve(OutResult.SampleCount);
	BeginPlayCallSamples.Reserve(OutResult.SampleCount);
	TickCallSamples.Reserve(OutResult.SampleCount);
	UnloadSamples.Reserve(OutResult.SampleCount);

	const int32 TotalRuns = OutResult.WarmupCount + OutResult.SampleCount;
	const float TickDeltaSeconds = Options.TickDeltaSeconds > 0.0f ? Options.TickDeltaSeconds : 1.0f / 60.0f;

	for (int32 RunIndex = 0; RunIndex < TotalRuns; ++RunIndex)
	{
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult SmokeResult;

		if (!Runtime.LoadEmbeddedSmokeModule(SmokeResult))
		{
			SetFailureFromSmokeResult(SmokeResult, OutResult);
			return false;
		}

		if (!Runtime.BeginPlay(SmokeResult))
		{
			SetFailureFromSmokeResult(SmokeResult, OutResult);
			return false;
		}

		if (!Runtime.Tick(TickDeltaSeconds, SmokeResult))
		{
			SetFailureFromSmokeResult(SmokeResult, OutResult);
			return false;
		}

		Runtime.Unload(SmokeResult);
		if (!SmokeResult.bUnloaded)
		{
			SetFailureFromSmokeResult(SmokeResult, OutResult);
			if (OutResult.ErrorMessage.IsEmpty())
			{
				OutResult.ErrorCategory = TEXT("unload_failed");
				OutResult.ErrorMessage = TEXT("Embedded runtime did not report a completed unload.");
			}
			return false;
		}

		if (RunIndex >= OutResult.WarmupCount)
		{
			AppendMetrics(
				SmokeResult.Metrics,
				RuntimeInitSamples,
				ModuleLoadSamples,
				ModuleInstantiateSamples,
				ExecEnvCreateSamples,
				BeginPlayCallSamples,
				TickCallSamples,
				UnloadSamples);
		}
	}

	OutResult.RuntimeInit = CalculateStats(RuntimeInitSamples);
	OutResult.ModuleLoad = CalculateStats(ModuleLoadSamples);
	OutResult.ModuleInstantiate = CalculateStats(ModuleInstantiateSamples);
	OutResult.ExecEnvCreate = CalculateStats(ExecEnvCreateSamples);
	OutResult.BeginPlayCall = CalculateStats(BeginPlayCallSamples);
	OutResult.TickCall = CalculateStats(TickCallSamples);
	OutResult.Unload = CalculateStats(UnloadSamples);
	OutResult.bSucceeded = true;
	OutResult.Summary = FString::Printf(
		TEXT("runtime_microbenchmark | warmup=%d | samples=%d | load_avg_ms=%.4f | instantiate_avg_ms=%.4f | begin_avg_ms=%.4f | tick_avg_ms=%.4f | tick_p95_ms=%.4f | unload_avg_ms=%.4f"),
		OutResult.WarmupCount,
		OutResult.SampleCount,
		OutResult.ModuleLoad.AvgMs,
		OutResult.ModuleInstantiate.AvgMs,
		OutResult.BeginPlayCall.AvgMs,
		OutResult.TickCall.AvgMs,
		OutResult.TickCall.P95Ms,
		OutResult.Unload.AvgMs);

	UE_LOG(LogAvidScriptRuntimeBenchmark, Display, TEXT("%s"), *OutResult.Summary);
	return true;
}

bool FAvidScriptRuntimeBenchmark::RunTimerSchedulerBenchmark(
	const FAvidScriptTimerSchedulerBenchmarkOptions& Options,
	FAvidScriptTimerSchedulerBenchmarkResult& OutResult)
{
	OutResult = FAvidScriptTimerSchedulerBenchmarkResult();
	OutResult.WarmupCount = FMath::Max(Options.WarmupCount, 0);
	OutResult.SampleCount = FMath::Max(Options.SampleCount, 1);
	OutResult.PendingTimerCount = FMath::Clamp(Options.PendingTimerCount, 1, 1023);
	OutResult.IterationsPerSample = FMath::Max(Options.IterationsPerSample, 1);

	TArray<double> IdleTickSamples;
	TArray<double> SetCancelChurnSamples;
	IdleTickSamples.Reserve(OutResult.SampleCount);
	SetCancelChurnSamples.Reserve(OutResult.SampleCount);

	const int32 TotalRuns = OutResult.WarmupCount + OutResult.SampleCount;
	for (int32 RunIndex = 0; RunIndex < TotalRuns; ++RunIndex)
	{
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult SmokeResult;
		if (!Runtime.LoadEmbeddedSmokeModule(SmokeResult) || !Runtime.BeginPlay(SmokeResult))
		{
			SetTimerSchedulerFailure(OutResult, SmokeResult.ErrorCategory, SmokeResult.ErrorMessage);
			return false;
		}

		for (int32 TimerIndex = 0; TimerIndex < OutResult.PendingTimerCount; ++TimerIndex)
		{
			if (Runtime.HandleTimerSetOnceImport(3600.0f, TimerIndex) <= 0)
			{
				SetTimerSchedulerFailure(
					OutResult,
					TEXT("timer_setup_failed"),
					FString::Printf(TEXT("Failed to register pending timer %d of %d."), TimerIndex + 1, OutResult.PendingTimerCount));
				return false;
			}
		}

		const double IdleTickStartSeconds = FPlatformTime::Seconds();
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			if (!Runtime.Tick(0.0f, SmokeResult))
			{
				SetTimerSchedulerFailure(OutResult, SmokeResult.ErrorCategory, SmokeResult.ErrorMessage);
				return false;
			}
		}
		const double IdleTickMs = MeasureElapsedPerIterationMs(IdleTickStartSeconds, OutResult.IterationsPerSample);

		const double ChurnStartSeconds = FPlatformTime::Seconds();
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			const int32 TimerHandle = Runtime.HandleTimerSetOnceImport(3600.0f, IterationIndex);
			if (TimerHandle <= 0 || Runtime.HandleTimerCancelImport(TimerHandle) != 1)
			{
				SetTimerSchedulerFailure(
					OutResult,
					TEXT("timer_churn_failed"),
					FString::Printf(TEXT("Set/cancel churn failed at iteration %d."), IterationIndex));
				return false;
			}
		}
		const double SetCancelChurnMs = MeasureElapsedPerIterationMs(ChurnStartSeconds, OutResult.IterationsPerSample);

		Runtime.Unload(SmokeResult);
		if (!SmokeResult.bUnloaded)
		{
			SetTimerSchedulerFailure(
				OutResult,
				SmokeResult.ErrorCategory.IsEmpty() ? FString(TEXT("unload_failed")) : SmokeResult.ErrorCategory,
				SmokeResult.ErrorMessage.IsEmpty() ? FString(TEXT("Timer benchmark runtime did not unload.")) : SmokeResult.ErrorMessage);
			return false;
		}

		if (RunIndex >= OutResult.WarmupCount)
		{
			IdleTickSamples.Add(IdleTickMs);
			SetCancelChurnSamples.Add(SetCancelChurnMs);
		}
	}

	OutResult.IdleTick = CalculateStats(IdleTickSamples);
	OutResult.SetCancelChurn = CalculateStats(SetCancelChurnSamples);
	OutResult.bSucceeded = true;
	OutResult.Summary = FString::Printf(
		TEXT("timer_scheduler_benchmark | warmup=%d | samples=%d | pending=%d | iterations=%d | idle_tick_avg_ms=%.6f | idle_tick_p95_ms=%.6f | set_cancel_avg_ms=%.6f | set_cancel_p95_ms=%.6f"),
		OutResult.WarmupCount,
		OutResult.SampleCount,
		OutResult.PendingTimerCount,
		OutResult.IterationsPerSample,
		OutResult.IdleTick.AvgMs,
		OutResult.IdleTick.P95Ms,
		OutResult.SetCancelChurn.AvgMs,
		OutResult.SetCancelChurn.P95Ms);

	UE_LOG(LogAvidScriptRuntimeBenchmark, Display, TEXT("%s"), *OutResult.Summary);
	return true;
}

bool FAvidScriptRuntimeBenchmark::RunHostBindingBenchmark(
	const FAvidScriptHostBindingBenchmarkOptions& Options,
	FAvidScriptHostBindingBenchmarkResult& OutResult)
{
	OutResult = FAvidScriptHostBindingBenchmarkResult();
	OutResult.WarmupCount = FMath::Max(Options.WarmupCount, 0);
	OutResult.SampleCount = FMath::Max(Options.SampleCount, 1);
	OutResult.IterationsPerSample = FMath::Max(Options.IterationsPerSample, 1);

	UWorld* World = nullptr;
	if (!CreateBenchmarkWorld(World))
	{
		SetHostBindingFailure(
			OutResult,
			TEXT("world_create_failed"),
			TEXT("Failed to create the host binding benchmark world."),
			TEXT("Run this benchmark in an initialized editor or commandlet context."));
		DestroyBenchmarkWorld(World);
		return false;
	}

	AActor* Actor = SpawnBenchmarkActor(World);
	if (Actor == nullptr)
	{
		SetHostBindingFailure(
			OutResult,
			TEXT("actor_spawn_failed"),
			TEXT("Failed to spawn the host binding benchmark actor."),
			TEXT("Verify the benchmark world can spawn actors and register a root component."));
		DestroyBenchmarkWorld(World);
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	if (!RegisterResult.bSucceeded || !ActorHandle.IsValid())
	{
		SetHostBindingFailure(
			OutResult,
			RegisterResult.ErrorCategory.IsEmpty() ? FString(TEXT("register_failed")) : RegisterResult.ErrorCategory,
			RegisterResult.ErrorMessage,
			RegisterResult.NextAction);
		DestroyBenchmarkWorld(World);
		return false;
	}

	TArray<double> DirectGetSamples;
	TArray<double> RegistryResolveSamples;
	TArray<double> BindingGetSamples;
	TArray<double> BindingSetSamples;
	DirectGetSamples.Reserve(OutResult.SampleCount);
	RegistryResolveSamples.Reserve(OutResult.SampleCount);
	BindingGetSamples.Reserve(OutResult.SampleCount);
	BindingSetSamples.Reserve(OutResult.SampleCount);

	const int32 TotalRuns = OutResult.WarmupCount + OutResult.SampleCount;
	for (int32 RunIndex = 0; RunIndex < TotalRuns; ++RunIndex)
	{
		const double DirectStartSeconds = FPlatformTime::Seconds();
		FVector LastDirectLocation = FVector::ZeroVector;
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			LastDirectLocation = Actor->GetActorLocation();
		}
		const double DirectGetMs = MeasureElapsedPerIterationMs(DirectStartSeconds, OutResult.IterationsPerSample);

		OutResult.LastReadLocation = LastDirectLocation;

		const double RegistryStartSeconds = FPlatformTime::Seconds();
		AActor* ResolvedActor = nullptr;
		FAvidScriptObjectHandleResult ResolveResult;
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			ResolvedActor = Registry.ResolveObject<AActor>(ActorHandle, ResolveResult);
			if (ResolvedActor == nullptr)
			{
				SetHostBindingFailure(
					OutResult,
					ResolveResult.ErrorCategory.IsEmpty() ? FString(TEXT("resolve_failed")) : ResolveResult.ErrorCategory,
					ResolveResult.ErrorMessage,
					ResolveResult.NextAction);
				DestroyBenchmarkWorld(World);
				return false;
			}
		}
		const double RegistryResolveMs = MeasureElapsedPerIterationMs(RegistryStartSeconds, OutResult.IterationsPerSample);

		const double BindingGetStartSeconds = FPlatformTime::Seconds();
		FVector BindingLocation = FVector::ZeroVector;
		FAvidScriptActorBindingResult BindingGetResult;
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			if (!FAvidScriptActorBinding::GetActorLocation(Registry, ActorHandle, BindingLocation, BindingGetResult))
			{
				SetHostBindingFailure(
					OutResult,
					BindingGetResult.ErrorCategory.IsEmpty() ? FString(TEXT("binding_get_failed")) : BindingGetResult.ErrorCategory,
					BindingGetResult.ErrorMessage,
					BindingGetResult.NextAction);
				DestroyBenchmarkWorld(World);
				return false;
			}
		}
		const double BindingGetMs = MeasureElapsedPerIterationMs(BindingGetStartSeconds, OutResult.IterationsPerSample);

		const double BindingSetStartSeconds = FPlatformTime::Seconds();
		FAvidScriptActorBindingResult BindingSetResult;
		for (int32 IterationIndex = 0; IterationIndex < OutResult.IterationsPerSample; ++IterationIndex)
		{
			const FVector TargetLocation(
				1000.0 + static_cast<double>(RunIndex * OutResult.IterationsPerSample + IterationIndex),
				2000.0,
				3000.0);
			if (!FAvidScriptActorBinding::SetActorLocation(
				Registry,
				ActorHandle,
				TargetLocation,
				EAvidScriptActorWritePolicy::AllowWrites,
				BindingSetResult))
			{
				SetHostBindingFailure(
					OutResult,
					BindingSetResult.ErrorCategory.IsEmpty() ? FString(TEXT("binding_set_failed")) : BindingSetResult.ErrorCategory,
					BindingSetResult.ErrorMessage,
					BindingSetResult.NextAction);
				DestroyBenchmarkWorld(World);
				return false;
			}
		}
		const double BindingSetMs = MeasureElapsedPerIterationMs(BindingSetStartSeconds, OutResult.IterationsPerSample);

		OutResult.LastReadLocation = BindingLocation;
		OutResult.FinalActorLocation = Actor->GetActorLocation();

		if (RunIndex >= OutResult.WarmupCount)
		{
			DirectGetSamples.Add(DirectGetMs);
			RegistryResolveSamples.Add(RegistryResolveMs);
			BindingGetSamples.Add(BindingGetMs);
			BindingSetSamples.Add(BindingSetMs);
		}
	}

	OutResult.DirectGetActorLocation = CalculateStats(DirectGetSamples);
	OutResult.RegistryResolveActor = CalculateStats(RegistryResolveSamples);
	OutResult.BindingGetActorLocation = CalculateStats(BindingGetSamples);
	OutResult.BindingSetActorLocation = CalculateStats(BindingSetSamples);
	OutResult.bSucceeded = true;
	OutResult.Summary = FString::Printf(
		TEXT("host_binding_benchmark | warmup=%d | samples=%d | iterations=%d | direct_get_avg_ms=%.6f | registry_resolve_avg_ms=%.6f | binding_get_avg_ms=%.6f | binding_set_avg_ms=%.6f | binding_set_p95_ms=%.6f"),
		OutResult.WarmupCount,
		OutResult.SampleCount,
		OutResult.IterationsPerSample,
		OutResult.DirectGetActorLocation.AvgMs,
		OutResult.RegistryResolveActor.AvgMs,
		OutResult.BindingGetActorLocation.AvgMs,
		OutResult.BindingSetActorLocation.AvgMs,
		OutResult.BindingSetActorLocation.P95Ms);

	UE_LOG(LogAvidScriptRuntimeBenchmark, Display, TEXT("%s"), *OutResult.Summary);
	DestroyBenchmarkWorld(World);
	return true;
}
