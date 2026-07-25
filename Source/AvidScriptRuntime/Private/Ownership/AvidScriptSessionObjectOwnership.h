#pragma once

#include "AvidScriptObjectFactoryPolicy.h"
#include "AvidScriptObjectOwnership.h"
#include "AvidScriptObjectRegistry.h"

#include "UObject/GCObject.h"
#include "UObject/ObjectKey.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

class AActor;

class FAvidScriptSessionObjectOwnership final
	: public FGCObject
	, public IAvidScriptObjectOwnershipDomain
{
public:
	virtual bool Adopt(
		FAvidScriptObjectRegistry& Registry,
		UObject& Object,
		const FAvidScriptObjectHandle& Handle,
		EAvidScriptObjectFactoryKind Kind,
		FAvidScriptObjectHandleResult& OutResult) override;
	virtual bool Borrow(
		FAvidScriptObjectRegistry& Registry,
		UObject& Object,
		FAvidScriptObjectHandleResult& OutResult) override;
	virtual bool Release(
		const FAvidScriptObjectHandle& Handle,
		FAvidScriptObjectRegistry& Registry,
		FAvidScriptObjectHandleResult& OutResult) override;
	virtual bool Owns(
		const FAvidScriptObjectHandle& Handle,
		const UObject* ExpectedObject = nullptr) const override;
	virtual void Cleanup(FAvidScriptObjectRegistry& Registry) override;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;

	int32 Num() const { return OwnedObjects.Num(); }
	int32 GetBorrowedHandleCount() const { return BorrowedObjects.Num(); }
	bool RollbackBorrowedHandles(
		FAvidScriptObjectRegistry& Registry,
		int32 RetainedCount,
		FString& OutError);

private:
	struct FOwnedObject
	{
		TObjectKey<UObject> ObjectKey;
		TWeakObjectPtr<UObject> Object;
		TWeakObjectPtr<AActor> ComponentOwner;
		TObjectPtr<UObject> StrongObject;
		FAvidScriptObjectHandle Handle;
		EAvidScriptObjectFactoryKind Kind = EAvidScriptObjectFactoryKind::NewObject;
	};
	struct FBorrowedObject
	{
		TObjectKey<UObject> ObjectKey;
		FAvidScriptObjectHandle Handle;
	};

	static void SetFailure(
		FAvidScriptObjectHandleResult& OutResult,
		const FAvidScriptObjectHandle& Handle,
		const UObject* Object,
		const TCHAR* ErrorCategory,
		const TCHAR* NextAction);
	static void DestroyOwnedComponent(const FOwnedObject& OwnedObject);
	void RemoveAt(int32 OwnedObjectIndex);
	void RemoveBorrowedAt(int32 BorrowedObjectIndex);
	void ResetBoundRegistryIfEmpty();

	TArray<FOwnedObject> OwnedObjects;
	TMap<TObjectKey<UObject>, int32> ObjectToOwnedIndex;
	TMap<uint64, int32> HandleToOwnedIndex;
	TArray<FBorrowedObject> BorrowedObjects;
	TMap<TObjectKey<UObject>, int32> ObjectToBorrowedIndex;
	TMap<uint64, int32> HandleToBorrowedIndex;
	FAvidScriptObjectRegistry* BoundRegistry = nullptr;
};
