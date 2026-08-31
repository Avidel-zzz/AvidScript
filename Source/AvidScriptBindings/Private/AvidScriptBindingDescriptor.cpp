#include "AvidScriptBindingDescriptor.h"

#include "AvidScriptHash.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"

namespace
{
bool IsAvidScriptBindingLowerSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool ReadAvidScriptBindingRequiredString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	FString& OutValue,
	FString& OutErrorSource)
{
	const TSharedPtr<FJsonValue> Value = Object.IsValid()
		? Object->TryGetField(Field)
		: nullptr;
	if (!Value.IsValid()
		|| Value->Type != EJson::String
		|| !Value->TryGetString(OutValue)
		|| OutValue.IsEmpty())
	{
		OutErrorSource = Field;
		return false;
	}
	return true;
}

bool ReadAvidScriptBindingRequiredStringAllowEmpty(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	FString& OutValue,
	FString& OutErrorSource)
{
	const TSharedPtr<FJsonValue> Value = Object.IsValid()
		? Object->TryGetField(Field)
		: nullptr;
	if (!Value.IsValid()
		|| Value->Type != EJson::String
		|| !Value->TryGetString(OutValue))
	{
		OutErrorSource = Field;
		return false;
	}
	return true;
}

bool ReadAvidScriptBindingRequiredBool(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	bool& OutValue,
	FString& OutErrorSource)
{
	if (!Object.IsValid() || !Object->TryGetBoolField(Field, OutValue))
	{
		OutErrorSource = Field;
		return false;
	}
	return true;
}

bool ReadAvidScriptBindingRequiredInt(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	int32& OutValue,
	FString& OutErrorSource)
{
	double Number = 0.0;
	if (!Object.IsValid() || !Object->TryGetNumberField(Field, Number)
		|| !FMath::IsFinite(Number)
		|| Number < static_cast<double>(MIN_int32)
		|| Number > static_cast<double>(MAX_int32)
		|| FMath::FloorToDouble(Number) != Number)
	{
		OutErrorSource = Field;
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return true;
}

bool ReadAvidScriptBindingStringArray(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	TArray<FString>& OutValues,
	FString& OutErrorSource,
	bool bAllowEmpty)
{
	OutValues.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || Values == nullptr)
	{
		OutErrorSource = Field;
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Text;
		if (!Value.IsValid() || !Value->TryGetString(Text) || Text.IsEmpty())
		{
			OutErrorSource = Field;
			return false;
		}
		OutValues.Add(MoveTemp(Text));
	}
	if (!bAllowEmpty && OutValues.IsEmpty())
	{
		OutErrorSource = Field;
		return false;
	}
	return true;
}

bool ReadAvidScriptBindingEnumValues(
	const TSharedPtr<FJsonObject>& Object,
	const FString& Kind,
	TArray<FAvidScriptBindingEnumValue>& OutValues,
	FString& OutErrorSource)
{
	OutValues.Reset();
	if (Kind != TEXT("enum"))
	{
		if (Object.IsValid() && Object->HasField(TEXT("enum_values")))
		{
			OutErrorSource = TEXT("enum_values");
			return false;
		}
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid()
		|| !Object->TryGetArrayField(TEXT("enum_values"), Values)
		|| Values == nullptr
		|| Values->IsEmpty())
	{
		OutErrorSource = TEXT("enum_values");
		return false;
	}

	TSet<FString> Names;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject> EnumValueObject = Value.IsValid() ? Value->AsObject() : nullptr;
		FAvidScriptBindingEnumValue EnumValue;
		double Number = 0.0;
		if (!ReadAvidScriptBindingRequiredString(EnumValueObject, TEXT("name"), EnumValue.Name, OutErrorSource)
			|| !EnumValueObject->TryGetNumberField(TEXT("value"), Number)
			|| !FMath::IsFinite(Number)
			|| FMath::FloorToDouble(Number) != Number
			|| Number < static_cast<double>(MIN_int32)
			|| Number > static_cast<double>(MAX_int32)
			|| Names.Contains(EnumValue.Name))
		{
			OutErrorSource = TEXT("enum_values");
			return false;
		}
		EnumValue.Value = static_cast<int64>(Number);
		Names.Add(EnumValue.Name);
		OutValues.Add(MoveTemp(EnumValue));
	}
	return true;
}

bool ReadAvidScriptBindingStructFields(
	const TSharedPtr<FJsonObject>& Object,
	const int32 SchemaVersion,
	const FString& Kind,
	TArray<FAvidScriptBindingStructFieldModel>& OutFields,
	FString& OutErrorSource)
{
	OutFields.Reset();
	if (SchemaVersion < 9)
	{
		if (Object.IsValid() && Object->HasField(TEXT("fields")))
		{
			OutErrorSource = TEXT("fields");
			return false;
		}
		return true;
	}
	if (Kind != TEXT("struct_wire"))
	{
		if (Object.IsValid() && Object->HasField(TEXT("fields")))
		{
			OutErrorSource = TEXT("fields");
			return false;
		}
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid()
		|| !Object->TryGetArrayField(TEXT("fields"), Values)
		|| Values == nullptr
		|| Values->IsEmpty())
	{
		OutErrorSource = TEXT("fields");
		return false;
	}

	TSet<FString> Names;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject> FieldObject = Value.IsValid() ? Value->AsObject() : nullptr;
		FAvidScriptBindingStructFieldModel Field;
		if (!ReadAvidScriptBindingRequiredString(FieldObject, TEXT("name"), Field.Name, OutErrorSource)
			|| !ReadAvidScriptBindingRequiredString(FieldObject, TEXT("type_id"), Field.TypeId, OutErrorSource)
			|| !ReadAvidScriptBindingRequiredInt(FieldObject, TEXT("wire_offset"), Field.WireOffset, OutErrorSource)
			|| !IsAvidScriptBindingLowerSha256(Field.TypeId)
			|| Field.WireOffset < 0
			|| Names.Contains(Field.Name))
		{
			OutErrorSource = TEXT("fields");
			return false;
		}
		Names.Add(Field.Name);
		OutFields.Add(MoveTemp(Field));
	}
	return true;
}

bool ReadAvidScriptBindingElementTypeId(
	const TSharedPtr<FJsonObject>& Object,
	const int32 SchemaVersion,
	const FString& Kind,
	FString& OutElementTypeId,
	FString& OutErrorSource)
{
	OutElementTypeId.Reset();
	if (SchemaVersion < 10)
	{
		if (Object.IsValid() && Object->HasField(TEXT("element_type_id")))
		{
			OutErrorSource = TEXT("element_type_id");
			return false;
		}
		return true;
	}
	if (Kind != TEXT("array"))
	{
		if (Object.IsValid() && Object->HasField(TEXT("element_type_id")))
		{
			OutErrorSource = TEXT("element_type_id");
			return false;
		}
		return true;
	}
	if (!ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("element_type_id"),
			OutElementTypeId,
			OutErrorSource)
		|| !IsAvidScriptBindingLowerSha256(OutElementTypeId))
	{
		OutErrorSource = TEXT("element_type_id");
		return false;
	}
	return true;
}

bool ReadAvidScriptBindingCompositeMetadata(
	const TSharedPtr<FJsonObject>& Object,
	const int32 SchemaVersion,
	const FString& Kind,
	const FString& ElementTypeId,
	TArray<FString>& OutTypeArguments,
	FString& OutCapabilityKind,
	FString& OutErrorSource)
{
	OutTypeArguments.Reset();
	OutCapabilityKind.Reset();
	if (SchemaVersion < 19)
	{
		if (Object.IsValid()
			&& (Object->HasField(TEXT("type_arguments"))
				|| Object->HasField(TEXT("capability_kind"))))
		{
			OutErrorSource = TEXT("composite_metadata");
			return false;
		}
		return true;
	}
	if (!ReadAvidScriptBindingStringArray(
			Object,
			TEXT("type_arguments"),
			OutTypeArguments,
			OutErrorSource,
			true)
		|| !ReadAvidScriptBindingRequiredStringAllowEmpty(
			Object,
			TEXT("capability_kind"),
			OutCapabilityKind,
			OutErrorSource))
	{
		return false;
	}
	for (const FString& TypeArgument : OutTypeArguments)
	{
		if (!IsAvidScriptBindingLowerSha256(TypeArgument))
		{
			OutErrorSource = TEXT("type_arguments");
			return false;
		}
	}
	if ((Kind == TEXT("array")
			&& (OutTypeArguments.Num() != 1
				|| OutTypeArguments[0] != ElementTypeId
				|| (OutCapabilityKind != TEXT("array_flat")
					&& OutCapabilityKind != TEXT("composite"))))
		|| (Kind == TEXT("set")
			&& (OutTypeArguments.Num() != 1
				|| OutCapabilityKind != TEXT("composite")))
		|| (Kind == TEXT("map")
			&& (OutTypeArguments.Num() != 2
				|| OutCapabilityKind != TEXT("composite")))
		|| (Kind == TEXT("text_capability")
			&& (!OutTypeArguments.IsEmpty()
				|| OutCapabilityKind != TEXT("composite")))
		|| ((Kind == TEXT("soft_object_capability")
				|| Kind == TEXT("weak_object_capability"))
			&& (OutTypeArguments.Num() != 1
				|| OutCapabilityKind != TEXT("composite")))
		|| (Kind == TEXT("name_utf8") || Kind == TEXT("string_utf8"))
			&& OutCapabilityKind != TEXT("utf8")
		|| (Kind == TEXT("object_handle") && OutCapabilityKind != TEXT("object"))
		|| (Kind != TEXT("array")
			&& Kind != TEXT("set")
			&& Kind != TEXT("map")
			&& Kind != TEXT("text_capability")
			&& Kind != TEXT("soft_object_capability")
			&& Kind != TEXT("weak_object_capability")
			&& Kind != TEXT("name_utf8")
			&& Kind != TEXT("string_utf8")
			&& Kind != TEXT("object_handle")
			&& (!OutTypeArguments.IsEmpty() || !OutCapabilityKind.IsEmpty())))
	{
		OutErrorSource = Kind + TEXT(":composite_metadata");
		return false;
	}
	return true;
}

