#include "AvidScriptRuntimeBenchmark.h"

#include "AvidScriptWasmRuntime.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptRuntimeBenchmark, Log, All);

namespace
{
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
