#include "AvidScriptBindingInvocation.h"

#include "AvidScriptBindingFastPath.h"
#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptHash.h"
#include "Invocation/AvidScriptBindingCodecProgram.h"
#include "Invocation/AvidScriptBindingObjectCompatibility.h"
#include "Invocation/AvidScriptBindingPreparedInvocation.h"
#include "AvidScriptObjectFactoryBinding.h"
#include "AvidScriptObjectTypeBinding.h"
#include "AvidScriptSceneAttachmentBinding.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Containers/StringConv.h"
#include "Engine/LatentActionManager.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/EngineVersion.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
using EAvidScriptRuntimeBindingDirection =
	UE::AvidScript::BindingPrivate::EValueCodecDirection;
using EAvidScriptRuntimeBindingKind =
	UE::AvidScript::BindingPrivate::EValueCodecKind;
using FAvidScriptRuntimeBindingValuePlan =
	UE::AvidScript::BindingPrivate::FValueCodecProgram;
using FAvidScriptRuntimeBindingInvocationPlan =
	UE::AvidScript::BindingPrivate::FInvocationCodecProgram;

void SetAvidScriptBindingLoadFailure(
	FAvidScriptBindingPackageLoadResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& Details);

bool TryParseAvidScriptGeneratedShape(
	const FString& Shape,
	EAvidScriptGeneratedBindingShape& OutGeneratedShape,
	EAvidScriptVmTypedHostShape& OutVmShape,
	FString& OutSignature)
{
	if (Shape == TEXT("i32_pair_to_i32"))
	{
		OutGeneratedShape = EAvidScriptGeneratedBindingShape::I32PairToI32;
		OutVmShape = EAvidScriptVmTypedHostShape::SelfI32PairToI32;
		OutSignature = TEXT("(iiii)i");
		return true;
	}
	if (Shape == TEXT("property_i32_get_set"))
	{
		OutGeneratedShape = EAvidScriptGeneratedBindingShape::PropertyI32GetSet;
		OutVmShape = EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet;
		OutSignature = TEXT("(iii)i");
		return true;
	}
	if (Shape == TEXT("property_i32_get"))
	{
		OutGeneratedShape = EAvidScriptGeneratedBindingShape::PropertyI32Get;
		OutVmShape = EAvidScriptVmTypedHostShape::SelfPropertyI32Get;
		OutSignature = TEXT("(ii)i");
		return true;
	}
	if (Shape == TEXT("property_i32_set"))
	{
		OutGeneratedShape = EAvidScriptGeneratedBindingShape::PropertyI32Set;
		OutVmShape = EAvidScriptVmTypedHostShape::SelfPropertyI32Set;
		OutSignature = TEXT("(iii)i");
		return true;
	}
	if (Shape == TEXT("vector_value"))
	{
		OutGeneratedShape = EAvidScriptGeneratedBindingShape::VectorValue;
		OutVmShape = EAvidScriptVmTypedHostShape::SelfVectorValue;
		OutSignature = TEXT("(iii)i");
		return true;
	}
	if (Shape == TEXT("stable_object_roundtrip"))
	{
		OutGeneratedShape =
			EAvidScriptGeneratedBindingShape::StableObjectRoundtrip;
		OutVmShape = EAvidScriptVmTypedHostShape::StableObjectRoundtrip;
		OutSignature = TEXT("(iiiii)i");
		return true;
	}
	return false;
}

bool AttachAvidScriptGeneratedPlan(
	const FAvidScriptBindingPackageModel& Model,
	const FAvidScriptBindingFunctionModel& Binding,
	FAvidScriptRuntimeBindingInvocationPlan& Plan,
	FAvidScriptBindingPackageLoadResult& OutResult)
{
	if (Binding.DispatchMode != TEXT("generated_native_s1"))
	{
		return true;
	}

	EAvidScriptGeneratedBindingShape GeneratedShape;
	EAvidScriptVmTypedHostShape VmShape;
	FString TypedSignature;
	if (!TryParseAvidScriptGeneratedShape(
			Binding.GeneratedShape,
			GeneratedShape,
			VmShape,
			TypedSignature))
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("generated_binding_mismatch"),
			Binding.CanonicalIdentity,
			TEXT("The generated binding shape is not supported by the typed runtime."));
		return false;
	}
	const bool bLegacyGeneratedProperty =
		GeneratedShape == EAvidScriptGeneratedBindingShape::PropertyI32GetSet;
	const bool bGeneratedPropertyShapeMatches =
		(Binding.BindingKind == TEXT("property_get")
			&& (GeneratedShape == EAvidScriptGeneratedBindingShape::PropertyI32Get
				|| bLegacyGeneratedProperty))
		|| (Binding.BindingKind == TEXT("property_set")
			&& (GeneratedShape == EAvidScriptGeneratedBindingShape::PropertyI32Set
				|| bLegacyGeneratedProperty));
	if ((Binding.BindingKind == TEXT("property_get")
			|| Binding.BindingKind == TEXT("property_set"))
		&& !bGeneratedPropertyShapeMatches)
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("generated_binding_mismatch"),
			Binding.CanonicalIdentity,
			TEXT("Generated property plans require the matching split property shape or the legacy property_i32_get_set shape."));
		return false;
	}
	if (Binding.HostImport.Signature != TypedSignature)
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("generated_binding_mismatch"),
			Binding.CanonicalIdentity,
			TEXT("The generated import signature does not match its typed shape."));
		return false;
	}
	FAvidScriptGeneratedBindingRegistry& Registry =
		FAvidScriptGeneratedBindingRegistry::Get();
	const FString& GeneratedSourcePackageHash =
		Model.GeneratedSourcePackageHash.IsEmpty()
		? Model.PackageHash
		: Model.GeneratedSourcePackageHash;
	if (!Registry.IsPackageActive(GeneratedSourcePackageHash))
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("generated_binding_unavailable"),
			Binding.CanonicalIdentity,
			TEXT("The generated binding package is not registered or was revoked before VM load."));
		return false;
	}

	FString RegistryError;
	if (!Registry.Acquire(
			GeneratedSourcePackageHash,
			Binding.StableId,
			Binding.CanonicalIdentity,
			GeneratedShape,
			Plan.GeneratedLease,
			RegistryError))
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("generated_binding_mismatch"),
			Binding.CanonicalIdentity,
			RegistryError);
		return false;
	}

	Plan.GeneratedEntry = Plan.GeneratedLease.GetEntry();
	const EAvidScriptGeneratedReceiverMode ExpectedReceiverMode =
		Binding.GeneratedReceiverMode == TEXT("stable_borrow")
		? EAvidScriptGeneratedReceiverMode::StableBorrow
		: EAvidScriptGeneratedReceiverMode::SelfBound;
	if (Plan.GeneratedEntry == nullptr
		|| Plan.GeneratedEntry->ReceiverMode != ExpectedReceiverMode)
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("generated_binding_mismatch"),
			Binding.CanonicalIdentity,
			TEXT("The generated registry receiver mode does not match the descriptor."));
		return false;
	}

	Plan.TypedHostImport.StableId = Binding.StableId;
	Plan.TypedHostImport.BindingOrdinal = static_cast<uint32>(Binding.Ordinal);
	Plan.TypedHostImport.ModuleName = Binding.HostImport.Module;
	Plan.TypedHostImport.ImportName = Binding.GeneratedImportName;
	Plan.TypedHostImport.Signature = Binding.HostImport.Signature;
	Plan.TypedHostImport.Shape = VmShape;
	return true;
}

void SetAvidScriptBindingLoadFailure(
	FAvidScriptBindingPackageLoadResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& Details)
{
	OutResult = FAvidScriptBindingPackageLoadResult();
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.ErrorDetails = Details;
}

void SetAvidScriptBindingDispatchFailure(
	FAvidScriptDynamicHostCallResult& OutResult,
	const FString& Category,
	const FString& Source,
	const FString& Details)
{
	OutResult = FAvidScriptDynamicHostCallResult();
	OutResult.Details = FString::Printf(
		TEXT("%s | source=%s | %s"),
		*Category,
		Source.IsEmpty() ? TEXT("<none>") : *Source,
		*Details);
}

bool ValidateAvidScriptPreparedReflectionNativeGuard(
	const void* InvocationCell,
	UObject& Receiver)
{
	const auto* Plan = static_cast<const
		FAvidScriptRuntimeBindingInvocationPlan*>(InvocationCell);
	return Plan != nullptr
		&& UE::AvidScript::BindingPrivate::ValidatePreparedNativeCallCell(
			Plan->FastPath,
			Receiver);
}

bool ValidateAvidScriptPreparedReflectionPropertyGuard(
	const void* InvocationCell,
	UObject& Receiver)
{
	const auto* Plan = static_cast<const
		FAvidScriptRuntimeBindingInvocationPlan*>(InvocationCell);
	return Plan != nullptr
		&& Plan->OwnerClass != nullptr
		&& CastField<FIntProperty>(Plan->ReflectedProperty) != nullptr
		&& Plan->ReflectedProperty->GetOwnerStruct() == Plan->OwnerClass
		&& UE::AvidScript::BindingPrivate::ValidatePreparedNativeTarget(
			Plan->OwnerClass,
			Receiver);
}

bool InvokeAvidScriptPreparedReflectionI32PairCall(
	const void* InvocationCell,
	UObject& Receiver,
	const int32 Left,
	const int32 Right,
	const bool bUseNative,
	int32& OutValue,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	const auto* Plan = static_cast<const
		FAvidScriptRuntimeBindingInvocationPlan*>(InvocationCell);
	if (Plan == nullptr)
	{
		OutErrorCategory = TEXT("binding_prepared_identity_mismatch");
		OutErrorDetails = TEXT("The prepared reflection call cell is unavailable.");
		return false;
	}
	return UE::AvidScript::BindingPrivate::
		InvokePreparedScalarI32PairCallCell(
			Plan->FastPath,
			Receiver,
			Left,
			Right,
			bUseNative,
			OutValue,
			OutErrorCategory,
			OutErrorDetails);
}

bool InvokeAvidScriptPreparedReflectionVectorCall(
	const void* InvocationCell,
	UObject& Receiver,
	const FVector& Input,
	const bool bUseNative,
	FVector& OutValue,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	const auto* Plan = static_cast<const
		FAvidScriptRuntimeBindingInvocationPlan*>(InvocationCell);
	if (Plan == nullptr)
	{
		OutErrorCategory = TEXT("binding_prepared_identity_mismatch");
		OutErrorDetails = TEXT("The prepared FVector call cell is unavailable.");
		return false;
	}
	return UE::AvidScript::BindingPrivate::InvokePreparedVectorCallCell(
		Plan->FastPath,
		Receiver,
		Input,
		bUseNative,
		OutValue,
		OutErrorCategory,
		OutErrorDetails);
}

bool InvokeAvidScriptPreparedReflectionObjectCall(
	const void* InvocationCell,
	UObject& Receiver,
	UObject* Input,
	const bool bUseNative,
	UObject*& OutValue,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	const auto* Plan = static_cast<const
		FAvidScriptRuntimeBindingInvocationPlan*>(InvocationCell);
	if (Plan == nullptr)
	{
		OutErrorCategory = TEXT("binding_prepared_identity_mismatch");
		OutErrorDetails = TEXT("The prepared UObject call cell is unavailable.");
		return false;
	}
	return UE::AvidScript::BindingPrivate::InvokePreparedObjectCallCell(
		Plan->FastPath,
		Receiver,
		Input,
		bUseNative,
		OutValue,
		OutErrorCategory,
		OutErrorDetails);
}

bool ReadAvidScriptPreparedReflectionPropertyI32(
	const void* InvocationCell,
	UObject& Receiver,
	int32& OutValue)
{
	OutValue = 0;
	const auto* Plan = static_cast<const
		FAvidScriptRuntimeBindingInvocationPlan*>(InvocationCell);
	const FIntProperty* Property = Plan == nullptr
		? nullptr
		: CastField<FIntProperty>(Plan->ReflectedProperty);
	if (Property == nullptr || !Receiver.IsA(Plan->OwnerClass))
	{
		return false;
	}
	OutValue = Property->GetPropertyValue_InContainer(&Receiver);
	return true;
}

bool WriteAvidScriptPreparedReflectionPropertyI32(
	const void* InvocationCell,
	UObject& Receiver,
	const int32 Value)
{
	const auto* Plan = static_cast<const
		FAvidScriptRuntimeBindingInvocationPlan*>(InvocationCell);
	FIntProperty* Property = Plan == nullptr
		? nullptr
		: CastField<FIntProperty>(Plan->ReflectedProperty);
	if (Property == nullptr || !Receiver.IsA(Plan->OwnerClass))
	{
		return false;
	}
	Property->SetPropertyValue_InContainer(&Receiver, Value);
	return true;
}

UE::AvidScript::BindingPrivate::EFastPathValueKind
GetAvidScriptFastPathValueKind(
	const EAvidScriptRuntimeBindingKind Kind)
{
	using EFastPathValueKind =
		UE::AvidScript::BindingPrivate::EFastPathValueKind;
	switch (Kind)
	{
	case EAvidScriptRuntimeBindingKind::Int32:
		return EFastPathValueKind::Int32;
	case EAvidScriptRuntimeBindingKind::Vector:
		return EFastPathValueKind::Vector;
	case EAvidScriptRuntimeBindingKind::Object:
		return EFastPathValueKind::Object;
	case EAvidScriptRuntimeBindingKind::StructWire:
		return EFastPathValueKind::Unsupported;
	default:
		return EFastPathValueKind::Unsupported;
	}
}

bool IsAvidScriptRuntimeFunctionAllowed(const UFunction* Function)
{
	return Function != nullptr
		&& Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)
		&& !Function->HasAnyFunctionFlags(
			FUNC_EditorOnly
			| FUNC_Delegate
			| FUNC_MulticastDelegate
			| FUNC_NetRequest
			| FUNC_NetResponse)
		&& !Function->HasMetaData(TEXT("Latent"))
		&& !Function->HasMetaData(TEXT("CustomThunk"));
}

struct FAvidScriptRuntimeLatentContract
{
	FStructProperty* LatentInfoProperty = nullptr;
	FObjectPropertyBase* WorldContextProperty = nullptr;
	FAvidScriptBindingLatentCompletionContract Completion;
};

bool ResolveAvidScriptRuntimeLatentContract(
	UFunction* Function,
	const FAvidScriptBindingFunctionModel& Binding,
	FAvidScriptRuntimeLatentContract& OutContract)
{
	OutContract = {};
	if (Function == nullptr
		|| !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure)
		|| Function->HasAnyFunctionFlags(
			FUNC_EditorOnly
			| FUNC_Delegate
			| FUNC_MulticastDelegate
			| FUNC_NetRequest
			| FUNC_NetResponse)
		|| !Function->HasMetaData(TEXT("Latent"))
		|| Function->HasMetaData(TEXT("CustomThunk"))
		|| Function->GetReturnProperty() != nullptr
		|| Binding.LatentInfoParameter.IsEmpty()
		|| Function->GetMetaData(TEXT("LatentInfo"))
			!= Binding.LatentInfoParameter
		|| Function->GetMetaData(TEXT("WorldContext"))
			!= Binding.WorldContextParameter)
	{
		return false;
	}

	OutContract.LatentInfoProperty = FindFProperty<FStructProperty>(
		Function,
		FName(*Binding.LatentInfoParameter));
	OutContract.WorldContextProperty = Binding.WorldContextParameter.IsEmpty()
		? nullptr
		: FindFProperty<FObjectPropertyBase>(
			Function,
			FName(*Binding.WorldContextParameter));
	if (OutContract.LatentInfoProperty == nullptr
		|| OutContract.LatentInfoProperty->Struct
			!= FLatentActionInfo::StaticStruct()
		|| !OutContract.LatentInfoProperty->HasAnyPropertyFlags(CPF_Parm)
		|| OutContract.LatentInfoProperty->HasAnyPropertyFlags(CPF_ReturnParm)
		|| (!Binding.WorldContextParameter.IsEmpty()
			&& (OutContract.WorldContextProperty == nullptr
				|| !OutContract.WorldContextProperty->HasAnyPropertyFlags(CPF_Parm)
				|| OutContract.WorldContextProperty->HasAnyPropertyFlags(CPF_ReturnParm)
				|| !UWorld::StaticClass()->IsChildOf(
					OutContract.WorldContextProperty->PropertyClass))))
	{
		return false;
	}
	if (Binding.Completion.Mode == TEXT("provider"))
	{
		const TSharedPtr<IAvidScriptLatentCompletionProvider> Provider =
			FAvidScriptLatentCompletionProviderRegistry::FindByProviderId(
				Binding.Completion.ProviderId);
		if (!Provider.IsValid()
			|| Provider->GetFunctionPath() != Function->GetPathName()
			|| Provider->GetPayloadTypeId()
				!= Binding.Completion.PayloadTypeId)
		{
			return false;
		}
		OutContract.Completion.Mode = Binding.Completion.Mode;
		OutContract.Completion.ProviderId = Binding.Completion.ProviderId;
		OutContract.Completion.PayloadTypeId =
			Binding.Completion.PayloadTypeId;
		OutContract.Completion.StatusPolicy =
			Binding.Completion.StatusPolicy;
		OutContract.Completion.bCancellable =
			Binding.Completion.bCancellable;
		OutContract.Completion.Provider = Provider;
	}

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Parm)
			|| Property->HasAnyPropertyFlags(CPF_ReturnParm)
			|| Property == OutContract.LatentInfoProperty
			|| Property == OutContract.WorldContextProperty)
		{
			continue;
		}
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		if (Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm)
			|| CastField<FDelegateProperty>(Property) != nullptr
			|| CastField<FMulticastDelegateProperty>(Property) != nullptr
			|| (StructProperty != nullptr
				&& StructProperty->Struct == FLatentActionInfo::StaticStruct()))
		{
			return false;
		}
	}
	return true;
}

bool IsAvidScriptRuntimePropertyReadable(const FProperty* Property)
{
	return Property != nullptr
		&& Property->HasAnyPropertyFlags(CPF_BlueprintVisible)
		&& !Property->HasAnyPropertyFlags(CPF_Parm | CPF_EditorOnly | CPF_Deprecated);
}

bool IsAvidScriptRuntimePropertyWritable(const FProperty* Property)
{
	const EPropertyFlags RejectedFlags =
		CPF_BlueprintReadOnly
		| CPF_EditConst
		| CPF_Config
		| CPF_GlobalConfig
		| CPF_EditorOnly
		| CPF_Deprecated
		| CPF_InstancedReference
		| CPF_ContainsInstancedReference;
	return IsAvidScriptRuntimePropertyReadable(Property)
		&& !Property->HasAnyPropertyFlags(RejectedFlags);
}

FString GetAvidScriptRuntimePropertyDirection(const FProperty* Property)
{
	if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
	{
		return TEXT("return");
	}
	const bool bReference = Property->HasAnyPropertyFlags(CPF_ReferenceParm);
	const bool bOut = Property->HasAnyPropertyFlags(CPF_OutParm);
	const bool bConst = Property->HasAnyPropertyFlags(CPF_ConstParm);
	if (bReference && bOut)
	{
		return bConst ? TEXT("const_ref") : TEXT("ref");
	}
	if (bOut)
	{
		return TEXT("out");
	}
	if (bReference)
	{
		return bConst ? TEXT("const_ref") : TEXT("ref");
	}
	return TEXT("value");
}

bool ParseAvidScriptRuntimeDirection(
	const FString& Direction,
	EAvidScriptRuntimeBindingDirection& OutDirection)
{
	if (Direction == TEXT("value")) { OutDirection = EAvidScriptRuntimeBindingDirection::Value; return true; }
	if (Direction == TEXT("const_ref")) { OutDirection = EAvidScriptRuntimeBindingDirection::ConstRef; return true; }
	if (Direction == TEXT("ref")) { OutDirection = EAvidScriptRuntimeBindingDirection::Ref; return true; }
	if (Direction == TEXT("out")) { OutDirection = EAvidScriptRuntimeBindingDirection::Out; return true; }
	if (Direction == TEXT("return")) { OutDirection = EAvidScriptRuntimeBindingDirection::Return; return true; }
	return false;
}

bool MatchesAvidScriptRuntimeScalarModel(
	const FAvidScriptBindingValueModel& Model,
	const TCHAR* CanonicalType,
	const TCHAR* CppType,
	const TCHAR* AbiType)
{
	return Model.Kind == TEXT("scalar")
		&& Model.CanonicalType == CanonicalType
		&& Model.CppType == CppType
		&& Model.AbiTypes.Num() == 1
		&& Model.AbiTypes[0] == AbiType;
}

