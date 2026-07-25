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

struct FAvidScriptObjectFactoryBenchmarkOptions
{
	int32 WarmupCount = 3;
	int32 SampleCount = 20;
	int32 IterationsPerSample = 64;
	int32 ComponentCount = 64;
};

struct FAvidScriptObjectFactoryBenchmarkResult
{
	bool bSucceeded = false;
	int32 WarmupCount = 0;
	int32 SampleCount = 0;
	int32 IterationsPerSample = 0;
	int32 ComponentCount = 0;
	int32 BindingPackageClassLoadsDuringLoad = 0;
	int32 BindingPackageReflectedNameLookupsDuringLoad = 0;
	int32 BindingPackageClassLoadsDuringWarmLoop = 0;
	int32 BindingPackageReflectedNameLookupsDuringWarmLoop = 0;
	int32 ConstructImportsPerWasmIteration = 0;
	int32 FindImportsPerWasmIteration = 0;
	int32 AttachImportsPerWasmIteration = 0;
	int32 ReleaseImportsPerWasmIteration = 0;
	int32 WasmImportsObserved = 0;
	FString ErrorCategory;
	FString ErrorMessage;
	FString Summary;
	FAvidScriptBenchmarkStats NativeConstructComponent;
	FAvidScriptBenchmarkStats BindingConstructComponent;
	FAvidScriptBenchmarkStats NativeFindComponent;
	FAvidScriptBenchmarkStats BindingFindComponent;
	FAvidScriptBenchmarkStats NativeAttachComponent;
	FAvidScriptBenchmarkStats BindingAttachComponent;
	FAvidScriptBenchmarkStats NativeReleaseComponent;
	FAvidScriptBenchmarkStats BindingReleaseComponent;
	FAvidScriptBenchmarkStats WasmComponentCycle;
	FAvidScriptBenchmarkStats FactoryOrdinalResolve;
	FAvidScriptBenchmarkStats RegistryResolveComponent;
};

struct FAvidScriptTypedObjectBenchmarkOptions
{
	int32 WarmupCount = 3;
	int32 SampleCount = 20;
	int32 IterationsPerSample = 1000;
};

struct FAvidScriptTypedObjectBenchmarkResult
{
	bool bSucceeded = false;
	int32 WarmupCount = 0;
	int32 SampleCount = 0;
	int32 IterationsPerSample = 0;
	int32 NativeIsAOperationCount = 0;
	int32 BindingObjectTypeOperationCount = 0;
	int32 WasmCheckedCastOperationCount = 0;
	int32 TypedUpcastOperationCount = 0;
	int32 WasmCheckedCastHostCrossingCount = 0;
	int32 TypedUpcastHostImportCount = 0;
	int32 UpcastHostImportsPerIteration = 0;
	int32 LastWasmCheckedCastResult = 0;
	uint64 NativeIsAResultChecksum = 0;
	uint64 BindingObjectTypeResultChecksum = 0;
	uint64 WasmCheckedCastResultChecksum = 0;
	uint64 TypedUpcastResultChecksum = 0;
	uint64 NativeTargetSelectorChecksum = 0;
	uint64 BindingTargetSelectorChecksum = 0;
	int32 BindingPackageClassLoadsDuringLoad = 0;
	int32 BindingPackageReflectedNameLookupsDuringLoad = 0;
	int32 BindingPackageClassLoadsDuringWarmLoop = 0;
	int32 BindingPackageReflectedNameLookupsDuringWarmLoop = 0;
	double Phase49TypedBindingMedianMs = 0.0;
	double ExistingTypedBindingRegressionPercent = 0.0;
	bool bHasComparablePhase49Baseline = false;
	bool bExistingTypedBindingWithinRegressionBudget = false;
	FString ExistingTypedBindingRegressionStatus;
	FString ErrorCategory;
	FString ErrorMessage;
	FString Summary;
	FAvidScriptBenchmarkStats NativeIsA;
	FAvidScriptBenchmarkStats BindingObjectTypeIsA;
	FAvidScriptBenchmarkStats WasmCheckedCast;
	FAvidScriptBenchmarkStats ExistingTypedBindingGetActorLocation;
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

	static bool RunObjectFactoryBenchmark(
		const FAvidScriptObjectFactoryBenchmarkOptions& Options,
		FAvidScriptObjectFactoryBenchmarkResult& OutResult);

	static bool RunTypedObjectBenchmark(
		const FAvidScriptTypedObjectBenchmarkOptions& Options,
		FAvidScriptTypedObjectBenchmarkResult& OutResult);
};
