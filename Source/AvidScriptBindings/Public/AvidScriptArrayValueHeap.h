#pragma once

#include "CoreMinimal.h"

struct FAvidScriptArrayValueReservation
{
	uint32 Token = 0;
	bool bActive = false;
};

struct FAvidScriptArrayValueView
{
	FString TypeId;
	TConstArrayView<uint8> Bytes;
	int32 ElementCount = 0;
	int32 ElementStride = 0;
	int32 ElementAlignment = 1;
};

struct FAvidScriptArrayValueHeapStats
{
	int32 LiveValues = 0;
	int32 ReservedValues = 0;
	uint64 LiveBytes = 0;
	uint64 PeakLiveBytes = 0;
	uint64 PublishedValues = 0;
	uint64 ReleasedValues = 0;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptArrayValueHeap
{
public:
	static constexpr int32 MaxElements = 4096;
	static constexpr uint32 MaxValueBytes = 1024u * 1024u;
	static constexpr int32 MaxSlots = MAX_uint16;

	bool Reserve(
		FAvidScriptArrayValueReservation& OutReservation,
		FString& OutError);
	bool PublishReserved(
		FAvidScriptArrayValueReservation& Reservation,
		const FString& TypeId,
		int32 ElementCount,
		int32 ElementStride,
		int32 ElementAlignment,
		TConstArrayView<uint8> Bytes,
		uint32& OutToken,
		FString& OutError);
	bool Resolve(
		uint32 Token,
		const FString& ExpectedTypeId,
		FAvidScriptArrayValueView& OutValue,
		FString& OutError) const;
	bool ReadElement(
		uint32 Token,
		int32 ElementIndex,
		TArrayView<uint8> OutBytes,
		FString& OutError) const;
	bool WriteElement(
		uint32 Token,
		int32 ElementIndex,
		TConstArrayView<uint8> Bytes,
		FString& OutError);
	bool ReleaseValue(uint32 Token, FString& OutError);
	void ReleaseReservation(FAvidScriptArrayValueReservation& Reservation);
	void RemoveCreatedValue(uint32 Token);
	void Reset();

	FAvidScriptArrayValueHeapStats GetStats() const;

private:
	struct FSlot
	{
		FString TypeId;
		TArray<uint8> Bytes;
		uint32 Token = 0;
		int32 ElementCount = 0;
		int32 ElementStride = 0;
		int32 ElementAlignment = 1;
		bool bReserved = false;
		bool bOccupied = false;
	};

	bool ResolveSlot(uint32 Token, int32& OutSlotIndex, FSlot*& OutSlot);
	bool ResolveSlot(uint32 Token, int32& OutSlotIndex, const FSlot*& OutSlot) const;
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
