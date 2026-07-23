#include "AvidScriptActorBinding.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"

bool FAvidScriptActorBinding::GetActorTransform(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	FAvidScriptActorTransformSnapshot& OutTransform,
	FAvidScriptActorBindingResult& OutResult,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	OutTransform = FAvidScriptActorTransformSnapshot();
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult, DiagnosticsPolicy);
	if (Actor == nullptr)
	{
		return false;
	}

	const FTransform Transform = Actor->GetActorTransform();
	OutTransform.Handle = ActorHandle;
	OutTransform.Location = Transform.GetLocation();
	OutTransform.Rotation = Transform.Rotator();
	OutTransform.Scale3D = Transform.GetScale3D();
	SetSuccess(OutResult, OutResult.ObjectResult, OutTransform.Location, OutTransform.Rotation, OutTransform.Scale3D);
	return true;
}

bool FAvidScriptActorBinding::GetActorLocation(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	FVector& OutLocation,
	FAvidScriptActorBindingResult& OutResult,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult, DiagnosticsPolicy);
	if (Actor == nullptr)
	{
		OutLocation = FVector::ZeroVector;
		return false;
	}

	OutLocation = Actor->GetActorLocation();
	SetSuccess(OutResult, OutResult.ObjectResult, OutLocation, Actor->GetActorRotation(), Actor->GetActorScale3D());
	return true;
}

bool FAvidScriptActorBinding::SetActorLocation(
	FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	const FVector& Location,
	EAvidScriptActorWritePolicy WritePolicy,
	FAvidScriptActorBindingResult& OutResult,
	IAvidScriptBindingHostEffectJournal* HostEffectJournal,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult, DiagnosticsPolicy);
	if (Actor == nullptr)
	{
		return false;
	}

	if (WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetFailure(
			OutResult,
			OutResult.ObjectResult,
			TEXT("write_denied"),
			TEXT("Enable the actor write policy only for host-authorized script calls."));
		return false;
	}
	if (!PrepareTransformWrite(Registry, ActorHandle, *Actor, HostEffectJournal, OutResult))
	{
		return false;
	}

	Actor->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	const FVector AppliedLocation = Actor->GetActorLocation();
	if (!AppliedLocation.Equals(Location))
	{
		SetFailure(
			OutResult,
			OutResult.ObjectResult,
			TEXT("movement_failed"),
			TEXT("Verify that the actor can be moved by the host in the current world state."));
		OutResult.Location = AppliedLocation;
		return false;
	}

	SetSuccess(OutResult, OutResult.ObjectResult, AppliedLocation, Actor->GetActorRotation(), Actor->GetActorScale3D());
	return true;
}

bool FAvidScriptActorBinding::AddActorLocationOffset(
	FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	const FVector& Offset,
	EAvidScriptActorWritePolicy WritePolicy,
	FAvidScriptActorBindingResult& OutResult,
	IAvidScriptBindingHostEffectJournal* HostEffectJournal,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	FVector CurrentLocation = FVector::ZeroVector;
	if (!GetActorLocation(Registry, ActorHandle, CurrentLocation, OutResult, DiagnosticsPolicy))
	{
		return false;
	}

	return SetActorLocation(
		Registry,
		ActorHandle,
		CurrentLocation + Offset,
		WritePolicy,
		OutResult,
		HostEffectJournal,
		DiagnosticsPolicy);
}

bool FAvidScriptActorBinding::GetActorRotation(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	FRotator& OutRotation,
	FAvidScriptActorBindingResult& OutResult,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult, DiagnosticsPolicy);
	if (Actor == nullptr)
	{
		OutRotation = FRotator::ZeroRotator;
		return false;
	}

	OutRotation = Actor->GetActorRotation();
	SetSuccess(OutResult, OutResult.ObjectResult, Actor->GetActorLocation(), OutRotation, Actor->GetActorScale3D());
	return true;
}

