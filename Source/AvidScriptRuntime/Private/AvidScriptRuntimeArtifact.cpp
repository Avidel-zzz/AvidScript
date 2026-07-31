#include "AvidScriptRuntimeArtifact.h"

#include "AvidScriptHash.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
FAvidScriptVmBackendSelection MakeLegacyBackendSelection()
{
	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wamr;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Auto;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	Selection.bAllowFallback = true;
	return Selection;
}

FAvidScriptVmBackendSelection MakeWasmtimeJitSelection()
{
	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	return Selection;
}

FAvidScriptVmBackendSelection MakeWasmtimePrecompiledSelection()
{
	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Aot;
	Selection.ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	return Selection;
}

void SetArtifactLoadFailure(
	FAvidScriptRuntimeArtifactLoadResult& OutResult,
	const FString& Category,
	const FString& Details,
	const FString& NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.CanonicalResult.bSucceeded = false;
	OutResult.CanonicalResult.ErrorCategory = Category;
	OutResult.CanonicalResult.NextAction = NextAction;
	OutResult.CanonicalResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript runtime artifact load error | manifest=%s | execution=%s | category=%s | details=%s | next=%s"),
		OutResult.CanonicalResult.ManifestPath.IsEmpty()
			? TEXT("<none>")
			: *OutResult.CanonicalResult.ManifestPath,
		OutResult.ExecutionPath.IsEmpty()
			? TEXT("<none>")
			: *OutResult.ExecutionPath,
		*Category,
		*Details,
		*NextAction);
}

bool IsLowercaseSha256(const FString& Value)
{
	if (Value.Len() != 64 || Value != Value.ToLower())
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsHexDigit(Character))
		{
			return false;
		}
	}
	return true;
}

bool IsLowercaseAttestationId(const FString& Value)
{
	if (Value.Len() != 32 || Value != Value.ToLower())
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsHexDigit(Character))
		{
			return false;
		}
	}
	return true;
}

FString NormalizeArtifactPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

bool HasParentTraversal(const FString& Path)
{
	FString Normalized = Path;
	FPaths::NormalizeFilename(Normalized);
	TArray<FString> Segments;
	Normalized.ParseIntoArray(Segments, TEXT("/"), false);
	return Segments.Contains(TEXT(".."));
}

bool ResolveExecutionPath(
	const FString& ManifestPath,
	const FString& RelativePath,
	FString& OutPath)
{
	OutPath.Reset();
	if (RelativePath.IsEmpty()
		|| !FPaths::IsRelative(RelativePath)
		|| HasParentTraversal(RelativePath)
		|| FPaths::GetExtension(RelativePath, true) != TEXT(".cwasm"))
	{
		return false;
	}

	FString NormalizedRelative = RelativePath;
	FPaths::NormalizeFilename(NormalizedRelative);
	const FString ManifestDirectory =
		NormalizeArtifactPath(FPaths::GetPath(ManifestPath));
	const FString ProjectDirectory = NormalizeArtifactPath(FPaths::ProjectDir());
	const FString ManifestCandidate = NormalizeArtifactPath(
		FPaths::Combine(ManifestDirectory, NormalizedRelative));
	const FString ProjectCandidate = NormalizeArtifactPath(
		FPaths::Combine(ProjectDirectory, NormalizedRelative));
	const bool bLooksProjectRelative =
		NormalizedRelative.StartsWith(TEXT("Saved/"), ESearchCase::IgnoreCase)
		|| NormalizedRelative.StartsWith(TEXT("Content/"), ESearchCase::IgnoreCase)
		|| NormalizedRelative.StartsWith(TEXT("Plugins/"), ESearchCase::IgnoreCase);
	const FString Candidates[] = {
		bLooksProjectRelative ? ProjectCandidate : ManifestCandidate,
		bLooksProjectRelative ? ManifestCandidate : ProjectCandidate
	};
	for (const FString& Candidate : Candidates)
	{
		const bool bAllowed =
			FPaths::IsUnderDirectory(Candidate, ProjectDirectory)
			|| FPaths::IsUnderDirectory(Candidate, ManifestDirectory);
		if (bAllowed && FPaths::FileExists(Candidate))
		{
			OutPath = Candidate;
			return true;
		}
	}
	OutPath = Candidates[0];
	return true;
}

bool RequireExecutionString(
	const FJsonObject& ExecutionObject,
	const TCHAR* FieldName,
	FString& OutValue)
{
	return ExecutionObject.TryGetStringField(FieldName, OutValue)
		&& !OutValue.IsEmpty();
}

