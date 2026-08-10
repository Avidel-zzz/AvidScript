#include "AvidScriptUtf8ValueHeap.h"

#include "AvidScriptValueCapability.h"

#include "Containers/StringConv.h"
#include "Misc/Crc.h"

bool FAvidScriptUtf8ValueHeap::IsHeapToken(const uint32 ValueReference)
{
	return FAvidScriptValueCapability::IsToken(ValueReference);
}

bool FAvidScriptUtf8ValueHeap::Reserve(
	FAvidScriptUtf8ValueReservation& OutReservation,
	FString& OutError)
{
	OutReservation = FAvidScriptUtf8ValueReservation();
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
			OutError = TEXT("utf8_value_heap_corrupt: a free UTF-8 value slot is not reusable.");
			return false;
		}
	}
	else
	{
		if (Slots.Num() >= MaxSlots)
		{
			OutError = TEXT("utf8_value_heap_exhausted: the session has no free UTF-8 value slots.");
			return false;
		}
		SlotIndex = Slots.Num();
	}

	const uint32 Token = FAvidScriptValueCapability::AllocateToken();
	if (Token == 0)
	{
		OutError = TEXT("utf8_value_token_space_exhausted: the process has exhausted UTF-8 capability tokens.");
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

bool FAvidScriptUtf8ValueHeap::InternReserved(
	FAvidScriptUtf8ValueReservation& Reservation,
	const TConstArrayView<uint8> Utf8Bytes,
	uint32& OutToken,
	bool& bOutCreated,
	FString& OutError)
{
	OutToken = 0;
	bOutCreated = false;
	OutError.Reset();
	if (!Reservation.bActive)
	{
		OutError = TEXT("utf8_value_reservation_invalid: the UTF-8 value reservation is inactive.");
		return false;
	}

	int32 ReservedSlotIndex = INDEX_NONE;
	FSlot* ReservedSlot = nullptr;
	if (!ResolveSlot(Reservation.Token, ReservedSlotIndex, ReservedSlot)
		|| ReservedSlot == nullptr || !ReservedSlot->bReserved
		|| ReservedSlot->bOccupied)
	{
		OutError = TEXT("utf8_value_reservation_stale: the UTF-8 value reservation is stale.");
		return false;
	}
	if (Utf8Bytes.Num() < 0
		|| static_cast<uint32>(Utf8Bytes.Num()) > MaxValueBytes)
	{
		OutError = TEXT("utf8_value_too_large: the UTF-8 value exceeds the 1 MiB session limit.");
		return false;
	}
	if (!IsCanonicalUtf8(Utf8Bytes, OutError))
	{
		return false;
	}

	const uint32 Hash = FCrc::MemCrc32(
		Utf8Bytes.GetData(),
		Utf8Bytes.Num());
	if (const TArray<int32>* Candidates = HashToSlots.Find(Hash))
	{
		for (const int32 CandidateIndex : *Candidates)
		{
			if (!Slots.IsValidIndex(CandidateIndex))
			{
				continue;
			}
			const FSlot& Candidate = Slots[CandidateIndex];
			if (Candidate.bOccupied
				&& Candidate.Bytes.Num() == Utf8Bytes.Num()
				&& (Utf8Bytes.IsEmpty()
					|| FMemory::Memcmp(
						Candidate.Bytes.GetData(),
						Utf8Bytes.GetData(),
						Utf8Bytes.Num()) == 0))
			{
				OutToken = Candidate.Token;
				ReleaseSlot(ReservedSlotIndex);
				Reservation = FAvidScriptUtf8ValueReservation();
				return true;
			}
		}
	}

	ReservedSlot->Bytes.Reset(Utf8Bytes.Num());
	if (!Utf8Bytes.IsEmpty())
	{
		ReservedSlot->Bytes.Append(Utf8Bytes.GetData(), Utf8Bytes.Num());
	}
	ReservedSlot->Hash = Hash;
	ReservedSlot->bReserved = false;
	ReservedSlot->bOccupied = true;
	--ReservedValueCount;
	++LiveValueCount;
	HashToSlots.FindOrAdd(Hash).Add(ReservedSlotIndex);
	OutToken = Reservation.Token;
	bOutCreated = true;
	Reservation = FAvidScriptUtf8ValueReservation();
	return true;
}

bool FAvidScriptUtf8ValueHeap::Resolve(
	const uint32 Token,
	TConstArrayView<uint8>& OutUtf8Bytes,
	FString& OutError) const
{
	OutUtf8Bytes = TConstArrayView<uint8>();
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	const FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr || !Slot->bOccupied || Slot->bReserved)
	{
		OutError = TEXT("utf8_value_token_stale: the UTF-8 value token is invalid or stale.");
		return false;
	}
	OutUtf8Bytes = MakeArrayView(Slot->Bytes);
	return true;
}