bool ParseAvidScriptBindingType(
	const TSharedPtr<FJsonObject>& Object,
	const int32 SchemaVersion,
	FAvidScriptBindingTypeModel& OutType,
	FString& OutErrorSource)
{
	if (!ReadAvidScriptBindingRequiredString(Object, TEXT("stable_id"), OutType.StableId, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("canonical_type"), OutType.CanonicalType, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("kind"), OutType.Kind, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("cpp_type"), OutType.CppType, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredInt(Object, TEXT("size"), OutType.Size, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredInt(Object, TEXT("alignment"), OutType.Alignment, OutErrorSource)
		|| !ReadAvidScriptBindingStringArray(Object, TEXT("abi_types"), OutType.AbiTypes, OutErrorSource, false)
		|| !ReadAvidScriptBindingEnumValues(Object, OutType.Kind, OutType.EnumValues, OutErrorSource)
		|| !ReadAvidScriptBindingStructFields(
			Object,
			SchemaVersion,
			OutType.Kind,
			OutType.StructFields,
			OutErrorSource)
		|| !ReadAvidScriptBindingElementTypeId(
			Object,
			SchemaVersion,
			OutType.Kind,
			OutType.ElementTypeId,
			OutErrorSource)
		|| !ReadAvidScriptBindingCompositeMetadata(
			Object,
			SchemaVersion,
			OutType.Kind,
			OutType.ElementTypeId,
			OutType.TypeArguments,
			OutType.CapabilityKind,
			OutErrorSource)
		|| !IsAvidScriptBindingLowerSha256(OutType.StableId)
		|| OutType.StableId != FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
			OutType.CanonicalType,
			OutType.EnumValues,
			OutType.StructFields,
			SchemaVersion >= 9 && OutType.Kind == TEXT("struct_wire")
				? OutType.Size
				: INDEX_NONE,
			SchemaVersion >= 9 && OutType.Kind == TEXT("struct_wire")
				? OutType.Alignment
				: INDEX_NONE,
			SchemaVersion >= 10 && OutType.Kind == TEXT("array")
				? OutType.ElementTypeId
				: FString(),
			SchemaVersion >= 19 ? OutType.TypeArguments : TArray<FString>())
		|| (OutType.Kind == TEXT("enum")) != OutType.CanonicalType.StartsWith(TEXT("enum:"))
		|| (OutType.Kind == TEXT("struct_wire")) != OutType.CanonicalType.StartsWith(TEXT("struct_wire:"))
		|| (OutType.Kind == TEXT("array")) != OutType.CanonicalType.StartsWith(TEXT("array:tarray<"))
		|| (OutType.Kind == TEXT("text_capability")) != (OutType.CanonicalType == TEXT("text:ftext"))
		|| (OutType.Kind == TEXT("soft_object_capability"))
			!= OutType.CanonicalType.StartsWith(TEXT("soft_object:"))
		|| (OutType.Kind == TEXT("weak_object_capability"))
			!= OutType.CanonicalType.StartsWith(TEXT("weak_object:"))
		|| OutType.Size <= 0
		|| OutType.Alignment <= 0)
	{
		return false;
	}
	if (SchemaVersion >= 6)
	{
		if (!ReadAvidScriptBindingRequiredInt(
				Object,
				TEXT("object_type_ordinal"),
				OutType.ObjectTypeOrdinal,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredStringAllowEmpty(
				Object,
				TEXT("class_path"),
				OutType.ClassPath,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredStringAllowEmpty(
				Object,
				TEXT("base_type_id"),
				OutType.BaseTypeId,
				OutErrorSource)
			|| OutType.ObjectTypeOrdinal < INDEX_NONE
			|| (OutType.Kind != TEXT("object_handle")
				&& (OutType.ObjectTypeOrdinal != INDEX_NONE
					|| !OutType.ClassPath.IsEmpty()
					|| !OutType.BaseTypeId.IsEmpty()))
			|| (OutType.Kind == TEXT("object_handle")
				&& OutType.ObjectTypeOrdinal == INDEX_NONE
				&& (!OutType.ClassPath.IsEmpty() || !OutType.BaseTypeId.IsEmpty()))
			|| (OutType.Kind == TEXT("object_handle")
				&& OutType.ObjectTypeOrdinal != INDEX_NONE
				&& (OutType.ClassPath.IsEmpty()
					|| OutType.CanonicalType != TEXT("object:") + OutType.ClassPath)))
		{
			return false;
		}
	}
	return true;
}

bool IsAvidScriptBindingPowerOfTwo(const int32 Value)
{
	return Value > 0 && (Value & (Value - 1)) == 0;
}

bool MatchesAvidScriptBindingStructWireLeaf(
	const FAvidScriptBindingTypeModel& Type,
	const TCHAR* CanonicalType,
	const TCHAR* Kind,
	const TCHAR* CppType,
	const int32 Size,
	const int32 Alignment,
	const TArray<FString>& AbiTypes)
{
	return Type.CanonicalType == CanonicalType
		&& Type.Kind == Kind
		&& Type.CppType == CppType
		&& Type.Size == Size
		&& Type.Alignment == Alignment
		&& Type.AbiTypes == AbiTypes
		&& Type.StructFields.IsEmpty();
}

bool IsCanonicalAvidScriptBindingStructWireLeaf(
	const FAvidScriptBindingTypeModel& Type)
{
	if (!IsAvidScriptBindingPowerOfTwo(Type.Alignment))
	{
		return false;
	}
	if (MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:bool"), TEXT("scalar"), TEXT("bool"), 4, 4, { TEXT("i") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:i8"), TEXT("scalar"), TEXT("int8"), 1, 1, { TEXT("i") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:u8"), TEXT("scalar"), TEXT("uint8"), 1, 1, { TEXT("i") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:i16"), TEXT("scalar"), TEXT("int16"), 2, 2, { TEXT("i") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:u16"), TEXT("scalar"), TEXT("uint16"), 2, 2, { TEXT("i") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:i32"), TEXT("scalar"), TEXT("int32"), 4, 4, { TEXT("i") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:u32"), TEXT("scalar"), TEXT("uint32"), 4, 4, { TEXT("i") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:i64"), TEXT("scalar"), TEXT("int64"), 8, 8, { TEXT("I") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:u64"), TEXT("scalar"), TEXT("uint64"), 8, 8, { TEXT("I") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:f32"), TEXT("scalar"), TEXT("float"), 4, 4, { TEXT("f") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type, TEXT("scalar:f64"), TEXT("scalar"), TEXT("double"), 8, 8, { TEXT("F") }))
	{
		return true;
	}
	if (Type.Kind == TEXT("enum"))
	{
		return Type.CanonicalType.StartsWith(TEXT("enum:/Script/"))
			&& !Type.CppType.IsEmpty()
			&& Type.Size == 4
			&& Type.Alignment == 4
			&& Type.AbiTypes == TArray<FString>{ TEXT("i") }
			&& Type.StructFields.IsEmpty();
	}
	if (Type.Kind == TEXT("object_handle"))
	{
		return Type.CanonicalType.StartsWith(TEXT("object:/Script/"))
			&& !Type.CppType.IsEmpty()
			&& Type.Size == 8
			&& Type.Alignment == 4
			&& Type.AbiTypes == TArray<FString>{ TEXT("i"), TEXT("i") }
			&& Type.StructFields.IsEmpty();
	}
	return MatchesAvidScriptBindingStructWireLeaf(
			Type,
			TEXT("struct:/Script/CoreUObject.Vector"),
			TEXT("struct"),
			TEXT("FVector"),
			12,
			4,
			{ TEXT("f"), TEXT("f"), TEXT("f") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type,
			TEXT("struct:/Script/CoreUObject.Rotator"),
			TEXT("struct"),
			TEXT("FRotator"),
			12,
			4,
			{ TEXT("f"), TEXT("f"), TEXT("f") })
		|| MatchesAvidScriptBindingStructWireLeaf(
			Type,
			TEXT("struct:/Script/CoreUObject.Transform"),
			TEXT("struct"),
			TEXT("FTransform"),
			36,
			4,
			{
				TEXT("f"), TEXT("f"), TEXT("f"),
				TEXT("f"), TEXT("f"), TEXT("f"),
				TEXT("f"), TEXT("f"), TEXT("f")
			});
}

bool ValidateAvidScriptBindingStructWireGraph(
	const TArray<FAvidScriptBindingTypeModel>& Types,
	FString& OutErrorSource)
{
	if (!Types.ContainsByPredicate([](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.Kind == TEXT("struct_wire");
		}))
	{
		return true;
	}

	TMap<FString, const FAvidScriptBindingTypeModel*> TypesById;
	for (const FAvidScriptBindingTypeModel& Type : Types)
	{
		TypesById.Add(Type.StableId, &Type);
	}
	TSet<FString> ActiveTypes;
	TFunction<bool(const FAvidScriptBindingTypeModel&, int32, int32&)> ValidateType;
	ValidateType = [&TypesById, &ActiveTypes, &ValidateType, &OutErrorSource](
		const FAvidScriptBindingTypeModel& Type,
		const int32 Depth,
		int32& InOutNodes)
	{
		if (Type.Kind != TEXT("struct_wire"))
		{
			if (!IsCanonicalAvidScriptBindingStructWireLeaf(Type))
			{
				OutErrorSource = TEXT("types.fields");
				return false;
			}
			return true;
		}
		if (Depth > 8
			|| Type.Size > 4096
			|| !IsAvidScriptBindingPowerOfTwo(Type.Alignment)
			|| Type.Alignment > 4096
			|| Type.Size % Type.Alignment != 0
			|| Type.AbiTypes.Num() != 1
			|| Type.AbiTypes[0] != TEXT("i")
			|| ActiveTypes.Contains(Type.StableId))
		{
			OutErrorSource = TEXT("types.fields");
			return false;
		}

		ActiveTypes.Add(Type.StableId);
		int32 PreviousEnd = 0;
		int32 ExpectedAlignment = 1;
		for (const FAvidScriptBindingStructFieldModel& Field : Type.StructFields)
		{
			++InOutNodes;
			const FAvidScriptBindingTypeModel* const Child = TypesById.FindRef(Field.TypeId);
			const int32 ExpectedWireOffset = Child == nullptr || Child->Alignment <= 0
				? INDEX_NONE
				: PreviousEnd + ((Child->Alignment - (PreviousEnd % Child->Alignment)) % Child->Alignment);
			if (InOutNodes > 128
				|| Child == nullptr
				|| !IsAvidScriptBindingPowerOfTwo(Child->Alignment)
				|| Child->Alignment > 4096
				|| Child->Size <= 0
				|| Child->Size > 4096
				|| Field.WireOffset != ExpectedWireOffset
				|| Field.WireOffset > 4096
				|| Child->Size > 4096 - Field.WireOffset
				|| !ValidateType(*Child, Depth + 1, InOutNodes))
			{
				ActiveTypes.Remove(Type.StableId);
				OutErrorSource = TEXT("types.fields");
				return false;
			}
			PreviousEnd = Field.WireOffset + Child->Size;
			ExpectedAlignment = FMath::Max(ExpectedAlignment, Child->Alignment);
		}
		const int32 Remainder = PreviousEnd % ExpectedAlignment;
		const int32 Padding = Remainder == 0 ? 0 : ExpectedAlignment - Remainder;
		if (Padding > 4096 - PreviousEnd
			|| Type.Alignment != ExpectedAlignment
			|| Type.Size != PreviousEnd + Padding)
		{
			ActiveTypes.Remove(Type.StableId);
			OutErrorSource = TEXT("types.fields");
			return false;
		}
		ActiveTypes.Remove(Type.StableId);
		return true;
	};

	for (const FAvidScriptBindingTypeModel& Type : Types)
	{
		if (Type.Kind == TEXT("struct_wire"))
		{
			int32 Nodes = 0;
			if (!ValidateType(Type, 1, Nodes))
			{
				return false;
			}
		}
	}
	return true;
}

bool ParseAvidScriptBindingValue(
	const TSharedPtr<FJsonObject>& Object,
	FAvidScriptBindingValueModel& OutValue,
	FString& OutErrorSource)
{
	if (!ReadAvidScriptBindingRequiredString(Object, TEXT("name"), OutValue.Name, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("direction"), OutValue.Direction, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredBool(Object, TEXT("has_default"), OutValue.bHasDefault, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("canonical_type"), OutValue.CanonicalType, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("type_id"), OutValue.TypeId, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("kind"), OutValue.Kind, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("cpp_type"), OutValue.CppType, OutErrorSource)
		|| !ReadAvidScriptBindingStringArray(
			Object,
			TEXT("abi_types"),
			OutValue.AbiTypes,
			OutErrorSource,
			OutValue.CanonicalType == TEXT("void")))
	{
		return false;
	}
	if (OutValue.bHasDefault
		&& (!Object->TryGetStringField(TEXT("default_value"), OutValue.DefaultValue) || OutValue.DefaultValue.IsEmpty()))
	{
		OutErrorSource = TEXT("default_value");
		return false;
	}
	if (!IsAvidScriptBindingLowerSha256(OutValue.TypeId)
		|| (OutValue.Direction != TEXT("value")
			&& OutValue.Direction != TEXT("const_ref")
			&& OutValue.Direction != TEXT("ref")
			&& OutValue.Direction != TEXT("out")
			&& OutValue.Direction != TEXT("return")))
	{
		OutErrorSource = OutValue.Name;
		return false;
	}
	return true;
}

bool IsAvidScriptBindingIdentifier(const FString& Value);

bool ParseAvidScriptBindingAsyncAction(
	const TSharedPtr<FJsonObject>& Object,
	FAvidScriptBindingAsyncActionModel& OutAction,
	FString& OutErrorSource)
{
	if (!ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("mode"),
			OutAction.Mode,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("action_class"),
			OutAction.ActionClass,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("activation_function"),
			OutAction.ActivationFunction,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("payload_type_id"),
			OutAction.PayloadTypeId,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("completion_policy"),
			OutAction.CompletionPolicy,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredBool(
			Object,
			TEXT("cancellable"),
			OutAction.bCancellable,
			OutErrorSource)
		|| OutAction.Mode != TEXT("blueprint_async_action")
		|| OutAction.ActionClass.IsEmpty()
		|| OutAction.ActivationFunction != TEXT("Activate")
		|| !IsAvidScriptBindingLowerSha256(OutAction.PayloadTypeId)
		|| OutAction.CompletionPolicy != TEXT("first_broadcast_wins")
		|| !OutAction.bCancellable)
	{
		OutErrorSource = TEXT("async_action");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Outcomes = nullptr;
	if (!Object->TryGetArrayField(TEXT("outcomes"), Outcomes)
		|| Outcomes == nullptr
		|| Outcomes->IsEmpty())
	{
		OutErrorSource = TEXT("async_action.outcomes");
		return false;
	}
	TSet<FString> StableIds;
	TSet<FString> DelegateMembers;
	for (int32 Index = 0; Index < Outcomes->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> OutcomeObject =
			(*Outcomes)[Index].IsValid()
				? (*Outcomes)[Index]->AsObject()
				: nullptr;
		FAvidScriptBindingAsyncActionOutcomeModel Outcome;
		if (!ReadAvidScriptBindingRequiredString(
				OutcomeObject,
				TEXT("stable_id"),
				Outcome.StableId,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredInt(
				OutcomeObject,
				TEXT("ordinal"),
				Outcome.Ordinal,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredString(
				OutcomeObject,
				TEXT("delegate_member"),
				Outcome.DelegateMember,
				OutErrorSource)
			|| !IsAvidScriptBindingLowerSha256(Outcome.StableId)
			|| Outcome.Ordinal != Index
			|| !IsAvidScriptBindingIdentifier(Outcome.DelegateMember)
			|| StableIds.Contains(Outcome.StableId)
			|| DelegateMembers.Contains(Outcome.DelegateMember))
		{
			OutErrorSource = FString::Printf(
				TEXT("async_action.outcomes[%d]"),
				Index);
			return false;
		}
		StableIds.Add(Outcome.StableId);
		DelegateMembers.Add(Outcome.DelegateMember);
		OutAction.Outcomes.Add(MoveTemp(Outcome));
	}
	return true;
}

bool ParseAvidScriptBindingFunction(
	const TSharedPtr<FJsonObject>& Object,
	const int32 SchemaVersion,
	FAvidScriptBindingFunctionModel& OutBinding,
	FString& OutErrorSource)
{
	if (!ReadAvidScriptBindingRequiredString(Object, TEXT("stable_id"), OutBinding.StableId, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("canonical_identity"), OutBinding.CanonicalIdentity, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredInt(Object, TEXT("ordinal"), OutBinding.Ordinal, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("owner_class"), OutBinding.OwnerClass, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("script_name"), OutBinding.ScriptName, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("dispatch_mode"), OutBinding.DispatchMode, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredBool(Object, TEXT("is_static"), OutBinding.bStatic, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredBool(Object, TEXT("is_const"), OutBinding.bConst, OutErrorSource)
		|| !IsAvidScriptBindingLowerSha256(OutBinding.StableId))
	{
		return false;
	}
	if (SchemaVersion <= 3)
	{
		if (!ReadAvidScriptBindingRequiredString(Object, TEXT("ue_function"), OutBinding.UeFunction, OutErrorSource))
		{
			return false;
		}
		OutBinding.BindingKind = TEXT("function");
		OutBinding.UeMember = OutBinding.UeFunction;
	}
	else
	{
		if (!ReadAvidScriptBindingRequiredString(Object, TEXT("binding_kind"), OutBinding.BindingKind, OutErrorSource)
			|| !ReadAvidScriptBindingRequiredString(Object, TEXT("ue_member"), OutBinding.UeMember, OutErrorSource)
			|| (OutBinding.BindingKind != TEXT("function")
				&& OutBinding.BindingKind != TEXT("property_get")
				&& (SchemaVersion < 8 || OutBinding.BindingKind != TEXT("property_set"))))
		{
			OutErrorSource = TEXT("binding_kind");
			return false;
		}
		if (OutBinding.BindingKind == TEXT("function"))
		{
			OutBinding.UeFunction = OutBinding.UeMember;
		}
		else if (SchemaVersion >= 8
			&& OutBinding.BindingKind == TEXT("property_set")
			&& !ReadAvidScriptBindingRequiredStringAllowEmpty(
				Object,
				TEXT("ue_function"),
				OutBinding.UeFunction,
				OutErrorSource))
		{
			return false;
		}
	}
	if (SchemaVersion >= 21)
	{
		if (!ReadAvidScriptBindingRequiredString(
				Object,
				TEXT("reflected_owner_kind"),
				OutBinding.ReflectedOwnerKind,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredStringAllowEmpty(
				Object,
				TEXT("reflected_owner_asset"),
				OutBinding.ReflectedOwnerAsset,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredStringAllowEmpty(
				Object,
				TEXT("reflected_function_fingerprint"),
				OutBinding.ReflectedFunctionFingerprint,
				OutErrorSource))
		{
			return false;
		}
		const bool bNativeProvenance =
			OutBinding.ReflectedOwnerKind == TEXT("native")
			&& OutBinding.ReflectedOwnerAsset.IsEmpty()
			&& OutBinding.ReflectedFunctionFingerprint.IsEmpty();
		const bool bBlueprintProvenance =
			OutBinding.BindingKind == TEXT("function")
			&& OutBinding.ReflectedOwnerKind == TEXT("blueprint")
			&& !OutBinding.ReflectedOwnerAsset.IsEmpty()
			&& IsAvidScriptBindingLowerSha256(
				OutBinding.ReflectedFunctionFingerprint);
		if (!bNativeProvenance && !bBlueprintProvenance)
		{
			OutErrorSource = TEXT("reflection_provenance");
			return false;
		}
	}
	if (SchemaVersion >= 8)
	{
		if (!ReadAvidScriptBindingRequiredString(
				Object,
				TEXT("write_policy"),
				OutBinding.WritePolicy,
				OutErrorSource))
		{
			return false;
		}
	}
	else
	{
		OutBinding.WritePolicy = TEXT("none");
	}
	if (SchemaVersion >= 8
		&& OutBinding.DispatchMode == TEXT("generated_native_s1"))
	{
		if (!ReadAvidScriptBindingRequiredString(
				Object,
				TEXT("generated_shape"),
				OutBinding.GeneratedShape,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredString(
				Object,
				TEXT("generated_receiver_mode"),
				OutBinding.GeneratedReceiverMode,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredString(
				Object,
				TEXT("generated_import_name"),
				OutBinding.GeneratedImportName,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredInt(
				Object,
				TEXT("semantic_fallback_ordinal"),
				OutBinding.SemanticFallbackOrdinal,
				OutErrorSource))
		{
			return false;
		}
	}
	if (SchemaVersion >= 12
		&& OutBinding.DispatchMode == TEXT("latent_process_event"))
	{
		if (!ReadAvidScriptBindingRequiredString(
				Object,
				TEXT("latent_info_parameter"),
				OutBinding.LatentInfoParameter,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredStringAllowEmpty(
				Object,
				TEXT("world_context_parameter"),
				OutBinding.WorldContextParameter,
				OutErrorSource))
		{
			return false;
		}
	}
	if (SchemaVersion >= 13
		&& OutBinding.DispatchMode == TEXT("latent_process_event"))
	{
		const TSharedPtr<FJsonObject>* Completion = nullptr;
		if (!Object->TryGetObjectField(TEXT("completion"), Completion)
			|| Completion == nullptr
			|| !Completion->IsValid()
			|| !ReadAvidScriptBindingRequiredString(
				*Completion,
				TEXT("mode"),
				OutBinding.Completion.Mode,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredStringAllowEmpty(
				*Completion,
				TEXT("provider_id"),
				OutBinding.Completion.ProviderId,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredStringAllowEmpty(
				*Completion,
				TEXT("payload_type_id"),
				OutBinding.Completion.PayloadTypeId,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredString(
				*Completion,
				TEXT("status_policy"),
				OutBinding.Completion.StatusPolicy,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredBool(
				*Completion,
				TEXT("cancellable"),
				OutBinding.Completion.bCancellable,
				OutErrorSource))
		{
			OutErrorSource = TEXT("completion");
			return false;
		}
	}
	else if (Object->HasField(TEXT("completion")))
	{
		OutErrorSource = TEXT("completion");
		return false;
	}
	if (SchemaVersion >= 23
		&& OutBinding.DispatchMode == TEXT("blueprint_async_action"))
	{
		const TSharedPtr<FJsonObject>* AsyncAction = nullptr;
		if (!Object->TryGetObjectField(TEXT("async_action"), AsyncAction)
			|| AsyncAction == nullptr
			|| !AsyncAction->IsValid()
			|| !ParseAvidScriptBindingAsyncAction(
				*AsyncAction,
				OutBinding.AsyncAction,
				OutErrorSource))
		{
			OutErrorSource = TEXT("async_action");
			return false;
		}
	}
	else if (Object->HasField(TEXT("async_action")))
	{
		OutErrorSource = TEXT("async_action");
		return false;
	}
	if (SchemaVersion >= 15)
	{
		FString NetworkMode;
		if (!ReadAvidScriptBindingRequiredString(
				Object,
				TEXT("network_mode"),
				NetworkMode,
				OutErrorSource)
			|| !TryParseAvidScriptBindingNetworkMode(
				NetworkMode,
				OutBinding.Network.Mode)
			|| !ReadAvidScriptBindingRequiredBool(
				Object,
				TEXT("network_reliable"),
				OutBinding.Network.bReliable,
				OutErrorSource))
		{
			OutErrorSource = TEXT("network");
			return false;
		}
	}
	else if (Object->HasField(TEXT("network_mode"))
		|| Object->HasField(TEXT("network_reliable")))
	{
		OutErrorSource = TEXT("network");
		return false;
	}
	if (SchemaVersion >= 16)
	{
		FString ReplicationMode;
		FString RepNotifyFunction;
		if (!ReadAvidScriptBindingRequiredString(
				Object,
				TEXT("property_replication"),
				ReplicationMode,
				OutErrorSource)
			|| !TryParseAvidScriptBindingPropertyReplicationMode(
				ReplicationMode,
				OutBinding.PropertyReplication.Mode)
			|| !ReadAvidScriptBindingRequiredString(
				Object,
				TEXT("rep_notify"),
				RepNotifyFunction,
				OutErrorSource))
		{
			OutErrorSource = TEXT("property_replication");
			return false;
		}
		OutBinding.PropertyReplication.RepNotifyFunction =
			FName(*RepNotifyFunction);
	}
	else if (Object->HasField(TEXT("property_replication"))
		|| Object->HasField(TEXT("rep_notify")))
	{
		OutErrorSource = TEXT("property_replication");
		return false;
	}

	if (SchemaVersion == 2)
	{
		OutBinding.ReloadEffect = OutBinding.bConst
			? EAvidScriptBindingReloadEffect::None
			: EAvidScriptBindingReloadEffect::Unsupported;
	}
	else
	{
		FString ReloadEffect;
		if (!ReadAvidScriptBindingRequiredString(Object, TEXT("reload_effect"), ReloadEffect, OutErrorSource)
			|| !TryParseAvidScriptBindingReloadEffect(ReloadEffect, OutBinding.ReloadEffect)
			|| (OutBinding.bConst && OutBinding.ReloadEffect != EAvidScriptBindingReloadEffect::None))
		{
			OutErrorSource = TEXT("reload_effect");
			return false;
		}
	}

	const TSharedPtr<FJsonObject>* ReturnObject = nullptr;
	if (!Object->TryGetObjectField(TEXT("return"), ReturnObject)
		|| ReturnObject == nullptr
		|| !ReturnObject->IsValid()
		|| !ParseAvidScriptBindingValue(*ReturnObject, OutBinding.ReturnValue, OutErrorSource)
		|| OutBinding.ReturnValue.Direction != TEXT("return"))
	{
		OutErrorSource = TEXT("return");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
	if (!Object->TryGetArrayField(TEXT("parameters"), Parameters) || Parameters == nullptr)
	{
		OutErrorSource = TEXT("parameters");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Parameters)
	{
		const TSharedPtr<FJsonObject> ParameterObject = Value.IsValid() ? Value->AsObject() : nullptr;
		FAvidScriptBindingValueModel Parameter;
		if (!ParseAvidScriptBindingValue(ParameterObject, Parameter, OutErrorSource)
			|| Parameter.Direction == TEXT("return"))
		{
			OutErrorSource = TEXT("parameters");
			return false;
		}
		OutBinding.Parameters.Add(MoveTemp(Parameter));
	}

	const TSharedPtr<FJsonObject>* HostImport = nullptr;
	if (!Object->TryGetObjectField(TEXT("host_import"), HostImport)
		|| HostImport == nullptr
		|| !HostImport->IsValid()
		|| !ReadAvidScriptBindingRequiredString(*HostImport, TEXT("module"), OutBinding.HostImport.Module, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(*HostImport, TEXT("name"), OutBinding.HostImport.Name, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(*HostImport, TEXT("signature"), OutBinding.HostImport.Signature, OutErrorSource))
	{
		OutErrorSource = TEXT("host_import");
		return false;
	}
	return true;
}

bool IsAvidScriptBindingIdentifier(const FString& Value)
{
	if (Value.IsEmpty() || (!FChar::IsAlpha(Value[0]) && Value[0] != TEXT('_')))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

bool ParseAvidScriptBindingClassReference(
	const TSharedPtr<FJsonObject>& Object,
	const int32 SchemaVersion,
	FAvidScriptBindingClassReferenceModel& OutReference,
	FString& OutErrorSource)
{
	if (!ReadAvidScriptBindingRequiredString(Object, TEXT("stable_id"), OutReference.StableId, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredInt(Object, TEXT("ordinal"), OutReference.Ordinal, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("script_name"), OutReference.ScriptName, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("class_path"), OutReference.ClassPath, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("base_class_path"), OutReference.BaseClassPath, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Object, TEXT("load_policy"), OutReference.LoadPolicy, OutErrorSource)
		|| !IsAvidScriptBindingLowerSha256(OutReference.StableId)
		|| !IsAvidScriptBindingIdentifier(OutReference.ScriptName)
		|| (OutReference.LoadPolicy != TEXT("EditorLoad")
			&& OutReference.LoadPolicy != TEXT("CookRequired"))
		|| OutReference.StableId != FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
			OutReference.ClassPath,
			OutReference.BaseClassPath,
			OutReference.LoadPolicy))
	{
		return false;
	}
	if (SchemaVersion >= 6
		&& !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("result_type_id"),
			OutReference.ResultTypeId,
			OutErrorSource))
	{
		return false;
	}
	return true;
}

bool ParseAvidScriptBindingObjectFactory(
	const TSharedPtr<FJsonObject>& Object,
	FAvidScriptBindingObjectFactoryModel& OutFactory,
	FString& OutErrorSource)
{
	FString Kind;
	FString Ownership;
	FString Registration;
	if (!ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("stable_id"),
			OutFactory.StableId,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredInt(
			Object,
			TEXT("ordinal"),
			OutFactory.Ordinal,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("script_name"),
			OutFactory.ScriptName,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("class_reference_id"),
			OutFactory.ClassReferenceId,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("kind"),
			Kind,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("outer_type_id"),
			OutFactory.OuterTypeId,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("ownership"),
			Ownership,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("registration"),
			Registration,
			OutErrorSource)
		|| !IsAvidScriptBindingLowerSha256(OutFactory.StableId)
		|| !IsAvidScriptBindingLowerSha256(OutFactory.ClassReferenceId)
		|| !IsAvidScriptBindingLowerSha256(OutFactory.OuterTypeId)
		|| !IsAvidScriptBindingIdentifier(OutFactory.ScriptName)
		|| !TryParseAvidScriptObjectFactoryKind(Kind, OutFactory.Kind)
		|| !TryParseAvidScriptObjectOwnershipPolicy(
			Ownership,
			OutFactory.Ownership)
		|| !TryParseAvidScriptComponentRegistrationPolicy(
			Registration,
			OutFactory.Registration)
		|| OutFactory.StableId
			!= FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
				OutFactory.ClassReferenceId,
				OutFactory.Kind,
				OutFactory.OuterTypeId,
				OutFactory.Ownership,
				OutFactory.Registration))
	{
		return false;
	}
	return true;
}

bool ValidateAvidScriptBindingV6ObjectTypes(
	const FAvidScriptBindingPackageModel& Package,
	FString& OutErrorSource)
{
	if (Package.SchemaVersion < 6)
	{
		return true;
	}

	TMap<FString, const FAvidScriptBindingTypeModel*> TypesById;
	TMap<FString, const FAvidScriptBindingTypeModel*> GraphTypesById;
	TSet<int32> GraphOrdinals;
	int32 MaximumOrdinal = INDEX_NONE;
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		TypesById.Add(Type.StableId, &Type);
		if (Type.ObjectTypeOrdinal == INDEX_NONE)
		{
			continue;
		}
		if (Type.Kind != TEXT("object_handle")
			|| GraphOrdinals.Contains(Type.ObjectTypeOrdinal)
			|| GraphTypesById.Contains(Type.StableId))
		{
			OutErrorSource = TEXT("types.object_type_ordinal");
			return false;
		}
		GraphOrdinals.Add(Type.ObjectTypeOrdinal);
		GraphTypesById.Add(Type.StableId, &Type);
		MaximumOrdinal = FMath::Max(MaximumOrdinal, Type.ObjectTypeOrdinal);
	}
	if (MaximumOrdinal != GraphTypesById.Num() - 1)
	{
		OutErrorSource = TEXT("types.object_type_ordinal");
		return false;
	}

	TArray<const FAvidScriptBindingTypeModel*> GraphTypesByOrdinal;
	GraphTypesByOrdinal.SetNumZeroed(GraphTypesById.Num());
	for (const TPair<FString, const FAvidScriptBindingTypeModel*>& Pair : GraphTypesById)
	{
		const FAvidScriptBindingTypeModel& Type = *Pair.Value;
		if (!GraphTypesByOrdinal.IsValidIndex(Type.ObjectTypeOrdinal))
		{
			OutErrorSource = TEXT("types.object_type_ordinal");
			return false;
		}
		GraphTypesByOrdinal[Type.ObjectTypeOrdinal] = &Type;
	}

	FString PreviousClassPath;
	int32 RootCount = 0;
	for (const FAvidScriptBindingTypeModel* Type : GraphTypesByOrdinal)
	{
		if (Type == nullptr
			|| (!PreviousClassPath.IsEmpty()
				&& PreviousClassPath.Compare(Type->ClassPath, ESearchCase::CaseSensitive) >= 0))
		{
			OutErrorSource = TEXT("types.object_type_ordinal");
			return false;
		}
		PreviousClassPath = Type->ClassPath;
		if (Type->BaseTypeId.IsEmpty())
		{
			++RootCount;
			if (Type->ClassPath != TEXT("/Script/CoreUObject.Object"))
			{
				OutErrorSource = TEXT("types.base_type_id");
				return false;
			}
		}
		else if (!GraphTypesById.Contains(Type->BaseTypeId)
			|| Type->BaseTypeId == Type->StableId)
		{
			OutErrorSource = TEXT("types.base_type_id");
			return false;
		}
	}
	if (!GraphTypesByOrdinal.IsEmpty() && RootCount != 1)
	{
		OutErrorSource = TEXT("types.base_type_id");
		return false;
	}

	for (const TPair<FString, const FAvidScriptBindingTypeModel*>& Pair : GraphTypesById)
	{
		TSet<FString> Visited;
		const FAvidScriptBindingTypeModel* Current = Pair.Value;
		while (Current != nullptr && !Current->BaseTypeId.IsEmpty())
		{
			if (Visited.Contains(Current->StableId))
			{
				OutErrorSource = TEXT("types.base_type_id");
				return false;
			}
			Visited.Add(Current->StableId);
			Current = GraphTypesById.FindRef(Current->BaseTypeId);
		}
	}

	const auto IsGraphType = [&GraphTypesById](const FString& TypeId)
	{
		return GraphTypesById.Contains(TypeId);
	};
	const auto ValidateObjectValue = [&IsGraphType, &OutErrorSource](
		const FAvidScriptBindingValueModel& Value)
	{
		if (Value.Kind == TEXT("object_handle") && !IsGraphType(Value.TypeId))
		{
			OutErrorSource = Value.Name;
			return false;
		}
		return true;
	};

	TSet<FString> StaticOwnerTypeIds;
	bool bRequiresSelf = !Package.ClassReferences.IsEmpty();
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		const FString OwnerTypeId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
			TEXT("object:") + Binding.OwnerClass,
			{});
		const FAvidScriptBindingTypeModel* OwnerType = TypesById.FindRef(OwnerTypeId);
		if (OwnerType == nullptr || OwnerType->CanonicalType != TEXT("object:") + Binding.OwnerClass)
		{
			OutErrorSource = Binding.OwnerClass;
			return false;
		}
		if (Binding.bStatic)
		{
			StaticOwnerTypeIds.Add(OwnerTypeId);
		}
		else
		{
			bRequiresSelf = true;
			if (!IsGraphType(OwnerTypeId))
			{
				OutErrorSource = Binding.OwnerClass;
				return false;
			}
		}
		if (!ValidateObjectValue(Binding.ReturnValue)
			|| Binding.Parameters.ContainsByPredicate(
				[&ValidateObjectValue](const FAvidScriptBindingValueModel& Parameter)
				{
					return !ValidateObjectValue(Parameter);
				}))
		{
			return false;
		}
	}

	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		if (Type.Kind == TEXT("object_handle")
			&& Type.ObjectTypeOrdinal == INDEX_NONE
			&& !StaticOwnerTypeIds.Contains(Type.StableId))
		{
			OutErrorSource = Type.CanonicalType;
			return false;
		}
	}

	if ((bRequiresSelf && Package.SelfTypeId.IsEmpty())
		|| (!Package.SelfTypeId.IsEmpty() && !IsGraphType(Package.SelfTypeId)))
	{
		OutErrorSource = TEXT("self_type_id");
		return false;
	}
	for (const FAvidScriptBindingClassReferenceModel& Reference : Package.ClassReferences)
	{
		const FAvidScriptBindingTypeModel* ResultType =
			GraphTypesById.FindRef(Reference.ResultTypeId);
		if (ResultType == nullptr || ResultType->ClassPath != Reference.BaseClassPath)
		{
			OutErrorSource = TEXT("class_references.result_type_id");
			return false;
		}
	}
	return true;
}

bool ValidateAvidScriptBindingV7ObjectFactories(
	const FAvidScriptBindingPackageModel& Package,
	FString& OutErrorSource)
{
	if (Package.SchemaVersion < 7)
	{
		return true;
	}

	TSet<FString> ClassReferenceIds;
	TSet<FString> FactoryClassReferenceIds;
	for (const FAvidScriptBindingObjectFactoryModel& Factory :
		Package.ObjectFactories)
	{
		FactoryClassReferenceIds.Add(Factory.ClassReferenceId);
	}
	for (const FAvidScriptBindingClassReferenceModel& Reference :
		Package.ClassReferences)
	{
		ClassReferenceIds.Add(Reference.StableId);
		const bool bFactoryClassReference =
			FactoryClassReferenceIds.Contains(Reference.StableId);
		const bool bActorLifecycleReference =
			FAvidScriptBindingDescriptorTypeGraph::IsDerivedFromClassPath(
				Package,
				Reference.ResultTypeId,
				TEXT("/Script/Engine.Actor"));
		if (bFactoryClassReference == bActorLifecycleReference)
		{
			OutErrorSource = TEXT("class_references.capability");
			return false;
		}
	}

	TSet<FString> ObjectTypeIds;
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		if (Type.Kind == TEXT("object_handle")
			&& Type.ObjectTypeOrdinal != INDEX_NONE)
		{
			ObjectTypeIds.Add(Type.StableId);
		}
	}

	for (const FAvidScriptBindingObjectFactoryModel& Factory :
		Package.ObjectFactories)
	{
		if (!ClassReferenceIds.Contains(Factory.ClassReferenceId))
		{
			OutErrorSource = TEXT("object_factories.class_reference_id");
			return false;
		}
		if (!ObjectTypeIds.Contains(Factory.OuterTypeId))
		{
			OutErrorSource = TEXT("object_factories.outer_type_id");
			return false;
		}
		const bool bRegistrationMatches =
			(Factory.Kind == EAvidScriptObjectFactoryKind::NewObject
				&& Factory.Registration
					== EAvidScriptComponentRegistrationPolicy::None)
			|| (Factory.Kind == EAvidScriptObjectFactoryKind::ActorComponent
				&& Factory.Registration
					== EAvidScriptComponentRegistrationPolicy::RegisterInstance);
		if (!bRegistrationMatches)
		{
			OutErrorSource = TEXT("object_factories.registration");
			return false;
		}
	}
	return true;
}

void AppendAvidScriptBindingIdentityField(
	FString& Identity,
	const TCHAR* Label,
	const FString& Value)
{
	Identity += FString::Printf(
		TEXT("|%s:%d:%s"),
		Label,
		Value.Len(),
		*Value);
}

void AppendAvidScriptBindingValueIdentity(
	FString& Identity,
	const TCHAR* Prefix,
	const FAvidScriptBindingValueModel& Value)
{
	AppendAvidScriptBindingIdentityField(Identity, Prefix, Value.Name);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("direction"), Value.Direction);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("has_default"), Value.bHasDefault ? TEXT("1") : TEXT("0"));
	AppendAvidScriptBindingIdentityField(Identity, TEXT("default"), Value.DefaultValue);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("canonical_type"), Value.CanonicalType);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("type_id"), Value.TypeId);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("kind"), Value.Kind);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("cpp_type"), Value.CppType);
	for (const FString& AbiType : Value.AbiTypes)
	{
		AppendAvidScriptBindingIdentityField(Identity, TEXT("abi"), AbiType);
	}
}
} // namespace

bool FAvidScriptBindingDescriptorLayout::ValidateStructWireGraph(
	const TArray<FAvidScriptBindingTypeModel>& Types,
	FString& OutErrorSource)
{
	return ValidateAvidScriptBindingStructWireGraph(Types, OutErrorSource);
}

bool FAvidScriptBindingDescriptorLayout::ValidateTypeGraph(
	const TArray<FAvidScriptBindingTypeModel>& Types,
	FString& OutErrorSource)
{
	if (!ValidateAvidScriptBindingStructWireGraph(Types, OutErrorSource))
	{
		return false;
	}
	TMap<FString, const FAvidScriptBindingTypeModel*> TypesById;
	for (const FAvidScriptBindingTypeModel& Type : Types)
	{
		TypesById.Add(Type.StableId, &Type);
	}
	if (Types.Num() > 4096)
	{
		OutErrorSource = TEXT("type_graph_node_limit");
		return false;
	}
	TFunction<bool(const FAvidScriptBindingTypeModel&, int32, TSet<FString>&, TSet<FString>&)> ValidateArguments;
	ValidateArguments = [&TypesById, &OutErrorSource, &ValidateArguments](
		const FAvidScriptBindingTypeModel& Type,
		const int32 Depth,
		TSet<FString>& Active,
		TSet<FString>& Visited)
	{
		if (Depth > 8 || Active.Contains(Type.StableId))
		{
			OutErrorSource = Type.StableId + TEXT(":type_arguments_cycle");
			return false;
		}
		if (Visited.Contains(Type.StableId))
		{
			return true;
		}
		Active.Add(Type.StableId);
		for (const FString& TypeArgument : Type.TypeArguments)
		{
			const FAvidScriptBindingTypeModel* Argument = TypesById.FindRef(TypeArgument);
			if (Argument == nullptr
				|| !ValidateArguments(*Argument, Depth + 1, Active, Visited))
			{
				if (OutErrorSource.IsEmpty())
				{
					OutErrorSource = Type.StableId + TEXT(":type_arguments");
				}
				Active.Remove(Type.StableId);
				return false;
			}
		}
		Active.Remove(Type.StableId);
		Visited.Add(Type.StableId);
		return true;
	};
	TSet<FString> VisitedTypes;
	for (const FAvidScriptBindingTypeModel& Type : Types)
	{
		TSet<FString> ActiveTypes;
		if (!ValidateArguments(Type, 0, ActiveTypes, VisitedTypes))
		{
			return false;
		}
	}
	for (const FAvidScriptBindingTypeModel& Type : Types)
	{
		for (const FString& TypeArgument : Type.TypeArguments)
		{
			if (!TypesById.Contains(TypeArgument))
			{
				OutErrorSource = Type.StableId + TEXT(":type_arguments");
				return false;
			}
		}
		if (Type.Kind == TEXT("soft_object_capability")
			|| Type.Kind == TEXT("weak_object_capability"))
		{
			const FAvidScriptBindingTypeModel* ObjectType =
				Type.TypeArguments.Num() == 1
					? TypesById.FindRef(Type.TypeArguments[0])
					: nullptr;
			const bool bSoftObject = Type.Kind == TEXT("soft_object_capability");
			const FString CanonicalPrefix = bSoftObject
				? TEXT("soft_object:")
				: TEXT("weak_object:");
			const FString CppPrefix = bSoftObject
				? TEXT("TSoftObjectPtr<")
				: TEXT("TWeakObjectPtr<");
			if (ObjectType == nullptr
				|| ObjectType->Kind != TEXT("object_handle")
				|| ObjectType->ClassPath.IsEmpty()
				|| ObjectType->CanonicalType != TEXT("object:") + ObjectType->ClassPath
				|| Type.CanonicalType != CanonicalPrefix + ObjectType->ClassPath
				|| Type.CppType != CppPrefix + ObjectType->CppType + TEXT(">")
				|| Type.Size != 4
				|| Type.Alignment != 4
				|| Type.AbiTypes != TArray<FString>({ TEXT("i") })
				|| Type.CapabilityKind != TEXT("composite"))
			{
				OutErrorSource = Type.StableId + TEXT(":type_arguments");
				return false;
			}
			continue;
		}
		if (Type.Kind == TEXT("set") || Type.Kind == TEXT("map"))
		{
			const int32 ExpectedArguments = Type.Kind == TEXT("set") ? 1 : 2;
			const FAvidScriptBindingTypeModel* KeyOrElement =
				Type.TypeArguments.IsValidIndex(0)
					? TypesById.FindRef(Type.TypeArguments[0])
					: nullptr;
			const FAvidScriptBindingTypeModel* Mapped =
				Type.Kind == TEXT("map") && Type.TypeArguments.IsValidIndex(1)
					? TypesById.FindRef(Type.TypeArguments[1])
					: nullptr;
			const FString ExpectedCanonical = Type.Kind == TEXT("set")
				? (KeyOrElement == nullptr
					? FString()
					: TEXT("set:tset<") + KeyOrElement->CanonicalType + TEXT(">"))
				: (KeyOrElement == nullptr || Mapped == nullptr
					? FString()
					: TEXT("map:tmap<") + KeyOrElement->CanonicalType
						+ TEXT(",") + Mapped->CanonicalType + TEXT(">"));
			const FString ExpectedCpp = Type.Kind == TEXT("set")
				? (KeyOrElement == nullptr
					? FString()
					: TEXT("TSet<") + KeyOrElement->CppType + TEXT(">"))
				: (KeyOrElement == nullptr || Mapped == nullptr
					? FString()
					: TEXT("TMap<") + KeyOrElement->CppType
						+ TEXT(",") + Mapped->CppType + TEXT(">"));
			if (Type.TypeArguments.Num() != ExpectedArguments
				|| Type.TypeArguments.ContainsByPredicate(
					[&TypesById](const FString& TypeId)
					{
						const FAvidScriptBindingTypeModel* Argument =
							TypesById.FindRef(TypeId);
						return Argument == nullptr || Argument->Kind == TEXT("void");
					})
				|| Type.CanonicalType != ExpectedCanonical
				|| Type.CppType != ExpectedCpp
				|| Type.Size != 4
				|| Type.Alignment != 4
				|| Type.AbiTypes != TArray<FString>({ TEXT("i") })
				|| Type.CapabilityKind != TEXT("composite"))
			{
				OutErrorSource = Type.StableId + TEXT(":type_arguments");
				return false;
			}
			continue;
		}
		if (Type.Kind != TEXT("array"))
		{
			continue;
		}
		const FAvidScriptBindingTypeModel* Element = TypesById.FindRef(Type.ElementTypeId);
		const bool bFlatArray = Type.CapabilityKind == TEXT("array_flat");
		if (Element == nullptr
			|| (bFlatArray
				&& (Element->Kind == TEXT("array")
					|| Element->Kind == TEXT("set")
					|| Element->Kind == TEXT("map")
					|| Element->Kind == TEXT("name_utf8")
					|| Element->Kind == TEXT("string_utf8")
					|| Element->CapabilityKind == TEXT("composite")))
			|| Element->Kind == TEXT("void")
			|| Element->Size <= 0
			|| Element->Alignment <= 0
			|| Element->Alignment > 16
			|| Type.Size != 4
			|| Type.Alignment != 4
			|| Type.AbiTypes != TArray<FString>({ TEXT("i") })
			|| (Type.CapabilityKind != TEXT("array_flat")
				&& Type.CapabilityKind != TEXT("composite")))
		{
			OutErrorSource = Type.StableId + TEXT(":element_type_id");
			return false;
		}
	}
	return true;
}

bool ParseAvidScriptBindingDelegateEvent(
	const TSharedPtr<FJsonObject>& Object,
	const int32 SchemaVersion,
	FAvidScriptBindingDelegateEventModel& OutEvent,
	FString& OutErrorSource)
{
	if (!ReadAvidScriptBindingRequiredString(
			Object, TEXT("stable_id"), OutEvent.StableId, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("canonical_identity"),
			OutEvent.CanonicalIdentity,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredInt(
			Object, TEXT("ordinal"), OutEvent.Ordinal, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object, TEXT("owner_class"), OutEvent.OwnerClass, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object, TEXT("ue_member"), OutEvent.UeMember, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object, TEXT("script_name"), OutEvent.ScriptName, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object,
			TEXT("delegate_kind"),
			OutEvent.DelegateKind,
			OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object, TEXT("source_mode"), OutEvent.SourceMode, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(
			Object, TEXT("export_name"), OutEvent.ExportName, OutErrorSource)
		|| !IsAvidScriptBindingLowerSha256(OutEvent.StableId))
	{
		return false;
	}
	if (SchemaVersion >= 22)
	{
		if (!ReadAvidScriptBindingRequiredString(
				Object,
				TEXT("reflected_owner_kind"),
				OutEvent.ReflectedOwnerKind,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredStringAllowEmpty(
				Object,
				TEXT("reflected_owner_asset"),
				OutEvent.ReflectedOwnerAsset,
				OutErrorSource)
			|| !ReadAvidScriptBindingRequiredStringAllowEmpty(
				Object,
				TEXT("reflected_function_fingerprint"),
				OutEvent.ReflectedFunctionFingerprint,
				OutErrorSource))
		{
			return false;
		}
		const bool bNativeProvenance =
			OutEvent.ReflectedOwnerKind == TEXT("native")
			&& OutEvent.ReflectedOwnerAsset.IsEmpty()
			&& OutEvent.ReflectedFunctionFingerprint.IsEmpty();
		const bool bBlueprintProvenance =
			(OutEvent.DelegateKind == TEXT("network_rpc")
				|| OutEvent.DelegateKind == TEXT("rep_notify")
				|| OutEvent.DelegateKind == TEXT("blueprint_event"))
			&& OutEvent.ReflectedOwnerKind == TEXT("blueprint")
			&& !OutEvent.ReflectedOwnerAsset.IsEmpty()
			&& IsAvidScriptBindingLowerSha256(
				OutEvent.ReflectedFunctionFingerprint);
		if ((!bNativeProvenance && !bBlueprintProvenance)
			|| (OutEvent.DelegateKind == TEXT("blueprint_event")
				&& !bBlueprintProvenance))
		{
			OutErrorSource = TEXT("reflection_provenance");
			return false;
		}
	}
	else if (Object->HasField(TEXT("reflected_owner_kind"))
		|| Object->HasField(TEXT("reflected_owner_asset"))
		|| Object->HasField(TEXT("reflected_function_fingerprint")))
	{
		OutErrorSource = TEXT("reflection_provenance");
		return false;
	}

	FString NetworkMode(TEXT("none"));
	FString RepNotifyProperty;
	if (SchemaVersion >= 18)
	{
		if (!Object->TryGetStringField(
				TEXT("handler_mode"),
				OutEvent.HandlerMode))
		{
			OutErrorSource = TEXT("handler_mode");
			return false;
		}
	}
	else if (Object->HasField(TEXT("handler_mode")))
	{
		OutErrorSource = TEXT("handler_mode");
		return false;
	}
	if (SchemaVersion >= 17)
	{
		if (!Object->TryGetStringField(TEXT("network_mode"), NetworkMode)
			|| !TryParseAvidScriptBindingNetworkMode(
				NetworkMode,
				OutEvent.Network.Mode)
			|| !Object->TryGetBoolField(
				TEXT("network_reliable"),
				OutEvent.Network.bReliable)
			|| !Object->TryGetStringField(
				TEXT("rep_notify_property"),
				RepNotifyProperty))
		{
			OutErrorSource = TEXT("callback_contract");
			return false;
		}
	}
	else if (Object->HasField(TEXT("network_mode"))
		|| Object->HasField(TEXT("network_reliable"))
		|| Object->HasField(TEXT("rep_notify_property")))
	{
		OutErrorSource = TEXT("callback_contract");
		return false;
	}
	OutEvent.RepNotifyProperty = FName(*RepNotifyProperty);
	if (SchemaVersion >= 20)
	{
		const TSharedPtr<FJsonObject>* ReturnObject = nullptr;
		if (!Object->TryGetObjectField(TEXT("return"), ReturnObject)
			|| ReturnObject == nullptr
			|| !ReturnObject->IsValid()
			|| !ParseAvidScriptBindingValue(
				*ReturnObject,
				OutEvent.ReturnValue,
				OutErrorSource)
			|| OutEvent.ReturnValue.Direction != TEXT("return")
			|| OutEvent.ReturnValue.bHasDefault)
		{
			OutErrorSource = TEXT("return");
			return false;
		}
	}
	else
	{
		if (Object->HasField(TEXT("return")))
		{
			OutErrorSource = TEXT("return");
			return false;
		}
		OutEvent.ReturnValue.Name = TEXT("ReturnValue");
		OutEvent.ReturnValue.Direction = TEXT("return");
		OutEvent.ReturnValue.CanonicalType = TEXT("void");
		OutEvent.ReturnValue.TypeId =
			FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
				TEXT("void"),
				{});
		OutEvent.ReturnValue.Kind = TEXT("void");
		OutEvent.ReturnValue.CppType = TEXT("void");
	}

	const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
	if (!Object->TryGetArrayField(TEXT("parameters"), Parameters)
		|| Parameters == nullptr)
	{
		OutErrorSource = TEXT("parameters");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Parameters)
	{
		FAvidScriptBindingValueModel Parameter;
		if (!ParseAvidScriptBindingValue(
				Value.IsValid() ? Value->AsObject() : nullptr,
				Parameter,
				OutErrorSource)
			|| Parameter.Direction == TEXT("return")
			|| Parameter.bHasDefault)
		{
			OutErrorSource = TEXT("parameters");
			return false;
		}
		OutEvent.Parameters.Add(MoveTemp(Parameter));
	}
	return true;
}

bool FAvidScriptBindingDescriptorTypeGraph::IsDerivedFromClassPath(
	const FAvidScriptBindingPackageModel& Package,
	const FString& TypeId,
	const FString& ClassPath)
{
	if (TypeId.IsEmpty() || ClassPath.IsEmpty())
	{
		return false;
	}

	FString CurrentTypeId = TypeId;
	TSet<FString> VisitedTypeIds;
	while (!CurrentTypeId.IsEmpty()
		&& !VisitedTypeIds.Contains(CurrentTypeId))
	{
		VisitedTypeIds.Add(CurrentTypeId);
		const FAvidScriptBindingTypeModel* Type =
			Package.Types.FindByPredicate(
				[&CurrentTypeId](
					const FAvidScriptBindingTypeModel& Candidate)
				{
					return Candidate.StableId == CurrentTypeId;
				});
		if (Type == nullptr
			|| Type->Kind != TEXT("object_handle")
			|| Type->ObjectTypeOrdinal == INDEX_NONE)
		{
			return false;
		}
		if (Type->ClassPath == ClassPath)
		{
			return true;
		}
		CurrentTypeId = Type->BaseTypeId;
	}
	return false;
}

FString FAvidScriptBindingDescriptorIdentity::MakeTypeIdentity(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues,
	const TArray<FAvidScriptBindingStructFieldModel>& StructFields,
	const int32 WireSize,
	const int32 WireAlignment,
	const FString& ElementTypeId,
	const TArray<FString>& TypeArguments)
{
	FString Identity = CanonicalType;
	for (const FAvidScriptBindingEnumValue& EnumValue : EnumValues)
	{
		Identity += FString::Printf(
			TEXT("|enum:%d:%s:%lld"),
			EnumValue.Name.Len(),
			*EnumValue.Name,
			EnumValue.Value);
	}
	for (const FAvidScriptBindingStructFieldModel& Field : StructFields)
	{
		Identity += FString::Printf(
			TEXT("|field:%d:%s:%s:%d"),
			Field.Name.Len(),
			*Field.Name,
			*Field.TypeId,
			Field.WireOffset);
	}
	if (!StructFields.IsEmpty()
		&& WireSize != INDEX_NONE
		&& WireAlignment != INDEX_NONE)
	{
		Identity += FString::Printf(
			TEXT("|wire:%d:%d"),
			WireSize,
			WireAlignment);
	}
	if (!ElementTypeId.IsEmpty())
	{
		Identity += TEXT("|element:") + ElementTypeId;
	}
	for (const FString& TypeArgument : TypeArguments)
	{
		Identity += FString::Printf(
			TEXT("|argument:%d:%s"),
			TypeArgument.Len(),
			*TypeArgument);
	}
	return Identity;
}

FString FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues,
	const TArray<FAvidScriptBindingStructFieldModel>& StructFields,
	const int32 WireSize,
	const int32 WireAlignment,
	const FString& ElementTypeId,
	const TArray<FString>& TypeArguments)
{
	return FAvidScriptHash::Sha256HexUtf8(MakeTypeIdentity(
		CanonicalType,
		EnumValues,
		StructFields,
		WireSize,
		WireAlignment,
		ElementTypeId,
		TypeArguments));
}

FString FAvidScriptBindingDescriptorIdentity::MakeClassReferenceIdentity(
	const FString& ClassPath,
	const FString& BaseClassPath,
	const FString& LoadPolicy)
{
	FString Identity(TEXT("class_reference"));
	AppendAvidScriptBindingIdentityField(Identity, TEXT("class"), ClassPath);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("base"), BaseClassPath);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("policy"), LoadPolicy);
	return Identity;
}

FString FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
	const FString& ClassPath,
	const FString& BaseClassPath,
	const FString& LoadPolicy)
{
	return FAvidScriptHash::Sha256HexUtf8(MakeClassReferenceIdentity(
		ClassPath,
		BaseClassPath,
		LoadPolicy));
}

FString FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryIdentity(
	const FString& ClassReferenceId,
	const EAvidScriptObjectFactoryKind Kind,
	const FString& OuterTypeId,
	const EAvidScriptObjectOwnershipPolicy Ownership,
	const EAvidScriptComponentRegistrationPolicy Registration)
{
	FString Identity(TEXT("object_factory"));
	AppendAvidScriptBindingIdentityField(
		Identity,
		TEXT("class_reference_id"),
		ClassReferenceId);
	AppendAvidScriptBindingIdentityField(
		Identity,
		TEXT("kind"),
		LexToString(Kind));
	AppendAvidScriptBindingIdentityField(
		Identity,
		TEXT("outer_type_id"),
		OuterTypeId);
	AppendAvidScriptBindingIdentityField(
		Identity,
		TEXT("ownership"),
		LexToString(Ownership));
	AppendAvidScriptBindingIdentityField(
		Identity,
		TEXT("registration"),
		LexToString(Registration));
	return Identity;
}

FString FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
	const FString& ClassReferenceId,
	const EAvidScriptObjectFactoryKind Kind,
	const FString& OuterTypeId,
	const EAvidScriptObjectOwnershipPolicy Ownership,
	const EAvidScriptComponentRegistrationPolicy Registration)
{
	return FAvidScriptHash::Sha256HexUtf8(MakeObjectFactoryIdentity(
		ClassReferenceId,
		Kind,
		OuterTypeId,
		Ownership,
		Registration));
}

bool FAvidScriptBindingDescriptorIdentity::IsFunctionDispatchModeSupported(
	const int32 SchemaVersion,
	const FString& DispatchMode)
{
	return DispatchMode == TEXT("cached_process_event")
		|| (SchemaVersion >= 8
			&& (DispatchMode == TEXT("qualified_native_direct")
				|| DispatchMode == TEXT("generated_native_s1")))
		|| (SchemaVersion >= 12
			&& DispatchMode == TEXT("latent_process_event"))
		|| (SchemaVersion >= 23
			&& DispatchMode == TEXT("blueprint_async_action"));
}

bool IsAvidScriptBindingLowerHex(const FString& Value)
{
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character)
			&& (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool IsAvidScriptGeneratedShape(const FString& Value)
{
	return Value == TEXT("i32_pair_to_i32")
		|| Value == TEXT("property_i32_get_set")
		|| Value == TEXT("property_i32_get")
		|| Value == TEXT("property_i32_set")
		|| Value == TEXT("vector_value")
		|| Value == TEXT("stable_object_roundtrip");
}

FString FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
	const FString& BaseCanonicalIdentity,
	const FString& DispatchMode,
	const FString& GeneratedShape,
	const FString& GeneratedReceiverMode,
	const FString& GeneratedImportName)
{
	if (DispatchMode == TEXT("cached_process_event"))
	{
		return BaseCanonicalIdentity;
	}
	FString Identity = BaseCanonicalIdentity + TEXT("::dispatch:") + DispatchMode;
	if (DispatchMode == TEXT("generated_native_s1"))
	{
		AppendAvidScriptBindingIdentityField(
			Identity,
			TEXT("generated_shape"),
			GeneratedShape);
		AppendAvidScriptBindingIdentityField(
			Identity,
			TEXT("generated_receiver"),
			GeneratedReceiverMode);
		AppendAvidScriptBindingIdentityField(
			Identity,
			TEXT("generated_import"),
			GeneratedImportName);
	}
	return Identity;
}

FString FAvidScriptBindingDescriptorIdentity::MakeReflectedFunctionFingerprint(
	const FString& CanonicalIdentity,
	const UFunction& Function)
{
	return FAvidScriptHash::Sha256HexUtf8(
		CanonicalIdentity
		+ TEXT("|bytecode_sha256=")
		+ FAvidScriptHash::Sha256Hex(Function.Script));
}

FString FAvidScriptBindingDescriptorIdentity::MakePropertySetCanonicalIdentity(
	const FString& OwnerClass,
	const FString& PropertyName,
	const FString& CanonicalValueType,
	const FString& BlueprintSetterFunction)
{
	FString Identity = OwnerClass
		+ TEXT("::property_set:") + PropertyName
		+ TEXT("(") + CanonicalValueType + TEXT(")");
	if (!BlueprintSetterFunction.IsEmpty())
	{
		Identity += TEXT("::blueprint_setter:") + BlueprintSetterFunction;
	}
	return Identity;
}

FString FAvidScriptBindingDescriptorIdentity::MakeDelegateEventCanonicalIdentity(
	const FString& OwnerClass,
	const FString& DelegatePropertyName,
	const FString& DelegateKind,
	const FString& SourceMode,
	const TConstArrayView<FAvidScriptBindingValueModel> Parameters,
	const FAvidScriptBindingNetworkContract& Network,
	const FName RepNotifyProperty,
	const FString& HandlerMode,
	const FAvidScriptBindingValueModel* ReturnValue)
{
	FString Identity(TEXT("delegate_event"));
	AppendAvidScriptBindingIdentityField(Identity, TEXT("owner"), OwnerClass);
	AppendAvidScriptBindingIdentityField(
		Identity,
		TEXT("member"),
		DelegatePropertyName);
	AppendAvidScriptBindingIdentityField(
		Identity,
		TEXT("kind"),
		DelegateKind);
	AppendAvidScriptBindingIdentityField(
		Identity,
		TEXT("source"),
		SourceMode);
	const bool bFunctionHandler = DelegateKind == TEXT("network_rpc")
		|| DelegateKind == TEXT("rep_notify")
		|| DelegateKind == TEXT("blueprint_event");
	if (bFunctionHandler)
	{
		AppendAvidScriptBindingIdentityField(
			Identity,
			TEXT("network_mode"),
			LexToString(Network.Mode));
		AppendAvidScriptBindingIdentityField(
			Identity,
			TEXT("network_reliable"),
			Network.bReliable ? TEXT("true") : TEXT("false"));
		AppendAvidScriptBindingIdentityField(
			Identity,
			TEXT("rep_notify_property"),
			RepNotifyProperty.ToString());
	}
	if (bFunctionHandler && !HandlerMode.IsEmpty())
	{
		AppendAvidScriptBindingIdentityField(
			Identity,
			TEXT("handler_mode"),
			HandlerMode);
	}
	for (const FAvidScriptBindingValueModel& Parameter : Parameters)
	{
		AppendAvidScriptBindingValueIdentity(
			Identity,
			TEXT("parameter"),
			Parameter);
	}
	if (ReturnValue != nullptr)
	{
		AppendAvidScriptBindingValueIdentity(
			Identity,
			TEXT("return"),
			*ReturnValue);
	}
	return Identity;
}

FString FAvidScriptBindingDescriptorIdentity::MakeDelegateEventStableId(
	const FString& OwnerClass,
	const FString& DelegatePropertyName,
	const FString& DelegateKind,
	const FString& SourceMode,
	const TConstArrayView<FAvidScriptBindingValueModel> Parameters,
	const FAvidScriptBindingNetworkContract& Network,
	const FName RepNotifyProperty,
	const FString& HandlerMode,
	const FAvidScriptBindingValueModel* ReturnValue)
{
	return FAvidScriptHash::Sha256HexUtf8(MakeDelegateEventCanonicalIdentity(
		OwnerClass,
		DelegatePropertyName,
		DelegateKind,
		SourceMode,
		Parameters,
		Network,
		RepNotifyProperty,
		HandlerMode,
		ReturnValue));
}

bool FAvidScriptBindingDescriptorIdentity::TryMakeDelegateInvokeSpec(
	const FAvidScriptBindingDelegateEventModel& Event,
	const int32 BindingOrdinal,
	FAvidScriptBindingDelegateInvokeSpec& OutSpec)
{
	OutSpec = FAvidScriptBindingDelegateInvokeSpec();
	if ((Event.DelegateKind != TEXT("singlecast")
			&& Event.DelegateKind != TEXT("multicast"))
		|| Event.StableId.Len() != 64
		|| Event.CanonicalIdentity.IsEmpty()
		|| BindingOrdinal < 0)
	{
		return false;
	}

	FString Parameters(TEXT("ii"));
	for (const FAvidScriptBindingValueModel& Parameter : Event.Parameters)
	{
		if (Parameter.Direction == TEXT("ref")
			|| Parameter.Direction == TEXT("out"))
		{
			Parameters += TEXT("i");
			continue;
		}
		if ((Parameter.Direction != TEXT("value")
				&& Parameter.Direction != TEXT("const_ref"))
			|| Parameter.AbiTypes.IsEmpty())
		{
			return false;
		}
		Parameters += FString::Join(Parameter.AbiTypes, TEXT(""));
	}
	if (Event.ReturnValue.CanonicalType != TEXT("void"))
	{
		if (Event.ReturnValue.Direction != TEXT("return"))
		{
			return false;
		}
		Parameters += TEXT("i");
	}

	OutSpec.CanonicalIdentity = Event.CanonicalIdentity
		+ TEXT("::delegate_invoke:v1");
	OutSpec.StableId = FAvidScriptHash::Sha256HexUtf8(
		OutSpec.CanonicalIdentity);
	OutSpec.BindingOrdinal = BindingOrdinal;
	OutSpec.ModuleName = TEXT("avidscript");
	OutSpec.ImportName = TEXT("avid_ue_") + OutSpec.StableId.Left(16);
	OutSpec.Signature = TEXT("(") + Parameters + TEXT(")i");
	return true;
}

FString FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(
	const FAvidScriptBindingPackageModel& Package)
{
	TArray<FString> SelectionKeys;
	SelectionKeys.Reserve(Package.Bindings.Num() + Package.DelegateEvents.Num());
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		FString Key = Binding.BindingKind == TEXT("function")
			? Binding.OwnerClass + TEXT(".") + Binding.UeMember
			: Binding.BindingKind + TEXT(":") + Binding.OwnerClass + TEXT(".") + Binding.UeMember;
		if (Package.SchemaVersion >= 8
			&& ((Binding.BindingKind == TEXT("function")
					&& Binding.DispatchMode != TEXT("cached_process_event"))
				|| Binding.DispatchMode == TEXT("generated_native_s1")))
		{
			Key += TEXT("|dispatch=") + Binding.DispatchMode;
			if (Binding.DispatchMode == TEXT("generated_native_s1"))
			{
				Key += TEXT("|shape=") + Binding.GeneratedShape
					+ TEXT("|receiver=") + Binding.GeneratedReceiverMode
					+ TEXT("|import=") + Binding.GeneratedImportName;
			}
		}
		if (Package.SchemaVersion >= 12
			&& Binding.DispatchMode == TEXT("latent_process_event"))
		{
			Key += TEXT("|latent_info=") + Binding.LatentInfoParameter
				+ TEXT("|world_context=") + Binding.WorldContextParameter;
			if (Package.SchemaVersion >= 13)
			{
				Key += TEXT("|completion=") + Binding.Completion.Mode
					+ TEXT("|provider_id=") + Binding.Completion.ProviderId
					+ TEXT("|payload_type_id=")
					+ Binding.Completion.PayloadTypeId
					+ TEXT("|status_policy=")
					+ Binding.Completion.StatusPolicy
					+ TEXT("|cancellable=")
					+ (Binding.Completion.bCancellable
						? TEXT("1")
						: TEXT("0"));
			}
		}
		if (Package.SchemaVersion >= 21)
		{
			Key += TEXT("|reflected_owner_kind=")
				+ Binding.ReflectedOwnerKind
				+ TEXT("|reflected_owner_asset=")
				+ Binding.ReflectedOwnerAsset
				+ TEXT("|reflected_function_fingerprint=")
				+ Binding.ReflectedFunctionFingerprint;
		}
		if (Package.SchemaVersion >= 23
			&& Binding.AsyncAction.IsEnabled())
		{
			Key += TEXT("|async_action_class=")
				+ Binding.AsyncAction.ActionClass
				+ TEXT("|activation=")
				+ Binding.AsyncAction.ActivationFunction
				+ TEXT("|payload_type_id=")
				+ Binding.AsyncAction.PayloadTypeId
				+ TEXT("|completion_policy=")
				+ Binding.AsyncAction.CompletionPolicy;
			for (const FAvidScriptBindingAsyncActionOutcomeModel& Outcome :
				Binding.AsyncAction.Outcomes)
			{
				Key += TEXT("|outcome=")
					+ FString::FromInt(Outcome.Ordinal)
					+ TEXT(":") + Outcome.DelegateMember
					+ TEXT(":") + Outcome.StableId;
			}
		}
		SelectionKeys.Add(MoveTemp(Key));
	}
	if (Package.SchemaVersion >= 11)
	{
		for (const FAvidScriptBindingDelegateEventModel& Event :
			Package.DelegateEvents)
		{
			FString Key = TEXT("delegate_event:")
				+ Event.OwnerClass
				+ TEXT(".")
				+ Event.UeMember;
			if (Package.SchemaVersion >= 17)
			{
				Key += TEXT("|kind=") + Event.DelegateKind
					+ TEXT("|network_mode=")
					+ LexToString(Event.Network.Mode)
					+ TEXT("|network_reliable=")
					+ (Event.Network.bReliable ? TEXT("true") : TEXT("false"))
					+ TEXT("|rep_notify_property=")
					+ Event.RepNotifyProperty.ToString();
			}
			if (Package.SchemaVersion >= 18)
			{
				Key += TEXT("|handler_mode=") + Event.HandlerMode;
			}
			if (Package.SchemaVersion >= 22)
			{
				Key += TEXT("|reflected_owner_kind=")
					+ Event.ReflectedOwnerKind
					+ TEXT("|reflected_owner_asset=")
					+ Event.ReflectedOwnerAsset
					+ TEXT("|reflected_function_fingerprint=")
					+ Event.ReflectedFunctionFingerprint;
			}
			SelectionKeys.Add(MoveTemp(Key));
		}
	}
	SelectionKeys.Sort([](const FString& Left, const FString& Right)
	{
		return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
	});

	if (Package.SchemaVersion < 5)
	{
		return FAvidScriptHash::Sha256HexUtf8(FString::Join(SelectionKeys, TEXT("\n")));
	}

	FString Identity(Package.SchemaVersion >= 23
		? TEXT("descriptor_selection_v23")
		: Package.SchemaVersion >= 22
		? TEXT("descriptor_selection_v22")
		: Package.SchemaVersion >= 21
		? TEXT("descriptor_selection_v21")
		: Package.SchemaVersion >= 20
		? TEXT("descriptor_selection_v20")
		: Package.SchemaVersion >= 18
		? TEXT("descriptor_selection_v18")
		: Package.SchemaVersion >= 17
		? TEXT("descriptor_selection_v17")
		: Package.SchemaVersion >= 16
		? TEXT("descriptor_selection_v16")
		: Package.SchemaVersion >= 15
		? TEXT("descriptor_selection_v15")
		: Package.SchemaVersion >= 14
		? TEXT("descriptor_selection_v14")
		: Package.SchemaVersion >= 13
		? TEXT("descriptor_selection_v13")
		: Package.SchemaVersion >= 11
		? TEXT("descriptor_selection_v11")
		: Package.SchemaVersion >= 9
		? TEXT("descriptor_selection_v9")
		: Package.SchemaVersion >= 8
		? TEXT("descriptor_selection_v8")
		: Package.SchemaVersion >= 7
			? TEXT("descriptor_selection_v7")
		: Package.SchemaVersion >= 6
			? TEXT("descriptor_selection_v6")
			: TEXT("descriptor_selection_v5"));
	for (const FString& Key : SelectionKeys)
	{
		AppendAvidScriptBindingIdentityField(Identity, TEXT("binding"), Key);
	}
	if (Package.SchemaVersion >= 15)
	{
		for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("network_mode"),
				LexToString(Binding.Network.Mode));
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("network_reliable"),
				Binding.Network.bReliable ? TEXT("1") : TEXT("0"));
		}
	}
	if (Package.SchemaVersion >= 16)
	{
		for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("property_replication"),
				LexToString(Binding.PropertyReplication.Mode));
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("rep_notify"),
				Binding.PropertyReplication.RepNotifyFunction.ToString());
		}
	}
	if (Package.SchemaVersion >= 6)
	{
		AppendAvidScriptBindingIdentityField(Identity, TEXT("self_type_id"), Package.SelfTypeId);
		if (Package.bHasActiveObjectTypeOrdinals)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("runtime_object_type_slice"),
				TEXT("1"));
			for (const int32 Ordinal : Package.ActiveObjectTypeOrdinals)
			{
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("active_object_type_ordinal"),
					FString::FromInt(Ordinal));
			}
		}
		for (const FAvidScriptBindingTypeModel& Type : Package.Types)
		{
			if (Type.ObjectTypeOrdinal == INDEX_NONE)
			{
				continue;
			}
			AppendAvidScriptBindingIdentityField(Identity, TEXT("object_type_id"), Type.StableId);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("object_type_ordinal"),
				FString::FromInt(Type.ObjectTypeOrdinal));
			AppendAvidScriptBindingIdentityField(Identity, TEXT("object_class_path"), Type.ClassPath);
			AppendAvidScriptBindingIdentityField(Identity, TEXT("object_base_type_id"), Type.BaseTypeId);
		}
	}
	if (Package.SchemaVersion >= 9)
	{
		for (const FAvidScriptBindingTypeModel& Type : Package.Types)
		{
			if (Type.Kind != TEXT("struct_wire"))
			{
				continue;
			}
			AppendAvidScriptBindingIdentityField(Identity, TEXT("struct_wire_type_id"), Type.StableId);
			AppendAvidScriptBindingIdentityField(Identity, TEXT("struct_wire_type_size"), FString::FromInt(Type.Size));
			AppendAvidScriptBindingIdentityField(Identity, TEXT("struct_wire_type_alignment"), FString::FromInt(Type.Alignment));
			for (const FAvidScriptBindingStructFieldModel& Field : Type.StructFields)
			{
				AppendAvidScriptBindingIdentityField(Identity, TEXT("struct_wire_field_name"), Field.Name);
				AppendAvidScriptBindingIdentityField(Identity, TEXT("struct_wire_field_type_id"), Field.TypeId);
				AppendAvidScriptBindingIdentityField(Identity, TEXT("struct_wire_field_wire_offset"), FString::FromInt(Field.WireOffset));
			}
		}
	}
	for (const FAvidScriptBindingClassReferenceModel& Reference : Package.ClassReferences)
	{
		AppendAvidScriptBindingIdentityField(Identity, TEXT("class_stable_id"), Reference.StableId);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("class_script_name"), Reference.ScriptName);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("class_path"), Reference.ClassPath);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("base_class_path"), Reference.BaseClassPath);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("load_policy"), Reference.LoadPolicy);
		if (Package.SchemaVersion >= 6)
		{
			AppendAvidScriptBindingIdentityField(Identity, TEXT("result_type_id"), Reference.ResultTypeId);
		}
	}
	if (Package.SchemaVersion >= 7)
	{
		for (const FAvidScriptBindingObjectFactoryModel& Factory :
			Package.ObjectFactories)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_id"),
				Factory.StableId);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_ordinal"),
				FString::FromInt(Factory.Ordinal));
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_script_name"),
				Factory.ScriptName);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_class_reference_id"),
				Factory.ClassReferenceId);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_kind"),
				LexToString(Factory.Kind));
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_outer_type_id"),
				Factory.OuterTypeId);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_ownership"),
				LexToString(Factory.Ownership));
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_registration"),
				LexToString(Factory.Registration));
		}
	}
	return FAvidScriptHash::Sha256HexUtf8(Identity);
}

