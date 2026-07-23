#include "AvidScriptEditorBindingSchemaGenerator.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
struct FResolvedBinding
{
	FAvidScriptReflectedBindingSpec Spec;
	UClass* OwnerClass = nullptr;
	UFunction* Function = nullptr;
};

void SetSchemaFailure(
	FAvidScriptBindingSchemaGenerateResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& NextAction)
{
	OutResult = FAvidScriptBindingSchemaGenerateResult();
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript binding schema error | category=%s | source=%s | next=%s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source,
		*NextAction);
}

FString GetScriptFunctionName(const UFunction* Function)
{
	FString ScriptName = Function->GetMetaData(TEXT("ScriptName"));
	if (!ScriptName.IsEmpty())
	{
		return ScriptName;
	}

	ScriptName = Function->GetName();
	ScriptName.RemoveFromStart(TEXT("K2_"));
	return ScriptName;
}

bool DescribeProperty(const FProperty* Property, FString& OutType)
{
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		OutType = ObjectProperty->PropertyClass->GetPrefixCPP() + ObjectProperty->PropertyClass->GetName();
		return true;
	}

	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		OutType = StructProperty->Struct->GetStructCPPName();
		return true;
	}

	if (Property->IsA<FBoolProperty>())
	{
		OutType = TEXT("bool");
		return true;
	}

	if (Property->IsA<FFloatProperty>())
	{
		OutType = TEXT("float");
		return true;
	}

	if (Property->IsA<FDoubleProperty>())
	{
		OutType = TEXT("double");
		return true;
	}

	if (Property->IsA<FIntProperty>())
	{
		OutType = TEXT("int32");
		return true;
	}

	if (Property->IsA<FInt64Property>())
	{
		OutType = TEXT("int64");
		return true;
	}

	if (Property->IsA<FEnumProperty>() || Property->IsA<FByteProperty>())
	{
		OutType = Property->GetCPPType();
		return !OutType.IsEmpty();
	}

	return false;
}

void WriteIntrinsic(
	const TSharedRef<TJsonWriter<>>& Writer,
	const TCHAR* ImportModule,
	const TCHAR* ImportName,
	const TCHAR* AbiSignature,
	const TCHAR* Kind)
{
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("import_module"), ImportModule);
	Writer->WriteValue(TEXT("import_name"), ImportName);
	Writer->WriteValue(TEXT("abi_signature"), AbiSignature);
	Writer->WriteValue(TEXT("kind"), Kind);
	Writer->WriteObjectEnd();
}
} // namespace

TArray<FAvidScriptReflectedBindingSpec> FAvidScriptEditorBindingSchemaGenerator::MakeDefaultSpecs()
{
	return {
		{ TEXT("/Script/Engine.Actor"), TEXT("K2_GetActorLocation"), TEXT("env"), TEXT("actor_get_location"), TEXT("(iii)i"), TEXT("struct_out") },
		{ TEXT("/Script/Engine.Actor"), TEXT("K2_SetActorLocation"), TEXT("env"), TEXT("actor_set_location"), TEXT("(iifff)i"), TEXT("custom_wrapper") },
		{ TEXT("/Script/Engine.Actor"), TEXT("K2_AddActorWorldOffset"), TEXT("env"), TEXT("actor_add_location_offset"), TEXT("(iifff)i"), TEXT("custom_wrapper") },
		{ TEXT("/Script/Engine.Actor"), TEXT("K2_GetActorRotation"), TEXT("env"), TEXT("actor_get_rotation"), TEXT("(iii)i"), TEXT("struct_out") },
		{ TEXT("/Script/Engine.Actor"), TEXT("K2_SetActorRotation"), TEXT("env"), TEXT("actor_set_rotation"), TEXT("(iifff)i"), TEXT("custom_wrapper") },
		{ TEXT("/Script/Engine.Actor"), TEXT("GetActorScale3D"), TEXT("env"), TEXT("actor_get_scale"), TEXT("(iii)i"), TEXT("struct_out") },
		{ TEXT("/Script/Engine.Actor"), TEXT("SetActorScale3D"), TEXT("env"), TEXT("actor_set_scale"), TEXT("(iifff)i"), TEXT("custom_wrapper") },
		{ TEXT("/Script/Engine.Actor"), TEXT("K2_GetRootComponent"), TEXT("env"), TEXT("actor_get_root_component"), TEXT("(iii)i"), TEXT("object_handle_out") },
		{ TEXT("/Script/Engine.SceneComponent"), TEXT("K2_GetComponentLocation"), TEXT("env"), TEXT("scene_component_get_world_location"), TEXT("(iii)i"), TEXT("struct_out") },
		{ TEXT("/Script/Engine.SceneComponent"), TEXT("K2_SetWorldLocation"), TEXT("env"), TEXT("scene_component_set_world_location"), TEXT("(iifff)i"), TEXT("custom_wrapper") }
	};
}

