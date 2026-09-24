#include "Continuation/AvidScriptSessionTaskResults.h"

#include "Continuation/AvidScriptSessionContinuations.h"

#include <atomic>

namespace AvidScriptTaskResultsPrivate
{
	constexpr uint64 TokenKindMask = 0x4000000000000000ull;
	std::atomic<uint32> NextGeneration{1};
}

int64 FAvidScriptSessionTaskResults::PackToken(const uint32 Slot, const uint32 Generation)
{
	return static_cast<int64>(AvidScriptTaskResultsPrivate::TokenKindMask
		| (static_cast<uint64>(Generation) << 32)
		| static_cast<uint64>(Slot + 1));
}

bool FAvidScriptSessionTaskResults::UnpackToken(
	const int64 Token, uint32& OutSlot, uint32& OutGeneration)
{
	const uint64 Packed = static_cast<uint64>(Token);
	const uint32 EncodedSlot = static_cast<uint32>(Packed);
	OutGeneration = static_cast<uint32>((Packed >> 32) & 0x3fffffffu);
	if (Token <= 0 || (Packed & 0xc000000000000000ull)
		!= AvidScriptTaskResultsPrivate::TokenKindMask
		|| EncodedSlot == 0 || OutGeneration == 0)
	{
		return false;
	}
	OutSlot = EncodedSlot - 1;
	return true;
}

FAvidScriptSessionTaskResults::FSlot* FAvidScriptSessionTaskResults::Find(const int64 Token)
{
	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(Token, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return nullptr;
	}
	FSlot& Slot = Slots[SlotIndex];
	return Slot.Generation == Generation && Slot.Entry.IsSet() ? &Slot : nullptr;
}

const FAvidScriptSessionTaskResults::FSlot* FAvidScriptSessionTaskResults::Find(
	const int64 Token) const
{
	return const_cast<FAvidScriptSessionTaskResults*>(this)->Find(Token);
}

int64 FAvidScriptSessionTaskResults::Create(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	FString TypeId)
{
	check(IsInGameThread());
	if (ActivationSerial == 0 || TypeId.IsEmpty() || OccupiedCount >= MaximumTasks)
	{
		return 0;
	}
	const uint32 Generation = AvidScriptTaskResultsPrivate::NextGeneration.fetch_add(
		1, std::memory_order_relaxed);
	if (Generation == 0 || Generation > 0x3fffffffu)
	{
		return 0;
	}
	const uint32 SlotIndex = FreeSlots.IsEmpty()
		? static_cast<uint32>(Slots.AddDefaulted())
		: FreeSlots.Pop(EAllowShrinking::No);
	FSlot& Slot = Slots[SlotIndex];
	check(!Slot.Entry.IsSet());
	Slot.Generation = Generation;
	FEntry& Entry = Slot.Entry.Emplace();
	Entry.Lane = Lane;
	Entry.ActivationSerial = ActivationSerial;
	Entry.TypeId = MoveTemp(TypeId);
	++OccupiedCount;
	return PackToken(SlotIndex, Generation);
}

bool FAvidScriptSessionTaskResults::Retain(const int64 Token)
{
	check(IsInGameThread());
	FSlot* const Slot = Find(Token);
	if (Slot == nullptr || Slot->Entry->ReferenceCount <= 0
		|| Slot->Entry->ReferenceCount == MAX_int32)
	{
		return false;
	}
	++Slot->Entry->ReferenceCount;
	return true;
}

bool FAvidScriptSessionTaskResults::Release(const int64 Token)
{
	check(IsInGameThread());
	FSlot* const Slot = Find(Token);
	if (Slot == nullptr || Slot->Entry->ReferenceCount <= 0)
	{
		return false;
	}
	--Slot->Entry->ReferenceCount;
	if (Slot->Entry->ReferenceCount == 0
		&& Slot->Entry->State != EAvidScriptTaskResultState::Running)
	{
		uint32 SlotIndex = 0;
		uint32 Generation = 0;
		UnpackToken(Token, SlotIndex, Generation);
		ReleaseSlot(SlotIndex);
	}
	return true;
}

EAvidScriptTaskWaitRegistration FAvidScriptSessionTaskResults::RegisterWaiter(
	const int64 Token, const int64 WaiterToken)
{
	check(IsInGameThread());
	FSlot* const Slot = Find(Token);
	if (Slot == nullptr || WaiterToken <= 0 || Slot->Entry->ReferenceCount <= 0)
	{
		return EAvidScriptTaskWaitRegistration::Invalid;
	}
	FEntry& Entry = Slot->Entry.GetValue();
	if (Entry.State != EAvidScriptTaskResultState::Running)
	{
		return EAvidScriptTaskWaitRegistration::Ready;
	}
	if (WaiterCount >= MaximumWaiters || Entry.Waiters.Contains(WaiterToken))
	{
		return EAvidScriptTaskWaitRegistration::Invalid;
	}
	Entry.Waiters.Add(WaiterToken);
	++WaiterCount;
	return EAvidScriptTaskWaitRegistration::Queued;
}

bool FAvidScriptSessionTaskResults::UnregisterWaiter(
	const int64 Token, const int64 WaiterToken)
{
	check(IsInGameThread());
	FSlot* const Slot = Find(Token);
	if (Slot == nullptr || Slot->Entry->State != EAvidScriptTaskResultState::Running)
	{
		return false;
	}
	const int32 Index = Slot->Entry->Waiters.IndexOfByKey(WaiterToken);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	Slot->Entry->Waiters.RemoveAt(Index);
	check(WaiterCount > 0);
	--WaiterCount;
	return true;
}

