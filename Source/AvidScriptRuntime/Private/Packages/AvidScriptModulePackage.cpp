#include "Packages/AvidScriptModulePackage.h"

#include "AvidScriptHash.h"
#include "Packages/AvidScriptModulePackageSchema.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
using AvidScript::ModulePackage::FArtifactEntry;
using AvidScript::ModulePackage::FCatalogEntry;
using AvidScript::ModulePackage::FDocument;

void SetResolveFailure(
	FAvidScriptModuleResolveResult& OutResult,
	const TCHAR* Category,
	const FString& Details,
	const TCHAR* NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = Category;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript module resolve error | catalog=%s | module=%s | package=%s | category=%s | details=%s | next=%s"),
		OutResult.CatalogPath.IsEmpty() ? TEXT("<none>") : *OutResult.CatalogPath,
		OutResult.ModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.ModuleId,
		OutResult.PackageId.IsEmpty() ? TEXT("<none>") : *OutResult.PackageId,
		Category,
		*Details,
		NextAction);
}

FString NormalizeModulePackagePath(const FString& Path)
{
	FString Normalized = FPaths::ConvertRelativePathToFull(Path);
	FPaths::CollapseRelativeDirectories(Normalized, true);
	FPaths::NormalizeFilename(Normalized);
	return Normalized;
}

bool IsPathUnderRoot(const FString& Path, const FString& Root)
{
	return Path == Root || FPaths::IsUnderDirectory(Path, Root);
}

bool LoadJsonObject(
	const FString& Path,
	TSharedPtr<FJsonObject>& OutObject)
{
	FString Json;
	return FFileHelper::LoadFileToString(Json, *Path)
		&& FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Json),
			OutObject)
		&& OutObject.IsValid();
}

bool MatchesPlatformContext(
	const FCatalogEntry& Entry,
	const FAvidScriptModulePlatformContext& Context)
{
	return Entry.Platform == Context.Platform
		&& Entry.Architecture == Context.Architecture
		&& Entry.Configuration == Context.Configuration
		&& Entry.Backend == Context.Backend
		&& Entry.Format == Context.Format;
}

FString DescribePlatformContext(
	const FAvidScriptModulePlatformContext& Context)
{
	return FString::Printf(
		TEXT("platform=%s architecture=%s configuration=%s backend=%s format=%s"),
		*Context.Platform,
		*Context.Architecture,
		*Context.Configuration,
		*Context.Backend,
		*Context.Format);
}

bool LoadVerifiedBytes(
	const FString& Path,
	const FString& ExpectedSha256,
	TArray<uint8>& OutBytes)
{
	return FFileHelper::LoadFileToArray(OutBytes, *Path)
		&& !OutBytes.IsEmpty()
		&& FAvidScriptHash::Sha256Hex(OutBytes) == ExpectedSha256;
}

FString ResolveFixedArtifactPath(
	const FString& PackageRoot,
	const FString& RelativePath)
{
	return NormalizeModulePackagePath(FPaths::Combine(PackageRoot, RelativePath));
}

bool ValidateExactFileSet(
	const FString& PackageRoot,
	const FDocument& Package)
{
	TSet<FString> Expected{
		TEXT("package.json"),
		Package.RuntimeManifest.File,
		Package.CanonicalWasm.File,
		Package.Precompiled.File,
		Package.BindingManifest.File,
		Package.BindingDescriptor.File
	};
	if (Package.DebugMap.IsSet())
	{
		Expected.Add(Package.DebugMap->File);
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *PackageRoot, TEXT("*"), true, false);
	if (Files.Num() != Expected.Num())
	{
		return false;
	}
	const FString PackageRootWithSlash = PackageRoot + TEXT("/");
	for (const FString& File : Files)
	{
		FString Relative = File;
		FPaths::MakePathRelativeTo(Relative, *PackageRootWithSlash);
		FPaths::NormalizeFilename(Relative);
		if (!Expected.Contains(Relative))
		{
			return false;
		}
	}
	return true;
}

bool IsRuntimeVersionCompatible(const FString& MinimumVersion)
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (!Plugin.IsValid())
	{
		return false;
	}
	TStaticArray<int32, 3> Current;
	TStaticArray<int32, 3> Minimum;
	if (!AvidScript::ModulePackage::TryParseSimpleSemanticVersion(
			Plugin->GetDescriptor().VersionName,
			Current)
		|| !AvidScript::ModulePackage::TryParseSimpleSemanticVersion(
			MinimumVersion,
			Minimum))
	{
		return false;
	}
	for (int32 Index = 0; Index < Current.Num(); ++Index)
	{
		if (Current[Index] != Minimum[Index])
		{
			return Current[Index] > Minimum[Index];
		}
	}
	return true;
}

