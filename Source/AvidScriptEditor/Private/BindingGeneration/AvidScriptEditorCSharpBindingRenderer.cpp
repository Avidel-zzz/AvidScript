#include "BindingGeneration/AvidScriptEditorCSharpBindingRenderer.h"

#include "BindingGeneration/AvidScriptEditorCSharpBindingArtifact.h"
#include "BindingGeneration/AvidScriptEditorCSharpDefaultValueFormatter.h"
#include "BindingGeneration/AvidScriptEditorCSharpObjectFactoryRenderer.h"
#include "BindingGeneration/AvidScriptEditorCSharpStateContractRenderer.h"
#include "BindingGeneration/AvidScriptEditorCSharpSyntax.h"
#include "AvidScriptObjectFactoryBinding.h"
#include "AvidScriptObjectLifecycleBinding.h"
#include "AvidScriptObjectTypeBinding.h"
#include "AvidScriptSceneAttachmentBinding.h"
#include "Serialization/JsonWriter.h"

namespace BindingArtifact = AvidScriptCSharpBindingArtifact;

namespace
{
struct FCSharpComponent
{
	FString Name;
	FString CSharpType;
	FString Access;
};

struct FCSharpRenderedMethod
{
	TArray<FString> MethodLines;
	TArray<FString> NativeLines;
	FString SignatureKey;
};

FString EscapeCSharpString(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	return Escaped;
}

FString MakeNativeMethodName(int32 Ordinal)
{
	return FString::Printf(TEXT("Invoke%04d"), Ordinal);
}

const FAvidScriptBindingTypeModel* FindRenderedType(
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
	const FString& CanonicalType)
{
	const FAvidScriptBindingTypeModel* const* Type = TypesByCanonical.Find(CanonicalType);
	return Type == nullptr ? nullptr : *Type;
}

const FAvidScriptBindingTypeModel* FindRenderedTypeById(
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesById,
	const FString& TypeId)
{
	const FAvidScriptBindingTypeModel* const* Type = TypesById.Find(TypeId);
	return Type == nullptr ? nullptr : *Type;
}

bool IsExactUtf8StringDescriptor(
	const FAvidScriptBindingValueModel& Value,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
	const TCHAR* CanonicalType,
	const TCHAR* Kind,
	const TCHAR* CppType)
{
	const FAvidScriptBindingTypeModel* Type = FindRenderedType(TypesByCanonical, CanonicalType);
	return Value.CanonicalType == CanonicalType
		&& Value.Kind == Kind
		&& Value.CppType == CppType
		&& Value.AbiTypes == TArray<FString>{ TEXT("i") }
		&& Type != nullptr
		&& Type->CanonicalType == CanonicalType
		&& Type->Kind == Kind
		&& Type->CppType == CppType
		&& Type->Size == 4
		&& Type->Alignment == 4
		&& Type->AbiTypes == TArray<FString>{ TEXT("i") };
}

bool ValidateBindingValueDirections(
	const FAvidScriptBindingFunctionModel& Binding,
	FString& OutErrorSource)
{
	for (int32 ParameterIndex = 0; ParameterIndex < Binding.Parameters.Num(); ++ParameterIndex)
	{
		const FString& Direction = Binding.Parameters[ParameterIndex].Direction;
		if (Direction != TEXT("value")
			&& Direction != TEXT("const_ref")
			&& Direction != TEXT("ref")
			&& Direction != TEXT("out"))
		{
			OutErrorSource = FString::Printf(
				TEXT("%s.parameters[%d].direction"),
				*Binding.CanonicalIdentity,
				ParameterIndex);
			return false;
		}
	}

	if (Binding.ReturnValue.Direction != TEXT("return"))
	{
		OutErrorSource = Binding.CanonicalIdentity + TEXT(".return.direction");
		return false;
	}
	return true;
}

bool IsKnownExpandedStruct(const FAvidScriptBindingTypeModel& Type)
{
	return Type.Kind == TEXT("struct")
		&& (Type.CppType == TEXT("FVector")
			|| Type.CppType == TEXT("FRotator")
			|| Type.CppType == TEXT("FTransform"));
}

bool ResolveCSharpType(
	const FAvidScriptBindingTypeModel& Type,
	FString& OutType,
	FString& OutErrorSource)
{
	if (Type.CanonicalType == TEXT("scalar:bool")) { OutType = TEXT("bool"); return Type.Kind == TEXT("scalar"); }
	if (Type.CanonicalType == TEXT("scalar:f32")) { OutType = TEXT("float"); return Type.Kind == TEXT("scalar"); }
	if (Type.CanonicalType == TEXT("scalar:f64")) { OutType = TEXT("double"); return Type.Kind == TEXT("scalar"); }
	if (Type.CanonicalType == TEXT("scalar:i8")) { OutType = TEXT("sbyte"); return Type.Kind == TEXT("scalar"); }
	if (Type.CanonicalType == TEXT("scalar:u8")) { OutType = TEXT("byte"); return Type.Kind == TEXT("scalar"); }
	if (Type.CanonicalType == TEXT("scalar:i16")) { OutType = TEXT("short"); return Type.Kind == TEXT("scalar"); }
	if (Type.CanonicalType == TEXT("scalar:u16")) { OutType = TEXT("ushort"); return Type.Kind == TEXT("scalar"); }
	if (Type.CanonicalType == TEXT("scalar:i32")) { OutType = TEXT("int"); return Type.Kind == TEXT("scalar"); }
	if (Type.CanonicalType == TEXT("scalar:u32")) { OutType = TEXT("uint"); return Type.Kind == TEXT("scalar"); }
	if (Type.CanonicalType == TEXT("scalar:i64")) { OutType = TEXT("long"); return Type.Kind == TEXT("scalar"); }
	if (Type.CanonicalType == TEXT("scalar:u64")) { OutType = TEXT("ulong"); return Type.Kind == TEXT("scalar"); }
	if (Type.Kind == TEXT("enum") || Type.Kind == TEXT("object_handle") || IsKnownExpandedStruct(Type))
	{
		OutType = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Type.CppType);
		return !OutType.IsEmpty();
	}
	if (Type.Kind == TEXT("struct_wire")
		&& Type.CanonicalType.StartsWith(TEXT("struct_wire:"))
		&& Type.AbiTypes == TArray<FString>{ TEXT("i") })
	{
		OutType = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Type.CppType);
		return !OutType.IsEmpty();
	}
	OutErrorSource = Type.StableId.IsEmpty() ? Type.CanonicalType : Type.StableId;
	return false;
}

bool ResolveCSharpType(
	const FAvidScriptBindingValueModel& Value,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesById,
	FString& OutType,
	FString& OutErrorSource)
{
	if (Value.CanonicalType == TEXT("void"))
	{
		OutType = TEXT("void");
		return true;
	}
	if (Value.CanonicalType == TEXT("scalar:bool")) { OutType = TEXT("bool"); return true; }
	if (Value.CanonicalType == TEXT("scalar:f32")) { OutType = TEXT("float"); return true; }
	if (Value.CanonicalType == TEXT("scalar:f64")) { OutType = TEXT("double"); return true; }
	if (Value.CanonicalType == TEXT("scalar:i8")) { OutType = TEXT("sbyte"); return true; }
	if (Value.CanonicalType == TEXT("scalar:u8")) { OutType = TEXT("byte"); return true; }
	if (Value.CanonicalType == TEXT("scalar:i16")) { OutType = TEXT("short"); return true; }
	if (Value.CanonicalType == TEXT("scalar:u16")) { OutType = TEXT("ushort"); return true; }
	if (Value.CanonicalType == TEXT("scalar:i32")) { OutType = TEXT("int"); return true; }
	if (Value.CanonicalType == TEXT("scalar:u32")) { OutType = TEXT("uint"); return true; }
	if (Value.CanonicalType == TEXT("scalar:i64")) { OutType = TEXT("long"); return true; }
	if (Value.CanonicalType == TEXT("scalar:u64")) { OutType = TEXT("ulong"); return true; }
	if (Value.CanonicalType == TEXT("name:fname"))
	{
		if (!IsExactUtf8StringDescriptor(
				Value,
				TypesByCanonical,
				TEXT("name:fname"),
				TEXT("name_utf8"),
				TEXT("FName")))
		{
			OutErrorSource = Value.CanonicalType;
			return false;
		}
		OutType = TEXT("string");
		return true;
	}
	if (Value.CanonicalType == TEXT("string:fstring"))
	{
		if (!IsExactUtf8StringDescriptor(
				Value,
				TypesByCanonical,
				TEXT("string:fstring"),
				TEXT("string_utf8"),
				TEXT("FString")))
		{
			OutErrorSource = Value.CanonicalType;
			return false;
		}
		OutType = TEXT("string");
		return true;
	}
	if (Value.Kind == TEXT("struct_wire"))
	{
		const FAvidScriptBindingTypeModel* Type = FindRenderedTypeById(TypesById, Value.TypeId);
		if (Type == nullptr
			|| Type->CanonicalType != Value.CanonicalType
			|| Type->Kind != Value.Kind
			|| Type->CppType != Value.CppType
			|| Type->AbiTypes != Value.AbiTypes)
		{
			OutErrorSource = Value.TypeId;
			return false;
		}
		return ResolveCSharpType(*Type, OutType, OutErrorSource);
	}
	if (Value.Kind == TEXT("enum") || Value.Kind == TEXT("object_handle") || Value.Kind == TEXT("struct"))
	{
		OutType = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Value.CppType);
		return true;
	}
	OutErrorSource = Value.CanonicalType;
	return false;
}

bool ResolveStorageType(
	const FAvidScriptBindingValueModel& Value,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesById,
	FString& OutType,
	FString& OutErrorSource)
{
	if (Value.Kind == TEXT("object_handle"))
	{
		OutType = TEXT("FAvidScriptObjectHandle");
		return true;
	}
	if (Value.Kind == TEXT("enum") || Value.CanonicalType == TEXT("scalar:bool"))
	{
		OutType = TEXT("int");
		return true;
	}
	return ResolveCSharpType(Value, TypesByCanonical, TypesById, OutType, OutErrorSource);
}

bool ResolveComponents(
	const FAvidScriptBindingValueModel& Value,
	TArray<FCSharpComponent>& OutComponents,
	FString& OutErrorSource)
{
	OutComponents.Reset();
	if (Value.Kind != TEXT("struct"))
	{
		return false;
	}
	if (Value.CppType == TEXT("FVector"))
	{
		OutComponents = {
			{ TEXT("X"), TEXT("float"), TEXT(".X") },
			{ TEXT("Y"), TEXT("float"), TEXT(".Y") },
			{ TEXT("Z"), TEXT("float"), TEXT(".Z") }
		};
		return true;
	}
	if (Value.CppType == TEXT("FRotator"))
	{
		OutComponents = {
			{ TEXT("Pitch"), TEXT("float"), TEXT(".Pitch") },
			{ TEXT("Yaw"), TEXT("float"), TEXT(".Yaw") },
			{ TEXT("Roll"), TEXT("float"), TEXT(".Roll") }
		};
		return true;
	}
	if (Value.CppType == TEXT("FTransform"))
	{
		OutComponents = {
			{ TEXT("TranslationX"), TEXT("float"), TEXT(".Translation.X") },
			{ TEXT("TranslationY"), TEXT("float"), TEXT(".Translation.Y") },
			{ TEXT("TranslationZ"), TEXT("float"), TEXT(".Translation.Z") },
			{ TEXT("RotationPitch"), TEXT("float"), TEXT(".Rotation.Pitch") },
			{ TEXT("RotationYaw"), TEXT("float"), TEXT(".Rotation.Yaw") },
			{ TEXT("RotationRoll"), TEXT("float"), TEXT(".Rotation.Roll") },
			{ TEXT("ScaleX"), TEXT("float"), TEXT(".Scale3D.X") },
			{ TEXT("ScaleY"), TEXT("float"), TEXT(".Scale3D.Y") },
			{ TEXT("ScaleZ"), TEXT("float"), TEXT(".Scale3D.Z") }
		};
		return true;
	}
	OutErrorSource = Value.CanonicalType;
	return false;
}

