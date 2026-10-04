#include "Continuation/AvidScriptTaskValuePlan.h"
#include <algorithm>
#include <utility>

namespace AvidScript::TaskResult
{
namespace ValuePlanPrivate
{
bool Alignment(std::uint32_t Value) { return Value != 0 && Value <= 16 && (Value & (Value - 1)) == 0; }
bool IdentityUtf8(std::span<const std::uint8_t> Bytes)
{
    // Reject overlong encodings, surrogate code points, controls and truncation.
    bool HasNonWhitespace = false;
    for (std::size_t I = 0; I < Bytes.size();)
    {
        std::uint32_t Code = Bytes[I++], Minimum = 0;
        unsigned Count = 0;
        if (Code >= 0xc2 && Code <= 0xdf) { Code &= 0x1f; Count = 1; Minimum = 0x80; }
        else if (Code >= 0xe0 && Code <= 0xef) { Code &= 0x0f; Count = 2; Minimum = 0x800; }
        else if (Code >= 0xf0 && Code <= 0xf4) { Code &= 7; Count = 3; Minimum = 0x10000; }
        else if (Code >= 0x80) return false;
        if (Bytes.size() - I < Count) return false;
        for (unsigned J = 0; J < Count; ++J)
        {
            if ((Bytes[I] & 0xc0) != 0x80) return false;
            Code = (Code << 6) | (Bytes[I++] & 0x3f);
        }
        if (Code < Minimum || Code > 0x10ffff || (Code >= 0xd800 && Code <= 0xdfff)
            || Code < 0x20 || (Code >= 0x7f && Code <= 0x9f)) return false;
        const bool Whitespace = Code == 0x20 || Code == 0xa0 || Code == 0x1680
            || (Code >= 0x2000 && Code <= 0x200a) || Code == 0x2028 || Code == 0x2029
            || Code == 0x202f || Code == 0x205f || Code == 0x3000;
        HasNonWhitespace = HasNonWhitespace || !Whitespace;
    }
    return HasNonWhitespace || Bytes.empty();
}
class FReader
{
public:
    explicit FReader(std::span<const std::uint8_t> In) : Bytes(In) {}
    bool U32(std::uint32_t& Out)
    {
        if (Bytes.size() - Position < 4) return false;
        Out = 0; for (unsigned I = 0; I < 4; ++I) Out |= std::uint32_t(Bytes[Position++]) << (8 * I);
        return true;
    }
    bool Identity(std::string& Out, bool Optional = false)
    {
        std::uint32_t Count = 0;
        if (!U32(Count) || Count > ValueAbi::MaxIdentityBytes || (!Optional && Count == 0)
            || Bytes.size() - Position < Count) return false;
        const auto Text = Bytes.subspan(Position, Count);
        if (!IdentityUtf8(Text) || (!Optional && std::all_of(Text.begin(), Text.end(),
            [](std::uint8_t C) { return C == ' '; }))) return false;
        Out.assign(reinterpret_cast<const char*>(Text.data()), Count); Position += Count; return true;
    }
    bool Hash(std::array<std::uint8_t, ValueAbi::ShapeHashBytes>& Out)
    {
        if (Bytes.size() - Position < Out.size()) return false;
        std::copy_n(Bytes.begin() + Position, Out.size(), Out.begin()); Position += Out.size();
        return std::any_of(Out.begin(), Out.end(), [](std::uint8_t Byte) { return Byte != 0; });
    }
    bool End() const { return Position == Bytes.size(); }
private:
    std::span<const std::uint8_t> Bytes;
    std::size_t Position = 0;
};
bool LeafShape(const FValueLeaf& Leaf)
{
    using ValueAbi::ELeafKind;
    if (!Alignment(Leaf.Alignment) || Leaf.Alignment > Leaf.Size || Leaf.Offset % Leaf.Alignment != 0)
        return false;
    switch (Leaf.Kind)
    {
    case ELeafKind::Integer:
        return (Leaf.Size == 1 || Leaf.Size == 2 || Leaf.Size == 4 || Leaf.Size == 8) && Leaf.TargetTypeId.empty();
    case ELeafKind::Float:
        return (Leaf.Size == 4 || Leaf.Size == 8) && Leaf.TargetTypeId.empty();
    case ELeafKind::ManagedReference:
        return Leaf.Size == 8 && Leaf.Alignment == 8;
    case ELeafKind::LinearArray:
        return Leaf.Size == 4 && Leaf.Alignment == 4 && !Leaf.TargetTypeId.empty();
    case ELeafKind::UeHandle:
        return Leaf.Size == 8 && Leaf.Alignment == 8 && Leaf.TargetTypeId.empty();
    case ELeafKind::LinearString: case ELeafKind::ClassReference: case ELeafKind::FactoryReference:
    case ELeafKind::ObjectTypeReference: case ELeafKind::CompositeReference: case ELeafKind::FunctionReference:
        return Leaf.Size == 4 && Leaf.Alignment == 4 && Leaf.TargetTypeId.empty();
    default: return false;
    }
}
}

bool FValuePlan::RequiresLease() const
{
    return std::any_of(Leaves.begin(), Leaves.end(), [](const FValueLeaf& Leaf)
    { return Leaf.Kind != ValueAbi::ELeafKind::Integer && Leaf.Kind != ValueAbi::ELeafKind::Float; });
}

EValueError ReadValuePlan(std::span<const std::uint8_t> Packet, FValuePlan& OutPlan)
{
    using namespace ValuePlanPrivate;
    if (Packet.size() > ValueAbi::MaxPlanBytes) return EValueError::LimitExceeded;
    FReader Reader(Packet); std::uint32_t Magic = 0, Version = 0, Count = 0;
    if (!Reader.U32(Magic) || !Reader.U32(Version)) return EValueError::InvalidPlan;
    if (Magic != ValueAbi::Magic || Version != ValueAbi::Version) return EValueError::InvalidVersion;
    FValuePlan Plan;
    if (!Reader.U32(Plan.Size) || !Reader.U32(Plan.Alignment) || !Reader.U32(Count)) return EValueError::InvalidPlan;
    if (Plan.Size > ValueAbi::MaxValueBytes || Count > ValueAbi::MaxLeaves) return EValueError::LimitExceeded;
    if (!Alignment(Plan.Alignment) || Plan.Size % Plan.Alignment != 0
        || !Reader.Identity(Plan.TypeId) || !Reader.Hash(Plan.ShapeHash)) return EValueError::InvalidPlan;
    Plan.Leaves.reserve(Count);
    std::uint32_t End = 0, MaximumAlignment = 1;
    for (std::uint32_t I = 0; I < Count; ++I)
    {
        FValueLeaf Leaf; std::uint32_t Kind = 0;
        if (!Reader.U32(Leaf.Offset) || !Reader.U32(Leaf.Size) || !Reader.U32(Leaf.Alignment)
            || !Reader.U32(Kind) || !Reader.Identity(Leaf.TypeId) || !Reader.Identity(Leaf.TargetTypeId, true))
            return EValueError::InvalidPlan;
        Leaf.Kind = static_cast<ValueAbi::ELeafKind>(Kind);
        if (!LeafShape(Leaf) || Leaf.Offset < End || Leaf.Offset > Plan.Size || Leaf.Size > Plan.Size - Leaf.Offset)
            return EValueError::InvalidPlan;
        End = Leaf.Offset + Leaf.Size; MaximumAlignment = std::max(MaximumAlignment, Leaf.Alignment);
        Plan.Leaves.push_back(std::move(Leaf));
    }
    if (!Reader.End() || Plan.Alignment != MaximumAlignment
        || Plan.Size != ((End + Plan.Alignment - 1) & ~(Plan.Alignment - 1))) return EValueError::InvalidPlan;
    OutPlan = std::move(Plan); return EValueError::Ok;
}
}
