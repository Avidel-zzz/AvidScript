#include "AvidScriptArrayValueHeap.h"

#include "AvidScriptValueCapability.h"

namespace
{
bool IsValidArrayLayout(
	const int32 ElementCount,
	const int32 ElementStride,
	const int32 ElementAlignment,
	const int32 ByteCount)
{
	return ElementCount >= 0
		&& ElementCount <= FAvidScriptArrayValueHeap::MaxElements
		&& ElementStride > 0
		&& ElementStride <= static_cast<int32>(FAvidScriptArrayValueHeap::MaxValueBytes)
		&& FMath::IsPowerOfTwo(ElementAlignment)
		&& ElementAlignment <= 16
		&& ByteCount >= 0
		&& static_cast<uint32>(ByteCount) <= FAvidScriptArrayValueHeap::MaxValueBytes
		&& static_cast<int64>(ElementCount) * ElementStride == ByteCount;
}

bool IsValidArrayRange(
	const int32 ElementIndex,
	const int32 ElementCount,
	const int32 AvailableElements,
	const int32 ElementStride,
	const int32 ByteCount)
{
	const int64 EndIndex = static_cast<int64>(ElementIndex) + ElementCount;
	const int64 RequiredBytes = static_cast<int64>(ElementCount) * ElementStride;
	return ElementIndex >= 0
		&& ElementCount >= 0
		&& EndIndex <= AvailableElements
		&& RequiredBytes >= 0
		&& RequiredBytes <= FAvidScriptArrayValueHeap::MaxValueBytes
		&& RequiredBytes == ByteCount;
}
}

bool FAvidScriptArrayValueHeap::Reserve(
	FAvidScriptArrayValueReservation& OutReservation,
	FString& OutError)
{
	OutReservation = FAvidScriptArrayValueReservation();
	OutError.Reset();

	int32 SlotIndex = INDEX_NONE;
	if (!FreeSlots.IsEmpty())
	{
		SlotIndex = FreeSlots.Last();
		if (!Slots.IsValidIndex(SlotIndex)
			|| Slots[SlotIndex].bReserved
			|| Slots[SlotIndex].bOccupied
			|| Slots[SlotIndex].Token != 0)
		{
			OutError = TEXT("array_value_heap_corrupt: a free array value slot is not reusable.");
			return false;
		}
	}
	else
	{
		if (Slots.Num() >= MaxSlots)
		{
			OutError = TEXT("array_value_heap_exhausted: the session has no free array value slots.");
			return false;
		}
		SlotIndex = Slots.Num();
	}

	const uint32 Token = FAvidScriptValueCapability::AllocateToken();
	if (Token == 0)
	{
		OutError = TEXT("value_token_space_exhausted: the process has exhausted value capability tokens.");
		return false;
	}
	if (!FreeSlots.IsEmpty())
	{
		FreeSlots.Pop(EAllowShrinking::No);
	}
	else
	{
		Slots.AddDefaulted();
	}
	FSlot& Slot = Slots[SlotIndex];
	Slot.Token = Token;
	Slot.bReserved = true;
	TokenToSlots.Add(Token, SlotIndex);
	++ReservedValueCount;
	OutReservation.Token = Token;
	OutReservation.bActive = true;
	return true;
}

