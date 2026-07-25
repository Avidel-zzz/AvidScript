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
	virtual bool Release(
		UObject& Object,
		FAvidScriptObjectRegistry& Registry,
		FAvidScriptObjectHandleResult& OutResult) override;
	virtual bool Owns(const UObject& Object) const override;
	virtual void Cleanup(FAvidScriptObjectRegistry& Registry) override;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;

	int32 Num() const { return OwnedObjects.Num(); }

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

	static void SetFailure(
		FAvidScriptObjectHandleResult& OutResult,
		const FAvidScriptObjectHandle& Handle,
		const UObject* Object,
		const TCHAR* ErrorCategory,
		const TCHAR* NextAction);
	static void DestroyOwnedComponent(const FOwnedObject& OwnedObject);
	void RemoveAt(int32 OwnedObjectIndex);

	TArray<FOwnedObject> OwnedObjects;
	TMap<TObjectKey<UObject>, int32> ObjectToOwnedIndex;
	FAvidScriptObjectRegistry* BoundRegistry = nullptr;
};
