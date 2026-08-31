#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr TCHAR GeneratorVersion[] = TEXT("1.5");

bool Fail(FString& OutError, const FString& Message)
{
	OutError = Message;
	return false;
}

bool TryGetUInt32Field(const FJsonObject& Object, const TCHAR* FieldName, uint32& OutValue)
{
	double Number = 0.0;
	if (!Object.TryGetNumberField(FieldName, Number)
		|| !FMath::IsFinite(Number)
		|| Number < 0.0
		|| Number > static_cast<double>(MAX_uint32)
		|| Number != FMath::TruncToDouble(Number))
	{
		return false;
	}

	OutValue = static_cast<uint32>(Number);
	return true;
}

bool TryGetRequiredString(const FJsonObject& Object, const TCHAR* FieldName, FString& OutValue)
{
	return Object.TryGetStringField(FieldName, OutValue) && !OutValue.IsEmpty();
}

bool IsModuleName(const FString& Value)
{
	const auto IsAsciiLetter = [](const TCHAR Character)
	{
		return (Character >= TEXT('A') && Character <= TEXT('Z'))
			|| (Character >= TEXT('a') && Character <= TEXT('z'));
	};
	if (Value.IsEmpty() || !(IsAsciiLetter(Value[0]) || Value[0] == TEXT('_')))
	{
		return false;
	}
	for (int32 Index = 1; Index < Value.Len(); ++Index)
	{
		if (!(IsAsciiLetter(Value[Index])
			|| (Value[Index] >= TEXT('0') && Value[Index] <= TEXT('9'))
			|| Value[Index] == TEXT('_')))
		{
			return false;
		}
	}
	return true;
}

bool IsCanonicalExportName(const FString& Value)
{
	constexpr int32 PrefixLength = 8;
	if (!Value.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive)
		|| Value.Len() != PrefixLength + 32)
	{
		return false;
	}
	for (int32 Index = PrefixLength; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool TryGetFlags(
	const FJsonObject& Object,
	TArray<FString>& OutFlags,
	FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.TryGetArrayField(TEXT("flags"), Values) || Values == nullptr)
	{
		return Fail(OutError, TEXT("generated member flags are missing"));
	}

	OutFlags.Reset(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Flag;
		if (!Value.IsValid() || !Value->TryGetString(Flag) || Flag.IsEmpty())
		{
			return Fail(OutError, TEXT("generated member flags contain a non-string or empty value"));
		}
		OutFlags.Add(MoveTemp(Flag));
	}
	return true;
}

bool ClaimIdentity(
	TSet<FString>& Identities,
	const FString& Identity,
	const TCHAR* IdentityKind,
	FString& OutError)
{
	if (Identity.IsEmpty() || Identities.Contains(Identity))
	{
		return Fail(
			OutError,
			FString::Printf(TEXT("generated manifest has an empty or duplicate %s '%s'"), IdentityKind, *Identity));
	}
	Identities.Add(Identity);
	return true;
}

bool ClaimMemberOrdinal(
	FAvidScriptGeneratedTypePlan& TypePlan,
	const uint32 MemberOrdinal,
	FString& OutError)
{
	if (!TypePlan.Members.IsValidIndex(static_cast<int32>(MemberOrdinal))
		|| TypePlan.Members[MemberOrdinal].Kind != EAvidScriptGeneratedMemberKind::Invalid)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("generated type ordinal %u has an invalid or duplicate member ordinal %u"),
				TypePlan.TypeOrdinal,
				MemberOrdinal));
	}
	return true;
}

