#include "AvidScriptEditorCallStack.h"

namespace
{
FString MakeAvidScriptCallStackDisplayName(const FAvidScriptWasmDiagnosticFrame& Frame)
{
	switch (Frame.Kind)
	{
	case EAvidScriptWasmDiagnosticFrameKind::HostImport:
		return FString::Printf(TEXT("UE host import: %s"), *Frame.FunctionName);
	case EAvidScriptWasmDiagnosticFrameKind::HostEntry:
		return FString::Printf(TEXT("UE entry: %s"), *Frame.FunctionName);
	case EAvidScriptWasmDiagnosticFrameKind::CSharp:
		return Frame.FunctionName;
	case EAvidScriptWasmDiagnosticFrameKind::Wasm:
	default:
		return Frame.FunctionName.IsEmpty()
			? Frame.RawFunctionToken
			: Frame.FunctionName;
	}
}
} // namespace

void FAvidScriptEditorCallStack::Build(
	TConstArrayView<FAvidScriptWasmDiagnosticFrame> RuntimeFrames,
	TArray<FAvidScriptEditorCallStackFrame>& OutFrames)
{
	OutFrames.Reset(RuntimeFrames.Num());
	for (int32 Index = 0; Index < RuntimeFrames.Num(); ++Index)
	{
		const FAvidScriptWasmDiagnosticFrame& RuntimeFrame = RuntimeFrames[Index];
		FAvidScriptEditorCallStackFrame& Frame = OutFrames.AddDefaulted_GetRef();
		Frame.Ordinal = Index;
		Frame.Kind = RuntimeFrame.Kind;
		Frame.DisplayName = MakeAvidScriptCallStackDisplayName(RuntimeFrame);
		Frame.RawFunctionToken = RuntimeFrame.RawFunctionToken;
		Frame.FunctionIndex = RuntimeFrame.FunctionIndex;
		Frame.FunctionOffset = RuntimeFrame.FunctionOffset;
		Frame.SourceKind = RuntimeFrame.SourceKind;
		Frame.SourceLocation.File = RuntimeFrame.SourceFile;
		Frame.SourceLocation.SourceSha256 = RuntimeFrame.SourceSha256;
		Frame.SourceLocation.Line = RuntimeFrame.Line;
		Frame.SourceLocation.Column = RuntimeFrame.Column;
		Frame.bSourceMapped = RuntimeFrame.bSourceMapped;
		Frame.bSequencePointMapped = RuntimeFrame.bSequencePointMapped;
	}
}

bool FAvidScriptEditorCallStack::ResolveSource(
	const FAvidScriptEditorCallStackFrame& Frame,
	const FString& ProjectDirectory,
	FAvidScriptEditorDiagnosticNavigationResult& OutResult)
{
	return FAvidScriptEditorDiagnosticNavigation::Resolve(
		Frame.SourceLocation,
		ProjectDirectory,
		OutResult);
}

bool FAvidScriptEditorCallStack::OpenSource(
	const FAvidScriptEditorCallStackFrame& Frame,
	const FString& ProjectDirectory,
	FAvidScriptEditorDiagnosticNavigationResult& OutResult)
{
	return FAvidScriptEditorDiagnosticNavigation::Open(
		Frame.SourceLocation,
		ProjectDirectory,
		OutResult);
}
