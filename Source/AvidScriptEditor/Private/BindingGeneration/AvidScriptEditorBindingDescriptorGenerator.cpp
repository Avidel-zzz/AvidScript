#include "AvidScriptEditorBindingDescriptorGenerator.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptEditorBindingPropertySelectionResolver.h"
#include "AvidScriptEditorBindingSelectionResolver.h"
#include "AvidScriptHash.h"
#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"
#include "BindingGeneration/AvidScriptEditorBindingReloadEffectPolicy.h"
#include "BindingGeneration/AvidScriptEditorObjectTypeGraph.h"
#include "BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.h"
#include "BindingGeneration/AvidScriptEditorReflectedPropertyPolicy.h"
#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
constexpr const TCHAR* GeneratorVersion = TEXT("50.1.0");
constexpr const TCHAR* ObjectFactoryGeneratorVersion = TEXT("51.1.0");
constexpr const TCHAR* WritablePropertyGeneratorVersion = TEXT("52.1.0");
constexpr const TCHAR* NativeDirectGeneratorVersion = TEXT("54.5.0");
constexpr const TCHAR* GeneratedNativeGeneratorVersion = TEXT("54.6.0");

struct FResolvedBindingDescriptor
{
	FAvidScriptReflectedFunctionSelection Selection;
	UClass* OwnerClass = nullptr;
	UFunction* Function = nullptr;
	FProperty* Property = nullptr;
	FString BindingKind = TEXT("function");
	FString UeMember;
	FString UeFunction;
	FAvidScriptProjectedFunction Projection;
	FString ScriptName;
	FString CanonicalIdentity;
	FString StableId;
	FString ImportName;
	FString DispatchMode = TEXT("cached_process_event");
	FString GeneratedShape;
	FString GeneratedReceiverMode;
	FString WritePolicy = TEXT("none");
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
	const FAvidScriptProjectedFunction& Projection,
	const FString& DispatchMode)
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
	return FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
		Identity,
		DispatchMode);
}

