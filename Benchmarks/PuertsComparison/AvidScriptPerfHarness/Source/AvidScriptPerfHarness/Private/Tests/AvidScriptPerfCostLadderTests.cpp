#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptPerfCostLadder.h"
#include "Misc/AutomationTest.h"

namespace
{
	FAvidScriptPerfCostDriver MakeChecksumDriver(const uint32 Salt)
	{
		return [Salt](const uint64 Iterations, const uint32 Seed, uint32& OutChecksum, FString&)
		{
			uint32 Value = Seed ^ Salt;
			for (uint64 Index = 0; Index < Iterations; ++Index)
			{
				Value = (Value * 1664525U) + 1013904223U + static_cast<uint32>(Index);
			}
			OutChecksum = Value;
			return true;
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPerfCostLadderOrderingTest,
	"AvidScript.PerformanceComparison.CostLadder.Ordering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPerfCostLadderOrderingTest::RunTest(const FString& Parameters)
{
	FAvidScriptPerfCostLadderDrivers Drivers;
	Drivers.CachedExport = MakeChecksumDriver(1U);
	Drivers.TypedEmptyImport = MakeChecksumDriver(2U);
	Drivers.GenericEmptyImport = MakeChecksumDriver(3U);
	Drivers.ImmutableEnvironmentDispatch = MakeChecksumDriver(4U);

	TArray<FAvidScriptPerfCostLadderRecord> Records;
	FString Error;
	const FAvidScriptPerfCostLadderRequest Request{ 128, 1397313U };
	TestTrue(TEXT("cost ladder accepts injected stage drivers"), FAvidScriptPerfCostLadder::Run(Request, Drivers, Records, Error));
	TestEqual(TEXT("cost ladder record count"), Records.Num(), 5);

	const TArray<FName> ExpectedStages = {
		TEXT("native_no_op"),
		TEXT("cached_export"),
		TEXT("typed_empty_import"),
		TEXT("generic_empty_import"),
		TEXT("immutable_environment_dispatch")
	};
	for (int32 Index = 0; Index < Records.Num(); ++Index)
	{
		TestEqual(TEXT("stage ordering"), Records[Index].Stage, ExpectedStages[Index]);
		TestEqual(TEXT("immutable iteration count"), Records[Index].Iterations, Request.Iterations);
		TestTrue(TEXT("successful stage has timing"), Records[Index].ElapsedCycles > 0);
		TestTrue(TEXT("successful stage is correct"), Records[Index].bCorrect);
	}
	TestNotEqual(TEXT("injected stage checksums stay distinct"), Records[1].Checksum, Records[2].Checksum);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPerfCostLadderMissingDriverTest,
	"AvidScript.PerformanceComparison.CostLadder.MissingDriver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPerfCostLadderMissingDriverTest::RunTest(const FString& Parameters)
{
	FAvidScriptPerfCostLadderDrivers Drivers;
	TArray<FAvidScriptPerfCostLadderRecord> Records;
	FString Error;
	const FAvidScriptPerfCostLadderRequest Request{ 8, 7U };
	TestFalse(TEXT("missing runtime driver is not faked"), FAvidScriptPerfCostLadder::Run(Request, Drivers, Records, Error));
	TestEqual(TEXT("missing cached export stops after native baseline"), Records.Num(), 2);
	TestFalse(TEXT("missing stage is marked incorrect"), Records.Last().bCorrect);
	TestTrue(TEXT("missing driver error is explicit"), Error.Contains(TEXT("No measurement driver")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPerfCostBudgetTest,
	"AvidScript.PerformanceComparison.CostLadder.Budget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPerfCostBudgetTest::RunTest(const FString& Parameters)
{
	FAvidScriptPerfCostBudgetInput Input;
	Input.TypedEmptyP95Ns = 20.0;
	Input.PuertsStaticScalarP50Ns = 25.0;
	Input.GenericEmptyP95Ns = 120.0;
	Input.ImmutableEnvironmentDispatchP95Ns = 45.0;
	FAvidScriptPerfCostBudgetResult Result;
	FString Error;
	TestTrue(TEXT("valid budget evaluates"), FAvidScriptPerfCostLadder::EvaluateBudget(Input, Result, Error));
	TestEqual(TEXT("typed to Puerts ratio"), Result.TypedToPuertsRatio, 0.8);
	TestEqual(TEXT("generic crossing delta"), Result.GenericMinusTypedNs, 100.0);
	TestEqual(TEXT("immutable dispatch delta"), Result.ImmutableDispatchMinusTypedNs, 25.0);
	TestTrue(TEXT("70 percent stop-loss is inclusive"), Result.bSingleCallBudgetConstrained);

	Input.TypedEmptyP95Ns = 17.49;
	TestTrue(TEXT("sub-threshold budget evaluates"), FAvidScriptPerfCostLadder::EvaluateBudget(Input, Result, Error));
	TestFalse(TEXT("sub-threshold budget remains unconstrained"), Result.bSingleCallBudgetConstrained);

	Input.PuertsStaticScalarP50Ns = 0.0;
	TestFalse(TEXT("zero baseline rejects"), FAvidScriptPerfCostLadder::EvaluateBudget(Input, Result, Error));
	TestTrue(TEXT("invalid budget has explicit error"), Error.Contains(TEXT("positive Puerts baseline")));
	return true;
}

#endif