FString ConvertFromStorage(const FAvidScriptBindingValueModel& Value, const FString& StorageExpression)
{
	if (Value.Kind == TEXT("object_handle"))
	{
		return FString::Printf(
			TEXT("new %s(%s.Slot, %s.Generation)"),
			*FAvidScriptEditorCSharpSyntax::MakeIdentifier(Value.CppType),
			*StorageExpression,
			*StorageExpression);
	}
	if (Value.Kind == TEXT("enum"))
	{
		return FString::Printf(TEXT("(%s)%s"), *FAvidScriptEditorCSharpSyntax::MakeIdentifier(Value.CppType), *StorageExpression);
	}
	if (Value.CanonicalType == TEXT("scalar:bool"))
	{
		return StorageExpression + TEXT(" != 0");
	}
	return StorageExpression;
}

FString ConvertToStorage(const FAvidScriptBindingValueModel& Value, const FString& PublicExpression)
{
	if (Value.Kind == TEXT("object_handle"))
	{
		return FString::Printf(
			TEXT("new FAvidScriptObjectHandle(%s.AvidScriptSlot, %s.AvidScriptGeneration)"),
			*PublicExpression,
			*PublicExpression);
	}
	if (Value.Kind == TEXT("enum"))
	{
		return TEXT("(int)") + PublicExpression;
	}
	if (Value.CanonicalType == TEXT("scalar:bool"))
	{
		return PublicExpression + TEXT(" ? 1 : 0");
	}
	return PublicExpression;
}

FString MakeExpectedAbiSignature(const FAvidScriptBindingFunctionModel& Binding)
{
	if (Binding.DispatchMode == TEXT("generated_native_s1"))
	{
		if (Binding.GeneratedShape == TEXT("i32_pair_to_i32"))
		{
			return TEXT("(iiii)i");
		}
		if (Binding.GeneratedShape == TEXT("property_i32_get_set")
			|| Binding.GeneratedShape == TEXT("vector_value"))
		{
			return TEXT("(iii)i");
		}
		if (Binding.GeneratedShape == TEXT("property_i32_get"))
		{
			return TEXT("(ii)i");
		}
		if (Binding.GeneratedShape == TEXT("property_i32_set"))
		{
			return TEXT("(iii)i");
		}
		if (Binding.GeneratedShape == TEXT("stable_object_roundtrip"))
		{
			return TEXT("(iiiii)i");
		}
	}

	FString Parameters;
	if (!Binding.bStatic)
	{
		Parameters += TEXT("ii");
	}
	for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
	{
		if (Parameter.Direction == TEXT("ref") || Parameter.Direction == TEXT("out"))
		{
			Parameters += TEXT("i");
		}
		else
		{
			Parameters += FString::Join(Parameter.AbiTypes, TEXT(""));
		}
	}
	if (Binding.ReturnValue.CanonicalType != TEXT("void"))
	{
		Parameters += TEXT("i");
	}
	return TEXT("(") + Parameters + TEXT(")i");
}

bool RenderMethod(
	const FAvidScriptBindingFunctionModel& Binding,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesById,
	FCSharpRenderedMethod& OutMethod,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	if (MakeExpectedAbiSignature(Binding) != Binding.HostImport.Signature)
	{
		OutErrorCategory = TEXT("abi_signature_mismatch");
		OutErrorSource = Binding.CanonicalIdentity;
		return false;
	}

	FString ReturnType;
	if (!ResolveCSharpType(Binding.ReturnValue, TypesByCanonical, TypesById, ReturnType, OutErrorSource))
	{
		OutErrorCategory = TEXT("unsupported_csharp_type");
		return false;
	}
	const bool bGeneratedI32Pair =
		Binding.DispatchMode == TEXT("generated_native_s1")
		&& Binding.GeneratedShape == TEXT("i32_pair_to_i32");
	const bool bGeneratedVectorValue =
		Binding.DispatchMode == TEXT("generated_native_s1")
		&& Binding.GeneratedShape == TEXT("vector_value");

	TArray<FString> PublicParameters;
	TArray<FString> NativeParameters;
	TArray<FString> NativeArguments;
	TArray<FString> BeforeCall;
	TArray<FString> AfterCall;
	TArray<FString> SignatureParameterTypes;
	bool bAllFollowingDefaultable = true;
	TArray<bool> EmitDefault;
	EmitDefault.SetNumZeroed(Binding.Parameters.Num());
	for (int32 Index = Binding.Parameters.Num() - 1; Index >= 0; --Index)
	{
		FString DefaultExpression;
		const bool bCanEmit = FAvidScriptEditorCSharpDefaultValueFormatter::TryFormat(Binding.Parameters[Index], TypesByCanonical, DefaultExpression);
		bAllFollowingDefaultable = bCanEmit && bAllFollowingDefaultable;
		EmitDefault[Index] = bAllFollowingDefaultable;
	}

	for (int32 ParameterIndex = 0; ParameterIndex < Binding.Parameters.Num(); ++ParameterIndex)
	{
		const FAvidScriptBindingValueModel& Parameter = Binding.Parameters[ParameterIndex];
		FString PublicType;
		if (!ResolveCSharpType(Parameter, TypesByCanonical, TypesById, PublicType, OutErrorSource))
		{
			OutErrorCategory = TEXT("unsupported_csharp_type");
			return false;
		}
		const FString PublicName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Parameter.Name);
		FString Modifier;
		if (Parameter.Direction == TEXT("out")) { Modifier = TEXT("out "); }
		else if (Parameter.Direction == TEXT("ref")) { Modifier = TEXT("ref "); }
		FString PublicDeclaration = Modifier + PublicType + TEXT(" ") + PublicName;
		if (EmitDefault[ParameterIndex])
		{
			FString DefaultExpression;
			FAvidScriptEditorCSharpDefaultValueFormatter::TryFormat(Parameter, TypesByCanonical, DefaultExpression);
			PublicDeclaration += TEXT(" = ") + DefaultExpression;
		}
		PublicParameters.Add(PublicDeclaration);
		SignatureParameterTypes.Add(Modifier + PublicType);

		if (bGeneratedVectorValue)
		{
			if (ParameterIndex != 0
				|| Binding.Parameters.Num() != 1
				|| PublicType != TEXT("FVector"))
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = Binding.CanonicalIdentity;
				return false;
			}
			NativeParameters.Add(
				TEXT("ref FAvidScriptVectorValueBuffer value"));
			NativeArguments.Add(TEXT("ref __vectorValue"));
			BeforeCall.Add(
				TEXT("FAvidScriptVectorValueBuffer __vectorValue = new(")
				+ PublicName + TEXT(");"));
			continue;
		}

		if (Parameter.Direction == TEXT("ref") || Parameter.Direction == TEXT("out"))
		{
			FString StorageType;
			if (!ResolveStorageType(Parameter, TypesByCanonical, TypesById, StorageType, OutErrorSource))
			{
				OutErrorCategory = TEXT("unsupported_csharp_type");
				return false;
			}
			const FString NativeName = FString::Printf(TEXT("p%d_%s"), ParameterIndex, *PublicName.Replace(TEXT("@"), TEXT("")));
			NativeParameters.Add(Modifier + StorageType + TEXT(" ") + NativeName);
			const bool bDirect = StorageType == PublicType;
			if (bDirect)
			{
				NativeArguments.Add(Modifier + PublicName);
			}
			else
			{
				const FString LocalName = TEXT("__") + NativeName;
				if (Parameter.Direction == TEXT("ref"))
				{
					BeforeCall.Add(StorageType + TEXT(" ") + LocalName + TEXT(" = ") + ConvertToStorage(Parameter, PublicName) + TEXT(";"));
				}
				else
				{
					BeforeCall.Add(StorageType + TEXT(" ") + LocalName + TEXT(";"));
				}
				NativeArguments.Add(Modifier + LocalName);
				AfterCall.Add(PublicName + TEXT(" = ") + ConvertFromStorage(Parameter, LocalName) + TEXT(";"));
			}
			continue;
		}

		if (Parameter.Kind == TEXT("object_handle"))
		{
			NativeParameters.Add(FString::Printf(TEXT("int p%dSlot"), ParameterIndex));
			NativeParameters.Add(FString::Printf(TEXT("int p%dGeneration"), ParameterIndex));
			NativeArguments.Add(PublicName + TEXT(".AvidScriptSlot"));
			NativeArguments.Add(PublicName + TEXT(".AvidScriptGeneration"));
			continue;
		}
		if (Parameter.Kind == TEXT("struct"))
		{
			TArray<FCSharpComponent> Components;
			if (!ResolveComponents(Parameter, Components, OutErrorSource)
				|| Components.Num() != Parameter.AbiTypes.Num())
			{
				OutErrorCategory = TEXT("unsupported_csharp_type");
				return false;
			}
			for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
			{
				const FCSharpComponent& Component = Components[ComponentIndex];
				NativeParameters.Add(FString::Printf(
					TEXT("%s p%d%s"),
					*Component.CSharpType,
					ParameterIndex,
					*Component.Name));
				NativeArguments.Add(PublicName + Component.Access);
			}
			continue;
		}
		if (Parameter.Kind == TEXT("struct_wire"))
		{
			NativeParameters.Add(FString::Printf(TEXT("in %s p%d_%s"), *PublicType, ParameterIndex, *PublicName.Replace(TEXT("@"), TEXT(""))));
			NativeArguments.Add(TEXT("in ") + PublicName);
			continue;
		}

		FString StorageType;
		if (!ResolveStorageType(Parameter, TypesByCanonical, TypesById, StorageType, OutErrorSource))
		{
			OutErrorCategory = TEXT("unsupported_csharp_type");
			return false;
		}
		NativeParameters.Add(FString::Printf(TEXT("%s p%d"), *StorageType, ParameterIndex));
		NativeArguments.Add(ConvertToStorage(Parameter, PublicName));
	}

	if (!Binding.bStatic)
	{
		NativeParameters.Insert(TEXT("int selfGeneration"), 0);
		NativeParameters.Insert(TEXT("int selfSlot"), 0);
		NativeArguments.Insert(TEXT("this.Generation"), 0);
		NativeArguments.Insert(TEXT("this.Slot"), 0);
	}

	if (Binding.ReturnValue.CanonicalType != TEXT("void")
		&& !bGeneratedI32Pair
		&& !bGeneratedVectorValue)
	{
		FString StorageType;
		if (!ResolveStorageType(Binding.ReturnValue, TypesByCanonical, TypesById, StorageType, OutErrorSource))
		{
			OutErrorCategory = TEXT("unsupported_csharp_type");
			return false;
		}
		NativeParameters.Add(TEXT("out ") + StorageType + TEXT(" returnValue"));
		BeforeCall.Add(StorageType + TEXT(" __returnValue;"));
		NativeArguments.Add(TEXT("out __returnValue"));
	}

	const FString MethodName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Binding.ScriptName);
	const FString StaticModifier = Binding.bStatic ? TEXT("static ") : FString();
	OutMethod.MethodLines.Add(FString::Printf(
		TEXT("    public %s%s %s(%s)"),
		*StaticModifier,
		*ReturnType,
		*MethodName,
		*FString::Join(PublicParameters, TEXT(", "))));
	OutMethod.MethodLines.Add(TEXT("    {"));
	for (const FString& Line : BeforeCall)
	{
		OutMethod.MethodLines.Add(TEXT("        ") + Line);
	}
	if (bGeneratedI32Pair)
	{
		OutMethod.MethodLines.Add(FString::Printf(
			TEXT("        return AvidScriptNative.%s(%s);"),
			*MakeNativeMethodName(Binding.Ordinal),
			*FString::Join(NativeArguments, TEXT(", "))));
	}
	else
	{
		OutMethod.MethodLines.Add(FString::Printf(
			TEXT("        _ = AvidScriptNative.%s(%s);"),
			*MakeNativeMethodName(Binding.Ordinal),
			*FString::Join(NativeArguments, TEXT(", "))));
	}
	for (const FString& Line : AfterCall)
	{
		OutMethod.MethodLines.Add(TEXT("        ") + Line);
	}
	if (bGeneratedVectorValue)
	{
		OutMethod.MethodLines.Add(TEXT("        return __vectorValue.Result;"));
	}
	else if (Binding.ReturnValue.CanonicalType != TEXT("void")
		&& !bGeneratedI32Pair)
	{
		OutMethod.MethodLines.Add(TEXT("        return ") + ConvertFromStorage(Binding.ReturnValue, TEXT("__returnValue")) + TEXT(";"));
	}
	OutMethod.MethodLines.Add(TEXT("    }"));

	OutMethod.NativeLines.Add(FString::Printf(
		TEXT("    [DllImport(\"%s\", EntryPoint = \"%s\")]"),
		*EscapeCSharpString(Binding.HostImport.Module),
		*EscapeCSharpString(Binding.HostImport.Name)));
	OutMethod.NativeLines.Add(FString::Printf(
		TEXT("    internal static extern int %s(%s);"),
		*MakeNativeMethodName(Binding.Ordinal),
		*FString::Join(NativeParameters, TEXT(", "))));
	OutMethod.SignatureKey = MethodName + TEXT("(") + FString::Join(SignatureParameterTypes, TEXT(",")) + TEXT(")");
	return true;
}

