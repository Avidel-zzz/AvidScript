#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmReload.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Ssl.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
const uint8 GAvidScriptReloadCompatibleWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x07,
	0x02, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

const uint8 GAvidScriptReloadMissingTickWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x16, 0x01, 0x12, 0x61, 0x76, 0x69, 0x64,
	0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69,
	0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00,
	0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b
};

FString ReloadTestBytesToLowerHex(const uint8* Bytes, int32 ByteCount)
{
	FString Hex;
	Hex.Reserve(ByteCount * 2);
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Hex += FString::Printf(TEXT("%02x"), Bytes[Index]);
	}
	return Hex;
}

FString ComputeReloadTestSha256Hex(const TArray<uint8>& Bytes)
{
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	SHA256(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Digest);
	return ReloadTestBytesToLowerHex(Digest, UE_ARRAY_COUNT(Digest));
}

FString GetReloadManifestTestRoot()
{
	FString TestRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptReloadManifestTests"));
	TestRoot = FPaths::ConvertRelativePathToFull(TestRoot);
	FPaths::NormalizeFilename(TestRoot);
	return TestRoot;
}

FString JsonPathForReloadManifestTest(const FString& Path)
{
	return Path.Replace(TEXT("\\"), TEXT("/"));
}

FString ProjectRelativeJsonPathForReloadManifestTest(const FString& Path)
{
	FString RelativePath = Path;
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeFilename(ProjectDir);
	if (FPaths::MakePathRelativeTo(RelativePath, *ProjectDir))
	{
		return JsonPathForReloadManifestTest(RelativePath);
	}

	return JsonPathForReloadManifestTest(Path);
}

