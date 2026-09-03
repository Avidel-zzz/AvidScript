#include "Startup/AvidScriptStartupCoordinator.h"

#include "AvidScriptComponent.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/SoftObjectPath.h"

namespace
{
constexpr const TCHAR* ScenarioArgument = TEXT("AvidScriptScenario=");
constexpr const TCHAR* ScenarioFileArgument = TEXT("AvidScriptScenarioFile=");

bool IsAvidScriptStartupPathUnderRoot(const FString& Path, const FString& Root)
{
	FString FullPath = FPaths::ConvertRelativePathToFull(Path);
	FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeFilename(FullPath);
	FPaths::NormalizeDirectoryName(FullRoot);
	return FullPath.StartsWith(FullRoot + TEXT("/"), ESearchCase::IgnoreCase);
}

FString GetPluginScenarioRoot()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	return Plugin.IsValid()
		? FPaths::Combine(Plugin->GetContentDir(), TEXT("AvidScript/Startup"))
		: FString();
}

FString GetDefaultScenarioPath()
{
	const FString ProjectPath = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("AvidScript/Startup/scenarios.json"));
	if (FPaths::FileExists(ProjectPath))
	{
		return ProjectPath;
	}
	const FString PluginRoot = GetPluginScenarioRoot();
	return PluginRoot.IsEmpty()
		? ProjectPath
		: FPaths::Combine(PluginRoot, TEXT("scenarios.json"));
}

bool ResolveScenarioPath(FString& OutPath, FString& OutError)
{
	FString RequestedPath;
	if (!FParse::Value(FCommandLine::Get(), ScenarioFileArgument, RequestedPath))
	{
		OutPath = GetDefaultScenarioPath();
		return true;
	}

#if UE_BUILD_SHIPPING
	OutError = TEXT("Shipping builds reject AvidScriptScenarioFile overrides.");
	return false;
#else
	if (RequestedPath.IsEmpty())
	{
		OutError = TEXT("AvidScriptScenarioFile is empty.");
		return false;
	}
	if (FPaths::IsRelative(RequestedPath))
	{
		RequestedPath = FPaths::Combine(FPaths::ProjectContentDir(), RequestedPath);
	}
	RequestedPath = FPaths::ConvertRelativePathToFull(RequestedPath);
	FPaths::NormalizeFilename(RequestedPath);
	const FString PluginRoot = GetPluginScenarioRoot();
	if (!IsAvidScriptStartupPathUnderRoot(RequestedPath, FPaths::ProjectContentDir())
		&& (PluginRoot.IsEmpty()
			|| !IsAvidScriptStartupPathUnderRoot(RequestedPath, PluginRoot)))
	{
		OutError = TEXT("AvidScriptScenarioFile must remain under project or plugin Content.");
		return false;
	}
	OutPath = MoveTemp(RequestedPath);
	return true;
#endif
}

FString GetWorldPackageName(const UWorld& World)
{
	return World.GetOutermost() != nullptr
		? World.GetOutermost()->GetName()
		: FString();
}

UClass* ResolveActorClass(const FString& ClassPath)
{
	UClass* ActorClass = FindObject<UClass>(nullptr, *ClassPath);
	if (ActorClass == nullptr)
	{
		ActorClass = FSoftClassPath(ClassPath).TryLoadClass<AActor>();
	}
	if (ActorClass == nullptr
		|| !ActorClass->IsChildOf(AActor::StaticClass())
		|| ActorClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return nullptr;
	}
	return ActorClass;
}
} // namespace

