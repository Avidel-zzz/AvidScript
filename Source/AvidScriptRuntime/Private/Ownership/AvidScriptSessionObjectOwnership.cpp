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
	const EOwnedObjectKind OwnedKind =
		Kind == EAvidScriptObjectFactoryKind::ActorComponent
			? EOwnedObjectKind::ActorComponent
			: EOwnedObjectKind::NewObject;
	return AdoptInternal(Registry, Object, Handle, OwnedKind, OutResult);
}

bool FAvidScriptSessionObjectOwnership::AdoptSpawnedActor(
	FAvidScriptObjectRegistry& Registry,
	AActor& Actor,
	const FAvidScriptObjectHandle& Handle,
	FAvidScriptObjectHandleResult& OutResult)
{
	return AdoptInternal(
		Registry,
		Actor,
		Handle,
		EOwnedObjectKind::SpawnedActor,
		OutResult);
}

bool FAvidScriptSessionObjectOwnership::AdoptInternal(
	FAvidScriptObjectRegistry& Registry,
	UObject& Object,
	const FAvidScriptObjectHandle& Handle,
	const EOwnedObjectKind Kind,
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
	const bool bKindMatches =
		(Kind == EOwnedObjectKind::ActorComponent
			&& Component != nullptr && IsValid(ComponentOwner))
		|| (Kind == EOwnedObjectKind::NewObject
			&& Component == nullptr && !Object.IsA<AActor>())
		|| (Kind == EOwnedObjectKind::SpawnedActor
			&& Object.IsA<AActor>());
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
	OwnedObject.StrongObject = Kind == EOwnedObjectKind::NewObject
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
	if (OwnedObject.Kind == EOwnedObjectKind::SpawnedActor)
	{
		AActor* const Actor = Cast<AActor>(OwnedObject.Object.Get());
		if (IsValid(Actor)
			&& !Actor->IsActorBeingDestroyed()
			&& !Actor->Destroy())
		{
			SetFailure(
				OutResult,
				Handle,
				Actor,
				TEXT("ownership_actor_destroy_failed"),
				TEXT("retry only while the session-owned Actor remains live"));
			return false;
		}
	}
	if (!Registry.ReleaseHandle(OwnedObject.Handle, OutResult, false))
	{
		return false;
	}

	RemoveAt(*OwnedObjectIndex);
	ResetBoundRegistryIfEmpty();
	DestroyOwnedObject(OwnedObject);
	return true;
}

