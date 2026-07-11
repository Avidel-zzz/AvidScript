#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptActorBinding.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptSceneComponentBinding.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace
{
bool CreateSceneComponentBindingWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptSceneComponentBindingWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroySceneComponentBindingWorld(UWorld*& World)
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
	FAvidScriptSceneComponentBindingWorldLocationSmokeTest,
	"AvidScript.Binding.SceneComponent.WorldLocationSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSceneComponentBindingWorldLocationSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateSceneComponentBindingWorld(World))
	{
		AddError(TEXT("Failed to create SceneComponent binding world."));
		return true;
	}

	AAvidScriptActorBindingTestActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroySceneComponentBindingWorld(World);
		return true;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers"), RegisterResult.bSucceeded);

	FAvidScriptObjectHandle ComponentHandle;
	FAvidScriptActorBindingResult RootResult;
	TestTrue(
		TEXT("Actor returns a handle-backed root component"),
		FAvidScriptActorBinding::GetRootComponentHandle(Registry, ActorHandle, ComponentHandle, RootResult));
	TestTrue(TEXT("Root component handle is valid"), ComponentHandle.IsValid());
	TestNotEqual(TEXT("Root component uses a distinct object slot"), ComponentHandle.Slot, ActorHandle.Slot);
	const int32 SlotsAfterFirstLookup = Registry.NumSlots();
	FAvidScriptObjectHandle RepeatedComponentHandle;
	FAvidScriptActorBindingResult RepeatedRootResult;
	TestTrue(
		TEXT("Repeated root lookup succeeds"),
		FAvidScriptActorBinding::GetRootComponentHandle(Registry, ActorHandle, RepeatedComponentHandle, RepeatedRootResult));
	TestEqual(TEXT("Repeated root lookup reuses the handle"), RepeatedComponentHandle.ToUInt64(), ComponentHandle.ToUInt64());
	TestEqual(TEXT("Repeated root lookup does not grow registry"), Registry.NumSlots(), SlotsAfterFirstLookup);

	const FVector InitialLocation(11.0, 22.0, 33.0);
	Actor->GetRootComponent()->SetWorldLocation(InitialLocation);
	FVector ReadLocation = FVector::ZeroVector;
	FAvidScriptSceneComponentBindingResult ReadResult;
	TestTrue(
		TEXT("SceneComponent world location is readable"),
		FAvidScriptSceneComponentBinding::GetWorldLocation(Registry, ComponentHandle, ReadLocation, ReadResult));
	TestTrue(TEXT("Read location matches component"), ReadLocation.Equals(InitialLocation, 0.01));

	const FVector TargetLocation(101.0, 202.0, 303.0);
	FAvidScriptSceneComponentBindingResult WriteResult;
	TestTrue(
		TEXT("SceneComponent world location is writable"),
		FAvidScriptSceneComponentBinding::SetWorldLocation(
			Registry,
			ComponentHandle,
			TargetLocation,
			EAvidScriptActorWritePolicy::AllowWrites,
			WriteResult));
	TestTrue(TEXT("Component moves to target"), Actor->GetRootComponent()->GetComponentLocation().Equals(TargetLocation, 0.01));

	FAvidScriptSceneComponentBindingResult DeniedResult;
	TestFalse(
		TEXT("Read-only policy denies component writes"),
		FAvidScriptSceneComponentBinding::SetWorldLocation(
			Registry,
			ComponentHandle,
			FVector::ZeroVector,
			EAvidScriptActorWritePolicy::ReadOnly,
			DeniedResult));
	TestEqual(TEXT("Denied write reports policy category"), DeniedResult.ErrorCategory, FString(TEXT("write_denied")));

	FAvidScriptObjectHandleResult ReleaseResult;
	TestTrue(TEXT("Component handle releases"), Registry.ReleaseHandle(ComponentHandle, ReleaseResult));
	ReadLocation = FVector(9.0, 9.0, 9.0);
	FAvidScriptSceneComponentBindingResult StaleResult;
	TestFalse(
		TEXT("Released component handle fails closed"),
		FAvidScriptSceneComponentBinding::GetWorldLocation(Registry, ComponentHandle, ReadLocation, StaleResult));
	TestEqual(TEXT("Failed read zeros output"), ReadLocation, FVector::ZeroVector);

	DestroySceneComponentBindingWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptSceneComponentBindingTypeMismatchSmokeTest,
	"AvidScript.Binding.SceneComponent.TypeMismatchSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptSceneComponentBindingTypeMismatchSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	UObject* NonComponent = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle Handle = Registry.RegisterObject(NonComponent, RegisterResult);

	FVector Location(1.0, 2.0, 3.0);
	FAvidScriptSceneComponentBindingResult Result;
	TestFalse(
		TEXT("Non-component handle is rejected"),
		FAvidScriptSceneComponentBinding::GetWorldLocation(Registry, Handle, Location, Result));
	TestEqual(TEXT("Wrong type reports type mismatch"), Result.ErrorCategory, FString(TEXT("type_mismatch")));
	TestEqual(TEXT("Wrong type read zeros output"), Location, FVector::ZeroVector);
	return true;
}

#endif