bool FAvidScriptStartupCoordinator::ActivateFromProcess(
	UWorld& World,
	FAvidScriptStartupRuntimeResult& OutResult)
{
	OutResult = FAvidScriptStartupRuntimeResult();
	FString ScenarioId;
	if (!FParse::Value(FCommandLine::Get(), ScenarioArgument, ScenarioId))
	{
		OutResult.bSucceeded = true;
		return true;
	}
	OutResult.bRequested = true;
	OutResult.ScenarioId = ScenarioId;
	if (ScenarioId.IsEmpty())
	{
		SetFailure(OutResult, TEXT("scenario_id_invalid"), TEXT("scenario id is empty"));
		return false;
	}

	FString ScenarioPath;
	FString PathError;
	if (!ResolveScenarioPath(ScenarioPath, PathError))
	{
		SetFailure(OutResult, TEXT("scenario_path_rejected"), PathError);
		return false;
	}
	OutResult.DocumentPath = ScenarioPath;

	FAvidScriptStartupDocument Document;
	FAvidScriptStartupLoadResult LoadResult;
	if (!AvidScript::Startup::LoadDocumentFile(ScenarioPath, Document, LoadResult))
	{
		SetFailure(
			OutResult,
			LoadResult.ErrorCategory.IsEmpty() ? TEXT("scenario_load_failed") : *LoadResult.ErrorCategory,
			LoadResult.ErrorMessage);
		return false;
	}

	const FAvidScriptStartupScenario* Scenario =
		AvidScript::Startup::FindScenario(Document, ScenarioId);
	if (Scenario == nullptr)
	{
		SetFailure(OutResult, TEXT("scenario_not_found"), ScenarioId);
		return false;
	}
	ActiveDocumentPath = ScenarioPath;
	return Activate(World, *Scenario, OutResult);
}

bool FAvidScriptStartupCoordinator::Activate(
	UWorld& World,
	const FAvidScriptStartupScenario& Scenario,
	FAvidScriptStartupRuntimeResult& OutResult,
	const bool bUseEmbeddedSmokeModuleForTesting)
{
	const FString RequestedDocumentPath = OutResult.DocumentPath;
	OutResult = FAvidScriptStartupRuntimeResult();
	OutResult.bRequested = true;
	OutResult.ScenarioId = Scenario.ScenarioId;
	OutResult.DocumentPath = RequestedDocumentPath.IsEmpty()
		? ActiveDocumentPath
		: RequestedDocumentPath;

	if (bActive)
	{
		if (ActiveWorld.Get() == &World && ActiveScenarioId == Scenario.ScenarioId)
		{
			FillSuccess(OutResult);
			return true;
		}
		SetFailure(OutResult, TEXT("coordinator_already_active"), ActiveScenarioId);
		return false;
	}
	if (!AvidScript::Startup::IsWorldAllowed(Scenario, GetWorldPackageName(World)))
	{
		SetFailure(OutResult, TEXT("world_not_allowed"), GetWorldPackageName(World));
		return false;
	}

	ActiveWorld = &World;
	ActiveScenarioId = Scenario.ScenarioId;
	ActiveBindingCount = Scenario.Bindings.Num();
	for (const FAvidScriptStartupBinding& Binding : Scenario.Bindings)
	{
		if (!ActivateBinding(
				World,
				Binding,
				bUseEmbeddedSmokeModuleForTesting,
				OutResult))
		{
			Deactivate();
			return false;
		}
	}
	bActive = true;
	FillSuccess(OutResult);
	return true;
}

