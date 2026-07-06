#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptActorBinding.h"
#include "AvidScriptObjectRegistry.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

namespace
{
bool CreateActorBindingWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptActorBindingWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);

	return true;
}

void DestroyActorBindingWorld(UWorld*& World)
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
	FAvidScriptActorBindingLocationReadWriteSmokeTest,
	"AvidScript.Binding.ActorBinding.LocationReadWriteSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptActorBindingLocationReadWriteSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateActorBindingWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript actor binding world."));
		DestroyActorBindingWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyActorBindingWorld(World);
		return true;
	}

	const FVector InitialLocation(10.0, 20.0, 30.0);
	Actor->SetActorLocation(InitialLocation);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers as handle"), RegisterResult.bSucceeded);

	FVector ReadLocation = FVector::ZeroVector;
	FAvidScriptActorBindingResult ReadResult;
	TestTrue(
		TEXT("Actor location is readable through handle"),
		FAvidScriptActorBinding::GetActorLocation(Registry, ActorHandle, ReadLocation, ReadResult));
	TestEqual(TEXT("Read location matches actor"), ReadLocation, InitialLocation);
	TestTrue(TEXT("Read result is structured success"), ReadResult.bSucceeded);

	const FVector TargetLocation(101.0, 202.0, 303.0);
	FAvidScriptActorBindingResult WriteResult;
	TestTrue(
		TEXT("Actor location is writable when policy allows writes"),
		FAvidScriptActorBinding::SetActorLocation(
			Registry,
			ActorHandle,
			TargetLocation,
			EAvidScriptActorWritePolicy::AllowWrites,
			WriteResult));
	TestEqual(TEXT("Actor moved to target location"), Actor->GetActorLocation(), TargetLocation);
	TestTrue(TEXT("Write result is structured success"), WriteResult.bSucceeded);

	DestroyActorBindingWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptActorBindingAddLocationOffsetSmokeTest,
	"AvidScript.Binding.ActorBinding.AddLocationOffsetSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptActorBindingAddLocationOffsetSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateActorBindingWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript actor binding world."));
		DestroyActorBindingWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyActorBindingWorld(World);
		return true;
	}

	const FVector InitialLocation(10.0, 20.0, 30.0);
	Actor->SetActorLocation(InitialLocation);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers as handle"), RegisterResult.bSucceeded);

	const FVector Offset(1.5, -2.0, 3.25);
	const FVector ExpectedLocation = InitialLocation + Offset;
	FAvidScriptActorBindingResult OffsetResult;
	TestTrue(
		TEXT("Actor location offset is writable when policy allows writes"),
		FAvidScriptActorBinding::AddActorLocationOffset(
			Registry,
			ActorHandle,
			Offset,
			EAvidScriptActorWritePolicy::AllowWrites,
			OffsetResult));
	TestEqual(TEXT("Actor moved by offset"), Actor->GetActorLocation(), ExpectedLocation);
	TestTrue(TEXT("Offset result is structured success"), OffsetResult.bSucceeded);
	TestEqual(TEXT("Offset result reports applied location"), OffsetResult.Location, ExpectedLocation);

	DestroyActorBindingWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptActorBindingWritePolicySmokeTest,
	"AvidScript.Binding.ActorBinding.WritePolicySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptActorBindingWritePolicySmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateActorBindingWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript actor binding world."));
		DestroyActorBindingWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyActorBindingWorld(World);
		return true;
	}

	const FVector InitialLocation(1.0, 2.0, 3.0);
	Actor->SetActorLocation(InitialLocation);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers as handle"), RegisterResult.bSucceeded);

	FAvidScriptActorBindingResult WriteResult;
	TestFalse(
		TEXT("Read-only policy denies actor location writes"),
		FAvidScriptActorBinding::SetActorLocation(
			Registry,
			ActorHandle,
			FVector(9.0, 9.0, 9.0),
			EAvidScriptActorWritePolicy::ReadOnly,
			WriteResult));
	TestEqual(TEXT("Denied write reports policy category"), WriteResult.ErrorCategory, FString(TEXT("write_denied")));
	TestEqual(TEXT("Denied write keeps actor location unchanged"), Actor->GetActorLocation(), InitialLocation);

	DestroyActorBindingWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptActorBindingInvalidHandleSmokeTest,
	"AvidScript.Binding.ActorBinding.InvalidHandleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptActorBindingInvalidHandleSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;

	FVector ReadLocation = FVector::ZeroVector;
	FAvidScriptActorBindingResult InvalidReadResult;
	TestFalse(
		TEXT("Invalid actor handle does not resolve"),
		FAvidScriptActorBinding::GetActorLocation(
			Registry,
			FAvidScriptObjectHandle(),
			ReadLocation,
			InvalidReadResult));
	TestEqual(TEXT("Invalid actor handle reports invalid_handle"), InvalidReadResult.ErrorCategory, FString(TEXT("invalid_handle")));

	UObject* NonActorObject = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle NonActorHandle = Registry.RegisterObject(NonActorObject, RegisterResult);
	TestTrue(TEXT("Non-actor object registers"), RegisterResult.bSucceeded);

	FAvidScriptActorBindingResult WrongTypeResult;
	TestFalse(
		TEXT("Non-actor handle does not satisfy actor binding"),
		FAvidScriptActorBinding::GetActorLocation(
			Registry,
			NonActorHandle,
			ReadLocation,
			WrongTypeResult));
	TestEqual(TEXT("Non-actor handle reports type_mismatch"), WrongTypeResult.ErrorCategory, FString(TEXT("type_mismatch")));

	return true;
}

#endif