bool FAvidScriptSessionTaskResults::Finish(
	const int64 Token,
	const EAvidScriptTaskResultState State,
	const TConstArrayView<uint8> Value,
	FString ErrorCode,
	TArray<int64>& OutWaiters)
{
	check(IsInGameThread());
	FSlot* const Slot = Find(Token);
	if (Slot == nullptr || Slot->Entry->State != EAvidScriptTaskResultState::Running
		|| Value.Num() > MaximumValueBytes
		|| (State == EAvidScriptTaskResultState::Faulted && ErrorCode.IsEmpty()))
	{
		return false;
	}
	FEntry& Entry = Slot->Entry.GetValue();
	Entry.State = State;
	if (State == EAvidScriptTaskResultState::Succeeded)
	{
		Entry.Value.Append(Value.GetData(), Value.Num());
	}
	Entry.ErrorCode = MoveTemp(ErrorCode);
	WaiterCount -= Entry.Waiters.Num();
	OutWaiters = MoveTemp(Entry.Waiters);
	if (Entry.ReferenceCount == 0)
	{
		uint32 SlotIndex = 0;
		uint32 Generation = 0;
		UnpackToken(Token, SlotIndex, Generation);
		ReleaseSlot(SlotIndex);
	}
	return true;
}

bool FAvidScriptSessionTaskResults::Succeed(
	const int64 Token, const TConstArrayView<uint8> Value, TArray<int64>& OutWaiters)
{
	return Finish(Token, EAvidScriptTaskResultState::Succeeded, Value, {}, OutWaiters);
}

bool FAvidScriptSessionTaskResults::Fault(
	const int64 Token, FString ErrorCode, TArray<int64>& OutWaiters)
{
	return Finish(Token, EAvidScriptTaskResultState::Faulted, {},
		MoveTemp(ErrorCode), OutWaiters);
}

bool FAvidScriptSessionTaskResults::Cancel(
	const int64 Token, TArray<int64>& OutWaiters)
{
	return Finish(Token, EAvidScriptTaskResultState::Cancelled, {}, {}, OutWaiters);
}

bool FAvidScriptSessionTaskResults::Read(
	const int64 Token, FAvidScriptTaskResultSnapshot& OutSnapshot) const
{
	check(IsInGameThread());
	const FSlot* const Slot = Find(Token);
	if (Slot == nullptr || Slot->Entry->State == EAvidScriptTaskResultState::Running)
	{
		return false;
	}
	const FEntry& Entry = Slot->Entry.GetValue();
	OutSnapshot.State = Entry.State;
	OutSnapshot.TypeId = Entry.TypeId;
	OutSnapshot.Value = Entry.Value;
	OutSnapshot.ErrorCode = Entry.ErrorCode;
	return true;
}

bool FAvidScriptSessionTaskResults::MatchesOwner(
	const int64 Token, const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial) const
{
	check(IsInGameThread());
	const FSlot* const Slot = Find(Token);
	return Slot != nullptr && Slot->Entry->Lane == Lane
		&& Slot->Entry->ActivationSerial == ActivationSerial;
}

bool FAvidScriptSessionTaskResults::HasType(
	const int64 Token, const FString& TypeId) const
{
	check(IsInGameThread());
	const FSlot* const Slot = Find(Token);
	return Slot != nullptr && Slot->Entry->ReferenceCount > 0
		&& Slot->Entry->TypeId == TypeId;
}

bool FAvidScriptSessionTaskResults::HasLaneEntries(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial) const
{
	check(IsInGameThread());
	return Slots.ContainsByPredicate(
		[Lane, ActivationSerial](const FSlot& Slot)
		{
			return Slot.Entry.IsSet() && Slot.Entry->Lane == Lane
				&& Slot.Entry->ActivationSerial == ActivationSerial;
		});
}

void FAvidScriptSessionTaskResults::ReleaseSlot(const uint32 SlotIndex)
{
	FSlot& Slot = Slots[SlotIndex];
	check(Slot.Entry.IsSet());
	WaiterCount -= Slot.Entry->Waiters.Num();
	Slot.Entry.Reset();
	FreeSlots.Add(SlotIndex);
	--OccupiedCount;
}

void FAvidScriptSessionTaskResults::RetireLane(
	const EAvidScriptContinuationLane Lane, const uint64 ActivationSerial)
{
	check(IsInGameThread());
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		const FSlot& Slot = Slots[Index];
		if (Slot.Entry.IsSet() && Slot.Entry->Lane == Lane
			&& Slot.Entry->ActivationSerial == ActivationSerial)
		{
			ReleaseSlot(static_cast<uint32>(Index));
		}
	}
}

void FAvidScriptSessionTaskResults::PromotePrepared(const uint64 ActivationSerial)
{
	check(IsInGameThread());
	for (FSlot& Slot : Slots)
	{
		if (Slot.Entry.IsSet()
			&& Slot.Entry->Lane == EAvidScriptContinuationLane::Prepared
			&& Slot.Entry->ActivationSerial == ActivationSerial)
		{
			Slot.Entry->Lane = EAvidScriptContinuationLane::Active;
		}
	}
}
