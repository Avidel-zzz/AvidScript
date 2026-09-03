#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AvidScriptPrimitiveHost.generated.h"

class UStaticMeshComponent;

UCLASS(BlueprintType)
class AVIDSCRIPTRUNTIME_API AAvidScriptPrimitiveHost : public AActor
{
	GENERATED_BODY()

public:
	AAvidScriptPrimitiveHost();

	UStaticMeshComponent* GetPrimitiveMesh() const { return PrimitiveMesh; }

private:
	UPROPERTY(VisibleAnywhere, Category = "AvidScript")
	TObjectPtr<UStaticMeshComponent> PrimitiveMesh;
};