bool BuildPropertySetterInterop(
	const FAvidScriptBindingFunctionModel& Setter,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesById,
	FString& OutPublicType,
	TArray<FString>& OutNativeParameters,
	TArray<FString>& OutNativeArguments,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	if (Setter.BindingKind != TEXT("property_set")
		|| Setter.bStatic
		|| Setter.bConst
		|| Setter.ReturnValue.CanonicalType != TEXT("void")
		|| Setter.Parameters.Num() != 1
		|| Setter.Parameters[0].Direction != TEXT("value")
		|| MakeExpectedAbiSignature(Setter) != Setter.HostImport.Signature)
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		OutErrorSource = Setter.CanonicalIdentity;
		return false;
	}
	if (!ResolveCSharpType(
			Setter.Parameters[0],
			TypesByCanonical,
			TypesById,
			OutPublicType,
			OutErrorSource))
	{
		OutErrorCategory = TEXT("unsupported_csharp_type");
		return false;
	}

	const FAvidScriptBindingValueModel& Value = Setter.Parameters[0];
	OutNativeParameters = { TEXT("int selfSlot"), TEXT("int selfGeneration") };
	OutNativeArguments = { TEXT("this.Slot"), TEXT("this.Generation") };
	if (Value.Kind == TEXT("object_handle"))
	{
		OutNativeParameters.Add(TEXT("int valueSlot"));
		OutNativeParameters.Add(TEXT("int valueGeneration"));
		OutNativeArguments.Add(TEXT("value.AvidScriptSlot"));
		OutNativeArguments.Add(TEXT("value.AvidScriptGeneration"));
	}
	else if (Value.Kind == TEXT("struct"))
	{
		TArray<FCSharpComponent> Components;
		if (!ResolveComponents(Value, Components, OutErrorSource)
			|| Components.Num() != Value.AbiTypes.Num())
		{
			OutErrorCategory = TEXT("unsupported_csharp_type");
			return false;
		}
		for (const FCSharpComponent& Component : Components)
		{
			OutNativeParameters.Add(
				Component.CSharpType + TEXT(" value") + Component.Name);
			OutNativeArguments.Add(TEXT("value") + Component.Access);
		}
	}
	else if (Value.Kind == TEXT("struct_wire"))
	{
		OutNativeParameters.Add(TEXT("in ") + OutPublicType + TEXT(" value"));
		OutNativeArguments.Add(TEXT("in value"));
	}
	else
	{
		FString StorageType;
		if (!ResolveStorageType(
				Value,
				TypesByCanonical,
				TypesById,
				StorageType,
				OutErrorSource))
		{
			OutErrorCategory = TEXT("unsupported_csharp_type");
			return false;
		}
		OutNativeParameters.Add(StorageType + TEXT(" value"));
		OutNativeArguments.Add(ConvertToStorage(Value, TEXT("value")));
	}
	return true;
}

void AppendPropertySetterInterop(
	const FAvidScriptBindingFunctionModel& Setter,
	const TArray<FString>& NativeParameters,
	const TArray<FString>& NativeArguments,
	FCSharpRenderedMethod& OutMethod)
{
	const bool bBufferedGeneratedI32 =
		Setter.DispatchMode == TEXT("generated_native_s1")
		&& (Setter.GeneratedShape == TEXT("property_i32_set")
			|| Setter.GeneratedShape == TEXT("property_i32_get_set"))
		&& Setter.Parameters.Num() == 1
		&& Setter.Parameters[0].CanonicalType == TEXT("scalar:i32");
	if (bBufferedGeneratedI32)
	{
		OutMethod.MethodLines.Add(FString::Printf(
			TEXT("        [AvidScriptDataLane(\"buffered_write\", %d)]"),
			Setter.Ordinal));
	}
	OutMethod.MethodLines.Append({
		TEXT("        set"),
		TEXT("        {"),
		FString::Printf(
			TEXT("            _ = AvidScriptNative.%s(%s);"),
			*MakeNativeMethodName(Setter.Ordinal),
			*FString::Join(NativeArguments, TEXT(", "))),
		TEXT("        }")
	});
	if (bBufferedGeneratedI32)
	{
		OutMethod.NativeLines.Add(FString::Printf(
			TEXT("    [AvidScriptDataLane(\"buffered_write\", %d)]"),
			Setter.Ordinal));
	}
	OutMethod.NativeLines.Append({
		FString::Printf(
			TEXT("    [DllImport(\"%s\", EntryPoint = \"%s\")]"),
			*EscapeCSharpString(Setter.HostImport.Module),
			*EscapeCSharpString(Setter.HostImport.Name)),
		FString::Printf(
			TEXT("    internal static extern int %s(%s);"),
			*MakeNativeMethodName(Setter.Ordinal),
			*FString::Join(NativeParameters, TEXT(", ")))
	});
}

bool RenderPropertyGetter(
	const FAvidScriptBindingFunctionModel& Binding,
	const FAvidScriptBindingFunctionModel* Setter,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesById,
	FCSharpRenderedMethod& OutMethod,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	if (Binding.BindingKind != TEXT("property_get")
		|| Binding.bStatic
		|| !Binding.Parameters.IsEmpty()
		|| MakeExpectedAbiSignature(Binding) != Binding.HostImport.Signature)
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		OutErrorSource = Binding.CanonicalIdentity;
		return false;
	}
	FString PublicType;
	FString StorageType;
	if (!ResolveCSharpType(Binding.ReturnValue, TypesByCanonical, TypesById, PublicType, OutErrorSource)
		|| !ResolveStorageType(Binding.ReturnValue, TypesByCanonical, TypesById, StorageType, OutErrorSource))
	{
		OutErrorCategory = TEXT("unsupported_csharp_type");
		return false;
	}

	const FString PropertyName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Binding.ScriptName);
	const bool bDirectGeneratedI32Getter =
		Binding.DispatchMode == TEXT("generated_native_s1")
		&& Binding.GeneratedShape == TEXT("property_i32_get")
		&& Binding.ReturnValue.CanonicalType == TEXT("scalar:i32")
		&& StorageType == TEXT("int");
	OutMethod.MethodLines.Append({
		FString::Printf(TEXT("    public %s %s"), *PublicType, *PropertyName),
		TEXT("    {"),
		TEXT("        get"),
		TEXT("        {")
	});
	if (bDirectGeneratedI32Getter)
	{
		OutMethod.MethodLines.Add(FString::Printf(
			TEXT("            return AvidScriptNative.%s(this.Slot, this.Generation);"),
			*MakeNativeMethodName(Binding.Ordinal)));
		OutMethod.MethodLines.Add(TEXT("        }"));
		OutMethod.NativeLines.Append({
			FString::Printf(
				TEXT("    [DllImport(\"%s\", EntryPoint = \"%s\")]"),
				*EscapeCSharpString(Binding.HostImport.Module),
				*EscapeCSharpString(Binding.HostImport.Name)),
			FString::Printf(
				TEXT("    internal static extern int %s(int selfSlot, int selfGeneration);"),
				*MakeNativeMethodName(Binding.Ordinal))
		});
	}
	else
	{
		OutMethod.MethodLines.Append({
			FString::Printf(TEXT("            %s __returnValue;"), *StorageType),
			FString::Printf(
				TEXT("            _ = AvidScriptNative.%s(this.Slot, this.Generation, out __returnValue);"),
				*MakeNativeMethodName(Binding.Ordinal)),
			TEXT("            return ") + ConvertFromStorage(Binding.ReturnValue, TEXT("__returnValue")) + TEXT(";"),
			TEXT("        }")
		});
		OutMethod.NativeLines.Append({
			FString::Printf(
				TEXT("    [DllImport(\"%s\", EntryPoint = \"%s\")]"),
				*EscapeCSharpString(Binding.HostImport.Module),
				*EscapeCSharpString(Binding.HostImport.Name)),
			FString::Printf(
				TEXT("    internal static extern int %s(int selfSlot, int selfGeneration, out %s returnValue);"),
				*MakeNativeMethodName(Binding.Ordinal),
				*StorageType)
		});
	}

	if (Setter != nullptr)
	{
		if (Setter->OwnerClass != Binding.OwnerClass
			|| Setter->UeMember != Binding.UeMember
			|| Setter->ScriptName != Binding.ScriptName
			|| Setter->Parameters.Num() != 1
			|| Setter->Parameters[0].CanonicalType
				!= Binding.ReturnValue.CanonicalType)
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = Setter->CanonicalIdentity;
			return false;
		}
		FString SetterPublicType;
		TArray<FString> NativeParameters;
		TArray<FString> NativeArguments;
		if (!BuildPropertySetterInterop(
				*Setter,
				TypesByCanonical,
				TypesById,
				SetterPublicType,
				NativeParameters,
				NativeArguments,
				OutErrorCategory,
				OutErrorSource))
		{
			return false;
		}
		if (SetterPublicType != PublicType)
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = Setter->CanonicalIdentity;
			return false;
		}
		AppendPropertySetterInterop(
			*Setter,
			NativeParameters,
			NativeArguments,
			OutMethod);
	}
	OutMethod.MethodLines.Add(TEXT("    }"));
	OutMethod.SignatureKey = PropertyName;
	return true;
}

bool RenderPropertySetter(
	const FAvidScriptBindingFunctionModel& Setter,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesByCanonical,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesById,
	FCSharpRenderedMethod& OutMethod,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	FString PublicType;
	TArray<FString> NativeParameters;
	TArray<FString> NativeArguments;
	if (!BuildPropertySetterInterop(
			Setter,
			TypesByCanonical,
			TypesById,
			PublicType,
			NativeParameters,
			NativeArguments,
			OutErrorCategory,
			OutErrorSource))
	{
		return false;
	}

	const FString PropertyName =
		FAvidScriptEditorCSharpSyntax::MakeIdentifier(Setter.ScriptName);
	OutMethod.MethodLines.Append({
		FString::Printf(TEXT("    public %s %s"), *PublicType, *PropertyName),
		TEXT("    {")
	});
	AppendPropertySetterInterop(
		Setter,
		NativeParameters,
		NativeArguments,
		OutMethod);
	OutMethod.MethodLines.Add(TEXT("    }"));
	OutMethod.SignatureKey = PropertyName;
	return true;
}