bool ResolveGeneratedFunctionShape(
	const UClass* OwnerClass,
	const UFunction* Function,
	const FAvidScriptProjectedFunction& Projection,
	FString& OutShape,
	FString& OutReceiverMode,
	FString& OutCategory)
{
	OutShape.Reset();
	OutReceiverMode.Reset();
	OutCategory.Reset();
	if (OwnerClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
	{
		OutCategory = TEXT("generated_native_blueprint_owner");
		return false;
	}
	if (OwnerClass->HasAnyClassFlags(CLASS_Interface)
		|| Function->HasAnyFunctionFlags(
			FUNC_Static | FUNC_Event | FUNC_Net | FUNC_Delegate
				| FUNC_MulticastDelegate)
		|| !Function->HasAnyFunctionFlags(FUNC_Native)
		|| Function->HasMetaData(TEXT("CustomThunk")))
	{
		OutCategory = TEXT("generated_native_callable_unsupported");
		return false;
	}
	for (const FAvidScriptProjectedBindingValue& Parameter : Projection.Parameters)
	{
		if (Parameter.Direction != TEXT("value"))
		{
			OutCategory = TEXT("generated_native_reference_direction_unsupported");
			return false;
		}
	}

	const FString& ReturnType = Projection.ReturnValue.Type.CanonicalType;
	if (ReturnType == TEXT("scalar:i32")
		&& Projection.Parameters.Num() == 2
		&& Projection.Parameters[0].Type.CanonicalType == TEXT("scalar:i32")
		&& Projection.Parameters[1].Type.CanonicalType == TEXT("scalar:i32"))
	{
		OutShape = TEXT("i32_pair_to_i32");
		OutReceiverMode = TEXT("self_bound");
		return true;
	}
	if (Projection.Parameters.Num() == 1
		&& ReturnType == TEXT("struct:/Script/CoreUObject.Vector")
		&& Projection.Parameters[0].Type.CanonicalType == ReturnType)
	{
		OutShape = TEXT("vector_value");
		OutReceiverMode = TEXT("self_bound");
		return true;
	}
	if (Projection.Parameters.Num() == 1
		&& ReturnType == TEXT("object:/Script/CoreUObject.Object")
		&& Projection.Parameters[0].Type.CanonicalType == ReturnType)
	{
		OutShape = TEXT("stable_object_roundtrip");
		OutReceiverMode = TEXT("stable_borrow");
		return true;
	}

	OutCategory = TEXT("generated_native_shape_unsupported");
	return false;
}

void FinalizeType(FAvidScriptProjectedBindingType& Type)
{
	Type.StableId = FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
		Type.CanonicalType,
		Type.EnumValues);
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
	const TSet<FString>& NativeDirectFunctionKeys,
	const TSet<FString>& GeneratedNativeFunctionKeys,
	const TArray<FAvidScriptReflectedPropertySelection>& PropertySelections,
	const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
	const TArray<FAvidScriptProjectObjectFactorySpec>& ObjectFactories,
	UClass* RequestedSelfClass,
	const bool bUseDefaultActorSelf,
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
		&& ClassReferences.IsEmpty()
		&& ObjectFactories.IsEmpty()
		&& RequestedSelfClass == nullptr)
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
		const bool bGeneratedNative =
			GeneratedNativeFunctionKeys.Contains(SelectionKey);
		Binding.DispatchMode = bGeneratedNative
			? FString(TEXT("generated_native_s1"))
			: NativeDirectFunctionKeys.Contains(SelectionKey)
				? FString(TEXT("qualified_native_direct"))
				: FString(TEXT("cached_process_event"));
		const FString SemanticCanonicalIdentity = MakeCanonicalIdentity(
			OwnerClass,
			Function,
			Binding.Projection,
			TEXT("cached_process_event"));
		if (bGeneratedNative)
		{
			FString EligibilityCategory;
			if (!ResolveGeneratedFunctionShape(
					OwnerClass,
					Function,
					Binding.Projection,
					Binding.GeneratedShape,
					Binding.GeneratedReceiverMode,
					EligibilityCategory))
			{
				SetFailure(
					OutResult,
					EligibilityCategory,
					SelectionKey,
					TEXT("Select a native instance callable supported by the generated S1 shape contract."));
				return false;
			}
			const FString OwnerModule = OwnerClass->GetOutermost()->GetName()
				.Replace(TEXT("/Script/"), TEXT(""));
			const FString OwnerHeader =
				OwnerClass->GetMetaData(TEXT("ModuleRelativePath"));
			if (OwnerModule.IsEmpty() || OwnerHeader.IsEmpty())
			{
				SetFailure(
					OutResult,
					TEXT("generated_native_owner_identity_missing"),
					SelectionKey,
					TEXT("Provide UHT owner module and ModuleRelativePath metadata."));
				return false;
			}
			Binding.ImportName =
				TEXT("avid_s1_") + HashSha256(SemanticCanonicalIdentity).Left(16);
			Binding.CanonicalIdentity =
				FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
					SemanticCanonicalIdentity,
					Binding.DispatchMode,
					Binding.GeneratedShape,
					Binding.GeneratedReceiverMode,
					Binding.ImportName);
		}
		else
		{
			Binding.CanonicalIdentity = MakeCanonicalIdentity(
				OwnerClass,
				Function,
				Binding.Projection,
				Binding.DispatchMode);
		}
		Binding.StableId = HashSha256(Binding.CanonicalIdentity);
		if (!bGeneratedNative)
		{
			Binding.ImportName = TEXT("avid_ue_") + Binding.StableId.Left(16);
		}
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
		Binding.DispatchMode = TEXT("cached_property_get");
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

		if (Selection.bWritable)
		{
			FString SetterDispatchMode;
			FString SetterWritePolicy;
			if (!FAvidScriptEditorReflectedPropertyPolicy::EvaluateWritable(
					Property,
					SetterDispatchMode,
					SetterWritePolicy,
					PropertyPolicyCategory,
					PropertyPolicySource))
			{
				SetFailure(
					OutResult,
					PropertyPolicyCategory,
					PropertyPolicySource,
					TEXT("Remove the property from writable_properties or use a setter-compatible runtime property."));
				return false;
			}

			FResolvedBindingDescriptor SetterBinding;
			SetterBinding.OwnerClass = OwnerClass;
			SetterBinding.Property = Property;
			SetterBinding.BindingKind = TEXT("property_set");
			SetterBinding.UeMember = Property->GetName();
			SetterBinding.UeFunction =
				Property->GetMetaData(TEXT("BlueprintSetter"));
			SetterBinding.DispatchMode = MoveTemp(SetterDispatchMode);
			SetterBinding.WritePolicy = MoveTemp(SetterWritePolicy);
			SetterBinding.Projection.ReturnValue.Name = TEXT("return");
			SetterBinding.Projection.ReturnValue.Direction = TEXT("return");
			SetterBinding.Projection.ReturnValue.Type =
				FAvidScriptEditorReflectedTypePolicy::MakeVoidType();
			FinalizeType(SetterBinding.Projection.ReturnValue.Type);

			FAvidScriptProjectedBindingValue Value = Binding.Projection.ReturnValue;
			Value.Name = TEXT("value");
			Value.Direction = TEXT("value");
			Value.bHasDefaultValue = false;
			Value.DefaultValue.Empty();
			SetterBinding.Projection.Parameters.Add(MoveTemp(Value));
			SetterBinding.Projection.AbiSignature = TEXT("(ii")
				+ FString::Join(
					SetterBinding.Projection.Parameters[0].Type.AbiValueTypes,
					TEXT(""))
				+ TEXT(")i");
			SetterBinding.ScriptName = Property->GetAuthoredName();
			SetterBinding.CanonicalIdentity =
				FAvidScriptBindingDescriptorIdentity::MakePropertySetCanonicalIdentity(
					OwnerClass->GetPathName(),
					Property->GetName(),
					SetterBinding.Projection.Parameters[0].Type.CanonicalType,
					SetterBinding.UeFunction);
			SetterBinding.StableId = HashSha256(SetterBinding.CanonicalIdentity);
			SetterBinding.ImportName = TEXT("avid_ue_")
				+ SetterBinding.StableId.Left(16);
			SetterBinding.ReloadEffect =
				EAvidScriptBindingReloadEffect::ReflectedProperty;
			Bindings.Add(MoveTemp(SetterBinding));
		}
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
	const bool bHasWritableProperties = Bindings.ContainsByPredicate(
		[](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.BindingKind == TEXT("property_set");
		});
	const bool bHasNativeDirectFunctions = Bindings.ContainsByPredicate(
		[](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.BindingKind == TEXT("function")
				&& Binding.DispatchMode == TEXT("qualified_native_direct");
		});
	const bool bHasGeneratedNativeFunctions = Bindings.ContainsByPredicate(
		[](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.BindingKind == TEXT("function")
				&& Binding.DispatchMode == TEXT("generated_native_s1");
		});
	Package.SchemaVersion = bHasWritableProperties || bHasNativeDirectFunctions
			|| bHasGeneratedNativeFunctions
		? 8
		: ObjectFactories.IsEmpty()
			? 6
			: 7;
	Package.GeneratorVersion = bHasGeneratedNativeFunctions
		? GeneratedNativeGeneratorVersion
		: bHasNativeDirectFunctions
		? NativeDirectGeneratorVersion
		: bHasWritableProperties
			? WritablePropertyGeneratorVersion
			: ObjectFactories.IsEmpty()
				? GeneratorVersion
				: ObjectFactoryGeneratorVersion;
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
		BindingModel.UeFunction = Binding.Function == nullptr
			? Binding.UeFunction
			: Binding.Function->GetName();
		BindingModel.ScriptName = Binding.ScriptName;
		BindingModel.DispatchMode = Binding.DispatchMode;
		BindingModel.GeneratedShape = Binding.GeneratedShape;
		BindingModel.GeneratedReceiverMode = Binding.GeneratedReceiverMode;
		BindingModel.GeneratedImportName = Binding.ImportName;
		BindingModel.SemanticFallbackOrdinal = Binding.Ordinal;
		BindingModel.WritePolicy = Binding.WritePolicy;
		BindingModel.bStatic = Binding.Function != nullptr && Binding.Function->HasAnyFunctionFlags(FUNC_Static);
		BindingModel.bConst = Binding.BindingKind == TEXT("property_get")
			|| (Binding.Function != nullptr
				&& Binding.Function->HasAnyFunctionFlags(FUNC_Const));
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

	const bool bPureStaticPackage = Package.ClassReferences.IsEmpty()
		&& Bindings.ContainsByPredicate([](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.Function == nullptr
				|| !Binding.Function->HasAnyFunctionFlags(FUNC_Static);
		}) == false;
	UClass* SelfClass = RequestedSelfClass;
	if (SelfClass == nullptr && bUseDefaultActorSelf && !bPureStaticPackage)
	{
		SelfClass = AActor::StaticClass();
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
			ObjectFactories,
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

	for (const FAvidScriptEditorObjectTypeNode& Node : ObjectTypeGraph.Nodes)
	{
		FAvidScriptBindingTypeModel* TypeModel = Package.Types.FindByPredicate(
			[&Node](const FAvidScriptBindingTypeModel& Type)
			{
				return Type.StableId == Node.TypeId;
			});
		if (TypeModel == nullptr)
		{
			UClass* ObjectClass = LoadObject<UClass>(nullptr, *Node.CanonicalClassPath);
			if (ObjectClass == nullptr)
			{
				SetFailure(
					OutResult,
					TEXT("object_type_class_missing"),
					Node.CanonicalClassPath,
					TEXT("Keep graph classes loaded until descriptor model publication completes."));
				return false;
			}
			const FAvidScriptProjectedBindingType ProjectedType =
				FAvidScriptEditorReflectedTypePolicy::MakeObjectType(ObjectClass);
			FAvidScriptBindingTypeModel AddedType;
			AddedType.StableId = Node.TypeId;
			AddedType.CanonicalType = TEXT("object:") + Node.CanonicalClassPath;
			AddedType.Kind = ProjectedType.Kind;
			AddedType.CppType = ProjectedType.CppType;
			AddedType.Size = ProjectedType.Size;
			AddedType.Alignment = ProjectedType.Alignment;
			AddedType.AbiTypes = ProjectedType.AbiValueTypes;
			TypeModel = &Package.Types.Add_GetRef(MoveTemp(AddedType));
		}
		if (TypeModel->CanonicalType != TEXT("object:") + Node.CanonicalClassPath
			|| TypeModel->Kind != TEXT("object_handle"))
		{
			SetFailure(
				OutResult,
				TEXT("object_type_model_mismatch"),
				Node.CanonicalClassPath,
				TEXT("Use the object type graph as the sole source of v6 object identity."));
			return false;
		}
		TypeModel->ObjectTypeOrdinal = Node.Ordinal;
		TypeModel->ClassPath = Node.CanonicalClassPath;
		TypeModel->BaseTypeId = Node.BaseTypeId;
	}
	Package.Types.Sort([](
		const FAvidScriptBindingTypeModel& Left,
		const FAvidScriptBindingTypeModel& Right)
	{
		return Left.CanonicalType.Compare(Right.CanonicalType, ESearchCase::CaseSensitive) < 0;
	});

	if (SelfClass != nullptr)
	{
		const FAvidScriptEditorObjectTypeNode* SelfNode = ObjectTypeGraph.Nodes.FindByPredicate(
			[SelfClass](const FAvidScriptEditorObjectTypeNode& Node)
			{
				return Node.CanonicalClassPath == SelfClass->GetPathName();
			});
		if (SelfNode == nullptr)
		{
			SetFailure(
				OutResult,
				TEXT("self_type_missing"),
				SelfClass->GetPathName(),
				TEXT("Publish Self through the v6 object type graph."));
			return false;
		}
		Package.SelfTypeId = SelfNode->TypeId;
	}
	for (FAvidScriptBindingClassReferenceModel& Reference : Package.ClassReferences)
	{
		const FAvidScriptEditorObjectTypeNode* ResultNode = ObjectTypeGraph.Nodes.FindByPredicate(
			[&Reference](const FAvidScriptEditorObjectTypeNode& Node)
			{
				return Node.CanonicalClassPath == Reference.BaseClassPath;
			});
		if (ResultNode == nullptr)
		{
			SetFailure(
				OutResult,
				TEXT("class_reference_result_type_missing"),
				Reference.BaseClassPath,
				TEXT("Publish class-reference result types through the v6 object type graph."));
			return false;
		}
		Reference.ResultTypeId = ResultNode->TypeId;
	}

	TSet<FString> ObjectFactoryStableIds;
	TSet<FString> ObjectFactoryScriptNames;
	for (const FAvidScriptProjectObjectFactorySpec& Factory : ObjectFactories)
	{
		const FAvidScriptBindingClassReferenceModel* ClassReference =
			Package.ClassReferences.FindByPredicate(
				[&Factory](
					const FAvidScriptBindingClassReferenceModel& Candidate)
				{
					return Candidate.ScriptName == Factory.ClassReference;
				});
		const FAvidScriptEditorObjectTypeNode* OuterNode =
			ObjectTypeGraph.Nodes.FindByPredicate(
				[&Factory](const FAvidScriptEditorObjectTypeNode& Node)
				{
					return Node.CanonicalClassPath
						== Factory.OuterBaseClassPath;
				});
		if (ClassReference == nullptr || OuterNode == nullptr)
		{
			SetFailure(
				OutResult,
				ClassReference == nullptr
					? FString(TEXT("binding_factory_class_reference_missing"))
					: FString(TEXT("binding_factory_outer_type_missing")),
				ClassReference == nullptr
					? Factory.ClassReference
					: Factory.OuterBaseClassPath,
				TEXT("Resolve factory class and Outer identities through the descriptor object graph."));
			return false;
		}

		FAvidScriptBindingObjectFactoryModel FactoryModel;
		FactoryModel.ScriptName = Factory.ScriptName;
		FactoryModel.ClassReferenceId = ClassReference->StableId;
		FactoryModel.OuterTypeId = OuterNode->TypeId;
		switch (Factory.Kind)
		{
		case EAvidScriptProjectObjectFactoryKind::NewObject:
			FactoryModel.Kind = EAvidScriptObjectFactoryKind::NewObject;
			break;
		case EAvidScriptProjectObjectFactoryKind::ActorComponent:
			FactoryModel.Kind =
				EAvidScriptObjectFactoryKind::ActorComponent;
			break;
		default:
			SetFailure(
				OutResult,
				TEXT("binding_factory_kind_invalid"),
				Factory.ScriptName,
				TEXT("Use a resolved new_object or actor_component factory."));
			return false;
		}
		switch (Factory.Ownership)
		{
		case EAvidScriptProjectObjectOwnership::Session:
			FactoryModel.Ownership =
				EAvidScriptObjectOwnershipPolicy::Session;
			break;
		default:
			SetFailure(
				OutResult,
				TEXT("binding_factory_ownership_invalid"),
				Factory.ScriptName,
				TEXT("Use resolved session ownership."));
			return false;
		}
		switch (Factory.Registration)
		{
		case EAvidScriptProjectComponentRegistration::None:
			FactoryModel.Registration =
				EAvidScriptComponentRegistrationPolicy::None;
			break;
		case EAvidScriptProjectComponentRegistration::RegisterInstance:
			FactoryModel.Registration =
				EAvidScriptComponentRegistrationPolicy::RegisterInstance;
			break;
		default:
			SetFailure(
				OutResult,
				TEXT("binding_factory_registration_invalid"),
				Factory.ScriptName,
				TEXT("Use a resolved component registration policy."));
			return false;
		}
		FactoryModel.StableId =
			FAvidScriptBindingDescriptorIdentity::MakeObjectFactoryStableId(
				FactoryModel.ClassReferenceId,
				FactoryModel.Kind,
				FactoryModel.OuterTypeId,
				FactoryModel.Ownership,
				FactoryModel.Registration);
		if (FactoryModel.ScriptName.IsEmpty()
			|| ObjectFactoryStableIds.Contains(FactoryModel.StableId)
			|| ObjectFactoryScriptNames.Contains(FactoryModel.ScriptName))
		{
			SetFailure(
				OutResult,
				TEXT("binding_factory_invalid"),
				FactoryModel.ScriptName,
				TEXT("Resolve duplicate or invalid object factory declarations before descriptor generation."));
			return false;
		}
		ObjectFactoryStableIds.Add(FactoryModel.StableId);
		ObjectFactoryScriptNames.Add(FactoryModel.ScriptName);
		Package.ObjectFactories.Add(MoveTemp(FactoryModel));
	}
	Package.ObjectFactories.Sort([](
		const FAvidScriptBindingObjectFactoryModel& Left,
		const FAvidScriptBindingObjectFactoryModel& Right)
	{
		return Left.StableId.Compare(
			Right.StableId,
			ESearchCase::CaseSensitive) < 0;
	});
	for (int32 Index = 0; Index < Package.ObjectFactories.Num(); ++Index)
	{
		Package.ObjectFactories[Index].Ordinal = Index;
	}

	Package.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
	Package.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);
	if (!FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical(
			Package,
			OutJson))
	{
		OutJson.Empty();
		SetFailure(
			OutResult,
			TEXT("serialize_failed"),
			PackageName,
			TEXT("Inspect the canonical binding descriptor serializer state."));
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.BindingCount = Bindings.Num();
	OutResult.TypeCount = Package.Types.Num();
	OutResult.ClassReferenceCount = Package.ClassReferences.Num();
	OutResult.ObjectFactoryCount = Package.ObjectFactories.Num();
	OutResult.PackageHash = Package.PackageHash;
	OutResult.SelectionHash = Package.SelectionHash;
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
	return GenerateWithObjectFactories(
		PackageName,
		FunctionSelections,
		PropertySelections,
		ClassReferences,
		{},
		OutJson,
		OutResult);
}

