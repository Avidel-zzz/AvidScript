#include "Session/AvidScriptSessionDelegateSubscriptions.h"

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptDelegateSubscriptions, Log, All);

namespace
{
struct FAvidScriptDelegateSubscriptionEntry
{
	FAvidScriptPreparedDelegateEvent Event;
	TWeakObjectPtr<UObject> Source;
	TStrongObjectPtr<UAvidScriptDelegateBridge> Bridge;
	FScriptDelegate Delegate;
	uint64 Token = 0;
	bool bBound = false;
};

void UnbindEntries(TArray<FAvidScriptDelegateSubscriptionEntry>& Entries)
{
	check(IsInGameThread());
	for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
	{
		FAvidScriptDelegateSubscriptionEntry& Entry = Entries[Index];
		if (Entry.bBound
			&& Entry.Event.DelegateProperty != nullptr
			&& Entry.Source.IsValid())
		{
			Entry.Event.DelegateProperty->RemoveDelegate(
				Entry.Delegate,
				Entry.Source.Get());
		}
		Entry.bBound = false;
		if (Entry.Bridge.IsValid())
		{
			Entry.Bridge->Deactivate();
		}
	}
	Entries.Reset();
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
	uint32 Generation = 1;
	bool bDispatchEnabled = false;
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
	if (Events.IsEmpty())
	{
		return true;
	}
	if (!IsValid(Source))
	{
		OutError = TEXT("delegate_source_unavailable");
		return false;
	}

	for (const FAvidScriptPreparedDelegateEvent& Event : Events)
	{
		if (Event.EventOrdinal >= static_cast<uint32>(MAX_int32)
			|| Event.ExpectedSourceClass == nullptr
			|| !Source->IsA(Event.ExpectedSourceClass)
			|| Event.DelegateProperty == nullptr
			|| Event.SignatureFunction == nullptr
			|| Event.ImmutableCodecIdentity == nullptr
			|| Event.Encode == nullptr
			|| Event.ParameterCellCount > FAvidScriptVmCallFrame::MaxCells)
		{
			OutError = TEXT("delegate_prepared_plan_invalid");
			DiscardPrepared();
			return false;
		}

		UFunction* BridgeFunction = nullptr;
		if (!PrepareAvidScriptDelegateBridgeFunction(
				Event.StableId,
				*Event.SignatureFunction,
				BridgeFunction,
				OutError))
		{
			DiscardPrepared();
			return false;
		}

		FAvidScriptDelegateSubscriptionEntry& Entry =
			Impl->Prepared.AddDefaulted_GetRef();
		Entry.Event = Event;
		Entry.Source = Source;
		Entry.Token =
			(static_cast<uint64>(Impl->Generation) << 32)
			| static_cast<uint64>(Event.EventOrdinal + 1);
		Entry.Bridge.Reset(NewObject<UAvidScriptDelegateBridge>(
			GetTransientPackage(),
			NAME_None,
			RF_Transient));
		if (!Entry.Bridge.IsValid())
		{
			OutError = TEXT("delegate_bridge_allocation_failed");
			DiscardPrepared();
			return false;
		}
		Entry.Bridge->Initialize(*this, Entry.Token, *BridgeFunction);
		Entry.Delegate.BindUFunction(Entry.Bridge.Get(), BridgeFunction->GetFName());
		if (!Entry.Delegate.IsBound())
		{
			OutError = TEXT("delegate_bridge_bind_failed");
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
	for (FAvidScriptDelegateSubscriptionEntry& Entry : Impl->Active)
	{
		UObject* const Source = Entry.Source.Get();
		check(Source != nullptr);
		Entry.Event.DelegateProperty->AddDelegate(
			Entry.Delegate,
			Source);
		Entry.bBound = true;
	}
	++Impl->Generation;
	if (Impl->Generation == 0)
	{
		Impl->Generation = 1;
	}
}

void FAvidScriptSessionDelegateSubscriptions::DiscardPrepared()
{
	UnbindEntries(Impl->Prepared);
}

void FAvidScriptSessionDelegateSubscriptions::UnbindActive()
{
	Impl->bDispatchEnabled = false;
	UnbindEntries(Impl->Active);
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
	const FAvidScriptDelegateSubscriptionEntry* const Entry =
		Impl->Active.FindByPredicate(
			[SubscriptionToken](
				const FAvidScriptDelegateSubscriptionEntry& Candidate)
			{
				return Candidate.Token == SubscriptionToken;
			});
	if (Entry == nullptr)
	{
		return;
	}

	FAvidScriptWasmSmokeResult Result;
	if (!Impl->Session.DispatchPreparedDelegateEvent(
			Entry->Event,
			Parameters,
			Result)
		&& Result.ErrorCategory != TEXT("reentrant_operation"))
	{
		UE_LOG(
			LogAvidScriptDelegateSubscriptions,
			Warning,
			TEXT("AvidScript delegate callback failed | event=%s | export=%s | category=%s | details=%s"),
			*Entry->Event.StableId,
			*Entry->Event.ExportName,
			Result.ErrorCategory.IsEmpty() ? TEXT("unknown") : *Result.ErrorCategory,
			Result.ErrorMessage.IsEmpty() ? TEXT("<none>") : *Result.ErrorMessage);
	}
}