bool ParseProperty(
	const FJsonObject& Object,
	FAvidScriptGeneratedTypePlan& TypePlan,
	TSet<FString>& StableMemberIds,
	TSet<FString>& ImportNames,
	FString& OutError)
{
	uint32 MemberOrdinal = 0;
	FString StableMemberId;
	FString Name;
	FString GetterImportName;
	FString SetterImportName;
	if (!TryGetUInt32Field(Object, TEXT("member_ordinal"), MemberOrdinal)
		|| !TryGetRequiredString(Object, TEXT("stable_member_id"), StableMemberId)
		|| !TryGetRequiredString(Object, TEXT("name"), Name)
		|| !Object.TryGetStringField(TEXT("getter_import_name"), GetterImportName)
		|| !Object.TryGetStringField(TEXT("setter_import_name"), SetterImportName)
		|| (GetterImportName.IsEmpty() && SetterImportName.IsEmpty()))
	{
		return Fail(OutError, TEXT("generated property fields are missing or invalid"));
	}
	if (!ClaimMemberOrdinal(TypePlan, MemberOrdinal, OutError)
		|| !ClaimIdentity(StableMemberIds, StableMemberId, TEXT("stable member id"), OutError)
		|| (!GetterImportName.IsEmpty()
			&& !ClaimIdentity(ImportNames, GetterImportName, TEXT("property import"), OutError))
		|| (!SetterImportName.IsEmpty()
			&& !ClaimIdentity(ImportNames, SetterImportName, TEXT("property import"), OutError)))
	{
		return false;
	}

	FProperty* const Property = FindFProperty<FProperty>(TypePlan.Class, FName(*Name));
	if (Property == nullptr || Property->GetOwnerClass() != TypePlan.Class)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("generated property '%s' does not resolve on exact class '%s'"),
				*Name,
				*TypePlan.Class->GetPathName()));
	}

	FAvidScriptGeneratedMemberPlan& Member = TypePlan.Members[MemberOrdinal];
	Member.MemberOrdinal = MemberOrdinal;
	Member.Kind = EAvidScriptGeneratedMemberKind::Property;
	Member.StableMemberId = MoveTemp(StableMemberId);
	Member.Property = Property;
	Member.GetterImportName = MoveTemp(GetterImportName);
	Member.SetterImportName = MoveTemp(SetterImportName);
	return true;
}

bool ParseFunction(
	const FJsonObject& Object,
	FAvidScriptGeneratedTypePlan& TypePlan,
	TSet<FString>& StableMemberIds,
	TSet<FString>& ExportNames,
	FString& OutError)
{
	uint32 MemberOrdinal = 0;
	FString StableMemberId;
	FString NativeName;
	FString ExportName;
	TArray<FString> Flags;
	if (!TryGetUInt32Field(Object, TEXT("member_ordinal"), MemberOrdinal)
		|| !TryGetRequiredString(Object, TEXT("stable_member_id"), StableMemberId)
		|| !TryGetRequiredString(Object, TEXT("native_name"), NativeName)
		|| !TryGetRequiredString(Object, TEXT("export_name"), ExportName)
		|| !IsCanonicalExportName(ExportName)
		|| !TryGetFlags(Object, Flags, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("generated function fields are missing or invalid");
		}
		return false;
	}
	if (!ClaimMemberOrdinal(TypePlan, MemberOrdinal, OutError)
		|| !ClaimIdentity(StableMemberIds, StableMemberId, TEXT("stable member id"), OutError)
		|| !ClaimIdentity(ExportNames, ExportName, TEXT("function export"), OutError))
	{
		return false;
	}

	const bool bLifecycle = Flags.Contains(TEXT("lifecycle"));
	UFunction* const Function = TypePlan.Class->FindFunctionByName(FName(*NativeName));
	if (!bLifecycle && Function == nullptr)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("generated function '%s' does not resolve on class '%s'"),
				*NativeName,
				*TypePlan.Class->GetPathName()));
	}

	FAvidScriptGeneratedMemberPlan& Member = TypePlan.Members[MemberOrdinal];
	Member.MemberOrdinal = MemberOrdinal;
	Member.Kind = EAvidScriptGeneratedMemberKind::Function;
	Member.StableMemberId = MoveTemp(StableMemberId);
	Member.Function = Function;
	Member.ExportName = MoveTemp(ExportName);
	Member.bLifecycle = bLifecycle;
	return true;
}
}