void AppendVector(TArray<FString>& Lines)
{
	Lines.Append({
		TEXT("[StructLayout(LayoutKind.Sequential)]"),
		TEXT("public readonly struct FVector"),
		TEXT("{"),
		TEXT("    public readonly float X;"),
		TEXT("    public readonly float Y;"),
		TEXT("    public readonly float Z;"),
		TEXT(""),
		TEXT("    public FVector(float x, float y, float z)"),
		TEXT("    {"),
		TEXT("        X = x;"),
		TEXT("        Y = y;"),
		TEXT("        Z = z;"),
		TEXT("    }"),
		TEXT(""),
		TEXT("    public static FVector Zero => new(0.0f, 0.0f, 0.0f);"),
		TEXT("    public static FVector operator +(FVector left, FVector right)"),
		TEXT("        => new(left.X + right.X, left.Y + right.Y, left.Z + right.Z);"),
		TEXT("}"),
		TEXT("")
	});
}

void AppendGeneratedVectorValueBuffer(TArray<FString>& Lines)
{
	Lines.Append({
		TEXT("[StructLayout(LayoutKind.Sequential)]"),
		TEXT("internal struct FAvidScriptVectorValueBuffer"),
		TEXT("{"),
		TEXT("    internal float InputX;"),
		TEXT("    internal float InputY;"),
		TEXT("    internal float InputZ;"),
		TEXT("    internal float OutputX;"),
		TEXT("    internal float OutputY;"),
		TEXT("    internal float OutputZ;"),
		TEXT(""),
		TEXT("    internal FAvidScriptVectorValueBuffer(FVector value)"),
		TEXT("    {"),
		TEXT("        InputX = value.X;"),
		TEXT("        InputY = value.Y;"),
		TEXT("        InputZ = value.Z;"),
		TEXT("        OutputX = 0.0f;"),
		TEXT("        OutputY = 0.0f;"),
		TEXT("        OutputZ = 0.0f;"),
		TEXT("    }"),
		TEXT(""),
		TEXT("    internal FVector Result"),
		TEXT("        => new(OutputX, OutputY, OutputZ);"),
		TEXT("}"),
		TEXT("")
	});
}

void AppendInputEvent(TArray<FString>& Lines)
{
	Lines.Append({
		TEXT("[StructLayout(LayoutKind.Sequential)]"),
		TEXT("public readonly struct InputEvent"),
		TEXT("{"),
		TEXT("    public readonly int ActionId;"),
		TEXT("    public readonly int TriggerEvent;"),
		TEXT("    public readonly FVector Value;"),
		TEXT(""),
		TEXT("    internal InputEvent(int actionId, int triggerEvent, FVector value)"),
		TEXT("    {"),
		TEXT("        ActionId = actionId;"),
		TEXT("        TriggerEvent = triggerEvent;"),
		TEXT("        Value = value;"),
		TEXT("    }"),
		TEXT("}"),
		TEXT("")
	});
}

void AppendRotator(TArray<FString>& Lines)
{
	Lines.Append({
		TEXT("[StructLayout(LayoutKind.Sequential)]"),
		TEXT("public readonly struct FRotator"),
		TEXT("{"),
		TEXT("    public readonly float Pitch;"),
		TEXT("    public readonly float Yaw;"),
		TEXT("    public readonly float Roll;"),
		TEXT(""),
		TEXT("    public FRotator(float pitch, float yaw, float roll)"),
		TEXT("    {"),
		TEXT("        Pitch = pitch;"),
		TEXT("        Yaw = yaw;"),
		TEXT("        Roll = roll;"),
		TEXT("    }"),
		TEXT(""),
		TEXT("    public static FRotator Zero => new(0.0f, 0.0f, 0.0f);"),
		TEXT("    public static FRotator operator +(FRotator left, FRotator right)"),
		TEXT("        => new(left.Pitch + right.Pitch, left.Yaw + right.Yaw, left.Roll + right.Roll);"),
		TEXT("}"),
		TEXT("")
	});
}

void AppendTransform(TArray<FString>& Lines)
{
	Lines.Append({
		TEXT("[StructLayout(LayoutKind.Sequential)]"),
		TEXT("public readonly struct FTransform"),
		TEXT("{"),
		TEXT("    public readonly FVector Translation;"),
		TEXT("    public readonly FRotator Rotation;"),
		TEXT("    public readonly FVector Scale3D;"),
		TEXT(""),
		TEXT("    public FTransform(FVector translation, FRotator rotation, FVector scale3D)"),
		TEXT("    {"),
		TEXT("        Translation = translation;"),
		TEXT("        Rotation = rotation;"),
		TEXT("        Scale3D = scale3D;"),
		TEXT("    }"),
		TEXT(""),
		TEXT("    public static FTransform Identity => new(FVector.Zero, FRotator.Zero, new FVector(1.0f, 1.0f, 1.0f));"),
		TEXT("}"),
		TEXT("")
	});
}

bool AppendStructWireDeclarations(
	const FAvidScriptBindingPackageModel& Package,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& TypesById,
	TArray<FString>& Lines,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	TArray<const FAvidScriptBindingTypeModel*> StructWireTypes;
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		if (Type.Kind == TEXT("struct_wire"))
		{
			StructWireTypes.Add(&Type);
		}
	}
	StructWireTypes.Sort([](const FAvidScriptBindingTypeModel& Left, const FAvidScriptBindingTypeModel& Right)
	{
		return Left.CanonicalType < Right.CanonicalType;
	});

	TSet<FString> ActiveTypeIds;
	TSet<FString> EmittedTypeIds;
	TFunction<bool(const FAvidScriptBindingTypeModel*)> AppendType;
	AppendType = [&AppendType, &ActiveTypeIds, &EmittedTypeIds, &TypesById, &Lines, &OutErrorCategory, &OutErrorSource](
		const FAvidScriptBindingTypeModel* Type)
	{
		if (Type == nullptr
			|| Type->Kind != TEXT("struct_wire")
			|| Type->StableId.IsEmpty()
			|| !Type->CanonicalType.StartsWith(TEXT("struct_wire:"))
			|| Type->AbiTypes != TArray<FString>{ TEXT("i") })
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = Type == nullptr ? TEXT("types.fields") : Type->StableId;
			return false;
		}
		if (EmittedTypeIds.Contains(Type->StableId))
		{
			return true;
		}
		if (ActiveTypeIds.Contains(Type->StableId))
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = Type->StableId;
			return false;
		}

		ActiveTypeIds.Add(Type->StableId);
		TArray<FString> FieldTypes;
		TArray<FString> FieldNames;
		TArray<bool> FieldUsesBoolWireStorage;
		TSet<FString> SeenFieldNames;
		for (const FAvidScriptBindingStructFieldModel& Field : Type->StructFields)
		{
			const FAvidScriptBindingTypeModel* ChildType = FindRenderedTypeById(TypesById, Field.TypeId);
			if (ChildType == nullptr)
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = TEXT("types.fields");
				return false;
			}
			FString FieldType;
			if (!ResolveCSharpType(*ChildType, FieldType, OutErrorSource))
			{
				OutErrorCategory = TEXT("unsupported_csharp_type");
				return false;
			}
			const FString FieldName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Field.Name);
			if (FieldName.IsEmpty() || FieldName == FAvidScriptEditorCSharpSyntax::MakeIdentifier(Type->CppType) || SeenFieldNames.Contains(FieldName))
			{
				OutErrorCategory = TEXT("csharp_member_collision");
				OutErrorSource = Type->CanonicalType + TEXT(".") + FieldName;
				return false;
			}
			SeenFieldNames.Add(FieldName);
			if (ChildType->Kind == TEXT("struct_wire") && !AppendType(ChildType))
			{
				return false;
			}
			FieldTypes.Add(MoveTemp(FieldType));
			FieldNames.Add(FieldName);
			FieldUsesBoolWireStorage.Add(
				ChildType->Kind == TEXT("scalar")
				&& ChildType->CanonicalType == TEXT("scalar:bool")
				&& ChildType->Size == 4
				&& ChildType->Alignment == 4);
		}
		TArray<FString> FieldStorageNames;
		FieldStorageNames.SetNum(FieldNames.Num());
		for (int32 FieldIndex = 0; FieldIndex < FieldNames.Num(); ++FieldIndex)
		{
			if (!FieldUsesBoolWireStorage[FieldIndex])
			{
				continue;
			}
			FieldStorageNames[FieldIndex] = FString::Printf(
				TEXT("__avidscript_bool_%d"),
				FieldIndex);
			if (SeenFieldNames.Contains(FieldStorageNames[FieldIndex]))
			{
				OutErrorCategory = TEXT("csharp_member_collision");
				OutErrorSource = Type->CanonicalType + TEXT(".") + FieldStorageNames[FieldIndex];
				return false;
			}
		}

		ActiveTypeIds.Remove(Type->StableId);
		EmittedTypeIds.Add(Type->StableId);
		const FString TypeName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Type->CppType);
		Lines.Add(FString::Printf(TEXT("[StructLayout(LayoutKind.Explicit, Size = %d)]"), Type->Size));
		Lines.Add(TEXT("public readonly struct ") + TypeName);
		Lines.Add(TEXT("{"));
		for (int32 FieldIndex = 0; FieldIndex < Type->StructFields.Num(); ++FieldIndex)
		{
			Lines.Add(FString::Printf(TEXT("    [FieldOffset(%d)]"), Type->StructFields[FieldIndex].WireOffset));
			if (FieldUsesBoolWireStorage[FieldIndex])
			{
				Lines.Add(TEXT("    private readonly int ") + FieldStorageNames[FieldIndex] + TEXT(";"));
				Lines.Add(TEXT("    public bool ") + FieldNames[FieldIndex] + TEXT(" => ") + FieldStorageNames[FieldIndex] + TEXT(" != 0;"));
			}
			else
			{
				Lines.Add(TEXT("    public readonly ") + FieldTypes[FieldIndex] + TEXT(" ") + FieldNames[FieldIndex] + TEXT(";"));
			}
		}
		if (!Type->StructFields.IsEmpty())
		{
			TArray<FString> Parameters;
			for (int32 FieldIndex = 0; FieldIndex < FieldTypes.Num(); ++FieldIndex)
			{
				Parameters.Add(FieldTypes[FieldIndex] + TEXT(" ") + FieldNames[FieldIndex]);
			}
			Lines.Add(TEXT(""));
			Lines.Add(TEXT("    public ") + TypeName + TEXT("(") + FString::Join(Parameters, TEXT(", ")) + TEXT(")"));
			Lines.Add(TEXT("    {"));
			for (int32 FieldIndex = 0; FieldIndex < FieldNames.Num(); ++FieldIndex)
			{
				if (FieldUsesBoolWireStorage[FieldIndex])
				{
					Lines.Add(TEXT("        this.") + FieldStorageNames[FieldIndex] + TEXT(" = ") + FieldNames[FieldIndex] + TEXT(" ? 1 : 0;"));
				}
				else
				{
					Lines.Add(TEXT("        this.") + FieldNames[FieldIndex] + TEXT(" = ") + FieldNames[FieldIndex] + TEXT(";"));
				}
			}
			Lines.Add(TEXT("    }"));
		}
		Lines.Add(TEXT("}"));
		Lines.Add(TEXT(""));
		return true;
	};

	for (const FAvidScriptBindingTypeModel* Type : StructWireTypes)
	{
		if (!AppendType(Type))
		{
			return false;
		}
	}
	return true;
}

