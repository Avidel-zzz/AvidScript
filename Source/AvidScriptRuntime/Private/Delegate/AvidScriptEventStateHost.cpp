#include "AvidScriptWasmRuntime.h"
#include "AvidScriptEventStateAbi.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/World.h"

bool FAvidScriptWasmRuntimeInstance::DispatchEventManagedStateCall(
	const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult)
{
	OutResult = {};
	auto Fail = [&OutResult](const TCHAR* Category, const TCHAR* Details)
	{
		OutResult.ErrorCategory = Category;
		OutResult.Details = Details;
		return false;
	};
	if (!IsInGameThread() || !IsLoaded() || !ManagedHeap || ManagedHeapInvocationDepth == 0
		|| !HostContext.EventSubscriptions || !HostContext.World.IsValid() || HostContext.World->bIsTearingDown)
		return Fail(TEXT("event_state_context"), TEXT("Managed event state requires an active invocation, World and subscription owner."));
	const bool bSubscribe = Call.BindingId == EAvidScriptHostBindingId::EventManagedStateSubscribeV1;
	const uint32 Type = static_cast<uint32>(Call.IntArgs[bSubscribe ? 3 : 0]);
	if (Type == 0)
		return Fail(TEXT("event_state_identity"), TEXT("Event state requires a concrete heap type."));
	uint8 State[AvidScript::EventState::Abi::StateBytes] = {};
	if (bSubscribe)
	{
		const uint64 Object = static_cast<uint64>(Call.Int64Args[0]);
		if (!ManagedHeap->IsAlive(Object, Type))
			return Fail(TEXT("event_state_object"), TEXT("Event state is null, stale, foreign or has the wrong concrete heap type."));
		TUniquePtr<IAvidScriptManagedStateLease> Lease;
		const uint64 Objects[] = {Object};
		if (!CreateManagedStateLease(MakeArrayView(Objects), Lease))
			return Fail(TEXT("event_state_roots"), TEXT("Unable to retain event state within the current heap limits."));
		for (unsigned I = 0; I < 4; ++I) State[I] = static_cast<uint8>(Type >> (I * 8));
		for (unsigned I = 0; I < 8; ++I) State[4 + I] = static_cast<uint8>(Object >> (I * 8));
		// Reuse source capability, generation, World and prepared catalog checks.
		// On rejection the temporary lease releases without publishing a partial entry.
		OutResult.ReturnValueI64 = HandleEventSubscribeInternal(
			Call.IntArgs[0], Call.IntArgs[1], Call.IntArgs[2], MakeArrayView(State), &Lease);
		if (bHasPendingHostImportFailure)
		{
			FString ImportModule, ImportName;
			ConsumePendingHostImportFailure(ImportModule, ImportName, OutResult.Details, &OutResult.ErrorCategory);
			return false;
		}
	}
	else if (Call.BindingId == EAvidScriptHostBindingId::EventManagedStateReadV1)
	{
		if (ManagedEventInvocationDepth == 0 || ManagedEventInvocationDepth != ManagedHeapInvocationDepth)
			return Fail(TEXT("event_state_authority"), TEXT("Only the current event Guest entry may read its state."));
		if (!HostContext.EventSubscriptions->ReadCurrentManagedState(*this, MakeArrayView(State)))
			return Fail(TEXT("event_state_unavailable"), TEXT("Event state is absent, consumed or belongs to another Runtime lifetime."));
		uint32 StoredType = 0;
		uint64 Object = 0;
		for (unsigned I = 0; I < 4; ++I) StoredType |= uint32(State[I]) << (I * 8);
		for (unsigned I = 0; I < 8; ++I) Object |= uint64(State[4 + I]) << (I * 8);
		if (StoredType != Type || !ManagedHeap->IsAlive(Object, Type))
			return Fail(TEXT("event_state_type"), TEXT("Event state does not match the callback's concrete heap layout."));
		// Dispatch retains the lease even if this callback unsubscribes itself.
		OutResult.ReturnValueI64 = static_cast<int64>(Object);
	}
	else return Fail(TEXT("event_state_import"), TEXT("Unknown managed event state operation."));
	OutResult.bSucceeded = true;
	return true;
}
