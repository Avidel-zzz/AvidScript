#include "Continuation/AvidScriptSessionContinuations.h"

#include "Continuation/AvidScriptAsyncObjectLoader.h"
#include "Containers/StringConv.h"
#include "Engine/LatentActionManager.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

#include <atomic>

namespace
{
std::atomic<uint32> GAvidScriptContinuationGeneration{1};

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
	if (HasLaneEntries(
			EAvidScriptContinuationLane::Prepared,
			PreparedActivation)
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
	CancelEntryProducer(Slot.Entry.GetValue());
	RemoveReadyToken(Token);
	ReleaseSlot(SlotIndex);
	return true;
}

bool FAvidScriptSessionContinuations::BeginLatent(
	const EAvidScriptContinuationLane Lane,
	const uint64 ActivationSerial,
	const int32 CallbackId,
	FAvidScriptBindingLatentReservation& OutReservation)
{
	OutReservation = {};
	if (!IsInGameThread()
		|| bTearingDown
		|| CallbackId < 0
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
	const uint32 EncodedSlot = static_cast<uint32>(Packed & 0xffffffffu);
	OutGeneration = static_cast<uint32>(Packed >> 32);
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

void FAvidScriptSessionContinuations::ReleaseSlot(const uint32 SlotIndex)
{
	FSlot& Slot = Slots[SlotIndex];
	check(Slot.Entry.IsSet());
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
	Entry.bReady = true;
	Entry.bLatentCompletionPending = false;
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

void FAvidScriptSessionContinuations::CancelEntryProducer(FEntry& Entry)
{
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
		RetireLatentProxy(Entry);
		return;
	}

	if (!Entry.bReady && Entry.AsyncLoadHandle)
	{
		Entry.AsyncLoadHandle->Cancel();
	}
	Entry.AsyncLoadHandle.Reset();
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