bool FAvidScriptSessionObjectOwnership::Borrow(
	FAvidScriptObjectRegistry& Registry,
	UObject& Object,
	FAvidScriptObjectHandleResult& OutResult)
{
	if (!IsValid(&Object))
	{
		SetFailure(
			OutResult,
			FAvidScriptObjectHandle(),
			&Object,
			TEXT("ownership_invalid_object"),
			TEXT("borrow only a live object returned by the active session"));
		return false;
	}
	if (BoundRegistry != nullptr && BoundRegistry != &Registry)
	{
		SetFailure(
			OutResult,
			FAvidScriptObjectHandle(),
			&Object,
			TEXT("ownership_registry_mismatch"),
			TEXT("borrow through the object registry bound to the active session"));
		return false;
	}

	const TObjectKey<UObject> ObjectKey(&Object);
	if (const int32* OwnedIndex = ObjectToOwnedIndex.Find(ObjectKey))
	{
		OutResult = FAvidScriptObjectHandleResult();
		OutResult.bSucceeded = true;
		OutResult.Handle = OwnedObjects[*OwnedIndex].Handle;
		return true;
	}
	if (const int32* BorrowedIndex = ObjectToBorrowedIndex.Find(ObjectKey))
	{
		OutResult = FAvidScriptObjectHandleResult();
		OutResult.bSucceeded = true;
		OutResult.Handle = BorrowedObjects[*BorrowedIndex].Handle;
		return true;
	}

	const FAvidScriptObjectHandle Handle = Registry.AcquireBorrowedObject(
		&Object,
		OutResult,
		false);
	if (!OutResult.bSucceeded || !Handle.IsValid())
	{
		return false;
	}

	FBorrowedObject& BorrowedObject = BorrowedObjects.Emplace_GetRef();
	BorrowedObject.ObjectKey = ObjectKey;
	BorrowedObject.Handle = Handle;
	ObjectToBorrowedIndex.Add(ObjectKey, BorrowedObjects.Num() - 1);
	HandleToBorrowedIndex.Add(Handle.ToUInt64(), BorrowedObjects.Num() - 1);
	BoundRegistry = &Registry;
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

bool FAvidScriptSessionObjectOwnership::HasCapability(
	const FAvidScriptObjectHandle& Handle,
	const UObject* ExpectedObject) const
{
	if (Owns(Handle, ExpectedObject))
	{
		return true;
	}
	const int32* const BorrowedObjectIndex =
		HandleToBorrowedIndex.Find(Handle.ToUInt64());
	return BorrowedObjectIndex != nullptr
		&& BorrowedObjects.IsValidIndex(*BorrowedObjectIndex)
		&& BorrowedObjects[*BorrowedObjectIndex].Handle == Handle
		&& (ExpectedObject == nullptr
			|| BorrowedObjects[*BorrowedObjectIndex].ObjectKey
				== TObjectKey<UObject>(ExpectedObject));
}

void FAvidScriptSessionObjectOwnership::Cleanup(FAvidScriptObjectRegistry& Registry)
{
	if (BoundRegistry != nullptr && BoundRegistry != &Registry)
	{
		ensureMsgf(false, TEXT("AvidScript ownership cleanup used a registry other than the bound session registry."));
		return;
	}

	TArray<FOwnedObject> CleanupObjects = MoveTemp(OwnedObjects);
	TArray<FBorrowedObject> CleanupBorrowedObjects = MoveTemp(BorrowedObjects);
	OwnedObjects.Reset();
	ObjectToOwnedIndex.Reset();
	HandleToOwnedIndex.Reset();
	BorrowedObjects.Reset();
	ObjectToBorrowedIndex.Reset();
	HandleToBorrowedIndex.Reset();
	BoundRegistry = nullptr;

	for (int32 OwnedObjectIndex = CleanupObjects.Num() - 1; OwnedObjectIndex >= 0; --OwnedObjectIndex)
	{
		FOwnedObject& OwnedObject = CleanupObjects[OwnedObjectIndex];
		FAvidScriptObjectHandleResult ReleaseResult;
		Registry.ReleaseHandle(OwnedObject.Handle, ReleaseResult, false);
		DestroyOwnedObject(OwnedObject);
		OwnedObject.StrongObject = nullptr;
	}
	for (int32 BorrowedIndex = CleanupBorrowedObjects.Num() - 1;
		BorrowedIndex >= 0;
		--BorrowedIndex)
	{
		FAvidScriptObjectHandleResult ReleaseResult;
		Registry.ReleaseBorrowedHandle(
			CleanupBorrowedObjects[BorrowedIndex].Handle,
			ReleaseResult,
			false);
	}
}

void FAvidScriptSessionObjectOwnership::PruneInvalidBorrowedHandles(
	FAvidScriptObjectRegistry& Registry)
{
	check(IsInGameThread());
	if (!ensureMsgf(BoundRegistry == nullptr || BoundRegistry == &Registry,
		TEXT("Borrowed-handle pruning used a registry other than the bound session registry.")))
	{
		return;
	}

	// Retain journal order in one pass. The Session must not call this while a
	// candidate owns a RetainedCount checkpoint into the borrowed journal.
	int32 RetainedCount = 0;
	for (int32 Index = 0; Index < BorrowedObjects.Num(); ++Index)
	{
		const FBorrowedObject& BorrowedObject = BorrowedObjects[Index];
		if (!IsValid(BorrowedObject.ObjectKey.ResolveObjectPtr()))
		{
			FAvidScriptObjectHandleResult ReleaseResult;
			// The registry validates the generation and releases only this lease;
			// an already invalidated handle must not affect a recycled slot.
			Registry.ReleaseBorrowedHandle(BorrowedObject.Handle, ReleaseResult, false);
			ObjectToBorrowedIndex.Remove(BorrowedObject.ObjectKey);
			HandleToBorrowedIndex.Remove(BorrowedObject.Handle.ToUInt64());
			continue;
		}
		if (RetainedCount != Index)
		{
			BorrowedObjects[RetainedCount] = MoveTemp(BorrowedObjects[Index]);
			const FBorrowedObject& Retained = BorrowedObjects[RetainedCount];
			ObjectToBorrowedIndex.FindChecked(Retained.ObjectKey) = RetainedCount;
			HandleToBorrowedIndex.FindChecked(Retained.Handle.ToUInt64()) = RetainedCount;
		}
		++RetainedCount;
	}
	BorrowedObjects.SetNum(RetainedCount, EAllowShrinking::No);
	ResetBoundRegistryIfEmpty();
}

bool FAvidScriptSessionObjectOwnership::RollbackBorrowedHandles(
	FAvidScriptObjectRegistry& Registry,
	const int32 RetainedCount,
	FString& OutError)
{
	OutError.Empty();
	if (RetainedCount < 0 || RetainedCount > BorrowedObjects.Num())
	{
		OutError = TEXT("The borrowed-handle checkpoint is outside the active lease journal.");
		return false;
	}
	if (BoundRegistry != nullptr && BoundRegistry != &Registry)
	{
		OutError = TEXT("Borrowed-handle rollback used a registry other than the bound session registry.");
		return false;
	}

	bool bSucceeded = true;
	for (int32 Index = BorrowedObjects.Num() - 1; Index >= RetainedCount; --Index)
	{
		FAvidScriptObjectHandleResult ReleaseResult;
		if (!Registry.ReleaseBorrowedHandle(
				BorrowedObjects[Index].Handle,
				ReleaseResult,
				false))
		{
			bSucceeded = false;
			if (OutError.IsEmpty())
			{
				OutError = ReleaseResult.ErrorMessage;
			}
		}
		RemoveBorrowedAt(Index);
	}
	ResetBoundRegistryIfEmpty();
	return bSucceeded;
}

void FAvidScriptSessionObjectOwnership::AddReferencedObjects(FReferenceCollector& Collector)
{
	for (FOwnedObject& OwnedObject : OwnedObjects)
	{
		if (OwnedObject.Kind == EOwnedObjectKind::NewObject
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

void FAvidScriptSessionObjectOwnership::DestroyOwnedObject(
	const FOwnedObject& OwnedObject)
{
	if (OwnedObject.Kind == EOwnedObjectKind::SpawnedActor)
	{
		AActor* const Actor = Cast<AActor>(OwnedObject.Object.Get());
		if (IsValid(Actor) && !Actor->IsActorBeingDestroyed())
		{
			Actor->Destroy();
		}
		return;
	}
	if (OwnedObject.Kind != EOwnedObjectKind::ActorComponent)
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

void FAvidScriptSessionObjectOwnership::RemoveBorrowedAt(
	const int32 BorrowedObjectIndex)
{
	ObjectToBorrowedIndex.Remove(BorrowedObjects[BorrowedObjectIndex].ObjectKey);
	HandleToBorrowedIndex.Remove(
		BorrowedObjects[BorrowedObjectIndex].Handle.ToUInt64());
	BorrowedObjects.RemoveAt(BorrowedObjectIndex, 1, EAllowShrinking::No);
	for (int32 Index = BorrowedObjectIndex; Index < BorrowedObjects.Num(); ++Index)
	{
		ObjectToBorrowedIndex.FindChecked(BorrowedObjects[Index].ObjectKey) = Index;
		HandleToBorrowedIndex.FindChecked(
			BorrowedObjects[Index].Handle.ToUInt64()) = Index;
	}
}

void FAvidScriptSessionObjectOwnership::ResetBoundRegistryIfEmpty()
{
	if (OwnedObjects.IsEmpty() && BorrowedObjects.IsEmpty())
	{
		BoundRegistry = nullptr;
	}
}
