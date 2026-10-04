#include "Continuation/AvidScriptTaskValueCatalog.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace AvidScriptTaskValueCatalogTestsPrivate
{
using namespace AvidScript::TaskResult;
using namespace AvidScript::Managed;
void Check(bool Value, const char* Message) { if (!Value) throw std::runtime_error(Message); }
void Ok(EHeapError Error) { Check(Error == EHeapError::Ok, "catalog heap operation failed"); }
void U32(std::vector<std::uint8_t>& Bytes, std::size_t Offset, std::uint32_t Value)
{ for (unsigned I = 0; I < 4; ++I) Bytes[Offset + I] = std::uint8_t(Value >> (I * 8)); }
void Token(std::vector<std::uint8_t>& Bytes, std::size_t Offset, FToken Value)
{ for (unsigned I = 0; I < 8; ++I) Bytes[Offset + I] = std::uint8_t(Value >> (I * 8)); }
std::array<std::uint8_t, 32> Source() { std::array<std::uint8_t, 32> Value; Value.fill(0xaa); return Value; }
FValueCatalog Read(const std::vector<std::uint8_t>& Packet)
{ FValueCatalog Value; Check(ReadValueCatalog(Packet, "catalog-fixture", Source(), Value) == EValueError::Ok, "compiler catalog rejected"); return Value; }
void Admission(const std::vector<std::uint8_t>& Scalar, const std::vector<std::uint8_t>& Managed)
{
    auto Catalog = Read(Scalar); Check(Catalog.GetResultCount() == 2 && Catalog.Find(1)->GetTypeId() == "type:float32", "scalar ordering changed");
    for (std::size_t I = 0; I < Managed.size(); ++I)
    {
        Check(ReadValueCatalog({Managed.data(), I}, "catalog-fixture", Source(), Catalog) != EValueError::Ok, "truncated catalog accepted");
        Check(Catalog.GetResultCount() == 2 && Catalog.Find(2)->GetTypeId() == "type:int32", "failed read changed original catalog");
    }
    auto Bad = Managed; Bad.push_back(0);
    Check(ReadValueCatalog(Bad, "catalog-fixture", Source(), Catalog) == EValueError::InvalidPlan, "trailing catalog bytes accepted");
    Bad = Managed; U32(Bad, 4, 2);
    Check(ReadValueCatalog(Bad, "catalog-fixture", Source(), Catalog) == EValueError::InvalidVersion, "future catalog version accepted");
    Bad = Managed; U32(Bad, 8 + 4 + std::string("catalog-fixture").size() + 32, CatalogAbi::MaxResults + 1);
    Check(ReadValueCatalog(Bad, "catalog-fixture", Source(), Catalog) == EValueError::LimitExceeded, "catalog result budget ignored");
    Check(ReadValueCatalog(Managed, "foreign-module", Source(), Catalog) == EValueError::InvalidPlan, "foreign module identity accepted");
    auto WrongSource = Source(); WrongSource[0] ^= 1;
    Check(ReadValueCatalog(Managed, "catalog-fixture", WrongSource, Catalog) == EValueError::InvalidPlan, "foreign source identity accepted");
    Bad.assign(CatalogAbi::MaxCatalogBytes + 1, 0);
    Check(ReadValueCatalog(Bad, "catalog-fixture", Source(), Catalog) == EValueError::LimitExceeded, "catalog bytes unbounded");
    Check(!Catalog.Find(0) && !Catalog.Find(3), "invalid result ordinal resolves");
}
void LayoutAuthority(const std::vector<std::uint8_t>& Packet)
{
    const auto Catalog = Read(Packet); const auto* Plan = Catalog.Find(2);
    Check(Plan && Plan->GetTypeId() == "v:result" && Plan->GetSize() == 24, "managed result layout changed");
    FHeap Heap; const std::array<FHeapLayout, 1> Layouts{{{1, 16, {{8, 1}}}}}; Ok(Heap.Configure(Layouts));
    Check(Catalog.MatchesHeap(Heap), "original heap layout did not bind");
    FCapturedValue Value; std::vector<std::uint8_t> Scalar(4, 0); U32(Scalar, 0, 0x80000000u);
    Check(Catalog.Capture(1, Heap, Scalar, 0, Value) == EValueError::Ok, "catalog scalar capture failed");
    std::vector<std::uint8_t> Bytes(24, 0); Bytes[3] = 0x80;
    for (const auto& Layout : std::array<FHeapLayout, 3>{{{1, 24, {{8, 1}}}, {1, 16, {{0, 1}}}, {1, 16, {{8, 0}}}}})
    {
        FHeap Wrong; const std::array<FHeapLayout, 1> WrongLayouts{{Layout}}; Ok(Wrong.Configure(WrongLayouts));
        FToken Frame = 0; Ok(Wrong.PushFrame(Frame));
        Check(!Catalog.MatchesHeap(Wrong) && Catalog.Capture(2, Wrong, Bytes, 0, Value) == EValueError::UnknownManagedType,
            "matching numeric ordinal disguised a different heap payload");
        Check(Value.GetBytes().size() == 4 && Value.GetBytes()[3] == 0x80 && Wrong.GetStats().LiveRoots == 0,
            "failed layout binding replaced capture or leaked roots"); Ok(Wrong.PopFrame(Frame));
    }
    FToken Producer = 0, A = 0, B = 0, ARoot = 0, BRoot = 0; Ok(Heap.PushFrame(Producer));
    Ok(Heap.CreateRoot(Producer, 0, ARoot)); Ok(Heap.CreateRoot(Producer, 0, BRoot));
    Ok(Heap.Allocate(1, ARoot, A)); Ok(Heap.Allocate(1, BRoot, B)); Ok(Heap.WriteReference(A, 1, 8, B));
    Token(Bytes, 8, A); Token(Bytes, 16, A);
    Check(Catalog.Capture(2, Heap, Bytes, 0, Value) == EValueError::Ok && Value.GetRootCount() == 1, "catalog alias capture failed");
    Ok(Heap.PopFrame(Producer)); Ok(Heap.Collect()); Check(Heap.IsAlive(A) && Heap.IsAlive(B), "catalog result graph lost");
    FToken Consumer = 0; Ok(Heap.PushFrame(Consumer)); std::vector<std::uint8_t> Out(24, 0);
    Check(Value.ReadIntoFrame(*Plan, Heap, Out, 0) == EValueError::Ok && Out == Bytes, "catalog reader failed to take over");
    auto Roots = Value.TakeRoots(); Roots.Reset(); Ok(Heap.Collect()); Check(Heap.IsAlive(A) && Heap.IsAlive(B), "catalog reader lost nested graph");
    Ok(Heap.PopFrame(Consumer)); Ok(Heap.Collect()); Check(Heap.GetStats().LiveObjects == 0 && Heap.GetStats().LiveRoots == 0, "catalog graph leaked");
    Heap.Close(); Check(!Catalog.MatchesHeap(Heap), "closed heap retained catalog authority");
}
void MappingRejections(const std::vector<std::uint8_t>& Packet)
{
    FValueCatalog Catalog; auto Bad = Packet; U32(Bad, Bad.size() - 4, 2);
    Check(ReadValueCatalog(Bad, "catalog-fixture", Source(), Catalog) == EValueError::InvalidPlan, "unknown payload target ordinal accepted");
    Bad = Packet; U32(Bad, Bad.size() - 8, 9);
    Check(ReadValueCatalog(Bad, "catalog-fixture", Source(), Catalog) == EValueError::InvalidPlan, "misaligned payload reference accepted");
    FHeap Heap; const std::array<FHeapLayout, 2> Layouts{{{1, 16, {{8, 1}}}, {2, 16, {}}}}; Ok(Heap.Configure(Layouts));
    const std::array<FHeapLayout, 2> Duplicated{{Layouts[0], Layouts[0]}};
    Check(!Heap.MatchesLayouts(Duplicated), "duplicate expected layout hid a real type");
    Check(!Read(Packet).MatchesHeap(Heap), "extra runtime heap type bypassed complete layout check");
}
}

