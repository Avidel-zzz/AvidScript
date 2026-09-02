#pragma once

#include "CoreMinimal.h"

#include "AvidScriptModuleReference.generated.h"

USTRUCT(BlueprintType)
struct AVIDSCRIPTRUNTIME_API FAvidScriptModuleReference
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AvidScript")
	FName ModuleId;

	bool IsSet() const
	{
		return !ModuleId.IsNone();
	}
};
