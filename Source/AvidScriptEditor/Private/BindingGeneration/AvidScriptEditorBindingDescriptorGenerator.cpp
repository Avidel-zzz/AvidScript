#include "AvidScriptEditorBindingDescriptorGenerator.h"

#include "AvidScriptHash.h"
#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
constexpr const TCHAR* GeneratorVersion = TEXT("42.1.0");

struct FResolvedBindingDescriptor
{
	FAvidScriptReflectedFunctionSelection Selection;
	UClass* OwnerClass = nullptr;
	UFunction* Function = nullptr;
	FAvidScriptProjectedFunction Projection;
	FString ScriptName;
	FString CanonicalIdentity;
	FString StableId;
	FString ImportName;
	int32 Ordinal = INDEX_NONE;
};

FString HashSha256(const FString& Value)
{
	return FAvidScriptHash::Sha256HexUtf8(Value);
}

void SetFailure(
	FAvidScriptBindingDescriptorGenerateResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& NextAction)
{
	OutResult = FAvidScriptBindingDescriptorGenerateResult();
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript binding descriptor error | category=%s | source=%s | next=%s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source,
		*NextAction);
}

FString GetDescriptorScriptFunctionName(const UFunction* Function)
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

FString MakeSelectionKey(const FAvidScriptReflectedFunctionSelection& Selection)
{
	return Selection.OwnerClassPath + TEXT(".") + Selection.FunctionName.ToString();
}

FString MakeCanonicalIdentity(
	const UClass* OwnerClass,
	const UFunction* Function,
	const FAvidScriptProjectedFunction& Projection)
{
	FString Identity = OwnerClass->GetPathName()
		+ TEXT("::")
		+ Function->GetName()
		+ TEXT("(")
		+ Projection.ReturnValue.Type.CanonicalType;
	for (const FAvidScriptProjectedBindingValue& Parameter : Projection.Parameters)
	{
		Identity += TEXT(";")
			+ Parameter.Name
			+ TEXT(":")
			+ Parameter.Direction
			+ TEXT(":")
			+ Parameter.Type.CanonicalType;
	}
	Identity += TEXT(")");
	return Identity;
}

bool IsFunctionAllowed(const UFunction* Function, FString& OutCategory, FString& OutSource)
{
	if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
	{
		OutCategory = TEXT("function_not_callable");
		OutSource = Function->GetPathName();
		return false;
	}
	if (Function->HasAnyFunctionFlags(FUNC_EditorOnly | FUNC_Delegate | FUNC_MulticastDelegate | FUNC_NetRequest | FUNC_NetResponse)
		|| Function->HasMetaData(TEXT("Latent"))
		|| Function->HasMetaData(TEXT("CustomThunk")))
	{
		OutCategory = TEXT("function_not_allowed");
		OutSource = Function->GetPathName();
		return false;
	}
	return true;
}

void FinalizeType(FAvidScriptProjectedBindingType& Type)
{
	Type.StableId = HashSha256(Type.CanonicalType);
}

void WriteStringArray(const TSharedRef<TJsonWriter<>>& Writer, const TCHAR* Name, const TArray<FString>& Values)
{
	Writer->WriteArrayStart(Name);
	for (const FString& Value : Values)
	{
		Writer->WriteValue(Value);
	}
	Writer->WriteArrayEnd();
}

void WriteProjectedValue(
	const TSharedRef<TJsonWriter<>>& Writer,
	const FAvidScriptProjectedBindingValue& Value)
{
	Writer->WriteValue(TEXT("name"), Value.Name);
	Writer->WriteValue(TEXT("direction"), Value.Direction);
	Writer->WriteValue(TEXT("has_default"), Value.bHasDefaultValue);
	if (Value.bHasDefaultValue)
	{
		Writer->WriteValue(TEXT("default_value"), Value.DefaultValue);
	}
	Writer->WriteValue(TEXT("canonical_type"), Value.Type.CanonicalType);
	Writer->WriteValue(TEXT("type_id"), Value.Type.StableId);
	Writer->WriteValue(TEXT("kind"), Value.Type.Kind);
	Writer->WriteValue(TEXT("cpp_type"), Value.Type.CppType);
	WriteStringArray(Writer, TEXT("abi_types"), Value.Type.AbiValueTypes);
}
} // namespace

