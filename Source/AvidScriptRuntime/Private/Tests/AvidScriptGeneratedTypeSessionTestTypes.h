#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "AvidScriptGeneratedTypeSessionTestTypes.generated.h"

UCLASS()
class UAvidScriptGeneratedTypeSessionTestObject final : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 Value = 0;

	UFUNCTION()
	int32 GetScriptValue() const
	{
		return 0;
	}
};
