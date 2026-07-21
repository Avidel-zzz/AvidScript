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

FAvidScriptProjectedBindingType MakeVoidType()
{
	FAvidScriptProjectedBindingType Type;
	Type.CanonicalType = TEXT("void");
	Type.Kind = TEXT("void");
	Type.CppType = TEXT("void");
	Type.bVoid = true;
	return Type;
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
} // namespace

FAvidScriptProjectedBindingType FAvidScriptEditorReflectedTypePolicy::MakeObjectType(const UClass* ObjectClass)
{
	FAvidScriptProjectedBindingType Type;
	Type.CanonicalType = TEXT("object:") + ObjectClass->GetPathName();
	Type.Kind = TEXT("object_handle");
	Type.CppType = ObjectClass->GetPrefixCPP() + ObjectClass->GetName();
	Type.Size = 8;
	Type.Alignment = 4;
	Type.AbiValueTypes = { TEXT("i"), TEXT("i") };
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
		if (!ProjectProperty(ReturnProperty, OutProjection.ReturnValue, OutErrorSource))
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
		if (!ProjectProperty(Property, Value, OutErrorSource))
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
	if (Property == nullptr || !ProjectProperty(Property, OutValue, OutErrorSource))
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
	FString& OutErrorSource)
{
	OutValue = FAvidScriptProjectedBindingValue();
	OutValue.Name = Property->GetName();
	OutValue.Direction = GetPropertyDirection(Property);

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

		OutErrorSource = Type.CppType;
		return false;
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
