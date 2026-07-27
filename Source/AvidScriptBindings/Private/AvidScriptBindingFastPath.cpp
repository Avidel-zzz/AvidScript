#include "AvidScriptBindingFastPath.h"

#include "CoreGlobals.h"
#include "UObject/Class.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectThreadContext.h"

namespace UE::AvidScript::BindingPrivate
{
namespace
{
constexpr int32 ScalarI32PairFrameSize = 3 * sizeof(int32);

bool HasEditorClassGenerator(const UClass& Class)
{
#if WITH_EDITORONLY_DATA
	return Class.ClassGeneratedBy != nullptr;
#else
	return false;
#endif
}

bool IsTrivialInt32Property(
	const FProperty* Property,
	const int32 FrameSize,
	int32& OutFrameOffset)
{
	OutFrameOffset = INDEX_NONE;
	if (Property == nullptr
		|| !Property->IsA<FIntProperty>()
		|| Property->ArrayDim != 1
		|| Property->GetElementSize() != sizeof(int32)
		|| Property->GetSize() != sizeof(int32)
		|| Property->GetMinAlignment() != alignof(int32)
		|| !Property->HasAllPropertyFlags(
			CPF_ZeroConstructor | CPF_IsPlainOldData | CPF_NoDestructor))
	{
		return false;
	}

	const int32 FrameOffset = Property->GetOffset_ForInternal();
	if (FrameOffset < 0
		|| FrameOffset % alignof(int32) != 0
		|| FrameSize < static_cast<int32>(sizeof(int32))
		|| FrameOffset > FrameSize - static_cast<int32>(sizeof(int32)))
	{
		return false;
	}

	OutFrameOffset = FrameOffset;
	return true;
}

bool IsQualifiedNativeDirectInt32Property(
	const FField* Field,
	const UFunction& Function,
	const int32 ExpectedOffset,
	const bool bReturnValue,
	const FIntProperty*& OutProperty)
{
	OutProperty = nullptr;
	const FIntProperty* Property = CastField<FIntProperty>(Field);
	if (Property == nullptr
		|| Property->GetClass() != FIntProperty::StaticClass()
		|| Property->GetOwnerStruct() != &Function
		|| Property->ArrayDim != 1
		|| Property->GetElementSize() != sizeof(int32)
		|| Property->GetSize() != sizeof(int32)
		|| Property->GetMinAlignment() != alignof(int32)
		|| Property->GetOffset_ForUFunction() != ExpectedOffset
		|| !Property->HasAllPropertyFlags(
			CPF_ZeroConstructor | CPF_IsPlainOldData | CPF_NoDestructor))
	{
		return false;
	}

	const EPropertyFlags RejectedInputFlags =
		CPF_OutParm | CPF_ReturnParm | CPF_ReferenceParm | CPF_ConstParm;
	const EPropertyFlags RejectedReturnFlags =
		CPF_ReferenceParm | CPF_ConstParm;
	if (bReturnValue)
	{
		if (!Property->HasAllPropertyFlags(
				CPF_Parm | CPF_OutParm | CPF_ReturnParm)
			|| Property->HasAnyPropertyFlags(RejectedReturnFlags))
		{
			return false;
		}
	}
	else if (!Property->HasAllPropertyFlags(CPF_Parm)
		|| Property->HasAnyPropertyFlags(RejectedInputFlags))
	{
		return false;
	}

	OutProperty = Property;
	return true;
}

bool PrepareScalarI32PairFrame(
	const FFastPathPlan& Plan,
	const FAvidScriptDynamicHostCall& Call,
	TArray<uint8>& InvocationScratch,
	uint8*& OutFrame,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	OutFrame = nullptr;
	const UPTRINT ScratchAddress =
		reinterpret_cast<UPTRINT>(InvocationScratch.GetData());
	const UPTRINT FrameAddress =
		Align(ScratchAddress, static_cast<UPTRINT>(Plan.FrameAlignment));
	const int32 FrameOffset =
		static_cast<int32>(FrameAddress - ScratchAddress);
	if (FrameOffset < 0
		|| FrameOffset + Plan.FrameSize > InvocationScratch.Num())
	{
		OutErrorCategory = TEXT("binding_scratch_alignment_failed");
		OutErrorDetails =
			TEXT("The typed thunk could not satisfy its cached frame alignment.");
		return false;
	}

	OutFrame = reinterpret_cast<uint8*>(FrameAddress);
	FMemory::Memzero(OutFrame, Plan.FrameSize);
	for (int32 ParameterIndex = 0; ParameterIndex < 2; ++ParameterIndex)
	{
		const int32 Value = static_cast<int32>(
			static_cast<uint32>(Call.Arguments[2 + ParameterIndex]));
		FMemory::Memcpy(
			OutFrame + Plan.ParameterFrameOffsets[ParameterIndex],
			&Value,
			sizeof(Value));
	}
	return true;
}

bool WriteScalarI32Return(
	const FFastPathPlan& Plan,
	const FAvidScriptDynamicHostCall& Call,
	const uint8* Frame,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	int32 ReturnValue = 0;
	FMemory::Memcpy(
		&ReturnValue,
		Frame + Plan.ReturnFrameOffset,
		sizeof(ReturnValue));
	FString MemoryError;
	if (!Call.GuestMemory->WriteBytes(
		static_cast<uint32>(Call.Arguments[Plan.ReturnGuestArgumentOffset]),
		MakeArrayView(
			reinterpret_cast<const uint8*>(&ReturnValue),
			sizeof(ReturnValue)),
		MemoryError))
	{
		OutErrorCategory = TEXT("binding_return_write_failed");
		OutErrorDetails = MemoryError.IsEmpty()
			? FString(TEXT("The typed thunk could not write its int32 return value."))
			: MoveTemp(MemoryError);
		return false;
	}

	return true;
}

bool DispatchSemanticScalarI32PairToI32(
	const FFastPathPlan& Plan,
	UObject& Target,
	const FAvidScriptDynamicHostCall& Call,
	TArray<uint8>& InvocationScratch,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	uint8* Frame = nullptr;
	if (!PrepareScalarI32PairFrame(
			Plan,
			Call,
			InvocationScratch,
			Frame,
			OutErrorCategory,
			OutErrorDetails))
	{
		return false;
	}

	Target.ProcessEvent(Plan.Function, Frame);
	return WriteScalarI32Return(
		Plan,
		Call,
		Frame,
		OutErrorCategory,
		OutErrorDetails);
}

bool DispatchNativeDirectScalarI32PairToI32(
	const FFastPathPlan& Plan,
	UObject& Target,
	const FAvidScriptDynamicHostCall& Call,
	TArray<uint8>& InvocationScratch,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	uint8* Frame = nullptr;
	if (!PrepareScalarI32PairFrame(
			Plan,
			Call,
			InvocationScratch,
			Frame,
			OutErrorCategory,
			OutErrorDetails))
	{
		return false;
	}

	FFrame Stack(
		&Target,
		Plan.Function,
		Frame,
		nullptr,
		Plan.Function->ChildProperties);
	Stack.Code = nullptr;
	Plan.Function->Invoke(
		&Target,
		Stack,
		Frame + Plan.ReturnFrameOffset);
	if (Stack.bAbortingExecution)
	{
		OutErrorCategory = TEXT("binding_native_direct_aborted");
		OutErrorDetails =
			TEXT("The qualified native exec thunk aborted execution.");
		return false;
	}

	return WriteScalarI32Return(
		Plan,
		Call,
		Frame,
		OutErrorCategory,
		OutErrorDetails);
}
} // namespace

bool IsQualifiedNativeDirectFunction(const UFunction& Function)
{
	const EFunctionFlags RequiredFlags =
		FUNC_Native | FUNC_Final | FUNC_Public;
	const EFunctionFlags RejectedFlags =
		FUNC_NetFuncFlags
		| FUNC_NetRequest
		| FUNC_NetResponse
		| FUNC_NetValidate
		| FUNC_Event
		| FUNC_BlueprintEvent
		| FUNC_Delegate
		| FUNC_MulticastDelegate
		| FUNC_Exec
		| FUNC_Static
		| FUNC_BlueprintAuthorityOnly
		| FUNC_BlueprintCosmetic
		| FUNC_EditorOnly
		| FUNC_UbergraphFunction
		| FUNC_DLLImport
		| FUNC_HasOutParms
		| FUNC_HasDefaults;
	const UClass* OwnerClass = Function.GetOwnerClass();
	if (!Function.HasAllFunctionFlags(RequiredFlags)
		|| Function.HasAnyFunctionFlags(RejectedFlags)
		|| Function.GetNativeFunc() == nullptr
		|| OwnerClass == nullptr
		|| !OwnerClass->HasAllClassFlags(CLASS_Native)
		|| OwnerClass->HasAnyClassFlags(
			CLASS_Interface | CLASS_CompiledFromBlueprint)
		|| HasEditorClassGenerator(*OwnerClass)
		|| Function.GetSuperFunction() != nullptr
		|| !Function.Script.IsEmpty()
		|| Function.NumParms != 3
		|| Function.ParmsSize != ScalarI32PairFrameSize
		|| Function.PropertiesSize != ScalarI32PairFrameSize
		|| Function.ReturnValueOffset != 2 * sizeof(int32)
		|| Function.GetStructureSize() != ScalarI32PairFrameSize
		|| Function.GetMinAlignment() != alignof(int32)
		|| Function.FirstPropertyToInit != nullptr
		|| Function.PostConstructLink != nullptr
		|| Function.DestructorLink != nullptr)
	{
		return false;
	}

	const FIntProperty* Properties[3] = {};
	const FField* Field = Function.ChildProperties;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		if (!IsQualifiedNativeDirectInt32Property(
				Field,
				Function,
				Index * sizeof(int32),
				Index == 2,
				Properties[Index]))
		{
			return false;
		}
		Field = Field->Next;
	}
	return Field == nullptr
		&& Function.GetReturnProperty() == Properties[2];
}