const TCHAR* FAvidScriptEditorBindingDescriptorGenerator::GetDefaultPackageName()
{
	return TEXT("avidscript.engine.core");
}

TArray<FAvidScriptReflectedFunctionSelection> FAvidScriptEditorBindingDescriptorGenerator::MakeDefaultSelections()
{
	return {
		{ TEXT("/Script/Engine.Actor"), TEXT("K2_GetActorLocation") },
		{ TEXT("/Script/Engine.Actor"), TEXT("K2_GetActorRotation") },
		{ TEXT("/Script/Engine.Actor"), TEXT("GetActorScale3D") },
		{ TEXT("/Script/Engine.Actor"), TEXT("SetActorScale3D") },
		{ TEXT("/Script/Engine.Actor"), TEXT("K2_GetRootComponent") },
		{ TEXT("/Script/Engine.Actor"), TEXT("GetDistanceTo") },
		{ TEXT("/Script/Engine.SceneComponent"), TEXT("K2_GetComponentLocation") },
		{ TEXT("/Script/Engine.SceneComponent"), TEXT("K2_GetComponentRotation") }
	};
}

bool FAvidScriptEditorBindingDescriptorGenerator::Generate(
	const FString& PackageName,
	const TArray<FAvidScriptReflectedFunctionSelection>& Selections,
	FString& OutJson,
	FAvidScriptBindingDescriptorGenerateResult& OutResult)
{
	OutJson.Empty();
	OutResult = FAvidScriptBindingDescriptorGenerateResult();
	if (PackageName.IsEmpty())
	{
		SetFailure(OutResult, TEXT("package_name_missing"), PackageName, TEXT("Provide a stable non-empty binding package name."));
		return false;
	}
	if (Selections.IsEmpty())
	{
		SetFailure(OutResult, TEXT("selection_empty"), PackageName, TEXT("Select at least one reflected function for the binding package."));
		return false;
	}

	TArray<FAvidScriptReflectedFunctionSelection> SortedSelections = Selections;
	SortedSelections.Sort([](const FAvidScriptReflectedFunctionSelection& Left, const FAvidScriptReflectedFunctionSelection& Right)
	{
		return MakeSelectionKey(Left).Compare(MakeSelectionKey(Right), ESearchCase::CaseSensitive) < 0;
	});

	TArray<FString> SelectionKeys;
	TArray<FResolvedBindingDescriptor> Bindings;
	TSet<FString> SeenSelections;
	for (const FAvidScriptReflectedFunctionSelection& Selection : SortedSelections)
	{
		const FString SelectionKey = MakeSelectionKey(Selection);
		if (SeenSelections.Contains(SelectionKey))
		{
			SetFailure(OutResult, TEXT("duplicate_selection"), SelectionKey, TEXT("Keep each reflected owner and function pair exactly once."));
			return false;
		}
		SeenSelections.Add(SelectionKey);
		SelectionKeys.Add(SelectionKey);

		UClass* OwnerClass = LoadObject<UClass>(nullptr, *Selection.OwnerClassPath);
		if (OwnerClass == nullptr)
		{
			SetFailure(OutResult, TEXT("class_missing"), Selection.OwnerClassPath, TEXT("Use a loaded reflected UClass path from the active UE5.8 build."));
			return false;
		}

		UFunction* Function = OwnerClass->FindFunctionByName(Selection.FunctionName);
		if (Function == nullptr)
		{
			SetFailure(OutResult, TEXT("function_missing"), SelectionKey, TEXT("Update the selection to a reflected UFunction available in the active engine version."));
			return false;
		}

		FString FunctionPolicyCategory;
		FString FunctionPolicySource;
		if (!IsFunctionAllowed(Function, FunctionPolicyCategory, FunctionPolicySource))
		{
			SetFailure(OutResult, FunctionPolicyCategory, FunctionPolicySource, TEXT("Select a public script-callable, non-latent runtime UFunction."));
			return false;
		}

		FResolvedBindingDescriptor Binding;
		Binding.Selection = Selection;
		Binding.OwnerClass = OwnerClass;
		Binding.Function = Function;
		FString ProjectionErrorSource;
		if (!FAvidScriptEditorReflectedTypePolicy::ProjectFunction(
			Function,
			Function->HasAnyFunctionFlags(FUNC_Static),
			Binding.Projection,
			ProjectionErrorSource))
		{
			SetFailure(
				OutResult,
				TEXT("unsupported_property"),
				SelectionKey + TEXT(":") + ProjectionErrorSource,
				TEXT("Add the property type to the shared reflected type policy before selecting this function."));
			return false;
		}

		FinalizeType(Binding.Projection.ReturnValue.Type);
		for (FAvidScriptProjectedBindingValue& Parameter : Binding.Projection.Parameters)
		{
			FinalizeType(Parameter.Type);
		}
		Binding.ScriptName = GetDescriptorScriptFunctionName(Function);
		Binding.CanonicalIdentity = MakeCanonicalIdentity(OwnerClass, Function, Binding.Projection);
		Binding.StableId = HashSha256(Binding.CanonicalIdentity);
		Binding.ImportName = TEXT("avid_ue_") + Binding.StableId.Left(16);
		Bindings.Add(MoveTemp(Binding));
	}

	Bindings.Sort([](const FResolvedBindingDescriptor& Left, const FResolvedBindingDescriptor& Right)
	{
		return Left.CanonicalIdentity.Compare(Right.CanonicalIdentity, ESearchCase::CaseSensitive) < 0;
	});

	TSet<FString> SeenStableIds;
	TMap<FString, FAvidScriptProjectedBindingType> TypesByCanonicalName;
	for (int32 Index = 0; Index < Bindings.Num(); ++Index)
	{
		FResolvedBindingDescriptor& Binding = Bindings[Index];
		if (Binding.StableId.IsEmpty() || SeenStableIds.Contains(Binding.StableId))
		{
			SetFailure(OutResult, TEXT("duplicate_stable_id"), Binding.CanonicalIdentity, TEXT("Resolve the canonical identity collision before publishing the package."));
			return false;
		}
		SeenStableIds.Add(Binding.StableId);
		Binding.Ordinal = Index;

		FAvidScriptProjectedBindingType OwnerType = FAvidScriptEditorReflectedTypePolicy::MakeObjectType(Binding.OwnerClass);
		FinalizeType(OwnerType);
		TypesByCanonicalName.FindOrAdd(OwnerType.CanonicalType) = MoveTemp(OwnerType);
		if (!Binding.Projection.ReturnValue.Type.bVoid)
		{
			TypesByCanonicalName.FindOrAdd(Binding.Projection.ReturnValue.Type.CanonicalType) = Binding.Projection.ReturnValue.Type;
		}
		for (const FAvidScriptProjectedBindingValue& Parameter : Binding.Projection.Parameters)
		{
			TypesByCanonicalName.FindOrAdd(Parameter.Type.CanonicalType) = Parameter.Type;
		}
	}

	TArray<FAvidScriptProjectedBindingType> Types;
	TypesByCanonicalName.GenerateValueArray(Types);
	Types.Sort([](const FAvidScriptProjectedBindingType& Left, const FAvidScriptProjectedBindingType& Right)
	{
		return Left.CanonicalType.Compare(Right.CanonicalType, ESearchCase::CaseSensitive) < 0;
	});

	const FString SelectionHash = HashSha256(FString::Join(SelectionKeys, TEXT("\n")));
	FString PackageIdentity = PackageName
		+ TEXT("|") + GeneratorVersion
		+ TEXT("|") + FEngineVersion::Current().ToString(EVersionComponent::Patch)
		+ TEXT("|") + SelectionHash;
	for (const FAvidScriptProjectedBindingType& Type : Types)
	{
		PackageIdentity += TEXT("|type:") + Type.CanonicalType + TEXT(":") + FString::Join(Type.AbiValueTypes, TEXT(""));
	}
	for (const FResolvedBindingDescriptor& Binding : Bindings)
	{
		PackageIdentity += TEXT("|binding:") + Binding.CanonicalIdentity + TEXT(":") + Binding.Projection.AbiSignature;
		for (const FAvidScriptProjectedBindingValue& Parameter : Binding.Projection.Parameters)
		{
			PackageIdentity += TEXT("|default:") + Parameter.Name + TEXT(":");
			if (Parameter.bHasDefaultValue)
			{
				PackageIdentity += TEXT("1:") + FString::FromInt(Parameter.DefaultValue.Len()) + TEXT(":") + Parameter.DefaultValue;
			}
			else
			{
				PackageIdentity += TEXT("0");
			}
		}
	}
	const FString PackageHash = HashSha256(PackageIdentity);

	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema_version"), 2);
	Writer->WriteValue(TEXT("generator_version"), GeneratorVersion);
	Writer->WriteValue(TEXT("engine_version"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
	Writer->WriteValue(TEXT("source"), TEXT("ue_reflection"));
	Writer->WriteValue(TEXT("package_name"), PackageName);
	Writer->WriteValue(TEXT("package_hash"), PackageHash);
	Writer->WriteValue(TEXT("selection_hash"), SelectionHash);
	Writer->WriteArrayStart(TEXT("types"));
	for (const FAvidScriptProjectedBindingType& Type : Types)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("stable_id"), Type.StableId);
		Writer->WriteValue(TEXT("canonical_type"), Type.CanonicalType);
		Writer->WriteValue(TEXT("kind"), Type.Kind);
		Writer->WriteValue(TEXT("cpp_type"), Type.CppType);
		Writer->WriteValue(TEXT("size"), Type.Size);
		Writer->WriteValue(TEXT("alignment"), Type.Alignment);
		WriteStringArray(Writer, TEXT("abi_types"), Type.AbiValueTypes);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteArrayStart(TEXT("bindings"));
	for (const FResolvedBindingDescriptor& Binding : Bindings)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("stable_id"), Binding.StableId);
		Writer->WriteValue(TEXT("canonical_identity"), Binding.CanonicalIdentity);
		Writer->WriteValue(TEXT("ordinal"), Binding.Ordinal);
		Writer->WriteValue(TEXT("owner_class"), Binding.OwnerClass->GetPathName());
		Writer->WriteValue(TEXT("ue_function"), Binding.Function->GetName());
		Writer->WriteValue(TEXT("script_name"), Binding.ScriptName);
		Writer->WriteValue(TEXT("dispatch_mode"), TEXT("cached_process_event"));
		Writer->WriteValue(TEXT("is_static"), Binding.Function->HasAnyFunctionFlags(FUNC_Static));
		Writer->WriteValue(TEXT("is_const"), Binding.Function->HasAnyFunctionFlags(FUNC_Const));
		Writer->WriteObjectStart(TEXT("return"));
		WriteProjectedValue(Writer, Binding.Projection.ReturnValue);
		Writer->WriteObjectEnd();
		Writer->WriteArrayStart(TEXT("parameters"));
		for (const FAvidScriptProjectedBindingValue& Parameter : Binding.Projection.Parameters)
		{
			Writer->WriteObjectStart();
			WriteProjectedValue(Writer, Parameter);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteObjectStart(TEXT("host_import"));
		Writer->WriteValue(TEXT("module"), TEXT("avidscript"));
		Writer->WriteValue(TEXT("name"), Binding.ImportName);
		Writer->WriteValue(TEXT("signature"), Binding.Projection.AbiSignature);
		Writer->WriteObjectEnd();
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	if (!Writer->Close())
	{
		OutJson.Empty();
		SetFailure(OutResult, TEXT("serialize_failed"), PackageName, TEXT("Inspect the reflected descriptor writer state."));
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.BindingCount = Bindings.Num();
	OutResult.TypeCount = Types.Num();
	OutResult.PackageHash = PackageHash;
	OutResult.SelectionHash = SelectionHash;
	return true;
}

bool FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(
	FString& OutJson,
	FAvidScriptBindingDescriptorGenerateResult& OutResult)
{
	return Generate(GetDefaultPackageName(), MakeDefaultSelections(), OutJson, OutResult);
}

bool FAvidScriptEditorBindingDescriptorGenerator::WriteDefault(
	const FString& OutputPath,
	FAvidScriptBindingDescriptorGenerateResult& OutResult)
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
		SetFailure(OutResult, TEXT("write_failed"), TemporaryPath, TEXT("Verify that the generated binding package directory is writable."));
		return false;
	}
	if (!IFileManager::Get().Move(*FullOutputPath, *TemporaryPath, true, true, false, true))
	{
		IFileManager::Get().Delete(*TemporaryPath);
		SetFailure(OutResult, TEXT("write_failed"), FullOutputPath, TEXT("Close readers of the previous descriptor and retry generation."));
		return false;
	}

	OutResult.OutputPath = FullOutputPath;
	return true;
}