bool FAvidScriptArrayValueHeap::PublishReserved(
	FAvidScriptArrayValueReservation& Reservation,
	const FString& TypeId,
	const int32 ElementCount,
	const int32 ElementStride,
	const int32 ElementAlignment,
	const TConstArrayView<uint8> Bytes,
	uint32& OutToken,
	FString& OutError)
{
	OutToken = 0;
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (!Reservation.bActive
		|| TypeId.IsEmpty()
		|| !ResolveSlot(Reservation.Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bReserved
		|| Slot->bOccupied)
	{
		OutError = TEXT("array_value_reservation_stale: the array value reservation is invalid or stale.");
		return false;
	}
	if (!IsValidArrayLayout(
			ElementCount,
			ElementStride,
			ElementAlignment,
			Bytes.Num()))
	{
		OutError = TEXT("array_value_layout_invalid: the array value exceeds its bounded canonical layout.");
		return false;
	}

	Slot->TypeId = TypeId;
	Slot->Bytes.Reset(Bytes.Num());
	if (!Bytes.IsEmpty())
	{
		Slot->Bytes.Append(Bytes.GetData(), Bytes.Num());
	}
	Slot->ElementCount = ElementCount;
	Slot->ElementStride = ElementStride;
	Slot->ElementAlignment = ElementAlignment;
	Slot->bReserved = false;
	Slot->bOccupied = true;
	--ReservedValueCount;
	++LiveValueCount;
	++PublishedValueCount;
	LiveByteCount += static_cast<uint64>(Bytes.Num());
	PeakLiveByteCount = FMath::Max(PeakLiveByteCount, LiveByteCount);
	OutToken = Reservation.Token;
	Reservation = FAvidScriptArrayValueReservation();
	return true;
}

bool FAvidScriptArrayValueHeap::Resolve(
	const uint32 Token,
	const FString& ExpectedTypeId,
	FAvidScriptArrayValueView& OutValue,
	FString& OutError) const
{
	OutValue = FAvidScriptArrayValueView();
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	const FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved)
	{
		OutError = TEXT("array_value_token_stale: the array value token is invalid or stale.");
		return false;
	}
	if (!ExpectedTypeId.IsEmpty() && Slot->TypeId != ExpectedTypeId)
	{
		OutError = TEXT("array_value_type_mismatch: the array capability type does not match the prepared descriptor.");
		return false;
	}
	OutValue.TypeId = Slot->TypeId;
	OutValue.Bytes = MakeArrayView(Slot->Bytes);
	OutValue.ElementCount = Slot->ElementCount;
	OutValue.ElementStride = Slot->ElementStride;
	OutValue.ElementAlignment = Slot->ElementAlignment;
	return true;
}

bool FAvidScriptArrayValueHeap::ReadElement(
	const uint32 Token,
	const int32 ElementIndex,
	const TArrayView<uint8> OutBytes,
	FString& OutError) const
{
	FAvidScriptArrayValueView Value;
	if (!Resolve(Token, FString(), Value, OutError)
		|| ElementIndex < 0
		|| ElementIndex >= Value.ElementCount
		|| OutBytes.Num() != Value.ElementStride)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("array_value_element_invalid: the requested array element or byte width is invalid.");
		}
		return false;
	}
	FMemory::Memcpy(
		OutBytes.GetData(),
		Value.Bytes.GetData() + ElementIndex * Value.ElementStride,
		OutBytes.Num());
	return true;
}

bool FAvidScriptArrayValueHeap::ReadRange(
	const uint32 Token,
	const int32 ElementIndex,
	const int32 ElementCount,
	const TArrayView<uint8> OutBytes,
	FString& OutError) const
{
	FAvidScriptArrayValueView Value;
	if (!Resolve(Token, FString(), Value, OutError)
		|| !IsValidArrayRange(
			ElementIndex,
			ElementCount,
			Value.ElementCount,
			Value.ElementStride,
			OutBytes.Num()))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("array_value_range_invalid: the requested array range or byte width is invalid.");
		}
		return false;
	}
	if (!OutBytes.IsEmpty())
	{
		FMemory::Memcpy(
			OutBytes.GetData(),
			Value.Bytes.GetData() + ElementIndex * Value.ElementStride,
			OutBytes.Num());
	}
	return true;
}

bool FAvidScriptArrayValueHeap::WriteElement(
	const uint32 Token,
	const int32 ElementIndex,
	const TConstArrayView<uint8> Bytes,
	FString& OutError)
{
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved
		|| ElementIndex < 0
		|| ElementIndex >= Slot->ElementCount
		|| Bytes.Num() != Slot->ElementStride)
	{
		OutError = TEXT("array_value_element_invalid: the requested array element or byte width is invalid.");
		return false;
	}
	FMemory::Memcpy(
		Slot->Bytes.GetData() + ElementIndex * Slot->ElementStride,
		Bytes.GetData(),
		Bytes.Num());
	return true;
}

bool FAvidScriptArrayValueHeap::WriteRange(
	const uint32 Token,
	const int32 ElementIndex,
	const int32 ElementCount,
	const TConstArrayView<uint8> Bytes,
	FString& OutError)
{
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved
		|| !IsValidArrayRange(
			ElementIndex,
			ElementCount,
			Slot->ElementCount,
			Slot->ElementStride,
			Bytes.Num()))
	{
		OutError = TEXT("array_value_range_invalid: the requested array range or byte width is invalid.");
		return false;
	}
	if (!Bytes.IsEmpty())
	{
		FMemory::Memcpy(
			Slot->Bytes.GetData() + ElementIndex * Slot->ElementStride,
			Bytes.GetData(),
			Bytes.Num());
	}
	return true;
}

