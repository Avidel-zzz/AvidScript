#pragma once

#include "AvidScriptBindingLatent.h"
#include "AvidScriptContinuation.h"
#include "AvidScriptObjectRegistry.h"
#include "Continuation/AvidScriptLatentCallbackProxy.h"
#include "Continuation/AvidScriptSessionTaskResults.h"
#include "Delegate/AvidScriptDelegateBridge.h"
#include "Templates/SharedPointer.h"
#include "TimerManager.h"
#include "UObject/ObjectKey.h"
#include "UObject/StrongObjectPtr.h"

class FAvidScriptObjectRegistry;
class FAvidScriptSessionObjectOwnership;
class IAvidScriptAsyncObjectLoadHandle;
class IAvidScriptAsyncObjectLoader;
class UWorld;
class FAvidScriptSessionContinuations;

enum class EAvidScriptContinuationLane : uint8
{
	Active,
	Prepared
};

class FAvidScriptContinuationHostEndpoint final
	: public IAvidScriptContinuationHost
	, public IAvidScriptTaskHost
	, public IAvidScriptBindingLatentHost
{
public:
	FAvidScriptContinuationHostEndpoint(
		TWeakPtr<FAvidScriptSessionContinuations> InOwner,
		EAvidScriptContinuationLane InLane,
		uint64 InActivationSerial);

	int64 ScheduleDelay(float DelaySeconds, int32 CallbackId) override;
	int64 ScheduleObjectLoad(FString ObjectPath, int32 CallbackId) override;
	bool Cancel(int64 Token) override;
	int64 CreateCancellationSource() override;
	bool CancelCancellationSource(int64 SourceToken) override;
	bool ReleaseCancellationSource(int64 SourceToken) override;
	bool BindCancellationSource(
		int64 SourceToken,
		int64 ContinuationToken) override;
	bool StoreState(
		int64 ContinuationToken,
		TConstArrayView<uint8> StateBytes) override;
	bool ReadState(
		int64 ContinuationToken,
		TArrayView<uint8> OutStateBytes) override;
	bool ConsumeResult(
		int64 ContinuationToken,
		int32 Slot,
		int32 Generation,
		const FString& ExpectedTypeId,
		FAvidScriptBindingLatentCompletionPayload& OutPayload) override;
	bool StoreManagedState(int64 ContinuationToken,
		const FAvidScriptWasmRuntimeInstance& Runtime, TConstArrayView<uint8> StateBytes,
		TUniquePtr<IAvidScriptContinuationStateLease>&& Lease) override;
	bool ReadManagedState(int64 ContinuationToken,
		const FAvidScriptWasmRuntimeInstance& Runtime, TArrayView<uint8> OutStateBytes) override;
	int64 CreateTaskResult(FString TypeId) override;
	bool RetainTaskResult(int64 Token) override;
	bool ReleaseTaskResult(int64 Token) override;
	bool HasTaskResultType(int64 Token, const FString& TypeId) const override;
	EAvidScriptTaskWaitRegistration AwaitTaskResult(
		int64 Token, int32 CallbackId, int64& OutContinuationToken) override;
	bool BindTaskProducer(int64 TaskToken, int64 ContinuationToken) override;
	bool SucceedTaskResult(int64 Token, TConstArrayView<uint8> Value,
		TArray<int64>& OutWaiters) override;
	bool FaultTaskResult(int64 Token, FString ErrorCode, TArray<int64>& OutWaiters) override;
	bool CancelTaskResult(int64 Token, TArray<int64>& OutWaiters) override;
	bool ReadTaskResult(int64 Token, FAvidScriptTaskResultSnapshot& OutSnapshot) const override;
	bool BeginLatent(
		int32 CallbackId,
		FAvidScriptBindingLatentReservation& OutReservation) override;
	bool BeginLatentWithCompletion(
		int32 CallbackId,
		const FAvidScriptBindingLatentCompletionContract& Completion,
		FAvidScriptBindingLatentReservation& OutReservation) override;
	bool CommitLatent(int64 Token) override;
	bool AbortLatent(int64 Token) override;
	int64 BeginAsyncAction(
		int32 CallbackId,
		UObject& Action,
		const FAvidScriptBindingAsyncActionContract& Contract,
		FString& OutError) override;

	void Invalidate();
	void PromoteToActive();
	uint64 GetActivationSerial() const { return ActivationSerial; }

private:
	TSharedPtr<FAvidScriptSessionContinuations> PinTaskOwner(int64 Token = 0) const;
	TWeakPtr<FAvidScriptSessionContinuations> Owner;
	EAvidScriptContinuationLane Lane = EAvidScriptContinuationLane::Prepared;
	uint64 ActivationSerial = 0;
	bool bValid = true;
};

