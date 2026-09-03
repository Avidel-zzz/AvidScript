#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptComponent.h"
#include "Startup/AvidScriptStartupCoordinator.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
bool CreateStartupWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(
		EWorldType::PIE,
		false,
		MakeUniqueObjectName(nullptr, UWorld::StaticClass(), TEXT("AvidScriptStartupWorld")));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::PIE);
	Context.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyStartupWorld(UWorld*& World)
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

FAvidScriptStartupBinding MakeWorldHostBinding()
{
	FAvidScriptStartupBinding Binding;
	Binding.ModuleId = TEXT("startup.world");
	Binding.Target.Mode = EAvidScriptStartupTargetMode::WorldHost;
	return Binding;
}

FAvidScriptStartupBinding MakeExistingBinding()
{
	FAvidScriptStartupBinding Binding;
	Binding.ModuleId = TEXT("startup.existing");
	Binding.Target.Mode = EAvidScriptStartupTargetMode::ExistingActor;
	Binding.Target.ClassPath = AActor::StaticClass()->GetPathName();
	Binding.Target.RequiredTag = TEXT("startup_target");
	Binding.Target.MaxInstances = 1;
	return Binding;
}

FAvidScriptStartupBinding MakeSpawnBinding()
{
	FAvidScriptStartupBinding Binding;
	Binding.ModuleId = TEXT("startup.spawn");
	Binding.Target.Mode = EAvidScriptStartupTargetMode::SpawnActor;
	Binding.Target.ClassPath = AActor::StaticClass()->GetPathName();
	Binding.Target.MaxInstances = 1;
	Binding.Target.SpawnTransforms.Add(FTransform(FVector(120.0, 40.0, 20.0)));
	return Binding;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptStartupCoordinatorLifecycleTest,
	"AvidScript.Runtime.StartupScenario.CoordinatorLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptStartupCoordinatorLifecycleTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	UWorld* World = nullptr;
	if (!CreateStartupWorld(World))
	{
		AddError(TEXT("Failed to create startup coordinator world."));
		return true;
	}

	AActor* ExistingActor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Existing target actor is created"), ExistingActor))
	{
		DestroyStartupWorld(World);
		return true;
	}
	ExistingActor->Tags.Add(TEXT("startup_target"));

	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);

	FAvidScriptStartupScenario Scenario;
	Scenario.ScenarioId = TEXT("coordinator_lifecycle");
	Scenario.Worlds.Add(World->GetOutermost()->GetName());
	Scenario.Bindings = {
		MakeWorldHostBinding(),
		MakeExistingBinding(),
		MakeSpawnBinding()
	};

	FAvidScriptStartupCoordinator Coordinator;
	FAvidScriptStartupRuntimeResult Result;
	TestTrue(
		TEXT("Coordinator atomically activates all target modes"),
		Coordinator.Activate(*World, Scenario, Result, true));
	TestTrue(TEXT("Coordinator reports active"), Result.bActive);
	TestEqual(TEXT("Three bindings are applied"), Result.BindingCount, 3);
	TestEqual(TEXT("Three components are attached"), Result.ComponentCount, 3);
	TestEqual(TEXT("World and spawn actors are owned"), Result.OwnedActorCount, 2);
	TestEqual(TEXT("All component runtimes load"), Result.RuntimeLoadedCount, 3);
	TestEqual(TEXT("All component BeginPlay callbacks run"), Result.BeginPlayCount, 3);

	FAvidScriptStartupRuntimeResult IdempotentResult;
	TestTrue(
		TEXT("Repeated activation is idempotent"),
		Coordinator.Activate(*World, Scenario, IdempotentResult, true));
	TestEqual(TEXT("Repeated activation creates no duplicate component"), IdempotentResult.ComponentCount, 3);

	World->Tick(LEVELTICK_All, 1.0f / 60.0f);
	TArray<UAvidScriptComponent*> Components;
	Coordinator.GetLiveComponents(Components);
	for (const UAvidScriptComponent* Component : Components)
	{
		TestTrue(TEXT("Mounted component receives Tick"), Component->GetRuntimeStats().TickCallCount > 0);
	}

	Coordinator.Deactivate();
	Coordinator.GetLiveComponents(Components);
	TestFalse(TEXT("Coordinator becomes inactive"), Coordinator.IsActive());
	TestEqual(TEXT("Coordinator releases mounted components"), Components.Num(), 0);
	TestEqual(TEXT("Coordinator releases owned actors"), Coordinator.GetLiveOwnedActorCount(), 0);
	TestFalse(TEXT("Existing target actor is not owned"), ExistingActor->IsActorBeingDestroyed());

	FAvidScriptStartupScenario ExistingOnlyScenario;
	ExistingOnlyScenario.ScenarioId = TEXT("existing_excludes_owned");
	ExistingOnlyScenario.Worlds = Scenario.Worlds;
	FAvidScriptStartupBinding SpawnStaticMesh = MakeSpawnBinding();
	SpawnStaticMesh.Target.ClassPath = TEXT("/Script/Engine.StaticMeshActor");
	ExistingOnlyScenario.Bindings.Add(SpawnStaticMesh);
	FAvidScriptStartupBinding FindStaticMesh = MakeExistingBinding();
	FindStaticMesh.Target.ClassPath = TEXT("/Script/Engine.StaticMeshActor");
	FindStaticMesh.Target.RequiredTag = NAME_None;
	ExistingOnlyScenario.Bindings.Add(FindStaticMesh);
	FAvidScriptStartupRuntimeResult ExistingOnlyResult;
	TestFalse(
		TEXT("Existing target excludes actors created by this activation"),
		Coordinator.Activate(*World, ExistingOnlyScenario, ExistingOnlyResult, true));
	TestEqual(
		TEXT("Owned-only matches report no existing target"),
		ExistingOnlyResult.ErrorCategory,
		FString(TEXT("target_not_found")));

	FAvidScriptStartupScenario RejectedScenario;
	RejectedScenario.ScenarioId = TEXT("atomic_rollback");
	RejectedScenario.Worlds = Scenario.Worlds;
	RejectedScenario.Bindings.Add(MakeWorldHostBinding());
	FAvidScriptStartupBinding InvalidBinding = MakeSpawnBinding();
	InvalidBinding.Target.ClassPath = TEXT("/Script/Engine.DoesNotExist");
	RejectedScenario.Bindings.Add(MoveTemp(InvalidBinding));
	FAvidScriptStartupRuntimeResult RejectedResult;
	TestFalse(
		TEXT("Unavailable target class rejects the scenario"),
		Coordinator.Activate(*World, RejectedScenario, RejectedResult, true));
	TestEqual(
		TEXT("Unavailable target class reports stable category"),
		RejectedResult.ErrorCategory,
		FString(TEXT("target_class_unavailable")));
	Coordinator.GetLiveComponents(Components);
	TestEqual(TEXT("Rejected scenario rolls back components"), Components.Num(), 0);
	TestEqual(TEXT("Rejected scenario rolls back actors"), Coordinator.GetLiveOwnedActorCount(), 0);

	DestroyStartupWorld(World);
	return true;
}

#endif