bool FAvidScriptArrayValueHeap::ReleaseValue(
	const uint32 Token,
	FString& OutError)
{
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved)
	{
		OutError = TEXT("array_value_token_stale: the array value token is invalid or stale.");
		return false;
	}
	ReleaseSlot(SlotIndex, true);
	return true;
}

void FAvidScriptArrayValueHeap::ReleaseReservation(
	FAvidScriptArrayValueReservation& Reservation)
{
	if (!Reservation.bActive)
	{
		return;
	}
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (ResolveSlot(Reservation.Token, SlotIndex, Slot)
		&& Slot != nullptr
		&& Slot->bReserved
		&& !Slot->bOccupied)
	{
		ReleaseSlot(SlotIndex, false);
	}
	Reservation = FAvidScriptArrayValueReservation();
}

void FAvidScriptArrayValueHeap::RemoveCreatedValue(const uint32 Token)
{
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (ResolveSlot(Token, SlotIndex, Slot)
		&& Slot != nullptr
		&& Slot->bOccupied
		&& !Slot->bReserved)
	{
		ReleaseSlot(SlotIndex, false);
	}
}

void FAvidScriptArrayValueHeap::Reset()
{
	ReleasedValueCount += static_cast<uint64>(LiveValueCount);
	Slots.Reset();
	FreeSlots.Reset();
	TokenToSlots.Reset();
	LiveValueCount = 0;
	ReservedValueCount = 0;
	LiveByteCount = 0;
}

FAvidScriptArrayValueHeapStats FAvidScriptArrayValueHeap::GetStats() const
{
	return {
		LiveValueCount,
		ReservedValueCount,
		LiveByteCount,
		PeakLiveByteCount,
		PublishedValueCount,
		ReleasedValueCount
	};
}

bool FAvidScriptArrayValueHeap::ResolveSlot(
	const uint32 Token,
	int32& OutSlotIndex,
	FSlot*& OutSlot)
{
	const FSlot* ConstSlot = nullptr;
	const bool bResolved = static_cast<const FAvidScriptArrayValueHeap*>(this)
		->ResolveSlot(Token, OutSlotIndex, ConstSlot);
	OutSlot = bResolved ? &Slots[OutSlotIndex] : nullptr;
	return bResolved;
}

bool FAvidScriptArrayValueHeap::ResolveSlot(
	const uint32 Token,
	int32& OutSlotIndex,
	const FSlot*& OutSlot) const
{
	OutSlotIndex = INDEX_NONE;
	OutSlot = nullptr;
	if (!FAvidScriptValueCapability::IsToken(Token))
	{
		return false;
	}
	const int32* SlotIndex = TokenToSlots.Find(Token);
	if (SlotIndex == nullptr
		|| !Slots.IsValidIndex(*SlotIndex)
		|| Slots[*SlotIndex].Token != Token)
	{
		return false;
	}
	OutSlotIndex = *SlotIndex;
	OutSlot = &Slots[OutSlotIndex];
	return true;
}

void FAvidScriptArrayValueHeap::ReleaseSlot(
	const int32 SlotIndex,
	const bool bCountRelease)
{
	if (!Slots.IsValidIndex(SlotIndex))
	{
		return;
	}
	FSlot& Slot = Slots[SlotIndex];
	if (Slot.bReserved)
	{
		--ReservedValueCount;
	}
	if (Slot.bOccupied)
	{
		--LiveValueCount;
		LiveByteCount -= static_cast<uint64>(Slot.Bytes.Num());
		if (bCountRelease)
		{
			++ReleasedValueCount;
		}
	}
	Slot.TypeId.Reset();
	Slot.Bytes.Reset();
	Slot.ElementCount = 0;
	Slot.ElementStride = 0;
	Slot.ElementAlignment = 1;
	Slot.bReserved = false;
	Slot.bOccupied = false;
	TokenToSlots.Remove(Slot.Token);
	Slot.Token = 0;
	FreeSlots.Add(SlotIndex);
}
