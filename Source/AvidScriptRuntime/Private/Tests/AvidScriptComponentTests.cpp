#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptComponent.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "AvidScriptVmArtifact.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Ssl.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
const uint8 GComponentReloadCompatibleModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x0d, 0x03, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x02,
	0x1b, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64, 0x73,
	0x63, 0x72, 0x69, 0x70, 0x74, 0x0c, 0x68, 0x6f,
	0x73, 0x74, 0x5f, 0x61, 0x64, 0x64, 0x5f, 0x69,
	0x33, 0x32, 0x00, 0x02, 0x03, 0x03, 0x02, 0x00,
	0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x01, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x02, 0x0a, 0x07,
	0x02, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

const uint8 GComponentReloadBeginPlayTrapModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x0d, 0x03, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x02,
	0x1b, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64, 0x73,
	0x63, 0x72, 0x69, 0x70, 0x74, 0x0c, 0x68, 0x6f,
	0x73, 0x74, 0x5f, 0x61, 0x64, 0x64, 0x5f, 0x69,
	0x33, 0x32, 0x00, 0x02, 0x03, 0x03, 0x02, 0x00,
	0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x01, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x02, 0x0a, 0x08,
	0x02, 0x03, 0x00, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

FString ComponentReloadBytesToLowerHex(const uint8* Bytes, int32 ByteCount)
{
	FString Hex;
	Hex.Reserve(ByteCount * 2);
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Hex += FString::Printf(TEXT("%02x"), Bytes[Index]);
	}
	return Hex;
}

FString ComputeComponentReloadSha256(const TArray<uint8>& Bytes)
{
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	SHA256(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Digest);
	return ComponentReloadBytesToLowerHex(Digest, UE_ARRAY_COUNT(Digest));
}

bool WriteComponentReloadFixture(
	const FString& Root,
	const FString& ModuleId,
	const uint8* Bytecode,
	const int32 BytecodeSize,
	FString& OutManifestPath,
	const FString& ExecutionJson = FString())
{
	if (!IFileManager::Get().MakeDirectory(*Root, true))
	{
		return false;
	}

	const TArray<uint8> WasmBytes(Bytecode, BytecodeSize);
	const FString WasmFileName = ModuleId + TEXT(".wasm");
	const FString WasmPath = FPaths::Combine(Root, WasmFileName);
	OutManifestPath = FPaths::Combine(Root, ModuleId + TEXT(".avidscript.json"));
	if (!FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath))
	{
		return false;
	}

	const FString ExecutionField = ExecutionJson.IsEmpty()
		? FString()
		: FString::Printf(TEXT("  \"execution\": %s,\n"), *ExecutionJson);
	const FString ManifestJson = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"module_id\": \"%s\",\n")
		TEXT("  \"abi_version\": 1,\n")
		TEXT("  \"language\": \"wasm\",\n")
		TEXT("  \"wasm\": { \"file\": \"%s\", \"sha256\": \"%s\" },\n")
		TEXT("%s")
		TEXT("  \"required_exports\": [\"avid_on_begin_play\", \"avid_on_tick\"],\n")
		TEXT("  \"required_imports\": [{ \"module\": \"avidscript\", \"name\": \"host_add_i32\" }]\n")
		TEXT("}\n"),
		*ModuleId,
		*WasmFileName,
		*ComputeComponentReloadSha256(WasmBytes),
		*ExecutionField);
	return FFileHelper::SaveStringToFile(ManifestJson, *OutManifestPath);
}

bool BuildComponentPrecompiledExecutionFixture(
	const FString& Root,
	const FString& ModuleId,
	TConstArrayView<uint8> CanonicalWasm,
	FString& OutExecutionJson)
{
	OutExecutionJson.Reset();
	if (!IFileManager::Get().MakeDirectory(*Root, true))
	{
		return false;
	}
	FAvidScriptVmArtifactCompileRequest Request;
	Request.Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Request.Selection.ExecutionMode = EAvidScriptVmExecutionMode::Aot;
	Request.Selection.ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	Request.CanonicalWasmBytes = CanonicalWasm;
	FAvidScriptVmArtifactCompileResult Result;
	if (!CompileAvidScriptVmArtifact(Request, Result))
	{
		return Result.Error.Category == TEXT("backend_unavailable");
	}
	const FString ArtifactFileName =
		ModuleId + TEXT(".wasmtime.cwasm");
	if (!FFileHelper::SaveArrayToFile(
			Result.Artifact.ExecutionBytes,
			*FPaths::Combine(Root, ArtifactFileName)))
	{
		return false;
	}
	OutExecutionJson = FString::Printf(
		TEXT("{\"format\":\"wasmtime_serialized_v1\",\"file\":\"%s\",")
		TEXT("\"sha256\":\"%s\",\"canonical_sha256\":\"%s\",")
		TEXT("\"compiler_build_identity\":\"%s\",\"target_triple\":\"%s\",")
		TEXT("\"attestation_id\":\"%s\",\"policy\":\"prefer_precompiled\",\"fallback\":\"wasmtime_jit\"}"),
		*ArtifactFileName,
		*Result.Artifact.ExecutionIdentity,
		*Result.Artifact.CanonicalWasmIdentity,
		*Result.Artifact.CompilerBuildIdentity,
		*Result.Artifact.TargetTriple,
		*Result.Artifact.AttestationId);
	return true;
}

bool BuildComposableComponentPrecompiledManifest(
	const FString& FixtureDirectory,
	const FString& ScenarioName,
	const FString& CanonicalManifestPath,
	FString& OutManifestPath)
{
	TArray<uint8> Wasm;
	FString ManifestJson;
	if (!FFileHelper::LoadFileToArray(
			Wasm,
			*FPaths::Combine(FixtureDirectory, ScenarioName + TEXT(".wasm")))
		|| !FFileHelper::LoadFileToString(ManifestJson, *CanonicalManifestPath))
	{
		return false;
	}
	FString ExecutionJson;
	if (!BuildComponentPrecompiledExecutionFixture(
			FixtureDirectory,
			ScenarioName + TEXT("-component-aot"),
			MakeArrayView(Wasm),
			ExecutionJson)
		|| ExecutionJson.IsEmpty())
	{
		return false;
	}
	TSharedPtr<FJsonObject> ManifestObject;
	TSharedPtr<FJsonObject> ExecutionObject;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestJson), ManifestObject)
		|| !ManifestObject.IsValid()
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ExecutionJson), ExecutionObject)
		|| !ExecutionObject.IsValid())
	{
		return false;
	}
	ManifestObject->SetObjectField(TEXT("execution"), ExecutionObject);
	FString PrecompiledManifestJson;
	if (!FJsonSerializer::Serialize(
			ManifestObject.ToSharedRef(),
			TJsonWriterFactory<>::Create(&PrecompiledManifestJson)))
	{
		return false;
	}
	OutManifestPath = FPaths::Combine(
		FixtureDirectory, ScenarioName + TEXT("-component-aot.avidscript.json"));
	return FFileHelper::SaveStringToFile(PrecompiledManifestJson, *OutManifestPath);
}

bool BuildComposableComponentBadHashManifest(
	const FString& CandidateManifestPath,
	const FString& OutManifestPath)
{
	FString ManifestJson;
	TSharedPtr<FJsonObject> ManifestObject;
	const TSharedPtr<FJsonObject>* WasmObject = nullptr;
	if (!FFileHelper::LoadFileToString(ManifestJson, *CandidateManifestPath)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestJson), ManifestObject)
		|| !ManifestObject.IsValid()
		|| !ManifestObject->TryGetObjectField(TEXT("wasm"), WasmObject)
		|| WasmObject == nullptr || !WasmObject->IsValid())
	{
		return false;
	}
	(*WasmObject)->SetStringField(TEXT("sha256"), FString::ChrN(64, '0'));
	FString InvalidManifestJson;
	return FJsonSerializer::Serialize(
			ManifestObject.ToSharedRef(),
			TJsonWriterFactory<>::Create(&InvalidManifestJson))
		&& FFileHelper::SaveStringToFile(InvalidManifestJson, *OutManifestPath);
}