void AppendObjectHandleProxy(
	TArray<FString>& Lines,
	const FString& TypeName,
	const FString& DirectBaseTypeName,
	const int32 ObjectTypeOrdinal)
{
	Lines.Add(TEXT("[StructLayout(LayoutKind.Sequential)]"));
	Lines.Add(TEXT("public readonly struct ") + TypeName);
	Lines.Add(TEXT("{"));
	Lines.Add(TEXT("    internal readonly int Slot;"));
	Lines.Add(TEXT("    internal readonly int Generation;"));
	Lines.Add(TEXT(""));
	Lines.Add(FString::Printf(TEXT("    internal %s(int slot, int generation)"), *TypeName));
	Lines.Add(TEXT("    {"));
	Lines.Add(TEXT("        Slot = slot;"));
	Lines.Add(TEXT("        Generation = generation;"));
	Lines.Add(TEXT("    }"));
	Lines.Add(TEXT(""));
	Lines.Add(TEXT("    internal int AvidScriptSlot => Slot;"));
	Lines.Add(TEXT("    internal int AvidScriptGeneration => Generation;"));
	Lines.Add(TEXT("    public bool IsNull => Slot == 0 && Generation == 0;"));
	Lines.Add(TEXT("    public bool HasHandle => Slot > 0 && Generation > 0;"));
	Lines.Add(TEXT("    public bool IsValid => Slot > 0 && Generation > 0;"));
	if (!DirectBaseTypeName.IsEmpty())
	{
		Lines.Append({
			TEXT(""),
			FString::Printf(TEXT("    public static implicit operator %s(%s value)"), *DirectBaseTypeName, *TypeName),
			TEXT("    {"),
			TEXT("        return new(value.Slot, value.Generation);"),
			TEXT("    }"),
			TEXT(""),
			FString::Printf(TEXT("    public static %s TryCast(%s value)"), *TypeName, *DirectBaseTypeName),
			TEXT("    {"),
			FString::Printf(
				TEXT("        if (AvidScriptNative.ObjectTypeIsA(value.Slot, value.Generation, %d) != 0)"),
				ObjectTypeOrdinal),
			TEXT("        {"),
			TEXT("            return new(value.Slot, value.Generation);"),
			TEXT("        }"),
			TEXT("        return default;"),
			TEXT("    }")
		});
	}
}

bool IsAvidScriptActorClassReference(
	const FAvidScriptBindingPackageModel& Package,
	const FAvidScriptBindingClassReferenceModel& Reference)
{
	if (Package.SchemaVersion < 6)
	{
		return true;
	}
	return FAvidScriptBindingDescriptorTypeGraph::IsDerivedFromClassPath(
		Package,
		Reference.ResultTypeId,
		TEXT("/Script/Engine.Actor"));
}

bool HasAvidScriptActorClassReferences(
	const FAvidScriptBindingPackageModel& Package)
{
	return Package.ClassReferences.ContainsByPredicate(
		[&Package](const FAvidScriptBindingClassReferenceModel& Reference)
		{
			return IsAvidScriptActorClassReference(Package, Reference);
		});
}

bool HasAvidScriptSceneComponentFactories(
	const FAvidScriptBindingPackageModel& Package)
{
	return Package.ObjectFactories.ContainsByPredicate(
		[&Package](const FAvidScriptBindingObjectFactoryModel& Factory)
		{
			const FAvidScriptBindingClassReferenceModel* Reference =
				Package.ClassReferences.FindByPredicate(
					[&Factory](
						const FAvidScriptBindingClassReferenceModel& Candidate)
					{
						return Candidate.StableId
							== Factory.ClassReferenceId;
					});
			const FAvidScriptBindingTypeModel* ConcreteType =
				Reference == nullptr
					? nullptr
					: Package.Types.FindByPredicate(
						[Reference](
							const FAvidScriptBindingTypeModel& Type)
						{
							return Type.ClassPath == Reference->ClassPath;
						});
			return ConcreteType != nullptr
				&& FAvidScriptBindingDescriptorTypeGraph::IsDerivedFromClassPath(
					Package,
					ConcreteType->StableId,
					TEXT("/Script/Engine.SceneComponent"));
		});
}

} // namespace

int32 FAvidScriptEditorCSharpBindingRenderer::GetLifecycleImportCount(
	const FAvidScriptBindingPackageModel& Package)
{
	return HasAvidScriptActorClassReferences(Package)
		? FAvidScriptObjectLifecycleBindings::GetSpecs().Num()
		: 0;
}

int32 FAvidScriptEditorCSharpBindingRenderer::GetManifestImportCount(
	const FAvidScriptBindingPackageModel& Package)
{
	int32 ImportCount = Package.Bindings.Num();
	ImportCount += GetLifecycleImportCount(Package);
	const bool bHasObjectTypeBindings = Package.Types.ContainsByPredicate(
		[](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.ObjectTypeOrdinal != INDEX_NONE;
		});
	if (bHasObjectTypeBindings)
	{
		ImportCount += FAvidScriptObjectTypeBindings::GetSpecs().Num();
	}
	if (Package.SchemaVersion >= 7 && !Package.ObjectFactories.IsEmpty())
	{
		ImportCount += FAvidScriptObjectFactoryBinding::GetSpecs().Num();
	}
	if (Package.SchemaVersion >= 7
		&& HasAvidScriptSceneComponentFactories(Package))
	{
		ImportCount += FAvidScriptSceneAttachmentBinding::GetSpecs().Num();
	}
	if (!Package.SelfTypeId.IsEmpty())
	{
		++ImportCount;
	}
	return ImportCount;
}

