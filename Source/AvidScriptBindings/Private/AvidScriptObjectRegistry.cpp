#include "AvidScriptObjectRegistry.h"

#include "UObject/UObjectGlobals.h"

FAvidScriptObjectHandle FAvidScriptObjectRegistry::RegisterObject(
	UObject* Object,
	FAvidScriptObjectHandleResult& OutResult,
	const bool bIncludeObjectPath)
{
	if (!IsValid(Object))
	{
		const FAvidScriptObjectHandle InvalidHandle;
		SetFailure(
			OutResult,
			InvalidHandle,
			TEXT("invalid_object"),
			Object,
			bIncludeObjectPath,
			TEXT("Register only live UObject instances owned by the current world or package."));
		return InvalidHandle;
	}

	const TObjectKey<UObject> ObjectKey(Object);
	if (const int32* ExistingSlotIndex = ObjectToSlot.Find(ObjectKey))
	{
		const FSlot& ExistingSlot = Slots[*ExistingSlotIndex];
		if (ExistingSlot.bOccupied && ExistingSlot.Object.Get() == Object)
		{
			const FAvidScriptObjectHandle ExistingHandle{
				static_cast<uint32>(*ExistingSlotIndex + 1),
				ExistingSlot.Generation };
			SetSuccess(OutResult, ExistingHandle, Object, bIncludeObjectPath);
			return ExistingHandle;
		}

		ObjectToSlot.Remove(ObjectKey);
	}

	int32 SlotIndex = INDEX_NONE;
	if (FreeSlots.Num() > 0)
	{
		SlotIndex = FreeSlots.Pop(EAllowShrinking::No);
	}
	else
	{
		SlotIndex = Slots.Emplace();
		Slots[SlotIndex].Generation = GenerationDomain;
	}

	FSlot& Slot = Slots[SlotIndex];
	Slot.Object = Object;
	Slot.ObjectKey = ObjectKey;
	Slot.bOccupied = true;
	ObjectToSlot.Add(ObjectKey, SlotIndex);
	++LiveHandleCount;

	const FAvidScriptObjectHandle Handle{ static_cast<uint32>(SlotIndex + 1), Slot.Generation };
	SetSuccess(OutResult, Handle, Object, bIncludeObjectPath);
	return Handle;
}

UObject* FAvidScriptObjectRegistry::ResolveObject(
	const FAvidScriptObjectHandle& Handle,
	FAvidScriptObjectHandleResult& OutResult,
	const bool bIncludeObjectPath) const
{
	if (!Handle.IsValid())
	{
		SetFailure(
			OutResult,
			Handle,
			TEXT("invalid_handle"),
			nullptr,
			bIncludeObjectPath,
			TEXT("Use a handle returned by the active AvidScript object registry."));
		return nullptr;
	}

	const int32 SlotIndex = static_cast<int32>(Handle.Slot - 1);
	if (!Slots.IsValidIndex(SlotIndex))
	{
		SetFailure(
			OutResult,
			Handle,
			TEXT("invalid_handle"),
			nullptr,
			bIncludeObjectPath,
			TEXT("Discard handles from previous worlds or registry resets."));
		return nullptr;
	}

	const FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Handle.Generation)
	{
		SetFailure(
			OutResult,
			Handle,
			TEXT("generation_mismatch"),
			Slot.Object.Get(),
			bIncludeObjectPath,
			TEXT("Discard stale handles and request a fresh handle from the host."));
		return nullptr;
	}

	if (!Slot.bOccupied)
	{
		SetFailure(
			OutResult,
			Handle,
			TEXT("stale_handle"),
			nullptr,
			bIncludeObjectPath,
			TEXT("Discard released handles and request a fresh handle from the host."));
		return nullptr;
	}

	UObject* Object = Slot.Object.Get();
	if (!IsValid(Object))
	{
		SetFailure(
			OutResult,
			Handle,
			TEXT("invalid_object"),
			Object,
			bIncludeObjectPath,
			TEXT("Stop using handles whose UObject has been destroyed or garbage collected."));
		return nullptr;
	}

	SetSuccess(OutResult, Handle, Object, bIncludeObjectPath);
	return Object;
}

