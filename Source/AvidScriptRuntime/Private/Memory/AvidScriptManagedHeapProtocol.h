#pragma once

#include "AvidScriptManagedHeap.h"
#include "AvidScriptManagedHeapAbi.h"

namespace AvidScript::Managed
{
enum class EHeapProtocolError : std::uint8_t
{
	Ok, InvalidPacket, InvalidVersion, UnknownCommand, InvalidOutput, LimitExceeded, FrameBoundary, HeapFailure
};
struct FHeapProtocolResult
{
	EHeapProtocolError Error = EHeapProtocolError::Ok;
	EHeapError HeapError = EHeapError::Ok;
	bool Succeeded() const { return Error == EHeapProtocolError::Ok; }
};

// Borrowed, disjoint views. Synchronous: no guest reentry or memory growth.
// Output and packet shape are checked before mutating the heap or output bytes.
FHeapProtocolResult ExecuteHeapCommand(FHeap& Heap, std::span<const std::uint8_t> Request,
	std::span<std::uint8_t> Response, std::uint32_t InvocationFrameFloor);
const char* HeapProtocolErrorName(EHeapProtocolError Error);
const char* HeapErrorName(EHeapError Error);
}
