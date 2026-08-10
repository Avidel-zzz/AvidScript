#include "AvidScriptBindingDescriptor.h"

#include "AvidScriptHash.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
				: FString())
		|| (OutType.Kind == TEXT("enum")) != OutType.CanonicalType.StartsWith(TEXT("enum:"))
		|| (OutType.Kind == TEXT("struct_wire")) != OutType.CanonicalType.StartsWith(TEXT("struct_wire:"))
		|| (OutType.Kind == TEXT("array")) != OutType.CanonicalType.StartsWith(TEXT("array:tarray<"))
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
	for (const FAvidScriptBindingTypeModel& Type : Types)
	{
		if (Type.Kind != TEXT("array"))
		{
			continue;
		}
		const FAvidScriptBindingTypeModel* Element = TypesById.FindRef(Type.ElementTypeId);
		if (Element == nullptr
			|| Element->Kind == TEXT("array")
			|| Element->Kind == TEXT("name_utf8")
			|| Element->Kind == TEXT("string_utf8")
			|| Element->Kind == TEXT("void")
			|| Element->Size <= 0
			|| Element->Alignment <= 0
			|| Element->Alignment > 16
			|| Type.Size != 4
			|| Type.Alignment != 4
			|| Type.AbiTypes != TArray<FString>({ TEXT("i") }))
		{
			OutErrorSource = Type.StableId + TEXT(":element_type_id");
			return false;
		}
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
	const FString& ElementTypeId)
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
	return Identity;
}

FString FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues,
	const TArray<FAvidScriptBindingStructFieldModel>& StructFields,
	const int32 WireSize,
	const int32 WireAlignment,
	const FString& ElementTypeId)
{
	return FAvidScriptHash::Sha256HexUtf8(MakeTypeIdentity(
		CanonicalType,
		EnumValues,
		StructFields,
		WireSize,
		WireAlignment,
		ElementTypeId));
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
				|| DispatchMode == TEXT("generated_native_s1")));
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

FString FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(
	const FAvidScriptBindingPackageModel& Package)
{
	TArray<FString> SelectionKeys;
	SelectionKeys.Reserve(Package.Bindings.Num());
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
		SelectionKeys.Add(MoveTemp(Key));
	}
	SelectionKeys.Sort([](const FString& Left, const FString& Right)
	{
		return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
	});

	if (Package.SchemaVersion < 5)
	{
		return FAvidScriptHash::Sha256HexUtf8(FString::Join(SelectionKeys, TEXT("\n")));
	}

	FString Identity(Package.SchemaVersion >= 9
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

	FString Identity(Package.SchemaVersion >= 9
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
			&& OutPackage.SchemaVersion != 10)
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
				&& OutPackage.SchemaVersion != 10)
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
		const FString ExpectedPropertyGetIdentity =
			Binding.OwnerClass
			+ TEXT("::property_get:") + Binding.UeMember
			+ TEXT("(") + Binding.ReturnValue.CanonicalType + TEXT(")");
		const FString ExpectedPropertySetIdentity =
			FAvidScriptBindingDescriptorIdentity::MakePropertySetCanonicalIdentity(
				Binding.OwnerClass,
				Binding.UeMember,
				Binding.Parameters.Num() == 1
					? Binding.Parameters[0].CanonicalType
					: FString(),
				Binding.UeFunction);
		if (!bParsedBinding
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
					|| Binding.ReloadEffect != EAvidScriptBindingReloadEffect::ReflectedProperty
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
	if (OutPackage.Bindings.IsEmpty() && OutPackage.ClassReferences.IsEmpty())
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
