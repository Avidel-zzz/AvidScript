#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "AvidScriptDelegateBridge.generated.h"

class IAvidScriptDelegateBridgeSink
{
public:
	virtual ~IAvidScriptDelegateBridgeSink() = default;
	virtual void HandleAvidScriptDelegateBroadcast(
		uint64 SubscriptionToken,
		void* Parameters) = 0;
};

UCLASS(Transient)
class UAvidScriptDelegateBridge final : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(
		IAvidScriptDelegateBridgeSink& InSink,
		uint64 InSubscriptionToken,
		UFunction& InBridgeFunction);
	void Deactivate();

	virtual void ProcessEvent(UFunction* Function, void* Parameters) override;

private:
	IAvidScriptDelegateBridgeSink* Sink = nullptr;
	UFunction* BridgeFunction = nullptr;
	uint64 SubscriptionToken = 0;
};

bool PrepareAvidScriptDelegateBridgeFunction(
	const FString& StableId,
	const UFunction& SignatureFunction,
	UFunction*& OutFunction,
	FString& OutError);
