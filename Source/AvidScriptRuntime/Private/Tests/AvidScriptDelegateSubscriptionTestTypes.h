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

DECLARE_DYNAMIC_DELEGATE_OneParam(
	FAvidScriptRuntimeSinglecastTestSignal,
	int32,
	IntValue);

UCLASS()
class UAvidScriptRuntimeDelegateTestObject final : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FAvidScriptRuntimeDelegateTestSignal OnSignal;

	UPROPERTY()
	FAvidScriptRuntimeSinglecastTestSignal OnSinglecast;

	void Broadcast(UObject* ObjectValue, int32 IntValue, float FloatValue)
	{
		OnSignal.Broadcast(ObjectValue, IntValue, FloatValue);
	}

	void ExecuteSinglecast(const int32 IntValue)
	{
		OnSinglecast.ExecuteIfBound(IntValue);
	}

	UFUNCTION()
	void NativeSinglecastValue(int32 Value)
	{
		LastNativeSinglecastValue = Value;
		++NativeSinglecastInvocationCount;
	}

	UFUNCTION()
	void ExternalSinglecastValue(int32 Value)
	{
		LastExternalSinglecastValue = Value;
		++ExternalSinglecastInvocationCount;
	}

	UFUNCTION()
	void NativeInboundValue(int32 Value)
	{
		LastNativeValue = Value;
		++NativeInvocationCount;
	}

	int32 LastNativeValue = 0;
	int32 NativeInvocationCount = 0;
	int32 LastNativeSinglecastValue = 0;
	int32 NativeSinglecastInvocationCount = 0;
	int32 LastExternalSinglecastValue = 0;
	int32 ExternalSinglecastInvocationCount = 0;
};
