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
	SetSuccess(OutResult, OutResult.ObjectResult, OutLocation);
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

	SetSuccess(OutResult, OutResult.ObjectResult, AppliedLocation);
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

	SetSuccess(OutResult, ObjectResult, Actor->GetActorLocation());
	return Actor;
}

void FAvidScriptActorBinding::SetSuccess(
	FAvidScriptActorBindingResult& OutResult,
	const FAvidScriptObjectHandleResult& ObjectResult,
	const FVector& Location)
{
	OutResult = FAvidScriptActorBindingResult();
	OutResult.bSucceeded = true;
	OutResult.Handle = ObjectResult.Handle;
	OutResult.ObjectPath = ObjectResult.ObjectPath;
	OutResult.Location = Location;
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
