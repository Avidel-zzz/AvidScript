#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmReload.h"

#include "AvidScriptRuntimeBackendTestLanes.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "AvidScriptVmArtifact.h"

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

const uint8 GAvidScriptReloadManifestWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x0d, 0x03, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x02,
	0x1b, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64, 0x73,
	0x63, 0x72, 0x69, 0x70, 0x74, 0x0c, 0x68, 0x6f,
	0x73, 0x74, 0x5f, 0x61, 0x64, 0x64, 0x5f, 0x69,
	0x33, 0x32, 0x00, 0x02, 0x03, 0x03, 0x02, 0x00,
	0x01, 0x07, 0x25, 0x02, 0x12, 0x61, 0x76, 0x69,
	0x64, 0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67,
	0x69, 0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00,
	0x01, 0x0c, 0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f,
	0x6e, 0x5f, 0x74, 0x69, 0x63, 0x6b, 0x00, 0x02,
	0x0a, 0x07, 0x02, 0x02, 0x00, 0x0b, 0x02, 0x00,
	0x0b
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
	const FString& WasmSha256,
	const FString& StateMigrationJson = FString(),
	const FString& RequiredImportsJson =
		TEXT("[{ \"module\": \"avidscript\", \"name\": \"host_add_i32\" }]"),
	const FString& ExecutionJson = FString())
{
	const FString StateMigrationField = StateMigrationJson.IsEmpty()
		? FString()
		: FString::Printf(TEXT("  \"state_migration\": %s,\n"), *StateMigrationJson);
	const FString ExecutionField = ExecutionJson.IsEmpty()
		? FString()
		: FString::Printf(TEXT("  \"execution\": %s,\n"), *ExecutionJson);
	const FString ManifestJson = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"module_id\": \"%s\",\n")
		TEXT("  \"abi_version\": 1,\n")
		TEXT("  \"language\": \"d\",\n")
		TEXT("  \"source\": { \"file\": \"Generated/manifest_smoke.d\" },\n")
		TEXT("%s")
		TEXT("  \"wasm\": { \"file\": \"%s\", \"sha256\": \"%s\" },\n")
		TEXT("%s")
		TEXT("  \"required_exports\": [\"avid_on_begin_play\", \"avid_on_tick\"],\n")
		TEXT("  \"required_imports\": %s,\n")
		TEXT("  \"toolchain\": { \"compiler\": \"ldc2\", \"version\": \"1.42.0\", \"target\": \"wasm32-unknown-unknown-wasm\", \"linker\": \"ldc2-internal-lld\" }\n")
		TEXT("}\n"),
		*ModuleId,
		*StateMigrationField,
		*ProjectRelativeJsonPathForReloadManifestTest(WasmPath),
		*WasmSha256,
		*ExecutionField,
		*RequiredImportsJson);

	return FFileHelper::SaveStringToFile(ManifestJson, *ManifestPath);
}

FString MakeReloadExecutionJson(
	const FAvidScriptVmOwnedArtifact& Artifact,
	const FString& FileName,
	const FString& Policy,
	const FString& ExecutionSha256 = FString(),
	const FString& TargetTriple = FString(),
	const FString& AttestationId = FString(),
	const FString& CompilerBuildIdentity = FString())
{
	return FString::Printf(
		TEXT("{\"format\":\"wasmtime_serialized_v1\",\"file\":\"%s\",")
		TEXT("\"sha256\":\"%s\",\"canonical_sha256\":\"%s\",")
		TEXT("\"compiler_build_identity\":\"%s\",\"target_triple\":\"%s\",")
		TEXT("\"attestation_id\":\"%s\",\"policy\":\"%s\",\"fallback\":\"wasmtime_jit\"}"),
		*FileName,
		ExecutionSha256.IsEmpty()
			? *Artifact.ExecutionIdentity
			: *ExecutionSha256,
		*Artifact.CanonicalWasmIdentity,
		CompilerBuildIdentity.IsEmpty()
			? *Artifact.CompilerBuildIdentity
			: *CompilerBuildIdentity,
		TargetTriple.IsEmpty() ? *Artifact.TargetTriple : *TargetTriple,
		AttestationId.IsEmpty() ? *Artifact.AttestationId : *AttestationId,
		*Policy);
}

