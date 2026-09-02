#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptModulePackageTrustDomain : uint8
{
	DevelopmentCatalog,
	CookedPackage
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptModulePlatformContext
{
	FString Platform;
	FString Architecture;
	FString Configuration;
	FString Backend;
	FString Format;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptResolvedModulePackage
{
	FName ModuleId;
	FString PackageId;
	FString Platform;
	FString Architecture;
	FString Configuration;
	FString ExecutionBackend;
	FString ExecutionFormat;
	int32 AbiVersion = 0;
	FString MinimumRuntimeVersion;
	FString DescriptorPath;
	FString RuntimeManifestPath;
	FString CanonicalWasmPath;
	FString PrecompiledArtifactPath;
	FString BindingManifestPath;
	FString BindingDescriptorPath;
	FString DebugMapPath;
	FString CompilerBuildIdentity;
	FString TargetTriple;
	FString CpuFeatures;
	FString ExecutionPolicy;
	EAvidScriptModulePackageTrustDomain TrustDomain =
		EAvidScriptModulePackageTrustDomain::DevelopmentCatalog;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptModuleResolveResult
{
	bool bSucceeded = false;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString CatalogPath;
	FString ModuleId;
	FString PackageId;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptModulePackageResolver
{
public:
	static FString GetDefaultCatalogPath();
	static FAvidScriptModulePlatformContext GetCurrentPlatformContext();

	static bool ResolveModule(
		FName ModuleId,
		FAvidScriptResolvedModulePackage& OutPackage,
		FAvidScriptModuleResolveResult& OutResult);

	static bool ResolveModule(
		FName ModuleId,
		const FAvidScriptModulePlatformContext& PlatformContext,
		FAvidScriptResolvedModulePackage& OutPackage,
		FAvidScriptModuleResolveResult& OutResult);

	static bool ResolveModuleFromCatalogFile(
		const FString& CatalogPath,
		FName ModuleId,
		FAvidScriptResolvedModulePackage& OutPackage,
		FAvidScriptModuleResolveResult& OutResult);

	static bool ResolveModuleFromCatalogFile(
		const FString& CatalogPath,
		FName ModuleId,
		const FAvidScriptModulePlatformContext& PlatformContext,
		FAvidScriptResolvedModulePackage& OutPackage,
		FAvidScriptModuleResolveResult& OutResult);
};
