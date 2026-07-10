#include "AvidScriptActorBinding.h"

#include "GameFramework/Actor.h"

bool FAvidScriptActorBinding::GetActorLocation(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	FVector& OutLocation,
	FAvidScriptActorBindingResult& OutResult)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult);
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
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	const FVector& Location,
	EAvidScriptActorWritePolicy WritePolicy,
	FAvidScriptActorBindingResult& OutResult)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult);
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
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	const FVector& Offset,
	EAvidScriptActorWritePolicy WritePolicy,
	FAvidScriptActorBindingResult& OutResult)
{
	FVector CurrentLocation = FVector::ZeroVector;
	if (!GetActorLocation(Registry, ActorHandle, CurrentLocation, OutResult))
	{
		return false;
	}

	return SetActorLocation(Registry, ActorHandle, CurrentLocation + Offset, WritePolicy, OutResult);
}

bool FAvidScriptActorBinding::GetActorRotation(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	FRotator& OutRotation,
	FAvidScriptActorBindingResult& OutResult)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult);
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
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	const FRotator& Rotation,
	EAvidScriptActorWritePolicy WritePolicy,
	FAvidScriptActorBindingResult& OutResult)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult);
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
	FAvidScriptActorBindingResult& OutResult)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult);
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
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	const FVector& Scale3D,
	EAvidScriptActorWritePolicy WritePolicy,
	FAvidScriptActorBindingResult& OutResult)
{
	AActor* Actor = ResolveActor(Registry, ActorHandle, OutResult);
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
AActor* FAvidScriptActorBinding::ResolveActor(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ActorHandle,
	FAvidScriptActorBindingResult& OutResult)
{
	FAvidScriptObjectHandleResult ObjectResult;
	AActor* Actor = Registry.ResolveObject<AActor>(ActorHandle, ObjectResult);
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

	SetSuccess(OutResult, ObjectResult, Actor->GetActorLocation(), Actor->GetActorRotation(), Actor->GetActorScale3D());
	return Actor;
}

void FAvidScriptActorBinding::SetSuccess(
	FAvidScriptActorBindingResult& OutResult,
	const FAvidScriptObjectHandleResult& ObjectResult,
	const FVector& Location,
	const FRotator& Rotation,
	const FVector& Scale3D)
{
	OutResult = FAvidScriptActorBindingResult();
	OutResult.bSucceeded = true;
	OutResult.Handle = ObjectResult.Handle;
	OutResult.ObjectPath = ObjectResult.ObjectPath;
	OutResult.Location = Location;
	OutResult.Rotation = Rotation;
	OutResult.Scale3D = Scale3D;
	OutResult.ObjectResult = ObjectResult;
}

void FAvidScriptActorBinding::SetFailure(
	FAvidScriptActorBindingResult& OutResult,
	const FAvidScriptObjectHandleResult& ObjectResult,
	const FString& ErrorCategory,
	const FString& NextAction)
{
	OutResult = FAvidScriptActorBindingResult();
	OutResult.Handle = ObjectResult.Handle;
	OutResult.ObjectPath = ObjectResult.ObjectPath;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.NextAction = NextAction;
	OutResult.ObjectResult = ObjectResult;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript actor binding error | category=%s | slot=%u | generation=%u | object=%s | next=%s"),
		*ErrorCategory,
		ObjectResult.Handle.Slot,
		ObjectResult.Handle.Generation,
		OutResult.ObjectPath.IsEmpty() ? TEXT("<none>") : *OutResult.ObjectPath,
		*NextAction);
}
