#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
FAvidScriptProjectedBindingType MakeScalarType(
	const TCHAR* CanonicalType,
	const TCHAR* CppType,
	const TCHAR* WasmType,
	int32 Size,
	int32 Alignment)
{
	FAvidScriptProjectedBindingType Type;
	Type.CanonicalType = CanonicalType;
	Type.Kind = TEXT("scalar");
	Type.CppType = CppType;
	Type.Size = Size;
	Type.Alignment = Alignment;
	Type.AbiValueTypes.Add(WasmType);
	return Type;
}

FAvidScriptProjectedBindingType MakeEnumType(const FProperty* Property, const UEnum* Enum)
{
	FAvidScriptProjectedBindingType Type;
	Type.CanonicalType = TEXT("enum:") + Enum->GetPathName();
	Type.Kind = TEXT("enum");
	Type.CppType = Property->GetCPPType();
	Type.Size = 4;
	Type.Alignment = 4;
	Type.AbiValueTypes.Add(TEXT("i"));
	for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
	{
		const FString Name = Enum->GetNameStringByIndex(Index);
		if (Name.IsEmpty()
			|| Enum->HasMetaData(TEXT("Hidden"), Index)
			|| Name.EndsWith(TEXT("_MAX"), ESearchCase::CaseSensitive))
		{
			continue;
		}
		Type.EnumValues.Add({ Name, Enum->GetValueByIndex(Index) });
	}
	return Type;
}

bool ProjectScalarProperty(const FProperty* Property, FAvidScriptProjectedBindingType& OutType)
{
	if (Property->IsA<FBoolProperty>())
	{
		OutType = MakeScalarType(TEXT("scalar:bool"), TEXT("bool"), TEXT("i"), 4, 4);
		return true;
	}
	if (Property->IsA<FFloatProperty>())
	{
		OutType = MakeScalarType(TEXT("scalar:f32"), TEXT("float"), TEXT("f"), 4, 4);
		return true;
	}
	if (Property->IsA<FDoubleProperty>())
	{
		OutType = MakeScalarType(TEXT("scalar:f64"), TEXT("double"), TEXT("F"), 8, 8);
		return true;
	}
	if (Property->IsA<FInt64Property>())
	{
		OutType = MakeScalarType(TEXT("scalar:i64"), TEXT("int64"), TEXT("I"), 8, 8);
		return true;
	}
	if (Property->IsA<FUInt64Property>())
	{
		OutType = MakeScalarType(TEXT("scalar:u64"), TEXT("uint64"), TEXT("I"), 8, 8);
		return true;
	}
	if (Property->IsA<FIntProperty>())
	{
		OutType = MakeScalarType(TEXT("scalar:i32"), TEXT("int32"), TEXT("i"), 4, 4);
		return true;
	}
	if (Property->IsA<FUInt32Property>())
	{
		OutType = MakeScalarType(TEXT("scalar:u32"), TEXT("uint32"), TEXT("i"), 4, 4);
		return true;
	}
	if (Property->IsA<FInt16Property>())
	{
		OutType = MakeScalarType(TEXT("scalar:i16"), TEXT("int16"), TEXT("i"), 2, 2);
		return true;
	}
	if (Property->IsA<FUInt16Property>())
	{
		OutType = MakeScalarType(TEXT("scalar:u16"), TEXT("uint16"), TEXT("i"), 2, 2);
		return true;
	}
	if (Property->IsA<FInt8Property>())
	{
		OutType = MakeScalarType(TEXT("scalar:i8"), TEXT("int8"), TEXT("i"), 1, 1);
		return true;
	}
	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (ByteProperty->Enum != nullptr)
		{
			OutType = MakeEnumType(Property, ByteProperty->Enum);
		}
		else
		{
			OutType = MakeScalarType(TEXT("scalar:u8"), TEXT("uint8"), TEXT("i"), 1, 1);
		}
		return true;
	}
	return false;
}

FString GetPropertyDirection(const FProperty* Property)
{
	if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
	{
		return TEXT("return");
	}
	const bool bReference = Property->HasAnyPropertyFlags(CPF_ReferenceParm);
	const bool bOut = Property->HasAnyPropertyFlags(CPF_OutParm);
	const bool bConst = Property->HasAnyPropertyFlags(CPF_ConstParm);
	if (bReference && bOut)
	{
		return bConst ? TEXT("const_ref") : TEXT("ref");
	}
	if (bOut)
	{
		return TEXT("out");
	}
	if (bReference)
	{
		return bConst ? TEXT("const_ref") : TEXT("ref");
	}
	return TEXT("value");
}

