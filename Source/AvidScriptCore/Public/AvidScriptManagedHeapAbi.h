#pragma once

#include <cstdint>

// Wire contract only: no engine/backend types and no native pointer representation.
namespace AvidScript::Managed::Abi
{
inline constexpr char ImportName[] = "avid_managed_heap_v1";
inline constexpr std::uint32_t Magic = 0x3150484d; // LE bytes: MHP1
inline constexpr std::uint32_t HeaderBytes = 8;
inline constexpr std::uint32_t MaxRequestBytes = 1024 * 1024;
inline constexpr std::uint32_t MaxResponseBytes = 64 * 1024;
inline constexpr std::uint32_t MaxLayouts = 1024;
inline constexpr std::uint32_t MaxReferencesPerLayout = 256;
inline constexpr std::uint32_t MaxTotalReferences = 65536;
inline constexpr std::uint32_t MaxStaticSlots = 4096;

enum class ECommand : std::uint32_t
{
	Configure = 1, PushFrame = 2, PopFrame = 3, CreateRoot = 4,
	SetRoot = 5, ReleaseRoot = 6, Allocate = 7, ReadBytes = 8,
	WriteBytes = 9, ReadReference = 10, WriteReference = 11, Collect = 12,
	ConfigureRootsOnly = 13, ConfigureStaticSlots = 14,
	ReadStaticSlot = 15, WriteStaticSlot = 16
};

// Signed lengths are accepted from i32 WASM arguments; addresses retain all 32 bits.
inline bool ValidateRanges(std::uint32_t Input, std::int32_t InputBytes,
	std::uint32_t Output, std::int32_t OutputBytes)
{
	if (Input == 0 || InputBytes < static_cast<std::int32_t>(HeaderBytes)
		|| InputBytes > static_cast<std::int32_t>(MaxRequestBytes)
		|| OutputBytes < 0 || OutputBytes > static_cast<std::int32_t>(MaxResponseBytes)) return false;
	const auto InputEnd = std::uint64_t(Input) + std::uint32_t(InputBytes);
	const auto OutputEnd = std::uint64_t(Output) + std::uint32_t(OutputBytes);
	if (InputEnd > (std::uint64_t(1) << 32) || OutputEnd > (std::uint64_t(1) << 32)) return false;
	if (OutputBytes == 0) return Output == 0;
	return Output != 0 && (InputEnd <= Output || OutputEnd <= Input);
}
}
