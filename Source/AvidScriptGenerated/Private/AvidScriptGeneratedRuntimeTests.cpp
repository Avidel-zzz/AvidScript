#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGeneratedTypes.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"

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

#endif
