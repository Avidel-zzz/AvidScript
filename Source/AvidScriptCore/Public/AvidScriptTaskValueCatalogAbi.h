#pragma once
#include <cstdint>

namespace AvidScript::TaskResult::CatalogAbi
{
inline constexpr std::uint32_t Magic = 0x31435654; // TVC1
inline constexpr std::uint32_t Version = 1;
inline constexpr std::uint32_t MaxCatalogBytes = 2 * 1024 * 1024;
inline constexpr std::uint32_t MaxResults = 128;
inline constexpr std::uint32_t MaxManagedTypes = 1024;
inline constexpr std::uint32_t MaxPayloadBytes = 65536;
inline constexpr std::uint32_t MaxReferences = 256;
inline constexpr std::uint32_t MaxTotalReferences = 65536;
inline constexpr const char* SectionName = "avidscript.task_values";
}
