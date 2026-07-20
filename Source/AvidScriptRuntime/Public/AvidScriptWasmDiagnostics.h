#pragma once

#include "CoreMinimal.h"

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmDebugProvenance
{
	FString GuestModuleId;
	FString SourceFile;
	FString SourceSha256;
	FString FrontendSha256;
	FString SemanticSha256;
	FString GuestIrSha256;
	uint32 ImportedFunctionCount = 0;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmDiagnosticFrame
{
	uint32 FunctionIndex = MAX_uint32;
	uint32 FunctionOffset = 0;
	FString RawFunctionToken;
	FString FunctionName;
	FString SourceFile;
	int32 Line = 0;
	int32 Column = 0;
	int32 EndLine = 0;
	int32 EndColumn = 0;
	bool bSourceMapped = false;
};
