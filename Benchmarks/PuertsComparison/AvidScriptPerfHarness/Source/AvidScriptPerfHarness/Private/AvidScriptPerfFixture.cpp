#include "AvidScriptPerfFixture.h"

namespace
{
	constexpr uint32 MixMultiplier = 1664525u;
	constexpr uint32 MixIncrement = 1013904223u;

	int32 MixInt32(const int32 Value)
	{
		return static_cast<int32>(static_cast<uint32>(Value) * MixMultiplier + MixIncrement);
	}
}

int32 UAvidScriptPerfFixture::ReflectNoOp(const int32 Value) const
{
	return NativeNoOp(Value);
}

int32 UAvidScriptPerfFixture::ReflectAddInt32(const int32 Left, const int32 Right) const
{
	return NativeAddInt32(Left, Right);
}

void UAvidScriptPerfFixture::ReflectSetScalar(const int32 Value)
{
	NativeSetScalar(Value);
}

int32 UAvidScriptPerfFixture::ReflectGetScalar() const
{
	return NativeGetScalar();
}

FVector UAvidScriptPerfFixture::ReflectVectorValue(const FVector& Value) const
{
	return NativeVectorValue(Value);
}

UObject* UAvidScriptPerfFixture::ReflectObjectRoundtrip(UObject* Value) const
{
	return NativeObjectRoundtrip(Value);
}

int32 UAvidScriptPerfFixture::ReflectBatchAdd(const int32 Seed, const int32 Count) const
{
	return NativeBatchAdd(Seed, Count);
}

void UAvidScriptPerfFixture::RegisterPuertsCallbacks(FJsObject WorkloadRunner, FJsObject EmptyCallback)
{
	PuertsWorkloadRunner = MoveTemp(WorkloadRunner);
	PuertsEmptyCallback = MoveTemp(EmptyCallback);
	bHasPuertsCallbacks = true;
}

int32 UAvidScriptPerfFixture::NativeNoOp(const int32 Value) const
{
	return Value;
}

int32 UAvidScriptPerfFixture::NativeAddInt32(const int32 Left, const int32 Right) const
{
	return static_cast<int32>(static_cast<uint32>(Left) + static_cast<uint32>(Right));
}

void UAvidScriptPerfFixture::NativeSetScalar(const int32 Value)
{
	ScalarValue = Value;
}

int32 UAvidScriptPerfFixture::NativeGetScalar() const
{
	return ScalarValue;
}

FVector UAvidScriptPerfFixture::NativeVectorValue(const FVector& Value) const
{
	return Value + FVector(1.0, 2.0, 3.0);
}

UObject* UAvidScriptPerfFixture::NativeObjectRoundtrip(UObject* Value) const
{
	return Value;
}

int32 UAvidScriptPerfFixture::NativeBatchAdd(const int32 Seed, const int32 Count) const
{
	int32 Result = Seed;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Result = MixInt32(Result ^ Index);
	}
	return Result;
}

bool UAvidScriptPerfFixture::HasPuertsCallbacks() const
{
	return bHasPuertsCallbacks;
}

int32 UAvidScriptPerfFixture::RunPuertsWorkload(
	const int32 WorkloadId,
	const int32 Iterations,
	const int32 Seed) const
{
	return PuertsWorkloadRunner.Func<int32>(WorkloadId, Iterations, Seed);
}

int32 UAvidScriptPerfFixture::RunPuertsEmptyCallback(const int32 Seed) const
{
	return PuertsEmptyCallback.Func<int32>(Seed);
}
