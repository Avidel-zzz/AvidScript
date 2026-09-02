#include "Continuation/AvidScriptSessionContinuations.h"

#include "AvidScriptArrayValueHeap.h"
#include "AvidScriptUtf8ValueHeap.h"
#include "Continuation/AvidScriptAsyncObjectLoader.h"
#include "Containers/StringConv.h"
#include "Engine/LatentActionManager.h"
#include "Engine/World.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Misc/PackageName.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptSessionContinuations, Log, All);

namespace
{
std::atomic<uint32> GAvidScriptContinuationGeneration{1};
constexpr uint64 CancellationSourceKindMask = 0x8000000000000000ull;

bool TryMakeAsyncObjectPath(
	const FString& ObjectPath,
	FSoftObjectPath& OutObjectPath)
{
	OutObjectPath.Reset();
	if (ObjectPath.IsEmpty()
		|| FCString::Strlen(*ObjectPath) != ObjectPath.Len())
	{
		return false;
	}

	const FTCHARToUTF8 Utf8Path(*ObjectPath);
	if (Utf8Path.Length() <= 0 || Utf8Path.Length() > 1024)
	{
		return false;
	}

	FSoftObjectPath Candidate(ObjectPath);
	const FString LongPackageName = Candidate.GetLongPackageName();
	if (Candidate.IsNull()
		|| !Candidate.IsValid()
		|| !FPackageName::IsValidLongPackageName(LongPackageName))
	{
		return false;
	}

	OutObjectPath = MoveTemp(Candidate);
	return true;
}

bool IsBoundedCompositeResultPayload(
	const FAvidScriptBindingLatentCompletionPayload& Payload)
{
	if (Payload.Fields.IsEmpty()
		|| Payload.Fields.Num()
			> FAvidScriptSessionContinuations::MaximumResultPayloadCells)
	{
		return false;
	}

	for (int32 FieldIndex = 0;
		FieldIndex < Payload.Fields.Num();
		++FieldIndex)
	{
		const FAvidScriptBindingLatentCompletionField& Field =
			Payload.Fields[FieldIndex];
		const int32 FieldSize =
			Field.Kind == EAvidScriptBindingLatentPayloadKind::FixedWire
				? Field.Bytes.Num()
				: Field.Kind == EAvidScriptBindingLatentPayloadKind::Object
					? static_cast<int32>(sizeof(FAvidScriptObjectHandle))
					: 0;
		if (Field.TypeId.IsEmpty()
			|| Field.WireOffset < 0
			|| FieldSize <= 0
			|| Field.WireOffset
				> FAvidScriptSessionContinuations::MaximumFixedResultBytes
					- FieldSize)
		{
			return false;
		}
		for (int32 PriorIndex = 0;
			PriorIndex < FieldIndex;
			++PriorIndex)
		{
			if (Payload.Fields[PriorIndex].WireOffset
				== Field.WireOffset)
			{
				return false;
			}
		}
	}
	return true;
}
}

FAvidScriptContinuationHostEndpoint::FAvidScriptContinuationHostEndpoint(
	TWeakPtr<FAvidScriptSessionContinuations> InOwner,
	const EAvidScriptContinuationLane InLane,
	const uint64 InActivationSerial)
	: Owner(MoveTemp(InOwner))
	, Lane(InLane)
	, ActivationSerial(InActivationSerial)
{
}

int64 FAvidScriptContinuationHostEndpoint::ScheduleDelay(
	const float DelaySeconds,
	const int32 CallbackId)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		? PinnedOwner->ScheduleDelay(
			Lane,
			ActivationSerial,
			DelaySeconds,
			CallbackId)
		: 0;
}

int64 FAvidScriptContinuationHostEndpoint::ScheduleObjectLoad(
	FString ObjectPath,
	const int32 CallbackId)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		? PinnedOwner->ScheduleObjectLoad(
			Lane,
			ActivationSerial,
			MoveTemp(ObjectPath),
			CallbackId)
		: 0;
}

bool FAvidScriptContinuationHostEndpoint::Cancel(const int64 Token)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->Cancel(Lane, ActivationSerial, Token);
}

int64 FAvidScriptContinuationHostEndpoint::CreateCancellationSource()
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		? PinnedOwner->CreateCancellationSource(Lane, ActivationSerial)
		: 0;
}

bool FAvidScriptContinuationHostEndpoint::CancelCancellationSource(
	const int64 SourceToken)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->CancelCancellationSource(
			Lane,
			ActivationSerial,
			SourceToken);
}

bool FAvidScriptContinuationHostEndpoint::ReleaseCancellationSource(
	const int64 SourceToken)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->ReleaseCancellationSource(
			Lane,
			ActivationSerial,
			SourceToken);
}

bool FAvidScriptContinuationHostEndpoint::BindCancellationSource(
	const int64 SourceToken,
	const int64 ContinuationToken)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->BindCancellationSource(
			Lane,
			ActivationSerial,
			SourceToken,
			ContinuationToken);
}

bool FAvidScriptContinuationHostEndpoint::StoreState(
	const int64 ContinuationToken,
	const TConstArrayView<uint8> StateBytes)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->StoreState(
			Lane,
			ActivationSerial,
			ContinuationToken,
			StateBytes);
}

bool FAvidScriptContinuationHostEndpoint::ReadState(
	const int64 ContinuationToken,
	const TArrayView<uint8> OutStateBytes)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->ReadState(
			Lane,
			ActivationSerial,
			ContinuationToken,
			OutStateBytes);
}

bool FAvidScriptContinuationHostEndpoint::ConsumeResult(
	const int64 ContinuationToken,
	const int32 Slot,
	const int32 Generation,
	const FString& ExpectedTypeId,
	FAvidScriptBindingLatentCompletionPayload& OutPayload)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->ConsumeResult(
			Lane,
			ActivationSerial,
			ContinuationToken,
			Slot,
			Generation,
			ExpectedTypeId,
			OutPayload);
}

bool FAvidScriptContinuationHostEndpoint::BeginLatent(
	const int32 CallbackId,
	FAvidScriptBindingLatentReservation& OutReservation)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->BeginLatent(
			Lane,
			ActivationSerial,
			CallbackId,
			OutReservation);
}

bool FAvidScriptContinuationHostEndpoint::BeginLatentWithCompletion(
	const int32 CallbackId,
	const FAvidScriptBindingLatentCompletionContract& Completion,
	FAvidScriptBindingLatentReservation& OutReservation)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->BeginLatentWithCompletion(
			Lane,
			ActivationSerial,
			CallbackId,
			Completion,
			OutReservation);
}

bool FAvidScriptContinuationHostEndpoint::CommitLatent(const int64 Token)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->CommitLatent(Lane, ActivationSerial, Token);
}

bool FAvidScriptContinuationHostEndpoint::AbortLatent(const int64 Token)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	return bValid && PinnedOwner
		&& PinnedOwner->AbortLatent(Lane, ActivationSerial, Token);
}

int64 FAvidScriptContinuationHostEndpoint::BeginAsyncAction(
	const int32 CallbackId,
	UObject& Action,
	const FAvidScriptBindingAsyncActionContract& Contract,
	FString& OutError)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	if (!bValid || !PinnedOwner)
	{
		OutError = TEXT("async_action_endpoint_stale");
		return 0;
	}
	return PinnedOwner->BeginAsyncAction(
		Lane,
		ActivationSerial,
		CallbackId,
		Action,
		Contract,
		OutError);
}

void FAvidScriptContinuationHostEndpoint::Invalidate()
{
	bValid = false;
}

void FAvidScriptContinuationHostEndpoint::PromoteToActive()
{
	check(bValid);
	Lane = EAvidScriptContinuationLane::Active;
}

FAvidScriptSessionContinuations::FAvidScriptSessionContinuations(
	TSharedPtr<IAvidScriptAsyncObjectLoader> InAsyncObjectLoader)
	: AsyncObjectLoader(InAsyncObjectLoader
		? MoveTemp(InAsyncObjectLoader)
		: CreateAvidScriptAsyncObjectLoader())
{
}

FAvidScriptSessionContinuations::~FAvidScriptSessionContinuations()
{
	check(IsInGameThread());
#if WITH_EDITOR
	if (ObjectsReinstancedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectsReinstanced.Remove(
			ObjectsReinstancedHandle);
		ObjectsReinstancedHandle.Reset();
	}
#endif
	Teardown();
}

FAvidScriptContinuationHostEndpoint& FAvidScriptSessionContinuations::BeginPrepared(
	UWorld* World,
	FAvidScriptObjectRegistry* ObjectRegistry,
	FAvidScriptSessionObjectOwnership* ObjectOwnership,
	const FAvidScriptObjectHandle OwnerHandle)
{
	check(IsInGameThread());
	CollectRetiredLatentProxies();
	DiscardPrepared();
	bTearingDown = false;
	PreparedWorld = World;
	PreparedObjectRegistry = ObjectRegistry;
	PreparedObjectOwnership = ObjectOwnership;
	PreparedOwnerHandle = OwnerHandle;
	const uint64 ActivationSerial = NextActivationSerial++;
	PreparedEndpoint = MakeShared<FAvidScriptContinuationHostEndpoint>(
		TWeakPtr<FAvidScriptSessionContinuations>(AsShared()),
		EAvidScriptContinuationLane::Prepared,
		ActivationSerial);
	return *PreparedEndpoint;
}

bool FAvidScriptSessionContinuations::ValidatePreparedCommit(
	FString& OutError) const
{
	OutError.Reset();
	if (!PreparedEndpoint)
	{
		OutError = TEXT("continuation_prepared_lane_missing");
		return false;
	}

	const uint64 PreparedActivation = PreparedEndpoint->GetActivationSerial();
	if ((HasLaneEntries(
				EAvidScriptContinuationLane::Prepared,
				PreparedActivation)
			|| HasLaneCancellationSources(
				EAvidScriptContinuationLane::Prepared,
				PreparedActivation))
		&& !IsLaneContextLive(EAvidScriptContinuationLane::Prepared))
	{
		OutError = TEXT("continuation_prepared_context_unavailable");
		return false;
	}

	for (const FSlot& Slot : Slots)
	{
		if (!Slot.Entry.IsSet()
			|| Slot.Entry->Lane != EAvidScriptContinuationLane::Prepared
			|| Slot.Entry->ActivationSerial != PreparedEndpoint->GetActivationSerial())
		{
			continue;
		}
		if (Slot.Entry->ProducerKind == EProducerKind::Timer)
		{
			UWorld* const World = Slot.Entry->World.Get();
			if (World == nullptr || World->bIsTearingDown)
			{
				OutError = TEXT("continuation_prepared_world_unavailable");
				return false;
			}
		}
		else if (Slot.Entry->ProducerKind == EProducerKind::LatentAction
			&& !Slot.Entry->bLatentCommitted)
		{
			OutError = TEXT("continuation_prepared_latent_uncommitted");
			return false;
		}
	}
	return true;
}

