#include "AvidScriptEditorBindingDescriptorGenerator.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptEditorBindingPropertySelectionResolver.h"
#include "AvidScriptEditorBindingSelectionResolver.h"
#include "AvidScriptHash.h"
#include "BindingGeneration/AvidScriptEditorBindingReloadEffectPolicy.h"
#include "BindingGeneration/AvidScriptEditorObjectTypeGraph.h"
#include "BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.h"
#include "BindingGeneration/AvidScriptEditorReflectedPropertyPolicy.h"
#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
constexpr const TCHAR* GeneratorVersion = TEXT("49.2.0");

struct FResolvedBindingDescriptor
{
	FAvidScriptReflectedFunctionSelection Selection;
	UClass* OwnerClass = nullptr;
	UFunction* Function = nullptr;
	FProperty* Property = nullptr;
	FString BindingKind = TEXT("function");
	FString UeMember;
	FAvidScriptProjectedFunction Projection;
	FString ScriptName;
	FString CanonicalIdentity;
	FString StableId;
	FString ImportName;
	EAvidScriptBindingReloadEffect ReloadEffect = EAvidScriptBindingReloadEffect::Unsupported;
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

FString MakeDescriptorPropertySelectionKey(const FAvidScriptReflectedPropertySelection& Selection)
{
	return TEXT("property_get:") + Selection.OwnerClassPath + TEXT(".") + Selection.PropertyName.ToString();
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

void FinalizeType(FAvidScriptProjectedBindingType& Type)
{
	Type.StableId = FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
		Type.CanonicalType,
		Type.EnumValues);
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

FAvidScriptBindingValueModel MakeBindingValueModel(
	const FAvidScriptProjectedBindingValue& Value)
{
	FAvidScriptBindingValueModel Model;
	Model.Name = Value.Name;
	Model.Direction = Value.Direction;
	Model.bHasDefault = Value.bHasDefaultValue;
	Model.DefaultValue = Value.DefaultValue;
	Model.CanonicalType = Value.Type.CanonicalType;
	Model.TypeId = Value.Type.StableId;
	Model.Kind = Value.Type.Kind;
	Model.CppType = Value.Type.CppType;
	Model.AbiTypes = Value.Type.AbiValueTypes;
	return Model;
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

FAvidScriptBindingSelectionProfile FAvidScriptEditorBindingDescriptorGenerator::MakeEngineGameplayProfile()
{
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.engine.gameplay");
	FAvidScriptReflectedClassSelection ActorRule;
	ActorRule.OwnerClassPath = TEXT("/Script/Engine.Actor");
	ActorRule.IncludeProperties.Add(TEXT("CustomTimeDilation"));
	ActorRule.IncludeProperties.Add(TEXT("RootComponent"));
	Profile.Classes.Add(MoveTemp(ActorRule));
	Profile.Classes.Add({ TEXT("/Script/Engine.ActorComponent") });
	Profile.Classes.Add({ TEXT("/Script/Engine.SceneComponent") });
	Profile.Classes.Add({ TEXT("/Script/Engine.PrimitiveComponent") });
	Profile.Classes.Add({ TEXT("/Script/Engine.Pawn") });
	Profile.Classes.Add({ TEXT("/Script/Engine.Controller") });
	return Profile;
}

bool FAvidScriptEditorBindingDescriptorGenerator::Generate(
	const FString& PackageName,
	const TArray<FAvidScriptReflectedFunctionSelection>& Selections,
	FString& OutJson,
	FAvidScriptBindingDescriptorGenerateResult& OutResult)
{
	return GenerateWithReadableProperties(
		PackageName,
		Selections,
		TArray<FAvidScriptReflectedPropertySelection>(),
		OutJson,
		OutResult);
}

bool FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
	const FString& PackageName,
	const TArray<FAvidScriptReflectedFunctionSelection>& FunctionSelections,
	const TArray<FAvidScriptReflectedPropertySelection>& PropertySelections,
	FString& OutJson,
	FAvidScriptBindingDescriptorGenerateResult& OutResult)
{
	return GenerateWithClassReferences(
		PackageName,
		FunctionSelections,
		PropertySelections,
		{},
		OutJson,
		OutResult);
}

namespace
{
void AddObjectHandleClass(const FProperty* Property, TArray<UClass*>& OutHandleClasses)
{
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		OutHandleClasses.Add(ObjectProperty->PropertyClass);
	}
}

void AddObjectHandleClasses(const UFunction* Function, TArray<UClass*>& OutHandleClasses)
{
	AddObjectHandleClass(Function->GetReturnProperty(), OutHandleClasses);
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		const FProperty* Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_Parm)
			&& !Property->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			AddObjectHandleClass(Property, OutHandleClasses);
		}
	}
}

