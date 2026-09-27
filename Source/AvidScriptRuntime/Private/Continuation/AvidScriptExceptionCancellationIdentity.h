#pragma once

#include "Memory/AvidScriptManagedHeap.h"

namespace AvidScript::Continuation
{
// Owned by the exception, not by a Task or cancellation source. No source lease.
class FExceptionCancellationIdentity final : public Managed::FNativeObjectData
{
public:
	static constexpr std::uint32_t Kind = 1;
	FExceptionCancellationIdentity(std::int32_t InExceptionType, std::int64_t InSource)
		: FNativeObjectData(Kind, sizeof(FExceptionCancellationIdentity)),
		ExceptionType(InExceptionType), Source(InSource) {}
	const std::int32_t ExceptionType;
	// Zero is a proven source-less cancellation. Missing data is not zero.
	const std::int64_t Source;
	static const FExceptionCancellationIdentity* Find(const Managed::FHeap& Heap, Managed::FToken Object)
	{
		return static_cast<const FExceptionCancellationIdentity*>(Heap.FindNativeData(Object, Kind));
	}
};
}