bool FAvidScriptStartupCoordinator::ActivateBinding(
	UWorld& World,
	const FAvidScriptStartupBinding& Binding,
	const bool bUseEmbeddedSmokeModuleForTesting,
	FAvidScriptStartupRuntimeResult& OutResult)
{
	if (Binding.Target.Mode == EAvidScriptStartupTargetMode::WorldHost)
	{
		AActor* Actor = SpawnOwnedActor(
			World,
			*AActor::StaticClass(),
			FTransform::Identity,
			true,
			OutResult);
		return Actor != nullptr
			&& AttachComponent(*Actor, Binding, bUseEmbeddedSmokeModuleForTesting, OutResult);
	}

	UClass* ActorClass = ResolveActorClass(Binding.Target.ClassPath);
	if (ActorClass == nullptr)
	{
		SetFailure(OutResult, TEXT("target_class_unavailable"), Binding.Target.ClassPath);
		return false;
	}
	if (Binding.Target.Mode == EAvidScriptStartupTargetMode::ExistingActor)
	{
		int32 AttachedCount = 0;
		for (TActorIterator<AActor> It(&World, ActorClass); It && AttachedCount < Binding.Target.MaxInstances; ++It)
		{
			AActor* Actor = *It;
			const bool bOwnedByCoordinator = OwnedActors.ContainsByPredicate(
				[Actor](const TWeakObjectPtr<AActor>& Candidate)
				{
					return Candidate.Get() == Actor;
				});
			if (!IsValid(Actor)
				|| bOwnedByCoordinator
				|| (!Binding.Target.RequiredTag.IsNone()
					&& !Actor->ActorHasTag(Binding.Target.RequiredTag)))
			{
				continue;
			}
			if (!AttachComponent(
					*Actor,
					Binding,
					bUseEmbeddedSmokeModuleForTesting,
					OutResult))
			{
				return false;
			}
			++AttachedCount;
		}
		if (AttachedCount == 0)
		{
			SetFailure(OutResult, TEXT("target_not_found"), Binding.Target.ClassPath);
			return false;
		}
		return true;
	}
	if (Binding.Target.Mode == EAvidScriptStartupTargetMode::SpawnActor)
	{
		for (const FTransform& Transform : Binding.Target.SpawnTransforms)
		{
			AActor* Actor = SpawnOwnedActor(World, *ActorClass, Transform, false, OutResult);
			if (Actor == nullptr
				|| !AttachComponent(
					*Actor,
					Binding,
					bUseEmbeddedSmokeModuleForTesting,
					OutResult))
			{
				return false;
			}
		}
		return true;
	}

	SetFailure(OutResult, TEXT("target_mode_unsupported"), Binding.ModuleId);
	return false;
}

bool FAvidScriptStartupCoordinator::AttachComponent(
	AActor& Actor,
	const FAvidScriptStartupBinding& Binding,
	const bool bUseEmbeddedSmokeModuleForTesting,
	FAvidScriptStartupRuntimeResult& OutResult)
{
	UAvidScriptComponent* Component = NewObject<UAvidScriptComponent>(
		&Actor,
		UAvidScriptComponent::StaticClass(),
		MakeUniqueObjectName(&Actor, UAvidScriptComponent::StaticClass(), TEXT("AvidScriptStartup")),
		RF_Transient);
	if (Component == nullptr)
	{
		SetFailure(OutResult, TEXT("component_create_failed"), Actor.GetPathName());
		return false;
	}
	if (!bUseEmbeddedSmokeModuleForTesting)
	{
		Component->SetScriptModuleId(FName(*Binding.ModuleId));
	}
	Actor.AddInstanceComponent(Component);
	Component->RegisterComponent();
	if (Actor.GetWorld() != nullptr
		&& Actor.GetWorld()->HasBegunPlay()
		&& !Component->HasBegunPlay())
	{
		Component->BeginPlay();
	}
	if (Component->HasBegunPlay())
	{
		Component->RegisterAllComponentTickFunctions(true);
		Component->SetComponentTickEnabled(true);
	}
	if (!Component->IsRegistered())
	{
		SetFailure(OutResult, TEXT("component_register_failed"), Actor.GetPathName());
		Component->DestroyComponent();
		return false;
	}
	Components.Add(Component);

	if (Actor.HasActorBegunPlay() && !Component->GetRuntimeStats().bRuntimeLoaded)
	{
		SetFailure(
			OutResult,
			TEXT("component_runtime_start_failed"),
			Component->GetRuntimeStats().LastErrorMessage);
		return false;
	}
	return true;
}

