#pragma once

#include "AvidScriptVmArtifact.h"
#include "AvidScriptWasmReloadTypes.h"

struct AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeArtifact
{
	FAvidScriptWasmReloadManifest Manifest;
	FAvidScriptVmOwnedArtifact VmArtifact;
	FAvidScriptVmBackendSelection BackendSelection;
	FString RequestedBackend;
	FString SelectedBackend;
	FString FallbackCategory;
	FString ExecutionPolicy;
	bool bUsesPrecompiledArtifact = false;

	static FAvidScriptRuntimeArtifact FromCanonicalWasm(
		const FAvidScriptWasmReloadManifest& InManifest,
		TConstArrayView<uint8> CanonicalWasmBytes,
		const FAvidScriptVmBackendSelection& InBackendSelection);
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeArtifactLoadResult
{
	bool bSucceeded = false;
	bool bUsesPrecompiledArtifact = false;
	bool bFellBackToJit = false;
	FString RequestedBackend;
	FString SelectedBackend;
	FString FallbackCategory;
	FString ExecutionPolicy;
	FString ExecutionPath;
	FAvidScriptWasmReloadManifestLoadResult CanonicalResult;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeArtifactLoader
{
public:
	static bool LoadFromFile(
		const FString& ManifestPath,
		FAvidScriptRuntimeArtifact& OutArtifact,
		FAvidScriptRuntimeArtifactLoadResult& OutResult);
};
