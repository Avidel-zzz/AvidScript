#include "AvidScriptObjectFactoryBinding.h"

#include "AvidScriptHash.h"
#include "AvidScriptObjectOwnership.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
FAvidScriptObjectFactoryBindingSpec MakeAvidScriptObjectFactorySpec(
	const EAvidScriptBindingInvocationKind Kind,
	const TCHAR* CanonicalIdentity,
	const TCHAR* ImportName,
	const TCHAR* Signature)
{
	FAvidScriptObjectFactoryBindingSpec Spec;
	Spec.Kind = Kind;
	Spec.StableId = FAvidScriptHash::Sha256HexUtf8(CanonicalIdentity);
	Spec.ModuleName = TEXT("avidscript");
	Spec.ImportName = ImportName;
	Spec.Signature = Signature;
	return Spec;
}

void SetAvidScriptObjectFactoryFailure(
	FAvidScriptObjectHandleResult& OutResult,
	const FAvidScriptObjectHandle& Handle,
	const TCHAR* ErrorCategory,
	const TCHAR* NextAction)
{
	OutResult = FAvidScriptObjectHandleResult();
	OutResult.Handle = Handle;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript object factory error | category=%s | slot=%u | generation=%u | next=%s"),
		ErrorCategory,
		Handle.Slot,
		Handle.Generation,
		NextAction);
}

void SetAvidScriptObjectFactorySuccess(
	FAvidScriptObjectHandleResult& OutResult,
	const FAvidScriptObjectHandle& Handle)
{
	OutResult = FAvidScriptObjectHandleResult();
	OutResult.bSucceeded = true;
	OutResult.Handle = Handle;
}

bool ValidateAvidScriptObjectFactoryPlan(
	const FAvidScriptObjectFactoryPlan& Plan,
	FAvidScriptObjectHandleResult& OutResult)
{
	const bool bClassValid = IsValid(Plan.ObjectClass)
		&& IsValid(Plan.RequiredOuterClass)
		&& !Plan.ObjectClass->HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
	const bool bIsActor = bClassValid
		&& Plan.ObjectClass->IsChildOf(AActor::StaticClass());
	const bool bIsComponent = bClassValid
		&& Plan.ObjectClass->IsChildOf(UActorComponent::StaticClass());
	const bool bKindValid =
		(Plan.Kind == EAvidScriptObjectFactoryKind::NewObject
			&& Plan.Registration == EAvidScriptComponentRegistrationPolicy::None
			&& !bIsActor
			&& !bIsComponent)
		|| (Plan.Kind == EAvidScriptObjectFactoryKind::ActorComponent
			&& Plan.Registration
				== EAvidScriptComponentRegistrationPolicy::RegisterInstance
			&& bIsComponent);
	if (!bClassValid
		|| !bKindValid
		|| Plan.Ownership != EAvidScriptObjectOwnershipPolicy::Session
		|| Plan.ResultObjectTypeOrdinal == INDEX_NONE)
	{
		SetAvidScriptObjectFactoryFailure(
			OutResult,
			FAvidScriptObjectHandle(),
			TEXT("binding_factory_plan_invalid"),
			TEXT("rebuild the descriptor so its immutable factory plan matches the active UE classes"));
		return false;
	}
	return true;
}
} // namespace

TConstArrayView<FAvidScriptObjectFactoryBindingSpec>
FAvidScriptObjectFactoryBinding::GetSpecs()
{
	static const TArray<FAvidScriptObjectFactoryBindingSpec> Specs = {
		MakeAvidScriptObjectFactorySpec(
			EAvidScriptBindingInvocationKind::ObjectConstruct,
			TEXT("avidscript.object_factory.v1|construct|factory_ordinal,outer_handle->packed_handle"),
			TEXT("avid_object_construct"),
			TEXT("(iii)I")),
		MakeAvidScriptObjectFactorySpec(
			EAvidScriptBindingInvocationKind::ObjectRelease,
			TEXT("avidscript.object_factory.v1|release|object_handle->i32"),
			TEXT("avid_object_release"),
			TEXT("(ii)i")),
		MakeAvidScriptObjectFactorySpec(
			EAvidScriptBindingInvocationKind::ActorFindComponent,
			TEXT("avidscript.object_factory.v1|find_component|actor_handle,object_type_ordinal->packed_handle"),
			TEXT("avid_actor_find_component"),
			TEXT("(iii)I"))
	};
	return Specs;
}

