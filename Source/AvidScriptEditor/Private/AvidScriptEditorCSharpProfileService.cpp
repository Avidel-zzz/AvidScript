#include "AvidScriptEditorCSharpProfileService.h"

#include "AvidScriptEditorBindingDescriptorGenerator.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
constexpr const TCHAR* AvidScriptDefaultCSharpProfileTemplateModuleId = TEXT("csharp_profile_actor_lifecycle");
constexpr const TCHAR* AvidScriptDefaultCSharpProfileTemplateArtifactStem = TEXT("profile_actor_lifecycle");
constexpr const TCHAR* AvidScriptNativeDirectAuthorizationSemantics =
	TEXT("native_direct_functions asserts standard UHT exec-thunk provenance, accepts bypassing inherited/custom ProcessEvent, and must never auto-authorize Engine or third-party functions");
constexpr const TCHAR* AvidScriptGeneratedNativeAuthorizationSemantics =
	TEXT("generated_native_functions explicitly authorizes project-owned generated S1 call sites and must remain a selected, non-wildcard function subset");

FString NormalizeAvidScriptCSharpProfilePath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}

	return Path;
}

FString NormalizeAvidScriptCSharpProfileFieldPath(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return FString();
	}

	FString Candidate = Path;
	FPaths::NormalizeFilename(Candidate);
	if (FPaths::IsRelative(Candidate))
	{
		Candidate = FPaths::Combine(FPaths::ProjectDir(), Candidate);
	}

	return NormalizeAvidScriptCSharpProfilePath(Candidate);
}

void SetAvidScriptCSharpProfileFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorCSharpProfileLoadResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
}

bool TryGetAvidScriptCSharpProfileStringField(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName,
	FString& OutValue)
{
	OutValue.Reset();
	if (!Object.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonValue> Value = Object->TryGetField(FieldName);
	return Value.IsValid()
		&& Value->Type == EJson::String
		&& Value->TryGetString(OutValue)
		&& !OutValue.IsEmpty();
}

TSharedPtr<FJsonObject> TryGetAvidScriptCSharpProfileObjectValue(
	const TSharedPtr<FJsonValue>& Value)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	return Value.IsValid()
		&& Value->TryGetObject(Object)
		&& Object != nullptr
		? *Object
		: nullptr;
}

bool TryGetAvidScriptCSharpProfileStringArray(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName,
	TArray<FString>& OutValues,
	FAvidScriptEditorCSharpProfileLoadResult& OutResult)
{
	OutValues.Empty();
	if (!Object.IsValid() || !Object->HasField(FieldName))
	{
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values) || Values == nullptr)
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("binding_profile_array_invalid"),
			FString::Printf(TEXT("C# binding_profile field %s must be an array."), FieldName),
			TEXT("replace the field with an array of non-empty strings"),
			OutResult);
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Text;
		if (!Value.IsValid()
			|| Value->Type != EJson::String
			|| !Value->TryGetString(Text)
			|| Text.IsEmpty())
		{
			SetAvidScriptCSharpProfileFailure(
				TEXT("binding_profile_array_value_invalid"),
				FString::Printf(TEXT("C# binding_profile field %s contains a non-string or empty value."), FieldName),
				TEXT("keep only non-empty string values in the array"),
				OutResult);
			return false;
		}
		OutValues.Add(MoveTemp(Text));
	}
	return true;
}

bool TryGetAvidScriptCSharpProfileNameArray(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName,
	TArray<FName>& OutValues,
	FAvidScriptEditorCSharpProfileLoadResult& OutResult)
{
	TArray<FString> StringValues;
	if (!TryGetAvidScriptCSharpProfileStringArray(Object, FieldName, StringValues, OutResult))
	{
		return false;
	}
	OutValues.Empty();
	for (const FString& Value : StringValues)
	{
		OutValues.Add(FName(*Value));
	}
	return true;
}