bool TryBuildFastPath(
	const FFastPathBuildSpec& Spec,
	FFastPathPlan& OutPlan)
{
	OutPlan = FFastPathPlan();
	if (Spec.Function == nullptr
		|| Spec.bStatic
		|| Spec.bRequiresWriteAccess
		|| Spec.bHasReloadEffect
		|| Spec.ExpectedArgumentCount != 5
		|| Spec.Parameters.Num() != 2
		|| Spec.FrameSize <= 0
		|| !FMath::IsPowerOfTwo(Spec.FrameAlignment)
		|| !Spec.ReturnValue.bIsInt32
		|| Spec.ReturnValue.ArgumentOffset != 4)
	{
		return false;
	}

	int32 ReflectedParameterCount = 0;
	for (TFieldIterator<FProperty> It(Spec.Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			++ReflectedParameterCount;
		}
	}
	if (ReflectedParameterCount != 3)
	{
		return false;
	}

	FFastPathPlan Candidate;
	Candidate.Kind = EAvidScriptBindingFastPathKind::ScalarI32PairToI32;
	Candidate.Function = Spec.Function;
	Candidate.SemanticThunk = &DispatchSemanticScalarI32PairToI32;
	Candidate.FrameSize = Spec.FrameSize;
	Candidate.FrameAlignment = Spec.FrameAlignment;
	Candidate.ReturnGuestArgumentOffset = Spec.ReturnValue.ArgumentOffset;
	for (int32 ParameterIndex = 0; ParameterIndex < 2; ++ParameterIndex)
	{
		const FFastPathValueSpec& Parameter = Spec.Parameters[ParameterIndex];
		if (!Parameter.bIsValue
			|| !Parameter.bIsInt32
			|| Parameter.ArgumentOffset != 2 + ParameterIndex
			|| !IsTrivialInt32Property(
				Parameter.Property,
				Spec.FrameSize,
				Candidate.ParameterFrameOffsets[ParameterIndex]))
		{
			return false;
		}
	}
	if (!IsTrivialInt32Property(
		Spec.ReturnValue.Property,
		Spec.FrameSize,
		Candidate.ReturnFrameOffset))
	{
		return false;
	}
	if (Spec.bQualifiedNativeDirectAuthorized
		&& IsQualifiedNativeDirectFunction(*Spec.Function))
	{
		Candidate.HighestInvocationMode =
			EAvidScriptBindingInvocationMode::QualifiedNativeDirect;
		Candidate.NativeDirectOwnerClass = Spec.Function->GetOwnerClass();
		Candidate.NativeDirectThunk =
			&DispatchNativeDirectScalarI32PairToI32;
		for (int32 ParameterIndex = 0; ParameterIndex < 2; ++ParameterIndex)
		{
			Candidate.ParameterFrameOffsets[ParameterIndex] =
				Spec.Parameters[ParameterIndex].Property
					->GetOffset_ForUFunction();
		}
		Candidate.ReturnFrameOffset =
			Spec.ReturnValue.Property->GetOffset_ForUFunction();
	}

	OutPlan = Candidate;
	return true;
}

