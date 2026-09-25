#pragma once

#include <cstdint>

// Fixed-width Task<int> Host ABI. The import returns i64 for every command:
// a task/continuation token, 0 for an already-ready await, 1 for accepted
// mutations, or a packed terminal state and i32 value for Read.
namespace AvidScript::TaskResult::Abi
{
inline constexpr char Int32Import[] = "avid_task_i32_v1"; // (iIii)I
inline constexpr char BindProducerImport[] = "avid_task_bind_producer_v1"; // (II)i
inline constexpr char PropagateFailureImport[] = "avid_task_propagate_failure_v1"; // (II)i
inline constexpr char RetainForContinuationImport[] = "avid_task_retain_for_continuation_v1"; // (II)i
inline constexpr char FaultLanguageErrorImport[] = "avid_task_fault_language_error_v1"; // (IiiI)i
inline constexpr char LanguageErrorMetaImport[] = "avid_task_language_error_meta_v1"; // (I)I
inline constexpr char LanguageErrorRootImport[] = "avid_task_language_error_root_v1"; // (I)I
inline constexpr char Int32TypeId[] = "type:int32";

enum class ECommand : std::uint32_t
{
	Create = 1,
	Retain = 2,
	Release = 3,
	Await = 4,
	Succeed = 5,
	Cancel = 6,
	Read = 7
};

enum class EState : std::uint32_t
{
	Succeeded = 1,
	Faulted = 2,
	Cancelled = 3
};

inline constexpr std::uint64_t PackRead(EState State, std::int32_t Value)
{
	return (std::uint64_t(State) << 32) | std::uint32_t(Value);
}
}