bool FAvidScriptUtf8ValueHeap::ReleaseValue(
	const uint32 Token,
	FString& OutError)
{
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr || !Slot->bOccupied || Slot->bReserved)
	{
		OutError = TEXT("utf8_value_token_stale: the UTF-8 value token is invalid or stale.");
		return false;
	}
	RemoveHashIndex(Slot->Hash, SlotIndex);
	ReleaseSlot(SlotIndex);
	return true;
}

void FAvidScriptUtf8ValueHeap::ReleaseReservation(
	FAvidScriptUtf8ValueReservation& Reservation)
{
	if (!Reservation.bActive)
	{
		return;
	}
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (ResolveSlot(Reservation.Token, SlotIndex, Slot)
		&& Slot != nullptr && Slot->bReserved && !Slot->bOccupied)
	{
		ReleaseSlot(SlotIndex);
	}
	Reservation = FAvidScriptUtf8ValueReservation();
}

void FAvidScriptUtf8ValueHeap::RemoveCreatedValue(const uint32 Token)
{
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr || !Slot->bOccupied || Slot->bReserved)
	{
		return;
	}
	RemoveHashIndex(Slot->Hash, SlotIndex);
	ReleaseSlot(SlotIndex);
}

void FAvidScriptUtf8ValueHeap::Reset()
{
	Slots.Reset();
	FreeSlots.Reset();
	TokenToSlots.Reset();
	HashToSlots.Reset();
	LiveValueCount = 0;
	ReservedValueCount = 0;
}

bool FAvidScriptUtf8ValueHeap::IsCanonicalUtf8(
	const TConstArrayView<uint8> Bytes,
	FString& OutError)
{
	static constexpr ANSICHAR Empty[] = "";
	const ANSICHAR* Utf8 = Bytes.IsEmpty()
		? Empty
		: reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
	const FUTF8ToTCHAR Converted(Utf8, Bytes.Num());
	const FTCHARToUTF8 RoundTrip(Converted.Get(), Converted.Length());
	if (RoundTrip.Length() != Bytes.Num()
		|| (!Bytes.IsEmpty()
			&& FMemory::Memcmp(
				RoundTrip.Get(),
				Bytes.GetData(),
				Bytes.Num()) != 0))
	{
		OutError = TEXT("utf8_value_invalid: the value is not canonical UTF-8.");
		return false;
	}
	return true;
}

bool FAvidScriptUtf8ValueHeap::ResolveSlot(
	const uint32 Token,
	int32& OutSlotIndex,
	FSlot*& OutSlot)
{
	const FSlot* ConstSlot = nullptr;
	const bool bResolved = static_cast<const FAvidScriptUtf8ValueHeap*>(this)
		->ResolveSlot(Token, OutSlotIndex, ConstSlot);
	OutSlot = bResolved ? &Slots[OutSlotIndex] : nullptr;
	return bResolved;
}

bool FAvidScriptUtf8ValueHeap::ResolveSlot(
	const uint32 Token,
	int32& OutSlotIndex,
	const FSlot*& OutSlot) const
{
	OutSlotIndex = INDEX_NONE;
	OutSlot = nullptr;
	if (!IsHeapToken(Token))
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

void FAvidScriptUtf8ValueHeap::ReleaseSlot(const int32 SlotIndex)
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
	}
	Slot.Bytes.Reset();
	Slot.Hash = 0;
	Slot.bReserved = false;
	Slot.bOccupied = false;
	TokenToSlots.Remove(Slot.Token);
	Slot.Token = 0;
	FreeSlots.Add(SlotIndex);
}

void FAvidScriptUtf8ValueHeap::RemoveHashIndex(
	const uint32 Hash,
	const int32 SlotIndex)
{
	TArray<int32>* Indices = HashToSlots.Find(Hash);
	if (Indices == nullptr)
	{
		return;
	}
	Indices->RemoveSingleSwap(SlotIndex, EAllowShrinking::No);
	if (Indices->IsEmpty())
	{
		HashToSlots.Remove(Hash);
	}
}
