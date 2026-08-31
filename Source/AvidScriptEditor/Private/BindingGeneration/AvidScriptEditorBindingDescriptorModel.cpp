#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"

#include "Serialization/JsonWriter.h"

namespace
{
void WriteDescriptorModelStringArray(
	const TSharedRef<TJsonWriter<>>& Writer,
	const TCHAR* Name,
	const TArray<FString>& Values)
{
	Writer->WriteArrayStart(Name);
	for (const FString& Value : Values)
	{
		Writer->WriteValue(Value);
	}
	Writer->WriteArrayEnd();
}

void WriteDescriptorModelBindingValue(
	const TSharedRef<TJsonWriter<>>& Writer,
	const FAvidScriptBindingValueModel& Value)
{
	Writer->WriteValue(TEXT("name"), Value.Name);
	Writer->WriteValue(TEXT("direction"), Value.Direction);
	Writer->WriteValue(TEXT("has_default"), Value.bHasDefault);
	if (Value.bHasDefault)
	{
		Writer->WriteValue(TEXT("default_value"), Value.DefaultValue);
	}
	Writer->WriteValue(TEXT("canonical_type"), Value.CanonicalType);
	Writer->WriteValue(TEXT("type_id"), Value.TypeId);
	Writer->WriteValue(TEXT("kind"), Value.Kind);
	Writer->WriteValue(TEXT("cpp_type"), Value.CppType);
	WriteDescriptorModelStringArray(Writer, TEXT("abi_types"), Value.AbiTypes);
}
} // namespace

bool FAvidScriptEditorBindingDescriptorModelParser::Parse(
	const FString& Json,
	FAvidScriptBindingPackageModel& OutPackage,
	FString& OutErrorCategory,
	FString& OutErrorSource)
{
	return FAvidScriptBindingDescriptorParser::Parse(
		Json,
		OutPackage,
		OutErrorCategory,
		OutErrorSource);
}

