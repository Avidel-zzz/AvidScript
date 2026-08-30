#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "AvidScriptGeneratedTypeSessionTestTypes.generated.h"

UCLASS()
class UAvidScriptGeneratedTypeSessionTestObject final : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	int32 GetScriptValue() const
	{
		return 0;
	}
};
