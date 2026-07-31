#include "AvidScriptVmArtifact.h"

#include "AvidScriptHash.h"
#include "AvidScriptWasmtimeApi.h"
#include "AvidScriptWasmtimeRuntimeSupport.h"

#include "Containers/StringConv.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

#ifndef AVIDSCRIPT_WITH_WASMTIME
#define AVIDSCRIPT_WITH_WASMTIME 0
#endif

namespace
{
constexpr int32 ArtifactCacheCapacity = 32;
constexpr int32 AttestationRegistryCapacity = 32;

struct FAvidScriptVmArtifactCacheEntry
{
	FString Key;
	FAvidScriptVmOwnedArtifact Artifact;
	uint64 LastAccess = 0;
};

struct FAvidScriptVmArtifactAttestation
{
	FString Id;
	FString ExecutionIdentity;
	FString CanonicalWasmIdentity;
	FString CompilerBuildIdentity;
	FString TargetTriple;
	EAvidScriptVmArtifactFormat ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmBytecode;
	uint64 LastAccess = 0;
};

FCriticalSection GArtifactCompilerCriticalSection;
TArray<FAvidScriptVmArtifactCacheEntry> GArtifactCache;
TArray<FAvidScriptVmArtifactAttestation> GAttestationRegistry;
uint64 GArtifactAccessSequence = 0;

uint64 NextArtifactAccessSequence()
{
	++GArtifactAccessSequence;
	if (GArtifactAccessSequence == 0)
	{
		GArtifactAccessSequence = 1;
	}
	return GArtifactAccessSequence;
}

template <typename EntryType>
void RemoveLeastRecentlyUsed(TArray<EntryType>& Entries, int32 Capacity)
{
	if (Entries.Num() < Capacity)
	{
		return;
	}
	int32 OldestIndex = 0;
	for (int32 Index = 1; Index < Entries.Num(); ++Index)
	{
		if (Entries[Index].LastAccess < Entries[OldestIndex].LastAccess)
		{
			OldestIndex = Index;
		}
	}
	Entries.RemoveAtSwap(OldestIndex, 1, EAllowShrinking::No);
}

FString RegisterArtifactAttestationLocked(
	const FAvidScriptVmOwnedArtifact& Artifact)
{
	RemoveLeastRecentlyUsed(
		GAttestationRegistry,
		AttestationRegistryCapacity);
	FAvidScriptVmArtifactAttestation& Attestation =
		GAttestationRegistry.AddDefaulted_GetRef();
	Attestation.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Attestation.ExecutionIdentity = Artifact.ExecutionIdentity;
	Attestation.CanonicalWasmIdentity = Artifact.CanonicalWasmIdentity;
	Attestation.CompilerBuildIdentity = Artifact.CompilerBuildIdentity;
	Attestation.TargetTriple = Artifact.TargetTriple;
	Attestation.ArtifactFormat = Artifact.ArtifactFormat;
	Attestation.LastAccess = NextArtifactAccessSequence();
	return Attestation.Id;
}

void SetCompileError(
	FAvidScriptVmArtifactCompileResult& OutResult,
	const TCHAR* Category,
	const FString& Details,
	double StartSeconds)
{
	OutResult.Error.Reset();
	OutResult.Error.Category = Category;
	OutResult.Error.Details = Details;
	OutResult.CompileMs = FMath::Max(
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0,
		0.0001);
}

#if AVIDSCRIPT_WITH_WASMTIME && PLATFORM_WINDOWS
FString ConsumeArtifactCompilerFailure(AvidScriptWasmtimeFailure* Failure)
{
	if (Failure == nullptr)
	{
		return FString();
	}
	size_t MessageSize = 0;
	const char* Message = avidscript_wasmtime_failure_message(
		Failure,
		&MessageSize);
	FString Details;
	if (Message != nullptr && MessageSize > 0)
	{
		const FUTF8ToTCHAR Converted(
			Message,
			static_cast<int32>(FMath::Min<size_t>(MessageSize, MAX_int32)));
		Details = FString(Converted.Length(), Converted.Get());
	}
	avidscript_wasmtime_failure_delete(Failure);
	return Details;
}
#endif

FString MakeArtifactCacheKey(
	const FAvidScriptVmArtifactCompileRequest& Request,
	const FString& CanonicalIdentity,
	const FAvidScriptVmBackendInfo& RuntimeInfo)
{
	return FString::Printf(
		TEXT("%s|%d|%d|%d|%s|%s"),
		*CanonicalIdentity,
		static_cast<int32>(Request.Selection.BackendKind),
		static_cast<int32>(Request.Selection.ExecutionMode),
		static_cast<int32>(Request.Selection.ArtifactFormat),
		*RuntimeInfo.RuntimeBuildIdentity,
		*RuntimeInfo.TargetTriple);
}
} // namespace