void AppendAbiTypes(const FAvidScriptProjectedBindingValue& Value, TArray<FString>& OutTypes)
{
	if (Value.Direction == TEXT("out") || Value.Direction == TEXT("ref"))
	{
		OutTypes.Add(TEXT("i"));
		return;
	}
	OutTypes.Append(Value.Type.AbiValueTypes);
}

void FinalizeProjectedType(FAvidScriptProjectedBindingType& Type)
{
	Type.StableId = Type.Kind == TEXT("struct_wire")
		? FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
			Type.CanonicalType,
			Type.EnumValues,
			Type.StructFields,
			Type.Size,
			Type.Alignment)
		: FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
			Type.CanonicalType,
			Type.EnumValues,
			Type.StructFields);
}

bool IsUnsafeStructField(const FProperty* Property, const UScriptStruct* Struct)
{
	return Property == nullptr
		|| Struct == nullptr
		|| Property->ArrayDim != 1
		|| Property->GetOwnerStruct() == nullptr
		|| !Struct->IsChildOf(Property->GetOwnerStruct())
		|| !Property->HasAnyPropertyFlags(CPF_BlueprintVisible)
		|| Property->HasAnyPropertyFlags(
			CPF_Transient | CPF_EditorOnly | CPF_InstancedReference
				| CPF_ContainsInstancedReference)
		|| Property->IsA<FNameProperty>()
		|| Property->IsA<FStrProperty>()
		|| Property->IsA<FTextProperty>()
		|| Property->IsA<FArrayProperty>()
		|| Property->IsA<FSetProperty>()
		|| Property->IsA<FMapProperty>()
		|| Property->IsA<FDelegateProperty>()
		|| Property->IsA<FMulticastDelegateProperty>()
		|| Property->IsA<FSoftObjectProperty>()
		|| Property->IsA<FWeakObjectProperty>()
		|| Property->IsA<FLazyObjectProperty>();
}

int32 AlignStructWireOffset(const int32 Offset, const int32 Alignment)
{
	return Alignment > 1
		? ((Offset + Alignment - 1) / Alignment) * Alignment
		: Offset;
}
} // namespace

FAvidScriptProjectedBindingType FAvidScriptEditorReflectedTypePolicy::MakeVoidType()
{
	FAvidScriptProjectedBindingType Type;
	Type.CanonicalType = TEXT("void");
	Type.Kind = TEXT("void");
	Type.CppType = TEXT("void");
	Type.bVoid = true;
	return Type;
}

FAvidScriptProjectedBindingType FAvidScriptEditorReflectedTypePolicy::MakeObjectType(const UClass* ObjectClass)
{
	FAvidScriptProjectedBindingType Type;
	Type.CanonicalType = TEXT("object:") + ObjectClass->GetPathName();
	Type.Kind = TEXT("object_handle");
	Type.CppType = ObjectClass->GetPrefixCPP() + ObjectClass->GetName();
	Type.Size = 8;
	Type.Alignment = 4;
	Type.AbiValueTypes = { TEXT("i"), TEXT("i") };
	Type.ObjectClass = ObjectClass;
	return Type;
}

