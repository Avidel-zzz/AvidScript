#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace AvidScript::ModulePackage
{
inline constexpr int32 LegacyCatalogSchemaVersion = 1;
inline constexpr int32 CatalogSchemaVersion = 2;
inline constexpr int32 PackageSchemaVersion = 1;
inline constexpr TCHAR Win64Platform[] = TEXT("win64");
inline constexpr TCHAR Win64Architecture[] = TEXT("x86_64");
inline constexpr TCHAR Win64TargetTriple[] =
	TEXT("x86_64-pc-windows-msvc");
inline constexpr TCHAR Win64CpuFeatures[] = TEXT("x86-64-v3");
inline constexpr TCHAR AndroidPlatform[] = TEXT("android");
inline constexpr TCHAR AndroidArchitecture[] = TEXT("arm64");
inline constexpr TCHAR AndroidTargetTriple[] =
	TEXT("aarch64-linux-android");
inline constexpr TCHAR AndroidCpuFeatures[] = TEXT("arm64-v8a");
inline constexpr TCHAR ModuleIdPatternDescription[] =
	TEXT("a lowercase id matching [a-z][a-z0-9_.-]{0,63}");

struct FCatalogEntry
{
	FString ModuleId;
	FString PackageId;
	FString DescriptorFile;
	FString DescriptorSha256;
	FString Platform;
	FString Architecture;
	FString Configuration;
	FString Backend;
	FString Format;
};

struct FArtifactEntry
{
	FString File;
	FString Sha256;
};

struct FDocument
{
	FString PackageId;
	FString ModuleId;
	int32 AbiVersion = 0;
	FString Platform;
	FString Configuration;
	FString MinimumRuntimeVersion;
	FString Backend;
	FString Format;
	FString Policy;
	FString CompilerBuildIdentity;
	FString TargetTriple;
	FString CpuFeatures;
	FArtifactEntry RuntimeManifest;
	FArtifactEntry CanonicalWasm;
	FArtifactEntry Precompiled;
	FArtifactEntry BindingManifest;
	FArtifactEntry BindingDescriptor;
	TOptional<FArtifactEntry> DebugMap;
};

bool IsLowercaseSha256(const FString& Value);
bool IsNormalizedModuleId(const FString& Value);
bool TryParseSimpleSemanticVersion(
	const FString& Value,
	TStaticArray<int32, 3>& OutParts);
bool ParseCatalogEntry(
	const TSharedPtr<FJsonObject>& Object,
	FCatalogEntry& OutEntry);
bool ParseCatalogVariant(
	const FString& ModuleId,
	const TSharedPtr<FJsonObject>& Object,
	FCatalogEntry& OutEntry);
bool IsValidVariantIdentity(
	const FString& Platform,
	const FString& Architecture,
	const FString& Configuration,
	const FString& Backend,
	const FString& Format);
FString MakeVariantKey(const FCatalogEntry& Entry);
bool ParseDocument(
	const TSharedPtr<FJsonObject>& Root,
	FDocument& OutPackage);
FString MakeIdentityPayload(const FDocument& Package);
bool ValidateRuntimeManifest(
	const FString& RuntimeManifestPath,
	const FDocument& Package);
}
