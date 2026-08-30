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
	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);

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

	DestroyGeneratedScriptWorld(World);
	return true;
}

#endif
