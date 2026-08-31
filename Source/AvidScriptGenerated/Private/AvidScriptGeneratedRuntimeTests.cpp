#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGeneratedTypes.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
bool CreateGeneratedScriptWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptGeneratedRuntimeWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyGeneratedScriptWorld(UWorld*& World)
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

void DestroyGeneratedGameInstance(UGameInstance*& GameInstance)
{
	if (GameInstance == nullptr)
	{
		return;
	}
	UWorld* const World = GameInstance->GetWorld();
	GameInstance->Shutdown();
	if (World != nullptr && GEngine != nullptr)
	{
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
	}
	GameInstance->RemoveFromRoot();
	GameInstance = nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedCSharpPropertyInteractionTest,
	"AvidScript.GeneratedTypes.CSharpPropertyInteraction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpPropertyInteractionTest::RunTest(
	const FString& Parameters)
{
	static_cast<void>(Parameters);
	UWorld* World = nullptr;
	if (!CreateGeneratedScriptWorld(World))
	{
		AddError(TEXT("Failed to create the generated script test world."));
		return true;
	}

	AProjectile* const Projectile = World->SpawnActor<AProjectile>();
	if (!TestNotNull(TEXT("Generated C# projectile spawns"), Projectile))
	{
		DestroyGeneratedScriptWorld(World);
		return true;
	}
	TestEqual(
		TEXT("C# property initializer becomes the native instance default"),
		Projectile->Damage,
		25.0f);
	TestEqual(TEXT("Generated int32 default"), Projectile->ActivationCount, 1);
	TestEqual(TEXT("Generated int64 default"), Projectile->AccumulatedDamage, 100LL);
	TestEqual(TEXT("Generated float64 default"), Projectile->PrecisionScale, 1.5);
	TestFalse(TEXT("Generated bool default"), Projectile->IsActive);
	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);
	if (!Projectile->HasActorBegunPlay())
	{
		Projectile->DispatchBeginPlay();
	}
	TestTrue(TEXT("C# BeginPlay updates the UE property"), Projectile->HasBegunPlay);

	FString RuntimeError;
	if (!TestTrue(
		TEXT("Generated projectile owns a live script Session"),
		FAvidScriptGeneratedTypeRuntimeHost::Get().BeginInstance(
			*Projectile,
			3u,
			RuntimeError)))
	{
		AddError(RuntimeError);
		DestroyGeneratedScriptWorld(World);
		return true;
	}
	Projectile->Activate(2.0f);
	TestEqual(TEXT("C# property get/set updates the UE float property"), Projectile->Damage, 50.0f);
	TestEqual(TEXT("C# property codec updates int32"), Projectile->ActivationCount, 2);
	TestEqual(TEXT("C# property codec updates int64"), Projectile->AccumulatedDamage, 125LL);
	TestEqual(TEXT("C# property codec updates float64"), Projectile->PrecisionScale, 3.0);
	TestTrue(TEXT("C# property codec updates bool"), Projectile->IsActive);

	DestroyGeneratedScriptWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedCSharpInheritanceDispatchTest,
	"AvidScript.GeneratedTypes.CSharpInheritanceDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpInheritanceDispatchTest::RunTest(
	const FString& Parameters)
{
	static_cast<void>(Parameters);
	UWorld* World = nullptr;
	if (!CreateGeneratedScriptWorld(World))
	{
		AddError(TEXT("Failed to create the generated script inheritance test world."));
		return true;
	}

	AProjectile* const Projectile = World->SpawnActor<AExplosiveProjectile>();
	if (!TestNotNull(TEXT("Generated derived C# projectile spawns"), Projectile))
	{
		DestroyGeneratedScriptWorld(World);
		return true;
	}
	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);
	if (!Projectile->HasActorBegunPlay())
	{
		Projectile->DispatchBeginPlay();
	}
	TestTrue(
		TEXT("Derived instance reaches the inherited C# BeginPlay route"),
		Projectile->HasBegunPlay);

	Projectile->Activate(2.0f);
	TestEqual(
		TEXT("Virtual native dispatch reaches the C# override and direct base call"),
		Projectile->Damage,
		100.0f);
	TestEqual(
		TEXT("Inherited C# UFUNCTION routes through the base type ordinal"),
		Projectile->GetActivationCount(),
		2);
	TestEqual(TEXT("C# base call updates int64 state"), Projectile->AccumulatedDamage, 125LL);
	TestEqual(TEXT("C# base call updates float64 state"), Projectile->PrecisionScale, 3.0);
	TestTrue(TEXT("C# base call updates bool state"), Projectile->IsActive);

	DestroyGeneratedScriptWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedCSharpRepNotifyInteractionTest,
	"AvidScript.GeneratedTypes.CSharpRepNotifyInteraction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpRepNotifyInteractionTest::RunTest(
	const FString& Parameters)
{
	static_cast<void>(Parameters);
	UWorld* World = nullptr;
	if (!CreateGeneratedScriptWorld(World))
	{
		AddError(TEXT("Failed to create the generated RepNotify test world."));
		return true;
	}

	AProjectile* const Projectile = World->SpawnActor<AProjectile>();
	if (!TestNotNull(TEXT("Generated RepNotify projectile spawns"), Projectile))
	{
		DestroyGeneratedScriptWorld(World);
		return true;
	}
	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);
	if (!Projectile->HasActorBegunPlay())
	{
		Projectile->DispatchBeginPlay();
	}

	FFloatProperty* const DamageProperty = FindFProperty<FFloatProperty>(
		AProjectile::StaticClass(),
		TEXT("Damage"));
	UFunction* const RepNotifyFunction = AProjectile::StaticClass()->FindFunctionByName(
		TEXT("OnRepDamage"));
	if (!TestNotNull(TEXT("Generated Damage property resolves"), DamageProperty)
		|| !TestNotNull(TEXT("Generated OnRepDamage function resolves"), RepNotifyFunction))
	{
		DestroyGeneratedScriptWorld(World);
		return true;
	}
	TestTrue(TEXT("Damage is replicated"), DamageProperty->HasAnyPropertyFlags(CPF_Net));
	TestTrue(TEXT("Damage owns RepNotify metadata"), DamageProperty->HasAnyPropertyFlags(CPF_RepNotify));
	TestEqual(
		TEXT("Damage RepNotify targets the generated callback"),
		DamageProperty->RepNotifyFunc,
		FName(TEXT("OnRepDamage")));
	TestEqual(TEXT("RepNotify state starts from the C# default"), Projectile->DamageRepNotifyCount, 0);

	Projectile->ProcessEvent(RepNotifyFunction, nullptr);
	TestEqual(
		TEXT("UE RepNotify callback reaches C# WASM state"),
		Projectile->DamageRepNotifyCount,
		1);
	TestEqual(
		TEXT("C# RepNotify observes the native replicated value"),
		Projectile->LastReplicatedDamage,
		25.0f);

	DestroyGeneratedScriptWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedCSharpNetworkFunctionInteractionTest,
	"AvidScript.GeneratedTypes.CSharpNetworkFunctionInteraction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpNetworkFunctionInteractionTest::RunTest(
	const FString& Parameters)
{
	static_cast<void>(Parameters);
	UWorld* World = nullptr;
	if (!CreateGeneratedScriptWorld(World))
	{
		AddError(TEXT("Failed to create the generated network function test world."));
		return true;
	}

	AProjectile* const Projectile = World->SpawnActor<AProjectile>();
	if (!TestNotNull(TEXT("Generated network projectile spawns"), Projectile))
	{
		DestroyGeneratedScriptWorld(World);
		return true;
	}
	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);
	if (!Projectile->HasActorBegunPlay())
	{
		Projectile->DispatchBeginPlay();
	}

	UFunction* const ServerFunction = AProjectile::StaticClass()->FindFunctionByName(
		TEXT("ServerSubmitDamage"));
	UFunction* const ClientFunction = AProjectile::StaticClass()->FindFunctionByName(
		TEXT("ClientConfirmDamage"));
	UFunction* const MulticastFunction = AProjectile::StaticClass()->FindFunctionByName(
		TEXT("MulticastObserveDamage"));
	if (!TestNotNull(TEXT("Generated Server RPC resolves"), ServerFunction)
		|| !TestNotNull(TEXT("Generated Client RPC resolves"), ClientFunction)
		|| !TestNotNull(TEXT("Generated multicast RPC resolves"), MulticastFunction))
	{
		DestroyGeneratedScriptWorld(World);
		return true;
	}
	TestTrue(TEXT("Network contract enables native Actor replication"), Projectile->GetIsReplicated());
	TestTrue(
		TEXT("Server RPC owns UE server/reliable flags"),
		ServerFunction->HasAllFunctionFlags(FUNC_Net | FUNC_NetServer | FUNC_NetReliable));
	TestTrue(
		TEXT("Client RPC owns UE client/reliable flags"),
		ClientFunction->HasAllFunctionFlags(FUNC_Net | FUNC_NetClient | FUNC_NetReliable));
	TestTrue(
		TEXT("Multicast RPC owns UE multicast/reliable flags"),
		MulticastFunction->HasAllFunctionFlags(FUNC_Net | FUNC_NetMulticast | FUNC_NetReliable));

	Projectile->ServerSubmitDamage(41.0f);
	TestEqual(TEXT("Authority RPC reaches the C# WASM body"), Projectile->ServerRpcCount, 1);
	TestEqual(TEXT("C# Server RPC updates native replicated state"), Projectile->Damage, 41.0f);
	TestEqual(TEXT("C# Server RPC records its argument"), Projectile->LastServerDamage, 41.0f);

	Projectile->MulticastObserveDamage(41.0f);
	TestEqual(TEXT("Authority multicast executes the local C# body"), Projectile->MulticastCount, 1);
	TestEqual(TEXT("C# multicast records its argument"), Projectile->LastMulticastDamage, 41.0f);

	DestroyGeneratedScriptWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedCSharpLifecycleKindsTest,
	"AvidScript.GeneratedTypes.CSharpLifecycleKinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpLifecycleKindsTest::RunTest(
	const FString& Parameters)
{
	static_cast<void>(Parameters);
	UWorld* World = nullptr;
	if (!CreateGeneratedScriptWorld(World))
	{
		AddError(TEXT("Failed to create the generated lifecycle test world."));
		return true;
	}

	UEncounterSubsystem* const Encounter = World->GetSubsystem<UEncounterSubsystem>();
	AProjectile* const Projectile = World->SpawnActor<AProjectile>();
	UHealthComponent* const Health = Projectile != nullptr
		? NewObject<UHealthComponent>(Projectile)
		: nullptr;
	if (!TestNotNull(TEXT("Generated WorldSubsystem initializes"), Encounter)
		|| !TestNotNull(TEXT("Generated lifecycle Actor spawns"), Projectile)
		|| !TestNotNull(TEXT("Generated lifecycle Component creates"), Health))
	{
		DestroyGeneratedScriptWorld(World);
		return true;
	}
	Projectile->AddInstanceComponent(Health);
	Health->RegisterComponent();
	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);
	if (!Projectile->HasActorBegunPlay())
	{
		Projectile->DispatchBeginPlay();
	}
	if (!Health->HasBegunPlay())
	{
		static_cast<UActorComponent*>(Health)->BeginPlay();
	}

	TestTrue(TEXT("Generated Actor opts into native ticking"), Projectile->PrimaryActorTick.bCanEverTick);
	TestTrue(TEXT("Generated Component opts into native ticking"), Health->PrimaryComponentTick.bCanEverTick);
	TestEqual(TEXT("Actor BeginPlay reaches C#"), Projectile->HasBegunPlay, true);
	TestEqual(TEXT("Component BeginPlay reaches C# once"), Health->BeginPlayCount, 1);
	TestEqual(TEXT("WorldSubsystem Initialize reaches C# once"), Encounter->InitializeCount, 1);

	World->Tick(LEVELTICK_All, 1.0f / 60.0f);
	TestTrue(TEXT("Actor Tick scheduling reaches C#"), Projectile->TickCount > 0);
	TestTrue(TEXT("Component Tick scheduling reaches C#"), Health->TickCount > 0);
	TestTrue(TEXT("WorldSubsystem Tick scheduling reaches C#"), Encounter->TickCount > 0);

	TestTrue(TEXT("Destroying the Actor dispatches UE EndPlay"), Projectile->Destroy());
	TestEqual(TEXT("Actor EndPlay reaches C# once"), Projectile->EndPlayCount, 1);
	TestEqual(TEXT("Component EndPlay reaches C# once"), Health->EndPlayCount, 1);

	TStrongObjectPtr<UEncounterSubsystem> EncounterAfterDestroy(Encounter);
	DestroyGeneratedScriptWorld(World);
	TestEqual(
		TEXT("WorldSubsystem Deinitialize reaches C# once"),
		EncounterAfterDestroy->DeinitializeCount,
		1);

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!TestNotNull(TEXT("Lifecycle GameInstance creates"), GameInstance))
	{
		return true;
	}
	GameInstance->AddToRoot();
	GameInstance->InitializeStandalone(TEXT("AvidScriptGeneratedLifecycleGameInstance"));
	UProfileSubsystem* const Profile = GameInstance->GetSubsystem<UProfileSubsystem>();
	if (!TestNotNull(TEXT("Generated GameInstanceSubsystem initializes"), Profile))
	{
		DestroyGeneratedGameInstance(GameInstance);
		return true;
	}
	TestEqual(TEXT("GameInstanceSubsystem Initialize reaches C# once"), Profile->InitializeCount, 1);
	TStrongObjectPtr<UProfileSubsystem> ProfileAfterShutdown(Profile);
	DestroyGeneratedGameInstance(GameInstance);
	TestEqual(
		TEXT("GameInstanceSubsystem Deinitialize reaches C# once"),
		ProfileAfterShutdown->DeinitializeCount,
		1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedCookedPackageLoadTest,
	"AvidScript.GeneratedTypes.CookedPackageLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCookedPackageLoadTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (!TestTrue(TEXT("AvidScript plugin resolves"), Plugin.IsValid()))
	{
		return true;
	}
	const FString DescriptorPath = FPaths::Combine(
		Plugin->GetBaseDir(),
		TEXT("Content/AvidScriptGenerated/current.json"));
	if (!TestTrue(
		TEXT("Cook package pointer exists"),
		FPaths::FileExists(DescriptorPath)))
	{
		return true;
	}

	TUniquePtr<FAvidScriptGeneratedTypeRuntimeHost> Host =
		FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting();
	if (!TestNotNull(TEXT("Isolated generated type host starts"), Host.Get()))
	{
		return true;
	}
	FString Error;
	TestTrue(
		TEXT("Content-addressed Cook package installs"),
		Host->InstallPackageFromDescriptorFile(DescriptorPath, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	TestEqual(TEXT("Cook package starts without active instances"), Host->GetActiveInstanceCount(), 0);
	TestEqual(TEXT("Cook package starts without object handles"), Host->GetRegisteredHandleCount(), 0);
	Host->Shutdown();
	return true;
}

#endif
