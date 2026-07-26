#if WITH_DEV_AUTOMATION_TESTS

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

#endif
