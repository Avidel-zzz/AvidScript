#include "AvidScriptSceneComponentBinding.h"

#include "Components/SceneComponent.h"

bool FAvidScriptSceneComponentBinding::GetWorldLocation(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ComponentHandle,
	FVector& OutWorldLocation,
	FAvidScriptSceneComponentBindingResult& OutResult)
{
	USceneComponent* Component = ResolveSceneComponent(Registry, ComponentHandle, OutResult);
	if (Component == nullptr)
	{
		OutWorldLocation = FVector::ZeroVector;
		return false;
	}

	OutWorldLocation = Component->GetComponentLocation();
	SetSuccess(OutResult, OutResult.ObjectResult, OutWorldLocation);
	return true;
}

bool FAvidScriptSceneComponentBinding::SetWorldLocation(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ComponentHandle,
	const FVector& WorldLocation,
	EAvidScriptActorWritePolicy WritePolicy,
	FAvidScriptSceneComponentBindingResult& OutResult)
{
	USceneComponent* Component = ResolveSceneComponent(Registry, ComponentHandle, OutResult);
	if (Component == nullptr)
	{
		return false;
	}

	if (WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetFailure(OutResult, OutResult.ObjectResult, TEXT("write_denied"), TEXT("Enable the component write policy only for host-authorized script calls."));
		return false;
	}

	Component->SetWorldLocation(WorldLocation, false, nullptr, ETeleportType::TeleportPhysics);
	const FVector AppliedLocation = Component->GetComponentLocation();
	if (!AppliedLocation.Equals(WorldLocation, 0.01))
	{
		SetFailure(OutResult, OutResult.ObjectResult, TEXT("movement_failed"), TEXT("Verify that the scene component can move in the current world state."));
		OutResult.WorldLocation = AppliedLocation;
		return false;
	}

	SetSuccess(OutResult, OutResult.ObjectResult, AppliedLocation);
	return true;
}

USceneComponent* FAvidScriptSceneComponentBinding::ResolveSceneComponent(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ComponentHandle,
	FAvidScriptSceneComponentBindingResult& OutResult)
{
	FAvidScriptObjectHandleResult ObjectResult;
	USceneComponent* Component = Registry.ResolveObject<USceneComponent>(ComponentHandle, ObjectResult);
	if (Component == nullptr)
	{
		SetFailure(
			OutResult,
			ObjectResult,
			ObjectResult.ErrorCategory.IsEmpty() ? FString(TEXT("invalid_handle")) : ObjectResult.ErrorCategory,
			ObjectResult.NextAction.IsEmpty() ? FString(TEXT("Use a live USceneComponent handle returned by the active AvidScript object registry.")) : ObjectResult.NextAction);
		return nullptr;
	}

	SetSuccess(OutResult, ObjectResult, Component->GetComponentLocation());
	return Component;
}

void FAvidScriptSceneComponentBinding::SetSuccess(
	FAvidScriptSceneComponentBindingResult& OutResult,
	const FAvidScriptObjectHandleResult& ObjectResult,
	const FVector& WorldLocation)
{
	OutResult = FAvidScriptSceneComponentBindingResult();
	OutResult.bSucceeded = true;
	OutResult.Handle = ObjectResult.Handle;
	OutResult.ObjectPath = ObjectResult.ObjectPath;
	OutResult.WorldLocation = WorldLocation;
	OutResult.ObjectResult = ObjectResult;
}

void FAvidScriptSceneComponentBinding::SetFailure(
	FAvidScriptSceneComponentBindingResult& OutResult,
	const FAvidScriptObjectHandleResult& ObjectResult,
	const FString& ErrorCategory,
	const FString& NextAction)
{
	OutResult = FAvidScriptSceneComponentBindingResult();
	OutResult.Handle = ObjectResult.Handle;
	OutResult.ObjectPath = ObjectResult.ObjectPath;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.NextAction = NextAction;
	OutResult.ObjectResult = ObjectResult;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript scene component binding error | category=%s | slot=%u | generation=%u | object=%s | next=%s"),
		*ErrorCategory,
		ObjectResult.Handle.Slot,
		ObjectResult.Handle.Generation,
		OutResult.ObjectPath.IsEmpty() ? TEXT("<none>") : *OutResult.ObjectPath,
		*NextAction);
}
