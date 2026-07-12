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
	Options.TransformBatchSize = 64;

	FAvidScriptHostBindingBenchmarkResult Result;
	const bool bSucceeded = FAvidScriptRuntimeBenchmark::RunHostBindingBenchmark(Options, Result);

	if (!bSucceeded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("Benchmark succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Configured sample count is used"), Result.SampleCount, Options.SampleCount);
	TestEqual(TEXT("Configured iterations are used"), Result.IterationsPerSample, Options.IterationsPerSample);
	TestEqual(TEXT("Configured transform batch size is used"), Result.TransformBatchSize, Options.TransformBatchSize);
	TestTrue(TEXT("Direct actor get samples are recorded"), Result.DirectGetActorLocation.Count == Options.SampleCount);
	TestTrue(TEXT("Registry resolve samples are recorded"), Result.RegistryResolveActor.Count == Options.SampleCount);
	TestTrue(TEXT("Binding get samples are recorded"), Result.BindingGetActorLocation.Count == Options.SampleCount);
	TestTrue(TEXT("Binding set samples are recorded"), Result.BindingSetActorLocation.Count == Options.SampleCount);
	TestTrue(TEXT("Scalar transform samples are recorded"), Result.ScalarGetActorTransform.Count == Options.SampleCount);
	TestTrue(TEXT("Batch transform samples are recorded"), Result.BatchGetActorTransforms.Count == Options.SampleCount);
	TestTrue(TEXT("WASM scalar transform samples are recorded"), Result.WasmScalarGetActorTransforms.Count == Options.SampleCount);
	TestTrue(TEXT("WASM batch transform samples are recorded"), Result.WasmBatchGetActorTransforms.Count == Options.SampleCount);
	TestEqual(TEXT("WASM scalar path uses three imports per actor"), Result.WasmScalarImportsPerIteration, Options.TransformBatchSize * 3);
	TestEqual(TEXT("WASM batch path uses one import"), Result.WasmBatchImportsPerIteration, 1);
	TestTrue(TEXT("Direct get average is recorded"), Result.DirectGetActorLocation.AvgMs > 0.0);
	TestTrue(TEXT("Registry resolve average is recorded"), Result.RegistryResolveActor.AvgMs > 0.0);
	TestTrue(TEXT("Binding get average is recorded"), Result.BindingGetActorLocation.AvgMs > 0.0);
	TestTrue(TEXT("Binding set average is recorded"), Result.BindingSetActorLocation.AvgMs > 0.0);
	TestTrue(TEXT("Scalar transform average is recorded"), Result.ScalarGetActorTransform.AvgMs > 0.0);
	TestTrue(TEXT("Batch transform average is recorded"), Result.BatchGetActorTransforms.AvgMs > 0.0);
	TestTrue(TEXT("Binding set p95 is at least min"), Result.BindingSetActorLocation.P95Ms >= Result.BindingSetActorLocation.MinMs);
	TestTrue(TEXT("Summary includes benchmark label"), Result.Summary.Contains(TEXT("host_binding_benchmark")));
	TestTrue(TEXT("Summary includes scalar transform metric"), Result.Summary.Contains(TEXT("scalar_transform_avg_ms")));
	TestTrue(TEXT("Summary includes batch transform metric"), Result.Summary.Contains(TEXT("batch_transform_avg_ms")));
	TestTrue(TEXT("Summary includes WASM scalar transform metric"), Result.Summary.Contains(TEXT("wasm_scalar_transform_avg_ms")));
	TestTrue(TEXT("Summary includes WASM batch transform metric"), Result.Summary.Contains(TEXT("wasm_batch_transform_avg_ms")));

	return true;
}

#endif