bool ParseAvidScriptCSharpProjectBindingProfile(
	const TSharedPtr<FJsonObject>& Object,
	const int32 SchemaVersion,
	FAvidScriptEditorCSharpProfileLoadResult& OutResult)
{
	if (!Object.IsValid())
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("binding_profile_invalid"),
			TEXT("C# profile binding_profile must be an object."),
			TEXT("provide a binding_profile object or remove it to use EngineGameplay"),
			OutResult);
		return false;
	}

	FAvidScriptProjectBindingProfileSpec Spec;
	if (!TryGetAvidScriptCSharpProfileStringField(Object, TEXT("package_name"), Spec.PackageName))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("binding_profile_package_missing"),
			TEXT("C# binding_profile requires package_name."),
			TEXT("set a stable package_name for generated bindings"),
			OutResult);
		return false;
	}
	if (!TryGetAvidScriptCSharpProfileStringArray(Object, TEXT("module_paths"), Spec.ModulePaths, OutResult))
	{
		return false;
	}
	if (Object->HasField(TEXT("self_class_path")))
	{
		if (SchemaVersion != 3
			&& SchemaVersion != 4
			&& SchemaVersion != 5
			&& SchemaVersion != 6
			&& SchemaVersion != 7)
		{
			SetAvidScriptCSharpProfileFailure(
				TEXT("profile_self_field_not_supported"),
				TEXT("binding_profile.self_class_path requires C# profile schema_version 3 through 7."),
				TEXT("upgrade the C# profile to schema_version 7 before declaring self_class_path"),
				OutResult);
			return false;
		}
		if (!TryGetAvidScriptCSharpProfileStringField(
				Object,
				TEXT("self_class_path"),
				Spec.SelfClassPath))
		{
			SetAvidScriptCSharpProfileFailure(
				TEXT("binding_profile_self_class_invalid"),
				TEXT("binding_profile.self_class_path must be a non-empty string."),
				TEXT("set self_class_path to a loadable AActor-derived UClass path"),
				OutResult);
			return false;
		}
	}

	if (Object->HasField(TEXT("classes")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
		if (!Object->TryGetArrayField(TEXT("classes"), Classes) || Classes == nullptr)
		{
			SetAvidScriptCSharpProfileFailure(
				TEXT("binding_profile_classes_invalid"),
				TEXT("C# binding_profile classes must be an array."),
				TEXT("replace classes with an array of class selection objects"),
				OutResult);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& ClassValue : *Classes)
		{
			const TSharedPtr<FJsonObject> ClassObject =
				TryGetAvidScriptCSharpProfileObjectValue(ClassValue);
			FAvidScriptReflectedClassSelection ClassSelection;
			if (!ClassObject.IsValid()
				|| !TryGetAvidScriptCSharpProfileStringField(ClassObject, TEXT("class_path"), ClassSelection.OwnerClassPath)
				|| !TryGetAvidScriptCSharpProfileNameArray(ClassObject, TEXT("include_functions"), ClassSelection.IncludeFunctions, OutResult)
				|| !TryGetAvidScriptCSharpProfileNameArray(ClassObject, TEXT("exclude_functions"), ClassSelection.ExcludeFunctions, OutResult)
				|| !TryGetAvidScriptCSharpProfileNameArray(ClassObject, TEXT("include_properties"), ClassSelection.IncludeProperties, OutResult)
				|| !TryGetAvidScriptCSharpProfileNameArray(ClassObject, TEXT("exclude_properties"), ClassSelection.ExcludeProperties, OutResult)
				|| !TryGetAvidScriptCSharpProfileNameArray(ClassObject, TEXT("writable_properties"), ClassSelection.WritableProperties, OutResult))
			{
				if (OutResult.ErrorCategory.IsEmpty())
				{
					SetAvidScriptCSharpProfileFailure(
						TEXT("binding_profile_class_invalid"),
						TEXT("Each C# binding_profile class requires class_path and valid member arrays."),
						TEXT("fix or remove the invalid class selection object"),
						OutResult);
				}
				return false;
			}
			if (ClassObject->HasField(TEXT("native_direct_functions")))
			{
				if (SchemaVersion != 6 && SchemaVersion != 7)
				{
					SetAvidScriptCSharpProfileFailure(
						TEXT("binding_profile_native_direct_schema_unsupported"),
						TEXT("binding_profile classes native_direct_functions requires C# profile schema_version 6 or 7."),
						AvidScriptNativeDirectAuthorizationSemantics,
						OutResult);
					return false;
				}
				if (!TryGetAvidScriptCSharpProfileNameArray(
						ClassObject,
						TEXT("native_direct_functions"),
						ClassSelection.NativeDirectFunctions,
						OutResult))
				{
					const FString Category = OutResult.ErrorCategory;
					SetAvidScriptCSharpProfileFailure(
						Category,
						TEXT("C# binding_profile native_direct_functions must be an array of non-empty strings."),
						AvidScriptNativeDirectAuthorizationSemantics,
						OutResult);
					return false;
				}
			}
			if (ClassObject->HasField(TEXT("generated_native_functions")))
			{
				if (SchemaVersion != 7)
				{
					SetAvidScriptCSharpProfileFailure(
						TEXT("binding_profile_generated_native_schema_unsupported"),
						TEXT("binding_profile classes generated_native_functions requires C# profile schema_version 7."),
						AvidScriptGeneratedNativeAuthorizationSemantics,
						OutResult);
					return false;
				}
				if (!TryGetAvidScriptCSharpProfileNameArray(
						ClassObject,
						TEXT("generated_native_functions"),
						ClassSelection.GeneratedNativeFunctions,
						OutResult))
				{
					const FString Category = OutResult.ErrorCategory;
					SetAvidScriptCSharpProfileFailure(
						Category,
						TEXT("C# binding_profile generated_native_functions must be an array of non-empty strings."),
						AvidScriptGeneratedNativeAuthorizationSemantics,
						OutResult);
					return false;
				}
			}
			if (ClassObject->HasField(TEXT("writable_properties"))
				&& SchemaVersion != 5
				&& SchemaVersion != 6
				&& SchemaVersion != 7)
			{
				SetAvidScriptCSharpProfileFailure(
					TEXT("binding_profile_writable_property_schema_unsupported"),
					TEXT("binding_profile classes writable_properties requires C# profile schema_version 5, 6, or 7."),
					TEXT("upgrade schema_version to 7 or remove writable_properties"),
					OutResult);
				return false;
			}
			if (ClassObject->HasField(TEXT("discover_readable_properties"))
				&& !ClassObject->TryGetBoolField(
					TEXT("discover_readable_properties"),
					ClassSelection.bDiscoverReadableProperties))
			{
				SetAvidScriptCSharpProfileFailure(
					TEXT("binding_profile_discovery_invalid"),
					TEXT("discover_readable_properties must be true or false."),
					TEXT("use a JSON boolean for discover_readable_properties"),
					OutResult);
				return false;
			}
			Spec.Classes.Add(MoveTemp(ClassSelection));
		}
	}

	if (Object->HasField(TEXT("class_references")))
	{
		const TArray<TSharedPtr<FJsonValue>>* ClassReferences = nullptr;
		if (!Object->TryGetArrayField(TEXT("class_references"), ClassReferences) || ClassReferences == nullptr)
		{
			SetAvidScriptCSharpProfileFailure(
				TEXT("binding_profile_class_references_invalid"),
				TEXT("C# binding_profile class_references must be an array."),
				TEXT("replace class_references with an array of class reference objects"),
				OutResult);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& ClassReferenceValue : *ClassReferences)
		{
			const TSharedPtr<FJsonObject> ClassReferenceObject =
				TryGetAvidScriptCSharpProfileObjectValue(ClassReferenceValue);
			FAvidScriptProjectBindingClassSpec ClassReference;
			if (!ClassReferenceObject.IsValid()
				|| !TryGetAvidScriptCSharpProfileStringField(ClassReferenceObject, TEXT("script_name"), ClassReference.ScriptName)
				|| !TryGetAvidScriptCSharpProfileStringField(ClassReferenceObject, TEXT("class_path"), ClassReference.ClassPath)
				|| !TryGetAvidScriptCSharpProfileStringField(ClassReferenceObject, TEXT("base_class_path"), ClassReference.BaseClassPath))
			{
				SetAvidScriptCSharpProfileFailure(
					TEXT("binding_profile_class_reference_invalid"),
					TEXT("Each class reference requires script_name, class_path, and base_class_path."),
					TEXT("complete or remove the invalid class reference object"),
					OutResult);
				return false;
			}
			FString LoadPolicy;
			if (ClassReferenceObject->HasField(TEXT("load_policy")))
			{
				if (!TryGetAvidScriptCSharpProfileStringField(
						ClassReferenceObject,
						TEXT("load_policy"),
						LoadPolicy))
				{
					SetAvidScriptCSharpProfileFailure(
						TEXT("binding_profile_class_reference_load_policy_invalid"),
						TEXT("Class reference load_policy must be a non-empty string."),
						TEXT("use EditorLoad or CookRequired, or omit load_policy to use EditorLoad"),
						OutResult);
					return false;
				}
				ClassReference.LoadPolicy = MoveTemp(LoadPolicy);
			}
			Spec.ClassReferences.Add(MoveTemp(ClassReference));
		}
	}

	if (Object->HasField(TEXT("object_factories")))
	{
		if (SchemaVersion != 4
			&& SchemaVersion != 5
			&& SchemaVersion != 6
			&& SchemaVersion != 7)
		{
			SetAvidScriptCSharpProfileFailure(
				TEXT("binding_profile_factory_schema_unsupported"),
				TEXT("C# binding_profile object_factories requires profile schema_version 4 through 7."),
				TEXT("upgrade the C# profile to schema_version 7 before declaring object_factories"),
				OutResult);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* ObjectFactories = nullptr;
		if (!Object->TryGetArrayField(TEXT("object_factories"), ObjectFactories)
			|| ObjectFactories == nullptr)
		{
			SetAvidScriptCSharpProfileFailure(
				TEXT("binding_profile_object_factories_invalid"),
				TEXT("C# binding_profile object_factories must be an array."),
				TEXT("replace object_factories with an array of factory objects"),
				OutResult);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& FactoryValue : *ObjectFactories)
		{
			const TSharedPtr<FJsonObject> FactoryObject =
				TryGetAvidScriptCSharpProfileObjectValue(FactoryValue);
			FAvidScriptProjectObjectFactorySpec Factory;
			FString Kind;
			FString Ownership;
			if (!FactoryObject.IsValid()
				|| !TryGetAvidScriptCSharpProfileStringField(
					FactoryObject, TEXT("script_name"), Factory.ScriptName)
				|| !TryGetAvidScriptCSharpProfileStringField(
					FactoryObject, TEXT("class_reference"), Factory.ClassReference)
				|| !TryGetAvidScriptCSharpProfileStringField(
					FactoryObject, TEXT("kind"), Kind)
				|| !TryGetAvidScriptCSharpProfileStringField(
					FactoryObject, TEXT("outer_base_class_path"), Factory.OuterBaseClassPath)
				|| !TryGetAvidScriptCSharpProfileStringField(
					FactoryObject, TEXT("ownership"), Ownership))
			{
				SetAvidScriptCSharpProfileFailure(
					TEXT("binding_profile_factory_invalid"),
					TEXT("Each object factory requires non-empty script_name, class_reference, kind, outer_base_class_path, and ownership strings."),
					TEXT("complete or remove the invalid object factory"),
					OutResult);
				return false;
			}

			if (Kind == TEXT("new_object"))
			{
				Factory.Kind = EAvidScriptProjectObjectFactoryKind::NewObject;
			}
			else if (Kind == TEXT("actor_component"))
			{
				Factory.Kind = EAvidScriptProjectObjectFactoryKind::ActorComponent;
			}
			else
			{
				SetAvidScriptCSharpProfileFailure(
					TEXT("binding_profile_factory_kind_invalid"),
					FString::Printf(TEXT("Object factory kind is unsupported: %s"), *Kind),
					TEXT("use new_object or actor_component"),
					OutResult);
				return false;
			}

			if (Ownership != TEXT("session"))
			{
				SetAvidScriptCSharpProfileFailure(
					TEXT("binding_profile_factory_ownership_invalid"),
					FString::Printf(TEXT("Object factory ownership is unsupported: %s"), *Ownership),
					TEXT("use session ownership"),
					OutResult);
				return false;
			}
			Factory.Ownership = EAvidScriptProjectObjectOwnership::Session;

			FString Registration;
			if (FactoryObject->HasField(TEXT("registration")))
			{
				if (!TryGetAvidScriptCSharpProfileStringField(
						FactoryObject,
						TEXT("registration"),
						Registration))
				{
					SetAvidScriptCSharpProfileFailure(
						TEXT("binding_profile_factory_registration_invalid"),
						TEXT("Object factory registration must be a non-empty string."),
						TEXT("use none or register_instance"),
						OutResult);
					return false;
				}
				if (Registration == TEXT("none"))
				{
					Factory.Registration = EAvidScriptProjectComponentRegistration::None;
				}
				else if (Registration == TEXT("register_instance"))
				{
					Factory.Registration =
						EAvidScriptProjectComponentRegistration::RegisterInstance;
				}
				else
				{
					SetAvidScriptCSharpProfileFailure(
						TEXT("binding_profile_factory_registration_invalid"),
						FString::Printf(
							TEXT("Object factory registration is unsupported: %s"),
							*Registration),
						TEXT("use none or register_instance"),
						OutResult);
					return false;
				}
			}

			Spec.ObjectFactories.Add(MoveTemp(Factory));
		}
	}

	FAvidScriptBindingSelectionResolveResult ResolveResult;
	if (!FAvidScriptEditorProjectBindingProfile::Resolve(
		Spec,
		OutResult.ResolvedBindingSelection,
		OutResult.ResolvedClassReferences,
		OutResult.ResolvedObjectFactories,
		OutResult.BindingSelectionHash,
		ResolveResult))
	{
		SetAvidScriptCSharpProfileFailure(
			ResolveResult.ErrorCategory,
			ResolveResult.ErrorMessage,
			ResolveResult.NextAction,
			OutResult);
		return false;
	}

	OutResult.BindingSelectionValidation = MoveTemp(ResolveResult);
	OutResult.bUsesEngineGameplayBindingProfile = false;
	OutResult.ProjectBindingProfile = MoveTemp(Spec);
	return true;
}

FString MakeAvidScriptCSharpProfileSafeToken(const FString& RawValue, const FString& Fallback)
{
	FString Token;
	for (const TCHAR Character : RawValue)
	{
		if (FChar::IsAlnum(Character))
		{
			Token.AppendChar(FChar::ToLower(Character));
			continue;
		}

		if (!Token.EndsWith(TEXT("_")))
		{
			Token.AppendChar(TEXT('_'));
		}
	}

	Token.TrimStartAndEndInline();
	while (Token.StartsWith(TEXT("_")))
	{
		Token.RightChopInline(1);
	}
	while (Token.EndsWith(TEXT("_")))
	{
		Token.LeftChopInline(1);
	}

	return Token.IsEmpty() ? Fallback : Token;
}

void SetAvidScriptCSharpProfileTemplateFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorCSharpProfileTemplateResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.bCreated = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
}

