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

struct AVIDSCRIPTVM_API FAvidScriptWasmModuleLayout
{
	uint32 ImportedFunctionCount = 0;
	uint32 DefinedFunctionCount = 0;
	TArray<FAvidScriptWasmFunctionImport> FunctionImports;
	TArray<FAvidScriptWasmFunctionExport> FunctionExports;
};

AVIDSCRIPTVM_API bool InspectAvidScriptWasmModuleLayout(
	TConstArrayView<uint8> Bytecode,
	FAvidScriptWasmModuleLayout& OutLayout,
	FString& OutError);
