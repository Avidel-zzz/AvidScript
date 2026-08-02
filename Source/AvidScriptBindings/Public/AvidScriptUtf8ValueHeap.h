#pragma once

#include "CoreMinimal.h"

struct FAvidScriptUtf8ValueReservation
{
	uint32 Token = 0;
	bool bActive = false;
};

class AVIDSCRIPTBINDINGS_API FAvidScriptUtf8ValueHeap
{
public:
	static constexpr uint32 MaxValueBytes = 1024u * 1024u;
	static constexpr int32 MaxSlots = MAX_uint16;

	static bool IsHeapToken(uint32 ValueReference);

	bool Reserve(
		FAvidScriptUtf8ValueReservation& OutReservation,
		FString& OutError);
	bool InternReserved(
		FAvidScriptUtf8ValueReservation& Reservation,
		TConstArrayView<uint8> Utf8Bytes,
		uint32& OutToken,
		bool& bOutCreated,
		FString& OutError);
	bool Resolve(
		uint32 Token,
		TConstArrayView<uint8>& OutUtf8Bytes,
		FString& OutError) const;
	void ReleaseReservation(FAvidScriptUtf8ValueReservation& Reservation);
	void RemoveCreatedValue(uint32 Token);
	void Reset();

	int32 GetLiveValueCount() const { return LiveValueCount; }
	int32 GetReservedValueCount() const { return ReservedValueCount; }

private:
	struct FSlot
	{
		TArray<uint8> Bytes;
		uint32 Hash = 0;
		uint32 Token = 0;
		bool bReserved = false;
		bool bOccupied = false;
	};

	static constexpr uint32 TokenTag = 0x80000000u;

	static uint32 AllocateToken();
	static bool IsCanonicalUtf8(
		TConstArrayView<uint8> Bytes,
		FString& OutError);

	bool ResolveSlot(
		uint32 Token,
		int32& OutSlotIndex,
		FSlot*& OutSlot);
	bool ResolveSlot(
		uint32 Token,
		int32& OutSlotIndex,
		const FSlot*& OutSlot) const;
	void ReleaseSlot(int32 SlotIndex);
	void RemoveHashIndex(uint32 Hash, int32 SlotIndex);

	TArray<FSlot> Slots;
	TArray<int32> FreeSlots;
	TMap<uint32, int32> TokenToSlots;
	TMap<uint32, TArray<int32>> HashToSlots;
	int32 LiveValueCount = 0;
	int32 ReservedValueCount = 0;
};
