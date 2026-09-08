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
	bool bCooperativeSafepointProofVerified = false;
	FString CooperativeSafepointSiteSha256;

	FAvidScriptVmArtifactView MakeView(
		EAvidScriptVmArtifactTrust Trust) const
	{
		FAvidScriptVmArtifactView View;
		View.ExecutionBytes =
			ArtifactFormat == EAvidScriptVmArtifactFormat::WasmBytecode
				&& ExecutionBytes.IsEmpty()
			? MakeArrayView(CanonicalWasmBytes)
			: MakeArrayView(ExecutionBytes);
		View.ArtifactFormat = ArtifactFormat;
		View.CanonicalWasmBytes = CanonicalWasmBytes;
		View.ExecutionIdentity = ExecutionIdentity;
		View.CanonicalWasmIdentity = CanonicalWasmIdentity;
		View.CompilerBuildIdentity = CompilerBuildIdentity;
		View.TargetTriple = TargetTriple;
		View.Trust = Trust;
		View.bCooperativeSafepointProofVerified =
			bCooperativeSafepointProofVerified;
		View.CooperativeSafepointSiteSha256 =
			CooperativeSafepointSiteSha256;
		return View;
	}
};

struct FAvidScriptVmCooperativeSafepointReceipt
{
	int32 ReceiptSchemaVersion = 0;
	FString GuestIrIdentity;
	FString CanonicalWasmIdentity;
	int32 ProofSchemaVersion = 0;
	uint32 PollInterval = 0;
	uint32 LoopPollBlockCount = 0;
	uint32 RecursiveFunctionCount = 0;
	uint32 SiteCount = 0;
	FString SiteSha256;
	bool bVerified = false;
};

struct FAvidScriptVmArtifactCompileRequest
{
	FAvidScriptVmBackendSelection Selection;
	TArrayView<const uint8> CanonicalWasmBytes;
	FString TargetTriple;
	bool bConsumeFuel = true;
	bool bEpochInterruption = true;
	FAvidScriptVmCooperativeSafepointReceipt CooperativeSafepointReceipt;
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

AVIDSCRIPTVM_API int32 ReleaseAvidScriptVmArtifactMemoryCache();
