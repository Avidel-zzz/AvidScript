#pragma once

#include "CoreMinimal.h"
#include "Startup/AvidScriptStartupScenario.h"

class AActor;
class UAvidScriptComponent;
class UWorld;

struct FAvidScriptStartupRuntimeResult
{
	bool bRequested = false;
	bool bSucceeded = false;
	bool bActive = false;
	FString ScenarioId;
	FString DocumentPath;
	FString ErrorCategory;
	FString ErrorMessage;
	int32 BindingCount = 0;
	int32 ComponentCount = 0;
	int32 OwnedActorCount = 0;
	int32 RuntimeLoadedCount = 0;
	int32 BeginPlayCount = 0;
};

class FAvidScriptStartupCoordinator
{
public:
	bool ActivateFromProcess(UWorld& World, FAvidScriptStartupRuntimeResult& OutResult);
	bool Activate(
		UWorld& World,
		const FAvidScriptStartupScenario& Scenario,
		FAvidScriptStartupRuntimeResult& OutResult,
		bool bUseEmbeddedSmokeModuleForTesting = false);
	void Deactivate();

	bool IsActive() const { return bActive; }
	void GetLiveComponents(TArray<UAvidScriptComponent*>& OutComponents) const;
	int32 GetLiveOwnedActorCount() const;

private:
	bool ActivateBinding(
		UWorld& World,
		const FAvidScriptStartupBinding& Binding,
		bool bUseEmbeddedSmokeModuleForTesting,
		FAvidScriptStartupRuntimeResult& OutResult);
	bool AttachComponent(
		AActor& Actor,
		const FAvidScriptStartupBinding& Binding,
		bool bUseEmbeddedSmokeModuleForTesting,
		FAvidScriptStartupRuntimeResult& OutResult);
	AActor* SpawnOwnedActor(
		UWorld& World,
		UClass& ActorClass,
		const FTransform& Transform,
		bool bCreateSceneRoot,
		FAvidScriptStartupRuntimeResult& OutResult);
	void SetFailure(
		FAvidScriptStartupRuntimeResult& OutResult,
		const TCHAR* Category,
		const FString& Details);
	void FillSuccess(FAvidScriptStartupRuntimeResult& OutResult) const;

	TWeakObjectPtr<UWorld> ActiveWorld;
	TArray<TWeakObjectPtr<UAvidScriptComponent>> Components;
	TArray<TWeakObjectPtr<AActor>> OwnedActors;
	FString ActiveScenarioId;
	FString ActiveDocumentPath;
	int32 ActiveBindingCount = 0;
	bool bActive = false;
};