bool CompileAvidScriptVmArtifact(
	const FAvidScriptVmArtifactCompileRequest& Request,
	FAvidScriptVmArtifactCompileResult& OutResult)
{
	OutResult = FAvidScriptVmArtifactCompileResult();
	const double StartSeconds = FPlatformTime::Seconds();
	if (Request.Selection.BackendKind != EAvidScriptVmBackendKind::Wasmtime
		|| Request.Selection.ExecutionMode != EAvidScriptVmExecutionMode::Aot
		|| Request.Selection.ArtifactFormat !=
			EAvidScriptVmArtifactFormat::WasmtimeSerialized)
	{
		SetCompileError(
			OutResult,
			TEXT("artifact_selection_unsupported"),
			TEXT("Artifact production requires Wasmtime AOT with WasmtimeSerialized output."),
			StartSeconds);
		return false;
	}
	if (Request.CanonicalWasmBytes.IsEmpty())
	{
		SetCompileError(
			OutResult,
			TEXT("invalid_artifact"),
			TEXT("Canonical WASM bytes must be present."),
			StartSeconds);
		return false;
	}
#if !AVIDSCRIPT_WITH_WASMTIME
	SetCompileError(
		OutResult,
		TEXT("backend_unavailable"),
		TEXT("Wasmtime is unavailable for this target."),
		StartSeconds);
	return false;
#elif !PLATFORM_WINDOWS
	SetCompileError(
		OutResult,
		TEXT("platform_unsupported"),
		TEXT("Wasmtime artifact production currently supports Win64 only."),
		StartSeconds);
	return false;
#else
	FAvidScriptVmBackendInfo RuntimeInfo;
	RuntimeInfo.Kind = EAvidScriptVmBackendKind::Wasmtime;
	RuntimeInfo.ExecutionMode = EAvidScriptVmExecutionMode::Aot;
	RuntimeInfo.ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	FString RuntimeIdentityError;
	if (!ResolveAvidScriptWasmtimeRuntimeIdentity(
			RuntimeInfo,
			RuntimeIdentityError))
	{
		SetCompileError(
			OutResult,
			TEXT("runtime_init_failed"),
			RuntimeIdentityError,
			StartSeconds);
		return false;
	}

	const FString CanonicalIdentity =
		FAvidScriptHash::Sha256Hex(Request.CanonicalWasmBytes);
	const FString CacheKey = MakeArtifactCacheKey(
		Request,
		CanonicalIdentity,
		RuntimeInfo);
	{
		FScopeLock Lock(&GArtifactCompilerCriticalSection);
		for (FAvidScriptVmArtifactCacheEntry& Entry : GArtifactCache)
		{
			if (Entry.Key == CacheKey)
			{
				Entry.LastAccess = NextArtifactAccessSequence();
				OutResult.Artifact = Entry.Artifact;
				OutResult.Artifact.AttestationId =
					RegisterArtifactAttestationLocked(OutResult.Artifact);
				OutResult.bSucceeded = true;
				OutResult.bCacheHit = true;
				OutResult.CompileMs = FMath::Max(
					(FPlatformTime::Seconds() - StartSeconds) * 1000.0,
					0.0001);
				return true;
			}
		}
	}

	AvidScriptWasmtimeEngine* Engine = avidscript_wasmtime_engine_new();
	if (Engine == nullptr)
	{
		SetCompileError(
			OutResult,
			TEXT("runtime_init_failed"),
			TEXT("Wasmtime could not create a Cranelift engine."),
			StartSeconds);
		return false;
	}

	AvidScriptWasmtimeModule* Module = nullptr;
	AvidScriptWasmtimeFailure* Failure = avidscript_wasmtime_module_new(
		Engine,
		Request.CanonicalWasmBytes.GetData(),
		static_cast<size_t>(Request.CanonicalWasmBytes.Num()),
		&Module);
	if (Failure != nullptr || Module == nullptr)
	{
		const FString Details = Failure != nullptr
			? ConsumeArtifactCompilerFailure(Failure)
			: TEXT("Wasmtime module allocation failed.");
		avidscript_wasmtime_module_delete(Module);
		avidscript_wasmtime_engine_delete(Engine);
		SetCompileError(
			OutResult,
			TEXT("artifact_compile_failed"),
			Details,
			StartSeconds);
		return false;
	}

	uint8_t* SerializedBytes = nullptr;
	size_t SerializedSize = 0;
	Failure = avidscript_wasmtime_module_serialize(
		Module,
		&SerializedBytes,
		&SerializedSize);
	if (Failure != nullptr
		|| SerializedBytes == nullptr
		|| SerializedSize == 0
		|| SerializedSize > static_cast<size_t>(MAX_int32))
	{
		const FString Details = Failure != nullptr
			? ConsumeArtifactCompilerFailure(Failure)
			: TEXT("Wasmtime produced an invalid serialized module.");
		avidscript_wasmtime_serialized_bytes_delete(SerializedBytes);
		avidscript_wasmtime_module_delete(Module);
		avidscript_wasmtime_engine_delete(Engine);
		SetCompileError(
			OutResult,
			TEXT("artifact_serialize_failed"),
			Details,
			StartSeconds);
		return false;
	}

	FAvidScriptVmOwnedArtifact CompiledArtifact;
	CompiledArtifact.ExecutionBytes.Append(
		SerializedBytes,
		static_cast<int32>(SerializedSize));
	CompiledArtifact.ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	CompiledArtifact.CanonicalWasmBytes.Append(
		Request.CanonicalWasmBytes.GetData(),
		Request.CanonicalWasmBytes.Num());
	CompiledArtifact.ExecutionIdentity =
		FAvidScriptHash::Sha256Hex(CompiledArtifact.ExecutionBytes);
	CompiledArtifact.CanonicalWasmIdentity = CanonicalIdentity;
	CompiledArtifact.CompilerBuildIdentity = RuntimeInfo.RuntimeBuildIdentity;
	CompiledArtifact.TargetTriple = RuntimeInfo.TargetTriple;

	avidscript_wasmtime_serialized_bytes_delete(SerializedBytes);
	avidscript_wasmtime_module_delete(Module);
	avidscript_wasmtime_engine_delete(Engine);

	{
		FScopeLock Lock(&GArtifactCompilerCriticalSection);
		for (FAvidScriptVmArtifactCacheEntry& Entry : GArtifactCache)
		{
			if (Entry.Key == CacheKey)
			{
				Entry.LastAccess = NextArtifactAccessSequence();
				CompiledArtifact = Entry.Artifact;
				OutResult.bCacheHit = true;
				break;
			}
		}
		if (!OutResult.bCacheHit)
		{
			RemoveLeastRecentlyUsed(GArtifactCache, ArtifactCacheCapacity);
			FAvidScriptVmArtifactCacheEntry& Entry =
				GArtifactCache.AddDefaulted_GetRef();
			Entry.Key = CacheKey;
			Entry.Artifact = CompiledArtifact;
			Entry.LastAccess = NextArtifactAccessSequence();
		}
		CompiledArtifact.AttestationId =
			RegisterArtifactAttestationLocked(CompiledArtifact);
	}

	OutResult.Artifact = MoveTemp(CompiledArtifact);
	OutResult.bSucceeded = true;
	OutResult.CompileMs = FMath::Max(
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0,
		0.0001);
	return true;
#endif
}

