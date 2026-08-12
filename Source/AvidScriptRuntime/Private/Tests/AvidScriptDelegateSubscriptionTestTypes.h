#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "AvidScriptDelegateSubscriptionTestTypes.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FAvidScriptRuntimeDelegateTestSignal,
	UObject*,
	ObjectValue,
	int32,
	IntValue,
	float,
	FloatValue);

UCLASS()
class UAvidScriptRuntimeDelegateTestObject final : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FAvidScriptRuntimeDelegateTestSignal OnSignal;

	void Broadcast(UObject* ObjectValue, int32 IntValue, float FloatValue)
	{
		OnSignal.Broadcast(ObjectValue, IntValue, FloatValue);
	}
};
