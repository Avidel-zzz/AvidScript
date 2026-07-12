#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptRuntimeBenchmark.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptHostBindingOverheadSmokeTest,
	"AvidScript.Performance.HostBindingOverheadSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptHostBindingOverheadSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptHostBindingBenchmarkOptions Options;
	Options.SampleCount = 20;
	Options.WarmupCount = 3;
	Options.IterationsPerSample = 1000;

	FAvidScriptHostBindingBenchmarkResult Result;
	const bool bSucceeded = FAvidScriptRuntimeBenchmark::RunHostBindingBenchmark(Options, Result);

	if (!bSucceeded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("Benchmark succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Configured sample count is used"), Result.SampleCount, Options.SampleCount);
	TestEqual(TEXT("Configured iterations are used"), Result.IterationsPerSample, Options.IterationsPerSample);
	TestTrue(TEXT("Direct actor get samples are recorded"), Result.DirectGetActorLocation.Count == Options.SampleCount);
	TestTrue(TEXT("Registry resolve samples are recorded"), Result.RegistryResolveActor.Count == Options.SampleCount);
	TestTrue(TEXT("Binding get samples are recorded"), Result.BindingGetActorLocation.Count == Options.SampleCount);
	TestTrue(TEXT("Binding set samples are recorded"), Result.BindingSetActorLocation.Count == Options.SampleCount);
	TestTrue(TEXT("Direct get average is recorded"), Result.DirectGetActorLocation.AvgMs > 0.0);
	TestTrue(TEXT("Registry resolve average is recorded"), Result.RegistryResolveActor.AvgMs > 0.0);
	TestTrue(TEXT("Binding get average is recorded"), Result.BindingGetActorLocation.AvgMs > 0.0);
	TestTrue(TEXT("Binding set average is recorded"), Result.BindingSetActorLocation.AvgMs > 0.0);
	TestTrue(TEXT("Binding set p95 is at least min"), Result.BindingSetActorLocation.P95Ms >= Result.BindingSetActorLocation.MinMs);
	TestTrue(TEXT("Summary includes benchmark label"), Result.Summary.Contains(TEXT("host_binding_benchmark")));

	return true;
}

#endif
