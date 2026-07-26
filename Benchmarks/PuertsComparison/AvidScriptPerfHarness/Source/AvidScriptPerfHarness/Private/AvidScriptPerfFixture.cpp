#include "AvidScriptPerfFixture.h"

#include "Misc/Base64.h"

#include "UEDataBinding.hpp"

namespace
{
	constexpr uint32 PerfFixtureMixMultiplier = 1664525u;
	constexpr uint32 PerfFixtureMixIncrement = 1013904223u;

	int32 PerfFixtureMixInt32(const int32 Value)
	{
		return static_cast<int32>(
			static_cast<uint32>(Value) * PerfFixtureMixMultiplier + PerfFixtureMixIncrement);
	}
}

int32 AAvidScriptPerfFixture::ReflectNoOp(const int32 Value) const
{
	return NativeNoOp(Value);
}

int32 AAvidScriptPerfFixture::ReflectAddInt32(const int32 Left, const int32 Right) const
{
	return NativeAddInt32(Left, Right);
}

void AAvidScriptPerfFixture::ReflectSetScalar(const int32 Value)
{
	NativeSetScalar(Value);
}

int32 AAvidScriptPerfFixture::ReflectGetScalar() const
{
	return NativeGetScalar();
}

FVector AAvidScriptPerfFixture::ReflectVectorValue(const FVector& Value) const
{
	return NativeVectorValue(Value);
}

void AAvidScriptPerfFixture::ReflectVectorRefOut(
	FVector& InOutValue,
	FVector& OutValue) const
{
	NativeVectorRefOut(InOutValue, OutValue);
}

UObject* AAvidScriptPerfFixture::ReflectObjectRoundtrip(UObject* Value) const
{
	return NativeObjectRoundtrip(Value);
}

int32 AAvidScriptPerfFixture::ReflectBatchAdd(const int32 Seed, const int32 Count) const
{
	return NativeBatchAdd(Seed, Count);
}

void AAvidScriptPerfFixture::RegisterPuertsCallbacks(
	const int32 LaneId,
	FJsObject WorkloadRunner,
	FJsObject ResetCallback,
	FJsObject EmptyCallback,
	FJsObject TickCallback,
	FJsObject GetCallbackChecksum)
{
	if (LaneId == ReflectionLaneId)
	{
		ReflectionWorkloadRunner = MoveTemp(WorkloadRunner);
		ReflectionResetCallback = MoveTemp(ResetCallback);
		ReflectionEmptyCallback = MoveTemp(EmptyCallback);
		ReflectionTickCallback = MoveTemp(TickCallback);
		ReflectionGetCallbackChecksum = MoveTemp(GetCallbackChecksum);
		bHasReflectionCallbacks = true;
	}
	else if (LaneId == StaticLaneId)
	{
		StaticWorkloadRunner = MoveTemp(WorkloadRunner);
		StaticResetCallback = MoveTemp(ResetCallback);
		StaticEmptyCallback = MoveTemp(EmptyCallback);
		StaticTickCallback = MoveTemp(TickCallback);
		StaticGetCallbackChecksum = MoveTemp(GetCallbackChecksum);
		bHasStaticCallbacks = true;
	}
}

FString AAvidScriptPerfFixture::GetControlledWasmBase64() const
{
	return ControlledWasmBase64;
}

FString AAvidScriptPerfFixture::GetControlledWasmSha256() const
{
	return ControlledWasmSha256;
}

void AAvidScriptPerfFixture::RegisterControlledWasmRunner(
	FJsObject Runner,
	const FString& AdapterProofId,
	const FString& SourceWasmSha256,
	const FString& ArtifactWasmSha256)
{
	ControlledWasmRunner = MoveTemp(Runner);
	ControlledAdapterProofId = AdapterProofId;
	ControlledAdapterSourceWasmSha256 = SourceWasmSha256;
	ControlledAdapterArtifactWasmSha256 = ArtifactWasmSha256;
	bHasControlledWasmRunner = true;
}

void AAvidScriptPerfFixture::SetControlledWasmBytes(
	const TConstArrayView<uint8> Bytes,
	const FString& WasmSha256)
{
	ControlledWasmBase64 = FBase64::Encode(Bytes.GetData(), Bytes.Num());
	ControlledWasmSha256 = WasmSha256;
	ControlledWasmRunner = FJsObject();
	bHasControlledWasmRunner = false;
	ControlledAdapterProofId.Reset();
	ControlledAdapterSourceWasmSha256.Reset();
	ControlledAdapterArtifactWasmSha256.Reset();
}

bool AAvidScriptPerfFixture::HasControlledWasmRunner() const
{
	return bHasControlledWasmRunner;
}

bool AAvidScriptPerfFixture::ControlledRunnerUsesWebAssembly() const
{
	return bHasControlledWasmRunner &&
		ControlledAdapterProofId ==
			TEXT("webassembly.module_instance.cached_export.v1") &&
		ControlledAdapterSourceWasmSha256 == ControlledWasmSha256 &&
		ControlledAdapterArtifactWasmSha256 == ControlledWasmSha256;
}

const FString& AAvidScriptPerfFixture::GetControlledAdapterProofId() const
{
	return ControlledAdapterProofId;
}

const FString& AAvidScriptPerfFixture::GetControlledAdapterSourceWasmSha256() const
{
	return ControlledAdapterSourceWasmSha256;
}

