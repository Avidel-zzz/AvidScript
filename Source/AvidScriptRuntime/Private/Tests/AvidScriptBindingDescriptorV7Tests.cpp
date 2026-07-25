#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptObjectFactoryPolicy.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
FAvidScriptBindingTypeModel MakeV7ObjectType(
	const TCHAR* ClassPath,
	const TCHAR* CppType,
	const int32 Ordinal,
	const FString& BaseTypeId)
{
	FAvidScriptBindingTypeModel Type;
	Type.CanonicalType = TEXT("object:") + FString(ClassPath);
	Type.StableId =
		FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(Type.CanonicalType, {});
	Type.Kind = TEXT("object_handle");
	Type.CppType = CppType;
	Type.Size = 8;
	Type.Alignment = 4;
	Type.AbiTypes = { TEXT("i"), TEXT("i") };
	Type.ObjectTypeOrdinal = Ordinal;
	Type.ClassPath = ClassPath;
	Type.BaseTypeId = BaseTypeId;
	return Type;
}

FAvidScriptBindingClassReferenceModel MakeV7ClassReference(
	const TCHAR* ScriptName,
	const TCHAR* ClassPath,
	const TCHAR* BaseClassPath,
	const FString& ResultTypeId)
{
	FAvidScriptBindingClassReferenceModel Reference;
	Reference.ScriptName = ScriptName;
	Reference.ClassPath = ClassPath;
	Reference.BaseClassPath = BaseClassPath;
	Reference.LoadPolicy = TEXT("EditorLoad");
	Reference.ResultTypeId = ResultTypeId;
	Reference.StableId =
		FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
			Reference.ClassPath,
			Reference.BaseClassPath,
			Reference.LoadPolicy);
	return Reference;
}

FAvidScriptBindingObjectFactoryModel MakeV7Factory(
	const TCHAR* ScriptName,
	const FString& ClassReferenceId,
	const EAvidScriptObjectFactoryKind Kind,
	const FString& OuterTypeId,
	const EAvidScriptObjectOwnershipPolicy Ownership,
	const EAvidScriptComponentRegistrationPolicy Registration)
{
	FAvidScriptBindingObjectFactoryModel Factory;
	Factory.ScriptName = ScriptName;
	Factory.ClassReferenceId = ClassReferenceId;
	Factory.Kind = Kind;
	Factory.OuterTypeId = OuterTypeId;
	Factory.Ownership = Ownership;
	Factory.Registration = Registration;
	Factory.StableId =
		FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
			Factory.ClassReferenceId,
			Factory.Kind,
			Factory.OuterTypeId,
			Factory.Ownership,
			Factory.Registration);
	return Factory;
}