AActor* FAvidScriptStartupCoordinator::SpawnOwnedActor(
	UWorld& World,
	UClass& ActorClass,
	const FTransform& Transform,
	const bool bCreateSceneRoot,
	FAvidScriptStartupRuntimeResult& OutResult)
{
	FActorSpawnParameters Parameters;
	Parameters.Name = MakeUniqueObjectName(&World, &ActorClass, TEXT("AvidScriptStartupHost"));
	Parameters.ObjectFlags |= RF_Transient;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = World.SpawnActor<AActor>(&ActorClass, Transform, Parameters);
	if (Actor == nullptr)
	{
		SetFailure(OutResult, TEXT("actor_spawn_failed"), ActorClass.GetPathName());
		return nullptr;
	}
	OwnedActors.Add(Actor);

	if (bCreateSceneRoot && Actor->GetRootComponent() == nullptr)
	{
		USceneComponent* Root = NewObject<USceneComponent>(
			Actor,
			USceneComponent::StaticClass(),
			TEXT("AvidScriptStartupRoot"),
			RF_Transient);
		if (Root == nullptr)
		{
			SetFailure(OutResult, TEXT("scene_root_create_failed"), Actor->GetPathName());
			return nullptr;
		}
		Actor->SetRootComponent(Root);
		Actor->AddInstanceComponent(Root);
		Root->RegisterComponent();
	}
	return Actor;
}

void FAvidScriptStartupCoordinator::Deactivate()
{
	for (const TWeakObjectPtr<UAvidScriptComponent>& WeakComponent : Components)
	{
		if (UAvidScriptComponent* Component = WeakComponent.Get())
		{
			Component->DestroyComponent();
		}
	}
	for (const TWeakObjectPtr<AActor>& WeakActor : OwnedActors)
	{
		if (AActor* Actor = WeakActor.Get())
		{
			Actor->Destroy();
		}
	}
	Components.Reset();
	OwnedActors.Reset();
	ActiveWorld.Reset();
	ActiveScenarioId.Reset();
	ActiveDocumentPath.Reset();
	ActiveBindingCount = 0;
	bActive = false;
}

void FAvidScriptStartupCoordinator::GetLiveComponents(
	TArray<UAvidScriptComponent*>& OutComponents) const
{
	OutComponents.Reset();
	for (const TWeakObjectPtr<UAvidScriptComponent>& WeakComponent : Components)
	{
		if (UAvidScriptComponent* Component = WeakComponent.Get())
		{
			OutComponents.Add(Component);
		}
	}
}

int32 FAvidScriptStartupCoordinator::GetLiveOwnedActorCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<AActor>& WeakActor : OwnedActors)
	{
		Count += WeakActor.IsValid() ? 1 : 0;
	}
	return Count;
}

void FAvidScriptStartupCoordinator::SetFailure(
	FAvidScriptStartupRuntimeResult& OutResult,
	const TCHAR* Category,
	const FString& Details)
{
	OutResult.bSucceeded = false;
	OutResult.bActive = false;
	OutResult.ErrorCategory = Category;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript startup activation failed | category=%s | details=%s"),
		Category,
		Details.IsEmpty() ? TEXT("<none>") : *Details);
}

void FAvidScriptStartupCoordinator::FillSuccess(
	FAvidScriptStartupRuntimeResult& OutResult) const
{
	OutResult.bSucceeded = true;
	OutResult.bActive = bActive || !Components.IsEmpty();
	OutResult.ScenarioId = ActiveScenarioId;
	OutResult.DocumentPath = ActiveDocumentPath;
	OutResult.BindingCount = ActiveBindingCount;
	OutResult.ComponentCount = 0;
	OutResult.OwnedActorCount = GetLiveOwnedActorCount();
	OutResult.RuntimeLoadedCount = 0;
	OutResult.BeginPlayCount = 0;
	for (const TWeakObjectPtr<UAvidScriptComponent>& WeakComponent : Components)
	{
		if (const UAvidScriptComponent* Component = WeakComponent.Get())
		{
			++OutResult.ComponentCount;
			OutResult.RuntimeLoadedCount += Component->GetRuntimeStats().bRuntimeLoaded ? 1 : 0;
			OutResult.BeginPlayCount += Component->GetRuntimeStats().bBeginPlayCalled ? 1 : 0;
		}
	}
}
