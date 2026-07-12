#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptComponent.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "Fixtures/AvidScriptGameplayEventFixture.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Ssl.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
FString CollisionBytesToLowerHex(const uint8* Bytes, int32 ByteCount)
{
	FString Hex;
	Hex.Reserve(ByteCount * 2);
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Hex += FString::Printf(TEXT("%02x"), Bytes[Index]);
	}
	return Hex;
}

FString ComputeCollisionSha256(const TArray<uint8>& Bytes)
{
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	SHA256(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Digest);
	return CollisionBytesToLowerHex(Digest, UE_ARRAY_COUNT(Digest));
}

bool WriteCollisionFixture(bool bTrap, FString& OutManifestPath)
{
	FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests/Phase38/Collision"),
		bTrap ? TEXT("Trap") : TEXT("Success")));
	FPaths::NormalizeFilename(Root);
	if (!IFileManager::Get().MakeDirectory(*Root, true))
	{
		return false;
	}

	const TArray<uint8> WasmBytes = AvidScriptGameplayEventFixture::Build(bTrap, 100.0f);
	const FString WasmPath = FPaths::Combine(Root, TEXT("collision_event.wasm"));
	OutManifestPath = FPaths::Combine(Root, TEXT("collision_event.avidscript.json"));
	if (!FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath))
	{
		return false;
	}

	const FString ManifestJson = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"module_id\": \"collision_event_%s\",\n")
		TEXT("  \"abi_version\": 1,\n")
		TEXT("  \"language\": \"wasm\",\n")
		TEXT("  \"wasm\": { \"file\": \"collision_event.wasm\", \"sha256\": \"%s\" },\n")
		TEXT("  \"required_exports\": [\"avid_on_begin_play\", \"avid_on_tick\"],\n")
		TEXT("  \"required_imports\": [{ \"module\": \"avidscript\", \"name\": \"actor_set_location\" }]\n")
		TEXT("}\n"),
		bTrap ? TEXT("trap") : TEXT("success"),
		*ComputeCollisionSha256(WasmBytes));
	return FFileHelper::SaveStringToFile(ManifestJson, *OutManifestPath);
}