bool GenerateBindingDescriptor(
	const FString& PackageName,
	const TArray<FAvidScriptReflectedFunctionSelection>& FunctionSelections,
	const TArray<FAvidScriptReflectedPropertySelection>& PropertySelections,
	const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
	UClass* SelfClass,
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
	if (FunctionSelections.IsEmpty()
		&& PropertySelections.IsEmpty()
		&& ClassReferences.IsEmpty())
	{
		SetFailure(OutResult, TEXT("selection_empty"), PackageName, TEXT("Select at least one reflected function, readable property, or class reference for the binding package."));
		return false;
	}

	TArray<FAvidScriptReflectedFunctionSelection> SortedSelections = FunctionSelections;
	SortedSelections.Sort([](const FAvidScriptReflectedFunctionSelection& Left, const FAvidScriptReflectedFunctionSelection& Right)
	{
		return MakeSelectionKey(Left).Compare(MakeSelectionKey(Right), ESearchCase::CaseSensitive) < 0;
	});

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
		if (Function->GetOwnerClass() != OwnerClass)
		{
			SetFailure(
				OutResult,
				TEXT("function_owner_mismatch"),
				SelectionKey,
				TEXT("Select the reflected declaring class instead of publishing an inherited function under a derived facade."));
			return false;
		}

		FString FunctionPolicyCategory;
		FString FunctionPolicySource;
		if (!FAvidScriptEditorReflectedFunctionPolicy::Evaluate(
			Function,
			FunctionPolicyCategory,
			FunctionPolicySource))
		{
			SetFailure(OutResult, FunctionPolicyCategory, FunctionPolicySource, TEXT("Select a public script-callable, non-latent runtime UFunction."));
			return false;
		}

		FResolvedBindingDescriptor Binding;
		Binding.Selection = Selection;
		Binding.OwnerClass = OwnerClass;
		Binding.Function = Function;
		Binding.UeMember = Function->GetName();
		FString ProjectionErrorSource;
		if (!FAvidScriptEditorReflectedTypePolicy::ProjectFunction(
			Function,
			Function->HasAnyFunctionFlags(FUNC_Static),
			Binding.Projection,
			ProjectionErrorSource))
		{
			SetFailure(
				OutResult,
				ProjectionErrorSource.StartsWith(TEXT("FName:")) ? TEXT("unsupported_property_type") : TEXT("unsupported_property"),
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
		Binding.ReloadEffect = FAvidScriptEditorBindingReloadEffectPolicy::Classify(*Function);
		Bindings.Add(MoveTemp(Binding));
	}

	TArray<FAvidScriptReflectedPropertySelection> SortedPropertySelections = PropertySelections;
	SortedPropertySelections.Sort([](const FAvidScriptReflectedPropertySelection& Left, const FAvidScriptReflectedPropertySelection& Right)
	{
		return MakeDescriptorPropertySelectionKey(Left).Compare(MakeDescriptorPropertySelectionKey(Right), ESearchCase::CaseSensitive) < 0;
	});
	for (const FAvidScriptReflectedPropertySelection& Selection : SortedPropertySelections)
	{
		const FString SelectionKey = MakeDescriptorPropertySelectionKey(Selection);
		if (SeenSelections.Contains(SelectionKey))
		{
			SetFailure(OutResult, TEXT("duplicate_selection"), SelectionKey, TEXT("Keep each reflected property getter selection exactly once."));
			return false;
		}
		SeenSelections.Add(SelectionKey);

		UClass* OwnerClass = LoadObject<UClass>(nullptr, *Selection.OwnerClassPath);
		FProperty* Property = OwnerClass == nullptr
			? nullptr
			: FindFProperty<FProperty>(OwnerClass, Selection.PropertyName);
		if (OwnerClass == nullptr)
		{
			SetFailure(OutResult, TEXT("class_missing"), Selection.OwnerClassPath, TEXT("Use a loaded reflected UClass path from the active UE5.8 build."));
			return false;
		}
		if (Property == nullptr)
		{
			SetFailure(OutResult, TEXT("property_missing"), SelectionKey, TEXT("Select a reflected property available on the active owner class."));
			return false;
		}
		if (Property->GetOwnerStruct() != OwnerClass)
		{
			SetFailure(
				OutResult,
				TEXT("property_owner_mismatch"),
				SelectionKey,
				TEXT("Select the reflected declaring class instead of publishing an inherited property under a derived facade."));
			return false;
		}
		FString PropertyPolicyCategory;
		FString PropertyPolicySource;
		if (!FAvidScriptEditorReflectedPropertyPolicy::EvaluateReadable(
			Property,
			PropertyPolicyCategory,
			PropertyPolicySource))
		{
			SetFailure(OutResult, PropertyPolicyCategory, PropertyPolicySource, TEXT("Select a runtime-visible property supported by the shared type policy."));
			return false;
		}

		FResolvedBindingDescriptor Binding;
		Binding.OwnerClass = OwnerClass;
		Binding.Property = Property;
		Binding.BindingKind = TEXT("property_get");
		Binding.UeMember = Property->GetName();
		FString ProjectionErrorSource;
		if (!FAvidScriptEditorReflectedTypePolicy::ProjectReadableProperty(
			Property,
			Binding.Projection.ReturnValue,
			ProjectionErrorSource))
		{
			SetFailure(OutResult, TEXT("unsupported_property_type"), SelectionKey + TEXT(":") + ProjectionErrorSource, TEXT("Add the property type to the shared reflected type policy."));
			return false;
		}
		FinalizeType(Binding.Projection.ReturnValue.Type);
		Binding.Projection.AbiSignature = TEXT("(iii)i");
		Binding.ScriptName = Property->GetAuthoredName();
		Binding.CanonicalIdentity = OwnerClass->GetPathName()
			+ TEXT("::property_get:") + Property->GetName()
			+ TEXT("(") + Binding.Projection.ReturnValue.Type.CanonicalType + TEXT(")");
		Binding.StableId = HashSha256(Binding.CanonicalIdentity);
		Binding.ImportName = TEXT("avid_ue_") + Binding.StableId.Left(16);
		Binding.ReloadEffect = EAvidScriptBindingReloadEffect::None;
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

	FAvidScriptBindingPackageModel Package;
	Package.SchemaVersion = 5;
	Package.GeneratorVersion = GeneratorVersion;
	Package.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Package.Source = TEXT("ue_reflection");
	Package.PackageName = PackageName;
	for (const FAvidScriptProjectedBindingType& Type : Types)
	{
		FAvidScriptBindingTypeModel TypeModel;
		TypeModel.StableId = Type.StableId;
		TypeModel.CanonicalType = Type.CanonicalType;
		TypeModel.Kind = Type.Kind;
		TypeModel.CppType = Type.CppType;
		TypeModel.Size = Type.Size;
		TypeModel.Alignment = Type.Alignment;
		TypeModel.AbiTypes = Type.AbiValueTypes;
		TypeModel.EnumValues = Type.EnumValues;
		Package.Types.Add(MoveTemp(TypeModel));
	}
	for (const FResolvedBindingDescriptor& Binding : Bindings)
	{
		FAvidScriptBindingFunctionModel BindingModel;
		BindingModel.StableId = Binding.StableId;
		BindingModel.CanonicalIdentity = Binding.CanonicalIdentity;
		BindingModel.Ordinal = Binding.Ordinal;
		BindingModel.OwnerClass = Binding.OwnerClass->GetPathName();
		BindingModel.BindingKind = Binding.BindingKind;
		BindingModel.UeMember = Binding.UeMember;
		BindingModel.UeFunction = Binding.Function == nullptr ? FString() : Binding.Function->GetName();
		BindingModel.ScriptName = Binding.ScriptName;
		BindingModel.DispatchMode = Binding.BindingKind == TEXT("function")
			? TEXT("cached_process_event")
			: TEXT("cached_property_get");
		BindingModel.bStatic = Binding.Function != nullptr && Binding.Function->HasAnyFunctionFlags(FUNC_Static);
		BindingModel.bConst = Binding.Function == nullptr || Binding.Function->HasAnyFunctionFlags(FUNC_Const);
		BindingModel.ReloadEffect = Binding.ReloadEffect;
		BindingModel.ReturnValue = MakeBindingValueModel(Binding.Projection.ReturnValue);
		for (const FAvidScriptProjectedBindingValue& Parameter : Binding.Projection.Parameters)
		{
			BindingModel.Parameters.Add(MakeBindingValueModel(Parameter));
		}
		BindingModel.HostImport.Module = TEXT("avidscript");
		BindingModel.HostImport.Name = Binding.ImportName;
		BindingModel.HostImport.Signature = Binding.Projection.AbiSignature;
		Package.Bindings.Add(MoveTemp(BindingModel));
	}

	TSet<FString> ClassReferenceStableIds;
	TSet<FString> ClassReferenceScriptNames;
	for (const FAvidScriptProjectBindingClassSpec& ClassReference : ClassReferences)
	{
		FAvidScriptBindingClassReferenceModel ReferenceModel;
		ReferenceModel.ScriptName = ClassReference.ScriptName;
		ReferenceModel.ClassPath = ClassReference.ClassPath;
		ReferenceModel.BaseClassPath = ClassReference.BaseClassPath;
		ReferenceModel.LoadPolicy = ClassReference.LoadPolicy;
		ReferenceModel.StableId = FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
			ReferenceModel.ClassPath,
			ReferenceModel.BaseClassPath,
			ReferenceModel.LoadPolicy);
		if (ReferenceModel.ScriptName.IsEmpty()
			|| ReferenceModel.ScriptName == TEXT("ProjectClasses")
			|| ReferenceModel.ClassPath.IsEmpty()
			|| ReferenceModel.BaseClassPath.IsEmpty()
			|| (ReferenceModel.LoadPolicy != TEXT("EditorLoad")
				&& ReferenceModel.LoadPolicy != TEXT("CookRequired"))
			|| ClassReferenceStableIds.Contains(ReferenceModel.StableId)
			|| ClassReferenceScriptNames.Contains(ReferenceModel.ScriptName))
		{
			SetFailure(
				OutResult,
				TEXT("class_reference_invalid"),
				ReferenceModel.ScriptName,
				TEXT("Resolve duplicate or invalid class reference declarations before descriptor generation."));
			return false;
		}
		ClassReferenceStableIds.Add(ReferenceModel.StableId);
		ClassReferenceScriptNames.Add(ReferenceModel.ScriptName);
		Package.ClassReferences.Add(MoveTemp(ReferenceModel));
	}
	Package.ClassReferences.Sort([](
		const FAvidScriptBindingClassReferenceModel& Left,
		const FAvidScriptBindingClassReferenceModel& Right)
	{
		return Left.StableId.Compare(Right.StableId, ESearchCase::CaseSensitive) < 0;
	});
	for (int32 Index = 0; Index < Package.ClassReferences.Num(); ++Index)
	{
		Package.ClassReferences[Index].Ordinal = Index;
	}

	TArray<UClass*> HandleClasses;
	for (const FResolvedBindingDescriptor& Binding : Bindings)
	{
		if (Binding.Function == nullptr || !Binding.Function->HasAnyFunctionFlags(FUNC_Static))
		{
			HandleClasses.Add(Binding.OwnerClass);
		}
		if (Binding.Function != nullptr)
		{
			AddObjectHandleClasses(Binding.Function, HandleClasses);
		}
		else
		{
			AddObjectHandleClass(Binding.Property, HandleClasses);
		}
	}
	FAvidScriptEditorObjectTypeGraph ObjectTypeGraph;
	FString ObjectTypeGraphErrorCategory;
	FString ObjectTypeGraphErrorDetails;
	if (!FAvidScriptEditorObjectTypeGraph::Build(
		HandleClasses,
		SelfClass,
		ClassReferences,
		ObjectTypeGraph,
		ObjectTypeGraphErrorCategory,
		ObjectTypeGraphErrorDetails))
	{
		SetFailure(
			OutResult,
			ObjectTypeGraphErrorCategory,
			ObjectTypeGraphErrorDetails,
			TEXT("Resolve the handle-capable UObject type graph before generating the binding package."));
		return false;
	}

	Package.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
	Package.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);
	const int32 SchemaVersion = Package.SchemaVersion;
	const FString& EffectiveGeneratorVersion = Package.GeneratorVersion;
	const FString& SelectionHash = Package.SelectionHash;
	const FString& PackageHash = Package.PackageHash;

	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema_version"), SchemaVersion);
	Writer->WriteValue(TEXT("generator_version"), EffectiveGeneratorVersion);
	Writer->WriteValue(TEXT("engine_version"), Package.EngineVersion);
	Writer->WriteValue(TEXT("source"), Package.Source);
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
		Writer->WriteObjectEnd();
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
		if (SchemaVersion >= 4)
		{
			Writer->WriteValue(TEXT("binding_kind"), Binding.BindingKind);
			Writer->WriteValue(TEXT("ue_member"), Binding.UeMember);
		}
		else
		{
			Writer->WriteValue(TEXT("ue_function"), Binding.Function->GetName());
		}
		Writer->WriteValue(TEXT("script_name"), Binding.ScriptName);
		Writer->WriteValue(
			TEXT("dispatch_mode"),
			Binding.BindingKind == TEXT("function") ? TEXT("cached_process_event") : TEXT("cached_property_get"));
		Writer->WriteValue(TEXT("is_static"), Binding.Function != nullptr && Binding.Function->HasAnyFunctionFlags(FUNC_Static));
		Writer->WriteValue(TEXT("is_const"), Binding.Function == nullptr || Binding.Function->HasAnyFunctionFlags(FUNC_Const));
		Writer->WriteValue(TEXT("reload_effect"), LexToString(Binding.ReloadEffect));
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
	OutResult.ClassReferenceCount = Package.ClassReferences.Num();
	OutResult.PackageHash = PackageHash;
	OutResult.SelectionHash = SelectionHash;
	return true;
}
} // namespace

