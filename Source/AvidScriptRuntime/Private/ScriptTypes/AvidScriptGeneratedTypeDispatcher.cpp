#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"

#include "HAL/PlatformProcess.h"

#include <atomic>

namespace
{
std::atomic<IAvidScriptGeneratedTypeDispatchTarget*> GGeneratedTypeDispatchTarget = nullptr;
std::atomic<uint32> GGeneratedTypeActiveCalls = 0;
std::atomic<bool> GGeneratedTypeDispatchRetiring = false;
}

bool FAvidScriptGeneratedTypeDispatcher::Install(IAvidScriptGeneratedTypeDispatchTarget& Target)
{
	if (GGeneratedTypeDispatchRetiring.load(std::memory_order_acquire))
	{
		return false;
	}
	IAvidScriptGeneratedTypeDispatchTarget* Expected = nullptr;
	if (GGeneratedTypeDispatchTarget.compare_exchange_strong(
		Expected,
		&Target,
		std::memory_order_release,
		std::memory_order_relaxed))
	{
		return true;
	}
	return Expected == &Target;
}

void FAvidScriptGeneratedTypeDispatcher::Uninstall(IAvidScriptGeneratedTypeDispatchTarget& Target)
{
	bool bExpectedRetiring = false;
	if (!GGeneratedTypeDispatchRetiring.compare_exchange_strong(
		bExpectedRetiring,
		true,
		std::memory_order_acq_rel,
		std::memory_order_relaxed))
	{
		return;
	}
	IAvidScriptGeneratedTypeDispatchTarget* Expected = &Target;
	const bool bRemoved = GGeneratedTypeDispatchTarget.compare_exchange_strong(
		Expected,
		nullptr,
		std::memory_order_acq_rel,
		std::memory_order_relaxed);
	if (bRemoved)
	{
		while (GGeneratedTypeActiveCalls.load(std::memory_order_acquire) != 0)
		{
			FPlatformProcess::YieldThread();
		}
	}
	GGeneratedTypeDispatchRetiring.store(false, std::memory_order_release);
}

bool FAvidScriptGeneratedTypeDispatcher::Invoke(
	UObject* Receiver,
	const uint32 TypeOrdinal,
	const uint32 MemberOrdinal,
	const TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
	void* Result)
{
	if (Receiver == nullptr)
	{
		return false;
	}
	GGeneratedTypeActiveCalls.fetch_add(1, std::memory_order_acq_rel);
	IAvidScriptGeneratedTypeDispatchTarget* Target =
		GGeneratedTypeDispatchTarget.load(std::memory_order_acquire);
	const bool bResult = Target != nullptr
		&& Target->InvokeGeneratedTypeMember(
			*Receiver,
			TypeOrdinal,
			MemberOrdinal,
			Arguments,
			Result);
	GGeneratedTypeActiveCalls.fetch_sub(1, std::memory_order_release);
	return bResult;
}