FAvidScriptBindingPackageModel MakeV7Package()
{
	FAvidScriptBindingPackageModel Package;
	Package.SchemaVersion = 7;
	Package.GeneratorVersion = TEXT("51.1.test");
	Package.EngineVersion =
		FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Package.Source = TEXT("ue_reflection");
	Package.PackageName = TEXT("avidscript.test.descriptor_v7_factories");

	const FAvidScriptBindingTypeModel ObjectType = MakeV7ObjectType(
		TEXT("/Script/CoreUObject.Object"),
		TEXT("UObject"),
		0,
		FString());
	const FAvidScriptBindingTypeModel ActorType = MakeV7ObjectType(
		TEXT("/Script/Engine.Actor"),
		TEXT("AActor"),
		1,
		ObjectType.StableId);
	const FAvidScriptBindingTypeModel ActorComponentType = MakeV7ObjectType(
		TEXT("/Script/Engine.ActorComponent"),
		TEXT("UActorComponent"),
		2,
		ObjectType.StableId);
	Package.Types = { ObjectType, ActorType, ActorComponentType };
	Package.SelfTypeId = ActorType.StableId;

	Package.ClassReferences = {
		MakeV7ClassReference(
			TEXT("ConsoleClass"),
			TEXT("/Script/Engine.Console"),
			TEXT("/Script/CoreUObject.Object"),
			ObjectType.StableId),
		MakeV7ClassReference(
			TEXT("SceneComponentClass"),
			TEXT("/Script/Engine.SceneComponent"),
			TEXT("/Script/Engine.ActorComponent"),
			ActorComponentType.StableId)
	};
	Package.ClassReferences.Sort([](
		const FAvidScriptBindingClassReferenceModel& Left,
		const FAvidScriptBindingClassReferenceModel& Right)
	{
		return Left.StableId < Right.StableId;
	});
	for (int32 Index = 0; Index < Package.ClassReferences.Num(); ++Index)
	{
		Package.ClassReferences[Index].Ordinal = Index;
	}

	const FAvidScriptBindingClassReferenceModel* ConsoleReference =
		Package.ClassReferences.FindByPredicate([](
			const FAvidScriptBindingClassReferenceModel& Reference)
		{
			return Reference.ScriptName == TEXT("ConsoleClass");
		});
	const FAvidScriptBindingClassReferenceModel* ComponentReference =
		Package.ClassReferences.FindByPredicate([](
			const FAvidScriptBindingClassReferenceModel& Reference)
		{
			return Reference.ScriptName == TEXT("SceneComponentClass");
		});
	if (ConsoleReference != nullptr && ComponentReference != nullptr)
	{
		Package.ObjectFactories = {
			MakeV7Factory(
				TEXT("Console"),
				ConsoleReference->StableId,
				EAvidScriptObjectFactoryKind::NewObject,
				ObjectType.StableId,
				EAvidScriptObjectOwnershipPolicy::Session,
				EAvidScriptComponentRegistrationPolicy::None),
			MakeV7Factory(
				TEXT("SceneComponent"),
				ComponentReference->StableId,
				EAvidScriptObjectFactoryKind::ActorComponent,
				ActorType.StableId,
				EAvidScriptObjectOwnershipPolicy::Session,
				EAvidScriptComponentRegistrationPolicy::RegisterInstance)
		};
	}
	Package.ObjectFactories.Sort([](
		const FAvidScriptBindingObjectFactoryModel& Left,
		const FAvidScriptBindingObjectFactoryModel& Right)
	{
		return Left.StableId < Right.StableId;
	});
	for (int32 Index = 0; Index < Package.ObjectFactories.Num(); ++Index)
	{
		Package.ObjectFactories[Index].Ordinal = Index;
	}
	Package.SelectionHash =
		FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
	Package.PackageHash =
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);
	return Package;
}

void WriteV7Type(
	const TSharedRef<TJsonWriter<>>& Writer,
	const FAvidScriptBindingTypeModel& Type)
{
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("stable_id"), Type.StableId);
	Writer->WriteValue(TEXT("canonical_type"), Type.CanonicalType);
	Writer->WriteValue(TEXT("kind"), Type.Kind);
	Writer->WriteValue(TEXT("cpp_type"), Type.CppType);
	Writer->WriteValue(TEXT("size"), Type.Size);
	Writer->WriteValue(TEXT("alignment"), Type.Alignment);
	Writer->WriteArrayStart(TEXT("abi_types"));
	for (const FString& AbiType : Type.AbiTypes)
	{
		Writer->WriteValue(AbiType);
	}
	Writer->WriteArrayEnd();
	Writer->WriteValue(TEXT("object_type_ordinal"), Type.ObjectTypeOrdinal);
	Writer->WriteValue(TEXT("class_path"), Type.ClassPath);
	Writer->WriteValue(TEXT("base_type_id"), Type.BaseTypeId);
	Writer->WriteObjectEnd();
}

