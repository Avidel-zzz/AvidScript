#pragma once

#include "AvidScriptWasmReloadTypes.h"

struct FAvidScriptRuntimeStateMigrationResult
{
	bool bAttempted = false;
	bool bSucceeded = false;
	int32 MigratedSlotCount = 0;
	int32 MigratedByteCount = 0;
	int32 SkippedSlotCount = 0;
	int32 AliasedSlotCount = 0;
	FString StableId;
	FString ErrorCategory;
	FString ErrorDetails;
};

class FAvidScriptRuntimeStateMigration
{
public:
	static bool Migrate(
		FAvidScriptWasmRuntimeInstance& PreviousRuntime,
		const FAvidScriptWasmReloadManifest& PreviousManifest,
		FAvidScriptWasmRuntimeInstance& CandidateRuntime,
		const FAvidScriptWasmReloadManifest& CandidateManifest,
		FAvidScriptRuntimeStateMigrationResult& OutResult);
};
