#pragma once

#include "Continuation/AvidScriptTaskValuePlan.h"
#include "Memory/AvidScriptManagedHeap.h"
#include <functional>
#include <utility>

namespace AvidScript::TaskResult
{
class FCapturedValue final
{
public:
    std::span<const std::uint8_t> GetBytes() const { return Bytes; }
    Managed::FPersistentRoots TakeRoots() { return std::move(Roots); }
    std::size_t GetRootCount() const { return Roots.Count(); }
    // The caller supplies the trusted loaded plan and retains the task/owner
    // through this read. Failure preserves both output and current-frame roots.
    EValueError ReadIntoFrame(const FValuePlan& ExpectedPlan, Managed::FHeap& Heap,
        std::span<std::uint8_t> OutBytes, std::uint32_t InvocationFloor) const;
private:
    friend EValueError CaptureValue(const FValuePlan&, Managed::FHeap&, std::span<const std::uint8_t>,
        std::uint32_t, const std::function<bool(const FValueLeaf&, std::uint32_t&)>&, FCapturedValue&);
    std::vector<std::uint8_t> Bytes;
    FValuePlan Plan;
    std::vector<Managed::FToken> Objects;
    Managed::FPersistentRoots Roots;
};

// Native catalog resolver only; ordinals must come from the loaded heap plan,
// never from a result packet. Acquisition requires current invocation roots.
using FManagedTypeResolver = std::function<bool(const FValueLeaf&, std::uint32_t&)>;
// Scalars/enums and nested managed refs currently have capture implementations.
// Other resource leaves fail before mutation, including when their value is null.
// Failure preserves OutValue, including its previous roots and bytes.
EValueError CaptureValue(const FValuePlan& Plan, Managed::FHeap& Heap,
    std::span<const std::uint8_t> Bytes, std::uint32_t InvocationFloor,
    const FManagedTypeResolver& ResolveType, FCapturedValue& OutValue);
}