bool CreateCollisionWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(EWorldType::PIE, false, TEXT("AvidScriptCollisionWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::PIE);
	Context.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyCollisionWorld(UWorld*& World)
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

bool BeginCollisionWorld(UWorld* World)
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

UAvidScriptComponent* AddCollisionComponent(AActor* Owner, const FString& ManifestPath)
{
	UAvidScriptComponent* Component = NewObject<UAvidScriptComponent>(Owner);
	if (Component != nullptr)
	{
		Owner->AddInstanceComponent(Component);
		Component->SetScriptManifestPath(ManifestPath);
		Component->RegisterComponent();
	}
	return Component;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentCollisionDelegateTest,
	"AvidScript.Component.Collision.DelegatePayloadAndLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentCollisionDelegateTest::RunTest(const FString& Parameters)
{
	FString ManifestPath;
	if (!TestTrue(TEXT("collision fixture writes"), WriteCollisionFixture(false, ManifestPath)))
	{
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateCollisionWorld(World))
	{
		AddError(TEXT("Failed to create collision test world."));
		return true;
	}
	TestTrue(TEXT("collision world begins"), BeginCollisionWorld(World));

	AActor* Owner = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	AActor* Other = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("collision owner spawns"), Owner);
	TestNotNull(TEXT("collision other actor spawns"), Other);
	if (Owner == nullptr || Other == nullptr)
	{
		DestroyCollisionWorld(World);
		return true;
	}

	Owner->SetActorLocation(FVector(1.0, 2.0, 3.0));
	Other->SetActorLocation(FVector(10.0, 20.0, 30.0));
	UAvidScriptComponent* Component = AddCollisionComponent(Owner, ManifestPath);
	TestNotNull(TEXT("collision component attaches"), Component);
	if (Component == nullptr)
	{
		DestroyCollisionWorld(World);
		return true;
	}
	TestTrue(TEXT("collision delegates bind only for a running session"), Component->GetRuntimeStats().bCollisionDelegatesBound);

	Owner->OnActorBeginOverlap.Broadcast(Owner, Other);
	TestTrue(TEXT("begin overlap passes the OtherActor handle and vector to WASM"), Other->GetActorLocation().Equals(FVector(110.0, 20.0, 30.0), 0.01));
	TestEqual(TEXT("one delegate produces one guest callback"), Component->GetRuntimeStats().EventCallbackCount, 1);

	Owner->OnActorBeginOverlap.Broadcast(Owner, nullptr);
	TestEqual(TEXT("invalid OtherActor does not dispatch"), Component->GetRuntimeStats().EventCallbackCount, 1);
	TestTrue(TEXT("invalid OtherActor leaves runtime healthy"), Component->GetRuntimeStats().bRuntimeLoaded);

	Owner->OnActorEndOverlap.Broadcast(Owner, Other);
	TestTrue(TEXT("end overlap reuses the session handle"), Other->GetActorLocation().Equals(FVector(210.0, 20.0, 30.0), 0.01));
	TestEqual(TEXT("end overlap dispatches once"), Component->GetRuntimeStats().EventCallbackCount, 2);

	Owner->OnActorHit.Broadcast(Owner, Other, FVector(7.0, 8.0, 9.0), FHitResult());
	TestTrue(TEXT("hit forwards normal impulse"), Other->GetActorLocation().Equals(FVector(107.0, 8.0, 9.0), 0.01));
	TestEqual(TEXT("hit dispatches once"), Component->GetRuntimeStats().EventCallbackCount, 3);
	TestEqual(TEXT("last typed event records Hit"), Component->GetRuntimeStats().LastEventId, static_cast<int32>(EAvidScriptGameplayEventType::Hit));

	TestTrue(TEXT("collision world ends"), World->EndPlay(EEndPlayReason::Quit));
	TestFalse(TEXT("collision delegates unbind before runtime release"), Component->GetRuntimeStats().bCollisionDelegatesBound);
	const FVector LocationAfterEndPlay = Other->GetActorLocation();
	Owner->OnActorBeginOverlap.Broadcast(Owner, Other);
	TestTrue(TEXT("post-EndPlay delegate cannot enter WASM"), Other->GetActorLocation().Equals(LocationAfterEndPlay, 0.01));

	DestroyCollisionWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentCollisionTrapTest,
	"AvidScript.Component.Collision.TrapUnbinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentCollisionTrapTest::RunTest(const FString& Parameters)
{
	FString ManifestPath;
	if (!TestTrue(TEXT("collision trap fixture writes"), WriteCollisionFixture(true, ManifestPath)))
	{
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateCollisionWorld(World))
	{
		AddError(TEXT("Failed to create collision trap world."));
		return true;
	}
	TestTrue(TEXT("collision trap world begins"), BeginCollisionWorld(World));
	AActor* Owner = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	AActor* Other = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	UAvidScriptComponent* Component = Owner != nullptr ? AddCollisionComponent(Owner, ManifestPath) : nullptr;
	TestNotNull(TEXT("collision trap component attaches"), Component);
	if (Component != nullptr && Other != nullptr)
	{
		Owner->OnActorBeginOverlap.Broadcast(Owner, Other);
		TestFalse(TEXT("guest trap unloads the component session"), Component->GetRuntimeStats().bRuntimeLoaded);
		TestFalse(TEXT("guest trap immediately unbinds collision delegates"), Component->GetRuntimeStats().bCollisionDelegatesBound);
		TestTrue(TEXT("teardown preserves the guest trap as root cause"), Component->GetRuntimeStats().LastErrorMessage.Contains(TEXT("category=trap")));
	}
	DestroyCollisionWorld(World);
	return true;
}

#endif
