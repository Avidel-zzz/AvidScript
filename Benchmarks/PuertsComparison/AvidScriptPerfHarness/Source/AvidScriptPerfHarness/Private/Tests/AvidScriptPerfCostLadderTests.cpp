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
	Drivers.GuestLoopBaseline = MakeChecksumDriver(1U);
	Drivers.GenericExport = MakeChecksumDriver(2U);
	Drivers.PreparedExport = MakeChecksumDriver(3U);
	Drivers.TypedEmptyImport = MakeChecksumDriver(4U);
	Drivers.GenericEmptyImport = MakeChecksumDriver(5U);
	Drivers.TypedI32PairImport = MakeChecksumDriver(6U);

	TArray<FAvidScriptPerfCostLadderRecord> Records;
	FString Error;
	const FAvidScriptPerfCostLadderRequest Request{ 128, 1397313U };
	TestTrue(TEXT("cost ladder accepts injected stage drivers"), FAvidScriptPerfCostLadder::Run(Request, Drivers, Records, Error));
	TestEqual(TEXT("cost ladder record count"), Records.Num(), 7);

	const TArray<FName> ExpectedStages = {
		TEXT("native_no_op"),
		TEXT("guest_loop_baseline"),
		TEXT("generic_export"),
		TEXT("prepared_export"),
		TEXT("typed_empty_import"),
		TEXT("generic_empty_import"),
		TEXT("typed_i32_pair_import")
	};
	for (int32 Index = 0; Index < Records.Num(); ++Index)
	{
		TestEqual(TEXT("stage ordering"), Records[Index].Stage, ExpectedStages[Index]);
		TestEqual(TEXT("fixed iteration count"), Records[Index].Iterations, Request.Iterations);
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
	TestEqual(TEXT("missing guest loop stops after native baseline"), Records.Num(), 2);
	TestFalse(TEXT("missing stage is marked incorrect"), Records.Last().bCorrect);
	TestTrue(TEXT("missing driver error is explicit"), Error.Contains(TEXT("No measurement driver")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPerfCostReconciliationTest,
	"AvidScript.PerformanceComparison.CostLadder.Reconciliation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPerfCostReconciliationTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptPerfCostReconciliationInput Input;
	Input.ObservedP50Ns = 120.0;
	Input.BaselineP50Ns = 20.0;
	Input.PairedDeltaP50Ns = 98.0;
	Input.MaximumErrorRatio = 0.05;
	FAvidScriptPerfCostReconciliationResult Result;
	FString Error;
	TestTrue(
		TEXT("valid paired reconciliation evaluates"),
		FAvidScriptPerfCostLadder::EvaluateReconciliation(
			Input,
			Result,
			Error));
	TestEqual(
		TEXT("paired segments reconstruct P50"),
		Result.ReconstructedP50Ns,
		118.0);
	TestEqual(
		TEXT("reconciliation absolute error"),
		Result.ReconstructionErrorNs,
		2.0);
	TestTrue(
		TEXT("reconciliation is within tolerance"),
		Result.bWithinTolerance);

	Input.PairedDeltaP50Ns = 80.0;
	TestTrue(
		TEXT("large reconstruction error still evaluates"),
		FAvidScriptPerfCostLadder::EvaluateReconciliation(
			Input,
			Result,
			Error));
	TestFalse(
		TEXT("large reconstruction error fails tolerance"),
		Result.bWithinTolerance);

	Input.ObservedP50Ns = 0.0;
	TestFalse(
		TEXT("zero observation rejects"),
		FAvidScriptPerfCostLadder::EvaluateReconciliation(
			Input,
			Result,
			Error));
	TestTrue(
		TEXT("invalid reconciliation has explicit error"),
		Error.Contains(TEXT("positive observation")));
	return true;
}

#endif
