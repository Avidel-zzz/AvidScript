#include "AvidScriptEditorBindingDescriptorGenerator.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingLatent.h"
#include "AvidScriptEditorBindingDelegateEventSelectionResolver.h"
#include "AvidScriptEditorBindingPropertySelectionResolver.h"
#include "AvidScriptEditorBindingSelectionResolver.h"
#include "AvidScriptHash.h"
#include "BindingGeneration/AvidScriptEditorBindingDescriptorModel.h"
#include "BindingGeneration/AvidScriptEditorBindingReloadEffectPolicy.h"
#include "BindingGeneration/AvidScriptEditorObjectTypeGraph.h"
#include "BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.h"
#include "BindingGeneration/AvidScriptEditorReflectedDelegateEventPolicy.h"
#include "BindingGeneration/AvidScriptEditorReflectedPropertyPolicy.h"
#include "BindingGeneration/AvidScriptEditorReflectedTypePolicy.h"
#include "BindingGeneration/AvidScriptEditorCSharpSyntax.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet/BlueprintAsyncActionBase.h"
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
constexpr const TCHAR* GeneratedNativeGeneratorVersion = TEXT("55.1.0");
constexpr const TCHAR* StructWireGeneratorVersion = TEXT("57.11B1.0");
constexpr const TCHAR* ArrayGeneratorVersion = TEXT("57.11B3.0");
constexpr const TCHAR* DelegateEventGeneratorVersion = TEXT("57.12A.0");
constexpr const TCHAR* LatentGeneratorVersion = TEXT("57.12C5.0");
constexpr const TCHAR* LatentPayloadGeneratorVersion = TEXT("57.12C10.0");
constexpr const TCHAR* NetworkGeneratorVersion = TEXT("57.12D1.0");
constexpr const TCHAR* ReplicatedPropertyGeneratorVersion = TEXT("57.12D2.0");
constexpr const TCHAR* InboundHandlerGeneratorVersion = TEXT("57.12D4.0");
constexpr const TCHAR* CompositeValueGeneratorVersion = TEXT("58.1.0");
constexpr const TCHAR* DelegateOutputGeneratorVersion = TEXT("58.2.0");
constexpr const TCHAR* DelegateValueGeneratorVersion = TEXT("60.2.0");
constexpr const TCHAR* BlueprintFunctionGeneratorVersion = TEXT("60.3.0");
constexpr const TCHAR* BlueprintEventGeneratorVersion = TEXT("60.3.1");
constexpr const TCHAR* BlueprintAsyncActionGeneratorVersion = TEXT("60.4.1");

struct FResolvedAsyncActionOutcomeDescriptor
{
	FString StableId;
	FString DelegateMember;
	int32 Ordinal = INDEX_NONE;
	FAvidScriptProjectedDelegateEvent Projection;
};

struct FResolvedAsyncActionDescriptor
{
	UClass* ActionClass = nullptr;
	FAvidScriptProjectedBindingType PayloadType;
	TArray<FResolvedAsyncActionOutcomeDescriptor> Outcomes;

	bool IsEnabled() const
	{
		return ActionClass != nullptr;
	}
};

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
	FString LatentInfoParameter;
	FString WorldContextParameter;
	FString CompletionMode = TEXT("none");
	FString CompletionProviderId;
	FString CompletionPayloadTypeId;
	FString CompletionStatusPolicy = TEXT("abandon_on_cancel");
	FResolvedAsyncActionDescriptor AsyncAction;
	FString GeneratedShape;
	FString GeneratedReceiverMode;
	FString WritePolicy = TEXT("none");
	FAvidScriptBindingNetworkContract Network;
	FAvidScriptBindingPropertyReplicationContract PropertyReplication;
	EAvidScriptBindingReloadEffect ReloadEffect = EAvidScriptBindingReloadEffect::Unsupported;
	int32 Ordinal = INDEX_NONE;
};

struct FResolvedDelegateEventDescriptor
{
	FAvidScriptReflectedDelegateEventSelection Selection;
	UClass* OwnerClass = nullptr;
	FProperty* Property = nullptr;
	UFunction* SignatureFunction = nullptr;
	FProperty* RepNotifyProperty = nullptr;
	FString CallbackKind = TEXT("multicast");
	FString HandlerMode = TEXT("replace");
	FAvidScriptBindingNetworkContract Network;
	FAvidScriptProjectedDelegateEvent Projection;
	FString ScriptName;
	FString CanonicalIdentity;
	FString StableId;
	FString ExportName;
	int32 Ordinal = INDEX_NONE;
};

void FinalizeType(FAvidScriptProjectedBindingType& Type);

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
	const FAvidScriptBindingNetworkContract& Network,
	const FString& DispatchMode,
	const FString& LatentInfoParameter = FString(),
	const FString& WorldContextParameter = FString(),
	const FString& CompletionMode = TEXT("none"),
	const FString& CompletionProviderId = FString(),
	const FString& CompletionPayloadTypeId = FString(),
	const FString& CompletionStatusPolicy = TEXT("abandon_on_cancel"),
	const FResolvedAsyncActionDescriptor* AsyncAction = nullptr)
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
	if (Network.IsNetworked())
	{
		Identity += TEXT("|network_mode=") + FString(LexToString(Network.Mode))
			+ TEXT("|network_reliable=")
			+ (Network.bReliable ? TEXT("1") : TEXT("0"));
	}
	if (DispatchMode == TEXT("latent_process_event"))
	{
		Identity += TEXT("|latent_info=") + LatentInfoParameter
			+ TEXT("|world_context=") + WorldContextParameter;
		if (CompletionMode == TEXT("provider"))
		{
			Identity += TEXT("|completion=provider|provider_id=")
				+ CompletionProviderId
				+ TEXT("|payload_type_id=")
				+ CompletionPayloadTypeId
				+ TEXT("|status_policy=")
				+ CompletionStatusPolicy
				+ TEXT("|cancellable=1");
		}
	}
	if (DispatchMode == TEXT("blueprint_async_action")
		&& AsyncAction != nullptr
		&& AsyncAction->IsEnabled())
	{
		Identity += TEXT("|async_action_class=")
			+ AsyncAction->ActionClass->GetPathName()
			+ TEXT("|activation=Activate|payload_type_id=")
			+ AsyncAction->PayloadType.StableId
			+ TEXT("|completion_policy=first_broadcast_wins|cancellable=1");
		for (const FResolvedAsyncActionOutcomeDescriptor& Outcome :
			AsyncAction->Outcomes)
		{
			Identity += TEXT("|outcome=")
				+ FString::FromInt(Outcome.Ordinal)
				+ TEXT(":") + Outcome.DelegateMember
				+ TEXT(":") + Outcome.StableId;
		}
	}
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
		if (Parameter.Direction != TEXT("value")
			&& Parameter.Direction != TEXT("const_ref"))
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

FString MakeGeneratedFunctionAbiSignature(const FString& Shape)
{
	if (Shape == TEXT("i32_pair_to_i32"))
	{
		return TEXT("(iiii)i");
	}
	if (Shape == TEXT("vector_value"))
	{
		return TEXT("(iii)i");
	}
	if (Shape == TEXT("stable_object_roundtrip"))
	{
		return TEXT("(iiiii)i");
	}
	return FString();
}

bool ResolveGeneratedPropertyShape(
	const UClass* OwnerClass,
	const FProperty* Property,
	FString& OutCategory)
{
	OutCategory.Reset();
	if (!OwnerClass->HasAnyClassFlags(CLASS_Native)
		|| OwnerClass->HasAnyClassFlags(
			CLASS_CompiledFromBlueprint | CLASS_Interface))
	{
		OutCategory = TEXT("generated_native_property_owner_unsupported");
		return false;
	}
	if (!Property->IsA<FIntProperty>())
	{
		OutCategory = TEXT("generated_native_property_type_unsupported");
		return false;
	}
	if (!Property->HasAnyPropertyFlags(CPF_NativeAccessSpecifierPublic))
	{
		OutCategory = TEXT("generated_native_property_not_public");
		return false;
	}
	if (Property->HasMetaData(TEXT("BlueprintGetter"))
		|| Property->HasMetaData(TEXT("BlueprintSetter")))
	{
		OutCategory = TEXT("generated_native_property_accessor_unsupported");
		return false;
	}
	const FString OwnerModule = OwnerClass->GetOutermost()->GetName()
		.Replace(TEXT("/Script/"), TEXT(""));
	if (OwnerModule.IsEmpty()
		|| OwnerClass->GetMetaData(TEXT("ModuleRelativePath")).IsEmpty())
	{
		OutCategory = TEXT("generated_native_owner_identity_missing");
		return false;
	}
	return true;
}

