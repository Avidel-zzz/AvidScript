#include "CSharpBuild/AvidScriptEditorVmArtifactPublisher.h"

#include "AvidScriptHash.h"
#include "AvidScriptVmArtifact.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
FString GetVmArtifactPolicyName(EAvidScriptEditorVmArtifactPolicy Policy)
{
	switch (Policy)
	{
	case EAvidScriptEditorVmArtifactPolicy::JitOnly:
		return TEXT("jit_only");
	case EAvidScriptEditorVmArtifactPolicy::RequirePrecompiled:
		return TEXT("require_precompiled");
	default:
		return TEXT("prefer_precompiled");
	}
}

void ResetVmArtifactResult(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	OutResult.bVmArtifactPublished = false;
	OutResult.bVmArtifactCacheHit = false;
	OutResult.VmArtifactCompileMs = 0.0;
	OutResult.VmArtifactPath =
		FAvidScriptEditorVmArtifactPublisher::MakeArtifactPath(Config);
	OutResult.VmArtifactFormat.Reset();
	OutResult.VmArtifactSha256.Reset();
	OutResult.VmArtifactCanonicalSha256.Reset();
	OutResult.VmArtifactCompilerBuildIdentity.Reset();
	OutResult.VmArtifactTargetTriple.Reset();
	OutResult.VmArtifactAttestationId.Reset();
	OutResult.VmArtifactPolicy =
		GetVmArtifactPolicyName(Config.VmArtifactPolicy);
	OutResult.VmArtifactRequestedBackend =
		Config.VmArtifactPolicy == EAvidScriptEditorVmArtifactPolicy::JitOnly
			? TEXT("wasmtime.cranelift.jit")
			: TEXT("wasmtime.cranelift.precompiled");
	OutResult.VmArtifactSelectedBackend = TEXT("wasmtime.cranelift.jit");
	OutResult.VmArtifactFallbackCategory.Reset();
}

void SetVmArtifactFailure(
	FAvidScriptEditorCSharpBuildResult& OutResult,
	const FString& Category,
	const FString& Message,
	const FString& NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = Category;
	OutResult.ErrorMessage = Message;
	OutResult.NextAction = NextAction;
}

bool LoadManifestObject(
	const FString& ManifestPath,
	TSharedPtr<FJsonObject>& OutObject,
	FString& OutError)
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *ManifestPath))
	{
		OutError = FString::Printf(
			TEXT("C# manifest could not be read for VM artifact publication: %s"),
			*ManifestPath);
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, OutObject)
		|| !OutObject.IsValid())
	{
		OutError = FString::Printf(
			TEXT("C# manifest is not a valid JSON object: %s"),
			*ManifestPath);
		return false;
	}
	return true;
}

bool WriteManifestAtomic(
	const FString& ManifestPath,
	const TSharedRef<FJsonObject>& ManifestObject,
	FString& OutError)
{
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(ManifestObject, Writer))
	{
		OutError = TEXT("C# manifest JSON could not be serialized.");
		return false;
	}
	const FString TemporaryPath = FString::Printf(
		TEXT("%s.tmp.%s"),
		*ManifestPath,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!FFileHelper::SaveStringToFile(
			Json,
			*TemporaryPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(
			TEXT("Temporary C# manifest could not be written: %s"),
			*TemporaryPath);
		return false;
	}
	if (!IFileManager::Get().Move(
			*ManifestPath,
			*TemporaryPath,
			true,
			true,
			false,
			true))
	{
		IFileManager::Get().Delete(*TemporaryPath);
		OutError = FString::Printf(
			TEXT("C# manifest could not be replaced atomically: %s"),
			*ManifestPath);
		return false;
	}
	return true;
}

bool RemoveStaleArtifact(
	const FString& ArtifactPath,
	FString& OutError)
{
	if (!FPaths::FileExists(ArtifactPath))
	{
		return true;
	}
	if (!IFileManager::Get().Delete(
			*ArtifactPath,
			false,
			true,
			true))
	{
		OutError = FString::Printf(
			TEXT("Stale Wasmtime artifact could not be removed: %s"),
			*ArtifactPath);
		return false;
	}
	return true;
}

bool PublishSerializedBytesAtomic(
	const FString& ArtifactPath,
	const TArray<uint8>& Bytes,
	FString& OutError)
{
	const FString TemporaryPath = FString::Printf(
		TEXT("%s.tmp.%s"),
		*ArtifactPath,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!FFileHelper::SaveArrayToFile(Bytes, *TemporaryPath))
	{
		OutError = FString::Printf(
			TEXT("Temporary Wasmtime artifact could not be written: %s"),
			*TemporaryPath);
		return false;
	}
	if (!IFileManager::Get().Move(
			*ArtifactPath,
			*TemporaryPath,
			true,
			true,
			false,
			true))
	{
		IFileManager::Get().Delete(*TemporaryPath);
		OutError = FString::Printf(
			TEXT("Wasmtime artifact could not be replaced atomically: %s"),
			*ArtifactPath);
		return false;
	}
	return true;
}

