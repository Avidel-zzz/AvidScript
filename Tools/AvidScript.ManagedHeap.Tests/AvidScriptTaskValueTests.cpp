#include "Continuation/AvidScriptTaskValueCapture.h"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace AvidScriptTaskValueTestsPrivate
{
using namespace AvidScript::TaskResult;
using namespace AvidScript::Managed;
using ValueAbi::ELeafKind;
void Check(bool Value, const char* Message) { if (!Value) throw std::runtime_error(Message); }
void Ok(EHeapError Error) { Check(Error == EHeapError::Ok, "task value heap operation failed"); }
void U32(std::vector<std::uint8_t>& Bytes, std::uint32_t Value)
{ for (unsigned I = 0; I < 4; ++I) Bytes.push_back(std::uint8_t(Value >> (I * 8))); }
void Identity(std::vector<std::uint8_t>& Bytes, const std::string& Value)
{ U32(Bytes, static_cast<std::uint32_t>(Value.size())); Bytes.insert(Bytes.end(), Value.begin(), Value.end()); }
void Set32(std::vector<std::uint8_t>& Bytes, std::size_t Offset, std::uint32_t Value)
{ for (unsigned I = 0; I < 4; ++I) Bytes[Offset + I] = std::uint8_t(Value >> (I * 8)); }
void Token(std::vector<std::uint8_t>& Bytes, std::size_t Offset, FToken Value)
{ for (unsigned I = 0; I < 8; ++I) Bytes[Offset + I] = std::uint8_t(Value >> (I * 8)); }
std::vector<std::uint8_t> Packet(std::uint32_t Size, std::uint32_t Alignment,
    std::initializer_list<FValueLeaf> Leaves, const std::string& Type = "v:test")
{
    std::vector<std::uint8_t> Bytes;
    U32(Bytes, ValueAbi::Magic); U32(Bytes, ValueAbi::Version); U32(Bytes, Size); U32(Bytes, Alignment);
    U32(Bytes, static_cast<std::uint32_t>(Leaves.size())); Identity(Bytes, Type);
    for (unsigned I = 0; I < ValueAbi::ShapeHashBytes; ++I) Bytes.push_back(std::uint8_t(I + 1));
    for (const FValueLeaf& Leaf : Leaves)
    {
        U32(Bytes, Leaf.Offset); U32(Bytes, Leaf.Size); U32(Bytes, Leaf.Alignment);
        U32(Bytes, static_cast<std::uint32_t>(Leaf.Kind)); Identity(Bytes, Leaf.TypeId); Identity(Bytes, Leaf.TargetTypeId);
    }
    return Bytes;
}
FValuePlan Plan(std::uint32_t Size, std::uint32_t Alignment, std::initializer_list<FValueLeaf> Leaves)
{
    FValuePlan Value;
    Check(ReadValuePlan(Packet(Size, Alignment, Leaves), Value) == EValueError::Ok, "valid test plan rejected");
    return Value;
}
FValuePlan PairPlan()
{ return Plan(16, 8, {{0, 8, 8, ELeafKind::ManagedReference, "r:node", "p:node"}, {8, 8, 8, ELeafKind::ManagedReference, "r:node", "p:node"}}); }
const FManagedTypeResolver Resolver = [](const FValueLeaf& Leaf, std::uint32_t& Type)
{
    if (Leaf.TypeId != "r:node" || Leaf.TargetTypeId != "p:node") return false;
    Type = 1; return true;
};
FToken Frame(FHeap& Heap) { FToken Value = 0; Ok(Heap.PushFrame(Value)); return Value; }
FToken Object(FHeap& Heap, FToken InFrame, std::uint32_t Type = 1)
{ FToken Root = 0, Value = 0; Ok(Heap.CreateRoot(InFrame, 0, Root)); Ok(Heap.Allocate(Type, Root, Value)); return Value; }
void Configure(FHeap& Heap)
{ const std::array<FHeapLayout, 2> Layouts{{{1, 8, {{0, 1}}}, {2, 8, {}}}}; Ok(Heap.Configure(Layouts)); }

void ParseAndAtomicRejection()
{
    const auto Good = Packet(16, 8, {{0, 8, 8, ELeafKind::ManagedReference, "r:node", "p:node"},
        {8, 8, 8, ELeafKind::ManagedReference, "r:node", "p:node"}});
    FValuePlan Value;
    Check(ReadValuePlan(Good, Value) == EValueError::Ok && Value.RequiresLease(), "reference plan failed");
    for (std::size_t I = 0; I < Good.size(); ++I)
    {
        const std::span<const std::uint8_t> Truncated(Good.data(), I);
        Check(ReadValuePlan(Truncated, Value) != EValueError::Ok, "truncated plan accepted");
        Check(Value.GetSize() == 16 && Value.GetLeaves().size() == 2, "failed read changed prior plan");
    }
    auto Bad = Good; Bad.push_back(0);
    Check(ReadValuePlan(Bad, Value) == EValueError::InvalidPlan, "trailing bytes accepted");
    Bad = Good; Set32(Bad, 4, 2);
    Check(ReadValuePlan(Bad, Value) == EValueError::InvalidVersion, "future version accepted");
    Bad = Good; Set32(Bad, 16, ValueAbi::MaxLeaves + 1);
    Check(ReadValuePlan(Bad, Value) == EValueError::LimitExceeded, "leaf budget ignored");
    Bad = Good; Set32(Bad, 8, ValueAbi::MaxValueBytes + 1);
    Check(ReadValuePlan(Bad, Value) == EValueError::LimitExceeded, "value budget ignored");
    Bad = Good; Set32(Bad, 20, ValueAbi::MaxIdentityBytes + 1);
    Check(ReadValuePlan(Bad, Value) == EValueError::InvalidPlan, "identity budget ignored");
    Bad.assign(ValueAbi::MaxPlanBytes + 1, 0);
    Check(ReadValuePlan(Bad, Value) == EValueError::LimitExceeded, "plan budget ignored");
}
void InvalidLeafShapes()
{
    FValuePlan Value;
    for (auto Bad : {Packet(8, 8, {{0, 8, 8, static_cast<ELeafKind>(99), "s:value", ""}}),
        Packet(16, 8, {{0, 8, 8, ELeafKind::ManagedReference, "r:node", "p:node"}, {0, 8, 8, ELeafKind::ManagedReference, "r:node", "p:node"}}),
        Packet(16, 8, {{4, 8, 8, ELeafKind::ManagedReference, "r:node", "p:node"}}),
        Packet(4, 4, {{0, 4, 4, ELeafKind::LinearArray, "r:array", ""}}),
        Packet(8, 8, {{0, 8, 8, ELeafKind::Integer, "s:i64", "hidden"}}),
        Packet(8, 4, {{0, 4, 4, ELeafKind::Integer, "s:i32", ""}})})
        Check(ReadValuePlan(Bad, Value) == EValueError::InvalidPlan, "invalid leaf shape accepted");
    auto Bad = Packet(4, 4, {{0, 4, 4, ELeafKind::Float, "s:f32", ""}});
    const std::size_t Leaf = 24 + std::string("v:test").size() + ValueAbi::ShapeHashBytes;
    Set32(Bad, Leaf, 0xfffffffcu);
    Check(ReadValuePlan(Bad, Value) == EValueError::InvalidPlan, "offset wrap accepted");
}
void IdentityEncoding()
{
    FValuePlan Value;
    for (const std::string& Type : {std::string("bad\0id", 6), std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
        std::string("\xf4\x90\x80\x80"), std::string("\xe2\x82"), std::string("   ")})
        Check(ReadValuePlan(Packet(0, 1, {}, Type), Value) == EValueError::InvalidPlan, "invalid nominal identity accepted");
    Check(ReadValuePlan(Packet(0, 1, {}, "v:\xe7\x8e\xa9\xe5\xae\xb6"), Value) == EValueError::Ok,
        "valid Unicode identity rejected");
}
void ExactScalarBitsAndPadding()
{
    FHeap Heap; FCapturedValue Capture;
    auto Value = Plan(16, 8, {{0, 4, 4, ELeafKind::Float, "s:f32", ""}, {8, 8, 8, ELeafKind::Float, "s:f64", ""}});
    std::vector<std::uint8_t> Bytes(16, 0); Set32(Bytes, 0, 0x80000000u); Token(Bytes, 8, 0x7ff8123456789abcULL);
    Check(CaptureValue(Value, Heap, Bytes, 0, {}, Capture) == EValueError::Ok, "scalar capture failed");
    Check(std::equal(Capture.GetBytes().begin(), Capture.GetBytes().end(), Bytes.begin()) && Capture.GetRootCount() == 0,
        "negative zero or NaN payload was numerically converted");
    const auto Original = Bytes; Bytes[4] = 1;
    Check(CaptureValue(Value, Heap, Bytes, 0, {}, Capture) == EValueError::InvalidValue, "nonzero padding accepted");
    Check(std::equal(Capture.GetBytes().begin(), Capture.GetBytes().end(), Original.begin()), "failed padding admission changed result");
    Check(CaptureValue(Value, Heap, {}, 0, {}, Capture) == EValueError::InvalidValue, "wrong value size accepted");
    Check(CaptureValue(Plan(0, 1, {}), Heap, {}, 0, {}, Capture) == EValueError::Ok && Capture.GetBytes().empty(),
        "empty value could not replace scalar result");
}
void ReferenceGraphsAndAliases()
{
    FHeap Heap; Configure(Heap); FCapturedValue Capture; const auto Producer = Frame(Heap);
    const auto Parent = Object(Heap, Producer), Child = Object(Heap, Producer);
    Ok(Heap.WriteReference(Parent, 1, 0, Child));
    auto Value = PairPlan(); std::vector<std::uint8_t> Bytes(16, 0); Token(Bytes, 0, Parent); Token(Bytes, 8, Parent);
    Check(CaptureValue(Value, Heap, Bytes, 0, Resolver, Capture) == EValueError::Ok && Capture.GetRootCount() == 1,
        "aliases must retain one object root");
    Bytes[0] = 0; Ok(Heap.PopFrame(Producer)); Ok(Heap.Collect());
    Check(Heap.IsAlive(Parent) && Heap.IsAlive(Child), "captured graph did not survive producer exit");
    const auto Consumer = Frame(Heap); Ok(Heap.RootObjectInCurrentFrame(Parent, 0));
    auto Roots = Capture.TakeRoots(); Roots.Reset(); Ok(Heap.Collect());
    Check(Heap.IsAlive(Parent) && Heap.IsAlive(Child), "reader frame failed to take over graph ownership");
    Ok(Heap.PopFrame(Consumer)); Ok(Heap.Collect());
    Check(Heap.GetStats().LiveObjects == 0 && Heap.GetStats().LiveRoots == 0, "captured graph leaked");
}
void ReferenceAdmissionPreservesOldValue()
{
    FHeap Heap, Other; Configure(Heap); Configure(Other);
    const auto Producer = Frame(Heap), ForeignFrame = Frame(Other);
    const auto A = Object(Heap, Producer), B = Object(Heap, Producer), WrongType = Object(Heap, Producer, 2), Foreign = Object(Other, ForeignFrame);
    FCapturedValue Capture; auto Value = PairPlan(); std::vector<std::uint8_t> Bytes(16, 0); Token(Bytes, 0, A); Token(Bytes, 8, A);
    Check(CaptureValue(Value, Heap, Bytes, 0, Resolver, Capture) == EValueError::Ok, "initial capture failed");
    const auto Before = Heap.GetStats().LiveRoots;
    for (FToken Invalid : {WrongType, Foreign, Producer, FToken(1)})
    {
        Token(Bytes, 0, B); Token(Bytes, 8, Invalid);
        Check(CaptureValue(Value, Heap, Bytes, 0, Resolver, Capture) == EValueError::InvalidObject, "wrong token or type accepted");
        Check(Capture.GetRootCount() == 1 && Heap.GetStats().LiveRoots == Before, "failed capture leaked or replaced roots");
    }
    Token(Bytes, 0, A); Token(Bytes, 8, A);
    Check(CaptureValue(Value, Heap, Bytes, 0, {}, Capture) == EValueError::UnknownManagedType, "missing catalog accepted");
    const FManagedTypeResolver WrongResolver = [](const FValueLeaf&, std::uint32_t& Type) { Type = 0; return true; };
    Check(CaptureValue(Value, Heap, Bytes, 0, WrongResolver, Capture) == EValueError::UnknownManagedType, "typed reference erased by caller");
    const auto Nested = Frame(Heap);
    Check(CaptureValue(Value, Heap, Bytes, 0, Resolver, Capture) == EValueError::RootAuthority, "outer-frame root authorized nested result");
    Ok(Heap.RootObjectInCurrentFrame(A, 1));
    Check(CaptureValue(Value, Heap, Bytes, 2, Resolver, Capture) == EValueError::RootAuthority, "invocation floor ignored");
    Ok(Heap.PopFrame(Nested)); Ok(Heap.PopFrame(Producer)); Ok(Heap.Collect());
    Check(Heap.IsAlive(A) && !Heap.IsAlive(B), "failed replacement retained incoming object");
    Token(Bytes, 8, B);
    Check(CaptureValue(Value, Heap, Bytes, 0, Resolver, Capture) != EValueError::Ok, "stale capture succeeded without a frame");
    auto Roots = Capture.TakeRoots(); Roots.Reset(); Ok(Heap.Collect());
    Check(Heap.GetStats().LiveObjects == 0, "rejection leaked old graph");
    Ok(Other.PopFrame(ForeignFrame));
}
void RootBudgetIsAtomic()
{
    FHeapLimits Limits; Limits.MaxRoots = 4; FHeap Heap(Limits); Configure(Heap);
    const auto Producer = Frame(Heap), A = Object(Heap, Producer), B = Object(Heap, Producer);
    FCapturedValue Capture; auto Value = PairPlan(); std::vector<std::uint8_t> Bytes(16, 0); Token(Bytes, 0, A); Token(Bytes, 8, A);
    Check(CaptureValue(Value, Heap, Bytes, 0, Resolver, Capture) == EValueError::Ok, "budget initial capture failed");
    const auto Before = Heap.GetStats().LiveRoots; Token(Bytes, 8, B);
    Check(CaptureValue(Value, Heap, Bytes, 0, Resolver, Capture) == EValueError::HeapFailure, "root budget ignored");
    Check(Heap.GetStats().LiveRoots == Before && Capture.GetRootCount() == 1, "partial acquisition was not rolled back");
    Ok(Heap.PopFrame(Producer)); Ok(Heap.Collect()); Check(Heap.IsAlive(A) && !Heap.IsAlive(B), "failed budget replaced result graph");
    auto Roots = Capture.TakeRoots(); Roots.Reset(); Ok(Heap.Collect()); Check(Heap.GetStats().LiveObjects == 0, "budget failure leaked");
}
void NullAndUnimplementedResources()
{
    FHeap Heap; Configure(Heap); auto Value = PairPlan(); FCapturedValue Capture; std::vector<std::uint8_t> Bytes(16, 0);
    Check(CaptureValue(Value, Heap, Bytes, 0, Resolver, Capture) == EValueError::RootAuthority, "null result bypassed invocation authority");
    const auto Producer = Frame(Heap);
    Check(CaptureValue(Value, Heap, Bytes, 0, Resolver, Capture) == EValueError::Ok && Capture.GetRootCount() == 0,
        "valid null managed references rejected");
    auto Erased = Plan(8, 8, {{0, 8, 8, ELeafKind::ManagedReference, "r:erased", ""}});
    const auto A = Object(Heap, Producer); std::vector<std::uint8_t> ErasedBytes(8, 0); Token(ErasedBytes, 0, A);
    const FManagedTypeResolver ErasedResolver = [](const FValueLeaf& Leaf, std::uint32_t& Type)
    { Type = 0; return Leaf.TypeId == "r:erased" && Leaf.TargetTypeId.empty(); };
    Check(CaptureValue(Erased, Heap, ErasedBytes, 0, ErasedResolver, Capture) == EValueError::Ok, "erased managed reference failed");
    const auto Before = Heap.GetStats().LiveRoots;
    for (auto Kind : {ELeafKind::LinearString, ELeafKind::LinearArray, ELeafKind::UeHandle, ELeafKind::ClassReference,
        ELeafKind::FactoryReference, ELeafKind::ObjectTypeReference, ELeafKind::CompositeReference, ELeafKind::FunctionReference})
    {
        const std::uint32_t Size = Kind == ELeafKind::UeHandle ? 8u : 4u;
        auto Resource = Plan(Size, Size, {{0, Size, Size, Kind, "r:resource", Kind == ELeafKind::LinearArray ? "s:i32" : ""}});
        std::vector<std::uint8_t> Null(Size, 0);
        Check(CaptureValue(Resource, Heap, Null, 0, Resolver, Capture) == EValueError::UnsupportedResource,
            "unimplemented resource silently became plain bits");
        Check(Heap.GetStats().LiveRoots == Before && Capture.GetRootCount() == 1, "unsupported resource changed result");
    }
    Ok(Heap.PopFrame(Producer)); auto Roots = Capture.TakeRoots(); Roots.Reset(); Ok(Heap.Collect());
    Check(Heap.GetStats().LiveObjects == 0, "null/resource tests leaked");
}
int CompilerWireFixtures()
{
    std::filesystem::path Directory;
#ifdef _WIN32
    wchar_t* Buffer = nullptr; std::size_t Count = 0;
    const auto Error = _wdupenv_s(&Buffer, &Count, L"AVIDSCRIPT_TASK_VALUE_PLAN_DIR");
    Check(Error == 0, "compiler fixture environment could not be read");
    if (Buffer) { Directory = Buffer; std::free(Buffer); }
#else
    if (const char* Buffer = std::getenv("AVIDSCRIPT_TASK_VALUE_PLAN_DIR")) Directory = Buffer;
#endif
    if (Directory.empty()) { std::cout << "TaskValuePlan compiler-wire fixtures: not requested\n"; return 0; }
    auto Read = [&](const char* File)
    {
        std::ifstream Stream(std::filesystem::path(Directory) / File, std::ios::binary);
        Check(Stream.good(), "compiler task value fixture is missing");
        std::vector<std::uint8_t> Bytes{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
        FValuePlan Value; Check(ReadValuePlan(Bytes, Value) == EValueError::Ok, "compiler plan rejected by Native reader"); return Value;
    };
    for (const auto& Entry : std::array<std::pair<const char*, std::uint32_t>, 6>{{
        {"scalar_i32_1.plan", 1}, {"scalar_i32_2.plan", 2}, {"scalar_i32_4.plan", 4},
        {"scalar_i64_8.plan", 8}, {"scalar_f32_4.plan", 4}, {"scalar_f64_8.plan", 8}}})
    {
        const auto Value = Read(Entry.first);
        Check(Value.GetTypeId() == "s:value" && Value.GetSize() == Entry.second && !Value.RequiresLease(), "compiler scalar layout changed");
        Check(Value.GetLeaves().size() == 1 && Value.GetLeaves()[0].Kind == (std::string(Entry.first).find("_f") != std::string::npos
            ? ELeafKind::Float : ELeafKind::Integer), "compiler scalar storage kind changed");
    }
    const auto Nested = Read("nested.plan");
    Check(Nested.GetTypeId() == "v:result" && Nested.GetSize() == 40 && Nested.GetAlignment() == 8
        && Nested.RequiresLease() && Nested.GetLeaves().size() == 5, "compiler nested result shape changed");
    const std::array<std::uint32_t, 5> Offsets{{0, 8, 16, 24, 32}};
    for (std::size_t I = 0; I < Offsets.size(); ++I) Check(Nested.GetLeaves()[I].Offset == Offsets[I], "compiler nested field moved");
    Check(Nested.GetLeaves()[2].TypeId == "r:\xe7\x8e\xa9\xe5\xae\xb6" && Nested.GetLeaves()[2].TargetTypeId == "p:node",
        "UTF-8 managed nominal identity changed across languages");
    const auto Empty = Read("empty.plan"); Check(Empty.GetSize() == 0 && Empty.GetLeaves().empty(), "compiler empty value failed");
    const std::array<std::pair<const char*, ELeafKind>, 8> Resources{{
        {"string.plan", ELeafKind::LinearString}, {"array.plan", ELeafKind::LinearArray}, {"handle.plan", ELeafKind::UeHandle},
        {"class_ref.plan", ELeafKind::ClassReference}, {"factory_ref.plan", ELeafKind::FactoryReference},
        {"object_type_ref.plan", ELeafKind::ObjectTypeReference}, {"composite_ref.plan", ELeafKind::CompositeReference},
        {"function_ref.plan", ELeafKind::FunctionReference}}};
    for (const auto& Entry : Resources)
    {
        const auto Value = Read(Entry.first);
        Check(Value.RequiresLease() && Value.GetLeaves().size() == 1 && Value.GetLeaves()[0].Kind == Entry.second,
            "compiler resource kind degraded during Native parse");
    }
    std::cout << "AvidScript.TaskValuePlan.CompilerWire: 16/16 read\n";
    return 1;
}
}

int RunTaskValueTests()
{
    using namespace AvidScriptTaskValueTestsPrivate;
    ParseAndAtomicRejection(); InvalidLeafShapes(); IdentityEncoding(); ExactScalarBitsAndPadding();
    ReferenceGraphsAndAliases(); ReferenceAdmissionPreservesOldValue(); RootBudgetIsAtomic(); NullAndUnimplementedResources();
    return 8 + CompilerWireFixtures();
}
