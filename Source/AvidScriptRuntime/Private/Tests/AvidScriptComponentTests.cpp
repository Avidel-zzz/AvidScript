#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptComponent.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace
{
bool CreateComponentWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::PIE, false, TEXT("AvidScriptComponentWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::PIE);
	WorldContext.SetCurrentWorld(OutWorld);

	return true;
}

void DestroyComponentWorld(UWorld*& World)
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

UAvidScriptComponent* AddAvidScriptComponent(AActor* Actor)
{
	UAvidScriptComponent* Component = NewObject<UAvidScriptComponent>(Actor, TEXT("AvidScriptComponent"));
	if (Component != nullptr)
	{
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
	}

	return Component;
}

bool BeginComponentWorld(UWorld* World)
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentOwnerHandleLifecycleSmokeTest,
	"AvidScript.Component.OwnerHandleLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentOwnerHandleLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript component world."));
		DestroyComponentWorld(World);
		return true;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	UAvidScriptComponent* Component = AddAvidScriptComponent(Actor);
	TestNotNull(TEXT("AvidScript component is attachable to an actor"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	const FAvidScriptComponentRuntimeStats StatsAfterBeginPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component registers owner on BeginPlay"), StatsAfterBeginPlay.bOwnerRegistered);
	TestTrue(TEXT("Component exposes a valid owner handle"), StatsAfterBeginPlay.OwnerHandle.IsValid());
	TestEqual(TEXT("Component records owner object path"), StatsAfterBeginPlay.OwnerObjectPath, Actor->GetPathName());

	FAvidScriptObjectHandleResult ResolveResult;
	AActor* ResolvedOwner = nullptr;
	TestTrue(TEXT("Owner handle resolves while component is active"), Component->ResolveOwnerActor(ResolvedOwner, ResolveResult));
	TestEqual(TEXT("Resolved owner matches component owner"), ResolvedOwner, Actor);

	TestTrue(TEXT("Smoke world routes EndPlay"), World->EndPlay(EEndPlayReason::Quit));

	const FAvidScriptComponentRuntimeStats StatsAfterEndPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component records EndPlay cleanup"), StatsAfterEndPlay.bEndPlayCalled);
	TestTrue(TEXT("Component releases owner handle on EndPlay"), StatsAfterEndPlay.bOwnerReleased);

	ResolvedOwner = nullptr;
	TestFalse(TEXT("Released owner handle no longer resolves"), Component->ResolveOwnerActor(ResolvedOwner, ResolveResult));
	TestEqual(TEXT("Released owner handle reports stale generation"), ResolveResult.ErrorCategory, FString(TEXT("generation_mismatch")));

	DestroyComponentWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentRuntimeTickSmokeTest,
	"AvidScript.Component.RuntimeTickSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentRuntimeTickSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript component world."));
		DestroyComponentWorld(World);
		return true;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	UAvidScriptComponent* Component = AddAvidScriptComponent(Actor);
	TestNotNull(TEXT("AvidScript component is attachable to an actor"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	const FAvidScriptComponentRuntimeStats StatsAfterBeginPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component loads embedded smoke runtime on BeginPlay"), StatsAfterBeginPlay.bRuntimeLoaded);
	TestTrue(TEXT("Component calls avid_on_begin_play"), StatsAfterBeginPlay.bBeginPlayCalled);

	World->Tick(LEVELTICK_All, 1.0f / 60.0f);

	const FAvidScriptComponentRuntimeStats StatsAfterTick = Component->GetRuntimeStats();
	TestTrue(TEXT("Component tick calls avid_on_tick"), StatsAfterTick.TickCallCount > 0);

	TestTrue(TEXT("Smoke world routes EndPlay"), World->EndPlay(EEndPlayReason::Quit));

	const FAvidScriptComponentRuntimeStats StatsAfterEndPlay = Component->GetRuntimeStats();
	TestFalse(TEXT("Component unloads runtime on EndPlay"), StatsAfterEndPlay.bRuntimeLoaded);

	DestroyComponentWorld(World);
	return true;
}

#endif