bool RemoveExecutionAndPublishManifest(
	const FString& ArtifactPath,
	const FString& ManifestPath,
	const TSharedRef<FJsonObject>& ManifestObject,
	FString& OutError)
{
	if (!RemoveStaleArtifact(ArtifactPath, OutError))
	{
		return false;
	}
	ManifestObject->RemoveField(TEXT("execution"));
	return WriteManifestAtomic(ManifestPath, ManifestObject, OutError);
}
} // namespace

FString FAvidScriptEditorVmArtifactPublisher::MakeArtifactPath(
	const FAvidScriptEditorCSharpBuildConfig& Config)
{
	FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		Config.OutputRoot,
		Config.ArtifactStem + TEXT(".wasmtime.cwasm")));
	FPaths::NormalizeFilename(Path);
	return Path;
}

bool FAvidScriptEditorVmArtifactPublisher::Publish(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	ResetVmArtifactResult(Config, OutResult);
	TSharedPtr<FJsonObject> ManifestObject;
	FString Error;
	if (!LoadManifestObject(Config.ManifestPath, ManifestObject, Error))
	{
		SetVmArtifactFailure(
			OutResult,
			TEXT("vm_artifact_manifest_invalid"),
			Error,
			TEXT("rebuild the canonical C# manifest before publishing VM artifacts"));
		return false;
	}

	const TSharedPtr<FJsonObject>* WasmObject = nullptr;
	FString ManifestCanonicalSha256;
	FString ManifestWasmFile;
	if (!ManifestObject->TryGetObjectField(TEXT("wasm"), WasmObject)
		|| WasmObject == nullptr
		|| !WasmObject->IsValid()
		|| !(*WasmObject)->TryGetStringField(
			TEXT("file"),
			ManifestWasmFile)
		|| ManifestWasmFile.IsEmpty()
		|| !(*WasmObject)->TryGetStringField(
			TEXT("sha256"),
			ManifestCanonicalSha256)
		|| ManifestCanonicalSha256.IsEmpty())
	{
		SetVmArtifactFailure(
			OutResult,
			TEXT("vm_artifact_manifest_invalid"),
			TEXT("C# manifest requires wasm.file and wasm.sha256 before VM artifact publication."),
			TEXT("rebuild the canonical C# manifest with the current toolchain"));
		return false;
	}

	FString WasmPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		Config.OutputRoot,
		Config.ArtifactStem + TEXT(".wasm")));
	FPaths::NormalizeFilename(WasmPath);
	TArray<uint8> CanonicalWasmBytes;
	if (!FFileHelper::LoadFileToArray(CanonicalWasmBytes, *WasmPath)
		|| CanonicalWasmBytes.IsEmpty())
	{
		SetVmArtifactFailure(
			OutResult,
			TEXT("vm_artifact_wasm_read_failed"),
			FString::Printf(
				TEXT("Canonical WASM could not be read for VM artifact publication: %s"),
				*WasmPath),
			TEXT("rebuild the canonical WASM and retry"));
		return false;
	}
	const FString CanonicalSha256 =
		FAvidScriptHash::Sha256Hex(CanonicalWasmBytes);
	if (!CanonicalSha256.Equals(
			ManifestCanonicalSha256,
			ESearchCase::CaseSensitive))
	{
		SetVmArtifactFailure(
			OutResult,
			TEXT("vm_artifact_canonical_hash_mismatch"),
			FString::Printf(
				TEXT("Canonical WASM SHA-256 differs from the manifest: expected=%s observed=%s"),
				*ManifestCanonicalSha256,
				*CanonicalSha256),
			TEXT("rebuild the WASM and manifest in one C# build transaction"));
		return false;
	}
	OutResult.VmArtifactCanonicalSha256 = CanonicalSha256;

	if (Config.VmArtifactPolicy == EAvidScriptEditorVmArtifactPolicy::JitOnly)
	{
		if (!RemoveExecutionAndPublishManifest(
				OutResult.VmArtifactPath,
				Config.ManifestPath,
				ManifestObject.ToSharedRef(),
				Error))
		{
			SetVmArtifactFailure(
				OutResult,
				TEXT("vm_artifact_publish_failed"),
				Error,
				TEXT("close readers of the manifest and retry the JIT-only build"));
			return false;
		}
		OutResult.VmArtifactFallbackCategory = TEXT("jit_only");
		return true;
	}

	FAvidScriptVmArtifactCompileRequest CompileRequest;
	CompileRequest.Selection.BackendKind =
		EAvidScriptVmBackendKind::Wasmtime;
	CompileRequest.Selection.ExecutionMode =
		EAvidScriptVmExecutionMode::Aot;
	CompileRequest.Selection.ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	CompileRequest.TargetTriple = Config.VmArtifactTargetTriple;
	CompileRequest.CanonicalWasmBytes = CanonicalWasmBytes;
	FAvidScriptVmArtifactCompileResult CompileResult;
	if (!CompileAvidScriptVmArtifact(CompileRequest, CompileResult))
	{
		OutResult.VmArtifactCompileMs = CompileResult.CompileMs;
		OutResult.VmArtifactFallbackCategory = CompileResult.Error.Category;
		if (Config.VmArtifactPolicy ==
			EAvidScriptEditorVmArtifactPolicy::RequirePrecompiled)
		{
			SetVmArtifactFailure(
				OutResult,
				TEXT("vm_artifact_compile_failed"),
				FString::Printf(
					TEXT("Required Wasmtime artifact compilation failed: category=%s details=%s"),
					*CompileResult.Error.Category,
					*CompileResult.Error.Details),
				TEXT("repair the canonical WASM or choose PreferPrecompiled for an explicit JIT fallback"));
			return false;
		}
		if (!RemoveExecutionAndPublishManifest(
				OutResult.VmArtifactPath,
				Config.ManifestPath,
				ManifestObject.ToSharedRef(),
				Error))
		{
			SetVmArtifactFailure(
				OutResult,
				TEXT("vm_artifact_publish_failed"),
				Error,
				TEXT("close artifact readers and retry the C# build"));
			return false;
		}
		return true;
	}

	if (!PublishSerializedBytesAtomic(
			OutResult.VmArtifactPath,
			CompileResult.Artifact.ExecutionBytes,
			Error))
	{
		SetVmArtifactFailure(
			OutResult,
			TEXT("vm_artifact_publish_failed"),
			Error,
			TEXT("verify the C# output directory is writable and retry"));
		return false;
	}

	TSharedRef<FJsonObject> ExecutionObject = MakeShared<FJsonObject>();
	ExecutionObject->SetStringField(
		TEXT("format"),
		TEXT("wasmtime_serialized_v1"));
	ExecutionObject->SetStringField(
		TEXT("file"),
		FPaths::GetCleanFilename(OutResult.VmArtifactPath));
	ExecutionObject->SetStringField(
		TEXT("sha256"),
		CompileResult.Artifact.ExecutionIdentity);
	ExecutionObject->SetStringField(
		TEXT("canonical_sha256"),
		CompileResult.Artifact.CanonicalWasmIdentity);
	ExecutionObject->SetStringField(
		TEXT("compiler_build_identity"),
		CompileResult.Artifact.CompilerBuildIdentity);
	ExecutionObject->SetStringField(
		TEXT("target_triple"),
		CompileResult.Artifact.TargetTriple);
	ExecutionObject->SetStringField(
		TEXT("attestation_id"),
		CompileResult.Artifact.AttestationId);
	ExecutionObject->SetStringField(
		TEXT("policy"),
		GetVmArtifactPolicyName(Config.VmArtifactPolicy));
	ExecutionObject->SetStringField(
		TEXT("fallback"),
		TEXT("wasmtime_jit"));
	ManifestObject->SetObjectField(TEXT("execution"), ExecutionObject);
	if (!WriteManifestAtomic(
			Config.ManifestPath,
			ManifestObject.ToSharedRef(),
			Error))
	{
		SetVmArtifactFailure(
			OutResult,
			TEXT("vm_artifact_publish_failed"),
			Error,
			TEXT("close readers of the manifest and retry the C# build"));
		return false;
	}

	OutResult.bVmArtifactPublished = true;
	OutResult.bVmArtifactCacheHit = CompileResult.bCacheHit;
	OutResult.VmArtifactCompileMs = CompileResult.CompileMs;
	OutResult.VmArtifactFormat = TEXT("wasmtime_serialized_v1");
	OutResult.VmArtifactSha256 = CompileResult.Artifact.ExecutionIdentity;
	OutResult.VmArtifactCanonicalSha256 =
		CompileResult.Artifact.CanonicalWasmIdentity;
	OutResult.VmArtifactCompilerBuildIdentity =
		CompileResult.Artifact.CompilerBuildIdentity;
	OutResult.VmArtifactTargetTriple = CompileResult.Artifact.TargetTriple;
	OutResult.VmArtifactAttestationId = CompileResult.Artifact.AttestationId;
	OutResult.VmArtifactSelectedBackend =
		TEXT("wasmtime.cranelift.precompiled");
	return true;
}
