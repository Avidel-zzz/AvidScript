#pragma once

#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AvidScriptEditorTypedObjectBindingTestTypes.generated.h"

UCLASS()
class UAvidScriptTypedTestSceneComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable)
	void ApplyGameplayPulse(float DeltaSeconds)
	{
		++GameplayPulseCount;
		AccumulatedDeltaSeconds += DeltaSeconds;
	}

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "AvidScript")
	int32 GameplayPulseCount = 0;

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "AvidScript")
	float AccumulatedDeltaSeconds = 0.0f;
};

UCLASS()
class AAvidScriptTypedTestActor : public AActor
{
	GENERATED_BODY()

public:
	AAvidScriptTypedTestActor()
	{
		TypedRootComponent = CreateDefaultSubobject<
			UAvidScriptTypedTestSceneComponent>(TEXT("TypedRootComponent"));
		SetRootComponent(TypedRootComponent);
	}

	UFUNCTION(BlueprintCallable)
	void ApplyGameplayValue(float Delta)
	{
		GameplayValue += Delta;
	}

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "AvidScript")
	float GameplayValue = 0.0f;

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "AvidScript")
	TObjectPtr<UAvidScriptTypedTestSceneComponent> TypedRootComponent;
};

UCLASS()
class AAvidScriptTypedTestProjectile : public AAvidScriptTypedTestActor
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable)
	void ActivateProjectile()
	{
		++ActivationCount;
	}

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "AvidScript")
	int32 ActivationCount = 0;
};
