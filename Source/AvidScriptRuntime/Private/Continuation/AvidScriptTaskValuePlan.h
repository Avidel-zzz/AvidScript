#pragma once

#include "AvidScriptTaskValuePlanAbi.h"
#include <array>
#include <span>
#include <string>
#include <vector>

namespace AvidScript::TaskResult
{
enum class EValueError : std::uint8_t
{
    Ok, InvalidVersion, LimitExceeded, InvalidPlan, InvalidValue,
    UnsupportedResource, UnknownManagedType, InvalidObject, RootAuthority, HeapFailure
};

struct FValueLeaf
{
    std::uint32_t Offset = 0, Size = 0, Alignment = 1;
    ValueAbi::ELeafKind Kind = ValueAbi::ELeafKind::Integer;
    std::string TypeId, TargetTypeId;
};

// Immutable after parsing. Shape validation is not nominal authorization: the
// loaded artifact owner must also match the compiler's original type catalog.
class FValuePlan final
{
public:
    const std::string& GetTypeId() const { return TypeId; }
    std::uint32_t GetSize() const { return Size; }
    std::uint32_t GetAlignment() const { return Alignment; }
    std::span<const FValueLeaf> GetLeaves() const { return Leaves; }
    const std::array<std::uint8_t, ValueAbi::ShapeHashBytes>& GetShapeHash() const { return ShapeHash; }
    bool RequiresLease() const;
private:
    friend EValueError ReadValuePlan(std::span<const std::uint8_t>, FValuePlan&);
    std::string TypeId;
    std::uint32_t Size = 0, Alignment = 1;
    std::array<std::uint8_t, ValueAbi::ShapeHashBytes> ShapeHash{};
    std::vector<FValueLeaf> Leaves;
};

// Failure preserves OutPlan. Bounded allocations only after header admission.
EValueError ReadValuePlan(std::span<const std::uint8_t> Packet, FValuePlan& OutPlan);
}