bool FAvidScriptActorBinding::SetActorRotation(
	FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	const FRotator& Rotation,
	EAvidScriptActorWritePolicy WritePolicy,
	FAvidScriptActorBindingResult& OutResult,
	IAvidScriptBindingHostEffectJournal* HostEffectJournal,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult, DiagnosticsPolicy);
	if (Actor == nullptr)
	{
		return false;
	}

	if (WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetFailure(
			OutResult,
			OutResult.ObjectResult,
			TEXT("write_denied"),
			TEXT("Enable the actor write policy only for host-authorized script calls."));
		return false;
	}
	if (!PrepareTransformWrite(Registry, ActorHandle, *Actor, HostEffectJournal, OutResult))
	{
		return false;
	}

	Actor->SetActorRotation(Rotation, ETeleportType::TeleportPhysics);
	const FRotator AppliedRotation = Actor->GetActorRotation();
	if (!AppliedRotation.Equals(Rotation, 0.01))
	{
		SetFailure(
			OutResult,
			OutResult.ObjectResult,
			TEXT("rotation_failed"),
			TEXT("Verify that the actor can be rotated by the host in the current world state."));
		OutResult.Location = Actor->GetActorLocation();
		OutResult.Rotation = AppliedRotation;
		return false;
	}

	SetSuccess(OutResult, OutResult.ObjectResult, Actor->GetActorLocation(), AppliedRotation, Actor->GetActorScale3D());
	return true;
}

bool FAvidScriptActorBinding::GetActorScale3D(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	FVector& OutScale3D,
	FAvidScriptActorBindingResult& OutResult,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult, DiagnosticsPolicy);
	if (Actor == nullptr)
	{
		OutScale3D = FVector::ZeroVector;
		return false;
	}

	OutScale3D = Actor->GetActorScale3D();
	SetSuccess(OutResult, OutResult.ObjectResult, Actor->GetActorLocation(), Actor->GetActorRotation(), OutScale3D);
	return true;
}

bool FAvidScriptActorBinding::SetActorScale3D(
	FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	const FVector& Scale3D,
	EAvidScriptActorWritePolicy WritePolicy,
	FAvidScriptActorBindingResult& OutResult,
	IAvidScriptBindingHostEffectJournal* HostEffectJournal,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult, DiagnosticsPolicy);
	if (Actor == nullptr)
	{
		return false;
	}

	if (WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetFailure(
			OutResult,
			OutResult.ObjectResult,
			TEXT("write_denied"),
			TEXT("Enable the actor write policy only for host-authorized script calls."));
		return false;
	}
	if (!PrepareTransformWrite(Registry, ActorHandle, *Actor, HostEffectJournal, OutResult))
	{
		return false;
	}

	Actor->SetActorScale3D(Scale3D);
	const FVector AppliedScale = Actor->GetActorScale3D();
	if (!AppliedScale.Equals(Scale3D, 0.01))
	{
		SetFailure(
			OutResult,
			OutResult.ObjectResult,
			TEXT("scale_failed"),
			TEXT("Verify that the actor root component can be scaled by the host."));
		OutResult.Location = Actor->GetActorLocation();
		OutResult.Rotation = Actor->GetActorRotation();
		OutResult.Scale3D = AppliedScale;
		return false;
	}

	SetSuccess(OutResult, OutResult.ObjectResult, Actor->GetActorLocation(), Actor->GetActorRotation(), AppliedScale);
	return true;
}
bool FAvidScriptActorBinding::GetRootComponentHandle(
	FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	FAvidScriptObjectHandle& OutComponentHandle,
	FAvidScriptActorBindingResult& OutResult,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	OutComponentHandle = FAvidScriptObjectHandle();
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult, DiagnosticsPolicy);
	if (Actor == nullptr)
	{
		return false;
	}

	USceneComponent* RootComponent = Actor->GetRootComponent();
	if (!IsValid(RootComponent))
	{
		SetFailure(
			OutResult,
			OutResult.ObjectResult,
			TEXT("missing_root_component"),
			TEXT("Assign a live RootComponent before requesting it from the script host."));
		return false;
	}

	FAvidScriptObjectHandleResult RegisterResult;
	OutComponentHandle = Registry.RegisterObject(
		RootComponent,
		RegisterResult,
		DiagnosticsPolicy == EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);
	if (!RegisterResult.bSucceeded)
	{
		SetFailure(
			OutResult,
			RegisterResult,
			RegisterResult.ErrorCategory.IsEmpty() ? FString(TEXT("invalid_object")) : RegisterResult.ErrorCategory,
			RegisterResult.NextAction);
		return false;
	}

	SetSuccess(OutResult, OutResult.ObjectResult, Actor->GetActorLocation(), Actor->GetActorRotation(), Actor->GetActorScale3D());
	return true;
}