bool LoadReloadDynamicImportFixture(TArray<uint8>& OutBytecode)
{
	const FString FixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Tests/Fixtures/WasmBackend/P42_3_DynamicRawImport.wasm")));
	return FFileHelper::LoadFileToArray(OutBytecode, *FixturePath);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadStateMigrationManifestContractTest,
	"AvidScript.Reload.StateMigrationManifestContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadStateMigrationManifestContractTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = GetReloadManifestTestRoot();
	IFileManager::Get().MakeDirectory(*TestRoot, true);
	const FString WasmPath = FPaths::Combine(TestRoot, TEXT("state_migration_manifest.wasm"));
	TArray<uint8> WasmBytes;
	WasmBytes.Append(GAvidScriptReloadManifestWasmModule, UE_ARRAY_COUNT(GAvidScriptReloadManifestWasmModule));
	TestTrue(TEXT("State migration WASM fixture writes"), FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath));
	const FString WasmSha256 = ComputeReloadTestSha256Hex(WasmBytes);
	const FString FingerprintA(TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	const FString FingerprintB(TEXT("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
	const FString ValidSchema = FString::Printf(
		TEXT("{\"schema_version\":1,\"strategy\":\"host_snapshot\",\"owner_type_id\":\"type:Game.Script\",")
		TEXT("\"slots\":[")
		TEXT("{\"stable_id\":\"global:score\",\"type_fingerprint\":\"%s\",\"offset\":16,\"size\":4,\"alignment\":4},")
		TEXT("{\"stable_id\":\"global:timer\",\"type_fingerprint\":\"%s\",\"offset\":20,\"size\":4,\"alignment\":4}]}"),
		*FingerprintA,
		*FingerprintB);
	const FString ValidManifestPath = FPaths::Combine(TestRoot, TEXT("state_migration_valid.avidscript.json"));
	TestTrue(TEXT("Valid migration manifest writes"), WriteReloadManifestFixture(
		ValidManifestPath,
		TEXT("state_migration_contract"),
		WasmPath,
		WasmSha256,
		ValidSchema));

	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> LoadedBytecode;
	const bool bValidManifestLoaded = FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		ValidManifestPath,
		Manifest,
		LoadedBytecode,
		LoadResult);
	if (!TestTrue(TEXT("Valid migration manifest loads"), bValidManifestLoaded))
	{
		return true;
	}
	TestTrue(TEXT("Host snapshot strategy is enabled"), Manifest.StateMigration.IsEnabled());
	TestEqual(TEXT("V1 schema version defaults to one"), Manifest.StateMigration.SchemaVersion, 1);
	TestEqual(TEXT("V1 policy defaults to compatible"), Manifest.StateMigration.Policy, FString(TEXT("compatible")));
	TestEqual(TEXT("V1 contract version defaults to one"), Manifest.StateMigration.ContractVersion, 1);
	TestEqual(TEXT("Migration owner type"), Manifest.StateMigration.OwnerTypeId, FString(TEXT("type:Game.Script")));
	TestEqual(TEXT("Migration slot count"), Manifest.StateMigration.Slots.Num(), 2);
	if (Manifest.StateMigration.Slots.Num() != 2)
	{
		return true;
	}
	TestEqual(TEXT("Migration first offset"), Manifest.StateMigration.Slots[0].Offset, 16u);
	TestEqual(TEXT("V1 slots have no aliases"), Manifest.StateMigration.Slots[0].Aliases.Num(), 0);

	const FString ValidV2Schema = FString::Printf(
		TEXT("{\"schema_version\":2,\"strategy\":\"host_snapshot\",\"policy\":\"explicit\",\"contract_version\":2,\"owner_type_id\":\"type:Game.Script\",")
		TEXT("\"slots\":[")
		TEXT("{\"stable_id\":\"global:score\",\"aliases\":[\"global:old_score\"],\"type_fingerprint\":\"%s\",\"offset\":16,\"size\":4,\"alignment\":4},")
		TEXT("{\"stable_id\":\"global:timer\",\"aliases\":[\"global:old_timer\"],\"type_fingerprint\":\"%s\",\"offset\":20,\"size\":4,\"alignment\":4}]}"),
		*FingerprintA,
		*FingerprintB);
	const FString ValidV2ManifestPath = FPaths::Combine(TestRoot, TEXT("state_migration_v2_valid.avidscript.json"));
	TestTrue(TEXT("Valid v2 migration manifest writes"), WriteReloadManifestFixture(
		ValidV2ManifestPath,
		TEXT("state_migration_contract"),
		WasmPath,
		WasmSha256,
		ValidV2Schema));
	const bool bValidV2ManifestLoaded = FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		ValidV2ManifestPath,
		Manifest,
		LoadedBytecode,
		LoadResult);
	if (!TestTrue(TEXT("Valid v2 migration manifest loads"), bValidV2ManifestLoaded))
	{
		return true;
	}
	TestEqual(TEXT("V2 schema version is retained"), Manifest.StateMigration.SchemaVersion, 2);
	TestEqual(TEXT("V2 policy is retained"), Manifest.StateMigration.Policy, FString(TEXT("explicit")));
	TestEqual(TEXT("V2 contract version is retained"), Manifest.StateMigration.ContractVersion, 2);
	TestEqual(TEXT("V2 migration slot count"), Manifest.StateMigration.Slots.Num(), 2);
	if (Manifest.StateMigration.Slots.Num() != 2)
	{
		return true;
	}
	TestEqual(TEXT("V2 first slot alias count"), Manifest.StateMigration.Slots[0].Aliases.Num(), 1);
	if (Manifest.StateMigration.Slots[0].Aliases.Num() != 1)
	{
		return true;
	}
	TestEqual(TEXT("V2 alias is retained"), Manifest.StateMigration.Slots[0].Aliases[0], FString(TEXT("global:old_score")));

	const FString UnsortedAliasSchema = ValidV2Schema.Replace(
		TEXT("[\"global:old_score\"]"),
		TEXT("[\"global:z_score\",\"global:a_score\"]"));
	const FString UnsortedAliasManifestPath = FPaths::Combine(TestRoot, TEXT("state_migration_v2_unsorted_alias.avidscript.json"));
	TestTrue(TEXT("Unsorted alias manifest writes"), WriteReloadManifestFixture(
		UnsortedAliasManifestPath,
		TEXT("state_migration_contract"),
		WasmPath,
		WasmSha256,
		UnsortedAliasSchema));
	TestFalse(TEXT("V2 unsorted aliases are rejected"), FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		UnsortedAliasManifestPath,
		Manifest,
		LoadedBytecode,
		LoadResult));
	TestEqual(TEXT("Unsorted alias category"), LoadResult.ErrorCategory, FString(TEXT("manifest_invalid")));

	const FString DuplicateAliasSchema = ValidV2Schema.Replace(
		TEXT("[\"global:old_timer\"]"),
		TEXT("[\"global:old_score\"]"));
	const FString DuplicateAliasManifestPath = FPaths::Combine(TestRoot, TEXT("state_migration_v2_duplicate_alias.avidscript.json"));
	TestTrue(TEXT("Duplicate alias manifest writes"), WriteReloadManifestFixture(
		DuplicateAliasManifestPath,
		TEXT("state_migration_contract"),
		WasmPath,
		WasmSha256,
		DuplicateAliasSchema));
	TestFalse(TEXT("V2 duplicate aliases are rejected"), FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		DuplicateAliasManifestPath,
		Manifest,
		LoadedBytecode,
		LoadResult));
	TestEqual(TEXT("Duplicate alias category"), LoadResult.ErrorCategory, FString(TEXT("manifest_invalid")));

	const FString CurrentIdConflictSchema = ValidV2Schema.Replace(
		TEXT("[\"global:old_timer\"]"),
		TEXT("[\"global:score\"]"));
	const FString CurrentIdConflictManifestPath = FPaths::Combine(TestRoot, TEXT("state_migration_v2_current_id_conflict.avidscript.json"));
	TestTrue(TEXT("Current id conflict manifest writes"), WriteReloadManifestFixture(
		CurrentIdConflictManifestPath,
		TEXT("state_migration_contract"),
		WasmPath,
		WasmSha256,
		CurrentIdConflictSchema));
	TestFalse(TEXT("V2 alias conflicts with current id are rejected"), FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		CurrentIdConflictManifestPath,
		Manifest,
		LoadedBytecode,
		LoadResult));
	TestEqual(TEXT("Current id conflict category"), LoadResult.ErrorCategory, FString(TEXT("manifest_invalid")));

	const FString InvalidPolicySchema = ValidV2Schema.Replace(TEXT("\"explicit\""), TEXT("\"invalid\""));
	const FString InvalidPolicyManifestPath = FPaths::Combine(TestRoot, TEXT("state_migration_v2_invalid_policy.avidscript.json"));
	TestTrue(TEXT("Invalid policy manifest writes"), WriteReloadManifestFixture(
		InvalidPolicyManifestPath,
		TEXT("state_migration_contract"),
		WasmPath,
		WasmSha256,
		InvalidPolicySchema));
	TestFalse(TEXT("V2 invalid policy is rejected"), FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		InvalidPolicyManifestPath,
		Manifest,
		LoadedBytecode,
		LoadResult));
	TestEqual(TEXT("Invalid policy category"), LoadResult.ErrorCategory, FString(TEXT("manifest_invalid")));

	const FString DuplicateSchema = ValidSchema.Replace(TEXT("global:timer"), TEXT("global:score"));
	const FString DuplicateManifestPath = FPaths::Combine(TestRoot, TEXT("state_migration_duplicate.avidscript.json"));
	TestTrue(TEXT("Duplicate migration manifest writes"), WriteReloadManifestFixture(
		DuplicateManifestPath,
		TEXT("state_migration_contract"),
		WasmPath,
		WasmSha256,
		DuplicateSchema));
	TestFalse(TEXT("Duplicate migration stable id is rejected"), FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		DuplicateManifestPath,
		Manifest,
		LoadedBytecode,
		LoadResult));
	TestEqual(TEXT("Duplicate migration category"), LoadResult.ErrorCategory, FString(TEXT("manifest_invalid")));

	const FString OverlapSchema = ValidSchema.Replace(
		TEXT("\"offset\":16,\"size\":4"),
		TEXT("\"offset\":16,\"size\":8"));
	const FString OverlapManifestPath = FPaths::Combine(TestRoot, TEXT("state_migration_overlap.avidscript.json"));
	TestTrue(TEXT("Overlap migration manifest writes"), WriteReloadManifestFixture(
		OverlapManifestPath,
		TEXT("state_migration_contract"),
		WasmPath,
		WasmSha256,
		OverlapSchema));
	TestFalse(TEXT("Overlapping migration slots are rejected"), FAvidScriptWasmReloadManifestLoader::LoadFromFile(
		OverlapManifestPath,
		Manifest,
		LoadedBytecode,
		LoadResult));
	TestEqual(TEXT("Overlap migration category"), LoadResult.ErrorCategory, FString(TEXT("manifest_invalid")));
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
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmReloadSession Session;
		Session.SetBackendSelectionForTesting(Lane.Selection);
		FAvidScriptWasmReloadResult Result;
		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("initial module loads")),
			Session.LoadInitialModule(
				GAvidScriptReloadCompatibleWasmModule,
				UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
				FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v1")),
				Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("initial live module id")), Session.GetLiveModuleId(), FString(TEXT("reload_v1")));

		FAvidScriptWasmSmokeResult TickResult;
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("initial live runtime ticks")), Session.TickLive(1.0f / 60.0f, TickResult));
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, TickResult);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("initial live tick count")), Session.GetLiveTickCallCount(), 1);
		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("compatible reload applies")),
			Session.ReloadModule(
				GAvidScriptReloadCompatibleWasmModule,
				UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
				FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v2")),
				Result));
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("reload result reports applied")), Result.bReloadApplied);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("reload previous module")), Result.PreviousModuleId, FString(TEXT("reload_v1")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("reload active module")), Result.ActiveModuleId, FString(TEXT("reload_v2")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("live module id switches")), Session.GetLiveModuleId(), FString(TEXT("reload_v2")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("successful reload count")), Session.GetSuccessfulReloadCount(), 1);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("reloaded live runtime ticks")), Session.TickLive(1.0f / 60.0f, TickResult));
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, TickResult);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("reloaded tick count starts fresh")), Session.GetLiveTickCallCount(), 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadMissingExportRollbackSmokeTest,
	"AvidScript.Reload.MissingExportRollbackSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadMissingExportRollbackSmokeTest::RunTest(const FString& Parameters)
{
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmReloadSession Session;
		Session.SetBackendSelectionForTesting(Lane.Selection);
		FAvidScriptWasmReloadResult Result;
		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("rollback seed module loads")),
			Session.LoadInitialModule(
				GAvidScriptReloadCompatibleWasmModule,
				UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
				FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v1")),
				Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}

		FAvidScriptWasmSmokeResult TickResult;
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("rollback seed runtime ticks")), Session.TickLive(1.0f / 60.0f, TickResult));
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, TickResult);
		TestFalse(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing-export reload is rejected")),
			Session.ReloadModule(
				GAvidScriptReloadMissingTickWasmModule,
				UE_ARRAY_COUNT(GAvidScriptReloadMissingTickWasmModule),
				FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_missing_tick")),
				Result));
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("rollback preserves live runtime")), Result.bRollbackPreservedLiveRuntime);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing export category")), Result.ErrorCategory, FString(TEXT("missing_export")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing export name")), Result.ExportName, FString(TEXT("avid_on_tick")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("live module remains previous")), Session.GetLiveModuleId(), FString(TEXT("reload_v1")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("rejected reload count")), Session.GetRejectedReloadCount(), 1);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("previous live runtime still ticks")), Session.TickLive(1.0f / 60.0f, TickResult));
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, TickResult);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("previous live tick count continues")), Session.GetLiveTickCallCount(), 2);
	}

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
	FAvidScriptReloadGeneratedImportsRequirePackageTest,
	"AvidScript.Reload.GeneratedImportsRequirePackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadGeneratedImportsRequirePackageTest::RunTest(const FString& Parameters)
{
	TArray<uint8> DynamicImportBytecode;
	if (!TestTrue(TEXT("Generated import WASM fixture loads"), LoadReloadDynamicImportFixture(DynamicImportBytecode)))
	{
		return false;
	}

	FAvidScriptWasmReloadManifest Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(
		TEXT("reload_generated_import_without_package"));
	Manifest.RequiredExports = { TEXT("avid_on_begin_play") };
	Manifest.RequiredImports = {
		FAvidScriptWasmRequiredImport{ TEXT("avidscript"), TEXT("avid_ue_1111111111111111") }
	};

	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;
	TestFalse(
		TEXT("Generated UE imports fail closed without a binding package"),
		Session.LoadInitialModule(
			DynamicImportBytecode.GetData(),
			DynamicImportBytecode.Num(),
			Manifest,
			Result));
	TestEqual(
		TEXT("Missing generated binding package has a stable category"),
		Result.ErrorCategory,
		FString(TEXT("binding_package_missing")));
	TestFalse(TEXT("Rejected generated import module is not activated"), Session.IsLiveLoaded());

	FAvidScriptWasmReloadManifest PackedOwnerManifest = FAvidScriptWasmReloadManifest::MakeSmoke(
		TEXT("reload_packed_owner_without_package"));
	PackedOwnerManifest.RequiredImports = {
		FAvidScriptWasmRequiredImport{ TEXT("avidscript"), TEXT("avid_owner_get_handle") }
	};
	FAvidScriptWasmReloadSession PackedOwnerSession;
	TestFalse(
		TEXT("Manifest-only packed owner claim is rejected before package authorization"),
		PackedOwnerSession.LoadInitialModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			PackedOwnerManifest,
			Result));
	TestEqual(
		TEXT("Manifest-only packed owner claim has a stable category"),
		Result.ErrorCategory,
		FString(TEXT("manifest_wasm_import_mismatch")));
	TestFalse(TEXT("Rejected packed owner module is not activated"), PackedOwnerSession.IsLiveLoaded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadDirectSessionRejectsInvalidBytecodeTest,
	"AvidScript.Reload.DirectSessionRejectsInvalidBytecode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadDirectSessionRejectsInvalidBytecodeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;
	TestFalse(
		TEXT("Direct Session rejects empty bytecode before WASM inspection"),
		Session.LoadInitialModule(
			nullptr,
			0,
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_invalid_bytecode")),
			Result));
	TestEqual(
		TEXT("Invalid bytecode has the established stable category"),
		Result.ErrorCategory,
		FString(TEXT("invalid_bytecode")));
	TestFalse(TEXT("Invalid bytecode never activates a runtime"), Session.IsLiveLoaded());

	TestFalse(
		TEXT("Direct Session rejects a null pointer with a positive byte count before WASM inspection"),
		Session.LoadInitialModule(
			nullptr,
			8,
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_null_bytecode")),
			Result));
	TestEqual(TEXT("Null bytecode has a stable category"), Result.ErrorCategory, FString(TEXT("invalid_bytecode")));

	const uint8 PlaceholderByte = 0;
	TestFalse(
		TEXT("Direct Session rejects a negative byte count before constructing an array view"),
		Session.LoadInitialModule(
			&PlaceholderByte,
			-1,
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_negative_bytecode")),
			Result));
	TestEqual(TEXT("Negative byte count has a stable category"), Result.ErrorCategory, FString(TEXT("invalid_bytecode")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadReentrantMutationRejectedTest,
	"AvidScript.Reload.ReentrantMutationRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadReentrantMutationRejectedTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;
	if (!TestTrue(
		TEXT("Initial module loads"),
		Session.LoadInitialModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_reentrant_v1")),
			Result)))
	{
		return false;
	}

	bool bNestedReloadSucceeded = true;
	FAvidScriptWasmReloadResult NestedResult;
	Session.SetCandidateBeginPlayObserverForTesting([&]()
	{
		bNestedReloadSucceeded = Session.ReloadModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_reentrant_nested")),
			NestedResult);
	});

	TestTrue(
		TEXT("Outer reload remains valid"),
		Session.ReloadModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_reentrant_v2")),
			Result));
	TestFalse(TEXT("Nested reload is rejected while a runtime mutation is active"), bNestedReloadSucceeded);
	TestEqual(
		TEXT("Nested reload has a stable category"),
		NestedResult.ErrorCategory,
		FString(TEXT("reentrant_operation")));
	TestEqual(TEXT("Outer candidate remains active"), Session.GetLiveModuleId(), FString(TEXT("reload_reentrant_v2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadLiveExecutionRejectsReloadTest,
	"AvidScript.Reload.LiveExecutionRejectsReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadLiveExecutionRejectsReloadTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;
	if (!TestTrue(
		TEXT("Initial module loads"),
		Session.LoadInitialModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_live_guard_v1")),
			Result)))
	{
		return false;
	}

	bool bNestedReloadSucceeded = true;
	FAvidScriptWasmReloadResult NestedResult;
	Session.SetLiveExecutionObserverForTesting([&]()
	{
		bNestedReloadSucceeded = Session.ReloadModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_live_guard_nested")),
			NestedResult);
	});

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Outer guest call completes"), Session.TickLive(1.0f / 60.0f, TickResult));
	TestFalse(TEXT("Reload requested during guest execution is rejected"), bNestedReloadSucceeded);
	TestEqual(
		TEXT("Live-execution reload rejection has a stable category"),
		NestedResult.ErrorCategory,
		FString(TEXT("reentrant_operation")));
	TestEqual(TEXT("Original runtime remains active"), Session.GetLiveModuleId(), FString(TEXT("reload_live_guard_v1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadDirectSessionRejectsManifestImportMismatchTest,
	"AvidScript.Reload.DirectSessionRejectsManifestImportMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadDirectSessionRejectsManifestImportMismatchTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadManifest Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(
		TEXT("reload_direct_import_mismatch"));
	Manifest.RequiredImports = {
		FAvidScriptWasmRequiredImport{ TEXT("avidscript"), TEXT("host_add_i32") }
	};

	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;
	TestFalse(
		TEXT("Direct Session API rejects a manifest that misrepresents the actual WASM import table"),
		Session.LoadInitialModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			Manifest,
			Result));
	TestEqual(
		TEXT("Direct Session mismatch has a stable category"),
		Result.ErrorCategory,
		FString(TEXT("manifest_wasm_import_mismatch")));
	TestFalse(TEXT("Direct Session mismatch never activates a runtime"), Session.IsLiveLoaded());
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
	WasmBytes.Append(GAvidScriptReloadManifestWasmModule, UE_ARRAY_COUNT(GAvidScriptReloadManifestWasmModule));
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
	TestEqual(TEXT("Required import module"), Manifest.RequiredImports[0].ModuleName, FString(TEXT("avidscript")));
	TestEqual(TEXT("Required import name"), Manifest.RequiredImports[0].ImportName, FString(TEXT("host_add_i32")));
	TestEqual(TEXT("Loaded byte size"), LoadedBytecode.Num(), WasmBytes.Num());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeArtifactLoaderTest,
	"AvidScript.Reload.RuntimeArtifactLoader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeArtifactLoaderTest::RunTest(const FString& Parameters)
{
	FString TestRoot = FPaths::Combine(
		GetReloadManifestTestRoot(),
		TEXT("RuntimeArtifact"));
	TestRoot = NormalizeReloadTestFullPath(TestRoot);
	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	TestTrue(
		TEXT("Runtime artifact test root can be created"),
		IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString WasmPath = FPaths::Combine(
		TestRoot,
		TEXT("runtime_artifact.wasm"));
	const FString ManifestPath = FPaths::Combine(
		TestRoot,
		TEXT("runtime_artifact.avidscript.json"));
	const FString ArtifactFileName =
		TEXT("runtime_artifact.wasmtime.cwasm");
	const FString ArtifactPath = FPaths::Combine(
		TestRoot,
		ArtifactFileName);
	TArray<uint8> WasmBytes;
	WasmBytes.Append(
		GAvidScriptReloadCompatibleWasmModule,
		UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule));
	TestTrue(
		TEXT("Runtime artifact canonical WASM writes"),
		FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath));

	FAvidScriptVmArtifactCompileRequest CompileRequest;
	CompileRequest.Selection.BackendKind =
		EAvidScriptVmBackendKind::Wasmtime;
	CompileRequest.Selection.ExecutionMode =
		EAvidScriptVmExecutionMode::Aot;
	CompileRequest.Selection.ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	CompileRequest.CanonicalWasmBytes = WasmBytes;
	FAvidScriptVmArtifactCompileResult CompileResult;
	if (!CompileAvidScriptVmArtifact(CompileRequest, CompileResult))
	{
		if (CompileResult.Error.Category == TEXT("backend_unavailable"))
		{
			return true;
		}
		AddError(
			CompileResult.Error.Category
			+ TEXT(": ")
			+ CompileResult.Error.Details);
		return false;
	}
	TestTrue(TEXT("Runtime artifact fixture precompiles"), true);
	TestTrue(
		TEXT("Runtime artifact serialized bytes write"),
		FFileHelper::SaveArrayToFile(
			CompileResult.Artifact.ExecutionBytes,
			*ArtifactPath));

	auto WriteExecutionManifest = [this,
		&ManifestPath,
		&WasmPath,
		&WasmBytes](const FString& ExecutionJson)
	{
		return TestTrue(
			TEXT("Runtime execution manifest writes"),
			WriteReloadManifestFixture(
				ManifestPath,
				TEXT("runtime_artifact"),
				WasmPath,
				ComputeReloadTestSha256Hex(WasmBytes),
				FString(),
				TEXT("[]"),
				ExecutionJson));
	};

	const FString ValidExecutionJson = MakeReloadExecutionJson(
		CompileResult.Artifact,
		ArtifactFileName,
		TEXT("prefer_precompiled"));
	if (!WriteExecutionManifest(ValidExecutionJson))
	{
		return false;
	}
	FAvidScriptRuntimeArtifact RuntimeArtifact;
	FAvidScriptRuntimeArtifactLoadResult LoadResult;
	TestTrue(
		TEXT("Authorized runtime artifact loads"),
		FAvidScriptRuntimeArtifactLoader::LoadFromFile(
			ManifestPath,
			RuntimeArtifact,
			LoadResult));
	TestTrue(
		TEXT("Authorized runtime artifact selects precompiled execution"),
		LoadResult.bUsesPrecompiledArtifact);
	TestEqual(
		TEXT("Authorized runtime artifact selects Wasmtime serialized"),
		RuntimeArtifact.BackendSelection.ArtifactFormat,
		EAvidScriptVmArtifactFormat::WasmtimeSerialized);
	const FString MissingAttestationJson = MakeReloadExecutionJson(
		CompileResult.Artifact,
		ArtifactFileName,
		TEXT("prefer_precompiled"),
		FString(),
		FString(),
		TEXT("00000000000000000000000000000000"));
	WriteExecutionManifest(MissingAttestationJson);
	TestTrue(
		TEXT("PreferPrecompiled falls back when attestation expires"),
		FAvidScriptRuntimeArtifactLoader::LoadFromFile(
			ManifestPath,
			RuntimeArtifact,
			LoadResult));
	TestTrue(TEXT("Expired attestation records JIT fallback"), LoadResult.bFellBackToJit);
	TestEqual(
		TEXT("Expired attestation selects Wasmtime JIT"),
		RuntimeArtifact.BackendSelection.ExecutionMode,
		EAvidScriptVmExecutionMode::Jit);

	const FString DigestMismatchJson = MakeReloadExecutionJson(
		CompileResult.Artifact,
		ArtifactFileName,
		TEXT("prefer_precompiled"),
		TEXT("0000000000000000000000000000000000000000000000000000000000000000"));
	WriteExecutionManifest(DigestMismatchJson);
	TestTrue(
		TEXT("PreferPrecompiled falls back on cwasm digest mismatch"),
		FAvidScriptRuntimeArtifactLoader::LoadFromFile(
			ManifestPath,
			RuntimeArtifact,
			LoadResult));
	TestEqual(
		TEXT("Digest mismatch has a stable fallback category"),
		LoadResult.FallbackCategory,
		FString(TEXT("execution_identity_mismatch")));

	const FString CompilerMismatchJson = MakeReloadExecutionJson(
		CompileResult.Artifact,
		ArtifactFileName,
		TEXT("prefer_precompiled"),
		FString(),
		FString(),
		FString(),
		TEXT("foreign-compiler-build"));
	WriteExecutionManifest(CompilerMismatchJson);
	TestTrue(
		TEXT("PreferPrecompiled falls back on compiler mismatch"),
		FAvidScriptRuntimeArtifactLoader::LoadFromFile(
			ManifestPath,
			RuntimeArtifact,
			LoadResult));
	TestEqual(
		TEXT("Compiler mismatch is rejected before deserialize"),
		LoadResult.FallbackCategory,
		FString(TEXT("execution_attestation_invalid")));

	const FString TargetMismatchJson = MakeReloadExecutionJson(
		CompileResult.Artifact,
		ArtifactFileName,
		TEXT("prefer_precompiled"),
		FString(),
		TEXT("foreign-target"));
	WriteExecutionManifest(TargetMismatchJson);
	TestTrue(
		TEXT("PreferPrecompiled falls back on target mismatch"),
		FAvidScriptRuntimeArtifactLoader::LoadFromFile(
			ManifestPath,
			RuntimeArtifact,
			LoadResult));
	TestEqual(
		TEXT("Target mismatch has a stable fallback category"),
		LoadResult.FallbackCategory,
		FString(TEXT("execution_target_mismatch")));

	const FString RequireMissingAttestationJson = MakeReloadExecutionJson(
		CompileResult.Artifact,
		ArtifactFileName,
		TEXT("require_precompiled"),
		FString(),
		FString(),
		TEXT("00000000000000000000000000000000"));
	WriteExecutionManifest(RequireMissingAttestationJson);
	TestFalse(
		TEXT("RequirePrecompiled rejects an expired attestation"),
		FAvidScriptRuntimeArtifactLoader::LoadFromFile(
			ManifestPath,
			RuntimeArtifact,
			LoadResult));
	TestEqual(
		TEXT("RequirePrecompiled attestation rejection is stable"),
		LoadResult.CanonicalResult.ErrorCategory,
		FString(TEXT("execution_attestation_invalid")));

	const TArray<uint8> MalformedCanonical = {
		0x00, 0x61, 0x73, 0x6d
	};
	TestTrue(
		TEXT("Malformed canonical fixture writes"),
		FFileHelper::SaveArrayToFile(MalformedCanonical, *WasmPath));
	TestTrue(
		TEXT("Malformed canonical manifest writes"),
		WriteReloadManifestFixture(
			ManifestPath,
			TEXT("runtime_artifact_malformed"),
			WasmPath,
			ComputeReloadTestSha256Hex(MalformedCanonical),
			FString(),
			TEXT("[]"),
			ValidExecutionJson));
	TestFalse(
		TEXT("Canonical validation rejects before serialized selection"),
		FAvidScriptRuntimeArtifactLoader::LoadFromFile(
			ManifestPath,
			RuntimeArtifact,
			LoadResult));
	TestEqual(
		TEXT("Canonical layout failure remains authoritative"),
		LoadResult.CanonicalResult.ErrorCategory,
		FString(TEXT("wasm_layout_invalid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadManifestAllowsZeroImportsTest,
	"AvidScript.Reload.ManifestAllowsZeroImports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadManifestAllowsZeroImportsTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = GetReloadManifestTestRoot();
	IFileManager::Get().MakeDirectory(*TestRoot, true);

	const FString WasmPath = FPaths::Combine(TestRoot, TEXT("manifest_zero_imports.wasm"));
	const FString ManifestPath = FPaths::Combine(TestRoot, TEXT("manifest_zero_imports.avidscript.json"));
	TArray<uint8> WasmBytes;
	WasmBytes.Append(GAvidScriptReloadCompatibleWasmModule, UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule));
	TestTrue(TEXT("Zero-import WASM fixture writes"), FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath));
	TestTrue(
		TEXT("Zero-import manifest fixture writes"),
		WriteReloadManifestFixture(
			ManifestPath,
			TEXT("manifest_zero_imports"),
			WasmPath,
			ComputeReloadTestSha256Hex(WasmBytes),
			FString(),
			TEXT("[]")));

	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> LoadedBytecode;
	TestTrue(
		TEXT("A file manifest may declare an empty import array when WASM imports no functions"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			ManifestPath,
			Manifest,
			LoadedBytecode,
			LoadResult));
	TestEqual(TEXT("Zero-import manifest remains empty"), Manifest.RequiredImports.Num(), 0);
	TestEqual(TEXT("Zero-import bytecode is preserved"), LoadedBytecode.Num(), WasmBytes.Num());
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
	WasmBytes.Append(GAvidScriptReloadManifestWasmModule, UE_ARRAY_COUNT(GAvidScriptReloadManifestWasmModule));
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