void FAvidScriptSessionContinuations::CommitPrepared()
{
	check(IsInGameThread());
	check(PreparedEndpoint);
	const uint64 PreparedActivation = PreparedEndpoint->GetActivationSerial();
	if (ActiveEndpoint)
	{
		const uint64 ActiveActivation = ActiveEndpoint->GetActivationSerial();
		ActiveEndpoint->Invalidate();
		CancelLane(EAvidScriptContinuationLane::Active, ActiveActivation);
		RemoveReady(EAvidScriptContinuationLane::Active, ActiveActivation);
		RetiredEndpoint = MoveTemp(ActiveEndpoint);
	}

	for (FSlot& Slot : Slots)
	{
		if (Slot.Entry.IsSet()
			&& Slot.Entry->Lane == EAvidScriptContinuationLane::Prepared
			&& Slot.Entry->ActivationSerial == PreparedActivation)
		{
			Slot.Entry->Lane = EAvidScriptContinuationLane::Active;
		}
	}
	for (FCancellationSourceSlot& Slot : CancellationSourceSlots)
	{
		if (Slot.Entry.IsSet()
			&& Slot.Entry->Lane == EAvidScriptContinuationLane::Prepared
			&& Slot.Entry->ActivationSerial == PreparedActivation)
		{
			Slot.Entry->Lane = EAvidScriptContinuationLane::Active;
		}
	}
	for (FResultSlot& Slot : ResultSlots)
	{
		if (Slot.Entry.IsSet()
			&& Slot.Entry->Lane == EAvidScriptContinuationLane::Prepared
			&& Slot.Entry->ActivationSerial == PreparedActivation)
		{
			Slot.Entry->Lane = EAvidScriptContinuationLane::Active;
		}
	}
	for (FReadyCompletion& Ready : ReadyCompletions)
	{
		if (Ready.Lane == EAvidScriptContinuationLane::Prepared
			&& Ready.ActivationSerial == PreparedActivation)
		{
			Ready.Lane = EAvidScriptContinuationLane::Active;
		}
	}
	ActiveEntryCount += PreparedEntryCount;
	PreparedEntryCount = 0;

	PreparedEndpoint->PromoteToActive();
	ActiveEndpoint = MoveTemp(PreparedEndpoint);
	ActiveWorld = PreparedWorld;
	ActiveObjectRegistry = PreparedObjectRegistry;
	ActiveObjectOwnership = PreparedObjectOwnership;
	ActiveOwnerHandle = PreparedOwnerHandle;
	PreparedWorld.Reset();
	PreparedObjectRegistry = nullptr;
	PreparedObjectOwnership = nullptr;
	PreparedOwnerHandle = {};
}

void FAvidScriptSessionContinuations::ReleaseRetiredEndpoint()
{
	RetiredEndpoint.Reset();
}

void FAvidScriptSessionContinuations::DiscardPrepared()
{
	check(IsInGameThread());
	if (!PreparedEndpoint)
	{
		PreparedWorld.Reset();
		PreparedObjectRegistry = nullptr;
		PreparedObjectOwnership = nullptr;
		PreparedOwnerHandle = {};
		return;
	}
	const uint64 ActivationSerial = PreparedEndpoint->GetActivationSerial();
	PreparedEndpoint->Invalidate();
	CancelLane(EAvidScriptContinuationLane::Prepared, ActivationSerial);
	RemoveReady(EAvidScriptContinuationLane::Prepared, ActivationSerial);
	PreparedEndpoint.Reset();
	PreparedWorld.Reset();
	PreparedObjectRegistry = nullptr;
	PreparedObjectOwnership = nullptr;
	PreparedOwnerHandle = {};
}

FAvidScriptContinuationHostEndpoint& FAvidScriptSessionContinuations::ResetActive(
	UWorld* World,
	FAvidScriptObjectRegistry* ObjectRegistry,
	FAvidScriptSessionObjectOwnership* ObjectOwnership,
	const FAvidScriptObjectHandle OwnerHandle)
{
	check(IsInGameThread());
	CollectRetiredLatentProxies();
	DiscardPrepared();
	if (ActiveEndpoint)
	{
		const uint64 ActivationSerial = ActiveEndpoint->GetActivationSerial();
		ActiveEndpoint->Invalidate();
		CancelLane(EAvidScriptContinuationLane::Active, ActivationSerial);
		RemoveReady(EAvidScriptContinuationLane::Active, ActivationSerial);
		RetiredEndpoint = MoveTemp(ActiveEndpoint);
	}
	ClearRetainedLoadedObjects();
	bTearingDown = false;
	ActiveWorld = World;
	ActiveObjectRegistry = ObjectRegistry;
	ActiveObjectOwnership = ObjectOwnership;
	ActiveOwnerHandle = OwnerHandle;
	ActiveEndpoint = MakeShared<FAvidScriptContinuationHostEndpoint>(
		TWeakPtr<FAvidScriptSessionContinuations>(AsShared()),
		EAvidScriptContinuationLane::Active,
		NextActivationSerial++);
	return *ActiveEndpoint;
}

void FAvidScriptSessionContinuations::Teardown()
{
	check(IsInGameThread());
	bTearingDown = true;
	DiscardPrepared();
	if (ActiveEndpoint)
	{
		const uint64 ActivationSerial = ActiveEndpoint->GetActivationSerial();
		ActiveEndpoint->Invalidate();
		CancelLane(EAvidScriptContinuationLane::Active, ActivationSerial);
		RemoveReady(EAvidScriptContinuationLane::Active, ActivationSerial);
		RetiredEndpoint = MoveTemp(ActiveEndpoint);
	}
	ActiveWorld.Reset();
	ActiveObjectRegistry = nullptr;
	ActiveObjectOwnership = nullptr;
	ActiveOwnerHandle = {};
	ReadyCompletions.Reset();
	ClearRetainedLoadedObjects();
	CollectRetiredLatentProxies();
}

void FAvidScriptSessionContinuations::DrainReady(
	TArray<FAvidScriptContinuationCompletion>& OutCompletions)
{
	check(IsInGameThread());
	CollectRetiredLatentProxies();
	OutCompletions.Reset();
	if (bTearingDown || !ActiveEndpoint)
	{
		return;
	}
	SweepInvalidAsyncActions();
	const uint64 ActiveActivation = ActiveEndpoint->GetActivationSerial();
	if (!HasLaneEntries(
			EAvidScriptContinuationLane::Active,
			ActiveActivation))
	{
		return;
	}
	if (!IsLaneContextLive(EAvidScriptContinuationLane::Active))
	{
		InvalidateLane(EAvidScriptContinuationLane::Active, ActiveActivation);
		return;
	}

	int32 SelectedIndex = INDEX_NONE;
	for (int32 Index = 0; Index < ReadyCompletions.Num(); ++Index)
	{
		const FReadyCompletion& Ready = ReadyCompletions[Index];
		if (Ready.Lane == EAvidScriptContinuationLane::Active
			&& Ready.ActivationSerial == ActiveActivation
			&& (SelectedIndex == INDEX_NONE
				|| Ready.Completion.RegistrationSerial
					< ReadyCompletions[SelectedIndex].Completion.RegistrationSerial))
		{
			SelectedIndex = Index;
		}
	}
	if (SelectedIndex != INDEX_NONE)
	{
		FAvidScriptContinuationCompletion Completion =
			ReadyCompletions[SelectedIndex].Completion;
		ReadyCompletions.RemoveAtSwap(
			SelectedIndex,
			1,
			EAllowShrinking::No);
		uint32 SlotIndex = 0;
		uint32 Generation = 0;
		if (UnpackToken(Completion.Token, SlotIndex, Generation)
			&& Slots.IsValidIndex(static_cast<int32>(SlotIndex))
			&& Slots[SlotIndex].Generation == Generation
			&& Slots[SlotIndex].Entry.IsSet())
		{
			FEntry& Entry = Slots[SlotIndex].Entry.GetValue();
			if (Entry.bDispatching)
			{
				return;
			}

			Entry.bDispatching = true;
			if (ActiveObjectRegistry != nullptr
				&& ActiveObjectOwnership != nullptr)
			{
				Entry.BorrowedHandleCheckpoint =
					ActiveObjectOwnership->GetBorrowedHandleCount();
				Entry.bHasBorrowedHandleCheckpoint = true;
			}

			UObject* const LoadedObject = Entry.LoadedObject.Get();
			if (Completion.Status == EAvidScriptContinuationStatus::Completed
				&& Entry.ProducerKind == EProducerKind::AsyncObjectLoad)
			{
				const bool bRetentionLimitReached = IsValid(LoadedObject)
					&& !RetainedLoadedObjectKeys.Contains(
						TObjectKey<UObject>(LoadedObject))
					&& RetainedLoadedObjects.Num()
						>= MaximumRetainedLoadedObjects;
				FAvidScriptObjectHandleResult BorrowResult;
				if (!IsValid(LoadedObject)
					|| bRetentionLimitReached
					|| ActiveObjectRegistry == nullptr
					|| ActiveObjectOwnership == nullptr
					|| !ActiveObjectOwnership->Borrow(
						*ActiveObjectRegistry,
						*LoadedObject,
						BorrowResult)
					|| !BorrowResult.Handle.IsValid())
				{
					Completion.Status = EAvidScriptContinuationStatus::Failed;
					Entry.LoadedObject.Reset();
				}
				else
				{
					Completion.ObjectSlot = static_cast<int32>(
						BorrowResult.Handle.Slot);
					Completion.ObjectGeneration = static_cast<int32>(
						BorrowResult.Handle.Generation);
					Entry.bDispatchHasObjectResult = true;
				}
			}
			OutCompletions.Add(Completion);
		}
	}
}