bool FAvidScriptObjectRegistry::ReleaseHandle(
	const FAvidScriptObjectHandle& Handle,
	FAvidScriptObjectHandleResult& OutResult,
	const bool bIncludeObjectPath)
{
	if (!Handle.IsValid())
	{
		SetFailure(
			OutResult,
			Handle,
			TEXT("invalid_handle"),
			nullptr,
			bIncludeObjectPath,
			TEXT("Release only handles returned by the active AvidScript object registry."));
		return false;
	}

	const int32 SlotIndex = static_cast<int32>(Handle.Slot - 1);
	if (!Slots.IsValidIndex(SlotIndex))
	{
		SetFailure(
			OutResult,
			Handle,
			TEXT("invalid_handle"),
			nullptr,
			bIncludeObjectPath,
			TEXT("Ignore release requests for handles outside the active registry."));
		return false;
	}

	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Handle.Generation)
	{
		SetFailure(
			OutResult,
			Handle,
			TEXT("generation_mismatch"),
			Slot.Object.Get(),
			bIncludeObjectPath,
			TEXT("Ignore release requests for stale handle generations."));
		return false;
	}

	if (!Slot.bOccupied)
	{
		SetFailure(
			OutResult,
			Handle,
			TEXT("stale_handle"),
			nullptr,
			bIncludeObjectPath,
			TEXT("Ignore duplicate release requests."));
		return false;
	}

	SetSuccess(OutResult, Handle, Slot.Object.Get(), bIncludeObjectPath);
	ObjectToSlot.Remove(Slot.ObjectKey);
	Slot.Object.Reset();
	Slot.ObjectKey = TObjectKey<UObject>();
	Slot.bOccupied = false;
	Slot.Generation = AdvanceGeneration(Slot.Generation);
	FreeSlots.Add(SlotIndex);
	check(LiveHandleCount > 0);
	--LiveHandleCount;
	return true;
}

void FAvidScriptObjectRegistry::Reset()
{
	Slots.Reset();
	FreeSlots.Reset();
	ObjectToSlot.Reset();
	LiveHandleCount = 0;
	GenerationDomain = AdvanceGeneration(GenerationDomain);
}

uint32 FAvidScriptObjectRegistry::AdvanceGeneration(uint32 Generation)
{
	const uint32 NextGeneration = Generation + 1;
	return NextGeneration != 0 ? NextGeneration : 1;
}

void FAvidScriptObjectRegistry::SetSuccess(
	FAvidScriptObjectHandleResult& OutResult,
	const FAvidScriptObjectHandle& Handle,
	const UObject* Object,
	const bool bIncludeObjectPath)
{
	OutResult = FAvidScriptObjectHandleResult();
	OutResult.bSucceeded = true;
	OutResult.Handle = Handle;
	if (bIncludeObjectPath && Object != nullptr)
	{
		OutResult.ObjectPath = Object->GetPathName();
	}
}

void FAvidScriptObjectRegistry::SetFailure(
	FAvidScriptObjectHandleResult& OutResult,
	const FAvidScriptObjectHandle& Handle,
	const TCHAR* ErrorCategory,
	const UObject* Object,
	const bool bIncludeObjectPath,
	const TCHAR* NextAction)
{
	OutResult = FAvidScriptObjectHandleResult();
	OutResult.Handle = Handle;
	if (bIncludeObjectPath && Object != nullptr)
	{
		OutResult.ObjectPath = Object->GetPathName();
	}
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript object handle error | category=%s | slot=%u | generation=%u | object=%s | next=%s"),
		ErrorCategory,
		Handle.Slot,
		Handle.Generation,
		OutResult.ObjectPath.IsEmpty() ? TEXT("<none>") : *OutResult.ObjectPath,
		NextAction);
}
