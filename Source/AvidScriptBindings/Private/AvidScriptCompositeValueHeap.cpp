#include "AvidScriptCompositeValueHeap.h"

#include "AvidScriptValueCapability.h"

namespace
{
struct FAvidScriptCompositeValueBudget
{
	uint64 Bytes = 0;
	int32 Nodes = 0;
};

bool AddAvidScriptCompositeBytes(
	const uint64 Bytes,
	FAvidScriptCompositeValueBudget& InOutBudget,
	FString& OutError)
{
	if (Bytes > FAvidScriptCompositeValueHeap::MaxValueBytes
		|| InOutBudget.Bytes
			> FAvidScriptCompositeValueHeap::MaxValueBytes - Bytes)
	{
		OutError = TEXT("composite_value_limit_exceeded: the recursive value graph exceeds one MiB.");
		return false;
	}
	InOutBudget.Bytes += Bytes;
	return true;
}

bool MeasureAvidScriptCompositeValue(
	FProperty& Property,
	const void* Value,
	const int32 Depth,
	FAvidScriptCompositeValueBudget& InOutBudget,
	FString& OutError)
{
	if (Value == nullptr
		|| Depth > 8
		|| ++InOutBudget.Nodes > FAvidScriptCompositeValueHeap::MaxGraphNodes
		|| !AddAvidScriptCompositeBytes(
			static_cast<uint64>(FMath::Max(1, Property.GetSize())),
			InOutBudget,
			OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_value_limit_exceeded: the recursive value graph exceeds its depth or node limit.");
		}
		return false;
	}

	if (const FStrProperty* StringProperty = CastField<FStrProperty>(&Property))
	{
		const FString& String = StringProperty->GetPropertyValue(Value);
		return AddAvidScriptCompositeBytes(
			static_cast<uint64>(String.Len() + 1) * sizeof(TCHAR),
			InOutBudget,
			OutError);
	}
	if (Property.IsA<FTextProperty>())
	{
		const FString Presentation = static_cast<const FText*>(Value)->ToString();
		return AddAvidScriptCompositeBytes(
			static_cast<uint64>(Presentation.Len() + 1) * sizeof(TCHAR),
			InOutBudget,
			OutError);
	}
	if (Property.IsA<FSoftObjectProperty>())
	{
		const FString Path = static_cast<const FSoftObjectPtr*>(Value)
			->ToSoftObjectPath().ToString();
		return AddAvidScriptCompositeBytes(
			static_cast<uint64>(Path.Len() + 1) * sizeof(TCHAR),
			InOutBudget,
			OutError);
	}
	if (Property.IsA<FWeakObjectProperty>())
	{
		return true;
	}
	if (Property.IsA<FObjectPropertyBase>())
	{
		OutError = TEXT("composite_value_gc_unsupported: strong UObject leaves require an explicit GC anchor.");
		return false;
	}
	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(ArrayProperty, Value);
		if (Helper.Num() > FAvidScriptCompositeValueHeap::MaxChildValues)
		{
			OutError = TEXT("composite_value_limit_exceeded: an array exceeds the recursive element limit.");
			return false;
		}
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			if (!MeasureAvidScriptCompositeValue(
					*ArrayProperty->Inner,
					Helper.GetRawPtr(Index),
					Depth + 1,
					InOutBudget,
					OutError))
			{
				return false;
			}
		}
		return true;
	}
	if (FSetProperty* SetProperty = CastField<FSetProperty>(&Property))
	{
		FScriptSetHelper Helper(SetProperty, Value);
		if (Helper.Num() > FAvidScriptCompositeValueHeap::MaxChildValues)
		{
			OutError = TEXT("composite_value_limit_exceeded: a set exceeds the recursive element limit.");
			return false;
		}
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (Helper.IsValidIndex(Index)
				&& !MeasureAvidScriptCompositeValue(
					*SetProperty->ElementProp,
					Helper.GetElementPtr(Index),
					Depth + 1,
					InOutBudget,
					OutError))
			{
				return false;
			}
		}
		return true;
	}
	if (FMapProperty* MapProperty = CastField<FMapProperty>(&Property))
	{
		FScriptMapHelper Helper(MapProperty, Value);
		if (Helper.Num() > FAvidScriptCompositeValueHeap::MaxChildValues)
		{
			OutError = TEXT("composite_value_limit_exceeded: a map exceeds the recursive element limit.");
			return false;
		}
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (!Helper.IsValidIndex(Index))
			{
				continue;
			}
			if (!MeasureAvidScriptCompositeValue(
					*MapProperty->KeyProp,
					Helper.GetKeyPtr(Index),
					Depth + 1,
					InOutBudget,
					OutError)
				|| !MeasureAvidScriptCompositeValue(
					*MapProperty->ValueProp,
					Helper.GetValuePtr(Index),
					Depth + 1,
					InOutBudget,
					OutError))
			{
				return false;
			}
		}
		return true;
	}
	if (FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
	{
		for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
		{
			FProperty* Field = *It;
			if (Field == nullptr
				|| !MeasureAvidScriptCompositeValue(
					*Field,
					Field->ContainerPtrToValuePtr<void>(Value),
					Depth + 1,
					InOutBudget,
					OutError))
			{
				return false;
			}
		}
	}
	return true;
}