bool FAvidScriptSessionContinuations::FinalizeDispatched(
	const int64 Token,
	const bool bSucceeded)
{
	check(IsInGameThread());
	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(Token, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return false;
	}

	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Generation
		|| !Slot.Entry.IsSet()
		|| !Slot.Entry->bDispatching)
	{
		return false;
	}

	FEntry& Entry = Slot.Entry.GetValue();
	bool bFinalized = true;
	if (!bSucceeded
		&& Entry.bHasBorrowedHandleCheckpoint
		&& ActiveObjectRegistry != nullptr
		&& ActiveObjectOwnership != nullptr)
	{
		FString RollbackError;
		bFinalized = ActiveObjectOwnership->RollbackBorrowedHandles(
			*ActiveObjectRegistry,
			Entry.BorrowedHandleCheckpoint,
			RollbackError);
	}
	else if (!bSucceeded && Entry.bHasBorrowedHandleCheckpoint)
	{
		bFinalized = false;
	}

	UObject* const LoadedObject = Entry.LoadedObject.Get();
	if (bSucceeded
		&& Entry.bDispatchHasObjectResult
		&& IsValid(LoadedObject))
	{
		const TObjectKey<UObject> ObjectKey(LoadedObject);
		if (!RetainedLoadedObjectKeys.Contains(ObjectKey))
		{
			if (RetainedLoadedObjects.Num() >= MaximumRetainedLoadedObjects)
			{
				bFinalized = false;
			}
			else
			{
				RetainedLoadedObjects.Emplace(LoadedObject);
				RetainedLoadedObjectKeys.Add(ObjectKey);
			}
		}
	}

	if (Entry.ProducerKind == EProducerKind::LatentAction
		&& Entry.LatentProxy.IsValid())
	{
		Entry.LatentProxy->Disarm();
	}
	ReleaseSlot(SlotIndex);
	return bFinalized;
}

int32 FAvidScriptSessionContinuations::GetActiveCount() const
{
	if (!ActiveEndpoint)
	{
		return 0;
	}
	const uint64 ActivationSerial = ActiveEndpoint->GetActivationSerial();
	int32 Count = 0;
	for (const FSlot& Slot : Slots)
	{
		if (Slot.Entry.IsSet()
			&& Slot.Entry->Lane == EAvidScriptContinuationLane::Active
			&& Slot.Entry->ActivationSerial == ActivationSerial)
		{
			++Count;
		}
	}
	return Count;
}

int32 FAvidScriptSessionContinuations::GetPreparedCount() const
{
	if (!PreparedEndpoint)
	{
		return 0;
	}
	const uint64 ActivationSerial = PreparedEndpoint->GetActivationSerial();
	int32 Count = 0;
	for (const FSlot& Slot : Slots)
	{
		if (Slot.Entry.IsSet()
			&& Slot.Entry->Lane == EAvidScriptContinuationLane::Prepared
			&& Slot.Entry->ActivationSerial == ActivationSerial)
		{
			++Count;
		}
	}
	return Count;
}

int64 FAvidScriptSessionContinuations::ScheduleDelay(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const float DelaySeconds,
	const int32 CallbackId)
{
	if (!IsInGameThread()
		|| bTearingDown
		|| !MatchesCurrentEndpoint(Lane, ActivationSerial)
		|| !FMath::IsFinite(DelaySeconds)
		|| DelaySeconds < 0.0f
		|| CallbackId <= 0
		|| OccupiedSlotCount >= MaximumPendingContinuations)
	{
		return 0;
	}
	if (!IsLaneContextLive(Lane))
	{
		InvalidateLane(Lane, ActivationSerial);
		return 0;
	}

	UWorld* const World = GetWorldForLane(Lane);
	if (World == nullptr || World->bIsTearingDown)
	{
		return 0;
	}

	FEntry Entry;
	Entry.Lane = Lane;
	Entry.ActivationSerial = ActivationSerial;
	Entry.RegistrationSerial = NextRegistrationSerial++;
	Entry.CallbackId = CallbackId;
	Entry.World = World;
	Entry.ProducerKind = EProducerKind::Timer;
	const int64 Token = AllocateEntry(MoveTemp(Entry));
	if (Token == 0)
	{
		return 0;
	}

	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	check(UnpackToken(Token, SlotIndex, Generation));
	FEntry& StoredEntry = Slots[SlotIndex].Entry.GetValue();
	const TWeakPtr<FAvidScriptSessionContinuations> WeakOwner(AsShared());
	FTimerDelegate Completion = FTimerDelegate::CreateLambda([WeakOwner, Token]()
	{
		if (const TSharedPtr<FAvidScriptSessionContinuations> Owner = WeakOwner.Pin())
		{
			Owner->HandleTimerCompletion(Token);
		}
	});
	if (DelaySeconds == 0.0f)
	{
		StoredEntry.TimerHandle = World->GetTimerManager().SetTimerForNextTick(
			MoveTemp(Completion));
	}
	else
	{
		World->GetTimerManager().SetTimer(
			StoredEntry.TimerHandle,
			MoveTemp(Completion),
			DelaySeconds,
			false);
	}
	return Token;
}

int64 FAvidScriptSessionContinuations::ScheduleObjectLoad(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	FString ObjectPath,
	const int32 CallbackId)
{
	if (!IsInGameThread()
		|| bTearingDown
		|| !MatchesCurrentEndpoint(Lane, ActivationSerial)
		|| CallbackId <= 0
		|| OccupiedSlotCount >= MaximumPendingContinuations
		|| !AsyncObjectLoader)
	{
		return 0;
	}
	if (!IsLaneContextLive(Lane))
	{
		InvalidateLane(Lane, ActivationSerial);
		return 0;
	}

	FAvidScriptObjectRegistry* const ObjectRegistry =
		Lane == EAvidScriptContinuationLane::Active
			? ActiveObjectRegistry
			: PreparedObjectRegistry;
	FAvidScriptSessionObjectOwnership* const ObjectOwnership =
		Lane == EAvidScriptContinuationLane::Active
			? ActiveObjectOwnership
			: PreparedObjectOwnership;
	UWorld* const World = GetWorldForLane(Lane);
	FSoftObjectPath SoftObjectPath;
	if (ObjectRegistry == nullptr
		|| ObjectOwnership == nullptr
		|| World == nullptr
		|| !TryMakeAsyncObjectPath(ObjectPath, SoftObjectPath))
	{
		return 0;
	}

	FEntry Entry;
	Entry.Lane = Lane;
	Entry.ActivationSerial = ActivationSerial;
	Entry.RegistrationSerial = NextRegistrationSerial++;
	Entry.CallbackId = CallbackId;
	Entry.World = World;
	Entry.ProducerKind = EProducerKind::AsyncObjectLoad;
	const int64 Token = AllocateEntry(MoveTemp(Entry));
	if (Token == 0)
	{
		return 0;
	}

	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	check(UnpackToken(Token, SlotIndex, Generation));
	const TWeakPtr<FAvidScriptSessionContinuations> WeakOwner(AsShared());
	TSharedPtr<IAvidScriptAsyncObjectLoadHandle> LoadHandle =
		AsyncObjectLoader->RequestAsyncLoad(
			SoftObjectPath,
			[WeakOwner, Token](UObject* LoadedObject)
			{
				if (const TSharedPtr<FAvidScriptSessionContinuations> Owner =
						WeakOwner.Pin())
				{
					Owner->HandleObjectLoadCompletion(Token, LoadedObject);
				}
			});
	const bool bEntryStillValid =
		Slots.IsValidIndex(static_cast<int32>(SlotIndex))
		&& Slots[SlotIndex].Generation == Generation
		&& Slots[SlotIndex].Entry.IsSet();
	if (!LoadHandle)
	{
		if (bEntryStillValid)
		{
			RemoveReadyToken(Token);
			ReleaseSlot(SlotIndex);
		}
		return 0;
	}

	if (bEntryStillValid)
	{
		Slots[SlotIndex].Entry->AsyncLoadHandle = MoveTemp(LoadHandle);
		return Token;
	}

	LoadHandle->Cancel();
	return 0;
}

bool FAvidScriptSessionContinuations::Cancel(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int64 Token)
{
	if (!IsInGameThread() || !MatchesCurrentEndpoint(Lane, ActivationSerial))
	{
		return false;
	}
	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(Token, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return false;
	}
	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Generation
		|| !Slot.Entry.IsSet()
		|| Slot.Entry->Lane != Lane
		|| Slot.Entry->ActivationSerial != ActivationSerial
		|| Slot.Entry->bDispatching)
	{
		return false;
	}
	return CancelEntry(SlotIndex, true);
}

int64 FAvidScriptSessionContinuations::CreateCancellationSource(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial)
{
	if (!IsInGameThread()
		|| bTearingDown
		|| !MatchesCurrentEndpoint(Lane, ActivationSerial)
		|| OccupiedCancellationSourceCount >= MaximumCancellationSources)
	{
		return 0;
	}
	if (!IsLaneContextLive(Lane))
	{
		InvalidateLane(Lane, ActivationSerial);
		return 0;
	}

	FCancellationSourceEntry Entry;
	Entry.Lane = Lane;
	Entry.ActivationSerial = ActivationSerial;
	return AllocateCancellationSource(MoveTemp(Entry));
}

bool FAvidScriptSessionContinuations::CancelCancellationSource(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int64 SourceToken)
{
	if (!IsInGameThread() || !MatchesCurrentEndpoint(Lane, ActivationSerial))
	{
		return false;
	}
	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackCancellationSourceToken(SourceToken, SlotIndex, Generation)
		|| !CancellationSourceSlots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return false;
	}
	FCancellationSourceSlot& Slot = CancellationSourceSlots[SlotIndex];
	if (Slot.Generation != Generation
		|| !Slot.Entry.IsSet()
		|| Slot.Entry->Lane != Lane
		|| Slot.Entry->ActivationSerial != ActivationSerial
		|| Slot.Entry->State != ECancellationSourceState::Open)
	{
		return false;
	}

	Slot.Entry->State = ECancellationSourceState::Cancelled;
	TArray<int64> Bindings;
	Bindings.Reserve(Slot.Entry->Bindings.Num());
	for (const int64 ContinuationToken : Slot.Entry->Bindings)
	{
		Bindings.Add(ContinuationToken);
	}
	Bindings.Sort();
	for (const int64 ContinuationToken : Bindings)
	{
		Cancel(Lane, ActivationSerial, ContinuationToken);
	}
	return true;
}

