#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AvidScriptEditorTypedObjectBindingTestTypes.generated.h"

UCLASS()
class AAvidScriptTypedTestActor : public AActor
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable)
	void ApplyGameplayValue(float Delta)
	{
		GameplayValue += Delta;
	}

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "AvidScript")
	float GameplayValue = 0.0f;
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
