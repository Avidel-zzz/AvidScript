#include "Invocation/AvidScriptBindingPreparedInvocation.h"

#include "AvidScriptBindingLatent.h"
#include "Components/ActorComponent.h"
#include "Engine/LatentActionManager.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/Script.h"
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

AActor* ResolveNetworkActor(UObject& Receiver)
{
	if (AActor* Actor = Cast<AActor>(&Receiver))
	{
		return Actor;
	}
	if (const UActorComponent* Component = Cast<UActorComponent>(&Receiver))
	{
		return Component->GetOwner();
	}
	return nullptr;
}

bool PreflightNetworkInvocation(
	const FInvocationCodecProgram& Program,
	UObject& Receiver,
	const FAvidScriptBindingInvocationContext& InvocationContext,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	if (!Program.Network.IsNetworked())
	{
		return true;
	}
	if (InvocationContext.HostEffectJournal != nullptr)
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_network_reload_effect_unsupported"),
			Program.DebugPath,
			TEXT("Candidate reload cannot issue an irreversible network RPC."));
		return false;
	}

	AActor* Actor = ResolveNetworkActor(Receiver);
	if (Actor == nullptr)
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_network_owner_invalid"),
			Program.DebugPath,
			TEXT("The RPC receiver is not an Actor and has no Actor owner."));
		return false;
	}
	const bool bHasAuthority = Actor->HasAuthority();
	if ((Program.Network.Mode == EAvidScriptBindingNetworkMode::Client
			|| Program.Network.Mode
				== EAvidScriptBindingNetworkMode::Multicast)
		&& !bHasAuthority)
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_network_authority_denied"),
			Program.DebugPath,
			TEXT("Client and multicast RPCs may only be initiated by authority."));
		return false;
	}

	const int32 Callspace = Receiver.GetFunctionCallspace(
		Program.Function,
		nullptr);
	if (Callspace == FunctionCallspace::Absorbed)
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_network_call_absorbed"),
			Program.DebugPath,
			TEXT("UE networking absorbed the RPC for the receiver's current role, ownership, or connection state."));
		return false;
	}
	if (Program.Network.Mode == EAvidScriptBindingNetworkMode::Server
		&& !bHasAuthority
		&& (Callspace & FunctionCallspace::Remote) == 0)
	{
		SetDispatchFailure(
			OutResult,
			TEXT("binding_network_authority_denied"),
			Program.DebugPath,
			TEXT("A non-authority Server RPC requires UE Remote callspace."));
		return false;
	}
	return true;
}

struct FGuestOutputRange
{
	uint64 Begin = 0;
	uint64 End = 0;
	FString Source;
};

