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
		&& ExecuteStage(EAvidScriptPerfCostStage::CachedExport, Request, Drivers.CachedExport, OutRecords, OutError)
		&& ExecuteStage(EAvidScriptPerfCostStage::TypedEmptyImport, Request, Drivers.TypedEmptyImport, OutRecords, OutError)
		&& ExecuteStage(EAvidScriptPerfCostStage::GenericEmptyImport, Request, Drivers.GenericEmptyImport, OutRecords, OutError)
		&& ExecuteStage(EAvidScriptPerfCostStage::ImmutableEnvironmentDispatch, Request, Drivers.ImmutableEnvironmentDispatch, OutRecords, OutError);
}

FName FAvidScriptPerfCostLadder::GetStageName(const EAvidScriptPerfCostStage Stage)
{
	switch (Stage)
	{
	case EAvidScriptPerfCostStage::NativeNoOp:
		return TEXT("native_no_op");
	case EAvidScriptPerfCostStage::CachedExport:
		return TEXT("cached_export");
	case EAvidScriptPerfCostStage::TypedEmptyImport:
		return TEXT("typed_empty_import");
	case EAvidScriptPerfCostStage::GenericEmptyImport:
		return TEXT("generic_empty_import");
	case EAvidScriptPerfCostStage::ImmutableEnvironmentDispatch:
		return TEXT("immutable_environment_dispatch");
	default:
		return NAME_None;
	}
}

bool FAvidScriptPerfCostLadder::EvaluateBudget(
	const FAvidScriptPerfCostBudgetInput& Input,
	FAvidScriptPerfCostBudgetResult& OutResult,
	FString& OutError)
{
	OutResult = FAvidScriptPerfCostBudgetResult();
	OutError.Reset();
	if (!FMath::IsFinite(Input.TypedEmptyP95Ns)
		|| !FMath::IsFinite(Input.PuertsStaticScalarP50Ns)
		|| !FMath::IsFinite(Input.GenericEmptyP95Ns)
		|| !FMath::IsFinite(Input.ImmutableEnvironmentDispatchP95Ns)
		|| Input.TypedEmptyP95Ns < 0.0
		|| Input.PuertsStaticScalarP50Ns <= 0.0
		|| Input.GenericEmptyP95Ns < 0.0
		|| Input.ImmutableEnvironmentDispatchP95Ns < 0.0)
	{
		OutError = TEXT("Cost budget inputs must be finite non-negative timings with a positive Puerts baseline.");
		return false;
	}

	OutResult.TypedToPuertsRatio =
		Input.TypedEmptyP95Ns / Input.PuertsStaticScalarP50Ns;
	OutResult.GenericMinusTypedNs =
		Input.GenericEmptyP95Ns - Input.TypedEmptyP95Ns;
	OutResult.ImmutableDispatchMinusTypedNs =
		Input.ImmutableEnvironmentDispatchP95Ns - Input.TypedEmptyP95Ns;
	OutResult.bSingleCallBudgetConstrained =
		OutResult.TypedToPuertsRatio >= 0.70;
	return true;
}