int RunTaskValueCatalogTests()
{
    using namespace AvidScriptTaskValueCatalogTestsPrivate;
    std::filesystem::path Directory;
#ifdef _WIN32
    wchar_t* Buffer = nullptr; std::size_t Count = 0;
    Check(_wdupenv_s(&Buffer, &Count, L"AVIDSCRIPT_TASK_VALUE_CATALOG_DIR") == 0, "catalog environment cannot be read");
    if (Buffer) { Directory = Buffer; std::free(Buffer); }
#else
    if (const char* Buffer = std::getenv("AVIDSCRIPT_TASK_VALUE_CATALOG_DIR")) Directory = Buffer;
#endif
    if (Directory.empty()) { std::cout << "Task value catalog fixtures: not requested\n"; return 0; }
    auto Load = [&](const char* Name) { std::ifstream Stream(Directory / Name, std::ios::binary);
        Check(Stream.good(), "compiler catalog fixture missing"); return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()}; };
    const auto Scalar = Load("scalar.catalog"), Managed = Load("managed.catalog");
    Admission(Scalar, Managed); LayoutAuthority(Managed); MappingRejections(Managed);
    const auto Wasm = Read(Load("scalar-wasm.catalog"));
    Check(Wasm.GetResultCount() == 1 && Wasm.Find(1)->GetTypeId() == "type:int32", "WASM encoder's original catalog changed");
    std::cout << "AvidScript.TaskValueCatalog.CompilerWire: 3/3 read\n"; return 3;
}