bool FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(
	const FAvidScriptBindingPackageModel& Package,
	const FString& DescriptorHash,
	FString& OutSource,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	OutSource.Empty();
	TArray<const FAvidScriptBindingClassReferenceModel*>
		ActorLifecycleClassReferences;
	TSet<FString> FactoryClassReferenceIds;
	for (const FAvidScriptBindingObjectFactoryModel& Factory :
		Package.ObjectFactories)
	{
		FactoryClassReferenceIds.Add(Factory.ClassReferenceId);
	}
	for (const FAvidScriptBindingClassReferenceModel& Reference :
		Package.ClassReferences)
	{
		if (Package.SchemaVersion >= 6)
		{
			const FAvidScriptBindingTypeModel* ResultType =
				Package.Types.FindByPredicate(
					[&Reference](
						const FAvidScriptBindingTypeModel& Type)
					{
						return Type.StableId == Reference.ResultTypeId;
					});
			if (ResultType == nullptr
				|| ResultType->Kind != TEXT("object_handle")
				|| ResultType->ObjectTypeOrdinal == INDEX_NONE
				|| ResultType->ClassPath != Reference.BaseClassPath)
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = TEXT("class_references.result_type_id");
				return false;
			}
		}
		const bool bActorLifecycleReference =
			IsAvidScriptActorClassReference(Package, Reference);
		const bool bFactoryClassReference =
			FactoryClassReferenceIds.Contains(Reference.StableId);
		if (Package.SchemaVersion >= 7
			&& bActorLifecycleReference == bFactoryClassReference)
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = TEXT("class_references.capability");
			return false;
		}
		if (bActorLifecycleReference)
		{
			ActorLifecycleClassReferences.Add(&Reference);
		}
	}
	const bool bHasLifecycleBindings =
		!ActorLifecycleClassReferences.IsEmpty();
	const bool bHasTypedClassReferenceSurface =
		bHasLifecycleBindings && Package.SchemaVersion >= 6;
	const TConstArrayView<FAvidScriptObjectLifecycleBindingSpec> LifecycleSpecs =
		FAvidScriptObjectLifecycleBindings::GetSpecs();
	FString SpawnNativeMethod;
	FString DestroyNativeMethod;
	FString IsANativeMethod;
	if (bHasLifecycleBindings)
	{
		if (LifecycleSpecs.Num() != 3)
		{
			OutErrorCategory = TEXT("lifecycle_binding_contract_invalid");
			OutErrorSource = FString::FromInt(LifecycleSpecs.Num());
			return false;
		}
		for (int32 SpecIndex = 0; SpecIndex < LifecycleSpecs.Num(); ++SpecIndex)
		{
			const FAvidScriptObjectLifecycleBindingSpec& Spec = LifecycleSpecs[SpecIndex];
			const FString NativeMethod = MakeNativeMethodName(Package.Bindings.Num() + SpecIndex);
			switch (Spec.Kind)
			{
			case EAvidScriptBindingInvocationKind::ObjectSpawnActor:
				if (!SpawnNativeMethod.IsEmpty() || Spec.Signature != TEXT("(iii)i"))
				{
					OutErrorCategory = TEXT("lifecycle_binding_contract_invalid");
					OutErrorSource = Spec.StableId;
					return false;
				}
				SpawnNativeMethod = NativeMethod;
				break;
			case EAvidScriptBindingInvocationKind::ObjectDestroyActor:
				if (!DestroyNativeMethod.IsEmpty() || Spec.Signature != TEXT("(ii)i"))
				{
					OutErrorCategory = TEXT("lifecycle_binding_contract_invalid");
					OutErrorSource = Spec.StableId;
					return false;
				}
				DestroyNativeMethod = NativeMethod;
				break;
			case EAvidScriptBindingInvocationKind::ObjectIsA:
				if (!IsANativeMethod.IsEmpty() || Spec.Signature != TEXT("(iii)i"))
				{
					OutErrorCategory = TEXT("lifecycle_binding_contract_invalid");
					OutErrorSource = Spec.StableId;
					return false;
				}
				IsANativeMethod = NativeMethod;
				break;
			default:
				OutErrorCategory = TEXT("lifecycle_binding_contract_invalid");
				OutErrorSource = Spec.StableId;
				return false;
			}
		}
		if (SpawnNativeMethod.IsEmpty() || DestroyNativeMethod.IsEmpty() || IsANativeMethod.IsEmpty())
		{
			OutErrorCategory = TEXT("lifecycle_binding_contract_invalid");
			OutErrorSource = TEXT("missing_required_kind");
			return false;
		}
	}
	const bool bHasObjectFactoryBindings =
		Package.SchemaVersion >= 7 && !Package.ObjectFactories.IsEmpty();
	const bool bHasSceneAttachmentBindings =
		bHasObjectFactoryBindings
		&& HasAvidScriptSceneComponentFactories(Package);
	if (bHasObjectFactoryBindings
		&& !FAvidScriptEditorCSharpObjectFactoryRenderer::ValidateBindingContract(
			OutErrorCategory,
			OutErrorSource))
	{
		return false;
	}
	if (bHasSceneAttachmentBindings
		&& !FAvidScriptEditorCSharpObjectFactoryRenderer::ValidateAttachmentContract(
			OutErrorCategory,
			OutErrorSource))
	{
		return false;
	}

	TMap<FString, const FAvidScriptBindingTypeModel*> TypesByCanonical;
	TMap<FString, const FAvidScriptBindingTypeModel*> TypesById;
	TSet<FString> CSharpTypeNames;
	bool bDescriptorHasActorProxy = false;
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		if (Type.StableId.IsEmpty()
			|| TypesByCanonical.Contains(Type.CanonicalType)
			|| TypesById.Contains(Type.StableId))
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = Type.StableId.IsEmpty() ? Type.CanonicalType : Type.StableId;
			return false;
		}
		TypesByCanonical.Add(Type.CanonicalType, &Type);
		TypesById.Add(Type.StableId, &Type);
		if (Type.Kind == TEXT("struct") || Type.Kind == TEXT("struct_wire") || Type.Kind == TEXT("object_handle") || Type.Kind == TEXT("enum"))
		{
			const FString Name = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Type.CppType);
			if (CSharpTypeNames.Contains(Name))
			{
				OutErrorCategory = TEXT("csharp_type_collision");
				OutErrorSource = Name;
				return false;
			}
			CSharpTypeNames.Add(Name);
			bDescriptorHasActorProxy |= Type.Kind == TEXT("object_handle") && Name == TEXT("AActor");
		}
	}
	if (Package.SchemaVersion >= 9
		&& !FAvidScriptBindingDescriptorLayout::ValidateStructWireGraph(
			Package.Types,
			OutErrorSource))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		return false;
	}
	const FAvidScriptBindingTypeModel* const ActorType = FindRenderedType(
		TypesByCanonical,
		TEXT("object:/Script/Engine.Actor"));
	if (bHasLifecycleBindings
		&& (ActorType == nullptr
			|| (bHasTypedClassReferenceSurface
				&& ActorType->ObjectTypeOrdinal == INDEX_NONE)
			|| CSharpTypeNames.Contains(TEXT("TSubclassOfAActor"))
			|| CSharpTypeNames.Contains(TEXT("ProjectClasses"))
			|| (CSharpTypeNames.Contains(TEXT("AActor")) && !bDescriptorHasActorProxy)))
	{
		OutErrorCategory = TEXT("csharp_type_collision");
		OutErrorSource = TEXT("TSubclassOfAActor|ProjectClasses|AActor");
		return false;
	}

	struct FClassReferenceType
	{
		FString ReferenceKey;
		FString ClassReferenceTypeName;
		const FAvidScriptBindingTypeModel* ResultType = nullptr;
	};
	TArray<FClassReferenceType> ClassReferenceTypes;
	TMap<FString, FString> ClassReferenceTypeNamesByKey;
	if (bHasLifecycleBindings)
	{
		const auto AddClassReferenceType = [
			&ClassReferenceTypes,
			&ClassReferenceTypeNamesByKey,
			&CSharpTypeNames,
			bHasTypedClassReferenceSurface](
			const FAvidScriptBindingTypeModel* ResultType,
			const FString& ReferenceKey,
			FString& OutErrorCategory,
			FString& OutErrorSource)
		{
			if (ResultType == nullptr
				|| ResultType->Kind != TEXT("object_handle")
				|| ReferenceKey.IsEmpty()
				|| (bHasTypedClassReferenceSurface
					&& ResultType->ObjectTypeOrdinal == INDEX_NONE))
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = bHasTypedClassReferenceSurface
					? FString(TEXT("class_references.result_type_id"))
					: FString(TEXT("class_references.base_class_path"));
				return false;
			}
			if (ClassReferenceTypeNamesByKey.Contains(ReferenceKey))
			{
				return true;
			}
			const FString ClassReferenceTypeName = TEXT("TSubclassOf")
				+ FAvidScriptEditorCSharpSyntax::MakeIdentifier(ResultType->CppType);
			if (CSharpTypeNames.Contains(ClassReferenceTypeName))
			{
				OutErrorCategory = TEXT("csharp_type_collision");
				OutErrorSource = ClassReferenceTypeName;
				return false;
			}
			CSharpTypeNames.Add(ClassReferenceTypeName);
			ClassReferenceTypeNamesByKey.Add(ReferenceKey, ClassReferenceTypeName);
			ClassReferenceTypes.Add({ ReferenceKey, ClassReferenceTypeName, ResultType });
			return true;
		};
		const FString ActorReferenceKey = bHasTypedClassReferenceSurface
			? ActorType->StableId
			: FString(TEXT("/Script/Engine.Actor"));
		if (!AddClassReferenceType(
				ActorType,
				ActorReferenceKey,
				OutErrorCategory,
				OutErrorSource))
		{
			return false;
		}
		for (const FAvidScriptBindingClassReferenceModel* Reference :
			ActorLifecycleClassReferences)
		{
			const FString ReferenceKey = bHasTypedClassReferenceSurface
				? Reference->ResultTypeId
				: Reference->BaseClassPath;
			const FAvidScriptBindingTypeModel* ResultType = bHasTypedClassReferenceSurface
				? TypesById.FindRef(Reference->ResultTypeId)
				: FindRenderedType(
					TypesByCanonical,
					TEXT("object:") + Reference->BaseClassPath);
			if (!AddClassReferenceType(
					ResultType,
					ReferenceKey,
					OutErrorCategory,
					OutErrorSource))
			{
				return false;
			}
		}
		ClassReferenceTypes.Sort([](const FClassReferenceType& Left, const FClassReferenceType& Right)
		{
			if (Left.ClassReferenceTypeName == TEXT("TSubclassOfAActor"))
			{
				return Right.ClassReferenceTypeName != TEXT("TSubclassOfAActor");
			}
			if (Right.ClassReferenceTypeName == TEXT("TSubclassOfAActor"))
			{
				return false;
			}
			return Left.ClassReferenceTypeName < Right.ClassReferenceTypeName;
		});
	}

	const FAvidScriptBindingTypeModel* SelfType = nullptr;
	if (!Package.SelfTypeId.IsEmpty())
	{
		SelfType = TypesById.FindRef(Package.SelfTypeId);
		if (SelfType == nullptr || SelfType->ObjectTypeOrdinal == INDEX_NONE)
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = TEXT("self_type_id");
			return false;
		}
	}

	TArray<FAvidScriptEditorCSharpObjectFactorySurface> ObjectFactorySurfaces;
	if (!FAvidScriptEditorCSharpObjectFactoryRenderer::BuildSurfaces(
			Package,
			CSharpTypeNames,
			ObjectFactorySurfaces,
			OutErrorCategory,
			OutErrorSource))
	{
		return false;
	}

	TMap<FString, TArray<const FAvidScriptBindingFunctionModel*>> BindingsByOwner;
	TMap<FString, const FAvidScriptBindingFunctionModel*> PropertyGetters;
	TMap<FString, const FAvidScriptBindingFunctionModel*> PropertySetters;
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		if (!ValidateBindingValueDirections(Binding, OutErrorSource))
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			return false;
		}
		if (FindRenderedType(TypesByCanonical, TEXT("object:") + Binding.OwnerClass) == nullptr)
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			OutErrorSource = Binding.OwnerClass;
			return false;
		}
		BindingsByOwner.FindOrAdd(Binding.OwnerClass).Add(&Binding);
		if (Binding.BindingKind == TEXT("property_get")
			|| Binding.BindingKind == TEXT("property_set"))
		{
			const FString PropertyKey = Binding.OwnerClass
				+ TEXT("\n") + Binding.UeMember;
			TMap<FString, const FAvidScriptBindingFunctionModel*>& Accessors =
				Binding.BindingKind == TEXT("property_get")
					? PropertyGetters
					: PropertySetters;
			if (Accessors.Contains(PropertyKey))
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = PropertyKey;
				return false;
			}
			Accessors.Add(PropertyKey, &Binding);
		}
	}

	TSet<FString> ObjectTypesUsedAsValues;
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		if (Binding.ReturnValue.Kind == TEXT("object_handle"))
		{
			ObjectTypesUsedAsValues.Add(Binding.ReturnValue.CanonicalType);
		}
		for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
		{
			if (Parameter.Kind == TEXT("object_handle"))
			{
				ObjectTypesUsedAsValues.Add(Parameter.CanonicalType);
			}
		}
	}

	TMap<FString, bool> StaticOnlyOwners;
	for (const TPair<FString, TArray<const FAvidScriptBindingFunctionModel*>>& Pair : BindingsByOwner)
	{
		const bool bHasInstanceMethod = Pair.Value.ContainsByPredicate(
			[](const FAvidScriptBindingFunctionModel* Binding)
			{
				return Binding != nullptr && !Binding->bStatic;
			});
		const FAvidScriptBindingTypeModel* OwnerType = FindRenderedType(
			TypesByCanonical,
			TEXT("object:") + Pair.Key);
		StaticOnlyOwners.Add(
			Pair.Key,
			Package.SchemaVersion >= 6
				? OwnerType != nullptr && OwnerType->ObjectTypeOrdinal == INDEX_NONE
				: !bHasInstanceMethod
					&& !ObjectTypesUsedAsValues.Contains(TEXT("object:") + Pair.Key)
					&& !(bHasLifecycleBindings && Pair.Key == TEXT("/Script/Engine.Actor")));
	}

	static const TSet<FString> HandleMemberNames = {
		TEXT("Slot"),
		TEXT("Generation"),
		TEXT("AvidScriptSlot"),
		TEXT("AvidScriptGeneration"),
		TEXT("IsNull"),
		TEXT("HasHandle"),
		TEXT("IsValid"),
		TEXT("TryCast")
	};
	TMap<int32, FCSharpRenderedMethod> RenderedMethods;
	TSet<FString> MethodSignatures;
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		if (Binding.BindingKind == TEXT("property_set"))
		{
			const FString PropertyKey = Binding.OwnerClass
				+ TEXT("\n") + Binding.UeMember;
			if (PropertyGetters.Contains(PropertyKey))
			{
				RenderedMethods.Add(
					Binding.Ordinal,
					FCSharpRenderedMethod());
				continue;
			}
		}
		const FAvidScriptBindingTypeModel* OwnerType = FindRenderedType(
			TypesByCanonical,
			TEXT("object:") + Binding.OwnerClass);
		const FString MethodName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Binding.ScriptName);
		const bool bStaticOnlyOwner = StaticOnlyOwners.FindRef(Binding.OwnerClass);
		if (OwnerType == nullptr
			|| MethodName == FAvidScriptEditorCSharpSyntax::MakeIdentifier(OwnerType->CppType)
			|| (!bStaticOnlyOwner && HandleMemberNames.Contains(MethodName)))
		{
			OutErrorCategory = TEXT("generated_member_collision");
			OutErrorSource = Binding.OwnerClass + TEXT(".") + MethodName;
			return false;
		}

		FCSharpRenderedMethod Rendered;
		const FString PropertyKey = Binding.OwnerClass
			+ TEXT("\n") + Binding.UeMember;
		const bool bRendered = Binding.BindingKind == TEXT("property_get")
			? RenderPropertyGetter(
				Binding,
				PropertySetters.FindRef(PropertyKey),
				TypesByCanonical,
				TypesById,
				Rendered,
				OutErrorCategory,
				OutErrorSource)
			: Binding.BindingKind == TEXT("property_set")
				? RenderPropertySetter(
					Binding,
					TypesByCanonical,
					TypesById,
					Rendered,
					OutErrorCategory,
					OutErrorSource)
				: RenderMethod(
					Binding,
					TypesByCanonical,
					TypesById,
					Rendered,
					OutErrorCategory,
					OutErrorSource);
		if (!bRendered)
		{
			return false;
		}
		const FString CollisionKey = Binding.OwnerClass + TEXT("|") + Rendered.SignatureKey;
		if (MethodSignatures.Contains(CollisionKey))
		{
			OutErrorCategory = TEXT("script_name_collision");
			OutErrorSource = CollisionKey;
			return false;
		}
		MethodSignatures.Add(CollisionKey);
		RenderedMethods.Add(Binding.Ordinal, MoveTemp(Rendered));
	}

	bool bNeedsVector = false;
	bool bNeedsRotator = false;
	bool bNeedsTransform = false;
	const bool bNeedsGeneratedVectorValueBuffer =
		Package.Bindings.ContainsByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.DispatchMode == TEXT("generated_native_s1")
					&& Binding.GeneratedShape == TEXT("vector_value");
			});
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		bNeedsVector |= Type.CppType == TEXT("FVector");
		bNeedsRotator |= Type.CppType == TEXT("FRotator");
		bNeedsTransform |= Type.CppType == TEXT("FTransform");
	}
	if (bNeedsTransform)
	{
		bNeedsVector = true;
		bNeedsRotator = true;
	}
	if (bHasLifecycleBindings)
	{
		bNeedsVector = true;
		bNeedsRotator = true;
		bNeedsTransform = true;
	}
	bNeedsVector = true;

	TArray<FString> Lines = {
		TEXT("// <auto-generated />"),
		TEXT("#nullable enable"),
		TEXT("using System;"),
		TEXT("using System.Runtime.InteropServices;"),
		TEXT(""),
		TEXT("namespace AvidScript;"),
		TEXT("")
	};
	FAvidScriptEditorCSharpStateContractRenderer::AppendReferenceSurface(Lines);
	Lines.Append({
		TEXT("internal static class AvidScriptBindingPackage"),
		TEXT("{"),
		FString::Printf(TEXT("    internal const string PackageName = \"%s\";"), *EscapeCSharpString(Package.PackageName)),
		FString::Printf(TEXT("    internal const string PackageHash = \"%s\";"), *Package.PackageHash),
		FString::Printf(TEXT("    internal const string DescriptorHash = \"%s\";"), *DescriptorHash),
		TEXT("}"),
		TEXT(""),
		TEXT("[AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]"),
		TEXT("internal sealed class AvidScriptDataLaneAttribute : Attribute"),
		TEXT("{"),
		TEXT("    internal AvidScriptDataLaneAttribute(string optimizationClass, int bindingOrdinal)"),
		TEXT("    {"),
		TEXT("        OptimizationClass = optimizationClass;"),
		TEXT("        BindingOrdinal = bindingOrdinal;"),
		TEXT("    }"),
		TEXT(""),
		TEXT("    internal string OptimizationClass { get; }"),
		TEXT("    internal int BindingOrdinal { get; }"),
		TEXT("}"),
		TEXT("")
	});
	if (bNeedsVector) { AppendVector(Lines); }
	if (bNeedsGeneratedVectorValueBuffer)
	{
		AppendGeneratedVectorValueBuffer(Lines);
	}
	AppendInputEvent(Lines);
	if (bNeedsRotator) { AppendRotator(Lines); }
	if (bNeedsTransform) { AppendTransform(Lines); }
	if (bHasLifecycleBindings)
	{
		for (const FClassReferenceType& ClassReferenceType : ClassReferenceTypes)
		{
			const FString Declaration = ClassReferenceType.ClassReferenceTypeName == TEXT("TSubclassOfAActor")
				? TEXT("public readonly struct TSubclassOfAActor")
				: TEXT("public readonly struct ") + ClassReferenceType.ClassReferenceTypeName;
			Lines.Append({
				TEXT("[StructLayout(LayoutKind.Sequential)]"),
				Declaration,
				TEXT("{"),
				TEXT("    private readonly int Ordinal;"),
				TEXT(""),
				TEXT("    internal ") + ClassReferenceType.ClassReferenceTypeName + TEXT("(int ordinal)"),
				TEXT("    {"),
				TEXT("        Ordinal = ordinal;"),
				TEXT("    }"),
				TEXT(""),
				TEXT("    internal int AvidScriptOrdinal => Ordinal;")
			});
			const FAvidScriptBindingTypeModel* BaseType =
				TypesById.FindRef(ClassReferenceType.ResultType->BaseTypeId);
			const FString* BaseClassReferenceTypeName = nullptr;
			TSet<FString> VisitedBaseTypeIds;
			while (BaseType != nullptr && !VisitedBaseTypeIds.Contains(BaseType->StableId))
			{
				VisitedBaseTypeIds.Add(BaseType->StableId);
				BaseClassReferenceTypeName =
					ClassReferenceTypeNamesByKey.Find(BaseType->StableId);
				if (BaseClassReferenceTypeName != nullptr)
				{
					break;
				}
				BaseType = TypesById.FindRef(BaseType->BaseTypeId);
			}
			if (BaseClassReferenceTypeName != nullptr)
			{
				Lines.Append({
					TEXT(""),
					FString::Printf(
						TEXT("    public static implicit operator %s(%s value)"),
						**BaseClassReferenceTypeName,
						*ClassReferenceType.ClassReferenceTypeName),
					TEXT("    {"),
					TEXT("        return new(value.Ordinal);"),
					TEXT("    }")
				});
			}
			Lines.Append({
				TEXT("}"),
				TEXT("")
			});
		}
		Lines.Append({ TEXT("public static class ProjectClasses"), TEXT("{") });
		for (const FAvidScriptBindingClassReferenceModel* Reference :
			ActorLifecycleClassReferences)
		{
			const FString ReferenceKey = bHasTypedClassReferenceSurface
				? Reference->ResultTypeId
				: Reference->BaseClassPath;
			const FString* ClassReferenceTypeName = ClassReferenceTypeNamesByKey.Find(ReferenceKey);
			if (ClassReferenceTypeName == nullptr)
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = bHasTypedClassReferenceSurface
					? FString(TEXT("class_references.result_type_id"))
					: FString(TEXT("class_references.base_class_path"));
				return false;
			}
			Lines.Add(FString::Printf(
				TEXT("    public static %s %s => new(%d);"),
				**ClassReferenceTypeName,
				*FAvidScriptEditorCSharpSyntax::MakeIdentifier(
					Reference->ScriptName),
				Reference->Ordinal));
		}
		Lines.Append({ TEXT("}"), TEXT("") });
	}
	if (!ObjectFactorySurfaces.IsEmpty())
	{
		FAvidScriptEditorCSharpObjectFactoryRenderer::AppendCapabilityTokens(
			ObjectFactorySurfaces,
			Lines);
	}

	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		if (Type.Kind == TEXT("enum"))
		{
			const FString TypeName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Type.CppType);
			TSet<FString> MemberNames;
			Lines.Add(TEXT("public enum ") + TypeName + TEXT(" : int"));
			Lines.Add(TEXT("{"));
			for (const FAvidScriptBindingEnumValue& EnumValue : Type.EnumValues)
			{
				const FString MemberName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(EnumValue.Name);
				if (MemberName == TypeName || MemberNames.Contains(MemberName))
				{
					OutErrorCategory = TEXT("enum_member_collision");
					OutErrorSource = Type.CanonicalType + TEXT(".") + MemberName;
					return false;
				}
				MemberNames.Add(MemberName);
				Lines.Add(FString::Printf(TEXT("    %s = %lld,"), *MemberName, EnumValue.Value));
			}
			Lines.Add(TEXT("}"));
			Lines.Add(TEXT(""));
		}
	}

	Lines.Append({
		TEXT("[StructLayout(LayoutKind.Sequential)]"),
		TEXT("internal readonly struct FAvidScriptObjectHandle"),
		TEXT("{"),
		TEXT("    internal readonly int Slot;"),
		TEXT("    internal readonly int Generation;"),
		TEXT(""),
		TEXT("    internal FAvidScriptObjectHandle(int slot, int generation)"),
		TEXT("    {"),
		TEXT("        Slot = slot;"),
		TEXT("        Generation = generation;"),
		TEXT("    }"),
		TEXT("}"),
		TEXT("")
	});

	TArray<const FAvidScriptBindingTypeModel*> ObjectTypes;
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		if (Type.Kind == TEXT("object_handle"))
		{
			ObjectTypes.Add(&Type);
		}
	}
	ObjectTypes.Sort([](const FAvidScriptBindingTypeModel& Left, const FAvidScriptBindingTypeModel& Right)
	{
		return Left.CanonicalType < Right.CanonicalType;
	});

	bool bNeedsObjectTypeIsA = false;
	for (const FAvidScriptBindingTypeModel* Type : ObjectTypes)
	{
		const FString OwnerPath = Type->CanonicalType.RightChop(7);
		const TArray<const FAvidScriptBindingFunctionModel*>* OwnerBindings = BindingsByOwner.Find(OwnerPath);
		const bool bStaticOnly = StaticOnlyOwners.FindRef(OwnerPath);
		const FString TypeName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(Type->CppType);
		if (bStaticOnly)
		{
			Lines.Add(TEXT("public static class ") + TypeName);
			Lines.Add(TEXT("{"));
		}
		else
		{
			const FAvidScriptBindingTypeModel* DirectBaseType = Type->BaseTypeId.IsEmpty()
				? nullptr
				: TypesById.FindRef(Type->BaseTypeId);
			if (DirectBaseType != nullptr && Type->ObjectTypeOrdinal == INDEX_NONE)
			{
				OutErrorCategory = TEXT("descriptor_contract_invalid");
				OutErrorSource = Type->StableId;
				return false;
			}
			const FString DirectBaseTypeName = DirectBaseType == nullptr
				? FString()
				: FAvidScriptEditorCSharpSyntax::MakeIdentifier(DirectBaseType->CppType);
			AppendObjectHandleProxy(
				Lines,
				TypeName,
				DirectBaseTypeName,
				Type->ObjectTypeOrdinal);
			bNeedsObjectTypeIsA |= DirectBaseType != nullptr;
		}

		if (OwnerBindings != nullptr)
		{
			TArray<const FAvidScriptBindingFunctionModel*> SortedBindings = *OwnerBindings;
			SortedBindings.Sort([](const FAvidScriptBindingFunctionModel& Left, const FAvidScriptBindingFunctionModel& Right)
			{
				return Left.Ordinal < Right.Ordinal;
			});
			for (const FAvidScriptBindingFunctionModel* Binding : SortedBindings)
			{
				const FString PropertyKey = Binding->OwnerClass
					+ TEXT("\n") + Binding->UeMember;
				if (Binding->BindingKind == TEXT("property_set")
					&& PropertyGetters.Contains(PropertyKey))
				{
					continue;
				}
				Lines.Add(TEXT(""));
				Lines.Append(RenderedMethods.FindChecked(Binding->Ordinal).MethodLines);
			}
		}
		Lines.Add(TEXT("}"));
		Lines.Add(TEXT(""));
	}
	if (!AppendStructWireDeclarations(
			Package,
			TypesById,
			Lines,
			OutErrorCategory,
			OutErrorSource))
	{
		return false;
	}

	if (SelfType != nullptr || bHasLifecycleBindings || bHasObjectFactoryBindings)
	{
		Lines.Append({ TEXT("public static class UE"), TEXT("{") });
		if (SelfType != nullptr)
		{
			Lines.Append({
				TEXT("    public static ") + FAvidScriptEditorCSharpSyntax::MakeIdentifier(SelfType->CppType) + TEXT(" Self"),
				TEXT("    {"),
				TEXT("        get"),
				TEXT("        {"),
				TEXT("            long packedHandle = OwnerGetHandle();"),
				TEXT("            return new((int)packedHandle, (int)(packedHandle >> 32));"),
				TEXT("        }"),
				TEXT("    }")
			});
		}
		if (SelfType != nullptr && bHasLifecycleBindings)
		{
			Lines.Add(TEXT(""));
		}
		if (SelfType != nullptr || bHasLifecycleBindings)
		{
			Lines.Append({
				TEXT("    public static int SetTimer(float delaySeconds, int callbackId) => AvidScriptRuntimeNative.TimerSetOnce(delaySeconds, callbackId);"),
				TEXT("    public static bool CancelTimer(int timerHandle) => AvidScriptRuntimeNative.TimerCancel(timerHandle) != 0;")
			});
		}
		if (bHasLifecycleBindings)
		{
			for (const FClassReferenceType& ClassReferenceType : ClassReferenceTypes)
			{
				if (ClassReferenceType.ResultType == ActorType)
				{
					continue;
				}
				Lines.Add(TEXT(""));
				Lines.Append({
					FString::Printf(
						TEXT("    public static %s SpawnActor(%s actorClass, FTransform transform)"),
						*FAvidScriptEditorCSharpSyntax::MakeIdentifier(ClassReferenceType.ResultType->CppType),
						*ClassReferenceType.ClassReferenceTypeName),
					TEXT("    {"),
					FString::Printf(TEXT("        AvidScriptNative.%s("), *SpawnNativeMethod),
					TEXT("            actorClass.AvidScriptOrdinal, in transform,"),
					TEXT("            out FAvidScriptObjectHandle actorHandle);"),
					TEXT("        return new(actorHandle.Slot, actorHandle.Generation);"),
					TEXT("    }")
				});
			}
			Lines.Add(TEXT(""));
			Lines.Append({
				TEXT("    public static AActor SpawnActor(TSubclassOfAActor actorClass, FTransform transform)"),
				TEXT("    {"),
				FString::Printf(TEXT("        AvidScriptNative.%s("), *SpawnNativeMethod),
				TEXT("            actorClass.AvidScriptOrdinal, in transform,"),
				TEXT("            out FAvidScriptObjectHandle actorHandle);"),
				TEXT("        return new(actorHandle.Slot, actorHandle.Generation);"),
				TEXT("    }"),
				TEXT(""),
				TEXT("    public static bool DestroyActor(AActor actor)"),
				FString::Printf(
					TEXT("        => AvidScriptNative.%s(actor.AvidScriptSlot, actor.AvidScriptGeneration) != 0;"),
					*DestroyNativeMethod),
				TEXT(""),
				TEXT("    public static bool IsA(AActor actor, TSubclassOfAActor actorClass)"),
				FString::Printf(TEXT("        => AvidScriptNative.%s("), *IsANativeMethod),
				TEXT("            actor.AvidScriptSlot, actor.AvidScriptGeneration,"),
				TEXT("            actorClass.AvidScriptOrdinal) != 0;")
			});
		}
		if ((SelfType != nullptr || bHasLifecycleBindings)
			&& bHasObjectFactoryBindings)
		{
			Lines.Add(TEXT(""));
		}
		if (bHasObjectFactoryBindings)
		{
			FAvidScriptEditorCSharpObjectFactoryRenderer::AppendFacadeMethods(
				ObjectFactorySurfaces,
				Lines);
		}
		if (SelfType != nullptr)
		{
			Lines.Append({
				TEXT(""),
				TEXT("    [DllImport(\"avidscript\", EntryPoint = \"avid_owner_get_handle\")]"),
				TEXT("    private static extern long OwnerGetHandle();")
			});
		}
		Lines.Append({ TEXT("}"), TEXT("") });

		if (SelfType != nullptr || bHasLifecycleBindings)
		{
			Lines.Append({
				TEXT("internal static class AvidScriptRuntimeNative"),
				TEXT("{"),
				TEXT("    [DllImport(\"env\", EntryPoint = \"timer_set_once\")]"),
				TEXT("    internal static extern int TimerSetOnce(float delaySeconds, int callbackId);"),
				TEXT(""),
				TEXT("    [DllImport(\"env\", EntryPoint = \"timer_cancel\")]"),
				TEXT("    internal static extern int TimerCancel(int timerHandle);"),
				TEXT("}"),
				TEXT("")
			});
		}
	}

	Lines.Add(TEXT("internal static class AvidScriptNative"));
	Lines.Add(TEXT("{"));
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		if (Binding.Ordinal > 0)
		{
			Lines.Add(TEXT(""));
		}
		Lines.Append(RenderedMethods.FindChecked(Binding.Ordinal).NativeLines);
	}
	if (bHasLifecycleBindings)
	{
		for (int32 SpecIndex = 0; SpecIndex < LifecycleSpecs.Num(); ++SpecIndex)
		{
			if (!Package.Bindings.IsEmpty() || SpecIndex > 0)
			{
				Lines.Add(TEXT(""));
			}
			const FAvidScriptObjectLifecycleBindingSpec& Spec = LifecycleSpecs[SpecIndex];
			const FString NativeMethod = MakeNativeMethodName(Package.Bindings.Num() + SpecIndex);
			Lines.Add(FString::Printf(
				TEXT("    [DllImport(\"%s\", EntryPoint = \"%s\")]"),
				*EscapeCSharpString(Spec.ModuleName),
				*EscapeCSharpString(Spec.ImportName)));
			switch (Spec.Kind)
			{
			case EAvidScriptBindingInvocationKind::ObjectSpawnActor:
				Lines.Add(FString::Printf(
					TEXT("    internal static extern int %s(int classOrdinal, in FTransform transform, out FAvidScriptObjectHandle actorHandle);"),
					*NativeMethod));
				break;
			case EAvidScriptBindingInvocationKind::ObjectDestroyActor:
				Lines.Add(FString::Printf(
					TEXT("    internal static extern int %s(int slot, int generation);"),
					*NativeMethod));
				break;
			case EAvidScriptBindingInvocationKind::ObjectIsA:
				Lines.Add(FString::Printf(
					TEXT("    internal static extern int %s(int slot, int generation, int classOrdinal);"),
					*NativeMethod));
				break;
			default:
				checkNoEntry();
				break;
			}
		}
	}
	if (bNeedsObjectTypeIsA)
	{
		if (!Package.Bindings.IsEmpty() || bHasLifecycleBindings)
		{
			Lines.Add(TEXT(""));
		}
		Lines.Append({
			TEXT("    [DllImport(\"avidscript\", EntryPoint = \"avid_object_type_is_a\")]"),
			TEXT("    internal static extern int ObjectTypeIsA(int slot, int generation, int targetOrdinal);")
		});
	}
	if (bHasObjectFactoryBindings)
	{
		FAvidScriptEditorCSharpObjectFactoryRenderer::AppendNativeImports(
			!Package.Bindings.IsEmpty()
				|| bHasLifecycleBindings
				|| bNeedsObjectTypeIsA,
			Lines);
	}
	if (bHasSceneAttachmentBindings)
	{
		FAvidScriptEditorCSharpObjectFactoryRenderer::AppendSceneAttachmentNativeImports(
			true,
			Lines);
	}
	Lines.Add(TEXT("}"));
	Lines.Add(TEXT(""));
	OutSource = FString::Join(Lines, TEXT("\n"));
	return true;
}

