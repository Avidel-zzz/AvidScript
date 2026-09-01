#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include "AvidScriptContinuationTestTypes.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(
	FAvidScriptRuntimeAsyncActionOutcome);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FAvidScriptRuntimeAsyncActionPayloadOutcome,
	int32, Value);

UCLASS()
class UAvidScriptRuntimeAsyncActionTestObject final
	: public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvidScriptRuntimeAsyncActionOutcome Cancelled;

	UPROPERTY(BlueprintAssignable)
	FAvidScriptRuntimeAsyncActionOutcome Completed;

	virtual void Activate() override
	{
		++ActivationCount;
		if (OutcomeOnActivate == 0)
		{
			Cancelled.Broadcast();
		}
		else if (OutcomeOnActivate == 1)
		{
			Completed.Broadcast();
		}
	}

	void BroadcastCancelled()
	{
		Cancelled.Broadcast();
	}

	void BroadcastCompleted()
	{
		Completed.Broadcast();
	}

	int32 ActivationCount = 0;
	int32 OutcomeOnActivate = INDEX_NONE;
};

UCLASS()
class UAvidScriptRuntimeAsyncActionPayloadTestObject final
	: public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FAvidScriptRuntimeAsyncActionPayloadOutcome Completed;

	virtual void Activate() override
	{
		++ActivationCount;
	}

	void BroadcastCompleted(const int32 Value)
	{
		Completed.Broadcast(Value);
	}

	int32 ActivationCount = 0;
};
