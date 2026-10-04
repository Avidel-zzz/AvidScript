#include "Continuation/AvidScriptTaskValueCatalog.h"
#include <algorithm>
#include <unordered_set>

namespace AvidScript::TaskResult
{
namespace ValueCatalogPrivate
{
class FReader
{
public:
    explicit FReader(std::span<const std::uint8_t> In) : Bytes(In) {}
    bool U32(std::uint32_t& Out)
    {
        if (Bytes.size() - Position < 4) return false;
        Out = 0; for (unsigned I = 0; I < 4; ++I) Out |= std::uint32_t(Bytes[Position++]) << (I * 8);
        return true;
    }
    bool Block(std::size_t Count, std::span<const std::uint8_t>& Out)
    {
        if (Count > Bytes.size() - Position) return false;
        Out = Bytes.subspan(Position, Count); Position += Count; return true;
    }
    bool Identity(std::string& Out, bool Optional = false)
    {
        std::uint32_t Count = 0; std::span<const std::uint8_t> Text;
        if (!U32(Count) || Count > ValueAbi::MaxIdentityBytes || (!Optional && Count == 0)
            || !Block(Count, Text) || !IsValueIdentityUtf8(Text)) return false;
        if (Text.empty()) Out.clear(); else Out.assign(reinterpret_cast<const char*>(Text.data()), Text.size());
        return true;
    }
    bool End() const { return Position == Bytes.size(); }
private:
    std::span<const std::uint8_t> Bytes;
    std::size_t Position = 0;
};
}

const FValuePlan* FValueCatalog::Find(std::uint32_t Ordinal) const
{ return Ordinal > 0 && Ordinal <= Results.size() ? &Results[Ordinal - 1] : nullptr; }

EValueError FValueCatalog::Capture(std::uint32_t Ordinal, Managed::FHeap& Heap,
    std::span<const std::uint8_t> Bytes, std::uint32_t InvocationFloor, FCapturedValue& OutValue) const
{
    const auto* Plan = Find(Ordinal);
    if (!Plan) return EValueError::InvalidPlan;
    const bool bManaged = std::any_of(Plan->GetLeaves().begin(), Plan->GetLeaves().end(),
        [](const FValueLeaf& Leaf) { return Leaf.Kind == ValueAbi::ELeafKind::ManagedReference; });
    if (bManaged && !MatchesHeap(Heap)) return EValueError::UnknownManagedType;
    return CaptureValue(*Plan, Heap, Bytes, InvocationFloor, [this](const FValueLeaf& Leaf, std::uint32_t& Type)
    {
        const auto It = Bindings.find(Leaf.TypeId);
        if (It == Bindings.end() || It->second.PayloadType != Leaf.TargetTypeId) return false;
        Type = It->second.HeapOrdinal; return true;
    }, OutValue);
}

EValueError ReadValueCatalog(std::span<const std::uint8_t> Packet, const std::string& ExpectedModule,
    const std::array<std::uint8_t, 32>& ExpectedSource, FValueCatalog& OutCatalog)
{
    using namespace CatalogAbi;
    if (Packet.size() > MaxCatalogBytes) return EValueError::LimitExceeded;
    ValueCatalogPrivate::FReader Reader(Packet); FValueCatalog Candidate;
    std::uint32_t WireMagic = 0, WireVersion = 0, Count = 0; std::span<const std::uint8_t> Source;
    if (!Reader.U32(WireMagic) || !Reader.U32(WireVersion)) return EValueError::InvalidPlan;
    if (WireMagic != Magic || WireVersion != Version) return EValueError::InvalidVersion;
    if (ExpectedModule.empty() || !Reader.Identity(Candidate.ModuleId) || Candidate.ModuleId != ExpectedModule
        || !Reader.Block(32, Source) || !std::equal(Source.begin(), Source.end(), ExpectedSource.begin())
        || std::none_of(Source.begin(), Source.end(), [](std::uint8_t B) { return B != 0; }) || !Reader.U32(Count))
        return EValueError::InvalidPlan;
    if (Count == 0 || Count > MaxResults) return EValueError::LimitExceeded;
    Candidate.Results.reserve(Count); std::unordered_set<std::string> Types;
    for (std::uint32_t I = 0; I < Count; ++I)
    {
        std::uint32_t Ordinal = 0, Size = 0; std::span<const std::uint8_t> Value; FValuePlan Plan;
        if (!Reader.U32(Ordinal) || Ordinal != I + 1 || !Reader.U32(Size)
            || Size > ValueAbi::MaxPlanBytes || !Reader.Block(Size, Value)) return EValueError::InvalidPlan;
        const auto Error = ReadValuePlan(Value, Plan);
        if (Error != EValueError::Ok) return Error;
        if (!Types.insert(Plan.GetTypeId()).second) return EValueError::InvalidPlan;
        Candidate.Results.push_back(std::move(Plan));
    }
    if (!Reader.U32(Count) || Count > MaxManagedTypes) return EValueError::LimitExceeded;
    std::uint32_t NextOrdinal = 1, TotalReferences = 0;
    for (std::uint32_t I = 0; I < Count; ++I)
    {
        std::string Type; FValueCatalog::FBinding Binding; Managed::FHeapLayout Layout; std::uint32_t Edges = 0;
        if (!Reader.Identity(Type) || !Reader.Identity(Binding.PayloadType, true) || !Reader.U32(Binding.HeapOrdinal)
            || !Reader.U32(Layout.ByteSize) || !Reader.U32(Edges)) return EValueError::InvalidPlan;
        if (Binding.PayloadType.empty())
        {
            if (Binding.HeapOrdinal != 0 || Layout.ByteSize != 0 || Edges != 0) return EValueError::InvalidPlan;
        }
        else
        {
            if (Binding.HeapOrdinal != NextOrdinal++ || Layout.ByteSize == 0 || Layout.ByteSize > MaxPayloadBytes
                || Edges > MaxReferences || Edges > MaxTotalReferences - TotalReferences) return EValueError::InvalidPlan;
            Layout.TypeId = Binding.HeapOrdinal; TotalReferences += Edges; Layout.References.reserve(Edges);
            for (std::uint32_t J = 0; J < Edges; ++J)
            {
                Managed::FReferenceField Field;
                if (!Reader.U32(Field.Offset) || !Reader.U32(Field.TargetTypeId) || Field.Offset % 8 != 0
                    || Layout.ByteSize < 8 || Field.Offset > Layout.ByteSize - 8
                    || J > 0 && Field.Offset < Layout.References.back().Offset + 8) return EValueError::InvalidPlan;
                Layout.References.push_back(Field);
            }
            Candidate.Layouts.push_back(std::move(Layout));
        }
        if (!Candidate.Bindings.emplace(std::move(Type), std::move(Binding)).second) return EValueError::InvalidPlan;
    }
    if (!Reader.End()) return EValueError::InvalidPlan;
    for (const auto& Layout : Candidate.Layouts)
        for (const auto& Field : Layout.References)
            if (Field.TargetTypeId >= NextOrdinal) return EValueError::InvalidPlan;
    for (const auto& Plan : Candidate.Results)
        for (const auto& Leaf : Plan.GetLeaves())
            if (Leaf.Kind == ValueAbi::ELeafKind::ManagedReference)
            {
                const auto Binding = Candidate.Bindings.find(Leaf.TypeId);
                if (Binding == Candidate.Bindings.end() || Binding->second.PayloadType != Leaf.TargetTypeId) return EValueError::InvalidPlan;
            }
    OutCatalog = std::move(Candidate); return EValueError::Ok;
}
}