bool ResolveAvidScriptRuntimeKind(
	const FProperty* Property,
	const FAvidScriptBindingValueModel& Model,
	const FAvidScriptBindingTypeModel* DeclaredType,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& DeclaredTypesById,
	EAvidScriptRuntimeBindingKind& OutKind,
	UClass*& OutObjectClass)
{
	OutObjectClass = nullptr;
	if (Model.CanonicalType == TEXT("void"))
	{
		OutKind = EAvidScriptRuntimeBindingKind::Void;
		return Property == nullptr
			&& Model.Kind == TEXT("void")
			&& Model.CppType == TEXT("void")
			&& Model.AbiTypes.IsEmpty();
	}
	if (Property == nullptr)
	{
		return false;
	}
	if (Model.CanonicalType == TEXT("name:fname"))
	{
		if (!Property->IsA<FNameProperty>()
			|| Model.Kind != TEXT("name_utf8")
			|| Model.CppType != TEXT("FName")
			|| Model.AbiTypes != TArray<FString>({ TEXT("i") })
			|| DeclaredType == nullptr
			|| DeclaredType->CanonicalType != TEXT("name:fname")
			|| DeclaredType->Kind != TEXT("name_utf8")
			|| DeclaredType->CppType != TEXT("FName")
			|| DeclaredType->Size != 4
			|| DeclaredType->Alignment != 4
			|| DeclaredType->AbiTypes != TArray<FString>({ TEXT("i") }))
		{
			return false;
		}
		OutKind = EAvidScriptRuntimeBindingKind::Name;
		return true;
	}
	if (Model.CanonicalType == TEXT("string:fstring"))
	{
		if (!Property->IsA<FStrProperty>()
			|| Model.Kind != TEXT("string_utf8")
			|| Model.CppType != TEXT("FString")
			|| Model.AbiTypes != TArray<FString>({ TEXT("i") })
			|| DeclaredType == nullptr
			|| DeclaredType->CanonicalType != TEXT("string:fstring")
			|| DeclaredType->Kind != TEXT("string_utf8")
			|| DeclaredType->CppType != TEXT("FString")
			|| DeclaredType->Size != 4
			|| DeclaredType->Alignment != 4
			|| DeclaredType->AbiTypes != TArray<FString>({ TEXT("i") }))
		{
			return false;
		}
		OutKind = EAvidScriptRuntimeBindingKind::String;
		return true;
	}
	if (Model.CanonicalType == TEXT("text:ftext"))
	{
		if (!Property->IsA<FTextProperty>()
			|| Model.Kind != TEXT("text_capability")
			|| Model.CppType != TEXT("FText")
			|| Model.AbiTypes != TArray<FString>({ TEXT("i") })
			|| DeclaredType == nullptr
			|| DeclaredType->CanonicalType != TEXT("text:ftext")
			|| DeclaredType->Kind != TEXT("text_capability")
			|| DeclaredType->CppType != TEXT("FText")
			|| DeclaredType->Size != 4
			|| DeclaredType->Alignment != 4
			|| DeclaredType->AbiTypes != TArray<FString>({ TEXT("i") })
			|| !DeclaredType->TypeArguments.IsEmpty()
			|| DeclaredType->CapabilityKind != TEXT("composite"))
		{
			return false;
		}
		OutKind = EAvidScriptRuntimeBindingKind::Text;
		return true;
	}
	const bool bSoftObject = Model.Kind == TEXT("soft_object_capability");
	const bool bWeakObject = Model.Kind == TEXT("weak_object_capability");
	if (bSoftObject || bWeakObject)
	{
		const FObjectPropertyBase* ObjectProperty = nullptr;
		if (bSoftObject)
		{
			ObjectProperty = CastField<FSoftObjectProperty>(Property);
		}
		else
		{
			ObjectProperty = CastField<FWeakObjectProperty>(Property);
		}
		const FString CanonicalPrefix = bSoftObject
			? TEXT("soft_object:")
			: TEXT("weak_object:");
		const FAvidScriptBindingTypeModel* ObjectType =
			DeclaredType != nullptr && DeclaredType->TypeArguments.Num() == 1
				? DeclaredTypesById.FindRef(DeclaredType->TypeArguments[0])
				: nullptr;
		if (ObjectProperty == nullptr
			|| ObjectProperty->PropertyClass == nullptr
			|| Model.CanonicalType != CanonicalPrefix + ObjectProperty->PropertyClass->GetPathName()
			|| Model.CppType != Property->GetCPPType()
			|| Model.AbiTypes != TArray<FString>({ TEXT("i") })
			|| DeclaredType == nullptr
			|| DeclaredType->CanonicalType != Model.CanonicalType
			|| DeclaredType->Kind != Model.Kind
			|| DeclaredType->CppType != Model.CppType
			|| DeclaredType->Size != 4
			|| DeclaredType->Alignment != 4
			|| DeclaredType->AbiTypes != TArray<FString>({ TEXT("i") })
			|| DeclaredType->CapabilityKind != TEXT("composite")
			|| ObjectType == nullptr
			|| ObjectType->Kind != TEXT("object_handle")
			|| ObjectType->ClassPath != ObjectProperty->PropertyClass->GetPathName()
			|| ObjectType->CanonicalType != TEXT("object:") + ObjectType->ClassPath)
		{
			return false;
		}
		OutKind = bSoftObject
			? EAvidScriptRuntimeBindingKind::SoftObject
			: EAvidScriptRuntimeBindingKind::WeakObject;
		OutObjectClass = ObjectProperty->PropertyClass;
		return true;
	}

	if (Model.Kind == TEXT("object_handle"))
	{
		const FInterfaceProperty* InterfaceProperty =
			CastField<FInterfaceProperty>(Property);
		const FObjectPropertyBase* ObjectProperty =
			CastField<FObjectPropertyBase>(Property);
		UClass* ReflectedClass = InterfaceProperty != nullptr
			? InterfaceProperty->InterfaceClass.Get()
			: (ObjectProperty != nullptr ? ObjectProperty->PropertyClass.Get() : nullptr);
		if (ReflectedClass == nullptr
			|| Model.CanonicalType != TEXT("object:") + ReflectedClass->GetPathName()
			|| Model.AbiTypes != TArray<FString>({ TEXT("i"), TEXT("i") }))
		{
			return false;
		}
		OutKind = InterfaceProperty != nullptr
			? EAvidScriptRuntimeBindingKind::Interface
			: EAvidScriptRuntimeBindingKind::Object;
		OutObjectClass = ReflectedClass;
		return true;
	}

	if (Model.Kind == TEXT("struct"))
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		if (StructProperty == nullptr || StructProperty->Struct == nullptr
			|| Model.CanonicalType != TEXT("struct:") + StructProperty->Struct->GetPathName())
		{
			return false;
		}
		if (StructProperty->Struct == TBaseStructure<FVector>::Get()
			&& Model.CppType == TEXT("FVector")
			&& Model.AbiTypes == TArray<FString>({ TEXT("f"), TEXT("f"), TEXT("f") }))
		{
			OutKind = EAvidScriptRuntimeBindingKind::Vector;
			return true;
		}
		if (StructProperty->Struct == TBaseStructure<FRotator>::Get()
			&& Model.CppType == TEXT("FRotator")
			&& Model.AbiTypes == TArray<FString>({ TEXT("f"), TEXT("f"), TEXT("f") }))
		{
			OutKind = EAvidScriptRuntimeBindingKind::Rotator;
			return true;
		}
		if (StructProperty->Struct == TBaseStructure<FTransform>::Get()
			&& Model.CppType == TEXT("FTransform")
			&& Model.AbiTypes == TArray<FString>({
				TEXT("f"), TEXT("f"), TEXT("f"),
				TEXT("f"), TEXT("f"), TEXT("f"),
				TEXT("f"), TEXT("f"), TEXT("f") }))
		{
			OutKind = EAvidScriptRuntimeBindingKind::Transform;
			return true;
		}
		return false;
	}

	if (Model.Kind == TEXT("enum"))
	{
		const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property);
		const FByteProperty* ByteProperty = CastField<FByteProperty>(Property);
		const UEnum* Enum = EnumProperty != nullptr
			? EnumProperty->GetEnum()
			: (ByteProperty != nullptr ? ByteProperty->Enum.Get() : nullptr);
		if (Enum == nullptr
			|| Model.CanonicalType != TEXT("enum:") + Enum->GetPathName()
			|| Model.AbiTypes != TArray<FString>({ TEXT("i") }))
		{
			return false;
		}
		OutKind = EAvidScriptRuntimeBindingKind::Enum;
		return true;
	}

	if (Model.Kind != TEXT("scalar") || Model.AbiTypes.Num() != 1)
	{
		return false;
	}
	if (Property->IsA<FBoolProperty>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:bool"), TEXT("bool"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::Bool; return true; }
	if (Property->IsA<FInt8Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:i8"), TEXT("int8"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::Int8; return true; }
	if (const FByteProperty* Byte = CastField<FByteProperty>(Property); Byte != nullptr && Byte->Enum == nullptr && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:u8"), TEXT("uint8"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::UInt8; return true; }
	if (Property->IsA<FInt16Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:i16"), TEXT("int16"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::Int16; return true; }
	if (Property->IsA<FUInt16Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:u16"), TEXT("uint16"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::UInt16; return true; }
	if (Property->IsA<FIntProperty>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:i32"), TEXT("int32"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::Int32; return true; }
	if (Property->IsA<FUInt32Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:u32"), TEXT("uint32"), TEXT("i"))) { OutKind = EAvidScriptRuntimeBindingKind::UInt32; return true; }
	if (Property->IsA<FInt64Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:i64"), TEXT("int64"), TEXT("I"))) { OutKind = EAvidScriptRuntimeBindingKind::Int64; return true; }
	if (Property->IsA<FUInt64Property>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:u64"), TEXT("uint64"), TEXT("I"))) { OutKind = EAvidScriptRuntimeBindingKind::UInt64; return true; }
	if (Property->IsA<FFloatProperty>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:f32"), TEXT("float"), TEXT("f"))) { OutKind = EAvidScriptRuntimeBindingKind::Float; return true; }
	if (Property->IsA<FDoubleProperty>() && MatchesAvidScriptRuntimeScalarModel(Model, TEXT("scalar:f64"), TEXT("double"), TEXT("F"))) { OutKind = EAvidScriptRuntimeBindingKind::Double; return true; }
	return false;
}

int32 GetAvidScriptRuntimeGuestStorageSize(EAvidScriptRuntimeBindingKind Kind)
{
	switch (Kind)
	{
	case EAvidScriptRuntimeBindingKind::Bool:
	case EAvidScriptRuntimeBindingKind::Int32:
	case EAvidScriptRuntimeBindingKind::UInt32:
	case EAvidScriptRuntimeBindingKind::Float:
	case EAvidScriptRuntimeBindingKind::Enum:
	case EAvidScriptRuntimeBindingKind::Name:
	case EAvidScriptRuntimeBindingKind::String:
	case EAvidScriptRuntimeBindingKind::Text:
	case EAvidScriptRuntimeBindingKind::SoftObject:
	case EAvidScriptRuntimeBindingKind::WeakObject:
		return 4;
	case EAvidScriptRuntimeBindingKind::Int8:
	case EAvidScriptRuntimeBindingKind::UInt8:
		return 1;
	case EAvidScriptRuntimeBindingKind::Int16:
	case EAvidScriptRuntimeBindingKind::UInt16:
		return 2;
	case EAvidScriptRuntimeBindingKind::Int64:
	case EAvidScriptRuntimeBindingKind::UInt64:
	case EAvidScriptRuntimeBindingKind::Double:
	case EAvidScriptRuntimeBindingKind::Object:
	case EAvidScriptRuntimeBindingKind::Interface:
		return 8;
	case EAvidScriptRuntimeBindingKind::Vector:
	case EAvidScriptRuntimeBindingKind::Rotator:
		return 12;
	case EAvidScriptRuntimeBindingKind::Transform:
		return 36;
	default:
		return 0;
	}
}

int32 GetAvidScriptRuntimeGuestStorageAlignment(EAvidScriptRuntimeBindingKind Kind)
{
	switch (Kind)
	{
	case EAvidScriptRuntimeBindingKind::Int8:
	case EAvidScriptRuntimeBindingKind::UInt8:
		return 1;
	case EAvidScriptRuntimeBindingKind::Int16:
	case EAvidScriptRuntimeBindingKind::UInt16:
		return 2;
	case EAvidScriptRuntimeBindingKind::Bool:
	case EAvidScriptRuntimeBindingKind::Int32:
	case EAvidScriptRuntimeBindingKind::UInt32:
	case EAvidScriptRuntimeBindingKind::Float:
	case EAvidScriptRuntimeBindingKind::Enum:
	case EAvidScriptRuntimeBindingKind::Name:
	case EAvidScriptRuntimeBindingKind::String:
	case EAvidScriptRuntimeBindingKind::Text:
	case EAvidScriptRuntimeBindingKind::SoftObject:
	case EAvidScriptRuntimeBindingKind::WeakObject:
	case EAvidScriptRuntimeBindingKind::Object:
	case EAvidScriptRuntimeBindingKind::Interface:
	case EAvidScriptRuntimeBindingKind::Vector:
	case EAvidScriptRuntimeBindingKind::Rotator:
	case EAvidScriptRuntimeBindingKind::Transform:
		return 4;
	case EAvidScriptRuntimeBindingKind::Int64:
	case EAvidScriptRuntimeBindingKind::UInt64:
	case EAvidScriptRuntimeBindingKind::Double:
		return 8;
	default:
		return 0;
	}
}

bool MatchesAvidScriptRuntimeCanonicalLeafStorage(
	const FAvidScriptBindingTypeModel& Type,
	const EAvidScriptRuntimeBindingKind Kind,
	const UClass* ObjectClass)
{
	const int32 ExpectedSize = GetAvidScriptRuntimeGuestStorageSize(Kind);
	const int32 ExpectedAlignment = GetAvidScriptRuntimeGuestStorageAlignment(Kind);
	if (ExpectedSize <= 0 || ExpectedAlignment <= 0
		|| Type.Size != ExpectedSize || Type.Alignment != ExpectedAlignment)
	{
		return false;
	}

	if (Kind == EAvidScriptRuntimeBindingKind::Object
		|| Kind == EAvidScriptRuntimeBindingKind::Interface)
	{
		return ObjectClass != nullptr
			&& Type.ObjectTypeOrdinal != INDEX_NONE
			&& Type.ClassPath == ObjectClass->GetPathName()
			&& Type.CanonicalType == TEXT("object:") + Type.ClassPath
			&& Type.AbiTypes == TArray<FString>({ TEXT("i"), TEXT("i") });
	}
	return true;
}

int32 GetAvidScriptRuntimeArgumentWidth(
	const FAvidScriptBindingValueModel& Model,
	EAvidScriptRuntimeBindingDirection Direction)
{
	if (Model.CanonicalType == TEXT("void"))
	{
		return 0;
	}
	return Direction == EAvidScriptRuntimeBindingDirection::Ref
		|| Direction == EAvidScriptRuntimeBindingDirection::Out
		|| Direction == EAvidScriptRuntimeBindingDirection::Return
		? 1
		: Model.AbiTypes.Num();
}

bool IsAvidScriptStructWireFieldSafe(const FProperty* Property)
{
	return Property != nullptr
		&& Property->ArrayDim == 1
		&& Property->HasAnyPropertyFlags(CPF_BlueprintVisible)
		&& !Property->HasAnyPropertyFlags(
			CPF_Transient | CPF_EditorOnly | CPF_InstancedReference
				| CPF_ContainsInstancedReference)
		&& !Property->IsA<FNameProperty>()
		&& !Property->IsA<FStrProperty>()
		&& !Property->IsA<FTextProperty>()
		&& !Property->IsA<FArrayProperty>()
		&& !Property->IsA<FSetProperty>()
		&& !Property->IsA<FMapProperty>()
		&& !Property->IsA<FDelegateProperty>()
		&& !Property->IsA<FMulticastDelegateProperty>()
		&& !Property->IsA<FSoftObjectProperty>()
		&& !Property->IsA<FWeakObjectProperty>()
		&& !Property->IsA<FLazyObjectProperty>();
}

bool BuildAvidScriptStructWireProgram(
	FProperty* Property,
	const FAvidScriptBindingTypeModel& Type,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& DeclaredTypesById,
	const int32 Depth,
	int32& InOutNodes,
	TSet<FString>& ActiveTypes,
	FAvidScriptRuntimeBindingValuePlan& OutProgram,
	FString& OutDetails)
{
	FStructProperty* StructProperty = CastField<FStructProperty>(Property);
	if (StructProperty == nullptr || StructProperty->Struct == nullptr
		|| Type.Kind != TEXT("struct_wire")
		|| Type.CanonicalType != TEXT("struct_wire:") + StructProperty->Struct->GetPathName()
		|| Type.AbiTypes != TArray<FString>({ TEXT("i") })
		|| Type.Size <= 0 || Type.Size > 4096
		|| Type.Alignment <= 0 || Type.Alignment > 4096
		|| Depth > 8 || ActiveTypes.Contains(Type.StableId))
	{
		OutDetails = TEXT("The reflected struct no longer matches its fixed wire type.");
		return false;
	}

	TArray<FProperty*, TInlineAllocator<128>> ReflectedFields;
	for (TFieldIterator<FProperty> It(
		StructProperty->Struct,
		EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		ReflectedFields.Add(*It);
	}
	if (ReflectedFields.Num() != Type.StructFields.Num())
	{
		OutDetails = TEXT("The reflected struct field count changed since descriptor generation.");
		return false;
	}

	ActiveTypes.Add(Type.StableId);
	OutProgram.Kind = EAvidScriptRuntimeBindingKind::StructWire;
	OutProgram.StructType = StructProperty->Struct;
	OutProgram.WireSize = Type.Size;
	OutProgram.WireAlignment = Type.Alignment;
	OutProgram.GuestStorageSize = Type.Size;
	OutProgram.Children.Reset(Type.StructFields.Num());
	int32 PreviousEnd = 0;
	int32 ExpectedAlignment = 1;
	for (int32 Index = 0; Index < Type.StructFields.Num(); ++Index)
	{
		const FAvidScriptBindingStructFieldModel& Field = Type.StructFields[Index];
		FProperty* ReflectedField = ReflectedFields[Index];
		const FAvidScriptBindingTypeModel* ChildType = DeclaredTypesById.FindRef(Field.TypeId);
		if (++InOutNodes > 128 || ChildType == nullptr
			|| ReflectedField == nullptr
			|| ReflectedField->GetOwnerStruct() == nullptr
			|| !StructProperty->Struct->IsChildOf(ReflectedField->GetOwnerStruct())
			|| ReflectedField->GetName() != Field.Name
			|| !IsAvidScriptStructWireFieldSafe(ReflectedField))
		{
			ActiveTypes.Remove(Type.StableId);
			OutDetails = TEXT("The reflected struct field graph no longer matches its fixed wire layout.");
			return false;
		}

		FAvidScriptRuntimeBindingValuePlan& Child =
			OutProgram.Children.AddDefaulted_GetRef();
		Child.Property = ReflectedField;
		Child.Name = Field.Name;
		Child.WireOffset = Field.WireOffset;
		Child.WireSize = ChildType->Size;
		Child.WireAlignment = ChildType->Alignment;
		Child.GuestStorageSize = ChildType->Size;
		if (ChildType->Kind == TEXT("struct_wire"))
		{
			if (!BuildAvidScriptStructWireProgram(
				ReflectedField,
				*ChildType,
				DeclaredTypesById,
				Depth + 1,
				InOutNodes,
				ActiveTypes,
				Child,
				OutDetails))
			{
				ActiveTypes.Remove(Type.StableId);
				return false;
			}
			Child.WireOffset = Field.WireOffset;
		}
		else
		{
			FAvidScriptBindingValueModel ChildModel;
			ChildModel.Name = Field.Name;
			ChildModel.Direction = TEXT("value");
			ChildModel.CanonicalType = ChildType->CanonicalType;
			ChildModel.TypeId = ChildType->StableId;
			ChildModel.Kind = ChildType->Kind;
			ChildModel.CppType = ChildType->CppType;
			ChildModel.AbiTypes = ChildType->AbiTypes;
			if (!ResolveAvidScriptRuntimeKind(
				ReflectedField,
				ChildModel,
				ChildType,
				DeclaredTypesById,
				Child.Kind,
				Child.ObjectClass)
				|| Child.Kind == EAvidScriptRuntimeBindingKind::Name
				|| Child.Kind == EAvidScriptRuntimeBindingKind::String
				|| !MatchesAvidScriptRuntimeCanonicalLeafStorage(
					*ChildType,
					Child.Kind,
					Child.ObjectClass))
			{
				ActiveTypes.Remove(Type.StableId);
				OutDetails = TEXT("A reflected struct field is not a supported fixed-width leaf.");
				return false;
			}
			Child.WireSize = GetAvidScriptRuntimeGuestStorageSize(Child.Kind);
			Child.WireAlignment = GetAvidScriptRuntimeGuestStorageAlignment(Child.Kind);
			Child.GuestStorageSize = Child.WireSize;
		}
		if (Child.WireSize <= 0 || Child.WireSize > 4096
			|| Child.WireAlignment <= 0 || Child.WireAlignment > 4096
			|| Field.WireOffset < PreviousEnd
			|| Field.WireOffset % Child.WireAlignment != 0
			|| Field.WireOffset > Type.Size
			|| Child.WireSize > Type.Size - Field.WireOffset)
		{
			ActiveTypes.Remove(Type.StableId);
			OutDetails = TEXT("The reflected struct field graph no longer matches its fixed wire layout.");
			return false;
		}
		Child.WireOffset = Field.WireOffset;
		PreviousEnd = Field.WireOffset + Child.WireSize;
		ExpectedAlignment = FMath::Max(ExpectedAlignment, Child.WireAlignment);
	}
	ActiveTypes.Remove(Type.StableId);
	const int32 ExpectedSize = Align(PreviousEnd, ExpectedAlignment);
	if (Type.Alignment != ExpectedAlignment || Type.Size != ExpectedSize)
	{
		OutDetails = TEXT("The struct wire aggregate size or alignment changed.");
		return false;
	}
	return true;
}

bool BuildAvidScriptCompositeValueProgram(
	FProperty* Property,
	const FAvidScriptBindingTypeModel& Type,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& DeclaredTypesById,
	int32 Depth,
	int32& InOutNodes,
	FAvidScriptRuntimeBindingValuePlan& OutProgram,
	FString& OutDetails);

bool BuildAvidScriptArrayProgram(
	FProperty* Property,
	const FAvidScriptBindingTypeModel& Type,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& DeclaredTypesById,
	FAvidScriptRuntimeBindingValuePlan& OutProgram,
	FString& OutDetails)
{
	FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
	const FAvidScriptBindingTypeModel* ElementType =
		DeclaredTypesById.FindRef(Type.ElementTypeId);
	if (ArrayProperty == nullptr
		|| ArrayProperty->Inner == nullptr
		|| Type.Kind != TEXT("array")
		|| !Type.CanonicalType.StartsWith(TEXT("array:tarray<"))
		|| Type.AbiTypes != TArray<FString>({ TEXT("i") })
		|| Type.Size != 4
		|| Type.Alignment != 4
		|| ElementType == nullptr)
	{
		OutDetails = TEXT("The reflected array no longer matches its bounded descriptor type.");
		return false;
	}
	if (Type.CapabilityKind == TEXT("composite"))
	{
		int32 Nodes = 0;
		return BuildAvidScriptCompositeValueProgram(
			Property,
			Type,
			DeclaredTypesById,
			1,
			Nodes,
			OutProgram,
			OutDetails);
	}
	if (Type.CapabilityKind != TEXT("array_flat")
		|| ElementType->Kind == TEXT("array")
		|| ElementType->Kind == TEXT("set")
		|| ElementType->Kind == TEXT("map")
		|| ElementType->Kind == TEXT("name_utf8")
		|| ElementType->Kind == TEXT("string_utf8")
		|| ElementType->Kind == TEXT("text_capability")
		|| ElementType->Kind == TEXT("soft_object_capability")
		|| ElementType->Kind == TEXT("weak_object_capability"))
	{
		OutDetails = TEXT("The reflected array no longer matches its bounded descriptor type.");
		return false;
	}

	OutProgram.Kind = EAvidScriptRuntimeBindingKind::Array;
	OutProgram.TypeId = Type.StableId;
	OutProgram.WireSize = 4;
	OutProgram.WireAlignment = 4;
	OutProgram.GuestStorageSize = 4;
	FAvidScriptRuntimeBindingValuePlan& Element =
		OutProgram.Children.AddDefaulted_GetRef();
	Element.Property = ArrayProperty->Inner;
	Element.Name = TEXT("element");
	Element.TypeId = ElementType->StableId;
	Element.WireSize = ElementType->Size;
	Element.WireAlignment = ElementType->Alignment;
	Element.GuestStorageSize = ElementType->Size;
	if (ElementType->Kind == TEXT("struct_wire"))
	{
		int32 Nodes = 0;
		TSet<FString> ActiveTypes;
		if (!BuildAvidScriptStructWireProgram(
				ArrayProperty->Inner,
				*ElementType,
				DeclaredTypesById,
				1,
				Nodes,
				ActiveTypes,
				Element,
				OutDetails))
		{
			return false;
		}
		Element.TypeId = ElementType->StableId;
	}
	else
	{
		FAvidScriptBindingValueModel ElementModel;
		ElementModel.Name = TEXT("element");
		ElementModel.Direction = TEXT("value");
		ElementModel.CanonicalType = ElementType->CanonicalType;
		ElementModel.TypeId = ElementType->StableId;
		ElementModel.Kind = ElementType->Kind;
		ElementModel.CppType = ElementType->CppType;
		ElementModel.AbiTypes = ElementType->AbiTypes;
		if (!ResolveAvidScriptRuntimeKind(
				ArrayProperty->Inner,
				ElementModel,
				ElementType,
				DeclaredTypesById,
				Element.Kind,
				Element.ObjectClass)
			|| Element.Kind == EAvidScriptRuntimeBindingKind::Void
			|| Element.Kind == EAvidScriptRuntimeBindingKind::Name
			|| Element.Kind == EAvidScriptRuntimeBindingKind::String
			|| !MatchesAvidScriptRuntimeCanonicalLeafStorage(
				*ElementType,
				Element.Kind,
				Element.ObjectClass))
		{
			OutDetails = TEXT("The reflected array element is not a supported fixed-width value.");
			return false;
		}
		Element.WireSize = GetAvidScriptRuntimeGuestStorageSize(Element.Kind);
		Element.WireAlignment = GetAvidScriptRuntimeGuestStorageAlignment(Element.Kind);
		Element.GuestStorageSize = Element.WireSize;
	}
	const int32 ElementStride = Align(Element.WireSize, Element.WireAlignment);
	if (Element.WireSize <= 0
		|| Element.WireAlignment <= 0
		|| Element.WireAlignment > 16
		|| ElementStride <= 0
		|| static_cast<int64>(ElementStride)
			* FAvidScriptArrayValueHeap::MaxElements
			> FAvidScriptArrayValueHeap::MaxValueBytes)
	{
		OutDetails = TEXT("The reflected array element exceeds the bounded value layout.");
		return false;
	}
	return true;
}

bool BuildAvidScriptCompositeValueProgram(
	FProperty* Property,
	const FAvidScriptBindingTypeModel& Type,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& DeclaredTypesById,
	const int32 Depth,
	int32& InOutNodes,
	FAvidScriptRuntimeBindingValuePlan& OutProgram,
	FString& OutDetails)
{
	if (Property == nullptr || Depth > 8 || ++InOutNodes > 4096)
	{
		OutDetails = TEXT("The reflected composite value graph exceeds its bounded depth or node contract.");
		return false;
	}
	OutProgram.Property = Property;
	if (Type.Kind == TEXT("array"))
	{
		FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
		const FAvidScriptBindingTypeModel* ElementType =
			DeclaredTypesById.FindRef(Type.ElementTypeId);
		if (ArrayProperty == nullptr
			|| ArrayProperty->Inner == nullptr
			|| ElementType == nullptr
			|| Type.TypeArguments != TArray<FString>({ Type.ElementTypeId })
			|| Type.CanonicalType != TEXT("array:tarray<")
				+ ElementType->CanonicalType + TEXT(">")
			|| Type.CapabilityKind != TEXT("composite")
			|| Type.AbiTypes != TArray<FString>({ TEXT("i") })
			|| Type.Size != 4
			|| Type.Alignment != 4)
		{
			OutDetails = TEXT("The reflected composite array no longer matches its descriptor type graph.");
			return false;
		}
		OutProgram.Kind = EAvidScriptRuntimeBindingKind::CompositeArray;
		OutProgram.TypeId = Type.StableId;
		OutProgram.WireSize = 4;
		OutProgram.WireAlignment = 4;
		OutProgram.GuestStorageSize = 4;
		FAvidScriptRuntimeBindingValuePlan& Element =
			OutProgram.Children.AddDefaulted_GetRef();
		Element.Property = ArrayProperty->Inner;
		Element.Name = TEXT("element");
		return BuildAvidScriptCompositeValueProgram(
			ArrayProperty->Inner,
			*ElementType,
			DeclaredTypesById,
			Depth + 1,
			InOutNodes,
			Element,
			OutDetails);
	}
	const bool bSet = Type.Kind == TEXT("set");
	const bool bMap = Type.Kind == TEXT("map");
	if (bSet || bMap)
	{
		const int32 ExpectedArguments = bSet ? 1 : 2;
		if (Type.TypeArguments.Num() != ExpectedArguments
			|| Type.CapabilityKind != TEXT("composite")
			|| Type.AbiTypes != TArray<FString>({ TEXT("i") })
			|| Type.Size != 4
			|| Type.Alignment != 4)
		{
			OutDetails = TEXT("The reflected associative container no longer matches its descriptor type graph.");
			return false;
		}
		OutProgram.Kind = bSet
			? EAvidScriptRuntimeBindingKind::Set
			: EAvidScriptRuntimeBindingKind::Map;
		OutProgram.TypeId = Type.StableId;
		OutProgram.WireSize = 4;
		OutProgram.WireAlignment = 4;
		OutProgram.GuestStorageSize = 4;
		TArray<FProperty*, TInlineAllocator<2>> ReflectedChildren;
		if (bSet)
		{
			FSetProperty* SetProperty = CastField<FSetProperty>(Property);
			if (SetProperty != nullptr && SetProperty->ElementProp != nullptr)
			{
				ReflectedChildren.Add(SetProperty->ElementProp);
			}
		}
		else
		{
			FMapProperty* MapProperty = CastField<FMapProperty>(Property);
			if (MapProperty != nullptr
				&& MapProperty->KeyProp != nullptr
				&& MapProperty->ValueProp != nullptr)
			{
				ReflectedChildren.Add(MapProperty->KeyProp);
				ReflectedChildren.Add(MapProperty->ValueProp);
			}
		}
		if (ReflectedChildren.Num() != ExpectedArguments)
		{
			OutDetails = TEXT("The reflected associative container properties are unavailable.");
			return false;
		}
		for (int32 Index = 0; Index < ExpectedArguments; ++Index)
		{
			const FAvidScriptBindingTypeModel* ChildType =
				DeclaredTypesById.FindRef(Type.TypeArguments[Index]);
			if (ChildType == nullptr)
			{
				OutDetails = TEXT("The reflected associative container child type is unavailable.");
				return false;
			}
			FAvidScriptRuntimeBindingValuePlan& Child =
				OutProgram.Children.AddDefaulted_GetRef();
			Child.Property = ReflectedChildren[Index];
			Child.Name = bSet ? TEXT("element")
				: (Index == 0 ? TEXT("key") : TEXT("value"));
			if (!BuildAvidScriptCompositeValueProgram(
					ReflectedChildren[Index],
					*ChildType,
					DeclaredTypesById,
					Depth + 1,
					InOutNodes,
					Child,
					OutDetails))
			{
				return false;
			}
		}
		const EAvidScriptRuntimeBindingKind KeyKind =
			OutProgram.Children[0].Kind;
		const bool bCanonicalKeyKind =
			KeyKind == EAvidScriptRuntimeBindingKind::Bool
			|| KeyKind == EAvidScriptRuntimeBindingKind::Int8
			|| KeyKind == EAvidScriptRuntimeBindingKind::UInt8
			|| KeyKind == EAvidScriptRuntimeBindingKind::Int16
			|| KeyKind == EAvidScriptRuntimeBindingKind::UInt16
			|| KeyKind == EAvidScriptRuntimeBindingKind::Int32
			|| KeyKind == EAvidScriptRuntimeBindingKind::UInt32
			|| KeyKind == EAvidScriptRuntimeBindingKind::Int64
			|| KeyKind == EAvidScriptRuntimeBindingKind::UInt64
			|| KeyKind == EAvidScriptRuntimeBindingKind::Enum
			|| KeyKind == EAvidScriptRuntimeBindingKind::Name
			|| KeyKind == EAvidScriptRuntimeBindingKind::String;
		if (!bCanonicalKeyKind
			|| OutProgram.Children[0].Property == nullptr
			|| !OutProgram.Children[0].Property->HasAnyPropertyFlags(
				CPF_HasGetValueTypeHash))
		{
			OutDetails = TEXT("The reflected associative key is outside the canonical key whitelist.");
			return false;
		}
		return true;
	}

	if (Type.Kind == TEXT("struct_wire"))
	{
		TSet<FString> ActiveTypes;
		return BuildAvidScriptStructWireProgram(
			Property,
			Type,
			DeclaredTypesById,
			Depth,
			InOutNodes,
			ActiveTypes,
			OutProgram,
			OutDetails);
	}
	FAvidScriptBindingValueModel ValueModel;
	ValueModel.Name = Property->GetName();
	ValueModel.Direction = TEXT("value");
	ValueModel.CanonicalType = Type.CanonicalType;
	ValueModel.TypeId = Type.StableId;
	ValueModel.Kind = Type.Kind;
	ValueModel.CppType = Type.CppType;
	ValueModel.AbiTypes = Type.AbiTypes;
	if (!ResolveAvidScriptRuntimeKind(
			Property,
			ValueModel,
			&Type,
			DeclaredTypesById,
			OutProgram.Kind,
			OutProgram.ObjectClass)
		|| OutProgram.Kind == EAvidScriptRuntimeBindingKind::Void
		|| !MatchesAvidScriptRuntimeCanonicalLeafStorage(
			Type,
			OutProgram.Kind,
			OutProgram.ObjectClass))
	{
		OutDetails = TEXT("The reflected composite child is not a supported canonical value.");
		return false;
	}
	OutProgram.TypeId = Type.StableId;
	OutProgram.WireSize = GetAvidScriptRuntimeGuestStorageSize(OutProgram.Kind);
	OutProgram.WireAlignment = GetAvidScriptRuntimeGuestStorageAlignment(OutProgram.Kind);
	OutProgram.GuestStorageSize = OutProgram.WireSize;
	return true;
}

FString MakeAvidScriptRuntimeExpectedSignature(const FAvidScriptBindingFunctionModel& Binding)
{
	if (Binding.DispatchMode == TEXT("generated_native_s1"))
	{
		if (Binding.GeneratedShape == TEXT("i32_pair_to_i32"))
		{
			return TEXT("(iiii)i");
		}
		if (Binding.GeneratedShape == TEXT("property_i32_get_set")
			|| Binding.GeneratedShape == TEXT("vector_value"))
		{
			return TEXT("(iii)i");
		}
		if (Binding.GeneratedShape == TEXT("property_i32_get"))
		{
			return TEXT("(ii)i");
		}
		if (Binding.GeneratedShape == TEXT("property_i32_set"))
		{
			return TEXT("(iii)i");
		}
		if (Binding.GeneratedShape == TEXT("stable_object_roundtrip"))
		{
			return TEXT("(iiiii)i");
		}
	}

	FString Parameters;
	if (!Binding.bStatic)
	{
		Parameters += TEXT("ii");
	}
	for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
	{
		if (Parameter.Direction == TEXT("ref") || Parameter.Direction == TEXT("out"))
		{
			Parameters += TEXT("i");
		}
		else
		{
			Parameters += FString::Join(Parameter.AbiTypes, TEXT(""));
		}
	}
	if (Binding.DispatchMode == TEXT("latent_process_event"))
	{
		return TEXT("(") + Parameters + TEXT("i)I");
	}
	if (Binding.ReturnValue.CanonicalType != TEXT("void"))
	{
		Parameters += TEXT("i");
	}
	return TEXT("(") + Parameters + TEXT(")i");
}

FString MakeAvidScriptRuntimeCanonicalIdentity(
	const UClass* OwnerClass,
	const UFunction* Function,
	const FAvidScriptBindingFunctionModel& Binding)
{
	FString Identity = OwnerClass->GetPathName()
		+ TEXT("::")
		+ Function->GetName()
		+ TEXT("(")
		+ Binding.ReturnValue.CanonicalType;
	for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
	{
		Identity += TEXT(";")
			+ Parameter.Name
			+ TEXT(":")
			+ Parameter.Direction
			+ TEXT(":")
			+ Parameter.CanonicalType;
	}
	Identity += TEXT(")");
	if (Binding.Network.IsNetworked())
	{
		Identity += TEXT("|network_mode=")
			+ FString(LexToString(Binding.Network.Mode))
			+ TEXT("|network_reliable=")
			+ (Binding.Network.bReliable ? TEXT("1") : TEXT("0"));
	}
	if (Binding.DispatchMode == TEXT("latent_process_event"))
	{
		Identity += TEXT("|latent_info=") + Binding.LatentInfoParameter
			+ TEXT("|world_context=") + Binding.WorldContextParameter;
		if (Binding.Completion.Mode == TEXT("provider"))
		{
			Identity += TEXT("|completion=provider|provider_id=")
				+ Binding.Completion.ProviderId
				+ TEXT("|payload_type_id=")
				+ Binding.Completion.PayloadTypeId
				+ TEXT("|status_policy=")
				+ Binding.Completion.StatusPolicy
				+ TEXT("|cancellable=1");
		}
	}
	return FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
		Identity,
		Binding.DispatchMode,
		Binding.GeneratedShape,
		Binding.GeneratedReceiverMode,
		Binding.GeneratedImportName);
}

FString MakeAvidScriptRuntimePropertyGetCanonicalIdentity(
	const UClass* OwnerClass,
	const FProperty* Property,
	const FAvidScriptBindingFunctionModel& Binding)
{
	FString BaseIdentity = OwnerClass->GetPathName()
		+ TEXT("::property_get:") + Property->GetName()
		+ TEXT("(") + Binding.ReturnValue.CanonicalType + TEXT(")");
	if (Binding.PropertyReplication.IsReplicated())
	{
		BaseIdentity += TEXT("|property_replication=")
			+ FString(LexToString(Binding.PropertyReplication.Mode))
			+ TEXT("|rep_notify=")
			+ Binding.PropertyReplication.RepNotifyFunction.ToString();
	}
	if (Binding.DispatchMode != TEXT("generated_native_s1"))
	{
		return BaseIdentity;
	}
	return FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
		BaseIdentity,
		Binding.DispatchMode,
		Binding.GeneratedShape,
		Binding.GeneratedReceiverMode,
		Binding.GeneratedImportName);
}

FString MakeAvidScriptRuntimePropertySetCanonicalIdentity(
	const UClass* OwnerClass,
	const FProperty* Property,
	const FAvidScriptBindingFunctionModel& Binding)
{
	FString BaseIdentity =
		FAvidScriptBindingDescriptorIdentity::MakePropertySetCanonicalIdentity(
		OwnerClass->GetPathName(),
		Property->GetName(),
			Binding.Parameters[0].CanonicalType,
			Binding.UeFunction);
	if (Binding.PropertyReplication.IsReplicated())
	{
		BaseIdentity += TEXT("|property_replication=")
			+ FString(LexToString(Binding.PropertyReplication.Mode))
			+ TEXT("|rep_notify=")
			+ Binding.PropertyReplication.RepNotifyFunction.ToString();
	}
	if (Binding.DispatchMode != TEXT("generated_native_s1"))
	{
		return BaseIdentity;
	}
	return FAvidScriptBindingDescriptorIdentity::MakeFunctionCanonicalIdentity(
		BaseIdentity,
		Binding.DispatchMode,
		Binding.GeneratedShape,
		Binding.GeneratedReceiverMode,
		Binding.GeneratedImportName);
}

FString MakeAvidScriptRuntimeSelectionHash(const FAvidScriptBindingPackageModel& Package)
{
	return FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
}

FString MakeAvidScriptRuntimePackageHash(const FAvidScriptBindingPackageModel& Package)
{
	return FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);
}

bool BuildAvidScriptRuntimeValuePlan(
	FProperty* Property,
	const FAvidScriptBindingValueModel& Model,
	const TMap<FString, const FAvidScriptBindingTypeModel*>& DeclaredTypesById,
	int32 ArgumentOffset,
	FAvidScriptRuntimeBindingValuePlan& OutPlan,
	FString& OutDetails)
{
	OutPlan = FAvidScriptRuntimeBindingValuePlan();
	OutPlan.Property = Property;
	OutPlan.ArgumentOffset = ArgumentOffset;
	OutPlan.Name = Model.Name;
	const FAvidScriptBindingTypeModel* DeclaredType = Model.CanonicalType == TEXT("void")
		? nullptr
		: DeclaredTypesById.FindRef(Model.TypeId);
	if (!ParseAvidScriptRuntimeDirection(Model.Direction, OutPlan.Direction))
	{
		OutDetails = TEXT("The descriptor value direction is invalid.");
		return false;
	}
	if (Model.Kind == TEXT("struct_wire"))
	{
		int32 Nodes = 0;
		TSet<FString> ActiveTypes;
		if (DeclaredType == nullptr
			|| !BuildAvidScriptStructWireProgram(
				Property,
				*DeclaredType,
				DeclaredTypesById,
				1,
				Nodes,
				ActiveTypes,
				OutPlan,
				OutDetails))
		{
			return false;
		}
		OutPlan.ArgumentWidth = 1;
		return true;
	}
	if (Model.Kind == TEXT("array"))
	{
		if (DeclaredType == nullptr
			|| !BuildAvidScriptArrayProgram(
				Property,
				*DeclaredType,
				DeclaredTypesById,
				OutPlan,
				OutDetails))
		{
			return false;
		}
		OutPlan.ArgumentWidth = 1;
		return true;
	}
	if (Model.Kind == TEXT("set") || Model.Kind == TEXT("map"))
	{
		int32 Nodes = 0;
		if (DeclaredType == nullptr
			|| !BuildAvidScriptCompositeValueProgram(
				Property,
				*DeclaredType,
				DeclaredTypesById,
				1,
				Nodes,
				OutPlan,
				OutDetails))
		{
			return false;
		}
		OutPlan.ArgumentWidth = 1;
		return true;
	}
	if (!ResolveAvidScriptRuntimeKind(
			Property,
			Model,
			DeclaredType,
			DeclaredTypesById,
			OutPlan.Kind,
			OutPlan.ObjectClass))
	{
		OutDetails = FString::Printf(
			TEXT("Reflected property '%s' no longer matches descriptor type '%s'."),
			*Model.Name,
			*Model.CanonicalType);
		return false;
	}
	OutPlan.ArgumentWidth = GetAvidScriptRuntimeArgumentWidth(Model, OutPlan.Direction);
	OutPlan.TypeId = Model.TypeId;
	OutPlan.GuestStorageSize = GetAvidScriptRuntimeGuestStorageSize(OutPlan.Kind);
	OutPlan.WireSize = OutPlan.GuestStorageSize;
	OutPlan.WireAlignment = DeclaredType == nullptr ? 1 : DeclaredType->Alignment;
	return OutPlan.ArgumentWidth > 0 || OutPlan.Kind == EAvidScriptRuntimeBindingKind::Void;
}

struct FAvidScriptPreparedDelegateCodec
{
	FString StableId;
	FAvidScriptRuntimeBindingValuePlan ReturnValue;
	TArray<FAvidScriptRuntimeBindingValuePlan> Parameters;
	TArray<int32> OutputParameterIndices;
	uint32 ParameterCellCount = 0;
	bool bHasReturnValue = false;
};

struct FAvidScriptPreparedDelegateEventCell
{
	uint32 EventOrdinal = MAX_uint32;
	FString StableId;
	FString ExportName;
	FString CallbackKind = TEXT("multicast");
	FString HandlerMode = TEXT("replace");
	UClass* ExpectedSourceClass = nullptr;
	EAvidScriptPreparedDelegateKind Kind =
		EAvidScriptPreparedDelegateKind::Multicast;
	FDelegateProperty* SinglecastProperty = nullptr;
	FMulticastDelegateProperty* MulticastProperty = nullptr;
	UFunction* SignatureFunction = nullptr;
	FProperty* RepNotifyProperty = nullptr;
	FAvidScriptBindingNetworkContract Network;
	FAvidScriptPreparedDelegateCodec Codec;
};

bool IsAvidScriptPreparedDelegateValueSupported(
	const FAvidScriptRuntimeBindingValuePlan& Plan)
{
	if (Plan.Direction != EAvidScriptRuntimeBindingDirection::Value
		&& Plan.Direction != EAvidScriptRuntimeBindingDirection::ConstRef
		&& Plan.Direction != EAvidScriptRuntimeBindingDirection::Ref
		&& Plan.Direction != EAvidScriptRuntimeBindingDirection::Out
		&& Plan.Direction != EAvidScriptRuntimeBindingDirection::Return)
	{
		return false;
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Name
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::String
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::Text
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::SoftObject
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::WeakObject
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::Array
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::Void)
	{
		return false;
	}
	if (Plan.Kind != EAvidScriptRuntimeBindingKind::StructWire)
	{
		return true;
	}
	return !Plan.Children.IsEmpty()
		&& !Plan.Children.ContainsByPredicate(
			[](const FAvidScriptRuntimeBindingValuePlan& Child)
			{
				return !IsAvidScriptPreparedDelegateValueSupported(Child);
			});
}

bool CountAvidScriptPreparedDelegateValueCells(
	const FAvidScriptRuntimeBindingValuePlan& Plan,
	uint32& InOutCellCount)
{
	uint32 AddedCells = 0;
	switch (Plan.Kind)
	{
	case EAvidScriptRuntimeBindingKind::Bool:
	case EAvidScriptRuntimeBindingKind::Int8:
	case EAvidScriptRuntimeBindingKind::UInt8:
	case EAvidScriptRuntimeBindingKind::Int16:
	case EAvidScriptRuntimeBindingKind::UInt16:
	case EAvidScriptRuntimeBindingKind::Int32:
	case EAvidScriptRuntimeBindingKind::UInt32:
	case EAvidScriptRuntimeBindingKind::Float:
	case EAvidScriptRuntimeBindingKind::Enum:
		AddedCells = 1;
		break;
	case EAvidScriptRuntimeBindingKind::Int64:
	case EAvidScriptRuntimeBindingKind::UInt64:
	case EAvidScriptRuntimeBindingKind::Double:
	case EAvidScriptRuntimeBindingKind::Object:
	case EAvidScriptRuntimeBindingKind::Interface:
		AddedCells = 2;
		break;
	case EAvidScriptRuntimeBindingKind::Vector:
	case EAvidScriptRuntimeBindingKind::Rotator:
		AddedCells = 3;
		break;
	case EAvidScriptRuntimeBindingKind::Transform:
		AddedCells = 9;
		break;
	case EAvidScriptRuntimeBindingKind::StructWire:
		for (const FAvidScriptRuntimeBindingValuePlan& Child : Plan.Children)
		{
			if (!CountAvidScriptPreparedDelegateValueCells(Child, InOutCellCount))
			{
				return false;
			}
		}
		return true;
	default:
		return false;
	}

	if (AddedCells > FAvidScriptVmCallFrame::MaxCells - InOutCellCount)
	{
		return false;
	}
	InOutCellCount += AddedCells;
	return true;
}

template <typename TValue>
bool AppendAvidScriptPreparedDelegateCells(
	const TValue& Value,
	FAvidScriptVmCallFrame& OutFrame)
{
	static_assert(sizeof(TValue) % sizeof(uint32) == 0);
	constexpr uint32 CellCount = sizeof(TValue) / sizeof(uint32);
	if (CellCount > FAvidScriptVmCallFrame::MaxCells - OutFrame.CellCount)
	{
		return false;
	}
	FMemory::Memcpy(
		OutFrame.Cells + OutFrame.CellCount,
		&Value,
		sizeof(TValue));
	OutFrame.CellCount += CellCount;
	return true;
}

bool EncodeAvidScriptPreparedDelegateValue(
	const FAvidScriptRuntimeBindingValuePlan& Plan,
	const void* Container,
	const FAvidScriptBindingInvocationContext& InvocationContext,
	FAvidScriptVmCallFrame& OutFrame,
	TArray<FAvidScriptObjectHandle>& OutBorrowedHandles,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	if (Container == nullptr || Plan.Property == nullptr)
	{
		OutErrorCategory = TEXT("delegate_event_parameters_invalid");
		OutErrorDetails = TEXT("The native delegate parameter frame does not match the prepared codec.");
		return false;
	}
	const void* Value = Plan.Property->ContainerPtrToValuePtr<void>(Container);
	if (Value == nullptr)
	{
		OutErrorCategory = TEXT("delegate_event_parameters_invalid");
		OutErrorDetails = TEXT("A prepared delegate parameter has no native value address.");
		return false;
	}

	if (Plan.Kind == EAvidScriptRuntimeBindingKind::StructWire)
	{
		for (const FAvidScriptRuntimeBindingValuePlan& Child : Plan.Children)
		{
			if (!EncodeAvidScriptPreparedDelegateValue(
					Child,
					Value,
					InvocationContext,
					OutFrame,
					OutBorrowedHandles,
					OutErrorCategory,
					OutErrorDetails))
			{
				return false;
			}
		}
		return true;
	}

	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Bool)
	{
		const uint32 Stored = CastFieldChecked<FBoolProperty>(Plan.Property)
			->GetPropertyValue(Value) ? 1u : 0u;
		return AppendAvidScriptPreparedDelegateCells(Stored, OutFrame);
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Float)
	{
		const float Stored = CastFieldChecked<FFloatProperty>(Plan.Property)
			->GetPropertyValue(Value);
		return AppendAvidScriptPreparedDelegateCells(Stored, OutFrame);
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Double)
	{
		const double Stored = CastFieldChecked<FDoubleProperty>(Plan.Property)
			->GetPropertyValue(Value);
		return AppendAvidScriptPreparedDelegateCells(Stored, OutFrame);
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Object
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::Interface)
	{
		UObject* Object = Plan.Kind == EAvidScriptRuntimeBindingKind::Interface
			? CastFieldChecked<FInterfaceProperty>(Plan.Property)
				->GetPropertyValue(Value).GetObject()
			: CastFieldChecked<FObjectPropertyBase>(Plan.Property)
				->GetObjectPropertyValue(Value);
		FAvidScriptObjectHandle Handle;
		if (Object != nullptr)
		{
			if (!UE::AvidScript::BindingPrivate::IsObjectCompatibleWithReflectedType(
					Object,
					Plan.ObjectClass))
			{
				OutErrorCategory = TEXT("delegate_event_object_type_mismatch");
				OutErrorDetails = TEXT("A delegate UObject parameter no longer satisfies its prepared class contract.");
				return false;
			}
			if (InvocationContext.ObjectRegistry == nullptr)
			{
				OutErrorCategory = TEXT("delegate_event_object_registry_missing");
				OutErrorDetails = TEXT("A delegate UObject parameter requires the active session object registry.");
				return false;
			}
			FAvidScriptObjectHandleResult BorrowResult;
			Handle = InvocationContext.ObjectRegistry->AcquireBorrowedObject(
				Object,
				BorrowResult,
				false);
			if (!Handle.IsValid())
			{
				OutErrorCategory = TEXT("delegate_event_object_borrow_failed");
				OutErrorDetails = BorrowResult.ErrorMessage;
				return false;
			}
			OutBorrowedHandles.Add(Handle);
		}
		return AppendAvidScriptPreparedDelegateCells(Handle, OutFrame);
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Vector
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::Rotator
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::Transform)
	{
		float Components[9] = {};
		uint32 ComponentCount = 0;
		if (Plan.Kind == EAvidScriptRuntimeBindingKind::Vector)
		{
			const FVector& Stored = *static_cast<const FVector*>(Value);
			Components[0] = static_cast<float>(Stored.X);
			Components[1] = static_cast<float>(Stored.Y);
			Components[2] = static_cast<float>(Stored.Z);
			ComponentCount = 3;
		}
		else if (Plan.Kind == EAvidScriptRuntimeBindingKind::Rotator)
		{
			const FRotator& Stored = *static_cast<const FRotator*>(Value);
			Components[0] = static_cast<float>(Stored.Pitch);
			Components[1] = static_cast<float>(Stored.Yaw);
			Components[2] = static_cast<float>(Stored.Roll);
			ComponentCount = 3;
		}
		else
		{
			const FTransform& Stored = *static_cast<const FTransform*>(Value);
			const FVector Translation = Stored.GetTranslation();
			const FRotator Rotation = Stored.Rotator();
			const FVector Scale = Stored.GetScale3D();
			Components[0] = static_cast<float>(Translation.X);
			Components[1] = static_cast<float>(Translation.Y);
			Components[2] = static_cast<float>(Translation.Z);
			Components[3] = static_cast<float>(Rotation.Pitch);
			Components[4] = static_cast<float>(Rotation.Yaw);
			Components[5] = static_cast<float>(Rotation.Roll);
			Components[6] = static_cast<float>(Scale.X);
			Components[7] = static_cast<float>(Scale.Y);
			Components[8] = static_cast<float>(Scale.Z);
			ComponentCount = 9;
		}
		for (uint32 Index = 0; Index < ComponentCount; ++Index)
		{
			if (!AppendAvidScriptPreparedDelegateCells(Components[Index], OutFrame))
			{
				return false;
			}
		}
		return true;
	}

	const FNumericProperty* Numeric = CastField<FNumericProperty>(Plan.Property);
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Enum)
	{
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(Plan.Property))
		{
			Numeric = Enum->GetUnderlyingProperty();
		}
	}
	if (Numeric == nullptr)
	{
		OutErrorCategory = TEXT("delegate_event_codec_invalid");
		OutErrorDetails = TEXT("A prepared delegate numeric codec no longer has a numeric property.");
		return false;
	}

	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Int64)
	{
		const int64 Stored = Numeric->GetSignedIntPropertyValue(Value);
		return AppendAvidScriptPreparedDelegateCells(Stored, OutFrame);
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::UInt64)
	{
		const uint64 Stored = Numeric->GetUnsignedIntPropertyValue(Value);
		return AppendAvidScriptPreparedDelegateCells(Stored, OutFrame);
	}
	const bool bSigned = Plan.Kind == EAvidScriptRuntimeBindingKind::Int8
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::Int16
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::Int32;
	const uint32 Stored = bSigned
		? static_cast<uint32>(Numeric->GetSignedIntPropertyValue(Value))
		: static_cast<uint32>(Numeric->GetUnsignedIntPropertyValue(Value));
	return AppendAvidScriptPreparedDelegateCells(Stored, OutFrame);
}

bool EncodeAvidScriptPreparedDelegateEvent(
	const void* ImmutableCodecIdentity,
	const void* NativeParameters,
	const FAvidScriptBindingInvocationContext& InvocationContext,
	const uint32 OutputTransactionToken,
	FAvidScriptVmCallFrame& OutFrame,
	TArray<FAvidScriptObjectHandle>& OutBorrowedHandles,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	OutFrame = FAvidScriptVmCallFrame();
	OutBorrowedHandles.Reset();
	OutErrorCategory.Reset();
	OutErrorDetails.Reset();
	const FAvidScriptPreparedDelegateCodec* Codec =
		static_cast<const FAvidScriptPreparedDelegateCodec*>(ImmutableCodecIdentity);
	if (Codec == nullptr
		|| Codec->ParameterCellCount > FAvidScriptVmCallFrame::MaxCells
		|| (NativeParameters == nullptr && !Codec->Parameters.IsEmpty()))
	{
		OutErrorCategory = TEXT("delegate_event_codec_invalid");
		OutErrorDetails = TEXT("The prepared delegate codec or native parameter frame is unavailable.");
		return false;
	}
	if (!Codec->OutputParameterIndices.IsEmpty())
	{
		if (OutputTransactionToken == 0
			|| !AppendAvidScriptPreparedDelegateCells(
				OutputTransactionToken,
				OutFrame))
		{
			OutErrorCategory = TEXT("delegate_output_transaction_invalid");
			OutErrorDetails = TEXT("The prepared delegate output transaction token is unavailable.");
			return false;
		}
	}

	for (const FAvidScriptRuntimeBindingValuePlan& Parameter : Codec->Parameters)
	{
		if (Parameter.Direction == EAvidScriptRuntimeBindingDirection::Out)
		{
			continue;
		}
		if (!EncodeAvidScriptPreparedDelegateValue(
				Parameter,
				NativeParameters,
				InvocationContext,
				OutFrame,
				OutBorrowedHandles,
				OutErrorCategory,
				OutErrorDetails))
		{
			OutFrame = FAvidScriptVmCallFrame();
			return false;
		}
	}
	if (OutFrame.CellCount != Codec->ParameterCellCount)
	{
		OutFrame = FAvidScriptVmCallFrame();
		OutErrorCategory = TEXT("delegate_event_codec_invalid");
		OutErrorDetails = TEXT("The prepared delegate codec produced an unexpected VM cell count.");
		return false;
	}
	return true;
}

bool ResolveAvidScriptLifecycleWorld(
	const FAvidScriptBindingInvocationContext& Context,
	const FString& Source,
	UWorld*& OutWorld,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	OutWorld = Context.World.Get();
	if (!IsValid(OutWorld) || OutWorld->bIsTearingDown)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_world_invalid"),
			Source,
			TEXT("The Runtime Session has no live World for object lifecycle operations."));
		return false;
	}
	return true;
}

bool ResolveAvidScriptLifecycleClass(
	const FAvidScriptBindingPackage& Package,
	const uint64 RawOrdinal,
	const FString& Source,
	UClass*& OutClass,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	UClass* BaseClass = nullptr;
	if (RawOrdinal > MAX_uint32
		|| !Package.TryResolveClassReference(static_cast<uint32>(RawOrdinal), OutClass, BaseClass))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_class_ordinal_invalid"),
			Source,
			TEXT("The class reference ordinal is outside the immutable package class plan."));
		return false;
	}
	return true;
}