bool BuildComposableComponentMissingExportManifest(
	const FString& CandidateManifestPath,
	const FString& OutManifestPath)
{
	FString ManifestJson;
	TSharedPtr<FJsonObject> ManifestObject;
	const TArray<TSharedPtr<FJsonValue>>* RequiredExports = nullptr;
	if (!FFileHelper::LoadFileToString(ManifestJson, *CandidateManifestPath)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestJson), ManifestObject)
		|| !ManifestObject.IsValid()
		|| !ManifestObject->TryGetArrayField(TEXT("required_exports"), RequiredExports)
		|| RequiredExports == nullptr)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> InvalidExports = *RequiredExports;
	InvalidExports.Add(MakeShared<FJsonValueString>(TEXT("avid_missing_export_for_reload_rejection")));
	ManifestObject->SetArrayField(TEXT("required_exports"), MoveTemp(InvalidExports));
	FString InvalidManifestJson;
	return FJsonSerializer::Serialize(
			ManifestObject.ToSharedRef(),
			TJsonWriterFactory<>::Create(&InvalidManifestJson))
		&& FFileHelper::SaveStringToFile(InvalidManifestJson, *OutManifestPath);
}

bool CreateComponentWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::PIE, false, TEXT("AvidScriptComponentWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::PIE);
	WorldContext.SetCurrentWorld(OutWorld);

	return true;
}

void DestroyComponentWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}

	if (World->HasBegunPlay())
	{
		World->EndPlay(EEndPlayReason::Quit);
	}

	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}

	World->DestroyWorld(false);
	World = nullptr;
}

FString GetCSharpComponentManifestPath()
{
	FString ManifestPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ActorLifecycle"),
		TEXT("actor_lifecycle.avidscript.json"));
	ManifestPath = FPaths::ConvertRelativePathToFull(ManifestPath);
	FPaths::NormalizeFilename(ManifestPath);
	return ManifestPath;
}

UAvidScriptComponent* AddAvidScriptComponent(AActor* Actor, const FString& ScriptManifestPath = FString())
{
	UAvidScriptComponent* Component = NewObject<UAvidScriptComponent>(Actor, TEXT("AvidScriptComponent"));
	if (Component != nullptr)
	{
		Actor->AddInstanceComponent(Component);
		if (!ScriptManifestPath.IsEmpty())
		{
			Component->SetScriptManifestPath(ScriptManifestPath);
		}
		Component->RegisterComponent();
	}

	return Component;
}

