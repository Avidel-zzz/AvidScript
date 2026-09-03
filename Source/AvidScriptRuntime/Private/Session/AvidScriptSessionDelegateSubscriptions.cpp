#include "Session/AvidScriptSessionDelegateSubscriptions.h"

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptDelegateSubscriptions, Log, All);

namespace
{
constexpr int32 AvidScriptMaximumExplicitDelegateSubscriptions = 4096;

struct FAvidScriptDelegateSubscriptionEntry
{
	FAvidScriptPreparedDelegateEvent Event;
	TWeakObjectPtr<UObject> Source;
	TStrongObjectPtr<UAvidScriptDelegateBridge> Bridge;
	FScriptDelegate Delegate;
	FScriptDelegate PreviousSinglecastDelegate;
	uint64 BridgeToken = 0;
	int64 GuestToken = 0;
	bool bBound = false;
};

bool IsPreparedEventValid(const FAvidScriptPreparedDelegateEvent& Event)
{
	return Event.EventOrdinal < static_cast<uint32>(MAX_int32)
		&& Event.ExpectedSourceClass != nullptr
		&& (Event.Signature.Kind
				== EAvidScriptPreparedDelegateKind::Singlecast
			? Event.Signature.SinglecastProperty != nullptr
			: Event.Signature.Kind
					== EAvidScriptPreparedDelegateKind::Multicast
				? Event.Signature.MulticastProperty != nullptr
				: false)
		&& Event.Signature.SignatureFunction != nullptr
		&& Event.Signature.ImmutableCodecIdentity != nullptr
		&& Event.Signature.Encode != nullptr
		&& Event.Signature.ParameterCellCount
			<= FAvidScriptVmCallFrame::MaxCells;
}

bool InitializeEntry(
	IAvidScriptDelegateBridgeSink& Sink,
	UObject& Source,
	const FAvidScriptPreparedDelegateEvent& Event,
	const uint64 BridgeToken,
	const int64 GuestToken,
	FAvidScriptDelegateSubscriptionEntry& OutEntry,
	FString& OutError)
{
	if (!IsPreparedEventValid(Event))
	{
		OutError = TEXT("delegate_prepared_plan_invalid");
		return false;
	}
	if (!Source.IsA(Event.ExpectedSourceClass))
	{
		OutError = TEXT("delegate_source_type_mismatch");
		return false;
	}

	UFunction* BridgeFunction = nullptr;
	if (!PrepareAvidScriptDelegateBridgeFunction(
			Event.StableId,
			*Event.Signature.SignatureFunction,
			BridgeFunction,
			OutError))
	{
		return false;
	}

	OutEntry = FAvidScriptDelegateSubscriptionEntry();
	OutEntry.Event = Event;
	OutEntry.Source = &Source;
	OutEntry.BridgeToken = BridgeToken;
	OutEntry.GuestToken = GuestToken;
	OutEntry.Bridge.Reset(NewObject<UAvidScriptDelegateBridge>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient));
	if (!OutEntry.Bridge.IsValid())
	{
		OutError = TEXT("delegate_bridge_allocation_failed");
		return false;
	}
	OutEntry.Bridge->Initialize(Sink, BridgeToken, *BridgeFunction);
	OutEntry.Delegate.BindUFunction(
		OutEntry.Bridge.Get(),
		BridgeFunction->GetFName());
	if (!OutEntry.Delegate.IsBound())
	{
		OutError = TEXT("delegate_bridge_bind_failed");
		OutEntry.Bridge->Deactivate();
		return false;
	}
	return true;
}

void BindEntry(FAvidScriptDelegateSubscriptionEntry& Entry)
{
	check(Entry.Source.IsValid());
	if (Entry.Event.Signature.Kind
		== EAvidScriptPreparedDelegateKind::Multicast)
	{
		Entry.Event.Signature.MulticastProperty->AddDelegate(
			Entry.Delegate,
			Entry.Source.Get());
	}
	else
	{
		FScriptDelegate* const Current =
			Entry.Event.Signature.SinglecastProperty
				->GetPropertyValuePtr_InContainer(Entry.Source.Get());
		check(Current != nullptr);
		Entry.PreviousSinglecastDelegate = *Current;
		*Current = Entry.Delegate;
	}
	Entry.bBound = true;
}

