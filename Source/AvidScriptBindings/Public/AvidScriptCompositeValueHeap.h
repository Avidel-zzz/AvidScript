#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"

enum class EAvidScriptCompositeValueKind : uint8
{
	Text,
	SoftObject,
	WeakObject,
	Array,
	Set,
	Map,
	DelegateFrame
};

struct FAvidScriptCompositeValueReservation
{
	uint32 Token = 0;
	bool bActive = false;
};

struct FAvidScriptCompositeValueView
{
	FString TypeId;
	EAvidScriptCompositeValueKind Kind = EAvidScriptCompositeValueKind::Text;
	const FProperty* Property = nullptr;
	const void* Value = nullptr;
	TConstArrayView<uint32> ChildTokens;
};

struct FAvidScriptMutableCompositeValueView
{
	FString TypeId;
	EAvidScriptCompositeValueKind Kind = EAvidScriptCompositeValueKind::Text;
	FProperty* Property = nullptr;
	void* Value = nullptr;
};

struct FAvidScriptCompositeValueHeapStats
{
	int32 LiveValues = 0;
	int32 ReservedValues = 0;
	uint64 LiveBytes = 0;
	uint64 PeakLiveBytes = 0;
	uint64 PublishedValues = 0;
	uint64 ReleasedValues = 0;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptCompositeValueHeap
{
public:
	static constexpr int32 MaxSlots = MAX_uint16;
	static constexpr uint32 MaxValueBytes = 1024u * 1024u;
	static constexpr int32 MaxChildValues = 4096;
	static constexpr int32 MaxGraphNodes = MaxChildValues * 2 + 1;
	static constexpr uint64 MaxLiveBytes = 16ull * MaxValueBytes;

	FAvidScriptCompositeValueHeap() = default;
	~FAvidScriptCompositeValueHeap();

	FAvidScriptCompositeValueHeap(const FAvidScriptCompositeValueHeap&) = delete;
	FAvidScriptCompositeValueHeap& operator=(const FAvidScriptCompositeValueHeap&) = delete;

	bool Reserve(
		FAvidScriptCompositeValueReservation& OutReservation,
		FString& OutError);
	bool PublishReserved(
		FAvidScriptCompositeValueReservation& Reservation,
		const FString& TypeId,
		EAvidScriptCompositeValueKind Kind,
		FProperty& Property,
		const void* SourceValue,
		TConstArrayView<uint32> ChildTokens,
		uint32& OutToken,
		FString& OutError);
	bool Resolve(
		uint32 Token,
		const FString& ExpectedTypeId,
		const FProperty* ExpectedProperty,
		FAvidScriptCompositeValueView& OutValue,
		FString& OutError) const;
	bool ResolveMutable(
		uint32 Token,
		FAvidScriptMutableCompositeValueView& OutValue,
		FString& OutError);
	bool CopyToProperty(
		uint32 Token,
		const FString& ExpectedTypeId,
		FProperty& DestinationProperty,
		void* DestinationValue,
		FString& OutError) const;
	bool ReplaceValue(
		uint32 Token,
		FProperty& Property,
		const void* SourceValue,
		FString& OutError);
	bool TryGetCanonicalSnapshot(
		uint32 Token,
		TConstArrayView<int32>& OutIndices) const;
	bool TryResolveCanonicalIndex(
		uint32 Token,
		int32 InternalIndex,
		int32& OutCanonicalIndex) const;
	bool StoreCanonicalSnapshot(
		uint32 Token,
		TArray<int32>&& Indices,
		FString& OutError);
	void InvalidateCanonicalSnapshot(uint32 Token);
	bool ReleaseValue(uint32 Token, FString& OutError);
	void ReleaseReservation(FAvidScriptCompositeValueReservation& Reservation);
	void RemoveCreatedValue(uint32 Token);
	void Reset();

	FAvidScriptCompositeValueHeapStats GetStats() const;

private:
	struct FSlot
	{
		FString TypeId;
		FProperty* Property = nullptr;
		void* Value = nullptr;
		TArray<uint32> ChildTokens;
		TArray<int32> CanonicalSnapshotIndices;
		TArray<int32> CanonicalIndexByInternalIndex;
		uint32 Token = 0;
		uint32 ValueBytes = 0;
		int32 ReferenceCount = 0;
		EAvidScriptCompositeValueKind Kind = EAvidScriptCompositeValueKind::Text;
		bool bReserved = false;
		bool bOccupied = false;
		bool bCanonicalSnapshotValid = false;
	};

	bool ResolveSlot(uint32 Token, int32& OutSlotIndex, FSlot*& OutSlot);
	bool ResolveSlot(uint32 Token, int32& OutSlotIndex, const FSlot*& OutSlot) const;
	void ReleaseReference(uint32 Token, bool bCountRelease);
	void ReleaseSlot(int32 SlotIndex, bool bCountRelease);

	TArray<FSlot> Slots;
	TArray<int32> FreeSlots;
	TMap<uint32, int32> TokenToSlots;
	int32 LiveValueCount = 0;
	int32 ReservedValueCount = 0;
	uint64 LiveByteCount = 0;
	uint64 PeakLiveByteCount = 0;
	uint64 PublishedValueCount = 0;
	uint64 ReleasedValueCount = 0;
};
