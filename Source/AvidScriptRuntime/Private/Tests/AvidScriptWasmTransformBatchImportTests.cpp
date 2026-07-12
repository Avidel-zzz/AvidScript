#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmRuntime.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

namespace
{
bool CreateRuntimeBatchWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptRuntimeBatchWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyRuntimeBatchWorld(UWorld*& World)
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

AActor* SpawnRuntimeBatchActor(UWorld* World, const FTransform& Transform)
{
	AActor* Actor = World != nullptr ? World->SpawnActor<AActor>() : nullptr;
	if (Actor == nullptr)
	{
		return nullptr;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Actor, USceneComponent::StaticClass());
	if (Root == nullptr)
	{
		return nullptr;
	}
	Actor->SetRootComponent(Root);
	Actor->AddInstanceComponent(Root);
	Root->RegisterComponentWithWorld(World);
	Actor->SetActorTransform(Transform, false, nullptr, ETeleportType::TeleportPhysics);
	return Actor;
}

void AppendHandleCells(const FAvidScriptObjectHandle& Handle, TArray<uint32>& OutCells)
{
	OutCells.Add(Handle.Slot);
	OutCells.Add(Handle.Generation);
}

bool RegisterRuntimeBatchActor(
	FAutomationTestBase& Test,
	FAvidScriptObjectRegistry& Registry,
	AActor* Actor,
	FAvidScriptObjectHandle& OutHandle)
{
	FAvidScriptObjectHandleResult RegisterResult;
	OutHandle = Registry.RegisterObject(Actor, RegisterResult);
	Test.TestTrue(TEXT("runtime batch actor registers"), RegisterResult.bSucceeded);
	Test.TestTrue(TEXT("runtime batch handle is valid"), OutHandle.IsValid());
	return RegisterResult.bSucceeded && OutHandle.IsValid();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeTransformBatchDispatchTest,
	"AvidScript.Runtime.Binding.TransformBatchDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeTransformBatchDispatchTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateRuntimeBatchWorld(World))
	{
		AddError(TEXT("Failed to create runtime batch world."));
		return false;
	}

	const FTransform FirstTransform(FRotator(10.0, 20.0, 30.0), FVector(1.0, 2.0, 3.0), FVector(1.0, 2.0, 3.0));
	const FTransform SecondTransform(FRotator(40.0, 50.0, 60.0), FVector(4.0, 5.0, 6.0), FVector(4.0, 5.0, 6.0));
	AActor* FirstActor = SpawnRuntimeBatchActor(World, FirstTransform);
	AActor* SecondActor = SpawnRuntimeBatchActor(World, SecondTransform);
	if (FirstActor == nullptr || SecondActor == nullptr)
	{
		AddError(TEXT("Failed to spawn runtime batch actors."));
		DestroyRuntimeBatchWorld(World);
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandle FirstHandle;
	FAvidScriptObjectHandle SecondHandle;
	if (!RegisterRuntimeBatchActor(*this, Registry, FirstActor, FirstHandle) ||
		!RegisterRuntimeBatchActor(*this, Registry, SecondActor, SecondHandle))
	{
		DestroyRuntimeBatchWorld(World);
		return false;
	}

	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	Runtime.SetHostContext(HostContext);

	TArray<uint32> InputCells;
	AppendHandleCells(FirstHandle, InputCells);
	AppendHandleCells(SecondHandle, InputCells);
	TArray<float> OutputFloats;
	OutputFloats.Init(-777.0f, 18);
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ActorGetTransformBatch;
	Call.IntArgs[0] = 2;
	Call.InputCells = InputCells;
	Call.OutputFloats = OutputFloats;
	FAvidScriptHostCallResult Result;
	TestTrue(TEXT("runtime dispatches transform batch"), Runtime.DispatchHostCall(Call, Result));
	TestTrue(TEXT("runtime marks transform batch successful"), Result.bSucceeded);
	TestEqual(TEXT("runtime returns processed transform count"), Result.ReturnValue, 2);
	TestEqual(TEXT("first location x is published"), OutputFloats[0], 1.0f);
	TestEqual(TEXT("first location y is published"), OutputFloats[1], 2.0f);
	TestEqual(TEXT("first location z is published"), OutputFloats[2], 3.0f);
	TestEqual(TEXT("first rotation pitch is published"), OutputFloats[3], 10.0f);
	TestEqual(TEXT("first rotation yaw is published"), OutputFloats[4], 20.0f);
	TestEqual(TEXT("first rotation roll is published"), OutputFloats[5], 30.0f);
	TestEqual(TEXT("first scale z is published"), OutputFloats[8], 3.0f);
	TestEqual(TEXT("second location x is published"), OutputFloats[9], 4.0f);
	TestEqual(TEXT("second rotation yaw is published"), OutputFloats[13], 50.0f);
	TestEqual(TEXT("second scale z is published"), OutputFloats[17], 6.0f);

	InputCells[3] += 1;
	OutputFloats.Init(-333.0f, 18);
	Call.InputCells = InputCells;
	Call.OutputFloats = OutputFloats;
	Result = FAvidScriptHostCallResult();
	TestFalse(TEXT("stale middle handle rejects transform batch"), Runtime.DispatchHostCall(Call, Result));
	TestFalse(TEXT("failed transform batch is not successful"), Result.bSucceeded);
	TestTrue(TEXT("failed transform batch reports failing index"), Result.Details.Contains(TEXT("index=1")));
	for (const float OutputValue : OutputFloats)
	{
		TestEqual(TEXT("failed transform batch publishes no partial output"), OutputValue, -333.0f);
	}

	DestroyRuntimeBatchWorld(World);
	return true;
}

#endif