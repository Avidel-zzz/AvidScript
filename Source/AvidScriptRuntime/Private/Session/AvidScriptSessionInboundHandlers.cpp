#include "Session/AvidScriptSessionInboundHandlers.h"

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptRuntimeSession.h"
#include "UObject/Class.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptInboundHandlers, Log, All);

namespace
{
constexpr int32 MaxDeferredInboundHandlers = 64;

bool TryResolveChainMode(
	const FString& HandlerMode,
	EAvidScriptFunctionHookChainMode& OutMode)
{
	if (HandlerMode == TEXT("replace"))
	{
		OutMode = EAvidScriptFunctionHookChainMode::Replace;
		return true;
	}
	if (HandlerMode == TEXT("before"))
	{
		OutMode = EAvidScriptFunctionHookChainMode::Before;
		return true;
	}
	if (HandlerMode == TEXT("after"))
	{
		OutMode = EAvidScriptFunctionHookChainMode::After;
		return true;
	}
	return false;
}

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
		&& Handler.Signature.Kind
			== EAvidScriptPreparedDelegateKind::FunctionHandler
		&& Handler.Signature.SinglecastProperty == nullptr
		&& Handler.Signature.MulticastProperty == nullptr
		&& Handler.Signature.SignatureFunction != nullptr
		&& Handler.Signature.ImmutableCodecIdentity != nullptr
		&& Handler.Signature.Encode != nullptr
		&& (Handler.HandlerMode == TEXT("replace")
			|| Handler.HandlerMode == TEXT("before")
			|| Handler.HandlerMode == TEXT("after"))
		&& Handler.Signature.ParameterCellCount
			<= FAvidScriptVmCallFrame::MaxCells;
}

