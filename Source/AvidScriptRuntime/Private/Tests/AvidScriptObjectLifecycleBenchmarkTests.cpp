#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptRuntimeBenchmark.h"

#include "Benchmark/AvidScriptBenchmarkStatistics.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectLifecycleBenchmarkSmokeTest,
	"AvidScript.Performance.ObjectLifecycleBenchmarkSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectLifecycleBenchmarkSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectLifecycleBenchmarkOptions Options;
	Options.WarmupCount = 3;
	Options.SampleCount = 20;
	Options.IterationsPerSample = 128;

	FAvidScriptObjectLifecycleBenchmarkResult Result;
	const bool bSucceeded = FAvidScriptRuntimeBenchmark::RunObjectLifecycleBenchmark(Options, Result);
	if (!bSucceeded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("Object lifecycle benchmark succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Configured sample count is used"), Result.SampleCount, Options.SampleCount);
	TestEqual(TEXT("Configured iteration count is used"), Result.IterationsPerSample, Options.IterationsPerSample);
	TestEqual(TEXT("Native SpawnActor samples are recorded"), Result.NativeSpawnActor.Count, Options.SampleCount);
	TestEqual(TEXT("Binding SpawnActor samples are recorded"), Result.BindingSpawnActor.Count, Options.SampleCount);
	TestEqual(TEXT("Native DestroyActor samples are recorded"), Result.NativeDestroyActor.Count, Options.SampleCount);
	TestEqual(TEXT("Binding DestroyActor samples are recorded"), Result.BindingDestroyActor.Count, Options.SampleCount);
	TestEqual(TEXT("Class ordinal samples are recorded"), Result.ClassOrdinalResolve.Count, Options.SampleCount);
	TestEqual(TEXT("Registry resolve samples are recorded"), Result.RegistryResolveSpawnedActor.Count, Options.SampleCount);
	TestEqual(TEXT("Binding package load resolves one unique Actor class"), Result.BindingPackageClassLoadsDuringLoad, 1);
	TestEqual(TEXT("Class-only binding package performs no reflected member lookup"), Result.BindingPackageReflectedNameLookupsDuringLoad, 0);
	TestTrue(TEXT("Native SpawnActor P50 is recorded"), Result.NativeSpawnActor.P50Ms > 0.0);
	TestTrue(TEXT("Binding SpawnActor P95 is recorded"), Result.BindingSpawnActor.P95Ms > 0.0);
	TestTrue(TEXT("Native DestroyActor P50 is recorded"), Result.NativeDestroyActor.P50Ms > 0.0);
	TestTrue(TEXT("Binding DestroyActor P95 is recorded"), Result.BindingDestroyActor.P95Ms > 0.0);
	TestTrue(TEXT("Class ordinal timing is non-negative"), Result.ClassOrdinalResolve.P50Ms >= 0.0);
	TestTrue(TEXT("Registry resolve timing is non-negative"), Result.RegistryResolveSpawnedActor.P95Ms >= 0.0);
	TestTrue(TEXT("Class ordinal P95 is ordered"), Result.ClassOrdinalResolve.P95Ms >= Result.ClassOrdinalResolve.P50Ms);
	TestTrue(TEXT("Registry resolve P95 is ordered"), Result.RegistryResolveSpawnedActor.P95Ms >= Result.RegistryResolveSpawnedActor.P50Ms);
	TestEqual(TEXT("Warm loop performs no binding package class loads"), Result.BindingPackageClassLoadsDuringWarmLoop, 0);
	TestEqual(TEXT("Warm loop performs no binding package reflected name lookups"), Result.BindingPackageReflectedNameLookupsDuringWarmLoop, 0);
	TestEqual(TEXT("Real WAMR probe observes Spawn and Destroy imports"), Result.WasmLifecycleImportsObserved, 2);
	TestEqual(TEXT("SpawnActor uses one host import"), Result.SpawnImportsPerIteration, 1);
	TestEqual(TEXT("DestroyActor uses one host import"), Result.DestroyImportsPerIteration, 1);
	TestTrue(TEXT("Summary identifies the lifecycle benchmark"), Result.Summary.Contains(TEXT("object_lifecycle_benchmark")));
	TestTrue(TEXT("Summary includes Binding SpawnActor P95"), Result.Summary.Contains(TEXT("binding_spawn_p95_ms")));
	TestTrue(TEXT("Summary includes Binding DestroyActor P95"), Result.Summary.Contains(TEXT("binding_destroy_p95_ms")));
	TestTrue(TEXT("Summary scopes warm reflected lookup instrumentation to the binding package"), Result.Summary.Contains(TEXT("warm_binding_package_reflected_name_lookups")));

	TArray<double> OrderedSamples;
	for (int32 Value = 1; Value <= 20; ++Value)
	{
		OrderedSamples.Add(static_cast<double>(Value));
	}
	const FAvidScriptBenchmarkStats Percentiles = CalculateAvidScriptBenchmarkStats(MoveTemp(OrderedSamples));
	TestEqual(TEXT("Nearest-rank P50 uses the tenth of twenty samples"), Percentiles.P50Ms, 10.0);
	TestEqual(TEXT("Nearest-rank P95 uses the nineteenth of twenty samples"), Percentiles.P95Ms, 19.0);
	return true;
}

#endif
