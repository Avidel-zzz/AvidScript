#include "AvidScriptPerfFixture.h"

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
	FJsObject EmptyCallback)
{
	if (LaneId == ReflectionLaneId)
	{
		ReflectionWorkloadRunner = MoveTemp(WorkloadRunner);
		ReflectionEmptyCallback = MoveTemp(EmptyCallback);
		bHasReflectionCallbacks = true;
	}
	else if (LaneId == StaticLaneId)
	{
		StaticWorkloadRunner = MoveTemp(WorkloadRunner);
		StaticEmptyCallback = MoveTemp(EmptyCallback);
		bHasStaticCallbacks = true;
	}
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

bool AAvidScriptPerfFixture::HasPuertsCallbacks(const int32 LaneId) const
{
	return LaneId == ReflectionLaneId
		? bHasReflectionCallbacks
		: LaneId == StaticLaneId && bHasStaticCallbacks;
}

int32 AAvidScriptPerfFixture::RunPuertsWorkload(
	const int32 LaneId,
	const int32 WorkloadId,
	const int32 Iterations,
	const int32 Seed) const
{
	const FJsObject& Runner = LaneId == ReflectionLaneId
		? ReflectionWorkloadRunner
		: StaticWorkloadRunner;
	return Runner.Func<int32>(WorkloadId, Iterations, Seed);
}

int32 AAvidScriptPerfFixture::RunPuertsEmptyCallback(const int32 LaneId, const int32 Seed) const
{
	const FJsObject& Callback = LaneId == ReflectionLaneId
		? ReflectionEmptyCallback
		: StaticEmptyCallback;
	return Callback.Func<int32>(Seed);
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
