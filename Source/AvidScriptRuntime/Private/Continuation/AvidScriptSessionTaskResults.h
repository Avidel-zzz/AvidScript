#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptContinuationLane : uint8;

enum class EAvidScriptTaskResultState : uint8
{
	Running,
	Succeeded,
	Faulted,
	Cancelled
};

enum class EAvidScriptTaskWaitRegistration : uint8
{
	Invalid,
	Queued,
	Ready
};

struct FAvidScriptTaskResultSnapshot
{
	EAvidScriptTaskResultState State = EAvidScriptTaskResultState::Running;
	FString TypeId;
	TArray<uint8> Value;
	FString ErrorCode;
};

// Session-owned fixed-wire task results. Managed values and language-error roots
// require a separate validated codec before they can enter this table.
class FAvidScriptSessionTaskResults final
{
public:
	static constexpr int32 MaximumTasks = 4096;
	static constexpr int32 MaximumWaiters = 4096;
	static constexpr int32 MaximumValueBytes = 4096;

	int64 Create(EAvidScriptContinuationLane Lane, uint64 ActivationSerial, FString TypeId);
	bool Retain(int64 Token);
	bool Release(int64 Token);
	// The waiter owns a reference until it has read the terminal result.
	EAvidScriptTaskWaitRegistration RegisterWaiter(int64 Token, int64 WaiterToken);
	bool Succeed(int64 Token, TConstArrayView<uint8> Value, TArray<int64>& OutWaiters);
	bool Fault(int64 Token, FString ErrorCode, TArray<int64>& OutWaiters);
	bool Cancel(int64 Token, TArray<int64>& OutWaiters);
	bool Read(int64 Token, FAvidScriptTaskResultSnapshot& OutSnapshot) const;
	bool MatchesOwner(int64 Token, EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial) const;
	bool HasLaneEntries(EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial) const;
	void RetireLane(EAvidScriptContinuationLane Lane, uint64 ActivationSerial);
	void PromotePrepared(uint64 ActivationSerial);
	int32 GetCount() const { return OccupiedCount; }
	int32 GetWaiterCount() const { return WaiterCount; }

private:
	struct FEntry
	{
		EAvidScriptContinuationLane Lane;
		uint64 ActivationSerial = 0;
		EAvidScriptTaskResultState State = EAvidScriptTaskResultState::Running;
		FString TypeId;
		TArray<uint8> Value;
		FString ErrorCode;
		TArray<int64> Waiters;
		int32 ReferenceCount = 1;
	};
	struct FSlot
	{
		uint32 Generation = 0;
		TOptional<FEntry> Entry;
	};

	static int64 PackToken(uint32 Slot, uint32 Generation);
	static bool UnpackToken(int64 Token, uint32& OutSlot, uint32& OutGeneration);
	FSlot* Find(int64 Token);
	const FSlot* Find(int64 Token) const;
	void ReleaseSlot(uint32 SlotIndex);
	bool Finish(int64 Token, EAvidScriptTaskResultState State,
		TConstArrayView<uint8> Value, FString ErrorCode, TArray<int64>& OutWaiters);

	TArray<FSlot> Slots;
	TArray<uint32> FreeSlots;
	int32 OccupiedCount = 0;
	int32 WaiterCount = 0;
};
