#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "JsObject.h"

#include "AvidScriptPerfFixture.generated.h"

UCLASS()
class AVIDSCRIPTPERFHARNESS_API AAvidScriptPerfFixture final : public AActor
{
	GENERATED_BODY()

public:
	static constexpr int32 ReflectionLaneId = 1;
	static constexpr int32 StaticLaneId = 2;

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
	void RegisterPuertsCallbacks(int32 LaneId, FJsObject WorkloadRunner, FJsObject EmptyCallback);

	int32 NativeNoOp(int32 Value) const;
	int32 NativeAddInt32(int32 Left, int32 Right) const;
	void NativeSetScalar(int32 Value);
	int32 NativeGetScalar() const;
	FVector NativeVectorValue(const FVector& Value) const;
	UObject* NativeObjectRoundtrip(UObject* Value) const;
	int32 NativeBatchAdd(int32 Seed, int32 Count) const;

	bool HasPuertsCallbacks(int32 LaneId) const;
	int32 RunPuertsWorkload(int32 LaneId, int32 WorkloadId, int32 Iterations, int32 Seed) const;
	int32 RunPuertsEmptyCallback(int32 LaneId, int32 Seed) const;
	void ResetOperationCounts();
	uint64 GetOperationCallCount(int32 WorkloadId) const;

private:
	void RecordOperation(int32 WorkloadId) const;

	FJsObject ReflectionWorkloadRunner;
	FJsObject ReflectionEmptyCallback;
	FJsObject StaticWorkloadRunner;
	FJsObject StaticEmptyCallback;
	bool bHasReflectionCallbacks = false;
	bool bHasStaticCallbacks = false;
	mutable uint64 OperationCallCounts[7] = {};
};
