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
	void ReflectVectorRefOut(UPARAM(ref) FVector& InOutValue, FVector& OutValue) const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	UObject* ReflectObjectRoundtrip(UObject* Value) const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	int32 ReflectBatchAdd(int32 Seed, int32 Count) const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	void RegisterPuertsCallbacks(
		int32 LaneId,
		FJsObject WorkloadRunner,
		FJsObject ResetCallback,
		FJsObject EmptyCallback,
		FJsObject TickCallback,
		FJsObject GetCallbackChecksum);

	UFUNCTION(BlueprintPure, Category = "AvidScript|Performance")
	FString GetControlledWasmBase64() const;

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Performance")
	void RegisterControlledWasmRunner(
		FJsObject Runner,
		bool bUsesWebAssemblyModule,
		bool bUsesWebAssemblyInstance);

	void SetControlledWasmBytes(TConstArrayView<uint8> Bytes);
	bool HasControlledWasmRunner() const;
	bool ControlledRunnerUsesWebAssembly() const;
	int32 RunControlledWasm(int32 Iterations, int32 Seed) const;

	int32 NativeNoOp(int32 Value) const;
	int32 NativeAddInt32(int32 Left, int32 Right) const;
	void NativeSetScalar(int32 Value);
	int32 NativeGetScalar() const;
	FVector NativeVectorValue(const FVector& Value) const;
	void NativeVectorRefOut(FVector& InOutValue, FVector& OutValue) const;
	UObject* NativeObjectRoundtrip(UObject* Value) const;
	int32 NativeBatchAdd(int32 Seed, int32 Count) const;
	void ResetNativeCallbackState(int32 Seed);
	void NativeEmptyCallback(int32 Token);
	void NativeTickCallback(float DeltaSeconds);
	int32 GetNativeCallbackChecksum() const;

	bool HasPuertsCallbacks(int32 LaneId) const;
	void RunPuertsWorkload(int32 LaneId, int32 WorkloadId, int32 Iterations, int32 Seed);
	void ResetPuertsCallbackState(int32 LaneId, int32 Seed) const;
	void RunPuertsEmptyCallback(int32 LaneId, int32 Token) const;
	void RunPuertsTickCallback(int32 LaneId, float DeltaSeconds) const;
	int32 GetPuertsCallbackChecksum(int32 LaneId) const;
	void ResetOperationCounts();
	uint64 GetOperationCallCount(int32 WorkloadId) const;

private:
	void RecordOperation(int32 WorkloadId) const;

	FJsObject ReflectionWorkloadRunner;
	FJsObject ReflectionResetCallback;
	FJsObject ReflectionEmptyCallback;
	FJsObject ReflectionTickCallback;
	FJsObject ReflectionGetCallbackChecksum;
	FJsObject StaticWorkloadRunner;
	FJsObject StaticResetCallback;
	FJsObject StaticEmptyCallback;
	FJsObject StaticTickCallback;
	FJsObject StaticGetCallbackChecksum;
	FJsObject ControlledWasmRunner;
	FString ControlledWasmBase64;
	bool bHasReflectionCallbacks = false;
	bool bHasStaticCallbacks = false;
	bool bHasControlledWasmRunner = false;
	bool bControlledUsesWebAssemblyModule = false;
	bool bControlledUsesWebAssemblyInstance = false;
	uint32 NativeCallbackChecksum = 0;
	mutable uint64 OperationCallCounts[10] = {};
};
