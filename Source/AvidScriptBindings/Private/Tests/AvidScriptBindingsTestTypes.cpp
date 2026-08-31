#include "AvidScriptBindingsTestTypes.h"

int32 UAvidScriptBindingsInterfaceImplementer::TransformInterfaceValue_Implementation(
	const int32 Value)
{
	++InvocationCount;
	return Value * 3 + 1;
}

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

FString UAvidScriptBindingsTestObject::Utf8Roundtrip(
	const FName& InputName,
	const FString& InputString,
	FName& InOutName,
	FString& InOutString,
	FName& OutName)
{
	++Utf8InvocationCount;
	OutName = InputName;
	InOutName = FName(*(InOutName.ToString() + TEXT("_Touched")));
	InOutString += TEXT("|") + InputString;
	return InputName.ToString() + TEXT(":") + InputString;
}

TArray<int32> UAvidScriptBindingsTestObject::IntArrayRoundtrip(
	const TArray<int32>& Input,
	TArray<int32>& InOut,
	TArray<int32>& OutValue) const
{
	OutValue = Input;
	InOut.Append(Input);
	return InOut;
}