struct FGuestOutputTarget
{
	const FValueCodecProgram* Value = nullptr;
	uint32 GuestAddress = 0;
	FString PreflightFailureCategory;
	FString EncodeFailureCategory;
	FString Source;
	FPreparedValueOutput PreparedOutput;
};
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
	if (!PreflightNetworkInvocation(
			*Program,
			Receiver,
			InvocationContext,
			OutResult))
	{
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
		uint32 GuestAddress = 0;
		FCodecOutputTransaction OutputTransaction;
		FPreparedValueOutput PreparedOutput;
		bool bOutputCommitted = false;
		ON_SCOPE_EXIT
		{
			if (!bOutputCommitted)
			{
				OutputTransaction.Rollback(InvocationContext);
			}
		};
		if (GuestMemory == nullptr
			|| !ResolveGuestAddress(
				Arguments[Program->ReturnValue.ArgumentOffset],
				static_cast<uint32>(Program->ReturnValue.WireSize),
				GuestAddress,
				Details)
			|| !PreflightValueOutput(
				Program->ReturnValue,
				GuestAddress,
				*GuestMemory,
				InvocationContext,
				OutputTransaction,
				PreparedOutput,
				Details)
			|| !WriteValueToGuest(
				Program->ReturnValue,
				InvocationContext,
				&Receiver,
				OutputTransaction,
				PreparedOutput,
				Details))
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_property_read_failed"),
				Program->DebugPath,
				Details);
			return false;
		}
		PublishValueOutput(PreparedOutput);
		OutputTransaction.Commit();
		bOutputCommitted = true;
		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		return true;
	}
	if (Program->Kind
			== EAvidScriptBindingInvocationKind::ReflectedPropertyWrite
		&& Program->Function == nullptr)
	{
		if (Program->Parameters[0].Kind == EValueCodecKind::StructWire)
		{
			uint32 GuestAddress = 0;
			const UPTRINT ScratchAddress = reinterpret_cast<UPTRINT>(InvocationScratch.GetData());
			const UPTRINT ValueAddress = Align(
				ScratchAddress,
				static_cast<UPTRINT>(Program->FrameAlignment));
			void* TemporaryValue = reinterpret_cast<void*>(ValueAddress);
			if (GuestMemory == nullptr
				|| Program->Parameters[0].StructType == nullptr
				|| ValueAddress - ScratchAddress + Program->FrameSize
					> static_cast<UPTRINT>(InvocationScratch.Num())
				|| !ResolveGuestAddress(
					Arguments[Program->Parameters[0].ArgumentOffset],
					static_cast<uint32>(Program->Parameters[0].WireSize),
					GuestAddress,
					Details))
			{
				SetDispatchFailure(
					OutResult,
					TEXT("binding_property_write_failed"),
					Program->DebugPath,
					Details.IsEmpty() ? TEXT("The temporary struct property frame is invalid.") : Details);
				return false;
			}
			Program->Parameters[0].StructType->InitializeStruct(TemporaryValue);
			ON_SCOPE_EXIT
			{
				Program->Parameters[0].StructType->DestroyStruct(TemporaryValue);
			};
			if (!SetStructValueFromGuest(
					Program->Parameters[0],
					GuestAddress,
					*GuestMemory,
					InvocationContext,
					TemporaryValue,
					Details)
				|| !PrepareHostEffect())
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
			void* Destination = Program->Parameters[0].Property
				->ContainerPtrToValuePtr<void>(&Receiver);
			Program->Parameters[0].Property->CopyCompleteValue(
				Destination,
				TemporaryValue);
			OutResult.bSucceeded = true;
			OutResult.ReturnValue = 1;
			return true;
		}
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

	TArray<uint32, TInlineAllocator<16>> ParameterGuestAddresses;
	ParameterGuestAddresses.SetNumZeroed(Program->Parameters.Num());
	uint32 ReturnGuestAddress = 0;
	TArray<FGuestOutputRange, TInlineAllocator<16>> WritableGuestRanges;
	TArray<FGuestOutputTarget, TInlineAllocator<16>> OutputTargets;
	FCodecOutputTransaction OutputTransaction;
	bool bOutputCommitted = false;
	ON_SCOPE_EXIT
	{
		if (!bOutputCommitted)
		{
			OutputTransaction.Rollback(InvocationContext);
		}
	};
	const auto PreflightGuestOutput = [&Arguments,
		GuestMemory,
		&OutResult,
		&Details,
		&WritableGuestRanges,
		&OutputTargets](
		const FValueCodecProgram& Value,
		const FString& PreflightFailureCategory,
		const FString& EncodeFailureCategory,
		const FString& Source,
		uint32& OutGuestAddress)
	{
		OutGuestAddress = 0;
		if (GuestMemory == nullptr || Value.GuestStorageSize <= 0)
		{
			Details = GuestMemory == nullptr
				? TEXT("Writable guest output requires guest memory.")
				: TEXT("The cached guest output storage size is invalid.");
			SetDispatchFailure(
				OutResult,
				PreflightFailureCategory,
				Source,
				Details);
			return false;
		}

		const uint64 Begin = Arguments[Value.ArgumentOffset];
		const uint64 StorageSize = static_cast<uint64>(Value.GuestStorageSize);
		if (Begin > MAX_uint64 - StorageSize)
		{
			Details = TEXT("The guest output address range overflows 64-bit arithmetic.");
			SetDispatchFailure(
				OutResult,
				PreflightFailureCategory,
				Source,
				Details);
			return false;
		}
		const uint64 End = Begin + StorageSize;
		if (!ResolveGuestAddress(
				Begin,
				static_cast<uint32>(StorageSize),
				OutGuestAddress,
				Details))
		{
			SetDispatchFailure(
				OutResult,
				PreflightFailureCategory,
				Source,
				Details);
			return false;
		}

		for (const FGuestOutputRange& Existing : WritableGuestRanges)
		{
			if (Begin < Existing.End && Existing.Begin < End)
			{
				Details = FString::Printf(
					TEXT("Writable guest output '%s' range [%llu, %llu) overlaps '%s' range [%llu, %llu)."),
					*Source,
					static_cast<unsigned long long>(Begin),
					static_cast<unsigned long long>(End),
					*Existing.Source,
					static_cast<unsigned long long>(Existing.Begin),
					static_cast<unsigned long long>(Existing.End));
				SetDispatchFailure(
					OutResult,
					TEXT("binding_guest_output_overlap"),
					Source,
					Details);
				return false;
			}
		}
		WritableGuestRanges.Add(FGuestOutputRange{ Begin, End, Source });
		FGuestOutputTarget& Target = OutputTargets.AddDefaulted_GetRef();
		Target.Value = &Value;
		Target.GuestAddress = OutGuestAddress;
		Target.PreflightFailureCategory = PreflightFailureCategory;
		Target.EncodeFailureCategory = EncodeFailureCategory;
		Target.Source = Source;
		return true;
	};

	for (int32 ParameterIndex = 0;
		ParameterIndex < Program->Parameters.Num();
		++ParameterIndex)
	{
		const FValueCodecProgram& Parameter =
			Program->Parameters[ParameterIndex];
		if (Parameter.Direction != EValueCodecDirection::Ref
			&& Parameter.Direction != EValueCodecDirection::Out)
		{
			continue;
		}
		if (!PreflightGuestOutput(
				Parameter,
				TEXT("binding_guest_output_preflight_failed"),
				TEXT("binding_guest_write_failed"),
				Program->DebugPath + TEXT(":") + Parameter.Name,
				ParameterGuestAddresses[ParameterIndex]))
		{
			return false;
		}
	}
	if (Program->ReturnValue.Kind != EValueCodecKind::Void
		&& !PreflightGuestOutput(
			Program->ReturnValue,
			TEXT("binding_return_preflight_failed"),
			TEXT("binding_return_write_failed"),
			Program->DebugPath,
			ReturnGuestAddress))
	{
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

	for (int32 ParameterIndex = 0;
		ParameterIndex < Program->Parameters.Num();
		++ParameterIndex)
	{
		const FValueCodecProgram& Parameter =
			Program->Parameters[ParameterIndex];
		if (Parameter.Direction == EValueCodecDirection::Ref
			|| Parameter.Direction == EValueCodecDirection::Out)
		{
			if (Parameter.Direction == EValueCodecDirection::Ref
				&& (GuestMemory == nullptr
					|| !SetValueFromGuest(
						Parameter,
						ParameterGuestAddresses[ParameterIndex],
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

	if (Program->bLatent)
	{
		if (Program->LatentInfoProperty == nullptr
			|| Program->CallbackIdArgumentOffset < 0
			|| !Arguments.IsValidIndex(Program->CallbackIdArgumentOffset)
			|| Program->ReturnValue.Kind != EValueCodecKind::Void
			|| !OutputTargets.IsEmpty()
			|| InvocationContext.LatentHost == nullptr
			|| !InvocationContext.World.IsValid())
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_latent_context_invalid"),
				Program->DebugPath,
				TEXT("The latent invocation has no live continuation host, world, or immutable hidden-parameter plan."));
			return false;
		}

		FAvidScriptBindingLatentReservation Reservation;
		const int32 CallbackId = static_cast<int32>(
			Arguments[Program->CallbackIdArgumentOffset]);
		const bool bReserved = Program->LatentCompletion.IsProvider()
			? InvocationContext.LatentHost->BeginLatentWithCompletion(
				CallbackId,
				Program->LatentCompletion,
				Reservation)
			: InvocationContext.LatentHost->BeginLatent(
				CallbackId,
				Reservation);
		if (!bReserved
			|| !Reservation.IsValid())
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_latent_reservation_failed"),
				Program->DebugPath,
				TEXT("The session could not reserve a continuation token for the latent action."));
			return false;
		}

		bool bLatentCommitted = false;
		ON_SCOPE_EXIT
		{
			if (!bLatentCommitted)
			{
				InvocationContext.LatentHost->AbortLatent(Reservation.Token);
			}
		};
		FLatentActionInfo LatentInfo;
		LatentInfo.CallbackTarget = Reservation.CallbackTarget;
		LatentInfo.ExecutionFunction = Reservation.ExecutionFunction;
		LatentInfo.UUID = Reservation.UUID;
		LatentInfo.Linkage = Reservation.Linkage;
		*Program->LatentInfoProperty
			->ContainerPtrToValuePtr<FLatentActionInfo>(Frame) = LatentInfo;
		if (Program->WorldContextProperty != nullptr)
		{
			Program->WorldContextProperty->SetObjectPropertyValue_InContainer(
				Frame,
				InvocationContext.World.Get());
		}

		Receiver.ProcessEvent(Program->Function, Frame);
		if (!InvocationContext.LatentHost->CommitLatent(Reservation.Token))
		{
			SetDispatchFailure(
				OutResult,
				TEXT("binding_latent_registration_failed"),
				Program->DebugPath,
				TEXT("ProcessEvent did not register or synchronously complete the reserved latent action."));
			return false;
		}

		bLatentCommitted = true;
		bOutputCommitted = true;
		OutResult.bSucceeded = true;
		OutResult.ReturnValue = 1;
		OutResult.ReturnValueI64 = Reservation.Token;
		return true;
	}

	for (FGuestOutputTarget& Target : OutputTargets)
	{
		if (Target.Value == nullptr
			|| GuestMemory == nullptr
			|| !PreflightValueOutput(
				*Target.Value,
				Target.GuestAddress,
				*GuestMemory,
				InvocationContext,
				OutputTransaction,
				Target.PreparedOutput,
				Details))
		{
			SetDispatchFailure(
				OutResult,
				Target.PreflightFailureCategory,
				Target.Source,
				Details);
			return false;
		}
	}

	if (!PrepareHostEffect())
	{
		return false;
	}
	Receiver.ProcessEvent(Program->Function, Frame);

	for (FGuestOutputTarget& Target : OutputTargets)
	{
		if (Target.Value == nullptr
			|| !WriteValueToGuest(
				*Target.Value,
				InvocationContext,
				Frame,
				OutputTransaction,
				Target.PreparedOutput,
				Details))
		{
			SetDispatchFailure(
				OutResult,
				Target.EncodeFailureCategory,
				Target.Source,
				Details);
			return false;
		}
	}

	for (FGuestOutputTarget& Target : OutputTargets)
	{
		PublishValueOutput(Target.PreparedOutput);
	}

	OutputTransaction.Commit();
	bOutputCommitted = true;
	OutResult.bSucceeded = true;
	OutResult.ReturnValue = 1;
	return true;
}
} // namespace UE::AvidScript::BindingPrivate
