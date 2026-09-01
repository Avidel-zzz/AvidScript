#pragma once

#include "AvidScriptDebug.h"
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
	void BuildBreakpointCatalog(TArray<FAvidScriptDebugBreakpoint>& OutBreakpoints) const;
	bool BuildVariableSnapshot(
		uint64 ProbeId,
		TConstArrayView<uint8> FrameBytes,
		FAvidScriptDebugVariablesSnapshot& OutSnapshot,
		FString& OutError) const;

private:
	struct FSequencePoint
	{
		uint32 FunctionOffset = 0;
		uint64 ProbeId = 0;
		FString Kind;
		int32 Start = 0;
		int32 Length = 0;
		int32 Line = 0;
		int32 Column = 0;
		int32 EndLine = 0;
		int32 EndColumn = 0;
		bool bHidden = false;
		bool bHasProbeId = false;
	};

	struct FVariable
	{
		FString SymbolId;
		FString Name;
		FString Kind;
		FString TypeId;
		FString ValueKind;
		FString Storage;
		int32 Offset = 0;
		int32 ByteSize = 0;
		int32 DeclarationStart = 0;
		int32 DeclarationLength = 0;
		int32 ScopeStart = 0;
		int32 ScopeLength = 0;
	};

	struct FFunction
	{
		FString DisplayName;
		int32 Line = 0;
		int32 Column = 0;
		int32 EndLine = 0;
		int32 EndColumn = 0;
		int32 FrameByteCount = 0;
		TArray<FSequencePoint> SequencePoints;
		TArray<FVariable> Variables;
	};

	FString SourceFile;
	FString SourceSha256;
	TMap<uint32, FFunction> Functions;
	TMap<FString, uint32> FunctionIndicesByExportName;
	TMap<uint64, uint32> FunctionIndicesByProbeId;
};
