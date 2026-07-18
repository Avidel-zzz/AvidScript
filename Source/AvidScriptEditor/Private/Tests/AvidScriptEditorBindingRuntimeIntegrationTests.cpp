#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorCSharpBuildService.h"
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

bool CreateAvidScriptBindingRuntimeIntegrationWorld(UWorld*& OutWorld)
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
	OutWorld->InitializeActorsForPlay(FURL());
	return true;
}

void DestroyAvidScriptBindingRuntimeIntegrationWorld(UWorld*& World)
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
	return FAvidScriptEditorCSharpBuildService::BuildProfile(Config, OutBuildResult);
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
	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!TestTrue(
		TEXT("Custom C# lifecycle builds with an automatically published binding package"),
		BuildAvidScriptGeneratedBindingLifecycle(BuildResult)))
	{
		AddError(BuildResult.ErrorMessage + TEXT("\n") + BuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Generated lifecycle performs bootstrap and final builds"), BuildResult.BuildInvocationCount, 2);
	TestTrue(
		TEXT("Editor build records the complete authorization binding package"),
		FPaths::FileExists(BuildResult.AuthorizationBindingPackagePath));
	TestTrue(
		TEXT("Editor build records the minimal runtime binding package"),
		FPaths::FileExists(BuildResult.BindingPackagePath));
	TestNotEqual(
		TEXT("Generated lifecycle separates authorization and runtime packages"),
		BuildResult.AuthorizationBindingPackagePath,
		BuildResult.BindingPackagePath);

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!TestTrue(
		TEXT("Runtime transaction loads the generated C# manifest and binding package"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			BuildResult.ManifestPath,
			Manifest,
			Bytecode,
			ManifestLoadResult)))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return false;
	}
	if (!TestTrue(TEXT("Loaded manifest owns an immutable binding package"), Manifest.BindingPackage.IsValid()))
	{
		return false;
	}
	TestEqual(
		TEXT("Generated runtime package exposes only two reflected imports and cached plans"),
		Manifest.BindingPackage->GetVmPackage().Imports.Num(),
		2);
	int32 RequiredDynamicImportCount = 0;
	for (const FAvidScriptWasmRequiredImport& Import : Manifest.RequiredImports)
	{
		if (Import.ModuleName == TEXT("avidscript")
			&& Import.ImportName.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive))
		{
			++RequiredDynamicImportCount;
		}
	}
	TestEqual(
		TEXT("Generated C# lifecycle WASM keeps only its two reachable reflected imports"),
		RequiredDynamicImportCount,
		2);

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

	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("Generated C# lifecycle integration world is created"),
		CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!TestNotNull(TEXT("Generated C# lifecycle actor spawns"), Actor))
	{
		return false;
	}
	Actor->SetActorScale3D(FVector(1.0, 1.0, 1.0));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Generated C# lifecycle owner registers"), RegisterResult.bSucceeded);

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = ActorHandle;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptRuntimeSession Session;
	Session.SetHostContext(HostContext);
	FAvidScriptWasmReloadResult ReloadResult;
	if (!TestTrue(
		TEXT("C# BeginPlay activates through Runtime Session and WAMR"),
		Session.LoadInitialModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			Manifest,
			ReloadResult)))
	{
		AddError(ReloadResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("C# BeginPlay applies FVector scale through generated UE.Self binding"),
		Actor->GetActorScale3D().Equals(FVector(2.0, 3.0, 4.0), 0.001));

	FAvidScriptWasmSmokeResult TickResult;
	if (!TestTrue(TEXT("C# Tick executes through the live scheduler"), Session.TickLive(0.5f, TickResult)))
	{
		AddError(TickResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("C# Tick reads and writes FVector through cached reflected imports"),
		Actor->GetActorScale3D().Equals(FVector(2.5, 3.0, 4.0), 0.001));
	TestEqual(TEXT("Live scheduler records one C# Tick"), Session.GetLiveTickCallCount(), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