FString GetAvidScriptCSharpProfileTemplateOutputRoot()
{
	return NormalizeAvidScriptCSharpProfilePath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("Profiles"),
		AvidScriptDefaultCSharpProfileTemplateArtifactStem));
}

FString MakeAvidScriptCSharpProfileTemplateStoredPath(const FString& Path)
{
	FString StoredPath = NormalizeAvidScriptCSharpProfilePath(Path);
	FString ProjectDirectory = NormalizeAvidScriptCSharpProfilePath(FPaths::ProjectDir());
	if (FPaths::MakePathRelativeTo(StoredPath, *ProjectDirectory))
	{
		FPaths::NormalizeFilename(StoredPath);
	}
	return StoredPath;
}

void FillAvidScriptCSharpProfileTemplateResult(
	const FString& ProfilePath,
	FAvidScriptEditorCSharpProfileTemplateResult& OutResult)
{
	OutResult.NormalizedProfilePath = NormalizeAvidScriptCSharpProfilePath(ProfilePath);
	OutResult.SourcePath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath();
	OutResult.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	OutResult.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	OutResult.OutputRoot = GetAvidScriptCSharpProfileTemplateOutputRoot();
	OutResult.ModuleId = AvidScriptDefaultCSharpProfileTemplateModuleId;
	OutResult.ArtifactStem = AvidScriptDefaultCSharpProfileTemplateArtifactStem;
	OutResult.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(OutResult.OutputRoot, OutResult.ArtifactStem);
	OutResult.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(OutResult.OutputRoot, OutResult.ArtifactStem);
	OutResult.Configuration = TEXT("Release");
}

