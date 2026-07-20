#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptComponent.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorCSharpWorkspaceService.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{

uint64 MakeAvidScriptBindingRuntimeF32Cell(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

int32 FindAvidScriptBindingRuntimeBytes(
	const TConstArrayView<uint8> Bytes,
	const TConstArrayView<uint8> Sequence)
{
	if (Sequence.IsEmpty() || Sequence.Num() > Bytes.Num())
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index <= Bytes.Num() - Sequence.Num(); ++Index)
	{
		if (FMemory::Memcmp(Bytes.GetData() + Index, Sequence.GetData(), Sequence.Num()) == 0)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool CreateAvidScriptBindingRuntimeIntegrationWorld(
	UWorld*& OutWorld,
	bool bInitializeForPlay = true)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptBindingRuntimeIntegrationWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	if (bInitializeForPlay)
	{
		OutWorld->InitializeActorsForPlay(FURL());
	}
	return true;
}

void DestroyAvidScriptBindingRuntimeIntegrationWorld(UWorld*& World)
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

AActor* SpawnAvidScriptBindingRuntimeIntegrationActor(UWorld& World)
{
	AActor* Actor = World.SpawnActor<AActor>();
	if (Actor == nullptr)
	{
		return nullptr;
	}

	USceneComponent* RootComponent = NewObject<USceneComponent>(Actor, TEXT("BindingRuntimeRoot"));
	if (RootComponent == nullptr)
	{
		return nullptr;
	}
	Actor->SetRootComponent(RootComponent);
	RootComponent->RegisterComponent();
	return Actor;
}

bool LoadAvidScriptBindingRuntimeFixture(TArray<uint8>& OutBytecode)
{
	const FString FixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Tests/Fixtures/WasmBackend/P42_4_ReflectedSetActorScale.wasm")));
	return FFileHelper::LoadFileToArray(OutBytecode, *FixturePath);
}

bool LoadAvidScriptBindingRuntimeTrapFixture(TArray<uint8>& OutBytecode)
{
	if (!LoadAvidScriptBindingRuntimeFixture(OutBytecode))
	{
		return false;
	}

	const TArray<uint8> CodeHeader = { 0x0a, 0x47, 0x01, 0x45 };
	const TArray<uint8> CallTail = { 0x10, 0x02, 0x21, 0x05, 0x0f };
	const int32 CodeHeaderIndex = FindAvidScriptBindingRuntimeBytes(OutBytecode, CodeHeader);
	const int32 CallTailIndex = FindAvidScriptBindingRuntimeBytes(OutBytecode, CallTail);
	if (CodeHeaderIndex == INDEX_NONE || CallTailIndex == INDEX_NONE)
	{
		return false;
	}

	// This checked fixture has one small code body, so both encoded sizes remain one-byte LEB128 values.
	++OutBytecode[CodeHeaderIndex + 1];
	++OutBytecode[CodeHeaderIndex + 3];
	OutBytecode.Insert(0x00, CallTailIndex + 4);
	return true;
}

class FAvidScriptBindingRuntimeRecordingJournal final : public IAvidScriptBindingHostEffectJournal
{
public:
	explicit FAvidScriptBindingRuntimeRecordingJournal(const bool bInAcceptPrepare)
		: bAcceptPrepare(bInAcceptPrepare)
	{
	}

	bool PrepareEffect(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& Handle,
		UObject& Target,
		const EAvidScriptBindingReloadEffect Effect,
		FAvidScriptBindingHostEffectPrepareResult& OutResult) override
	{
		++PrepareCallCount;
		LastRegistry = &Registry;
		LastHandle = Handle;
		LastTarget = &Target;
		LastEffect = Effect;
		OutResult = FAvidScriptBindingHostEffectPrepareResult();
		OutResult.bSucceeded = bAcceptPrepare;
		if (!bAcceptPrepare)
		{
			OutResult.ErrorCategory = TEXT("test_host_effect_rejected");
			OutResult.ErrorSource = Target.GetPathName();
			OutResult.ErrorDetails = TEXT("The test journal rejected the candidate write.");
		}
		return bAcceptPrepare;
	}

	bool bAcceptPrepare = false;
	int32 PrepareCallCount = 0;
	FAvidScriptObjectRegistry* LastRegistry = nullptr;
	FAvidScriptObjectHandle LastHandle;
	UObject* LastTarget = nullptr;
	EAvidScriptBindingReloadEffect LastEffect = EAvidScriptBindingReloadEffect::Unsupported;
};

bool GenerateAvidScriptBindingRuntimePackage(
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FAvidScriptBindingPackageLoadResult& OutLoadResult,
	FString& OutDescriptorJson)
{
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	return FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(
			OutDescriptorJson,
			GenerateResult)
		&& FAvidScriptBindingPackage::LoadDescriptor(
			OutDescriptorJson,
			OutPackage,
			OutLoadResult);
}

bool BuildAvidScriptGeneratedBindingLifecycle(
	const FString& SemanticCacheRoot,
	FAvidScriptEditorCSharpBuildResult& OutBuildResult)
{
	FAvidScriptEditorCSharpBuildConfig Config;
	Config.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.SourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Samples/CSharp/GeneratedBindingLifecycle/GeneratedBindingLifecycleScript.cs")));
	Config.ModuleId = TEXT("csharp_generated_binding_lifecycle");
	Config.ArtifactStem = TEXT("generated_binding_lifecycle");
	Config.OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest/GeneratedBindingLifecycle")));
	Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
		Config.OutputRoot,
		Config.ArtifactStem);
	Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
		Config.OutputRoot,
		Config.ArtifactStem);
	Config.SemanticCacheRoot = SemanticCacheRoot;
	return FAvidScriptEditorCSharpBuildService::BuildProfile(Config, OutBuildResult);
}

