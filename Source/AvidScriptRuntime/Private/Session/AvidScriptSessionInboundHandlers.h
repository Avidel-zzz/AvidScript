#pragma once

#include "AvidScriptWasmRuntime.h"
#include "CoreMinimal.h"
#include "Network/AvidScriptFunctionHookRegistry.h"
#include "UObject/GCObject.h"

class FAvidScriptRuntimeSession;
struct FAvidScriptPreparedDelegateEvent;

class FAvidScriptSessionInboundHandlers final
	: public IAvidScriptFunctionHookSink
	, public FGCObject
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
	bool PumpDeferred(FAvidScriptWasmSmokeResult& OutResult);
	int32 NumActive() const;
	int32 NumPrepared() const;
	int32 NumDeferred() const;

	virtual EAvidScriptInboundFunctionDispatch HandleAvidScriptInboundFunction(
		uint32 HandlerOrdinal,
		UFunction& Function,
		void* Parameters) override;
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