void UnbindEntry(FAvidScriptDelegateSubscriptionEntry& Entry)
{
	if (Entry.bBound && Entry.Source.IsValid())
	{
		if (Entry.Event.Signature.Kind
			== EAvidScriptPreparedDelegateKind::Multicast)
		{
			Entry.Event.Signature.MulticastProperty->RemoveDelegate(
				Entry.Delegate,
				Entry.Source.Get());
		}
		else if (FScriptDelegate* const Current =
			Entry.Event.Signature.SinglecastProperty
				->GetPropertyValuePtr_InContainer(Entry.Source.Get());
			Current != nullptr && *Current == Entry.Delegate)
		{
			*Current = Entry.PreviousSinglecastDelegate;
		}
	}
	Entry.bBound = false;
	if (Entry.Bridge.IsValid())
	{
		Entry.Bridge->Deactivate();
	}
}

void UnbindEntries(TArray<FAvidScriptDelegateSubscriptionEntry>& Entries)
{
	check(IsInGameThread());
	for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
	{
		UnbindEntry(Entries[Index]);
	}
	Entries.Reset();
}

int32 CountExplicitEntries(
	TConstArrayView<FAvidScriptDelegateSubscriptionEntry> Entries)
{
	int32 Count = 0;
	for (const FAvidScriptDelegateSubscriptionEntry& Entry : Entries)
	{
		Count += Entry.GuestToken != 0 ? 1 : 0;
	}
	return Count;
}
} // namespace

struct FAvidScriptSessionDelegateSubscriptions::FImpl
{
	explicit FImpl(FAvidScriptRuntimeSession& InSession)
		: Session(InSession)
	{
	}

	FAvidScriptRuntimeSession& Session;
	TArray<FAvidScriptDelegateSubscriptionEntry> Active;
	TArray<FAvidScriptDelegateSubscriptionEntry> Prepared;
	TMap<uint32, FAvidScriptPreparedDelegateEvent> ActiveCatalog;
	TMap<uint32, FAvidScriptPreparedDelegateEvent> PreparedCatalog;
	TMap<uint64, int32> ActiveBridgeIndices;
	TWeakObjectPtr<UObject> CurrentSource;
	uint64 NextBridgeToken = 1;
	uint64 NextGuestToken = 1;
	bool bPreparing = false;
	bool bDispatchEnabled = false;

	uint64 AllocateBridgeToken()
	{
		for (int32 Attempt = 0;
			Attempt <= Active.Num() + Prepared.Num();
			++Attempt)
		{
			const uint64 Token = NextBridgeToken++;
			if (Token != 0
				&& !Active.ContainsByPredicate(
					[Token](const FAvidScriptDelegateSubscriptionEntry& Entry)
					{
						return Entry.BridgeToken == Token;
					})
				&& !Prepared.ContainsByPredicate(
					[Token](const FAvidScriptDelegateSubscriptionEntry& Entry)
					{
						return Entry.BridgeToken == Token;
					}))
			{
				return Token;
			}
		}
		return 0;
	}

	int64 AllocateGuestToken()
	{
		for (int32 Attempt = 0;
			Attempt <= Active.Num() + Prepared.Num();
			++Attempt)
		{
			if (NextGuestToken == 0
				|| NextGuestToken > static_cast<uint64>(MAX_int64))
			{
				NextGuestToken = 1;
			}
			const int64 Token = static_cast<int64>(NextGuestToken++);
			if (!Active.ContainsByPredicate(
					[Token](const FAvidScriptDelegateSubscriptionEntry& Entry)
					{
						return Entry.GuestToken == Token;
					})
				&& !Prepared.ContainsByPredicate(
					[Token](const FAvidScriptDelegateSubscriptionEntry& Entry)
					{
						return Entry.GuestToken == Token;
					}))
			{
				return Token;
			}
		}
		return 0;
	}

	void RebuildActiveBridgeIndices()
	{
		ActiveBridgeIndices.Reset();
		ActiveBridgeIndices.Reserve(Active.Num());
		for (int32 Index = 0; Index < Active.Num(); ++Index)
		{
			ActiveBridgeIndices.Add(Active[Index].BridgeToken, Index);
		}
	}
};

FAvidScriptSessionDelegateSubscriptions::
	FAvidScriptSessionDelegateSubscriptions(
		FAvidScriptRuntimeSession& InSession)
	: Impl(MakeUnique<FImpl>(InSession))
{
}

FAvidScriptSessionDelegateSubscriptions::
	~FAvidScriptSessionDelegateSubscriptions()
{
	UnbindActive();
	DiscardPrepared();
}

