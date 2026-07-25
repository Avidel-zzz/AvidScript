#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"
#include "UObject/WeakObjectPtr.h"

struct FAvidScriptObjectHandle
{
	uint32 Slot = 0;
	uint32 Generation = 0;

	bool operator==(const FAvidScriptObjectHandle&) const = default;

	bool IsValid() const
	{
		return Slot != 0 && Generation != 0;
	}

	uint64 ToUInt64() const
	{
		return (static_cast<uint64>(Generation) << 32) | static_cast<uint64>(Slot);
	}
};

struct FAvidScriptObjectHandleResult
{
	bool bSucceeded = false;
	FAvidScriptObjectHandle Handle;
	FString ObjectPath;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptObjectRegistry
{
public:
	FAvidScriptObjectHandle RegisterObject(
		UObject* Object,
		FAvidScriptObjectHandleResult& OutResult,
		bool bIncludeObjectPath = true);
	FAvidScriptObjectHandle AcquireBorrowedObject(
		UObject* Object,
		FAvidScriptObjectHandleResult& OutResult,
		bool bIncludeObjectPath = true);
	UObject* ResolveObject(
		const FAvidScriptObjectHandle& Handle,
		FAvidScriptObjectHandleResult& OutResult,
		bool bIncludeObjectPath = true) const;

	template <typename TObject>
	TObject* ResolveObject(const FAvidScriptObjectHandle& Handle, FAvidScriptObjectHandleResult& OutResult) const
	{
		return ResolveObject<TObject>(Handle, OutResult, true);
	}

	template <typename TObject>
	TObject* ResolveObject(
		const FAvidScriptObjectHandle& Handle,
		FAvidScriptObjectHandleResult& OutResult,
		const bool bIncludeObjectPath) const
	{
		UObject* Object = ResolveObject(Handle, OutResult, bIncludeObjectPath);
		if (Object == nullptr)
		{
			return nullptr;
		}

		TObject* TypedObject = Cast<TObject>(Object);
		if (TypedObject == nullptr)
		{
			SetFailure(
				OutResult,
				Handle,
				TEXT("type_mismatch"),
				Object,
				bIncludeObjectPath,
				TEXT("Use a handle API that matches the registered UObject type."));
			return nullptr;
		}

		return TypedObject;
	}

	bool ReleaseHandle(
		const FAvidScriptObjectHandle& Handle,
		FAvidScriptObjectHandleResult& OutResult,
		bool bIncludeObjectPath = true);
	bool ReleaseBorrowedHandle(
		const FAvidScriptObjectHandle& Handle,
		FAvidScriptObjectHandleResult& OutResult,
		bool bIncludeObjectPath = true);
	void Reset();

	int32 NumSlots() const { return Slots.Num(); }
	int32 NumFreeSlots() const { return FreeSlots.Num(); }
	int32 GetLiveHandleCount() const { return LiveHandleCount; }

private:
	struct FSlot
	{
		TWeakObjectPtr<UObject> Object;
		TObjectKey<UObject> ObjectKey;
		uint32 Generation = 1;
		int32 BorrowedLeaseCount = 0;
		bool bAnchored = false;
		bool bOccupied = false;
	};

	static uint32 AdvanceGeneration(uint32 Generation);
	void ReleaseSlot(int32 SlotIndex);
	static void SetSuccess(
		FAvidScriptObjectHandleResult& OutResult,
		const FAvidScriptObjectHandle& Handle,
		const UObject* Object,
		bool bIncludeObjectPath);
	static void SetFailure(
		FAvidScriptObjectHandleResult& OutResult,
		const FAvidScriptObjectHandle& Handle,
		const TCHAR* ErrorCategory,
		const UObject* Object,
		bool bIncludeObjectPath,
		const TCHAR* NextAction);

	TArray<FSlot> Slots;
	TArray<int32> FreeSlots;
	TMap<TObjectKey<UObject>, int32> ObjectToSlot;
	int32 LiveHandleCount = 0;
	uint32 GenerationDomain = 1;
};
