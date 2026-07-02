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
	Options.SampleCount = 5;
	Options.WarmupCount = 1;
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

#endif
