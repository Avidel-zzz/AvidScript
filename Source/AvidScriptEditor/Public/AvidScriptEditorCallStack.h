#pragma once

#include "AvidScriptEditorDiagnosticNavigation.h"
#include "AvidScriptWasmDiagnostics.h"

#include "CoreMinimal.h"

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorCallStackFrame
{
	int32 Ordinal = INDEX_NONE;
	EAvidScriptWasmDiagnosticFrameKind Kind = EAvidScriptWasmDiagnosticFrameKind::Wasm;
	FString DisplayName;
	FString RawFunctionToken;
	uint32 FunctionIndex = MAX_uint32;
	uint32 FunctionOffset = 0;
	FString SourceKind;
	FAvidScriptEditorSourceLocation SourceLocation;
	bool bSourceMapped = false;
	bool bSequencePointMapped = false;

	bool IsSourceNavigable() const
	{
		return bSourceMapped && SourceLocation.IsValid();
	}
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorCallStack
{
public:
	static void Build(
		TConstArrayView<FAvidScriptWasmDiagnosticFrame> RuntimeFrames,
		TArray<FAvidScriptEditorCallStackFrame>& OutFrames);

	static bool ResolveSource(
		const FAvidScriptEditorCallStackFrame& Frame,
		const FString& ProjectDirectory,
		FAvidScriptEditorDiagnosticNavigationResult& OutResult);

	static bool OpenSource(
		const FAvidScriptEditorCallStackFrame& Frame,
		const FString& ProjectDirectory,
		FAvidScriptEditorDiagnosticNavigationResult& OutResult);
};