struct FAvidScriptDeferredInboundHandler
{
	TWeakObjectPtr<UObject> Source;
	TWeakObjectPtr<UFunction> Function;
	uint32 HandlerOrdinal = MAX_uint32;
	bool bInvokeOriginalAfter = false;
	TUniquePtr<FStructOnScope> Parameters;
};
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
	TArray<FAvidScriptDeferredInboundHandler> Deferred;
	TWeakObjectPtr<UObject> ActiveSource;
	TWeakObjectPtr<UObject> PreparedSource;
	FString PendingFailureCategory;
	FString PendingFailureExport;
	FString PendingFailureDetails;
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
	Impl->PreparedSource = Source;

	for (const FAvidScriptPreparedDelegateEvent& Handler : Handlers)
	{
		EAvidScriptFunctionHookChainMode ChainMode;
		if (!IsPreparedHandlerValid(Handler)
			|| !Source->IsA(Handler.ExpectedSourceClass)
			|| Impl->Prepared.Contains(Handler.EventOrdinal)
			|| !TryResolveChainMode(Handler.HandlerMode, ChainMode))
		{
			OutError = TEXT("inbound_handler_prepared_plan_invalid");
			DiscardPrepared();
			return false;
		}
		Impl->Prepared.Add(Handler.EventOrdinal, Handler);
		Impl->PreparedRoutes.Add({
			Source,
			Handler.Signature.SignatureFunction,
			Handler.EventOrdinal,
			ChainMode
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
	Impl->ActiveSource = Impl->PreparedSource;
	Impl->PreparedSource.Reset();
	Impl->PreparedRoutes.Reset();
	return true;
}

void FAvidScriptSessionInboundHandlers::DiscardPrepared()
{
	Impl->Prepared.Reset();
	Impl->PreparedRoutes.Reset();
	Impl->PreparedSource.Reset();
}

void FAvidScriptSessionInboundHandlers::UnbindActive()
{
	Impl->bDispatchEnabled = false;
	FAvidScriptFunctionHookRegistry::RemoveRoutes(*this);
	Impl->Active.Reset();
	Impl->ActiveSource.Reset();
	Impl->Deferred.Reset();
	Impl->PendingFailureCategory.Reset();
	Impl->PendingFailureExport.Reset();
	Impl->PendingFailureDetails.Reset();
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

int32 FAvidScriptSessionInboundHandlers::NumDeferred() const
{
	return Impl->Deferred.Num();
}

EAvidScriptInboundFunctionDispatch
FAvidScriptSessionInboundHandlers::HandleAvidScriptInboundFunction(
	const uint32 HandlerOrdinal,
	UFunction& Function,
	void* Parameters)
{
	check(IsInGameThread());
	if (!Impl->bDispatchEnabled)
	{
		return EAvidScriptInboundFunctionDispatch::Unavailable;
	}
	const FAvidScriptPreparedDelegateEvent* const Handler =
		Impl->Active.Find(HandlerOrdinal);
	if (Handler == nullptr
		|| Handler->Signature.SignatureFunction != &Function)
	{
		return EAvidScriptInboundFunctionDispatch::Unavailable;
	}

	FAvidScriptWasmSmokeResult Result;
	if (Impl->Session.DispatchPreparedDelegateEvent(
			*Handler,
			Parameters,
			Result))
	{
		return EAvidScriptInboundFunctionDispatch::Handled;
	}
	if (Result.ErrorCategory == TEXT("reentrant_operation"))
	{
		if (Impl->Deferred.Num() >= MaxDeferredInboundHandlers)
		{
			Impl->bDispatchEnabled = false;
			Impl->Deferred.Reset();
			Impl->PendingFailureCategory =
				TEXT("inbound_handler_deferred_overflow");
			Impl->PendingFailureExport = Handler->ExportName;
			Impl->PendingFailureDetails = FString::Printf(
				TEXT("The bounded inbound handler queue exceeded %d entries."),
				MaxDeferredInboundHandlers);
			return EAvidScriptInboundFunctionDispatch::Failed;
		}
		UObject* const Source = Impl->ActiveSource.Get();
		if (!IsValid(Source)
			|| (Function.ParmsSize > 0 && Parameters == nullptr))
		{
			return EAvidScriptInboundFunctionDispatch::Failed;
		}

		TUniquePtr<FStructOnScope> Snapshot =
			MakeUnique<FStructOnScope>(&Function);
		if (!Snapshot->IsValid())
		{
			return EAvidScriptInboundFunctionDispatch::Failed;
		}
		for (TFieldIterator<FProperty> It(&Function); It; ++It)
		{
			FProperty* const Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Parm)
				&& !Property->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				Property->CopyCompleteValue_InContainer(
					Snapshot->GetStructMemory(),
					Parameters);
			}
		}

		FAvidScriptDeferredInboundHandler& Deferred =
			Impl->Deferred.AddDefaulted_GetRef();
		Deferred.Source = Source;
		Deferred.Function = &Function;
		Deferred.HandlerOrdinal = HandlerOrdinal;
		Deferred.bInvokeOriginalAfter = Handler->HandlerMode == TEXT("before");
		Deferred.Parameters = MoveTemp(Snapshot);
		return EAvidScriptInboundFunctionDispatch::Deferred;
	}

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
	return EAvidScriptInboundFunctionDispatch::Failed;
}

bool FAvidScriptSessionInboundHandlers::PumpDeferred(
	FAvidScriptWasmSmokeResult& OutResult)
{
	check(IsInGameThread());
	if (!Impl->PendingFailureCategory.IsEmpty())
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.ModuleId = Impl->Session.GetLiveModuleId();
		OutResult.ExportName = Impl->PendingFailureExport;
		OutResult.ErrorCategory = Impl->PendingFailureCategory;
		OutResult.NextAction =
			TEXT("reduce synchronous inbound recursion or split work across ticks");
		OutResult.ErrorMessage = Impl->PendingFailureDetails;
		Impl->PendingFailureCategory.Reset();
		Impl->PendingFailureExport.Reset();
		Impl->PendingFailureDetails.Reset();
		return false;
	}
	if (!Impl->bDispatchEnabled || Impl->Deferred.IsEmpty())
	{
		return true;
	}

	const int32 DrainCount = FMath::Min(
		Impl->Deferred.Num(),
		MaxDeferredInboundHandlers);
	for (int32 Index = 0; Index < DrainCount; ++Index)
	{
		FAvidScriptDeferredInboundHandler Deferred =
			MoveTemp(Impl->Deferred[0]);
		Impl->Deferred.RemoveAt(0, 1, EAllowShrinking::No);
		UObject* const Source = Deferred.Source.Get();
		UFunction* const Function = Deferred.Function.Get();
		const FAvidScriptPreparedDelegateEvent* const Handler =
			Impl->Active.Find(Deferred.HandlerOrdinal);
		if (!IsValid(Source)
			|| !IsValid(Function)
			|| Handler == nullptr
			|| Handler->Signature.SignatureFunction != Function
			|| !Deferred.Parameters.IsValid()
			|| !Deferred.Parameters->IsValid())
		{
			continue;
		}
		if (!Impl->Session.DispatchPreparedDelegateEvent(
				*Handler,
				Deferred.Parameters->GetStructMemory(),
				OutResult))
		{
			return false;
		}
		if (Deferred.bInvokeOriginalAfter)
		{
			FString Error;
			if (!FAvidScriptFunctionHookRegistry::InvokeOriginal(
					*this,
					*Source,
					*Function,
					Deferred.HandlerOrdinal,
					Deferred.Parameters->GetStructMemory(),
					Error))
			{
				OutResult = FAvidScriptWasmSmokeResult();
				OutResult.ModuleId = Impl->Session.GetLiveModuleId();
				OutResult.ExportName = Handler->ExportName;
				OutResult.ErrorCategory =
					TEXT("inbound_handler_original_unavailable");
				OutResult.NextAction =
					TEXT("reload the current binding package after the active route is stable");
				OutResult.ErrorMessage = Error;
				return false;
			}
		}
	}
	return true;
}

void FAvidScriptSessionInboundHandlers::AddReferencedObjects(
	FReferenceCollector& Collector)
{
	for (FAvidScriptDeferredInboundHandler& Deferred : Impl->Deferred)
	{
		if (Deferred.Parameters.IsValid())
		{
			Deferred.Parameters->AddReferencedObjects(Collector);
		}
	}
}

FString FAvidScriptSessionInboundHandlers::GetReferencerName() const
{
	return TEXT("FAvidScriptSessionInboundHandlers");
}
