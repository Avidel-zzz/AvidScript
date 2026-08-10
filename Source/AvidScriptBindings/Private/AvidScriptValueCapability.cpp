#include "AvidScriptValueCapability.h"

#include <atomic>

namespace
{
std::atomic<uint64> GNextAvidScriptValueCapability{ 1 };

const FAvidScriptValueCapabilityImportSpec GArrayImportSpecs[] = {
	{
		TEXT("avidscript.value_array_length.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_array_length"),
		TEXT("(i)i")
	},
	{
		TEXT("avidscript.value_array_load.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_array_load"),
		TEXT("(iiii)i")
	},
	{
		TEXT("avidscript.value_array_store.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_array_store"),
		TEXT("(iiii)i")
	},
	{
		TEXT("avidscript.value_release.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_release"),
		TEXT("(i)i")
	}
};
}

bool FAvidScriptValueCapability::IsToken(const uint32 ValueReference)
{
	return (ValueReference & TokenTag) != 0;
}

uint32 FAvidScriptValueCapability::AllocateToken()
{
	const uint64 Capability = GNextAvidScriptValueCapability.fetch_add(
		1,
		std::memory_order_relaxed);
	return Capability <= static_cast<uint64>(~TokenTag)
		? TokenTag | static_cast<uint32>(Capability)
		: 0;
}

TConstArrayView<FAvidScriptValueCapabilityImportSpec>
FAvidScriptValueCapability::GetArrayImportSpecs()
{
	return MakeArrayView(GArrayImportSpecs);
}
