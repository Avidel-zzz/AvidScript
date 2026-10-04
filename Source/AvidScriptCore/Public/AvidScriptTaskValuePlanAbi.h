#pragma once

#include <cstdint>

// Cold-path value description, not a Guest import or execution authorization.
// Little-endian integers; length-prefixed, strict UTF-8 nominal identities.
namespace AvidScript::TaskResult::ValueAbi
{
inline constexpr std::uint32_t Magic = 0x31565054; // TPV1
inline constexpr std::uint32_t Version = 1;
inline constexpr std::uint32_t MaxValueBytes = 4096;
inline constexpr std::uint32_t MaxLeaves = 256;
inline constexpr std::uint32_t MaxIdentityBytes = 1024;
inline constexpr std::uint32_t MaxPlanBytes = 1024 * 1024;
inline constexpr std::uint32_t ShapeHashBytes = 32;

enum class ELeafKind : std::uint32_t
{
    Integer = 1, Float = 2, ManagedReference = 3,
    LinearString = 4, LinearArray = 5, UeHandle = 6,
    ClassReference = 7, FactoryReference = 8, ObjectTypeReference = 9,
    CompositeReference = 10, FunctionReference = 11
};
}
