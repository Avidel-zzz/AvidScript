#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptPerfRunner.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPerfPuertsCorrectnessTest,
	"AvidScript.PerformanceComparison.Puerts.Correctness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPerfPuertsCorrectnessTest::RunTest(const FString& Parameters)
{
	FAvidScriptPerfSmokeResult Result;
	const bool bSucceeded = FAvidScriptPerfRunner::RunPuertsCorrectnessSmoke(64, 1397313073, Result);
	TestTrue(TEXT("Puerts reflection/static correctness smoke succeeds"), bSucceeded);
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
	return true;
}

#endif