bool DispatchFastPath(
	const FFastPathPlan& Plan,
	UObject& Target,
	const FAvidScriptDynamicHostCall& Call,
	TArray<uint8>& InvocationScratch,
	const EAvidScriptBindingInvocationPolicy InvocationPolicy,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	OutErrorCategory.Reset();
	OutErrorDetails.Reset();
	check(Plan.IsBound());
	FFastPathThunk Thunk = Plan.SemanticThunk;
	if (InvocationPolicy
			== EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect
		&& Plan.HighestInvocationMode
			== EAvidScriptBindingInvocationMode::QualifiedNativeDirect)
	{
		check(Plan.NativeDirectThunk != nullptr);
		if (!IsInGameThread())
		{
			OutErrorCategory = TEXT("binding_native_direct_wrong_thread");
			OutErrorDetails =
				TEXT("Qualified native direct invocation requires the Game Thread.");
			return false;
		}
		if (Target.GetClass() != Plan.NativeDirectOwnerClass)
		{
			OutErrorCategory =
				TEXT("binding_native_direct_exact_class_mismatch");
			OutErrorDetails =
				TEXT("Qualified native direct invocation requires the target's exact class to match the function owner.");
			return false;
		}
		if (IsGarbageCollecting())
		{
			OutErrorCategory = TEXT("binding_native_direct_gc_active");
			OutErrorDetails =
				TEXT("Qualified native direct invocation is unavailable during garbage collection.");
			return false;
		}
		if (FUObjectThreadContext::Get().IsRoutingPostLoad)
		{
			OutErrorCategory =
				TEXT("binding_native_direct_post_load_active");
			OutErrorDetails =
				TEXT("Qualified native direct invocation is unavailable while routing PostLoad.");
			return false;
		}
		if (GIntraFrameDebuggingGameThread)
		{
			OutErrorCategory =
				TEXT("binding_native_direct_debugging_active");
			OutErrorDetails =
				TEXT("Qualified native direct invocation is unavailable during intra-frame debugging.");
			return false;
		}
		Thunk = Plan.NativeDirectThunk;
	}

	return Thunk(
		Plan,
		Target,
		Call,
		InvocationScratch,
		OutErrorCategory,
		OutErrorDetails);
}
} // namespace UE::AvidScript::BindingPrivate
