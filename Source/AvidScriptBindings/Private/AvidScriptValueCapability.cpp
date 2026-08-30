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
	},
	{
		TEXT("avidscript.value_array_read_range.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_array_read_range"),
		TEXT("(iiiii)i")
	},
	{
		TEXT("avidscript.value_array_write_range.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_array_write_range"),
		TEXT("(iiiii)i")
	}
};

const FAvidScriptValueCapabilityImportSpec GCompositeImportSpecs[] = {
	{
		TEXT("avidscript.value_release.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_release"),
		TEXT("(i)i")
	},
	{
		TEXT("avidscript.value_text_to_string.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_text_to_string"),
		TEXT("(i)i")
	}
};

const FAvidScriptValueCapabilityImportSpec GCompositeContainerImportSpecs[] = {
	{
		TEXT("avidscript.value_container_count.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_container_count"),
		TEXT("(i)i")
	},
	{
		TEXT("avidscript.value_container_read.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_container_read"),
		TEXT("(iiii)i")
	},
	{
		TEXT("avidscript.value_container_write.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_container_write"),
		TEXT("(iiii)i")
	},
	{
		TEXT("avidscript.value_container_resize.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_container_resize"),
		TEXT("(ii)i")
	},
	{
		TEXT("avidscript.value_container_clear.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_container_clear"),
		TEXT("(i)i")
	},
	{
		TEXT("avidscript.value_container_find.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_container_find"),
		TEXT("(ii)i")
	},
	{
		TEXT("avidscript.value_container_upsert.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_container_upsert"),
		TEXT("(iii)i")
	},
	{
		TEXT("avidscript.value_container_remove.v1"),
		TEXT("avidscript"),
		TEXT("avid_value_container_remove"),
		TEXT("(ii)i")
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

TConstArrayView<FAvidScriptValueCapabilityImportSpec>
FAvidScriptValueCapability::GetCompositeImportSpecs()
{
	return MakeArrayView(GCompositeImportSpecs);
}

TConstArrayView<FAvidScriptValueCapabilityImportSpec>
FAvidScriptValueCapability::GetCompositeContainerImportSpecs()
{
	return MakeArrayView(GCompositeContainerImportSpecs);
}