bool SerializeV7Package(
	const FAvidScriptBindingPackageModel& Package,
	FString& OutJson)
{
	OutJson.Empty();
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema_version"), Package.SchemaVersion);
	Writer->WriteValue(TEXT("generator_version"), Package.GeneratorVersion);
	Writer->WriteValue(TEXT("engine_version"), Package.EngineVersion);
	Writer->WriteValue(TEXT("source"), Package.Source);
	Writer->WriteValue(TEXT("package_name"), Package.PackageName);
	Writer->WriteValue(TEXT("package_hash"), Package.PackageHash);
	Writer->WriteValue(TEXT("selection_hash"), Package.SelectionHash);
	Writer->WriteValue(TEXT("self_type_id"), Package.SelfTypeId);
	Writer->WriteArrayStart(TEXT("types"));
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		WriteV7Type(Writer, Type);
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("class_references"));
	for (const FAvidScriptBindingClassReferenceModel& Reference : Package.ClassReferences)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("stable_id"), Reference.StableId);
		Writer->WriteValue(TEXT("ordinal"), Reference.Ordinal);
		Writer->WriteValue(TEXT("script_name"), Reference.ScriptName);
		Writer->WriteValue(TEXT("class_path"), Reference.ClassPath);
		Writer->WriteValue(TEXT("base_class_path"), Reference.BaseClassPath);
		Writer->WriteValue(TEXT("load_policy"), Reference.LoadPolicy);
		Writer->WriteValue(TEXT("result_type_id"), Reference.ResultTypeId);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	if (Package.SchemaVersion >= 7)
	{
		Writer->WriteArrayStart(TEXT("object_factories"));
		for (const FAvidScriptBindingObjectFactoryModel& Factory : Package.ObjectFactories)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("stable_id"), Factory.StableId);
			Writer->WriteValue(TEXT("ordinal"), Factory.Ordinal);
			Writer->WriteValue(TEXT("script_name"), Factory.ScriptName);
			Writer->WriteValue(
				TEXT("class_reference_id"),
				Factory.ClassReferenceId);
			Writer->WriteValue(TEXT("kind"), LexToString(Factory.Kind));
			Writer->WriteValue(TEXT("outer_type_id"), Factory.OuterTypeId);
			Writer->WriteValue(TEXT("ownership"), LexToString(Factory.Ownership));
			Writer->WriteValue(
				TEXT("registration"),
				LexToString(Factory.Registration));
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}
	Writer->WriteArrayStart(TEXT("bindings"));
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	return Writer->Close();
}

bool ParseJsonObject(const FString& Json, TSharedPtr<FJsonObject>& OutObject)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