bool SerializeAvidScriptCSharpProfileTemplate(
	const FAvidScriptEditorCSharpProfileTemplateResult& TemplateResult,
	FString& OutJsonText)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("schema_version"), 2.0);
	Object->SetStringField(TEXT("language"), TEXT("csharp"));
	Object->SetStringField(
		TEXT("source_path"),
		MakeAvidScriptCSharpProfileTemplateStoredPath(TemplateResult.SourcePath));
	Object->SetStringField(
		TEXT("project_path"),
		MakeAvidScriptCSharpProfileTemplateStoredPath(TemplateResult.ProjectPath));
	Object->SetStringField(TEXT("module_id"), TemplateResult.ModuleId);
	Object->SetStringField(TEXT("artifact_stem"), TemplateResult.ArtifactStem);
	Object->SetStringField(
		TEXT("output_root"),
		MakeAvidScriptCSharpProfileTemplateStoredPath(TemplateResult.OutputRoot));
	Object->SetStringField(
		TEXT("report_path"),
		MakeAvidScriptCSharpProfileTemplateStoredPath(TemplateResult.ReportPath));
	Object->SetStringField(
		TEXT("manifest_path"),
		MakeAvidScriptCSharpProfileTemplateStoredPath(TemplateResult.ManifestPath));
	Object->SetStringField(
		TEXT("build_script_path"),
		MakeAvidScriptCSharpProfileTemplateStoredPath(TemplateResult.BuildScriptPath));
	Object->SetStringField(TEXT("configuration"), TemplateResult.Configuration);

	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJsonText);
	return FJsonSerializer::Serialize(Object, Writer);
}
bool LoadAvidScriptCSharpProfileJson(
	const FString& ProfilePath,
	TSharedPtr<FJsonObject>& OutObject,
	FAvidScriptEditorCSharpProfileLoadResult& OutResult)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ProfilePath))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_read_failed"),
			FString::Printf(TEXT("C# profile could not be read: %s"), *ProfilePath),
			TEXT("verify the profile file can be read and is not locked by another process"),
			OutResult);
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_json_invalid"),
			FString::Printf(TEXT("C# profile JSON is invalid: %s"), *ProfilePath),
			TEXT("fix the profile JSON syntax and rerun the command"),
			OutResult);
		return false;
	}

	return true;
}
} // namespace