bool MeasureAvidScriptCompositeValue(
	FProperty& Property,
	const void* Value,
	uint32& OutBytes,
	FString& OutError)
{
	OutBytes = 0;
	FAvidScriptCompositeValueBudget Budget;
	if (!MeasureAvidScriptCompositeValue(
			Property,
			Value,
			0,
			Budget,
			OutError))
	{
		return false;
	}
	OutBytes = static_cast<uint32>(Budget.Bytes);
	return true;
}
} // namespace

FAvidScriptCompositeValueHeap::~FAvidScriptCompositeValueHeap()
{
	Reset();
}

bool FAvidScriptCompositeValueHeap::Reserve(
	FAvidScriptCompositeValueReservation& OutReservation,
	FString& OutError)
{
	OutReservation = FAvidScriptCompositeValueReservation();
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
			OutError = TEXT("composite_value_heap_corrupt: a free composite value slot is not reusable.");
			return false;
		}
	}
	else
	{
		if (Slots.Num() >= MaxSlots)
		{
			OutError = TEXT("composite_value_heap_exhausted: the session has no free composite value slots.");
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

bool FAvidScriptCompositeValueHeap::PublishReserved(
	FAvidScriptCompositeValueReservation& Reservation,
	const FString& TypeId,
	const EAvidScriptCompositeValueKind Kind,
	FProperty& Property,
	const void* SourceValue,
	const TConstArrayView<uint32> ChildTokens,
	uint32& OutToken,
	FString& OutError)
{
	OutToken = 0;
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (!Reservation.bActive
		|| TypeId.IsEmpty()
		|| SourceValue == nullptr
		|| !ResolveSlot(Reservation.Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bReserved
		|| Slot->bOccupied)
	{
		OutError = TEXT("composite_value_reservation_stale: the composite value reservation is invalid or stale.");
		return false;
	}

	const int32 ValueSize = Property.GetSize();
	const uint32 ValueAlignment = static_cast<uint32>(FMath::Max(1, Property.GetMinAlignment()));
	uint32 MeasuredValueBytes = 0;
	if (ValueSize <= 0
		|| static_cast<uint32>(ValueSize) > MaxValueBytes
		|| !FMath::IsPowerOfTwo(ValueAlignment)
		|| ChildTokens.Num() > MaxChildValues
		|| !MeasureAvidScriptCompositeValue(
			Property,
			SourceValue,
			MeasuredValueBytes,
			OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_value_layout_invalid: the reflected value exceeds its bounded size, alignment, or child-count contract.");
		}
		return false;
	}
	if (LiveByteCount > MaxLiveBytes - MeasuredValueBytes)
	{
		OutError = TEXT("composite_value_heap_limit_exceeded: the session exceeds its live recursive value budget.");
		return false;
	}

	TArray<FSlot*> ChildSlots;
	ChildSlots.Reserve(ChildTokens.Num());
	TSet<uint32> UniqueChildren;
	for (const uint32 ChildToken : ChildTokens)
	{
		int32 ChildSlotIndex = INDEX_NONE;
		FSlot* ChildSlot = nullptr;
		if (ChildToken == Reservation.Token
			|| UniqueChildren.Contains(ChildToken)
			|| !ResolveSlot(ChildToken, ChildSlotIndex, ChildSlot)
			|| ChildSlot == nullptr
			|| !ChildSlot->bOccupied
			|| ChildSlot->bReserved
			|| ChildSlot->ReferenceCount <= 0)
		{
			OutError = TEXT("composite_value_child_invalid: a child capability is stale, duplicated, or would create a cycle.");
			return false;
		}
		UniqueChildren.Add(ChildToken);
		ChildSlots.Add(ChildSlot);
	}

	void* StoredValue = FMemory::Malloc(ValueSize, ValueAlignment);
	if (StoredValue == nullptr)
	{
		OutError = TEXT("composite_value_allocation_failed: the reflected value storage could not be allocated.");
		return false;
	}
	Property.InitializeValue(StoredValue);
	Property.CopyCompleteValue(StoredValue, SourceValue);

	Slot->TypeId = TypeId;
	Slot->Property = &Property;
	Slot->Value = StoredValue;
	if (!ChildTokens.IsEmpty())
	{
		Slot->ChildTokens.Append(ChildTokens.GetData(), ChildTokens.Num());
	}
	Slot->ValueBytes = MeasuredValueBytes;
	Slot->ReferenceCount = 1;
	Slot->Kind = Kind;
	Slot->bReserved = false;
	Slot->bOccupied = true;
	Slot->CanonicalSnapshotIndices.Reset();
	Slot->CanonicalIndexByInternalIndex.Reset();
	Slot->bCanonicalSnapshotValid = false;
	for (FSlot* ChildSlot : ChildSlots)
	{
		++ChildSlot->ReferenceCount;
	}

	--ReservedValueCount;
	++LiveValueCount;
	++PublishedValueCount;
	LiveByteCount += Slot->ValueBytes;
	PeakLiveByteCount = FMath::Max(PeakLiveByteCount, LiveByteCount);
	OutToken = Reservation.Token;
	Reservation = FAvidScriptCompositeValueReservation();
	return true;
}

bool FAvidScriptCompositeValueHeap::Resolve(
	const uint32 Token,
	const FString& ExpectedTypeId,
	const FProperty* ExpectedProperty,
	FAvidScriptCompositeValueView& OutValue,
	FString& OutError) const
{
	OutValue = FAvidScriptCompositeValueView();
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	const FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved
		|| Slot->ReferenceCount <= 0)
	{
		OutError = TEXT("composite_value_token_stale: the composite value token is invalid or stale.");
		return false;
	}
	if ((!ExpectedTypeId.IsEmpty() && Slot->TypeId != ExpectedTypeId)
		|| (ExpectedProperty != nullptr
			&& (Slot->Property == nullptr || !ExpectedProperty->SameType(Slot->Property))))
	{
		OutError = TEXT("composite_value_type_mismatch: the composite capability type does not match the prepared descriptor.");
		return false;
	}
	OutValue.TypeId = Slot->TypeId;
	OutValue.Kind = Slot->Kind;
	OutValue.Property = Slot->Property;
	OutValue.Value = Slot->Value;
	OutValue.ChildTokens = MakeArrayView(Slot->ChildTokens);
	return true;
}

bool FAvidScriptCompositeValueHeap::ResolveMutable(
	const uint32 Token,
	FAvidScriptMutableCompositeValueView& OutValue,
	FString& OutError)
{
	OutValue = FAvidScriptMutableCompositeValueView();
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved
		|| Slot->ReferenceCount <= 0)
	{
		OutError = TEXT("composite_value_token_stale: the composite value token is invalid or stale.");
		return false;
	}
	OutValue.TypeId = Slot->TypeId;
	OutValue.Kind = Slot->Kind;
	OutValue.Property = Slot->Property;
	OutValue.Value = Slot->Value;
	return true;
}

bool FAvidScriptCompositeValueHeap::CopyToProperty(
	const uint32 Token,
	const FString& ExpectedTypeId,
	FProperty& DestinationProperty,
	void* DestinationValue,
	FString& OutError) const
{
	FAvidScriptCompositeValueView Value;
	if (DestinationValue == nullptr
		|| !Resolve(Token, ExpectedTypeId, &DestinationProperty, Value, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("composite_value_destination_invalid: the reflected destination is null.");
		}
		return false;
	}
	DestinationProperty.CopyCompleteValue(DestinationValue, Value.Value);
	return true;
}

bool FAvidScriptCompositeValueHeap::ReplaceValue(
	const uint32 Token,
	FProperty& Property,
	const void* SourceValue,
	FString& OutError)
{
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (SourceValue == nullptr
		|| !ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved
		|| Slot->ReferenceCount <= 0
		|| Slot->Property != &Property
		|| Slot->Value == nullptr)
	{
		OutError = TEXT("composite_value_replace_invalid: the value capability or reflected property is invalid.");
		return false;
	}

	uint32 MeasuredValueBytes = 0;
	if (!MeasureAvidScriptCompositeValue(
			Property,
			SourceValue,
			MeasuredValueBytes,
			OutError))
	{
		return false;
	}
	const uint64 RetainedBytes = LiveByteCount - Slot->ValueBytes;
	if (RetainedBytes > MaxLiveBytes - MeasuredValueBytes)
	{
		OutError = TEXT("composite_value_heap_limit_exceeded: the replacement exceeds the session live recursive value budget.");
		return false;
	}

	Property.CopyCompleteValue(Slot->Value, SourceValue);
	LiveByteCount = RetainedBytes + MeasuredValueBytes;
	PeakLiveByteCount = FMath::Max(PeakLiveByteCount, LiveByteCount);
	Slot->ValueBytes = MeasuredValueBytes;
	Slot->CanonicalSnapshotIndices.Reset();
	Slot->CanonicalIndexByInternalIndex.Reset();
	Slot->bCanonicalSnapshotValid = false;
	return true;
}

bool FAvidScriptCompositeValueHeap::TryGetCanonicalSnapshot(
	const uint32 Token,
	TConstArrayView<int32>& OutIndices) const
{
	OutIndices = TConstArrayView<int32>();
	int32 SlotIndex = INDEX_NONE;
	const FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved
		|| Slot->ReferenceCount <= 0
		|| (Slot->Kind != EAvidScriptCompositeValueKind::Set
			&& Slot->Kind != EAvidScriptCompositeValueKind::Map)
		|| !Slot->bCanonicalSnapshotValid)
	{
		return false;
	}
	OutIndices = MakeArrayView(Slot->CanonicalSnapshotIndices);
	return true;
}