bool FAvidScriptObjectFactoryBinding::Construct(
	const FAvidScriptObjectFactoryPlan& Plan,
	FAvidScriptObjectRegistry& Registry,
	IAvidScriptObjectOwnershipDomain& Ownership,
	const FAvidScriptObjectHandle& OuterHandle,
	FAvidScriptObjectHandleResult& OutResult)
{
	if (!ValidateAvidScriptObjectFactoryPlan(Plan, OutResult))
	{
		return false;
	}

	FAvidScriptObjectHandleResult ResolveResult;
	UObject* const Outer = Registry.ResolveObject(OuterHandle, ResolveResult, false);
	if (Outer == nullptr || !Outer->IsA(Plan.RequiredOuterClass))
	{
		SetAvidScriptObjectFactoryFailure(
			OutResult,
			OuterHandle,
			TEXT("binding_factory_outer_mismatch"),
			TEXT("pass a live Outer handle compatible with the cached factory constraint"));
		return false;
	}
	if (Plan.ObjectClass->ClassWithin == nullptr
		|| !Outer->IsA(Plan.ObjectClass->ClassWithin))
	{
		SetAvidScriptObjectFactoryFailure(
			OutResult,
			OuterHandle,
			TEXT("binding_factory_outer_mismatch"),
			TEXT("pass an Outer that satisfies the active class ClassWithin contract"));
		return false;
	}

	AActor* const ComponentOwner = Plan.Kind
		== EAvidScriptObjectFactoryKind::ActorComponent
		? Cast<AActor>(Outer)
		: nullptr;
	if (Plan.Kind == EAvidScriptObjectFactoryKind::ActorComponent
		&& ComponentOwner == nullptr)
	{
		SetAvidScriptObjectFactoryFailure(
			OutResult,
			OuterHandle,
			TEXT("binding_factory_outer_mismatch"),
			TEXT("create ActorComponent factories with an Actor Outer"));
		return false;
	}

	UObject* const Object = NewObject<UObject>(Outer, Plan.ObjectClass);
	if (!IsValid(Object))
	{
		SetAvidScriptObjectFactoryFailure(
			OutResult,
			OuterHandle,
			TEXT("binding_factory_construct_failed"),
			TEXT("inspect the factory class constructor and active UE object constraints"));
		return false;
	}

	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle Handle = Registry.RegisterObject(
		Object,
		RegisterResult,
		false);
	if (!RegisterResult.bSucceeded || !Handle.IsValid())
	{
		OutResult = MoveTemp(RegisterResult);
		return false;
	}
	if (!Ownership.Adopt(Registry, *Object, Handle, Plan.Kind, OutResult))
	{
		FAvidScriptObjectHandleResult RollbackResult;
		Registry.ReleaseHandle(Handle, RollbackResult, false);
		return false;
	}

	if (Plan.Kind == EAvidScriptObjectFactoryKind::ActorComponent)
	{
		UActorComponent* const Component = CastChecked<UActorComponent>(Object);
		Component->CreationMethod = EComponentCreationMethod::Instance;
		ComponentOwner->AddInstanceComponent(Component);
		Component->RegisterComponent();
		if (!IsValid(Component) || !Component->IsRegistered())
		{
			FAvidScriptObjectHandleResult RollbackResult;
			Ownership.Release(Handle, Registry, RollbackResult);
			SetAvidScriptObjectFactoryFailure(
				OutResult,
				Handle,
				TEXT("binding_factory_registration_failed"),
				TEXT("inspect the component registration preconditions for the active Actor world"));
			return false;
		}
	}

	SetAvidScriptObjectFactorySuccess(OutResult, Handle);
	return true;
}

bool FAvidScriptObjectFactoryBinding::Release(
	FAvidScriptObjectRegistry& Registry,
	IAvidScriptObjectOwnershipDomain& Ownership,
	const FAvidScriptObjectHandle& Handle,
	FAvidScriptObjectHandleResult& OutResult)
{
	return Ownership.Release(Handle, Registry, OutResult);
}

bool FAvidScriptObjectFactoryBinding::FindComponent(
	FAvidScriptObjectRegistry& Registry,
	IAvidScriptObjectOwnershipDomain& Ownership,
	const FAvidScriptObjectHandle& ActorHandle,
	UClass& ComponentClass,
	FAvidScriptObjectHandleResult& OutResult)
{
	if (!ComponentClass.IsChildOf(UActorComponent::StaticClass()))
	{
		SetAvidScriptObjectFactoryFailure(
			OutResult,
			ActorHandle,
			TEXT("binding_component_type_mismatch"),
			TEXT("query with a cached UActorComponent-derived object type ordinal"));
		return false;
	}

	FAvidScriptObjectHandleResult ResolveResult;
	AActor* const Actor = Registry.ResolveObject<AActor>(
		ActorHandle,
		ResolveResult,
		false);
	if (Actor == nullptr)
	{
		SetAvidScriptObjectFactoryFailure(
			OutResult,
			ActorHandle,
			TEXT("binding_actor_invalid"),
			TEXT("query components on a live Actor handle from the active registry"));
		return false;
	}

	UActorComponent* const Component = Actor->FindComponentByClass(&ComponentClass);
	if (!IsValid(Component))
	{
		SetAvidScriptObjectFactorySuccess(OutResult, FAvidScriptObjectHandle());
		return true;
	}

	return Ownership.Borrow(Registry, *Component, OutResult)
		&& OutResult.bSucceeded
		&& OutResult.Handle.IsValid();
}