bool ReadAvidScriptLifecycleTransform(
	const uint64 RawAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	FTransform& OutTransform,
	FString& OutDetails)
{
	constexpr int32 ComponentCount = 9;
	constexpr int32 ByteCount = ComponentCount * sizeof(float);
	if (RawAddress > MAX_uint32)
	{
		OutDetails = TEXT("The FTransform address exceeds the 32-bit Guest address space.");
		return false;
	}

	uint8 Bytes[ByteCount] = {};
	if (!GuestMemory.ReadBytes(
		static_cast<uint32>(RawAddress),
		MakeArrayView(Bytes),
		OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = TEXT("The fixed 36-byte FTransform is outside Guest memory.");
		}
		return false;
	}

	float Components[ComponentCount] = {};
	FMemory::Memcpy(Components, Bytes, ByteCount);
	for (const float Component : Components)
	{
		if (!FMath::IsFinite(Component))
		{
			OutDetails = TEXT("The Guest FTransform contains a non-finite component.");
			return false;
		}
	}

	OutTransform = FTransform(
		FRotator(Components[3], Components[4], Components[5]),
		FVector(Components[0], Components[1], Components[2]),
		FVector(Components[6], Components[7], Components[8]));
	return true;
}

bool WriteAvidScriptLifecycleHandle(
	const uint64 RawAddress,
	const FAvidScriptObjectHandle& Handle,
	IAvidScriptVmGuestMemory& GuestMemory,
	FString& OutDetails)
{
	if (RawAddress > MAX_uint32)
	{
		OutDetails = TEXT("The object handle output address exceeds the 32-bit Guest address space.");
		return false;
	}

	uint8 Bytes[sizeof(uint32) * 2] = {};
	FMemory::Memcpy(Bytes, &Handle.Slot, sizeof(Handle.Slot));
	FMemory::Memcpy(Bytes + sizeof(Handle.Slot), &Handle.Generation, sizeof(Handle.Generation));
	if (!GuestMemory.WriteBytes(static_cast<uint32>(RawAddress), MakeArrayView(Bytes), OutDetails))
	{
		if (OutDetails.IsEmpty())
		{
			OutDetails = TEXT("The object handle output is outside Guest memory.");
		}
		return false;
	}
	return true;
}

bool DispatchAvidScriptObjectLifecycle(
	const FAvidScriptBindingPackage& Package,
	const FAvidScriptRuntimeBindingInvocationPlan& Plan,
	const FAvidScriptDynamicHostCall& Call,
	const FAvidScriptBindingInvocationContext& Context,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	if (Plan.bRequiresWriteAccess && Context.WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_write_denied"),
			Plan.DebugPath,
			TEXT("The object lifecycle operation requires an explicitly writable host context."));
		return false;
	}
	if (Context.HostEffectJournal != nullptr
		&& Plan.Kind != EAvidScriptBindingInvocationKind::ObjectIsA)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_reload_effect_unsupported"),
			Plan.DebugPath,
			TEXT("Candidate reload cannot roll back SpawnActor or DestroyActor side effects."));
		return false;
	}
	if (Context.ObjectRegistry == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_object_registry_missing"),
			Plan.DebugPath,
			TEXT("The Runtime Session has no object registry for lifecycle handles."));
		return false;
	}

	UWorld* World = nullptr;
	if (!ResolveAvidScriptLifecycleWorld(Context, Plan.DebugPath, World, OutResult))
	{
		return false;
	}

	if (Plan.Kind == EAvidScriptBindingInvocationKind::ObjectSpawnActor)
	{
		UClass* ActorClass = nullptr;
		if (!ResolveAvidScriptLifecycleClass(Package, Call.Arguments[0], Plan.DebugPath, ActorClass, OutResult))
		{
			return false;
		}

		FTransform Transform = FTransform::Identity;
		FString Details;
		if (!ReadAvidScriptLifecycleTransform(Call.Arguments[1], *Call.GuestMemory, Transform, Details))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_guest_read_failed"),
				Plan.DebugPath,
				Details);
			return false;
		}

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Actor = World->SpawnActor<AActor>(ActorClass, Transform, SpawnParameters);
		if (!IsValid(Actor) || Actor->GetWorld() != World)
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_spawn_failed"),
				Plan.DebugPath,
				TEXT("UWorld::SpawnActor did not return a live Actor in the injected World."));
			return false;
		}

		FAvidScriptObjectHandleResult RegisterResult;
		const FAvidScriptObjectHandle Handle = Context.ObjectRegistry->RegisterObject(Actor, RegisterResult, false);
		if (!RegisterResult.bSucceeded || !Handle.IsValid())
		{
			Actor->Destroy();
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_handle_registration_failed"),
				Plan.DebugPath,
				RegisterResult.ErrorMessage);
			return false;
		}

		if (!WriteAvidScriptLifecycleHandle(Call.Arguments[2], Handle, *Call.GuestMemory, Details))
		{
			FAvidScriptObjectHandleResult ReleaseResult;
			Context.ObjectRegistry->ReleaseHandle(Handle, ReleaseResult, false);
			Actor->Destroy();
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_guest_write_failed"),
				Plan.DebugPath,
				Details);
			return false;
		}

		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}

	if (Call.Arguments[0] > MAX_uint32 || Call.Arguments[1] > MAX_uint32)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			TEXT("The object handle cells exceed the 32-bit slot/generation ABI."));
		return false;
	}
	const FAvidScriptObjectHandle Handle{
		static_cast<uint32>(Call.Arguments[0]),
		static_cast<uint32>(Call.Arguments[1])
	};
	FAvidScriptObjectHandleResult ResolveResult;
	AActor* Actor = Context.ObjectRegistry->ResolveObject<AActor>(Handle, ResolveResult, false);
	if (Actor == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			ResolveResult.ErrorMessage);
		return false;
	}
	if (Actor->GetWorld() != World)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_cross_world"),
			Plan.DebugPath,
			TEXT("The Actor handle belongs to a different World than the active Runtime Session."));
		return false;
	}

	if (Plan.Kind == EAvidScriptBindingInvocationKind::ObjectDestroyActor)
	{
		if (Handle == Context.OwnerHandle)
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_destroy_owner_unsupported"),
				Plan.DebugPath,
				TEXT("Destroying the Runtime owner requires a deferred component shutdown path."));
			return false;
		}
		if (!Actor->Destroy())
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_destroy_failed"),
				Plan.DebugPath,
				TEXT("AActor::Destroy rejected the lifecycle request; the handle remains live."));
			return false;
		}

		FAvidScriptObjectHandleResult ReleaseResult;
		if (!Context.ObjectRegistry->ReleaseHandle(Handle, ReleaseResult, false))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("binding_handle_release_failed"),
				Plan.DebugPath,
				ReleaseResult.ErrorMessage);
			return false;
		}

		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}

	UClass* Class = nullptr;
	if (!ResolveAvidScriptLifecycleClass(Package, Call.Arguments[2], Plan.DebugPath, Class, OutResult))
	{
		return false;
	}
	OutResult.bSucceeded = true;
	OutResult.ReturnValue = Actor->IsA(Class) ? 1 : 0;
	return true;
}

bool DispatchAvidScriptObjectType(
	const FAvidScriptBindingPackage& Package,
	const FAvidScriptRuntimeBindingInvocationPlan& Plan,
	const FAvidScriptDynamicHostCall& Call,
	const FAvidScriptBindingInvocationContext& Context,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	if (Context.ObjectRegistry == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_object_registry_missing"),
			Plan.DebugPath,
			TEXT("The Runtime Session has no object registry for object type handles."));
		return false;
	}
	if (Call.Arguments[0] > MAX_uint32
		|| Call.Arguments[1] > MAX_uint32
		|| Call.Arguments[2] > MAX_uint32)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			TEXT("The object handle or object type ordinal exceeds the 32-bit ABI."));
		return false;
	}

	const FAvidScriptObjectHandle Handle{
		static_cast<uint32>(Call.Arguments[0]),
		static_cast<uint32>(Call.Arguments[1])
	};
	FAvidScriptObjectHandleResult ResolveResult;
	UObject* Object = Context.ObjectRegistry->ResolveObject(Handle, ResolveResult, false);
	if (Object == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			ResolveResult.ErrorMessage);
		return false;
	}

	UClass* CachedClass = nullptr;
	if (!Package.TryResolveObjectType(static_cast<uint32>(Call.Arguments[2]), CachedClass))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("object_type_ordinal_out_of_range"),
			Plan.DebugPath,
			TEXT("The object type ordinal is outside the immutable package type plan."));
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ReturnValue =
		UE::AvidScript::BindingPrivate::IsObjectCompatibleWithReflectedType(
			Object,
			CachedClass) ? 1 : 0;
	return true;
}

bool DispatchAvidScriptObjectFactory(
	const FAvidScriptBindingPackage& Package,
	const FAvidScriptRuntimeBindingInvocationPlan& Plan,
	const FAvidScriptDynamicHostCall& Call,
	const FAvidScriptBindingInvocationContext& Context,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	if (Plan.bRequiresWriteAccess
		&& Context.WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_write_denied"),
			Plan.DebugPath,
			TEXT("The object factory operation requires an explicitly writable host context."));
		return false;
	}
	if (Context.HostEffectJournal != nullptr
		&& Plan.Kind != EAvidScriptBindingInvocationKind::ActorFindComponent)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_reload_effect_unsupported"),
			Plan.DebugPath,
			TEXT("Candidate reload cannot roll back Construct or Release side effects."));
		return false;
	}
	if (Context.ObjectRegistry == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_object_registry_missing"),
			Plan.DebugPath,
			TEXT("The Runtime Session has no object registry for factory handles."));
		return false;
	}
	if (Context.ObjectOwnership == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_object_ownership_missing"),
			Plan.DebugPath,
			TEXT("The Runtime Session has no object ownership domain."));
		return false;
	}
	if (Call.Arguments.ContainsByPredicate(
		[](const uint64 Argument)
		{
			return Argument > MAX_uint32;
		}))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_argument_invalid"),
			Plan.DebugPath,
			TEXT("A factory ordinal or object handle cell exceeds the 32-bit ABI."));
		return false;
	}

	FAvidScriptObjectHandleResult BindingResult;
	bool bSucceeded = false;
	switch (Plan.Kind)
	{
	case EAvidScriptBindingInvocationKind::ObjectConstruct:
	{
		const FAvidScriptObjectFactoryPlan* FactoryPlan = nullptr;
		if (!Package.TryResolveObjectFactory(
			static_cast<uint32>(Call.Arguments[0]),
			FactoryPlan))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("object_factory_ordinal_out_of_range"),
				Plan.DebugPath,
				TEXT("The factory ordinal is outside the immutable package factory plan."));
			return false;
		}
		const FAvidScriptObjectHandle OuterHandle{
			static_cast<uint32>(Call.Arguments[1]),
			static_cast<uint32>(Call.Arguments[2])
		};
		bSucceeded = FAvidScriptObjectFactoryBinding::Construct(
			*FactoryPlan,
			*Context.ObjectRegistry,
			*Context.ObjectOwnership,
			OuterHandle,
			BindingResult);
		break;
	}
	case EAvidScriptBindingInvocationKind::ObjectRelease:
	{
		const FAvidScriptObjectHandle Handle{
			static_cast<uint32>(Call.Arguments[0]),
			static_cast<uint32>(Call.Arguments[1])
		};
		bSucceeded = FAvidScriptObjectFactoryBinding::Release(
			*Context.ObjectRegistry,
			*Context.ObjectOwnership,
			Handle,
			BindingResult);
		break;
	}
	case EAvidScriptBindingInvocationKind::ActorFindComponent:
	{
		UClass* ComponentClass = nullptr;
		if (!Package.TryResolveObjectType(
			static_cast<uint32>(Call.Arguments[2]),
			ComponentClass))
		{
			SetAvidScriptBindingDispatchFailure(
				OutResult,
				TEXT("object_type_ordinal_out_of_range"),
				Plan.DebugPath,
				TEXT("The object type ordinal is outside the immutable package type plan."));
			return false;
		}
		const FAvidScriptObjectHandle ActorHandle{
			static_cast<uint32>(Call.Arguments[0]),
			static_cast<uint32>(Call.Arguments[1])
		};
		bSucceeded = FAvidScriptObjectFactoryBinding::FindComponent(
			*Context.ObjectRegistry,
			*Context.ObjectOwnership,
			ActorHandle,
			*ComponentClass,
			BindingResult);
		break;
	}
	default:
		checkNoEntry();
		break;
	}

	if (!bSucceeded || !BindingResult.bSucceeded)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			BindingResult.ErrorCategory.IsEmpty()
				? FString(TEXT("binding_object_factory_failed"))
				: BindingResult.ErrorCategory,
			Plan.DebugPath,
			BindingResult.ErrorMessage.IsEmpty()
				? FString(TEXT("The object factory binding rejected the operation."))
				: BindingResult.ErrorMessage);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ReturnValue = 1;
	OutResult.ReturnValueI64 = static_cast<int64>(BindingResult.Handle.ToUInt64());
	return true;
}

bool DispatchAvidScriptSceneAttachment(
	const FAvidScriptRuntimeBindingInvocationPlan& Plan,
	const FAvidScriptDynamicHostCall& Call,
	const FAvidScriptBindingInvocationContext& Context,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	if (Plan.bRequiresWriteAccess
		&& Context.WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_write_denied"),
			Plan.DebugPath,
			TEXT("Scene attachment requires an explicitly writable host context."));
		return false;
	}
	if (Context.HostEffectJournal != nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_reload_effect_unsupported"),
			Plan.DebugPath,
			TEXT("Candidate reload cannot roll back Attach or Detach side effects."));
		return false;
	}
	if (Context.ObjectRegistry == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_object_registry_missing"),
			Plan.DebugPath,
			TEXT("The Runtime Session has no object registry for scene component handles."));
		return false;
	}
	if (Call.Arguments.ContainsByPredicate(
		[](const uint64 Argument)
		{
			return Argument > MAX_uint32;
		}))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_argument_invalid"),
			Plan.DebugPath,
			TEXT("A scene component handle cell or rules field exceeds the 32-bit ABI."));
		return false;
	}

	const FAvidScriptObjectHandle ChildHandle{
		static_cast<uint32>(Call.Arguments[0]),
		static_cast<uint32>(Call.Arguments[1])
	};
	FAvidScriptObjectHandleResult BindingResult;
	bool bSucceeded = false;
	switch (Plan.Kind)
	{
	case EAvidScriptBindingInvocationKind::SceneComponentAttach:
	{
		const FAvidScriptObjectHandle ParentHandle{
			static_cast<uint32>(Call.Arguments[2]),
			static_cast<uint32>(Call.Arguments[3])
		};
		bSucceeded = FAvidScriptSceneAttachmentBinding::Attach(
			*Context.ObjectRegistry,
			ChildHandle,
			ParentHandle,
			static_cast<uint32>(Call.Arguments[4]),
			BindingResult);
		break;
	}
	case EAvidScriptBindingInvocationKind::SceneComponentDetach:
		bSucceeded = FAvidScriptSceneAttachmentBinding::Detach(
			*Context.ObjectRegistry,
			ChildHandle,
			static_cast<uint32>(Call.Arguments[2]),
			BindingResult);
		break;
	default:
		checkNoEntry();
		break;
	}

	if (!bSucceeded || !BindingResult.bSucceeded)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			BindingResult.ErrorCategory.IsEmpty()
				? FString(TEXT("binding_scene_attachment_failed"))
				: BindingResult.ErrorCategory,
			Plan.DebugPath,
			BindingResult.ErrorMessage.IsEmpty()
				? FString(TEXT("The scene attachment binding rejected the operation."))
				: BindingResult.ErrorMessage);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ReturnValue = 1;
	return true;
}
} // namespace

struct FAvidScriptBindingPackage::FImpl
{
	struct FClassReferencePlan
	{
		UClass* Class = nullptr;
		UClass* BaseClass = nullptr;
	};

	FString PackageName;
	FString PackageHash;
	int32 DescriptorSchemaVersion = 0;
	FAvidScriptVmBindingPackage VmPackage;
	TArray<FAvidScriptBindingTypeModel> DescriptorTypes;
	TMap<FString, const FAvidScriptBindingTypeModel*> DescriptorTypesById;
	TMap<const FProperty*, FAvidScriptRuntimeBindingValuePlan>
		CompositeAccessPlansByProperty;
	TArray<FAvidScriptRuntimeBindingInvocationPlan> Plans;
	TArray<TOptional<FAvidScriptBindingTypeModel>> LatentResultTypes;
	TArray<TUniquePtr<
		UE::AvidScript::BindingPrivate::FPreparedDynamicInvocationCell>>
		PreparedDynamicCells;
	TArray<TUniquePtr<FAvidScriptPreparedDelegateEventCell>>
		PreparedDelegateEventCells;
	TArray<UClass*> ObjectTypePlans;
	UClass* ExpectedSelfClass = nullptr;
	TArray<FClassReferencePlan> ClassReferencePlans;
	TArray<FAvidScriptObjectFactoryPlan> ObjectFactoryPlans;
	TArray<TStrongObjectPtr<UClass>> LoadedClasses;
	FAvidScriptBindingPackageInstrumentation Instrumentation;
	int32 RequiredScratchSize = 0;
};