bool FAvidScriptSessionContinuations::ReleaseCancellationSource(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int64 SourceToken)
{
	if (!IsInGameThread() || !MatchesCurrentEndpoint(Lane, ActivationSerial))
	{
		return false;
	}
	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackCancellationSourceToken(SourceToken, SlotIndex, Generation)
		|| !CancellationSourceSlots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return false;
	}
	FCancellationSourceSlot& Slot = CancellationSourceSlots[SlotIndex];
	if (Slot.Generation != Generation
		|| !Slot.Entry.IsSet()
		|| Slot.Entry->Lane != Lane
		|| Slot.Entry->ActivationSerial != ActivationSerial)
	{
		return false;
	}

	TArray<int64> Bindings;
	Bindings.Reserve(Slot.Entry->Bindings.Num());
	for (const int64 ContinuationToken : Slot.Entry->Bindings)
	{
		Bindings.Add(ContinuationToken);
	}
	for (const int64 ContinuationToken : Bindings)
	{
		uint32 ContinuationSlotIndex = 0;
		uint32 ContinuationGeneration = 0;
		if (UnpackToken(
				ContinuationToken,
				ContinuationSlotIndex,
				ContinuationGeneration)
			&& Slots.IsValidIndex(static_cast<int32>(ContinuationSlotIndex)))
		{
			FSlot& ContinuationSlot = Slots[ContinuationSlotIndex];
			if (ContinuationSlot.Generation == ContinuationGeneration
				&& ContinuationSlot.Entry.IsSet()
				&& ContinuationSlot.Entry->CancellationSourceToken == SourceToken)
			{
				ContinuationSlot.Entry->CancellationSourceToken = 0;
				check(CancellationBindingCount > 0);
				--CancellationBindingCount;
			}
		}
	}
	Slot.Entry->Bindings.Reset();
	ReleaseCancellationSourceSlot(SlotIndex);
	return true;
}

bool FAvidScriptSessionContinuations::BindCancellationSource(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int64 SourceToken,
	const int64 ContinuationToken)
{
	if (!IsInGameThread() || !MatchesCurrentEndpoint(Lane, ActivationSerial))
	{
		return false;
	}

	uint32 ContinuationSlotIndex = 0;
	uint32 ContinuationGeneration = 0;
	if (!UnpackToken(
			ContinuationToken,
			ContinuationSlotIndex,
			ContinuationGeneration)
		|| !Slots.IsValidIndex(static_cast<int32>(ContinuationSlotIndex)))
	{
		return false;
	}
	FSlot& ContinuationSlot = Slots[ContinuationSlotIndex];
	if (ContinuationSlot.Generation != ContinuationGeneration
		|| !ContinuationSlot.Entry.IsSet()
		|| ContinuationSlot.Entry->Lane != Lane
		|| ContinuationSlot.Entry->ActivationSerial != ActivationSerial
		|| ContinuationSlot.Entry->bDispatching)
	{
		return false;
	}

	uint32 SourceSlotIndex = 0;
	uint32 SourceGeneration = 0;
	const bool bSourceTokenValid = UnpackCancellationSourceToken(
		SourceToken,
		SourceSlotIndex,
		SourceGeneration)
		&& CancellationSourceSlots.IsValidIndex(static_cast<int32>(SourceSlotIndex));
	FCancellationSourceSlot* const SourceSlot = bSourceTokenValid
		? &CancellationSourceSlots[SourceSlotIndex]
		: nullptr;
	if (SourceSlot == nullptr
		|| SourceSlot->Generation != SourceGeneration
		|| !SourceSlot->Entry.IsSet()
		|| SourceSlot->Entry->Lane != Lane
		|| SourceSlot->Entry->ActivationSerial != ActivationSerial
		|| ContinuationSlot.Entry->CancellationSourceToken != 0)
	{
		CancelEntry(ContinuationSlotIndex, false);
		return false;
	}

	if (SourceSlot->Entry->State == ECancellationSourceState::Cancelled)
	{
		return Cancel(Lane, ActivationSerial, ContinuationToken);
	}
	if (CancellationBindingCount >= MaximumCancellationBindings)
	{
		Cancel(Lane, ActivationSerial, ContinuationToken);
		return false;
	}

	ContinuationSlot.Entry->CancellationSourceToken = SourceToken;
	SourceSlot->Entry->Bindings.Add(ContinuationToken);
	++CancellationBindingCount;
	return true;
}

bool FAvidScriptSessionContinuations::StoreState(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int64 ContinuationToken,
	const TConstArrayView<uint8> StateBytes)
{
	if (!IsInGameThread()
		|| bTearingDown
		|| !MatchesCurrentEndpoint(Lane, ActivationSerial)
		|| StateBytes.IsEmpty()
		|| StateBytes.Num() > MaximumStateFrameBytes)
	{
		return false;
	}

	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(ContinuationToken, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return false;
	}

	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Generation
		|| !Slot.Entry.IsSet()
		|| Slot.Entry->Lane != Lane
		|| Slot.Entry->ActivationSerial != ActivationSerial
		|| Slot.Entry->bDispatching
		|| !Slot.Entry->StateFrame.IsEmpty()
		|| Slot.Entry->bStateConsumed)
	{
		return false;
	}

	Slot.Entry->StateFrame.Append(StateBytes.GetData(), StateBytes.Num());
	return true;
}

bool FAvidScriptSessionContinuations::ReadState(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int64 ContinuationToken,
	const TArrayView<uint8> OutStateBytes)
{
	if (!IsInGameThread()
		|| bTearingDown
		|| !MatchesCurrentEndpoint(Lane, ActivationSerial)
		|| OutStateBytes.IsEmpty()
		|| OutStateBytes.Num() > MaximumStateFrameBytes)
	{
		return false;
	}

	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(ContinuationToken, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return false;
	}

	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Generation
		|| !Slot.Entry.IsSet()
		|| Slot.Entry->Lane != Lane
		|| Slot.Entry->ActivationSerial != ActivationSerial
		|| !Slot.Entry->bDispatching
		|| Slot.Entry->bStateConsumed
		|| Slot.Entry->StateFrame.Num() != OutStateBytes.Num())
	{
		return false;
	}

	FMemory::Memcpy(
		OutStateBytes.GetData(),
		Slot.Entry->StateFrame.GetData(),
		OutStateBytes.Num());
	Slot.Entry->StateFrame.Reset();
	Slot.Entry->bStateConsumed = true;
	return true;
}

bool FAvidScriptSessionContinuations::BeginLatent(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int32 CallbackId,
	FAvidScriptBindingLatentReservation& OutReservation)
{
	FAvidScriptBindingLatentCompletionContract Completion;
	return BeginLatentWithCompletion(
		Lane,
		ActivationSerial,
		CallbackId,
		Completion,
		OutReservation);
}

bool FAvidScriptSessionContinuations::BeginLatentWithCompletion(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int32 CallbackId,
	const FAvidScriptBindingLatentCompletionContract& Completion,
	FAvidScriptBindingLatentReservation& OutReservation)
{
	OutReservation = {};
	const bool bNoneCompletion = Completion.Mode == TEXT("none")
		&& Completion.ProviderId.IsEmpty()
		&& Completion.PayloadTypeId.IsEmpty()
		&& !Completion.Provider.IsValid();
	const bool bProviderCompletion = Completion.IsProvider()
		&& (Completion.StatusPolicy == TEXT("abandon_on_cancel")
			|| Completion.StatusPolicy == TEXT("resume_outcome_on_cancel"))
		&& Completion.bCancellable
		&& Completion.Provider->GetProviderId() == Completion.ProviderId
		&& Completion.Provider->GetPayloadTypeId()
			== Completion.PayloadTypeId;
	if (!IsInGameThread()
		|| bTearingDown
		|| CallbackId < 0
		|| (!bNoneCompletion && !bProviderCompletion)
		|| !MatchesCurrentEndpoint(Lane, ActivationSerial)
		|| OccupiedSlotCount >= MaximumPendingContinuations)
	{
		return false;
	}
	if (!IsLaneContextLive(Lane))
	{
		InvalidateLane(Lane, ActivationSerial);
		return false;
	}

	CollectRetiredLatentProxies();
	FEntry Entry;
	Entry.Lane = Lane;
	Entry.ActivationSerial = ActivationSerial;
	Entry.RegistrationSerial = NextRegistrationSerial++;
	Entry.CallbackId = CallbackId;
	Entry.World = GetWorldForLane(Lane);
	Entry.ProducerKind = EProducerKind::LatentAction;
	Entry.LatentCompletion = Completion;
	const int64 Token = AllocateEntry(MoveTemp(Entry));
	if (Token == 0)
	{
		return false;
	}

	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	check(UnpackToken(Token, SlotIndex, Generation));
	check(Slots.IsValidIndex(static_cast<int32>(SlotIndex)));
	FSlot& Slot = Slots[SlotIndex];
	check(Slot.Generation == Generation && Slot.Entry.IsSet());
	UAvidScriptLatentCallbackProxy* const Proxy =
		NewObject<UAvidScriptLatentCallbackProxy>(GetTransientPackage());
	if (!IsValid(Proxy))
	{
		ReleaseSlot(SlotIndex);
		return false;
	}
	Slot.Entry->LatentProxy.Reset(Proxy);
	Proxy->Arm(
		TWeakPtr<FAvidScriptSessionContinuations>(AsShared()),
		Token);

	OutReservation.Token = Token;
	OutReservation.CallbackTarget = Proxy;
	OutReservation.ExecutionFunction = GET_FUNCTION_NAME_CHECKED(
		UAvidScriptLatentCallbackProxy,
		OnLatentCompleted);
	OutReservation.UUID = 1;
	OutReservation.Linkage = 0;
	return true;
}

bool FAvidScriptSessionContinuations::CommitLatent(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int64 Token)
{
	if (!IsInGameThread() || !MatchesCurrentEndpoint(Lane, ActivationSerial))
	{
		return false;
	}
	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(Token, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return false;
	}
	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Generation
		|| !Slot.Entry.IsSet()
		|| Slot.Entry->Lane != Lane
		|| Slot.Entry->ActivationSerial != ActivationSerial
		|| Slot.Entry->ProducerKind != EProducerKind::LatentAction
		|| Slot.Entry->bLatentCommitted
		|| Slot.Entry->bDispatching)
	{
		return false;
	}

	FEntry& Entry = Slot.Entry.GetValue();
	if (!IsEntryContextLive(Entry))
	{
		InvalidateLane(Lane, ActivationSerial);
		return false;
	}
	UWorld* const World = Entry.World.Get();
	UAvidScriptLatentCallbackProxy* const Proxy = Entry.LatentProxy.Get();
	if (!IsValid(World) || !IsValid(Proxy))
	{
		return false;
	}
	const bool bActionRegistered =
		World->GetLatentActionManager().GetNumActionsForObject(Proxy) > 0;
	if (!Entry.bLatentCompletionPending && !bActionRegistered)
	{
		return false;
	}

	Entry.bLatentCommitted = true;
	if (Entry.bLatentCompletionPending)
	{
		QueueLatentCompletion(Entry);
	}
	return true;
}