bool ApplyJitFallback(
	const FString& Category,
	const FString& Policy,
	const FAvidScriptWasmReloadManifest& Manifest,
	TConstArrayView<uint8> CanonicalWasmBytes,
	FAvidScriptRuntimeArtifact& OutArtifact,
	FAvidScriptRuntimeArtifactLoadResult& OutResult)
{
	if (Policy == TEXT("require_precompiled"))
	{
		SetArtifactLoadFailure(
			OutResult,
			Category,
			TEXT("The required Wasmtime serialized artifact is unavailable or untrusted."),
			TEXT("rebuild the artifact in this Editor session or choose PreferPrecompiled"));
		return false;
	}
	OutArtifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(
		Manifest,
		CanonicalWasmBytes,
		MakeWasmtimeJitSelection());
	OutArtifact.RequestedBackend = TEXT("wasmtime.cranelift.precompiled");
	OutArtifact.SelectedBackend = TEXT("wasmtime.cranelift.jit");
	OutArtifact.FallbackCategory = Category;
	OutArtifact.ExecutionPolicy = Policy;
	OutResult.bSucceeded = true;
	OutResult.bFellBackToJit = true;
	OutResult.RequestedBackend = OutArtifact.RequestedBackend;
	OutResult.SelectedBackend = OutArtifact.SelectedBackend;
	OutResult.FallbackCategory = Category;
	OutResult.ExecutionPolicy = Policy;
	return true;
}
} // namespace

FAvidScriptRuntimeArtifact FAvidScriptRuntimeArtifact::FromCanonicalWasm(
	const FAvidScriptWasmReloadManifest& InManifest,
	TConstArrayView<uint8> CanonicalWasmBytes,
	const FAvidScriptVmBackendSelection& InBackendSelection)
{
	FAvidScriptRuntimeArtifact Artifact;
	Artifact.Manifest = InManifest;
	if (!CanonicalWasmBytes.IsEmpty())
	{
		Artifact.VmArtifact.CanonicalWasmBytes.Append(
			CanonicalWasmBytes.GetData(),
			CanonicalWasmBytes.Num());
	}
	Artifact.VmArtifact.ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmBytecode;
	Artifact.VmArtifact.ExecutionIdentity =
		FAvidScriptHash::Sha256Hex(
			Artifact.VmArtifact.CanonicalWasmBytes);
	Artifact.VmArtifact.CanonicalWasmIdentity =
		Artifact.VmArtifact.ExecutionIdentity;
	Artifact.BackendSelection = InBackendSelection;
	return Artifact;
}