bool FAvidScriptEditorBindingSchemaGenerator::Generate(
	const TArray<FAvidScriptReflectedBindingSpec>& Specs,
	FString& OutJson,
	FAvidScriptBindingSchemaGenerateResult& OutResult)
{
	OutJson.Empty();
	OutResult = FAvidScriptBindingSchemaGenerateResult();

	TArray<FAvidScriptReflectedBindingSpec> SortedSpecs = Specs;
	SortedSpecs.Sort([](const FAvidScriptReflectedBindingSpec& Left, const FAvidScriptReflectedBindingSpec& Right)
	{
		const int32 ModuleComparison = Left.ImportModule.Compare(Right.ImportModule, ESearchCase::CaseSensitive);
		return ModuleComparison == 0
			? Left.ImportName.Compare(Right.ImportName, ESearchCase::CaseSensitive) < 0
			: ModuleComparison < 0;
	});

	TArray<FResolvedBinding> ResolvedBindings;
	ResolvedBindings.Reserve(SortedSpecs.Num());
	TSet<FString> SeenImports;
	for (const FAvidScriptReflectedBindingSpec& Spec : SortedSpecs)
	{
		const FString ImportKey = Spec.ImportModule + TEXT(".") + Spec.ImportName;
		if (SeenImports.Contains(ImportKey))
		{
			SetSchemaFailure(OutResult, TEXT("duplicate_import"), ImportKey, TEXT("Keep exactly one binding declaration for each module and import name pair."));
			return false;
		}
		SeenImports.Add(ImportKey);

		UClass* OwnerClass = LoadObject<UClass>(nullptr, *Spec.OwnerClassPath);
		if (OwnerClass == nullptr)
		{
			SetSchemaFailure(OutResult, TEXT("class_missing"), Spec.OwnerClassPath, TEXT("Use a loaded reflected UClass path from the active UE5.8 build."));
			return false;
		}

		UFunction* Function = OwnerClass->FindFunctionByName(Spec.FunctionName);
		if (Function == nullptr)
		{
			SetSchemaFailure(
				OutResult,
				TEXT("function_missing"),
				Spec.OwnerClassPath + TEXT(".") + Spec.FunctionName.ToString(),
				TEXT("Update the allowlist to a reflected UFunction available in the active engine version."));
			return false;
		}

		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}
			FString Type;
			if (!DescribeProperty(Property, Type))
			{
				SetSchemaFailure(
					OutResult,
					TEXT("unsupported_property"),
					Spec.OwnerClassPath + TEXT(".") + Spec.FunctionName.ToString() + TEXT(".") + Property->GetName(),
					TEXT("Add an explicit static ABI projection for this UE property type before exposing it."));
				return false;
			}
		}

		ResolvedBindings.Add({ Spec, OwnerClass, Function });
	}

	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema_version"), 1);
	Writer->WriteValue(TEXT("source"), TEXT("ue_reflection"));
	Writer->WriteArrayStart(TEXT("bindings"));
	for (const FResolvedBinding& Binding : ResolvedBindings)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("owner_class"), Binding.OwnerClass->GetPathName());
		Writer->WriteValue(TEXT("ue_function"), Binding.Function->GetName());
		Writer->WriteValue(TEXT("script_name"), GetScriptFunctionName(Binding.Function));
		Writer->WriteValue(TEXT("import_module"), Binding.Spec.ImportModule);
		Writer->WriteValue(TEXT("import_name"), Binding.Spec.ImportName);
		Writer->WriteValue(TEXT("abi_signature"), Binding.Spec.AbiSignature);
		Writer->WriteValue(TEXT("projection"), Binding.Spec.Projection);

		const FProperty* ReturnProperty = Binding.Function->GetReturnProperty();
		FString ReturnType = TEXT("void");
		if (ReturnProperty != nullptr)
		{
			DescribeProperty(ReturnProperty, ReturnType);
		}
		Writer->WriteValue(TEXT("return_type"), ReturnType);
		Writer->WriteArrayStart(TEXT("parameters"));
		for (TFieldIterator<FProperty> It(Binding.Function); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Parm) || Property->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			FString Type;
			DescribeProperty(Property, Type);
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("name"), Property->GetName());
			Writer->WriteValue(TEXT("type"), Type);
			Writer->WriteValue(TEXT("out"), Property->HasAnyPropertyFlags(CPF_OutParm));
			Writer->WriteValue(TEXT("reference"), Property->HasAnyPropertyFlags(CPF_ReferenceParm));
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("intrinsics"));
	WriteIntrinsic(Writer, TEXT("avidscript"), TEXT("avid_owner_get_handle"), TEXT("()I"), TEXT("host_context"));
	WriteIntrinsic(Writer, TEXT("env"), TEXT("owner_get_generation"), TEXT("()i"), TEXT("host_context"));
	WriteIntrinsic(Writer, TEXT("env"), TEXT("owner_get_slot"), TEXT("()i"), TEXT("host_context"));
	WriteIntrinsic(Writer, TEXT("env"), TEXT("timer_cancel"), TEXT("(i)i"), TEXT("runtime_service"));
	WriteIntrinsic(Writer, TEXT("env"), TEXT("timer_set_once"), TEXT("(fi)i"), TEXT("runtime_service"));
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	if (!Writer->Close())
	{
		OutJson.Empty();
		SetSchemaFailure(OutResult, TEXT("serialize_failed"), TEXT("default_binding_schema"), TEXT("Inspect the reflected metadata and JSON writer state."));
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.BindingCount = ResolvedBindings.Num();
	OutResult.IntrinsicCount = 4;
	return true;
}