bool FAvidScriptEditorCSharpBindingRenderer::EmitManifest(
	const FAvidScriptBindingPackageModel& Package,
	const FString& DescriptorHash,
	const FString& SourceHash,
	FString& OutManifest)
{
	OutManifest.Empty();
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutManifest);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema_version"), 1);
	Writer->WriteValue(TEXT("emitter_version"), BindingArtifact::EmitterVersion);
	Writer->WriteValue(TEXT("package_name"), Package.PackageName);
	Writer->WriteValue(TEXT("package_hash"), Package.PackageHash);
	Writer->WriteValue(TEXT("descriptor_schema_version"), Package.SchemaVersion);
	Writer->WriteValue(TEXT("class_reference_count"), Package.ClassReferences.Num());
	if (Package.SchemaVersion >= 7)
	{
		Writer->WriteValue(
			TEXT("object_factory_count"),
			Package.ObjectFactories.Num());
	}
	Writer->WriteValue(TEXT("descriptor_sha256"), DescriptorHash);
	Writer->WriteValue(TEXT("reference_source_sha256"), SourceHash);
	Writer->WriteObjectStart(TEXT("files"));
	Writer->WriteValue(TEXT("descriptor"), BindingArtifact::DescriptorFileName);
	Writer->WriteValue(TEXT("reference_source"), BindingArtifact::ReferenceSourceFileName);
	Writer->WriteObjectEnd();
	Writer->WriteArrayStart(TEXT("required_imports"));
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("stable_id"), Binding.StableId);
		Writer->WriteValue(TEXT("ordinal"), Binding.Ordinal);
		Writer->WriteValue(TEXT("module"), Binding.HostImport.Module);
		Writer->WriteValue(TEXT("name"), Binding.HostImport.Name);
		Writer->WriteValue(TEXT("signature"), Binding.HostImport.Signature);
		if (Binding.DispatchMode == TEXT("generated_native_s1"))
		{
			Writer->WriteValue(TEXT("dispatch_mode"), Binding.DispatchMode);
			Writer->WriteValue(TEXT("generated_shape"), Binding.GeneratedShape);
			Writer->WriteValue(
				TEXT("generated_receiver_mode"),
				Binding.GeneratedReceiverMode);
			Writer->WriteValue(
				TEXT("semantic_fallback_ordinal"),
				Binding.SemanticFallbackOrdinal);
		}
		Writer->WriteObjectEnd();
	}
	const bool bHasLifecycleBindings =
		HasAvidScriptActorClassReferences(Package);
	if (bHasLifecycleBindings)
	{
		const TConstArrayView<FAvidScriptObjectLifecycleBindingSpec> LifecycleSpecs =
			FAvidScriptObjectLifecycleBindings::GetSpecs();
		for (int32 SpecIndex = 0; SpecIndex < LifecycleSpecs.Num(); ++SpecIndex)
		{
			const FAvidScriptObjectLifecycleBindingSpec& Spec = LifecycleSpecs[SpecIndex];
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("stable_id"), Spec.StableId);
			Writer->WriteValue(TEXT("ordinal"), Package.Bindings.Num() + SpecIndex);
			Writer->WriteValue(TEXT("module"), Spec.ModuleName);
			Writer->WriteValue(TEXT("name"), Spec.ImportName);
			Writer->WriteValue(TEXT("signature"), Spec.Signature);
			Writer->WriteObjectEnd();
		}
	}
	const bool bHasObjectTypeBindings = Package.Types.ContainsByPredicate(
		[](const FAvidScriptBindingTypeModel& Type)
		{
			return Type.ObjectTypeOrdinal != INDEX_NONE;
		});
	if (bHasObjectTypeBindings)
	{
		const int32 ObjectTypeImportOffset =
			Package.Bindings.Num()
			+ (bHasLifecycleBindings
				? FAvidScriptObjectLifecycleBindings::GetSpecs().Num()
				: 0);
		const TConstArrayView<FAvidScriptObjectTypeBindingSpec> ObjectTypeSpecs =
			FAvidScriptObjectTypeBindings::GetSpecs();
		for (int32 SpecIndex = 0; SpecIndex < ObjectTypeSpecs.Num(); ++SpecIndex)
		{
			const FAvidScriptObjectTypeBindingSpec& Spec = ObjectTypeSpecs[SpecIndex];
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("stable_id"), Spec.StableId);
			Writer->WriteValue(TEXT("ordinal"), ObjectTypeImportOffset + SpecIndex);
			Writer->WriteValue(TEXT("module"), Spec.ModuleName);
			Writer->WriteValue(TEXT("name"), Spec.ImportName);
			Writer->WriteValue(TEXT("signature"), Spec.Signature);
			Writer->WriteObjectEnd();
		}
	}
	if (Package.SchemaVersion >= 7 && !Package.ObjectFactories.IsEmpty())
	{
		const int32 ObjectFactoryImportOffset =
			Package.Bindings.Num()
			+ (bHasLifecycleBindings
				? FAvidScriptObjectLifecycleBindings::GetSpecs().Num()
				: 0)
			+ (bHasObjectTypeBindings
				? FAvidScriptObjectTypeBindings::GetSpecs().Num()
				: 0);
		const TConstArrayView<FAvidScriptObjectFactoryBindingSpec>
			ObjectFactorySpecs = FAvidScriptObjectFactoryBinding::GetSpecs();
		for (int32 SpecIndex = 0; SpecIndex < ObjectFactorySpecs.Num(); ++SpecIndex)
		{
			const FAvidScriptObjectFactoryBindingSpec& Spec =
				ObjectFactorySpecs[SpecIndex];
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("stable_id"), Spec.StableId);
			Writer->WriteValue(
				TEXT("ordinal"),
				ObjectFactoryImportOffset + SpecIndex);
			Writer->WriteValue(TEXT("module"), Spec.ModuleName);
			Writer->WriteValue(TEXT("name"), Spec.ImportName);
			Writer->WriteValue(TEXT("signature"), Spec.Signature);
			Writer->WriteObjectEnd();
		}
		if (HasAvidScriptSceneComponentFactories(Package))
		{
			const int32 SceneAttachmentImportOffset =
				ObjectFactoryImportOffset + ObjectFactorySpecs.Num();
			const TConstArrayView<FAvidScriptSceneAttachmentBindingSpec>
				AttachmentSpecs = FAvidScriptSceneAttachmentBinding::GetSpecs();
			for (int32 SpecIndex = 0;
				SpecIndex < AttachmentSpecs.Num();
				++SpecIndex)
			{
				const FAvidScriptSceneAttachmentBindingSpec& Spec =
					AttachmentSpecs[SpecIndex];
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("stable_id"), Spec.StableId);
				Writer->WriteValue(
					TEXT("ordinal"),
					SceneAttachmentImportOffset + SpecIndex);
				Writer->WriteValue(TEXT("module"), Spec.ModuleName);
				Writer->WriteValue(TEXT("name"), Spec.ImportName);
				Writer->WriteValue(TEXT("signature"), Spec.Signature);
				Writer->WriteObjectEnd();
			}
		}
	}
	if (!Package.SelfTypeId.IsEmpty())
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("stable_id"), TEXT("avidscript.owner_get_handle.v1"));
		Writer->WriteValue(TEXT("ordinal"), INDEX_NONE);
		Writer->WriteValue(TEXT("module"), TEXT("avidscript"));
		Writer->WriteValue(TEXT("name"), TEXT("avid_owner_get_handle"));
		Writer->WriteValue(TEXT("signature"), TEXT("()I"));
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	return Writer->Close();
}
