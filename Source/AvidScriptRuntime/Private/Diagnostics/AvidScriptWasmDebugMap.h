#pragma once

#include "AvidScriptVmBackend.h"
#include "AvidScriptWasmModuleLayout.h"
#include "AvidScriptWasmDiagnostics.h"

class FAvidScriptWasmDebugMap
{
public:
	static bool LoadAndValidate(
		const FString& DebugMapPath,
		const FString& ExpectedArtifactSha256,
		const FAvidScriptWasmDebugProvenance& ExpectedProvenance,
		TConstArrayView<FAvidScriptWasmFunctionExport> FunctionExports,
		TSharedPtr<const FAvidScriptWasmDebugMap>& OutMap,
		FString& OutErrorCategory,
		FString& OutErrorSource);

	void MapFrames(
		TConstArrayView<FAvidScriptVmStackFrame> VmFrames,
		TArray<FAvidScriptWasmDiagnosticFrame>& OutFrames) const;

private:
	struct FSequencePoint
	{
		uint32 FunctionOffset = 0;
		FString Kind;
		int32 Line = 0;
		int32 Column = 0;
		int32 EndLine = 0;
		int32 EndColumn = 0;
		bool bHidden = false;
	};

	struct FFunction
	{
		FString DisplayName;
		int32 Line = 0;
		int32 Column = 0;
		int32 EndLine = 0;
		int32 EndColumn = 0;
		TArray<FSequencePoint> SequencePoints;
	};

	FString SourceFile;
	TMap<uint32, FFunction> Functions;
	TMap<FString, uint32> FunctionIndicesByExportName;
};
