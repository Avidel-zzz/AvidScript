#include "Packages/AvidScriptModulePackage.h"

#include "AvidScriptHash.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
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
	FString& OutPackageRoot)
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

	TSharedRef<FJsonObject> CatalogEntry = MakeShared<FJsonObject>();
	CatalogEntry->SetStringField(TEXT("module_id"), ModuleId);
	CatalogEntry->SetStringField(TEXT("package_id"), PackageId);
	CatalogEntry->SetStringField(
		TEXT("descriptor_file"),
		FString::Printf(TEXT("%s/%s/package.json"), *ModuleId, *PackageId));
	CatalogEntry->SetStringField(TEXT("descriptor_sha256"), HashFile(PackagePath));
	CatalogEntry->SetStringField(TEXT("platform"), TEXT("win64"));
	CatalogEntry->SetStringField(TEXT("configuration"), TEXT("shipping"));
	TSharedRef<FJsonObject> Catalog = MakeShared<FJsonObject>();
	Catalog->SetNumberField(TEXT("schema_version"), 1);
	Catalog->SetArrayField(
		TEXT("modules"),
		{ MakeShared<FJsonValueObject>(CatalogEntry) });
	OutCatalogPath = FPaths::Combine(CatalogRoot, TEXT("catalog.json"));
	return WriteJsonObject(OutCatalogPath, Catalog);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptModulePackageResolverTest,
	"AvidScript.Runtime.ModulePackage.Resolver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptModulePackageResolverTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FString TestRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(),
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
	TestTrue(
		TEXT("Verified Shipping package resolves"),
		FAvidScriptModulePackageResolver::ResolveModuleFromCatalogFile(
			CatalogPath,
			TEXT("actor_lifecycle"),
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
			Package,
			Result));
	TestEqual(
		TEXT("Invalid module id category"),
		Result.ErrorCategory,
		FString(TEXT("module_id_invalid")));
	return true;
}

#endif