bool FAvidScriptEditorReflectedTypePolicy::ProjectFunction(
	const UFunction* Function,
	bool bIsStatic,
	FAvidScriptProjectedFunction& OutProjection,
	FString& OutErrorSource)
{
	OutProjection = FAvidScriptProjectedFunction();
	OutErrorSource.Empty();

	const FProperty* ReturnProperty = Function->GetReturnProperty();
	if (ReturnProperty != nullptr)
	{
		int32 StructNodes = 0;
		TSet<const UScriptStruct*> ActiveStructs;
		if (!ProjectProperty(
				ReturnProperty,
				OutProjection.ReturnValue,
				OutErrorSource,
				0,
				StructNodes,
				ActiveStructs))
		{
			return false;
		}
	}
	else
	{
		OutProjection.ReturnValue.Name = TEXT("return");
		OutProjection.ReturnValue.Direction = TEXT("return");
		OutProjection.ReturnValue.Type = MakeVoidType();
	}

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		const FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Parm) || Property->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			continue;
		}

		FAvidScriptProjectedBindingValue Value;
		int32 StructNodes = 0;
		TSet<const UScriptStruct*> ActiveStructs;
		if (!ProjectProperty(
				Property,
				Value,
				OutErrorSource,
				0,
				StructNodes,
				ActiveStructs))
		{
			return false;
		}
		const FString DefaultMetadataKey = TEXT("CPP_Default_") + Property->GetName();
		if (Function->HasMetaData(*DefaultMetadataKey))
		{
			const FString ReflectedDefaultValue = Function->GetMetaData(*DefaultMetadataKey);
			if (!ReflectedDefaultValue.IsEmpty())
			{
				Value.bHasDefaultValue = true;
				Value.DefaultValue = ReflectedDefaultValue;
			}
		}
		OutProjection.Parameters.Add(MoveTemp(Value));
	}

	TArray<FString> AbiTypes;
	if (!bIsStatic)
	{
		AbiTypes.Add(TEXT("i"));
		AbiTypes.Add(TEXT("i"));
	}
	for (const FAvidScriptProjectedBindingValue& Parameter : OutProjection.Parameters)
	{
		AppendAbiTypes(Parameter, AbiTypes);
	}
	if (!OutProjection.ReturnValue.Type.bVoid)
	{
		AbiTypes.Add(TEXT("i"));
	}
	OutProjection.AbiSignature = TEXT("(") + FString::Join(AbiTypes, TEXT("")) + TEXT(")i");
	return true;
}

bool FAvidScriptEditorReflectedTypePolicy::ProjectReadableProperty(
	const FProperty* Property,
	FAvidScriptProjectedBindingValue& OutValue,
	FString& OutErrorSource)
{
	if (Property == nullptr)
	{
		return false;
	}
	if (Property->IsA<FNameProperty>())
	{
		OutErrorSource = TEXT("FName:return");
		return false;
	}
	int32 StructNodes = 0;
	TSet<const UScriptStruct*> ActiveStructs;
	if (!ProjectProperty(
			Property,
			OutValue,
			OutErrorSource,
			0,
			StructNodes,
			ActiveStructs))
	{
		return false;
	}
	OutValue.Name = TEXT("return");
	OutValue.Direction = TEXT("return");
	return true;
}