bool FAvidScriptSessionContinuations::AbortLatent(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int64 Token)
{
	if (!IsInGameThread() || !MatchesCurrentEndpoint(Lane, ActivationSerial))
	{
		return false;
	}
	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(Token, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return false;
	}
	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Generation
		|| !Slot.Entry.IsSet()
		|| Slot.Entry->Lane != Lane
		|| Slot.Entry->ActivationSerial != ActivationSerial
		|| Slot.Entry->ProducerKind != EProducerKind::LatentAction
		|| Slot.Entry->bDispatching)
	{
		return false;
	}

	CancelEntryProducer(Slot.Entry.GetValue());
	RemoveReadyToken(Token);
	ReleaseSlot(SlotIndex);
	return true;
}

int64 FAvidScriptSessionContinuations::BeginAsyncAction(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int32 CallbackId,
	UObject& Action,
	const FAvidScriptBindingAsyncActionContract& Contract,
	FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread()
		|| bTearingDown
		|| CallbackId < 0
		|| !Contract.IsValid()
		|| !Action.IsA(Contract.ActionClass)
		|| !Action.IsA(UBlueprintAsyncActionBase::StaticClass())
		|| !MatchesCurrentEndpoint(Lane, ActivationSerial)
		|| OccupiedSlotCount >= MaximumPendingContinuations)
	{
		OutError = TEXT("async_action_context_invalid");
		return 0;
	}
	if (!IsLaneContextLive(Lane))
	{
		InvalidateLane(Lane, ActivationSerial);
		OutError = TEXT("async_action_lane_stale");
		return 0;
	}

	FEntry Entry;
	Entry.Lane = Lane;
	Entry.ActivationSerial = ActivationSerial;
	Entry.RegistrationSerial = NextRegistrationSerial++;
	Entry.CallbackId = CallbackId;
	Entry.World = GetWorldForLane(Lane);
	Entry.ProducerKind = EProducerKind::AsyncAction;
	Entry.AsyncAction.Reset(&Action);
	Entry.AsyncActionPayloadTypeId = Contract.PayloadTypeId;
	const int64 Token = AllocateEntry(MoveTemp(Entry));
	if (Token == 0)
	{
		OutError = TEXT("async_action_continuation_capacity_exceeded");
		return 0;
	}

	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	check(UnpackToken(Token, SlotIndex, Generation));
	check(Slots.IsValidIndex(static_cast<int32>(SlotIndex)));
	FEntry& Stored = Slots[SlotIndex].Entry.GetValue();
#if WITH_EDITOR
	if (!ObjectsReinstancedHandle.IsValid())
	{
		ObjectsReinstancedHandle =
			FCoreUObjectDelegates::OnObjectsReinstanced.AddRaw(
				this,
				&FAvidScriptSessionContinuations::HandleObjectsReinstanced);
	}
#endif
	++PendingAsyncActionCount;
	for (const FAvidScriptBindingAsyncActionOutcomeContract& Outcome :
		Contract.Outcomes)
	{
		UFunction* BridgeFunction = nullptr;
		if (Outcome.Ordinal != Stored.AsyncActionBridges.Num()
			|| Outcome.DelegateProperty == nullptr
			|| Outcome.DelegateProperty->SignatureFunction == nullptr
			|| !PrepareAvidScriptDelegateBridgeFunction(
				Outcome.StableId,
				*Outcome.DelegateProperty->SignatureFunction,
				BridgeFunction,
				OutError))
		{
			CancelEntryProducer(Stored);
			ReleaseSlot(SlotIndex);
			if (OutError.IsEmpty())
			{
				OutError = TEXT("async_action_outcome_plan_invalid");
			}
			return 0;
		}

		const uint64 BridgeToken = AllocateAsyncActionBridgeToken();
		UAvidScriptDelegateBridge* const Bridge =
			NewObject<UAvidScriptDelegateBridge>(
				GetTransientPackage(),
				NAME_None,
				RF_Transient);
		FScriptDelegate Delegate;
		if (BridgeToken == 0 || !IsValid(Bridge))
		{
			OutError = TEXT("async_action_bridge_allocation_failed");
			CancelEntryProducer(Stored);
			ReleaseSlot(SlotIndex);
			return 0;
		}
		Bridge->Initialize(*this, BridgeToken, *BridgeFunction);
		Delegate.BindUFunction(Bridge, BridgeFunction->GetFName());
		if (!Delegate.IsBound())
		{
			Bridge->Deactivate();
			OutError = TEXT("async_action_bridge_bind_failed");
			CancelEntryProducer(Stored);
			ReleaseSlot(SlotIndex);
			return 0;
		}

		Outcome.DelegateProperty->AddDelegate(Delegate, &Action);
		Stored.AsyncActionBridges.Emplace(Bridge);
		Stored.AsyncActionDelegates.Add(Delegate);
		Stored.AsyncActionProperties.Add(Outcome.DelegateProperty);
		Stored.AsyncActionBridgeTokens.Add(BridgeToken);
		Stored.AsyncActionPayloadEncoders.Add(Outcome.PayloadEncoder);
		AsyncActionRoutes.Add(BridgeToken, { Token, Outcome.Ordinal });
	}

	CastChecked<UBlueprintAsyncActionBase>(&Action)->Activate();
	return Token;
}

void FAvidScriptSessionContinuations::HandleAvidScriptDelegateBroadcast(
	const uint64 SubscriptionToken,
	void* Parameters)
{
	if (!IsInGameThread() || bTearingDown || SubscriptionToken == 0)
	{
		return;
	}
	const FAsyncActionRoute* const Route =
		AsyncActionRoutes.Find(SubscriptionToken);
	if (Route == nullptr || Route->OutcomeOrdinal < 0)
	{
		return;
	}

	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(Route->ContinuationToken, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return;
	}
	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Generation
		|| !Slot.Entry.IsSet()
		|| Slot.Entry->ProducerKind != EProducerKind::AsyncAction
		|| Slot.Entry->bReady
		|| Slot.Entry->bDispatching)
	{
		return;
	}
	FEntry& Entry = Slot.Entry.GetValue();
	if (!IsEntryContextLive(Entry))
	{
		InvalidateLane(Entry.Lane, Entry.ActivationSerial);
		return;
	}

	FAvidScriptBindingLatentCompletionPayload Payload;
	FString CaptureError;
	const bool bHasPayloadEncoder =
		Entry.AsyncActionPayloadEncoders.IsValidIndex(
			Route->OutcomeOrdinal)
		&& Entry.AsyncActionPayloadEncoders[Route->OutcomeOrdinal].IsValid();
	bool bPayloadCaptured = false;
	if (bHasPayloadEncoder)
	{
		bPayloadCaptured =
			Entry.AsyncActionPayloadEncoders[Route->OutcomeOrdinal]->Capture(
				Parameters,
				Payload,
				CaptureError);
	}
	else
	{
		Payload.TypeId = Entry.AsyncActionPayloadTypeId;
		Payload.Kind = EAvidScriptBindingLatentPayloadKind::AbiCells;
		Payload.AbiCells.Add(static_cast<uint64>(Route->OutcomeOrdinal));
		bPayloadCaptured = true;
	}

	FAvidScriptContinuationCompletion Completion;
	Completion.CallbackId = Entry.CallbackId;
	Completion.Token = Entry.Token;
	Completion.Status = bPayloadCaptured
		? EAvidScriptContinuationStatus::Completed
		: EAvidScriptContinuationStatus::Failed;
	Completion.RegistrationSerial = Entry.RegistrationSerial;
	if (bPayloadCaptured)
	{
		FResultEntry Result;
		Result.Lane = Entry.Lane;
		Result.ActivationSerial = Entry.ActivationSerial;
		Result.ContinuationToken = Entry.Token;
		Result.Payload = MoveTemp(Payload);
		if (AllocateResult(
				MoveTemp(Result),
				Entry.ResultSlot,
				Entry.ResultGeneration))
		{
			Completion.ObjectSlot = Entry.ResultSlot;
			Completion.ObjectGeneration = Entry.ResultGeneration;
		}
		else
		{
			Completion.Status = EAvidScriptContinuationStatus::Failed;
			Entry.ResultSlot = 0;
			Entry.ResultGeneration = 0;
		}
	}
	else
	{
		Entry.ResultSlot = 0;
		Entry.ResultGeneration = 0;
		UE_LOG(
			LogAvidScriptSessionContinuations,
			Warning,
			TEXT("AsyncAction outcome %d payload capture failed: %s"),
			Route->OutcomeOrdinal,
			CaptureError.IsEmpty()
				? TEXT("async_action_payload_capture_failed")
				: *CaptureError);
	}
	Entry.bReady = true;
	ReleaseAsyncActionProducer(Entry);
	ReadyCompletions.Add(FReadyCompletion{
		Entry.Lane,
		Entry.ActivationSerial,
		Completion
	});
}

bool FAvidScriptSessionContinuations::ConsumeResult(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int64 ContinuationToken,
	const int32 SlotNumber,
	const int32 Generation,
	const FString& ExpectedTypeId,
	FAvidScriptBindingLatentCompletionPayload& OutPayload)
{
	OutPayload = FAvidScriptBindingLatentCompletionPayload();
	if (!IsInGameThread()
		|| ContinuationToken == 0
		|| SlotNumber <= 0
		|| Generation <= 0
		|| ExpectedTypeId.IsEmpty()
		|| !MatchesCurrentEndpoint(Lane, ActivationSerial))
	{
		return false;
	}
	const uint32 SlotIndex = static_cast<uint32>(SlotNumber - 1);
	if (!ResultSlots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return false;
	}
	FResultSlot& Slot = ResultSlots[SlotIndex];
	if (Slot.Generation != static_cast<uint32>(Generation)
		|| !Slot.Entry.IsSet()
		|| Slot.Entry->Lane != Lane
		|| Slot.Entry->ActivationSerial != ActivationSerial
		|| Slot.Entry->ContinuationToken != ContinuationToken
		|| Slot.Entry->Payload.TypeId != ExpectedTypeId)
	{
		return false;
	}

	uint32 ContinuationSlotIndex = 0;
	uint32 ContinuationGeneration = 0;
	if (!UnpackToken(
			ContinuationToken,
			ContinuationSlotIndex,
			ContinuationGeneration)
		|| !Slots.IsValidIndex(static_cast<int32>(ContinuationSlotIndex)))
	{
		return false;
	}
	FSlot& ContinuationSlot = Slots[ContinuationSlotIndex];
	if (ContinuationSlot.Generation != ContinuationGeneration
		|| !ContinuationSlot.Entry.IsSet()
		|| ContinuationSlot.Entry->Lane != Lane
		|| ContinuationSlot.Entry->ActivationSerial != ActivationSerial
		|| !ContinuationSlot.Entry->bDispatching
		|| ContinuationSlot.Entry->ResultSlot != SlotNumber
		|| ContinuationSlot.Entry->ResultGeneration != Generation)
	{
		return false;
	}

	OutPayload = MoveTemp(Slot.Entry->Payload);
	ContinuationSlot.Entry->ResultSlot = 0;
	ContinuationSlot.Entry->ResultGeneration = 0;
	ReleaseResultSlot(SlotIndex);
	return true;
}

