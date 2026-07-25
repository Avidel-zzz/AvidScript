#include "Ownership/AvidScriptSessionObjectOwnership.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"

bool FAvidScriptSessionObjectOwnership::Adopt(
	FAvidScriptObjectRegistry& Registry,
	UObject& Object,
	const FAvidScriptObjectHandle& Handle,
	const EAvidScriptObjectFactoryKind Kind,
	FAvidScriptObjectHandleResult& OutResult)
{
	if (!IsValid(&Object))
	{
		SetFailure(
			OutResult,
			Handle,
			&Object,
			TEXT("ownership_invalid_object"),
			TEXT("adopt only a live object created by the active session"));
		return false;
	}
	if (!Handle.IsValid())
	{
		SetFailure(
			OutResult,
			Handle,
			&Object,
			TEXT("ownership_invalid_handle"),
			TEXT("register the constructed object before transferring session ownership"));
		return false;
	}
	if (BoundRegistry != nullptr && BoundRegistry != &Registry)
	{
		SetFailure(
			OutResult,
			Handle,
			&Object,
			TEXT("ownership_registry_mismatch"),
			TEXT("use the object registry bound to the active session ownership domain"));
		return false;
	}

	FAvidScriptObjectHandleResult ResolveResult;
	if (Registry.ResolveObject(Handle, ResolveResult, false) != &Object)
	{
		SetFailure(
			OutResult,
			Handle,
			&Object,
			TEXT("ownership_handle_mismatch"),
			TEXT("adopt only when the supplied handle resolves to the same constructed object"));
		return false;
	}

	UActorComponent* const Component = Cast<UActorComponent>(&Object);
	AActor* const ComponentOwner = Component != nullptr ? Component->GetOwner() : nullptr;
	const bool bKindMatches = Kind == EAvidScriptObjectFactoryKind::ActorComponent
		? Component != nullptr && IsValid(ComponentOwner)
		: Component == nullptr && !Object.IsA<AActor>();
	if (!bKindMatches)
	{
		SetFailure(
			OutResult,
			Handle,
			&Object,
			TEXT("ownership_kind_mismatch"),
			TEXT("adopt the object with the factory kind used to construct it"));
		return false;
	}

	const TObjectKey<UObject> ObjectKey(&Object);
	if (ObjectToOwnedIndex.Contains(ObjectKey)
		|| HandleToOwnedIndex.Contains(Handle.ToUInt64()))
	{
		SetFailure(
			OutResult,
			Handle,
			&Object,
			TEXT("ownership_conflict"),
			TEXT("release the existing session-owned object before adopting it again"));
		return false;
	}

	FOwnedObject& OwnedObject = OwnedObjects.Emplace_GetRef();
	OwnedObject.ObjectKey = ObjectKey;
	OwnedObject.Object = &Object;
	OwnedObject.ComponentOwner = ComponentOwner;
	OwnedObject.StrongObject = Kind == EAvidScriptObjectFactoryKind::NewObject
		? &Object
		: nullptr;
	OwnedObject.Handle = Handle;
	OwnedObject.Kind = Kind;
	ObjectToOwnedIndex.Add(ObjectKey, OwnedObjects.Num() - 1);
	HandleToOwnedIndex.Add(Handle.ToUInt64(), OwnedObjects.Num() - 1);
	BoundRegistry = &Registry;

	OutResult = FAvidScriptObjectHandleResult();
	OutResult.bSucceeded = true;
	OutResult.Handle = Handle;
	OutResult.ObjectPath = Object.GetPathName();
	return true;
}

bool FAvidScriptSessionObjectOwnership::Release(
	const FAvidScriptObjectHandle& Handle,
	FAvidScriptObjectRegistry& Registry,
	FAvidScriptObjectHandleResult& OutResult)
{
	if (BoundRegistry != nullptr && BoundRegistry != &Registry)
	{
		SetFailure(
			OutResult,
			Handle,
			nullptr,
			TEXT("ownership_registry_mismatch"),
			TEXT("release through the object registry bound to the active session ownership domain"));
		return false;
	}

	const int32* const OwnedObjectIndex = HandleToOwnedIndex.Find(Handle.ToUInt64());
	if (OwnedObjectIndex == nullptr
		|| !OwnedObjects.IsValidIndex(*OwnedObjectIndex)
		|| OwnedObjects[*OwnedObjectIndex].Handle != Handle)
	{
		SetFailure(
			OutResult,
			Handle,
			nullptr,
			TEXT("ownership_violation"),
			TEXT("release only objects constructed and owned by the active session"));
		return false;
	}

	const FOwnedObject OwnedObject = OwnedObjects[*OwnedObjectIndex];
	if (!Registry.ReleaseHandle(OwnedObject.Handle, OutResult))
	{
		return false;
	}

	RemoveAt(*OwnedObjectIndex);
	if (OwnedObjects.IsEmpty())
	{
		BoundRegistry = nullptr;
	}
	DestroyOwnedComponent(OwnedObject);
	return true;
}

