#pragma once

#include "CoreTypes.h"

// Read-only query. Invalid is distinct from an open source and must not schedule work.
enum class EAvidScriptCancellationSourceStatus : int32
{
	Invalid = 0,
	Open = 1,
	Cancelled = 2
};

namespace AvidScript::ContinuationCancellation::Abi
{
inline constexpr const char* Module = "avidscript";
inline constexpr const char* StatusImport = "avid_continuation_cancel_status_v1";
}
