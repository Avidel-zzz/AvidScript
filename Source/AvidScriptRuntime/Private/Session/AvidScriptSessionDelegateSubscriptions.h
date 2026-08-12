#pragma once

#include "CoreMinimal.h"
#include "Delegate/AvidScriptDelegateBridge.h"

class FAvidScriptRuntimeSession;
struct FAvidScriptPreparedDelegateEvent;

class FAvidScriptSessionDelegateSubscriptions final
	: public IAvidScriptDelegateBridgeSink
{
public:
	explicit FAvidScriptSessionDelegateSubscriptions(
		FAvidScriptRuntimeSession& InSession);
	~FAvidScriptSessionDelegateSubscriptions();

	bool Prepare(
		UObject* Source,
		TConstArrayView<FAvidScriptPreparedDelegateEvent> Events,
		FString& OutError);
	void CommitPrepared();
	void DiscardPrepared();
	void UnbindActive();
	int32 NumActive() const;
	int32 NumPrepared() const;
	void SetDispatchEnabled(bool bEnabled);

	virtual void HandleAvidScriptDelegateBroadcast(
		uint64 SubscriptionToken,
		void* Parameters) override;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
