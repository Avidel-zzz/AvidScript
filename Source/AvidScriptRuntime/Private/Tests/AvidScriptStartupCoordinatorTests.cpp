#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptComponent.h"
#include "AvidScriptWorldSubsystem.h"
#include "Startup/AvidScriptStartupCoordinator.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace
{
bool CreateStartupWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	const FString WorldName = TEXT("AvidScriptStartupWorld_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* WorldPackage = CreatePackage(*(TEXT("/Game/AvidScriptTests/") + WorldName));
	WorldPackage->SetFlags(RF_Transient);
	OutWorld = UWorld::CreateWorld(
		EWorldType::PIE,
		false,
		FName(*WorldName),
		WorldPackage);
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

	FAvidScriptStartupScenario BeforeStartScenario;
	BeforeStartScenario.ScenarioId = TEXT("before_world_start");
	BeforeStartScenario.Worlds.Add(World->GetOutermost()->GetName());
	BeforeStartScenario.Bindings.Add(MakeWorldHostBinding());
	FAvidScriptStartupCoordinator BeforeStartCoordinator;
	FAvidScriptStartupRuntimeResult BeforeStartResult;
	TestFalse(
		TEXT("Activation cannot commit a runtime before actors begin"),
		BeforeStartCoordinator.Activate(*World, BeforeStartScenario, BeforeStartResult, true));
	TestEqual(TEXT("Pre-BeginPlay activation is rejected"), BeforeStartResult.ErrorCategory, FString(TEXT("world_not_started")));
	TestEqual(TEXT("Pre-BeginPlay activation creates no actors"), BeforeStartCoordinator.GetLiveOwnedActorCount(), 0);

	World->SetGameInstance(NewObject<UGameInstance>(GEngine));
	FURL Url;
	Url.AddOption(TEXT("game=/Script/Engine.GameModeBase"));
	if (!TestTrue(TEXT("Native GameMode is installed"), World->SetGameMode(Url)))
	{
		DestroyStartupWorld(World);
		return true;
	}
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	if (!TestTrue(TEXT("UWorld BeginPlay starts actors through GameMode"), World->HasBegunPlay()))
	{
		DestroyStartupWorld(World);
		return true;
	}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptStartupWorldBeginPlayRollbackTest,
	"AvidScript.Runtime.StartupScenario.WorldBeginPlayRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptStartupWorldBeginPlayRollbackTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	UWorld* World = nullptr;
	if (!CreateStartupWorld(World))
	{
		AddError(TEXT("Failed to create startup World BeginPlay fixture."));
		return true;
	}
	const FString OriginalCommandLine = FCommandLine::Get();
	const FString FixtureId = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	const FString DocumentPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectContentDir(), TEXT("AvidScriptStartupContract-") + FixtureId + TEXT(".json")));
	ON_SCOPE_EXIT
	{
		DestroyStartupWorld(World);
		FCommandLine::Set(*OriginalCommandLine);
		IFileManager::Get().Delete(*DocumentPath);
	};

	const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("mode"), TEXT("world_host"));
	const TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
	Binding->SetStringField(TEXT("module_id"), TEXT("startup.missing.") + FixtureId);
	Binding->SetObjectField(TEXT("target"), Target);
	const TSharedRef<FJsonObject> Scenario = MakeShared<FJsonObject>();
	Scenario->SetStringField(TEXT("scenario_id"), TEXT("world_start_failure"));
	Scenario->SetStringField(TEXT("activation"), TEXT("explicit"));
	Scenario->SetArrayField(TEXT("worlds"), {
		MakeShared<FJsonValueString>(World->GetOutermost()->GetName()) });
	Scenario->SetArrayField(TEXT("bindings"), { MakeShared<FJsonValueObject>(Binding) });
	const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("schema_version"), 1);
	Document->SetArrayField(TEXT("scenarios"), { MakeShared<FJsonValueObject>(Scenario) });
	FString Json;
	if (!TestTrue(TEXT("Startup fixture serializes"), FJsonSerializer::Serialize(
			Document, TJsonWriterFactory<>::Create(&Json)))
		|| !TestTrue(TEXT("Startup fixture is written"), FFileHelper::SaveStringToFile(Json, *DocumentPath)))
	{
		return true;
	}
	FCommandLine::Set(*FString::Printf(
		TEXT("-AvidScriptScenario=world_start_failure -AvidScriptScenarioFile=\"%s\""), *DocumentPath));
	UAvidScriptWorldSubsystem* Subsystem = World->GetSubsystem<UAvidScriptWorldSubsystem>();
	if (!TestNotNull(TEXT("Startup subsystem exists"), Subsystem))
	{
		return true;
	}
	World->SetGameInstance(NewObject<UGameInstance>(GEngine));
	FURL Url;
	Url.AddOption(TEXT("game=/Script/Engine.GameModeBase"));
	if (!TestTrue(TEXT("Native GameMode is installed"), World->SetGameMode(Url)))
	{
		return true;
	}
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	if (!TestTrue(TEXT("Real World BeginPlay has started actors"), World->HasBegunPlay()))
	{
		return true;
	}
	TestFalse(TEXT("Subsystem has not prematurely committed activation"), Subsystem->GetRuntimeStats().bStartupScenarioActive);
	TestTrue(TEXT("Subsystem is waiting to activate after actor startup"), Subsystem->IsTickable());
	AddExpectedError(TEXT("AvidScript startup activation failed"), EAutomationExpectedErrorFlags::Contains, 1);
	World->Tick(LEVELTICK_All, 1.0f / 60.0f);
	const FAvidScriptWorldRuntimeStats& Stats = Subsystem->GetRuntimeStats();
	TestTrue(TEXT("The actual process scenario was requested"), Stats.bStartupScenarioRequested);
	TestEqual(TEXT("Missing Runtime fails the activation"), Stats.LastErrorCategory, FString(TEXT("component_runtime_start_failed")));
	TestFalse(TEXT("Failed scenario is not active"), Stats.bStartupScenarioActive);
	TestFalse(TEXT("Failed scenario has no Runtime"), Stats.bRuntimeLoaded);
	TestFalse(TEXT("Failed startup stops its pending Tick"), Subsystem->IsTickable());
	int32 LiveStartupActors = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed() && It->GetName().StartsWith(TEXT("AvidScriptStartupHost")))
		{
			++LiveStartupActors;
		}
	}
	TestEqual(TEXT("Failed startup rolls back owned actors"), LiveStartupActors, 0);
	return true;
}

#endif
