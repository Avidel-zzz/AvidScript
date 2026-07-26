#pragma once

#include "CoreMinimal.h"
#include "JsObject.h"
#include "UObject/Object.h"

#include "AvidScriptPerfFixture.generated.h"

UCLASS()
class AVIDSCRIPTPERFHARNESS_API UAvidScriptPerfFixture final : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AvidScript|Performance")
	int32 ScalarValue = 0;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	int32 ReflectNoOp(int32 Value) const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	int32 ReflectAddInt32(int32 Left, int32 Right) const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	void ReflectSetScalar(int32 Value);

	UFUNCTION(BlueprintPure, Category = "AvidScript|Performance")
	int32 ReflectGetScalar() const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	FVector ReflectVectorValue(const FVector& Value) const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	UObject* ReflectObjectRoundtrip(UObject* Value) const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	int32 ReflectBatchAdd(int32 Seed, int32 Count) const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	void RegisterPuertsCallbacks(FJsObject WorkloadRunner, FJsObject EmptyCallback);

	int32 NativeNoOp(int32 Value) const;
	int32 NativeAddInt32(int32 Left, int32 Right) const;
	void NativeSetScalar(int32 Value);
	int32 NativeGetScalar() const;
	FVector NativeVectorValue(const FVector& Value) const;
	UObject* NativeObjectRoundtrip(UObject* Value) const;
	int32 NativeBatchAdd(int32 Seed, int32 Count) const;

	bool HasPuertsCallbacks() const;
	int32 RunPuertsWorkload(int32 WorkloadId, int32 Iterations, int32 Seed) const;
	int32 RunPuertsEmptyCallback(int32 Seed) const;

private:
	FJsObject PuertsWorkloadRunner;
	FJsObject PuertsEmptyCallback;
	bool bHasPuertsCallbacks = false;
};
