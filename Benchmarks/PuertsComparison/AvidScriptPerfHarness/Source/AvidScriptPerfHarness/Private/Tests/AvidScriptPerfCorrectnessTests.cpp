#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptPerfRunner.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPerfFourLaneCorrectnessTest,
	"AvidScript.PerformanceComparison.FourLane.Correctness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPerfFourLaneCorrectnessTest::RunTest(const FString& Parameters)
{
	FAvidScriptPerfSmokeResult Result;
	const bool bSucceeded = FAvidScriptPerfRunner::RunFourLaneCorrectnessSmoke(64, 1397313, Result);
	TestTrue(TEXT("four-lane correctness smoke succeeds"), bSucceeded);
	if (!bSucceeded)
	{
		AddError(Result.Error);
		return false;
	}

	TestEqual(TEXT("workload count"), Result.WorkloadCount, 7);
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
		Result.AvidScriptChecksum,
		Result.NativeChecksum);
	TestTrue(
		TEXT("AvidScript WAMR lane records host calls"),
		Result.AvidScriptHostCallCount > 0);
	return true;
}

#endif
