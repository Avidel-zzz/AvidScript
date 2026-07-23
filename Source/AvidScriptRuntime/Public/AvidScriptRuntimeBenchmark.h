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

struct FAvidScriptTimerSchedulerBenchmarkOptions
{
	int32 WarmupCount = 3;
	int32 SampleCount = 20;
	int32 PendingTimerCount = 512;
	int32 IterationsPerSample = 1000;
};

struct FAvidScriptTimerSchedulerBenchmarkResult
{
	bool bSucceeded = false;
	int32 WarmupCount = 0;
	int32 SampleCount = 0;
	int32 PendingTimerCount = 0;
	int32 IterationsPerSample = 0;
	FString ErrorCategory;
	FString ErrorMessage;
	FString Summary;
	FAvidScriptBenchmarkStats IdleTick;
	FAvidScriptBenchmarkStats SetCancelChurn;
};

struct FAvidScriptHostBindingBenchmarkOptions
{
	int32 WarmupCount = 2;
	int32 SampleCount = 20;
	int32 IterationsPerSample = 1000;
	int32 TransformBatchSize = 64;
};

struct FAvidScriptHostBindingBenchmarkResult
{
	bool bSucceeded = false;
	int32 WarmupCount = 0;
	int32 SampleCount = 0;
	int32 IterationsPerSample = 0;
	int32 TransformBatchSize = 0;
	int32 WasmScalarImportsPerIteration = 0;
	int32 WasmBatchImportsPerIteration = 0;
	FString ErrorCategory;
	FString ErrorMessage;
	FString Summary;
	FVector LastReadLocation = FVector::ZeroVector;
	FVector FinalActorLocation = FVector::ZeroVector;
	FAvidScriptBenchmarkStats DirectGetActorLocation;
	FAvidScriptBenchmarkStats RegistryResolveActor;
	FAvidScriptBenchmarkStats BindingGetActorLocation;
	FAvidScriptBenchmarkStats BindingSetActorLocation;
	FAvidScriptBenchmarkStats ScalarGetActorTransform;
	FAvidScriptBenchmarkStats BatchGetActorTransforms;
	FAvidScriptBenchmarkStats WasmScalarGetActorTransforms;
	FAvidScriptBenchmarkStats WasmBatchGetActorTransforms;
};

struct FAvidScriptObjectLifecycleBenchmarkOptions
{
	int32 WarmupCount = 2;
	int32 SampleCount = 20;
	int32 IterationsPerSample = 128;
};

struct FAvidScriptObjectLifecycleBenchmarkResult
{
	bool bSucceeded = false;
	int32 WarmupCount = 0;
	int32 SampleCount = 0;
	int32 IterationsPerSample = 0;
	int32 BindingPackageClassLoadsDuringLoad = 0;
	int32 BindingPackageReflectedNameLookupsDuringLoad = 0;
	int32 BindingPackageClassLoadsDuringWarmLoop = 0;
	int32 BindingPackageReflectedNameLookupsDuringWarmLoop = 0;
	int32 SpawnImportsPerIteration = 0;
	int32 DestroyImportsPerIteration = 0;
	int32 WasmLifecycleImportsObserved = 0;
	FString ErrorCategory;
	FString ErrorMessage;
	FString Summary;
	FAvidScriptBenchmarkStats NativeSpawnActor;
	FAvidScriptBenchmarkStats BindingSpawnActor;
	FAvidScriptBenchmarkStats NativeDestroyActor;
	FAvidScriptBenchmarkStats BindingDestroyActor;
	FAvidScriptBenchmarkStats ClassOrdinalResolve;
	FAvidScriptBenchmarkStats RegistryResolveSpawnedActor;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeBenchmark
{
public:
	static bool RunEmbeddedSmokeBenchmark(
		const FAvidScriptRuntimeBenchmarkOptions& Options,
		FAvidScriptRuntimeBenchmarkResult& OutResult);

	static bool RunTimerSchedulerBenchmark(
		const FAvidScriptTimerSchedulerBenchmarkOptions& Options,
		FAvidScriptTimerSchedulerBenchmarkResult& OutResult);

	static bool RunHostBindingBenchmark(
		const FAvidScriptHostBindingBenchmarkOptions& Options,
		FAvidScriptHostBindingBenchmarkResult& OutResult);

	static bool RunObjectLifecycleBenchmark(
		const FAvidScriptObjectLifecycleBenchmarkOptions& Options,
		FAvidScriptObjectLifecycleBenchmarkResult& OutResult);
};
