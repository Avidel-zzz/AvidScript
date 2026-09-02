#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace AvidScript::ModulePackage
{
inline constexpr int32 CatalogSchemaVersion = 1;
inline constexpr int32 PackageSchemaVersion = 1;
inline constexpr TCHAR ModuleIdPatternDescription[] =
	TEXT("a lowercase id matching [a-z][a-z0-9_.-]{0,63}");

struct FCatalogEntry
{
	FString ModuleId;
	FString PackageId;
	FString DescriptorFile;
	FString DescriptorSha256;
	FString Platform;
	FString Configuration;
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
bool ParseDocument(
	const TSharedPtr<FJsonObject>& Root,
	FDocument& OutPackage);
FString MakeIdentityPayload(const FDocument& Package);
bool ValidateRuntimeManifest(
	const FString& RuntimeManifestPath,
	const FDocument& Package);
}