bool FAvidScriptEditorReflectedTypePolicy::ProjectProperty(
	const FProperty* Property,
	FAvidScriptProjectedBindingValue& OutValue,
	FString& OutErrorSource,
	const int32 StructDepth,
	int32& InOutStructNodes,
	TSet<const UScriptStruct*>& ActiveStructs)
{
	OutValue = FAvidScriptProjectedBindingValue();
	OutValue.Name = Property->GetName();
	OutValue.Direction = GetPropertyDirection(Property);
	if (Property->ArrayDim != 1)
	{
		OutErrorSource = FString::Printf(
			TEXT("fixed_array:%s[%d]"),
			*Property->GetName(),
			Property->ArrayDim);
		return false;
	}

	if (Property->IsA<FNameProperty>())
	{
		if (OutValue.Direction != TEXT("value") && OutValue.Direction != TEXT("const_ref"))
		{
			OutErrorSource = TEXT("FName:") + OutValue.Direction;
			return false;
		}
		OutValue.Type.CanonicalType = TEXT("name:fname");
		OutValue.Type.Kind = TEXT("name_utf8");
		OutValue.Type.CppType = TEXT("FName");
		OutValue.Type.Size = 4;
		OutValue.Type.Alignment = 4;
		OutValue.Type.AbiValueTypes = { TEXT("i") };
		return true;
	}

	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		OutValue.Type = MakeObjectType(ObjectProperty->PropertyClass);
		return true;
	}

	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		FAvidScriptProjectedBindingType Type;
		Type.CanonicalType = TEXT("struct:") + StructProperty->Struct->GetPathName();
		Type.Kind = TEXT("struct");
		Type.CppType = StructProperty->Struct->GetStructCPPName();
		Type.Alignment = 4;
		if (StructProperty->Struct == TBaseStructure<FVector>::Get()
			|| StructProperty->Struct == TBaseStructure<FRotator>::Get())
		{
			Type.Size = 12;
			Type.AbiValueTypes = { TEXT("f"), TEXT("f"), TEXT("f") };
			OutValue.Type = MoveTemp(Type);
			return true;
		}
		if (StructProperty->Struct == TBaseStructure<FTransform>::Get())
		{
			Type.Size = 36;
			Type.AbiValueTypes = {
				TEXT("f"), TEXT("f"), TEXT("f"),
				TEXT("f"), TEXT("f"), TEXT("f"),
				TEXT("f"), TEXT("f"), TEXT("f")
			};
			OutValue.Type = MoveTemp(Type);
			return true;
		}
		if (StructProperty->Struct == nullptr
			|| StructDepth >= 8
			|| ActiveStructs.Contains(StructProperty->Struct))
		{
			OutErrorSource = Type.CppType;
			return false;
		}

		ActiveStructs.Add(StructProperty->Struct);
		Type.CanonicalType = TEXT("struct_wire:") + StructProperty->Struct->GetPathName();
		Type.Kind = TEXT("struct_wire");
		Type.AbiValueTypes = { TEXT("i") };
		Type.Alignment = 1;
		int32 WireOffset = 0;
		for (TFieldIterator<FProperty> It(
			StructProperty->Struct,
			EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* FieldProperty = *It;
			if (FieldProperty != nullptr && FieldProperty->ArrayDim != 1)
			{
				ActiveStructs.Remove(StructProperty->Struct);
				OutErrorSource = FString::Printf(
					TEXT("%s.%s:fixed_array:%s[%d]"),
					*StructProperty->Struct->GetPathName(),
					*FieldProperty->GetName(),
					*FieldProperty->GetName(),
					FieldProperty->ArrayDim);
				return false;
			}
			if (IsUnsafeStructField(FieldProperty, StructProperty->Struct)
				|| ++InOutStructNodes > 128)
			{
				ActiveStructs.Remove(StructProperty->Struct);
				OutErrorSource = StructProperty->Struct->GetPathName()
					+ TEXT(".") + (FieldProperty == nullptr ? TEXT("<invalid>") : FieldProperty->GetName());
				return false;
			}

			FAvidScriptProjectedBindingValue FieldValue;
			if (!ProjectProperty(
					FieldProperty,
					FieldValue,
					OutErrorSource,
					StructDepth + 1,
					InOutStructNodes,
					ActiveStructs))
			{
				ActiveStructs.Remove(StructProperty->Struct);
				OutErrorSource = StructProperty->Struct->GetPathName() + TEXT(".")
					+ FieldProperty->GetName() + TEXT(":") + OutErrorSource;
				return false;
			}
			if (FieldValue.Type.Kind == TEXT("name_utf8") || FieldValue.Type.bVoid
				|| FieldValue.Type.Size <= 0 || FieldValue.Type.Alignment <= 0)
			{
				ActiveStructs.Remove(StructProperty->Struct);
				OutErrorSource = StructProperty->Struct->GetPathName() + TEXT(".") + FieldProperty->GetName();
				return false;
			}
			WireOffset = AlignStructWireOffset(WireOffset, FieldValue.Type.Alignment);
			if (WireOffset < 0 || FieldValue.Type.Size > 4096 - WireOffset)
			{
				ActiveStructs.Remove(StructProperty->Struct);
				OutErrorSource = StructProperty->Struct->GetPathName();
				return false;
			}
			FinalizeProjectedType(FieldValue.Type);
			Type.StructFields.Add({ FieldProperty->GetName(), FieldValue.Type.StableId, WireOffset });
			Type.StructFieldTypes.Add(MakeShared<FAvidScriptProjectedBindingType>(MoveTemp(FieldValue.Type)));
			WireOffset += Type.StructFieldTypes.Last()->Size;
			Type.Alignment = FMath::Max(Type.Alignment, Type.StructFieldTypes.Last()->Alignment);
		}
		ActiveStructs.Remove(StructProperty->Struct);
		Type.Size = AlignStructWireOffset(WireOffset, Type.Alignment);
		if (Type.StructFields.IsEmpty() || Type.Size <= 0 || Type.Size > 4096)
		{
			OutErrorSource = Type.CppType;
			return false;
		}
		FinalizeProjectedType(Type);
		OutValue.Type = MoveTemp(Type);
		return true;
	}

	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		OutValue.Type = MakeEnumType(Property, EnumProperty->GetEnum());
		return true;
	}

	if (ProjectScalarProperty(Property, OutValue.Type))
	{
		return true;
	}

	OutErrorSource = Property->GetCPPType();
	return false;
}