FAvidScriptBindingPackage::FAvidScriptBindingPackage()
	: Impl(MakeUnique<FImpl>())
{
}

FAvidScriptBindingPackage::~FAvidScriptBindingPackage() = default;

#if WITH_DEV_AUTOMATION_TESTS
TSharedPtr<const FAvidScriptBindingPackage>
FAvidScriptBindingPackage::MakeGeneratedPlanForTesting(
	const FString& PackageHash,
	const FString& StableId,
	const FString& DescriptorIdentity,
	const EAvidScriptGeneratedBindingShape Shape,
	UClass* ExpectedClass,
	FProperty* ReflectedProperty,
	const bool bPropertyWrite,
	const bool bRequiresWriteAccess,
	const EAvidScriptBindingReloadEffect ReloadEffect)
{
	TSharedPtr<FAvidScriptBindingPackage> Package =
		MakeShareable(new FAvidScriptBindingPackage());
	Package->Impl->PackageHash = PackageHash;
	FAvidScriptRuntimeBindingInvocationPlan Plan;
	Plan.OwnerClass = ExpectedClass;
	Plan.ReflectedProperty = ReflectedProperty;
	Plan.Kind = bPropertyWrite
		? EAvidScriptBindingInvocationKind::ReflectedPropertyWrite
		: EAvidScriptBindingInvocationKind::ReflectedFunction;
	Plan.bRequiresWriteAccess = bRequiresWriteAccess;
	Plan.ReloadEffect = ReloadEffect;
	FString Error;
	if (!FAvidScriptGeneratedBindingRegistry::Get().Acquire(
			PackageHash,
			StableId,
			DescriptorIdentity,
			Shape,
			Plan.GeneratedLease,
			Error))
	{
		return nullptr;
	}
	Plan.GeneratedEntry = Plan.GeneratedLease.GetEntry();
	Plan.TypedHostImport.StableId = StableId;
	Plan.TypedHostImport.BindingOrdinal = 0;
	Plan.TypedHostImport.ModuleName = TEXT("avidscript");
	Plan.TypedHostImport.ImportName = TEXT("avid_s1_test_fixture");
	switch (Shape)
	{
	case EAvidScriptGeneratedBindingShape::I32PairToI32:
		Plan.TypedHostImport.Shape =
			EAvidScriptVmTypedHostShape::SelfI32PairToI32;
		Plan.TypedHostImport.Signature = TEXT("(iiii)i");
		break;
	case EAvidScriptGeneratedBindingShape::PropertyI32GetSet:
		Plan.TypedHostImport.Shape =
			EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet;
		Plan.TypedHostImport.Signature = TEXT("(iii)i");
		break;
	case EAvidScriptGeneratedBindingShape::PropertyI32Get:
		Plan.TypedHostImport.Shape =
			EAvidScriptVmTypedHostShape::SelfPropertyI32Get;
		Plan.TypedHostImport.Signature = TEXT("(ii)i");
		break;
	case EAvidScriptGeneratedBindingShape::PropertyI32Set:
		Plan.TypedHostImport.Shape =
			EAvidScriptVmTypedHostShape::SelfPropertyI32Set;
		Plan.TypedHostImport.Signature = TEXT("(iii)i");
		break;
	case EAvidScriptGeneratedBindingShape::VectorValue:
		Plan.TypedHostImport.Shape =
			EAvidScriptVmTypedHostShape::SelfVectorValue;
		Plan.TypedHostImport.Signature = TEXT("(iii)i");
		break;
	case EAvidScriptGeneratedBindingShape::StableObjectRoundtrip:
		Plan.TypedHostImport.Shape =
			EAvidScriptVmTypedHostShape::StableObjectRoundtrip;
		Plan.TypedHostImport.Signature = TEXT("(iiiii)i");
		break;
	default:
		return nullptr;
	}
	Package->Impl->Instrumentation.GeneratedNativeS1PlanCount = 1;
	Package->Impl->Plans.Add(MoveTemp(Plan));
	return Package;
}

TSharedPtr<const FAvidScriptBindingPackage>
FAvidScriptBindingPackage::MakePreparedDynamicPlanForTesting(
	const FString& StableId,
	const FString& ImportName,
	const FString& Signature,
	UClass* ExpectedClass,
	UFunction* Function,
	const int32 ExpectedArgumentCount,
	const int32 RequiredScratchSize,
	const bool bRequiresGuestMemory,
	const bool bStatic)
{
	if (StableId.IsEmpty()
		|| ImportName.IsEmpty()
		|| Signature.IsEmpty()
		|| ExpectedClass == nullptr
		|| Function == nullptr
		|| ExpectedArgumentCount < 0
		|| RequiredScratchSize < 0)
	{
		return nullptr;
	}

	TSharedPtr<FAvidScriptBindingPackage> Package =
		MakeShareable(new FAvidScriptBindingPackage());
	Package->Impl->PackageName = TEXT("avidscript.test.prepared_dynamic");
	Package->Impl->PackageHash = StableId;
	Package->Impl->VmPackage.PackageName = Package->Impl->PackageName;
	Package->Impl->VmPackage.PackageHash = Package->Impl->PackageHash;
	FAvidScriptRuntimeBindingInvocationPlan& Program =
		Package->Impl->Plans.AddDefaulted_GetRef();
	Program.Kind = EAvidScriptBindingInvocationKind::ReflectedFunction;
	Program.OwnerClass = ExpectedClass;
	Program.Function = Function;
	Program.DebugPath = Function->GetPathName();
	Program.bStatic = bStatic;
	Program.bRequiresGuestMemory = bRequiresGuestMemory;
	Program.ExpectedArgumentCount = ExpectedArgumentCount;
	Program.RequiredScratchSize = RequiredScratchSize;
	Package->Impl->RequiredScratchSize = RequiredScratchSize;
	Package->Impl->VmPackage.Imports.Add({
		StableId,
		0,
		TEXT("avidscript"),
		ImportName,
		Signature
	});
	TUniquePtr<
		UE::AvidScript::BindingPrivate::FPreparedDynamicInvocationCell> Cell =
		MakeUnique<
			UE::AvidScript::BindingPrivate::FPreparedDynamicInvocationCell>();
	Cell->Program = &Program;
	Cell->BindingOrdinal = 0;
	Package->Impl->PreparedDynamicCells.Add(MoveTemp(Cell));
	return Package;
}
#endif

bool FAvidScriptBindingPackage::LoadDescriptor(
	const FString& DescriptorJson,
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FAvidScriptBindingPackageLoadResult& OutResult)
{
	OutPackage.Reset();
	OutResult = FAvidScriptBindingPackageLoadResult();
	FAvidScriptBindingPackageModel Model;
	FString ErrorCategory;
	FString ErrorSource;
	if (!FAvidScriptBindingDescriptorParser::Parse(
		DescriptorJson,
		Model,
		ErrorCategory,
		ErrorSource))
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			ErrorCategory,
			ErrorSource,
			TEXT("The binding descriptor failed its shared schema contract."));
		return false;
	}

	const FString CurrentEngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	if (Model.EngineVersion != CurrentEngineVersion)
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("binding_engine_mismatch"),
			Model.EngineVersion,
			FString::Printf(TEXT("Expected UE %s."), *CurrentEngineVersion));
		return false;
	}
	if (Model.SelectionHash != MakeAvidScriptRuntimeSelectionHash(Model)
		|| Model.PackageHash != MakeAvidScriptRuntimePackageHash(Model))
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("binding_package_hash_mismatch"),
			Model.PackageName,
			TEXT("Selection or package identity does not match the descriptor contents."));
		return false;
	}
	TMap<FString, const FAvidScriptBindingTypeModel*> DeclaredTypesById;
	TMap<FString, const FAvidScriptBindingTypeModel*> DeclaredTypesByClassPath;
	DeclaredTypesById.Reserve(Model.Types.Num());
	DeclaredTypesByClassPath.Reserve(Model.Types.Num());
	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		DeclaredTypesById.Add(Type.StableId, &Type);
		if (!Type.ClassPath.IsEmpty())
		{
			DeclaredTypesByClassPath.Add(Type.ClassPath, &Type);
		}
	}

	TSharedPtr<FAvidScriptBindingPackage> Package = MakeShareable(new FAvidScriptBindingPackage());
	Package->Impl->PackageName = Model.PackageName;
	Package->Impl->PackageHash = Model.PackageHash;
	Package->Impl->DescriptorSchemaVersion = Model.SchemaVersion;
	Package->Impl->VmPackage.PackageName = Model.PackageName;
	Package->Impl->VmPackage.PackageHash = Model.PackageHash;
	Package->Impl->DescriptorTypes = Model.Types;
	Package->Impl->DescriptorTypesById.Reserve(
		Package->Impl->DescriptorTypes.Num());
	for (const FAvidScriptBindingTypeModel& Type :
		Package->Impl->DescriptorTypes)
	{
		Package->Impl->DescriptorTypesById.Add(Type.StableId, &Type);
	}
	Package->Impl->LatentResultTypes.SetNum(Model.Bindings.Num());
	for (const FAvidScriptBindingFunctionModel& Binding : Model.Bindings)
	{
		if (Binding.Completion.Mode != TEXT("provider"))
		{
			continue;
		}
		const FAvidScriptBindingTypeModel* const PayloadType =
			DeclaredTypesById.FindRef(Binding.Completion.PayloadTypeId);
		if (PayloadType == nullptr
			|| !Package->Impl->LatentResultTypes.IsValidIndex(Binding.Ordinal))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_latent_completion_type_missing"),
				Binding.CanonicalIdentity,
				TEXT("The provider result type is not available at its frozen binding ordinal."));
			return false;
		}
		Package->Impl->LatentResultTypes[Binding.Ordinal].Emplace(*PayloadType);
	}
	int32 ObjectTypeCount = 0;
	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		if (Type.ObjectTypeOrdinal != INDEX_NONE)
		{
			++ObjectTypeCount;
		}
	}
	const int32 ObjectTypeBindingCount = ObjectTypeCount == 0
		? 0
		: FAvidScriptObjectTypeBindings::GetSpecs().Num();
	Package->Impl->ObjectTypePlans.SetNumZeroed(ObjectTypeCount);
	Package->Impl->ClassReferencePlans.Reserve(Model.ClassReferences.Num());
	Package->Impl->ObjectFactoryPlans.SetNum(Model.ObjectFactories.Num());

	TMap<FString, UClass*> LoadedClassesByPath;
	TSet<FString> AttemptedClassPaths;
	const auto LoadClass = [&LoadedClassesByPath, &AttemptedClassPaths, &Package](
		const FString& ClassPath) -> UClass*
	{
		if (AttemptedClassPaths.Contains(ClassPath))
		{
			return LoadedClassesByPath.FindRef(ClassPath);
		}
		AttemptedClassPaths.Add(ClassPath);
		++Package->Impl->Instrumentation.ClassLoadCount;
		UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath);
		if (LoadedClass != nullptr && LoadedClass->GetPathName() == ClassPath)
		{
			LoadedClassesByPath.Add(ClassPath, LoadedClass);
			Package->Impl->LoadedClasses.Emplace(LoadedClass);
			return LoadedClass;
		}
		LoadedClassesByPath.Add(ClassPath, nullptr);
		return nullptr;
	};

	TMap<FString, int32> ObjectTypeOrdinalsById;
	TMap<FString, int32> ObjectTypeOrdinalsByClassPath;
	TMap<int32, const FAvidScriptBindingTypeModel*> ObjectTypesByOrdinal;
	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		if (Type.ObjectTypeOrdinal == INDEX_NONE)
		{
			continue;
		}
		ObjectTypeOrdinalsById.Add(Type.StableId, Type.ObjectTypeOrdinal);
		ObjectTypeOrdinalsByClassPath.Add(Type.ClassPath, Type.ObjectTypeOrdinal);
		ObjectTypesByOrdinal.Add(Type.ObjectTypeOrdinal, &Type);
	}

	TSet<FString> ActiveObjectTypeIds;
	TArray<FString> PendingObjectTypeIds;
	FString MissingRequiredTypeId;
	const auto RequireObjectType = [
		&DeclaredTypesById,
		&ActiveObjectTypeIds,
		&PendingObjectTypeIds,
		&MissingRequiredTypeId](const FString& TypeId)
	{
		if (TypeId.IsEmpty() || !MissingRequiredTypeId.IsEmpty())
		{
			return;
		}
		const FAvidScriptBindingTypeModel* Type = DeclaredTypesById.FindRef(TypeId);
		if (Type == nullptr)
		{
			MissingRequiredTypeId = TypeId;
			return;
		}
		if (Type->ObjectTypeOrdinal != INDEX_NONE
			&& !ActiveObjectTypeIds.Contains(TypeId))
		{
			ActiveObjectTypeIds.Add(TypeId);
			PendingObjectTypeIds.Add(TypeId);
		}
	};

	if (Model.bHasActiveObjectTypeOrdinals)
	{
		for (const int32 Ordinal : Model.ActiveObjectTypeOrdinals)
		{
			const FAvidScriptBindingTypeModel* Type =
				ObjectTypesByOrdinal.FindRef(Ordinal);
			if (Type != nullptr)
			{
				RequireObjectType(Type->StableId);
			}
		}
	}
	else
	{
		for (const TPair<int32, const FAvidScriptBindingTypeModel*>& Pair :
			ObjectTypesByOrdinal)
		{
			RequireObjectType(Pair.Value->StableId);
		}
	}
	RequireObjectType(Model.SelfTypeId);
	for (const FAvidScriptBindingClassReferenceModel& Reference : Model.ClassReferences)
	{
		RequireObjectType(Reference.ResultTypeId);
	}
	for (const FAvidScriptBindingObjectFactoryModel& Factory : Model.ObjectFactories)
	{
		RequireObjectType(Factory.OuterTypeId);
		const FAvidScriptBindingClassReferenceModel* Reference =
			Model.ClassReferences.FindByPredicate(
				[&Factory](const FAvidScriptBindingClassReferenceModel& Candidate)
				{
					return Candidate.StableId == Factory.ClassReferenceId;
				});
		const FAvidScriptBindingTypeModel* ConcreteType = Reference == nullptr
			? nullptr
			: DeclaredTypesByClassPath.FindRef(Reference->ClassPath);
		if (ConcreteType == nullptr)
		{
			MissingRequiredTypeId = Reference == nullptr
				? Factory.ClassReferenceId
				: Reference->ClassPath;
		}
		else
		{
			RequireObjectType(ConcreteType->StableId);
		}
	}
	for (const FAvidScriptBindingFunctionModel& Binding : Model.Bindings)
	{
		if (const FAvidScriptBindingTypeModel* OwnerType =
			DeclaredTypesByClassPath.FindRef(Binding.OwnerClass))
		{
			RequireObjectType(OwnerType->StableId);
		}
		if (Binding.ReturnValue.Kind == TEXT("object_handle"))
		{
			RequireObjectType(Binding.ReturnValue.TypeId);
		}
		for (const FAvidScriptBindingValueModel& Parameter : Binding.Parameters)
		{
			if (Parameter.Kind == TEXT("object_handle"))
			{
				RequireObjectType(Parameter.TypeId);
			}
		}
	}
	for (const FAvidScriptBindingDelegateEventModel& Event : Model.DelegateEvents)
	{
		if (const FAvidScriptBindingTypeModel* OwnerType =
			DeclaredTypesByClassPath.FindRef(Event.OwnerClass))
		{
			RequireObjectType(OwnerType->StableId);
		}
		for (const FAvidScriptBindingValueModel& Parameter : Event.Parameters)
		{
			if (Parameter.Kind == TEXT("object_handle"))
			{
				RequireObjectType(Parameter.TypeId);
			}
		}
	}
	while (!PendingObjectTypeIds.IsEmpty())
	{
		const FString TypeId = PendingObjectTypeIds.Pop(EAllowShrinking::No);
		const FAvidScriptBindingTypeModel* Type = DeclaredTypesById.FindRef(TypeId);
		if (Type != nullptr)
		{
			RequireObjectType(Type->BaseTypeId);
		}
	}
	if (!MissingRequiredTypeId.IsEmpty())
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("binding_object_type_required_missing"),
			MissingRequiredTypeId,
			TEXT("A selected binding capability references a type outside the descriptor type graph."));
		return false;
	}

	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		if (Type.ObjectTypeOrdinal == INDEX_NONE
			|| !ActiveObjectTypeIds.Contains(Type.StableId))
		{
			continue;
		}
		UClass* ObjectClass = LoadClass(Type.ClassPath);
		if (ObjectClass == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_object_type_class_missing"),
				Type.ClassPath,
				TEXT("A v6 object type class is unavailable or does not keep its canonical path."));
			return false;
		}
		Package->Impl->ObjectTypePlans[Type.ObjectTypeOrdinal] = ObjectClass;
	}
	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		if (Type.ObjectTypeOrdinal == INDEX_NONE
			|| !ActiveObjectTypeIds.Contains(Type.StableId))
		{
			continue;
		}
		UClass* ObjectClass = Package->Impl->ObjectTypePlans[Type.ObjectTypeOrdinal];
		UClass* ExpectedBaseClass = nullptr;
		if (!Type.BaseTypeId.IsEmpty())
		{
			const int32* BaseOrdinal = ObjectTypeOrdinalsById.Find(Type.BaseTypeId);
			ExpectedBaseClass = BaseOrdinal == nullptr
				? nullptr
				: Package->Impl->ObjectTypePlans[*BaseOrdinal];
		}
		if (ObjectClass == nullptr
			|| ObjectClass->GetSuperClass() != ExpectedBaseClass)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_object_type_base_mismatch"),
				Type.ClassPath,
				TEXT("The v6 base_type_id must match the reflected direct superclass."));
			return false;
		}
	}
	if (!Model.SelfTypeId.IsEmpty())
	{
		const int32* SelfOrdinal = ObjectTypeOrdinalsById.Find(Model.SelfTypeId);
		if (SelfOrdinal == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_self_type_missing"),
				Model.SelfTypeId,
				TEXT("The v6 Self type must resolve through the immutable object type plan."));
			return false;
		}
		Package->Impl->ExpectedSelfClass = Package->Impl->ObjectTypePlans[*SelfOrdinal];
		if (Package->Impl->ExpectedSelfClass == nullptr
			|| !Package->Impl->ExpectedSelfClass->IsChildOf(AActor::StaticClass()))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_self_type_mismatch"),
				Model.SelfTypeId,
				TEXT("The v6 Self type must resolve to an Actor-derived class."));
			return false;
		}
	}

	TSet<FString> FactoryClassReferenceIds;
	for (const FAvidScriptBindingObjectFactoryModel& Factory : Model.ObjectFactories)
	{
		FactoryClassReferenceIds.Add(Factory.ClassReferenceId);
	}
	TMap<FString, UClass*> FactoryClassesByReferenceId;
	bool bHasLifecycleClassReferences = false;
	if (Model.SchemaVersion >= 7)
	{
		Package->Impl->ClassReferencePlans.SetNum(Model.ClassReferences.Num());
	}

	for (const FAvidScriptBindingClassReferenceModel& Reference : Model.ClassReferences)
	{
#if !WITH_EDITOR
		if (Reference.LoadPolicy == TEXT("EditorLoad"))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_class_editor_only"),
				Reference.ClassPath,
				TEXT("EditorLoad class references cannot be activated outside an Editor target."));
			return false;
		}