const FString& AAvidScriptPerfFixture::GetControlledAdapterArtifactWasmSha256() const
{
	return ControlledAdapterArtifactWasmSha256;
}

int32 AAvidScriptPerfFixture::RunControlledWasm(
	const int32 Iterations,
	const int32 Seed) const
{
	return ControlledWasmRunner.Func<int32>(Iterations, Seed);
}

int32 AAvidScriptPerfFixture::NativeNoOp(const int32 Value) const
{
	RecordOperation(1);
	return Value;
}

int32 AAvidScriptPerfFixture::NativeAddInt32(const int32 Left, const int32 Right) const
{
	RecordOperation(2);
	return static_cast<int32>(static_cast<uint32>(Left) + static_cast<uint32>(Right));
}

void AAvidScriptPerfFixture::NativeSetScalar(const int32 Value)
{
	ScalarValue = Value;
}

int32 AAvidScriptPerfFixture::NativeGetScalar() const
{
	return ScalarValue;
}

FVector AAvidScriptPerfFixture::NativeVectorValue(const FVector& Value) const
{
	RecordOperation(4);
	return Value + FVector(1.0, 2.0, 3.0);
}

void AAvidScriptPerfFixture::NativeVectorRefOut(
	FVector& InOutValue,
	FVector& OutValue) const
{
	RecordOperation(9);
	OutValue = InOutValue + FVector(4.0, 5.0, 6.0);
	InOutValue += FVector(1.0, 2.0, 3.0);
}

UObject* AAvidScriptPerfFixture::NativeObjectRoundtrip(UObject* Value) const
{
	RecordOperation(5);
	return Value;
}

int32 AAvidScriptPerfFixture::NativeBatchAdd(const int32 Seed, const int32 Count) const
{
	RecordOperation(6);
	int32 Result = Seed;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Result = PerfFixtureMixInt32(Result ^ Index);
	}
	return Result;
}

void AAvidScriptPerfFixture::ResetNativeCallbackState(const int32 Seed)
{
	NativeCallbackChecksum = static_cast<uint32>(Seed);
}

void AAvidScriptPerfFixture::NativeEmptyCallback(const int32 Token)
{
	NativeCallbackChecksum = static_cast<uint32>(
		PerfFixtureMixInt32(static_cast<int32>(
			NativeCallbackChecksum ^ static_cast<uint32>(Token))));
}

void AAvidScriptPerfFixture::NativeTickCallback(const float DeltaSeconds)
{
	(void)DeltaSeconds;
	NativeCallbackChecksum = static_cast<uint32>(
		PerfFixtureMixInt32(static_cast<int32>(NativeCallbackChecksum ^ 1u)));
}

int32 AAvidScriptPerfFixture::GetNativeCallbackChecksum() const
{
	return static_cast<int32>(NativeCallbackChecksum);
}

bool AAvidScriptPerfFixture::HasPuertsCallbacks(const int32 LaneId) const
{
	return LaneId == ReflectionLaneId
		? bHasReflectionCallbacks
		: LaneId == StaticLaneId && bHasStaticCallbacks;
}

void AAvidScriptPerfFixture::RunPuertsWorkload(
	const int32 LaneId,
	const int32 WorkloadId,
	const int32 Iterations,
	const int32 Seed)
{
	const FJsObject& Runner = LaneId == ReflectionLaneId
		? ReflectionWorkloadRunner
		: StaticWorkloadRunner;
	Runner.Action(this, WorkloadId, Iterations, Seed);
}

void AAvidScriptPerfFixture::ResetPuertsCallbackState(
	const int32 LaneId,
	const int32 Seed) const
{
	const FJsObject& Callback = LaneId == ReflectionLaneId
		? ReflectionResetCallback
		: StaticResetCallback;
	Callback.Action(Seed);
}

void AAvidScriptPerfFixture::RunPuertsEmptyCallback(
	const int32 LaneId,
	const int32 Token) const
{
	const FJsObject& Callback = LaneId == ReflectionLaneId
		? ReflectionEmptyCallback
		: StaticEmptyCallback;
	Callback.Action(Token);
}

void AAvidScriptPerfFixture::RunPuertsTickCallback(
	const int32 LaneId,
	const float DeltaSeconds) const
{
	const FJsObject& Callback = LaneId == ReflectionLaneId
		? ReflectionTickCallback
		: StaticTickCallback;
	Callback.Action(DeltaSeconds);
}

int32 AAvidScriptPerfFixture::GetPuertsCallbackChecksum(const int32 LaneId) const
{
	const FJsObject& Callback = LaneId == ReflectionLaneId
		? ReflectionGetCallbackChecksum
		: StaticGetCallbackChecksum;
	return Callback.Func<int32>();
}

void AAvidScriptPerfFixture::ResetOperationCounts()
{
	FMemory::Memzero(OperationCallCounts);
}

uint64 AAvidScriptPerfFixture::GetOperationCallCount(const int32 WorkloadId) const
{
	return WorkloadId >= 0 && WorkloadId < UE_ARRAY_COUNT(OperationCallCounts)
		? OperationCallCounts[WorkloadId]
		: 0;
}

void AAvidScriptPerfFixture::RecordOperation(const int32 WorkloadId) const
{
	if (WorkloadId >= 0 && WorkloadId < UE_ARRAY_COUNT(OperationCallCounts))
	{
		++OperationCallCounts[WorkloadId];
	}
}