bool FAvidScriptSessionDelegateSubscriptions::Prepare(
	UObject* Source,
	const TConstArrayView<FAvidScriptPreparedDelegateEvent> Events,
	FString& OutError)
{
	check(IsInGameThread());
	DiscardPrepared();
	OutError.Reset();
	Impl->bPreparing = true;
	if (Events.IsEmpty())
	{
		return true;
	}
	if (!IsValid(Source))
	{
		OutError = TEXT("delegate_source_unavailable");
		DiscardPrepared();
		return false;
	}

	for (const FAvidScriptPreparedDelegateEvent& Event : Events)
	{
		if (!IsPreparedEventValid(Event)
			|| Impl->PreparedCatalog.Contains(Event.EventOrdinal))
		{
			OutError = TEXT("delegate_prepared_plan_invalid");
			DiscardPrepared();
			return false;
		}
		Impl->PreparedCatalog.Add(Event.EventOrdinal, Event);
		if (Event.Signature.Kind
			== EAvidScriptPreparedDelegateKind::Singlecast)
		{
			continue;
		}
		if (!Source->IsA(Event.ExpectedSourceClass))
		{
			continue;
		}

		FAvidScriptDelegateSubscriptionEntry& Entry =
			Impl->Prepared.AddDefaulted_GetRef();
		const uint64 BridgeToken = Impl->AllocateBridgeToken();
		if (BridgeToken == 0
			|| !InitializeEntry(
				*this,
				*Source,
				Event,
				BridgeToken,
				0,
				Entry,
				OutError))
		{
			DiscardPrepared();
			return false;
		}
	}
	return true;
}

void FAvidScriptSessionDelegateSubscriptions::CommitPrepared()
{
	check(IsInGameThread());
	UnbindEntries(Impl->Active);
	Impl->Active = MoveTemp(Impl->Prepared);
	Impl->ActiveCatalog = MoveTemp(Impl->PreparedCatalog);
	Impl->RebuildActiveBridgeIndices();
	for (FAvidScriptDelegateSubscriptionEntry& Entry : Impl->Active)
	{
		BindEntry(Entry);
	}
	Impl->bPreparing = false;
}

void FAvidScriptSessionDelegateSubscriptions::DiscardPrepared()
{
	UnbindEntries(Impl->Prepared);
	Impl->PreparedCatalog.Reset();
	Impl->bPreparing = false;
}

void FAvidScriptSessionDelegateSubscriptions::UnbindActive()
{
	Impl->bDispatchEnabled = false;
	Impl->CurrentSource.Reset();
	UnbindEntries(Impl->Active);
	Impl->ActiveCatalog.Reset();
	Impl->ActiveBridgeIndices.Reset();
}

int32 FAvidScriptSessionDelegateSubscriptions::NumActive() const
{
	return Impl->Active.Num();
}

int32 FAvidScriptSessionDelegateSubscriptions::NumPrepared() const
{
	return Impl->Prepared.Num();
}

void FAvidScriptSessionDelegateSubscriptions::SetDispatchEnabled(
	const bool bEnabled)
{
	Impl->bDispatchEnabled = bEnabled;
}

int64 FAvidScriptSessionDelegateSubscriptions::Subscribe(
	UObject& Source,
	const uint32 EventOrdinal,
	FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	TArray<FAvidScriptDelegateSubscriptionEntry>& Entries =
		Impl->bPreparing ? Impl->Prepared : Impl->Active;
	const TMap<uint32, FAvidScriptPreparedDelegateEvent>& Catalog =
		Impl->bPreparing ? Impl->PreparedCatalog : Impl->ActiveCatalog;
	if (!Impl->bPreparing && !Impl->bDispatchEnabled)
	{
		OutError = TEXT("delegate_session_inactive");
		return 0;
	}
	if (CountExplicitEntries(Entries)
		>= AvidScriptMaximumExplicitDelegateSubscriptions)
	{
		OutError = TEXT("delegate_subscription_limit_reached");
		return 0;
	}
	const FAvidScriptPreparedDelegateEvent* Event = Catalog.Find(EventOrdinal);
	if (Event == nullptr)
	{
		OutError = TEXT("delegate_event_unavailable");
		return 0;
	}
	if (Event->Signature.Kind
			== EAvidScriptPreparedDelegateKind::Singlecast
		&& Entries.ContainsByPredicate(
			[&Source, EventOrdinal](
				const FAvidScriptDelegateSubscriptionEntry& Entry)
			{
				return Entry.Event.EventOrdinal == EventOrdinal
					&& Entry.Source.Get() == &Source;
			}))
	{
		OutError = TEXT("delegate_singlecast_already_bound");
		return 0;
	}

	const int64 GuestToken = Impl->AllocateGuestToken();
	if (GuestToken <= 0)
	{
		OutError = TEXT("delegate_subscription_token_exhausted");
		return 0;
	}

	FAvidScriptDelegateSubscriptionEntry Entry;
	const uint64 BridgeToken = Impl->AllocateBridgeToken();
	if (BridgeToken == 0
		|| !InitializeEntry(
			*this,
			Source,
			*Event,
			BridgeToken,
			GuestToken,
			Entry,
			OutError))
	{
		return 0;
	}
	if (!Impl->bPreparing)
	{
		BindEntry(Entry);
	}
	Entries.Add(MoveTemp(Entry));
	if (!Impl->bPreparing)
	{
		Impl->ActiveBridgeIndices.Add(BridgeToken, Entries.Num() - 1);
	}
	return GuestToken;
}

