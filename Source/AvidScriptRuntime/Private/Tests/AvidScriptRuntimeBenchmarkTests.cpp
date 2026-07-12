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

#endif
