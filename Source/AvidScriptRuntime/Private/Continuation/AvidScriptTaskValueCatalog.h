#pragma once
#include "AvidScriptTaskValueCatalogAbi.h"
#include "Continuation/AvidScriptTaskValueCapture.h"
#include <unordered_map>

namespace AvidScript::TaskResult
{
class FValueCatalog final
{
public:
    const FValuePlan* Find(std::uint32_t Ordinal) const;
    std::size_t GetResultCount() const { return Results.size(); }
    const std::string& GetModuleId() const { return ModuleId; }
    bool MatchesHeap(const Managed::FHeap& Heap) const { return Heap.MatchesLayouts(Layouts); }
    EValueError Capture(std::uint32_t Ordinal, Managed::FHeap& Heap,
        std::span<const std::uint8_t> Bytes, std::uint32_t InvocationFloor, FCapturedValue& OutValue) const;
private:
    friend EValueError ReadValueCatalog(std::span<const std::uint8_t>, const std::string&,
        const std::array<std::uint8_t, 32>&, FValueCatalog&);
    struct FBinding { std::string PayloadType; std::uint32_t HeapOrdinal = 0; };
    std::string ModuleId;
    std::vector<FValuePlan> Results;
    std::unordered_map<std::string, FBinding> Bindings;
    std::vector<Managed::FHeapLayout> Layouts;
};
// Expected identity comes from the same canonical module's validated provenance.
// Failure preserves OutCatalog. This metadata grants no executable capability.
EValueError ReadValueCatalog(std::span<const std::uint8_t> Packet, const std::string& ExpectedModule,
    const std::array<std::uint8_t, 32>& ExpectedSource, FValueCatalog& OutCatalog);
}
