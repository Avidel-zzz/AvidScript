#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmReload.h"

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptObjectRegistryTestTypes.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
FString GetAvidScriptFrontendManifestPath()
{
	FString ManifestPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptGenerated"),
		TEXT("actor_set_location"),
		TEXT("actor_set_location.avidscript.json"));
	ManifestPath = FPaths::ConvertRelativePathToFull(ManifestPath);
	FPaths::NormalizeFilename(ManifestPath);
	return ManifestPath;
}

bool CreateAvidScriptFrontendWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptFrontendWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyAvidScriptFrontendWorld(UWorld*& World)
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFrontendActorSetLocationSmokeTest,
	"AvidScript.Guest.AvidScript.FrontendActorSetLocationSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptFrontendActorSetLocationSmokeTest::RunTest(const FString& Parameters)
{
	const FString ManifestPath = GetAvidScriptFrontendManifestPath();
	if (!FPaths::FileExists(ManifestPath))
	{
		AddWarning(FString::Printf(
			TEXT("AvidScript frontend manifest is missing. Run BuildAvidScriptActor.ps1 before using this as a true frontend smoke. manifest=%s"),
			*ManifestPath));
		return true;
	}

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(ManifestPath, Manifest, Bytecode, ManifestLoadResult))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return true;
	}

	TestEqual(TEXT("Manifest language is AvidScript"), Manifest.Language, FString(TEXT("avidscript")));
	TestEqual(TEXT("Manifest module id"), Manifest.ModuleId, FString(TEXT("actor_set_location")));
	TestTrue(TEXT("Frontend bytecode is loaded"), Bytecode.Num() > 0);

	UWorld* World = nullptr;
	if (!CreateAvidScriptFrontendWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript frontend test world."));
		DestroyAvidScriptFrontendWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyAvidScriptFrontendWorld(World);
		return true;
	}

	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers"), RegisterResult.bSucceeded);
	TestEqual(TEXT("AvidScript sample slot matches first registry handle"), ActorHandle.Slot, static_cast<uint32>(1));
	TestEqual(TEXT("AvidScript sample generation matches first registry handle"), ActorHandle.Generation, static_cast<uint32>(1));

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptWasmReloadSession Session;
	Session.SetHostContext(HostContext);

	FAvidScriptWasmReloadResult ReloadResult;
	if (!Session.LoadInitialModule(Bytecode.GetData(), Bytecode.Num(), Manifest, ReloadResult))
	{
		AddError(ReloadResult.ErrorMessage);
		DestroyAvidScriptFrontendWorld(World);
		return true;
	}

	TestTrue(TEXT("AvidScript frontend module loads"), ReloadResult.bSucceeded);
	TestEqual(TEXT("AvidScript frontend BeginPlay moves actor"), Actor->GetActorLocation(), FVector(123.0, 456.0, 789.0));

	DestroyAvidScriptFrontendWorld(World);
	return true;
}

#endif
