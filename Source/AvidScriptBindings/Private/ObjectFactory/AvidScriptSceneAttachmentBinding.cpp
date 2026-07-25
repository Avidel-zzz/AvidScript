#include "AvidScriptSceneAttachmentBinding.h"

#include "AvidScriptHash.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"

namespace
{
constexpr uint32 AttachmentRuleMask = 0x3u;
constexpr uint32 AttachmentAllowedMask =
	AttachmentRuleMask | FAvidScriptSceneAttachmentRules::WeldSimulatedBodiesBit;
constexpr uint32 DetachmentAllowedMask = AttachmentRuleMask;

FAvidScriptSceneAttachmentBindingSpec MakeAvidScriptSceneAttachmentSpec(
	const EAvidScriptBindingInvocationKind Kind,
	const TCHAR* CanonicalIdentity,
	const TCHAR* ImportName,
	const TCHAR* Signature)
{
	FAvidScriptSceneAttachmentBindingSpec Spec;
	Spec.Kind = Kind;
	Spec.StableId = FAvidScriptHash::Sha256HexUtf8(CanonicalIdentity);
	Spec.ModuleName = TEXT("avidscript");
	Spec.ImportName = ImportName;
	Spec.Signature = Signature;
	return Spec;
}

void SetAvidScriptSceneAttachmentFailure(
	FAvidScriptObjectHandleResult& OutResult,
	const FAvidScriptObjectHandle& Handle,
	const FString& ErrorCategory,
	const FString& NextAction,
	const FString& Details = FString())
{
	OutResult = FAvidScriptObjectHandleResult();
	OutResult.Handle = Handle;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.NextAction = NextAction;
	const FString DetailsSuffix = Details.IsEmpty()
		? FString()
		: FString(TEXT(" | details=")) + Details;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript scene attachment error | category=%s | slot=%u | generation=%u | next=%s%s"),
		*ErrorCategory,
		Handle.Slot,
		Handle.Generation,
		*NextAction,
		*DetailsSuffix);
}

void SetAvidScriptSceneAttachmentFailure(
	FAvidScriptObjectHandleResult& OutResult,
	const FAvidScriptObjectHandleResult& ResolveResult)
{
	SetAvidScriptSceneAttachmentFailure(
		OutResult,
		ResolveResult.Handle,
		ResolveResult.ErrorCategory.IsEmpty()
			? FString(TEXT("binding_attachment_handle_invalid"))
			: ResolveResult.ErrorCategory,
		ResolveResult.NextAction.IsEmpty()
			? FString(TEXT("Use live USceneComponent handles from the active Runtime Session."))
			: ResolveResult.NextAction);
}

void SetAvidScriptSceneAttachmentSuccess(
	FAvidScriptObjectHandleResult& OutResult,
	const FAvidScriptObjectHandle& Handle)
{
	OutResult = FAvidScriptObjectHandleResult();
	OutResult.bSucceeded = true;
	OutResult.Handle = Handle;
}

bool TryDecodeAttachmentRules(
	const uint32 Rules,
	FAttachmentTransformRules& OutRules)
{
	const uint32 RuleValue = Rules & AttachmentRuleMask;
	if ((Rules & ~AttachmentAllowedMask) != 0
		|| RuleValue > static_cast<uint32>(EAttachmentRule::SnapToTarget))
	{
		return false;
	}

	OutRules = FAttachmentTransformRules(
		static_cast<EAttachmentRule>(RuleValue),
		(Rules & FAvidScriptSceneAttachmentRules::WeldSimulatedBodiesBit) != 0);
	return true;
}

bool TryDecodeDetachmentRules(
	const uint32 Rules,
	FDetachmentTransformRules& OutRules)
{
	const uint32 RuleValue = Rules & AttachmentRuleMask;
	if ((Rules & ~DetachmentAllowedMask) != 0
		|| RuleValue > static_cast<uint32>(EDetachmentRule::KeepWorld))
	{
		return false;
	}

	OutRules = FDetachmentTransformRules(
		static_cast<EDetachmentRule>(RuleValue),
		true);
	return true;
}
} // namespace

