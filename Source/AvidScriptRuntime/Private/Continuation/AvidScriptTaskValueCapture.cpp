#include "Continuation/AvidScriptTaskValueCapture.h"
#include <algorithm>

namespace AvidScript::TaskResult
{
EValueError CaptureValue(const FValuePlan& Plan, Managed::FHeap& Heap,
    std::span<const std::uint8_t> Bytes, std::uint32_t InvocationFloor,
    const FManagedTypeResolver& ResolveType, FCapturedValue& OutValue)
{
    using ValueAbi::ELeafKind;
    if (Plan.GetTypeId().empty() || Bytes.size() != Plan.GetSize()) return EValueError::InvalidValue;
    std::vector<Managed::FToken> Objects;
    std::size_t End = 0;
    for (const FValueLeaf& Leaf : Plan.GetLeaves())
    {
        if (std::any_of(Bytes.begin() + End, Bytes.begin() + Leaf.Offset,
            [](std::uint8_t Byte) { return Byte != 0; })) return EValueError::InvalidValue;
        End = Leaf.Offset + Leaf.Size;
        if (Leaf.Kind == ELeafKind::Integer || Leaf.Kind == ELeafKind::Float) continue;
        if (Leaf.Kind != ELeafKind::ManagedReference) return EValueError::UnsupportedResource;
        if (Heap.GetStats().ActiveFrames <= InvocationFloor) return EValueError::RootAuthority;
        std::uint32_t Type = 0;
        if (!ResolveType || !ResolveType(Leaf, Type) || (Leaf.TargetTypeId.empty() != (Type == 0)))
            return EValueError::UnknownManagedType;
        Managed::FToken Object = 0;
        for (unsigned I = 0; I < 8; ++I) Object |= std::uint64_t(Bytes[Leaf.Offset + I]) << (I * 8);
        if (Object == 0) continue;
        if (!Heap.IsAlive(Object, Type)) return EValueError::InvalidObject;
        if (!Heap.IsObjectRootedInCurrentFrame(Object, InvocationFloor)) return EValueError::RootAuthority;
        if (std::find(Objects.begin(), Objects.end(), Object) == Objects.end()) Objects.push_back(Object);
    }
    if (std::any_of(Bytes.begin() + End, Bytes.end(), [](std::uint8_t Byte) { return Byte != 0; }))
        return EValueError::InvalidValue;
    FCapturedValue Captured;
    Captured.Bytes.assign(Bytes.begin(), Bytes.end());
    if (!Objects.empty() && Heap.RetainPersistent(Objects, Captured.Roots) != Managed::EHeapError::Ok)
        return EValueError::HeapFailure;
    OutValue = std::move(Captured); return EValueError::Ok;
}
}