bool SerializeJsonObject(
	const TSharedPtr<FJsonObject>& Object,
	FString& OutJson)
{
	OutJson.Empty();
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	return Object.IsValid() && FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptBindingDescriptorV7ObjectFactoriesTest,
	"AvidScript.Binding.Descriptor.V7ObjectFactories",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBindingDescriptorV7ObjectFactoriesTest::RunTest(
	const FString& Parameters)
{
	const FAvidScriptBindingPackageModel Package = MakeV7Package();
	FString Json;
	TestTrue(TEXT("Canonical descriptor v7 serializes"), SerializeV7Package(Package, Json));

	FAvidScriptBindingPackageModel ParsedPackage;
	FString ErrorCategory;
	FString ErrorSource;
	TestTrue(
		TEXT("Canonical descriptor v7 parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			Json,
			ParsedPackage,
			ErrorCategory,
			ErrorSource));
	TestEqual(TEXT("Descriptor v7 retains two factories"), ParsedPackage.ObjectFactories.Num(), 2);

	const auto ParserRejectsMutation = [
		this,
		&Json](
			const TCHAR* Label,
			const TFunctionRef<void(TSharedPtr<FJsonObject>&)>& Mutate)
	{
		TSharedPtr<FJsonObject> Root;
		FString MutatedJson;
		FAvidScriptBindingPackageModel MutatedPackage;
		FString Category;
		FString Source;
		const bool bRejected =
			ParseJsonObject(Json, Root)
			&& (Mutate(Root), true)
			&& SerializeJsonObject(Root, MutatedJson)
			&& !FAvidScriptBindingDescriptorParser::Parse(
				MutatedJson,
				MutatedPackage,
				Category,
				Source);
		TestTrue(Label, bRejected);
		TestEqual(
			TEXT("Hostile factory table reports descriptor contract failure"),
			Category,
			FString(TEXT("descriptor_contract_invalid")));
	};

	ParserRejectsMutation(
		TEXT("Duplicate factory ordinals fail closed"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			Root->GetArrayField(TEXT("object_factories"))[1]
				->AsObject()
				->SetNumberField(TEXT("ordinal"), 0);
		});
	ParserRejectsMutation(
		TEXT("Non-contiguous factory ordinals fail closed"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			Root->GetArrayField(TEXT("object_factories"))[1]
				->AsObject()
				->SetNumberField(TEXT("ordinal"), 2);
		});
	ParserRejectsMutation(
		TEXT("Dangling factory class reference fails closed"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			const TSharedPtr<FJsonObject> Factory =
				Root->GetArrayField(TEXT("object_factories"))[0]->AsObject();
			const FString MissingId = FString::ChrN(64, TEXT('a'));
			Factory->SetStringField(TEXT("class_reference_id"), MissingId);
			EAvidScriptObjectFactoryKind Kind;
			EAvidScriptObjectOwnershipPolicy Ownership;
			EAvidScriptComponentRegistrationPolicy Registration;
			if (TryParseAvidScriptObjectFactoryKind(
					Factory->GetStringField(TEXT("kind")),
					Kind)
				&& TryParseAvidScriptObjectOwnershipPolicy(
					Factory->GetStringField(TEXT("ownership")),
					Ownership)
				&& TryParseAvidScriptComponentRegistrationPolicy(
					Factory->GetStringField(TEXT("registration")),
					Registration))
			{
				Factory->SetStringField(
					TEXT("stable_id"),
					FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
						MissingId,
						Kind,
						Factory->GetStringField(TEXT("outer_type_id")),
						Ownership,
						Registration));
			}
		});
	ParserRejectsMutation(
		TEXT("Dangling factory outer type fails closed"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			const TSharedPtr<FJsonObject> Factory =
				Root->GetArrayField(TEXT("object_factories"))[0]->AsObject();
			const FString MissingId = FString::ChrN(64, TEXT('b'));
			Factory->SetStringField(TEXT("outer_type_id"), MissingId);
			EAvidScriptObjectFactoryKind Kind;
			EAvidScriptObjectOwnershipPolicy Ownership;
			EAvidScriptComponentRegistrationPolicy Registration;
			if (TryParseAvidScriptObjectFactoryKind(
					Factory->GetStringField(TEXT("kind")),
					Kind)
				&& TryParseAvidScriptObjectOwnershipPolicy(
					Factory->GetStringField(TEXT("ownership")),
					Ownership)
				&& TryParseAvidScriptComponentRegistrationPolicy(
					Factory->GetStringField(TEXT("registration")),
					Registration))
			{
				Factory->SetStringField(
					TEXT("stable_id"),
					FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
						Factory->GetStringField(TEXT("class_reference_id")),
						Kind,
						MissingId,
						Ownership,
						Registration));
			}
		});
	ParserRejectsMutation(
		TEXT("Unknown factory kind fails closed"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			Root->GetArrayField(TEXT("object_factories"))[0]
				->AsObject()
				->SetStringField(TEXT("kind"), TEXT("pooled_object"));
		});
	ParserRejectsMutation(
		TEXT("Unknown factory ownership fails closed"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			Root->GetArrayField(TEXT("object_factories"))[0]
				->AsObject()
				->SetStringField(TEXT("ownership"), TEXT("world"));
		});
	ParserRejectsMutation(
		TEXT("Unknown factory registration fails closed"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			Root->GetArrayField(TEXT("object_factories"))[0]
				->AsObject()
				->SetStringField(TEXT("registration"), TEXT("auto"));
		});
	ParserRejectsMutation(
		TEXT("New object component registration fails closed"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			for (const TSharedPtr<FJsonValue>& Value :
				Root->GetArrayField(TEXT("object_factories")))
			{
				const TSharedPtr<FJsonObject> Factory = Value->AsObject();
				if (Factory->GetStringField(TEXT("kind")) == TEXT("new_object"))
				{
					Factory->SetStringField(
						TEXT("registration"),
						TEXT("register_instance"));
					Factory->SetStringField(
						TEXT("stable_id"),
						FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
							Factory->GetStringField(TEXT("class_reference_id")),
							EAvidScriptObjectFactoryKind::NewObject,
							Factory->GetStringField(TEXT("outer_type_id")),
							EAvidScriptObjectOwnershipPolicy::Session,
							EAvidScriptComponentRegistrationPolicy::RegisterInstance));
					break;
				}
			}
		});
	ParserRejectsMutation(
		TEXT("Unowned non-Actor class references fail closed"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			Root->SetArrayField(
				TEXT("object_factories"),
				TArray<TSharedPtr<FJsonValue>>());
		});
	ParserRejectsMutation(
		TEXT("Actor lifecycle and object factory capabilities cannot overlap"),
		[](TSharedPtr<FJsonObject>& Root)
		{
			FString FactoryClassReferenceId;
			for (const TSharedPtr<FJsonValue>& Value :
				Root->GetArrayField(TEXT("object_factories")))
			{
				const TSharedPtr<FJsonObject> Factory = Value->AsObject();
				if (Factory->GetStringField(TEXT("kind"))
					== TEXT("new_object"))
				{
					FactoryClassReferenceId = Factory->GetStringField(
						TEXT("class_reference_id"));
					break;
				}
			}
			for (const TSharedPtr<FJsonValue>& Value :
				Root->GetArrayField(TEXT("class_references")))
			{
				const TSharedPtr<FJsonObject> Reference = Value->AsObject();
				if (Reference->GetStringField(TEXT("stable_id"))
					== FactoryClassReferenceId)
				{
					Reference->SetStringField(
						TEXT("result_type_id"),
						Root->GetStringField(TEXT("self_type_id")));
					break;
				}
			}
		});

	FAvidScriptBindingPackageModel DifferentFactoryPackage = Package;
	FAvidScriptBindingObjectFactoryModel* DifferentFactory =
		DifferentFactoryPackage.ObjectFactories.FindByPredicate([](
			const FAvidScriptBindingObjectFactoryModel& Factory)
		{
			return Factory.Kind == EAvidScriptObjectFactoryKind::NewObject;
		});
	if (TestNotNull(TEXT("Hash fixture finds the new-object factory"), DifferentFactory))
	{
		DifferentFactory->OuterTypeId = Package.SelfTypeId;
		DifferentFactory->StableId =
			FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
				DifferentFactory->ClassReferenceId,
				DifferentFactory->Kind,
				DifferentFactory->OuterTypeId,
				DifferentFactory->Ownership,
				DifferentFactory->Registration);
	}
	DifferentFactoryPackage.ObjectFactories.Sort([](
		const FAvidScriptBindingObjectFactoryModel& Left,
		const FAvidScriptBindingObjectFactoryModel& Right)
	{
		return Left.StableId < Right.StableId;
	});
	for (int32 Index = 0; Index < DifferentFactoryPackage.ObjectFactories.Num(); ++Index)
	{
		DifferentFactoryPackage.ObjectFactories[Index].Ordinal = Index;
	}
	DifferentFactoryPackage.SelectionHash =
		FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(
			DifferentFactoryPackage);
	DifferentFactoryPackage.PackageHash =
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(
			DifferentFactoryPackage);
	TestNotEqual(
		TEXT("Factory policy changes selection hash"),
		DifferentFactoryPackage.SelectionHash,
		Package.SelectionHash);
	TestNotEqual(
		TEXT("Factory policy changes package hash"),
		DifferentFactoryPackage.PackageHash,
		Package.PackageHash);

	for (int32 LegacySchemaVersion = 2; LegacySchemaVersion <= 6;
		++LegacySchemaVersion)
	{
		TSharedPtr<FJsonObject> LegacyRoot;
		TestTrue(
			*FString::Printf(
				TEXT("Schema v%d hostile fixture clones canonical v7 JSON"),
				LegacySchemaVersion),
			ParseJsonObject(Json, LegacyRoot));
		LegacyRoot->SetNumberField(TEXT("schema_version"), LegacySchemaVersion);
		if (LegacySchemaVersion < 5)
		{
			LegacyRoot->RemoveField(TEXT("class_references"));
		}
		FString HostileLegacyJson;
		ErrorCategory.Empty();
		ErrorSource.Empty();
		TestFalse(
			*FString::Printf(
				TEXT("Schema v%d rejects object_factories"),
				LegacySchemaVersion),
			SerializeJsonObject(LegacyRoot, HostileLegacyJson)
				&& FAvidScriptBindingDescriptorParser::Parse(
					HostileLegacyJson,
					ParsedPackage,
					ErrorCategory,
					ErrorSource));
		TestEqual(
			TEXT("Legacy object factory rejection category is stable"),
			ErrorCategory,
			FString(TEXT("descriptor_contract_invalid")));
		TestEqual(
			TEXT("Legacy object factory rejection source is stable"),
			ErrorSource,
			FString(TEXT("object_factories")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
