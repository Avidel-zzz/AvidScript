#pragma once

#include "CoreMinimal.h"

struct AVIDSCRIPTVM_API FAvidScriptWasmFunctionExport
{
	FString Name;
	uint32 FunctionIndex = MAX_uint32;
};

struct AVIDSCRIPTVM_API FAvidScriptWasmFunctionImport
{
	FString ModuleName;
	FString ImportName;
};

struct AVIDSCRIPTVM_API FAvidScriptWasmCooperativeSafepointProof
{
	bool bPresent = false;
	uint32 SchemaVersion = 0;
	uint32 PollInterval = 0;
	uint32 LoopPollBlockCount = 0;
	uint32 RecursiveFunctionCount = 0;
	uint32 SiteCount = 0;
	FString Mode;
	FString GuestIrIdentity;
	FString SiteSha256;
	bool bHasSiteIdentity = false;
};

struct AVIDSCRIPTVM_API FAvidScriptWasmModuleLayout
{
	uint32 ImportedFunctionCount = 0;
	uint32 DefinedFunctionCount = 0;
	TArray<FAvidScriptWasmFunctionImport> FunctionImports;
	TArray<FAvidScriptWasmFunctionExport> FunctionExports;
	FAvidScriptWasmCooperativeSafepointProof CooperativeSafepointProof;
};

AVIDSCRIPTVM_API bool InspectAvidScriptWasmModuleLayout(
	TConstArrayView<uint8> Bytecode,
	FAvidScriptWasmModuleLayout& OutLayout,
	FString& OutError);

// Copies one bounded custom-section payload. Duplicate requested names are rejected;
// interpretation and capability authorization remain with the caller's domain.
AVIDSCRIPTVM_API bool ReadAvidScriptWasmCustomSection(
	TConstArrayView<uint8> Bytecode, const FString& SectionName, uint32 MaxPayloadBytes,
	TArray<uint8>& OutPayload, bool& bOutFound, FString& OutError);