bool FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical(
	const FAvidScriptBindingPackageModel& Package,
	FString& OutJson)
{
	OutJson.Empty();
	FString StructWireLayoutError;
	if (Package.SchemaVersion >= 9
		&& !FAvidScriptBindingDescriptorLayout::ValidateTypeGraph(
			Package.Types,
			StructWireLayoutError))
	{
		return false;
	}
	const FString NativeDirectIdentitySuffix =
		FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
			FString(),
			TEXT("qualified_native_direct"));
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		const FString GeneratedIdentitySuffix =
			FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
				FString(),
				TEXT("generated_native_s1"),
				Binding.GeneratedShape,
				Binding.GeneratedReceiverMode,
				Binding.GeneratedImportName);
		if (Binding.BindingKind == TEXT("function")
			&& (!FAvidScriptBindingDescriptorIdentity::IsFunctionDispatchModeSupported(
					Package.SchemaVersion,
					Binding.DispatchMode)
				|| ((Binding.DispatchMode == TEXT("qualified_native_direct"))
					!= Binding.CanonicalIdentity.EndsWith(
						NativeDirectIdentitySuffix,
						ESearchCase::CaseSensitive))
				|| ((Binding.DispatchMode == TEXT("generated_native_s1"))
					!= Binding.CanonicalIdentity.EndsWith(
						GeneratedIdentitySuffix,
						ESearchCase::CaseSensitive))))
		{
			return false;
		}
	}
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema_version"), Package.SchemaVersion);
	Writer->WriteValue(TEXT("generator_version"), Package.GeneratorVersion);
	Writer->WriteValue(TEXT("engine_version"), Package.EngineVersion);
	Writer->WriteValue(TEXT("source"), Package.Source);
	Writer->WriteValue(TEXT("package_name"), Package.PackageName);
	Writer->WriteValue(TEXT("package_hash"), Package.PackageHash);
	Writer->WriteValue(TEXT("selection_hash"), Package.SelectionHash);
	if (Package.SchemaVersion >= 8
		&& !Package.GeneratedSourcePackageHash.IsEmpty())
	{
		Writer->WriteValue(
			TEXT("generated_source_package_hash"),
			Package.GeneratedSourcePackageHash);
	}
	if (Package.SchemaVersion >= 6)
	{
		Writer->WriteValue(TEXT("self_type_id"), Package.SelfTypeId);
		if (Package.bHasActiveObjectTypeOrdinals)
		{
			Writer->WriteArrayStart(TEXT("active_object_type_ordinals"));
			for (const int32 Ordinal : Package.ActiveObjectTypeOrdinals)
			{
				Writer->WriteValue(Ordinal);
			}
			Writer->WriteArrayEnd();
		}
	}

	Writer->WriteArrayStart(TEXT("types"));
	for (const FAvidScriptBindingTypeModel& Type : Package.Types)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("stable_id"), Type.StableId);
		Writer->WriteValue(TEXT("canonical_type"), Type.CanonicalType);
		Writer->WriteValue(TEXT("kind"), Type.Kind);
		Writer->WriteValue(TEXT("cpp_type"), Type.CppType);
		Writer->WriteValue(TEXT("size"), Type.Size);
		Writer->WriteValue(TEXT("alignment"), Type.Alignment);
		WriteDescriptorModelStringArray(Writer, TEXT("abi_types"), Type.AbiTypes);
		if (Package.SchemaVersion >= 6)
		{
			Writer->WriteValue(TEXT("object_type_ordinal"), Type.ObjectTypeOrdinal);
			Writer->WriteValue(TEXT("class_path"), Type.ClassPath);
			Writer->WriteValue(TEXT("base_type_id"), Type.BaseTypeId);
		}
		if (Type.Kind == TEXT("enum"))
		{
			Writer->WriteArrayStart(TEXT("enum_values"));
			for (const FAvidScriptBindingEnumValue& EnumValue : Type.EnumValues)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("name"), EnumValue.Name);
				Writer->WriteValue(TEXT("value"), EnumValue.Value);
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
		}
		if (Package.SchemaVersion >= 9 && Type.Kind == TEXT("struct_wire"))
		{
			Writer->WriteArrayStart(TEXT("fields"));
			for (const FAvidScriptBindingStructFieldModel& Field : Type.StructFields)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("name"), Field.Name);
				Writer->WriteValue(TEXT("type_id"), Field.TypeId);
				Writer->WriteValue(TEXT("wire_offset"), Field.WireOffset);
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
		}
		if (Package.SchemaVersion >= 10 && Type.Kind == TEXT("array"))
		{
			Writer->WriteValue(TEXT("element_type_id"), Type.ElementTypeId);
		}
		if (Package.SchemaVersion >= 19)
		{
			WriteDescriptorModelStringArray(
				Writer,
				TEXT("type_arguments"),
				Type.TypeArguments);
			Writer->WriteValue(TEXT("capability_kind"), Type.CapabilityKind);
		}
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();

	if (Package.SchemaVersion >= 5)
	{
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
			if (Package.SchemaVersion >= 6)
			{
				Writer->WriteValue(TEXT("result_type_id"), Reference.ResultTypeId);
			}
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}

	if (Package.SchemaVersion >= 7)
	{
		Writer->WriteArrayStart(TEXT("object_factories"));
		for (const FAvidScriptBindingObjectFactoryModel& Factory :
			Package.ObjectFactories)
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

	if (Package.SchemaVersion >= 11)
	{
		Writer->WriteArrayStart(TEXT("delegate_events"));
		for (const FAvidScriptBindingDelegateEventModel& Event :
			Package.DelegateEvents)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("stable_id"), Event.StableId);
			Writer->WriteValue(
				TEXT("canonical_identity"),
				Event.CanonicalIdentity);
			Writer->WriteValue(TEXT("ordinal"), Event.Ordinal);
			Writer->WriteValue(TEXT("owner_class"), Event.OwnerClass);
			Writer->WriteValue(TEXT("ue_member"), Event.UeMember);
			Writer->WriteValue(TEXT("script_name"), Event.ScriptName);
			Writer->WriteValue(TEXT("delegate_kind"), Event.DelegateKind);
			if (Package.SchemaVersion >= 18)
			{
				Writer->WriteValue(TEXT("handler_mode"), Event.HandlerMode);
			}
			Writer->WriteValue(TEXT("source_mode"), Event.SourceMode);
			if (Package.SchemaVersion >= 17)
			{
				Writer->WriteValue(
					TEXT("network_mode"),
					LexToString(Event.Network.Mode));
				Writer->WriteValue(
					TEXT("network_reliable"),
					Event.Network.bReliable);
				Writer->WriteValue(
					TEXT("rep_notify_property"),
					Event.RepNotifyProperty.ToString());
			}
			Writer->WriteValue(TEXT("export_name"), Event.ExportName);
			if (Package.SchemaVersion >= 20)
			{
				Writer->WriteObjectStart(TEXT("return"));
				WriteDescriptorModelBindingValue(Writer, Event.ReturnValue);
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayStart(TEXT("parameters"));
			for (const FAvidScriptBindingValueModel& Parameter :
				Event.Parameters)
			{
				Writer->WriteObjectStart();
				WriteDescriptorModelBindingValue(Writer, Parameter);
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}

	Writer->WriteArrayStart(TEXT("bindings"));
	for (const FAvidScriptBindingFunctionModel& Binding : Package.Bindings)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("stable_id"), Binding.StableId);
		Writer->WriteValue(TEXT("canonical_identity"), Binding.CanonicalIdentity);
		Writer->WriteValue(TEXT("ordinal"), Binding.Ordinal);
		Writer->WriteValue(TEXT("owner_class"), Binding.OwnerClass);
		if (Package.SchemaVersion >= 4)
		{
			Writer->WriteValue(TEXT("binding_kind"), Binding.BindingKind);
			Writer->WriteValue(TEXT("ue_member"), Binding.UeMember);
			if (Package.SchemaVersion >= 8
				&& Binding.BindingKind == TEXT("property_set"))
			{
				Writer->WriteValue(TEXT("ue_function"), Binding.UeFunction);
			}
		}
		else
		{
			Writer->WriteValue(TEXT("ue_function"), Binding.UeFunction);
		}
		Writer->WriteValue(TEXT("script_name"), Binding.ScriptName);
		Writer->WriteValue(TEXT("dispatch_mode"), Binding.DispatchMode);
		if (Package.SchemaVersion >= 21)
		{
			Writer->WriteValue(
				TEXT("reflected_owner_kind"),
				Binding.ReflectedOwnerKind);
			Writer->WriteValue(
				TEXT("reflected_owner_asset"),
				Binding.ReflectedOwnerAsset);
			Writer->WriteValue(
				TEXT("reflected_function_fingerprint"),
				Binding.ReflectedFunctionFingerprint);
		}
		if (Package.SchemaVersion >= 12
			&& Binding.DispatchMode == TEXT("latent_process_event"))
		{
			Writer->WriteValue(
				TEXT("latent_info_parameter"),
				Binding.LatentInfoParameter);
			Writer->WriteValue(
				TEXT("world_context_parameter"),
				Binding.WorldContextParameter);
			if (Package.SchemaVersion >= 13)
			{
				Writer->WriteObjectStart(TEXT("completion"));
				Writer->WriteValue(TEXT("mode"), Binding.Completion.Mode);
				Writer->WriteValue(
					TEXT("provider_id"),
					Binding.Completion.ProviderId);
				Writer->WriteValue(
					TEXT("payload_type_id"),
					Binding.Completion.PayloadTypeId);
				Writer->WriteValue(
					TEXT("status_policy"),
					Binding.Completion.StatusPolicy);
				Writer->WriteValue(
					TEXT("cancellable"),
					Binding.Completion.bCancellable);
				Writer->WriteObjectEnd();
			}
		}
		if (Binding.DispatchMode == TEXT("generated_native_s1"))
		{
			Writer->WriteValue(TEXT("generated_shape"), Binding.GeneratedShape);
			Writer->WriteValue(
				TEXT("generated_receiver_mode"),
				Binding.GeneratedReceiverMode);
			Writer->WriteValue(
				TEXT("generated_import_name"),
				Binding.GeneratedImportName);
			Writer->WriteValue(
				TEXT("semantic_fallback_ordinal"),
				Binding.SemanticFallbackOrdinal);
		}
		if (Package.SchemaVersion >= 8)
		{
			Writer->WriteValue(TEXT("write_policy"), Binding.WritePolicy);
		}
		Writer->WriteValue(TEXT("is_static"), Binding.bStatic);
		Writer->WriteValue(TEXT("is_const"), Binding.bConst);
		if (Package.SchemaVersion >= 15)
		{
			Writer->WriteValue(
				TEXT("network_mode"),
				LexToString(Binding.Network.Mode));
			Writer->WriteValue(
				TEXT("network_reliable"),
				Binding.Network.bReliable);
		}
		if (Package.SchemaVersion >= 16)
		{
			Writer->WriteValue(
				TEXT("property_replication"),
				LexToString(Binding.PropertyReplication.Mode));
			Writer->WriteValue(
				TEXT("rep_notify"),
				Binding.PropertyReplication.RepNotifyFunction.ToString());
		}
		if (Package.SchemaVersion >= 3)
		{
			Writer->WriteValue(TEXT("reload_effect"), LexToString(Binding.ReloadEffect));
		}
		Writer->WriteObjectStart(TEXT("return"));
		WriteDescriptorModelBindingValue(Writer, Binding.ReturnValue);
		Writer->WriteObjectEnd();
		Writer->WriteArrayStart(TEXT("parameters"));
		for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
		{
			Writer->WriteObjectStart();
			WriteDescriptorModelBindingValue(Writer, Parameter);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteObjectStart(TEXT("host_import"));
		Writer->WriteValue(TEXT("module"), Binding.HostImport.Module);
		Writer->WriteValue(TEXT("name"), Binding.HostImport.Name);
		Writer->WriteValue(TEXT("signature"), Binding.HostImport.Signature);
		Writer->WriteObjectEnd();
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	if (!Writer->Close())
	{
		OutJson.Empty();
		return false;
	}
	return true;
}