void FinalizeType(FAvidScriptProjectedBindingType& Type)
{
	TArray<FString> TypeArgumentIds;
	TypeArgumentIds.Reserve(Type.TypeArguments.Num());
	for (const TSharedPtr<FAvidScriptProjectedBindingType>& Argument :
		Type.TypeArguments)
	{
		if (Argument.IsValid())
		{
			TypeArgumentIds.Add(Argument->StableId);
		}
	}
	Type.StableId = Type.Kind == TEXT("struct_wire")
		? FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
			Type.CanonicalType,
			Type.EnumValues,
			Type.StructFields,
			Type.Size,
			Type.Alignment)
		: FAvidScriptEditorBindingDescriptorIdentity::MakeTypeStableId(
			Type.CanonicalType,
			Type.EnumValues,
			Type.StructFields,
			INDEX_NONE,
			INDEX_NONE,
			Type.Kind == TEXT("array") && Type.ElementType.IsValid()
				? Type.ElementType->StableId
				: FString(),
			TypeArgumentIds);
}

FString MakeAsyncActionAbiSignature(
	const FAvidScriptProjectedFunction& Projection)
{
	FString Parameters;
	for (const FAvidScriptProjectedBindingValue& Parameter :
		Projection.Parameters)
	{
		Parameters += FString::Join(Parameter.Type.AbiValueTypes, TEXT(""));
	}
	return TEXT("(") + Parameters + TEXT("i)I");
}

bool IsBlueprintAsyncActionPayloadTypeSupported(
	const FAvidScriptProjectedBindingType& Type,
	const bool bNestedStruct = false)
{
	if (Type.Kind == TEXT("scalar")
		|| Type.Kind == TEXT("enum")
		|| Type.Kind == TEXT("struct"))
	{
		return Type.Size > 0 && Type.Size <= 4096;
	}
	if (Type.Kind == TEXT("object_handle"))
	{
		return !bNestedStruct && Type.Size == 8;
	}
	if (Type.Kind != TEXT("struct_wire")
		|| Type.Size <= 0
		|| Type.Size > 4096
		|| Type.StructFieldTypes.Num() != Type.StructFields.Num())
	{
		return false;
	}
	for (const TSharedPtr<FAvidScriptProjectedBindingType>& Child :
		Type.StructFieldTypes)
	{
		if (!Child.IsValid()
			|| !IsBlueprintAsyncActionPayloadTypeSupported(*Child, true))
		{
			return false;
		}
	}
	return true;
}

FString MakeBlueprintAsyncActionPayloadFieldName(
	const FString& Outcome,
	const FString& Parameter)
{
	return Outcome + TEXT("_") + Parameter;
}

bool ResolveBlueprintAsyncAction(
	const UFunction& Function,
	const FAvidScriptProjectedFunction& Projection,
	FResolvedAsyncActionDescriptor& OutAction,
	FString& OutCategory,
	FString& OutSource)
{
	OutAction = {};
	OutCategory.Reset();
	OutSource.Reset();
	const FObjectPropertyBase* ReturnProperty =
		CastField<FObjectPropertyBase>(Function.GetReturnProperty());
	UClass* const ActionClass = ReturnProperty == nullptr
		? nullptr
		: ReturnProperty->PropertyClass;
	if (ActionClass == nullptr
		|| !ActionClass->IsChildOf(UBlueprintAsyncActionBase::StaticClass()))
	{
		return true;
	}
	if (!Function.HasAnyFunctionFlags(FUNC_Static)
		|| Function.HasAnyFunctionFlags(FUNC_Net)
		|| Function.HasMetaData(TEXT("Latent"))
		|| Projection.Parameters.ContainsByPredicate(
			[](const FAvidScriptProjectedBindingValue& Parameter)
			{
				return Parameter.Direction != TEXT("value")
					&& Parameter.Direction != TEXT("const_ref");
			}))
	{
		OutCategory = TEXT("async_action_factory_shape_unsupported");
		OutSource = Function.GetPathName();
		return false;
	}

	TArray<FMulticastDelegateProperty*> DelegateProperties;
	for (TFieldIterator<FMulticastDelegateProperty> It(
		ActionClass,
		EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_BlueprintAssignable))
		{
			DelegateProperties.Add(*It);
		}
	}
	DelegateProperties.Sort([](
		const FMulticastDelegateProperty& Left,
		const FMulticastDelegateProperty& Right)
	{
		return Left.GetPathName().Compare(
			Right.GetPathName(),
			ESearchCase::CaseSensitive) < 0;
	});
	if (DelegateProperties.IsEmpty())
	{
		OutCategory = TEXT("async_action_outcome_missing");
		OutSource = ActionClass->GetPathName();
		return false;
	}

	OutAction.ActionClass = ActionClass;
	for (int32 Index = 0; Index < DelegateProperties.Num(); ++Index)
	{
		FMulticastDelegateProperty* const DelegateProperty =
			DelegateProperties[Index];
		FAvidScriptProjectedDelegateEvent OutcomeProjection;
		FString ProjectionCategory;
		FString ProjectionSource;
		if (!FAvidScriptEditorReflectedDelegateEventPolicy::EvaluateAndProject(
				DelegateProperty,
				OutcomeProjection,
				ProjectionCategory,
				ProjectionSource))
		{
			OutCategory = TEXT("async_action_outcome_type_unsupported");
			OutSource = ProjectionSource.IsEmpty()
				? DelegateProperty->GetPathName()
				: ProjectionSource;
			return false;
		}
		FinalizeType(OutcomeProjection.ReturnValue.Type);
		for (FAvidScriptProjectedBindingValue& Parameter :
			OutcomeProjection.Parameters)
		{
			FinalizeType(Parameter.Type);
		}
		if (OutcomeProjection.ReturnValue.Type.Kind != TEXT("void"))
		{
			OutCategory = TEXT("async_action_outcome_payload_pending");
			OutSource = DelegateProperty->GetPathName();
			return false;
		}
		for (const FAvidScriptProjectedBindingValue& Parameter :
			OutcomeProjection.Parameters)
		{
			if ((Parameter.Direction != TEXT("value")
					&& Parameter.Direction != TEXT("const_ref"))
				|| !IsBlueprintAsyncActionPayloadTypeSupported(
					Parameter.Type))
			{
				OutCategory = TEXT("async_action_outcome_payload_type_unsupported");
				OutSource = DelegateProperty->GetPathName()
					+ TEXT(":") + Parameter.Name;
				return false;
			}
		}

		FResolvedAsyncActionOutcomeDescriptor Outcome;
		Outcome.Ordinal = Index;
		Outcome.DelegateMember = DelegateProperty->GetName();
		Outcome.StableId = HashSha256(
			ActionClass->GetPathName()
			+ TEXT("::async_outcome:")
			+ Outcome.DelegateMember
			+ TEXT("::")
			+ DelegateProperty->SignatureFunction->GetPathName());
		Outcome.Projection = MoveTemp(OutcomeProjection);
		OutAction.Outcomes.Add(MoveTemp(Outcome));
	}

	FAvidScriptProjectedBindingType OutcomeType;
	OutcomeType.CanonicalType =
		TEXT("enum:avidscript_async_action:") + Function.GetPathName();
	OutcomeType.Kind = TEXT("enum");
	OutcomeType.CppType = ActionClass->GetName()
		+ TEXT("_") + Function.GetName() + TEXT("Outcome");
	OutcomeType.Size = sizeof(int32);
	OutcomeType.Alignment = alignof(int32);
	OutcomeType.AbiValueTypes = { TEXT("i") };
	bool bHasPayloadParameters = false;
	for (const FResolvedAsyncActionOutcomeDescriptor& Outcome :
		OutAction.Outcomes)
	{
		OutcomeType.EnumValues.Add({
			Outcome.DelegateMember,
			Outcome.Ordinal
		});
		bHasPayloadParameters |= !Outcome.Projection.Parameters.IsEmpty();
	}
	FinalizeType(OutcomeType);
	if (!bHasPayloadParameters)
	{
		OutAction.PayloadType = MoveTemp(OutcomeType);
		return true;
	}

	FAvidScriptProjectedBindingType ResultType;
	ResultType.CanonicalType =
		TEXT("struct_wire:avidscript_async_action_result:")
		+ Function.GetPathName();
	ResultType.Kind = TEXT("struct_wire");
	ResultType.CppType = ActionClass->GetName()
		+ TEXT("_") + Function.GetName() + TEXT("Result");
	ResultType.Alignment = OutcomeType.Alignment;
	ResultType.AbiValueTypes = { TEXT("i") };
	ResultType.StructFields.Add({
		TEXT("Outcome"),
		OutcomeType.StableId,
		0
	});
	ResultType.StructFieldTypes.Add(
		MakeShared<FAvidScriptProjectedBindingType>(OutcomeType));
	int32 WireOffset = OutcomeType.Size;
	for (const FResolvedAsyncActionOutcomeDescriptor& Outcome :
		OutAction.Outcomes)
	{
		for (const FAvidScriptProjectedBindingValue& Parameter :
			Outcome.Projection.Parameters)
		{
			WireOffset = Align(
				WireOffset,
				FMath::Max(1, Parameter.Type.Alignment));
			if (WireOffset < 0
				|| Parameter.Type.Size <= 0
				|| Parameter.Type.Size > 4096 - WireOffset)
			{
				OutCategory = TEXT("async_action_outcome_payload_size_exceeded");
				OutSource = Function.GetPathName();
				return false;
			}
			ResultType.StructFields.Add({
				MakeBlueprintAsyncActionPayloadFieldName(
					Outcome.DelegateMember,
					Parameter.Name),
				Parameter.Type.StableId,
				WireOffset
			});
			ResultType.StructFieldTypes.Add(
				MakeShared<FAvidScriptProjectedBindingType>(
					Parameter.Type));
			ResultType.Alignment = FMath::Max(
				ResultType.Alignment,
				Parameter.Type.Alignment);
			WireOffset += Parameter.Type.Size;
		}
	}
	ResultType.Size = Align(
		WireOffset,
		FMath::Max(1, ResultType.Alignment));
	if (ResultType.Size <= 0 || ResultType.Size > 4096)
	{
		OutCategory = TEXT("async_action_outcome_payload_size_exceeded");
		OutSource = Function.GetPathName();
		return false;
	}
	FinalizeType(ResultType);
	OutAction.PayloadType = MoveTemp(ResultType);
	return true;
}