class FAvidScriptSessionContinuations final
	: public TSharedFromThis<FAvidScriptSessionContinuations>
	, public IAvidScriptDelegateBridgeSink
{
	friend class FAvidScriptContinuationHostEndpoint;
public:
	static constexpr int32 MaximumPendingContinuations = 4096;
	static constexpr int32 MaximumRetainedLoadedObjects = 1024;
	static constexpr int32 MaximumCancellationSources = 1024;
	static constexpr int32 MaximumCancellationBindings = 4096;
	static constexpr int32 MaximumResultSlots = 1024;
	static constexpr int32 MaximumResultPayloadCells = 64;
	static constexpr int32 MaximumFixedResultBytes = 4096;
	static constexpr int32 MaximumStateFrameBytes = 4096;

	explicit FAvidScriptSessionContinuations(
		TSharedPtr<IAvidScriptAsyncObjectLoader> InAsyncObjectLoader = nullptr);

	~FAvidScriptSessionContinuations();

	FAvidScriptContinuationHostEndpoint& BeginPrepared(
		UWorld* World,
		FAvidScriptObjectRegistry* ObjectRegistry = nullptr,
		FAvidScriptSessionObjectOwnership* ObjectOwnership = nullptr,
		FAvidScriptObjectHandle OwnerHandle = {});
	bool ValidatePreparedCommit(FString& OutError) const;
	void CommitPrepared();
	void ReleaseRetiredEndpoint();
	void DiscardPrepared();
	FAvidScriptContinuationHostEndpoint& ResetActive(
		UWorld* World,
		FAvidScriptObjectRegistry* ObjectRegistry = nullptr,
		FAvidScriptSessionObjectOwnership* ObjectOwnership = nullptr,
		FAvidScriptObjectHandle OwnerHandle = {});
	void Teardown();

	void DrainReady(TArray<FAvidScriptContinuationCompletion>& OutCompletions);
	bool NeedsTickPump() const;
	bool FinalizeDispatched(int64 Token, bool bSucceeded);
	int32 GetActiveCount() const;
	int32 GetPreparedCount() const;
#if WITH_DEV_AUTOMATION_TESTS
	FAvidScriptSessionTaskResults& GetTaskResultsForTesting() { return TaskResults; }
	int32 GetCancellationSourceCountForTesting() const
	{
		return OccupiedCancellationSourceCount;
	}
	int32 GetCancellationBindingCountForTesting() const
	{
		return CancellationBindingCount;
	}
	int32 GetResultSlotCountForTesting() const
	{
		return OccupiedResultSlotCount;
	}
	int32 GetRetainedLoadedObjectCountForTesting() const
	{
		return RetainedLoadedObjects.Num();
	}
	int32 GetStateFrameByteCountForTesting() const
	{
		int32 ByteCount = 0;
		for (const FSlot& Slot : Slots)
		{
			ByteCount += Slot.Entry.IsSet()
				? Slot.Entry->StateFrame.Num()
				: 0;
		}
		return ByteCount;
	}
#endif

	int64 ScheduleDelay(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		float DelaySeconds,
		int32 CallbackId);
	int64 ScheduleObjectLoad(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		FString ObjectPath,
		int32 CallbackId);
	EAvidScriptTaskWaitRegistration AwaitTaskResult(
		EAvidScriptContinuationLane Lane, uint64 ActivationSerial,
		int64 TaskToken, int32 CallbackId, int64& OutContinuationToken);
	bool BindTaskProducer(EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial, int64 TaskToken, int64 ContinuationToken);
	bool Cancel(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int64 Token);
	int64 CreateCancellationSource(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial);
	bool CancelCancellationSource(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int64 SourceToken);
	bool ReleaseCancellationSource(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int64 SourceToken);
	bool BindCancellationSource(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int64 SourceToken,
		int64 ContinuationToken);
	bool StoreState(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int64 ContinuationToken,
		TConstArrayView<uint8> StateBytes);
	bool ReadState(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int64 ContinuationToken,
		TArrayView<uint8> OutStateBytes);
	bool BeginLatent(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int32 CallbackId,
		FAvidScriptBindingLatentReservation& OutReservation);
	bool BeginLatentWithCompletion(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int32 CallbackId,
		const FAvidScriptBindingLatentCompletionContract& Completion,
		FAvidScriptBindingLatentReservation& OutReservation);
	bool CommitLatent(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int64 Token);
	bool AbortLatent(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int64 Token);
	bool ConsumeResult(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int64 ContinuationToken,
		int32 Slot,
		int32 Generation,
		const FString& ExpectedTypeId,
		FAvidScriptBindingLatentCompletionPayload& OutPayload);
	int64 BeginAsyncAction(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial,
		int32 CallbackId,
		UObject& Action,
		const FAvidScriptBindingAsyncActionContract& Contract,
		FString& OutError);
	virtual void HandleAvidScriptDelegateBroadcast(
		uint64 SubscriptionToken,
		void* Parameters) override;

private:
	friend class UAvidScriptLatentCallbackProxy;
	friend class FAvidScriptContinuationHostEndpoint;
	bool StoreStateImpl(EAvidScriptContinuationLane Lane, uint64 ActivationSerial,
		int64 ContinuationToken, TConstArrayView<uint8> StateBytes,
		TUniquePtr<IAvidScriptContinuationStateLease>&& Lease);
	bool ReadStateImpl(EAvidScriptContinuationLane Lane, uint64 ActivationSerial,
		int64 ContinuationToken, TArrayView<uint8> OutStateBytes,
		const FAvidScriptWasmRuntimeInstance* Runtime);

	enum class EProducerKind : uint8
	{
		Timer,
		AsyncObjectLoad,
		LatentAction,
		AsyncAction,
		TaskResult
	};

	struct FEntry
	{
		EAvidScriptContinuationLane Lane = EAvidScriptContinuationLane::Prepared;
		uint64 ActivationSerial = 0;
		uint64 RegistrationSerial = 0;
		int32 CallbackId = 0;
		int64 Token = 0;
		int64 TaskResultToken = 0;
		int64 ProducerTaskToken = 0;
		int64 CancellationSourceToken = 0;
		TWeakObjectPtr<UWorld> World;
		FTimerHandle TimerHandle;
		TSharedPtr<IAvidScriptAsyncObjectLoadHandle> AsyncLoadHandle;
		TStrongObjectPtr<UObject> LoadedObject;
		TStrongObjectPtr<UAvidScriptLatentCallbackProxy> LatentProxy;
		TStrongObjectPtr<UObject> AsyncAction;
		TArray<TStrongObjectPtr<UAvidScriptDelegateBridge>> AsyncActionBridges;
		TArray<FScriptDelegate> AsyncActionDelegates;
		TArray<FMulticastDelegateProperty*> AsyncActionProperties;
		TArray<uint64> AsyncActionBridgeTokens;
		TArray<TSharedPtr<const IAvidScriptBindingAsyncActionPayloadEncoder>>
			AsyncActionPayloadEncoders;
		FString AsyncActionPayloadTypeId;
		FAvidScriptBindingLatentCompletionContract LatentCompletion;
		EProducerKind ProducerKind = EProducerKind::Timer;
		int32 ResultSlot = 0;
		int32 ResultGeneration = 0;
		int32 BorrowedHandleCheckpoint = 0;
		TArray<uint8, TInlineAllocator<128>> StateFrame;
		TUniquePtr<IAvidScriptContinuationStateLease> StateLease;
		bool bReady = false;
		bool bDispatching = false;
		bool bHasBorrowedHandleCheckpoint = false;
		bool bDispatchHasObjectResult = false;
		bool bLatentCommitted = false;
		bool bLatentCompletionPending = false;
		bool bCancelledTerminalQueued = false;
		bool bStateConsumed = false;
	};

	struct FSlot
	{
		uint32 Generation = 0;
		TOptional<FEntry> Entry;
	};
	enum class ECancellationSourceState : uint8
	{
		Open,
		Cancelled
	};
	struct FCancellationSourceEntry
	{
		EAvidScriptContinuationLane Lane = EAvidScriptContinuationLane::Prepared;
		uint64 ActivationSerial = 0;
		int64 Token = 0;
		ECancellationSourceState State = ECancellationSourceState::Open;
		TSet<int64> Bindings;
	};
	struct FCancellationSourceSlot
	{
		uint32 Generation = 0;
		TOptional<FCancellationSourceEntry> Entry;
	};
	struct FResultEntry
	{
		EAvidScriptContinuationLane Lane = EAvidScriptContinuationLane::Prepared;
		uint64 ActivationSerial = 0;
		int64 ContinuationToken = 0;
		FAvidScriptBindingLatentCompletionPayload Payload;
	};
	struct FResultSlot
	{
		uint32 Generation = 0;
		TOptional<FResultEntry> Entry;
	};
	struct FReadyCompletion
	{
		EAvidScriptContinuationLane Lane = EAvidScriptContinuationLane::Prepared;
		uint64 ActivationSerial = 0;
		FAvidScriptContinuationCompletion Completion;
	};
	struct FRetiredLatentProxy
	{
		TWeakObjectPtr<UWorld> World;
		TStrongObjectPtr<UAvidScriptLatentCallbackProxy> Proxy;
	};
	struct FAsyncActionRoute
	{
		int64 ContinuationToken = 0;
		int32 OutcomeOrdinal = INDEX_NONE;
	};

	static int64 PackToken(uint32 Slot, uint32 Generation);
	static bool UnpackToken(int64 Token, uint32& OutSlot, uint32& OutGeneration);
	static int64 PackCancellationSourceToken(uint32 Slot, uint32 Generation);
	static bool UnpackCancellationSourceToken(
		int64 Token,
		uint32& OutSlot,
		uint32& OutGeneration);
	static uint32 AllocateGeneration();

	int64 AllocateEntry(FEntry&& Entry);
	int64 AllocateCancellationSource(FCancellationSourceEntry&& Entry);
	bool AllocateResult(
		FResultEntry&& Entry,
		int32& OutSlot,
		int32& OutGeneration);
	void ReleaseSlot(uint32 Slot);
	void ReleaseCancellationSourceSlot(uint32 Slot);
	void ReleaseResultSlot(uint32 Slot);
	void ReleaseEntryResult(FEntry& Entry);
	void UnbindEntryFromCancellationSource(FEntry& Entry);
	bool CancelEntry(uint32 SlotIndex, bool bDeliverTerminal);
	void FinishBoundProducerTask(FEntry& Entry, bool bFault);
	void HandleTimerCompletion(int64 Token);
	void HandleObjectLoadCompletion(int64 Token, UObject* LoadedObject);
	void HandleLatentCompletion(int64 Token, int32 Linkage);
	void QueueLatentCompletion(FEntry& Entry);
	void ReleaseAsyncActionProducer(FEntry& Entry);
	void HandleObjectsReinstanced(
		const TMap<UObject*, UObject*>& ReplacementObjects);
	void SweepInvalidAsyncActions();
	uint64 AllocateAsyncActionBridgeToken();
	void CancelEntryProducer(FEntry& Entry);
	void QueueTaskWaiters(int64 TaskToken, EAvidScriptTaskResultState State,
		const TArray<int64>& Waiters);
	void RetireLatentProxy(FEntry& Entry);
	void CollectRetiredLatentProxies();
	void CancelLane(EAvidScriptContinuationLane Lane, uint64 ActivationSerial);
	void ReleaseCancellationSourcesForLane(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial);
	void InvalidateLane(EAvidScriptContinuationLane Lane, uint64 ActivationSerial);
	void RemoveReady(EAvidScriptContinuationLane Lane, uint64 ActivationSerial);
	void RemoveReadyToken(int64 Token);
	void ClearRetainedLoadedObjects();
	bool MatchesCurrentEndpoint(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial) const;
	bool CanUseTaskResults(EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial) const;
	bool HasLaneEntries(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial) const;
	bool HasLaneCancellationSources(
		EAvidScriptContinuationLane Lane,
		uint64 ActivationSerial) const;
	bool IsLaneContextLive(EAvidScriptContinuationLane Lane) const;
	bool IsEntryContextLive(const FEntry& Entry) const;
	UWorld* GetWorldForLane(EAvidScriptContinuationLane Lane) const;

	TArray<FSlot> Slots;
	TArray<uint32> FreeSlots;
	TArray<FCancellationSourceSlot> CancellationSourceSlots;
	TArray<uint32> FreeCancellationSourceSlots;
	TArray<FResultSlot> ResultSlots;
	TArray<uint32> FreeResultSlots;
	TArray<FReadyCompletion> ReadyCompletions;
	FAvidScriptSessionTaskResults TaskResults;
	TArray<FRetiredLatentProxy> RetiredLatentProxies;
	TMap<uint64, FAsyncActionRoute> AsyncActionRoutes;
	TArray<TStrongObjectPtr<UObject>> RetainedLoadedObjects;
	TSet<TObjectKey<UObject>> RetainedLoadedObjectKeys;
	TSharedPtr<IAvidScriptAsyncObjectLoader> AsyncObjectLoader;
	FDelegateHandle ObjectsReinstancedHandle;
	TSharedPtr<FAvidScriptContinuationHostEndpoint> ActiveEndpoint;
	TSharedPtr<FAvidScriptContinuationHostEndpoint> PreparedEndpoint;
	TSharedPtr<FAvidScriptContinuationHostEndpoint> RetiredEndpoint;
	TWeakObjectPtr<UWorld> ActiveWorld;
	TWeakObjectPtr<UWorld> PreparedWorld;
	FAvidScriptObjectRegistry* ActiveObjectRegistry = nullptr;
	FAvidScriptObjectRegistry* PreparedObjectRegistry = nullptr;
	FAvidScriptSessionObjectOwnership* ActiveObjectOwnership = nullptr;
	FAvidScriptSessionObjectOwnership* PreparedObjectOwnership = nullptr;
	FAvidScriptObjectHandle ActiveOwnerHandle;
	FAvidScriptObjectHandle PreparedOwnerHandle;
	uint64 NextActivationSerial = 1;
	uint64 NextRegistrationSerial = 1;
	uint64 NextAsyncActionBridgeToken = 1;
	int32 OccupiedSlotCount = 0;
	int32 PendingAsyncActionCount = 0;
	int32 OccupiedCancellationSourceCount = 0;
	int32 CancellationBindingCount = 0;
	int32 OccupiedResultSlotCount = 0;
	int32 ActiveEntryCount = 0;
	int32 PreparedEntryCount = 0;
	bool bTearingDown = false;
};
