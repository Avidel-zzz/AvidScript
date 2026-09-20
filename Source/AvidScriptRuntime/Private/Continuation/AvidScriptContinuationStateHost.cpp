#include "AvidScriptWasmRuntime.h"
#include "AvidScriptContinuationStateAbi.h"
#include "Memory/AvidScriptManagedHeap.h"

bool FAvidScriptWasmRuntimeInstance::DispatchContinuationManagedStateCall(
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
		|| !HostContext.Continuations)
		return Fail(TEXT("continuation_state_context"), TEXT("Managed continuation state requires an active invocation and continuation owner."));
	const int64 Token = Call.Int64Args[0];
	const uint32 Type = static_cast<uint32>(Call.IntArgs[0]);
	if (Token <= 0 || Type == 0)
		return Fail(TEXT("continuation_state_identity"), TEXT("Managed state requires a continuation token and concrete heap type."));
	uint8 State[AvidScript::ContinuationState::Abi::StateBytes] = {};
	if (Call.BindingId == EAvidScriptHostBindingId::ContinuationManagedStateStoreV1)
	{
		const uint64 Object = static_cast<uint64>(Call.Int64Args[1]);
		if (!ManagedHeap->IsAlive(Object, Type))
			return Fail(TEXT("continuation_state_object"), TEXT("State object is null, stale, foreign or has the wrong heap type."));
		TUniquePtr<IAvidScriptContinuationStateLease> Lease;
		const uint64 Objects[] = {Object};
		if (!CreateContinuationStateLease(MakeArrayView(Objects), Lease))
			return Fail(TEXT("continuation_state_roots"), TEXT("Unable to acquire state roots within the current heap limits."));
		for (unsigned I = 0; I < 4; ++I) State[I] = static_cast<uint8>(Type >> (I * 8));
		for (unsigned I = 0; I < 8; ++I) State[4 + I] = static_cast<uint8>(Object >> (I * 8));
		// A cancelled/stale/full continuation is a recoverable scheduling failure.
		// The temporary lease releases automatically if publication is rejected.
		OutResult.ReturnValue = HostContext.Continuations->StoreManagedState(
			Token, *this, MakeArrayView(State), MoveTemp(Lease)) ? 1 : 0;
	}
	else if (Call.BindingId == EAvidScriptHostBindingId::ContinuationManagedStateReadV1)
	{
		if (!bContinuationDispatchActive || bContinuationStateConsumed || Token != ActiveContinuationToken)
			return Fail(TEXT("continuation_state_authority"), TEXT("Only the current continuation may consume its state once."));
		if (!HostContext.Continuations->ReadManagedState(Token, *this, MakeArrayView(State)))
			return Fail(TEXT("continuation_state_unavailable"), TEXT("Managed state is absent, consumed or belongs to another Runtime lifetime."));
		bContinuationStateConsumed = true;
		uint32 StoredType = 0;
		uint64 Object = 0;
		for (unsigned I = 0; I < 4; ++I) StoredType |= uint32(State[I]) << (I * 8);
		for (unsigned I = 0; I < 8; ++I) Object |= uint64(State[4 + I]) << (I * 8);
		if (StoredType != Type || !ManagedHeap->IsAlive(Object, Type))
			return Fail(TEXT("continuation_state_type"), TEXT("Resumed state does not match the required concrete heap layout."));
		// The continuation lease survives this return until dispatch finalization.
		OutResult.ReturnValueI64 = static_cast<int64>(Object);
	}
	else return Fail(TEXT("continuation_state_import"), TEXT("Unknown managed continuation state operation."));
	OutResult.bSucceeded = true;
	return true;
}