bool FAvidScriptCompositeValueHeap::TryResolveCanonicalIndex(
	const uint32 Token,
	const int32 InternalIndex,
	int32& OutCanonicalIndex) const
{
	OutCanonicalIndex = INDEX_NONE;
	int32 SlotIndex = INDEX_NONE;
	const FSlot* Slot = nullptr;
	if (InternalIndex < 0
		|| !ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved
		|| Slot->ReferenceCount <= 0
		|| !Slot->bCanonicalSnapshotValid
		|| !Slot->CanonicalIndexByInternalIndex.IsValidIndex(InternalIndex))
	{
		return false;
	}
	OutCanonicalIndex = Slot->CanonicalIndexByInternalIndex[InternalIndex];
	return OutCanonicalIndex != INDEX_NONE;
}

bool FAvidScriptCompositeValueHeap::StoreCanonicalSnapshot(
	const uint32 Token,
	TArray<int32>&& Indices,
	FString& OutError)
{
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (Indices.Num() > MaxChildValues
		|| Indices.ContainsByPredicate([](const int32 Index) { return Index < 0; })
		|| !ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved
		|| Slot->ReferenceCount <= 0
		|| (Slot->Kind != EAvidScriptCompositeValueKind::Set
			&& Slot->Kind != EAvidScriptCompositeValueKind::Map))
	{
		OutError = TEXT("composite_container_snapshot_invalid: the canonical snapshot does not belong to a live associative container.");
		return false;
	}
	int32 MaxInternalIndex = INDEX_NONE;
	for (const int32 InternalIndex : Indices)
	{
		MaxInternalIndex = FMath::Max(MaxInternalIndex, InternalIndex);
	}
	TArray<int32> ReverseIndices;
	ReverseIndices.Init(INDEX_NONE, MaxInternalIndex + 1);
	for (int32 CanonicalIndex = 0; CanonicalIndex < Indices.Num(); ++CanonicalIndex)
	{
		const int32 InternalIndex = Indices[CanonicalIndex];
		if (ReverseIndices[InternalIndex] != INDEX_NONE)
		{
			OutError = TEXT("composite_container_snapshot_invalid: the canonical snapshot contains a duplicate internal index.");
			return false;
		}
		ReverseIndices[InternalIndex] = CanonicalIndex;
	}
	Slot->CanonicalSnapshotIndices = MoveTemp(Indices);
	Slot->CanonicalIndexByInternalIndex = MoveTemp(ReverseIndices);
	Slot->bCanonicalSnapshotValid = true;
	return true;
}

