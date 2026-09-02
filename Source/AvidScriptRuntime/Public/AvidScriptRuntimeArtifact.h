#pragma once

#include "AvidScriptVmArtifact.h"
#include "AvidScriptWasmReloadTypes.h"
#include "Packages/AvidScriptModulePackage.h"

struct AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeArtifact
{
	FAvidScriptWasmReloadManifest Manifest;
	FAvidScriptVmOwnedArtifact VmArtifact;
	FAvidScriptVmBackendSelection BackendSelection;
	FString RequestedBackend;
	FString SelectedBackend;
	FString FallbackCategory;
	FString ExecutionPolicy;
	EAvidScriptVmArtifactTrust ArtifactTrust =
		EAvidScriptVmArtifactTrust::Untrusted;
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
	FString PackageId;
	bool bVerifiedPublishedPackage = false;
	FAvidScriptWasmReloadManifestLoadResult CanonicalResult;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptRuntimeArtifactLoader
{
public:
	static bool LoadFromFile(
		const FString& ManifestPath,
		FAvidScriptRuntimeArtifact& OutArtifact,
		FAvidScriptRuntimeArtifactLoadResult& OutResult);

	static bool LoadPublishedModule(
		FName ModuleId,
		const FString& ExpectedPackageId,
		FAvidScriptRuntimeArtifact& OutArtifact,
		FAvidScriptRuntimeArtifactLoadResult& OutResult,
		FAvidScriptResolvedModulePackage* OutPackage = nullptr);
};
