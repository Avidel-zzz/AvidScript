#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmReload.h"

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptObjectRegistryTestTypes.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
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

FString NormalizeReloadTestFullPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString GetReloadDGuestManifestPath(const TCHAR* VariantName)
{
	return NormalizeReloadTestFullPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptDGuest"),
		TEXT("Reload"),
		VariantName,
		TEXT("actor_set_location_guest.avidscript.json")));
}

bool AreReloadDGuestArtifactsAvailable(FString& OutMissingPath)
{
	const FString ManifestV1Path = GetReloadDGuestManifestPath(TEXT("v1"));
	if (!FPaths::FileExists(ManifestV1Path))
	{
		OutMissingPath = ManifestV1Path;
		return false;
	}

	const FString ManifestV2Path = GetReloadDGuestManifestPath(TEXT("v2"));
	if (!FPaths::FileExists(ManifestV2Path))
	{
		OutMissingPath = ManifestV2Path;
		return false;
	}

	return true;
}

bool CreateReloadDGuestWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptReloadDGuestWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyReloadDGuestWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}

	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}

	World->DestroyWorld(false);
	World = nullptr;
}

bool WriteReloadDGuestCorruptHashManifest(
	const FString& SourceManifestPath,
	const FString& ActualSha256,
	const FString& CorruptManifestPath)
{
	FString ManifestJson;
	if (!FFileHelper::LoadFileToString(ManifestJson, *SourceManifestPath))
	{
		return false;
	}

	const FString BadSha256(TEXT("0000000000000000000000000000000000000000000000000000000000000000"));
	ManifestJson = ManifestJson.Replace(*ActualSha256, *BadSha256);
	return FFileHelper::SaveStringToFile(ManifestJson, *CorruptManifestPath);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadDGuestActorHostContextSmokeTest,
	"AvidScript.Reload.DGuestActorHostContextSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadDGuestActorHostContextSmokeTest::RunTest(const FString& Parameters)
{
	FString MissingArtifactPath;
	if (!AreReloadDGuestArtifactsAvailable(MissingArtifactPath))
	{
		AddWarning(FString::Printf(
			TEXT("D reload guest artifact is missing; build v1/v2 with BuildDGuestActorSetLocation.ps1 before running this smoke. missing=%s"),
			*MissingArtifactPath));
		return true;
	}

	const FString ManifestV1Path = GetReloadDGuestManifestPath(TEXT("v1"));
	const FString ManifestV2Path = GetReloadDGuestManifestPath(TEXT("v2"));

	FAvidScriptWasmReloadManifestLoadResult LoadV1Result;
	FAvidScriptWasmReloadManifest ManifestV1;
	TArray<uint8> BytecodeV1;
	const bool bLoadedV1 = FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		ManifestV1Path,
		ManifestV1,
		BytecodeV1,
		LoadV1Result);
	if (!bLoadedV1)
	{
		AddError(LoadV1Result.ErrorMessage);
		return true;
	}
	TestTrue(TEXT("D reload v1 manifest loads"), bLoadedV1);

	FAvidScriptWasmReloadManifestLoadResult LoadV2Result;
	FAvidScriptWasmReloadManifest ManifestV2;
	TArray<uint8> BytecodeV2;
	const bool bLoadedV2 = FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		ManifestV2Path,
		ManifestV2,
		BytecodeV2,
		LoadV2Result);
	if (!bLoadedV2)
	{
		AddError(LoadV2Result.ErrorMessage);
		return true;
	}
	TestTrue(TEXT("D reload v2 manifest loads"), bLoadedV2);

	UWorld* World = nullptr;
	if (!CreateReloadDGuestWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript D reload smoke world."));
		DestroyReloadDGuestWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyReloadDGuestWorld(World);
		return true;
	}

	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers"), RegisterResult.bSucceeded);
	TestEqual(TEXT("D reload sample slot matches first registry handle"), ActorHandle.Slot, static_cast<uint32>(1));
	TestEqual(TEXT("D reload sample generation matches first registry handle"), ActorHandle.Generation, static_cast<uint32>(1));

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptWasmReloadSession Session;
	Session.SetHostContext(HostContext);

	FAvidScriptWasmReloadResult ReloadResult;
	const bool bInitialLoaded = Session.LoadInitialModule(
		BytecodeV1.GetData(),
		BytecodeV1.Num(),
		ManifestV1,
		ReloadResult);
	if (!bInitialLoaded)
	{
		AddError(ReloadResult.ErrorMessage);
		DestroyReloadDGuestWorld(World);
		return true;
	}
	TestTrue(TEXT("D reload v1 initial module loads"), bInitialLoaded);
	TestEqual(TEXT("Actor moved by D reload v1"), Actor->GetActorLocation(), FVector(123.0, 456.0, 789.0));

	const bool bReloadedV2 = Session.ReloadModule(
		BytecodeV2.GetData(),
		BytecodeV2.Num(),
		ManifestV2,
		ReloadResult);
	if (!bReloadedV2)
	{
		AddError(ReloadResult.ErrorMessage);
		DestroyReloadDGuestWorld(World);
		return true;
	}
	TestTrue(TEXT("D reload v2 applies"), bReloadedV2);
	TestEqual(TEXT("Successful D reload count"), Session.GetSuccessfulReloadCount(), 1);
	TestEqual(TEXT("Actor moved by D reload v2"), Actor->GetActorLocation(), FVector(321.0, 654.0, 987.0));

	const FString CorruptManifestPath = FPaths::Combine(GetReloadManifestTestRoot(), TEXT("d_guest_reload_bad_hash.avidscript.json"));
	TestTrue(
		TEXT("Corrupt D reload manifest writes"),
		WriteReloadDGuestCorruptHashManifest(ManifestV2Path, ManifestV2.WasmSha256, CorruptManifestPath));

	FAvidScriptWasmReloadManifestLoadResult BadLoadResult;
	FAvidScriptWasmReloadManifest BadManifest;
	TArray<uint8> BadBytecode;
	TestFalse(
		TEXT("Corrupt D reload manifest rejects before staging"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(CorruptManifestPath, BadManifest, BadBytecode, BadLoadResult));
	TestEqual(TEXT("Corrupt D reload category"), BadLoadResult.ErrorCategory, FString(TEXT("module_hash_mismatch")));
	TestEqual(TEXT("Live D reload module remains v2 after bad manifest"), Session.GetLiveModuleId(), ManifestV2.ModuleId);
	TestEqual(TEXT("Actor remains at D reload v2 location"), Actor->GetActorLocation(), FVector(321.0, 654.0, 987.0));

	IFileManager::Get().Delete(*CorruptManifestPath);
	DestroyReloadDGuestWorld(World);
	return true;
}
#endif
