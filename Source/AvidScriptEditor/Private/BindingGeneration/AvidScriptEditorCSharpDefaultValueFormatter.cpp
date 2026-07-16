#include "BindingGeneration/AvidScriptEditorCSharpDefaultValueFormatter.h"

#include "BindingGeneration/AvidScriptEditorCSharpSyntax.h"
#include "String/LexFromString.h"

namespace
{
const FAvidScriptBindingTypeModel* FindDefaultValueType(
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
	const FString& CanonicalType)
{
	const FAvidScriptBindingTypeModel* const* Type = TypesByCanonical.Find(CanonicalType);
	return Type == nullptr ? nullptr : *Type;
}

bool TryParseUnsignedDecimal(
	const FString& Input,
	uint64 MaxValue,
	uint64& OutValue)
{
	const FString Value = Input.TrimStartAndEnd();
	if (Value.IsEmpty())
	{
		return false;
	}
	int32 Index = Value[0] == TEXT('+') ? 1 : 0;
	if (Index == Value.Len())
	{
		return false;
	}
	uint64 Parsed = 0;
	for (; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!FChar::IsDigit(Character))
		{
			return false;
		}
		const uint64 Digit = static_cast<uint64>(Character - TEXT('0'));
		if (Parsed > (MaxValue - Digit) / 10)
		{
			return false;
		}
		Parsed = Parsed * 10 + Digit;
	}
	OutValue = Parsed;
	return true;
}

bool TryParseSignedDecimal(
	const FString& Input,
	int64 MinValue,
	int64 MaxValue,
	int64& OutValue)
{
	const FString Value = Input.TrimStartAndEnd();
	if (Value.IsEmpty())
	{
		return false;
	}
	int32 Index = 0;
	bool bNegative = false;
	if (Value[0] == TEXT('-') || Value[0] == TEXT('+'))
	{
		bNegative = Value[0] == TEXT('-');
		++Index;
	}
	if (Index == Value.Len())
	{
		return false;
	}
	const uint64 Limit = bNegative
		? static_cast<uint64>(-(MinValue + 1)) + 1
		: static_cast<uint64>(MaxValue);
	uint64 Magnitude = 0;
	for (; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!FChar::IsDigit(Character))
		{
			return false;
		}
		const uint64 Digit = static_cast<uint64>(Character - TEXT('0'));
		if (Magnitude > (Limit - Digit) / 10)
		{
			return false;
		}
		Magnitude = Magnitude * 10 + Digit;
	}
	if (bNegative)
	{
		OutValue = Magnitude == static_cast<uint64>(MAX_int64) + 1
			? MIN_int64
			: -static_cast<int64>(Magnitude);
	}
	else
	{
		OutValue = static_cast<int64>(Magnitude);
	}
	return OutValue >= MinValue && OutValue <= MaxValue;
}

bool IsStrictDecimalFloat(const FString& Input)
{
	const FString Value = Input.TrimStartAndEnd();
	if (Value.IsEmpty())
	{
		return false;
	}
	int32 Index = 0;
	if (Value[Index] == TEXT('-') || Value[Index] == TEXT('+'))
	{
		++Index;
	}
	bool bHasMantissaDigit = false;
	while (Index < Value.Len() && FChar::IsDigit(Value[Index]))
	{
		bHasMantissaDigit = true;
		++Index;
	}
	if (Index < Value.Len() && Value[Index] == TEXT('.'))
	{
		++Index;
		while (Index < Value.Len() && FChar::IsDigit(Value[Index]))
		{
			bHasMantissaDigit = true;
			++Index;
		}
	}
	if (!bHasMantissaDigit)
	{
		return false;
	}
	if (Index < Value.Len() && (Value[Index] == TEXT('e') || Value[Index] == TEXT('E')))
	{
		++Index;
		if (Index < Value.Len() && (Value[Index] == TEXT('-') || Value[Index] == TEXT('+')))
		{
			++Index;
		}
		const int32 ExponentStart = Index;
		while (Index < Value.Len() && FChar::IsDigit(Value[Index]))
		{
			++Index;
		}
		if (Index == ExponentStart)
		{
			return false;
		}
	}
	return Index == Value.Len();
}

} // namespace