FString FAvidScriptEditorCSharpProfileService::GetDefaultProfilePath()
{
	return NormalizeAvidScriptCSharpProfilePath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpProfiles"),
		TEXT("default.csharp-profile.json")));
}

bool FAvidScriptEditorCSharpProfileService::WriteDefaultProfileTemplate(
	FAvidScriptEditorCSharpProfileTemplateResult& OutResult,
	bool bOverwrite)
{
	return WriteProfileTemplate(GetDefaultProfilePath(), OutResult, bOverwrite);
}

bool FAvidScriptEditorCSharpProfileService::WriteProfileTemplate(
	const FString& ProfilePath,
	FAvidScriptEditorCSharpProfileTemplateResult& OutResult,
	bool bOverwrite)
{
	OutResult = FAvidScriptEditorCSharpProfileTemplateResult();

	if (ProfilePath.IsEmpty())
	{
		SetAvidScriptCSharpProfileTemplateFailure(
			TEXT("profile_path_empty"),
			TEXT("C# profile template path is empty."),
			TEXT("choose a destination path for the C# profile template"),
			OutResult);
		return false;
	}

	FillAvidScriptCSharpProfileTemplateResult(ProfilePath, OutResult);

	if (FPaths::FileExists(OutResult.NormalizedProfilePath) && !bOverwrite)
	{
		OutResult.bSucceeded = true;
		OutResult.bCreated = false;
		OutResult.NextAction = TEXT("edit the existing C# profile or rerun with overwrite enabled");
		return true;
	}

	const FString ProfileDirectory = FPaths::GetPath(OutResult.NormalizedProfilePath);
	if (!IFileManager::Get().MakeDirectory(*ProfileDirectory, true))
	{
		SetAvidScriptCSharpProfileTemplateFailure(
			TEXT("profile_directory_failed"),
			FString::Printf(TEXT("C# profile template directory could not be created: %s"), *ProfileDirectory),
			TEXT("create the directory manually or choose another profile path"),
			OutResult);
		return false;
	}

	FString JsonText;
	if (!SerializeAvidScriptCSharpProfileTemplate(OutResult, JsonText))
	{
		SetAvidScriptCSharpProfileTemplateFailure(
			TEXT("profile_json_write_failed"),
			TEXT("C# profile template JSON could not be serialized."),
			TEXT("retry template generation after checking the profile fields"),
			OutResult);
		return false;
	}

	JsonText += LINE_TERMINATOR;
	if (!FFileHelper::SaveStringToFile(JsonText, *OutResult.NormalizedProfilePath))
	{
		SetAvidScriptCSharpProfileTemplateFailure(
			TEXT("profile_write_failed"),
			FString::Printf(TEXT("C# profile template could not be written: %s"), *OutResult.NormalizedProfilePath),
			TEXT("verify the destination path is writable and retry"),
			OutResult);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.bCreated = true;
	OutResult.NextAction = TEXT("edit source_path if needed, then run Build And Bind C# Profile Script");
	return true;
}
bool FAvidScriptEditorCSharpProfileService::LoadProfile(
	const FString& ProfilePath,
	FAvidScriptEditorCSharpProfileLoadResult& OutResult)
{
	OutResult = FAvidScriptEditorCSharpProfileLoadResult();

	if (ProfilePath.IsEmpty())
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_empty"),
			TEXT("C# profile path is empty."),
			TEXT("choose or create a C# profile JSON file"),
			OutResult);
		return false;
	}

	OutResult.NormalizedProfilePath = NormalizeAvidScriptCSharpProfilePath(ProfilePath);
	if (!FPaths::FileExists(OutResult.NormalizedProfilePath))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_missing"),
			FString::Printf(TEXT("C# profile does not exist: %s"), *OutResult.NormalizedProfilePath),
			TEXT("create the C# profile JSON file or choose an existing profile"),
			OutResult);
		return false;
	}

	TSharedPtr<FJsonObject> ProfileObject;
	if (!LoadAvidScriptCSharpProfileJson(OutResult.NormalizedProfilePath, ProfileObject, OutResult))
	{
		return false;
	}

	double SchemaVersion = 0.0;
	if (!ProfileObject->TryGetNumberField(TEXT("schema_version"), SchemaVersion)
		|| (SchemaVersion != 1.0
			&& SchemaVersion != 2.0
			&& SchemaVersion != 3.0
			&& SchemaVersion != 4.0
			&& SchemaVersion != 5.0
			&& SchemaVersion != 6.0
			&& SchemaVersion != 7.0))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_schema_unsupported"),
			TEXT("C# profile schema_version must be an integer from 1 through 7."),
			TEXT("update the profile JSON to schema_version 7"),
			OutResult);
		return false;
	}
	OutResult.SchemaVersion = static_cast<int32>(SchemaVersion);

	FString Language;
	if (!TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("language"), Language) || !Language.Equals(TEXT("csharp"), ESearchCase::IgnoreCase))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_language_unsupported"),
			TEXT("C# profile language must be csharp."),
			TEXT("set language to csharp in the profile JSON"),
			OutResult);
		return false;
	}

	FString SourcePath;
	if (!TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("source_path"), SourcePath))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("source_empty"),
			TEXT("C# profile requires source_path."),
			TEXT("set source_path to an existing .cs file"),
			OutResult);
		return false;
	}

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.SourcePath = NormalizeAvidScriptCSharpProfileFieldPath(SourcePath);
	if (!FPaths::GetExtension(Config.SourcePath, true).Equals(TEXT(".cs"), ESearchCase::IgnoreCase))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("source_not_csharp"),
			FString::Printf(TEXT("C# profile source_path must use .cs: %s"), *Config.SourcePath),
			TEXT("choose a C# .cs source file"),
			OutResult);
		return false;
	}

	if (!FPaths::FileExists(Config.SourcePath))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("source_missing"),
			FString::Printf(TEXT("C# profile source does not exist: %s"), *Config.SourcePath),
			TEXT("create the C# source file or update source_path"),
			OutResult);
		return false;
	}

	FString ProjectPath;
	Config.ProjectPath = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("project_path"), ProjectPath)
		? NormalizeAvidScriptCSharpProfileFieldPath(ProjectPath)
		: FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();

	FString ModuleId;
	FString ArtifactStem;
	const FString SourceToken = MakeAvidScriptCSharpProfileSafeToken(FPaths::GetBaseFilename(Config.SourcePath), TEXT("profile"));
	Config.ModuleId = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("module_id"), ModuleId)
		? ModuleId
		: FString::Printf(TEXT("csharp_%s"), *SourceToken);
	Config.ArtifactStem = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("artifact_stem"), ArtifactStem)
		? ArtifactStem
		: SourceToken;

	FString OutputRoot;
	Config.OutputRoot = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("output_root"), OutputRoot)
		? NormalizeAvidScriptCSharpProfileFieldPath(OutputRoot)
		: NormalizeAvidScriptCSharpProfilePath(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScriptCSharpGuest"),
			TEXT("Profiles"),
			Config.ArtifactStem));

	FString ReportPath;
	Config.ReportPath = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("report_path"), ReportPath)
		? NormalizeAvidScriptCSharpProfileFieldPath(ReportPath)
		: FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);

	FString ManifestPath;
	Config.ManifestPath = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("manifest_path"), ManifestPath)
		? NormalizeAvidScriptCSharpProfileFieldPath(ManifestPath)
		: FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);

	FString BuildScriptPath;
	Config.BuildScriptPath = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("build_script_path"), BuildScriptPath)
		? NormalizeAvidScriptCSharpProfileFieldPath(BuildScriptPath)
		: FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();

	FString DotNetPath;
	if (TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("dotnet_path"), DotNetPath))
	{
		Config.DotNetPath = NormalizeAvidScriptCSharpProfileFieldPath(DotNetPath);
	}

	FString BindingPackagePath;
	if (TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("binding_package_path"), BindingPackagePath))
	{
		Config.BindingPackagePath = NormalizeAvidScriptCSharpProfileFieldPath(BindingPackagePath);
	}

	FString Configuration;
	Config.Configuration = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("configuration"), Configuration)
		? Configuration
		: FString(TEXT("Release"));

	OutResult.ResolvedBindingSelection =
		FAvidScriptEditorBindingDescriptorGenerator::MakeEngineGameplayProfile();
	const TSharedPtr<FJsonObject>* BindingProfileObject = nullptr;
	const bool bHasBindingProfile = ProfileObject->HasField(TEXT("binding_profile"));
	const bool bHasSelfClassPath =
		ProfileObject->TryGetObjectField(TEXT("binding_profile"), BindingProfileObject)
		&& BindingProfileObject != nullptr
		&& (*BindingProfileObject).IsValid()
		&& (*BindingProfileObject)->HasField(TEXT("self_class_path"));
	const bool bHasObjectFactories =
		BindingProfileObject != nullptr
		&& (*BindingProfileObject).IsValid()
		&& (*BindingProfileObject)->HasField(TEXT("object_factories"));
	if (OutResult.SchemaVersion != 3
		&& OutResult.SchemaVersion != 4
		&& OutResult.SchemaVersion != 5
		&& OutResult.SchemaVersion != 6
		&& OutResult.SchemaVersion != 7
		&& bHasSelfClassPath)
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_self_field_not_supported"),
			TEXT("binding_profile.self_class_path requires C# profile schema_version 3 through 7."),
			TEXT("upgrade the C# profile to schema_version 7 before declaring self_class_path"),
			OutResult);
		return false;
	}
	if (OutResult.SchemaVersion != 4
		&& OutResult.SchemaVersion != 5
		&& OutResult.SchemaVersion != 6
		&& OutResult.SchemaVersion != 7
		&& bHasObjectFactories)
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("binding_profile_factory_schema_unsupported"),
			TEXT("binding_profile.object_factories requires C# profile schema_version 4 through 7."),
			TEXT("upgrade schema_version to 7 or remove object_factories"),
			OutResult);
		return false;
	}
	if (OutResult.SchemaVersion == 1 && bHasBindingProfile)
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("binding_profile_schema_unsupported"),
			TEXT("C# profile schema_version 1 cannot declare binding_profile."),
			TEXT("upgrade schema_version to 2 or remove binding_profile"),
			OutResult);
		return false;
	}
	if ((OutResult.SchemaVersion == 2
			|| OutResult.SchemaVersion == 3
			|| OutResult.SchemaVersion == 4
			|| OutResult.SchemaVersion == 5
			|| OutResult.SchemaVersion == 6
			|| OutResult.SchemaVersion == 7)
		&& bHasBindingProfile)
	{
		if (!ProfileObject->TryGetObjectField(TEXT("binding_profile"), BindingProfileObject)
			|| BindingProfileObject == nullptr
			|| !ParseAvidScriptCSharpProjectBindingProfile(
				*BindingProfileObject,
				OutResult.SchemaVersion,
				OutResult))
		{
			if (OutResult.ErrorCategory.IsEmpty())
			{
				SetAvidScriptCSharpProfileFailure(
					TEXT("binding_profile_invalid"),
					TEXT("C# profile binding_profile must be a valid object."),
					TEXT("fix binding_profile or remove it to use EngineGameplay"),
					OutResult);
			}
			return false;
		}
	}

	OutResult.BuildConfig = MoveTemp(Config);
	OutResult.bSucceeded = true;
	return true;
}

FAvidScriptEditorCSharpBuildRequest
FAvidScriptEditorCSharpProfileService::MakeBuildRequest(
	const FAvidScriptEditorCSharpProfileLoadResult& Profile)
{
	FAvidScriptEditorCSharpBuildRequest Request;
	Request.Config = Profile.BuildConfig;
	Request.AuthorizationBindingProfile = Profile.ResolvedBindingSelection;
	Request.AuthorizationClassReferences = Profile.ResolvedClassReferences;
	Request.AuthorizationObjectFactories = Profile.ResolvedObjectFactories;
	Request.BindingSelectionHash = Profile.BindingSelectionHash;
	Request.bUsesEngineGameplayBindingProfile =
		Profile.bUsesEngineGameplayBindingProfile;
	return Request;
}
