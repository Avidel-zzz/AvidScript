#include "Session/AvidScriptSessionInboundHandlers.h"

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptRuntimeSession.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptInboundHandlers, Log, All);

namespace
{
bool IsPreparedHandlerValid(
	const FAvidScriptPreparedDelegateEvent& Handler)
{
	const bool bKindValid = Handler.CallbackKind == TEXT("network_rpc")
		? Handler.Network.IsNetworked()
			&& Handler.RepNotifyProperty == nullptr
		: Handler.CallbackKind == TEXT("rep_notify")
			&& !Handler.Network.IsNetworked()
			&& Handler.RepNotifyProperty != nullptr;
	return bKindValid
		&& Handler.EventOrdinal < static_cast<uint32>(MAX_int32)
		&& Handler.ExpectedSourceClass != nullptr
		&& Handler.DelegateProperty == nullptr
		&& Handler.SignatureFunction != nullptr
		&& Handler.ImmutableCodecIdentity != nullptr
		&& Handler.Encode != nullptr
		&& Handler.ParameterCellCount <= FAvidScriptVmCallFrame::MaxCells;
}
} // namespace

struct FAvidScriptSessionInboundHandlers::FImpl
{
	explicit FImpl(FAvidScriptRuntimeSession& InSession)
		: Session(InSession)
	{
	}

	FAvidScriptRuntimeSession& Session;
	TMap<uint32, FAvidScriptPreparedDelegateEvent> Active;
	TMap<uint32, FAvidScriptPreparedDelegateEvent> Prepared;
	TArray<FAvidScriptFunctionHookRoute> PreparedRoutes;
	bool bDispatchEnabled = false;
};

FAvidScriptSessionInboundHandlers::FAvidScriptSessionInboundHandlers(
	FAvidScriptRuntimeSession& InSession)
	: Impl(MakeUnique<FImpl>(InSession))
{
}

FAvidScriptSessionInboundHandlers::~FAvidScriptSessionInboundHandlers()
{
	UnbindActive();
	DiscardPrepared();
}

bool FAvidScriptSessionInboundHandlers::Prepare(
	UObject* Source,
	const TConstArrayView<FAvidScriptPreparedDelegateEvent> Handlers,
	FString& OutError)
{
	check(IsInGameThread());
	DiscardPrepared();
	OutError.Reset();
	if (Handlers.IsEmpty())
	{
		return true;
	}
	if (!IsValid(Source))
	{
		OutError = TEXT("inbound_handler_source_unavailable");
		return false;
	}

	for (const FAvidScriptPreparedDelegateEvent& Handler : Handlers)
	{
		if (!IsPreparedHandlerValid(Handler)
			|| !Source->IsA(Handler.ExpectedSourceClass)
			|| Impl->Prepared.Contains(Handler.EventOrdinal))
		{
			OutError = TEXT("inbound_handler_prepared_plan_invalid");
			DiscardPrepared();
			return false;
		}
		Impl->Prepared.Add(Handler.EventOrdinal, Handler);
		Impl->PreparedRoutes.Add({
			Source,
			Handler.SignatureFunction,
			Handler.EventOrdinal
		});
	}
	return ValidatePreparedCommit(OutError);
}

bool FAvidScriptSessionInboundHandlers::ValidatePreparedCommit(
	FString& OutError)
{
	return FAvidScriptFunctionHookRegistry::ValidateReplacement(
		*this,
		Impl->PreparedRoutes,
		OutError);
}

bool FAvidScriptSessionInboundHandlers::CommitPrepared(FString& OutError)
{
	check(IsInGameThread());
	if (!FAvidScriptFunctionHookRegistry::ReplaceRoutes(
			*this,
			Impl->PreparedRoutes,
			OutError))
	{
		return false;
	}
	Impl->Active = MoveTemp(Impl->Prepared);
	Impl->PreparedRoutes.Reset();
	return true;
}

void FAvidScriptSessionInboundHandlers::DiscardPrepared()
{
	Impl->Prepared.Reset();
	Impl->PreparedRoutes.Reset();
}

void FAvidScriptSessionInboundHandlers::UnbindActive()
{
	Impl->bDispatchEnabled = false;
	FAvidScriptFunctionHookRegistry::RemoveRoutes(*this);
	Impl->Active.Reset();
}

void FAvidScriptSessionInboundHandlers::SetDispatchEnabled(
	const bool bEnabled)
{
	Impl->bDispatchEnabled = bEnabled;
}

int32 FAvidScriptSessionInboundHandlers::NumActive() const
{
	return Impl->Active.Num();
}

int32 FAvidScriptSessionInboundHandlers::NumPrepared() const
{
	return Impl->Prepared.Num();
}

void FAvidScriptSessionInboundHandlers::HandleAvidScriptInboundFunction(
	const uint32 HandlerOrdinal,
	UFunction& Function,
	void* Parameters)
{
	check(IsInGameThread());
	if (!Impl->bDispatchEnabled)
	{
		return;
	}
	const FAvidScriptPreparedDelegateEvent* const Handler =
		Impl->Active.Find(HandlerOrdinal);
	if (Handler == nullptr || Handler->SignatureFunction != &Function)
	{
		return;
	}

	FAvidScriptWasmSmokeResult Result;
	if (!Impl->Session.DispatchPreparedDelegateEvent(
			*Handler,
			Parameters,
			Result)
		&& Result.ErrorCategory != TEXT("reentrant_operation"))
	{
		UE_LOG(
			LogAvidScriptInboundHandlers,
			Warning,
			TEXT("AvidScript inbound callback failed | handler=%s | export=%s | category=%s | details=%s"),
			*Handler->StableId,
			*Handler->ExportName,
			Result.ErrorCategory.IsEmpty()
				? TEXT("unknown")
				: *Result.ErrorCategory,
			Result.ErrorMessage.IsEmpty()
				? TEXT("<none>")
				: *Result.ErrorMessage);
	}
}
