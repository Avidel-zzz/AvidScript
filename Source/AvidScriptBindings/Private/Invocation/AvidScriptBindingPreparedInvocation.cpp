#include "Invocation/AvidScriptBindingPreparedInvocation.h"

#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::AvidScript::BindingPrivate
{
namespace
{
void SetDispatchFailure(
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

bool IsReflectedProgram(const FInvocationCodecProgram& Program)
{
	return Program.OwnerClass != nullptr
		&& (Program.Kind == EAvidScriptBindingInvocationKind::ReflectedFunction
			|| Program.Kind
				== EAvidScriptBindingInvocationKind::ReflectedPropertyRead
			|| Program.Kind
				== EAvidScriptBindingInvocationKind::ReflectedPropertyWrite);
}
} // namespace

bool InvokePreparedDynamicReflection(
	const void* InvocationCell,
	UObject& Receiver,
	const TConstArrayView<uint64> Arguments,
	IAvidScriptVmGuestMemory* GuestMemory,
	const FAvidScriptBindingInvocationContext& InvocationContext,
	TArray<uint8>& InvocationScratch,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	OutResult = FAvidScriptDynamicHostCallResult();
	const auto* Cell =
		static_cast<const FPreparedDynamicInvocationCell*>(InvocationCell);
	const FInvocationCodecProgram* Program =
		Cell == nullptr ? nullptr : Cell->Program;
	if (Program == nullptr || !IsReflectedProgram(*Program))
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_prepared_identity_mismatch"),
			TEXT("<prepared-dynamic>"),
			TEXT("The immutable reflected invocation program is unavailable."));
		return false;
	}

	const bool bRequestedNativeDirect =
		InvocationContext.InvocationPolicy
		== EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect;
	const bool bRequestedAdaptive =
		InvocationContext.InvocationPolicy
		== EAvidScriptBindingInvocationPolicy::AdaptiveSemantic;
	EAvidScriptBindingInvocationMode ActualInvocationMode =
		EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	bool bAdaptiveGuardRejected = false;
	ON_SCOPE_EXIT
	{
		FAvidScriptBindingInvocationInstrumentation* Instrumentation =
			InvocationContext.InvocationInstrumentation;
		if (Instrumentation == nullptr || !OutResult.bSucceeded)
		{
			return;
		}
		if (ActualInvocationMode
			== EAvidScriptBindingInvocationMode::QualifiedNativeDirect)
		{
			++Instrumentation->QualifiedNativeDirectCount;
			return;
		}
		if (ActualInvocationMode
			== EAvidScriptBindingInvocationMode::AdaptivePreparedNative)
		{
			++Instrumentation->AdaptivePreparedNativeHitCount;
			return;
		}
		++Instrumentation->SemanticProcessEventCount;
		if (bRequestedNativeDirect)
		{
			++Instrumentation->RequestedNativeDirectFallbackCount;
		}
		if (bRequestedAdaptive)
		{
			++Instrumentation->AdaptiveProcessEventFallbackCount;
			if (bAdaptiveGuardRejected)
			{
				++Instrumentation->AdaptiveGuardRejectCount;
			}
		}
	};

	if (Arguments.Num() != Program->ExpectedArgumentCount
		|| (Program->bRequiresGuestMemory && GuestMemory == nullptr))
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_frame_mismatch"),
			Program->DebugPath,
			TEXT("The raw argument count or guest memory contract does not match the prepared invocation program."));
		return false;
	}
	if (InvocationScratch.Num() < Program->RequiredScratchSize)
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_scratch_too_small"),
			Program->DebugPath,
			TEXT("The runtime did not preallocate the prepared program's required invocation scratch size."));
		return false;
	}
	if (!Receiver.IsA(Program->OwnerClass))
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_target_invalid"),
			Program->DebugPath,
			TEXT("The prepared receiver no longer satisfies the immutable owner class."));
		return false;
	}
	if (Program->bRequiresWriteAccess
		&& InvocationContext.WritePolicy
			!= EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_write_denied"),
			Program->DebugPath,
			TEXT("The reflected binding requires an explicitly writable host context."));
		return false;
	}

	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Cell->BindingOrdinal;
	Call.Arguments = Arguments;
	Call.GuestMemory = GuestMemory;
	const auto PrepareHostEffect = [&]()
	{
		if (InvocationContext.HostEffectJournal == nullptr
			|| !Program->bRequiresWriteAccess)
		{
			return true;
		}
		if (Program->Kind
				== EAvidScriptBindingInvocationKind::ReflectedPropertyWrite
			&& Program->Function != nullptr)
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_reload_effect_unsupported"),
				Program->DebugPath,
				TEXT("BlueprintSetter candidate reload is not reversible because ProcessEvent may produce additional host effects."));
			return false;
		}
		if (Program->ReloadEffect
			== EAvidScriptBindingReloadEffect::Unsupported)
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_reload_effect_unsupported"),
				Program->DebugPath,
				TEXT("The reflected write has no reversible candidate reload adapter."));
			return false;
		}
		if (InvocationContext.ObjectRegistry == nullptr)
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_host_effect_registry_missing"),
				Program->DebugPath,
				TEXT("The candidate host effect journal requires an object registry."));
			return false;
		}
		if (!Program->bStatic && Arguments.Num() < 2)
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_frame_mismatch"),
				Program->DebugPath,
				TEXT("The prepared instance invocation has no receiver handle cells."));
			return false;
		}

		const FAvidScriptObjectHandle EffectHandle = Program->bStatic
			? InvocationContext.OwnerHandle
			: FAvidScriptObjectHandle{
				static_cast<uint32>(Arguments[0]),
				static_cast<uint32>(Arguments[1])
			};
		FAvidScriptBindingHostEffectPrepareResult PrepareResult;
		const bool bPrepared = Program->ReloadEffect
			== EAvidScriptBindingReloadEffect::ReflectedProperty
			? Program->ReflectedProperty != nullptr
				&& InvocationContext.HostEffectJournal
					->PrepareReflectedProperty(
						*InvocationContext.ObjectRegistry,
						EffectHandle,
						Receiver,
						*Program->ReflectedProperty,
						PrepareResult)
			: InvocationContext.HostEffectJournal->PrepareEffect(
				*InvocationContext.ObjectRegistry,
				EffectHandle,
				Receiver,
				Program->ReloadEffect,
				PrepareResult);
		if (!bPrepared)
		{
			SetDispatchFailure(
				OutResult,
				PrepareResult.ErrorCategory.IsEmpty()
					? FString(TEXT("binding_host_effect_prepare_failed"))
					: PrepareResult.ErrorCategory,
				PrepareResult.ErrorSource.IsEmpty()
					? Program->DebugPath
					: PrepareResult.ErrorSource,
				PrepareResult.ErrorDetails.IsEmpty()
					? FString(TEXT("The candidate host effect could not be prepared."))
					: PrepareResult.ErrorDetails);
			return false;
		}
		return true;
	};

	FString Details;
	if (Program->Kind
		== EAvidScriptBindingInvocationKind::ReflectedPropertyRead)
	{
		if (GuestMemory == nullptr
			|| !WriteValueToGuest(
				Program->ReturnValue,
				static_cast<uint32>(
					Arguments[Program->ReturnValue.ArgumentOffset]),
				*GuestMemory,
				InvocationContext,
				&Receiver,
				Details))
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_property_read_failed"),
				Program->DebugPath,
				Details);
			return false;
		}
		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}
	if (Program->Kind
			== EAvidScriptBindingInvocationKind::ReflectedPropertyWrite
		&& Program->Function == nullptr)
	{
		if (!PrepareHostEffect()
			|| !SetValueFromCells(
				Program->Parameters[0],
				Arguments.Slice(
					Program->Parameters[0].ArgumentOffset,
					Program->Parameters[0].ArgumentWidth),
				GuestMemory,
				InvocationContext,
				&Receiver,
				Details))
		{
			if (OutResult.Details.IsEmpty())
			{
				SetDispatchFailure(
					OutResult,
					TEXT("binding_property_write_failed"),
					Program->DebugPath,
					Details);
			}
			return false;
		}
		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}

	if (Program->FastPath.IsBound())
	{
		if (!PrepareHostEffect())
		{
			return false;
		}
		FString ErrorCategory;
		FString ErrorDetails;
		if (!DispatchFastPath(
				Program->FastPath,
				Receiver,
				Call,
				InvocationScratch,
				InvocationContext.InvocationPolicy,
				ActualInvocationMode,
				bAdaptiveGuardRejected,
				ErrorCategory,
				ErrorDetails))
		{
			SetDispatchFailure(
				OutResult,
				ErrorCategory.IsEmpty()
					? FString(TEXT("binding_fast_path_failed"))
					: ErrorCategory,
				Program->DebugPath,
				ErrorDetails.IsEmpty()
					? FString(TEXT("The cached typed thunk rejected the invocation."))
					: ErrorDetails);
			return false;
		}
		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}

	if (Program->Function == nullptr)
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_prepared_identity_mismatch"),
			Program->DebugPath,
			TEXT("The prepared ProcessEvent program has no UFunction."));
		return false;
	}
	const UPTRINT ScratchAddress =
		reinterpret_cast<UPTRINT>(InvocationScratch.GetData());
	const UPTRINT FrameAddress = Align(
		ScratchAddress,
		static_cast<UPTRINT>(Program->FrameAlignment));
	const int32 FrameOffset =
		static_cast<int32>(FrameAddress - ScratchAddress);
	if (FrameOffset + Program->FrameSize > InvocationScratch.Num())
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_scratch_alignment_failed"),
			Program->DebugPath,
			TEXT("The runtime invocation scratch could not satisfy the cached frame alignment."));
		return false;
	}
	void* Frame = reinterpret_cast<void*>(FrameAddress);
	Program->Function->InitializeStruct(Frame);
	ON_SCOPE_EXIT
	{
		Program->Function->DestroyStruct(Frame);
	};

	for (const FValueCodecProgram& Parameter : Program->Parameters)
	{
		if (Parameter.Direction == EValueCodecDirection::Ref
			|| Parameter.Direction == EValueCodecDirection::Out)
		{
			if (Parameter.Direction == EValueCodecDirection::Ref
				&& (GuestMemory == nullptr
					|| !SetValueFromGuest(
						Parameter,
						static_cast<uint32>(
							Arguments[Parameter.ArgumentOffset]),
						*GuestMemory,
						InvocationContext,
						Frame,
						Details)))
			{
				SetDispatchFailure(
					OutResult,
					TEXT("binding_guest_read_failed"),
					Program->DebugPath + TEXT(":") + Parameter.Name,
					Details);
				return false;
			}
			continue;
		}

		if (!SetValueFromCells(
				Parameter,
				Arguments.Slice(
					Parameter.ArgumentOffset,
					Parameter.ArgumentWidth),
				GuestMemory,
				InvocationContext,
				Frame,
				Details))
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_argument_invalid"),
				Program->DebugPath + TEXT(":") + Parameter.Name,
				Details);
			return false;
		}
	}

	if (!PrepareHostEffect())
	{
		return false;
	}
	Receiver.ProcessEvent(Program->Function, Frame);

	for (const FValueCodecProgram& Parameter : Program->Parameters)
	{
		if ((Parameter.Direction == EValueCodecDirection::Ref
				|| Parameter.Direction == EValueCodecDirection::Out)
			&& (GuestMemory == nullptr
				|| !WriteValueToGuest(
					Parameter,
					static_cast<uint32>(
						Arguments[Parameter.ArgumentOffset]),
					*GuestMemory,
					InvocationContext,
					Frame,
					Details)))
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_guest_write_failed"),
				Program->DebugPath + TEXT(":") + Parameter.Name,
				Details);
			return false;
		}
	}

	if (Program->ReturnValue.Kind != EValueCodecKind::Void
		&& (GuestMemory == nullptr
			|| !WriteValueToGuest(
				Program->ReturnValue,
				static_cast<uint32>(
					Arguments[Program->ReturnValue.ArgumentOffset]),
				*GuestMemory,
				InvocationContext,
				Frame,
				Details)))
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_return_write_failed"),
			Program->DebugPath,
			Details);
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ReturnValue = 1;
	return true;
}
} // namespace UE::AvidScript::BindingPrivate