bool AuthorizeAvidScriptVmArtifact(
	const FString& AttestationId,
	const FAvidScriptVmOwnedArtifact& Artifact)
{
	if (AttestationId.IsEmpty()
		|| AttestationId != Artifact.AttestationId
		|| Artifact.ExecutionBytes.IsEmpty()
		|| Artifact.CanonicalWasmBytes.IsEmpty()
		|| Artifact.ExecutionIdentity !=
			FAvidScriptHash::Sha256Hex(Artifact.ExecutionBytes)
		|| Artifact.CanonicalWasmIdentity !=
			FAvidScriptHash::Sha256Hex(Artifact.CanonicalWasmBytes))
	{
		return false;
	}

	FScopeLock Lock(&GArtifactCompilerCriticalSection);
	for (FAvidScriptVmArtifactAttestation& Attestation : GAttestationRegistry)
	{
		if (Attestation.Id == AttestationId)
		{
			const bool bAuthorized =
				Attestation.ExecutionIdentity == Artifact.ExecutionIdentity
				&& Attestation.CanonicalWasmIdentity ==
					Artifact.CanonicalWasmIdentity
				&& Attestation.CompilerBuildIdentity ==
					Artifact.CompilerBuildIdentity
				&& Attestation.TargetTriple == Artifact.TargetTriple
				&& Attestation.ArtifactFormat == Artifact.ArtifactFormat;
			if (bAuthorized)
			{
				Attestation.LastAccess = NextArtifactAccessSequence();
			}
			return bAuthorized;
		}
	}
	return false;
}