const FAvidScriptGeneratedMemberPlan* FAvidScriptGeneratedTypePlan::FindMember(
	const uint32 MemberOrdinal) const
{
	if (!Members.IsValidIndex(static_cast<int32>(MemberOrdinal)))
	{
		return nullptr;
	}
	const FAvidScriptGeneratedMemberPlan& Member = Members[MemberOrdinal];
	return Member.Kind != EAvidScriptGeneratedMemberKind::Invalid ? &Member : nullptr;
}

const FAvidScriptGeneratedTypePlan* FAvidScriptGeneratedTypeRegistrySnapshot::FindTypeByOrdinal(
	const uint32 TypeOrdinal) const
{
	return Types.IsValidIndex(static_cast<int32>(TypeOrdinal)) ? &Types[TypeOrdinal] : nullptr;
}

const FAvidScriptGeneratedTypePlan* FAvidScriptGeneratedTypeRegistrySnapshot::FindTypeByStableId(
	const FString& StableTypeId) const
{
	const uint32* const TypeOrdinal = TypeOrdinalsByStableId.Find(StableTypeId);
	return TypeOrdinal != nullptr ? FindTypeByOrdinal(*TypeOrdinal) : nullptr;
}

const FAvidScriptGeneratedTypePlan* FAvidScriptGeneratedTypeRegistrySnapshot::FindTypeByClass(
	const UClass* Class) const
{
	const uint32* const TypeOrdinal = TypeOrdinalsByClass.Find(Class);
	return TypeOrdinal != nullptr ? FindTypeByOrdinal(*TypeOrdinal) : nullptr;
}

TConstArrayView<FAvidScriptGeneratedTypePlan>
FAvidScriptGeneratedTypeRegistrySnapshot::GetTypes() const
{
	return Types;
}

int32 FAvidScriptGeneratedTypeRegistrySnapshot::Num() const
{
	return Types.Num();
}