bool AcceptAvidScriptGeneratedBindingLifecycleBuild(
	FAutomationTestBase& Test,
	const FString& BuildLabel,
	const FAvidScriptEditorCSharpBuildResult& BuildResult,
	FAvidScriptWasmReloadManifest& OutManifest,
	TArray<uint8>& OutBytecode)
{
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s authorization binding package exists"), *BuildLabel),
			FPaths::FileExists(BuildResult.AuthorizationBindingPackagePath))
		|| !Test.TestTrue(
			*FString::Printf(TEXT("%s runtime binding package exists"), *BuildLabel),
			FPaths::FileExists(BuildResult.BindingPackagePath)))
	{
		return false;
	}
	Test.TestNotEqual(
		*FString::Printf(TEXT("%s separates authorization and runtime packages"), *BuildLabel),
		BuildResult.AuthorizationBindingPackagePath,
		BuildResult.BindingPackagePath);

	FString AuthorizationPackageJson;
	TSharedPtr<FJsonObject> AuthorizationPackageObject;
	const TArray<TSharedPtr<FJsonValue>>* AuthorizationImports = nullptr;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package can be read"), *BuildLabel),
			FFileHelper::LoadFileToString(
				AuthorizationPackageJson,
				*BuildResult.AuthorizationBindingPackagePath))
		|| !Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package parses"), *BuildLabel),
			FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(AuthorizationPackageJson),
				AuthorizationPackageObject))
		|| !Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package exposes required imports"), *BuildLabel),
			AuthorizationPackageObject.IsValid()
				&& AuthorizationPackageObject->TryGetArrayField(
					TEXT("required_imports"),
					AuthorizationImports))
		|| AuthorizationImports == nullptr)
	{
		return false;
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s authorization ceiling contains 115 generated bindings"), *BuildLabel),
		AuthorizationImports->Num(),
		115);

	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s manifest, WASM, and runtime package load"), *BuildLabel),
			FAvidScriptWasmReloadManifestLoader::LoadFromFile(
				BuildResult.ManifestPath,
				OutManifest,
				OutBytecode,
				ManifestLoadResult)))
	{
		Test.AddError(ManifestLoadResult.ErrorMessage);
		return false;
	}
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s owns an immutable runtime package"), *BuildLabel),
			OutManifest.BindingPackage.IsValid()))
	{
		return false;
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s runtime package contains two reachable bindings"), *BuildLabel),
		OutManifest.BindingPackage->GetVmPackage().Imports.Num(),
		2);
	int32 RequiredDynamicImportCount = 0;
	for (const FAvidScriptWasmRequiredImport& Import : OutManifest.RequiredImports)
	{
		if (Import.ModuleName == TEXT("avidscript")
			&& Import.ImportName.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive))
		{
			++RequiredDynamicImportCount;
		}
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s WASM requires two reachable reflected imports"), *BuildLabel),
		RequiredDynamicImportCount,
		2);

	UWorld* World = nullptr;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s integration world is created"), *BuildLabel),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!Test.TestNotNull(
			*FString::Printf(TEXT("%s lifecycle actor spawns"), *BuildLabel),
			Actor))
	{
		return false;
	}
	Actor->SetActorScale3D(FVector(1.0, 1.0, 1.0));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	Test.TestTrue(
		*FString::Printf(TEXT("%s lifecycle owner registers"), *BuildLabel),
		RegisterResult.bSucceeded);

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = ActorHandle;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptRuntimeSession Session;
	Session.SetHostContext(HostContext);
	FAvidScriptWasmReloadResult ReloadResult;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s BeginPlay activates through Runtime Session and WAMR"), *BuildLabel),
			Session.LoadInitialModule(
				OutBytecode.GetData(),
				OutBytecode.Num(),
				OutManifest,
				ReloadResult)))
	{
		Test.AddError(ReloadResult.ErrorMessage);
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s BeginPlay applies generated FVector binding"), *BuildLabel),
		Actor->GetActorScale3D().Equals(FVector(2.0, 3.0, 4.0), 0.001));

	FAvidScriptWasmSmokeResult TickResult;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s Tick executes through the live scheduler"), *BuildLabel),
			Session.TickLive(0.5f, TickResult)))
	{
		Test.AddError(TickResult.ErrorMessage);
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s Tick reads and writes through generated bindings"), *BuildLabel),
		Actor->GetActorScale3D().Equals(FVector(2.5, 3.0, 4.0), 0.001));
	Test.TestEqual(
		*FString::Printf(TEXT("%s scheduler records one Tick"), *BuildLabel),
		Session.GetLiveTickCallCount(),
		1);

	FAvidScriptWasmReloadManifest CommitManifest = OutManifest;
	CommitManifest.ModuleId += TEXT("_transaction_commit");
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s C# candidate reload commits"), *BuildLabel),
			Session.ReloadModule(
				OutBytecode.GetData(),
				OutBytecode.Num(),
				CommitManifest,
				ReloadResult)))
	{
		Test.AddError(ReloadResult.ErrorMessage);
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s candidate opens a host effect transaction"), *BuildLabel),
		ReloadResult.bHostEffectTransactionAttempted);
	Test.TestTrue(
		*FString::Printf(TEXT("%s candidate commits its host effect transaction"), *BuildLabel),
		ReloadResult.bHostEffectTransactionCommitted);
	Test.TestEqual(
		*FString::Printf(TEXT("%s candidate captures one reflected Actor transform"), *BuildLabel),
		ReloadResult.HostEffectCapturedObjectCount,
		1);
	Test.TestTrue(
		*FString::Printf(TEXT("%s committed C# BeginPlay scale remains applied"), *BuildLabel),
		Actor->GetActorScale3D().Equals(FVector(2.0, 3.0, 4.0), 0.001));

	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s committed C# runtime ticks"), *BuildLabel),
			Session.TickLive(0.25f, TickResult)))
	{
		Test.AddError(TickResult.ErrorMessage);
		return false;
	}
	const FVector ScaleBeforeRejectedCandidate = Actor->GetActorScale3D();
	Test.TestTrue(
		*FString::Printf(TEXT("%s committed C# Tick retains live reflected writes"), *BuildLabel),
		ScaleBeforeRejectedCandidate.Equals(FVector(2.25, 3.0, 4.0), 0.001));

	TArray<uint8> TrapBytecode;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s dynamic write-then-trap fixture loads"), *BuildLabel),
			LoadAvidScriptBindingRuntimeTrapFixture(TrapBytecode)))
	{
		return false;
	}
	FAvidScriptWasmReloadManifest TrapManifest = OutManifest;
	TrapManifest.ModuleId += TEXT("_transaction_trap");
	TrapManifest.RequiredExports = { TEXT("avid_on_begin_play") };
	TrapManifest.RequiredImports = {
		{ TEXT("env"), TEXT("owner_get_slot") },
		{ TEXT("env"), TEXT("owner_get_generation") },
		{ TEXT("avidscript"), TEXT("avid_ue_e493dae7c6aae6c7") }
	};
	Test.TestFalse(
		*FString::Printf(TEXT("%s reflected write-then-trap candidate is rejected"), *BuildLabel),
		Session.ReloadModule(
			TrapBytecode.GetData(),
			TrapBytecode.Num(),
			TrapManifest,
			ReloadResult));
	Test.TestTrue(
		*FString::Printf(TEXT("%s rejected candidate attempts rollback"), *BuildLabel),
		ReloadResult.bHostEffectRollbackAttempted);
	Test.TestTrue(
		*FString::Printf(TEXT("%s rejected candidate restores reflected host effects"), *BuildLabel),
		ReloadResult.bHostEffectRollbackSucceeded);
	Test.TestEqual(
		*FString::Printf(TEXT("%s rejected candidate restores one Actor transform"), *BuildLabel),
		ReloadResult.HostEffectRestoredObjectCount,
		1);
	Test.TestTrue(
		*FString::Printf(TEXT("%s rejected candidate preserves the committed scale"), *BuildLabel),
		Actor->GetActorScale3D().Equals(ScaleBeforeRejectedCandidate, 0.001));
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s old C# runtime ticks after candidate rollback"), *BuildLabel),
			Session.TickLive(0.25f, TickResult)))
	{
		Test.AddError(TickResult.ErrorMessage);
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s old C# Tick continues reflected gameplay after rollback"), *BuildLabel),
		Actor->GetActorScale3D().Equals(FVector(2.5, 3.0, 4.0), 0.001));
	return true;
}

