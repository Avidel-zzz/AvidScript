#include "AvidScriptWasmtimeRuntimeSupport.h"

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
			TEXT("Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0/lib/wasmtime.dll"))
	};
	const FString ExpectedDllSha256 =
		FString(UTF8_TO_TCHAR(AVIDSCRIPT_WASMTIME_DLL_SHA256)).ToLower();
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
			OutError = FString::Printf(
				TEXT("The Wasmtime DLL SHA-256 does not match the linked managed artifact: ")
				TEXT("path=%s expected=%s observed=%s"),
				*Candidate,
				*ExpectedDllSha256,
				*ObservedDllSha256);
			return false;
		}
		GWasmtimeDllHandle = FPlatformProcess::GetDllHandle(*Candidate);
		if (GWasmtimeDllHandle != nullptr)
		{
			GWasmtimeObservedDllSha256 = ObservedDllSha256;
			OutObservedDllSha256 = ObservedDllSha256;
			return true;
		}
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
	OutError.Reset();
	InitializeAvidScriptWasmtimeRuntimeDescriptor(InOutInfo);
#if !AVIDSCRIPT_WITH_WASMTIME
	OutError = TEXT("Wasmtime is unavailable for this target.");
	return false;
#elif !PLATFORM_WINDOWS
	OutError = TEXT("Wasmtime artifact production currently supports Win64 only.");
	return false;
#else
	FString ObservedDllSha256;
	if (!EnsureWasmtimeDllLoaded(ObservedDllSha256, OutError))
	{
		return false;
	}
	InOutInfo.RuntimeArtifactSha256 = ObservedDllSha256;
	InOutInfo.RuntimeBuildIdentity = FString::Printf(
		TEXT("wasmtime-v%s;cranelift=1;opt=speed_and_size;wasm32_memory_stable=1;dll_sha256=%s"),
		*InOutInfo.RuntimeVersion,
		*ObservedDllSha256);
	return true;
#endif
}
