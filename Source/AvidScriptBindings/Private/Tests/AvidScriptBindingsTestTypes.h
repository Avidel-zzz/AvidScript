#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "AvidScriptBindingsTestTypes.generated.h"

UCLASS()
class UAvidScriptBindingsTestObject : public UObject
{
    GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, Category = "AvidScript|Tests")
	int32 FastPathInt32Property = 0;

	UFUNCTION(BlueprintPure, Category = "AvidScript|Tests")
	int32 FastPathAddInt32(int32 Left, int32 Right) const;

	UFUNCTION(BlueprintPure, Category = "AvidScript|Tests")
	int32 FastPathMaxInt32(int32 Left, int32 Right) const;

	UFUNCTION(BlueprintPure, Category = "AvidScript|Tests")
	float ReflectionFallbackAddFloat(float Left, float Right) const;

	UFUNCTION(BlueprintPure, Category = "AvidScript|Tests")
	FVector FastPathVectorValue(FVector Value) const;

	UFUNCTION(BlueprintPure, Category = "AvidScript|Tests")
	UObject* FastPathObjectRoundtrip(UObject* Value) const;
};
