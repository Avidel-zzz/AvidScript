#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "AvidScriptLatentCallbackProxy.generated.h"

class FAvidScriptSessionContinuations;

UCLASS()
class UAvidScriptLatentCallbackProxy final : public UObject
{
	GENERATED_BODY()

public:
	void Arm(
		TWeakPtr<FAvidScriptSessionContinuations> InOwner,
		int64 InToken);
	void Disarm();

	UFUNCTION()
	void OnLatentCompleted(int32 Linkage);

private:
	TWeakPtr<FAvidScriptSessionContinuations> Owner;
	int64 Token = 0;
};
