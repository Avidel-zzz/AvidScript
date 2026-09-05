#include "Packages/AvidScriptModulePackage.h"
#include "Packages/AvidScriptModulePackageSchema.h"

#include "AvidScriptHash.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
bool WriteJsonObject(
	const FString& Path,
	const TSharedRef<FJsonObject>& Object)
{
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	return FJsonSerializer::Serialize(Object, Writer)
		&& FFileHelper::SaveStringToFile(
			Json,
			*Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString HashFile(const FString& Path)
{
	TArray<uint8> Bytes;
	return FFileHelper::LoadFileToArray(Bytes, *Path)
		? FAvidScriptHash::Sha256Hex(Bytes)
		: FString();
}

TSharedRef<FJsonObject> MakeFileEntry(
	const TCHAR* File,
	const FString& Sha256)
{
	TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("file"), File);
	Entry->SetStringField(TEXT("sha256"), Sha256);
	return Entry;
}

FString MakePackageIdentity(
	const FString& ModuleId,
	const FString& RuntimeManifestSha256,
	const FString& WasmSha256,
	const FString& PrecompiledSha256,
	const FString& BindingManifestSha256,
	const FString& BindingDescriptorSha256)
{
	const TArray<FString> Fields{
		TEXT("1"),
		ModuleId,
		TEXT("1"),
		TEXT("win64"),
		TEXT("shipping"),
		TEXT("0.1.0"),
		TEXT("wasmtime"),
		TEXT("wasmtime_serialized_v1"),
		TEXT("require_precompiled"),
		TEXT("wasmtime-test-build"),
		TEXT("x86_64-pc-windows-msvc"),
		TEXT("x86-64-v3"),
		RuntimeManifestSha256,
		WasmSha256,
		PrecompiledSha256,
		BindingManifestSha256,
		BindingDescriptorSha256
	};
	return FAvidScriptHash::Sha256HexUtf8(FString::Join(Fields, TEXT("\n")));
}

bool WriteShippingFixture(
	const FString& Root,
	const FString& ModuleId,
	FString& OutCatalogPath,
	FString& OutPackageRoot,
	const bool bUseLegacyCatalog = false)
{
	const FString StagingRoot = FPaths::Combine(Root, TEXT("staging"));
	const FString BindingRoot = FPaths::Combine(StagingRoot, TEXT("bindings"));
	if (!IFileManager::Get().MakeDirectory(*BindingRoot, true))
	{
		return false;
	}

	const FString WasmPath = FPaths::Combine(StagingRoot, TEXT("module.wasm"));
	const FString PrecompiledPath = FPaths::Combine(
		StagingRoot,
		TEXT("module.wasmtime.cwasm"));
	const FString BindingManifestPath = FPaths::Combine(
		BindingRoot,
		TEXT("package.json"));
	const FString BindingDescriptorPath = FPaths::Combine(
		BindingRoot,
		TEXT("bindings.json"));
	const TArray<uint8> WasmBytes{ 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	const TArray<uint8> PrecompiledBytes{ 0x61, 0x76, 0x69, 0x64, 0x63, 0x77, 0x61, 0x73, 0x6d };
	if (!FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath)
		|| !FFileHelper::SaveArrayToFile(PrecompiledBytes, *PrecompiledPath))
	{
		return false;
	}
	TSharedRef<FJsonObject> BindingDescriptor = MakeShared<FJsonObject>();
	BindingDescriptor->SetNumberField(TEXT("schema_version"), 1);
	TSharedRef<FJsonObject> BindingManifest = MakeShared<FJsonObject>();
	BindingManifest->SetNumberField(TEXT("schema_version"), 1);
	if (!WriteJsonObject(BindingDescriptorPath, BindingDescriptor)
		|| !WriteJsonObject(BindingManifestPath, BindingManifest))
	{
		return false;
	}

	const FString WasmSha256 = HashFile(WasmPath);
	const FString PrecompiledSha256 = HashFile(PrecompiledPath);
	const FString BindingManifestSha256 = HashFile(BindingManifestPath);
	const FString BindingDescriptorSha256 = HashFile(BindingDescriptorPath);

	TSharedRef<FJsonObject> RuntimeManifest = MakeShared<FJsonObject>();
	RuntimeManifest->SetNumberField(TEXT("schema_version"), 1);
	RuntimeManifest->SetStringField(TEXT("module_id"), ModuleId);
	RuntimeManifest->SetNumberField(TEXT("abi_version"), 1);
	RuntimeManifest->SetStringField(TEXT("language"), TEXT("csharp"));
	RuntimeManifest->SetObjectField(
		TEXT("wasm"),
		MakeFileEntry(TEXT("module.wasm"), WasmSha256));
	TSharedRef<FJsonObject> Execution = MakeShared<FJsonObject>();
	Execution->SetStringField(TEXT("format"), TEXT("wasmtime_serialized_v1"));
	Execution->SetStringField(TEXT("file"), TEXT("module.wasmtime.cwasm"));
	Execution->SetStringField(TEXT("sha256"), PrecompiledSha256);
	Execution->SetStringField(TEXT("canonical_sha256"), WasmSha256);
	Execution->SetStringField(
		TEXT("compiler_build_identity"),
		TEXT("wasmtime-test-build"));
	Execution->SetStringField(
		TEXT("target_triple"),
		TEXT("x86_64-pc-windows-msvc"));
	Execution->SetStringField(
		TEXT("attestation_id"),
		TEXT("00000000000000000000000000000000"));
	Execution->SetStringField(TEXT("policy"), TEXT("require_precompiled"));
	Execution->SetStringField(TEXT("fallback"), TEXT("wasmtime_jit"));
	RuntimeManifest->SetObjectField(TEXT("execution"), Execution);
	TSharedRef<FJsonObject> BindingPackage = MakeShared<FJsonObject>();
	BindingPackage->SetStringField(
		TEXT("manifest_file"),
		TEXT("bindings/package.json"));
	BindingPackage->SetStringField(
		TEXT("manifest_sha256"),
		BindingManifestSha256);
	BindingPackage->SetStringField(
		TEXT("descriptor_file"),
		TEXT("bindings/bindings.json"));
	BindingPackage->SetStringField(
		TEXT("descriptor_sha256"),
		BindingDescriptorSha256);
	RuntimeManifest->SetObjectField(TEXT("binding_package"), BindingPackage);
	RuntimeManifest->SetArrayField(
		TEXT("required_exports"),
		TArray<TSharedPtr<FJsonValue>>());
	RuntimeManifest->SetArrayField(
		TEXT("required_imports"),
		TArray<TSharedPtr<FJsonValue>>());
	const FString RuntimeManifestPath = FPaths::Combine(
		StagingRoot,
		TEXT("runtime.avidscript.json"));
	if (!WriteJsonObject(RuntimeManifestPath, RuntimeManifest))
	{
		return false;
	}
	const FString RuntimeManifestSha256 = HashFile(RuntimeManifestPath);
	const FString PackageId = MakePackageIdentity(
		ModuleId,
		RuntimeManifestSha256,
		WasmSha256,
		PrecompiledSha256,
		BindingManifestSha256,
		BindingDescriptorSha256);

	const FString CatalogRoot = FPaths::Combine(Root, TEXT("Modules"));
	OutPackageRoot = FPaths::Combine(CatalogRoot, ModuleId, PackageId);
	if (!IFileManager::Get().MakeDirectory(
			*FPaths::Combine(OutPackageRoot, TEXT("bindings")),
			true))
	{
		return false;
	}
	if (IFileManager::Get().Copy(
		*FPaths::Combine(OutPackageRoot, TEXT("runtime.avidscript.json")),
		*RuntimeManifestPath) != COPY_OK
		|| IFileManager::Get().Copy(
		*FPaths::Combine(OutPackageRoot, TEXT("module.wasm")),
		*WasmPath) != COPY_OK
		|| IFileManager::Get().Copy(
		*FPaths::Combine(OutPackageRoot, TEXT("module.wasmtime.cwasm")),
		*PrecompiledPath) != COPY_OK
		|| IFileManager::Get().Copy(
		*FPaths::Combine(OutPackageRoot, TEXT("bindings/package.json")),
		*BindingManifestPath) != COPY_OK
		|| IFileManager::Get().Copy(
		*FPaths::Combine(OutPackageRoot, TEXT("bindings/bindings.json")),
		*BindingDescriptorPath) != COPY_OK)
	{
		return false;
	}

	TSharedRef<FJsonObject> Artifacts = MakeShared<FJsonObject>();
	Artifacts->SetObjectField(
		TEXT("runtime_manifest"),
		MakeFileEntry(TEXT("runtime.avidscript.json"), RuntimeManifestSha256));
	Artifacts->SetObjectField(
		TEXT("canonical_wasm"),
		MakeFileEntry(TEXT("module.wasm"), WasmSha256));
	Artifacts->SetObjectField(
		TEXT("precompiled"),
		MakeFileEntry(TEXT("module.wasmtime.cwasm"), PrecompiledSha256));
	Artifacts->SetObjectField(
		TEXT("binding_manifest"),
		MakeFileEntry(TEXT("bindings/package.json"), BindingManifestSha256));
	Artifacts->SetObjectField(
		TEXT("binding_descriptor"),
		MakeFileEntry(TEXT("bindings/bindings.json"), BindingDescriptorSha256));
	TSharedRef<FJsonObject> PackageExecution = MakeShared<FJsonObject>();
	PackageExecution->SetStringField(TEXT("backend"), TEXT("wasmtime"));
	PackageExecution->SetStringField(TEXT("format"), TEXT("wasmtime_serialized_v1"));
	PackageExecution->SetStringField(TEXT("policy"), TEXT("require_precompiled"));
	PackageExecution->SetStringField(
		TEXT("compiler_build_identity"),
		TEXT("wasmtime-test-build"));
	PackageExecution->SetStringField(
		TEXT("target_triple"),
		TEXT("x86_64-pc-windows-msvc"));
	PackageExecution->SetStringField(TEXT("cpu_features"), TEXT("x86-64-v3"));
	TSharedRef<FJsonObject> Package = MakeShared<FJsonObject>();
	Package->SetNumberField(TEXT("schema_version"), 1);
	Package->SetStringField(TEXT("package_id"), PackageId);
	Package->SetStringField(TEXT("module_id"), ModuleId);
	Package->SetNumberField(TEXT("abi_version"), 1);
	Package->SetStringField(TEXT("platform"), TEXT("win64"));
	Package->SetStringField(TEXT("configuration"), TEXT("shipping"));
	Package->SetStringField(TEXT("minimum_runtime_version"), TEXT("0.1.0"));
	Package->SetObjectField(TEXT("execution"), PackageExecution);
	Package->SetObjectField(TEXT("artifacts"), Artifacts);
	const FString PackagePath = FPaths::Combine(OutPackageRoot, TEXT("package.json"));
	if (!WriteJsonObject(PackagePath, Package))
	{
		return false;
	}

	TSharedRef<FJsonObject> CatalogVariant = MakeShared<FJsonObject>();
	CatalogVariant->SetStringField(TEXT("package_id"), PackageId);
	CatalogVariant->SetStringField(
		TEXT("descriptor_file"),
		FString::Printf(TEXT("%s/%s/package.json"), *ModuleId, *PackageId));
	CatalogVariant->SetStringField(TEXT("descriptor_sha256"), HashFile(PackagePath));
	CatalogVariant->SetStringField(TEXT("platform"), TEXT("win64"));
	CatalogVariant->SetStringField(TEXT("configuration"), TEXT("shipping"));
	TSharedRef<FJsonObject> CatalogEntry = MakeShared<FJsonObject>();
	CatalogEntry->SetStringField(TEXT("module_id"), ModuleId);
	if (bUseLegacyCatalog)
	{
		CatalogEntry->SetStringField(TEXT("package_id"), PackageId);
		CatalogEntry->SetStringField(
			TEXT("descriptor_file"),
			FString::Printf(TEXT("%s/%s/package.json"), *ModuleId, *PackageId));
		CatalogEntry->SetStringField(TEXT("descriptor_sha256"), HashFile(PackagePath));
		CatalogEntry->SetStringField(TEXT("platform"), TEXT("win64"));
		CatalogEntry->SetStringField(TEXT("configuration"), TEXT("shipping"));
	}
	else
	{
		CatalogVariant->SetStringField(TEXT("architecture"), TEXT("x86_64"));
		CatalogVariant->SetStringField(TEXT("backend"), TEXT("wasmtime"));
		CatalogVariant->SetStringField(
			TEXT("format"),
			TEXT("wasmtime_serialized_v1"));
		CatalogEntry->SetArrayField(
			TEXT("variants"),
			{ MakeShared<FJsonValueObject>(CatalogVariant) });
	}
	TSharedRef<FJsonObject> Catalog = MakeShared<FJsonObject>();
	Catalog->SetNumberField(TEXT("schema_version"), bUseLegacyCatalog ? 1 : 2);
	Catalog->SetArrayField(
		TEXT("modules"),
		{ MakeShared<FJsonValueObject>(CatalogEntry) });
	OutCatalogPath = FPaths::Combine(CatalogRoot, TEXT("catalog.json"));
	return WriteJsonObject(OutCatalogPath, Catalog);
}

FAvidScriptModulePlatformContext MakeWin64ShippingContext()
{
	FAvidScriptModulePlatformContext Context;
	Context.Platform = TEXT("win64");
	Context.Architecture = TEXT("x86_64");
	Context.Configuration = TEXT("shipping");
	Context.Backend = TEXT("wasmtime");
	Context.Format = TEXT("wasmtime_serialized_v1");
	return Context;
}

TSharedRef<FJsonObject> MakePackageDocument(
	const FString& Platform,
	const FString& Configuration,
	const FString& Policy,
	const FString& TargetTriple,
	const FString& CpuFeatures)
{
	const FString Sha256 = FString::ChrN(64, TEXT('0'));
	AvidScript::ModulePackage::FDocument Identity;
	Identity.ModuleId = TEXT("platform_contract");
	Identity.AbiVersion = 1;
	Identity.Platform = Platform;
	Identity.Configuration = Configuration;
	Identity.MinimumRuntimeVersion = TEXT("0.1.0");
	Identity.Backend = TEXT("wasmtime");
	Identity.Format = TEXT("wasmtime_serialized_v1");
	Identity.Policy = Policy;
	Identity.CompilerBuildIdentity = TEXT("wasmtime-test-build");
	Identity.TargetTriple = TargetTriple;
	Identity.CpuFeatures = CpuFeatures;
	Identity.RuntimeManifest = { TEXT("runtime.avidscript.json"), Sha256 };
	Identity.CanonicalWasm = { TEXT("module.wasm"), Sha256 };
	Identity.Precompiled = { TEXT("module.wasmtime.cwasm"), Sha256 };
	Identity.BindingManifest = { TEXT("bindings/package.json"), Sha256 };
	Identity.BindingDescriptor = { TEXT("bindings/bindings.json"), Sha256 };
	Identity.PackageId = FAvidScriptHash::Sha256HexUtf8(
		AvidScript::ModulePackage::MakeIdentityPayload(Identity));

	TSharedRef<FJsonObject> Execution = MakeShared<FJsonObject>();
	Execution->SetStringField(TEXT("backend"), Identity.Backend);
	Execution->SetStringField(TEXT("format"), Identity.Format);
	Execution->SetStringField(TEXT("policy"), Identity.Policy);
	Execution->SetStringField(
		TEXT("compiler_build_identity"),
		Identity.CompilerBuildIdentity);
	Execution->SetStringField(TEXT("target_triple"), Identity.TargetTriple);
	Execution->SetStringField(TEXT("cpu_features"), Identity.CpuFeatures);

	TSharedRef<FJsonObject> Artifacts = MakeShared<FJsonObject>();
	Artifacts->SetObjectField(
		TEXT("runtime_manifest"),
		MakeFileEntry(TEXT("runtime.avidscript.json"), Sha256));
	Artifacts->SetObjectField(
		TEXT("canonical_wasm"),
		MakeFileEntry(TEXT("module.wasm"), Sha256));
	Artifacts->SetObjectField(
		TEXT("precompiled"),
		MakeFileEntry(TEXT("module.wasmtime.cwasm"), Sha256));
	Artifacts->SetObjectField(
		TEXT("binding_manifest"),
		MakeFileEntry(TEXT("bindings/package.json"), Sha256));
	Artifacts->SetObjectField(
		TEXT("binding_descriptor"),
		MakeFileEntry(TEXT("bindings/bindings.json"), Sha256));

	TSharedRef<FJsonObject> Package = MakeShared<FJsonObject>();
	Package->SetNumberField(
		TEXT("schema_version"),
		AvidScript::ModulePackage::PackageSchemaVersion);
	Package->SetStringField(TEXT("package_id"), Identity.PackageId);
	Package->SetStringField(TEXT("module_id"), Identity.ModuleId);
	Package->SetNumberField(TEXT("abi_version"), Identity.AbiVersion);
	Package->SetStringField(TEXT("platform"), Identity.Platform);
	Package->SetStringField(TEXT("configuration"), Identity.Configuration);
	Package->SetStringField(
		TEXT("minimum_runtime_version"),
		Identity.MinimumRuntimeVersion);
	Package->SetObjectField(TEXT("execution"), Execution);
	Package->SetObjectField(TEXT("artifacts"), Artifacts);
	return Package;
}

bool ParsePackageDocument(const TSharedRef<FJsonObject>& Package)
{
	AvidScript::ModulePackage::FDocument Parsed;
	return AvidScript::ModulePackage::ParseDocument(Package, Parsed);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptModulePackagePlatformSchemaTest,
	"AvidScript.Runtime.ModulePackage.PlatformSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptModulePackagePlatformSchemaTest::RunTest(
	const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace AvidScript::ModulePackage;

	TestTrue(
		TEXT("Win64 Development may prefer precompiled"),
		ParsePackageDocument(MakePackageDocument(
			Win64Platform,
			TEXT("development"),
			TEXT("prefer_precompiled"),
			Win64TargetTriple,
			Win64CpuFeatures)));
	TestTrue(
		TEXT("Win64 Shipping requires precompiled"),
		ParsePackageDocument(MakePackageDocument(
			Win64Platform,
			TEXT("shipping"),
			TEXT("require_precompiled"),
			Win64TargetTriple,
			Win64CpuFeatures)));
	TestFalse(
		TEXT("Win64 Shipping rejects prefer precompiled"),
		ParsePackageDocument(MakePackageDocument(
			Win64Platform,
			TEXT("shipping"),
			TEXT("prefer_precompiled"),
			Win64TargetTriple,
			Win64CpuFeatures)));

	const TArray<FString> AndroidConfigurations{
		TEXT("development"),
		TEXT("shipping")
	};
	for (const FString& Configuration : AndroidConfigurations)
	{
		TestTrue(
			*FString::Printf(
				TEXT("Android %s requires precompiled"),
				*Configuration),
			ParsePackageDocument(MakePackageDocument(
				AndroidPlatform,
				Configuration,
				TEXT("require_precompiled"),
				AndroidTargetTriple,
				AndroidCpuFeatures)));
		TestFalse(
			*FString::Printf(
				TEXT("Android %s rejects prefer precompiled"),
				*Configuration),
			ParsePackageDocument(MakePackageDocument(
				AndroidPlatform,
				Configuration,
				TEXT("prefer_precompiled"),
				AndroidTargetTriple,
				AndroidCpuFeatures)));
	}

	TestFalse(
		TEXT("Win64 rejects Android target"),
		ParsePackageDocument(MakePackageDocument(
			Win64Platform,
			TEXT("development"),
			TEXT("require_precompiled"),
			AndroidTargetTriple,
			Win64CpuFeatures)));
	TestFalse(
		TEXT("Android rejects Win64 CPU features"),
		ParsePackageDocument(MakePackageDocument(
			AndroidPlatform,
			TEXT("development"),
			TEXT("require_precompiled"),
			AndroidTargetTriple,
			Win64CpuFeatures)));
	TestFalse(
		TEXT("Unknown platform fails closed"),
		ParsePackageDocument(MakePackageDocument(
			TEXT("linux"),
			TEXT("development"),
			TEXT("require_precompiled"),
			TEXT("x86_64-unknown-linux-gnu"),
			TEXT("x86-64-v3"))));
	TestTrue(
		TEXT("Win64 catalog architecture is exact"),
		IsValidVariantIdentity(
			Win64Platform,
			Win64Architecture,
			TEXT("development"),
			TEXT("wasmtime"),
			TEXT("wasmtime_serialized_v1")));
	TestTrue(
		TEXT("Android catalog architecture is exact"),
		IsValidVariantIdentity(
			AndroidPlatform,
			AndroidArchitecture,
			TEXT("shipping"),
			TEXT("wasmtime"),
			TEXT("wasmtime_serialized_v1")));
	TestFalse(
		TEXT("Cross-platform catalog architecture fails closed"),
		IsValidVariantIdentity(
			AndroidPlatform,
			Win64Architecture,
			TEXT("shipping"),
			TEXT("wasmtime"),
			TEXT("wasmtime_serialized_v1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptModulePackageResolverTest,
	"AvidScript.Runtime.ModulePackage.Resolver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptModulePackageResolverTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString TestRoot = FPaths::Combine(
		FPlatformProcess::UserTempDir(),
		TEXT("AvidScriptTests/ModulePackageResolver"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	};

	FString CatalogPath;
	FString PackageRoot;
	if (!TestTrue(
			TEXT("Shipping module package fixture writes"),
			WriteShippingFixture(
				TestRoot,
				TEXT("actor_lifecycle"),
				CatalogPath,
				PackageRoot)))
	{
		return false;
	}

	FAvidScriptResolvedModulePackage Package;
	FAvidScriptModuleResolveResult Result;
	const FAvidScriptModulePlatformContext ShippingContext =
		MakeWin64ShippingContext();
	TestTrue(
		TEXT("Verified Shipping package resolves"),
		FAvidScriptModulePackageResolver::ResolveModuleFromCatalogFile(
			CatalogPath,
			TEXT("actor_lifecycle"),
			ShippingContext,
			Package,
			Result));
	TestTrue(TEXT("Resolve result succeeds"), Result.bSucceeded);
	TestEqual(
		TEXT("Resolved module identity"),
		Package.ModuleId,
		FName(TEXT("actor_lifecycle")));
	TestEqual(
		TEXT("Resolved policy is precompiled-only"),
		Package.ExecutionPolicy,
		FString(TEXT("require_precompiled")));
	TestEqual(
		TEXT("Resolved architecture is explicit"),
		Package.Architecture,
		FString(TEXT("x86_64")));
	TestEqual(
		TEXT("Resolved backend is explicit"),
		Package.ExecutionBackend,
		FString(TEXT("wasmtime")));
	TestEqual(
		TEXT("Custom catalog remains Development trust"),
		Package.TrustDomain,
		EAvidScriptModulePackageTrustDomain::DevelopmentCatalog);

	const FString ExtraPath = FPaths::Combine(PackageRoot, TEXT("undeclared.bin"));
	TestTrue(
		TEXT("Undeclared fixture file writes"),
		FFileHelper::SaveStringToFile(TEXT("extra"), *ExtraPath));
	TestFalse(
		TEXT("Package rejects undeclared files"),
		FAvidScriptModulePackageResolver::ResolveModuleFromCatalogFile(
			CatalogPath,
			TEXT("actor_lifecycle"),
			ShippingContext,
			Package,
			Result));
	TestEqual(
		TEXT("Undeclared file category"),
		Result.ErrorCategory,
		FString(TEXT("package_file_set_invalid")));
	IFileManager::Get().Delete(*ExtraPath);

	const FString WasmPath = FPaths::Combine(PackageRoot, TEXT("module.wasm"));
	TestTrue(
		TEXT("Canonical WASM tamper writes"),
		FFileHelper::SaveStringToFile(TEXT("tampered"), *WasmPath));
	TestFalse(
		TEXT("Package rejects artifact hash drift"),
		FAvidScriptModulePackageResolver::ResolveModuleFromCatalogFile(
			CatalogPath,
			TEXT("actor_lifecycle"),
			ShippingContext,
			Package,
			Result));
	TestEqual(
		TEXT("Artifact drift category"),
		Result.ErrorCategory,
		FString(TEXT("artifact_identity_mismatch")));

	TestFalse(
		TEXT("Resolver rejects non-normalized module ids"),
		FAvidScriptModulePackageResolver::ResolveModuleFromCatalogFile(
			CatalogPath,
			TEXT("Actor/Lifecycle"),
			ShippingContext,
			Package,
			Result));
	TestEqual(
		TEXT("Invalid module id category"),
		Result.ErrorCategory,
		FString(TEXT("module_id_invalid")));

	FAvidScriptModulePlatformContext AndroidContext = ShippingContext;
	AndroidContext.Platform = TEXT("android");
	AndroidContext.Architecture = TEXT("arm64");
	TestFalse(
		TEXT("Missing Android variant fails closed"),
		FAvidScriptModulePackageResolver::ResolveModuleFromCatalogFile(
			CatalogPath,
			TEXT("actor_lifecycle"),
			AndroidContext,
			Package,
			Result));
	TestEqual(
		TEXT("Missing Android variant category"),
		Result.ErrorCategory,
		FString(TEXT("module_variant_not_found")));

	const FString LegacyRoot = FPaths::Combine(TestRoot, TEXT("Legacy"));
	FString LegacyCatalogPath;
	FString LegacyPackageRoot;
	TestTrue(
		TEXT("Legacy schema v1 fixture writes"),
		WriteShippingFixture(
			LegacyRoot,
			TEXT("legacy_lifecycle"),
			LegacyCatalogPath,
			LegacyPackageRoot,
			true));
	TestTrue(
		TEXT("Legacy schema v1 remains readable"),
		FAvidScriptModulePackageResolver::ResolveModuleFromCatalogFile(
			LegacyCatalogPath,
			TEXT("legacy_lifecycle"),
			ShippingContext,
			Package,
			Result));
	return true;
}

#endif