FString FAvidScriptBindingDescriptorIdentity::MakePackageHash(
	const FAvidScriptBindingPackageModel& Package)
{
	if (Package.SchemaVersion < 5)
	{
		FString Identity = Package.PackageName
			+ TEXT("|") + Package.GeneratorVersion
			+ TEXT("|") + Package.EngineVersion
			+ TEXT("|") + Package.SelectionHash;
		for (const FAvidScriptBindingTypeModel& Type : Package.Types)
		{
			Identity += TEXT("|type:") + Type.StableId + TEXT(":") + Type.CanonicalType + TEXT(":") + FString::Join(Type.AbiTypes, TEXT(""));
		}
		for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
		{
			Identity += TEXT("|binding:") + Binding.CanonicalIdentity + TEXT(":") + Binding.HostImport.Signature;
			if (Package.SchemaVersion >= 3)
			{
				Identity += TEXT("|reload_effect:") + FString(LexToString(Binding.ReloadEffect));
			}
			for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
			{
				Identity += TEXT("|default:") + Parameter.Name + TEXT(":");
				if (Parameter.bHasDefault)
				{
					Identity += TEXT("1:") + FString::FromInt(Parameter.DefaultValue.Len()) + TEXT(":") + Parameter.DefaultValue;
				}
				else
				{
					Identity += TEXT("0");
				}
			}
		}
		return FAvidScriptHash::Sha256HexUtf8(Identity);
	}

	FString Identity(Package.SchemaVersion >= 23
		? TEXT("descriptor_package_v23")
		: Package.SchemaVersion >= 22
		? TEXT("descriptor_package_v22")
		: Package.SchemaVersion >= 21
		? TEXT("descriptor_package_v21")
		: Package.SchemaVersion >= 20
		? TEXT("descriptor_package_v20")
		: Package.SchemaVersion >= 18
		? TEXT("descriptor_package_v18")
		: Package.SchemaVersion >= 17
		? TEXT("descriptor_package_v17")
		: Package.SchemaVersion >= 16
		? TEXT("descriptor_package_v16")
		: Package.SchemaVersion >= 15
		? TEXT("descriptor_package_v15")
		: Package.SchemaVersion >= 14
		? TEXT("descriptor_package_v14")
		: Package.SchemaVersion >= 13
		? TEXT("descriptor_package_v13")
		: Package.SchemaVersion >= 11
		? TEXT("descriptor_package_v11")
		: Package.SchemaVersion >= 9
		? TEXT("descriptor_package_v9")
		: Package.SchemaVersion >= 8
		? TEXT("descriptor_package_v8")
		: Package.SchemaVersion >= 7
			? TEXT("descriptor_package_v7")
		: Package.SchemaVersion >= 6
			? TEXT("descriptor_package_v6")
			: TEXT("descriptor_package_v5"));
	AppendAvidScriptBindingIdentityField(Identity, TEXT("schema"), FString::FromInt(Package.SchemaVersion));
	AppendAvidScriptBindingIdentityField(Identity, TEXT("generator"), Package.GeneratorVersion);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("engine"), Package.EngineVersion);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("source"), Package.Source);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("package"), Package.PackageName);
	AppendAvidScriptBindingIdentityField(Identity, TEXT("selection"), Package.SelectionHash);
	if (Package.SchemaVersion >= 8
		&& !Package.GeneratedSourcePackageHash.IsEmpty())
	{
		AppendAvidScriptBindingIdentityField(
			Identity,
			TEXT("generated_source_package_hash"),
			Package.GeneratedSourcePackageHash);
	}
	if (Package.SchemaVersion >= 6)
	{
		AppendAvidScriptBindingIdentityField(Identity, TEXT("self_type_id"), Package.SelfTypeId);
		if (Package.bHasActiveObjectTypeOrdinals)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("runtime_object_type_slice"),
				TEXT("1"));
			for (const int32 Ordinal : Package.ActiveObjectTypeOrdinals)
			{
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("active_object_type_ordinal"),
					FString::FromInt(Ordinal));
			}
		}
	}
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		AppendAvidScriptBindingIdentityField(Identity, TEXT("type_id"), Type.StableId);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("canonical_type"), Type.CanonicalType);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("type_kind"), Type.Kind);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("type_cpp"), Type.CppType);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("type_size"), FString::FromInt(Type.Size));
		AppendAvidScriptBindingIdentityField(Identity, TEXT("type_alignment"), FString::FromInt(Type.Alignment));
		for (const FString& AbiType : Type.AbiTypes)
		{
			AppendAvidScriptBindingIdentityField(Identity, TEXT("type_abi"), AbiType);
		}
		for (const FAvidScriptBindingEnumValue& EnumValue : Type.EnumValues)
		{
			AppendAvidScriptBindingIdentityField(Identity, TEXT("enum_name"), EnumValue.Name);
			AppendAvidScriptBindingIdentityField(Identity, TEXT("enum_value"), FString::Printf(TEXT("%lld"), EnumValue.Value));
		}
		if (Package.SchemaVersion >= 9 && Type.Kind == TEXT("struct_wire"))
		{
			for (const FAvidScriptBindingStructFieldModel& Field : Type.StructFields)
			{
				AppendAvidScriptBindingIdentityField(Identity, TEXT("struct_wire_field_name"), Field.Name);
				AppendAvidScriptBindingIdentityField(Identity, TEXT("struct_wire_field_type_id"), Field.TypeId);
				AppendAvidScriptBindingIdentityField(Identity, TEXT("struct_wire_field_wire_offset"), FString::FromInt(Field.WireOffset));
			}
		}
		if (Package.SchemaVersion >= 10 && Type.Kind == TEXT("array"))
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("array_element_type_id"),
				Type.ElementTypeId);
		}
		if (Package.SchemaVersion >= 19)
		{
			for (const FString& TypeArgument : Type.TypeArguments)
			{
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("type_argument"),
					TypeArgument);
			}
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("capability_kind"),
				Type.CapabilityKind);
		}
		if (Package.SchemaVersion >= 6)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("object_type_ordinal"),
				FString::FromInt(Type.ObjectTypeOrdinal));
			AppendAvidScriptBindingIdentityField(Identity, TEXT("object_class_path"), Type.ClassPath);
			AppendAvidScriptBindingIdentityField(Identity, TEXT("object_base_type_id"), Type.BaseTypeId);
		}
	}
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		AppendAvidScriptBindingIdentityField(Identity, TEXT("binding_id"), Binding.StableId);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("binding_identity"), Binding.CanonicalIdentity);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("binding_ordinal"), FString::FromInt(Binding.Ordinal));
		AppendAvidScriptBindingIdentityField(Identity, TEXT("owner"), Binding.OwnerClass);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("binding_kind"), Binding.BindingKind);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("member"), Binding.UeMember);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("script_name"), Binding.ScriptName);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("dispatch"), Binding.DispatchMode);
		if (Package.SchemaVersion >= 21)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("reflected_owner_kind"),
				Binding.ReflectedOwnerKind);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("reflected_owner_asset"),
				Binding.ReflectedOwnerAsset);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("reflected_function_fingerprint"),
				Binding.ReflectedFunctionFingerprint);
		}
		if (Package.SchemaVersion >= 23
			&& Binding.AsyncAction.IsEnabled())
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("async_action_class"),
				Binding.AsyncAction.ActionClass);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("async_action_activation"),
				Binding.AsyncAction.ActivationFunction);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("async_action_payload_type"),
				Binding.AsyncAction.PayloadTypeId);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("async_action_completion_policy"),
				Binding.AsyncAction.CompletionPolicy);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("async_action_cancellable"),
				Binding.AsyncAction.bCancellable
					? TEXT("1")
					: TEXT("0"));
			for (const FAvidScriptBindingAsyncActionOutcomeModel& Outcome :
				Binding.AsyncAction.Outcomes)
			{
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("async_action_outcome_id"),
					Outcome.StableId);
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("async_action_outcome_ordinal"),
					FString::FromInt(Outcome.Ordinal));
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("async_action_outcome_delegate"),
					Outcome.DelegateMember);
			}
		}
		if (Package.SchemaVersion >= 12
			&& Binding.DispatchMode == TEXT("latent_process_event"))
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("latent_info_parameter"),
				Binding.LatentInfoParameter);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("world_context_parameter"),
				Binding.WorldContextParameter);
			if (Package.SchemaVersion >= 13)
			{
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("completion_mode"),
					Binding.Completion.Mode);
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("completion_provider"),
					Binding.Completion.ProviderId);
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("completion_payload_type"),
					Binding.Completion.PayloadTypeId);
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("completion_status_policy"),
					Binding.Completion.StatusPolicy);
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("completion_cancellable"),
					Binding.Completion.bCancellable
						? TEXT("1")
						: TEXT("0"));
			}
		}
		if (Binding.DispatchMode == TEXT("generated_native_s1"))
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("generated_shape"),
				Binding.GeneratedShape);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("generated_receiver"),
				Binding.GeneratedReceiverMode);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("generated_import"),
				Binding.GeneratedImportName);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("semantic_fallback_ordinal"),
				FString::FromInt(Binding.SemanticFallbackOrdinal));
		}
		if (Package.SchemaVersion >= 8)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("write_policy"),
				Binding.WritePolicy);
			if (Binding.BindingKind == TEXT("property_set"))
			{
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("ue_function"),
					Binding.UeFunction);
			}
		}
		AppendAvidScriptBindingIdentityField(Identity, TEXT("static"), Binding.bStatic ? TEXT("1") : TEXT("0"));
		AppendAvidScriptBindingIdentityField(Identity, TEXT("const"), Binding.bConst ? TEXT("1") : TEXT("0"));
		if (Package.SchemaVersion >= 15)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("network_mode"),
				LexToString(Binding.Network.Mode));
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("network_reliable"),
				Binding.Network.bReliable ? TEXT("1") : TEXT("0"));
		}
		if (Package.SchemaVersion >= 16)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("property_replication"),
				LexToString(Binding.PropertyReplication.Mode));
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("rep_notify"),
				Binding.PropertyReplication.RepNotifyFunction.ToString());
		}
		AppendAvidScriptBindingIdentityField(Identity, TEXT("reload"), LexToString(Binding.ReloadEffect));
		AppendAvidScriptBindingValueIdentity(Identity, TEXT("return"), Binding.ReturnValue);
		for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
		{
			AppendAvidScriptBindingValueIdentity(Identity, TEXT("parameter"), Parameter);
		}
		AppendAvidScriptBindingIdentityField(Identity, TEXT("import_module"), Binding.HostImport.Module);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("import_name"), Binding.HostImport.Name);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("import_signature"), Binding.HostImport.Signature);
	}
	if (Package.SchemaVersion >= 11)
	{
		for (const FAvidScriptBindingDelegateEventModel& Event :
			Package.DelegateEvents)
		{
			AppendAvidScriptBindingIdentityField(
				Identity, TEXT("event_id"), Event.StableId);
			AppendAvidScriptBindingIdentityField(
				Identity, TEXT("event_identity"), Event.CanonicalIdentity);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("event_ordinal"),
				FString::FromInt(Event.Ordinal));
			AppendAvidScriptBindingIdentityField(
				Identity, TEXT("event_owner"), Event.OwnerClass);
			AppendAvidScriptBindingIdentityField(
				Identity, TEXT("event_member"), Event.UeMember);
			AppendAvidScriptBindingIdentityField(
				Identity, TEXT("event_script_name"), Event.ScriptName);
			AppendAvidScriptBindingIdentityField(
				Identity, TEXT("event_kind"), Event.DelegateKind);
			AppendAvidScriptBindingIdentityField(
				Identity, TEXT("event_source"), Event.SourceMode);
			if (Package.SchemaVersion >= 17)
			{
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("event_network_mode"),
					LexToString(Event.Network.Mode));
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("event_network_reliable"),
					Event.Network.bReliable ? TEXT("true") : TEXT("false"));
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("event_rep_notify_property"),
					Event.RepNotifyProperty.ToString());
			}
			if (Package.SchemaVersion >= 18)
			{
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("event_handler_mode"),
					Event.HandlerMode);
			}
			if (Package.SchemaVersion >= 22)
			{
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("event_reflected_owner_kind"),
					Event.ReflectedOwnerKind);
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("event_reflected_owner_asset"),
					Event.ReflectedOwnerAsset);
				AppendAvidScriptBindingIdentityField(
					Identity,
					TEXT("event_reflected_function_fingerprint"),
					Event.ReflectedFunctionFingerprint);
			}
			AppendAvidScriptBindingIdentityField(
				Identity, TEXT("event_export"), Event.ExportName);
			if (Package.SchemaVersion >= 20)
			{
				AppendAvidScriptBindingValueIdentity(
					Identity,
					TEXT("event_return"),
					Event.ReturnValue);
			}
			for (const FAvidScriptBindingValueModel& Parameter :
				Event.Parameters)
			{
				AppendAvidScriptBindingValueIdentity(
					Identity,
					TEXT("event_parameter"),
					Parameter);
			}
		}
	}
	for (const FAvidScriptBindingClassReferenceModel& Reference : Package.ClassReferences)
	{
		AppendAvidScriptBindingIdentityField(Identity, TEXT("class_id"), Reference.StableId);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("class_ordinal"), FString::FromInt(Reference.Ordinal));
		AppendAvidScriptBindingIdentityField(Identity, TEXT("class_script_name"), Reference.ScriptName);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("class_path"), Reference.ClassPath);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("base_class_path"), Reference.BaseClassPath);
		AppendAvidScriptBindingIdentityField(Identity, TEXT("load_policy"), Reference.LoadPolicy);
		if (Package.SchemaVersion >= 6)
		{
			AppendAvidScriptBindingIdentityField(Identity, TEXT("result_type_id"), Reference.ResultTypeId);
		}
	}
	if (Package.SchemaVersion >= 7)
	{
		for (const FAvidScriptBindingObjectFactoryModel& Factory :
			Package.ObjectFactories)
		{
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_id"),
				Factory.StableId);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_ordinal"),
				FString::FromInt(Factory.Ordinal));
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_script_name"),
				Factory.ScriptName);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_class_reference_id"),
				Factory.ClassReferenceId);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_kind"),
				LexToString(Factory.Kind));
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_outer_type_id"),
				Factory.OuterTypeId);
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_ownership"),
				LexToString(Factory.Ownership));
			AppendAvidScriptBindingIdentityField(
				Identity,
				TEXT("factory_registration"),
				LexToString(Factory.Registration));
		}
	}
	return FAvidScriptHash::Sha256HexUtf8(Identity);
}

