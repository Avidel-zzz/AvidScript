#include "AvidScriptWasmRuntime.h"
#include "Memory/AvidScriptManagedHeapProtocol.h"
#include "Memory/AvidScriptManagedRootTransfer.h"

uint64 FAvidScriptWasmRuntimeInstance::BeginVmInvocation()
{
	const uint32 Depth = ManagedHeap ? ManagedHeap->GetStats().ActiveFrames : 0;
	const uint64 Token = (uint64(ManagedHeapFrameFloor) << 32) | Depth;
	ManagedHeapFrameFloor = Depth;
	++ManagedHeapInvocationDepth;
	return Token;
}

void FAvidScriptWasmRuntimeInstance::EndVmInvocation(uint64 Token)
{
	if (ManagedHeap && ManagedHeapInvocationDepth > 0)
	{
		// Closed/unconfigured heaps have no live frames. Pop is forbidden below
		// the current entry floor, so a nested invocation cannot unwind its caller.
		ManagedHeap->UnwindToDepth(static_cast<uint32>(Token));
		ManagedHeapFrameFloor = static_cast<uint32>(Token >> 32);
		--ManagedHeapInvocationDepth;
	}
}

bool FAvidScriptWasmRuntimeInstance::DispatchManagedHeapCall(
	const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult)
{
	OutResult = FAvidScriptHostCallResult();
	if (!ManagedHeap || ManagedHeapInvocationDepth == 0)
	{
		OutResult.ErrorCategory = TEXT("managed_heap_invocation");
		OutResult.Details = TEXT("Managed heap commands require an active export invocation; WASM start functions cannot access this ABI.");
		return false;
	}
	std::span<const AvidScript::Managed::FToken> ReturnedRoots;
	if (ActiveRootTransfer && ActiveRootTransfer->InvocationDepth == ManagedHeapInvocationDepth
		&& ActiveRootTransfer->FrameFloor == ManagedHeapFrameFloor)
		ReturnedRoots = {ActiveRootTransfer->Roots.GetData(), static_cast<size_t>(ActiveRootTransfer->Roots.Num())};
	const auto Result = AvidScript::Managed::ExecuteHeapCommand(*ManagedHeap,
		{Call.InputBytes.GetData(), static_cast<size_t>(Call.InputBytes.Num())},
		{Call.OutputBytes.GetData(), static_cast<size_t>(Call.OutputBytes.Num())}, ManagedHeapFrameFloor, ReturnedRoots);
	OutResult.bSucceeded = Result.Succeeded();
	OutResult.ReturnValue = Result.Succeeded() ? 1 : 0;
	if (!Result.Succeeded())
	{
		OutResult.ErrorCategory = Result.Error == AvidScript::Managed::EHeapProtocolError::HeapFailure
			? TEXT("managed_heap_rejected") : TEXT("managed_heap_protocol");
		OutResult.Details = FString::Printf(TEXT("Managed heap v1: %s / %s"),
			UTF8_TO_TCHAR(AvidScript::Managed::HeapProtocolErrorName(Result.Error)),
			UTF8_TO_TCHAR(AvidScript::Managed::HeapErrorName(Result.HeapError)));
	}
	return OutResult.bSucceeded;
}
