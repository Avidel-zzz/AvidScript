#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmRuntime.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

namespace
{
const uint8 GAvidScriptActorImportLinkWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x18, 0x04, 0x60, 0x03, 0x7f, 0x7f, 0x7f,
	0x01, 0x7f, 0x60, 0x05, 0x7f, 0x7f, 0x7d, 0x7d,
	0x7d, 0x01, 0x7f, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x02, 0x41, 0x02, 0x0a, 0x61, 0x76,
	0x69, 0x64, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74,
	0x12, 0x61, 0x63, 0x74, 0x6f, 0x72, 0x5f, 0x67,
	0x65, 0x74, 0x5f, 0x6c, 0x6f, 0x63, 0x61, 0x74,
	0x69, 0x6f, 0x6e, 0x00, 0x00, 0x0a, 0x61, 0x76,
	0x69, 0x64, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74,
	0x12, 0x61, 0x63, 0x74, 0x6f, 0x72, 0x5f, 0x73,
	0x65, 0x74, 0x5f, 0x6c, 0x6f, 0x63, 0x61, 0x74,
	0x69, 0x6f, 0x6e, 0x00, 0x01, 0x03, 0x03, 0x02,
	0x02, 0x03, 0x07, 0x25, 0x02, 0x12, 0x61, 0x76,
	0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65,
	0x67, 0x69, 0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79,
	0x00, 0x02, 0x0c, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x74, 0x69, 0x63, 0x6b, 0x00,
	0x03, 0x0a, 0x07, 0x02, 0x02, 0x00, 0x0b, 0x02,
	0x00, 0x0b
};

bool CreateWasmActorImportWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptWasmActorImportWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyWasmActorImportWorld(UWorld*& World)
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
	FAvidScriptWasmActorImportLinkSmokeTest,
	"AvidScript.Runtime.WasmActor.ImportLinkSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmActorImportLinkSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;

	const bool bLoaded = Runtime.LoadModule(
		GAvidScriptActorImportLinkWasmModule,
		UE_ARRAY_COUNT(GAvidScriptActorImportLinkWasmModule),
		TEXT("actor_import_link"),
		Result);

	if (!bLoaded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("WASM guest links actor host imports"), bLoaded);
	TestTrue(TEXT("Linked module can call BeginPlay"), bLoaded && Runtime.BeginPlay(Result));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmActorImportDirectHandlerSmokeTest,
	"AvidScript.Runtime.WasmActor.DirectHandlerSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmActorImportDirectHandlerSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateWasmActorImportWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript WASM actor import test world."));
		DestroyWasmActorImportWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyWasmActorImportWorld(World);
		return true;
	}

	const FVector InitialLocation(12.0, 34.0, 56.0);
	Actor->SetActorLocation(InitialLocation);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers"), RegisterResult.bSucceeded);

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);

	FVector ReadLocation = FVector::ZeroVector;
	TestEqual(
		TEXT("Actor get import succeeds"),
		Runtime.HandleActorGetLocationImport(ActorHandle.Slot, ActorHandle.Generation, ReadLocation),
		1);
	TestEqual(TEXT("Read location matches actor"), ReadLocation, InitialLocation);

	const FVector TargetLocation(123.0, 456.0, 789.0);
	TestEqual(
		TEXT("Actor set import succeeds"),
		Runtime.HandleActorSetLocationImport(ActorHandle.Slot, ActorHandle.Generation, TargetLocation),
		1);
	TestEqual(TEXT("Actor moved by import handler"), Actor->GetActorLocation(), TargetLocation);

	DestroyWasmActorImportWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmActorImportMissingContextSmokeTest,
	"AvidScript.Runtime.WasmActor.MissingContextSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmActorImportMissingContextSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;

	FVector ReadLocation = FVector::ZeroVector;
	TestEqual(
		TEXT("Missing host context fails closed on get"),
		Runtime.HandleActorGetLocationImport(1, 1, ReadLocation),
		0);
	TestEqual(TEXT("Failed get leaves zero vector"), ReadLocation, FVector::ZeroVector);

	TestEqual(
		TEXT("Missing host context fails closed on set"),
		Runtime.HandleActorSetLocationImport(1, 1, FVector(1.0, 2.0, 3.0)),
		0);

	return true;
}

#endif
