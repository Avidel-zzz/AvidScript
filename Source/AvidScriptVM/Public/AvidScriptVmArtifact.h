#pragma once

#include "AvidScriptVmBackend.h"

struct FAvidScriptVmOwnedArtifact
{
	TArray<uint8> ExecutionBytes;
	EAvidScriptVmArtifactFormat ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmBytecode;
	TArray<uint8> CanonicalWasmBytes;
	FString ExecutionIdentity;
	FString CanonicalWasmIdentity;
	FString CompilerBuildIdentity;
	FString TargetTriple;
	FString AttestationId;

	FAvidScriptVmArtifactView MakeView(
		EAvidScriptVmArtifactTrust Trust) const
	{
		FAvidScriptVmArtifactView View;
		View.ExecutionBytes = ExecutionBytes;
		View.ArtifactFormat = ArtifactFormat;
		View.CanonicalWasmBytes = CanonicalWasmBytes;
		View.ExecutionIdentity = ExecutionIdentity;
		View.CanonicalWasmIdentity = CanonicalWasmIdentity;
		View.CompilerBuildIdentity = CompilerBuildIdentity;
		View.TargetTriple = TargetTriple;
		View.Trust = Trust;
		return View;
	}
};

struct FAvidScriptVmArtifactCompileRequest
{
	FAvidScriptVmBackendSelection Selection;
	TArrayView<const uint8> CanonicalWasmBytes;
};

struct FAvidScriptVmArtifactCompileResult
{
	bool bSucceeded = false;
	bool bCacheHit = false;
	double CompileMs = 0.0;
	FAvidScriptVmOwnedArtifact Artifact;
	FAvidScriptVmError Error;
};

AVIDSCRIPTVM_API bool CompileAvidScriptVmArtifact(
	const FAvidScriptVmArtifactCompileRequest& Request,
	FAvidScriptVmArtifactCompileResult& OutResult);

AVIDSCRIPTVM_API bool AuthorizeAvidScriptVmArtifact(
	const FString& AttestationId,
	const FAvidScriptVmOwnedArtifact& Artifact);