void FAvidScriptCompositeValueHeap::InvalidateCanonicalSnapshot(
	const uint32 Token)
{
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (ResolveSlot(Token, SlotIndex, Slot) && Slot != nullptr)
	{
		Slot->CanonicalSnapshotIndices.Reset();
		Slot->CanonicalIndexByInternalIndex.Reset();
		Slot->bCanonicalSnapshotValid = false;
	}
}

bool FAvidScriptCompositeValueHeap::ReleaseValue(
	const uint32 Token,
	FString& OutError)
{
	OutError.Reset();
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->bReserved
		|| Slot->ReferenceCount <= 0)
	{
		OutError = TEXT("composite_value_token_stale: the composite value token is invalid or stale.");
		return false;
	}
	ReleaseReference(Token, true);
	return true;
}

void FAvidScriptCompositeValueHeap::ReleaseReservation(
	FAvidScriptCompositeValueReservation& Reservation)
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
	Reservation = FAvidScriptCompositeValueReservation();
}

void FAvidScriptCompositeValueHeap::RemoveCreatedValue(const uint32 Token)
{
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (ResolveSlot(Token, SlotIndex, Slot)
		&& Slot != nullptr
		&& Slot->bOccupied
		&& !Slot->bReserved
		&& Slot->ReferenceCount > 0)
	{
		ReleaseReference(Token, false);
	}
}