bool FAvidScriptGeneratedTypeRegistry::BuildFromJson(
	const FString& Json,
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& OutSnapshot,
	FString& OutError)
{
	OutSnapshot.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		return Fail(OutError, TEXT("generated type registry must be built on the GameThread"));
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return Fail(OutError, TEXT("generated type manifest is not valid JSON"));
	}

	uint32 SchemaVersion = 0;
	FString ParsedGeneratorVersion;
	FString ModuleName;
	const TArray<TSharedPtr<FJsonValue>>* TypeValues = nullptr;
	if (!TryGetUInt32Field(*Root, TEXT("schema_version"), SchemaVersion)
		|| SchemaVersion != ManifestSchemaVersion
		|| !TryGetRequiredString(*Root, TEXT("generator_version"), ParsedGeneratorVersion)
		|| ParsedGeneratorVersion != GeneratorVersion
		|| !TryGetRequiredString(*Root, TEXT("module_name"), ModuleName)
		|| !IsModuleName(ModuleName)
		|| !Root->TryGetArrayField(TEXT("types"), TypeValues)
		|| TypeValues == nullptr
		|| TypeValues->Num() > MaxTypeCount)
	{
		return Fail(OutError, TEXT("generated type manifest root contract is unsupported or invalid"));
	}

	TSharedRef<FAvidScriptGeneratedTypeRegistrySnapshot> Snapshot =
		MakeShared<FAvidScriptGeneratedTypeRegistrySnapshot>();
	Snapshot->Types.Reserve(TypeValues->Num());
	TSet<FString> StableMemberIds;
	TSet<FString> ImportNames;
	TSet<FString> ExportNames;
	for (int32 TypeIndex = 0; TypeIndex < TypeValues->Num(); ++TypeIndex)
	{
		const TSharedPtr<FJsonObject> TypeObject = (*TypeValues)[TypeIndex].IsValid()
			? (*TypeValues)[TypeIndex]->AsObject()
			: nullptr;
		uint32 TypeOrdinal = 0;
		FString StableTypeId;
		FString EngineName;
		FString ClassPath;
		const TArray<TSharedPtr<FJsonValue>>* PropertyValues = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* FunctionValues = nullptr;
		if (!TypeObject.IsValid()
			|| !TryGetUInt32Field(*TypeObject, TEXT("type_ordinal"), TypeOrdinal)
			|| TypeOrdinal != static_cast<uint32>(TypeIndex)
			|| !TryGetRequiredString(*TypeObject, TEXT("stable_type_id"), StableTypeId)
			|| !TryGetRequiredString(*TypeObject, TEXT("engine_name"), EngineName)
			|| !TryGetRequiredString(*TypeObject, TEXT("class_path"), ClassPath)
			|| ClassPath != FString::Printf(TEXT("/Script/%s.%s"), *ModuleName, *EngineName)
			|| !TypeObject->TryGetArrayField(TEXT("properties"), PropertyValues)
			|| PropertyValues == nullptr
			|| !TypeObject->TryGetArrayField(TEXT("functions"), FunctionValues)
			|| FunctionValues == nullptr
			|| static_cast<int64>(PropertyValues->Num()) + FunctionValues->Num()
				> MaxMemberCountPerType)
		{
			return Fail(
				OutError,
				FString::Printf(TEXT("generated type entry %d is invalid or not densely ordered"), TypeIndex));
		}
		if (Snapshot->TypeOrdinalsByStableId.Contains(StableTypeId))
		{
			return Fail(OutError, FString::Printf(TEXT("duplicate stable type id '%s'"), *StableTypeId));
		}

		UClass* const Class = FindObject<UClass>(nullptr, *ClassPath);
		if (Class == nullptr || Class->GetPathName() != ClassPath || Snapshot->TypeOrdinalsByClass.Contains(Class))
		{
			return Fail(OutError, FString::Printf(TEXT("generated class path '%s' cannot be resolved uniquely"), *ClassPath));
		}

		FAvidScriptGeneratedTypePlan TypePlan;
		TypePlan.TypeOrdinal = TypeOrdinal;
		TypePlan.StableTypeId = StableTypeId;
		TypePlan.Class = Class;
		TypePlan.Members.SetNum(PropertyValues->Num() + FunctionValues->Num());
		for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertyValues)
		{
			const TSharedPtr<FJsonObject> PropertyObject = PropertyValue.IsValid()
				? PropertyValue->AsObject()
				: nullptr;
			if (!PropertyObject.IsValid())
			{
				return Fail(OutError, TEXT("generated property entry is not an object"));
			}
			if (!ParseProperty(*PropertyObject, TypePlan, StableMemberIds, ImportNames, OutError))
			{
				return false;
			}
		}
		for (const TSharedPtr<FJsonValue>& FunctionValue : *FunctionValues)
		{
			const TSharedPtr<FJsonObject> FunctionObject = FunctionValue.IsValid()
				? FunctionValue->AsObject()
				: nullptr;
			if (!FunctionObject.IsValid())
			{
				return Fail(OutError, TEXT("generated function entry is not an object"));
			}
			if (!ParseFunction(*FunctionObject, TypePlan, StableMemberIds, ExportNames, OutError))
			{
				return false;
			}
		}
		for (const FAvidScriptGeneratedMemberPlan& Member : TypePlan.Members)
		{
			if (Member.Kind == EAvidScriptGeneratedMemberKind::Invalid)
			{
				return Fail(
					OutError,
					FString::Printf(TEXT("generated type ordinal %u has a sparse member table"), TypeOrdinal));
			}
		}

		Snapshot->TypeOrdinalsByStableId.Add(StableTypeId, TypeOrdinal);
		Snapshot->TypeOrdinalsByClass.Add(Class, TypeOrdinal);
		Snapshot->Types.Add(MoveTemp(TypePlan));
	}

	OutSnapshot = Snapshot;
	return true;
}