int64 FAvidScriptSessionContinuations::PackToken(
	const uint32 Slot,
	const uint32 Generation)
{
	const uint64 Packed = (static_cast<uint64>(Generation) << 32)
		| static_cast<uint64>(Slot + 1);
	return static_cast<int64>(Packed);
}

bool FAvidScriptSessionContinuations::UnpackToken(
	const int64 Token,
	uint32& OutSlot,
	uint32& OutGeneration)
{
	const uint64 Packed = static_cast<uint64>(Token);
	if ((Packed & CancellationSourceKindMask) != 0)
	{
		return false;
	}
	const uint32 EncodedSlot = static_cast<uint32>(Packed & 0xffffffffu);
	OutGeneration = static_cast<uint32>(Packed >> 32);
	if (EncodedSlot == 0 || OutGeneration == 0)
	{
		return false;
	}
	OutSlot = EncodedSlot - 1;
	return true;
}

int64 FAvidScriptSessionContinuations::PackCancellationSourceToken(
	const uint32 Slot,
	const uint32 Generation)
{
	const uint64 Packed = CancellationSourceKindMask
		| (static_cast<uint64>(Generation) << 32)
		| static_cast<uint64>(Slot + 1);
	return static_cast<int64>(Packed);
}

bool FAvidScriptSessionContinuations::UnpackCancellationSourceToken(
	const int64 Token,
	uint32& OutSlot,
	uint32& OutGeneration)
{
	const uint64 Packed = static_cast<uint64>(Token);
	if ((Packed & CancellationSourceKindMask) == 0)
	{
		return false;
	}
	const uint32 EncodedSlot = static_cast<uint32>(Packed & 0xffffffffu);
	OutGeneration = static_cast<uint32>((Packed >> 32) & 0x7fffffffu);
	if (EncodedSlot == 0 || OutGeneration == 0)
	{
		return false;
	}
	OutSlot = EncodedSlot - 1;
	return true;
}

uint32 FAvidScriptSessionContinuations::AllocateGeneration()
{
	for (;;)
	{
		const uint32 Candidate = GAvidScriptContinuationGeneration.fetch_add(
			1,
			std::memory_order_relaxed) & 0x7fffffffu;
		if (Candidate != 0)
		{
			return Candidate;
		}
	}
}

int64 FAvidScriptSessionContinuations::AllocateEntry(FEntry&& Entry)
{
	uint32 SlotIndex = 0;
	if (!FreeSlots.IsEmpty())
	{
		SlotIndex = FreeSlots.Pop(EAllowShrinking::No);
	}
	else
	{
		SlotIndex = static_cast<uint32>(Slots.AddDefaulted());
		Slots[SlotIndex].Generation = AllocateGeneration();
	}

	FSlot& Slot = Slots[SlotIndex];
	check(!Slot.Entry.IsSet());
	if (Slot.Generation == 0)
	{
		Slot.Generation = AllocateGeneration();
	}
	Entry.Token = PackToken(SlotIndex, Slot.Generation);
	Slot.Entry.Emplace(MoveTemp(Entry));
	++OccupiedSlotCount;
	int32& LaneEntryCount =
		Slot.Entry->Lane == EAvidScriptContinuationLane::Active
			? ActiveEntryCount
			: PreparedEntryCount;
	++LaneEntryCount;
	return Slot.Entry->Token;
}

int64 FAvidScriptSessionContinuations::AllocateCancellationSource(
	FCancellationSourceEntry&& Entry)
{
	uint32 SlotIndex = 0;
	if (!FreeCancellationSourceSlots.IsEmpty())
	{
		SlotIndex = FreeCancellationSourceSlots.Pop(EAllowShrinking::No);
	}
	else
	{
		SlotIndex = static_cast<uint32>(CancellationSourceSlots.AddDefaulted());
		CancellationSourceSlots[SlotIndex].Generation = AllocateGeneration();
	}

	FCancellationSourceSlot& Slot = CancellationSourceSlots[SlotIndex];
	check(!Slot.Entry.IsSet());
	if (Slot.Generation == 0)
	{
		Slot.Generation = AllocateGeneration();
	}
	Entry.Token = PackCancellationSourceToken(SlotIndex, Slot.Generation);
	Slot.Entry.Emplace(MoveTemp(Entry));
	++OccupiedCancellationSourceCount;
	return Slot.Entry->Token;
}

bool FAvidScriptSessionContinuations::AllocateResult(
	FResultEntry&& Entry,
	int32& OutSlot,
	int32& OutGeneration)
{
	OutSlot = 0;
	OutGeneration = 0;
	if (OccupiedResultSlotCount >= MaximumResultSlots
		|| Entry.Payload.TypeId.IsEmpty())
	{
		return false;
	}

	const FAvidScriptBindingLatentCompletionPayload& Payload = Entry.Payload;
	bool bPayloadValid = false;
	switch (Payload.Kind)
	{
	case EAvidScriptBindingLatentPayloadKind::AbiCells:
		bPayloadValid = !Payload.AbiCells.IsEmpty()
			&& Payload.AbiCells.Num() <= MaximumResultPayloadCells;
		break;
	case EAvidScriptBindingLatentPayloadKind::FixedWire:
		bPayloadValid = !Payload.Bytes.IsEmpty()
			&& Payload.Bytes.Num() <= MaximumFixedResultBytes;
		break;
	case EAvidScriptBindingLatentPayloadKind::Object:
		bPayloadValid = Payload.ObjectValue.IsValid();
		break;
	case EAvidScriptBindingLatentPayloadKind::Utf8:
		bPayloadValid = Payload.Bytes.Num()
			<= static_cast<int32>(FAvidScriptUtf8ValueHeap::MaxValueBytes);
		break;
	case EAvidScriptBindingLatentPayloadKind::Array:
	{
		const int64 ExpectedBytes = static_cast<int64>(Payload.ElementCount)
			* static_cast<int64>(Payload.ElementStride);
		bPayloadValid = !Payload.ElementTypeId.IsEmpty()
			&& Payload.ElementCount >= 0
			&& Payload.ElementCount <= FAvidScriptArrayValueHeap::MaxElements
			&& Payload.ElementStride > 0
			&& Payload.ElementAlignment > 0
			&& ExpectedBytes == Payload.Bytes.Num()
			&& ExpectedBytes <= FAvidScriptArrayValueHeap::MaxValueBytes;
		break;
	}
	case EAvidScriptBindingLatentPayloadKind::Composite:
		bPayloadValid = IsBoundedCompositeResultPayload(Payload);
		break;
	default:
		break;
	}
	if (!bPayloadValid)
	{
		return false;
	}

	uint32 SlotIndex = 0;
	if (!FreeResultSlots.IsEmpty())
	{
		SlotIndex = FreeResultSlots.Pop(EAllowShrinking::No);
	}
	else
	{
		SlotIndex = static_cast<uint32>(ResultSlots.AddDefaulted());
		ResultSlots[SlotIndex].Generation = AllocateGeneration();
	}
	FResultSlot& Slot = ResultSlots[SlotIndex];
	check(!Slot.Entry.IsSet());
	if (Slot.Generation == 0)
	{
		Slot.Generation = AllocateGeneration();
	}
	Slot.Entry.Emplace(MoveTemp(Entry));
	++OccupiedResultSlotCount;
	OutSlot = static_cast<int32>(SlotIndex + 1);
	OutGeneration = static_cast<int32>(Slot.Generation);
	return true;
}

void FAvidScriptSessionContinuations::ReleaseSlot(const uint32 SlotIndex)
{
	FSlot& Slot = Slots[SlotIndex];
	check(Slot.Entry.IsSet());
	ReleaseEntryResult(Slot.Entry.GetValue());
	UnbindEntryFromCancellationSource(Slot.Entry.GetValue());
	int32& LaneEntryCount =
		Slot.Entry->Lane == EAvidScriptContinuationLane::Active
			? ActiveEntryCount
			: PreparedEntryCount;
	check(LaneEntryCount > 0);
	--LaneEntryCount;
	Slot.Entry.Reset();
	Slot.Generation = AllocateGeneration();
	FreeSlots.Add(SlotIndex);
	--OccupiedSlotCount;
}

void FAvidScriptSessionContinuations::ReleaseCancellationSourceSlot(
	const uint32 SlotIndex)
{
	FCancellationSourceSlot& Slot = CancellationSourceSlots[SlotIndex];
	check(Slot.Entry.IsSet());
	check(Slot.Entry->Bindings.IsEmpty());
	Slot.Entry.Reset();
	Slot.Generation = AllocateGeneration();
	FreeCancellationSourceSlots.Add(SlotIndex);
	check(OccupiedCancellationSourceCount > 0);
	--OccupiedCancellationSourceCount;
}

void FAvidScriptSessionContinuations::ReleaseResultSlot(
	const uint32 SlotIndex)
{
	FResultSlot& Slot = ResultSlots[SlotIndex];
	check(Slot.Entry.IsSet());
	Slot.Entry.Reset();
	Slot.Generation = AllocateGeneration();
	FreeResultSlots.Add(SlotIndex);
	check(OccupiedResultSlotCount > 0);
	--OccupiedResultSlotCount;
}

void FAvidScriptSessionContinuations::ReleaseEntryResult(FEntry& Entry)
{
	if (Entry.ResultSlot <= 0 || Entry.ResultGeneration <= 0)
	{
		Entry.ResultSlot = 0;
		Entry.ResultGeneration = 0;
		return;
	}
	const uint32 SlotIndex = static_cast<uint32>(Entry.ResultSlot - 1);
	if (ResultSlots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		FResultSlot& Slot = ResultSlots[SlotIndex];
		if (Slot.Generation == static_cast<uint32>(Entry.ResultGeneration)
			&& Slot.Entry.IsSet()
			&& Slot.Entry->ContinuationToken == Entry.Token)
		{
			ReleaseResultSlot(SlotIndex);
		}
	}
	Entry.ResultSlot = 0;
	Entry.ResultGeneration = 0;
}