void FAvidScriptCompositeValueHeap::Reset()
{
	ReleasedValueCount += static_cast<uint64>(LiveValueCount);
	for (FSlot& Slot : Slots)
	{
		if (Slot.Property != nullptr && Slot.Value != nullptr)
		{
			Slot.Property->DestroyValue(Slot.Value);
		}
		if (Slot.Value != nullptr)
		{
			FMemory::Free(Slot.Value);
		}
		Slot.Value = nullptr;
		Slot.Property = nullptr;
		Slot.CanonicalSnapshotIndices.Reset();
		Slot.CanonicalIndexByInternalIndex.Reset();
		Slot.bCanonicalSnapshotValid = false;
	}
	Slots.Reset();
	FreeSlots.Reset();
	TokenToSlots.Reset();
	LiveValueCount = 0;
	ReservedValueCount = 0;
	LiveByteCount = 0;
}

FAvidScriptCompositeValueHeapStats FAvidScriptCompositeValueHeap::GetStats() const
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

bool FAvidScriptCompositeValueHeap::ResolveSlot(
	const uint32 Token,
	int32& OutSlotIndex,
	FSlot*& OutSlot)
{
	const FSlot* ConstSlot = nullptr;
	const bool bResolved = static_cast<const FAvidScriptCompositeValueHeap*>(this)
		->ResolveSlot(Token, OutSlotIndex, ConstSlot);
	OutSlot = bResolved ? &Slots[OutSlotIndex] : nullptr;
	return bResolved;
}

bool FAvidScriptCompositeValueHeap::ResolveSlot(
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

void FAvidScriptCompositeValueHeap::ReleaseReference(
	const uint32 Token,
	const bool bCountRelease)
{
	int32 SlotIndex = INDEX_NONE;
	FSlot* Slot = nullptr;
	if (!ResolveSlot(Token, SlotIndex, Slot)
		|| Slot == nullptr
		|| !Slot->bOccupied
		|| Slot->ReferenceCount <= 0)
	{
		return;
	}
	--Slot->ReferenceCount;
	if (Slot->ReferenceCount == 0)
	{
		ReleaseSlot(SlotIndex, bCountRelease);
	}
}

void FAvidScriptCompositeValueHeap::ReleaseSlot(
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

	TArray<uint32> Children;
	if (Slot.bOccupied)
	{
		Children = MoveTemp(Slot.ChildTokens);
		if (Slot.Property != nullptr && Slot.Value != nullptr)
		{
			Slot.Property->DestroyValue(Slot.Value);
		}
		if (Slot.Value != nullptr)
		{
			FMemory::Free(Slot.Value);
		}
		--LiveValueCount;
		LiveByteCount -= Slot.ValueBytes;
		if (bCountRelease)
		{
			++ReleasedValueCount;
		}
	}

	Slot.TypeId.Reset();
	Slot.Property = nullptr;
	Slot.Value = nullptr;
	Slot.ChildTokens.Reset();
	Slot.CanonicalSnapshotIndices.Reset();
	Slot.CanonicalIndexByInternalIndex.Reset();
	Slot.ValueBytes = 0;
	Slot.ReferenceCount = 0;
	Slot.Kind = EAvidScriptCompositeValueKind::Text;
	Slot.bReserved = false;
	Slot.bOccupied = false;
	Slot.bCanonicalSnapshotValid = false;
	TokenToSlots.Remove(Slot.Token);
	Slot.Token = 0;
	FreeSlots.Add(SlotIndex);

	for (const uint32 ChildToken : Children)
	{
		ReleaseReference(ChildToken, false);
	}
}