AActor* FAvidScriptActorBinding::ResolveActor(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	FAvidScriptActorBindingResult& OutResult,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	FAvidScriptObjectHandleResult ObjectResult;
	AActor* Actor = Registry.ResolveObject<AActor>(
		ActorHandle,
		ObjectResult,
		DiagnosticsPolicy == EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);
	if (Actor == nullptr)
	{
		SetFailure(
			OutResult,
			ObjectResult,
			ObjectResult.ErrorCategory.IsEmpty() ? FString(TEXT("invalid_handle")) : ObjectResult.ErrorCategory,
			ObjectResult.NextAction.IsEmpty()
				? FString(TEXT("Use a live AActor handle returned by the active AvidScript object registry."))
				: ObjectResult.NextAction);
		return nullptr;
	}

	OutResult = FAvidScriptActorBindingResult();
	OutResult.Handle = ObjectResult.Handle;
	OutResult.ObjectPath = ObjectResult.ObjectPath;
	OutResult.ObjectResult = ObjectResult;
	return Actor;
}

bool FAvidScriptActorBinding::PrepareTransformWrite(
	FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	AActor& Actor,
	IAvidScriptBindingHostEffectJournal* HostEffectJournal,
	FAvidScriptActorBindingResult& OutResult)
{
	if (HostEffectJournal == nullptr)
	{
		return true;
	}

	FAvidScriptBindingHostEffectPrepareResult PrepareResult;
	if (HostEffectJournal->PrepareEffect(
		Registry,
		ActorHandle,
		Actor,
		EAvidScriptBindingReloadEffect::ActorTransform,
		PrepareResult))
	{
		return true;
	}

	SetFailure(
		OutResult,
		OutResult.ObjectResult,
		PrepareResult.ErrorCategory.IsEmpty()
			? FString(TEXT("host_effect_snapshot_failed"))
			: PrepareResult.ErrorCategory,
		TEXT("Reject this candidate reload or add a reversible host effect adapter for the binding."));
	if (!PrepareResult.ErrorDetails.IsEmpty())
	{
		OutResult.ErrorMessage += FString::Printf(
			TEXT(" | effect_source=%s | details=%s"),
			PrepareResult.ErrorSource.IsEmpty() ? TEXT("<none>") : *PrepareResult.ErrorSource,
			*PrepareResult.ErrorDetails);
	}
	return false;
}

void FAvidScriptActorBinding::SetSuccess(
	FAvidScriptActorBindingResult& OutResult,
	const FAvidScriptObjectHandleResult& ObjectResult,
	const FVector& Location,
	const FRotator& Rotation,
	const FVector& Scale3D)
{
	FAvidScriptActorBindingResult SuccessResult;
	SuccessResult.bSucceeded = true;
	SuccessResult.Handle = ObjectResult.Handle;
	SuccessResult.ObjectPath = ObjectResult.ObjectPath;
	SuccessResult.Location = Location;
	SuccessResult.Rotation = Rotation;
	SuccessResult.Scale3D = Scale3D;
	SuccessResult.ObjectResult = ObjectResult;
	OutResult = MoveTemp(SuccessResult);
}

void FAvidScriptActorBinding::SetFailure(
	FAvidScriptActorBindingResult& OutResult,
	const FAvidScriptObjectHandleResult& ObjectResult,
	const FString& ErrorCategory,
	const FString& NextAction)
{
	FAvidScriptActorBindingResult FailureResult;
	FailureResult.Handle = ObjectResult.Handle;
	FailureResult.ObjectPath = ObjectResult.ObjectPath;
	FailureResult.ErrorCategory = ErrorCategory;
	FailureResult.NextAction = NextAction;
	FailureResult.ObjectResult = ObjectResult;
	FailureResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript actor binding error | category=%s | slot=%u | generation=%u | object=%s | next=%s"),
		*ErrorCategory,
		ObjectResult.Handle.Slot,
		ObjectResult.Handle.Generation,
		FailureResult.ObjectPath.IsEmpty() ? TEXT("<none>") : *FailureResult.ObjectPath,
		*NextAction);
	OutResult = MoveTemp(FailureResult);
}