bool AcceptAvidScriptProjectGameplayWorkspaceBuild(
	FAutomationTestBase& Test,
	const FString& BuildLabel,
	const FAvidScriptEditorCSharpWorkspaceResult& WorkspaceResult,
	const FAvidScriptEditorCSharpBuildResult& BuildResult)
{
	FString AuthorizationPackageJson;
	TSharedPtr<FJsonObject> AuthorizationPackageObject;
	const TArray<TSharedPtr<FJsonValue>>* AuthorizationImports = nullptr;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package can be read"), *BuildLabel),
			FFileHelper::LoadFileToString(
				AuthorizationPackageJson,
				*BuildResult.AuthorizationBindingPackagePath))
		|| !Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package parses"), *BuildLabel),
			FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(AuthorizationPackageJson),
				AuthorizationPackageObject))
		|| !Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package exposes required imports"), *BuildLabel),
			AuthorizationPackageObject.IsValid()
				&& AuthorizationPackageObject->TryGetArrayField(
					TEXT("required_imports"),
					AuthorizationImports))
		|| AuthorizationImports == nullptr)
	{
		return false;
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s authorization ceiling contains 115 gameplay bindings"), *BuildLabel),
		AuthorizationImports->Num(),
		115);

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s manifest, WASM, and runtime package load"), *BuildLabel),
			FAvidScriptWasmReloadManifestLoader::LoadFromFile(
				BuildResult.ManifestPath,
				Manifest,
				Bytecode,
				ManifestLoadResult)))
	{
		Test.AddError(ManifestLoadResult.ErrorMessage);
		return false;
	}
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s owns a runtime binding package"), *BuildLabel),
			Manifest.BindingPackage.IsValid()))
	{
		return false;
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s runtime package contains three reachable bindings"), *BuildLabel),
		Manifest.BindingPackage->GetVmPackage().Imports.Num(),
		3);
	int32 DynamicImportCount = 0;
	for (const FAvidScriptWasmRequiredImport& Import : Manifest.RequiredImports)
	{
		if (Import.ModuleName == TEXT("avidscript")
			&& Import.ImportName.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive))
		{
			++DynamicImportCount;
		}
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s WASM requires three reflected imports"), *BuildLabel),
		DynamicImportCount,
		3);

	FString ManifestJson;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s manifest can be read for provenance checks"), *BuildLabel),
			FFileHelper::LoadFileToString(ManifestJson, *BuildResult.ManifestPath)))
	{
		return false;
	}
	Test.TestFalse(
		*FString::Printf(TEXT("%s manifest excludes generated facade path"), *BuildLabel),
		ManifestJson.Contains(WorkspaceResult.FacadePath, ESearchCase::CaseSensitive));

	UWorld* World = nullptr;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s component lifecycle world is created"), *BuildLabel),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World, false)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	World->SetBegunPlay(true);

	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!Test.TestNotNull(
			*FString::Printf(TEXT("%s gameplay actor spawns"), *BuildLabel),
			Actor))
	{
		return false;
	}
	Actor->SetActorScale3D(FVector(2.0, 2.0, 2.0));
	Actor->SetActorRotation(FRotator(0.0, 10.0, 0.0));

	FAvidScriptEditorComponentBindingResult BindingResult;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s report binds through ComponentBindingService"), *BuildLabel),
			FAvidScriptEditorComponentBindingService::ApplyCSharpReportToActor(
				BuildResult.ReportPath,
				Actor,
				BindingResult)))
	{
		Test.AddError(BindingResult.ErrorMessage);
		return false;
	}
	if (!Test.TestNotNull(
			*FString::Printf(TEXT("%s binding creates an AvidScript component"), *BuildLabel),
			BindingResult.Component))
	{
		return false;
	}

	const FAvidScriptComponentRuntimeStats StatsAfterBeginPlay =
		BindingResult.Component->GetRuntimeStats();
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s component loads the C# WASM runtime"), *BuildLabel),
			StatsAfterBeginPlay.bRuntimeLoaded))
	{
		Test.AddError(StatsAfterBeginPlay.LastErrorMessage);
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s component calls C# BeginPlay"), *BuildLabel),
		StatsAfterBeginPlay.bBeginPlayCalled);
	Test.TestTrue(
		*FString::Printf(TEXT("%s BeginPlay resets Actor scale"), *BuildLabel),
		Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.001));

	BindingResult.Component->TickComponent(0.5f, LEVELTICK_All, nullptr);
	const FAvidScriptComponentRuntimeStats StatsAfterTick =
		BindingResult.Component->GetRuntimeStats();
	Test.TestEqual(
		*FString::Printf(TEXT("%s component records one Tick"), *BuildLabel),
		StatsAfterTick.TickCallCount,
		1);
	Test.TestTrue(
		*FString::Printf(TEXT("%s Tick rotates Actor yaw by 45 degrees"), *BuildLabel),
		FMath::IsNearlyEqual(Actor->GetActorRotation().Yaw, 55.0, 0.01));
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeScalarMetadataFailureTest,
	"AvidScript.Editor.BindingRuntime.ScalarMetadataFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeScalarMetadataFailureTest::RunTest(const FString& Parameters)
{
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
		TEXT("Default descriptor generates for scalar metadata validation"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(
			DescriptorJson,
			GenerateResult)))
	{
		return false;
	}

	const FString TamperedJson = DescriptorJson.Replace(
		TEXT("\"cpp_type\": \"float\""),
		TEXT("\"cpp_type\": \"int32\""),
		ESearchCase::CaseSensitive);
	TestFalse(TEXT("Scalar cpp_type metadata was changed"), TamperedJson == DescriptorJson);

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	TestFalse(
		TEXT("Runtime package rejects same-width scalar cpp_type tampering"),
		FAvidScriptBindingPackage::LoadDescriptor(TamperedJson, Package, LoadResult));
	TestEqual(
		TEXT("Scalar metadata failure is attributed to the reflected return contract"),
		LoadResult.ErrorCategory,
		FString(TEXT("binding_return_contract_mismatch")));
	TestFalse(TEXT("Failed scalar package is not published"), Package.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeReflectedSetActorScaleTest,
	"AvidScript.Editor.BindingRuntime.ReflectedSetActorScaleLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeReflectedSetActorScaleTest::RunTest(const FString& Parameters)
{
	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult PackageResult;
	FString DescriptorJson;
	if (!TestTrue(
		TEXT("Default reflection descriptor resolves into a cached runtime package"),
		GenerateAvidScriptBindingRuntimePackage(Package, PackageResult, DescriptorJson)))
	{
		AddError(PackageResult.ErrorCategory + TEXT(": ") + PackageResult.ErrorDetails);
		return false;
	}
	TestEqual(TEXT("Cached package contains the default eight bindings"), PackageResult.BindingCount, 8);

	TArray<uint8> Bytecode;
	if (!TestTrue(
		TEXT("Generated reflected binding WASM fixture loads"),
		LoadAvidScriptBindingRuntimeFixture(Bytecode)))
	{
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("Binding runtime integration world is created"),
		CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!TestNotNull(TEXT("Binding runtime integration actor spawns"), Actor))
	{
		return false;
	}
	const FVector InitialScale(1.0, 1.0, 1.0);
	const FVector TargetScale(2.0, 3.0, 4.0);
	Actor->SetActorScale3D(InitialScale);
	USceneComponent* RootComponent = Actor->GetRootComponent();
	if (!TestNotNull(TEXT("Binding runtime integration actor retains its root component"), RootComponent))
	{
		return false;
	}
	TestEqual(TEXT("Binding runtime integration actor has authority"), Actor->GetLocalRole(), ROLE_Authority);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RootRegisterResult;
	const FAvidScriptObjectHandle RootHandle = Registry.RegisterObject(RootComponent, RootRegisterResult);
	TestTrue(TEXT("Root component reserves the first registry slot"), RootRegisterResult.bSucceeded);
	TestEqual(TEXT("Root component uses slot one"), RootHandle.Slot, 1u);

	FAvidScriptObjectHandleResult ActorRegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, ActorRegisterResult);
	TestTrue(TEXT("Owner actor registers"), ActorRegisterResult.bSucceeded);
	TestEqual(TEXT("Owner actor is deliberately not hardcoded to slot one"), ActorHandle.Slot, 2u);

	const FAvidScriptVmDynamicImport* SetScaleImport = Package->GetVmPackage().Imports.FindByPredicate(
		[](const FAvidScriptVmDynamicImport& Import)
		{
			return Import.ImportName == TEXT("avid_ue_e493dae7c6aae6c7");
		});
	if (!TestNotNull(TEXT("Cached package exposes the reflected SetActorScale3D import"), SetScaleImport))
	{
		return false;
	}
	const uint64 DirectArguments[] = {
		ActorHandle.Slot,
		ActorHandle.Generation,
		MakeAvidScriptBindingRuntimeF32Cell(2.0f),
		MakeAvidScriptBindingRuntimeF32Cell(3.0f),
		MakeAvidScriptBindingRuntimeF32Cell(4.0f)
	};
	FAvidScriptDynamicHostCall DirectCall;
	DirectCall.BindingOrdinal = SetScaleImport->Ordinal;
	DirectCall.Arguments = MakeArrayView(DirectArguments);
	FAvidScriptBindingInvocationContext DirectContext;
	DirectContext.ObjectRegistry = &Registry;
	DirectContext.OwnerHandle = ActorHandle;
	DirectContext.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TArray<uint8> DirectScratch;
	DirectScratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	FAvidScriptDynamicHostCallResult DirectResult;
	TestTrue(
		TEXT("Cached reflected package directly dispatches SetActorScale3D"),
		Package->Dispatch(DirectCall, DirectContext, DirectScratch, DirectResult));
	TestTrue(
		TEXT("Direct cached ProcessEvent applies FVector scale"),
		Actor->GetActorScale3D().Equals(TargetScale, 0.001));
	Actor->SetActorScale3D(InitialScale);

	FAvidScriptBindingRuntimeRecordingJournal RejectingJournal(false);
	DirectContext.HostEffectJournal = &RejectingJournal;
	TestFalse(
		TEXT("Candidate journal rejection prevents reflected SetActorScale3D"),
		Package->Dispatch(DirectCall, DirectContext, DirectScratch, DirectResult));
	TestEqual(TEXT("Candidate journal receives one prepare call"), RejectingJournal.PrepareCallCount, 1);
	TestEqual(TEXT("Candidate journal receives the invocation registry"), RejectingJournal.LastRegistry, &Registry);
	TestEqual(TEXT("Candidate journal receives the Actor handle"), RejectingJournal.LastHandle, ActorHandle);
	TestEqual(TEXT("Candidate journal receives the Actor target"), RejectingJournal.LastTarget, static_cast<UObject*>(Actor));
	TestEqual(
		TEXT("Candidate journal receives the generated Actor transform effect"),
		RejectingJournal.LastEffect,
		EAvidScriptBindingReloadEffect::ActorTransform);
	TestTrue(
		TEXT("Candidate journal rejection preserves Actor scale before ProcessEvent"),
		Actor->GetActorScale3D().Equals(InitialScale, 0.001));
	TestTrue(
		TEXT("Candidate journal failure keeps its stable category"),
		DirectResult.Details.Contains(TEXT("test_host_effect_rejected"), ESearchCase::CaseSensitive));

	FString UnsupportedDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult UnsupportedGenerateResult;
	TSharedPtr<const FAvidScriptBindingPackage> UnsupportedPackage;
	FAvidScriptBindingPackageLoadResult UnsupportedLoadResult;
	if (!TestTrue(
			TEXT("Unsupported SetVisibility descriptor generates"),
			FAvidScriptEditorBindingDescriptorGenerator::Generate(
				TEXT("avidscript.test.reload_unsupported"),
				{ { TEXT("/Script/Engine.SceneComponent"), TEXT("SetVisibility") } },
				UnsupportedDescriptorJson,
				UnsupportedGenerateResult))
		|| !TestTrue(
			TEXT("Unsupported SetVisibility package loads"),
			FAvidScriptBindingPackage::LoadDescriptor(
				UnsupportedDescriptorJson,
				UnsupportedPackage,
				UnsupportedLoadResult)))
	{
		AddError(UnsupportedGenerateResult.ErrorMessage + TEXT("\n") + UnsupportedLoadResult.ErrorDetails);
		return false;
	}
	const FAvidScriptVmDynamicImport* SetVisibilityImport = UnsupportedPackage->GetVmPackage().Imports.GetData();
	if (!TestNotNull(TEXT("Unsupported package exposes SetVisibility"), SetVisibilityImport))
	{
		return false;
	}
	const uint64 VisibilityArguments[] = {
		RootHandle.Slot,
		RootHandle.Generation,
		0,
		0
	};
	FAvidScriptDynamicHostCall VisibilityCall;
	VisibilityCall.BindingOrdinal = SetVisibilityImport->Ordinal;
	VisibilityCall.Arguments = MakeArrayView(VisibilityArguments);
	TArray<uint8> VisibilityScratch;
	VisibilityScratch.SetNumUninitialized(UnsupportedPackage->GetRequiredScratchSize());
	FAvidScriptDynamicHostCallResult VisibilityResult;
	FAvidScriptBindingRuntimeRecordingJournal PermissiveJournal(true);
	FAvidScriptBindingInvocationContext VisibilityContext = DirectContext;
	VisibilityContext.OwnerHandle = RootHandle;
	VisibilityContext.HostEffectJournal = &PermissiveJournal;
	RootComponent->SetVisibility(true);
	TestFalse(
		TEXT("Candidate rejects an unsupported reflected mutation before ProcessEvent"),
		UnsupportedPackage->Dispatch(
			VisibilityCall,
			VisibilityContext,
			VisibilityScratch,
			VisibilityResult));
	TestTrue(TEXT("Rejected unsupported mutation keeps the component visible"), RootComponent->IsVisible());
	TestTrue(
		TEXT("Unsupported mutation reports a stable category"),
		VisibilityResult.Details.Contains(TEXT("binding_reload_effect_unsupported"), ESearchCase::CaseSensitive));
	VisibilityContext.HostEffectJournal = nullptr;
	TestTrue(
		TEXT("Live context preserves existing SetVisibility behavior"),
		UnsupportedPackage->Dispatch(
			VisibilityCall,
			VisibilityContext,
			VisibilityScratch,
			VisibilityResult));
	TestFalse(TEXT("Live SetVisibility reaches ProcessEvent"), RootComponent->IsVisible());

	FAvidScriptWasmHostContext ReadOnlyContext;
	ReadOnlyContext.ObjectRegistry = &Registry;
	ReadOnlyContext.OwnerHandle = ActorHandle;
	ReadOnlyContext.ActorWritePolicy = EAvidScriptActorWritePolicy::ReadOnly;

	FAvidScriptWasmRuntimeInstance ReadOnlyRuntime;
	ReadOnlyRuntime.SetHostContext(ReadOnlyContext);
	FAvidScriptWasmSmokeResult RuntimeResult;
	TestTrue(
		TEXT("Read-only runtime links the reflected binding package"),
		ReadOnlyRuntime.LoadModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			TEXT("p42_4_reflected_set_actor_scale_read_only"),
			Package,
			RuntimeResult));
	TestFalse(
		TEXT("Read-only runtime rejects the reflected SetActorScale3D call"),
		ReadOnlyRuntime.BeginPlay(RuntimeResult));
	TestTrue(
		TEXT("Denied reflected write keeps the actor scale unchanged"),
		Actor->GetActorScale3D().Equals(InitialScale, 0.001));
	ReadOnlyRuntime.Unload();

	FAvidScriptWasmHostContext WritableContext = ReadOnlyContext;
	WritableContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	FAvidScriptWasmRuntimeInstance WritableRuntime;
	WritableRuntime.SetHostContext(WritableContext);
	TestTrue(
		TEXT("Writable runtime links the same immutable reflected binding package"),
		WritableRuntime.LoadModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			TEXT("p42_4_reflected_set_actor_scale_writable"),
			Package,
			RuntimeResult));
	TestTrue(
		TEXT("BeginPlay executes the generated dynamic reflected import"),
		WritableRuntime.BeginPlay(RuntimeResult));
	const FVector AppliedScale = Actor->GetActorScale3D();
	TestTrue(
		*FString::Printf(
			TEXT("Reflected ProcessEvent applies FVector scale from WASM | actual=(%.6f, %.6f, %.6f)"),
			AppliedScale.X,
			AppliedScale.Y,
			AppliedScale.Z),
		AppliedScale.Equals(TargetScale, 0.001));
	TestEqual(TEXT("Dynamic reflected import reports success"), RuntimeResult.LastHostImportResult, 1);
	TestTrue(
		TEXT("Lifecycle call observed owner imports and the dynamic reflected import"),
		RuntimeResult.HostImportCallCount >= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeGeneratedCSharpLifecycleTest,
	"AvidScript.Editor.BindingRuntime.GeneratedCSharpLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeGeneratedCSharpLifecycleTest::RunTest(const FString& Parameters)
{
	FString SemanticCacheRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P43_5/GeneratedBindingLifecycle/CSharpSemanticCache/v1")));
	FPaths::NormalizeFilename(SemanticCacheRoot);
	IFileManager::Get().DeleteDirectory(*SemanticCacheRoot, false, true);

	FAvidScriptEditorCSharpBuildResult ColdBuildResult;
	if (!TestTrue(
		TEXT("Cold custom C# lifecycle builds and publishes semantic cache state"),
		BuildAvidScriptGeneratedBindingLifecycle(SemanticCacheRoot, ColdBuildResult)))
	{
		AddError(ColdBuildResult.ErrorMessage + TEXT("\n") + ColdBuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Cold lifecycle performs bootstrap and final builds"), ColdBuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Cold lifecycle invokes the C# frontend once"), ColdBuildResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Cold lifecycle invokes C# semantic analysis once"), ColdBuildResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Cold lifecycle invokes Guest IR twice"), ColdBuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Cold lifecycle invokes WASM backend twice"), ColdBuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Cold lifecycle records semantic cache miss"), ColdBuildResult.SemanticCacheLookup, FString(TEXT("miss")));
	TestTrue(TEXT("Cold lifecycle publishes semantic cache entry"), ColdBuildResult.bSemanticCachePublished);

	FAvidScriptWasmReloadManifest ColdManifest;
	TArray<uint8> ColdBytecode;
	if (!AcceptAvidScriptGeneratedBindingLifecycleBuild(
			*this,
			TEXT("Cold lifecycle"),
			ColdBuildResult,
			ColdManifest,
			ColdBytecode))
	{
		return false;
	}

	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!TestTrue(
		TEXT("Warm custom C# lifecycle reuses semantic cache and remains loadable"),
		BuildAvidScriptGeneratedBindingLifecycle(SemanticCacheRoot, BuildResult)))
	{
		AddError(BuildResult.ErrorMessage + TEXT("\n") + BuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Generated lifecycle performs bootstrap and final builds"), BuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Warm lifecycle skips the C# frontend"), BuildResult.FrontendInvocationCount, 0);
	TestEqual(TEXT("Warm lifecycle skips C# semantic analysis"), BuildResult.SemanticInvocationCount, 0);
	TestEqual(TEXT("Warm lifecycle still invokes Guest IR twice"), BuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Warm lifecycle still invokes WASM backend twice"), BuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Warm lifecycle records semantic cache hit"), BuildResult.SemanticCacheLookup, FString(TEXT("hit")));
	TestFalse(TEXT("Warm lifecycle does not republish semantic cache entry"), BuildResult.bSemanticCachePublished);

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	if (!AcceptAvidScriptGeneratedBindingLifecycleBuild(
			*this,
			TEXT("Warm lifecycle"),
			BuildResult,
			Manifest,
			Bytecode))
	{
		return false;
	}

	FString ManifestJson;
	TSharedPtr<FJsonObject> ManifestObject;
	const TSharedPtr<FJsonObject>* BindingPackageObject = nullptr;
	TestTrue(
		TEXT("Generated C# manifest can be read for tamper validation"),
		FFileHelper::LoadFileToString(ManifestJson, *BuildResult.ManifestPath));
	const TSharedRef<TJsonReader<>> ManifestReader = TJsonReaderFactory<>::Create(ManifestJson);
	TestTrue(
		TEXT("Generated C# manifest parses for tamper validation"),
		FJsonSerializer::Deserialize(ManifestReader, ManifestObject));
	if (!TestTrue(
		TEXT("Generated C# manifest exposes binding package metadata"),
		ManifestObject.IsValid()
			&& ManifestObject->TryGetObjectField(TEXT("binding_package"), BindingPackageObject))
		|| BindingPackageObject == nullptr
		|| !BindingPackageObject->IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("Generated C# manifest records a two-binding runtime profile"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("profile_import_count"))),
		2);
	TestEqual(
		TEXT("Generated C# manifest records two used binding stable identities"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("used_import_count"))),
		2);
	(*BindingPackageObject)->SetStringField(
		TEXT("descriptor_sha256"),
		TEXT("0000000000000000000000000000000000000000000000000000000000000000"));
	FString TamperedManifestJson;
	const TSharedRef<TJsonWriter<>> ManifestWriter = TJsonWriterFactory<>::Create(&TamperedManifestJson);
	TestTrue(
		TEXT("Tampered C# manifest serializes"),
		FJsonSerializer::Serialize(ManifestObject.ToSharedRef(), ManifestWriter));
	const FString TamperedManifestPath = FPaths::Combine(
		FPaths::GetPath(BuildResult.ManifestPath),
		TEXT("generated_binding_lifecycle.tampered.avidscript.json"));
	TestTrue(
		TEXT("Tampered C# manifest writes"),
		FFileHelper::SaveStringToFile(TamperedManifestJson, *TamperedManifestPath));
	FAvidScriptWasmReloadManifest TamperedManifest;
	TArray<uint8> TamperedBytecode;
	FAvidScriptWasmReloadManifestLoadResult TamperedLoadResult;
	TestFalse(
		TEXT("Runtime transaction rejects a tampered binding descriptor hash"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			TamperedManifestPath,
			TamperedManifest,
			TamperedBytecode,
			TamperedLoadResult));
	TestEqual(
		TEXT("Tampered binding descriptor hash has a stable category"),
		TamperedLoadResult.ErrorCategory,
		FString(TEXT("binding_package_hash_mismatch")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeProjectCSharpGameplayWorkspaceTest,
	"AvidScript.Editor.BindingRuntime.ProjectCSharpGameplayWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeProjectCSharpGameplayWorkspaceTest::RunTest(const FString& Parameters)
{
	FString TestSavedRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P44/GameplayWorkspace")));
	FString GeneratedRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("AvidScript/Tests/P44/GameplayWorkspace/CSharpWorkspace")));
	FString OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest/Tests/P44/GameplayWorkspace")));
	FPaths::NormalizeFilename(TestSavedRoot);
	FPaths::NormalizeFilename(GeneratedRoot);
	FPaths::NormalizeFilename(OutputRoot);
	IFileManager::Get().DeleteDirectory(*TestSavedRoot, false, true);
	IFileManager::Get().DeleteDirectory(*GeneratedRoot, false, true);
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);

	FAvidScriptEditorCSharpWorkspaceConfig WorkspaceConfig;
	WorkspaceConfig.WorkspaceRoot = FPaths::Combine(TestSavedRoot, TEXT("Workspace"));
	WorkspaceConfig.GeneratedRoot = GeneratedRoot;
	WorkspaceConfig.BindingPackageRoot = FPaths::Combine(GeneratedRoot, TEXT("BindingPackages"));
	WorkspaceConfig.OutputRoot = OutputRoot;
	FAvidScriptEditorCSharpWorkspaceResult WorkspaceResult;
	if (!TestTrue(
			TEXT("Project C# gameplay workspace is created in isolation"),
			FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(
				WorkspaceConfig,
				WorkspaceResult)))
	{
		AddError(WorkspaceResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Project C# gameplay workspace creates four user files"), WorkspaceResult.CreatedUserFileCount, 4);

	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	if (!TestTrue(
			TEXT("Generated project C# gameplay profile loads"),
			FAvidScriptEditorCSharpProfileService::LoadProfile(
				WorkspaceResult.ProfilePath,
				ProfileResult)))
	{
		AddError(ProfileResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Profile owns the workspace source"), ProfileResult.BuildConfig.SourcePath, WorkspaceResult.SourcePath);
	TestEqual(TEXT("Profile owns the workspace project"), ProfileResult.BuildConfig.ProjectPath, WorkspaceResult.ProjectPath);
	ProfileResult.BuildConfig.SemanticCacheRoot = FPaths::Combine(
		TestSavedRoot,
		TEXT("CSharpSemanticCache/v1"));

	FAvidScriptEditorCSharpBuildResult ColdBuildResult;
	if (!TestTrue(
			TEXT("Cold project C# gameplay build succeeds"),
			FAvidScriptEditorCSharpBuildService::BuildProfile(
				ProfileResult.BuildConfig,
				ColdBuildResult)))
	{
		AddError(ColdBuildResult.ErrorMessage + TEXT("\n") + ColdBuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Cold gameplay build performs two build passes"), ColdBuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Cold gameplay build invokes Frontend once"), ColdBuildResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Cold gameplay build invokes Semantic once"), ColdBuildResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Cold gameplay build invokes Guest IR twice"), ColdBuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Cold gameplay build invokes WASM twice"), ColdBuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Cold gameplay build records cache miss"), ColdBuildResult.SemanticCacheLookup, FString(TEXT("miss")));
	if (!AcceptAvidScriptProjectGameplayWorkspaceBuild(
			*this,
			TEXT("Cold gameplay workspace"),
			WorkspaceResult,
			ColdBuildResult))
	{
		return false;
	}

	FAvidScriptEditorCSharpBuildResult WarmBuildResult;
	if (!TestTrue(
			TEXT("Warm project C# gameplay build succeeds"),
			FAvidScriptEditorCSharpBuildService::BuildProfile(
				ProfileResult.BuildConfig,
				WarmBuildResult)))
	{
		AddError(WarmBuildResult.ErrorMessage + TEXT("\n") + WarmBuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Warm gameplay build performs two build passes"), WarmBuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Warm gameplay build skips Frontend"), WarmBuildResult.FrontendInvocationCount, 0);
	TestEqual(TEXT("Warm gameplay build skips Semantic"), WarmBuildResult.SemanticInvocationCount, 0);
	TestEqual(TEXT("Warm gameplay build still invokes Guest IR twice"), WarmBuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Warm gameplay build still invokes WASM twice"), WarmBuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Warm gameplay build records cache hit"), WarmBuildResult.SemanticCacheLookup, FString(TEXT("hit")));
	return AcceptAvidScriptProjectGameplayWorkspaceBuild(
		*this,
		TEXT("Warm gameplay workspace"),
		WorkspaceResult,
		WarmBuildResult);
}

#endif // WITH_DEV_AUTOMATION_TESTS
