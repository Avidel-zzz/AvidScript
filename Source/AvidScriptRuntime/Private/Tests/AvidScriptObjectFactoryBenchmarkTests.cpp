#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptRuntimeBenchmark.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectFactoryBenchmarkSmokeTest,
	"AvidScript.Performance.ObjectFactoryBenchmarkSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectFactoryBenchmarkSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectFactoryBenchmarkOptions Options;

	FAvidScriptObjectFactoryBenchmarkResult Result;
	const bool bRan = FAvidScriptRuntimeBenchmark::RunObjectFactoryBenchmark(
		Options,
		Result);
	if (!TestTrue(TEXT("Object factory benchmark completes"), bRan)
		|| !TestTrue(TEXT("Object factory benchmark succeeds"), Result.bSucceeded))
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	TestEqual(TEXT("Benchmark records requested sample count"), Result.SampleCount, Options.SampleCount);
	TestEqual(TEXT("Benchmark records requested iteration count"), Result.IterationsPerSample, Options.IterationsPerSample);
	TestEqual(TEXT("Benchmark records the component population"), Result.ComponentCount, Options.ComponentCount);
	TestEqual(TEXT("Native Construct records every sample"), Result.NativeConstructComponent.Count, Options.SampleCount);
	TestEqual(TEXT("Binding Construct records every sample"), Result.BindingConstructComponent.Count, Options.SampleCount);
	TestEqual(TEXT("Native Find records every sample"), Result.NativeFindComponent.Count, Options.SampleCount);
	TestEqual(TEXT("Binding Find records every sample"), Result.BindingFindComponent.Count, Options.SampleCount);
	TestEqual(TEXT("Native Attach records every sample"), Result.NativeAttachComponent.Count, Options.SampleCount);
	TestEqual(TEXT("Binding Attach records every sample"), Result.BindingAttachComponent.Count, Options.SampleCount);
	TestEqual(TEXT("Native Release records every sample"), Result.NativeReleaseComponent.Count, Options.SampleCount);
	TestEqual(TEXT("Binding Release records every sample"), Result.BindingReleaseComponent.Count, Options.SampleCount);
	TestEqual(TEXT("WAMR cycle records every sample"), Result.WasmComponentCycle.Count, Options.SampleCount);
	TestEqual(TEXT("Factory ordinal records every sample"), Result.FactoryOrdinalResolve.Count, Options.SampleCount);
	TestEqual(TEXT("Registry resolve records every sample"), Result.RegistryResolveComponent.Count, Options.SampleCount);
	TestEqual(TEXT("WAMR Construct crossing is exactly one"), Result.ConstructImportsPerWasmIteration, 1);
	TestEqual(TEXT("WAMR Find crossing is exactly one"), Result.FindImportsPerWasmIteration, 1);
	TestEqual(TEXT("WAMR Attach crossing is exactly one"), Result.AttachImportsPerWasmIteration, 1);
	TestEqual(TEXT("WAMR Release crossing is exactly one"), Result.ReleaseImportsPerWasmIteration, 1);
	TestEqual(
		TEXT("WAMR observes the exact import count"),
		Result.WasmImportsObserved,
		(Options.WarmupCount + Options.SampleCount)
			* Options.IterationsPerSample
			* 4);
	TestEqual(TEXT("Warm loop performs no class loads"), Result.BindingPackageClassLoadsDuringWarmLoop, 0);
	TestEqual(TEXT("Warm loop performs no reflected-name lookups"),
		Result.BindingPackageReflectedNameLookupsDuringWarmLoop,
		0);
	TestTrue(TEXT("Summary exposes the stable benchmark marker"),
		Result.Summary.Contains(TEXT("object_factory_benchmark")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