bool BeginComponentWorld(UWorld* World)
{
	if (World == nullptr)
	{
		return false;
	}

	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentRuntimeDiagnosticsTest,
	"AvidScript.Component.RuntimeDiagnosticsSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentRuntimeDiagnosticsTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateComponentWorld(World)) { AddError(TEXT("Diagnostics world creation failed")); return false; }
	TestTrue(TEXT("Diagnostics world begins play"), BeginComponentWorld(World));
	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	UAvidScriptComponent* Component = Actor ? AddAvidScriptComponent(Actor) : nullptr;
	if (!Component) { AddError(TEXT("Diagnostics component creation failed")); DestroyComponentWorld(World); return false; }

	FAvidScriptRuntimeSessionSnapshot Snapshot;
	FAvidScriptVmBackendInfo Backend;
	const int32 TicksBefore = Component->GetRuntimeStats().TickCallCount;
	TestTrue(TEXT("Active component exposes a copied snapshot"), Component->CaptureRuntimeDiagnostics(Snapshot, Backend));
	TestTrue(TEXT("Snapshot observes active runtime"), Snapshot.bHasActiveRuntime);
	TestFalse(TEXT("Actual backend identity is not empty"), Backend.StableBackendId.IsEmpty());
	TestEqual(TEXT("Capture does not enter Guest Tick"), Component->GetRuntimeStats().TickCallCount, TicksBefore);

	TestTrue(TEXT("Off-thread capture rejects and clears outputs"), Async(EAsyncExecution::Thread, [Component]()
	{
		FAvidScriptRuntimeSessionSnapshot OtherSnapshot;
		FAvidScriptVmBackendInfo OtherBackend;
		OtherSnapshot.bHasActiveRuntime = true;
		OtherBackend.StableBackendId = TEXT("stale");
		return !Component->CaptureRuntimeDiagnostics(OtherSnapshot, OtherBackend)
			&& !OtherSnapshot.bHasActiveRuntime && OtherBackend.StableBackendId.IsEmpty();
	}).Get());

	FAvidScriptRuntimeSession* Session = Component->GetRuntimeSessionForTesting();
	bool bReentrantCaptureRejected = false;
	if (Session)
	{
		Session->SetLiveExecutionObserverForTesting([&]()
		{
			bReentrantCaptureRejected = !Component->CaptureRuntimeDiagnostics(Snapshot, Backend)
				&& !Snapshot.bHasActiveRuntime && Backend.StableBackendId.IsEmpty();
		});
		Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
		Session->SetLiveExecutionObserverForTesting(TFunction<void()>());
	}
	TestTrue(TEXT("Active Guest capture is rejected without stale data"), bReentrantCaptureRejected);
	TestTrue(TEXT("Snapshot is available again after Guest returns"), Component->CaptureRuntimeDiagnostics(Snapshot, Backend));
	TestTrue(TEXT("Diagnostics world ends play"), World->EndPlay(EEndPlayReason::Quit));
	TestFalse(TEXT("Stopped component rejects capture"), Component->CaptureRuntimeDiagnostics(Snapshot, Backend));
	TestFalse(TEXT("Stopped capture clears runtime state"), Snapshot.bHasActiveRuntime);
	TestTrue(TEXT("Stopped capture clears backend identity"), Backend.StableBackendId.IsEmpty());
	DestroyComponentWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentReentrantReleaseTest,
	"AvidScript.Component.ReentrantRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentReentrantReleaseTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create the reentrant release world."));
		return false;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));
	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Reentrant release actor spawns"), Actor);
	UAvidScriptComponent* Component = Actor != nullptr ? AddAvidScriptComponent(Actor) : nullptr;
	TestNotNull(TEXT("Reentrant release component is created"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		return false;
	}

	FAvidScriptRuntimeSession* Session = Component->GetRuntimeSessionForTesting();
	TestNotNull(TEXT("Component owns a live Runtime Session"), Session);
	if (Session != nullptr)
	{
		Session->SetLiveExecutionObserverForTesting([Component]()
		{
			Component->DestroyComponent();
		});
		Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	}

	TestTrue(TEXT("Component EndPlay is observed during the active guest call"), Component->GetRuntimeStats().bComponentEndPlayObserved);
	TestFalse(TEXT("Deferred Runtime release completes after the guest call unwinds"), Component->GetRuntimeStats().bRuntimeLoaded);
	TestFalse(TEXT("Destroyed component is no longer registered"), Component->IsRegistered());
	DestroyComponentWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentTransactionalReloadTest,
	"AvidScript.Component.TransactionalReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentTransactionalReloadTest::RunTest(const FString& Parameters)
{
	FString FixtureRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests/Phase44/ComponentReload")));
	FPaths::NormalizeFilename(FixtureRoot);
	IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);

	FString ManifestV1Path;
	FString ManifestV2Path;
	FString TrapManifestPath;
	FString V1ExecutionJson;
	if (!TestTrue(
			TEXT("v1 precompiled execution fixture writes"),
			BuildComponentPrecompiledExecutionFixture(
				FixtureRoot,
				TEXT("component_reload_v1"),
				MakeArrayView(
					GComponentReloadCompatibleModule,
					UE_ARRAY_COUNT(GComponentReloadCompatibleModule)),
				V1ExecutionJson)))
	{
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return true;
	}
	if (!TestTrue(TEXT("v1 fixture writes"), WriteComponentReloadFixture(
			FixtureRoot,
			TEXT("component_reload_v1"),
			GComponentReloadCompatibleModule,
			UE_ARRAY_COUNT(GComponentReloadCompatibleModule),
			ManifestV1Path,
			V1ExecutionJson)) ||
		!TestTrue(TEXT("v2 fixture writes"), WriteComponentReloadFixture(
			FixtureRoot,
			TEXT("component_reload_v2"),
			GComponentReloadCompatibleModule,
			UE_ARRAY_COUNT(GComponentReloadCompatibleModule),
			ManifestV2Path)) ||
		!TestTrue(TEXT("trap fixture writes"), WriteComponentReloadFixture(
			FixtureRoot,
			TEXT("component_reload_trap"),
			GComponentReloadBeginPlayTrapModule,
			UE_ARRAY_COUNT(GComponentReloadBeginPlayTrapModule),
			TrapManifestPath)))
	{
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create transactional component reload world."));
		DestroyComponentWorld(World);
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return true;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));
	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Reload test actor spawns"), Actor);
	UAvidScriptComponent* Component = Actor != nullptr
		? AddAvidScriptComponent(Actor, ManifestV1Path)
		: nullptr;
	TestNotNull(TEXT("Reload component is created"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return true;
	}

	TestEqual(
		TEXT("v1 is initially active"),
		Component->GetRuntimeStats().ModuleId,
		FString(TEXT("component_reload_v1")));
	if (!V1ExecutionJson.IsEmpty())
	{
		FAvidScriptRuntimeSession* InitialSession =
			Component->GetRuntimeSessionForTesting();
		FAvidScriptWasmRuntimeInstance* InitialRuntime =
			InitialSession != nullptr
				? InitialSession->GetLiveRuntimeForTesting()
				: nullptr;
		if (TestNotNull(
				TEXT("v1 precompiled Runtime is active"),
				InitialRuntime))
		{
			TestEqual(
				TEXT("Component selects the Wasmtime serialized lane"),
				InitialRuntime->GetActiveBackendInfo().ArtifactFormat,
				EAvidScriptVmArtifactFormat::WasmtimeSerialized);
		}
	}
	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("v1 ticks"), Component->GetRuntimeStats().TickCallCount, 1);

	UFunction* ReloadFunction = Component->FindFunction(GET_FUNCTION_NAME_CHECKED(UAvidScriptComponent, ReloadScript));
	TestNotNull(TEXT("ReloadScript is reflected for Blueprint"), ReloadFunction);
	if (ReloadFunction != nullptr)
	{
		TestTrue(TEXT("ReloadScript is BlueprintCallable"), ReloadFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	Component->SetScriptManifestPath(ManifestV2Path);
	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(TEXT("compatible component reload applies"), Component->ReloadConfiguredScript(ReloadResult));
	TestTrue(TEXT("component reload result reports applied"), ReloadResult.bReloadApplied);
	TestEqual(TEXT("v2 becomes active"), Component->GetRuntimeStats().ModuleId, FString(TEXT("component_reload_v2")));
	TestEqual(TEXT("component records successful reload"), Component->GetRuntimeStats().SuccessfulReloadCount, 1);
	TestEqual(TEXT("active manifest commits v2"), Component->GetRuntimeStats().ScriptManifestPath, ManifestV2Path);
	TestTrue(TEXT("component stays loaded after successful reload"), Component->GetRuntimeStats().bRuntimeLoaded);

	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("v2 starts a fresh tick count"), Component->GetRuntimeStats().TickCallCount, 1);

	Component->SetScriptManifestPath(TrapManifestPath);
	TestFalse(TEXT("BeginPlay trap component reload is rejected"), Component->ReloadConfiguredScript(ReloadResult));
	TestTrue(TEXT("rejected component reload reports rollback"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestFalse(TEXT("rejected component reload is not applied"), ReloadResult.bReloadApplied);
	TestEqual(TEXT("v2 remains active after rejection"), Component->GetRuntimeStats().ModuleId, FString(TEXT("component_reload_v2")));
	TestEqual(TEXT("component records rejected reload"), Component->GetRuntimeStats().RejectedReloadCount, 1);
	TestEqual(TEXT("active manifest remains v2"), Component->GetRuntimeStats().ScriptManifestPath, ManifestV2Path);
	TestTrue(TEXT("component stays loaded after rejected reload"), Component->GetRuntimeStats().bRuntimeLoaded);
	TestTrue(TEXT("component tick remains enabled after rejected reload"), Component->IsComponentTickEnabled());

	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("old v2 runtime continues ticking"), Component->GetRuntimeStats().TickCallCount, 2);

	DestroyComponentWorld(World);
	IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentOwnerHandleLifecycleSmokeTest,
	"AvidScript.Component.OwnerHandleLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentOwnerHandleLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript component world."));
		DestroyComponentWorld(World);
		return true;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	UAvidScriptComponent* Component = AddAvidScriptComponent(Actor);
	TestNotNull(TEXT("AvidScript component is attachable to an actor"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	UFunction* DispatchEventFunction = Component->FindFunction(GET_FUNCTION_NAME_CHECKED(UAvidScriptComponent, DispatchScriptEvent));
	TestNotNull(TEXT("DispatchScriptEvent is reflected for Blueprint"), DispatchEventFunction);
	if (DispatchEventFunction != nullptr)
	{
		TestTrue(TEXT("DispatchScriptEvent is BlueprintCallable"), DispatchEventFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	UFunction* DispatchInputFunction = Component->FindFunction(GET_FUNCTION_NAME_CHECKED(UAvidScriptComponent, DispatchScriptInput));
	TestNotNull(TEXT("DispatchScriptInput is reflected for Blueprint"), DispatchInputFunction);
	if (DispatchInputFunction != nullptr)
	{
		TestTrue(TEXT("DispatchScriptInput is BlueprintCallable"), DispatchInputFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	const FAvidScriptComponentRuntimeStats StatsAfterBeginPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component registers owner on BeginPlay"), StatsAfterBeginPlay.bOwnerRegistered);
	TestTrue(TEXT("Component exposes a valid owner handle"), StatsAfterBeginPlay.OwnerHandle.IsValid());
	TestEqual(TEXT("Component records owner object path"), StatsAfterBeginPlay.OwnerObjectPath, Actor->GetPathName());

	FAvidScriptObjectHandleResult ResolveResult;
	AActor* ResolvedOwner = nullptr;
	TestTrue(TEXT("Owner handle resolves while component is active"), Component->ResolveOwnerActor(ResolvedOwner, ResolveResult));
	TestEqual(TEXT("Resolved owner matches component owner"), ResolvedOwner, Actor);

	TestTrue(TEXT("Smoke world routes EndPlay"), World->EndPlay(EEndPlayReason::Quit));

	const FAvidScriptComponentRuntimeStats StatsAfterEndPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component records receiving EndPlay"), StatsAfterEndPlay.bComponentEndPlayObserved);
	TestFalse(TEXT("Missing optional guest EndPlay is not marked called"), StatsAfterEndPlay.bEndPlayCalled);
	TestTrue(TEXT("Component releases owner handle on EndPlay"), StatsAfterEndPlay.bOwnerReleased);

	ResolvedOwner = nullptr;
	TestFalse(TEXT("Released owner handle no longer resolves"), Component->ResolveOwnerActor(ResolvedOwner, ResolveResult));
	TestEqual(TEXT("Released owner handle reports stale generation"), ResolveResult.ErrorCategory, FString(TEXT("generation_mismatch")));

	DestroyComponentWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentRuntimeTickSmokeTest,
	"AvidScript.Component.RuntimeTickSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentRuntimeTickSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript component world."));
		DestroyComponentWorld(World);
		return true;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	UAvidScriptComponent* Component = AddAvidScriptComponent(Actor);
	TestNotNull(TEXT("AvidScript component is attachable to an actor"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	const FAvidScriptComponentRuntimeStats StatsAfterBeginPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component loads embedded smoke runtime on BeginPlay"), StatsAfterBeginPlay.bRuntimeLoaded);
	TestTrue(TEXT("Component calls avid_on_begin_play"), StatsAfterBeginPlay.bBeginPlayCalled);

	World->Tick(LEVELTICK_All, 1.0f / 60.0f);

	const FAvidScriptComponentRuntimeStats StatsAfterTick = Component->GetRuntimeStats();
	TestTrue(TEXT("Component tick calls avid_on_tick"), StatsAfterTick.TickCallCount > 0);

	TestTrue(TEXT("Smoke world routes EndPlay"), World->EndPlay(EEndPlayReason::Quit));

	const FAvidScriptComponentRuntimeStats StatsAfterEndPlay = Component->GetRuntimeStats();
	TestFalse(TEXT("Component unloads runtime on EndPlay"), StatsAfterEndPlay.bRuntimeLoaded);

	DestroyComponentWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentCSharpManifestLifecycleSmokeTest,
	"AvidScript.Component.CSharpManifestLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentCSharpManifestLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	const FString ManifestPath = GetCSharpComponentManifestPath();
	if (!TestTrue(TEXT("C# adapter manifest exists before component lifecycle test"), FPaths::FileExists(ManifestPath)))
	{
		AddError(FString::Printf(
			TEXT("Run BuildCSharpActorLifecycle.ps1 before this component smoke. manifest=%s"),
			*ManifestPath));
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript component world."));
		DestroyComponentWorld(World);
		return true;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0));
	Actor->SetActorRotation(FRotator(5.0, 10.0, 15.0));
	Actor->SetActorScale3D(FVector(2.0, 2.0, 2.0));

	UAvidScriptComponent* Component = AddAvidScriptComponent(Actor, ManifestPath);
	TestNotNull(TEXT("AvidScript component is attachable to an actor with a C# manifest"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	const FAvidScriptComponentRuntimeStats StatsAfterBeginPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component loads C# manifest runtime on BeginPlay"), StatsAfterBeginPlay.bRuntimeLoaded);
	TestTrue(TEXT("Component calls C# avid_on_begin_play"), StatsAfterBeginPlay.bBeginPlayCalled);
	TestTrue(TEXT("C# component BeginPlay moves actor"), Actor->GetActorLocation().Equals(FVector(100.0, 200.0, 300.0), 0.01));
	TestTrue(TEXT("C# component BeginPlay resets rotation"), Actor->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));
	TestTrue(TEXT("C# component BeginPlay resets scale"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.01));

	World->Tick(LEVELTICK_All, 1.0f / 60.0f);

	const FAvidScriptComponentRuntimeStats StatsAfterTick = Component->GetRuntimeStats();
	TestTrue(TEXT("Component tick calls C# avid_on_tick"), StatsAfterTick.TickCallCount > 0);
	TestTrue(TEXT("C# component Tick moves actor"), Actor->GetActorLocation().Equals(FVector(102.0, 200.0, 300.0), 0.01));
	TestTrue(TEXT("C# component Tick rotates actor"), Actor->GetActorRotation().Equals(FRotator(0.0, 1.5, 0.0), 0.01));
	TestTrue(TEXT("C# component Tick scales actor"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.01), 0.01));
	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	const FAvidScriptComponentRuntimeStats StatsAfterTimer = Component->GetRuntimeStats();
	TestTrue(TEXT("C# component Timer callback moves actor"), Actor->GetActorLocation().Equals(FVector(106.0, 200.0, 350.0), 0.01));
	TestEqual(TEXT("Component records one Timer callback"), StatsAfterTimer.TimerCallbackCount, 1);
	TestEqual(TEXT("Component records Timer callback id"), StatsAfterTimer.LastTimerCallbackId, 7);
	TestTrue(TEXT("Component records Timer handle"), StatsAfterTimer.LastTimerHandle > 0);
	TestTrue(TEXT("Component dispatches a gameplay event"), Component->DispatchScriptEvent(3, 25.0f));
	const FAvidScriptComponentRuntimeStats StatsAfterEvent = Component->GetRuntimeStats();
	TestTrue(TEXT("C# component event callback moves actor"), Actor->GetActorLocation().Equals(FVector(106.0, 225.0, 350.0), 0.01));
	TestEqual(TEXT("Component records one gameplay event"), StatsAfterEvent.EventCallbackCount, 1);
	TestEqual(TEXT("Component records gameplay event id"), StatsAfterEvent.LastEventId, 3);
	TestEqual(TEXT("Component records gameplay event value"), StatsAfterEvent.LastEventValue, 25.0f);
	TestFalse(TEXT("Component rejects an invalid gameplay event id"), Component->DispatchScriptEvent(-1, 1.0f));
	TestTrue(TEXT("Invalid host event does not unload a healthy runtime"), Component->GetRuntimeStats().bRuntimeLoaded);

	TestTrue(TEXT("Component dispatches typed input"), Component->DispatchScriptInput(5, 2, FVector(1.0, 2.0, 3.0)));
	const FAvidScriptComponentRuntimeStats StatsAfterInput = Component->GetRuntimeStats();
	TestTrue(TEXT("C# input maps ids and vector"), Actor->GetActorLocation().Equals(FVector(6.0, 4.0, 3.0), 0.01));
	TestEqual(TEXT("Input uses shared event accounting"), StatsAfterInput.EventCallbackCount, 2);
	TestEqual(TEXT("Component records Input event type"), StatsAfterInput.LastEventId, static_cast<int32>(EAvidScriptGameplayEventType::Input));
	TestEqual(TEXT("Component records input action id"), StatsAfterInput.LastInputActionId, 5);
	TestEqual(TEXT("Component records input trigger event"), StatsAfterInput.LastInputTriggerEvent, 2);
	TestTrue(TEXT("Component records input vector"), StatsAfterInput.LastInputValue.Equals(FVector(1.0, 2.0, 3.0), 0.01));
	TestFalse(TEXT("Component rejects a negative input action id"), Component->DispatchScriptInput(-1, 2, FVector::ZeroVector));
	TestTrue(TEXT("Invalid input does not unload a healthy runtime"), Component->GetRuntimeStats().bRuntimeLoaded);

	TestTrue(TEXT("Smoke world routes EndPlay"), World->EndPlay(EEndPlayReason::Quit));
	TestFalse(TEXT("Input cannot dispatch after EndPlay"), Component->DispatchScriptInput(5, 2, FVector::ZeroVector));
	TestTrue(TEXT("C# component EndPlay moves actor"), Actor->GetActorLocation().Equals(FVector::ZeroVector, 0.01));
	TestTrue(TEXT("C# component EndPlay resets rotation"), Actor->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));
	TestTrue(TEXT("C# component EndPlay resets scale"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.01));

	const FAvidScriptComponentRuntimeStats StatsAfterEndPlay = Component->GetRuntimeStats();
	TestFalse(TEXT("Component unloads C# runtime on EndPlay"), StatsAfterEndPlay.bRuntimeLoaded);
	TestTrue(TEXT("Component records receiving C# EndPlay"), StatsAfterEndPlay.bComponentEndPlayObserved);
	TestTrue(TEXT("Component records the C# guest EndPlay export"), StatsAfterEndPlay.bEndPlayCalled);

	DestroyComponentWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentComposableOriginalTeardownTest,
	"AvidScript.Component.ComposableOriginalTeardown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentComposableOriginalTeardownTest::RunTest(const FString& Parameters)
{
	const FString FixtureDirectory = FPlatformMisc::GetEnvironmentVariable(
		TEXT("AVIDSCRIPT_COMPOSABLE_ORIGINAL_DIR"));
	FString CasesJson;
	TArray<TSharedPtr<FJsonValue>> Cases;
	if (!TestTrue(TEXT("All 29 original IR35 scenarios have fixture metadata"),
		!FixtureDirectory.IsEmpty()
			&& FFileHelper::LoadFileToString(
				CasesJson, *FPaths::Combine(FixtureDirectory, TEXT("cases.json")))
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CasesJson), Cases)
			&& Cases.Num() == 29))
	{
		return false;
	}
	TSet<FString> Names;
	for (const TSharedPtr<FJsonValue>& CaseValue : Cases)
	{
		const TSharedPtr<FJsonObject>* CaseObject = nullptr;
		FString Name;
		const TArray<TSharedPtr<FJsonValue>>* TaskObservations = nullptr;
		if (!TestTrue(TEXT("Original IR35 scenario has a safe unique name and Task oracle"),
			CaseValue.IsValid() && CaseValue->TryGetObject(CaseObject)
				&& CaseObject != nullptr && CaseObject->IsValid()
				&& (*CaseObject)->TryGetStringField(TEXT("name"), Name)
				&& !Name.IsEmpty() && FPaths::GetCleanFilename(Name) == Name
				&& !Name.Contains(TEXT(".")) && !Names.Contains(Name)
				&& (*CaseObject)->TryGetArrayField(TEXT("taskObservations"), TaskObservations)
				&& TaskObservations != nullptr && !TaskObservations->IsEmpty()))
		{
			return false;
		}
		Names.Add(Name);
		bool bExpectedSuspended = false;
		for (const TSharedPtr<FJsonValue>& TaskValue : *TaskObservations)
		{
			const TSharedPtr<FJsonObject>* TaskObject = nullptr;
			FString InitialState;
			if (!TestTrue(*(Name + TEXT(" has a supported initial Task state")),
				TaskValue.IsValid() && TaskValue->TryGetObject(TaskObject)
					&& TaskObject != nullptr && TaskObject->IsValid()
					&& (*TaskObject)->TryGetStringField(TEXT("initialState"), InitialState)
					&& (InitialState == TEXT("running") || InitialState == TEXT("succeeded")
						|| InitialState == TEXT("faulted") || InitialState == TEXT("cancelled"))))
			{
				return false;
			}
			bExpectedSuspended |= InitialState == TEXT("running");
		}
		const FString ManifestPath = FPaths::Combine(
			FixtureDirectory, Name + TEXT(".avidscript.json"));
		if (!TestTrue(*(Name + TEXT(" production manifest exists")),
			FPaths::FileExists(ManifestPath)))
		{
			return false;
		}
		FString PrecompiledManifestPath;
		if (!TestTrue(*(Name + TEXT(" same WASM has a Wasmtime serialized manifest")),
			BuildComposableComponentPrecompiledManifest(
				FixtureDirectory, Name, ManifestPath, PrecompiledManifestPath)))
		{
			return false;
		}
		for (int32 Scenario = 0; Scenario < 4; ++Scenario)
		{
			const int32 Backend = Scenario / 2;
			const int32 Mode = Scenario % 2;
			const FString Teardown = Mode == 0 ? TEXT("actor") : TEXT("world");
			const FString Label = FString::Printf(
				TEXT("scenario=%s backend=%d teardown=%s"), *Name, Backend, *Teardown);
			const FString& ActiveManifestPath = Backend == 0
				? ManifestPath : PrecompiledManifestPath;
			UWorld* World = nullptr;
			if (!TestTrue(*(Label + TEXT(" world is created")), CreateComponentWorld(World)))
			{
				return false;
			}
			ON_SCOPE_EXIT { DestroyComponentWorld(World); };
			if (!TestTrue(*(Label + TEXT(" world begins play")), BeginComponentWorld(World)))
			{
				return false;
			}
			AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
			if (!TestNotNull(*(Label + TEXT(" actor spawns")), Actor))
			{
				return false;
			}
			UAvidScriptComponent* Component = AddAvidScriptComponent(Actor, ActiveManifestPath);
			if (!TestNotNull(*(Label + TEXT(" production component is registered")), Component))
			{
				return false;
			}
			FAvidScriptRuntimeSession* Session = Component->GetRuntimeSessionForTesting();
			if (!TestNotNull(*(Label + TEXT(" production Session is active")), Session))
			{
				AddError(Component->GetRuntimeStats().LastErrorMessage);
				return false;
			}
			const FAvidScriptRuntimeSessionTestSnapshot Suspended = Session->GetTestSnapshot();
			FAvidScriptRuntimeSessionSnapshot RuntimeSnapshot;
			FAvidScriptVmBackendInfo BackendInfo;
			if (!TestTrue(*(Label + TEXT(" backend identity is available")),
				Component->CaptureRuntimeDiagnostics(RuntimeSnapshot, BackendInfo)))
			{
				return false;
			}
			TestEqual(*(Label + TEXT(" selects the requested VM")), BackendInfo.Kind,
				Backend == 0 ? EAvidScriptVmBackendKind::Wamr : EAvidScriptVmBackendKind::Wasmtime);
			TestEqual(*(Label + TEXT(" selects the requested artifact")), BackendInfo.ArtifactFormat,
				Backend == 0 ? EAvidScriptVmArtifactFormat::WasmBytecode
					: EAvidScriptVmArtifactFormat::WasmtimeSerialized);
			if (!TestTrue(*(Label + TEXT(" original C# BeginPlay loaded")),
				Component->GetRuntimeStats().bRuntimeLoaded
					&& Component->GetRuntimeStats().bBeginPlayCalled
					&& Suspended.Runtime.bHasActiveRuntime))
			{
				AddError(Component->GetRuntimeStats().LastErrorMessage);
				return false;
			}
			if (bExpectedSuspended
				&& !TestTrue(*(Label + TEXT(" original running Task is suspended")),
					Suspended.TaskCount > 0 && Suspended.TaskWaiterCount > 0
						&& Suspended.Runtime.PendingContinuationCount > 0
						&& Suspended.ContinuationStateBytes > 0))
			{
				return false;
			}

			const auto RuntimeLease = Session->GetRuntimeLeaseForTesting();
			const int32 TicksBefore = Component->GetRuntimeStats().TickCallCount;
			if (Mode == 0)
			{
				if (!TestTrue(TEXT("The actual Actor is destroyed"), World->DestroyActor(Actor)))
				{
					return false;
				}
			}
			else if (!TestTrue(TEXT("The actual World ends play"), World->EndPlay(EEndPlayReason::Quit)))
			{
				return false;
			}
			if (!TestTrue(*(Label + TEXT(" component observed EndPlay")),
				Component->GetRuntimeStats().bComponentEndPlayObserved)
				|| !TestFalse(*(Label + TEXT(" component runtime is unloaded")),
					Component->GetRuntimeStats().bRuntimeLoaded)
				|| !TestNull(*(Label + TEXT(" Session is released")),
					Component->GetRuntimeSessionForTesting())
				|| !TestFalse(*(Label + TEXT(" old VM lease has expired")), RuntimeLease.IsValid()))
			{
				return false;
			}
			if (Mode == 0)
			{
				for (int32 Round = 0; Round < 4; ++Round)
				{
					World->Tick(LEVELTICK_All, 0.02f);
					++GFrameCounter;
				}
				TestEqual(TEXT("Destroyed Actor receives no later script Tick"),
					Component->GetRuntimeStats().TickCallCount, TicksBefore);
			}
			AddInfo(FString::Printf(
				TEXT("original-ir35-component scenario=%s backend=%d teardown=%s suspended=%d tasks_before=%d waiters_before=%d released=1"),
				*Name, Backend, *Teardown, bExpectedSuspended ? 1 : 0,
				Suspended.TaskCount, Suspended.TaskWaiterCount));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentComposableOriginalTwoActorsTest,
	"AvidScript.Component.ComposableOriginalTwoActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentComposableOriginalTwoActorsTest::RunTest(const FString& Parameters)
{
	const FString FixtureDirectory = FPlatformMisc::GetEnvironmentVariable(
		TEXT("AVIDSCRIPT_COMPOSABLE_ORIGINAL_DIR"));
	FString CasesJson;
	TArray<TSharedPtr<FJsonValue>> Cases;
	if (!TestTrue(TEXT("The original C# Task oracle is present"),
		!FixtureDirectory.IsEmpty()
			&& FFileHelper::LoadFileToString(
				CasesJson, *FPaths::Combine(FixtureDirectory, TEXT("cases.json")))
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CasesJson), Cases)))
	{
		return false;
	}
	int32 ExpectedResult = 0;
	int32 ExpectedTrace = 0;
	int32 ResultOffset = -1;
	int32 TraceOffset = -1;
	bool bFoundOracle = false;
	for (const TSharedPtr<FJsonValue>& CaseValue : Cases)
	{
		const TSharedPtr<FJsonObject>* CaseObject = nullptr;
		FString Name;
		if (CaseValue.IsValid() && CaseValue->TryGetObject(CaseObject)
			&& CaseObject != nullptr && CaseObject->IsValid()
			&& (*CaseObject)->TryGetStringField(TEXT("name"), Name)
			&& Name == TEXT("field-mode-0"))
		{
			bFoundOracle = (*CaseObject)->TryGetNumberField(TEXT("expected"), ExpectedResult)
				&& (*CaseObject)->TryGetNumberField(TEXT("trace"), ExpectedTrace)
				&& (*CaseObject)->TryGetNumberField(TEXT("resultOffset"), ResultOffset)
				&& (*CaseObject)->TryGetNumberField(TEXT("traceOffset"), TraceOffset)
				&& ResultOffset >= 0 && ResultOffset < 65532
				&& TraceOffset >= 0 && TraceOffset < 65532;
			break;
		}
	}
	if (!TestTrue(TEXT("The original field-mode-0 result and trace oracle is valid"), bFoundOracle))
	{
		return false;
	}
	const FString ManifestPath = FPaths::Combine(
		FixtureDirectory, TEXT("field-mode-0.avidscript.json"));
	FString PrecompiledManifestPath;
	if (!TestTrue(TEXT("The original C# module has a serialized Wasmtime candidate"),
		FPaths::FileExists(ManifestPath)
			&& BuildComposableComponentPrecompiledManifest(
				FixtureDirectory, TEXT("field-mode-0"), ManifestPath,
				PrecompiledManifestPath)))
	{
		return false;
	}
	for (int32 Backend = 0; Backend < 2; ++Backend)
	{
		const FString Label = FString::Printf(TEXT("backend=%d"), Backend);
		const FString& ActiveManifest = Backend == 0
			? ManifestPath : PrecompiledManifestPath;
		UWorld* World = nullptr;
		if (!TestTrue(*(Label + TEXT(" world is created")), CreateComponentWorld(World)))
		{
			return false;
		}
		ON_SCOPE_EXIT { DestroyComponentWorld(World); };
		if (!TestTrue(*(Label + TEXT(" world begins play")), BeginComponentWorld(World)))
		{
			return false;
		}
		AActor* RetiringActor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
		AActor* SurvivingActor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
		if (!TestNotNull(*(Label + TEXT(" retiring actor spawns")), RetiringActor)
			|| !TestNotNull(*(Label + TEXT(" surviving actor spawns")), SurvivingActor))
		{
			return false;
		}
		UAvidScriptComponent* Retiring = AddAvidScriptComponent(RetiringActor, ActiveManifest);
		UAvidScriptComponent* Surviving = AddAvidScriptComponent(SurvivingActor, ActiveManifest);
		if (!TestNotNull(*(Label + TEXT(" retiring component loads")), Retiring)
			|| !TestNotNull(*(Label + TEXT(" surviving component loads")), Surviving))
		{
			return false;
		}
		FAvidScriptRuntimeSession* RetiringSession = Retiring->GetRuntimeSessionForTesting();
		FAvidScriptRuntimeSession* SurvivingSession = Surviving->GetRuntimeSessionForTesting();
		if (!TestNotNull(*(Label + TEXT(" retiring Session is active")), RetiringSession)
			|| !TestNotNull(*(Label + TEXT(" surviving Session is active")), SurvivingSession))
		{
			AddError(Retiring->GetRuntimeStats().LastErrorMessage);
			AddError(Surviving->GetRuntimeStats().LastErrorMessage);
			return false;
		}
		const auto RetiringLease = RetiringSession->GetRuntimeLeaseForTesting();
		const auto SurvivingLease = SurvivingSession->GetRuntimeLeaseForTesting();
		const auto RetiringSnapshot = RetiringSession->GetTestSnapshot();
		const auto SurvivingSnapshot = SurvivingSession->GetTestSnapshot();
		if (!TestTrue(*(Label + TEXT(" the two Actors own distinct live VMs")),
			RetiringLease.IsValid() && SurvivingLease.IsValid()
				&& RetiringLease.Pin().Get() != SurvivingLease.Pin().Get()
				&& RetiringSession != SurvivingSession)
			|| !TestTrue(*(Label + TEXT(" both original Tasks are suspended")),
				RetiringSnapshot.TaskCount > 0 && RetiringSnapshot.TaskWaiterCount > 0
				&& SurvivingSnapshot.TaskCount > 0 && SurvivingSnapshot.TaskWaiterCount > 0))
		{
			return false;
		}
		if (!TestTrue(*(Label + TEXT(" one actual Actor is destroyed")),
			World->DestroyActor(RetiringActor))
			|| !TestNull(*(Label + TEXT(" only its Session is released")),
				Retiring->GetRuntimeSessionForTesting())
			|| !TestFalse(*(Label + TEXT(" its VM lease expires")), RetiringLease.IsValid())
			|| !TestTrue(*(Label + TEXT(" the other Session retains its VM")),
				Surviving->GetRuntimeSessionForTesting() == SurvivingSession
					&& SurvivingLease.IsValid()
					&& Surviving->GetRuntimeStats().bRuntimeLoaded))
		{
			return false;
		}
		for (int32 Round = 0; Round < 64; ++Round)
		{
			World->Tick(LEVELTICK_All, 0.02f);
			++GFrameCounter;
		}
		auto ReadState = [&](int32 Offset, int32& OutValue) -> bool
		{
			uint8 Bytes[4] = {};
			FString Error;
			FAvidScriptWasmRuntimeInstance* Runtime = SurvivingSession->GetLiveRuntimeForTesting();
			if (Runtime == nullptr || !Runtime->ReadStateBytes(Offset, MakeArrayView(Bytes), Error))
			{
				AddError(Error.IsEmpty() ? TEXT("Surviving Runtime is unavailable") : Error);
				return false;
			}
			FMemory::Memcpy(&OutValue, Bytes, sizeof(OutValue));
			return true;
		};
		int32 Result = 0;
		int32 Trace = 0;
		if (!TestTrue(*(Label + TEXT(" surviving C# state is readable")),
			ReadState(ResultOffset, Result) && ReadState(TraceOffset, Trace))
			|| !TestEqual(*(Label + TEXT(" surviving result matches .NET")),
				Result, ExpectedResult)
			|| !TestEqual(*(Label + TEXT(" surviving trace matches .NET")),
				Trace, ExpectedTrace))
		{
			return false;
		}
		const auto Completed = SurvivingSession->GetTestSnapshot();
		if (!TestTrue(*(Label + TEXT(" surviving Task and continuation finish")),
			Completed.TaskCount == 0 && Completed.TaskWaiterCount == 0
				&& Completed.Runtime.PendingContinuationCount == 0
				&& Completed.ContinuationStateBytes == 0))
		{
			return false;
		}
		if (!TestTrue(*(Label + TEXT(" World ends play")), World->EndPlay(EEndPlayReason::Quit))
			|| !TestNull(*(Label + TEXT(" surviving Session releases")),
				Surviving->GetRuntimeSessionForTesting())
			|| !TestFalse(*(Label + TEXT(" surviving VM lease expires")),
				SurvivingLease.IsValid()))
		{
			return false;
		}
		AddInfo(FString::Printf(
			TEXT("original-ir35-two-actors backend=%d result=%d trace=%d retired=1 survivor_released=1"),
			Backend, Result, Trace));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentComposableOriginalSuspendedReloadTest,
	"AvidScript.Component.ComposableOriginalSuspendedReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentComposableOriginalSuspendedReloadTest::RunTest(const FString& Parameters)
{
	const FString FixtureDirectory = FPlatformMisc::GetEnvironmentVariable(
		TEXT("AVIDSCRIPT_COMPOSABLE_ORIGINAL_DIR"));
	FString CasesJson;
	TArray<TSharedPtr<FJsonValue>> Cases;
	if (!TestTrue(TEXT("Original C# reload oracles are present"),
		!FixtureDirectory.IsEmpty()
			&& FFileHelper::LoadFileToString(
				CasesJson, *FPaths::Combine(FixtureDirectory, TEXT("cases.json")))
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CasesJson), Cases)))
	{
		return false;
	}
	struct FOriginalOracle
	{
		int32 Result = 0;
		int32 Trace = 0;
		int32 ResultOffset = -1;
		int32 TraceOffset = -1;
	};
	auto FindOracle = [&](const FString& Name, FOriginalOracle& OutOracle) -> bool
	{
		for (const TSharedPtr<FJsonValue>& CaseValue : Cases)
		{
			const TSharedPtr<FJsonObject>* CaseObject = nullptr;
			FString CaseName;
			if (!CaseValue.IsValid() || !CaseValue->TryGetObject(CaseObject)
				|| CaseObject == nullptr || !CaseObject->IsValid()
				|| !(*CaseObject)->TryGetStringField(TEXT("name"), CaseName)
				|| CaseName != Name)
			{
				continue;
			}
			return (*CaseObject)->TryGetNumberField(TEXT("expected"), OutOracle.Result)
				&& (*CaseObject)->TryGetNumberField(TEXT("trace"), OutOracle.Trace)
				&& (*CaseObject)->TryGetNumberField(TEXT("resultOffset"), OutOracle.ResultOffset)
				&& (*CaseObject)->TryGetNumberField(TEXT("traceOffset"), OutOracle.TraceOffset)
				&& OutOracle.ResultOffset >= 0 && OutOracle.ResultOffset < 65532
				&& OutOracle.TraceOffset >= 0 && OutOracle.TraceOffset < 65532;
		}
		return false;
	};
	FOriginalOracle OldOracle;
	FOriginalOracle NewOracle;
	if (!TestTrue(TEXT("Both original C# versions have .NET result and trace oracles"),
		FindOracle(TEXT("field-mode-0"), OldOracle)
			&& FindOracle(TEXT("field-mode-1"), NewOracle)
			&& OldOracle.Trace != NewOracle.Trace))
	{
		return false;
	}
	const FString OldManifest = FPaths::Combine(
		FixtureDirectory, TEXT("field-mode-0.avidscript.json"));
	const FString NewManifest = FPaths::Combine(
		FixtureDirectory, TEXT("field-mode-1.avidscript.json"));
	FString OldAotManifest;
	FString NewAotManifest;
	if (!TestTrue(TEXT("Both original C# versions have Win64 VM artifacts"),
		FPaths::FileExists(OldManifest) && FPaths::FileExists(NewManifest)
			&& BuildComposableComponentPrecompiledManifest(
				FixtureDirectory, TEXT("field-mode-0"), OldManifest, OldAotManifest)
			&& BuildComposableComponentPrecompiledManifest(
				FixtureDirectory, TEXT("field-mode-1"), NewManifest, NewAotManifest)))
	{
		return false;
	}
	for (int32 Backend = 0; Backend < 2; ++Backend)
	{
		const FString Label = FString::Printf(TEXT("backend=%d"), Backend);
		const FString& ActiveManifest = Backend == 0 ? OldManifest : OldAotManifest;
		const FString& CandidateManifest = Backend == 0 ? NewManifest : NewAotManifest;
		FString NormalizedActiveManifest = ActiveManifest;
		FString NormalizedCandidateManifest = CandidateManifest;
		FPaths::NormalizeFilename(NormalizedActiveManifest);
		FPaths::NormalizeFilename(NormalizedCandidateManifest);
		const FString InvalidManifest = FPaths::Combine(
			FixtureDirectory,
			FString::Printf(TEXT("field-mode-1-bad-hash-%d.avidscript.json"), Backend));
		const FString MissingExportManifest = FPaths::Combine(
			FixtureDirectory,
			FString::Printf(TEXT("field-mode-1-missing-export-%d.avidscript.json"), Backend));
		if (!TestTrue(*(Label + TEXT(" invalid candidate manifest is written")),
			BuildComposableComponentBadHashManifest(CandidateManifest, InvalidManifest))
			|| !TestTrue(*(Label + TEXT(" candidate validation failure is written")),
				BuildComposableComponentMissingExportManifest(CandidateManifest, MissingExportManifest)))
		{
			return false;
		}
		UWorld* World = nullptr;
		if (!TestTrue(*(Label + TEXT(" world is created")), CreateComponentWorld(World)))
		{
			return false;
		}
		ON_SCOPE_EXIT { DestroyComponentWorld(World); };
		if (!TestTrue(*(Label + TEXT(" world begins play")), BeginComponentWorld(World)))
		{
			return false;
		}
		AActor* UpdatingActor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
		AActor* UnchangedActor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
		if (!TestNotNull(*(Label + TEXT(" updating Actor spawns")), UpdatingActor)
			|| !TestNotNull(*(Label + TEXT(" unchanged Actor spawns")), UnchangedActor))
		{
			return false;
		}
		UAvidScriptComponent* Updating = AddAvidScriptComponent(UpdatingActor, ActiveManifest);
		UAvidScriptComponent* Unchanged = AddAvidScriptComponent(UnchangedActor, ActiveManifest);
		if (!TestNotNull(*(Label + TEXT(" updating Component loads")), Updating)
			|| !TestNotNull(*(Label + TEXT(" unchanged Component loads")), Unchanged))
		{
			return false;
		}
		FAvidScriptRuntimeSession* UpdatingSession = Updating->GetRuntimeSessionForTesting();
		FAvidScriptRuntimeSession* UnchangedSession = Unchanged->GetRuntimeSessionForTesting();
		if (!TestNotNull(*(Label + TEXT(" updating Session is active")), UpdatingSession)
			|| !TestNotNull(*(Label + TEXT(" unchanged Session is active")), UnchangedSession))
		{
			AddError(Updating->GetRuntimeStats().LastErrorMessage);
			AddError(Unchanged->GetRuntimeStats().LastErrorMessage);
			return false;
		}
		const auto OldLease = UpdatingSession->GetRuntimeLeaseForTesting();
		const auto UnchangedLease = UnchangedSession->GetRuntimeLeaseForTesting();
		const auto Suspended = UpdatingSession->GetTestSnapshot();
		const auto OtherSuspended = UnchangedSession->GetTestSnapshot();
		if (!TestTrue(*(Label + TEXT(" both VMs are distinct and suspended")),
			OldLease.IsValid() && UnchangedLease.IsValid()
				&& OldLease.Pin().Get() != UnchangedLease.Pin().Get()
				&& Suspended.TaskCount > 0 && Suspended.TaskWaiterCount > 0
				&& Suspended.Runtime.PendingContinuationCount > 0
				&& OtherSuspended.TaskCount > 0 && OtherSuspended.TaskWaiterCount > 0))
		{
			return false;
		}
		Updating->SetScriptManifestPath(InvalidManifest);
		FAvidScriptWasmReloadResult ReloadResult;
		const bool bRejected = Updating->ReloadConfiguredScript(ReloadResult);
		bool bPreserved = TestFalse(
			*(Label + TEXT(" invalid artifact reload is rejected")), bRejected);
		bPreserved &= TestTrue(
			*(Label + TEXT(" rejection reports rollback")), ReloadResult.bRollbackPreservedLiveRuntime);
		bPreserved &= TestFalse(
			*(Label + TEXT(" rejected candidate is not applied")), ReloadResult.bReloadApplied);
		bPreserved &= TestFalse(
			*(Label + TEXT(" rejection identifies its error")), ReloadResult.ErrorCategory.IsEmpty());
		bPreserved &= TestTrue(
			*(Label + TEXT(" original Session survives")), Updating->GetRuntimeSessionForTesting() == UpdatingSession);
		bPreserved &= TestTrue(
			*(Label + TEXT(" original VM lease survives")),
			UpdatingSession->GetRuntimeLeaseForTesting().Pin().Get() == OldLease.Pin().Get());
		bPreserved &= TestEqual(
			*(Label + TEXT(" active manifest remains the original")), Updating->GetRuntimeStats().ScriptManifestPath, NormalizedActiveManifest);
		bPreserved &= TestEqual(
			*(Label + TEXT(" rejected reload is counted")), Updating->GetRuntimeStats().RejectedReloadCount, 1);
		if (!bPreserved)
		{
			return false;
		}
		const auto Rejected = UpdatingSession->GetTestSnapshot();
		if (!TestTrue(*(Label + TEXT(" pending Task survives candidate rejection")),
			Rejected.TaskCount == Suspended.TaskCount
				&& Rejected.TaskWaiterCount == Suspended.TaskWaiterCount
				&& Rejected.ContinuationStateBytes == Suspended.ContinuationStateBytes
				&& Rejected.Runtime.PendingContinuationCount == Suspended.Runtime.PendingContinuationCount
				&& UnchangedSession->GetRuntimeLeaseForTesting().Pin().Get() == UnchangedLease.Pin().Get()))
		{
			return false;
		}
		Unchanged->SetScriptManifestPath(MissingExportManifest);
		FAvidScriptWasmReloadResult ValidationFailure;
		const bool bValidationApplied = Unchanged->ReloadConfiguredScript(ValidationFailure);
		bool bValidationPreserved = TestFalse(
			*(Label + TEXT(" VM validation rejects the candidate")), bValidationApplied);
		bValidationPreserved &= TestEqual(
			*(Label + TEXT(" candidate lacks the required export")),
			ValidationFailure.ErrorCategory, FString(TEXT("missing_export")));
		bValidationPreserved &= TestTrue(
			*(Label + TEXT(" VM validation rolls back")),
			ValidationFailure.bRollbackPreservedLiveRuntime && !ValidationFailure.bReloadApplied
				&& Unchanged->GetRuntimeSessionForTesting() == UnchangedSession
				&& UnchangedSession->GetRuntimeLeaseForTesting().Pin().Get() == UnchangedLease.Pin().Get()
				&& Unchanged->GetRuntimeStats().RejectedReloadCount == 1
				&& Unchanged->GetRuntimeStats().ScriptManifestPath == NormalizedActiveManifest);
		const auto ValidationRejected = UnchangedSession->GetTestSnapshot();
		bValidationPreserved &= TestTrue(
			*(Label + TEXT(" old Task survives VM validation")),
			ValidationRejected.TaskCount == OtherSuspended.TaskCount
				&& ValidationRejected.TaskWaiterCount == OtherSuspended.TaskWaiterCount
				&& ValidationRejected.ContinuationStateBytes == OtherSuspended.ContinuationStateBytes
				&& ValidationRejected.Runtime.PendingContinuationCount == OtherSuspended.Runtime.PendingContinuationCount);
		if (!bValidationPreserved)
		{
			return false;
		}
		Updating->SetScriptManifestPath(CandidateManifest);
		if (!TestTrue(*(Label + TEXT(" original C# update is applied")),
			Updating->ReloadConfiguredScript(ReloadResult)))
		{
			AddError(ReloadResult.ErrorMessage);
			return false;
		}
		const auto NewLease = UpdatingSession->GetRuntimeLeaseForTesting();
		if (!TestTrue(*(Label + TEXT(" commit replaces only one VM")),
			ReloadResult.bReloadApplied
				&& Updating->GetRuntimeSessionForTesting() == UpdatingSession
				&& !OldLease.IsValid() && NewLease.IsValid()
				&& NewLease.Pin().Get() != UnchangedLease.Pin().Get()
				&& UnchangedLease.IsValid()
				&& Updating->GetRuntimeStats().ScriptManifestPath == NormalizedCandidateManifest
				&& Updating->GetRuntimeStats().SuccessfulReloadCount == 1))
		{
			return false;
		}
		for (int32 Round = 0; Round < 64; ++Round)
		{
			World->Tick(LEVELTICK_All, 0.02f);
			++GFrameCounter;
		}
		auto ReadValue = [&](FAvidScriptRuntimeSession* Session, int32 Offset, int32& OutValue) -> bool
		{
			uint8 Bytes[4] = {};
			FString Error;
			FAvidScriptWasmRuntimeInstance* Runtime = Session->GetLiveRuntimeForTesting();
			if (Runtime == nullptr || !Runtime->ReadStateBytes(Offset, MakeArrayView(Bytes), Error))
			{
				AddError(Error.IsEmpty() ? TEXT("Runtime state is unavailable") : Error);
				return false;
			}
			FMemory::Memcpy(&OutValue, Bytes, sizeof(OutValue));
			return true;
		};
		int32 NewResult = 0;
		int32 NewTrace = 0;
		int32 OldResult = 0;
		int32 OldTrace = 0;
		if (!TestTrue(*(Label + TEXT(" both C# VM states are readable")),
			ReadValue(UpdatingSession, NewOracle.ResultOffset, NewResult)
				&& ReadValue(UpdatingSession, NewOracle.TraceOffset, NewTrace)
				&& ReadValue(UnchangedSession, OldOracle.ResultOffset, OldResult)
				&& ReadValue(UnchangedSession, OldOracle.TraceOffset, OldTrace))
			|| !TestEqual(*(Label + TEXT(" new result matches .NET")), NewResult, NewOracle.Result)
			|| !TestEqual(*(Label + TEXT(" new trace matches .NET")), NewTrace, NewOracle.Trace)
			|| !TestEqual(*(Label + TEXT(" old result matches .NET")), OldResult, OldOracle.Result)
			|| !TestEqual(*(Label + TEXT(" old trace matches .NET")), OldTrace, OldOracle.Trace))
		{
			return false;
		}
		const auto Updated = UpdatingSession->GetTestSnapshot();
		const auto UnchangedDone = UnchangedSession->GetTestSnapshot();
		if (!TestTrue(*(Label + TEXT(" both Task graphs are released")),
			Updated.TaskCount == 0 && Updated.TaskWaiterCount == 0
				&& Updated.Runtime.PendingContinuationCount == 0
				&& Updated.ContinuationStateBytes == 0
				&& UnchangedDone.TaskCount == 0 && UnchangedDone.TaskWaiterCount == 0
				&& UnchangedDone.Runtime.PendingContinuationCount == 0
				&& UnchangedDone.ContinuationStateBytes == 0))
		{
			return false;
		}
		if (!TestTrue(*(Label + TEXT(" world ends play")), World->EndPlay(EEndPlayReason::Quit))
			|| !TestNull(*(Label + TEXT(" updated Session releases")),
				Updating->GetRuntimeSessionForTesting())
			|| !TestNull(*(Label + TEXT(" unchanged Session releases")),
				Unchanged->GetRuntimeSessionForTesting())
			|| !TestFalse(*(Label + TEXT(" updated VM lease expires")), NewLease.IsValid())
			|| !TestFalse(*(Label + TEXT(" unchanged VM lease expires")), UnchangedLease.IsValid()))
		{
			return false;
		}
		AddInfo(FString::Printf(
			TEXT("original-ir35-reload backend=%d new_result=%d new_trace=%d old_result=%d old_trace=%d loader_rejected=1 validation_rejected=1 applied=1 released=1"),
			Backend, NewResult, NewTrace, OldResult, OldTrace));
	}
	return true;
}

#endif
