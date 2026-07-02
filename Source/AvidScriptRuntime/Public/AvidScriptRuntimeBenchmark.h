#pragma once

#include "CoreMinimal.h"

struct FAvidScriptBenchmarkStats
{
	int32 Count = 0;
	double MinMs = 0.0;
	double MaxMs = 0.0;
	double AvgMs = 0.0;
	double P50Ms = 0.0;
	double P95Ms = 0.0;
};

struct FAvidScriptRuntimeBenchmarkOptions
{
	int32 WarmupCount = 2;
	int32 SampleCount = 20;
	float TickDeltaSeconds = 1.0f / 60.0f;
};

struct FAvidScriptRuntimeBenchmarkResult
{
	bool bSucceeded = false;
	int32 WarmupCount = 0;
	int32 SampleCount = 0;
	FString ErrorCategory;
	FString ErrorMessage;
	FString Summary;
	FAvidScriptBenchmarkStats RuntimeInit;
	FAvidScriptBenchmarkStats ModuleLoad;
	FAvidScriptBenchmarkStats ModuleInstantiate;
	FAvidScriptBenchmarkStats ExecEnvCreate;
	FAvidScriptBenchmarkStats BeginPlayCall;
	FAvidScriptBenchmarkStats TickCall;
	FAvidScriptBenchmarkStats Unload;
};

struct FAvidScriptHostBindingBenchmarkOptions
{
	int32 WarmupCount = 2;
	int32 SampleCount = 20;
	int32 IterationsPerSample = 1000;
};

struct FAvidScriptHostBindingBenchmarkResult
{
	bool bSucceeded = false;
	int32 WarmupCount = 0;
	int32 SampleCount = 0;
	int32 IterationsPerSample = 0;
	FString ErrorCategory;
	FString ErrorMessage;
	FString Summary;
	FVector LastReadLocation = FVector::ZeroVector;
	FVector FinalActorLocation = FVector::ZeroVector;
	FAvidScriptBenchmarkStats DirectGetActorLocation;
	FAvidScriptBenchmarkStats RegistryResolveActor;
	FAvidScriptBenchmarkStats BindingGetActorLocation;
	FAvidScriptBenchmarkStats BindingSetActorLocation;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeBenchmark
{
public:
	static bool RunEmbeddedSmokeBenchmark(
		const FAvidScriptRuntimeBenchmarkOptions& Options,
		FAvidScriptRuntimeBenchmarkResult& OutResult);

	static bool RunHostBindingBenchmark(
		const FAvidScriptHostBindingBenchmarkOptions& Options,
		FAvidScriptHostBindingBenchmarkResult& OutResult);
};
