#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/Object.h"

#include "AvidScriptObjectRegistryTestTypes.generated.h"

UCLASS()
class UAvidScriptObjectRegistryTestObject : public UObject
{
	GENERATED_BODY()
};

UCLASS()
class AAvidScriptActorBindingTestActor : public AActor
{
	GENERATED_BODY()

public:
	AAvidScriptActorBindingTestActor()
	{
		RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	}
};
