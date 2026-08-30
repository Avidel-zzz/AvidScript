#pragma once

#include "AvidScriptWasmRuntime.h"
#include "CoreMinimal.h"
#include "Network/AvidScriptFunctionHookRegistry.h"

class FAvidScriptRuntimeSession;
struct FAvidScriptPreparedDelegateEvent;

class FAvidScriptSessionInboundHandlers final
	: public IAvidScriptFunctionHookSink
{
public:
	explicit FAvidScriptSessionInboundHandlers(
		FAvidScriptRuntimeSession& InSession);
	~FAvidScriptSessionInboundHandlers();

	bool Prepare(
		UObject* Source,
		TConstArrayView<FAvidScriptPreparedDelegateEvent> Handlers,
		FString& OutError);
	bool ValidatePreparedCommit(FString& OutError);
	bool CommitPrepared(FString& OutError);
	void DiscardPrepared();
	void UnbindActive();
	void SetDispatchEnabled(bool bEnabled);
	int32 NumActive() const;
	int32 NumPrepared() const;

	virtual void HandleAvidScriptInboundFunction(
		uint32 HandlerOrdinal,
		UFunction& Function,
		void* Parameters) override;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