TConstArrayView<FAvidScriptSceneAttachmentBindingSpec>
FAvidScriptSceneAttachmentBinding::GetSpecs()
{
	static const TArray<FAvidScriptSceneAttachmentBindingSpec> Specs = {
		MakeAvidScriptSceneAttachmentSpec(
			EAvidScriptBindingInvocationKind::SceneComponentAttach,
			TEXT("avidscript.scene_attachment.v1|attach|child_handle,parent_handle,rules->i32"),
			TEXT("avid_scene_component_attach"),
			TEXT("(iiiii)i")),
		MakeAvidScriptSceneAttachmentSpec(
			EAvidScriptBindingInvocationKind::SceneComponentDetach,
			TEXT("avidscript.scene_attachment.v1|detach|child_handle,rules->i32"),
			TEXT("avid_scene_component_detach"),
			TEXT("(iii)i"))
	};
	return Specs;
}

bool FAvidScriptSceneAttachmentBinding::Attach(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ChildHandle,
	const FAvidScriptObjectHandle& ParentHandle,
	const uint32 Rules,
	FAvidScriptObjectHandleResult& OutResult)
{
	FAvidScriptObjectHandleResult ResolveResult;
	USceneComponent* const Child = Registry.ResolveObject<USceneComponent>(
		ChildHandle,
		ResolveResult,
		false);
	if (Child == nullptr)
	{
		SetAvidScriptSceneAttachmentFailure(OutResult, ResolveResult);
		return false;
	}

	USceneComponent* const Parent = Registry.ResolveObject<USceneComponent>(
		ParentHandle,
		ResolveResult,
		false);
	if (Parent == nullptr)
	{
		SetAvidScriptSceneAttachmentFailure(OutResult, ResolveResult);
		return false;
	}

	FAttachmentTransformRules AttachmentRules =
		FAttachmentTransformRules::KeepRelativeTransform;
	if (!TryDecodeAttachmentRules(Rules, AttachmentRules))
	{
		SetAvidScriptSceneAttachmentFailure(
			OutResult,
			ChildHandle,
			TEXT("binding_attachment_rules_invalid"),
			TEXT("Use KeepRelative, KeepWorld, or SnapToTarget with the optional weld bit only."));
		return false;
	}

	UWorld* const ChildWorld = Child->GetWorld();
	UWorld* const ParentWorld = Parent->GetWorld();
	if (!IsValid(ChildWorld) || ChildWorld != ParentWorld)
	{
		SetAvidScriptSceneAttachmentFailure(
			OutResult,
			ChildHandle,
			TEXT("binding_attachment_world_mismatch"),
			TEXT("Attach only live scene components that belong to the same World."));
		return false;
	}
	if (Child == Parent || Parent->IsAttachedTo(Child))
	{
		SetAvidScriptSceneAttachmentFailure(
			OutResult,
			ChildHandle,
			TEXT("binding_attachment_cycle"),
			TEXT("Choose a parent that is neither the child nor one of its descendants."));
		return false;
	}

	if (!Child->AttachToComponent(Parent, AttachmentRules, NAME_None)
		|| Child->GetAttachParent() != Parent)
	{
		SetAvidScriptSceneAttachmentFailure(
			OutResult,
			ChildHandle,
			TEXT("binding_attachment_failed"),
			TEXT("Verify component mobility and attachment preconditions in the active World."));
		return false;
	}

	SetAvidScriptSceneAttachmentSuccess(OutResult, ChildHandle);
	return true;
}

bool FAvidScriptSceneAttachmentBinding::Detach(
	const FAvidScriptObjectRegistry& Registry,
	const FAvidScriptObjectHandle& ChildHandle,
	const uint32 Rules,
	FAvidScriptObjectHandleResult& OutResult)
{
	FAvidScriptObjectHandleResult ResolveResult;
	USceneComponent* const Child = Registry.ResolveObject<USceneComponent>(
		ChildHandle,
		ResolveResult,
		false);
	if (Child == nullptr)
	{
		SetAvidScriptSceneAttachmentFailure(OutResult, ResolveResult);
		return false;
	}

	FDetachmentTransformRules DetachmentRules =
		FDetachmentTransformRules::KeepRelativeTransform;
	if (!TryDecodeDetachmentRules(Rules, DetachmentRules))
	{
		SetAvidScriptSceneAttachmentFailure(
			OutResult,
			ChildHandle,
			TEXT("binding_attachment_rules_invalid"),
			TEXT("Use KeepRelative or KeepWorld detachment rules without reserved bits."));
		return false;
	}

	Child->DetachFromComponent(DetachmentRules);
	if (Child->GetAttachParent() != nullptr)
	{
		SetAvidScriptSceneAttachmentFailure(
			OutResult,
			ChildHandle,
			TEXT("binding_detachment_failed"),
			TEXT("Verify that the scene component can detach in the active World state."));
		return false;
	}

	SetAvidScriptSceneAttachmentSuccess(OutResult, ChildHandle);
	return true;
}