bool LoadAndValidateArtifacts(
	const FString& PackageRoot,
	const FDocument& Package,
	FAvidScriptModuleResolveResult& OutResult)
{
	const TArray<TPair<FString, FArtifactEntry>> Artifacts{
		{ TEXT("runtime manifest"), Package.RuntimeManifest },
		{ TEXT("canonical WASM"), Package.CanonicalWasm },
		{ TEXT("precompiled artifact"), Package.Precompiled },
		{ TEXT("binding manifest"), Package.BindingManifest },
		{ TEXT("binding descriptor"), Package.BindingDescriptor }
	};
	for (const TPair<FString, FArtifactEntry>& Pair : Artifacts)
	{
		TArray<uint8> Bytes;
		const FString Path = ResolveFixedArtifactPath(PackageRoot, Pair.Value.File);
		if (!IsPathUnderRoot(Path, PackageRoot)
			|| !LoadVerifiedBytes(Path, Pair.Value.Sha256, Bytes))
		{
			SetResolveFailure(
				OutResult,
				TEXT("artifact_identity_mismatch"),
				FString::Printf(TEXT("%s is missing or has the wrong SHA-256"), *Pair.Key),
				TEXT("restore or republish the complete module package"));
			return false;
		}
	}
	if (Package.DebugMap.IsSet())
	{
		TArray<uint8> DebugMapBytes;
		const FString DebugMapPath = ResolveFixedArtifactPath(
			PackageRoot,
			Package.DebugMap->File);
		if (!LoadVerifiedBytes(
				DebugMapPath,
				Package.DebugMap->Sha256,
				DebugMapBytes))
		{
			SetResolveFailure(
				OutResult,
				TEXT("artifact_identity_mismatch"),
				TEXT("debug map is missing or has the wrong SHA-256"),
				TEXT("restore or republish the Development package"));
			return false;
		}
	}
	return true;
}

void PopulateResolvedPackage(
	const FString& DescriptorPath,
	const FString& PackageRoot,
	const FCatalogEntry& Entry,
	const FDocument& Package,
	FAvidScriptResolvedModulePackage& OutPackage)
{
	OutPackage.ModuleId = FName(*Package.ModuleId);
	OutPackage.PackageId = Package.PackageId;
	OutPackage.Platform = Package.Platform;
	OutPackage.Architecture = Entry.Architecture;
	OutPackage.Configuration = Package.Configuration;
	OutPackage.ExecutionBackend = Package.Backend;
	OutPackage.ExecutionFormat = Package.Format;
	OutPackage.AbiVersion = Package.AbiVersion;
	OutPackage.MinimumRuntimeVersion = Package.MinimumRuntimeVersion;
	OutPackage.DescriptorPath = DescriptorPath;
	OutPackage.RuntimeManifestPath = ResolveFixedArtifactPath(
		PackageRoot,
		Package.RuntimeManifest.File);
	OutPackage.CanonicalWasmPath = ResolveFixedArtifactPath(
		PackageRoot,
		Package.CanonicalWasm.File);
	OutPackage.PrecompiledArtifactPath = ResolveFixedArtifactPath(
		PackageRoot,
		Package.Precompiled.File);
	OutPackage.BindingManifestPath = ResolveFixedArtifactPath(
		PackageRoot,
		Package.BindingManifest.File);
	OutPackage.BindingDescriptorPath = ResolveFixedArtifactPath(
		PackageRoot,
		Package.BindingDescriptor.File);
	if (Package.DebugMap.IsSet())
	{
		OutPackage.DebugMapPath = ResolveFixedArtifactPath(
			PackageRoot,
			Package.DebugMap->File);
	}
	OutPackage.CompilerBuildIdentity = Package.CompilerBuildIdentity;
	OutPackage.TargetTriple = Package.TargetTriple;
	OutPackage.CpuFeatures = Package.CpuFeatures;
	OutPackage.ExecutionPolicy = Package.Policy;
}
} // namespace

FString FAvidScriptModulePackageResolver::GetDefaultCatalogPath()
{
	return NormalizeModulePackagePath(FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("AvidScript/Modules/catalog.json")));
}