bool FAvidScriptBindingDescriptorParser::Parse(
	const FString& Json,
	FAvidScriptBindingPackageModel& OutPackage,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	OutPackage = FAvidScriptBindingPackageModel();
	OutErrorCategory.Empty();
	OutErrorSource.Empty();
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutErrorCategory = TEXT("descriptor_invalid_json");
		OutErrorSource = TEXT("root");
		return false;
	}

	if (!ReadAvidScriptBindingRequiredInt(Root, TEXT("schema_version"), OutPackage.SchemaVersion, OutErrorSource)
		|| (OutPackage.SchemaVersion != 2
			&& OutPackage.SchemaVersion != 3
			&& OutPackage.SchemaVersion != 4
			&& OutPackage.SchemaVersion != 5
			&& OutPackage.SchemaVersion != 6
			&& OutPackage.SchemaVersion != 7
			&& OutPackage.SchemaVersion != 8
			&& OutPackage.SchemaVersion != 9
			&& OutPackage.SchemaVersion != 10
			&& OutPackage.SchemaVersion != 11
			&& OutPackage.SchemaVersion != 12
			&& OutPackage.SchemaVersion != 13
			&& OutPackage.SchemaVersion != 14
			&& OutPackage.SchemaVersion != 15
			&& OutPackage.SchemaVersion != 16
			&& OutPackage.SchemaVersion != 17
			&& OutPackage.SchemaVersion != 18
			&& OutPackage.SchemaVersion != 19
			&& OutPackage.SchemaVersion != 20
			&& OutPackage.SchemaVersion != 21
			&& OutPackage.SchemaVersion != 22
			&& OutPackage.SchemaVersion != 23)
		|| !ReadAvidScriptBindingRequiredString(Root, TEXT("generator_version"), OutPackage.GeneratorVersion, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Root, TEXT("engine_version"), OutPackage.EngineVersion, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Root, TEXT("source"), OutPackage.Source, OutErrorSource)
		|| OutPackage.Source != TEXT("ue_reflection")
		|| !ReadAvidScriptBindingRequiredString(Root, TEXT("package_name"), OutPackage.PackageName, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Root, TEXT("package_hash"), OutPackage.PackageHash, OutErrorSource)
		|| !ReadAvidScriptBindingRequiredString(Root, TEXT("selection_hash"), OutPackage.SelectionHash, OutErrorSource)
		|| !IsAvidScriptBindingLowerSha256(OutPackage.PackageHash)
		|| !IsAvidScriptBindingLowerSha256(OutPackage.SelectionHash))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		if (OutErrorSource.IsEmpty())
		{
			if (OutPackage.SchemaVersion != 2
				&& OutPackage.SchemaVersion != 3
				&& OutPackage.SchemaVersion != 4
				&& OutPackage.SchemaVersion != 5
				&& OutPackage.SchemaVersion != 6
				&& OutPackage.SchemaVersion != 7
				&& OutPackage.SchemaVersion != 8
				&& OutPackage.SchemaVersion != 9
				&& OutPackage.SchemaVersion != 10
				&& OutPackage.SchemaVersion != 11
				&& OutPackage.SchemaVersion != 12
				&& OutPackage.SchemaVersion != 13
				&& OutPackage.SchemaVersion != 14
				&& OutPackage.SchemaVersion != 15
				&& OutPackage.SchemaVersion != 16
				&& OutPackage.SchemaVersion != 17
				&& OutPackage.SchemaVersion != 18
				&& OutPackage.SchemaVersion != 19
				&& OutPackage.SchemaVersion != 20
				&& OutPackage.SchemaVersion != 21
				&& OutPackage.SchemaVersion != 22
				&& OutPackage.SchemaVersion != 23)
			{
				OutErrorSource = TEXT("schema_version");
			}
			else if (OutPackage.Source != TEXT("ue_reflection"))
			{
				OutErrorSource = TEXT("source");
			}
			else if (!IsAvidScriptBindingLowerSha256(OutPackage.PackageHash))
			{
				OutErrorSource = TEXT("package_hash");
			}
			else if (!IsAvidScriptBindingLowerSha256(OutPackage.SelectionHash))
			{
				OutErrorSource = TEXT("selection_hash");
			}
			else
			{
				OutErrorSource = TEXT("root");
			}
		}
		return false;
	}
	if (OutPackage.SchemaVersion >= 6
		&& !ReadAvidScriptBindingRequiredStringAllowEmpty(
			Root,
			TEXT("self_type_id"),
			OutPackage.SelfTypeId,
			OutErrorSource))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		return false;
	}
	if (Root->HasField(TEXT("generated_source_package_hash")))
	{
		if (OutPackage.SchemaVersion < 8
			|| !Root->TryGetStringField(
				TEXT("generated_source_package_hash"),
				OutPackage.GeneratedSourcePackageHash)
			|| !IsAvidScriptBindingLowerSha256(
				OutPackage.GeneratedSourcePackageHash))
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = TEXT("generated_source_package_hash");
			return false;
		}
	}
	if (OutPackage.SchemaVersion < 5 && Root->HasField(TEXT("class_references")))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		OutErrorSource = TEXT("class_references");
		return false;
	}
	if (OutPackage.SchemaVersion < 7 && Root->HasField(TEXT("object_factories")))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		OutErrorSource = TEXT("object_factories");
		return false;
	}
	if (OutPackage.SchemaVersion < 11 && Root->HasField(TEXT("delegate_events")))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		OutErrorSource = TEXT("delegate_events");
		return false;
	}
	if (OutPackage.SchemaVersion < 6
		&& Root->HasField(TEXT("active_object_type_ordinals")))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		OutErrorSource = TEXT("active_object_type_ordinals");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
	if (!Root->TryGetArrayField(TEXT("types"), Types)
		|| Types == nullptr
		|| (OutPackage.SchemaVersion < 5 && Types->IsEmpty()))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		OutErrorSource = TEXT("types");
		return false;
	}
	TSet<FString> TypeIds;
	TSet<FString> CanonicalTypes;
	TMap<FString, FAvidScriptBindingTypeModel> TypesByCanonical;
	for (const TSharedPtr<FJsonValue>& Value : *Types)
	{
		FAvidScriptBindingTypeModel Type;
		if (!ParseAvidScriptBindingType(
				Value.IsValid() ? Value->AsObject() : nullptr,
				OutPackage.SchemaVersion,
				Type,
				OutErrorSource)
			|| TypeIds.Contains(Type.StableId)
			|| CanonicalTypes.Contains(Type.CanonicalType))
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			if (OutErrorSource.IsEmpty())
			{
				OutErrorSource = FString::Printf(
					TEXT("types[%d]"),
					OutPackage.Types.Num());
			}
			return false;
		}
		TypeIds.Add(Type.StableId);
		CanonicalTypes.Add(Type.CanonicalType);
		TypesByCanonical.Add(Type.CanonicalType, Type);
		OutPackage.Types.Add(MoveTemp(Type));
	}
	if (!FAvidScriptBindingDescriptorLayout::ValidateTypeGraph(
			OutPackage.Types,
			OutErrorSource))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		return false;
	}
	if (Root->HasField(TEXT("active_object_type_ordinals")))
	{
		const TArray<TSharedPtr<FJsonValue>>* ActiveOrdinals = nullptr;
		if (!Root->TryGetArrayField(
				TEXT("active_object_type_ordinals"),
				ActiveOrdinals)
			|| ActiveOrdinals == nullptr)
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = TEXT("active_object_type_ordinals");
			return false;
		}
		int32 ObjectTypeCount = 0;
		for (const FAvidScriptBindingTypeModel& Type : OutPackage.Types)
		{
			ObjectTypeCount += Type.ObjectTypeOrdinal != INDEX_NONE ? 1 : 0;
		}
		int32 PreviousOrdinal = INDEX_NONE;
		for (const TSharedPtr<FJsonValue>& Value : *ActiveOrdinals)
		{
			const double Number = Value.IsValid() && Value->Type == EJson::Number
				? Value->AsNumber()
				: -1.0;
			if (!FMath::IsFinite(Number)
				|| Number < 0.0
				|| Number > static_cast<double>(MAX_int32)
				|| FMath::TruncToDouble(Number) != Number)
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = TEXT("active_object_type_ordinals");
				return false;
			}
			const int32 Ordinal = static_cast<int32>(Number);
			if (Ordinal <= PreviousOrdinal || Ordinal >= ObjectTypeCount)
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = TEXT("active_object_type_ordinals");
				return false;
			}
			OutPackage.ActiveObjectTypeOrdinals.Add(Ordinal);
			PreviousOrdinal = Ordinal;
		}
		OutPackage.bHasActiveObjectTypeOrdinals = true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
	if (!Root->TryGetArrayField(TEXT("bindings"), Bindings)
		|| Bindings == nullptr
		|| (OutPackage.SchemaVersion < 5 && Bindings->IsEmpty()))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		OutErrorSource = TEXT("bindings");
		return false;
	}
	TSet<FString> BindingIds;
	TSet<FString> Imports;
	for (int32 Index = 0; Index < Bindings->Num(); ++Index)
	{
		FAvidScriptBindingFunctionModel Binding;
		const bool bParsedBinding = ParseAvidScriptBindingFunction(
				(*Bindings)[Index].IsValid() ? (*Bindings)[Index]->AsObject() : nullptr,
				OutPackage.SchemaVersion,
				Binding,
				OutErrorSource);
		FString ExpectedGeneratedImport;
		FString GeneratedSemanticIdentity;
		if (bParsedBinding
			&& Binding.DispatchMode == TEXT("generated_native_s1"))
		{
			const FString GeneratedSuffix =
				FAvidScriptBindingDescriptorIdentity::
					MakeFunctionCanonicalIdentity(
						FString(),
						TEXT("generated_native_s1"),
						Binding.GeneratedShape,
						Binding.GeneratedReceiverMode,
						Binding.GeneratedImportName);
			if (Binding.CanonicalIdentity.EndsWith(
					GeneratedSuffix,
					ESearchCase::CaseSensitive))
			{
				GeneratedSemanticIdentity =
					Binding.CanonicalIdentity.LeftChop(
						GeneratedSuffix.Len());
				ExpectedGeneratedImport =
					TEXT("avid_s1_")
					+ FAvidScriptHash::Sha256HexUtf8(
						GeneratedSemanticIdentity).Left(16);
			}
		}
		const bool bGeneratedProperty =
			Binding.DispatchMode == TEXT("generated_native_s1")
			&& (Binding.GeneratedShape == TEXT("property_i32_get_set")
				|| (Binding.BindingKind == TEXT("property_get")
					&& Binding.GeneratedShape == TEXT("property_i32_get"))
				|| (Binding.BindingKind == TEXT("property_set")
					&& Binding.GeneratedShape == TEXT("property_i32_set")))
			&& Binding.GeneratedReceiverMode == TEXT("self_bound");
		const bool bLatentBinding =
			Binding.BindingKind == TEXT("function")
			&& Binding.DispatchMode == TEXT("latent_process_event");
		const bool bAsyncActionBinding =
			Binding.BindingKind == TEXT("function")
			&& Binding.DispatchMode == TEXT("blueprint_async_action");
		const bool bCompletionValid = OutPackage.SchemaVersion < 13
			? Binding.Completion.Mode == TEXT("none")
				&& Binding.Completion.ProviderId.IsEmpty()
				&& Binding.Completion.PayloadTypeId.IsEmpty()
				&& !Binding.Completion.bCancellable
			: !bLatentBinding
				? Binding.Completion.Mode == TEXT("none")
					&& Binding.Completion.ProviderId.IsEmpty()
					&& Binding.Completion.PayloadTypeId.IsEmpty()
					&& !Binding.Completion.bCancellable
				: Binding.Completion.bCancellable
					&& ((Binding.Completion.Mode == TEXT("none")
							&& Binding.Completion.StatusPolicy
								== TEXT("abandon_on_cancel")
							&& Binding.Completion.ProviderId.IsEmpty()
							&& Binding.Completion.PayloadTypeId.IsEmpty())
						|| (Binding.Completion.Mode == TEXT("provider")
							&& Binding.Completion.StatusPolicy
								== (OutPackage.SchemaVersion >= 14
									? TEXT("resume_outcome_on_cancel")
									: TEXT("abandon_on_cancel"))
							&& !Binding.Completion.ProviderId.IsEmpty()
							&& IsAvidScriptBindingLowerSha256(
								Binding.Completion.PayloadTypeId)
							&& TypeIds.Contains(
								Binding.Completion.PayloadTypeId)));
		const bool bNetworked = Binding.Network.IsNetworked();
		const bool bNetworkContractValid = OutPackage.SchemaVersion < 15
			? !bNetworked && !Binding.Network.bReliable
			: !bNetworked
				? !Binding.Network.bReliable
				: Binding.BindingKind == TEXT("function")
					&& Binding.DispatchMode == TEXT("cached_process_event")
					&& !Binding.bStatic
					&& !Binding.bConst
					&& Binding.ReloadEffect
						== EAvidScriptBindingReloadEffect::Unsupported
					&& Binding.ReturnValue.CanonicalType == TEXT("void")
					&& Binding.LatentInfoParameter.IsEmpty()
					&& Binding.WorldContextParameter.IsEmpty()
					&& Binding.Completion.Mode == TEXT("none")
					&& Binding.Parameters.ContainsByPredicate(
						[](const FAvidScriptBindingValueModel& Parameter)
						{
							return Parameter.Direction == TEXT("ref")
								|| Parameter.Direction == TEXT("out");
						}) == false;
		const bool bReplicatedProperty =
			Binding.PropertyReplication.IsReplicated();
		const bool bPropertyReplicationContractValid =
			OutPackage.SchemaVersion < 16
				? !bReplicatedProperty
					&& Binding.PropertyReplication.RepNotifyFunction.IsNone()
				: !bReplicatedProperty
					? Binding.PropertyReplication.RepNotifyFunction.IsNone()
					: !bNetworked
						&& (Binding.BindingKind == TEXT("property_get")
							|| Binding.BindingKind == TEXT("property_set"))
						&& (Binding.PropertyReplication.Mode
								== EAvidScriptBindingPropertyReplicationMode::RepNotify
							? !Binding.PropertyReplication.RepNotifyFunction.IsNone()
							: Binding.PropertyReplication.RepNotifyFunction.IsNone());
		FString ExpectedLatentSignature;
		if (bLatentBinding)
		{
			FString LatentParameters = Binding.bStatic ? FString() : FString(TEXT("ii"));
			for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
			{
				LatentParameters += FString::Join(Parameter.AbiTypes, TEXT(""));
			}
			ExpectedLatentSignature = TEXT("(") + LatentParameters + TEXT("i)I");
		}
		FString ExpectedAsyncActionSignature;
		if (bAsyncActionBinding)
		{
			FString AsyncActionParameters;
			for (const FAvidScriptBindingValueModel& Parameter :
				Binding.Parameters)
			{
				AsyncActionParameters += FString::Join(
					Parameter.AbiTypes,
					TEXT(""));
			}
			ExpectedAsyncActionSignature = TEXT("(")
				+ AsyncActionParameters + TEXT("i)I");
		}
		const FAvidScriptBindingTypeModel* AsyncPayloadType =
			bAsyncActionBinding
				? OutPackage.Types.FindByPredicate(
					[&Binding](const FAvidScriptBindingTypeModel& Type)
					{
						return Type.StableId
							== Binding.AsyncAction.PayloadTypeId;
					})
				: nullptr;
		const bool bAsyncActionContractValid = !bAsyncActionBinding
			? !Binding.AsyncAction.IsEnabled()
				&& Binding.AsyncAction.ActionClass.IsEmpty()
				&& Binding.AsyncAction.PayloadTypeId.IsEmpty()
				&& Binding.AsyncAction.Outcomes.IsEmpty()
			: OutPackage.SchemaVersion >= 23
				&& Binding.AsyncAction.IsEnabled()
				&& Binding.ReturnValue.CanonicalType
					== TEXT("object:")
						+ Binding.AsyncAction.ActionClass
				&& Binding.AsyncAction.ActivationFunction == TEXT("Activate")
				&& Binding.AsyncAction.CompletionPolicy
					== TEXT("first_broadcast_wins")
				&& Binding.AsyncAction.bCancellable
				&& AsyncPayloadType != nullptr
				&& AsyncPayloadType->Kind == TEXT("enum")
				&& AsyncPayloadType->Size == sizeof(int32)
				&& AsyncPayloadType->AbiTypes
					== TArray<FString>{ TEXT("i") }
				&& AsyncPayloadType->EnumValues.Num()
					== Binding.AsyncAction.Outcomes.Num()
				&& Binding.AsyncAction.Outcomes.Num() > 0
				&& Binding.AsyncAction.Outcomes.ContainsByPredicate(
					[AsyncPayloadType](
						const FAvidScriptBindingAsyncActionOutcomeModel& Outcome)
					{
						return !AsyncPayloadType->EnumValues.ContainsByPredicate(
							[&Outcome](
								const FAvidScriptBindingEnumValue& Value)
							{
								return Value.Name == Outcome.DelegateMember
									&& Value.Value == Outcome.Ordinal;
							});
					}) == false;
		FString ExpectedPropertyGetIdentity =
			Binding.OwnerClass
			+ TEXT("::property_get:") + Binding.UeMember
			+ TEXT("(") + Binding.ReturnValue.CanonicalType + TEXT(")");
		FString ExpectedPropertySetIdentity =
			FAvidScriptBindingDescriptorIdentity::MakePropertySetCanonicalIdentity(
				Binding.OwnerClass,
				Binding.UeMember,
				Binding.Parameters.Num() == 1
					? Binding.Parameters[0].CanonicalType
					: FString(),
				Binding.UeFunction);
		if (bReplicatedProperty)
		{
			const FString ReplicationSuffix = TEXT("|property_replication=")
				+ FString(LexToString(Binding.PropertyReplication.Mode))
				+ TEXT("|rep_notify=")
				+ Binding.PropertyReplication.RepNotifyFunction.ToString();
			ExpectedPropertyGetIdentity += ReplicationSuffix;
			ExpectedPropertySetIdentity += ReplicationSuffix;
		}
		if (!bParsedBinding
			|| !bCompletionValid
			|| !bAsyncActionContractValid
			|| !bNetworkContractValid
			|| !bPropertyReplicationContractValid
			|| Binding.Ordinal != Index
			|| (Binding.BindingKind == TEXT("function")
				&& (!FAvidScriptBindingDescriptorIdentity::IsFunctionDispatchModeSupported(
						OutPackage.SchemaVersion,
						Binding.DispatchMode)
					|| Binding.WritePolicy != TEXT("none")
					|| ((Binding.DispatchMode == TEXT("qualified_native_direct"))
						!= Binding.CanonicalIdentity.EndsWith(
							FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
								FString(),
								TEXT("qualified_native_direct")),
							ESearchCase::CaseSensitive))
					|| (bLatentBinding
						&& (Binding.ReloadEffect
								!= EAvidScriptBindingReloadEffect::ContinuationProducer
							|| Binding.bConst
							|| Binding.ReturnValue.CanonicalType != TEXT("void")
							|| Binding.LatentInfoParameter.IsEmpty()
							|| Binding.LatentInfoParameter
								== Binding.WorldContextParameter
							|| !Binding.ScriptName.EndsWith(
								TEXT("Async"),
								ESearchCase::CaseSensitive)
							|| Binding.Parameters.ContainsByPredicate(
								[](const FAvidScriptBindingValueModel& Parameter)
								{
									return Parameter.Direction == TEXT("ref")
										|| Parameter.Direction == TEXT("out");
								})
							|| Binding.HostImport.Signature
								!= ExpectedLatentSignature))
					|| (bAsyncActionBinding
						&& (!Binding.bStatic
							|| Binding.bConst
							|| Binding.ReturnValue.Kind
								!= TEXT("object_handle")
							|| Binding.ReloadEffect
								!= EAvidScriptBindingReloadEffect::ContinuationProducer
							|| !Binding.ScriptName.EndsWith(
								TEXT("Async"),
								ESearchCase::CaseSensitive)
							|| !Binding.LatentInfoParameter.IsEmpty()
							|| !Binding.WorldContextParameter.IsEmpty()
							|| Binding.Parameters.ContainsByPredicate(
								[](const FAvidScriptBindingValueModel& Parameter)
								{
									return Parameter.Direction == TEXT("ref")
										|| Parameter.Direction == TEXT("out");
								})
							|| Binding.HostImport.Signature
								!= ExpectedAsyncActionSignature))
					|| (!bLatentBinding && !bAsyncActionBinding
						&& (!Binding.LatentInfoParameter.IsEmpty()
							|| !Binding.WorldContextParameter.IsEmpty()
							|| Binding.ReloadEffect
								== EAvidScriptBindingReloadEffect::ContinuationProducer))
					|| bGeneratedProperty))
			|| (Binding.BindingKind == TEXT("property_get")
				&& Binding.DispatchMode != TEXT("cached_property_get")
				&& !bGeneratedProperty)
			|| (Binding.BindingKind == TEXT("property_get")
				&& (Binding.bStatic
					|| !Binding.bConst
					|| Binding.WritePolicy != TEXT("none")
					|| Binding.ReloadEffect != EAvidScriptBindingReloadEffect::None
					|| !Binding.Parameters.IsEmpty()
					|| Binding.ReturnValue.CanonicalType == TEXT("void")
					|| Binding.HostImport.Signature
						!= (Binding.GeneratedShape == TEXT("property_i32_get")
							? FString(TEXT("(ii)i"))
							: FString(TEXT("(iii)i")))
					|| (bGeneratedProperty
						&& (Binding.ReturnValue.CanonicalType != TEXT("scalar:i32")
							|| GeneratedSemanticIdentity
								!= ExpectedPropertyGetIdentity))))
			|| (Binding.BindingKind == TEXT("property_set")
				&& (Binding.bStatic
					|| Binding.bConst
					|| Binding.ReloadEffect
						!= (bReplicatedProperty
							? EAvidScriptBindingReloadEffect::Unsupported
							: EAvidScriptBindingReloadEffect::ReflectedProperty)
					|| Binding.ReturnValue.CanonicalType != TEXT("void")
					|| Binding.Parameters.Num() != 1
					|| Binding.Parameters[0].Direction != TEXT("value")
					|| ((Binding.DispatchMode != TEXT("cached_property_set")
							|| Binding.WritePolicy != TEXT("direct")
							|| !Binding.UeFunction.IsEmpty())
						&& (Binding.DispatchMode != TEXT("cached_blueprint_setter")
							|| Binding.WritePolicy != TEXT("blueprint_setter")
							|| Binding.UeFunction.IsEmpty())
						&& (!bGeneratedProperty
							|| Binding.WritePolicy != TEXT("direct")
							|| !Binding.UeFunction.IsEmpty()))
					|| (bReplicatedProperty && bGeneratedProperty)
					|| (bGeneratedProperty
						? (Binding.Parameters[0].CanonicalType
								!= TEXT("scalar:i32")
							|| GeneratedSemanticIdentity
								!= ExpectedPropertySetIdentity)
						: Binding.CanonicalIdentity
							!= ExpectedPropertySetIdentity)))
			|| Binding.StableId != FAvidScriptHash::Sha256HexUtf8(Binding.CanonicalIdentity)
			|| Binding.HostImport.Module != TEXT("avidscript")
			|| (Binding.DispatchMode == TEXT("generated_native_s1")
				? (!IsAvidScriptGeneratedShape(Binding.GeneratedShape)
					|| (Binding.GeneratedReceiverMode != TEXT("self_bound")
						&& Binding.GeneratedReceiverMode != TEXT("stable_borrow"))
					|| Binding.GeneratedImportName
						!= Binding.HostImport.Name
					|| Binding.GeneratedImportName
						!= ExpectedGeneratedImport
					|| !Binding.GeneratedImportName.StartsWith(
						TEXT("avid_s1_"),
						ESearchCase::CaseSensitive)
					|| Binding.GeneratedImportName.Len() != 24
					|| !IsAvidScriptBindingLowerHex(
						Binding.GeneratedImportName.Right(16))
					|| Binding.SemanticFallbackOrdinal != Binding.Ordinal
					|| !Binding.CanonicalIdentity.EndsWith(
						FAvidScriptBindingDescriptorIdentity::
							MakeFunctionCanonicalIdentity(
								FString(),
								TEXT("generated_native_s1"),
								Binding.GeneratedShape,
								Binding.GeneratedReceiverMode,
								Binding.GeneratedImportName),
						ESearchCase::CaseSensitive))
				: Binding.HostImport.Name
					!= TEXT("avid_ue_") + Binding.StableId.Left(16))
			|| BindingIds.Contains(Binding.StableId)
			|| Imports.Contains(Binding.HostImport.Module + TEXT(".") + Binding.HostImport.Name))
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			if (OutErrorSource.IsEmpty())
			{
				OutErrorSource = FString::Printf(TEXT("bindings[%d]"), Index);
			}
			return false;
		}
		if (Binding.BindingKind == TEXT("property_set"))
		{
			const FString ExpectedSignature = TEXT("(ii")
				+ FString::Join(Binding.Parameters[0].AbiTypes, TEXT(""))
				+ TEXT(")i");
			if (Binding.HostImport.Signature != ExpectedSignature)
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = Binding.CanonicalIdentity;
				return false;
			}
		}

		BindingIds.Add(Binding.StableId);
		Imports.Add(Binding.HostImport.Module + TEXT(".") + Binding.HostImport.Name);

		const auto ValidateValueType = [&TypesByCanonical](const FAvidScriptBindingValueModel& ValueModel)
		{
			if (ValueModel.CanonicalType == TEXT("void"))
			{
				return ValueModel.TypeId == FAvidScriptHash::Sha256HexUtf8(TEXT("void"))
					&& ValueModel.Kind == TEXT("void")
					&& ValueModel.CppType == TEXT("void")
					&& ValueModel.AbiTypes.IsEmpty();
			}
			const FAvidScriptBindingTypeModel* ExpectedType = TypesByCanonical.Find(ValueModel.CanonicalType);
			return ExpectedType != nullptr
				&& ExpectedType->StableId == ValueModel.TypeId
				&& ExpectedType->Kind == ValueModel.Kind
				&& ExpectedType->CppType == ValueModel.CppType
				&& ExpectedType->AbiTypes == ValueModel.AbiTypes;
		};
		if (!ValidateValueType(Binding.ReturnValue)
			|| Binding.Parameters.ContainsByPredicate([&ValidateValueType](const FAvidScriptBindingValueModel& Parameter)
			{
				return !ValidateValueType(Parameter);
			}))
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = Binding.CanonicalIdentity;
			return false;
		}
		OutPackage.Bindings.Add(MoveTemp(Binding));
	}

	if (OutPackage.SchemaVersion >= 11)
	{
		const TArray<TSharedPtr<FJsonValue>>* DelegateEvents = nullptr;
		if (!Root->TryGetArrayField(TEXT("delegate_events"), DelegateEvents)
			|| DelegateEvents == nullptr)
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = TEXT("delegate_events");
			return false;
		}

		TSet<FString> StableIds;
		TSet<FString> ScriptNames;
		TSet<FString> ExportNames;
		for (int32 Index = 0; Index < DelegateEvents->Num(); ++Index)
		{
			FAvidScriptBindingDelegateEventModel Event;
			const bool bParsed = ParseAvidScriptBindingDelegateEvent(
				(*DelegateEvents)[Index].IsValid()
					? (*DelegateEvents)[Index]->AsObject()
					: nullptr,
				OutPackage.SchemaVersion,
				Event,
				OutErrorSource);
			const FString ExpectedIdentity =
				FAvidScriptBindingDescriptorIdentity::
					MakeDelegateEventCanonicalIdentity(
						Event.OwnerClass,
						Event.UeMember,
						Event.DelegateKind,
						Event.SourceMode,
						Event.Parameters,
						Event.Network,
						Event.RepNotifyProperty,
						OutPackage.SchemaVersion >= 18
							? Event.HandlerMode
							: FString(),
						OutPackage.SchemaVersion >= 20
							? &Event.ReturnValue
							: nullptr);
			const bool bDelegateCallbackValid =
				(Event.DelegateKind == TEXT("multicast")
					|| Event.DelegateKind == TEXT("singlecast"))
				&& !Event.Network.IsNetworked()
				&& !Event.Network.bReliable
				&& Event.RepNotifyProperty.IsNone();
			const bool bNetworkRpcCallbackValid =
				Event.DelegateKind == TEXT("network_rpc")
				&& Event.Network.IsNetworked()
				&& Event.RepNotifyProperty.IsNone();
			const bool bRepNotifyCallbackValid =
				Event.DelegateKind == TEXT("rep_notify")
				&& !Event.Network.IsNetworked()
				&& !Event.Network.bReliable
				&& !Event.RepNotifyProperty.IsNone();
			const bool bBlueprintEventCallbackValid =
				Event.DelegateKind == TEXT("blueprint_event")
				&& !Event.Network.IsNetworked()
				&& !Event.Network.bReliable
				&& Event.RepNotifyProperty.IsNone();
			const bool bCallbackKindValid = bDelegateCallbackValid
				|| bNetworkRpcCallbackValid
				|| bRepNotifyCallbackValid
				|| bBlueprintEventCallbackValid;
			const bool bHandlerModeValid =
				Event.DelegateKind == TEXT("multicast")
					|| Event.DelegateKind == TEXT("singlecast")
					? Event.HandlerMode == TEXT("replace")
					: Event.HandlerMode == TEXT("replace")
						|| Event.HandlerMode == TEXT("before")
						|| Event.HandlerMode == TEXT("after");
			if (!bParsed
				|| Event.Ordinal != Index
				|| !bCallbackKindValid
				|| !bHandlerModeValid
				|| (OutPackage.SchemaVersion < 17
					&& Event.DelegateKind != TEXT("multicast"))
				|| (OutPackage.SchemaVersion < 20
					&& Event.DelegateKind == TEXT("singlecast"))
				|| (OutPackage.SchemaVersion < 22
					&& Event.DelegateKind == TEXT("blueprint_event"))
				|| (Event.DelegateKind == TEXT("blueprint_event")
					&& (Event.ReturnValue.Kind != TEXT("void")
						|| Event.Parameters.ContainsByPredicate(
							[](const FAvidScriptBindingValueModel& Parameter)
							{
								return Parameter.Direction == TEXT("ref")
									|| Parameter.Direction == TEXT("out");
							})))
				|| Event.SourceMode != TEXT("self")
				|| !IsAvidScriptBindingIdentifier(Event.ScriptName)
				|| Event.CanonicalIdentity != ExpectedIdentity
				|| Event.StableId
					!= FAvidScriptHash::Sha256HexUtf8(ExpectedIdentity)
				|| Event.ExportName
					!= TEXT("avid_on_delegate_") + Event.StableId.Left(16)
				|| StableIds.Contains(Event.StableId)
				|| ScriptNames.Contains(Event.ScriptName)
				|| ExportNames.Contains(Event.ExportName))
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = FString::Printf(
					TEXT("delegate_events[%d]"),
					Index);
				return false;
			}

			for (const FAvidScriptBindingValueModel& Parameter :
				Event.Parameters)
			{
				const FAvidScriptBindingTypeModel* Type =
					TypesByCanonical.Find(Parameter.CanonicalType);
				if (Type == nullptr
					|| Type->StableId != Parameter.TypeId
					|| Type->Kind != Parameter.Kind
					|| Type->CppType != Parameter.CppType
					|| Type->AbiTypes != Parameter.AbiTypes)
				{
					OutErrorCategory = TEXT("descriptor_contract_invalid");
					OutErrorSource = Event.CanonicalIdentity;
					return false;
				}
			}
			if (OutPackage.SchemaVersion >= 20)
			{
				const bool bVoidReturn =
					Event.ReturnValue.CanonicalType == TEXT("void")
					&& Event.ReturnValue.TypeId
						== FAvidScriptHash::Sha256HexUtf8(TEXT("void"))
					&& Event.ReturnValue.Kind == TEXT("void")
					&& Event.ReturnValue.CppType == TEXT("void")
					&& Event.ReturnValue.AbiTypes.IsEmpty();
				const FAvidScriptBindingTypeModel* ReturnType = bVoidReturn
					? nullptr
					: TypesByCanonical.Find(
						Event.ReturnValue.CanonicalType);
				if (!bVoidReturn
					&& (ReturnType == nullptr
						|| ReturnType->StableId != Event.ReturnValue.TypeId
						|| ReturnType->Kind != Event.ReturnValue.Kind
						|| ReturnType->CppType != Event.ReturnValue.CppType
						|| ReturnType->AbiTypes
							!= Event.ReturnValue.AbiTypes))
				{
					OutErrorCategory = TEXT("descriptor_contract_invalid");
					OutErrorSource = Event.CanonicalIdentity + TEXT(":return");
					return false;
				}
			}
			StableIds.Add(Event.StableId);
			ScriptNames.Add(Event.ScriptName);
			ExportNames.Add(Event.ExportName);
			OutPackage.DelegateEvents.Add(MoveTemp(Event));
		}
	}

	if (OutPackage.SchemaVersion >= 5)
	{
		const TArray<TSharedPtr<FJsonValue>>* ClassReferences = nullptr;
		if (!Root->TryGetArrayField(TEXT("class_references"), ClassReferences)
			|| ClassReferences == nullptr)
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = TEXT("class_references");
			return false;
		}

		TSet<FString> StableIds;
		TSet<FString> ScriptNames;
		TSet<FString> Identities;
		FString PreviousStableId;
		for (int32 Index = 0; Index < ClassReferences->Num(); ++Index)
		{
			FAvidScriptBindingClassReferenceModel Reference;
			const TSharedPtr<FJsonObject> ReferenceObject = (*ClassReferences)[Index].IsValid()
				&& (*ClassReferences)[Index]->Type == EJson::Object
				? (*ClassReferences)[Index]->AsObject()
				: nullptr;
			if (!ParseAvidScriptBindingClassReference(
					ReferenceObject,
					OutPackage.SchemaVersion,
					Reference,
					OutErrorSource)
				|| Reference.Ordinal != Index
				|| StableIds.Contains(Reference.StableId)
				|| ScriptNames.Contains(Reference.ScriptName)
				|| (!PreviousStableId.IsEmpty()
					&& PreviousStableId.Compare(Reference.StableId, ESearchCase::CaseSensitive) >= 0))
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = TEXT("class_references");
				return false;
			}
			const FString Identity = FAvidScriptBindingDescriptorIdentity::MakeClassReferenceIdentity(
				Reference.ClassPath,
				Reference.BaseClassPath,
				Reference.LoadPolicy);
			if (Identities.Contains(Identity))
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = TEXT("class_references");
				return false;
			}
			PreviousStableId = Reference.StableId;
			StableIds.Add(Reference.StableId);
			ScriptNames.Add(Reference.ScriptName);
			Identities.Add(Identity);
			OutPackage.ClassReferences.Add(MoveTemp(Reference));
		}
	}
	if (OutPackage.SchemaVersion >= 7)
	{
		const TArray<TSharedPtr<FJsonValue>>* ObjectFactories = nullptr;
		if (!Root->TryGetArrayField(TEXT("object_factories"), ObjectFactories)
			|| ObjectFactories == nullptr)
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = TEXT("object_factories");
			return false;
		}

		TSet<FString> StableIds;
		TSet<FString> ScriptNames;
		FString PreviousStableId;
		for (int32 Index = 0; Index < ObjectFactories->Num(); ++Index)
		{
			FAvidScriptBindingObjectFactoryModel Factory;
			const TSharedPtr<FJsonObject> FactoryObject =
				(*ObjectFactories)[Index].IsValid()
				&& (*ObjectFactories)[Index]->Type == EJson::Object
				? (*ObjectFactories)[Index]->AsObject()
				: nullptr;
			if (!ParseAvidScriptBindingObjectFactory(
					FactoryObject,
					Factory,
					OutErrorSource)
				|| Factory.Ordinal != Index
				|| StableIds.Contains(Factory.StableId)
				|| ScriptNames.Contains(Factory.ScriptName)
				|| (!PreviousStableId.IsEmpty()
					&& PreviousStableId.Compare(
						Factory.StableId,
						ESearchCase::CaseSensitive) >= 0))
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = TEXT("object_factories");
				return false;
			}
			PreviousStableId = Factory.StableId;
			StableIds.Add(Factory.StableId);
			ScriptNames.Add(Factory.ScriptName);
			OutPackage.ObjectFactories.Add(MoveTemp(Factory));
		}
	}
	if (OutPackage.Bindings.IsEmpty()
		&& OutPackage.DelegateEvents.IsEmpty()
		&& OutPackage.ClassReferences.IsEmpty())
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		OutErrorSource = TEXT("bindings|class_references");
		return false;
	}
	if (!ValidateAvidScriptBindingV6ObjectTypes(OutPackage, OutErrorSource))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		if (OutErrorSource.IsEmpty())
		{
			OutErrorSource = TEXT("object_type_graph");
		}
		return false;
	}
	if (!ValidateAvidScriptBindingV7ObjectFactories(OutPackage, OutErrorSource))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		if (OutErrorSource.IsEmpty())
		{
			OutErrorSource = TEXT("object_factories");
		}
		return false;
	}
	return true;
}
