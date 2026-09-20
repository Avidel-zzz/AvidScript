#pragma once

// Versioned scalar ABI. Object/type refer to the current Runtime's validated
// managed heap, never a native address or a caller-supplied reference map.
namespace AvidScript::ContinuationState::Abi
{
inline constexpr char StoreImport[] = "avid_continuation_state_store_v1"; // (IiI)i: continuation, type, object -> accepted
inline constexpr char ReadImport[] = "avid_continuation_state_read_v1"; // (Ii)I: current continuation, type -> object
inline constexpr unsigned StateBytes = 12; // native owner storage: LE u32 type, u64 object
}
