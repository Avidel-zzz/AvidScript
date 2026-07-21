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
	if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
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

bool ParseAvidScriptBindingType(
	const TSharedPtr<FJsonObject>& Object,
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
		|| !IsAvidScriptBindingLowerSha256(OutType.StableId)
		|| OutType.StableId != FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
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
			|| (OutBinding.BindingKind != TEXT("function") && OutBinding.BindingKind != TEXT("property_get")))
		{
			OutErrorSource = TEXT("binding_kind");
			return false;
		}
		if (OutBinding.BindingKind == TEXT("function"))
		{
			OutBinding.UeFunction = OutBinding.UeMember;
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
} // namespace

FString FAvidScriptBindingDescriptorIdentity::MakeTypeIdentity(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues)
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
	return Identity;
}

FString FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(
	const FString& CanonicalType,
	const TArray<FAvidScriptBindingEnumValue>& EnumValues)
{
	return FAvidScriptHash::Sha256HexUtf8(MakeTypeIdentity(CanonicalType, EnumValues));
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
		|| (OutPackage.SchemaVersion != 2 && OutPackage.SchemaVersion != 3 && OutPackage.SchemaVersion != 4)
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
		if (!ParseAvidScriptBindingType(Value.IsValid() ? Value->AsObject() : nullptr, Type, OutErrorSource)
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
		if (!ParseAvidScriptBindingFunction(
				(*Bindings)[Index].IsValid() ? (*Bindings)[Index]->AsObject() : nullptr,
				OutPackage.SchemaVersion,
				Binding,
				OutErrorSource)
			|| Binding.Ordinal != Index
			|| (Binding.BindingKind == TEXT("function") && Binding.DispatchMode != TEXT("cached_process_event"))
			|| (Binding.BindingKind == TEXT("property_get") && Binding.DispatchMode != TEXT("cached_property_get"))
			|| (Binding.BindingKind == TEXT("property_get")
				&& (Binding.bStatic
					|| !Binding.bConst
					|| Binding.ReloadEffect != EAvidScriptBindingReloadEffect::None
					|| !Binding.Parameters.IsEmpty()
					|| Binding.ReturnValue.CanonicalType == TEXT("void")
					|| Binding.HostImport.Signature != TEXT("(iii)i")))
			|| Binding.StableId != FAvidScriptHash::Sha256HexUtf8(Binding.CanonicalIdentity)
			|| Binding.HostImport.Module != TEXT("avidscript")
			|| Binding.HostImport.Name != TEXT("avid_ue_") + Binding.StableId.Left(16)
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