bool FAvidScriptEditorBindingDescriptorGenerator::GenerateWithObjectFactories(
	const FString& PackageName,
	const TArray<FAvidScriptReflectedFunctionSelection>& FunctionSelections,
	const TArray<FAvidScriptReflectedPropertySelection>& PropertySelections,
	const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
	const TArray<FAvidScriptProjectObjectFactorySpec>& ObjectFactories,
	FString& OutJson,
	FAvidScriptBindingDescriptorGenerateResult& OutResult)
{
	return GenerateBindingDescriptor(
		PackageName,
		FunctionSelections,
		{},
		{},
		PropertySelections,
		ClassReferences,
		ObjectFactories,
		nullptr,
		true,
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
	return GenerateFromProfile(
		Profile,
		ClassReferences,
		{},
		OutJson,
		OutSelectionResult,
		OutResult);
}

bool FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
	const FAvidScriptBindingSelectionProfile& Profile,
	const TArray<FAvidScriptProjectBindingClassSpec>& ClassReferences,
	const TArray<FAvidScriptProjectObjectFactorySpec>& ObjectFactories,
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
		const bool bHasNonFunctionSurface =
			!Profile.ExplicitProperties.IsEmpty()
			|| !Profile.SelfClassPath.IsEmpty()
			|| !ClassReferences.IsEmpty()
			|| !ObjectFactories.IsEmpty();
		if (OutSelectionResult.ErrorCategory != TEXT("profile_empty") || !bHasNonFunctionSurface)
		{
			SetFailure(
				OutResult,
				OutSelectionResult.ErrorCategory,
				OutSelectionResult.ErrorSource,
				OutSelectionResult.NextAction);
			return false;
		}
		OutSelectionResult = FAvidScriptBindingSelectionResolveResult();
	}

	bool bRequestsReadableProperties = !Profile.ExplicitProperties.IsEmpty();
	for (const FAvidScriptReflectedClassSelection& Rule : Profile.Classes)
	{
		bRequestsReadableProperties |= Rule.bDiscoverReadableProperties
			|| !Rule.IncludeProperties.IsEmpty()
			|| !Rule.WritableProperties.IsEmpty();
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
			OutSelectionResult.CandidateWritablePropertyCount = PropertyResult.CandidateWritablePropertyCount;
			OutSelectionResult.AcceptedWritablePropertyCount = PropertyResult.AcceptedWritablePropertyCount;
			OutSelectionResult.RejectedWritablePropertyCount = PropertyResult.RejectedWritablePropertyCount;
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
		OutSelectionResult.CandidateWritablePropertyCount = PropertyResult.CandidateWritablePropertyCount;
		OutSelectionResult.AcceptedWritablePropertyCount = PropertyResult.AcceptedWritablePropertyCount;
		OutSelectionResult.RejectedWritablePropertyCount = PropertyResult.RejectedWritablePropertyCount;
		OutSelectionResult.Issues.Append(PropertyResult.Issues);
	}
	OutSelectionResult.bSucceeded = true;
	TSet<FString> NativeDirectFunctionKeys;
	TSet<FString> GeneratedNativeFunctionKeys;
	for (const FAvidScriptReflectedClassSelection& Rule : Profile.Classes)
	{
		for (const FName FunctionName : Rule.NativeDirectFunctions)
		{
			NativeDirectFunctionKeys.Add(
				MakeSelectionKey({ Rule.OwnerClassPath, FunctionName }));
		}
		for (const FName FunctionName : Rule.GeneratedNativeFunctions)
		{
			GeneratedNativeFunctionKeys.Add(
				MakeSelectionKey({ Rule.OwnerClassPath, FunctionName }));
		}
	}
	UClass* SelfClass = nullptr;
	if (!Profile.SelfClassPath.IsEmpty())
	{
		SelfClass = LoadObject<UClass>(nullptr, *Profile.SelfClassPath);
		if (SelfClass == nullptr
			|| SelfClass->GetPathName() != Profile.SelfClassPath
			|| !SelfClass->IsChildOf(AActor::StaticClass()))
		{
			SetFailure(
				OutResult,
				TEXT("self_class_missing"),
				Profile.SelfClassPath,
				TEXT("Use a loadable canonical Actor class path for the profile Self type."));
			return false;
		}
	}
	return GenerateBindingDescriptor(
		Profile.PackageName,
		FunctionSelections,
		NativeDirectFunctionKeys,
		GeneratedNativeFunctionKeys,
		PropertySelections,
		ClassReferences,
		ObjectFactories,
		SelfClass,
		true,
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