FAvidScriptModulePlatformContext
FAvidScriptModulePackageResolver::GetCurrentPlatformContext()
{
	FAvidScriptModulePlatformContext Context;
#if PLATFORM_WINDOWS
	Context.Platform = TEXT("win64");
#elif PLATFORM_ANDROID
	Context.Platform = TEXT("android");
#elif PLATFORM_IOS
	Context.Platform = TEXT("ios");
#else
	Context.Platform = TEXT("unsupported");
#endif

#if defined(PLATFORM_CPU_ARM_FAMILY) && PLATFORM_CPU_ARM_FAMILY
	Context.Architecture = TEXT("arm64");
#else
	Context.Architecture = TEXT("x86_64");
#endif

#if UE_BUILD_SHIPPING
	Context.Configuration = TEXT("shipping");
#else
	Context.Configuration = TEXT("development");
#endif
	Context.Backend = TEXT("wasmtime");
	Context.Format = TEXT("wasmtime_serialized_v1");
	return Context;
}

bool FAvidScriptModulePackageResolver::ResolveModule(
	const FName ModuleId,
	FAvidScriptResolvedModulePackage& OutPackage,
	FAvidScriptModuleResolveResult& OutResult)
{
	return ResolveModule(
		ModuleId,
		GetCurrentPlatformContext(),
		OutPackage,
		OutResult);
}

bool FAvidScriptModulePackageResolver::ResolveModule(
	const FName ModuleId,
	const FAvidScriptModulePlatformContext& PlatformContext,
	FAvidScriptResolvedModulePackage& OutPackage,
	FAvidScriptModuleResolveResult& OutResult)
{
	return ResolveModuleFromCatalogFile(
		GetDefaultCatalogPath(),
		ModuleId,
		PlatformContext,
		OutPackage,
		OutResult);
}

bool FAvidScriptModulePackageResolver::ResolveModuleFromCatalogFile(
	const FString& CatalogPath,
	const FName ModuleId,
	FAvidScriptResolvedModulePackage& OutPackage,
	FAvidScriptModuleResolveResult& OutResult)
{
	return ResolveModuleFromCatalogFile(
		CatalogPath,
		ModuleId,
		GetCurrentPlatformContext(),
		OutPackage,
		OutResult);
}

