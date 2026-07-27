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
