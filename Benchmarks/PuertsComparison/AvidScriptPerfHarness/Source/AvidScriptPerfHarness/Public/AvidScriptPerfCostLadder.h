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

class AVIDSCRIPTPERFHARNESS_API FAvidScriptPerfCostLadder
{
public:
	static bool Run(
		const FAvidScriptPerfCostLadderRequest& Request,
		const FAvidScriptPerfCostLadderDrivers& Drivers,
		TArray<FAvidScriptPerfCostLadderRecord>& OutRecords,
		FString& OutError);

	static FName GetStageName(EAvidScriptPerfCostStage Stage);
};
