#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptActorBinding.h"
#include "AvidScriptObjectRegistry.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace
{
bool CreateTransformBatchTestWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptTransformBatchTestWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyTransformBatchTestWorld(UWorld*& World)
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
	FAvidScriptActorTransformEmptyBatchSmokeTest,
	"AvidScript.Binding.ActorTransformBatch.EmptySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptActorTransformEmptyBatchSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	TArray<FAvidScriptObjectHandle> Handles;
	TArray<FAvidScriptActorTransformSnapshot> Snapshots;
	FAvidScriptActorTransformBatchResult Result;

	TestTrue(TEXT("Empty transform batch succeeds"), FAvidScriptActorBinding::GetActorTransforms(Registry, Handles, Snapshots, Result));
	TestTrue(TEXT("Empty transform batch reports success"), Result.bSucceeded);
	TestEqual(TEXT("Empty transform batch processes zero actors"), Result.ProcessedCount, 0);
	TestEqual(TEXT("Empty transform batch has no failed index"), Result.FailedIndex, INDEX_NONE);
	TestEqual(TEXT("Empty transform batch returns no snapshots"), Snapshots.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptActorTransformMultiBatchSmokeTest,
	"AvidScript.Binding.ActorTransformBatch.MultiActorSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptActorTransformMultiBatchSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateTransformBatchTestWorld(World))
	{
		AddError(TEXT("Failed to create transform batch test world."));
		return true;
	}

	FAvidScriptObjectRegistry Registry;
	TArray<FAvidScriptObjectHandle> Handles;
	TArray<FTransform> ExpectedTransforms;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AAvidScriptActorBindingTestActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
		TestNotNull(TEXT("Transform batch actor spawns"), Actor);
		if (Actor == nullptr)
		{
			DestroyTransformBatchTestWorld(World);
			return true;
		}

		const FTransform Transform(
			FRotator(5.0 * Index, 15.0 * Index, -2.0 * Index),
			FVector(100.0 * Index, 20.0 + Index, 30.0),
			FVector(1.0 + Index, 2.0 + Index, 3.0 + Index));
		Actor->SetActorTransform(Transform, false, nullptr, ETeleportType::TeleportPhysics);
		ExpectedTransforms.Add(Actor->GetActorTransform());

		FAvidScriptObjectHandleResult RegisterResult;
		Handles.Add(Registry.RegisterObject(Actor, RegisterResult));
		TestTrue(TEXT("Transform batch actor registers"), RegisterResult.bSucceeded);
	}

	TArray<FAvidScriptActorTransformSnapshot> Snapshots;
	FAvidScriptActorTransformBatchResult Result;
	TestTrue(TEXT("Multi-actor transform batch succeeds"), FAvidScriptActorBinding::GetActorTransforms(Registry, Handles, Snapshots, Result));
	TestEqual(TEXT("Multi-actor transform batch count"), Snapshots.Num(), Handles.Num());
	TestEqual(TEXT("Multi-actor processed count"), Result.ProcessedCount, Handles.Num());
	for (int32 Index = 0; Index < Snapshots.Num(); ++Index)
	{
		TestEqual(TEXT("Snapshot preserves handle"), Snapshots[Index].Handle, Handles[Index]);
		TestTrue(TEXT("Snapshot location matches"), Snapshots[Index].Location.Equals(ExpectedTransforms[Index].GetLocation(), 0.01));
		TestTrue(TEXT("Snapshot rotation matches"), Snapshots[Index].Rotation.Equals(ExpectedTransforms[Index].Rotator(), 0.01));
		TestTrue(TEXT("Snapshot scale matches"), Snapshots[Index].Scale3D.Equals(ExpectedTransforms[Index].GetScale3D(), 0.01));
	}

	DestroyTransformBatchTestWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptActorTransformAtomicFailureSmokeTest,
	"AvidScript.Binding.ActorTransformBatch.AtomicFailureSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptActorTransformAtomicFailureSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateTransformBatchTestWorld(World))
	{
		AddError(TEXT("Failed to create transform batch failure world."));
		return true;
	}

	AAvidScriptActorBindingTestActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ValidHandle = Registry.RegisterObject(Actor, RegisterResult);
	TArray<FAvidScriptObjectHandle> Handles{ValidHandle, FAvidScriptObjectHandle(), ValidHandle};
	TArray<FAvidScriptActorTransformSnapshot> Snapshots;
	FAvidScriptActorTransformBatchResult Result;

	TestFalse(TEXT("Invalid middle handle fails the batch"), FAvidScriptActorBinding::GetActorTransforms(Registry, Handles, Snapshots, Result));
	TestFalse(TEXT("Failed batch reports failure"), Result.bSucceeded);
	TestEqual(TEXT("Failed batch identifies the middle index"), Result.FailedIndex, 1);
	TestEqual(TEXT("Failed batch does not publish partial snapshots"), Snapshots.Num(), 0);
	TestEqual(TEXT("Failed batch reports invalid handle"), Result.ErrorCategory, FString(TEXT("invalid_handle")));

	DestroyTransformBatchTestWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptActorTransformBatchCapacitySmokeTest,
	"AvidScript.Binding.ActorTransformBatch.CapacitySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptActorTransformBatchCapacitySmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	TArray<FAvidScriptObjectHandle> Handles;
	Handles.SetNum(257);
	TArray<FAvidScriptActorTransformSnapshot> Snapshots;
	FAvidScriptActorTransformBatchResult Result;

	TestFalse(TEXT("Transform batch rejects more than 256 items"), FAvidScriptActorBinding::GetActorTransforms(Registry, Handles, Snapshots, Result));
	TestEqual(TEXT("Oversized batch reports category"), Result.ErrorCategory, FString(TEXT("batch_too_large")));
	TestEqual(TEXT("Oversized batch does not resolve an item"), Result.FailedIndex, INDEX_NONE);
	TestEqual(TEXT("Oversized batch returns no snapshots"), Snapshots.Num(), 0);
	return true;
}

#endif