FString MakeDelegateEventSelectionKey(
	const FAvidScriptReflectedDelegateEventSelection& Selection)
{
	return Selection.CallbackKind + TEXT(":")
		+ Selection.OwnerClassPath + TEXT(".")
		+ Selection.EventName.ToString();
}

void AddProjectedTypeAndChildren(
	const FAvidScriptProjectedBindingType& Type,
	TMap<FString, FAvidScriptProjectedBindingType>& TypesByCanonicalName)
{
	for (const TSharedPtr<FAvidScriptProjectedBindingType>& Child : Type.StructFieldTypes)
	{
		if (Child.IsValid())
		{
			AddProjectedTypeAndChildren(*Child, TypesByCanonicalName);
		}
	}
	if (Type.ElementType.IsValid())
	{
		AddProjectedTypeAndChildren(*Type.ElementType, TypesByCanonicalName);
	}
	for (const TSharedPtr<FAvidScriptProjectedBindingType>& Argument : Type.TypeArguments)
	{
		if (Argument.IsValid() && Argument != Type.ElementType)
		{
			AddProjectedTypeAndChildren(*Argument, TypesByCanonicalName);
		}
	}
	TypesByCanonicalName.FindOrAdd(Type.CanonicalType) = Type;
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
void AddProjectedObjectHandleClasses(
	const FAvidScriptProjectedBindingType& Type,
	TArray<UClass*>& OutHandleClasses)
{
	if (Type.Kind == TEXT("object_handle") && Type.ObjectClass != nullptr)
	{
		OutHandleClasses.AddUnique(const_cast<UClass*>(Type.ObjectClass));
	}
	for (const TSharedPtr<FAvidScriptProjectedBindingType>& Child : Type.StructFieldTypes)
	{
		if (Child.IsValid())
		{
			AddProjectedObjectHandleClasses(*Child, OutHandleClasses);
		}
	}
	if (Type.ElementType.IsValid())
	{
		AddProjectedObjectHandleClasses(*Type.ElementType, OutHandleClasses);
	}
}

bool GenerateBindingDescriptor(
	const FString& PackageName,
	const TArray<FAvidScriptReflectedFunctionSelection>& FunctionSelections,
	const TSet<FString>& NativeDirectFunctionKeys,
	const TSet<FString>& GeneratedNativeFunctionKeys,
	const TSet<FString>& GeneratedNativePropertyKeys,
	const TArray<FAvidScriptReflectedPropertySelection>& PropertySelections,
	const TArray<FAvidScriptReflectedDelegateEventSelection>& DelegateEventSelections,
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
		&& DelegateEventSelections.IsEmpty()
		&& ClassReferences.IsEmpty()
		&& ObjectFactories.IsEmpty()
		&& RequestedSelfClass == nullptr)
	{
		SetFailure(OutResult, TEXT("selection_empty"), PackageName, TEXT("Select at least one reflected function, readable property, delegate event, or class reference for the binding package."));
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

		FAvidScriptEditorLatentFunctionContract LatentContract;
		FString FunctionPolicyCategory;
		FString FunctionPolicySource;
		if (!FAvidScriptEditorReflectedFunctionPolicy::Evaluate(
			Function,
			FunctionPolicyCategory,
			FunctionPolicySource,
			&LatentContract))
		{
			SetFailure(OutResult, FunctionPolicyCategory, FunctionPolicySource, TEXT("Select a supported public script-callable runtime UFunction."));
			return false;
		}

		FResolvedBindingDescriptor Binding;
		Binding.Selection = Selection;
		Binding.OwnerClass = OwnerClass;
		Binding.Function = Function;
		if (!TryResolveAvidScriptBindingNetworkContract(
				*Function,
				Binding.Network))
		{
			SetFailure(
				OutResult,
				TEXT("network_contract_invalid"),
				Function->GetPathName(),
				TEXT("Regenerate reflection data after fixing the malformed UE RPC flags."));
			return false;
		}
		Binding.UeMember = Function->GetName();
		Binding.LatentInfoParameter = LatentContract.LatentInfoParameter;
		Binding.WorldContextParameter = LatentContract.WorldContextParameter;
		if (LatentContract.bLatent)
		{
			const TSharedPtr<IAvidScriptLatentCompletionProvider> Provider =
				FAvidScriptLatentCompletionProviderRegistry::FindByFunctionPath(
					Function->GetPathName());
			if (Provider.IsValid())
			{
				Binding.CompletionMode = TEXT("provider");
				Binding.CompletionProviderId = Provider->GetProviderId();
				Binding.CompletionPayloadTypeId =
					Provider->GetPayloadTypeId();
				Binding.CompletionStatusPolicy =
					TEXT("resume_outcome_on_cancel");
			}
		}
		FString ProjectionErrorSource;
		const bool bProjected = LatentContract.bLatent
			? FAvidScriptEditorReflectedTypePolicy::ProjectLatentFunction(
				Function,
				Function->HasAnyFunctionFlags(FUNC_Static),
				Binding.LatentInfoParameter,
				Binding.WorldContextParameter,
				Binding.Projection,
				ProjectionErrorSource)
			: FAvidScriptEditorReflectedTypePolicy::ProjectFunction(
				Function,
				Function->HasAnyFunctionFlags(FUNC_Static),
				Binding.Projection,
				ProjectionErrorSource);
		if (!bProjected)
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
		FString AsyncActionCategory;
		FString AsyncActionSource;
		if (!LatentContract.bLatent
			&& !ResolveBlueprintAsyncAction(
				*Function,
				Binding.Projection,
				Binding.AsyncAction,
				AsyncActionCategory,
				AsyncActionSource))
		{
			SetFailure(
				OutResult,
				AsyncActionCategory,
				AsyncActionSource,
				TEXT("Use a static Blueprint async-action factory whose BlueprintAssignable outcomes have a supported frozen payload shape."));
			return false;
		}
		Binding.ScriptName = GetDescriptorScriptFunctionName(Function)
			+ (LatentContract.bLatent || Binding.AsyncAction.IsEnabled()
				? FString(TEXT("Async"))
				: FString());
		const bool bGeneratedNative =
			!LatentContract.bLatent
			&& !Binding.AsyncAction.IsEnabled()
			&& !Binding.Network.IsNetworked()
			&& GeneratedNativeFunctionKeys.Contains(SelectionKey);
		Binding.DispatchMode = Binding.AsyncAction.IsEnabled()
			? FString(TEXT("blueprint_async_action"))
			: LatentContract.bLatent
			? FString(TEXT("latent_process_event"))
			: Binding.Network.IsNetworked()
			? FString(TEXT("cached_process_event"))
			: bGeneratedNative
			? FString(TEXT("generated_native_s1"))
			: NativeDirectFunctionKeys.Contains(SelectionKey)
				? FString(TEXT("qualified_native_direct"))
				: FString(TEXT("cached_process_event"));
		const FString SemanticCanonicalIdentity = MakeCanonicalIdentity(
			OwnerClass,
			Function,
			Binding.Projection,
			Binding.Network,
			TEXT("cached_process_event"),
			Binding.LatentInfoParameter,
			Binding.WorldContextParameter,
			Binding.CompletionMode,
			Binding.CompletionProviderId,
			Binding.CompletionPayloadTypeId,
			Binding.CompletionStatusPolicy,
			Binding.AsyncAction.IsEnabled()
				? &Binding.AsyncAction
				: nullptr);
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
			Binding.Projection.AbiSignature =
				MakeGeneratedFunctionAbiSignature(Binding.GeneratedShape);
			check(!Binding.Projection.AbiSignature.IsEmpty());
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
				Binding.Network,
				Binding.DispatchMode,
				Binding.LatentInfoParameter,
				Binding.WorldContextParameter,
				Binding.CompletionMode,
				Binding.CompletionProviderId,
				Binding.CompletionPayloadTypeId,
				Binding.CompletionStatusPolicy,
				Binding.AsyncAction.IsEnabled()
					? &Binding.AsyncAction
					: nullptr);
		}
		Binding.StableId = HashSha256(Binding.CanonicalIdentity);
		if (!bGeneratedNative)
		{
			Binding.ImportName = TEXT("avid_ue_") + Binding.StableId.Left(16);
		}
		if (Binding.AsyncAction.IsEnabled())
		{
			Binding.Projection.AbiSignature =
				MakeAsyncActionAbiSignature(Binding.Projection);
		}
		Binding.ReloadEffect = LatentContract.bLatent
			|| Binding.AsyncAction.IsEnabled()
			? EAvidScriptBindingReloadEffect::ContinuationProducer
			: FAvidScriptEditorBindingReloadEffectPolicy::Classify(*Function);
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
		if (!TryResolveAvidScriptBindingPropertyReplicationContract(
				*Property,
				Binding.PropertyReplication))
		{
			SetFailure(
				OutResult,
				TEXT("property_replication_contract_invalid"),
				Property->GetPathName(),
				TEXT("Register the property for replication and provide a valid RepNotify function before generating bindings."));
			return false;
		}
		if (Binding.PropertyReplication.IsReplicated()
			&& (!IsAvidScriptBindingNetworkOwnerClass(OwnerClass)
				|| (Binding.PropertyReplication.Mode
						== EAvidScriptBindingPropertyReplicationMode::RepNotify
					&& OwnerClass->FindFunctionByName(
						Binding.PropertyReplication.RepNotifyFunction) == nullptr)))
		{
			SetFailure(
				OutResult,
				TEXT("property_replication_owner_invalid"),
				Property->GetPathName(),
				TEXT("Replicated bindings require an Actor or ActorComponent owner and a valid active RepNotify function."));
			return false;
		}
		Binding.BindingKind = TEXT("property_get");
		Binding.UeMember = Property->GetName();
		const bool bGeneratedNative =
			!Binding.PropertyReplication.IsReplicated()
			&& GeneratedNativePropertyKeys.Contains(SelectionKey);
		if (bGeneratedNative)
		{
			FString EligibilityCategory;
			if (!ResolveGeneratedPropertyShape(
					OwnerClass,
					Property,
					EligibilityCategory))
			{
				SetFailure(
					OutResult,
					EligibilityCategory,
					SelectionKey,
					TEXT("Select a public native int32 property without Blueprint accessors."));
				return false;
			}
		}
		Binding.DispatchMode = bGeneratedNative
			? FString(TEXT("generated_native_s1"))
			: FString(TEXT("cached_property_get"));
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
		FString SemanticCanonicalIdentity = OwnerClass->GetPathName()
			+ TEXT("::property_get:") + Property->GetName()
			+ TEXT("(") + Binding.Projection.ReturnValue.Type.CanonicalType + TEXT(")");
		if (Binding.PropertyReplication.IsReplicated())
		{
			SemanticCanonicalIdentity += TEXT("|property_replication=")
				+ FString(LexToString(Binding.PropertyReplication.Mode))
				+ TEXT("|rep_notify=")
				+ Binding.PropertyReplication.RepNotifyFunction.ToString();
		}
		if (bGeneratedNative)
		{
			Binding.Projection.AbiSignature = TEXT("(ii)i");
			Binding.GeneratedShape = TEXT("property_i32_get");
			Binding.GeneratedReceiverMode = TEXT("self_bound");
			Binding.ImportName =
				TEXT("avid_s1_")
				+ HashSha256(SemanticCanonicalIdentity).Left(16);
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
			Binding.CanonicalIdentity = SemanticCanonicalIdentity;
		}
		Binding.StableId = HashSha256(Binding.CanonicalIdentity);
		if (!bGeneratedNative)
		{
			Binding.ImportName = TEXT("avid_ue_") + Binding.StableId.Left(16);
		}
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
			SetterBinding.PropertyReplication = Binding.PropertyReplication;
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
			FString SetterSemanticCanonicalIdentity =
				FAvidScriptBindingDescriptorIdentity::MakePropertySetCanonicalIdentity(
					OwnerClass->GetPathName(),
					Property->GetName(),
					SetterBinding.Projection.Parameters[0].Type.CanonicalType,
					SetterBinding.UeFunction);
			if (SetterBinding.PropertyReplication.IsReplicated())
			{
				SetterSemanticCanonicalIdentity += TEXT("|property_replication=")
					+ FString(LexToString(SetterBinding.PropertyReplication.Mode))
					+ TEXT("|rep_notify=")
					+ SetterBinding.PropertyReplication.RepNotifyFunction.ToString();
			}
			if (bGeneratedNative)
			{
				if (SetterBinding.DispatchMode != TEXT("cached_property_set")
					|| SetterBinding.WritePolicy != TEXT("direct")
					|| !SetterBinding.UeFunction.IsEmpty())
				{
					SetFailure(
						OutResult,
						TEXT("generated_native_property_write_policy_unsupported"),
						SelectionKey,
						TEXT("Keep generated property setters on the direct write policy."));
					return false;
				}
				SetterBinding.DispatchMode = TEXT("generated_native_s1");
				SetterBinding.GeneratedShape = TEXT("property_i32_set");
				SetterBinding.GeneratedReceiverMode = TEXT("self_bound");
				SetterBinding.ImportName =
					TEXT("avid_s1_")
					+ HashSha256(SetterSemanticCanonicalIdentity).Left(16);
				SetterBinding.CanonicalIdentity =
					FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
						SetterSemanticCanonicalIdentity,
						SetterBinding.DispatchMode,
						SetterBinding.GeneratedShape,
						SetterBinding.GeneratedReceiverMode,
						SetterBinding.ImportName);
			}
			else
			{
				SetterBinding.CanonicalIdentity =
					SetterSemanticCanonicalIdentity;
			}
			SetterBinding.StableId = HashSha256(SetterBinding.CanonicalIdentity);
			if (!bGeneratedNative)
			{
				SetterBinding.ImportName = TEXT("avid_ue_")
					+ SetterBinding.StableId.Left(16);
			}
			SetterBinding.ReloadEffect =
				SetterBinding.PropertyReplication.IsReplicated()
				? EAvidScriptBindingReloadEffect::Unsupported
				: EAvidScriptBindingReloadEffect::ReflectedProperty;
			Bindings.Add(MoveTemp(SetterBinding));
		}
		Bindings.Add(MoveTemp(Binding));
	}

	TArray<FAvidScriptReflectedDelegateEventSelection> SortedDelegateEventSelections =
		DelegateEventSelections;
	SortedDelegateEventSelections.Sort([](
		const FAvidScriptReflectedDelegateEventSelection& Left,
		const FAvidScriptReflectedDelegateEventSelection& Right)
	{
		return MakeDelegateEventSelectionKey(Left).Compare(
			MakeDelegateEventSelectionKey(Right),
			ESearchCase::CaseSensitive) < 0;
	});
	TArray<FResolvedDelegateEventDescriptor> DelegateEvents;
	for (const FAvidScriptReflectedDelegateEventSelection& Selection :
		SortedDelegateEventSelections)
	{
		const FString SelectionKey = MakeDelegateEventSelectionKey(Selection);
		if (SeenSelections.Contains(SelectionKey))
		{
			SetFailure(
				OutResult,
				TEXT("duplicate_selection"),
				SelectionKey,
				TEXT("Keep each reflected delegate event selection exactly once."));
			return false;
		}
		SeenSelections.Add(SelectionKey);

		UClass* OwnerClass = LoadObject<UClass>(nullptr, *Selection.OwnerClassPath);
		if (OwnerClass == nullptr)
		{
			SetFailure(
				OutResult,
				TEXT("class_missing"),
				Selection.OwnerClassPath,
				TEXT("Use a loaded reflected UClass path from the active UE5.8 build."));
			return false;
		}

		FResolvedDelegateEventDescriptor Event;
		Event.Selection = Selection;
		Event.OwnerClass = OwnerClass;
		Event.CallbackKind = Selection.CallbackKind;
		Event.HandlerMode = Selection.HandlerMode;
		FString PolicyCategory;
		FString PolicySource;
		const bool bDelegatePropertySelection =
			Selection.CallbackKind == TEXT("multicast")
			|| Selection.CallbackKind == TEXT("singlecast");
		if (bDelegatePropertySelection)
		{
			FProperty* const Property = FindFProperty<FProperty>(
				OwnerClass,
				Selection.EventName);
			Event.Property = Property;
			const FDelegateProperty* const Singlecast =
				CastField<FDelegateProperty>(Property);
			const FMulticastDelegateProperty* const Multicast =
				CastField<FMulticastDelegateProperty>(Property);
			const bool bKindMatches =
				(Selection.CallbackKind == TEXT("singlecast")
					&& Singlecast != nullptr)
				|| (Selection.CallbackKind == TEXT("multicast")
					&& Multicast != nullptr);
			if (!bKindMatches
				|| Property->GetOwnerStruct() != OwnerClass)
			{
				SetFailure(
					OutResult,
					TEXT("delegate_event_kind_mismatch"),
					SelectionKey,
					TEXT("Select a declared dynamic delegate property matching the requested singlecast or multicast kind."));
				return false;
			}
			Event.SignatureFunction = Singlecast != nullptr
				? Singlecast->SignatureFunction
				: Multicast->SignatureFunction;
			if (!FAvidScriptEditorReflectedDelegateEventPolicy::EvaluateAndProject(
					Property,
					Event.Projection,
					PolicyCategory,
					PolicySource))
			{
				SetFailure(
					OutResult,
					PolicyCategory,
					PolicySource,
					TEXT("Use a value-only supported signature of at most eight ABI cells."));
				return false;
			}
		}
		else
		{
			Event.SignatureFunction = OwnerClass->FindFunctionByName(
				Selection.EventName,
				EIncludeSuperFlag::ExcludeSuper);
			if (Event.SignatureFunction == nullptr
				|| (!Event.SignatureFunction->HasAnyFunctionFlags(FUNC_Native)
					&& Event.SignatureFunction->Script.IsEmpty())
				|| Event.SignatureFunction->HasAnyFunctionFlags(
					FUNC_Static | FUNC_Delegate | FUNC_MulticastDelegate)
				|| Event.SignatureFunction->HasMetaData(TEXT("Latent"))
				|| !IsAvidScriptBindingNetworkOwnerClass(OwnerClass)
				|| (Event.HandlerMode != TEXT("replace")
					&& Event.HandlerMode != TEXT("before")
					&& Event.HandlerMode != TEXT("after"))
				|| !TryResolveAvidScriptBindingNetworkContract(
					*Event.SignatureFunction,
					Event.Network))
			{
				SetFailure(
					OutResult,
					TEXT("function_handler_contract_invalid"),
					SelectionKey,
					TEXT("Select a native or Blueprint-bytecode Actor/ActorComponent RPC or RepNotify function with a valid chain mode."));
				return false;
			}
			for (TFieldIterator<FProperty> It(
					OwnerClass,
					EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* const Candidate = *It;
				if (Candidate->HasAnyPropertyFlags(CPF_RepNotify)
					&& Candidate->RepNotifyFunc
						== Event.SignatureFunction->GetFName())
				{
					if (Event.RepNotifyProperty != nullptr)
					{
						SetFailure(
							OutResult,
							TEXT("function_handler_rep_notify_ambiguous"),
							SelectionKey,
							TEXT("Use one RepNotify property per generated handler."));
						return false;
					}
					Event.RepNotifyProperty = Candidate;
				}
			}
			const bool bBlueprintEvent = !Event.Network.IsNetworked()
				&& Event.RepNotifyProperty == nullptr
				&& OwnerClass->ClassGeneratedBy != nullptr
				&& Event.SignatureFunction->GetOwnerClass() == OwnerClass
				&& !Event.SignatureFunction->HasAnyFunctionFlags(FUNC_Native)
				&& !Event.SignatureFunction->Script.IsEmpty();
			const bool bKindExclusive = Event.Network.IsNetworked()
				? Event.RepNotifyProperty == nullptr
				: Event.RepNotifyProperty != nullptr || bBlueprintEvent;
			FString ResolvedKind;
			if (bKindExclusive)
			{
				ResolvedKind = TEXT("blueprint_event");
				if (Event.Network.IsNetworked())
				{
					ResolvedKind = TEXT("network_rpc");
				}
				else if (Event.RepNotifyProperty != nullptr)
				{
					ResolvedKind = TEXT("rep_notify");
				}
			}
			if (ResolvedKind.IsEmpty()
				|| ResolvedKind != Selection.CallbackKind
				|| !FAvidScriptEditorReflectedDelegateEventPolicy::
					EvaluateSignatureAndProject(
						Event.SignatureFunction,
						Event.Projection,
						PolicyCategory,
						PolicySource))
			{
				SetFailure(
					OutResult,
					ResolvedKind.IsEmpty()
						? FString(TEXT("function_handler_kind_unsupported"))
						: ResolvedKind != Selection.CallbackKind
						? FString(TEXT("function_handler_kind_mismatch"))
						: PolicyCategory,
					PolicySource.IsEmpty() ? SelectionKey : PolicySource,
					TEXT("Use a value-only native or Blueprint-bytecode RPC or RepNotify signature of at most eight ABI cells."));
				return false;
			}
			if (ResolvedKind == TEXT("blueprint_event")
				&& (Event.Projection.ReturnValue.Type.Kind != TEXT("void")
					|| Event.Projection.Parameters.ContainsByPredicate(
						[](const FAvidScriptProjectedBindingValue& Parameter)
						{
							return Parameter.Direction == TEXT("ref")
								|| Parameter.Direction == TEXT("out");
						})))
			{
				SetFailure(
					OutResult,
					TEXT("blueprint_event_signature_unsupported"),
					Event.SignatureFunction->GetPathName(),
					TEXT("Use a void Blueprint event with value or const-ref inputs only."));
				return false;
			}
		}
		TArray<FAvidScriptBindingValueModel> ParameterModels;
		ParameterModels.Reserve(Event.Projection.Parameters.Num());
		FinalizeType(Event.Projection.ReturnValue.Type);
		const FAvidScriptBindingValueModel ReturnValueModel =
			MakeBindingValueModel(Event.Projection.ReturnValue);
		const bool bUsesDelegateValueSystem =
			Selection.CallbackKind == TEXT("singlecast")
			|| Event.Projection.ReturnValue.Type.Kind != TEXT("void");
		for (FAvidScriptProjectedBindingValue& Parameter :
			Event.Projection.Parameters)
		{
			FinalizeType(Parameter.Type);
			ParameterModels.Add(MakeBindingValueModel(Parameter));
		}
		Event.ScriptName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(
			bDelegatePropertySelection
				? Event.Property->GetAuthoredName()
				: GetDescriptorScriptFunctionName(
					Event.SignatureFunction));
		if (Event.ScriptName.StartsWith(TEXT("@")))
		{
			Event.ScriptName = TEXT("Event_") + Event.ScriptName.Mid(1);
		}
		if (Event.ScriptName.IsEmpty())
		{
			SetFailure(
				OutResult,
				TEXT("callback_script_name_invalid"),
				SelectionKey,
				TEXT("Use a callback member name that maps to a C# identifier."));
			return false;
		}
		Event.CanonicalIdentity =
			FAvidScriptBindingDescriptorIdentity::MakeDelegateEventCanonicalIdentity(
				OwnerClass->GetPathName(),
				Selection.EventName.ToString(),
				Event.CallbackKind,
				TEXT("self"),
				ParameterModels,
				Event.Network,
				Event.RepNotifyProperty == nullptr
					? NAME_None
					: Event.RepNotifyProperty->GetFName(),
				bDelegatePropertySelection ? FString() : Event.HandlerMode,
				bUsesDelegateValueSystem ? &ReturnValueModel : nullptr);
		Event.StableId = FAvidScriptBindingDescriptorIdentity::MakeDelegateEventStableId(
			OwnerClass->GetPathName(),
			Selection.EventName.ToString(),
			Event.CallbackKind,
			TEXT("self"),
			ParameterModels,
			Event.Network,
			Event.RepNotifyProperty == nullptr
				? NAME_None
				: Event.RepNotifyProperty->GetFName(),
			bDelegatePropertySelection ? FString() : Event.HandlerMode,
			bUsesDelegateValueSystem ? &ReturnValueModel : nullptr);
		Event.ExportName = TEXT("avid_on_delegate_") + Event.StableId.Left(16);
		DelegateEvents.Add(MoveTemp(Event));
	}

	DelegateEvents.Sort([](
		const FResolvedDelegateEventDescriptor& Left,
		const FResolvedDelegateEventDescriptor& Right)
	{
		return Left.CanonicalIdentity.Compare(
			Right.CanonicalIdentity,
			ESearchCase::CaseSensitive) < 0;
	});
	TMap<FString, int32> EventScriptNameCounts;
	for (const FResolvedDelegateEventDescriptor& Event : DelegateEvents)
	{
		++EventScriptNameCounts.FindOrAdd(Event.ScriptName);
	}
	TSet<FString> PublishedEventScriptNames;
	for (int32 Index = 0; Index < DelegateEvents.Num(); ++Index)
	{
		FResolvedDelegateEventDescriptor& Event = DelegateEvents[Index];
		if (EventScriptNameCounts.FindRef(Event.ScriptName) > 1)
		{
			Event.ScriptName = FAvidScriptEditorCSharpSyntax::MakeIdentifier(
				FString(Event.OwnerClass->GetPrefixCPP())
					+ Event.OwnerClass->GetName())
				+ TEXT("_") + Event.ScriptName;
		}
		if (PublishedEventScriptNames.Contains(Event.ScriptName))
		{
			Event.ScriptName += TEXT("_") + Event.StableId.Left(8);
		}
		if (PublishedEventScriptNames.Contains(Event.ScriptName))
		{
			SetFailure(
				OutResult,
				TEXT("delegate_event_script_name_collision"),
				Event.ScriptName,
				TEXT("Rename the delegate property to produce a unique C# event constant."));
			return false;
		}
		PublishedEventScriptNames.Add(Event.ScriptName);
		Event.Ordinal = Index;
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
		AddProjectedTypeAndChildren(OwnerType, TypesByCanonicalName);
		if (!Binding.Projection.ReturnValue.Type.bVoid)
		{
			AddProjectedTypeAndChildren(Binding.Projection.ReturnValue.Type, TypesByCanonicalName);
		}
		for (const FAvidScriptProjectedBindingValue& Parameter : Binding.Projection.Parameters)
		{
			AddProjectedTypeAndChildren(Parameter.Type, TypesByCanonicalName);
		}
		if (Binding.AsyncAction.IsEnabled())
		{
			AddProjectedTypeAndChildren(
				Binding.AsyncAction.PayloadType,
				TypesByCanonicalName);
		}
	}
	for (const FResolvedDelegateEventDescriptor& Event : DelegateEvents)
	{
		if (Event.StableId.IsEmpty() || SeenStableIds.Contains(Event.StableId))
		{
			SetFailure(
				OutResult,
				TEXT("duplicate_stable_id"),
				Event.CanonicalIdentity,
				TEXT("Resolve the delegate event canonical identity collision."));
			return false;
		}
		SeenStableIds.Add(Event.StableId);
		FAvidScriptProjectedBindingType OwnerType =
			FAvidScriptEditorReflectedTypePolicy::MakeObjectType(Event.OwnerClass);
		FinalizeType(OwnerType);
		AddProjectedTypeAndChildren(OwnerType, TypesByCanonicalName);
		for (const FAvidScriptProjectedBindingValue& Parameter :
			Event.Projection.Parameters)
		{
			AddProjectedTypeAndChildren(Parameter.Type, TypesByCanonicalName);
		}
	}

	TArray<FAvidScriptProjectedBindingType> Types;
	TypesByCanonicalName.GenerateValueArray(Types);
	Types.Sort([](const FAvidScriptProjectedBindingType& Left, const FAvidScriptProjectedBindingType& Right)
	{
		return Left.CanonicalType.Compare(Right.CanonicalType, ESearchCase::CaseSensitive) < 0;
	});
	for (const FResolvedBindingDescriptor& Binding : Bindings)
	{
		if (Binding.CompletionMode == TEXT("provider")
			&& !Types.ContainsByPredicate(
				[&Binding](const FAvidScriptProjectedBindingType& Type)
				{
					return Type.StableId
						== Binding.CompletionPayloadTypeId;
				}))
		{
			SetFailure(
				OutResult,
				TEXT("latent_completion_payload_type_missing"),
				Binding.Function->GetPathName(),
				TEXT("Register a payload type identity present in the selected descriptor type graph."));
			return false;
		}
	}
	for (const FResolvedDelegateEventDescriptor& Event : DelegateEvents)
	{
		if (Event.CallbackKind == TEXT("blueprint_event")
			&& Event.HandlerMode == TEXT("replace")
			&& Bindings.ContainsByPredicate(
				[&Event](const FResolvedBindingDescriptor& Binding)
				{
					return Binding.BindingKind == TEXT("function")
						&& Binding.OwnerClass == Event.OwnerClass
						&& Binding.Function == Event.SignatureFunction;
				}))
		{
			SetFailure(
				OutResult,
				TEXT("blueprint_event_replace_invocation_conflict"),
				Event.SignatureFunction->GetPathName(),
				TEXT("Do not expose the same Blueprint function for outbound invocation and replace-mode inbound handling."));
			return false;
		}
	}

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
	const bool bHasGeneratedNativeBindings = Bindings.ContainsByPredicate(
		[](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.DispatchMode == TEXT("generated_native_s1");
		});
	const bool bHasStructWireTypes = Types.ContainsByPredicate(
		[](const FAvidScriptProjectedBindingType& Type)
		{
			return Type.Kind == TEXT("struct_wire");
		});
	const bool bHasArrayTypes = Types.ContainsByPredicate(
		[](const FAvidScriptProjectedBindingType& Type)
		{
			return Type.Kind == TEXT("array");
		});
	const bool bHasCompositeValueTypes = Types.ContainsByPredicate(
		[](const FAvidScriptProjectedBindingType& Type)
		{
			return Type.CapabilityKind == TEXT("composite");
		});
	const bool bHasGenericValueTypes = Types.ContainsByPredicate(
		[](const FAvidScriptProjectedBindingType& Type)
		{
			return !Type.TypeArguments.IsEmpty();
		});
	const bool bHasLatentBindings = Bindings.ContainsByPredicate(
		[](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.DispatchMode == TEXT("latent_process_event");
		});
	const bool bHasLatentPayloadBindings = Bindings.ContainsByPredicate(
		[](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.CompletionMode == TEXT("provider");
		});
	const bool bHasNetworkBindings = Bindings.ContainsByPredicate(
		[](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.Network.IsNetworked();
		});
	const bool bHasReplicatedProperties = Bindings.ContainsByPredicate(
		[](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.PropertyReplication.IsReplicated();
		});
	const bool bHasInboundHandlers = DelegateEvents.ContainsByPredicate(
		[](const FResolvedDelegateEventDescriptor& Event)
		{
			return Event.CallbackKind == TEXT("network_rpc")
				|| Event.CallbackKind == TEXT("rep_notify")
				|| Event.CallbackKind == TEXT("blueprint_event");
		});
	const bool bHasDelegateOutputs = DelegateEvents.ContainsByPredicate(
		[](const FResolvedDelegateEventDescriptor& Event)
		{
			return Event.Projection.ReturnValue.Type.Kind != TEXT("void")
				|| Event.Projection.Parameters.ContainsByPredicate(
				[](const FAvidScriptProjectedBindingValue& Parameter)
				{
					return Parameter.Direction == TEXT("ref")
						|| Parameter.Direction == TEXT("out");
				});
		});
	const bool bHasDelegateValueSystem =
		DelegateEvents.ContainsByPredicate(
			[](const FResolvedDelegateEventDescriptor& Event)
			{
				return Event.CallbackKind == TEXT("singlecast")
					|| Event.Projection.ReturnValue.Type.Kind != TEXT("void");
			});
	const bool bHasBlueprintDeclaredFunctions = Bindings.ContainsByPredicate(
		[](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.BindingKind == TEXT("function")
				&& Binding.OwnerClass != nullptr
				&& Binding.Function != nullptr
				&& Binding.Function->GetOwnerClass() == Binding.OwnerClass
				&& Binding.OwnerClass->ClassGeneratedBy != nullptr
				&& !Binding.Function->Script.IsEmpty();
		});
	const bool bHasBlueprintDeclaredEvents = DelegateEvents.ContainsByPredicate(
		[](const FResolvedDelegateEventDescriptor& Event)
		{
			return Event.CallbackKind == TEXT("blueprint_event");
		});
	const bool bHasBlueprintAsyncActions = Bindings.ContainsByPredicate(
		[](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.AsyncAction.IsEnabled();
		});
	Package.SchemaVersion = bHasWritableProperties || bHasNativeDirectFunctions
			|| bHasGeneratedNativeBindings
		? 8
		: ObjectFactories.IsEmpty()
			? 6
			: 7;
	if (bHasStructWireTypes)
	{
		Package.SchemaVersion = 9;
	}
	if (bHasArrayTypes)
	{
		Package.SchemaVersion = 10;
	}
	if (!DelegateEvents.IsEmpty())
	{
		Package.SchemaVersion = 11;
	}
	if (bHasLatentBindings)
	{
		Package.SchemaVersion = 12;
	}
	if (bHasLatentPayloadBindings)
	{
		Package.SchemaVersion = 14;
	}
	if (bHasNetworkBindings)
	{
		Package.SchemaVersion = 15;
	}
	if (bHasReplicatedProperties)
	{
		Package.SchemaVersion = 16;
	}
	if (bHasInboundHandlers)
	{
		Package.SchemaVersion = 18;
	}
	if (bHasGenericValueTypes || bHasCompositeValueTypes || bHasDelegateOutputs)
	{
		Package.SchemaVersion = 19;
	}
	if (bHasDelegateValueSystem)
	{
		Package.SchemaVersion = 20;
	}
	if (bHasBlueprintDeclaredFunctions)
	{
		Package.SchemaVersion = 21;
	}
	if (bHasBlueprintDeclaredEvents)
	{
		Package.SchemaVersion = 22;
	}
	if (bHasBlueprintAsyncActions)
	{
		Package.SchemaVersion = 23;
	}
	Package.GeneratorVersion = bHasBlueprintAsyncActions
		? BlueprintAsyncActionGeneratorVersion
		: bHasBlueprintDeclaredEvents
		? BlueprintEventGeneratorVersion
		: bHasBlueprintDeclaredFunctions
		? BlueprintFunctionGeneratorVersion
		: bHasDelegateValueSystem
		? DelegateValueGeneratorVersion
		: bHasDelegateOutputs
		? DelegateOutputGeneratorVersion
		: bHasGenericValueTypes || bHasCompositeValueTypes
		? CompositeValueGeneratorVersion
		: bHasInboundHandlers
		? InboundHandlerGeneratorVersion
		: bHasReplicatedProperties
		? ReplicatedPropertyGeneratorVersion
		: bHasNetworkBindings
		? NetworkGeneratorVersion
		: bHasLatentPayloadBindings
		? LatentPayloadGeneratorVersion
		: bHasLatentBindings
		? LatentGeneratorVersion
		: !DelegateEvents.IsEmpty()
		? DelegateEventGeneratorVersion
		: bHasArrayTypes
		? ArrayGeneratorVersion
		: bHasStructWireTypes
		? StructWireGeneratorVersion
		: bHasGeneratedNativeBindings
		? GeneratedNativeGeneratorVersion
		: bHasNativeDirectFunctions
		? NativeDirectGeneratorVersion
		: bHasWritableProperties
			? WritablePropertyGeneratorVersion
			: ObjectFactories.IsEmpty()
				? GeneratorVersion
				: ObjectFactoryGeneratorVersion;
	if (Package.SchemaVersion >= 18 && !DelegateEvents.IsEmpty())
	{
		for (FResolvedDelegateEventDescriptor& Event : DelegateEvents)
		{
			TArray<FAvidScriptBindingValueModel> ParameterModels;
			ParameterModels.Reserve(Event.Projection.Parameters.Num());
			for (const FAvidScriptProjectedBindingValue& Parameter :
				Event.Projection.Parameters)
			{
				ParameterModels.Add(MakeBindingValueModel(Parameter));
			}
			const FAvidScriptBindingValueModel ReturnValueModel =
				MakeBindingValueModel(Event.Projection.ReturnValue);
			Event.CanonicalIdentity =
				FAvidScriptBindingDescriptorIdentity::MakeDelegateEventCanonicalIdentity(
					Event.OwnerClass->GetPathName(),
					Event.Selection.EventName.ToString(),
					Event.CallbackKind,
					TEXT("self"),
					ParameterModels,
					Event.Network,
					Event.RepNotifyProperty == nullptr
						? NAME_None
						: Event.RepNotifyProperty->GetFName(),
					Event.HandlerMode,
					Package.SchemaVersion >= 20
						? &ReturnValueModel
						: nullptr);
			Event.StableId = FAvidScriptHash::Sha256HexUtf8(
				Event.CanonicalIdentity);
			Event.ExportName =
				TEXT("avid_on_delegate_") + Event.StableId.Left(16);
		}
		DelegateEvents.Sort([](
			const FResolvedDelegateEventDescriptor& Left,
			const FResolvedDelegateEventDescriptor& Right)
		{
			return Left.CanonicalIdentity.Compare(
				Right.CanonicalIdentity,
				ESearchCase::CaseSensitive) < 0;
		});
		for (int32 Index = 0; Index < DelegateEvents.Num(); ++Index)
		{
			DelegateEvents[Index].Ordinal = Index;
		}
	}
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
		TypeModel.StructFields = Type.StructFields;
		TypeModel.ElementTypeId = Type.ElementType.IsValid()
			? Type.ElementType->StableId
			: FString();
		for (const TSharedPtr<FAvidScriptProjectedBindingType>& Argument : Type.TypeArguments)
		{
			if (Argument.IsValid())
			{
				TypeModel.TypeArguments.Add(Argument->StableId);
			}
		}
		TypeModel.CapabilityKind = Type.CapabilityKind;
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
		if (Binding.BindingKind == TEXT("function")
			&& Binding.OwnerClass != nullptr
			&& Binding.Function != nullptr
			&& Binding.Function->GetOwnerClass() == Binding.OwnerClass
			&& Binding.OwnerClass->ClassGeneratedBy != nullptr
			&& !Binding.Function->Script.IsEmpty())
		{
			BindingModel.ReflectedOwnerKind = TEXT("blueprint");
			BindingModel.ReflectedOwnerAsset =
				Binding.OwnerClass->ClassGeneratedBy->GetPathName();
			BindingModel.ReflectedFunctionFingerprint =
				FAvidScriptBindingDescriptorIdentity::
				MakeReflectedFunctionFingerprint(
					Binding.CanonicalIdentity,
					*Binding.Function);
		}
		BindingModel.LatentInfoParameter = Binding.LatentInfoParameter;
		BindingModel.WorldContextParameter = Binding.WorldContextParameter;
		if (Package.SchemaVersion >= 13
			&& Binding.DispatchMode == TEXT("latent_process_event"))
		{
			BindingModel.Completion.Mode = Binding.CompletionMode;
			BindingModel.Completion.ProviderId =
				Binding.CompletionProviderId;
			BindingModel.Completion.PayloadTypeId =
				Binding.CompletionPayloadTypeId;
			BindingModel.Completion.StatusPolicy =
				Binding.CompletionStatusPolicy;
			BindingModel.Completion.bCancellable = true;
		}
		if (Package.SchemaVersion >= 23
			&& Binding.AsyncAction.IsEnabled())
		{
			BindingModel.AsyncAction.Mode = TEXT("blueprint_async_action");
			BindingModel.AsyncAction.ActionClass =
				Binding.AsyncAction.ActionClass->GetPathName();
			BindingModel.AsyncAction.ActivationFunction = TEXT("Activate");
			BindingModel.AsyncAction.PayloadTypeId =
				Binding.AsyncAction.PayloadType.StableId;
			BindingModel.AsyncAction.CompletionPolicy =
				TEXT("first_broadcast_wins");
			BindingModel.AsyncAction.bCancellable = true;
			for (const FResolvedAsyncActionOutcomeDescriptor& Outcome :
				Binding.AsyncAction.Outcomes)
			{
				FAvidScriptBindingAsyncActionOutcomeModel OutcomeModel;
				OutcomeModel.StableId = Outcome.StableId;
				OutcomeModel.Ordinal = Outcome.Ordinal;
				OutcomeModel.DelegateMember = Outcome.DelegateMember;
				BindingModel.AsyncAction.Outcomes.Add(MoveTemp(OutcomeModel));
			}
		}
		BindingModel.GeneratedShape = Binding.GeneratedShape;
		BindingModel.GeneratedReceiverMode = Binding.GeneratedReceiverMode;
		BindingModel.GeneratedImportName = Binding.ImportName;
		BindingModel.SemanticFallbackOrdinal = Binding.Ordinal;
		BindingModel.WritePolicy = Binding.WritePolicy;
		BindingModel.bStatic = Binding.Function != nullptr && Binding.Function->HasAnyFunctionFlags(FUNC_Static);
		BindingModel.bConst = Binding.BindingKind == TEXT("property_get")
			|| (Binding.Function != nullptr
				&& Binding.Function->HasAnyFunctionFlags(FUNC_Const));
		BindingModel.Network = Binding.Network;
		BindingModel.PropertyReplication = Binding.PropertyReplication;
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
	for (const FResolvedDelegateEventDescriptor& Event : DelegateEvents)
	{
		FAvidScriptBindingDelegateEventModel EventModel;
		EventModel.StableId = Event.StableId;
		EventModel.CanonicalIdentity = Event.CanonicalIdentity;
		EventModel.Ordinal = Event.Ordinal;
		EventModel.OwnerClass = Event.OwnerClass->GetPathName();
		EventModel.UeMember = Event.Selection.EventName.ToString();
		EventModel.ScriptName = Event.ScriptName;
		EventModel.DelegateKind = Event.CallbackKind;
		EventModel.HandlerMode = Event.HandlerMode;
		EventModel.SourceMode = TEXT("self");
		if (Event.SignatureFunction != nullptr
			&& Event.OwnerClass != nullptr
			&& Event.SignatureFunction->GetOwnerClass() == Event.OwnerClass
			&& Event.OwnerClass->ClassGeneratedBy != nullptr
			&& !Event.SignatureFunction->HasAnyFunctionFlags(FUNC_Native)
			&& !Event.SignatureFunction->Script.IsEmpty())
		{
			EventModel.ReflectedOwnerKind = TEXT("blueprint");
			EventModel.ReflectedOwnerAsset =
				Event.OwnerClass->ClassGeneratedBy->GetPathName();
			EventModel.ReflectedFunctionFingerprint =
				FAvidScriptBindingDescriptorIdentity::
				MakeReflectedFunctionFingerprint(
					Event.CanonicalIdentity,
					*Event.SignatureFunction);
		}
		EventModel.Network = Event.Network;
		EventModel.RepNotifyProperty = Event.RepNotifyProperty == nullptr
			? NAME_None
			: Event.RepNotifyProperty->GetFName();
		EventModel.ExportName = Event.ExportName;
		EventModel.ReturnValue =
			MakeBindingValueModel(Event.Projection.ReturnValue);
		for (const FAvidScriptProjectedBindingValue& Parameter :
			Event.Projection.Parameters)
		{
			EventModel.Parameters.Add(MakeBindingValueModel(Parameter));
		}
		Package.DelegateEvents.Add(MoveTemp(EventModel));
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
		&& DelegateEvents.IsEmpty()
		&& Bindings.ContainsByPredicate([](const FResolvedBindingDescriptor& Binding)
		{
			return Binding.Function == nullptr
				|| !Binding.Function->HasAnyFunctionFlags(FUNC_Static);
		}) == false;
	UClass* SelfClass = RequestedSelfClass;
	const bool bHasRequestedSelfClass = SelfClass != nullptr;
	if (!DelegateEvents.IsEmpty())
	{
		for (const FResolvedDelegateEventDescriptor& Event : DelegateEvents)
		{
			const bool bDelegateProperty =
				Event.CallbackKind == TEXT("multicast")
				|| Event.CallbackKind == TEXT("singlecast");
			const bool bOwnerSupported = bDelegateProperty
				? Event.OwnerClass->IsChildOf(AActor::StaticClass())
				: IsAvidScriptBindingNetworkOwnerClass(Event.OwnerClass);
			if (!bOwnerSupported)
			{
				SetFailure(
					OutResult,
					TEXT("callback_owner_unsupported"),
					Event.OwnerClass->GetPathName(),
					TEXT("Use an Actor delegate event or an Actor/ActorComponent inbound network handler."));
				return false;
			}
			if (SelfClass == nullptr)
			{
				SelfClass = Event.OwnerClass;
			}
			else if (!SelfClass->IsChildOf(Event.OwnerClass))
			{
				if (!bHasRequestedSelfClass
					&& Event.OwnerClass->IsChildOf(SelfClass))
				{
					SelfClass = Event.OwnerClass;
				}
				else
				{
					SetFailure(
						OutResult,
						TEXT("delegate_event_self_type_mismatch"),
						SelfClass->GetPathName() + TEXT(" -> ")
							+ Event.OwnerClass->GetPathName(),
						TEXT("Use one Self type derived from every selected callback owner."));
					return false;
				}
			}
		}
	}
	if (SelfClass == nullptr && bUseDefaultActorSelf && !bPureStaticPackage)
	{
		SelfClass = AActor::StaticClass();
	}

	TArray<UClass*> HandleClasses;
	for (const FResolvedBindingDescriptor& Binding : Bindings)
	{
		if (Binding.Function == nullptr || !Binding.Function->HasAnyFunctionFlags(FUNC_Static))
		{
			HandleClasses.AddUnique(Binding.OwnerClass);
		}
		if (!Binding.Projection.ReturnValue.Type.bVoid)
		{
			AddProjectedObjectHandleClasses(
				Binding.Projection.ReturnValue.Type,
				HandleClasses);
		}
		for (const FAvidScriptProjectedBindingValue& Parameter : Binding.Projection.Parameters)
		{
			AddProjectedObjectHandleClasses(Parameter.Type, HandleClasses);
		}
	}
	for (const FResolvedDelegateEventDescriptor& Event : DelegateEvents)
	{
		HandleClasses.AddUnique(Event.OwnerClass);
		for (const FAvidScriptProjectedBindingValue& Parameter :
			Event.Projection.Parameters)
		{
			AddProjectedObjectHandleClasses(Parameter.Type, HandleClasses);
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
			AddedType.CapabilityKind = ProjectedType.CapabilityKind;
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
	FString TypeGraphError;
	if (!FAvidScriptBindingDescriptorLayout::ValidateTypeGraph(
			Package.Types,
			TypeGraphError))
	{
		SetFailure(
			OutResult,
			TEXT("serialize_failed"),
			TypeGraphError.IsEmpty() ? PackageName : TypeGraphError,
			TEXT("Inspect the generated descriptor type graph."));
		return false;
	}
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
	OutResult.DelegateEventCount = DelegateEvents.Num();
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
		{},
		PropertySelections,
		{},
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
		const bool bHasClassPropertySurface =
			Profile.Classes.ContainsByPredicate(
				[](const FAvidScriptReflectedClassSelection& Rule)
				{
					return Rule.bDiscoverReadableProperties
						|| !Rule.IncludeProperties.IsEmpty()
						|| !Rule.WritableProperties.IsEmpty()
						|| !Rule.IncludeEvents.IsEmpty();
				});
		const bool bHasNonFunctionSurface =
			!Profile.ExplicitProperties.IsEmpty()
			|| !Profile.ExplicitDelegateEvents.IsEmpty()
			|| bHasClassPropertySurface
			|| !Profile.SelfClassPath.IsEmpty()
			|| !ClassReferences.IsEmpty()
			|| !ObjectFactories.IsEmpty();
		const bool bHasExpectedEmptyFunctionSelection =
			OutSelectionResult.ErrorCategory == TEXT("profile_empty")
			|| OutSelectionResult.ErrorCategory == TEXT("selection_empty");
		if (!bHasExpectedEmptyFunctionSelection || !bHasNonFunctionSurface)
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
	TArray<FAvidScriptReflectedDelegateEventSelection> DelegateEventSelections;
	const bool bRequestsDelegateEvents = !Profile.ExplicitDelegateEvents.IsEmpty()
		|| Profile.Classes.ContainsByPredicate(
			[](const FAvidScriptReflectedClassSelection& Rule)
			{
				return !Rule.IncludeEvents.IsEmpty()
					|| !Rule.IncludeHandlers.IsEmpty()
					|| !Rule.BeforeHandlers.IsEmpty()
					|| !Rule.AfterHandlers.IsEmpty();
			});
	if (bRequestsDelegateEvents)
	{
		FAvidScriptBindingSelectionResolveResult EventResult;
		if (!FAvidScriptEditorBindingDelegateEventSelectionResolver::Resolve(
				Profile,
				DelegateEventSelections,
				EventResult))
		{
			OutSelectionResult.bSucceeded = false;
			OutSelectionResult.CandidateDelegateEventCount =
				EventResult.CandidateDelegateEventCount;
			OutSelectionResult.AcceptedDelegateEventCount =
				EventResult.AcceptedDelegateEventCount;
			OutSelectionResult.RejectedDelegateEventCount =
				EventResult.RejectedDelegateEventCount;
			OutSelectionResult.Issues.Append(EventResult.Issues);
			OutSelectionResult.ErrorCategory = EventResult.ErrorCategory;
			OutSelectionResult.ErrorSource = EventResult.ErrorSource;
			OutSelectionResult.NextAction = EventResult.NextAction;
			OutSelectionResult.ErrorMessage = EventResult.ErrorMessage;
			SetFailure(
				OutResult,
				EventResult.ErrorCategory,
				EventResult.ErrorSource,
				EventResult.NextAction);
			return false;
		}
		OutSelectionResult.CandidateDelegateEventCount =
			EventResult.CandidateDelegateEventCount;
		OutSelectionResult.AcceptedDelegateEventCount =
			EventResult.AcceptedDelegateEventCount;
		OutSelectionResult.RejectedDelegateEventCount =
			EventResult.RejectedDelegateEventCount;
		OutSelectionResult.Issues.Append(EventResult.Issues);
	}
	OutSelectionResult.bSucceeded = true;
	TSet<FString> NativeDirectFunctionKeys;
	TSet<FString> GeneratedNativeFunctionKeys;
	TSet<FString> GeneratedNativePropertyKeys;
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
		for (const FName PropertyName : Rule.GeneratedNativeProperties)
		{
			GeneratedNativePropertyKeys.Add(
				MakeDescriptorPropertySelectionKey(
					{ Rule.OwnerClassPath, PropertyName, false }));
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
		GeneratedNativePropertyKeys,
		PropertySelections,
		DelegateEventSelections,
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
