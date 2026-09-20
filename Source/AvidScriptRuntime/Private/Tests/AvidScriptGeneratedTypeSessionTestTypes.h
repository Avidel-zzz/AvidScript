#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "AvidScriptGeneratedTypeSessionTestTypes.generated.h"

UCLASS()
class UAvidScriptGeneratedTypeSessionTestObject : public UObject
{
	GENERATED_BODY()

public:
	UWorld* GetWorld() const override { return GetOuter() ? GetOuter()->GetWorld() : nullptr; }

	UPROPERTY()
	int32 Value = 0;

	UFUNCTION()
	int32 GetScriptValue() const
	{
		return 0;
	}
};

UCLASS()
class UAvidScriptGeneratedTypeDerivedTestObject : public UAvidScriptGeneratedTypeSessionTestObject
{
	GENERATED_BODY()
};

UCLASS()
class UAvidScriptGeneratedTypeNativeChildTestObject final : public UAvidScriptGeneratedTypeDerivedTestObject
{
	GENERATED_BODY()
};