bool FAvidScriptRuntimeArtifactLoader::LoadFromFile(
	const FString& ManifestPath,
	FAvidScriptRuntimeArtifact& OutArtifact,
	FAvidScriptRuntimeArtifactLoadResult& OutResult)
{
	OutArtifact = FAvidScriptRuntimeArtifact();
	OutResult = FAvidScriptRuntimeArtifactLoadResult();
	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> CanonicalWasmBytes;
	if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			ManifestPath,
			Manifest,
			CanonicalWasmBytes,
			OutResult.CanonicalResult))
	{
		return false;
	}
	const FString& CanonicalManifestPath =
		OutResult.CanonicalResult.ManifestPath;

	FString ManifestJson;
	TSharedPtr<FJsonObject> RootObject;
	if (!FFileHelper::LoadFileToString(
			ManifestJson,
			*CanonicalManifestPath)
		|| !FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(ManifestJson),
			RootObject)
		|| !RootObject.IsValid())
	{
		SetArtifactLoadFailure(
			OutResult,
			TEXT("manifest_invalid"),
			TEXT("The canonical manifest could not be reparsed for execution metadata."),
			TEXT("rebuild the manifest and retry"));
		return false;
	}

	const TSharedPtr<FJsonObject>* ExecutionObjectPtr = nullptr;
	if (!RootObject->HasField(TEXT("execution")))
	{
		OutArtifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(
			Manifest,
			CanonicalWasmBytes,
			MakeLegacyBackendSelection());
		OutArtifact.RequestedBackend = TEXT("legacy.default");
		OutArtifact.SelectedBackend = TEXT("legacy.default");
		OutResult.bSucceeded = true;
		OutResult.RequestedBackend = OutArtifact.RequestedBackend;
		OutResult.SelectedBackend = OutArtifact.SelectedBackend;
		return true;
	}
	if (!RootObject->TryGetObjectField(
			TEXT("execution"),
			ExecutionObjectPtr)
		|| ExecutionObjectPtr == nullptr
		|| !ExecutionObjectPtr->IsValid())
	{
		SetArtifactLoadFailure(
			OutResult,
			TEXT("execution_manifest_invalid"),
			TEXT("The optional execution field must be a JSON object."),
			TEXT("rebuild the manifest with the current Editor publisher"));
		return false;
	}

	const FJsonObject& ExecutionObject = *ExecutionObjectPtr->Get();
	FString Format;
	FString ExecutionFile;
	FString ExecutionSha256;
	FString CanonicalSha256;
	FString CompilerBuildIdentity;
	FString TargetTriple;
	FString AttestationId;
	FString Policy;
	FString Fallback;
	if (!RequireExecutionString(ExecutionObject, TEXT("format"), Format)
		|| !RequireExecutionString(ExecutionObject, TEXT("file"), ExecutionFile)
		|| !RequireExecutionString(ExecutionObject, TEXT("sha256"), ExecutionSha256)
		|| !RequireExecutionString(
			ExecutionObject,
			TEXT("canonical_sha256"),
			CanonicalSha256)
		|| !RequireExecutionString(
			ExecutionObject,
			TEXT("compiler_build_identity"),
			CompilerBuildIdentity)
		|| !RequireExecutionString(
			ExecutionObject,
			TEXT("target_triple"),
			TargetTriple)
		|| !RequireExecutionString(
			ExecutionObject,
			TEXT("attestation_id"),
			AttestationId)
		|| !RequireExecutionString(ExecutionObject, TEXT("policy"), Policy)
		|| !RequireExecutionString(ExecutionObject, TEXT("fallback"), Fallback)
		|| Format != TEXT("wasmtime_serialized_v1")
		|| (Policy != TEXT("prefer_precompiled")
			&& Policy != TEXT("require_precompiled"))
		|| Fallback != TEXT("wasmtime_jit")
		|| !IsLowercaseSha256(ExecutionSha256)
		|| !IsLowercaseSha256(CanonicalSha256)
		|| !IsLowercaseAttestationId(AttestationId))
	{
		SetArtifactLoadFailure(
			OutResult,
			TEXT("execution_manifest_invalid"),
			TEXT("The execution object does not match the Wasmtime serialized v1 contract."),
			TEXT("rebuild the manifest with the current Editor publisher"));
		return false;
	}
	OutResult.ExecutionPolicy = Policy;
	if (!ResolveExecutionPath(
			CanonicalManifestPath,
			ExecutionFile,
			OutResult.ExecutionPath))
	{
		SetArtifactLoadFailure(
			OutResult,
			TEXT("execution_path_invalid"),
			TEXT("The serialized artifact path is absolute, escapes its allowed roots, or has the wrong extension."),
			TEXT("publish cwasm beside the canonical manifest"));
		return false;
	}
	if (!FPaths::FileExists(OutResult.ExecutionPath))
	{
		return ApplyJitFallback(
			TEXT("execution_file_missing"),
			Policy,
			Manifest,
			CanonicalWasmBytes,
			OutArtifact,
			OutResult);
	}

	TArray<uint8> SerializedBytes;
	if (!FFileHelper::LoadFileToArray(
			SerializedBytes,
			*OutResult.ExecutionPath)
		|| SerializedBytes.IsEmpty())
	{
		return ApplyJitFallback(
			TEXT("execution_file_read_failed"),
			Policy,
			Manifest,
			CanonicalWasmBytes,
			OutArtifact,
			OutResult);
	}
	if (ExecutionSha256 != FAvidScriptHash::Sha256Hex(SerializedBytes)
		|| CanonicalSha256 != Manifest.WasmSha256)
	{
		return ApplyJitFallback(
			TEXT("execution_identity_mismatch"),
			Policy,
			Manifest,
			CanonicalWasmBytes,
			OutArtifact,
			OutResult);
	}
#if PLATFORM_WINDOWS
	if (TargetTriple != TEXT("x86_64-pc-windows-msvc"))
	{
		return ApplyJitFallback(
			TEXT("execution_target_mismatch"),
			Policy,
			Manifest,
			CanonicalWasmBytes,
			OutArtifact,
			OutResult);
	}
#endif

	FAvidScriptVmOwnedArtifact VmArtifact;
	VmArtifact.ExecutionBytes = MoveTemp(SerializedBytes);
	VmArtifact.ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	VmArtifact.CanonicalWasmBytes = CanonicalWasmBytes;
	VmArtifact.ExecutionIdentity = ExecutionSha256;
	VmArtifact.CanonicalWasmIdentity = CanonicalSha256;
	VmArtifact.CompilerBuildIdentity = CompilerBuildIdentity;
	VmArtifact.TargetTriple = TargetTriple;
	VmArtifact.AttestationId = AttestationId;
	if (!AuthorizeAvidScriptVmArtifact(AttestationId, VmArtifact))
	{
		return ApplyJitFallback(
			TEXT("execution_attestation_invalid"),
			Policy,
			Manifest,
			CanonicalWasmBytes,
			OutArtifact,
			OutResult);
	}

	OutArtifact.Manifest = MoveTemp(Manifest);
	OutArtifact.VmArtifact = MoveTemp(VmArtifact);
	OutArtifact.BackendSelection = MakeWasmtimePrecompiledSelection();
	OutArtifact.RequestedBackend = TEXT("wasmtime.cranelift.precompiled");
	OutArtifact.SelectedBackend = TEXT("wasmtime.cranelift.precompiled");
	OutArtifact.ExecutionPolicy = Policy;
	OutArtifact.bUsesPrecompiledArtifact = true;
	OutResult.bSucceeded = true;
	OutResult.bUsesPrecompiledArtifact = true;
	OutResult.RequestedBackend = OutArtifact.RequestedBackend;
	OutResult.SelectedBackend = OutArtifact.SelectedBackend;
	return true;
}