#endif
		const bool bFactoryClassReference =
			FactoryClassReferenceIds.Contains(Reference.StableId);
		const bool bActorLifecycleReference =
			Model.SchemaVersion < 6
			|| FAvidScriptBindingDescriptorTypeGraph::IsDerivedFromClassPath(
				Model,
				Reference.ResultTypeId,
				TEXT("/Script/Engine.Actor"));
		if (bFactoryClassReference == bActorLifecycleReference)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_class_capability_missing"),
				Reference.StableId,
				TEXT("Each class reference must expose exactly one of Actor lifecycle or object factory capability."));
			return false;
		}
		if (bFactoryClassReference)
		{
			UClass* Class = LoadClass(Reference.ClassPath);
			const int32* BaseOrdinal =
				ObjectTypeOrdinalsById.Find(Reference.ResultTypeId);
			UClass* BaseClass = BaseOrdinal == nullptr
				? nullptr
				: Package->Impl->ObjectTypePlans[*BaseOrdinal];
			if (Class == nullptr || BaseClass == nullptr)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					Reference.LoadPolicy == TEXT("CookRequired")
						? FString(TEXT("binding_class_cook_missing"))
						: FString(TEXT("binding_class_missing")),
					Reference.ClassPath,
					TEXT("The factory class is unavailable under the declared load policy."));
				return false;
			}
			if (!Class->IsChildOf(BaseClass))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_factory_class_inheritance_mismatch"),
					Reference.ClassPath + TEXT(" -> ")
						+ Reference.BaseClassPath,
					TEXT("The factory class must satisfy its declared base constraint."));
				return false;
			}
			FactoryClassesByReferenceId.Add(Reference.StableId, Class);
			continue;
		}

		UClass* Class = LoadClass(Reference.ClassPath);
		UClass* BaseClass = nullptr;
		if (Model.SchemaVersion >= 6)
		{
			const int32* ResultOrdinal = ObjectTypeOrdinalsById.Find(Reference.ResultTypeId);
			BaseClass = ResultOrdinal == nullptr
				? nullptr
				: Package->Impl->ObjectTypePlans[*ResultOrdinal];
		}
		else
		{
			BaseClass = LoadClass(Reference.BaseClassPath);
		}
		if (Class == nullptr || BaseClass == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				Reference.LoadPolicy == TEXT("CookRequired")
					? FString(TEXT("binding_class_cook_missing"))
					: FString(TEXT("binding_class_missing")),
				Class == nullptr ? Reference.ClassPath : Reference.BaseClassPath,
				TEXT("The class reference or its base constraint is unavailable under the declared load policy."));
			return false;
		}
		if (!Class->IsChildOf(BaseClass)
			|| !Class->IsChildOf(AActor::StaticClass())
			|| !BaseClass->IsChildOf(AActor::StaticClass()))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_class_inheritance_mismatch"),
				Reference.ClassPath + TEXT(" -> ") + Reference.BaseClassPath,
				TEXT("The resolved class must satisfy its AActor-derived base constraint."));
			return false;
		}
		if (Class->HasAnyClassFlags(
			CLASS_Abstract | CLASS_NotPlaceable | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_class_not_spawnable"),
				Reference.ClassPath,
				TEXT("The resolved class is abstract, deprecated, superseded, or not placeable."));
			return false;
		}
		if (Model.SchemaVersion >= 7)
		{
			Package->Impl->ClassReferencePlans[Reference.Ordinal] = { Class, BaseClass };
		}
		else
		{
			Package->Impl->ClassReferencePlans.Add({ Class, BaseClass });
		}
		bHasLifecycleClassReferences = true;
	}

	for (const FAvidScriptBindingObjectFactoryModel& Factory : Model.ObjectFactories)
	{
		UClass* ObjectClass = FactoryClassesByReferenceId.FindRef(Factory.ClassReferenceId);
		const int32* OuterOrdinal = ObjectTypeOrdinalsById.Find(Factory.OuterTypeId);
		const int32* ResultOrdinal = ObjectClass == nullptr
			? nullptr
			: ObjectTypeOrdinalsByClassPath.Find(ObjectClass->GetPathName());
		if (ObjectClass == nullptr || OuterOrdinal == nullptr || ResultOrdinal == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_result_type_missing"),
				Factory.StableId,
				TEXT("The factory class, required Outer, or concrete result type is missing from the immutable plan."));
			return false;
		}

		UClass* RequiredOuterClass = Package->Impl->ObjectTypePlans[*OuterOrdinal];
		if (RequiredOuterClass == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_outer_mismatch"),
				Factory.StableId,
				TEXT("The factory required Outer is unavailable from the immutable object type plan."));
			return false;
		}
		if (ObjectClass->HasAnyClassFlags(CLASS_Abstract))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_class_abstract"),
				ObjectClass->GetPathName(),
				TEXT("Factory classes must be concrete."));
			return false;
		}
		if (ObjectClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_class_deprecated"),
				ObjectClass->GetPathName(),
				TEXT("Factory classes cannot be deprecated or superseded."));
			return false;
		}

		const bool bActorClass = ObjectClass->IsChildOf(AActor::StaticClass());
		const bool bActorComponentClass = ObjectClass->IsChildOf(UActorComponent::StaticClass());
		const bool bKindMatches =
			(Factory.Kind == EAvidScriptObjectFactoryKind::NewObject
				&& Factory.Registration == EAvidScriptComponentRegistrationPolicy::None
				&& !bActorClass
				&& !bActorComponentClass)
			|| (Factory.Kind == EAvidScriptObjectFactoryKind::ActorComponent
				&& Factory.Registration
					== EAvidScriptComponentRegistrationPolicy::RegisterInstance
				&& bActorComponentClass);
		if (!bKindMatches)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_kind_mismatch"),
				Factory.StableId,
				TEXT("The factory kind, registration policy, and reflected class are incompatible."));
			return false;
		}

		UClass* ClassWithin = ObjectClass->ClassWithin;
		const bool bOuterMatchesClassWithin = ClassWithin != nullptr
			&& RequiredOuterClass->IsChildOf(ClassWithin);
		const bool bComponentOuterMatches = Factory.Kind
			!= EAvidScriptObjectFactoryKind::ActorComponent
			|| RequiredOuterClass->IsChildOf(AActor::StaticClass());
		if (!bOuterMatchesClassWithin || !bComponentOuterMatches)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_factory_outer_mismatch"),
				Factory.StableId,
				TEXT("The factory required Outer does not satisfy the class ClassWithin contract."));
			return false;
		}

		FAvidScriptObjectFactoryPlan& Plan =
			Package->Impl->ObjectFactoryPlans[Factory.Ordinal];
		Plan.Kind = Factory.Kind;
		Plan.ObjectClass = ObjectClass;
		Plan.RequiredOuterClass = RequiredOuterClass;
		Plan.ResultObjectTypeOrdinal = *ResultOrdinal;
		Plan.Ownership = Factory.Ownership;
		Plan.Registration = Factory.Registration;
	}

	Package->Impl->PreparedDelegateEventCells.Reserve(Model.DelegateEvents.Num());
	for (const FAvidScriptBindingDelegateEventModel& Event : Model.DelegateEvents)
	{
		UClass* OwnerClass = LoadClass(Event.OwnerClass);
		if (OwnerClass == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_delegate_class_missing"),
				Event.OwnerClass,
				TEXT("The delegate owner class is unavailable in this runtime build."));
			return false;
		}

		++Package->Impl->Instrumentation.ReflectedNameLookupCount;
		FDelegateProperty* SinglecastProperty = nullptr;
		FMulticastDelegateProperty* MulticastProperty = nullptr;
		UFunction* SignatureFunction = nullptr;
		FProperty* RepNotifyProperty = nullptr;
		FAvidScriptBindingNetworkContract Network;
		bool bNetworkContractValid = true;
		if (Event.DelegateKind == TEXT("multicast"))
		{
			MulticastProperty = FindFProperty<FMulticastDelegateProperty>(
				OwnerClass,
				FName(*Event.UeMember));
			SignatureFunction = MulticastProperty == nullptr
				? nullptr
				: MulticastProperty->SignatureFunction;
		}
		else if (Event.DelegateKind == TEXT("singlecast"))
		{
			SinglecastProperty = FindFProperty<FDelegateProperty>(
				OwnerClass,
				FName(*Event.UeMember));
			SignatureFunction = SinglecastProperty == nullptr
				? nullptr
				: SinglecastProperty->SignatureFunction;
		}
		else
		{
			SignatureFunction = OwnerClass->FindFunctionByName(
				FName(*Event.UeMember),
				EIncludeSuperFlag::ExcludeSuper);
			if (!Event.RepNotifyProperty.IsNone())
			{
				RepNotifyProperty = FindFProperty<FProperty>(
					OwnerClass,
					Event.RepNotifyProperty);
			}
			if (SignatureFunction != nullptr)
			{
				bNetworkContractValid =
					TryResolveAvidScriptBindingNetworkContract(
					*SignatureFunction,
					Network);
			}
		}
		const bool bMulticastValid = Event.DelegateKind == TEXT("multicast")
			&& MulticastProperty != nullptr
			&& MulticastProperty->GetOwnerStruct() == OwnerClass
			&& MulticastProperty->HasAnyPropertyFlags(CPF_BlueprintAssignable)
			&& SignatureFunction != nullptr
			&& SignatureFunction->HasAllFunctionFlags(
				FUNC_Delegate | FUNC_MulticastDelegate);
		const bool bSinglecastValid = Event.DelegateKind == TEXT("singlecast")
			&& SinglecastProperty != nullptr
			&& SinglecastProperty->GetOwnerStruct() == OwnerClass
			&& SignatureFunction != nullptr
			&& SignatureFunction->HasAnyFunctionFlags(FUNC_Delegate)
			&& !SignatureFunction->HasAnyFunctionFlags(FUNC_MulticastDelegate);
		const bool bNetworkRpcValid = Event.DelegateKind == TEXT("network_rpc")
			&& SignatureFunction != nullptr
			&& SignatureFunction->GetOuterUClass() == OwnerClass
			&& (SignatureFunction->HasAnyFunctionFlags(FUNC_Native)
				|| !SignatureFunction->Script.IsEmpty())
			&& !SignatureFunction->HasAnyFunctionFlags(
				FUNC_Static | FUNC_Delegate | FUNC_MulticastDelegate)
			&& !SignatureFunction->HasMetaData(TEXT("Latent"))
			&& IsAvidScriptBindingNetworkOwnerClass(OwnerClass)
			&& bNetworkContractValid
			&& Network == Event.Network
			&& Network.IsNetworked()
			&& RepNotifyProperty == nullptr;
		const bool bRepNotifyValid = Event.DelegateKind == TEXT("rep_notify")
			&& SignatureFunction != nullptr
			&& SignatureFunction->GetOuterUClass() == OwnerClass
			&& (SignatureFunction->HasAnyFunctionFlags(FUNC_Native)
				|| !SignatureFunction->Script.IsEmpty())
			&& !SignatureFunction->HasAnyFunctionFlags(
				FUNC_Static | FUNC_Delegate | FUNC_MulticastDelegate)
			&& !SignatureFunction->HasMetaData(TEXT("Latent"))
			&& IsAvidScriptBindingNetworkOwnerClass(OwnerClass)
			&& bNetworkContractValid
			&& !Network.IsNetworked()
			&& RepNotifyProperty != nullptr
			&& RepNotifyProperty->HasAnyPropertyFlags(CPF_RepNotify)
			&& RepNotifyProperty->RepNotifyFunc
				== SignatureFunction->GetFName();
		if (!bMulticastValid
			&& !bSinglecastValid
			&& !bNetworkRpcValid
			&& !bRepNotifyValid)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_callback_member_missing"),
				Event.OwnerClass + TEXT(".") + Event.UeMember,
				TEXT("The reflected callback member no longer satisfies its singlecast, multicast, RPC, or RepNotify contract."));
			return false;
		}
		if (Model.SchemaVersion < 20
			&& SignatureFunction->GetReturnProperty() != nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_delegate_return_unsupported"),
				Event.CanonicalIdentity,
				TEXT("P57.12A delegate events cannot expose return values."));
			return false;
		}

		TArray<FProperty*> ReflectedParameters;
		for (TFieldIterator<FProperty> It(SignatureFunction); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Parm)
				&& !Property->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReflectedParameters.Add(Property);
			}
		}
		if (ReflectedParameters.Num() != Event.Parameters.Num())
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_delegate_parameter_count_mismatch"),
				Event.CanonicalIdentity,
				TEXT("The reflected delegate parameter count changed since descriptor generation."));
			return false;
		}

		TUniquePtr<FAvidScriptPreparedDelegateEventCell> Cell =
			MakeUnique<FAvidScriptPreparedDelegateEventCell>();
		Cell->EventOrdinal = static_cast<uint32>(Event.Ordinal);
		Cell->StableId = Event.StableId;
		Cell->ExportName = Event.ExportName;
		Cell->CallbackKind = Event.DelegateKind;
		Cell->HandlerMode = Event.HandlerMode;
		Cell->ExpectedSourceClass = OwnerClass;
		Cell->Kind = Event.DelegateKind == TEXT("singlecast")
			? EAvidScriptPreparedDelegateKind::Singlecast
			: Event.DelegateKind == TEXT("multicast")
				? EAvidScriptPreparedDelegateKind::Multicast
				: EAvidScriptPreparedDelegateKind::FunctionHandler;
		Cell->SinglecastProperty = SinglecastProperty;
		Cell->MulticastProperty = MulticastProperty;
		Cell->SignatureFunction = SignatureFunction;
		Cell->RepNotifyProperty = RepNotifyProperty;
		Cell->Network = Network;
		Cell->Codec.StableId = Event.StableId;
		FProperty* const ReflectedReturn = SignatureFunction->GetReturnProperty();
		const bool bDescriptorHasReturn =
			Event.ReturnValue.Kind != TEXT("void");
		if (Model.SchemaVersion >= 20
			&& ((ReflectedReturn != nullptr) != bDescriptorHasReturn))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_delegate_return_contract_mismatch"),
				Event.CanonicalIdentity,
				TEXT("The reflected delegate return presence changed since descriptor generation."));
			return false;
		}
		if (ReflectedReturn != nullptr)
		{
			FString ReturnDetails;
			if (GetAvidScriptRuntimePropertyDirection(ReflectedReturn)
					!= Event.ReturnValue.Direction
				|| !BuildAvidScriptRuntimeValuePlan(
					ReflectedReturn,
					Event.ReturnValue,
					DeclaredTypesById,
					0,
					Cell->Codec.ReturnValue,
					ReturnDetails)
				|| !IsAvidScriptPreparedDelegateValueSupported(
					Cell->Codec.ReturnValue)
				|| Cell->Codec.ReturnValue.Direction
					!= EAvidScriptRuntimeBindingDirection::Return)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_delegate_return_contract_mismatch"),
					Event.CanonicalIdentity,
					ReturnDetails.IsEmpty()
						? TEXT("The reflected delegate return no longer satisfies its fixed-width prepared value contract.")
						: ReturnDetails);
				return false;
			}
			Cell->Codec.bHasReturnValue = true;
			++Cell->Codec.ParameterCellCount;
		}
		Cell->Codec.Parameters.Reserve(Event.Parameters.Num());
		for (int32 Index = 0; Index < Event.Parameters.Num(); ++Index)
		{
			FProperty* Property = ReflectedParameters[Index];
			const FAvidScriptBindingValueModel& Parameter = Event.Parameters[Index];
			if (Property->GetName() != Parameter.Name
				|| GetAvidScriptRuntimePropertyDirection(Property)
					!= Parameter.Direction)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_delegate_parameter_contract_mismatch"),
					Event.CanonicalIdentity + TEXT(":") + Parameter.Name,
					TEXT("A reflected delegate parameter name or direction changed since descriptor generation."));
				return false;
			}

			FAvidScriptRuntimeBindingValuePlan ValuePlan;
			FString Details;
			if (!BuildAvidScriptRuntimeValuePlan(
					Property,
					Parameter,
					DeclaredTypesById,
					0,
					ValuePlan,
					Details))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_delegate_parameter_contract_mismatch"),
					Event.CanonicalIdentity + TEXT(":") + Parameter.Name,
					Details);
				return false;
			}
			if (!IsAvidScriptPreparedDelegateValueSupported(ValuePlan))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_delegate_parameter_unsupported"),
					Event.CanonicalIdentity + TEXT(":") + Parameter.Name,
					TEXT("Prepared delegate events support fixed-width value, const-ref, ref and out parameters; variable-layout values fail closed."));
				return false;
			}
			const bool bOutput = ValuePlan.Direction
				== EAvidScriptRuntimeBindingDirection::Ref
				|| ValuePlan.Direction == EAvidScriptRuntimeBindingDirection::Out;
			if (bOutput
				&& !Cell->Codec.bHasReturnValue
				&& Cell->Codec.OutputParameterIndices.IsEmpty())
			{
				++Cell->Codec.ParameterCellCount;
			}
			if ((ValuePlan.Direction != EAvidScriptRuntimeBindingDirection::Out
					&& !CountAvidScriptPreparedDelegateValueCells(
						ValuePlan,
						Cell->Codec.ParameterCellCount))
				|| Cell->Codec.ParameterCellCount > FAvidScriptVmCallFrame::MaxCells)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_delegate_cell_limit_exceeded"),
					Event.CanonicalIdentity,
					FString::Printf(
						TEXT("The flattened delegate payload exceeds the fixed %u-cell VM call-frame capacity."),
						FAvidScriptVmCallFrame::MaxCells));
				return false;
			}
			if (bOutput)
			{
				Cell->Codec.OutputParameterIndices.Add(Index);
			}
			Cell->Codec.Parameters.Add(MoveTemp(ValuePlan));
		}

		const FString ExpectedIdentity =
			FAvidScriptBindingDescriptorIdentity::MakeDelegateEventCanonicalIdentity(
				OwnerClass->GetPathName(),
				Event.UeMember,
				Event.DelegateKind,
				TEXT("self"),
				Event.Parameters,
				Network,
				RepNotifyProperty == nullptr
					? NAME_None
					: RepNotifyProperty->GetFName(),
				Model.SchemaVersion >= 18
					? Event.HandlerMode
					: FString(),
				Model.SchemaVersion >= 20
					? &Event.ReturnValue
					: nullptr);
		if (Event.CanonicalIdentity != ExpectedIdentity
			|| Event.StableId != FAvidScriptHash::Sha256HexUtf8(ExpectedIdentity))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_delegate_identity_mismatch"),
				Event.CanonicalIdentity,
				TEXT("The delegate descriptor no longer matches the active reflection snapshot."));
			return false;
		}
		Package->Impl->PreparedDelegateEventCells.Add(MoveTemp(Cell));
	}

	const int32 LifecycleBindingCount = bHasLifecycleClassReferences
		? FAvidScriptObjectLifecycleBindings::GetSpecs().Num()
		: 0;
	const int32 ObjectFactoryBindingCount = Model.SchemaVersion >= 7
		&& !Package->Impl->ObjectFactoryPlans.IsEmpty()
		? FAvidScriptObjectFactoryBinding::GetSpecs().Num()
		: 0;
	const bool bHasSceneComponentFactory =
		Model.SchemaVersion >= 7
		&& Package->Impl->ObjectFactoryPlans.ContainsByPredicate(
			[](const FAvidScriptObjectFactoryPlan& Factory)
			{
				return IsValid(Factory.ObjectClass)
					&& Factory.ObjectClass->IsChildOf(
						USceneComponent::StaticClass());
			});
	const int32 SceneAttachmentBindingCount = bHasSceneComponentFactory
		? FAvidScriptSceneAttachmentBinding::GetSpecs().Num()
		: 0;
	int32 DelegateInvokeBindingCount = 0;
	for (const FAvidScriptBindingDelegateEventModel& Event :
		Model.DelegateEvents)
	{
		DelegateInvokeBindingCount += Event.DelegateKind == TEXT("singlecast")
			|| Event.DelegateKind == TEXT("multicast")
			? 1
			: 0;
	}
	const int32 TotalImportCount =
		Model.Bindings.Num()
		+ LifecycleBindingCount
		+ ObjectTypeBindingCount
		+ ObjectFactoryBindingCount
		+ SceneAttachmentBindingCount
		+ DelegateInvokeBindingCount;
	Package->Impl->Plans.Reserve(TotalImportCount);
	Package->Impl->VmPackage.Imports.Reserve(TotalImportCount);
	const auto PublishGeneratedPlan = [&Package](
		FAvidScriptRuntimeBindingInvocationPlan& Plan)
	{
		if (Plan.GeneratedEntry == nullptr)
		{
			return;
		}
		Plan.FastPath.HighestInvocationMode =
			EAvidScriptBindingInvocationMode::GeneratedNativeS1;
		++Package->Impl->Instrumentation.GeneratedNativeS1PlanCount;
	};

	for (const FAvidScriptBindingFunctionModel& Binding : Model.Bindings)
	{
		UClass* OwnerClass = LoadClass(Binding.OwnerClass);
		if (OwnerClass == nullptr)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_class_missing"),
				Binding.OwnerClass,
				TEXT("The reflected owner class is unavailable in this runtime build."));
			return false;
		}
		if (Binding.BindingKind == TEXT("property_set"))
		{
			++Package->Impl->Instrumentation.ReflectedNameLookupCount;
			FProperty* Property = FindFProperty<FProperty>(
				OwnerClass,
				FName(*Binding.UeMember));
			FAvidScriptBindingPropertyReplicationContract RuntimeReplication;
			if (Property == nullptr
				|| !TryResolveAvidScriptBindingPropertyReplicationContract(
					*Property,
					RuntimeReplication)
				|| RuntimeReplication != Binding.PropertyReplication
				|| !IsAvidScriptRuntimePropertyWritable(Property)
				|| Property->GetOwnerStruct() != OwnerClass
				|| (RuntimeReplication.IsReplicated()
					&& (!IsAvidScriptBindingNetworkOwnerClass(OwnerClass)
						|| Binding.DispatchMode == TEXT("generated_native_s1")
						|| (RuntimeReplication.Mode
								== EAvidScriptBindingPropertyReplicationMode::RepNotify
							&& OwnerClass->FindFunctionByName(
								RuntimeReplication.RepNotifyFunction) == nullptr)))
				|| Binding.Parameters.Num() != 1)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_replication_contract_mismatch"),
					Binding.OwnerClass + TEXT(".") + Binding.UeMember,
					TEXT("The reflected property is missing or no longer matches the runtime write and replication contract."));
				return false;
			}

			const FString BlueprintSetterName =
				Property->GetMetaData(TEXT("BlueprintSetter"));
			UFunction* BlueprintSetter = nullptr;
			TArray<FProperty*> SetterParameters;
			if (Binding.UeFunction != BlueprintSetterName)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_blueprint_setter_mismatch"),
					Binding.CanonicalIdentity,
					TEXT("The authorized BlueprintSetter name no longer matches the property metadata."));
				return false;
			}
			if (BlueprintSetterName.IsEmpty())
			{
				const bool bAuthorizedDirectWrite =
					(Binding.DispatchMode == TEXT("cached_property_set")
						|| Binding.DispatchMode
							== TEXT("generated_native_s1"))
					&& Binding.WritePolicy == TEXT("direct");
				if (!bAuthorizedDirectWrite)
				{
					SetAvidScriptBindingLoadFailure(
						OutResult,
						TEXT("binding_property_write_policy_mismatch"),
						Binding.CanonicalIdentity,
						TEXT("Direct reflected property writes require the cached direct policy."));
					return false;
				}
			}
			else
			{
				++Package->Impl->Instrumentation.ReflectedNameLookupCount;
				BlueprintSetter = OwnerClass->FindFunctionByName(
					FName(*Binding.UeFunction));
				if (BlueprintSetter != nullptr)
				{
					for (TFieldIterator<FProperty> It(BlueprintSetter); It; ++It)
					{
						FProperty* Parameter = *It;
						if (Parameter->HasAnyPropertyFlags(CPF_Parm)
							&& !Parameter->HasAnyPropertyFlags(CPF_ReturnParm))
						{
							SetterParameters.Add(Parameter);
						}
					}
				}
				if (Binding.DispatchMode != TEXT("cached_blueprint_setter")
					|| Binding.WritePolicy != TEXT("blueprint_setter")
					|| !IsAvidScriptRuntimeFunctionAllowed(BlueprintSetter)
					|| BlueprintSetter->GetOwnerClass() != OwnerClass
					|| BlueprintSetter->HasAnyFunctionFlags(FUNC_Static)
					|| BlueprintSetter->GetReturnProperty() != nullptr
					|| SetterParameters.Num() != 1)
				{
					SetAvidScriptBindingLoadFailure(
						OutResult,
						TEXT("binding_property_blueprint_setter_mismatch"),
						Binding.CanonicalIdentity,
						TEXT("The cached BlueprintSetter no longer matches the authorized property."));
					return false;
				}
			}

			FAvidScriptRuntimeBindingValuePlan PropertyValuePlan;
			FString ValueDetails;
			if (!BuildAvidScriptRuntimeValuePlan(
					Property,
					Binding.Parameters[0],
					DeclaredTypesById,
					2,
					PropertyValuePlan,
					ValueDetails))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_contract_mismatch"),
					Binding.CanonicalIdentity,
					ValueDetails);
				return false;
			}

			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = EAvidScriptBindingInvocationKind::ReflectedPropertyWrite;
			Plan.OwnerClass = OwnerClass;
			Plan.Function = BlueprintSetter;
			Plan.ReflectedProperty = Property;
			Plan.PropertyReplication = RuntimeReplication;
			Plan.DebugPath = Property->GetPathName();
			Plan.bRequiresWriteAccess = true;
			Plan.ReloadEffect = RuntimeReplication.IsReplicated()
				? EAvidScriptBindingReloadEffect::Unsupported
				: EAvidScriptBindingReloadEffect::ReflectedProperty;
			if (BlueprintSetter != nullptr)
			{
				Plan.DebugPath += TEXT(":") + BlueprintSetter->GetName();
				Plan.FrameSize = BlueprintSetter->GetStructureSize();
				Plan.FrameAlignment = FMath::Max(
					1,
					BlueprintSetter->GetMinAlignment());
				if (Plan.FrameSize < BlueprintSetter->ParmsSize
					|| !FMath::IsPowerOfTwo(Plan.FrameAlignment)
					|| Plan.FrameSize > MAX_int32 - (Plan.FrameAlignment - 1))
				{
					SetAvidScriptBindingLoadFailure(
						OutResult,
						TEXT("binding_frame_layout_invalid"),
						Binding.CanonicalIdentity,
						TEXT("The BlueprintSetter frame size or alignment is invalid."));
					return false;
				}
				Plan.RequiredScratchSize = Plan.FrameSize
					+ Plan.FrameAlignment - 1;
				if (!BuildAvidScriptRuntimeValuePlan(
						SetterParameters[0],
						Binding.Parameters[0],
						DeclaredTypesById,
						2,
						Plan.Parameters.AddDefaulted_GetRef(),
						ValueDetails))
				{
					SetAvidScriptBindingLoadFailure(
						OutResult,
						TEXT("binding_property_blueprint_setter_mismatch"),
						Binding.CanonicalIdentity,
						ValueDetails);
					return false;
				}
			}
			else
			{
				Plan.Parameters.Add(MoveTemp(PropertyValuePlan));
			}

			FString ReturnDetails;
			const int32 ReturnOffset = 2 + Plan.Parameters[0].ArgumentWidth;
			if (!BuildAvidScriptRuntimeValuePlan(
					nullptr,
					Binding.ReturnValue,
					DeclaredTypesById,
					ReturnOffset,
					Plan.ReturnValue,
					ReturnDetails))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_return_contract_mismatch"),
					Binding.CanonicalIdentity,
					ReturnDetails);
				return false;
			}
			Plan.ExpectedArgumentCount = ReturnOffset;
			Plan.bRequiresGuestMemory =
				Plan.Parameters[0].Kind == EAvidScriptRuntimeBindingKind::Name
				|| Plan.Parameters[0].Kind == EAvidScriptRuntimeBindingKind::Array
				|| Plan.Parameters[0].Kind == EAvidScriptRuntimeBindingKind::String
				|| Plan.Parameters[0].Kind == EAvidScriptRuntimeBindingKind::StructWire;
			if (BlueprintSetter == nullptr
				&& Plan.Parameters[0].Kind == EAvidScriptRuntimeBindingKind::StructWire)
			{
				Plan.FrameSize = Plan.Parameters[0].StructType->GetStructureSize();
				Plan.FrameAlignment = FMath::Max(
					1,
					Plan.Parameters[0].StructType->GetMinAlignment());
				Plan.RequiredScratchSize = Plan.FrameSize + Plan.FrameAlignment - 1;
			}
			const FString ExpectedIdentity =
				MakeAvidScriptRuntimePropertySetCanonicalIdentity(
					OwnerClass,
					Property,
					Binding);
			if (Binding.CanonicalIdentity != ExpectedIdentity
				|| Binding.StableId
					!= FAvidScriptHash::Sha256HexUtf8(ExpectedIdentity)
				|| Binding.HostImport.Signature
					!= MakeAvidScriptRuntimeExpectedSignature(Binding))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_identity_mismatch"),
					Binding.CanonicalIdentity,
					TEXT("The writable property descriptor no longer matches the active reflection snapshot."));
				return false;
			}

			if (!AttachAvidScriptGeneratedPlan(
					Model,
					Binding,
					Plan,
					OutResult))
			{
				return false;
			}
			PublishGeneratedPlan(Plan);
			Package->Impl->RequiredScratchSize = FMath::Max(
				Package->Impl->RequiredScratchSize,
				Plan.RequiredScratchSize);
			Package->Impl->VmPackage.Imports.Add({
				Binding.StableId,
				static_cast<uint32>(Binding.Ordinal),
				Binding.HostImport.Module,
				Binding.HostImport.Name,
				Binding.HostImport.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
			continue;
		}
		if (Binding.BindingKind == TEXT("property_get"))
		{
			++Package->Impl->Instrumentation.ReflectedNameLookupCount;
			FProperty* Property = FindFProperty<FProperty>(OwnerClass, FName(*Binding.UeMember));
			FAvidScriptBindingPropertyReplicationContract RuntimeReplication;
			if (Property == nullptr
				|| !TryResolveAvidScriptBindingPropertyReplicationContract(
					*Property,
					RuntimeReplication)
				|| RuntimeReplication != Binding.PropertyReplication
				|| !IsAvidScriptRuntimePropertyReadable(Property)
				|| Property->GetOwnerStruct() != OwnerClass
				|| (RuntimeReplication.IsReplicated()
					&& (!IsAvidScriptBindingNetworkOwnerClass(OwnerClass)
						|| (RuntimeReplication.Mode
								== EAvidScriptBindingPropertyReplicationMode::RepNotify
							&& OwnerClass->FindFunctionByName(
								RuntimeReplication.RepNotifyFunction) == nullptr))))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_missing"),
					Binding.OwnerClass + TEXT(".") + Binding.UeMember,
					TEXT("The reflected property is missing or no longer satisfies runtime read policy."));
				return false;
			}

			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = EAvidScriptBindingInvocationKind::ReflectedPropertyRead;
			Plan.OwnerClass = OwnerClass;
			Plan.ReflectedProperty = Property;
			Plan.PropertyReplication = RuntimeReplication;
			Plan.DebugPath = Property->GetPathName();
			Plan.bRequiresGuestMemory = true;
			Plan.ExpectedArgumentCount = 3;
			FString ReturnDetails;
			if (!BuildAvidScriptRuntimeValuePlan(
				Property,
				Binding.ReturnValue,
				DeclaredTypesById,
				2,
				Plan.ReturnValue,
				ReturnDetails))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_contract_mismatch"),
					Binding.CanonicalIdentity,
					ReturnDetails);
				return false;
			}
			const FString ExpectedIdentity = MakeAvidScriptRuntimePropertyGetCanonicalIdentity(
				OwnerClass,
				Property,
				Binding);
			if (Binding.CanonicalIdentity != ExpectedIdentity
				|| Binding.StableId != FAvidScriptHash::Sha256HexUtf8(ExpectedIdentity)
				|| Binding.HostImport.Signature
					!= MakeAvidScriptRuntimeExpectedSignature(Binding))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_identity_mismatch"),
					Binding.CanonicalIdentity,
					TEXT("The property descriptor no longer matches the active reflection snapshot."));
				return false;
			}

			if (!AttachAvidScriptGeneratedPlan(
					Model,
					Binding,
					Plan,
					OutResult))
			{
				return false;
			}
			PublishGeneratedPlan(Plan);
			Package->Impl->VmPackage.Imports.Add({
				Binding.StableId,
				static_cast<uint32>(Binding.Ordinal),
				Binding.HostImport.Module,
				Binding.HostImport.Name,
				Binding.HostImport.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
			continue;
		}

		++Package->Impl->Instrumentation.ReflectedNameLookupCount;
		UFunction* Function = OwnerClass->FindFunctionByName(FName(*Binding.UeFunction));
		const bool bLatentBinding =
			Binding.DispatchMode == TEXT("latent_process_event");
		FAvidScriptRuntimeLatentContract LatentContract;
		const bool bFunctionAllowed = bLatentBinding
			? ResolveAvidScriptRuntimeLatentContract(
				Function,
				Binding,
				LatentContract)
			: IsAvidScriptRuntimeFunctionAllowed(Function);
		if (!bFunctionAllowed)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_function_missing"),
				Binding.OwnerClass + TEXT(".") + Binding.UeFunction,
				TEXT("The reflected function is missing or no longer satisfies runtime policy."));
			return false;
		}
		FAvidScriptBindingNetworkContract RuntimeNetwork;
		if (!TryResolveAvidScriptBindingNetworkContract(
				*Function,
				RuntimeNetwork)
			|| RuntimeNetwork != Binding.Network)
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_network_contract_mismatch"),
				Binding.CanonicalIdentity,
				TEXT("The descriptor network mode or reliability no longer matches the active UFunction flags."));
			return false;
		}
		if (RuntimeNetwork.IsNetworked()
			&& (!IsAvidScriptBindingNetworkOwnerClass(OwnerClass)
				|| Binding.DispatchMode != TEXT("cached_process_event")
				|| Function->HasAnyFunctionFlags(FUNC_Static | FUNC_Const)
				|| Function->HasMetaData(TEXT("Latent"))
				|| Function->GetReturnProperty() != nullptr))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_network_owner_invalid"),
				Binding.CanonicalIdentity,
				TEXT("AvidScript RPC bindings require a non-static Actor or ActorComponent function with cached ProcessEvent dispatch and no return value."));
			return false;
		}
		if (Binding.bStatic != Function->HasAnyFunctionFlags(FUNC_Static)
			|| Binding.bConst != Function->HasAnyFunctionFlags(FUNC_Const)
			|| Binding.HostImport.Signature != MakeAvidScriptRuntimeExpectedSignature(Binding))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_function_contract_mismatch"),
				Binding.CanonicalIdentity,
				TEXT("Function flags or ABI signature changed since descriptor generation."));
			return false;
		}
		const bool bRuntimeReadOnly = Function->HasAnyFunctionFlags(FUNC_Const | FUNC_BlueprintPure);
		if ((Binding.ReloadEffect == EAvidScriptBindingReloadEffect::None && !bRuntimeReadOnly)
			|| (bLatentBinding
				&& Binding.ReloadEffect
					!= EAvidScriptBindingReloadEffect::ContinuationProducer)
			|| (!bLatentBinding
				&& Binding.ReloadEffect
					== EAvidScriptBindingReloadEffect::ContinuationProducer)
			|| ((Binding.ReloadEffect == EAvidScriptBindingReloadEffect::ActorTransform
					|| Binding.ReloadEffect == EAvidScriptBindingReloadEffect::SceneComponentTransform)
				&& bRuntimeReadOnly))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_reload_effect_mismatch"),
				Binding.CanonicalIdentity,
				TEXT("The descriptor reload effect conflicts with the reflected function flags."));
			return false;
		}

		TArray<FProperty*> ReflectedParameters;
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Parm)
				&& !Property->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				if (bLatentBinding
					&& (Property == LatentContract.LatentInfoProperty
						|| Property == LatentContract.WorldContextProperty))
				{
					continue;
				}
				ReflectedParameters.Add(Property);
			}
		}
		if (RuntimeNetwork.IsNetworked()
			&& ReflectedParameters.ContainsByPredicate(
				[](const FProperty* Property)
				{
					return Property != nullptr
						&& (Property->HasAnyPropertyFlags(CPF_OutParm)
							|| (Property->HasAnyPropertyFlags(CPF_ReferenceParm)
								&& !Property->HasAnyPropertyFlags(CPF_ConstParm)));
				}))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_network_contract_mismatch"),
				Binding.CanonicalIdentity,
				TEXT("AvidScript RPC bindings do not support ref or out parameters."));
			return false;
		}
		if (ReflectedParameters.Num() != Binding.Parameters.Num())
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_parameter_count_mismatch"),
				Binding.CanonicalIdentity,
				TEXT("Reflected parameter count changed since descriptor generation."));
			return false;
		}

		FAvidScriptRuntimeBindingInvocationPlan Plan;
		Plan.OwnerClass = OwnerClass;
		Plan.Function = Function;
		Plan.DebugPath = Function->GetPathName();
		Plan.bStatic = Binding.bStatic;
		Plan.bLatent = bLatentBinding;
		Plan.LatentInfoProperty = LatentContract.LatentInfoProperty;
		Plan.WorldContextProperty = LatentContract.WorldContextProperty;
		Plan.LatentCompletion = LatentContract.Completion;
		Plan.Network = RuntimeNetwork;
		Plan.ReloadEffect = Binding.ReloadEffect;
		Plan.bRequiresWriteAccess = RuntimeNetwork.IsNetworked()
			|| Binding.ReloadEffect != EAvidScriptBindingReloadEffect::None;
		Plan.FrameSize = Function->GetStructureSize();
		Plan.FrameAlignment = FMath::Max(1, Function->GetMinAlignment());
		if (Plan.FrameSize < Function->ParmsSize
			|| !FMath::IsPowerOfTwo(Plan.FrameAlignment)
			|| Plan.FrameSize > MAX_int32 - (Plan.FrameAlignment - 1))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_frame_layout_invalid"),
				Binding.CanonicalIdentity,
				TEXT("The reflected function frame size or alignment is invalid."));
			return false;
		}
		Plan.RequiredScratchSize = Plan.FrameSize + Plan.FrameAlignment - 1;
		int32 ArgumentOffset = Binding.bStatic ? 0 : 2;
		for (int32 Index = 0; Index < Binding.Parameters.Num(); ++Index)
		{
			FProperty* Property = ReflectedParameters[Index];
			const FAvidScriptBindingValueModel& Parameter = Binding.Parameters[Index];
			if (Property->GetName() != Parameter.Name
				|| GetAvidScriptRuntimePropertyDirection(Property) != Parameter.Direction)
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_parameter_contract_mismatch"),
					Binding.CanonicalIdentity + TEXT(":") + Parameter.Name,
					TEXT("Reflected parameter name or direction changed since descriptor generation."));
				return false;
			}
			FAvidScriptRuntimeBindingValuePlan ValuePlan;
			FString Details;
			if (!BuildAvidScriptRuntimeValuePlan(
				Property,
				Parameter,
				DeclaredTypesById,
				ArgumentOffset,
				ValuePlan,
				Details))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_property_contract_mismatch"),
					Binding.CanonicalIdentity + TEXT(":") + Parameter.Name,
					Details);
				return false;
			}
			ArgumentOffset += ValuePlan.ArgumentWidth;
			Plan.bRequiresGuestMemory |= ValuePlan.Direction == EAvidScriptRuntimeBindingDirection::Ref
				|| ValuePlan.Direction == EAvidScriptRuntimeBindingDirection::Out
				|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::Name
				|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::String
				|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::StructWire
				|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::Array;
			Plan.Parameters.Add(MoveTemp(ValuePlan));
		}
		if (bLatentBinding)
		{
			Plan.CallbackIdArgumentOffset = ArgumentOffset;
			++ArgumentOffset;
		}

		FProperty* ReturnProperty = Function->GetReturnProperty();
		FString ReturnDetails;
		if (!BuildAvidScriptRuntimeValuePlan(
			ReturnProperty,
			Binding.ReturnValue,
			DeclaredTypesById,
			ArgumentOffset,
			Plan.ReturnValue,
			ReturnDetails))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_return_contract_mismatch"),
				Binding.CanonicalIdentity,
				ReturnDetails);
			return false;
		}
		ArgumentOffset += Plan.ReturnValue.ArgumentWidth;
		Plan.bRequiresGuestMemory |= Plan.ReturnValue.Kind != EAvidScriptRuntimeBindingKind::Void;
		Plan.ExpectedArgumentCount = ArgumentOffset;
		if (Binding.CanonicalIdentity != MakeAvidScriptRuntimeCanonicalIdentity(OwnerClass, Function, Binding)
			|| Binding.StableId != FAvidScriptHash::Sha256HexUtf8(Binding.CanonicalIdentity))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_identity_mismatch"),
				Binding.CanonicalIdentity,
				TEXT("The descriptor identity no longer matches the active reflection snapshot."));
			return false;
		}

		if (!AttachAvidScriptGeneratedPlan(
				Model,
				Binding,
				Plan,
				OutResult))
		{
			return false;
		}

		TArray<
			UE::AvidScript::BindingPrivate::FFastPathValueSpec,
			TInlineAllocator<2>> FastPathParameters;
		FastPathParameters.Reserve(Plan.Parameters.Num());
		for (const FAvidScriptRuntimeBindingValuePlan& Parameter : Plan.Parameters)
		{
			FastPathParameters.Add({
				Parameter.Property,
				Parameter.ArgumentOffset,
				GetAvidScriptFastPathValueKind(Parameter.Kind),
				Parameter.Direction == EAvidScriptRuntimeBindingDirection::Value
					|| Parameter.Direction
						== EAvidScriptRuntimeBindingDirection::ConstRef
			});
		}
		UE::AvidScript::BindingPrivate::FFastPathBuildSpec FastPathSpec;
		FastPathSpec.Function = Plan.Function;
		FastPathSpec.FrameSize = Plan.FrameSize;
		FastPathSpec.FrameAlignment = Plan.FrameAlignment;
		FastPathSpec.ExpectedArgumentCount = Plan.ExpectedArgumentCount;
		FastPathSpec.bStatic = Plan.bStatic;
		FastPathSpec.bRequiresWriteAccess = Plan.bRequiresWriteAccess;
		FastPathSpec.bHasReloadEffect =
			Plan.ReloadEffect != EAvidScriptBindingReloadEffect::None;
		FastPathSpec.bQualifiedNativeDirectAuthorized =
			Binding.DispatchMode == TEXT("qualified_native_direct");
		FastPathSpec.Parameters = FastPathParameters;
		FastPathSpec.ReturnValue = {
			Plan.ReturnValue.Property,
			Plan.ReturnValue.ArgumentOffset,
			GetAvidScriptFastPathValueKind(Plan.ReturnValue.Kind),
			false
		};
		if (!bLatentBinding
			&& UE::AvidScript::BindingPrivate::TryBuildFastPath(
			FastPathSpec,
			Plan.FastPath))
		{
			++Package->Impl->Instrumentation.TypedThunkPlanCount;
		}
		else
		{
			++Package->Impl->Instrumentation.ReflectionFallbackPlanCount;
		}
		PublishGeneratedPlan(Plan);

		Package->Impl->RequiredScratchSize = FMath::Max(
			Package->Impl->RequiredScratchSize,
			Plan.RequiredScratchSize);
		Package->Impl->VmPackage.Imports.Add({
			Binding.StableId,
			static_cast<uint32>(Binding.Ordinal),
			Binding.HostImport.Module,
			Binding.HostImport.Name,
			Binding.HostImport.Signature
		});
		Package->Impl->Plans.Add(MoveTemp(Plan));
	}

	const auto PublishDelegateInvokePlans = [&]()
	{
	for (int32 EventIndex = 0;
		EventIndex < Model.DelegateEvents.Num();
		++EventIndex)
	{
		const FAvidScriptBindingDelegateEventModel& Event =
			Model.DelegateEvents[EventIndex];
		if (Event.DelegateKind != TEXT("singlecast")
			&& Event.DelegateKind != TEXT("multicast"))
		{
			continue;
		}
		if (!Package->Impl->PreparedDelegateEventCells.IsValidIndex(
				EventIndex))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_delegate_invoke_cell_missing"),
				Event.CanonicalIdentity,
				TEXT("The active delegate event has no prepared signature cell."));
			return false;
		}
		const FAvidScriptPreparedDelegateEventCell& EventCell =
			*Package->Impl->PreparedDelegateEventCells[EventIndex];
		const int32 BindingOrdinal = Package->Impl->Plans.Num();
		FAvidScriptBindingDelegateInvokeSpec InvokeSpec;
		if (EventCell.StableId != Event.StableId
			|| EventCell.ExpectedSourceClass == nullptr
			|| EventCell.SignatureFunction == nullptr
			|| !FAvidScriptBindingDescriptorIdentity::TryMakeDelegateInvokeSpec(
				Event,
				BindingOrdinal,
				InvokeSpec))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_delegate_invoke_contract_mismatch"),
				Event.CanonicalIdentity,
				TEXT("The delegate invoke import cannot be derived from the prepared event contract."));
			return false;
		}

		FAvidScriptRuntimeBindingInvocationPlan Plan;
		Plan.Kind = Event.DelegateKind == TEXT("singlecast")
			? EAvidScriptBindingInvocationKind::DelegateSinglecastInvoke
			: EAvidScriptBindingInvocationKind::DelegateMulticastBroadcast;
		Plan.OwnerClass = EventCell.ExpectedSourceClass;
		Plan.Function = EventCell.SignatureFunction;
		Plan.ReflectedProperty = Event.DelegateKind == TEXT("singlecast")
			? static_cast<FProperty*>(EventCell.SinglecastProperty)
			: static_cast<FProperty*>(EventCell.MulticastProperty);
		Plan.DebugPath = Plan.ReflectedProperty == nullptr
			? Event.CanonicalIdentity
			: Plan.ReflectedProperty->GetPathName();
		Plan.bRequiresWriteAccess = true;
		Plan.ReloadEffect = EAvidScriptBindingReloadEffect::Unsupported;
		Plan.FrameSize = Plan.Function->GetStructureSize();
		Plan.FrameAlignment = FMath::Max(1, Plan.Function->GetMinAlignment());
		if (Plan.ReflectedProperty == nullptr
			|| Plan.FrameSize < Plan.Function->ParmsSize
			|| !FMath::IsPowerOfTwo(Plan.FrameAlignment)
			|| Plan.FrameSize > MAX_int32 - (Plan.FrameAlignment - 1))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_delegate_invoke_layout_invalid"),
				Event.CanonicalIdentity,
				TEXT("The prepared delegate property or parameter frame layout is invalid."));
			return false;
		}
		Plan.RequiredScratchSize = Plan.FrameSize + Plan.FrameAlignment - 1;

		TArray<FProperty*> ReflectedParameters;
		for (TFieldIterator<FProperty> It(Plan.Function); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Parm)
				&& !Property->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReflectedParameters.Add(Property);
			}
		}
		if (ReflectedParameters.Num() != Event.Parameters.Num())
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_delegate_invoke_parameter_count_mismatch"),
				Event.CanonicalIdentity,
				TEXT("The delegate invoke parameter count changed since descriptor generation."));
			return false;
		}

		int32 ArgumentOffset = 2;
		for (int32 ParameterIndex = 0;
			ParameterIndex < Event.Parameters.Num();
			++ParameterIndex)
		{
			FProperty* Property = ReflectedParameters[ParameterIndex];
			const FAvidScriptBindingValueModel& Parameter =
				Event.Parameters[ParameterIndex];
			FAvidScriptRuntimeBindingValuePlan ValuePlan;
			FString Details;
			if (Property->GetName() != Parameter.Name
				|| GetAvidScriptRuntimePropertyDirection(Property)
					!= Parameter.Direction
				|| !BuildAvidScriptRuntimeValuePlan(
					Property,
					Parameter,
					DeclaredTypesById,
					ArgumentOffset,
					ValuePlan,
					Details))
			{
				SetAvidScriptBindingLoadFailure(
					OutResult,
					TEXT("binding_delegate_invoke_parameter_mismatch"),
					Event.CanonicalIdentity + TEXT(":") + Parameter.Name,
					Details.IsEmpty()
						? TEXT("The delegate invoke parameter no longer matches its prepared codec.")
						: Details);
				return false;
			}
			ArgumentOffset += ValuePlan.ArgumentWidth;
			Plan.bRequiresGuestMemory |= ValuePlan.Direction
					== EAvidScriptRuntimeBindingDirection::Ref
				|| ValuePlan.Direction
					== EAvidScriptRuntimeBindingDirection::Out
				|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::Name
				|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::String
				|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::StructWire
				|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::Array;
			Plan.Parameters.Add(MoveTemp(ValuePlan));
		}

		FString ReturnDetails;
		if (!BuildAvidScriptRuntimeValuePlan(
				Plan.Function->GetReturnProperty(),
				Event.ReturnValue,
				DeclaredTypesById,
				ArgumentOffset,
				Plan.ReturnValue,
				ReturnDetails))
		{
			SetAvidScriptBindingLoadFailure(
				OutResult,
				TEXT("binding_delegate_invoke_return_mismatch"),
				Event.CanonicalIdentity,
				ReturnDetails);
			return false;
		}
		ArgumentOffset += Plan.ReturnValue.ArgumentWidth;
		Plan.bRequiresGuestMemory |= Plan.ReturnValue.Kind
			!= EAvidScriptRuntimeBindingKind::Void;
		Plan.ExpectedArgumentCount = ArgumentOffset;

		Package->Impl->RequiredScratchSize = FMath::Max(
			Package->Impl->RequiredScratchSize,
			Plan.RequiredScratchSize);
		Package->Impl->VmPackage.Imports.Add({
			InvokeSpec.StableId,
			static_cast<uint32>(InvokeSpec.BindingOrdinal),
			InvokeSpec.ModuleName,
			InvokeSpec.ImportName,
			InvokeSpec.Signature
		});
		Package->Impl->Plans.Add(MoveTemp(Plan));
	}
	return true;
	};

	if (bHasLifecycleClassReferences)
	{
		for (const FAvidScriptObjectLifecycleBindingSpec& Spec : FAvidScriptObjectLifecycleBindings::GetSpecs())
		{
			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = Spec.Kind;
			Plan.DebugPath = Spec.ModuleName + TEXT(".") + Spec.ImportName;
			Plan.bRequiresWriteAccess = Spec.Kind == EAvidScriptBindingInvocationKind::ObjectSpawnActor
				|| Spec.Kind == EAvidScriptBindingInvocationKind::ObjectDestroyActor;
			Plan.bRequiresGuestMemory = Spec.Kind == EAvidScriptBindingInvocationKind::ObjectSpawnActor;
			switch (Spec.Kind)
			{
			case EAvidScriptBindingInvocationKind::ObjectSpawnActor:
				Plan.ExpectedArgumentCount = 3;
				break;
			case EAvidScriptBindingInvocationKind::ObjectDestroyActor:
				Plan.ExpectedArgumentCount = 2;
				break;
			case EAvidScriptBindingInvocationKind::ObjectIsA:
				Plan.ExpectedArgumentCount = 3;
				break;
			default:
				checkNoEntry();
				break;
			}

			Package->Impl->VmPackage.Imports.Add({
				Spec.StableId,
				static_cast<uint32>(Package->Impl->Plans.Num()),
				Spec.ModuleName,
				Spec.ImportName,
				Spec.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
		}
	}

	if (!Package->Impl->ObjectTypePlans.IsEmpty())
	{
		for (const FAvidScriptObjectTypeBindingSpec& Spec : FAvidScriptObjectTypeBindings::GetSpecs())
		{
			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = Spec.Kind;
			Plan.DebugPath = Spec.ModuleName + TEXT(".") + Spec.ImportName;
			Plan.ExpectedArgumentCount = 3;

			Package->Impl->VmPackage.Imports.Add({
				Spec.StableId,
				static_cast<uint32>(Package->Impl->Plans.Num()),
				Spec.ModuleName,
				Spec.ImportName,
				Spec.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
		}
	}

	if (ObjectFactoryBindingCount > 0)
	{
		for (const FAvidScriptObjectFactoryBindingSpec& Spec :
			FAvidScriptObjectFactoryBinding::GetSpecs())
		{
			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = Spec.Kind;
			Plan.DebugPath = Spec.ModuleName + TEXT(".") + Spec.ImportName;
			Plan.bRequiresWriteAccess =
				Spec.Kind == EAvidScriptBindingInvocationKind::ObjectConstruct
				|| Spec.Kind == EAvidScriptBindingInvocationKind::ObjectRelease;
			Plan.ExpectedArgumentCount = Spec.Kind
				== EAvidScriptBindingInvocationKind::ObjectRelease
				? 2
				: 3;

			Package->Impl->VmPackage.Imports.Add({
				Spec.StableId,
				static_cast<uint32>(Package->Impl->Plans.Num()),
				Spec.ModuleName,
				Spec.ImportName,
				Spec.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
		}
	}

	if (SceneAttachmentBindingCount > 0)
	{
		for (const FAvidScriptSceneAttachmentBindingSpec& Spec :
			FAvidScriptSceneAttachmentBinding::GetSpecs())
		{
			FAvidScriptRuntimeBindingInvocationPlan Plan;
			Plan.Kind = Spec.Kind;
			Plan.DebugPath = Spec.ModuleName + TEXT(".") + Spec.ImportName;
			Plan.bRequiresWriteAccess = true;
			Plan.ExpectedArgumentCount = Spec.Kind
				== EAvidScriptBindingInvocationKind::SceneComponentAttach
				? 5
				: 3;

			Package->Impl->VmPackage.Imports.Add({
				Spec.StableId,
				static_cast<uint32>(Package->Impl->Plans.Num()),
				Spec.ModuleName,
				Spec.ImportName,
				Spec.Signature
			});
			Package->Impl->Plans.Add(MoveTemp(Plan));
		}
	}
	if (!PublishDelegateInvokePlans())
	{
		return false;
	}

	for (int32 PlanIndex = 0; PlanIndex < Model.Bindings.Num(); ++PlanIndex)
	{
		const FAvidScriptRuntimeBindingInvocationPlan& Plan =
			Package->Impl->Plans[PlanIndex];
		if (Plan.GeneratedEntry == nullptr
			&& Plan.FastPath.bAdaptiveNativeEligible)
		{
			++Package->Impl->Instrumentation
				.AdaptivePreparedNativePlanCount;
		}
		else if (Plan.GeneratedEntry == nullptr)
		{
			++Package->Impl->Instrumentation
				.AdaptiveStrictFallbackPlanCount;
		}
		if (Plan.FastPath.HighestInvocationMode
			== EAvidScriptBindingInvocationMode::QualifiedNativeDirect)
		{
			++Package->Impl->Instrumentation.QualifiedNativeDirectPlanCount;
		}
		else
		{
			++Package->Impl->Instrumentation.SemanticOnlyPlanCount;
		}
	}

	bool bCompositeAccessCacheValid = true;
	FString CompositeAccessCacheError;
	TFunction<void(const FAvidScriptRuntimeBindingValuePlan&)>
		CacheCompositeAccessPlan;
	CacheCompositeAccessPlan =
		[&Package, &bCompositeAccessCacheValid, &CompositeAccessCacheError,
			&CacheCompositeAccessPlan](
			const FAvidScriptRuntimeBindingValuePlan& ValuePlan)
	{
		if (!bCompositeAccessCacheValid)
		{
			return;
		}
		const bool bCompositeContainer =
			ValuePlan.Kind == EAvidScriptRuntimeBindingKind::CompositeArray
			|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::Set
			|| ValuePlan.Kind == EAvidScriptRuntimeBindingKind::Map;
		if (bCompositeContainer)
		{
			if (ValuePlan.Property == nullptr || ValuePlan.TypeId.IsEmpty())
			{
				bCompositeAccessCacheValid = false;
				CompositeAccessCacheError = TEXT("composite_access_plan_invalid");
				return;
			}
			const FAvidScriptRuntimeBindingValuePlan* Existing =
				Package->Impl->CompositeAccessPlansByProperty.Find(
					ValuePlan.Property);
			if (Existing != nullptr
				&& (Existing->TypeId != ValuePlan.TypeId
					|| Existing->Kind != ValuePlan.Kind))
			{
				bCompositeAccessCacheValid = false;
				CompositeAccessCacheError = ValuePlan.Property->GetPathName();
				return;
			}
			if (Existing == nullptr)
			{
				Package->Impl->CompositeAccessPlansByProperty.Add(
					ValuePlan.Property,
					ValuePlan);
			}
		}
		for (const FAvidScriptRuntimeBindingValuePlan& Child : ValuePlan.Children)
		{
			CacheCompositeAccessPlan(Child);
		}
	};
	for (const FAvidScriptRuntimeBindingInvocationPlan& Plan :
		Package->Impl->Plans)
	{
		for (const FAvidScriptRuntimeBindingValuePlan& Parameter :
			Plan.Parameters)
		{
			CacheCompositeAccessPlan(Parameter);
		}
		CacheCompositeAccessPlan(Plan.ReturnValue);
	}
	if (!bCompositeAccessCacheValid)
	{
		SetAvidScriptBindingLoadFailure(
			OutResult,
			TEXT("binding_composite_access_plan_invalid"),
			CompositeAccessCacheError,
			TEXT("The activated package contains conflicting recursive container access plans."));
		return false;
	}

	Package->Impl->PreparedDynamicCells.Reserve(
		Model.Bindings.Num() + DelegateInvokeBindingCount);
	for (int32 PlanIndex = 0;
		PlanIndex < Package->Impl->Plans.Num();
		++PlanIndex)
	{
		const FAvidScriptRuntimeBindingInvocationPlan& Plan =
			Package->Impl->Plans[PlanIndex];
		const bool bReflected =
			Plan.Kind == EAvidScriptBindingInvocationKind::ReflectedFunction
			|| Plan.Kind
				== EAvidScriptBindingInvocationKind::ReflectedPropertyRead
			|| Plan.Kind
				== EAvidScriptBindingInvocationKind::ReflectedPropertyWrite
			|| Plan.Kind
				== EAvidScriptBindingInvocationKind::DelegateSinglecastInvoke
			|| Plan.Kind
				== EAvidScriptBindingInvocationKind::DelegateMulticastBroadcast;
		if (!bReflected
			|| Plan.OwnerClass == nullptr
			|| Plan.GeneratedEntry != nullptr)
		{
			continue;
		}

		TUniquePtr<
			UE::AvidScript::BindingPrivate::FPreparedDynamicInvocationCell> Cell =
			MakeUnique<
				UE::AvidScript::BindingPrivate::FPreparedDynamicInvocationCell>();
		Cell->Program = &Plan;
		Cell->BindingOrdinal = static_cast<uint32>(PlanIndex);
		Package->Impl->PreparedDynamicCells.Add(MoveTemp(Cell));
	}

	OutResult.bSucceeded = true;
	OutResult.BindingCount = Model.Bindings.Num();
	OutResult.ClassReferenceCount = Package->Impl->ClassReferencePlans.Num();
	OutResult.ObjectFactoryCount = Package->Impl->ObjectFactoryPlans.Num();
	OutResult.RequiredScratchSize = Package->Impl->RequiredScratchSize;
	OutResult.PackageName = Package->Impl->PackageName;
	OutResult.PackageHash = Package->Impl->PackageHash;
	OutPackage = Package;
	return true;
}

const FString& FAvidScriptBindingPackage::GetPackageName() const
{
	return Impl->PackageName;
}

const FString& FAvidScriptBindingPackage::GetPackageHash() const
{
	return Impl->PackageHash;
}

int32 FAvidScriptBindingPackage::GetDescriptorSchemaVersion() const
{
	return Impl->DescriptorSchemaVersion;
}

int32 FAvidScriptBindingPackage::GetDelegateEventCount() const
{
	return Impl->PreparedDelegateEventCells.Num();
}

int32 FAvidScriptBindingPackage::GetMulticastDelegateEventCount() const
{
	int32 Count = 0;
	for (const TUniquePtr<FAvidScriptPreparedDelegateEventCell>& Cell :
		Impl->PreparedDelegateEventCells)
	{
		Count += Cell.IsValid()
			&& Cell->CallbackKind == TEXT("multicast") ? 1 : 0;
	}
	return Count;
}

int32 FAvidScriptBindingPackage::GetInboundHandlerCount() const
{
	int32 Count = 0;
	for (const TUniquePtr<FAvidScriptPreparedDelegateEventCell>& Cell :
		Impl->PreparedDelegateEventCells)
	{
		Count += Cell.IsValid()
			&& Cell->Kind == EAvidScriptPreparedDelegateKind::FunctionHandler
			? 1
			: 0;
	}
	return Count;
}

const FAvidScriptVmBindingPackage& FAvidScriptBindingPackage::GetVmPackage() const
{
	return Impl->VmPackage;
}

const FAvidScriptBindingPackageInstrumentation& FAvidScriptBindingPackage::GetInstrumentation() const
{
	return Impl->Instrumentation;
}

bool FAvidScriptBindingPackage::TryGetFastPathKind(
	const uint32 Ordinal,
	EAvidScriptBindingFastPathKind& OutKind) const
{
	OutKind = EAvidScriptBindingFastPathKind::None;
	if (!Impl->Plans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	OutKind = Impl->Plans[Ordinal].FastPath.Kind;
	return true;
}

bool FAvidScriptBindingPackage::TryGetInvocationMode(
	const uint32 Ordinal,
	EAvidScriptBindingInvocationMode& OutMode) const
{
	OutMode = EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	if (!Impl->Plans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	OutMode = Impl->Plans[Ordinal].FastPath.HighestInvocationMode;
	return true;
}

bool FAvidScriptBindingPackage::TryGetInvocationMode(
	const uint32 Ordinal,
	const EAvidScriptBindingInvocationPolicy Policy,
	EAvidScriptBindingInvocationMode& OutMode) const
{
	OutMode = EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	if (!Impl->Plans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	const FAvidScriptRuntimeBindingInvocationPlan& Plan =
		Impl->Plans[Ordinal];
	if (Plan.GeneratedEntry != nullptr)
	{
		OutMode = EAvidScriptBindingInvocationMode::GeneratedNativeS1;
	}
	else if (Policy == EAvidScriptBindingInvocationPolicy::AdaptiveSemantic
		&& Plan.FastPath.bAdaptiveNativeEligible)
	{
		OutMode =
			EAvidScriptBindingInvocationMode::AdaptivePreparedNative;
	}
	else if (Policy
			== EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect
		&& Plan.FastPath.HighestInvocationMode
			== EAvidScriptBindingInvocationMode::QualifiedNativeDirect)
	{
		OutMode =
			EAvidScriptBindingInvocationMode::QualifiedNativeDirect;
	}
	return true;
}

bool FAvidScriptBindingPackage::BuildTypedHostImports(
	TArray<FAvidScriptVmTypedHostImport>& OutImports,
	FString& OutError) const
{
	OutImports.Reset();
	OutError.Reset();
	for (const FAvidScriptRuntimeBindingInvocationPlan& Plan : Impl->Plans)
	{
		if (Plan.GeneratedEntry != nullptr
			&& Plan.GeneratedLease.GetEntry() != Plan.GeneratedEntry)
		{
			OutError = TEXT("generated_binding_unavailable");
			return false;
		}
	}

	OutImports.Reserve(
		static_cast<int32>(Impl->Instrumentation.GeneratedNativeS1PlanCount));
	for (const FAvidScriptRuntimeBindingInvocationPlan& Plan : Impl->Plans)
	{
		if (Plan.GeneratedEntry != nullptr)
		{
			OutImports.Add(Plan.TypedHostImport);
		}
	}
	return true;
}

bool FAvidScriptBindingPackage::BuildPreparedGeneratedBindings(
	TArray<FAvidScriptPreparedGeneratedBinding>& OutBindings,
	FString& OutError) const
{
	OutBindings.Reset();
	OutError.Reset();
	OutBindings.Reserve(
		static_cast<int32>(Impl->Instrumentation.GeneratedNativeS1PlanCount));
	for (int32 PlanIndex = 0; PlanIndex < Impl->Plans.Num(); ++PlanIndex)
	{
		const FAvidScriptRuntimeBindingInvocationPlan& Plan =
			Impl->Plans[PlanIndex];
		if (Plan.GeneratedEntry == nullptr)
		{
			continue;
		}
		if (Plan.GeneratedLease.GetEntry() != Plan.GeneratedEntry)
		{
			OutBindings.Reset();
			OutError = TEXT("generated_binding_unavailable");
			return false;
		}

		FAvidScriptPreparedGeneratedBinding& Binding =
			OutBindings.AddDefaulted_GetRef();
		Binding.BindingOrdinal = static_cast<uint32>(PlanIndex);
		Binding.Lease = Plan.GeneratedLease;
		Binding.Entry = Plan.GeneratedEntry;
		Binding.ExpectedClass = Plan.OwnerClass;
		Binding.ReflectedProperty = Plan.ReflectedProperty;
		Binding.ReloadEffect = Plan.ReloadEffect;
		Binding.bPropertyWrite =
			Plan.Kind
				== EAvidScriptBindingInvocationKind::ReflectedPropertyWrite;
		Binding.bPropertyWriteHasFunction =
			Binding.bPropertyWrite && Plan.Function != nullptr;
		Binding.bRequiresWriteAccess = Plan.bRequiresWriteAccess;
	}
	return true;
}

bool FAvidScriptBindingPackage::BuildPreparedReflectionBindings(
	TArray<FAvidScriptPreparedReflectionBinding>& OutBindings,
	FString& OutError) const
{
	OutBindings.Reset();
	OutError.Reset();
	const int32 ImportCount = Impl->VmPackage.Imports.Num();
	OutBindings.Reserve(ImportCount);
	for (int32 PlanIndex = 0;
		PlanIndex < ImportCount
			&& Impl->Plans.IsValidIndex(PlanIndex);
		++PlanIndex)
	{
		const FAvidScriptRuntimeBindingInvocationPlan& Plan =
			Impl->Plans[PlanIndex];
		if (Plan.GeneratedEntry != nullptr)
		{
			continue;
		}
		const bool bScalar = Plan.FastPath.Kind
			== EAvidScriptBindingFastPathKind::ScalarI32PairToI32;
		const bool bVector = Plan.FastPath.Kind
			== EAvidScriptBindingFastPathKind::VectorValueToVector;
		const bool bObject = Plan.FastPath.Kind
			== EAvidScriptBindingFastPathKind::ObjectToObject;
		const bool bPropertyRead =
			Plan.Kind == EAvidScriptBindingInvocationKind::ReflectedPropertyRead
			&& Plan.Function == nullptr
			&& Plan.ReflectedProperty != nullptr
			&& Plan.ReturnValue.Kind == EAvidScriptRuntimeBindingKind::Int32;
		const bool bPropertyWrite =
			Plan.Kind == EAvidScriptBindingInvocationKind::ReflectedPropertyWrite
			&& Plan.Function == nullptr
			&& Plan.ReflectedProperty != nullptr
			&& Plan.Parameters.Num() == 1
			&& Plan.Parameters[0].Kind
				== EAvidScriptRuntimeBindingKind::Int32;
		if (!bScalar
			&& !bVector
			&& !bObject
			&& !bPropertyRead
			&& !bPropertyWrite)
		{
			continue;
		}
		const FAvidScriptVmDynamicImport& DynamicImport =
			Impl->VmPackage.Imports[PlanIndex];
		const TCHAR* ExpectedSignature = bVector
			? TEXT("(iifffi)i")
			: (bPropertyRead || bPropertyWrite)
				? TEXT("(iii)i")
				: TEXT("(iiiii)i");
		if (DynamicImport.Ordinal != static_cast<uint32>(PlanIndex)
			|| DynamicImport.Signature != ExpectedSignature)
		{
			OutBindings.Reset();
			OutError = TEXT("prepared_reflection_import_mismatch");
			return false;
		}

		FAvidScriptPreparedReflectionBinding& Binding =
			OutBindings.AddDefaulted_GetRef();
		Binding.BindingOrdinal = static_cast<uint32>(PlanIndex);
		Binding.ExpectedClass = Plan.OwnerClass;
		Binding.ImmutablePlanIdentity = &Plan;
		Binding.bAdaptiveNativeEligible =
			Plan.FastPath.bAdaptiveNativeEligible
			|| bPropertyRead
			|| bPropertyWrite;
		Binding.bQualifiedNativeEligible =
			Plan.FastPath.HighestInvocationMode
				== EAvidScriptBindingInvocationMode::QualifiedNativeDirect;
		Binding.ExpectedObjectClass =
			bObject ? Plan.Parameters[0].ObjectClass : nullptr;
		Binding.ReflectedProperty = Plan.ReflectedProperty;
		Binding.ReloadEffect = Plan.ReloadEffect;
		Binding.bPropertyWrite = bPropertyWrite;
		Binding.bRequiresWriteAccess = Plan.bRequiresWriteAccess;
		Binding.TypedHostImport.StableId = DynamicImport.StableId;
		Binding.TypedHostImport.BindingOrdinal =
			static_cast<uint32>(PlanIndex);
		Binding.TypedHostImport.ModuleName =
			DynamicImport.ModuleName;
		Binding.TypedHostImport.ImportName =
			DynamicImport.ImportName;
		Binding.TypedHostImport.Signature =
			DynamicImport.Signature;
		if (bScalar)
		{
			Binding.NativeGuard =
				&ValidateAvidScriptPreparedReflectionNativeGuard;
			Binding.I32PairCall =
				&InvokeAvidScriptPreparedReflectionI32PairCall;
			Binding.TypedHostImport.Shape =
				EAvidScriptVmTypedHostShape::SelfI32PairToGuestI32;
		}
		else if (bVector)
		{
			Binding.NativeGuard =
				&ValidateAvidScriptPreparedReflectionNativeGuard;
			Binding.VectorCall =
				&InvokeAvidScriptPreparedReflectionVectorCall;
			Binding.TypedHostImport.Shape =
				EAvidScriptVmTypedHostShape::SelfF32TripleToGuestVector;
		}
		else if (bObject)
		{
			Binding.NativeGuard =
				&ValidateAvidScriptPreparedReflectionNativeGuard;
			Binding.ObjectCall =
				&InvokeAvidScriptPreparedReflectionObjectCall;
			Binding.TypedHostImport.Shape =
				EAvidScriptVmTypedHostShape::StableObjectRoundtrip;
		}
		else
		{
			Binding.NativeGuard =
				&ValidateAvidScriptPreparedReflectionPropertyGuard;
			Binding.PropertyI32Get = bPropertyRead
				? &ReadAvidScriptPreparedReflectionPropertyI32
				: nullptr;
			Binding.PropertyI32Set = bPropertyWrite
				? &WriteAvidScriptPreparedReflectionPropertyI32
				: nullptr;
			Binding.TypedHostImport.Shape =
				EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet;
		}
	}
	return true;
}

bool FAvidScriptBindingPackage::BuildPreparedDynamicBindings(
	TArray<FAvidScriptPreparedDynamicBinding>& OutBindings,
	FString& OutError) const
{
	OutBindings.Reset();
	OutError.Reset();
	OutBindings.Reserve(Impl->PreparedDynamicCells.Num());
	for (const TUniquePtr<
			UE::AvidScript::BindingPrivate::FPreparedDynamicInvocationCell>& Cell
		: Impl->PreparedDynamicCells)
	{
		if (!Cell.IsValid()
			|| Cell->Program == nullptr
			|| !Impl->VmPackage.Imports.IsValidIndex(
				static_cast<int32>(Cell->BindingOrdinal)))
		{
			OutBindings.Reset();
			OutError = TEXT("prepared_dynamic_cell_invalid");
			return false;
		}
		const FAvidScriptRuntimeBindingInvocationPlan& Program =
			*Cell->Program;
		const FAvidScriptVmDynamicImport& Import =
			Impl->VmPackage.Imports[
				static_cast<int32>(Cell->BindingOrdinal)];
		if (Import.Ordinal != Cell->BindingOrdinal
			|| Program.OwnerClass == nullptr
			|| Program.ExpectedArgumentCount < 0
			|| Program.RequiredScratchSize < 0)
		{
			OutBindings.Reset();
			OutError = TEXT("prepared_dynamic_import_mismatch");
			return false;
		}

		FAvidScriptPreparedDynamicBinding& Binding =
			OutBindings.AddDefaulted_GetRef();
		Binding.BindingOrdinal = Cell->BindingOrdinal;
		Binding.StableId = Import.StableId;
		Binding.ModuleName = Import.ModuleName;
		Binding.ImportName = Import.ImportName;
		Binding.Signature = Import.Signature;
		Binding.ImmutableInvocationCell = Cell.Get();
		Binding.ExpectedClass = Program.OwnerClass;
		Binding.ExpectedArgumentCount = Program.ExpectedArgumentCount;
		Binding.RequiredScratchSize = Program.RequiredScratchSize;
		Binding.Invoke = &UE::AvidScript::BindingPrivate::
			InvokePreparedDynamicReflection;
		Binding.bRequiresGuestMemory = Program.bRequiresGuestMemory;
		Binding.bStatic = Program.bStatic;
	}
	return true;
}

bool FAvidScriptBindingPackage::BuildPreparedDelegateEvents(
	TArray<FAvidScriptPreparedDelegateEvent>& OutEvents,
	FString& OutError) const
{
	OutEvents.Reset();
	OutError.Reset();
	OutEvents.Reserve(Impl->PreparedDelegateEventCells.Num());
	for (const TUniquePtr<FAvidScriptPreparedDelegateEventCell>& Cell :
		Impl->PreparedDelegateEventCells)
	{
		if (Cell.IsValid()
			&& Cell->Kind == EAvidScriptPreparedDelegateKind::FunctionHandler)
		{
			continue;
		}
		if (!Cell.IsValid()
			|| Cell->EventOrdinal == MAX_uint32
			|| Cell->StableId.IsEmpty()
			|| Cell->ExportName.IsEmpty()
			|| Cell->ExpectedSourceClass == nullptr
			|| (Cell->Kind == EAvidScriptPreparedDelegateKind::Singlecast
				? Cell->SinglecastProperty == nullptr
				: Cell->MulticastProperty == nullptr)
			|| Cell->SignatureFunction == nullptr
			|| Cell->Codec.ParameterCellCount > FAvidScriptVmCallFrame::MaxCells)
		{
			OutEvents.Reset();
			OutError = TEXT("The binding package contains an invalid prepared delegate event.");
			return false;
		}

		FAvidScriptPreparedDelegateEvent& Event = OutEvents.AddDefaulted_GetRef();
		Event.EventOrdinal = Cell->EventOrdinal;
		Event.StableId = Cell->StableId;
		Event.ExportName = Cell->ExportName;
		Event.CallbackKind = Cell->CallbackKind;
		Event.HandlerMode = Cell->HandlerMode;
		Event.ExpectedSourceClass = Cell->ExpectedSourceClass;
		Event.Signature.Kind = Cell->Kind;
		Event.Signature.SinglecastProperty = Cell->SinglecastProperty;
		Event.Signature.MulticastProperty = Cell->MulticastProperty;
		Event.Signature.SignatureFunction = Cell->SignatureFunction;
		Event.RepNotifyProperty = Cell->RepNotifyProperty;
		Event.Network = Cell->Network;
		Event.Signature.ParameterCellCount = Cell->Codec.ParameterCellCount;
		Event.Signature.OutputValueCount =
			Cell->Codec.OutputParameterIndices.Num()
			+ (Cell->Codec.bHasReturnValue ? 1 : 0);
		Event.Signature.ImmutableCodecIdentity = &Cell->Codec;
		Event.Signature.Encode = &EncodeAvidScriptPreparedDelegateEvent;
	}
	return true;
}

bool FAvidScriptBindingPackage::BuildPreparedInboundHandlers(
	TArray<FAvidScriptPreparedDelegateEvent>& OutHandlers,
	FString& OutError) const
{
	OutHandlers.Reset();
	OutError.Reset();
	OutHandlers.Reserve(GetInboundHandlerCount());
	for (const TUniquePtr<FAvidScriptPreparedDelegateEventCell>& Cell :
		Impl->PreparedDelegateEventCells)
	{
		if (Cell.IsValid()
			&& Cell->Kind != EAvidScriptPreparedDelegateKind::FunctionHandler)
		{
			continue;
		}
		const bool bKindValid = Cell.IsValid()
			&& (Cell->CallbackKind == TEXT("network_rpc")
					? Cell->Network.IsNetworked()
						&& Cell->RepNotifyProperty == nullptr
					: Cell->CallbackKind == TEXT("rep_notify")
						&& !Cell->Network.IsNetworked()
						&& Cell->RepNotifyProperty != nullptr);
		const bool bHandlerModeValid = Cell.IsValid()
			&& (Cell->HandlerMode == TEXT("replace")
				|| Cell->HandlerMode == TEXT("before")
				|| Cell->HandlerMode == TEXT("after"));
		if (!bKindValid
			|| !bHandlerModeValid
			|| Cell->EventOrdinal == MAX_uint32
			|| Cell->StableId.IsEmpty()
			|| Cell->ExportName.IsEmpty()
			|| Cell->ExpectedSourceClass == nullptr
			|| Cell->SinglecastProperty != nullptr
			|| Cell->MulticastProperty != nullptr
			|| Cell->SignatureFunction == nullptr
			|| Cell->Codec.ParameterCellCount > FAvidScriptVmCallFrame::MaxCells)
		{
			OutHandlers.Reset();
			OutError = TEXT("The binding package contains an invalid prepared inbound handler.");
			return false;
		}

		FAvidScriptPreparedDelegateEvent& Handler =
			OutHandlers.AddDefaulted_GetRef();
		Handler.EventOrdinal = Cell->EventOrdinal;
		Handler.StableId = Cell->StableId;
		Handler.ExportName = Cell->ExportName;
		Handler.CallbackKind = Cell->CallbackKind;
		Handler.HandlerMode = Cell->HandlerMode;
		Handler.ExpectedSourceClass = Cell->ExpectedSourceClass;
		Handler.Signature.Kind = Cell->Kind;
		Handler.Signature.SignatureFunction = Cell->SignatureFunction;
		Handler.RepNotifyProperty = Cell->RepNotifyProperty;
		Handler.Network = Cell->Network;
		Handler.Signature.ParameterCellCount = Cell->Codec.ParameterCellCount;
		Handler.Signature.OutputValueCount =
			Cell->Codec.OutputParameterIndices.Num()
			+ (Cell->Codec.bHasReturnValue ? 1 : 0);
		Handler.Signature.ImmutableCodecIdentity = &Cell->Codec;
		Handler.Signature.Encode = &EncodeAvidScriptPreparedDelegateEvent;
	}
	return true;
}

bool FAvidScriptBindingPackage::InvokePreparedReflectionI32Pair(
	const FAvidScriptPreparedReflectionBinding& Binding,
	UObject& Receiver,
	const int32 Left,
	const int32 Right,
	const FAvidScriptBindingInvocationContext& Context,
	int32& OutValue,
	FString& OutErrorCategory,
	FString& OutErrorDetails) const
{
	OutValue = 0;
	OutErrorCategory.Reset();
	OutErrorDetails.Reset();
	if (Binding.ImmutablePlanIdentity == nullptr
		|| Binding.ExpectedClass == nullptr
		|| Binding.NativeGuard == nullptr
		|| Binding.I32PairCall == nullptr)
	{
		OutErrorCategory = TEXT("binding_prepared_identity_mismatch");
		OutErrorDetails =
			TEXT("The prepared reflection binding has no immutable call cell.");
		return false;
	}

	bool bUseNative = false;
	bool bAdaptiveGuardRejected = false;
	EAvidScriptBindingInvocationMode ActualMode =
		EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	const bool bAdaptiveRequested =
		Context.InvocationPolicy
			== EAvidScriptBindingInvocationPolicy::AdaptiveSemantic
		&& Binding.bAdaptiveNativeEligible;
	const bool bQualifiedRequested =
		Context.InvocationPolicy
			== EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect
		&& Binding.bQualifiedNativeEligible;
	if (bAdaptiveRequested || bQualifiedRequested)
	{
		bUseNative = Binding.NativeGuard(
			Binding.ImmutablePlanIdentity,
			Receiver);
		bAdaptiveGuardRejected = bAdaptiveRequested && !bUseNative;
		if (bQualifiedRequested && !bUseNative)
		{
			OutErrorCategory = TEXT("binding_prepared_native_guard_rejected");
			OutErrorDetails =
				TEXT("The qualified prepared reflection native guard rejected the receiver.");
			return false;
		}
		if (bUseNative)
		{
			ActualMode = bQualifiedRequested
				? EAvidScriptBindingInvocationMode::QualifiedNativeDirect
				: EAvidScriptBindingInvocationMode::AdaptivePreparedNative;
		}
	}
	if (!bUseNative
		&& !UE::AvidScript::BindingPrivate::IsObjectCompatibleWithReflectedType(
			&Receiver,
			Binding.ExpectedClass))
	{
		OutErrorCategory = TEXT("binding_prepared_identity_mismatch");
		OutErrorDetails =
			TEXT("The prepared reflection call-site no longer matches its immutable plan.");
		return false;
	}

	if (!Binding.I32PairCall(
			Binding.ImmutablePlanIdentity,
			Receiver,
			Left,
			Right,
			bUseNative,
			OutValue,
			OutErrorCategory,
			OutErrorDetails))
	{
		return false;
	}

	FAvidScriptBindingInvocationInstrumentation* Instrumentation =
		Context.InvocationInstrumentation;
	if (Instrumentation == nullptr)
	{
		return true;
	}
	if (ActualMode
		== EAvidScriptBindingInvocationMode::AdaptivePreparedNative)
	{
		++Instrumentation->AdaptivePreparedNativeHitCount;
	}
	else if (ActualMode
		== EAvidScriptBindingInvocationMode::QualifiedNativeDirect)
	{
		++Instrumentation->QualifiedNativeDirectCount;
	}
	else
	{
		++Instrumentation->SemanticProcessEventCount;
		if (Context.InvocationPolicy
			== EAvidScriptBindingInvocationPolicy::AdaptiveSemantic)
		{
			++Instrumentation->AdaptiveProcessEventFallbackCount;
			if (bAdaptiveGuardRejected)
			{
				++Instrumentation->AdaptiveGuardRejectCount;
			}
		}
		else if (Context.InvocationPolicy
			== EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect)
		{
			++Instrumentation->RequestedNativeDirectFallbackCount;
		}
	}
	return true;
}

bool FAvidScriptBindingPackage::TryGetGeneratedBinding(
	const uint32 Ordinal,
	const FAvidScriptGeneratedBindingEntry*& OutEntry,
	UClass*& OutExpectedClass,
	bool& bOutPropertyWrite,
	bool& bOutRequiresWriteAccess) const
{
	OutEntry = nullptr;
	OutExpectedClass = nullptr;
	bOutPropertyWrite = false;
	bOutRequiresWriteAccess = false;
	if (!IsInGameThread()
		|| !Impl->Plans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	const FAvidScriptRuntimeBindingInvocationPlan& Plan = Impl->Plans[Ordinal];
	if (Plan.GeneratedEntry == nullptr
		|| Plan.GeneratedLease.GetEntry() != Plan.GeneratedEntry)
	{
		return false;
	}
	OutEntry = Plan.GeneratedEntry;
	OutExpectedClass = Plan.OwnerClass;
	bOutPropertyWrite =
		Plan.Kind == EAvidScriptBindingInvocationKind::ReflectedPropertyWrite;
	bOutRequiresWriteAccess = Plan.bRequiresWriteAccess;
	return true;
}

bool FAvidScriptBindingPackage::PrepareGeneratedHostEffect(
	const uint32 Ordinal,
	const FAvidScriptObjectHandle& ReceiverHandle,
	UObject& Receiver,
	const FAvidScriptBindingInvocationContext& Context) const
{
	if (!Impl->Plans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	const FAvidScriptRuntimeBindingInvocationPlan& Plan = Impl->Plans[Ordinal];
	FAvidScriptPreparedGeneratedBinding Binding;
	Binding.BindingOrdinal = Ordinal;
	Binding.Lease = Plan.GeneratedLease;
	Binding.Entry = Plan.GeneratedEntry;
	Binding.ExpectedClass = Plan.OwnerClass;
	Binding.ReflectedProperty = Plan.ReflectedProperty;
	Binding.ReloadEffect = Plan.ReloadEffect;
	Binding.bPropertyWrite =
		Plan.Kind == EAvidScriptBindingInvocationKind::ReflectedPropertyWrite;
	Binding.bPropertyWriteHasFunction =
		Binding.bPropertyWrite && Plan.Function != nullptr;
	Binding.bRequiresWriteAccess = Plan.bRequiresWriteAccess;
	return PrepareGeneratedHostEffect(
		Binding,
		ReceiverHandle,
		Receiver,
		Context);
}

bool FAvidScriptBindingPackage::PrepareGeneratedHostEffect(
	const FAvidScriptPreparedGeneratedBinding& Binding,
	const FAvidScriptObjectHandle& ReceiverHandle,
	UObject& Receiver,
	const FAvidScriptBindingInvocationContext& Context) const
{
	const EAvidScriptPreparedHostEffectMode EffectMode =
		ResolvePreparedHostEffectMode(Binding, Context);
	if (EffectMode == EAvidScriptPreparedHostEffectMode::Rejected)
	{
		return false;
	}
	if (EffectMode != EAvidScriptPreparedHostEffectMode::Journaled)
	{
		return true;
	}

	FAvidScriptBindingHostEffectPrepareResult PrepareResult;
	if (Binding.bPropertyWrite)
	{
		return Binding.ReloadEffect
				== EAvidScriptBindingReloadEffect::ReflectedProperty
			&& Binding.ReflectedProperty != nullptr
			&& Context.HostEffectJournal->PrepareReflectedProperty(
				*Context.ObjectRegistry,
				ReceiverHandle,
				Receiver,
				*Binding.ReflectedProperty,
				PrepareResult);
	}
	return Context.HostEffectJournal->PrepareEffect(
		*Context.ObjectRegistry,
		ReceiverHandle,
		Receiver,
		Binding.ReloadEffect,
		PrepareResult);
}

EAvidScriptPreparedHostEffectMode
FAvidScriptBindingPackage::ResolvePreparedHostEffectMode(
	const FAvidScriptPreparedGeneratedBinding& Binding,
	const FAvidScriptBindingInvocationContext& Context) const
{
	if (Binding.Entry == nullptr
		|| Binding.Lease.GetEntry() != Binding.Entry)
	{
		return EAvidScriptPreparedHostEffectMode::Rejected;
	}
	if (Binding.bRequiresWriteAccess
		&& Context.WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		return EAvidScriptPreparedHostEffectMode::Rejected;
	}
	if (!Binding.bRequiresWriteAccess)
	{
		return EAvidScriptPreparedHostEffectMode::DirectRead;
	}
	if (Context.HostEffectJournal == nullptr)
	{
		return EAvidScriptPreparedHostEffectMode::DirectWrite;
	}
	if (Context.ObjectRegistry == nullptr
		|| Binding.ReloadEffect
			== EAvidScriptBindingReloadEffect::Unsupported
		|| Binding.bPropertyWriteHasFunction
		|| (Binding.bPropertyWrite
			&& (Binding.ReloadEffect
					!= EAvidScriptBindingReloadEffect::ReflectedProperty
				|| Binding.ReflectedProperty == nullptr)))
	{
		return EAvidScriptPreparedHostEffectMode::Rejected;
	}
	return EAvidScriptPreparedHostEffectMode::Journaled;
}

bool FAvidScriptBindingPackage::TryGetGeneratedPropertyBinding(
	const uint32 Ordinal,
	const FAvidScriptGeneratedBindingEntry*& OutEntry,
	UClass*& OutExpectedClass,
	FProperty*& OutProperty,
	bool& bOutRequiresWriteAccess) const
{
	OutEntry = nullptr;
	OutExpectedClass = nullptr;
	OutProperty = nullptr;
	bOutRequiresWriteAccess = false;
	if (!IsInGameThread()
		|| !Impl->Plans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}

	const FAvidScriptRuntimeBindingInvocationPlan& Plan = Impl->Plans[Ordinal];
	if (Plan.GeneratedEntry == nullptr
		|| Plan.GeneratedLease.GetEntry() != Plan.GeneratedEntry
		|| Plan.Kind
			!= EAvidScriptBindingInvocationKind::ReflectedPropertyWrite
		|| Plan.ReflectedProperty == nullptr)
	{
		return false;
	}

	OutEntry = Plan.GeneratedEntry;
	OutExpectedClass = Plan.OwnerClass;
	OutProperty = Plan.ReflectedProperty;
	bOutRequiresWriteAccess = Plan.bRequiresWriteAccess;
	return true;
}

bool FAvidScriptBindingPackage::TryFindFunctionOrdinal(
	const UClass& OwnerClass,
	const FName FunctionName,
	uint32& OutOrdinal) const
{
	OutOrdinal = MAX_uint32;
	for (int32 Index = 0; Index < Impl->Plans.Num(); ++Index)
	{
		const FAvidScriptRuntimeBindingInvocationPlan& Plan = Impl->Plans[Index];
		if (Plan.OwnerClass != &OwnerClass
			|| Plan.Function == nullptr
			|| Plan.Function->GetFName() != FunctionName)
		{
			continue;
		}
		if (OutOrdinal != MAX_uint32)
		{
			OutOrdinal = MAX_uint32;
			return false;
		}
		OutOrdinal = static_cast<uint32>(Index);
	}
	return OutOrdinal != MAX_uint32;
}

bool FAvidScriptBindingPackage::TryGetLatentCompletionResultType(
	const uint32 Ordinal,
	const FAvidScriptBindingTypeModel*& OutType) const
{
	OutType = nullptr;
	if (!Impl->LatentResultTypes.IsValidIndex(static_cast<int32>(Ordinal))
		|| !Impl->LatentResultTypes[Ordinal].IsSet())
	{
		return false;
	}
	OutType = &Impl->LatentResultTypes[Ordinal].GetValue();
	return true;
}

int32 FAvidScriptBindingPackage::GetRequiredScratchSize() const
{
	return Impl->RequiredScratchSize;
}

int32 FAvidScriptBindingPackage::GetObjectTypeCount() const
{
	return Impl->ObjectTypePlans.Num();
}

bool FAvidScriptBindingPackage::TryResolveObjectType(
	const uint32 Ordinal,
	UClass*& OutClass) const
{
	OutClass = nullptr;
	if (!Impl->ObjectTypePlans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	OutClass = Impl->ObjectTypePlans[static_cast<int32>(Ordinal)];
	return OutClass != nullptr;
}

UClass* FAvidScriptBindingPackage::GetExpectedSelfClass() const
{
	return Impl->ExpectedSelfClass;
}

int32 FAvidScriptBindingPackage::GetClassReferenceCount() const
{
	return Impl->ClassReferencePlans.Num();
}

bool FAvidScriptBindingPackage::TryResolveClassReference(
	const uint32 Ordinal,
	UClass*& OutClass,
	UClass*& OutBaseClass) const
{
	OutClass = nullptr;
	OutBaseClass = nullptr;
	if (!Impl->ClassReferencePlans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	const FImpl::FClassReferencePlan& Plan =
		Impl->ClassReferencePlans[static_cast<int32>(Ordinal)];
	OutClass = Plan.Class;
	OutBaseClass = Plan.BaseClass;
	return OutClass != nullptr && OutBaseClass != nullptr;
}

int32 FAvidScriptBindingPackage::GetObjectFactoryCount() const
{
	return Impl->ObjectFactoryPlans.Num();
}

bool FAvidScriptBindingPackage::TryResolveObjectFactory(
	const uint32 Ordinal,
	const FAvidScriptObjectFactoryPlan*& OutPlan) const
{
	OutPlan = nullptr;
	if (!Impl->ObjectFactoryPlans.IsValidIndex(static_cast<int32>(Ordinal)))
	{
		return false;
	}
	OutPlan = &Impl->ObjectFactoryPlans[static_cast<int32>(Ordinal)];
	return OutPlan->ObjectClass != nullptr
		&& OutPlan->RequiredOuterClass != nullptr
		&& OutPlan->ResultObjectTypeOrdinal != INDEX_NONE;
}

namespace
{
const FAvidScriptRuntimeBindingValuePlan* FindAvidScriptCompositeAccessPlan(
	const FString& TypeId,
	const FProperty* Property,
	const TMap<const FProperty*, FAvidScriptRuntimeBindingValuePlan>&
		PlansByProperty,
	FString& OutError)
{
	const FAvidScriptRuntimeBindingValuePlan* Plan =
		Property == nullptr ? nullptr : PlansByProperty.Find(Property);
	if (Plan == nullptr
		|| Plan->Property != Property
		|| Plan->TypeId != TypeId
		|| (Plan->Kind != EAvidScriptRuntimeBindingKind::CompositeArray
			&& Plan->Kind != EAvidScriptRuntimeBindingKind::Set
			&& Plan->Kind != EAvidScriptRuntimeBindingKind::Map))
	{
		OutError = TEXT("composite_access_plan_missing: the activated package has no matching recursive container plan.");
		return nullptr;
	}
	return Plan;
}

bool IsAvidScriptCompositeContainerKind(
	const EAvidScriptCompositeValueKind Kind)
{
	return Kind == EAvidScriptCompositeValueKind::Array
		|| Kind == EAvidScriptCompositeValueKind::Set
		|| Kind == EAvidScriptCompositeValueKind::Map;
}

void AppendAvidScriptCanonicalUnsigned(
	const uint64 Value,
	const int32 Width,
	TArray<uint8>& OutBytes)
{
	for (int32 Shift = (Width - 1) * 8; Shift >= 0; Shift -= 8)
	{
		OutBytes.Add(static_cast<uint8>((Value >> Shift) & 0xffu));
	}
}

bool EncodeAvidScriptCanonicalKey(
	const FAvidScriptRuntimeBindingValuePlan& Plan,
	const void* Value,
	TArray<uint8>& OutBytes,
	FString& OutError)
{
	OutBytes.Reset();
	if (Plan.Property == nullptr || Value == nullptr)
	{
		OutError = TEXT("composite_container_key_invalid: the canonical key value is unavailable.");
		return false;
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Bool)
	{
		const FBoolProperty* Property = CastField<FBoolProperty>(Plan.Property);
		if (Property == nullptr)
		{
			OutError = TEXT("composite_container_key_invalid: the bool key property is unavailable.");
			return false;
		}
		OutBytes.Add(Property->GetPropertyValue(Value) ? 1u : 0u);
		return true;
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Name
		|| Plan.Kind == EAvidScriptRuntimeBindingKind::String)
	{
		FString Text;
		if (Plan.Kind == EAvidScriptRuntimeBindingKind::Name)
		{
			const FNameProperty* Property =
				CastField<FNameProperty>(Plan.Property);
			if (Property == nullptr)
			{
				OutError = TEXT("composite_container_key_invalid: the name key property is unavailable.");
				return false;
			}
			Text = Property->GetPropertyValue(Value).ToString().ToLower();
		}
		else
		{
			const FStrProperty* Property = CastField<FStrProperty>(Plan.Property);
			if (Property == nullptr)
			{
				OutError = TEXT("composite_container_key_invalid: the string key property is unavailable.");
				return false;
			}
			Text = Property->GetPropertyValue(Value);
		}
		const FTCHARToUTF8 Utf8(*Text);
		if (Utf8.Length() > 0)
		{
			OutBytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
		}
		return true;
	}

	const FNumericProperty* NumericProperty =
		CastField<FNumericProperty>(Plan.Property);
	if (const FEnumProperty* EnumProperty =
		CastField<FEnumProperty>(Plan.Property))
	{
		NumericProperty = EnumProperty->GetUnderlyingProperty();
	}
	if (NumericProperty == nullptr
		|| !NumericProperty->IsInteger()
		|| NumericProperty->IsFloatingPoint())
	{
		OutError = TEXT("composite_container_key_unsupported: the key has no deterministic canonical wire encoding.");
		return false;
	}
	const int32 Width = NumericProperty->GetSize();
	if (Width != 1 && Width != 2 && Width != 4 && Width != 8)
	{
		OutError = TEXT("composite_container_key_unsupported: the integer key width is unsupported.");
		return false;
	}
	const bool bUnsigned = NumericProperty->IsA<FByteProperty>()
		|| NumericProperty->IsA<FUInt16Property>()
		|| NumericProperty->IsA<FUInt32Property>()
		|| NumericProperty->IsA<FUInt64Property>();
	const uint64 IntegerValue = bUnsigned
		? NumericProperty->GetUnsignedIntPropertyValue(Value)
		: static_cast<uint64>(NumericProperty->GetSignedIntPropertyValue(Value));
	AppendAvidScriptCanonicalUnsigned(IntegerValue, Width, OutBytes);
	return true;
}

int32 CompareAvidScriptCanonicalBytes(
	const TArray<uint8>& Left,
	const TArray<uint8>& Right)
{
	const int32 SharedSize = FMath::Min(Left.Num(), Right.Num());
	if (SharedSize > 0)
	{
		const int32 Comparison = FMemory::Memcmp(
			Left.GetData(),
			Right.GetData(),
			SharedSize);
		if (Comparison != 0)
		{
			return Comparison;
		}
	}
	return Left.Num() < Right.Num() ? -1 : (Left.Num() > Right.Num() ? 1 : 0);
}

struct FAvidScriptCanonicalSnapshotEntry
{
	TArray<uint8> KeyBytes;
	int32 InternalIndex = INDEX_NONE;
};

struct FScopedAvidScriptPropertyContainer
{
	FProperty* Property = nullptr;
	TArray<uint8> Storage;
	void* Container = nullptr;
	void* Value = nullptr;

	~FScopedAvidScriptPropertyContainer()
	{
		if (Property != nullptr && Value != nullptr)
		{
			Property->DestroyValue(Value);
		}
	}

	bool Initialize(
		const FAvidScriptRuntimeBindingValuePlan& Plan,
		FString& OutError)
	{
		Property = Plan.Property;
		if (Property == nullptr)
		{
			OutError = TEXT("composite_container_value_invalid: the reflected child property is unavailable.");
			return false;
		}
		const int32 Offset = Property->GetOffset_ForInternal();
		const int32 ValueSize = Property->GetSize();
		const int32 Alignment = FMath::Max(1, Property->GetMinAlignment());
		if (Offset < 0 || ValueSize <= 0 || !FMath::IsPowerOfTwo(Alignment))
		{
			OutError = TEXT("composite_container_value_invalid: the reflected child layout is invalid.");
			return false;
		}
		Storage.SetNumZeroed(Offset + ValueSize + Alignment - 1);
		Container = reinterpret_cast<void*>(Align(
			reinterpret_cast<UPTRINT>(Storage.GetData()),
			static_cast<UPTRINT>(Alignment)));
		Value = Property->ContainerPtrToValuePtr<void>(Container);
		if (Value == nullptr)
		{
			OutError = TEXT("composite_container_value_invalid: the reflected child address is invalid.");
			return false;
		}
		Property->InitializeValue(Value);
		return true;
	}

	bool InitializeCopy(
		const FAvidScriptRuntimeBindingValuePlan& Plan,
		const void* SourceValue,
		FString& OutError)
	{
		if (SourceValue == nullptr || !Initialize(Plan, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("composite_container_value_invalid: the source value is unavailable.");
			}
			return false;
		}
		Property->CopyCompleteValue(Value, SourceValue);
		return true;
	}
};

bool GetAvidScriptSetCanonicalSnapshot(
	FAvidScriptCompositeValueHeap& Heap,
	const uint32 Token,
	FScriptSetHelper& Helper,
	const FAvidScriptRuntimeBindingValuePlan& ElementPlan,
	TConstArrayView<int32>& OutIndices,
	FString& OutError)
{
	if (Heap.TryGetCanonicalSnapshot(Token, OutIndices)
		&& OutIndices.Num() == Helper.Num())
	{
		return true;
	}
	Heap.InvalidateCanonicalSnapshot(Token);
	TArray<FAvidScriptCanonicalSnapshotEntry> Ordered;
	Ordered.Reserve(Helper.Num());
	for (int32 InternalIndex = 0; InternalIndex < Helper.GetMaxIndex(); ++InternalIndex)
	{
		if (!Helper.IsValidIndex(InternalIndex))
		{
			continue;
		}
		FAvidScriptCanonicalSnapshotEntry& Entry =
			Ordered.AddDefaulted_GetRef();
		Entry.InternalIndex = InternalIndex;
		if (!EncodeAvidScriptCanonicalKey(
				ElementPlan,
				Helper.GetElementPtr(InternalIndex),
				Entry.KeyBytes,
				OutError))
		{
			return false;
		}
	}
	Ordered.Sort([](
		const FAvidScriptCanonicalSnapshotEntry& Left,
		const FAvidScriptCanonicalSnapshotEntry& Right)
	{
		const int32 Comparison = CompareAvidScriptCanonicalBytes(
			Left.KeyBytes,
			Right.KeyBytes);
		return Comparison == 0
			? Left.InternalIndex < Right.InternalIndex
			: Comparison < 0;
	});
	for (int32 Index = 1; Index < Ordered.Num(); ++Index)
	{
		if (CompareAvidScriptCanonicalBytes(
			Ordered[Index - 1].KeyBytes,
			Ordered[Index].KeyBytes) == 0)
		{
			OutError = TEXT("composite_container_key_collision: a set contains duplicate canonical key bytes.");
			return false;
		}
	}
	TArray<int32> StoredIndices;
	StoredIndices.Reserve(Ordered.Num());
	for (const FAvidScriptCanonicalSnapshotEntry& Entry : Ordered)
	{
		StoredIndices.Add(Entry.InternalIndex);
	}
	return Heap.StoreCanonicalSnapshot(Token, MoveTemp(StoredIndices), OutError)
		&& Heap.TryGetCanonicalSnapshot(Token, OutIndices);
}

bool GetAvidScriptMapCanonicalSnapshot(
	FAvidScriptCompositeValueHeap& Heap,
	const uint32 Token,
	FScriptMapHelper& Helper,
	const FAvidScriptRuntimeBindingValuePlan& KeyPlan,
	TConstArrayView<int32>& OutIndices,
	FString& OutError)
{
	if (Heap.TryGetCanonicalSnapshot(Token, OutIndices)
		&& OutIndices.Num() == Helper.Num())
	{
		return true;
	}
	Heap.InvalidateCanonicalSnapshot(Token);
	TArray<FAvidScriptCanonicalSnapshotEntry> Ordered;
	Ordered.Reserve(Helper.Num());
	for (int32 InternalIndex = 0; InternalIndex < Helper.GetMaxIndex(); ++InternalIndex)
	{
		if (!Helper.IsValidIndex(InternalIndex))
		{
			continue;
		}
		FAvidScriptCanonicalSnapshotEntry& Entry =
			Ordered.AddDefaulted_GetRef();
		Entry.InternalIndex = InternalIndex;
		if (!EncodeAvidScriptCanonicalKey(
				KeyPlan,
				Helper.GetKeyPtr(InternalIndex),
				Entry.KeyBytes,
				OutError))
		{
			return false;
		}
	}
	Ordered.Sort([](
		const FAvidScriptCanonicalSnapshotEntry& Left,
		const FAvidScriptCanonicalSnapshotEntry& Right)
	{
		const int32 Comparison = CompareAvidScriptCanonicalBytes(
			Left.KeyBytes,
			Right.KeyBytes);
		return Comparison == 0
			? Left.InternalIndex < Right.InternalIndex
			: Comparison < 0;
	});
	for (int32 Index = 1; Index < Ordered.Num(); ++Index)
	{
		if (CompareAvidScriptCanonicalBytes(
			Ordered[Index - 1].KeyBytes,
			Ordered[Index].KeyBytes) == 0)
		{
			OutError = TEXT("composite_container_key_collision: a map contains duplicate canonical key bytes.");
			return false;
		}
	}
	TArray<int32> StoredIndices;
	StoredIndices.Reserve(Ordered.Num());
	for (const FAvidScriptCanonicalSnapshotEntry& Entry : Ordered)
	{
		StoredIndices.Add(Entry.InternalIndex);
	}
	return Heap.StoreCanonicalSnapshot(Token, MoveTemp(StoredIndices), OutError)
		&& Heap.TryGetCanonicalSnapshot(Token, OutIndices);
}

bool ResolveAvidScriptCompositeContainerElement(
	const FAvidScriptRuntimeBindingValuePlan& Plan,
	FAvidScriptCompositeValueHeap& Heap,
	const uint32 Token,
	void* ContainerValue,
	const int32 Index,
	const int32 Lane,
	const FAvidScriptRuntimeBindingValuePlan*& OutElementPlan,
	void*& OutElementValue,
	FString& OutError)
{
	OutElementPlan = nullptr;
	OutElementValue = nullptr;
	if (ContainerValue == nullptr || Index < 0 || Lane < 0)
	{
		OutError = TEXT("composite_container_index_invalid: the requested index or lane is invalid.");
		return false;
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::CompositeArray)
	{
		FArrayProperty* Property = CastField<FArrayProperty>(Plan.Property);
		if (Property == nullptr || Plan.Children.Num() != 1
			|| Lane != 0)
		{
			OutError = TEXT("composite_array_index_invalid: the requested array element is unavailable.");
			return false;
		}
		FScriptArrayHelper Helper(Property, ContainerValue);
		if (!Helper.IsValidIndex(Index))
		{
			OutError = TEXT("composite_array_index_invalid: the requested array element is unavailable.");
			return false;
		}
		OutElementPlan = &Plan.Children[0];
		OutElementValue = Helper.GetRawPtr(Index);
		return true;
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Set)
	{
		FSetProperty* Property = CastField<FSetProperty>(Plan.Property);
		if (Property == nullptr || Plan.Children.Num() != 1 || Lane != 0)
		{
			OutError = TEXT("composite_set_index_invalid: the requested set snapshot element is unavailable.");
			return false;
		}
		FScriptSetHelper Helper(Property, ContainerValue);
		TConstArrayView<int32> SnapshotIndices;
		if (!GetAvidScriptSetCanonicalSnapshot(
			Heap,
			Token,
			Helper,
			Plan.Children[0],
			SnapshotIndices,
			OutError)
			|| !SnapshotIndices.IsValidIndex(Index))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("composite_set_index_invalid: the requested set snapshot element is unavailable.");
			}
			return false;
		}
		const int32 InternalIndex = SnapshotIndices[Index];
		OutElementPlan = &Plan.Children[0];
		OutElementValue = Helper.GetElementPtr(InternalIndex);
		return true;
	}
	if (Plan.Kind == EAvidScriptRuntimeBindingKind::Map)
	{
		FMapProperty* Property = CastField<FMapProperty>(Plan.Property);
		if (Property == nullptr || Plan.Children.Num() != 2 || Lane > 1)
		{
			OutError = TEXT("composite_map_index_invalid: the requested map snapshot entry is unavailable.");
			return false;
		}
		FScriptMapHelper Helper(Property, ContainerValue);
		TConstArrayView<int32> SnapshotIndices;
		if (!GetAvidScriptMapCanonicalSnapshot(
			Heap,
			Token,
			Helper,
			Plan.Children[0],
			SnapshotIndices,
			OutError)
			|| !SnapshotIndices.IsValidIndex(Index))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("composite_map_index_invalid: the requested map snapshot entry is unavailable.");
			}
			return false;
		}
		const int32 InternalIndex = SnapshotIndices[Index];
		OutElementPlan = &Plan.Children[Lane];
		OutElementValue = Helper.GetPairPtr(InternalIndex);
		return true;
	}
	OutError = TEXT("composite_container_kind_invalid: the capability is not a recursive container.");
	return false;
}
} // namespace

struct FAvidScriptPreparedDelegateOutputTransaction::FImpl
{
	struct FOutputSlot
	{
		const FAvidScriptRuntimeBindingValuePlan* Plan = nullptr;
		TUniquePtr<FScopedAvidScriptPropertyContainer> Candidate;
		bool bStaged = false;
	};

	const FAvidScriptPreparedDelegateCodec* Codec = nullptr;
	void* NativeParameters = nullptr;
	TArray<FOutputSlot> Outputs;
	bool bCommitted = false;
};

const TCHAR* FAvidScriptPreparedDelegateOutputTransaction::GetImportStableId()
{
	return TEXT("avidscript.delegate_output_write.v1");
}

const TCHAR* FAvidScriptPreparedDelegateOutputTransaction::GetImportModuleName()
{
	return TEXT("avidscript");
}

const TCHAR* FAvidScriptPreparedDelegateOutputTransaction::GetImportName()
{
	return TEXT("avid_delegate_output_write");
}

const TCHAR* FAvidScriptPreparedDelegateOutputTransaction::GetImportSignature()
{
	return TEXT("(iii)i");
}

FAvidScriptPreparedDelegateOutputTransaction::
	FAvidScriptPreparedDelegateOutputTransaction(TUniquePtr<FImpl>&& InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FAvidScriptPreparedDelegateOutputTransaction::
	~FAvidScriptPreparedDelegateOutputTransaction() = default;

bool FAvidScriptPreparedDelegateOutputTransaction::StageOutput(
	const uint32 OutputOrdinal,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	FString& OutError)
{
	OutError.Reset();
	if (!Impl.IsValid() || Impl->bCommitted)
	{
		OutError = TEXT("delegate_output_transaction_inactive: the output transaction is unavailable.");
		return false;
	}
	if (!Impl->Outputs.IsValidIndex(static_cast<int32>(OutputOrdinal)))
	{
		OutError = TEXT("delegate_output_ordinal_invalid: the output ordinal is outside the prepared event contract.");
		return false;
	}
	if (GuestAddress == 0)
	{
		OutError = TEXT("delegate_output_address_invalid: the guest output address is zero.");
		return false;
	}

	FImpl::FOutputSlot& Slot = Impl->Outputs[OutputOrdinal];
	if (Slot.bStaged)
	{
		OutError = TEXT("delegate_output_duplicate: each prepared delegate output may be staged only once.");
		return false;
	}
	if (Slot.Plan == nullptr
		|| !Slot.Candidate.IsValid()
		|| Slot.Candidate->Container == nullptr
		|| !UE::AvidScript::BindingPrivate::SetValueFromGuest(
			*Slot.Plan,
			GuestAddress,
			GuestMemory,
			Context,
			Slot.Candidate->Container,
			OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("delegate_output_decode_failed: the guest output value does not satisfy the prepared event contract.");
		}
		return false;
	}
	Slot.bStaged = true;
	return true;
}

bool FAvidScriptPreparedDelegateOutputTransaction::IsComplete() const
{
	return Impl.IsValid()
		&& !Impl->bCommitted
		&& !Impl->Outputs.IsEmpty()
		&& !Impl->Outputs.ContainsByPredicate(
			[](const FImpl::FOutputSlot& Slot)
			{
				return !Slot.bStaged;
			});
}

bool FAvidScriptPreparedDelegateOutputTransaction::Commit(FString& OutError)
{
	OutError.Reset();
	if (!Impl.IsValid() || Impl->bCommitted)
	{
		OutError = TEXT("delegate_output_transaction_inactive: the output transaction cannot be committed.");
		return false;
	}
	if (!IsComplete())
	{
		OutError = TEXT("delegate_output_incomplete: every ref/out value must be staged before the callback returns.");
		return false;
	}
	if (Impl->NativeParameters == nullptr)
	{
		OutError = TEXT("delegate_output_native_frame_invalid: the native parameter frame is unavailable.");
		return false;
	}

	TArray<void*, TInlineAllocator<8>> Targets;
	Targets.Reserve(Impl->Outputs.Num());
	for (const FImpl::FOutputSlot& Slot : Impl->Outputs)
	{
		void* Target = Slot.Plan == nullptr || Slot.Plan->Property == nullptr
			? nullptr
			: Slot.Plan->Property->ContainerPtrToValuePtr<void>(Impl->NativeParameters);
		if (Target == nullptr
			|| !Slot.Candidate.IsValid()
			|| Slot.Candidate->Value == nullptr)
		{
			OutError = TEXT("delegate_output_native_frame_invalid: a prepared output destination is unavailable.");
			return false;
		}
		Targets.Add(Target);
	}

	for (int32 Index = 0; Index < Impl->Outputs.Num(); ++Index)
	{
		const FImpl::FOutputSlot& Slot = Impl->Outputs[Index];
		Slot.Plan->Property->CopyCompleteValue(
			Targets[Index],
			Slot.Candidate->Value);
	}
	Impl->bCommitted = true;
	return true;
}

bool FAvidScriptBindingPackage::BeginPreparedDelegateOutputTransaction(
	const FAvidScriptPreparedDelegateEvent& Event,
	void* NativeParameters,
	TUniquePtr<FAvidScriptPreparedDelegateOutputTransaction>& OutTransaction,
	FString& OutError) const
{
	OutTransaction.Reset();
	OutError.Reset();
	const FAvidScriptPreparedDelegateCodec* Codec =
		static_cast<const FAvidScriptPreparedDelegateCodec*>(
			Event.Signature.ImmutableCodecIdentity);
	if (Codec == nullptr
		|| NativeParameters == nullptr
		|| Event.StableId.IsEmpty()
		|| Codec->StableId != Event.StableId
		|| Event.Signature.OutputValueCount == 0
		|| Event.Signature.OutputValueCount
			!= static_cast<uint32>(Codec->OutputParameterIndices.Num())
				+ (Codec->bHasReturnValue ? 1u : 0u))
	{
		OutError = TEXT("delegate_output_contract_invalid: the prepared output contract is unavailable or stale.");
		return false;
	}

	TUniquePtr<FAvidScriptPreparedDelegateOutputTransaction::FImpl> TransactionImpl =
		MakeUnique<FAvidScriptPreparedDelegateOutputTransaction::FImpl>();
	TransactionImpl->Codec = Codec;
	TransactionImpl->NativeParameters = NativeParameters;
	TransactionImpl->Outputs.Reserve(Codec->OutputParameterIndices.Num());
	for (const int32 ParameterIndex : Codec->OutputParameterIndices)
	{
		if (!Codec->Parameters.IsValidIndex(ParameterIndex))
		{
			OutError = TEXT("delegate_output_contract_invalid: a prepared output parameter index is invalid.");
			return false;
		}
		const FAvidScriptRuntimeBindingValuePlan& Plan =
			Codec->Parameters[ParameterIndex];
		if ((Plan.Direction != EAvidScriptRuntimeBindingDirection::Ref
				&& Plan.Direction != EAvidScriptRuntimeBindingDirection::Out)
			|| Plan.Property == nullptr)
		{
			OutError = TEXT("delegate_output_contract_invalid: a prepared output parameter no longer has ref/out semantics.");
			return false;
		}

		FAvidScriptPreparedDelegateOutputTransaction::FImpl::FOutputSlot& Slot =
			TransactionImpl->Outputs.AddDefaulted_GetRef();
		Slot.Plan = &Plan;
		Slot.Candidate = MakeUnique<FScopedAvidScriptPropertyContainer>();
		if (!Slot.Candidate->Initialize(Plan, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("delegate_output_candidate_invalid: the temporary output value could not be initialized.");
			}
			return false;
		}
	}
	if (Codec->bHasReturnValue)
	{
		if (Codec->ReturnValue.Direction
				!= EAvidScriptRuntimeBindingDirection::Return
			|| Codec->ReturnValue.Property == nullptr)
		{
			OutError = TEXT("delegate_output_contract_invalid: the prepared return value no longer has return semantics.");
			return false;
		}

		FAvidScriptPreparedDelegateOutputTransaction::FImpl::FOutputSlot& Slot =
			TransactionImpl->Outputs.AddDefaulted_GetRef();
		Slot.Plan = &Codec->ReturnValue;
		Slot.Candidate = MakeUnique<FScopedAvidScriptPropertyContainer>();
		if (!Slot.Candidate->Initialize(Codec->ReturnValue, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("delegate_output_candidate_invalid: the temporary return value could not be initialized.");
			}
			return false;
		}
	}

	OutTransaction = TUniquePtr<FAvidScriptPreparedDelegateOutputTransaction>(
		new FAvidScriptPreparedDelegateOutputTransaction(
			MoveTemp(TransactionImpl)));
	return true;
}

bool FAvidScriptBindingPackage::GetCompositeContainerCount(
	const uint32 Token,
	const FAvidScriptBindingInvocationContext& Context,
	int32& OutCount,
	FString& OutError) const
{
	OutCount = 0;
	FAvidScriptCompositeValueView Value;
	if (Context.CompositeValueHeap == nullptr
		|| !Context.CompositeValueHeap->Resolve(
			Token,
			FString(),
			nullptr,
			Value,
			OutError)
		|| !IsAvidScriptCompositeContainerKind(Value.Kind)
		|| Value.Property == nullptr
		|| Value.Value == nullptr)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_container_invalid: the capability is not a live recursive container.");
		}
		return false;
	}
	const FAvidScriptRuntimeBindingValuePlan* Plan =
		FindAvidScriptCompositeAccessPlan(
			Value.TypeId,
			Value.Property,
			Impl->CompositeAccessPlansByProperty,
			OutError);
	if (Plan == nullptr)
	{
		return false;
	}
	if (Plan->Kind == EAvidScriptRuntimeBindingKind::CompositeArray)
	{
		OutCount = FScriptArrayHelper(
			CastFieldChecked<FArrayProperty>(Plan->Property),
			Value.Value).Num();
	}
	else if (Plan->Kind == EAvidScriptRuntimeBindingKind::Set)
	{
		OutCount = FScriptSetHelper(
			CastFieldChecked<FSetProperty>(Plan->Property),
			Value.Value).Num();
	}
	else if (Plan->Kind == EAvidScriptRuntimeBindingKind::Map)
	{
		OutCount = FScriptMapHelper(
			CastFieldChecked<FMapProperty>(Plan->Property),
			Value.Value).Num();
	}
	else
	{
		OutError = TEXT("composite_container_kind_invalid: the descriptor type is not a recursive container.");
		return false;
	}
	return OutCount >= 0 && OutCount <= FAvidScriptCompositeValueHeap::MaxChildValues;
}

bool FAvidScriptBindingPackage::ReadCompositeContainerValue(
	const uint32 Token,
	const int32 Index,
	const int32 Lane,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	FString& OutError) const
{
	FAvidScriptCompositeValueView Value;
	const FAvidScriptRuntimeBindingValuePlan* Plan = nullptr;
	const FAvidScriptRuntimeBindingValuePlan* ElementPlan = nullptr;
	void* ElementContainer = nullptr;
	if (Context.CompositeValueHeap == nullptr
		|| !Context.CompositeValueHeap->Resolve(Token, FString(), nullptr, Value, OutError)
		|| Value.Property == nullptr
		|| (Plan = FindAvidScriptCompositeAccessPlan(
				Value.TypeId,
				Value.Property,
				Impl->CompositeAccessPlansByProperty,
				OutError)) == nullptr
		|| !ResolveAvidScriptCompositeContainerElement(
			*Plan,
			*Context.CompositeValueHeap,
			Token,
			const_cast<void*>(Value.Value),
			Index,
			Lane,
			ElementPlan,
			ElementContainer,
			OutError))
	{
		return false;
	}
	using namespace UE::AvidScript::BindingPrivate;
	FCodecOutputTransaction Transaction;
	FPreparedValueOutput PreparedOutput;
	if (!PreflightValueOutput(
			*ElementPlan,
			GuestAddress,
			GuestMemory,
			Context,
			Transaction,
			PreparedOutput,
			OutError)
		|| !WriteValueToGuest(
			*ElementPlan,
			Context,
			ElementContainer,
			Transaction,
			PreparedOutput,
			OutError))
	{
		Transaction.Rollback(Context);
		return false;
	}
	PublishValueOutput(PreparedOutput);
	Transaction.Commit();
	return true;
}

bool FAvidScriptBindingPackage::WriteCompositeContainerValue(
	const uint32 Token,
	const int32 Index,
	const int32 Lane,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	FString& OutError) const
{
	FAvidScriptMutableCompositeValueView Value;
	const FAvidScriptRuntimeBindingValuePlan* Plan = nullptr;
	if (Context.CompositeValueHeap == nullptr
		|| !Context.CompositeValueHeap->ResolveMutable(Token, Value, OutError)
		|| Value.Property == nullptr
		|| (Plan = FindAvidScriptCompositeAccessPlan(
				Value.TypeId,
				Value.Property,
				Impl->CompositeAccessPlansByProperty,
				OutError)) == nullptr)
	{
		return false;
	}
	if (Plan->Kind == EAvidScriptRuntimeBindingKind::Set
		|| (Plan->Kind == EAvidScriptRuntimeBindingKind::Map && Lane == 0))
	{
		OutError = TEXT("composite_container_key_immutable: set elements and map keys must be changed through add/remove operations.");
		return false;
	}
	FScopedAvidScriptPropertyContainer Candidate;
	const FAvidScriptRuntimeBindingValuePlan* ElementPlan = nullptr;
	void* ElementContainer = nullptr;
	if (!Candidate.InitializeCopy(*Plan, Value.Value, OutError)
		|| !ResolveAvidScriptCompositeContainerElement(
			*Plan,
			*Context.CompositeValueHeap,
			Token,
			Candidate.Value,
			Index,
			Lane,
			ElementPlan,
			ElementContainer,
			OutError))
	{
		return false;
	}
	FScopedAvidScriptPropertyContainer TemporaryValue;
	if (!TemporaryValue.Initialize(*ElementPlan, OutError))
	{
		return false;
	}
	if (!UE::AvidScript::BindingPrivate::SetValueFromGuest(
			*ElementPlan,
			GuestAddress,
			GuestMemory,
			Context,
			TemporaryValue.Container,
			OutError))
	{
		return false;
	}
	void* ElementValue = ElementPlan->Property->ContainerPtrToValuePtr<void>(
		ElementContainer);
	ElementPlan->Property->CopyCompleteValue(
		ElementValue,
		TemporaryValue.Value);
	return Context.CompositeValueHeap->ReplaceValue(
		Token,
		*Plan->Property,
		Candidate.Value,
		OutError);
}

bool FAvidScriptBindingPackage::ResizeCompositeArray(
	const uint32 Token,
	const int32 NewCount,
	const FAvidScriptBindingInvocationContext& Context,
	FString& OutError) const
{
	FAvidScriptMutableCompositeValueView Value;
	const FAvidScriptRuntimeBindingValuePlan* Plan = nullptr;
	if (NewCount < 0 || NewCount > FAvidScriptCompositeValueHeap::MaxChildValues
		|| Context.CompositeValueHeap == nullptr
		|| !Context.CompositeValueHeap->ResolveMutable(Token, Value, OutError)
		|| (Plan = FindAvidScriptCompositeAccessPlan(
				Value.TypeId,
				Value.Property,
				Impl->CompositeAccessPlansByProperty,
				OutError)) == nullptr
		|| Plan->Kind != EAvidScriptRuntimeBindingKind::CompositeArray)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_array_resize_invalid: the requested size or capability is invalid.");
		}
		return false;
	}
	FScopedAvidScriptPropertyContainer Candidate;
	if (!Candidate.InitializeCopy(*Plan, Value.Value, OutError))
	{
		return false;
	}
	FScriptArrayHelper(
		CastFieldChecked<FArrayProperty>(Plan->Property),
		Candidate.Value).Resize(NewCount);
	return Context.CompositeValueHeap->ReplaceValue(
		Token,
		*Plan->Property,
		Candidate.Value,
		OutError);
}

bool FAvidScriptBindingPackage::ClearCompositeContainer(
	const uint32 Token,
	const FAvidScriptBindingInvocationContext& Context,
	FString& OutError) const
{
	FAvidScriptMutableCompositeValueView Value;
	const FAvidScriptRuntimeBindingValuePlan* Plan = nullptr;
	if (Context.CompositeValueHeap == nullptr
		|| !Context.CompositeValueHeap->ResolveMutable(Token, Value, OutError)
		|| (Plan = FindAvidScriptCompositeAccessPlan(
				Value.TypeId,
				Value.Property,
				Impl->CompositeAccessPlansByProperty,
				OutError)) == nullptr)
	{
		return false;
	}
	FScopedAvidScriptPropertyContainer Candidate;
	if (!Candidate.InitializeCopy(*Plan, Value.Value, OutError))
	{
		return false;
	}
	if (Plan->Kind == EAvidScriptRuntimeBindingKind::CompositeArray)
	{
		FScriptArrayHelper(CastFieldChecked<FArrayProperty>(Plan->Property), Candidate.Value).EmptyValues();
	}
	else if (Plan->Kind == EAvidScriptRuntimeBindingKind::Set)
	{
		FScriptSetHelper(CastFieldChecked<FSetProperty>(Plan->Property), Candidate.Value).EmptyElements();
	}
	else if (Plan->Kind == EAvidScriptRuntimeBindingKind::Map)
	{
		FScriptMapHelper(CastFieldChecked<FMapProperty>(Plan->Property), Candidate.Value).EmptyValues();
	}
	else
	{
		OutError = TEXT("composite_container_kind_invalid: the capability is not a recursive container.");
		return false;
	}
	return Context.CompositeValueHeap->ReplaceValue(
		Token,
		*Plan->Property,
		Candidate.Value,
		OutError);
}

bool FAvidScriptBindingPackage::FindCompositeContainerValue(
	const uint32 Token,
	const uint32 GuestAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	int32& OutIndex,
	FString& OutError) const
{
	OutIndex = INDEX_NONE;
	FAvidScriptCompositeValueView Value;
	if (GuestAddress == 0
		|| Context.CompositeValueHeap == nullptr
		|| !Context.CompositeValueHeap->Resolve(
			Token,
			FString(),
			nullptr,
			Value,
			OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_container_find_invalid: the key or capability is invalid.");
		}
		return false;
	}
	const FAvidScriptRuntimeBindingValuePlan* Plan =
		FindAvidScriptCompositeAccessPlan(
			Value.TypeId,
			Value.Property,
			Impl->CompositeAccessPlansByProperty,
			OutError);
	if (Plan == nullptr
		|| (Plan->Kind != EAvidScriptRuntimeBindingKind::Set
			&& Plan->Kind != EAvidScriptRuntimeBindingKind::Map)
		|| Plan->Children.IsEmpty())
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_container_find_invalid: only set and map capabilities support key lookup.");
		}
		return false;
	}
	FScopedAvidScriptPropertyContainer Key;
	if (!Key.Initialize(Plan->Children[0], OutError)
		|| !UE::AvidScript::BindingPrivate::SetValueFromGuest(
			Plan->Children[0],
			GuestAddress,
			GuestMemory,
			Context,
			Key.Container,
			OutError))
	{
		return false;
	}
	TConstArrayView<int32> SnapshotIndices;
	int32 InternalIndex = INDEX_NONE;
	if (Plan->Kind == EAvidScriptRuntimeBindingKind::Set)
	{
		FScriptSetHelper Helper(
			CastFieldChecked<FSetProperty>(Plan->Property),
			const_cast<void*>(Value.Value));
		InternalIndex = Helper.FindElementIndexFromHash(Key.Value);
		if (InternalIndex != INDEX_NONE
			&& !GetAvidScriptSetCanonicalSnapshot(
				*Context.CompositeValueHeap,
				Token,
				Helper,
				Plan->Children[0],
				SnapshotIndices,
				OutError))
		{
			return false;
		}
	}
	else
	{
		FScriptMapHelper Helper(
			CastFieldChecked<FMapProperty>(Plan->Property),
			const_cast<void*>(Value.Value));
		InternalIndex = Helper.FindMapPairIndexFromHash(Key.Value);
		if (InternalIndex != INDEX_NONE
			&& !GetAvidScriptMapCanonicalSnapshot(
				*Context.CompositeValueHeap,
				Token,
				Helper,
				Plan->Children[0],
				SnapshotIndices,
				OutError))
		{
			return false;
		}
	}
	if (InternalIndex != INDEX_NONE
		&& !Context.CompositeValueHeap->TryResolveCanonicalIndex(
			Token,
			InternalIndex,
			OutIndex))
	{
		OutError = TEXT("composite_container_snapshot_invalid: the located key is absent from its canonical snapshot.");
		return false;
	}
	return true;
}

bool FAvidScriptBindingPackage::UpsertCompositeContainerValue(
	const uint32 Token,
	const uint32 KeyAddress,
	const uint32 ValueAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	int32& OutMutationResult,
	FString& OutError) const
{
	OutMutationResult = 0;
	FAvidScriptMutableCompositeValueView Value;
	if (KeyAddress == 0
		|| Context.CompositeValueHeap == nullptr
		|| !Context.CompositeValueHeap->ResolveMutable(Token, Value, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_container_upsert_invalid: the input or capability is invalid.");
		}
		return false;
	}
	const FAvidScriptRuntimeBindingValuePlan* Plan =
		FindAvidScriptCompositeAccessPlan(
			Value.TypeId,
			Value.Property,
			Impl->CompositeAccessPlansByProperty,
			OutError);
	if (Plan == nullptr
		|| (Plan->Kind != EAvidScriptRuntimeBindingKind::Set
			&& Plan->Kind != EAvidScriptRuntimeBindingKind::Map)
		|| Plan->Children.IsEmpty()
		|| (Plan->Kind == EAvidScriptRuntimeBindingKind::Set && ValueAddress != 0)
		|| (Plan->Kind == EAvidScriptRuntimeBindingKind::Map
			&& (ValueAddress == 0 || Plan->Children.Num() != 2)))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_container_upsert_invalid: the mutation shape does not match a set or map.");
		}
		return false;
	}
	FScopedAvidScriptPropertyContainer Key;
	if (!Key.Initialize(Plan->Children[0], OutError)
		|| !UE::AvidScript::BindingPrivate::SetValueFromGuest(
			Plan->Children[0],
			KeyAddress,
			GuestMemory,
			Context,
			Key.Container,
			OutError))
	{
		return false;
	}
	FScopedAvidScriptPropertyContainer Candidate;
	if (!Candidate.InitializeCopy(*Plan, Value.Value, OutError))
	{
		return false;
	}
	if (Plan->Kind == EAvidScriptRuntimeBindingKind::Set)
	{
		FScriptSetHelper Helper(
			CastFieldChecked<FSetProperty>(Plan->Property),
			Candidate.Value);
		const int32 ExistingIndex = Helper.FindElementIndexFromHash(Key.Value);
		if (ExistingIndex == INDEX_NONE)
		{
			if (Helper.Num() >= FAvidScriptCompositeValueHeap::MaxChildValues)
			{
				OutError = TEXT("composite_container_limit_exceeded: the set reached its bounded element count.");
				return false;
			}
			Helper.AddElement(Key.Value);
			if (!Context.CompositeValueHeap->ReplaceValue(
					Token,
					*Plan->Property,
					Candidate.Value,
					OutError))
			{
				return false;
			}
		}
		OutMutationResult = ExistingIndex == INDEX_NONE ? 1 : 2;
		return true;
	}

	FScopedAvidScriptPropertyContainer MappedValue;
	if (!MappedValue.Initialize(Plan->Children[1], OutError)
		|| !UE::AvidScript::BindingPrivate::SetValueFromGuest(
			Plan->Children[1],
			ValueAddress,
			GuestMemory,
			Context,
			MappedValue.Container,
			OutError))
	{
		return false;
	}
	FScriptMapHelper Helper(
		CastFieldChecked<FMapProperty>(Plan->Property),
		Candidate.Value);
	const bool bInserted =
		Helper.FindMapPairIndexFromHash(Key.Value) == INDEX_NONE;
	if (bInserted
		&& Helper.Num() >= FAvidScriptCompositeValueHeap::MaxChildValues)
	{
		OutError = TEXT("composite_container_limit_exceeded: the map reached its bounded entry count.");
		return false;
	}
	Helper.AddPair(Key.Value, MappedValue.Value);
	if (!Context.CompositeValueHeap->ReplaceValue(
			Token,
			*Plan->Property,
			Candidate.Value,
			OutError))
	{
		return false;
	}
	OutMutationResult = bInserted ? 1 : 2;
	return true;
}

bool FAvidScriptBindingPackage::RemoveCompositeContainerValue(
	const uint32 Token,
	const uint32 KeyAddress,
	IAvidScriptVmGuestMemory& GuestMemory,
	const FAvidScriptBindingInvocationContext& Context,
	bool& bOutRemoved,
	FString& OutError) const
{
	bOutRemoved = false;
	FAvidScriptMutableCompositeValueView Value;
	if (KeyAddress == 0
		|| Context.CompositeValueHeap == nullptr
		|| !Context.CompositeValueHeap->ResolveMutable(Token, Value, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_container_remove_invalid: the key or capability is invalid.");
		}
		return false;
	}
	const FAvidScriptRuntimeBindingValuePlan* Plan =
		FindAvidScriptCompositeAccessPlan(
			Value.TypeId,
			Value.Property,
			Impl->CompositeAccessPlansByProperty,
			OutError);
	if (Plan == nullptr
		|| (Plan->Kind != EAvidScriptRuntimeBindingKind::Set
			&& Plan->Kind != EAvidScriptRuntimeBindingKind::Map)
		|| Plan->Children.IsEmpty())
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_container_remove_invalid: only set and map capabilities support removal.");
		}
		return false;
	}
	FScopedAvidScriptPropertyContainer Key;
	if (!Key.Initialize(Plan->Children[0], OutError)
		|| !UE::AvidScript::BindingPrivate::SetValueFromGuest(
			Plan->Children[0],
			KeyAddress,
			GuestMemory,
			Context,
			Key.Container,
			OutError))
	{
		return false;
	}
	FScopedAvidScriptPropertyContainer Candidate;
	if (!Candidate.InitializeCopy(*Plan, Value.Value, OutError))
	{
		return false;
	}
	int32 InternalIndex = INDEX_NONE;
	if (Plan->Kind == EAvidScriptRuntimeBindingKind::Set)
	{
		FScriptSetHelper Helper(
			CastFieldChecked<FSetProperty>(Plan->Property),
			Candidate.Value);
		InternalIndex = Helper.FindElementIndexFromHash(Key.Value);
		if (InternalIndex != INDEX_NONE)
		{
			Helper.RemoveAt(InternalIndex);
		}
	}
	else
	{
		FScriptMapHelper Helper(
			CastFieldChecked<FMapProperty>(Plan->Property),
			Candidate.Value);
		InternalIndex = Helper.FindMapPairIndexFromHash(Key.Value);
		if (InternalIndex != INDEX_NONE)
		{
			Helper.RemoveAt(InternalIndex);
		}
	}
	bOutRemoved = InternalIndex != INDEX_NONE;
	if (bOutRemoved
		&& !Context.CompositeValueHeap->ReplaceValue(
			Token,
			*Plan->Property,
			Candidate.Value,
			OutError))
	{
		bOutRemoved = false;
		return false;
	}
	return true;
}