bool FAvidScriptEditorBindingDescriptorGenerator::GenerateWithClassReferences(
	const FString& PackageName,
	const TArray<FAvidScriptReflectedFunctionSelection>& FunctionSelections,
	const TArray<FAvidScriptReflectedPropertySelection>& PropertySelections,
	const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
	FString& OutJson,
	FAvidScriptBindingDescriptorGenerateResult& OutResult)
{
	return GenerateBindingDescriptor(
		PackageName,
		FunctionSelections,
		PropertySelections,
		ClassReferences,
		nullptr,
		OutJson,
		OutResult);
}

bool FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
	const FAvidScriptBindingSelectionProfile& Profile,
	FString& OutJson,
	FAvidScriptBindingSelectionResolveResult& OutSelectionResult,
	FAvidScriptBindingDescriptorGenerateResult& OutResult)
{
	return GenerateFromProfile(
		Profile,
		{},
		OutJson,
		OutSelectionResult,
		OutResult);
}

bool FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
	const FAvidScriptBindingSelectionProfile& Profile,
	const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
	FString& OutJson,
	FAvidScriptBindingSelectionResolveResult& OutSelectionResult,
	FAvidScriptBindingDescriptorGenerateResult& OutResult)
{
	OutJson.Empty();
	OutResult = FAvidScriptBindingDescriptorGenerateResult();
	TArray<FAvidScriptReflectedFunctionSelection> FunctionSelections;
	if (!FAvidScriptEditorBindingSelectionResolver::Resolve(
		Profile,
		FunctionSelections,
		OutSelectionResult))
	{
		SetFailure(
			OutResult,
			OutSelectionResult.ErrorCategory,
			OutSelectionResult.ErrorSource,
			OutSelectionResult.NextAction);
		return false;
	}

	bool bRequestsReadableProperties = !Profile.ExplicitProperties.IsEmpty();
	for (const FAvidScriptReflectedClassSelection& Rule : Profile.Classes)
	{
		bRequestsReadableProperties |= Rule.bDiscoverReadableProperties || !Rule.IncludeProperties.IsEmpty();
	}
	TArray<FAvidScriptReflectedPropertySelection> PropertySelections;
	if (bRequestsReadableProperties)
	{
		FAvidScriptBindingSelectionResolveResult PropertyResult;
		if (!FAvidScriptEditorBindingPropertySelectionResolver::ResolveReadable(
			Profile,
			PropertySelections,
			PropertyResult))
		{
			OutSelectionResult.bSucceeded = false;
			OutSelectionResult.CandidatePropertyCount = PropertyResult.CandidatePropertyCount;
			OutSelectionResult.AcceptedPropertyCount = PropertyResult.AcceptedPropertyCount;
			OutSelectionResult.RejectedPropertyCount = PropertyResult.RejectedPropertyCount;
			OutSelectionResult.Issues.Append(PropertyResult.Issues);
			OutSelectionResult.ErrorCategory = PropertyResult.ErrorCategory;
			OutSelectionResult.ErrorSource = PropertyResult.ErrorSource;
			OutSelectionResult.NextAction = PropertyResult.NextAction;
			OutSelectionResult.ErrorMessage = PropertyResult.ErrorMessage;
			SetFailure(
				OutResult,
				PropertyResult.ErrorCategory,
				PropertyResult.ErrorSource,
				PropertyResult.NextAction);
			return false;
		}
		OutSelectionResult.CandidatePropertyCount = PropertyResult.CandidatePropertyCount;
		OutSelectionResult.AcceptedPropertyCount = PropertyResult.AcceptedPropertyCount;
		OutSelectionResult.RejectedPropertyCount = PropertyResult.RejectedPropertyCount;
		OutSelectionResult.Issues.Append(PropertyResult.Issues);
	}
	OutSelectionResult.bSucceeded = true;
	UClass* SelfClass = nullptr;
	if (!Profile.SelfClassPath.IsEmpty())
	{
		SelfClass = LoadObject<UClass>(nullptr, *Profile.SelfClassPath);
		if (SelfClass == nullptr)
		{
			SetFailure(
				OutResult,
				TEXT("self_class_missing"),
				Profile.SelfClassPath,
				TEXT("Use a loadable canonical Actor class path for the profile self type."));
			return false;
		}
	}
	else
	{
		const auto HasActorLifecycleOwner = [](const FString& OwnerClassPath)
		{
			UClass* OwnerClass = LoadObject<UClass>(nullptr, *OwnerClassPath);
			return OwnerClass != nullptr && OwnerClass->IsChildOf(AActor::StaticClass());
		};
		for (const FAvidScriptReflectedFunctionSelection& Selection : FunctionSelections)
		{
			if (HasActorLifecycleOwner(Selection.OwnerClassPath))
			{
				SelfClass = AActor::StaticClass();
				break;
			}
		}
		if (SelfClass == nullptr)
		{
			for (const FAvidScriptReflectedPropertySelection& Selection : PropertySelections)
			{
				if (HasActorLifecycleOwner(Selection.OwnerClassPath))
				{
					SelfClass = AActor::StaticClass();
					break;
				}
			}
		}
	}
	return GenerateBindingDescriptor(
		Profile.PackageName,
		FunctionSelections,
		PropertySelections,
		ClassReferences,
		SelfClass,
		OutJson,
		OutResult);
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
