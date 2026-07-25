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
class UAvidScriptSessionOwnershipTestComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	int32 DestructionOrderId = 0;

	static TArray<int32>& GetDestructionOrder()
	{
		static TArray<int32> DestructionOrder;
		return DestructionOrder;
	}
	static TFunction<void()>& GetDestructionObserver()
	{
		static TFunction<void()> Observer;
		return Observer;
	}

	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override
	{
		GetDestructionOrder().Add(DestructionOrderId);
		if (GetDestructionObserver())
		{
			TFunction<void()> Observer = MoveTemp(GetDestructionObserver());
			Observer();
		}
		Super::OnComponentDestroyed(bDestroyingHierarchy);
	}
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

	UPROPERTY()
	TObjectPtr<UAvidScriptObjectRegistryTestObject> HostEffectObjectProperty;
};
