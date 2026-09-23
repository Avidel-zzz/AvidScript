#pragma once

#include "CoreMinimal.h"
#include "AvidScriptWasmRuntime.h"
#include "Delegate/AvidScriptDelegateBridge.h"

class FAvidScriptRuntimeSession;
struct FAvidScriptPreparedDelegateEvent;

class FAvidScriptSessionDelegateSubscriptions final
	: public IAvidScriptDelegateBridgeSink
	, public IAvidScriptEventSubscriptionHost
{
public:
	explicit FAvidScriptSessionDelegateSubscriptions(
		FAvidScriptRuntimeSession& InSession);
	~FAvidScriptSessionDelegateSubscriptions();

	bool Prepare(
		UObject* Source,
		TConstArrayView<FAvidScriptPreparedDelegateEvent> Events,
		FString& OutError,
		const FAvidScriptWasmRuntimeInstance* Runtime = nullptr);
	void CommitPrepared();
	void DiscardPrepared();
	void UnbindActive();
	int32 NumActive() const;
	int32 NumPrepared() const;
	void SetDispatchEnabled(bool bEnabled);
	virtual int64 Subscribe(
		UObject& Source,
		uint32 EventOrdinal,
		FString& OutError) override;
	virtual bool Unsubscribe(
		int64 SubscriptionToken,
		FString& OutError) override;
	virtual bool IsCurrentSource(const UObject& Source) const override;
	// Native-only state from a validated layout. Failure preserves Lease.
	int64 SubscribeManaged(UObject& Source, uint32 EventOrdinal,
		const FAvidScriptWasmRuntimeInstance& Runtime, TConstArrayView<uint8> StateBytes,
		TUniquePtr<IAvidScriptManagedStateLease>&& Lease, FString& OutError) override;
	int64 SubscribeManagedLanguage(UObject& Source, uint32 EventOrdinal,
		const FAvidScriptWasmRuntimeInstance& Runtime, TConstArrayView<uint8> StateBytes,
		TUniquePtr<IAvidScriptManagedStateLease>&& Lease, FString& OutError) override;
	EAvidScriptLanguageEventStateResult ReadLanguageManagedState(
		UObject& Source, uint32 EventOrdinal, const FAvidScriptWasmRuntimeInstance& Runtime,
		TArrayView<uint8> OutStateBytes, FString& OutError) override;
	bool ReadCurrentManagedState(const FAvidScriptWasmRuntimeInstance& Runtime,
		TArrayView<uint8> OutStateBytes) override;

	virtual void HandleAvidScriptDelegateBroadcast(
		uint64 SubscriptionToken,
		void* Parameters) override;

private:
	int64 SubscribeInternal(UObject& Source, uint32 EventOrdinal, FString& OutError,
		const FAvidScriptWasmRuntimeInstance* Runtime, TConstArrayView<uint8> StateBytes,
		TUniquePtr<IAvidScriptManagedStateLease>* Lease, bool bLanguageManaged = false);
	void SweepInvalidSources();
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
