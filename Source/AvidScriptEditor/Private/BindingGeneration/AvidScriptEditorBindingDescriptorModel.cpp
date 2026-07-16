#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"

#include "AvidScriptHash.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
bool IsLowerSha256(const FString& Value)
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

bool ReadRequiredString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	FString& OutValue,
	FString& OutErrorSource)
{
	if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
	{
		OutErrorSource = Field;
		return false;
	}
	return true;
}

bool ReadRequiredBool(
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

bool ReadRequiredInt(
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

bool ReadStringArray(
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

bool ReadEnumValues(
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
		if (!ReadRequiredString(EnumValueObject, TEXT("name"), EnumValue.Name, OutErrorSource)
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

bool ParseType(
	const TSharedPtr<FJsonObject>& Object,
	FAvidScriptBindingTypeModel& OutType,
	FString& OutErrorSource)
{
	if (!ReadRequiredString(Object, TEXT("stable_id"), OutType.StableId, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("canonical_type"), OutType.CanonicalType, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("kind"), OutType.Kind, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("cpp_type"), OutType.CppType, OutErrorSource)
		|| !ReadRequiredInt(Object, TEXT("size"), OutType.Size, OutErrorSource)
		|| !ReadRequiredInt(Object, TEXT("alignment"), OutType.Alignment, OutErrorSource)
		|| !ReadStringArray(Object, TEXT("abi_types"), OutType.AbiTypes, OutErrorSource, false)
		|| !ReadEnumValues(Object, OutType.Kind, OutType.EnumValues, OutErrorSource)
		|| !IsLowerSha256(OutType.StableId)
		|| OutType.StableId != FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
			OutType.CanonicalType,
			OutType.EnumValues)
		|| (OutType.Kind == TEXT("enum")) != OutType.CanonicalType.StartsWith(TEXT("enum:"))
		|| OutType.Size <= 0
		|| OutType.Alignment <= 0)
	{
		return false;
	}
	return true;
}

bool ParseValue(
	const TSharedPtr<FJsonObject>& Object,
	FAvidScriptBindingValueModel& OutValue,
	FString& OutErrorSource)
{
	if (!ReadRequiredString(Object, TEXT("name"), OutValue.Name, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("direction"), OutValue.Direction, OutErrorSource)
		|| !ReadRequiredBool(Object, TEXT("has_default"), OutValue.bHasDefault, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("canonical_type"), OutValue.CanonicalType, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("type_id"), OutValue.TypeId, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("kind"), OutValue.Kind, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("cpp_type"), OutValue.CppType, OutErrorSource)
		|| !ReadStringArray(Object, TEXT("abi_types"), OutValue.AbiTypes, OutErrorSource, OutValue.CanonicalType == TEXT("void")))
	{
		return false;
	}
	if (OutValue.bHasDefault
		&& (!Object->TryGetStringField(TEXT("default_value"), OutValue.DefaultValue) || OutValue.DefaultValue.IsEmpty()))
	{
		OutErrorSource = TEXT("default_value");
		return false;
	}
	if (!IsLowerSha256(OutValue.TypeId)
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

bool ParseBinding(
	const TSharedPtr<FJsonObject>& Object,
	FAvidScriptBindingFunctionModel& OutBinding,
	FString& OutErrorSource)
{
	if (!ReadRequiredString(Object, TEXT("stable_id"), OutBinding.StableId, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("canonical_identity"), OutBinding.CanonicalIdentity, OutErrorSource)
		|| !ReadRequiredInt(Object, TEXT("ordinal"), OutBinding.Ordinal, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("owner_class"), OutBinding.OwnerClass, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("ue_function"), OutBinding.UeFunction, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("script_name"), OutBinding.ScriptName, OutErrorSource)
		|| !ReadRequiredString(Object, TEXT("dispatch_mode"), OutBinding.DispatchMode, OutErrorSource)
		|| !ReadRequiredBool(Object, TEXT("is_static"), OutBinding.bStatic, OutErrorSource)
		|| !ReadRequiredBool(Object, TEXT("is_const"), OutBinding.bConst, OutErrorSource)
		|| !IsLowerSha256(OutBinding.StableId))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ReturnObject = nullptr;
	if (!Object->TryGetObjectField(TEXT("return"), ReturnObject)
		|| ReturnObject == nullptr
		|| !ReturnObject->IsValid()
		|| !ParseValue(*ReturnObject, OutBinding.ReturnValue, OutErrorSource)
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
		if (!ParseValue(ParameterObject, Parameter, OutErrorSource) || Parameter.Direction == TEXT("return"))
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
		|| !ReadRequiredString(*HostImport, TEXT("module"), OutBinding.HostImport.Module, OutErrorSource)
		|| !ReadRequiredString(*HostImport, TEXT("name"), OutBinding.HostImport.Name, OutErrorSource)
		|| !ReadRequiredString(*HostImport, TEXT("signature"), OutBinding.HostImport.Signature, OutErrorSource))
	{
		OutErrorSource = TEXT("host_import");
		return false;
	}
	return true;
}
} // namespace

bool FAvidScriptEditorBindingDescriptorModelParser::Parse(
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

	if (!ReadRequiredInt(Root, TEXT("schema_version"), OutPackage.SchemaVersion, OutErrorSource)
		|| OutPackage.SchemaVersion != 2
		|| !ReadRequiredString(Root, TEXT("generator_version"), OutPackage.GeneratorVersion, OutErrorSource)
		|| !ReadRequiredString(Root, TEXT("engine_version"), OutPackage.EngineVersion, OutErrorSource)
		|| !ReadRequiredString(Root, TEXT("source"), OutPackage.Source, OutErrorSource)
		|| OutPackage.Source != TEXT("ue_reflection")
		|| !ReadRequiredString(Root, TEXT("package_name"), OutPackage.PackageName, OutErrorSource)
		|| !ReadRequiredString(Root, TEXT("package_hash"), OutPackage.PackageHash, OutErrorSource)
		|| !ReadRequiredString(Root, TEXT("selection_hash"), OutPackage.SelectionHash, OutErrorSource)
		|| !IsLowerSha256(OutPackage.PackageHash)
		|| !IsLowerSha256(OutPackage.SelectionHash))
	{
		OutErrorCategory = TEXT("descriptor_contract_invalid");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
	if (!Root->TryGetArrayField(TEXT("types"), Types) || Types == nullptr || Types->IsEmpty())
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
		if (!ParseType(Value.IsValid() ? Value->AsObject() : nullptr, Type, OutErrorSource)
			|| TypeIds.Contains(Type.StableId)
			|| CanonicalTypes.Contains(Type.CanonicalType))
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			return false;
		}
		TypeIds.Add(Type.StableId);
		CanonicalTypes.Add(Type.CanonicalType);
		TypesByCanonical.Add(Type.CanonicalType, Type);
		OutPackage.Types.Add(MoveTemp(Type));
	}

	const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
	if (!Root->TryGetArrayField(TEXT("bindings"), Bindings) || Bindings == nullptr || Bindings->IsEmpty())
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
		if (!ParseBinding((*Bindings)[Index].IsValid() ? (*Bindings)[Index]->AsObject() : nullptr, Binding, OutErrorSource)
			|| Binding.Ordinal != Index
			|| Binding.DispatchMode != TEXT("cached_process_event")
			|| BindingIds.Contains(Binding.StableId)
			|| Imports.Contains(Binding.HostImport.Module + TEXT(".") + Binding.HostImport.Name))
		{
			OutErrorCategory = TEXT("descriptor_contract_invalid");
			return false;
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
	return true;
}