bool FAvidScriptModulePackageResolver::ResolveModuleFromCatalogFile(
	const FString& CatalogPath,
	const FName ModuleId,
	const FAvidScriptModulePlatformContext& PlatformContext,
	FAvidScriptResolvedModulePackage& OutPackage,
	FAvidScriptModuleResolveResult& OutResult)
{
	OutPackage = FAvidScriptResolvedModulePackage();
	OutResult = FAvidScriptModuleResolveResult();
	OutResult.CatalogPath = NormalizeModulePackagePath(CatalogPath);
	OutResult.ModuleId = ModuleId.ToString();
	if (!AvidScript::ModulePackage::IsNormalizedModuleId(OutResult.ModuleId))
	{
		SetResolveFailure(
			OutResult,
			TEXT("module_id_invalid"),
			FString::Printf(
				TEXT("module id must be %s"),
				AvidScript::ModulePackage::ModuleIdPatternDescription),
			TEXT("publish and reference a normalized logical module id"));
		return false;
	}
	if (!AvidScript::ModulePackage::IsValidVariantIdentity(
			PlatformContext.Platform,
			PlatformContext.Architecture,
			PlatformContext.Configuration,
			PlatformContext.Backend,
			PlatformContext.Format))
	{
		SetResolveFailure(
			OutResult,
			TEXT("platform_context_invalid"),
			DescribePlatformContext(PlatformContext),
			TEXT("use a supported normalized platform variant identity"));
		return false;
	}
	if (!FPaths::FileExists(OutResult.CatalogPath))
	{
		SetResolveFailure(
			OutResult,
			TEXT("catalog_missing"),
			TEXT("module catalog file does not exist"),
			TEXT("publish the module release catalog before loading the game"));
		return false;
	}

	TSharedPtr<FJsonObject> Catalog;
	int32 CatalogVersion = 0;
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	if (!LoadJsonObject(OutResult.CatalogPath, Catalog)
		|| !Catalog->TryGetNumberField(TEXT("schema_version"), CatalogVersion)
		|| (CatalogVersion != AvidScript::ModulePackage::LegacyCatalogSchemaVersion
			&& CatalogVersion != AvidScript::ModulePackage::CatalogSchemaVersion)
		|| Catalog->Values.Num() != 2
		|| !Catalog->HasField(TEXT("modules"))
		|| !Catalog->TryGetArrayField(TEXT("modules"), Modules)
		|| Modules == nullptr)
	{
		SetResolveFailure(
			OutResult,
			TEXT("catalog_invalid"),
			TEXT("catalog schema is invalid"),
			TEXT("republish catalog schema v2 with the current release tool"));
		return false;
	}

	TOptional<FCatalogEntry> SelectedEntry;
	bool bModuleFound = false;
	FString PreviousModuleId;
	for (const TSharedPtr<FJsonValue>& Value : *Modules)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value.IsValid()
			|| !Value->TryGetObject(Object)
			|| Object == nullptr)
		{
			SetResolveFailure(
				OutResult,
				TEXT("catalog_invalid"),
				TEXT("catalog entries are invalid, duplicated, or not ordinally sorted"),
				TEXT("republish the complete module catalog"));
			return false;
		}

		FString CatalogModuleId;
		if (CatalogVersion == AvidScript::ModulePackage::LegacyCatalogSchemaVersion)
		{
			FCatalogEntry Entry;
			if (!AvidScript::ModulePackage::ParseCatalogEntry(*Object, Entry))
			{
				SetResolveFailure(
					OutResult,
					TEXT("catalog_invalid"),
					TEXT("legacy catalog entry is invalid"),
					TEXT("republish the complete module catalog"));
				return false;
			}
			CatalogModuleId = Entry.ModuleId;
			if (Entry.ModuleId == OutResult.ModuleId)
			{
				bModuleFound = true;
				if (MatchesPlatformContext(Entry, PlatformContext))
				{
					SelectedEntry = Entry;
				}
			}
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* Variants = nullptr;
			if ((*Object)->Values.Num() != 2
				|| !(*Object)->HasField(TEXT("module_id"))
				|| !(*Object)->HasField(TEXT("variants"))
				|| !(*Object)->TryGetStringField(TEXT("module_id"), CatalogModuleId)
				|| !AvidScript::ModulePackage::IsNormalizedModuleId(CatalogModuleId)
				|| !(*Object)->TryGetArrayField(TEXT("variants"), Variants)
				|| Variants == nullptr
				|| Variants->IsEmpty())
			{
				SetResolveFailure(
					OutResult,
					TEXT("catalog_invalid"),
					TEXT("catalog module or variants array is invalid"),
					TEXT("republish the complete module catalog"));
				return false;
			}

			FString PreviousVariantKey;
			for (const TSharedPtr<FJsonValue>& VariantValue : *Variants)
			{
				const TSharedPtr<FJsonObject>* VariantObject = nullptr;
				FCatalogEntry Entry;
				if (!VariantValue.IsValid()
					|| !VariantValue->TryGetObject(VariantObject)
					|| VariantObject == nullptr
					|| !AvidScript::ModulePackage::ParseCatalogVariant(
						CatalogModuleId,
						*VariantObject,
						Entry))
				{
					SetResolveFailure(
						OutResult,
						TEXT("catalog_invalid"),
						TEXT("catalog variant entry is invalid"),
						TEXT("republish the complete module catalog"));
					return false;
				}
				const FString VariantKey =
					AvidScript::ModulePackage::MakeVariantKey(Entry);
				if (!PreviousVariantKey.IsEmpty()
					&& PreviousVariantKey.Compare(
						VariantKey,
						ESearchCase::CaseSensitive) >= 0)
				{
					SetResolveFailure(
						OutResult,
						TEXT("catalog_invalid"),
						TEXT("catalog variants are duplicated or not ordinally sorted"),
						TEXT("republish the complete module catalog"));
					return false;
				}
				PreviousVariantKey = VariantKey;
				if (CatalogModuleId == OutResult.ModuleId)
				{
					bModuleFound = true;
					if (MatchesPlatformContext(Entry, PlatformContext))
					{
						SelectedEntry = Entry;
					}
				}
			}
		}

		if (!PreviousModuleId.IsEmpty()
			&& PreviousModuleId.Compare(
				CatalogModuleId,
				ESearchCase::CaseSensitive) >= 0)
		{
			SetResolveFailure(
				OutResult,
				TEXT("catalog_invalid"),
				TEXT("catalog modules are duplicated or not ordinally sorted"),
				TEXT("republish the complete module catalog"));
			return false;
		}
		PreviousModuleId = CatalogModuleId;
	}
	if (!SelectedEntry.IsSet())
	{
		SetResolveFailure(
			OutResult,
			bModuleFound ? TEXT("module_variant_not_found") : TEXT("module_not_found"),
			bModuleFound
				? DescribePlatformContext(PlatformContext)
				: TEXT("module id is not present in the release catalog"),
			bModuleFound
				? TEXT("publish the exact target variant before loading the game")
				: TEXT("publish the requested module or update the component reference"));
		return false;
	}

	const FCatalogEntry& Entry = SelectedEntry.GetValue();
	OutResult.PackageId = Entry.PackageId;
	const FString CatalogRoot = NormalizeModulePackagePath(FPaths::GetPath(OutResult.CatalogPath));
	const FString DescriptorPath = NormalizeModulePackagePath(
		FPaths::Combine(CatalogRoot, Entry.DescriptorFile));
	const FString PackageRoot = NormalizeModulePackagePath(FPaths::GetPath(DescriptorPath));
	if (!IsPathUnderRoot(DescriptorPath, CatalogRoot)
		|| !FPaths::FileExists(DescriptorPath))
	{
		SetResolveFailure(
			OutResult,
			TEXT("package_path_invalid"),
			TEXT("package descriptor is missing or escapes the catalog root"),
			TEXT("republish the content-addressed module package"));
		return false;
	}

	TArray<uint8> DescriptorBytes;
	if (!LoadVerifiedBytes(DescriptorPath, Entry.DescriptorSha256, DescriptorBytes))
	{
		SetResolveFailure(
			OutResult,
			TEXT("package_hash_mismatch"),
			TEXT("package descriptor hash does not match the catalog"),
			TEXT("restore or republish the release package"));
		return false;
	}

	TSharedPtr<FJsonObject> PackageObject;
	FDocument Package;
	if (!LoadJsonObject(DescriptorPath, PackageObject)
		|| !AvidScript::ModulePackage::ParseDocument(PackageObject, Package)
		|| Package.PackageId != Entry.PackageId
		|| Package.ModuleId != Entry.ModuleId
		|| Package.Platform != Entry.Platform
		|| Package.Configuration != Entry.Configuration
		|| Package.Backend != Entry.Backend
		|| Package.Format != Entry.Format)
	{
		SetResolveFailure(
			OutResult,
			TEXT("package_invalid"),
			TEXT("package schema, identity, platform, or configuration is invalid"),
			TEXT("republish the package and catalog as one transaction"));
		return false;
	}
	if (!IsRuntimeVersionCompatible(Package.MinimumRuntimeVersion))
	{
		SetResolveFailure(
			OutResult,
			TEXT("runtime_version_incompatible"),
			FString::Printf(
				TEXT("package requires AvidScript runtime %s or newer"),
				*Package.MinimumRuntimeVersion),
			TEXT("upgrade the plugin or republish for the installed runtime"));
		return false;
	}
	if (!ValidateExactFileSet(PackageRoot, Package))
	{
		SetResolveFailure(
			OutResult,
			TEXT("package_file_set_invalid"),
			TEXT("package directory has missing or undeclared files"),
			TEXT("remove stale outputs and republish the package"));
		return false;
	}
	if (!LoadAndValidateArtifacts(PackageRoot, Package, OutResult))
	{
		return false;
	}

	const FString RuntimeManifestPath = ResolveFixedArtifactPath(
		PackageRoot,
		Package.RuntimeManifest.File);
	if (!AvidScript::ModulePackage::ValidateRuntimeManifest(
			RuntimeManifestPath,
			Package))
	{
		SetResolveFailure(
			OutResult,
			TEXT("runtime_manifest_mismatch"),
			TEXT("Runtime manifest does not match the verified package contract"),
			TEXT("publish the Runtime manifest and package descriptor together"));
		return false;
	}

	PopulateResolvedPackage(DescriptorPath, PackageRoot, Entry, Package, OutPackage);
#if WITH_EDITOR
	OutPackage.TrustDomain = EAvidScriptModulePackageTrustDomain::DevelopmentCatalog;
#else
	OutPackage.TrustDomain = OutResult.CatalogPath == GetDefaultCatalogPath()
		? EAvidScriptModulePackageTrustDomain::CookedPackage
		: EAvidScriptModulePackageTrustDomain::DevelopmentCatalog;
#endif
	OutResult.bSucceeded = true;
	return true;
}
