#include "AvidScriptBindingFastPath.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::AvidScript::BindingPrivate
{
namespace
{
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
		|| !Property->HasAllPropertyFlags(CPF_ZeroConstructor | CPF_NoDestructor))
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

bool DispatchScalarI32PairToI32(
	const FFastPathPlan& Plan,
	UObject& Target,
	const FAvidScriptDynamicHostCall& Call,
	TArray<uint8>& InvocationScratch,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
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

	uint8* Frame = reinterpret_cast<uint8*>(FrameAddress);
	FMemory::Memzero(Frame, Plan.FrameSize);
	for (int32 ParameterIndex = 0; ParameterIndex < 2; ++ParameterIndex)
	{
		const int32 Value = static_cast<int32>(
			static_cast<uint32>(Call.Arguments[2 + ParameterIndex]));
		FMemory::Memcpy(
			Frame + Plan.ParameterFrameOffsets[ParameterIndex],
			&Value,
			sizeof(Value));
	}

	Target.ProcessEvent(Plan.Function, Frame);

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
} // namespace

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
	Candidate.Thunk = &DispatchScalarI32PairToI32;
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

	OutPlan = Candidate;
	return true;
}

bool DispatchFastPath(
	const FFastPathPlan& Plan,
	UObject& Target,
	const FAvidScriptDynamicHostCall& Call,
	TArray<uint8>& InvocationScratch,
	FString& OutErrorCategory,
	FString& OutErrorDetails)
{
	OutErrorCategory.Reset();
	OutErrorDetails.Reset();
	check(Plan.IsBound());
	return Plan.Thunk(
		Plan,
		Target,
		Call,
		InvocationScratch,
		OutErrorCategory,
		OutErrorDetails);
}
} // namespace UE::AvidScript::BindingPrivate
