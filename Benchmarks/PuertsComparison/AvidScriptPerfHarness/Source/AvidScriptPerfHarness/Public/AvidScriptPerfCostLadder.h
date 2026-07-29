#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptPerfCostStage : uint8
{
	NativeNoOp,
	GuestLoopBaseline,
	GenericExport,
	PreparedExport,
	TypedEmptyImport,
	GenericEmptyImport,
	TypedI32PairImport
};

struct FAvidScriptPerfCostLadderRecord
{
	FName Stage;
	uint64 Iterations = 0;
	uint64 ElapsedCycles = 0;
	uint32 Checksum = 0;
	bool bCorrect = false;
};

struct FAvidScriptPerfCostLadderRequest
{
	uint64 Iterations = 0;
	uint32 Seed = 0;
};

using FAvidScriptPerfCostDriver = TFunction<bool(uint64, uint32, uint32&, FString&)>;

struct FAvidScriptPerfCostLadderDrivers
{
	FAvidScriptPerfCostDriver GuestLoopBaseline;
	FAvidScriptPerfCostDriver GenericExport;
	FAvidScriptPerfCostDriver PreparedExport;
	FAvidScriptPerfCostDriver TypedEmptyImport;
	FAvidScriptPerfCostDriver GenericEmptyImport;
	FAvidScriptPerfCostDriver TypedI32PairImport;
};

struct FAvidScriptPerfCostReconciliationInput
{
	double ObservedP50Ns = 0.0;
	double BaselineP50Ns = 0.0;
	double PairedDeltaP50Ns = 0.0;
	double MaximumErrorRatio = 0.15;
};

struct FAvidScriptPerfCostReconciliationResult
{
	double ReconstructedP50Ns = 0.0;
	double ReconstructionErrorNs = 0.0;
	double ReconstructionErrorRatio = 0.0;
	bool bWithinTolerance = false;
};

class AVIDSCRIPTPERFHARNESS_API FAvidScriptPerfCostLadder
{
public:
	static bool Run(
		const FAvidScriptPerfCostLadderRequest& Request,
		const FAvidScriptPerfCostLadderDrivers& Drivers,
		TArray<FAvidScriptPerfCostLadderRecord>& OutRecords,
		FString& OutError);

	static FName GetStageName(EAvidScriptPerfCostStage Stage);

	static bool EvaluateReconciliation(
		const FAvidScriptPerfCostReconciliationInput& Input,
		FAvidScriptPerfCostReconciliationResult& OutResult,
		FString& OutError);
};
