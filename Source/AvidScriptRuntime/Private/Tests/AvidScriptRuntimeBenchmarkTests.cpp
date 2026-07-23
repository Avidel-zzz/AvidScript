#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptRuntimeBenchmark.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeMicrobenchmarkSmokeTest,
	"AvidScript.Performance.RuntimeMicrobenchmarkSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeMicrobenchmarkSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptRuntimeBenchmarkOptions Options;
	Options.SampleCount = 20;
	Options.WarmupCount = 3;
	Options.TickDeltaSeconds = 1.0f / 60.0f;

	FAvidScriptRuntimeBenchmarkResult Result;
	const bool bSucceeded = FAvidScriptRuntimeBenchmark::RunEmbeddedSmokeBenchmark(Options, Result);

	if (!bSucceeded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("Benchmark succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Configured sample count is used"), Result.SampleCount, Options.SampleCount);
	TestTrue(TEXT("Runtime init samples are recorded"), Result.RuntimeInit.Count == Options.SampleCount);
	TestTrue(TEXT("Module load average is recorded"), Result.ModuleLoad.AvgMs > 0.0);
	TestTrue(TEXT("Module instantiate average is recorded"), Result.ModuleInstantiate.AvgMs > 0.0);
	TestTrue(TEXT("Exec env average is recorded"), Result.ExecEnvCreate.AvgMs > 0.0);
	TestTrue(TEXT("BeginPlay average is recorded"), Result.BeginPlayCall.AvgMs > 0.0);
	TestTrue(TEXT("Average tick is recorded"), Result.TickCall.AvgMs > 0.0);
	TestTrue(TEXT("Unload average is recorded"), Result.Unload.AvgMs > 0.0);
	TestTrue(TEXT("Tick p95 is at least min"), Result.TickCall.P95Ms >= Result.TickCall.MinMs);
	TestTrue(TEXT("Summary includes benchmark label"), Result.Summary.Contains(TEXT("runtime_microbenchmark")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTimerSchedulerBenchmarkSmokeTest,
	"AvidScript.Performance.TimerSchedulerBenchmarkSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTimerSchedulerBenchmarkSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptTimerSchedulerBenchmarkOptions Options;
	Options.SampleCount = 20;
	Options.WarmupCount = 3;
	Options.PendingTimerCount = 512;
	Options.IterationsPerSample = 1000;

	FAvidScriptTimerSchedulerBenchmarkResult Result;
	const bool bSucceeded = FAvidScriptRuntimeBenchmark::RunTimerSchedulerBenchmark(Options, Result);
	if (!bSucceeded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("Timer scheduler benchmark succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Timer benchmark sample count is used"), Result.SampleCount, Options.SampleCount);
	TestEqual(TEXT("Pending timer count is used"), Result.PendingTimerCount, Options.PendingTimerCount);
	TestEqual(TEXT("Timer iterations are used"), Result.IterationsPerSample, Options.IterationsPerSample);
	TestEqual(TEXT("Idle Tick samples are recorded"), Result.IdleTick.Count, Options.SampleCount);
	TestEqual(TEXT("Set/cancel churn samples are recorded"), Result.SetCancelChurn.Count, Options.SampleCount);
	TestTrue(TEXT("Idle Tick average is recorded"), Result.IdleTick.AvgMs > 0.0);
	TestTrue(TEXT("Set/cancel churn average is recorded"), Result.SetCancelChurn.AvgMs > 0.0);
	TestTrue(TEXT("Summary includes timer benchmark label"), Result.Summary.Contains(TEXT("timer_scheduler_benchmark")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTypedObjectBenchmarkSmokeTest,
	"AvidScript.Performance.TypedObjectBenchmarkSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTypedObjectBenchmarkSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptTypedObjectBenchmarkOptions Options;
	Options.WarmupCount = 3;
	Options.SampleCount = 20;
	Options.IterationsPerSample = 1000;

	FAvidScriptTypedObjectBenchmarkResult Result;
	const bool bSucceeded = FAvidScriptRuntimeBenchmark::RunTypedObjectBenchmark(Options, Result);
	if (!bSucceeded)
	{
		AddError(Result.ErrorMessage);
	}

	const int32 TimedOperationCount = Options.SampleCount * Options.IterationsPerSample;
	TestTrue(TEXT("Typed object benchmark succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Typed object warmup count is used"), Result.WarmupCount, Options.WarmupCount);
	TestEqual(TEXT("Typed object sample count is used"), Result.SampleCount, Options.SampleCount);
	TestEqual(TEXT("Typed object iteration count is used"), Result.IterationsPerSample, Options.IterationsPerSample);
	TestEqual(TEXT("Native IsA samples are recorded"), Result.NativeIsA.Count, Options.SampleCount);
	TestEqual(TEXT("Binding object type samples are recorded"), Result.BindingObjectTypeIsA.Count, Options.SampleCount);
	TestEqual(TEXT("WAMR checked cast samples are recorded"), Result.WasmCheckedCast.Count, Options.SampleCount);
	TestEqual(TEXT("Existing typed binding samples are recorded"), Result.ExistingTypedBindingGetActorLocation.Count, Options.SampleCount);
	TestEqual(TEXT("Native operation count covers timed samples"), Result.NativeIsAOperationCount, TimedOperationCount);
	TestEqual(TEXT("Binding operation count covers timed samples"), Result.BindingObjectTypeOperationCount, TimedOperationCount);
	TestEqual(TEXT("WAMR operation count covers timed samples"), Result.WasmCheckedCastOperationCount, TimedOperationCount);
	TestEqual(TEXT("Typed upcast operation count covers timed samples"), Result.TypedUpcastOperationCount, TimedOperationCount);
	TestEqual(TEXT("WAMR performs one checked-cast crossing per timed operation"), Result.WasmCheckedCastHostCrossingCount, TimedOperationCount);
	TestEqual(TEXT("Observed typed upcast workload adds no host import"), Result.TypedUpcastHostImportCount, 0);
	TestEqual(TEXT("Warm typed dispatch performs no class loads"), Result.BindingPackageClassLoadsDuringWarmLoop, 0);
	TestEqual(TEXT("Warm typed dispatch performs no reflected name lookup"), Result.BindingPackageReflectedNameLookupsDuringWarmLoop, 0);
	TestEqual(TEXT("Typed upcasts use no host import"), Result.UpcastHostImportsPerIteration, 0);
	TestEqual(TEXT("WAMR checked cast result is host-observable and matched"), Result.LastWasmCheckedCastResult, 1);
	TestTrue(TEXT("Native IsA result checksum is observable"), Result.NativeIsAResultChecksum != 0);
	TestEqual(TEXT("Binding consumes the same successful type results"),
		Result.BindingObjectTypeResultChecksum,
		Result.NativeIsAResultChecksum);
	TestEqual(TEXT("WAMR consumes the same successful type results"),
		Result.WasmCheckedCastResultChecksum,
		Result.NativeIsAResultChecksum);
	TestTrue(TEXT("Typed upcast copied handle checksum is observable"), Result.TypedUpcastResultChecksum != 0);
	TestTrue(TEXT("Package load resolves the immutable object type graph"), Result.BindingPackageClassLoadsDuringLoad >= 3);
	TestTrue(TEXT("Package load may resolve the static schema sentinel once"), Result.BindingPackageReflectedNameLookupsDuringLoad >= 1);
	TestTrue(TEXT("Native IsA P95 is ordered"), Result.NativeIsA.P95Ms >= Result.NativeIsA.P50Ms);
	TestTrue(TEXT("Binding object type P95 is ordered"), Result.BindingObjectTypeIsA.P95Ms >= Result.BindingObjectTypeIsA.P50Ms);
	TestTrue(TEXT("WAMR checked cast P95 is ordered"), Result.WasmCheckedCast.P95Ms >= Result.WasmCheckedCast.P50Ms);
	TestTrue(TEXT("Current result does not fabricate a Phase 49 median baseline"), !Result.bHasComparablePhase49Baseline);
	TestFalse(TEXT("Current result does not claim the pending Phase 49 budget passed"), Result.bExistingTypedBindingWithinRegressionBudget);
	TestEqual(TEXT("Phase 49 comparison is explicitly deferred to centralized sampling"),
		Result.ExistingTypedBindingRegressionStatus,
		FString(TEXT("pending_same_machine_phase49_baseline")));
	TestTrue(TEXT("Summary identifies typed object benchmark"), Result.Summary.Contains(TEXT("typed_object_benchmark")));
	TestTrue(TEXT("Summary exposes WAMR crossing count"), Result.Summary.Contains(TEXT("wasm_host_crossings")));
	TestTrue(TEXT("Summary exposes observed typed upcast imports"), Result.Summary.Contains(TEXT("typed_upcast_host_imports")));
	TestTrue(TEXT("Summary exposes observable checksums"), Result.Summary.Contains(TEXT("native_checksum")));
	TestTrue(TEXT("Summary exposes warm lookup budget"), Result.Summary.Contains(TEXT("warm_reflected_name_lookups")));
	return true;
}

#endif
