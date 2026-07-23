#include "AvidScriptSceneComponentBinding.h"

#include "Components/SceneComponent.h"

bool FAvidScriptSceneComponentBinding::GetWorldLocation(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ComponentHandle,
	FVector& OutWorldLocation,
	FAvidScriptSceneComponentBindingResult& OutResult,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	USceneComponent* Component = ResolveSceneComponent(Registry, ComponentHandle, OutResult, DiagnosticsPolicy);
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
	FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ComponentHandle,
	const FVector& WorldLocation,
	EAvidScriptActorWritePolicy WritePolicy,
	FAvidScriptSceneComponentBindingResult& OutResult,
	IAvidScriptBindingHostEffectJournal* HostEffectJournal,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	USceneComponent* Component = ResolveSceneComponent(Registry, ComponentHandle, OutResult, DiagnosticsPolicy);
	if (Component == nullptr)
	{
		return false;
	}

	if (WritePolicy != EAvidScriptActorWritePolicy::AllowWrites)
	{
		SetFailure(OutResult, OutResult.ObjectResult, TEXT("write_denied"), TEXT("Enable the component write policy only for host-authorized script calls."));
		return false;
	}
	if (!PrepareTransformWrite(Registry, ComponentHandle, *Component, HostEffectJournal, OutResult))
	{
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
	FAvidScriptSceneComponentBindingResult& OutResult,
	EAvidScriptBindingDiagnosticsPolicy DiagnosticsPolicy)
{
	FAvidScriptObjectHandleResult ObjectResult;
	USceneComponent* Component = Registry.ResolveObject<USceneComponent>(
		ComponentHandle,
		ObjectResult,
		DiagnosticsPolicy == EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath);
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

bool FAvidScriptSceneComponentBinding::PrepareTransformWrite(
	FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ComponentHandle,
	USceneComponent& Component,
	IAvidScriptBindingHostEffectJournal* HostEffectJournal,
	FAvidScriptSceneComponentBindingResult& OutResult)
{
	if (HostEffectJournal == nullptr)
	{
		return true;
	}

	FAvidScriptBindingHostEffectPrepareResult PrepareResult;
	if (HostEffectJournal->PrepareEffect(
		Registry,
		ComponentHandle,
		Component,
		EAvidScriptBindingReloadEffect::SceneComponentTransform,
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

void FAvidScriptSceneComponentBinding::SetSuccess(
	FAvidScriptSceneComponentBindingResult& OutResult,
	const FAvidScriptObjectHandleResult& ObjectResult,
	const FVector& WorldLocation)
{
	FAvidScriptSceneComponentBindingResult SuccessResult;
	SuccessResult.bSucceeded = true;
	SuccessResult.Handle = ObjectResult.Handle;
	SuccessResult.ObjectPath = ObjectResult.ObjectPath;
	SuccessResult.WorldLocation = WorldLocation;
	SuccessResult.ObjectResult = ObjectResult;
	OutResult = MoveTemp(SuccessResult);
}

void FAvidScriptSceneComponentBinding::SetFailure(
	FAvidScriptSceneComponentBindingResult& OutResult,
	const FAvidScriptObjectHandleResult& ObjectResult,
	const FString& ErrorCategory,
	const FString& NextAction)
{
	FAvidScriptSceneComponentBindingResult FailureResult;
	FailureResult.Handle = ObjectResult.Handle;
	FailureResult.ObjectPath = ObjectResult.ObjectPath;
	FailureResult.ErrorCategory = ErrorCategory;
	FailureResult.NextAction = NextAction;
	FailureResult.ObjectResult = ObjectResult;
	FailureResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript scene component binding error | category=%s | slot=%u | generation=%u | object=%s | next=%s"),
		*ErrorCategory,
		ObjectResult.Handle.Slot,
		ObjectResult.Handle.Generation,
		FailureResult.ObjectPath.IsEmpty() ? TEXT("<none>") : *FailureResult.ObjectPath,
		*NextAction);
	OutResult = MoveTemp(FailureResult);
}
