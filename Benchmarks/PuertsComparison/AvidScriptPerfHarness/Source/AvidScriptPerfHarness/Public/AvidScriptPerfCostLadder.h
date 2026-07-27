#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptPerfCostStage : uint8
{
	NativeNoOp,
	CachedExport,
	TypedEmptyImport,
	GenericEmptyImport,
	ImmutableEnvironmentDispatch
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
	FAvidScriptPerfCostDriver CachedExport;
	FAvidScriptPerfCostDriver TypedEmptyImport;
	FAvidScriptPerfCostDriver GenericEmptyImport;
	FAvidScriptPerfCostDriver ImmutableEnvironmentDispatch;
};

struct FAvidScriptPerfCostBudgetInput
{
	double TypedEmptyP95Ns = 0.0;
	double PuertsStaticScalarP50Ns = 0.0;
	double GenericEmptyP95Ns = 0.0;
	double ImmutableEnvironmentDispatchP95Ns = 0.0;
};

struct FAvidScriptPerfCostBudgetResult
{
	double TypedToPuertsRatio = 0.0;
	double GenericMinusTypedNs = 0.0;
	double ImmutableDispatchMinusTypedNs = 0.0;
	bool bSingleCallBudgetConstrained = false;
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

	static bool EvaluateBudget(
		const FAvidScriptPerfCostBudgetInput& Input,
		FAvidScriptPerfCostBudgetResult& OutResult,
		FString& OutError);
};