bool FAvidScriptEditorBindingSchemaGenerator::GenerateDefault(
	FString& OutJson,
	FAvidScriptBindingSchemaGenerateResult& OutResult)
{
	return Generate(MakeDefaultSpecs(), OutJson, OutResult);
}

bool FAvidScriptEditorBindingSchemaGenerator::WriteDefault(
	const FString& OutputPath,
	FAvidScriptBindingSchemaGenerateResult& OutResult)
{
	FString Json;
	if (!GenerateDefault(Json, OutResult))
	{
		return false;
	}

	const FString FullOutputPath = FPaths::ConvertRelativePathToFull(OutputPath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullOutputPath), true);
	const FString TemporaryPath = FullOutputPath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Json, *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		SetSchemaFailure(OutResult, TEXT("write_failed"), TemporaryPath, TEXT("Verify that the generated schema directory is writable."));
		return false;
	}

	if (!IFileManager::Get().Move(*FullOutputPath, *TemporaryPath, true, true, false, true))
	{
		IFileManager::Get().Delete(*TemporaryPath);
		SetSchemaFailure(OutResult, TEXT("write_failed"), FullOutputPath, TEXT("Close readers of the previous schema and retry generation."));
		return false;
	}

	OutResult.OutputPath = FullOutputPath;
	return true;
}
bool FAvidScriptEditorBindingSchemaGenerator::ValidateManifestImports(
	const FString& ManifestPath,
	FAvidScriptBindingSchemaGenerateResult& OutResult)
{
	FString SchemaJson;
	if (!GenerateDefault(SchemaJson, OutResult))
	{
		return false;
	}

	FString ManifestJson;
	if (!FFileHelper::LoadFileToString(ManifestJson, *ManifestPath))
	{
		SetSchemaFailure(OutResult, TEXT("manifest_read_failed"), ManifestPath, TEXT("Generate the C# guest manifest before validating its binding contract."));
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		SetSchemaFailure(OutResult, TEXT("manifest_invalid"), ManifestPath, TEXT("Repair the guest manifest JSON and regenerate it from the source adapter."));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* RequiredImports = nullptr;
	if (!Root->TryGetArrayField(TEXT("required_imports"), RequiredImports) || RequiredImports == nullptr)
	{
		SetSchemaFailure(OutResult, TEXT("manifest_invalid"), ManifestPath + TEXT(".required_imports"), TEXT("Add the required_imports array to the guest manifest."));
		return false;
	}

	TSet<FString> SupportedImports;
	for (const FAvidScriptReflectedBindingSpec& Spec : MakeDefaultSpecs())
	{
		SupportedImports.Add(Spec.ImportModule + TEXT(".") + Spec.ImportName);
	}
	SupportedImports.Add(TEXT("env.owner_get_generation"));
	SupportedImports.Add(TEXT("env.owner_get_slot"));
	SupportedImports.Add(TEXT("env.timer_cancel"));
	SupportedImports.Add(TEXT("env.timer_set_once"));
	SupportedImports.Add(TEXT("avidscript.avid_owner_get_handle"));

	for (int32 Index = 0; Index < RequiredImports->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& ImportValue = (*RequiredImports)[Index];
		if (!ImportValue.IsValid() || ImportValue->Type != EJson::Object)
		{
			SetSchemaFailure(
				OutResult,
				TEXT("manifest_invalid"),
				FString::Printf(TEXT("%s.required_imports[%d]"), *ManifestPath, Index),
				TEXT("Each required import must be a JSON object."));
			return false;
		}
		const TSharedPtr<FJsonObject> Import = ImportValue->AsObject();
		FString Module;
		FString Name;
		if (!Import.IsValid()
			|| !Import->TryGetStringField(TEXT("module"), Module)
			|| !Import->TryGetStringField(TEXT("name"), Name)
			|| Module.IsEmpty()
			|| Name.IsEmpty())
		{
			SetSchemaFailure(
				OutResult,
				TEXT("manifest_invalid"),
				FString::Printf(TEXT("%s.required_imports[%d]"), *ManifestPath, Index),
				TEXT("Each required import must contain non-empty module and name strings."));
			return false;
		}

		const FString ImportKey = Module + TEXT(".") + Name;
		if (!SupportedImports.Contains(ImportKey))
		{
			SetSchemaFailure(
				OutResult,
				TEXT("binding_contract_mismatch"),
				ImportKey,
				TEXT("Add an explicit reflected binding projection or remove the unsupported guest import."));
			return false;
		}
	}

	OutResult.bSucceeded = true;
	return true;
}
