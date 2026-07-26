#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptControlledRuntimeRunner.h"
#include "AvidScriptPerfRunner.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPerfFiveLaneCorrectnessTest,
	"AvidScript.PerformanceComparison.FiveLane.Correctness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPerfFiveLaneCorrectnessTest::RunTest(const FString& Parameters)
{
	FAvidScriptPerfSmokeResult Result;
	const bool bSucceeded = FAvidScriptPerfRunner::RunFiveLaneCorrectnessSmoke(64, 1397313, Result);
	TestTrue(TEXT("five-lane correctness smoke succeeds"), bSucceeded);
	if (!bSucceeded)
	{
		AddError(Result.Error);
		return false;
	}

	TestEqual(
		TEXT("workload count"),
		Result.WorkloadCount,
		static_cast<int32>(EAvidScriptPerfWorkload::Count));
	TestEqual(
		TEXT("reflection aggregate matches native"),
		Result.PuertsReflectionChecksum,
		Result.NativeChecksum);
	TestEqual(
		TEXT("static aggregate matches native"),
		Result.PuertsStaticChecksum,
		Result.NativeChecksum);
	TestEqual(
		TEXT("AvidScript WAMR aggregate matches native"),
		Result.AvidScriptWamrChecksum,
		Result.NativeChecksum);
	TestEqual(
		TEXT("AvidScript Wasmtime aggregate matches native"),
		Result.AvidScriptWasmtimeChecksum,
		Result.NativeChecksum);
	TestTrue(
		TEXT("AvidScript WAMR lane records host calls"),
		Result.AvidScriptWamrHostCallCount > 0);
	TestTrue(
		TEXT("AvidScript Wasmtime lane records host calls"),
		Result.AvidScriptWasmtimeHostCallCount > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptControlledRuntimeCorrectnessTest,
	"AvidScript.PerformanceComparison.ControlledRuntime.Correctness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptControlledRuntimeCorrectnessTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptControlledRuntimeSmokeResult Result;
	const bool bSucceeded =
		FAvidScriptControlledRuntimeRunner::RunCorrectnessSmoke(
			4096,
			1397313,
			Result);
	TestTrue(TEXT("controlled runtime correctness smoke succeeds"), bSucceeded);
	if (!bSucceeded)
	{
		AddError(Result.Error);
		return false;
	}
	TestEqual(TEXT("native result matches oracle"), Result.NativeResult, Result.Expected);
	TestEqual(TEXT("V8 WASM result matches oracle"), Result.PuertsV8Result, Result.Expected);
	TestEqual(TEXT("WAMR result matches oracle"), Result.WamrResult, Result.Expected);
	TestEqual(TEXT("Wasmtime result matches oracle"), Result.WasmtimeResult, Result.Expected);
	TestTrue(
		TEXT("Puerts lane executes WebAssembly.Module and WebAssembly.Instance"),
		Result.bPuertsExecutedWebAssembly);
	TestEqual(
		TEXT("all VM lanes consume the tracked kernel digest"),
		Result.KernelWasmSha256,
		TEXT("230aed5ae6bd816e7780e0519354b100f29d23908ff0e6c1f0b52af5d4c834f7"));
	return true;
}

#endif
