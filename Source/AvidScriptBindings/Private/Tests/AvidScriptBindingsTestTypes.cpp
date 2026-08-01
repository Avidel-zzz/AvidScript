#include "AvidScriptBindingsTestTypes.h"

int32 UAvidScriptBindingsTestObject::FastPathAddInt32(
	const int32 Left,
	const int32 Right) const
{
	return Left + Right;
}

int32 UAvidScriptBindingsTestObject::FastPathMaxInt32(
	const int32 Left,
	const int32 Right) const
{
	return FMath::Max(Left, Right);
}

float UAvidScriptBindingsTestObject::ReflectionFallbackAddFloat(
	const float Left,
	const float Right) const
{
	return Left + Right;
}

FVector UAvidScriptBindingsTestObject::FastPathVectorValue(
	const FVector& Value) const
{
	return Value * 2.0 + FVector(1.0, 1.0, 1.0);
}

UObject* UAvidScriptBindingsTestObject::FastPathObjectRoundtrip(
	UObject* Value) const
{
	return Value;
}

FAvidScriptBindingsRecursiveStruct
UAvidScriptBindingsTestObject::RecursiveStructRoundtrip(
	const FAvidScriptBindingsRecursiveStruct& Input,
	FAvidScriptBindingsRecursiveStruct& InOut,
	FAvidScriptBindingsRecursiveStruct& OutValue) const
{
	OutValue = Input;
	InOut.bEnabled = Input.bEnabled;
	InOut.Mode = Input.Mode;
	InOut.Position += Input.Position;
	InOut.Target = Input.Target;
	InOut.Nested.Count += Input.Nested.Count;
	InOut.Nested.Ratio += Input.Nested.Ratio;
	return InOut;
}
