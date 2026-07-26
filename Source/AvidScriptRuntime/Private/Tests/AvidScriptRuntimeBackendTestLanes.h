#pragma once

#include "AvidScriptWasmRuntime.h"

#include "Misc/AutomationTest.h"

struct FAvidScriptRuntimeBackendTestLane
{
	const TCHAR* Name = TEXT("");
	FAvidScriptVmBackendSelection Selection;
	EAvidScriptVmBackendKind ExpectedKind = EAvidScriptVmBackendKind::Wamr;
	EAvidScriptVmExecutionMode ExpectedMode = EAvidScriptVmExecutionMode::Interpreter;
};

inline TArray<FAvidScriptRuntimeBackendTestLane> GetAvidScriptRuntimeBackendTestLanes()
{
	TArray<FAvidScriptRuntimeBackendTestLane> Lanes;

	FAvidScriptRuntimeBackendTestLane Wamr;
	Wamr.Name = TEXT("WAMR");
	Wamr.Selection.BackendKind = EAvidScriptVmBackendKind::Wamr;
	Wamr.Selection.ExecutionMode = EAvidScriptVmExecutionMode::Interpreter;
	Wamr.Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	Wamr.Selection.bAllowFallback = false;
	Lanes.Add(Wamr);

	FAvidScriptRuntimeBackendTestLane Wasmtime;
	Wasmtime.Name = TEXT("Wasmtime");
	Wasmtime.Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Wasmtime.Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Wasmtime.Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	Wasmtime.Selection.bAllowFallback = false;
	FAvidScriptVmError ProbeError;
	if (CreateAvidScriptVmBackend(Wasmtime.Selection, ProbeError))
	{
		Wasmtime.ExpectedKind = EAvidScriptVmBackendKind::Wasmtime;
		Wasmtime.ExpectedMode = EAvidScriptVmExecutionMode::Jit;
		Lanes.Add(Wasmtime);
	}
	return Lanes;
}

inline FString AvidScriptRuntimeLaneLabel(
	const FAvidScriptRuntimeBackendTestLane& Lane,
	const TCHAR* Assertion)
{
	return FString::Printf(TEXT("%s: %s"), Lane.Name, Assertion);
}

inline bool TestAvidScriptRuntimeLaneIdentity(
	FAutomationTestBase& Test,
	const FAvidScriptRuntimeBackendTestLane& Lane,
	const FAvidScriptWasmSmokeResult& Result)
{
	bool bMatches = true;
	bMatches &= Test.TestEqual(
		*AvidScriptRuntimeLaneLabel(Lane, TEXT("actual backend kind")),
		Result.BackendInfo.Kind,
		Lane.ExpectedKind);
	bMatches &= Test.TestEqual(
		*AvidScriptRuntimeLaneLabel(Lane, TEXT("actual execution mode")),
		Result.BackendInfo.ExecutionMode,
		Lane.ExpectedMode);
	bMatches &= Test.TestEqual(
		*AvidScriptRuntimeLaneLabel(Lane, TEXT("actual artifact format")),
		Result.BackendInfo.ArtifactFormat,
		EAvidScriptVmArtifactFormat::WasmBytecode);
	return bMatches;
}
