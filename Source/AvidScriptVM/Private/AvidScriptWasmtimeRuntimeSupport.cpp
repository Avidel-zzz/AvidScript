#include "AvidScriptWasmtimeRuntimeSupport.h"

#include "AvidScriptWasmtimeCompilerProfile.h"

#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

#ifndef AVIDSCRIPT_WITH_WASMTIME
#define AVIDSCRIPT_WITH_WASMTIME 0
#endif
#ifndef AVIDSCRIPT_WASMTIME_DLL_SHA256
#define AVIDSCRIPT_WASMTIME_DLL_SHA256 "unavailable"
#endif

namespace
{
FCriticalSection GWasmtimeDllCriticalSection;
void* GWasmtimeDllHandle = nullptr;
FString GWasmtimeObservedDllSha256;
void* GWasmtimeCompilerInliningExport = nullptr;

#if AVIDSCRIPT_WITH_WASMTIME && PLATFORM_WINDOWS
bool GetWasmtimeDllSha256(
	const FString& Path,
	FString& OutSha256,
	FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		OutError = FString::Printf(
			TEXT("The Wasmtime DLL could not be read for identity verification: %s"),
			*Path);
		return false;
	}
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	if (SHA256(
			Bytes.GetData(),
			static_cast<size_t>(Bytes.Num()),
			Digest) == nullptr)
	{
		OutError = FString::Printf(
			TEXT("The Wasmtime DLL SHA-256 could not be computed: %s"),
			*Path);
		return false;
	}
	OutSha256.Reset(SHA256_DIGEST_LENGTH * 2);
	for (const uint8 Byte : Digest)
	{
		OutSha256 += FString::Printf(TEXT("%02x"), Byte);
	}
	return true;
}

bool EnsureWasmtimeDllLoaded(
	FString& OutObservedDllSha256,
	FString& OutError)
{
	FScopeLock Lock(&GWasmtimeDllCriticalSection);
	if (GWasmtimeDllHandle != nullptr)
	{
		OutObservedDllSha256 = GWasmtimeObservedDllSha256;
		return true;
	}
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (!Plugin.IsValid())
	{
		OutError = TEXT("AvidScript plugin base directory is unavailable.");
		return false;
	}
	const FString Candidates[] = {
		FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Binaries/Win64/wasmtime.dll")),
		FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0-avidscript.1/lib/wasmtime.dll")),
		FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0/lib/wasmtime.dll"))
	};
	const FString ExpectedDllSha256 =
		FString(UTF8_TO_TCHAR(AVIDSCRIPT_WASMTIME_DLL_SHA256)).ToLower();
	TArray<FString, TInlineAllocator<3>> RejectedCandidates;
	for (const FString& Candidate : Candidates)
	{
		if (!FPaths::FileExists(Candidate))
		{
			continue;
		}
		FString ObservedDllSha256;
		if (!GetWasmtimeDllSha256(
				Candidate,
				ObservedDllSha256,
				OutError))
		{
			return false;
		}
		if (!ObservedDllSha256.Equals(
				ExpectedDllSha256,
				ESearchCase::CaseSensitive))
		{
			RejectedCandidates.Add(FString::Printf(
				TEXT("path=%s observed=%s"),
				*Candidate,
				*ObservedDllSha256));
			continue;
		}
		GWasmtimeDllHandle = FPlatformProcess::GetDllHandle(*Candidate);
		if (GWasmtimeDllHandle != nullptr)
		{
			GWasmtimeCompilerInliningExport = FPlatformProcess::GetDllExport(
				GWasmtimeDllHandle,
				TEXT("avidscript_wasmtime_config_compiler_inlining_set"));
			if (GWasmtimeCompilerInliningExport == nullptr)
			{
				FPlatformProcess::FreeDllHandle(GWasmtimeDllHandle);
				GWasmtimeDllHandle = nullptr;
				OutError = FString::Printf(
					TEXT("The verified Wasmtime DLL lacks the AvidScript compiler inlining extension: %s"),
					*Candidate);
				return false;
			}
			GWasmtimeObservedDllSha256 = ObservedDllSha256;
			OutObservedDllSha256 = ObservedDllSha256;
			return true;
		}
	}
	if (!RejectedCandidates.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("No Wasmtime DLL candidate matches the linked managed artifact: ")
			TEXT("expected=%s rejected=[%s]"),
			*ExpectedDllSha256,
			*FString::Join(RejectedCandidates, TEXT("; ")));
		return false;
	}
	OutError = TEXT(
		"The locked Wasmtime v45 DLL could not be loaded from the plugin deployment or managed dependency layout.");
	return false;
}
#endif
} // namespace