void FAvidScriptSessionContinuations::UnbindEntryFromCancellationSource(
	FEntry& Entry)
{
	if (Entry.CancellationSourceToken == 0)
	{
		return;
	}
	uint32 SourceSlotIndex = 0;
	uint32 SourceGeneration = 0;
	if (UnpackCancellationSourceToken(
			Entry.CancellationSourceToken,
			SourceSlotIndex,
			SourceGeneration)
		&& CancellationSourceSlots.IsValidIndex(static_cast<int32>(SourceSlotIndex)))
	{
		FCancellationSourceSlot& SourceSlot =
			CancellationSourceSlots[SourceSlotIndex];
		if (SourceSlot.Generation == SourceGeneration
			&& SourceSlot.Entry.IsSet()
			&& SourceSlot.Entry->Bindings.Remove(Entry.Token) > 0)
		{
			check(CancellationBindingCount > 0);
			--CancellationBindingCount;
		}
	}
	Entry.CancellationSourceToken = 0;
}

bool FAvidScriptSessionContinuations::CancelEntry(
	const uint32 SlotIndex,
	const bool bDeliverTerminal)
{
	check(IsInGameThread());
	if (!Slots.IsValidIndex(static_cast<int32>(SlotIndex))
		|| !Slots[SlotIndex].Entry.IsSet())
	{
		return false;
	}

	FEntry& Entry = Slots[SlotIndex].Entry.GetValue();
	if (Entry.bDispatching || Entry.bCancelledTerminalQueued)
	{
		return false;
	}

	const bool bResumeOutcome = bDeliverTerminal
		&& (Entry.LatentCompletion.ResumesOutcomeOnCancel()
			|| Entry.ProducerKind == EProducerKind::AsyncAction);
	CancelEntryProducer(Entry);
	RemoveReadyToken(Entry.Token);
	if (!bResumeOutcome)
	{
		ReleaseSlot(SlotIndex);
		return true;
	}

	ReleaseEntryResult(Entry);
	UnbindEntryFromCancellationSource(Entry);
	Entry.bReady = true;
	Entry.bCancelledTerminalQueued = true;
	FAvidScriptContinuationCompletion Completion;
	Completion.CallbackId = Entry.CallbackId;
	Completion.Token = Entry.Token;
	Completion.Status = EAvidScriptContinuationStatus::Cancelled;
	Completion.RegistrationSerial = Entry.RegistrationSerial;
	ReadyCompletions.Add(FReadyCompletion{
		Entry.Lane,
		Entry.ActivationSerial,
		Completion
	});
	return true;
}

void FAvidScriptSessionContinuations::HandleTimerCompletion(const int64 Token)
{
	if (!IsInGameThread() || bTearingDown)
	{
		return;
	}
	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(Token, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return;
	}
	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Generation || !Slot.Entry.IsSet())
	{
		return;
	}
	FEntry& Entry = Slot.Entry.GetValue();
	if (!IsEntryContextLive(Entry))
	{
		InvalidateLane(Entry.Lane, Entry.ActivationSerial);
		return;
	}
	if (Entry.bReady)
	{
		return;
	}
	Entry.bReady = true;
	Entry.TimerHandle.Invalidate();
	FAvidScriptContinuationCompletion Completion;
	Completion.CallbackId = Entry.CallbackId;
	Completion.Token = Entry.Token;
	Completion.Status = EAvidScriptContinuationStatus::Completed;
	Completion.RegistrationSerial = Entry.RegistrationSerial;
	ReadyCompletions.Add(FReadyCompletion{
		Entry.Lane,
		Entry.ActivationSerial,
		Completion
	});
}

void FAvidScriptSessionContinuations::HandleObjectLoadCompletion(
	const int64 Token,
	UObject* LoadedObject)
{
	if (!IsInGameThread() || bTearingDown)
	{
		return;
	}

	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(Token, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return;
	}

	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Generation || !Slot.Entry.IsSet())
	{
		return;
	}

	FEntry& Entry = Slot.Entry.GetValue();
	if (Entry.ProducerKind != EProducerKind::AsyncObjectLoad
		|| Entry.bReady
		|| Entry.bDispatching)
	{
		return;
	}
	if (!IsEntryContextLive(Entry))
	{
		Entry.bReady = true;
		InvalidateLane(Entry.Lane, Entry.ActivationSerial);
		return;
	}

	Entry.bReady = true;
	if (IsValid(LoadedObject))
	{
		Entry.LoadedObject.Reset(LoadedObject);
	}

	FAvidScriptContinuationCompletion Completion;
	Completion.CallbackId = Entry.CallbackId;
	Completion.Token = Entry.Token;
	Completion.Status = IsValid(LoadedObject)
		? EAvidScriptContinuationStatus::Completed
		: EAvidScriptContinuationStatus::Failed;
	Completion.RegistrationSerial = Entry.RegistrationSerial;
	ReadyCompletions.Add(FReadyCompletion{
		Entry.Lane,
		Entry.ActivationSerial,
		Completion
	});
}

void FAvidScriptSessionContinuations::HandleLatentCompletion(
	const int64 Token,
	const int32 Linkage)
{
	if (!IsInGameThread() || bTearingDown)
	{
		return;
	}
	uint32 SlotIndex = 0;
	uint32 Generation = 0;
	if (!UnpackToken(Token, SlotIndex, Generation)
		|| !Slots.IsValidIndex(static_cast<int32>(SlotIndex)))
	{
		return;
	}
	FSlot& Slot = Slots[SlotIndex];
	if (Slot.Generation != Generation || !Slot.Entry.IsSet())
	{
		return;
	}

	FEntry& Entry = Slot.Entry.GetValue();
	if (Entry.ProducerKind != EProducerKind::LatentAction
		|| Entry.bReady
		|| Entry.bDispatching)
	{
		return;
	}
	if (Linkage != 0 || !IsEntryContextLive(Entry))
	{
		InvalidateLane(Entry.Lane, Entry.ActivationSerial);
		return;
	}
	if (!Entry.bLatentCommitted)
	{
		Entry.bLatentCompletionPending = true;
		return;
	}
	QueueLatentCompletion(Entry);
}

void FAvidScriptSessionContinuations::QueueLatentCompletion(FEntry& Entry)
{
	check(Entry.ProducerKind == EProducerKind::LatentAction);
	check(!Entry.bReady && !Entry.bDispatching);
	FAvidScriptContinuationCompletion Completion;
	Completion.CallbackId = Entry.CallbackId;
	Completion.Token = Entry.Token;
	Completion.Status = EAvidScriptContinuationStatus::Completed;
	Completion.RegistrationSerial = Entry.RegistrationSerial;
	if (Entry.LatentCompletion.IsProvider())
	{
		FAvidScriptBindingLatentCompletionPayload Payload;
		const UObject* const CallbackTarget = Entry.LatentProxy.Get();
		const bool bPayloadReady = IsValid(CallbackTarget)
			&& Entry.LatentCompletion.Provider->ConsumePayload(
				Entry.LatentProxy.Get(),
				1,
				Payload)
			&& Payload.TypeId == Entry.LatentCompletion.PayloadTypeId;
		FResultEntry Result;
		Result.Lane = Entry.Lane;
		Result.ActivationSerial = Entry.ActivationSerial;
		Result.ContinuationToken = Entry.Token;
		Result.Payload = MoveTemp(Payload);
		if (!bPayloadReady
			|| !AllocateResult(
				MoveTemp(Result),
				Entry.ResultSlot,
				Entry.ResultGeneration))
		{
			Entry.LatentCompletion.Provider->AbandonPayload(
				Entry.LatentProxy.Get(),
				1);
			Completion.Status = EAvidScriptContinuationStatus::Failed;
			Entry.ResultSlot = 0;
			Entry.ResultGeneration = 0;
		}
		else
		{
			Completion.ObjectSlot = Entry.ResultSlot;
			Completion.ObjectGeneration = Entry.ResultGeneration;
		}
	}
	Entry.bReady = true;
	Entry.bLatentCompletionPending = false;
	ReadyCompletions.Add(FReadyCompletion{
		Entry.Lane,
		Entry.ActivationSerial,
		Completion
	});
}

void FAvidScriptSessionContinuations::CancelEntryProducer(FEntry& Entry)
{
	if (Entry.ProducerKind == EProducerKind::AsyncAction)
	{
		ReleaseAsyncActionProducer(Entry);
		return;
	}
	if (Entry.ProducerKind == EProducerKind::Timer)
	{
		if (UWorld* const World = Entry.World.Get())
		{
			World->GetTimerManager().ClearTimer(Entry.TimerHandle);
		}
		Entry.TimerHandle.Invalidate();
		return;
	}
	if (Entry.ProducerKind == EProducerKind::LatentAction)
	{
		if (Entry.LatentCompletion.IsProvider()
			&& !Entry.bReady
			&& Entry.LatentProxy.IsValid()
			&& Entry.ResultSlot == 0)
		{
			Entry.LatentCompletion.Provider->AbandonPayload(
				Entry.LatentProxy.Get(),
				1);
		}
		RetireLatentProxy(Entry);
		return;
	}

	if (!Entry.bReady && Entry.AsyncLoadHandle)
	{
		Entry.AsyncLoadHandle->Cancel();
	}
	Entry.AsyncLoadHandle.Reset();
}