bool WriteReloadManifestFixture(
	const FString& ManifestPath,
	const FString& ModuleId,
	const FString& WasmPath,
	const FString& WasmSha256)
{
	const FString ManifestJson = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"module_id\": \"%s\",\n")
		TEXT("  \"abi_version\": 1,\n")
		TEXT("  \"language\": \"d\",\n")
		TEXT("  \"source\": { \"file\": \"Generated/manifest_smoke.d\" },\n")
		TEXT("  \"wasm\": { \"file\": \"%s\", \"sha256\": \"%s\" },\n")
		TEXT("  \"required_exports\": [\"avid_on_begin_play\", \"avid_on_tick\"],\n")
		TEXT("  \"required_imports\": [{ \"module\": \"env\", \"name\": \"actor_set_location\" }],\n")
		TEXT("  \"toolchain\": { \"compiler\": \"ldc2\", \"version\": \"1.42.0\", \"target\": \"wasm32-unknown-unknown-wasm\", \"linker\": \"ldc2-internal-lld\" }\n")
		TEXT("}\n"),
		*ModuleId,
		*ProjectRelativeJsonPathForReloadManifestTest(WasmPath),
		*WasmSha256);

	return FFileHelper::SaveStringToFile(ManifestJson, *ManifestPath);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadCompatibleSmokeTest,
	"AvidScript.Reload.CompatibleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadCompatibleSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;

	TestTrue(
		TEXT("Initial module loads"),
		Session.LoadInitialModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v1")),
			Result));
	TestEqual(TEXT("Initial live module id"), Session.GetLiveModuleId(), FString(TEXT("reload_v1")));

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Initial live runtime ticks"), Session.TickLive(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Initial live tick count"), Session.GetLiveTickCallCount(), 1);

	TestTrue(
		TEXT("Compatible reload applies"),
		Session.ReloadModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v2")),
			Result));

	TestTrue(TEXT("Reload result reports applied"), Result.bReloadApplied);
	TestEqual(TEXT("Reload result previous module"), Result.PreviousModuleId, FString(TEXT("reload_v1")));
	TestEqual(TEXT("Reload result active module"), Result.ActiveModuleId, FString(TEXT("reload_v2")));
	TestEqual(TEXT("Live module id switches"), Session.GetLiveModuleId(), FString(TEXT("reload_v2")));
	TestEqual(TEXT("Successful reload count"), Session.GetSuccessfulReloadCount(), 1);

	TestTrue(TEXT("Reloaded live runtime ticks"), Session.TickLive(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Reloaded live tick count starts fresh"), Session.GetLiveTickCallCount(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadMissingExportRollbackSmokeTest,
	"AvidScript.Reload.MissingExportRollbackSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadMissingExportRollbackSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;

	TestTrue(
		TEXT("Initial module loads"),
		Session.LoadInitialModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v1")),
			Result));

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Initial live runtime ticks"), Session.TickLive(1.0f / 60.0f, TickResult));

	TestFalse(
		TEXT("Reload with missing tick export is rejected"),
		Session.ReloadModule(
			GAvidScriptReloadMissingTickWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadMissingTickWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_missing_tick")),
			Result));

	TestTrue(TEXT("Rollback preserved live runtime"), Result.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("Missing export category"), Result.ErrorCategory, FString(TEXT("missing_export")));
	TestEqual(TEXT("Missing export name"), Result.ExportName, FString(TEXT("avid_on_tick")));
	TestEqual(TEXT("Live module id stays on previous runtime"), Session.GetLiveModuleId(), FString(TEXT("reload_v1")));
	TestEqual(TEXT("Rejected reload count"), Session.GetRejectedReloadCount(), 1);

	TestTrue(TEXT("Previous live runtime still ticks"), Session.TickLive(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Previous live tick count continues"), Session.GetLiveTickCallCount(), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadAbiMismatchRollbackSmokeTest,
	"AvidScript.Reload.AbiMismatchRollbackSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadAbiMismatchRollbackSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;

	TestTrue(
		TEXT("Initial module loads"),
		Session.LoadInitialModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v1")),
			Result));

	FAvidScriptWasmReloadManifest MismatchedManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_abi_v2"));
	MismatchedManifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion + 1;

	TestFalse(
		TEXT("Reload with ABI mismatch is rejected"),
		Session.ReloadModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			MismatchedManifest,
			Result));

	TestTrue(TEXT("Rollback preserved live runtime"), Result.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("ABI mismatch category"), Result.ErrorCategory, FString(TEXT("abi_mismatch")));
	TestEqual(TEXT("Live module id stays on previous runtime"), Session.GetLiveModuleId(), FString(TEXT("reload_v1")));
	TestEqual(TEXT("Rejected reload count"), Session.GetRejectedReloadCount(), 1);

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Previous live runtime still ticks"), Session.TickLive(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Previous live tick count"), Session.GetLiveTickCallCount(), 1);

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadManifestLoadsWasmSmokeTest,
	"AvidScript.Reload.ManifestLoadsWasmSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadManifestLoadsWasmSmokeTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = GetReloadManifestTestRoot();
	IFileManager::Get().MakeDirectory(*TestRoot, true);

	const FString WasmPath = FPaths::Combine(TestRoot, TEXT("manifest_smoke.wasm"));
	const FString ManifestPath = FPaths::Combine(TestRoot, TEXT("manifest_smoke.avidscript.json"));

	TArray<uint8> WasmBytes;
	WasmBytes.Append(GAvidScriptReloadCompatibleWasmModule, UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule));
	TestTrue(TEXT("WASM fixture writes"), FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath));

	const FString WasmSha256 = ComputeReloadTestSha256Hex(WasmBytes);
	TestFalse(TEXT("WASM hash is non-empty"), WasmSha256.IsEmpty());
	TestTrue(TEXT("Manifest fixture writes"), WriteReloadManifestFixture(ManifestPath, TEXT("manifest_smoke"), WasmPath, WasmSha256));

	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> LoadedBytecode;
	const bool bLoadSucceeded = FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		ManifestPath,
		Manifest,
		LoadedBytecode,
		LoadResult);
	TestTrue(
		TEXT("Manifest loads matching WASM"),
		bLoadSucceeded);
	if (!bLoadSucceeded)
	{
		return false;
	}
	TestEqual(TEXT("Module id"), Manifest.ModuleId, FString(TEXT("manifest_smoke")));
	TestEqual(TEXT("Language"), Manifest.Language, FString(TEXT("d")));
	TestEqual(TEXT("WASM file"), Manifest.WasmFile, WasmPath);
	TestEqual(TEXT("WASM SHA256"), Manifest.WasmSha256, WasmSha256);
	TestEqual(TEXT("Required export count"), Manifest.RequiredExports.Num(), 2);
	TestEqual(TEXT("Required import count"), Manifest.RequiredImports.Num(), 1);
	TestEqual(TEXT("Required import module"), Manifest.RequiredImports[0].ModuleName, FString(TEXT("env")));
	TestEqual(TEXT("Required import name"), Manifest.RequiredImports[0].ImportName, FString(TEXT("actor_set_location")));
	TestEqual(TEXT("Loaded byte size"), LoadedBytecode.Num(), WasmBytes.Num());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadManifestRejectsHashMismatchSmokeTest,
	"AvidScript.Reload.ManifestRejectsHashMismatchSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadManifestRejectsHashMismatchSmokeTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = GetReloadManifestTestRoot();
	IFileManager::Get().MakeDirectory(*TestRoot, true);

	const FString WasmPath = FPaths::Combine(TestRoot, TEXT("manifest_hash_mismatch.wasm"));
	const FString ManifestPath = FPaths::Combine(TestRoot, TEXT("manifest_hash_mismatch.avidscript.json"));

	TArray<uint8> WasmBytes;
	WasmBytes.Append(GAvidScriptReloadCompatibleWasmModule, UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule));
	TestTrue(TEXT("WASM fixture writes"), FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath));
	TestTrue(
		TEXT("Manifest fixture writes"),
		WriteReloadManifestFixture(
			ManifestPath,
			TEXT("manifest_hash_mismatch"),
			WasmPath,
			TEXT("0000000000000000000000000000000000000000000000000000000000000000")));

	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> LoadedBytecode;
	TestFalse(
		TEXT("Manifest rejects mismatched hash"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(ManifestPath, Manifest, LoadedBytecode, LoadResult));
	TestEqual(TEXT("Hash mismatch category"), LoadResult.ErrorCategory, FString(TEXT("module_hash_mismatch")));
	TestEqual(TEXT("No bytecode returned on hash mismatch"), LoadedBytecode.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadManifestRejectsMissingWasmSmokeTest,
	"AvidScript.Reload.ManifestRejectsMissingWasmSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadManifestRejectsMissingWasmSmokeTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = GetReloadManifestTestRoot();
	IFileManager::Get().MakeDirectory(*TestRoot, true);

	const FString MissingWasmPath = FPaths::Combine(TestRoot, TEXT("missing_manifest_wasm.wasm"));
	const FString ManifestPath = FPaths::Combine(TestRoot, TEXT("manifest_missing_wasm.avidscript.json"));
	if (FPaths::FileExists(MissingWasmPath))
	{
		IFileManager::Get().Delete(*MissingWasmPath);
	}

	TestTrue(
		TEXT("Manifest fixture writes"),
		WriteReloadManifestFixture(
			ManifestPath,
			TEXT("manifest_missing_wasm"),
			MissingWasmPath,
			TEXT("0000000000000000000000000000000000000000000000000000000000000000")));

	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> LoadedBytecode;
	TestFalse(
		TEXT("Manifest rejects missing WASM"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(ManifestPath, Manifest, LoadedBytecode, LoadResult));
	TestEqual(TEXT("Missing module category"), LoadResult.ErrorCategory, FString(TEXT("module_file_missing")));
	TestEqual(TEXT("Missing module path"), LoadResult.ModulePath, MissingWasmPath);
	TestEqual(TEXT("No bytecode returned for missing module"), LoadedBytecode.Num(), 0);

	return true;
}
#endif