bool FAvidScriptBindingPackage::Dispatch(
	const FAvidScriptDynamicHostCall& Call,
	const FAvidScriptBindingInvocationContext& Context,
	TArray<uint8>& InvocationScratch,
	FAvidScriptDynamicHostCallResult& OutResult) const
{
	OutResult = FAvidScriptDynamicHostCallResult();
	if (!Impl->Plans.IsValidIndex(static_cast<int32>(Call.BindingOrdinal)))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_ordinal_invalid"),
			FString::FromInt(Call.BindingOrdinal),
			TEXT("The VM binding ordinal is outside the attached package."));
		return false;
	}
	const FAvidScriptRuntimeBindingInvocationPlan& Plan = Impl->Plans[Call.BindingOrdinal];
	const auto CompleteSpecialDispatch =
		[&Context, &OutResult](const bool bDispatched)
	{
		FAvidScriptBindingInvocationInstrumentation* Instrumentation =
			Context.InvocationInstrumentation;
		if (Instrumentation == nullptr
			|| !bDispatched
			|| !OutResult.bSucceeded)
		{
			return bDispatched;
		}
		++Instrumentation->SemanticProcessEventCount;
		if (Context.InvocationPolicy
			== EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect)
		{
			++Instrumentation->RequestedNativeDirectFallbackCount;
		}
		if (Context.InvocationPolicy
			== EAvidScriptBindingInvocationPolicy::AdaptiveSemantic)
		{
			++Instrumentation->AdaptiveProcessEventFallbackCount;
		}
		return bDispatched;
	};
	if (Call.Arguments.Num() != Plan.ExpectedArgumentCount
		|| (Plan.bRequiresGuestMemory && Call.GuestMemory == nullptr))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_frame_mismatch"),
			Plan.DebugPath,
			TEXT("The raw argument count or guest memory contract does not match the cached invocation plan."));
		return false;
	}
	if (InvocationScratch.Num() < Plan.RequiredScratchSize)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_scratch_too_small"),
			Plan.DebugPath,
			TEXT("The runtime did not preallocate the package's required invocation scratch size."));
		return false;
	}
	if (Plan.Kind == EAvidScriptBindingInvocationKind::ObjectSpawnActor
		|| Plan.Kind == EAvidScriptBindingInvocationKind::ObjectDestroyActor
		|| Plan.Kind == EAvidScriptBindingInvocationKind::ObjectIsA)
	{
		return CompleteSpecialDispatch(
			DispatchAvidScriptObjectLifecycle(
				*this,
				Plan,
				Call,
				Context,
				OutResult));
	}
	if (Plan.Kind == EAvidScriptBindingInvocationKind::ObjectTypeIsA)
	{
		return CompleteSpecialDispatch(
			DispatchAvidScriptObjectType(
				*this,
				Plan,
				Call,
				Context,
				OutResult));
	}
	if (Plan.Kind == EAvidScriptBindingInvocationKind::ObjectConstruct
		|| Plan.Kind == EAvidScriptBindingInvocationKind::ObjectRelease
		|| Plan.Kind == EAvidScriptBindingInvocationKind::ActorFindComponent)
	{
		return CompleteSpecialDispatch(
			DispatchAvidScriptObjectFactory(
				*this,
				Plan,
				Call,
				Context,
				OutResult));
	}
	if (Plan.Kind == EAvidScriptBindingInvocationKind::SceneComponentAttach
		|| Plan.Kind == EAvidScriptBindingInvocationKind::SceneComponentDetach)
	{
		return CompleteSpecialDispatch(
			DispatchAvidScriptSceneAttachment(
				Plan,
				Call,
				Context,
				OutResult));
	}

	UObject* Target = nullptr;
	FString Details;
	if (Plan.bStatic)
	{
		Target = Plan.OwnerClass->GetDefaultObject();
	}
	else if (!UE::AvidScript::BindingPrivate::ResolveObjectHandle(
		static_cast<uint32>(Call.Arguments[0]),
		static_cast<uint32>(Call.Arguments[1]),
		Plan.OwnerClass,
		Context,
		false,
		Target,
		Details))
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			Details);
		return false;
	}
	if (Target == nullptr)
	{
		SetAvidScriptBindingDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Plan.DebugPath,
			TEXT("The cached invocation target is null."));
		return false;
	}
	const UE::AvidScript::BindingPrivate::FPreparedDynamicInvocationCell Cell{
		&Plan,
		Call.BindingOrdinal
	};
	return UE::AvidScript::BindingPrivate::InvokePreparedDynamicReflection(
		&Cell,
		*Target,
		Call.Arguments,
		Call.GuestMemory,
		Context,
		InvocationScratch,
		OutResult);
}