void InitializeAvidScriptWasmtimeRuntimeDescriptor(
	FAvidScriptVmBackendInfo& InOutInfo)
{
	InOutInfo.RuntimeVersion = TEXT("45.0.0");
#if PLATFORM_WINDOWS
	InOutInfo.TargetTriple = TEXT("x86_64-pc-windows-msvc");
#else
	InOutInfo.TargetTriple = TEXT("unknown-unknown-unknown");
#endif
}

bool ResolveAvidScriptWasmtimeRuntimeIdentity(
	FAvidScriptVmBackendInfo& InOutInfo,
	FString& OutError)
{
	AvidScriptWasmtimeEngineProfile IgnoredProfile = {};
	return ResolveAvidScriptWasmtimeCompilerProfile(
		InOutInfo,
		IgnoredProfile,
		OutError);
}

bool ResolveAvidScriptWasmtimeCompilerProfile(
	FAvidScriptVmBackendInfo& InOutInfo,
	AvidScriptWasmtimeEngineProfile& OutProfile,
	FString& OutError,
	FString* OutErrorCategory)
{
	OutError.Reset();
	OutProfile = {};
	if (OutErrorCategory != nullptr)
	{
		OutErrorCategory->Reset();
	}
	InitializeAvidScriptWasmtimeRuntimeDescriptor(InOutInfo);
#if !AVIDSCRIPT_WITH_WASMTIME
	if (OutErrorCategory != nullptr)
	{
		*OutErrorCategory = TEXT("compiler_toolchain_unavailable");
	}
	OutError = TEXT("The AvidScript Wasmtime compiler toolchain is unavailable for this target.");
	return false;
#elif !PLATFORM_WINDOWS
	if (OutErrorCategory != nullptr)
	{
		*OutErrorCategory = TEXT("platform_unsupported");
	}
	OutError = TEXT("Wasmtime artifact production currently supports Win64 only.");
	return false;
#else
	FString ObservedDllSha256;
	if (!EnsureWasmtimeDllLoaded(ObservedDllSha256, OutError))
	{
		if (OutErrorCategory != nullptr)
		{
			*OutErrorCategory = OutError.Contains(TEXT("compiler inlining extension"))
				? TEXT("compiler_extension_missing")
				: TEXT("compiler_toolchain_unavailable");
		}
		return false;
	}
	if (GWasmtimeCompilerInliningExport == nullptr)
	{
		if (OutErrorCategory != nullptr)
		{
			*OutErrorCategory = TEXT("compiler_extension_missing");
		}
		OutError = TEXT("The AvidScript Wasmtime compiler extension is unavailable.");
		return false;
	}
	if (!ValidateAvidScriptWasmtimeCompilerCpuProfile(OutError))
	{
		if (OutErrorCategory != nullptr)
		{
			*OutErrorCategory = TEXT("cpu_profile_unsupported");
		}
		return false;
	}
	const FAvidScriptWasmtimeCompilerProfile& CompilerProfile =
		GetAvidScriptWasmtimeCompilerProfile();
	OutProfile = CompilerProfile.EngineProfile;
	OutProfile.CompilerInliningSetter =
		reinterpret_cast<AvidScriptWasmtimeCompilerInliningSetter>(
			GWasmtimeCompilerInliningExport);
	InOutInfo.TargetTriple = CompilerProfile.TargetTriple;
	InOutInfo.RuntimeArtifactSha256 = ObservedDllSha256;
	InOutInfo.RuntimeBuildIdentity = BuildAvidScriptWasmtimeCompilerIdentity(
		InOutInfo.RuntimeVersion,
		ObservedDllSha256);
	return true;
#endif
}