bool FAvidScriptSessionObjectOwnership::Owns(
	const FAvidScriptObjectHandle& Handle,
	const UObject* ExpectedObject) const
{
	const int32* const OwnedObjectIndex = HandleToOwnedIndex.Find(Handle.ToUInt64());
	return OwnedObjectIndex != nullptr
		&& OwnedObjects.IsValidIndex(*OwnedObjectIndex)
		&& OwnedObjects[*OwnedObjectIndex].Handle == Handle
		&& (ExpectedObject == nullptr
			|| OwnedObjects[*OwnedObjectIndex].ObjectKey == TObjectKey<UObject>(ExpectedObject));
}

void FAvidScriptSessionObjectOwnership::Cleanup(FAvidScriptObjectRegistry& Registry)
{
	if (BoundRegistry != nullptr && BoundRegistry != &Registry)
	{
		ensureMsgf(false, TEXT("AvidScript ownership cleanup used a registry other than the bound session registry."));
		return;
	}

	TArray<FOwnedObject> CleanupObjects = MoveTemp(OwnedObjects);
	OwnedObjects.Reset();
	ObjectToOwnedIndex.Reset();
	HandleToOwnedIndex.Reset();
	BoundRegistry = nullptr;

	for (int32 OwnedObjectIndex = CleanupObjects.Num() - 1; OwnedObjectIndex >= 0; --OwnedObjectIndex)
	{
		FOwnedObject& OwnedObject = CleanupObjects[OwnedObjectIndex];
		FAvidScriptObjectHandleResult ReleaseResult;
		Registry.ReleaseHandle(OwnedObject.Handle, ReleaseResult, false);
		DestroyOwnedComponent(OwnedObject);
		OwnedObject.StrongObject = nullptr;
	}
}

void FAvidScriptSessionObjectOwnership::AddReferencedObjects(FReferenceCollector& Collector)
{
	for (FOwnedObject& OwnedObject : OwnedObjects)
	{
		if (OwnedObject.Kind == EAvidScriptObjectFactoryKind::NewObject
			&& OwnedObject.StrongObject != nullptr)
		{
			Collector.AddReferencedObject(OwnedObject.StrongObject);
		}
	}
}

FString FAvidScriptSessionObjectOwnership::GetReferencerName() const
{
	return TEXT("FAvidScriptSessionObjectOwnership");
}

void FAvidScriptSessionObjectOwnership::SetFailure(
	FAvidScriptObjectHandleResult& OutResult,
	const FAvidScriptObjectHandle& Handle,
	const UObject* Object,
	const TCHAR* ErrorCategory,
	const TCHAR* NextAction)
{
	OutResult = FAvidScriptObjectHandleResult();
	OutResult.Handle = Handle;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.NextAction = NextAction;
	if (Object != nullptr)
	{
		OutResult.ObjectPath = Object->GetPathName();
	}
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript object ownership error | category=%s | slot=%u | generation=%u | object=%s | next=%s"),
		ErrorCategory,
		Handle.Slot,
		Handle.Generation,
		OutResult.ObjectPath.IsEmpty() ? TEXT("<none>") : *OutResult.ObjectPath,
		NextAction);
}

void FAvidScriptSessionObjectOwnership::DestroyOwnedComponent(const FOwnedObject& OwnedObject)
{
	if (OwnedObject.Kind != EAvidScriptObjectFactoryKind::ActorComponent)
	{
		return;
	}

	UActorComponent* const Component = Cast<UActorComponent>(OwnedObject.Object.Get());
	if (IsValid(Component) && !Component->IsBeingDestroyed())
	{
		Component->DestroyComponent();
	}
}

void FAvidScriptSessionObjectOwnership::RemoveAt(const int32 OwnedObjectIndex)
{
	ObjectToOwnedIndex.Remove(OwnedObjects[OwnedObjectIndex].ObjectKey);
	HandleToOwnedIndex.Remove(OwnedObjects[OwnedObjectIndex].Handle.ToUInt64());
	OwnedObjects.RemoveAt(OwnedObjectIndex, 1, EAllowShrinking::No);
	for (int32 Index = OwnedObjectIndex; Index < OwnedObjects.Num(); ++Index)
	{
		ObjectToOwnedIndex.FindChecked(OwnedObjects[Index].ObjectKey) = Index;
		HandleToOwnedIndex.FindChecked(OwnedObjects[Index].Handle.ToUInt64()) = Index;
	}
}