bool FAvidScriptSessionDelegateSubscriptions::Unsubscribe(
	const int64 SubscriptionToken,
	FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (SubscriptionToken <= 0)
	{
		OutError = TEXT("delegate_subscription_token_invalid");
		return false;
	}
	TArray<FAvidScriptDelegateSubscriptionEntry>& Entries =
		Impl->bPreparing ? Impl->Prepared : Impl->Active;
	const int32 Index = Entries.IndexOfByPredicate(
		[SubscriptionToken](
			const FAvidScriptDelegateSubscriptionEntry& Entry)
		{
			return Entry.GuestToken == SubscriptionToken;
		});
	if (Index == INDEX_NONE)
	{
		OutError = TEXT("delegate_subscription_token_stale");
		return false;
	}
	const uint64 RemovedBridgeToken = Entries[Index].BridgeToken;
	UnbindEntry(Entries[Index]);
	Entries.RemoveAtSwap(Index, 1, EAllowShrinking::No);
	if (!Impl->bPreparing)
	{
		Impl->ActiveBridgeIndices.Remove(RemovedBridgeToken);
		if (Entries.IsValidIndex(Index))
		{
			Impl->ActiveBridgeIndices.FindOrAdd(
				Entries[Index].BridgeToken) = Index;
		}
	}
	return true;
}

bool FAvidScriptSessionDelegateSubscriptions::IsCurrentSource(const UObject& Source) const
{
	return IsInGameThread() && Impl->bDispatchEnabled
		&& Impl->CurrentSource.IsValid() && Impl->CurrentSource.Get() == &Source;
}

void FAvidScriptSessionDelegateSubscriptions::
	HandleAvidScriptDelegateBroadcast(
		const uint64 SubscriptionToken,
		void* Parameters)
{
	check(IsInGameThread());
	if (!Impl->bDispatchEnabled)
	{
		return;
	}
	const int32* const EntryIndex =
		Impl->ActiveBridgeIndices.Find(SubscriptionToken);
	if (EntryIndex == nullptr
		|| !Impl->Active.IsValidIndex(*EntryIndex)
		|| Impl->Active[*EntryIndex].BridgeToken != SubscriptionToken)
	{
		return;
	}
	const FAvidScriptPreparedDelegateEvent Event =
		Impl->Active[*EntryIndex].Event;
	const TWeakObjectPtr<UObject> Source = Impl->Active[*EntryIndex].Source;
	if (!Source.IsValid())
	{
		return;
	}
	// Keep callback identity independent of entries that a handler may cancel or replace.
	TGuardValue<TWeakObjectPtr<UObject>> SourceGuard(Impl->CurrentSource, Source);

	FAvidScriptWasmSmokeResult Result;
	if (!Impl->Session.DispatchPreparedDelegateEvent(
			Event,
			Parameters,
			Result)
		&& Result.ErrorCategory != TEXT("reentrant_operation"))
	{
		UE_LOG(
			LogAvidScriptDelegateSubscriptions,
			Warning,
			TEXT("AvidScript delegate callback failed | event=%s | export=%s | category=%s | details=%s"),
			*Event.StableId,
			*Event.ExportName,
			Result.ErrorCategory.IsEmpty() ? TEXT("unknown") : *Result.ErrorCategory,
			Result.ErrorMessage.IsEmpty() ? TEXT("<none>") : *Result.ErrorMessage);
	}
}
