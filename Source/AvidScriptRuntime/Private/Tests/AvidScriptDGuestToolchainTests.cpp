#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmModuleLoader.h"
#include "AvidScriptWasmRuntime.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FString GetDGuestActorSetLocationArtifactPath()
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptDGuest"),
		TEXT("actor_set_location_guest.wasm"));
}

bool IsDGuestMissingToolchainDocumented()
{
	const FString PhaseDocPath = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript"),
		TEXT("Docs"),
		TEXT("Phase5"),
		TEXT("P5.0_D_Guest_Toolchain_Spike.md"));

	FString PhaseDoc;
	if (!FFileHelper::LoadFileToString(PhaseDoc, *PhaseDocPath))
	{
		return false;
	}

	return PhaseDoc.Contains(TEXT("missing_toolchain"))
		&& PhaseDoc.Contains(TEXT("ldc2"))
		&& PhaseDoc.Contains(TEXT("wasm-ld"));
}

bool CreateDGuestToolchainWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptDGuestToolchainWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyDGuestToolchainWorld(UWorld*& World)
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
	FAvidScriptDGuestExternalActorSetLocationSmokeTest,
	"AvidScript.Guest.D.ExternalActorSetLocationSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptDGuestExternalActorSetLocationSmokeTest::RunTest(const FString& Parameters)
{
	const FString ArtifactPath = GetDGuestActorSetLocationArtifactPath();
	if (!FPaths::FileExists(ArtifactPath))
	{
		if (IsDGuestMissingToolchainDocumented())
		{
			AddWarning(FString::Printf(
				TEXT("D guest WASM artifact is missing because local D/WASM toolchain is documented as missing. artifact=%s"),
				*ArtifactPath));
			return true;
		}

		AddError(FString::Printf(
			TEXT("D guest WASM artifact is missing without a documented missing-toolchain blocker. artifact=%s"),
			*ArtifactPath));
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateDGuestToolchainWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript D guest toolchain test world."));
		DestroyDGuestToolchainWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyDGuestToolchainWorld(World);
		return true;
	}

	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers"), RegisterResult.bSucceeded);
	TestEqual(TEXT("D guest sample slot matches first registry handle"), ActorHandle.Slot, static_cast<uint32>(1));
	TestEqual(TEXT("D guest sample generation matches first registry handle"), ActorHandle.Generation, static_cast<uint32>(1));

	TArray<uint8> LoadedBytes;
	FAvidScriptWasmModuleLoadResult LoadResult;
	const bool bLoadedFromFile = FAvidScriptWasmModuleLoader::LoadFromFile(ArtifactPath, LoadedBytes, LoadResult);
	if (!bLoadedFromFile)
	{
		AddError(LoadResult.ErrorMessage);
	}

	TestTrue(TEXT("D guest artifact loads from disk"), bLoadedFromFile);

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);

	FAvidScriptWasmSmokeResult RuntimeResult;
	const bool bRuntimeLoaded = Runtime.LoadModule(
		LoadedBytes.GetData(),
		LoadedBytes.Num(),
		TEXT("d_guest_actor_set_location"),
		RuntimeResult);
	if (!bRuntimeLoaded)
	{
		AddError(RuntimeResult.ErrorMessage);
	}

	TestTrue(TEXT("Runtime loads D guest artifact"), bRuntimeLoaded);

	const bool bBeginPlaySucceeded = bRuntimeLoaded && Runtime.BeginPlay(RuntimeResult);
	if (!bBeginPlaySucceeded)
	{
		AddError(RuntimeResult.ErrorMessage);
	}

	TestTrue(TEXT("D guest BeginPlay calls actor import"), bBeginPlaySucceeded);
	TestEqual(TEXT("Actor moved by D guest WASM"), Actor->GetActorLocation(), FVector(123.0, 456.0, 789.0));

	DestroyDGuestToolchainWorld(World);
	return true;
}

#endif