void FAvidScriptSessionContinuations::ReleaseAsyncActionProducer(
	FEntry& Entry)
{
	check(IsInGameThread());
	UObject* const Action = Entry.AsyncAction.Get();
	const int32 BoundCount = FMath::Min3(
		Entry.AsyncActionProperties.Num(),
		Entry.AsyncActionDelegates.Num(),
		Entry.AsyncActionBridges.Num());
	for (int32 Index = BoundCount - 1; Index >= 0; --Index)
	{
		if (Action != nullptr
			&& !Action->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& Entry.AsyncActionProperties[Index] != nullptr)
		{
			Entry.AsyncActionProperties[Index]->RemoveDelegate(
				Entry.AsyncActionDelegates[Index],
				Action);
		}
		if (Entry.AsyncActionBridges[Index].IsValid())
		{
			Entry.AsyncActionBridges[Index]->Deactivate();
		}
	}
	for (const uint64 BridgeToken : Entry.AsyncActionBridgeTokens)
	{
		AsyncActionRoutes.Remove(BridgeToken);
	}
	Entry.AsyncActionProperties.Reset();
	Entry.AsyncActionDelegates.Reset();
	Entry.AsyncActionBridges.Reset();
	Entry.AsyncActionBridgeTokens.Reset();
	Entry.AsyncActionPayloadEncoders.Reset();
	if (UBlueprintAsyncActionBase* const AsyncAction =
		Cast<UBlueprintAsyncActionBase>(Action))
	{
		AsyncAction->SetReadyToDestroy();
	}
	Entry.AsyncAction.Reset();
	if (Action != nullptr)
	{
		check(PendingAsyncActionCount > 0);
		--PendingAsyncActionCount;
#if WITH_EDITOR
		if (PendingAsyncActionCount == 0
			&& ObjectsReinstancedHandle.IsValid())
		{
			FCoreUObjectDelegates::OnObjectsReinstanced.Remove(
				ObjectsReinstancedHandle);
			ObjectsReinstancedHandle.Reset();
		}
#endif
	}
}

void FAvidScriptSessionContinuations::HandleObjectsReinstanced(
	const TMap<UObject*, UObject*>& ReplacementObjects)
{
	if (!IsInGameThread()
		|| bTearingDown
		|| ReplacementObjects.IsEmpty())
	{
		return;
	}

	for (int32 SlotIndex = Slots.Num() - 1; SlotIndex >= 0; --SlotIndex)
	{
		FSlot& Slot = Slots[SlotIndex];
		if (!Slot.Entry.IsSet()
			|| Slot.Entry->ProducerKind != EProducerKind::AsyncAction
			|| Slot.Entry->bReady
			|| Slot.Entry->bDispatching)
		{
			continue;
		}

		UObject* const Action = Slot.Entry->AsyncAction.Get();
		UClass* const ActionClass = Action == nullptr
			? nullptr
			: Action->GetClass();
		if ((Action != nullptr && ReplacementObjects.Contains(Action))
			|| (ActionClass != nullptr
				&& ReplacementObjects.Contains(ActionClass)))
		{
			CancelEntry(static_cast<uint32>(SlotIndex), true);
		}
	}
}

void FAvidScriptSessionContinuations::SweepInvalidAsyncActions()
{
	check(IsInGameThread());
	for (int32 SlotIndex = Slots.Num() - 1; SlotIndex >= 0; --SlotIndex)
	{
		FSlot& Slot = Slots[SlotIndex];
		if (!Slot.Entry.IsSet()
			|| Slot.Entry->ProducerKind != EProducerKind::AsyncAction
			|| Slot.Entry->bReady
			|| Slot.Entry->bDispatching)
		{
			continue;
		}

		UObject* const Action = Slot.Entry->AsyncAction.Get();
		UClass* const ActionClass = Action == nullptr
			? nullptr
			: Action->GetClass();
		if (!IsValid(Action)
			|| ActionClass == nullptr
			|| ActionClass->HasAnyClassFlags(CLASS_NewerVersionExists))
		{
			CancelEntry(static_cast<uint32>(SlotIndex), true);
		}
	}
}

uint64 FAvidScriptSessionContinuations::AllocateAsyncActionBridgeToken()
{
	for (int32 Attempt = 0; Attempt <= AsyncActionRoutes.Num(); ++Attempt)
	{
		const uint64 Candidate = NextAsyncActionBridgeToken++;
		if (Candidate != 0 && !AsyncActionRoutes.Contains(Candidate))
		{
			return Candidate;
		}
	}
	return 0;
}

void FAvidScriptSessionContinuations::RetireLatentProxy(FEntry& Entry)
{
	UAvidScriptLatentCallbackProxy* const Proxy = Entry.LatentProxy.Get();
	if (!IsValid(Proxy))
	{
		Entry.LatentProxy.Reset();
		return;
	}
	Proxy->Disarm();
	UWorld* const World = Entry.World.Get();
	if (IsValid(World))
	{
		FLatentActionManager& Manager = World->GetLatentActionManager();
		Manager.RemoveActionsForObject(Proxy);
		if (Manager.GetNumActionsForObject(Proxy) > 0)
		{
			RetiredLatentProxies.Add(FRetiredLatentProxy{
				World,
				MoveTemp(Entry.LatentProxy)
			});
			return;
		}
	}
	Entry.LatentProxy.Reset();
}

void FAvidScriptSessionContinuations::CollectRetiredLatentProxies()
{
	for (int32 Index = RetiredLatentProxies.Num() - 1; Index >= 0; --Index)
	{
		FRetiredLatentProxy& Retired = RetiredLatentProxies[Index];
		UWorld* const World = Retired.World.Get();
		UAvidScriptLatentCallbackProxy* const Proxy = Retired.Proxy.Get();
		if (!IsValid(World)
			|| !IsValid(Proxy)
			|| World->GetLatentActionManager().GetNumActionsForObject(Proxy) == 0)
		{
			RetiredLatentProxies.RemoveAtSwap(
				Index,
				1,
				EAllowShrinking::No);
		}
	}
}

void FAvidScriptSessionContinuations::CancelLane(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial)
{
	for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
	{
		FSlot& Slot = Slots[SlotIndex];
		if (!Slot.Entry.IsSet()
			|| Slot.Entry->Lane != Lane
			|| Slot.Entry->ActivationSerial != ActivationSerial)
		{
			continue;
		}
		CancelEntryProducer(Slot.Entry.GetValue());
		ReleaseSlot(static_cast<uint32>(SlotIndex));
	}
	ReleaseCancellationSourcesForLane(Lane, ActivationSerial);
}

void FAvidScriptSessionContinuations::ReleaseCancellationSourcesForLane(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial)
{
	for (int32 SlotIndex = 0; SlotIndex < CancellationSourceSlots.Num(); ++SlotIndex)
	{
		FCancellationSourceSlot& Slot = CancellationSourceSlots[SlotIndex];
		if (!Slot.Entry.IsSet()
			|| Slot.Entry->Lane != Lane
			|| Slot.Entry->ActivationSerial != ActivationSerial)
		{
			continue;
		}
		check(Slot.Entry->Bindings.IsEmpty());
		ReleaseCancellationSourceSlot(static_cast<uint32>(SlotIndex));
	}
}

void FAvidScriptSessionContinuations::InvalidateLane(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial)
{
	TSharedPtr<FAvidScriptContinuationHostEndpoint>& Endpoint =
		Lane == EAvidScriptContinuationLane::Active
			? ActiveEndpoint
			: PreparedEndpoint;
	if (!Endpoint
		|| Endpoint->GetActivationSerial() != ActivationSerial)
	{
		return;
	}
	Endpoint->Invalidate();
	CancelLane(Lane, ActivationSerial);
	RemoveReady(Lane, ActivationSerial);
}

void FAvidScriptSessionContinuations::RemoveReady(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial)
{
	ReadyCompletions.RemoveAll([Lane, ActivationSerial](const FReadyCompletion& Ready)
	{
		return Ready.Lane == Lane
			&& Ready.ActivationSerial == ActivationSerial;
	});
}

void FAvidScriptSessionContinuations::RemoveReadyToken(const int64 Token)
{
	ReadyCompletions.RemoveAll([Token](const FReadyCompletion& Ready)
	{
		return Ready.Completion.Token == Token;
	});
}

void FAvidScriptSessionContinuations::ClearRetainedLoadedObjects()
{
	RetainedLoadedObjectKeys.Reset();
	RetainedLoadedObjects.Reset();
}

bool FAvidScriptSessionContinuations::MatchesCurrentEndpoint(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial) const
{
	const TSharedPtr<FAvidScriptContinuationHostEndpoint>& Endpoint =
		Lane == EAvidScriptContinuationLane::Active
			? ActiveEndpoint
			: PreparedEndpoint;
	return Endpoint && Endpoint->GetActivationSerial() == ActivationSerial;
}

bool FAvidScriptSessionContinuations::HasLaneEntries(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial) const
{
	const int32 LaneEntryCount =
		Lane == EAvidScriptContinuationLane::Active
			? ActiveEntryCount
			: PreparedEntryCount;
	return LaneEntryCount > 0
		&& MatchesCurrentEndpoint(Lane, ActivationSerial);
}

bool FAvidScriptSessionContinuations::HasLaneCancellationSources(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial) const
{
	if (!MatchesCurrentEndpoint(Lane, ActivationSerial))
	{
		return false;
	}
	return CancellationSourceSlots.ContainsByPredicate(
		[Lane, ActivationSerial](const FCancellationSourceSlot& Slot)
		{
			return Slot.Entry.IsSet()
				&& Slot.Entry->Lane == Lane
				&& Slot.Entry->ActivationSerial == ActivationSerial;
		});
}

bool FAvidScriptSessionContinuations::IsLaneContextLive(
	const EAvidScriptContinuationLane Lane) const
{
	UWorld* const World = GetWorldForLane(Lane);
	if (!IsValid(World) || World->bIsTearingDown)
	{
		return false;
	}

	FAvidScriptObjectRegistry* const ObjectRegistry =
		Lane == EAvidScriptContinuationLane::Active
			? ActiveObjectRegistry
			: PreparedObjectRegistry;
	const FAvidScriptObjectHandle OwnerHandle =
		Lane == EAvidScriptContinuationLane::Active
			? ActiveOwnerHandle
			: PreparedOwnerHandle;
	if (!OwnerHandle.IsValid())
	{
		return true;
	}
	if (ObjectRegistry == nullptr)
	{
		return false;
	}

	FAvidScriptObjectHandleResult ResolveResult;
	UObject* const Owner = ObjectRegistry->ResolveObject(
		OwnerHandle,
		ResolveResult,
		false);
	if (!IsValid(Owner))
	{
		return false;
	}
	UWorld* const OwnerWorld = Owner->GetWorld();
	return OwnerWorld == nullptr || OwnerWorld == World;
}

bool FAvidScriptSessionContinuations::IsEntryContextLive(
	const FEntry& Entry) const
{
	return MatchesCurrentEndpoint(Entry.Lane, Entry.ActivationSerial)
		&& IsLaneContextLive(Entry.Lane)
		&& Entry.World.Get() == GetWorldForLane(Entry.Lane);
}

UWorld* FAvidScriptSessionContinuations::GetWorldForLane(
	const EAvidScriptContinuationLane Lane) const
{
	return Lane == EAvidScriptContinuationLane::Active
		? ActiveWorld.Get()
		: PreparedWorld.Get();
}
