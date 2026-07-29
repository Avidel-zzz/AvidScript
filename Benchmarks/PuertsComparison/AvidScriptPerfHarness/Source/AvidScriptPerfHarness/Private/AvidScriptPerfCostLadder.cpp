#include "AvidScriptPerfCostLadder.h"

#include "HAL/PlatformTime.h"

namespace
{
	uint32 RunNativeNoOp(const uint64 Iterations, const uint32 Seed)
	{
		uint32 Value = Seed;
		for (uint64 Index = 0; Index < Iterations; ++Index)
		{
			Value = (Value << 5) | (Value >> 27);
			Value ^= static_cast<uint32>(Index) + 0x9e3779b9U;
		}
		return Value;
	}

	bool ExecuteStage(
		const EAvidScriptPerfCostStage Stage,
		const FAvidScriptPerfCostLadderRequest& Request,
		const FAvidScriptPerfCostDriver& Driver,
		TArray<FAvidScriptPerfCostLadderRecord>& OutRecords,
		FString& OutError)
	{
		FAvidScriptPerfCostLadderRecord& Record = OutRecords.Emplace_GetRef();
		Record.Stage = FAvidScriptPerfCostLadder::GetStageName(Stage);
		Record.Iterations = Request.Iterations;

		if (!Driver)
		{
			OutError = FString::Printf(TEXT("No measurement driver is wired for cost stage '%s'."), *Record.Stage.ToString());
			return false;
		}

		const uint64 BeginCycles = FPlatformTime::Cycles64();
		FString DriverError;
		Record.bCorrect = Driver(Request.Iterations, Request.Seed, Record.Checksum, DriverError);
		Record.ElapsedCycles = FPlatformTime::Cycles64() - BeginCycles;
		if (!Record.bCorrect)
		{
			OutError = FString::Printf(TEXT("Cost stage '%s' failed: %s"), *Record.Stage.ToString(), *DriverError);
			return false;
		}

		return true;
	}
}

bool FAvidScriptPerfCostLadder::Run(
	const FAvidScriptPerfCostLadderRequest& Request,
	const FAvidScriptPerfCostLadderDrivers& Drivers,
	TArray<FAvidScriptPerfCostLadderRecord>& OutRecords,
	FString& OutError)
{
	OutRecords.Reset();
	OutError.Reset();

	const FAvidScriptPerfCostDriver NativeNoOp = [](const uint64 Iterations, const uint32 Seed, uint32& OutChecksum, FString&)
	{
		OutChecksum = RunNativeNoOp(Iterations, Seed);
		return true;
	};

	return ExecuteStage(EAvidScriptPerfCostStage::NativeNoOp, Request, NativeNoOp, OutRecords, OutError)
		&& ExecuteStage(EAvidScriptPerfCostStage::GuestLoopBaseline, Request, Drivers.GuestLoopBaseline, OutRecords, OutError)
		&& ExecuteStage(EAvidScriptPerfCostStage::GenericExport, Request, Drivers.GenericExport, OutRecords, OutError)
		&& ExecuteStage(EAvidScriptPerfCostStage::PreparedExport, Request, Drivers.PreparedExport, OutRecords, OutError)
		&& ExecuteStage(EAvidScriptPerfCostStage::TypedEmptyImport, Request, Drivers.TypedEmptyImport, OutRecords, OutError)
		&& ExecuteStage(EAvidScriptPerfCostStage::GenericEmptyImport, Request, Drivers.GenericEmptyImport, OutRecords, OutError)
		&& ExecuteStage(EAvidScriptPerfCostStage::TypedI32PairImport, Request, Drivers.TypedI32PairImport, OutRecords, OutError);
}

FName FAvidScriptPerfCostLadder::GetStageName(const EAvidScriptPerfCostStage Stage)
{
	switch (Stage)
	{
	case EAvidScriptPerfCostStage::NativeNoOp:
		return TEXT("native_no_op");
	case EAvidScriptPerfCostStage::GuestLoopBaseline:
		return TEXT("guest_loop_baseline");
	case EAvidScriptPerfCostStage::GenericExport:
		return TEXT("generic_export");
	case EAvidScriptPerfCostStage::PreparedExport:
		return TEXT("prepared_export");
	case EAvidScriptPerfCostStage::TypedEmptyImport:
		return TEXT("typed_empty_import");
	case EAvidScriptPerfCostStage::GenericEmptyImport:
		return TEXT("generic_empty_import");
	case EAvidScriptPerfCostStage::TypedI32PairImport:
		return TEXT("typed_i32_pair_import");
	default:
		return NAME_None;
	}
}

bool FAvidScriptPerfCostLadder::EvaluateReconciliation(
	const FAvidScriptPerfCostReconciliationInput& Input,
	FAvidScriptPerfCostReconciliationResult& OutResult,
	FString& OutError)
{
	OutResult = FAvidScriptPerfCostReconciliationResult();
	OutError.Reset();
	if (!FMath::IsFinite(Input.ObservedP50Ns)
		|| !FMath::IsFinite(Input.BaselineP50Ns)
		|| !FMath::IsFinite(Input.PairedDeltaP50Ns)
		|| !FMath::IsFinite(Input.MaximumErrorRatio)
		|| Input.ObservedP50Ns <= 0.0
		|| Input.BaselineP50Ns < 0.0
		|| Input.MaximumErrorRatio < 0.0)
	{
		OutError = TEXT(
			"Cost reconciliation requires finite paired P50 timings, a positive observation, and a non-negative tolerance.");
		return false;
	}

	OutResult.ReconstructedP50Ns =
		Input.BaselineP50Ns + Input.PairedDeltaP50Ns;
	OutResult.ReconstructionErrorNs = FMath::Abs(
		OutResult.ReconstructedP50Ns - Input.ObservedP50Ns);
	OutResult.ReconstructionErrorRatio =
		OutResult.ReconstructionErrorNs / Input.ObservedP50Ns;
	OutResult.bWithinTolerance =
		OutResult.ReconstructionErrorRatio <= Input.MaximumErrorRatio;
	return true;
}
