#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct FAvidScriptRuntimeCooperativeSafepointProvenance
{
	bool bEnabled = false;
	FString ReceiptSha256;
	FString GuestIrSha256;
	FString SiteSha256;
	uint32 ProofSchemaVersion = 0;
	uint32 PollInterval = 0;
	uint32 LoopPollBlockCount = 0;
	uint32 RecursiveFunctionCount = 0;
	uint32 SiteCount = 0;
};

bool ParseAvidScriptRuntimeCooperativeSafepointProvenance(
	const FJsonObject& RootObject,
	const FJsonObject& ExecutionObject,
	const FString& CompilerBuildIdentity,
	FAvidScriptRuntimeCooperativeSafepointProvenance& OutProvenance,
	FString& OutError);
