#pragma once

#include "AvidScriptContinuation.h"
#include "Templates/SharedPointer.h"
#include "TimerManager.h"

class UWorld;
class FAvidScriptSessionContinuations;

enum class EAvidScriptContinuationLane : uint8
{
	Active,
	Prepared
};

class FAvidScriptContinuationHostEndpoint final
	: public IAvidScriptContinuationHost
{
public:
	FAvidScriptContinuationHostEndpoint(
		TWeakPtr<FAvidScriptSessionContinuations> InOwner,
		EAvidScriptContinuationLane InLane,
		uint64 InActivationSerial);

	int64 ScheduleDelay(float DelaySeconds, int32 CallbackId) override;
	bool Cancel(int64 Token) override;

	void Invalidate();
	void PromoteToActive();
	uint64 GetActivationSerial() const { return ActivationSerial; }

private:
	TWeakPtr<FAvidScriptSessionContinuations> Owner;
	EAvidScriptContinuationLane Lane = EAvidScriptContinuationLane::Prepared;
	uint64 ActivationSerial = 0;
	bool bValid = true;
};

class FAvidScriptSessionContinuations final
	: public TSharedFromThis<FAvidScriptSessionContinuations>
{
public:
	static constexpr int32 MaximumPendingContinuations = 4096;

	~FAvidScriptSessionContinuations();

	IAvidScriptContinuationHost& BeginPrepared(UWorld* World);
	bool ValidatePreparedCommit(FString& OutError) const;
	void CommitPrepared();
	void ReleaseRetiredEndpoint();
	void DiscardPrepared();
	IAvidScriptContinuationHost& ResetActive(UWorld* World);
	void Teardown();

	void DrainReady(TArray<FAvidScriptContinuationCompletion>& OutCompletions);
	int32 GetActiveCount() const;
	int32 GetPreparedCount() const;

	int64 ScheduleDelay(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		float DelaySeconds,
		int32 CallbackId);
	bool Cancel(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int64 Token);

private:
	struct FEntry
	{
		EAvidScriptContinuationLane Lane = EAvidScriptContinuationLane::Prepared;
		uint64 ActivationSerial = 0;
		uint64 RegistrationSerial = 0;
		int32 CallbackId = 0;
		int64 Token = 0;
		TWeakObjectPtr<UWorld> World;
		FTimerHandle TimerHandle;
		bool bReady = false;
	};

	struct FSlot
	{
		uint32 Generation = 0;
		TOptional<FEntry> Entry;
	};
	struct FReadyCompletion
	{
		EAvidScriptContinuationLane Lane = EAvidScriptContinuationLane::Prepared;
		uint64 ActivationSerial = 0;
		FAvidScriptContinuationCompletion Completion;
	};

	static int64 PackToken(uint32 Slot, uint32 Generation);
	static bool UnpackToken(int64 Token, uint32& OutSlot, uint32& OutGeneration);
	static uint32 AllocateGeneration();

	int64 AllocateEntry(FEntry&& Entry);
	void ReleaseSlot(uint32 Slot);
	void HandleTimerCompletion(int64 Token);
	void CancelLane(EAvidScriptContinuationLane Lane, uint64 ActivationSerial);
	void RemoveReady(EAvidScriptContinuationLane Lane, uint64 ActivationSerial);
	void RemoveReadyToken(int64 Token);
	bool MatchesCurrentEndpoint(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial) const;
	UWorld* GetWorldForLane(EAvidScriptContinuationLane Lane) const;

	TArray<FSlot> Slots;
	TArray<uint32> FreeSlots;
	TArray<FReadyCompletion> ReadyCompletions;
	TSharedPtr<FAvidScriptContinuationHostEndpoint> ActiveEndpoint;
	TSharedPtr<FAvidScriptContinuationHostEndpoint> PreparedEndpoint;
	TSharedPtr<FAvidScriptContinuationHostEndpoint> RetiredEndpoint;
	TWeakObjectPtr<UWorld> ActiveWorld;
	TWeakObjectPtr<UWorld> PreparedWorld;
	uint64 NextActivationSerial = 1;
	uint64 NextRegistrationSerial = 1;
	int32 OccupiedSlotCount = 0;
	bool bTearingDown = false;
};