bool FAvidScriptEditorCSharpDefaultValueFormatter::TryFormat(
	const FAvidScriptBindingValueModel& Value,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
	FString& OutExpression)
{
	if (!Value.bHasDefault || Value.Direction == TEXT("ref") || Value.Direction == TEXT("out"))
	{
		return false;
	}
	if (Value.CanonicalType == TEXT("scalar:bool"))
	{
		if (Value.DefaultValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Value.DefaultValue == TEXT("1"))
		{
			OutExpression = TEXT("true");
			return true;
		}
		if (Value.DefaultValue.Equals(TEXT("false"), ESearchCase::IgnoreCase)
			|| Value.DefaultValue == TEXT("0"))
		{
			OutExpression = TEXT("false");
			return true;
		}
		return false;
	}
	if (Value.Kind == TEXT("object_handle")
		&& (Value.DefaultValue.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			|| Value.DefaultValue.Equals(TEXT("nullptr"), ESearchCase::IgnoreCase)
			|| Value.DefaultValue.Equals(TEXT("null"), ESearchCase::IgnoreCase)))
	{
		OutExpression = TEXT("default");
		return true;
	}
	if (Value.Kind == TEXT("enum"))
	{
		const FAvidScriptBindingTypeModel* EnumType = FindDefaultValueType(TypesByCanonical, Value.CanonicalType);
		if (EnumType == nullptr || EnumType->Kind != TEXT("enum"))
		{
			return false;
		}
		FString MemberName = Value.DefaultValue.TrimStartAndEnd();
		int32 ScopeIndex = INDEX_NONE;
		if (MemberName.FindLastChar(TEXT(':'), ScopeIndex))
		{
			MemberName.RightChopInline(ScopeIndex + 1);
		}
		for (const FAvidScriptBindingEnumValue& EnumValue : EnumType->EnumValues)
		{
			if (EnumValue.Name == MemberName)
			{
				OutExpression = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Value.CppType)
					+ TEXT(".")
					+ FAvidScriptEditorCSharpSyntax::MakeIdentifier(EnumValue.Name);
				return true;
			}
		}
		int64 Parsed = 0;
		if (TryParseSignedDecimal(Value.DefaultValue, MIN_int32, MAX_int32, Parsed))
		{
			OutExpression = FString::Printf(
				TEXT("(%s)%lld"),
				*FAvidScriptEditorCSharpSyntax::MakeIdentifier(Value.CppType),
				Parsed);
			return true;
		}
		return false;
	}
	if (Value.Kind == TEXT("scalar"))
	{
		if (Value.CanonicalType == TEXT("scalar:f32"))
		{
			float Parsed = 0.0f;
			if (!IsStrictDecimalFloat(Value.DefaultValue)
				|| !LexTryParseString(Parsed, *Value.DefaultValue)
				|| !FMath::IsFinite(Parsed))
			{
				return false;
			}
			OutExpression = FString::Printf(TEXT("%.9gf"), static_cast<double>(Parsed));
			return true;
		}
		if (Value.CanonicalType == TEXT("scalar:f64"))
		{
			double Parsed = 0.0;
			if (!IsStrictDecimalFloat(Value.DefaultValue)
				|| !LexTryParseString(Parsed, *Value.DefaultValue)
				|| !FMath::IsFinite(Parsed))
			{
				return false;
			}
			OutExpression = FString::Printf(TEXT("%.17gd"), Parsed);
			return true;
		}
		if (Value.CanonicalType == TEXT("scalar:u8")
			|| Value.CanonicalType == TEXT("scalar:u16")
			|| Value.CanonicalType == TEXT("scalar:u32")
			|| Value.CanonicalType == TEXT("scalar:u64"))
		{
			const uint64 MaxValue = Value.CanonicalType == TEXT("scalar:u8") ? MAX_uint8
				: Value.CanonicalType == TEXT("scalar:u16") ? MAX_uint16
				: Value.CanonicalType == TEXT("scalar:u32") ? MAX_uint32
				: MAX_uint64;
			uint64 Parsed = 0;
			if (!TryParseUnsignedDecimal(Value.DefaultValue, MaxValue, Parsed))
			{
				return false;
			}
			OutExpression = FString::Printf(TEXT("%llu"), Parsed);
			if (Value.CanonicalType == TEXT("scalar:u32")) { OutExpression += TEXT("u"); }
			if (Value.CanonicalType == TEXT("scalar:u64")) { OutExpression += TEXT("UL"); }
			return true;
		}

		const int64 MinValue = Value.CanonicalType == TEXT("scalar:i8") ? MIN_int8
			: Value.CanonicalType == TEXT("scalar:i16") ? MIN_int16
			: Value.CanonicalType == TEXT("scalar:i32") ? MIN_int32
			: MIN_int64;
		const int64 MaxValue = Value.CanonicalType == TEXT("scalar:i8") ? MAX_int8
			: Value.CanonicalType == TEXT("scalar:i16") ? MAX_int16
			: Value.CanonicalType == TEXT("scalar:i32") ? MAX_int32
			: MAX_int64;
		int64 Parsed = 0;
		if (!TryParseSignedDecimal(Value.DefaultValue, MinValue, MaxValue, Parsed))
		{
			return false;
		}
		if (Value.CanonicalType == TEXT("scalar:i64"))
		{
			OutExpression = Parsed == MIN_int64 ? TEXT("long.MinValue") : FString::Printf(TEXT("%lldL"), Parsed);
			return true;
		}
		if (Value.CanonicalType == TEXT("scalar:i8")
			|| Value.CanonicalType == TEXT("scalar:i16")
			|| Value.CanonicalType == TEXT("scalar:i32"))
		{
			OutExpression = FString::Printf(TEXT("%lld"), Parsed);
			return true;
		}
	}
	return false;
}
