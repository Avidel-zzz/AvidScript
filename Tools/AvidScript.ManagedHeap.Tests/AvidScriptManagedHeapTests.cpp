#include "Memory/AvidScriptManagedHeap.h"
#include "Memory/AvidScriptManagedHeapProtocol.h"
#include <array>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

using namespace AvidScript::Managed;
int RunManagedStaticTests();
namespace
{
const std::array<FHeapLayout, 3> Layouts{{{1, 16, {{0, 0}}}, {2, 8, {}}, {3, 16, {{0, 1}}}}};
void Check(bool Condition, const char* Message) { if (!Condition) throw std::runtime_error(Message); }
void Ok(EHeapError Error) { Check(Error == EHeapError::Ok, "unexpected heap error"); }
FToken Root(FHeap& Heap, FToken Frame = 0) { FToken Value = 0; Ok(Heap.CreateRoot(Frame, 0, Value)); return Value; }
FToken Allocate(FHeap& Heap, FToken RootToken, std::uint32_t Type = 1) { FToken Value = 0; Ok(Heap.Allocate(Type, RootToken, Value)); return Value; }
FToken Frame(FHeap& Heap) { FToken Value = 0; Ok(Heap.PushFrame(Value)); return Value; }
void WriteNumber(FHeap& Heap, FToken Object, std::uint64_t Value)
{
	std::array<std::uint8_t, 8> Bytes{};
	for (unsigned I = 0; I < 8; ++I) Bytes[I] = std::uint8_t(Value >> (I * 8));
	Ok(Heap.WriteBytes(Object, 1, 8, Bytes));
}
std::uint64_t ReadNumber(FHeap& Heap, FToken Object)
{
	std::array<std::uint8_t, 8> Bytes{};
	Ok(Heap.ReadBytes(Object, 1, 8, Bytes));
	std::uint64_t Value = 0;
	for (unsigned I = 0; I < 8; ++I) Value |= std::uint64_t(Bytes[I]) << (I * 8);
	return Value;
}
void SharedEscapingCells()
{
	FHeap Heap; Ok(Heap.Configure(Layouts));
	const auto OwnerFrame = Frame(Heap), LocalRoot = Root(Heap, OwnerFrame), Cell = Allocate(Heap, LocalRoot);
	WriteNumber(Heap, Cell, 10);
	const auto FirstRoot = Root(Heap), SecondRoot = Root(Heap);
	const auto First = Allocate(Heap, FirstRoot, 3), Second = Allocate(Heap, SecondRoot, 3);
	Ok(Heap.WriteReference(First, 3, 0, Cell)); Ok(Heap.WriteReference(Second, 3, 0, Cell));
	Ok(Heap.PopFrame(OwnerFrame)); Ok(Heap.Collect());
	for (auto Delegate : {First, Second, First})
	{
		FToken Captured = 0; Ok(Heap.ReadReference(Delegate, 3, 0, Captured));
		Check(Captured == Cell, "delegates must share identity after owner returns");
		WriteNumber(Heap, Captured, ReadNumber(Heap, Captured) + 1);
	}
	Check(ReadNumber(Heap, Cell) == 13, "shared writes were copied");
	Ok(Heap.ReleaseRoot(FirstRoot)); Ok(Heap.Collect());
	Check(!Heap.IsAlive(First) && Heap.IsAlive(Cell) && Heap.IsAlive(Second), "second delegate must retain shared cell");
	Ok(Heap.ReleaseRoot(SecondRoot)); Ok(Heap.Collect());
	Check(Heap.GetStats().LiveObjects == 0 && Heap.GetStats().LiveBytes == 0, "escaping graph leaked");
}
void Cycles()
{
	FHeap Heap; Ok(Heap.Configure(Layouts));
	const auto ABase = Root(Heap), BBase = Root(Heap), A = Allocate(Heap, ABase), B = Allocate(Heap, BBase);
	Ok(Heap.WriteReference(A, 1, 0, B)); Ok(Heap.WriteReference(B, 1, 0, A));
	Ok(Heap.ReleaseRoot(BBase)); Ok(Heap.Collect());
	Check(Heap.IsAlive(A) && Heap.IsAlive(B), "reachable cycle reclaimed");
	Ok(Heap.SetRoot(ABase, 0)); Ok(Heap.Collect());
	Check(!Heap.IsAlive(A) && !Heap.IsAlive(B), "unreachable cycle leaked");
	const auto Self = Allocate(Heap, ABase); Ok(Heap.WriteReference(Self, 1, 0, Self));
	Ok(Heap.ReleaseRoot(ABase)); Ok(Heap.Collect());
	Check(!Heap.IsAlive(Self), "self cycle leaked");
}
void TokensAndGenerations()
{
	FHeap Heap, Other; Ok(Heap.Configure(Layouts)); Ok(Other.Configure(Layouts));
	const auto F = Frame(Heap), R = Root(Heap, F), O = Allocate(Heap, R);
	Check(!Heap.IsAlive(R) && !Heap.IsAlive(F) && !Other.IsAlive(O), "token domains or owners alias");
	FToken Value = 0;
	Check(Other.CreateRoot(0, O, Value) == EHeapError::InvalidObject, "foreign root accepted");
	Check(Heap.Allocate(1, O, Value) == EHeapError::InvalidRoot, "object used as root");
	Ok(Heap.SetRoot(R, 0)); Ok(Heap.Collect());
	const auto Replacement = Allocate(Heap, R);
	Check(Replacement != O && !Heap.IsAlive(O), "recycled slot resurrected stale token");
	Check(Heap.SetRoot(R, O) == EHeapError::InvalidObject, "stale root update accepted");
	Ok(Heap.Collect()); Check(Heap.IsAlive(Replacement), "failed root write modified root");
	Ok(Heap.PopFrame(F));
	Check(Heap.ReleaseRoot(R) == EHeapError::InvalidRoot && Heap.PopFrame(F) == EHeapError::InvalidFrame, "stale frame/root accepted");
	const auto NextFrame = Frame(Heap), NextRoot = Root(Heap, NextFrame);
	Check(NextFrame != F && NextRoot != R, "root/frame generation did not advance");
}
void FramesAndUnwind()
{
	FHeap Heap; Ok(Heap.Configure(Layouts));
	const auto Outer = Frame(Heap), A = Allocate(Heap, Root(Heap, Outer));
	const auto Inner = Frame(Heap), B = Allocate(Heap, Root(Heap, Inner));
	const auto Saved = Root(Heap); Ok(Heap.SetRoot(Saved, B));
	Check(Heap.PopFrame(Outer) == EHeapError::FrameOrder, "out of order frame release accepted");
	Ok(Heap.UnwindToDepth(1)); Ok(Heap.Collect());
	Check(Heap.IsAlive(A) && Heap.IsAlive(B) && Heap.GetStats().ActiveFrames == 1, "reentrant unwind dropped owner roots");
	Ok(Heap.UnwindToDepth(0)); Ok(Heap.Collect());
	Check(!Heap.IsAlive(A) && Heap.IsAlive(B), "persistent continuation root not retained");
	Ok(Heap.ReleaseRoot(Saved)); Ok(Heap.Collect());
	Check(Heap.GetStats().LiveObjects == 0, "unwind graph leaked");
}
void RootListReuse()
{
	FHeap Heap; Ok(Heap.Configure(Layouts)); const auto F = Frame(Heap);
	const auto A = Root(Heap, F), B = Root(Heap, F), C = Root(Heap, F);
	Ok(Heap.ReleaseRoot(B)); const auto D = Root(Heap, F);
	Ok(Heap.ReleaseRoot(A)); Ok(Heap.ReleaseRoot(C));
	Allocate(Heap, D); Ok(Heap.PopFrame(F)); Ok(Heap.Collect());
	Check(Heap.GetStats().LiveRoots == 0 && Heap.GetStats().LiveObjects == 0, "intrusive root list was corrupted by slot reuse");
}
void TypedReferencesAndRanges()
{
	FHeap Heap; Ok(Heap.Configure(Layouts));
	const auto R = Root(Heap), Node = Allocate(Heap, R), Scalar = Allocate(Heap, Root(Heap), 2), Typed = Allocate(Heap, Root(Heap), 3);
	Ok(Heap.WriteReference(Typed, 3, 0, Node));
	Check(Heap.WriteReference(Typed, 3, 0, Scalar) == EHeapError::ReferenceTypeMismatch, "wrong target type accepted");
	std::array<std::uint8_t, 8> Bytes{};
	Check(Heap.WriteBytes(Typed, 3, 0, Bytes) == EHeapError::ReferenceOverlap, "raw bytes overwrote traced reference");
	Check(Heap.WriteBytes(Typed, 3, 7, Bytes) == EHeapError::ReferenceOverlap, "partial reference overwrite accepted");
	Check(Heap.ReadBytes(Node, 1, 0xffffffffu, Bytes) == EHeapError::InvalidRange, "range overflow accepted");
	Check(Heap.WriteReference(Node, 1, 8, Scalar) == EHeapError::InvalidReferenceField, "scalar field treated as reference");
	Check(Heap.ReadBytes(Node, 2, 8, Bytes) == EHeapError::InvalidObject, "wrong owner type accepted");
	FToken Ref = 0; Ok(Heap.ReadReference(Typed, 3, 0, Ref)); Check(Ref == Node, "failed stores changed reference");
	Check(ReadNumber(Heap, Node) == 0, "allocation is not zero initialized");
}
void AllocationLimits()
{
	FHeapLimits Limits; Limits.MaxObjects = 2; Limits.MaxLiveBytes = 32; Limits.MaxRoots = 2; Limits.MaxFrames = 1;
	FHeap Heap(Limits); Ok(Heap.Configure(Layouts));
	const auto F = Frame(Heap), R = Root(Heap, F), Other = Root(Heap), A = Allocate(Heap, R), B = Allocate(Heap, Other);
	FToken Value = 123;
	Check(Heap.Allocate(1, R, Value) == EHeapError::ByteLimit && Value == 0, "exhaustion must be explicit");
	Check(Heap.GetStats().LiveObjects == 2 && Heap.IsAlive(A) && Heap.IsAlive(B), "failed allocation changed roots");
	Check(Heap.CreateRoot(0, 0, Value) == EHeapError::RootLimit && Heap.PushFrame(Value) == EHeapError::FrameLimit, "metadata budget ignored");
	Ok(Heap.SetRoot(Other, 0)); const auto C = Allocate(Heap, R);
	Check(Heap.IsAlive(A) && Heap.IsAlive(C) && !Heap.IsAlive(B), "allocation-triggered collection lost existing root");
	Check(Heap.GetStats().PeakLiveBytes <= 32, "payload budget exceeded");
	Limits.MaxObjects = 1; Limits.MaxLiveBytes = 64;
	FHeap Slots(Limits); Ok(Slots.Configure(Layouts)); const auto SlotRoot = Root(Slots); Allocate(Slots, SlotRoot);
	Check(Slots.Allocate(1, SlotRoot, Value) == EHeapError::ObjectLimit, "object slot budget ignored");
}
void InvalidLayouts()
{
	for (const FHeapLayout Bad : std::vector<FHeapLayout>{{0, 8, {}}, {1, 0, {}}, {1, 8, {{4, 0}}}, {1, 8, {{0, 99}}}, {1, 8, {{0, 0}, {0, 0}}}, {1, 8, {{0xfffffff8u, 0}}}})
	{
		FHeap Heap; const std::array Candidate{Bad};
		Check(Heap.Configure(Candidate) == EHeapError::InvalidLayout, "invalid layout accepted");
		Ok(Heap.Configure(Layouts));
	}
	FHeap Heap; const std::array Duplicate{Layouts[0], Layouts[0]};
	Check(Heap.Configure(Duplicate) == EHeapError::InvalidLayout, "duplicate layout accepted");
	Ok(Heap.Configure(Layouts)); Check(Heap.Configure(Layouts) == EHeapError::AlreadyConfigured, "live type table replaced");
	FHeapLimits BadLimits; BadLimits.MaxObjects = 0; FHeap Invalid(BadLimits);
	Check(Invalid.Configure(Layouts) == EHeapError::InvalidLimits, "invalid limits accepted");
	FHeapLimits LayoutBudget; LayoutBudget.MaxTotalReferences = 1; FHeap Bounded(LayoutBudget);
	Check(Bounded.Configure(Layouts) == EHeapError::InvalidLayout, "aggregate descriptor reference budget ignored");
}
void GenerationRetirement()
{
	FHeapLimits Limits; Limits.MaxRoots = 1; FHeap Heap(Limits); Ok(Heap.Configure(Layouts));
	const auto First = Root(Heap); Ok(Heap.ReleaseRoot(First));
	for (std::uint32_t I = 1; I < (1u << 22) - 1; ++I) { const auto Current = Root(Heap); Ok(Heap.ReleaseRoot(Current)); }
	FToken Value = 1;
	Check(Heap.CreateRoot(0, 0, Value) == EHeapError::RootLimit && Value == 0, "generation wrap must retire the slot");
	Check(Heap.SetRoot(First, 0) == EHeapError::InvalidRoot, "generation wrap resurrected first root");
}
void GraphOracle()
{
	FHeap Heap; Ok(Heap.Configure(Layouts)); std::mt19937 Random(57);
	std::array<FToken, 8> RootTokens{}, RootValues{};
	for (auto& R : RootTokens) R = Root(Heap);
	std::map<FToken, FToken> Edges;
	for (int Step = 0; Step < 4000; ++Step)
	{
		const auto R = Random() % RootTokens.size();
		if (Step % 3 == 0 || Edges.empty()) { const auto O = Allocate(Heap, RootTokens[R]); RootValues[R] = O; Edges[O] = 0; }
		else
		{
			auto Source = Edges.begin(), Target = Edges.begin();
			std::advance(Source, Random() % Edges.size()); std::advance(Target, Random() % Edges.size());
			if (Step % 3 == 1) { Ok(Heap.WriteReference(Source->first, 1, 0, Target->first)); Source->second = Target->first; }
			else { RootValues[R] = Random() % 2 ? Target->first : 0; Ok(Heap.SetRoot(RootTokens[R], RootValues[R])); }
		}
		if (Step % 7 != 0) continue;
		std::set<FToken> Reachable;
		for (auto O : RootValues) while (O && Reachable.insert(O).second) O = Edges.at(O);
		Ok(Heap.Collect()); Check(Heap.GetStats().LiveObjects == Reachable.size(), "collector disagrees with graph oracle");
		for (auto It = Edges.begin(); It != Edges.end();)
		{
			Check(Heap.IsAlive(It->first) == Reachable.contains(It->first), "object liveness disagrees with oracle");
			if (!Reachable.contains(It->first)) It = Edges.erase(It); else ++It;
		}
	}
	Heap.Close(); Heap.Close(); Check(Heap.GetStats().LiveObjects == 0 && Heap.GetStats().LiveRoots == 0, "idempotent close leaked graph");
	FToken Value = 1; Check(Heap.CreateRoot(0, 0, Value) == EHeapError::Closed && Value == 0, "closed heap re-entered");
}

using FPacket = std::vector<std::uint8_t>;
void Wire(FPacket& Bytes, std::uint64_t Value, unsigned Size = 4)
{
	for (unsigned I = 0; I < Size; ++I) Bytes.push_back(static_cast<std::uint8_t>(Value >> (8 * I)));
}
FPacket Packet(Abi::ECommand Command)
{
	FPacket Bytes; Wire(Bytes, Abi::Magic); Wire(Bytes, static_cast<std::uint32_t>(Command)); return Bytes;
}
FToken RunPacket(FHeap& Heap, const FPacket& Bytes, bool HasToken = false)
{
	std::array<std::uint8_t, 8> Out{};
	Check(ExecuteHeapCommand(Heap, Bytes, HasToken ? std::span(Out) : std::span<std::uint8_t>{}, 0).Succeeded(), "wire command failed");
	FToken Value = 0; for (unsigned I = 0; I < 8; ++I) Value |= FToken(Out[I]) << (I * 8); return Value;
}
FPacket ConfigurePacket()
{
	auto P = Packet(Abi::ECommand::Configure);
	Wire(P, 1); Wire(P, 1); Wire(P, 16); Wire(P, 1); Wire(P, 0); Wire(P, 1); return P;
}
void ProtocolExecution()
{
	FHeap Heap; RunPacket(Heap, ConfigurePacket());
	const auto F = RunPacket(Heap, Packet(Abi::ECommand::PushFrame), true);
	auto P = Packet(Abi::ECommand::CreateRoot); Wire(P, F, 8); Wire(P, 0, 8);
	const auto R = RunPacket(Heap, P, true);
	P = Packet(Abi::ECommand::Allocate); Wire(P, 1); Wire(P, R, 8); const auto O = RunPacket(Heap, P, true);
	P = Packet(Abi::ECommand::WriteBytes); Wire(P, O, 8); Wire(P, 1); Wire(P, 8); Wire(P, 8); Wire(P, 0x123456789abcdef0, 8);
	RunPacket(Heap, P);
	P = Packet(Abi::ECommand::ReadBytes); Wire(P, O, 8); Wire(P, 1); Wire(P, 8); Wire(P, 8);
	Check(RunPacket(Heap, P, true) == 0x123456789abcdef0, "wire byte read/write lost endian or high bits");
	P = Packet(Abi::ECommand::WriteReference); Wire(P, O, 8); Wire(P, 1); Wire(P, 0); Wire(P, O, 8); RunPacket(Heap, P);
	P = Packet(Abi::ECommand::ReadReference); Wire(P, O, 8); Wire(P, 1); Wire(P, 0);
	Check(RunPacket(Heap, P, true) == O, "wire reference identity changed");
	P = Packet(Abi::ECommand::CreateRoot); Wire(P, 0, 8); Wire(P, O, 8); const auto Persistent = RunPacket(Heap, P, true);
	P = Packet(Abi::ECommand::PopFrame); Wire(P, F, 8);
	Check(ExecuteHeapCommand(Heap, P, {}, 1).Error == EHeapProtocolError::FrameBoundary, "nested call popped caller frame");
	RunPacket(Heap, P); RunPacket(Heap, Packet(Abi::ECommand::Collect));
	Check(Heap.IsAlive(O) && Heap.GetStats().LiveRoots == 1, "persistent reference did not escape frame");
	P = Packet(Abi::ECommand::SetRoot); Wire(P, Persistent, 8); Wire(P, 0, 8); RunPacket(Heap, P);
	RunPacket(Heap, Packet(Abi::ECommand::Collect)); Check(!Heap.IsAlive(O), "wire self cycle leaked");
	P = Packet(Abi::ECommand::ReleaseRoot); Wire(P, Persistent, 8); RunPacket(Heap, P);
}
void ProtocolRejections()
{
	const auto Config = ConfigurePacket();
	for (std::size_t Size = 0; Size < Config.size(); ++Size)
	{
		FHeap Heap;
		Check(!ExecuteHeapCommand(Heap, std::span(Config).first(Size), {}, 0).Succeeded(), "truncated descriptor accepted");
		RunPacket(Heap, Config); // Failed parse must not partially configure the heap.
	}
	FHeap Heap; auto Bad = Config; Bad.push_back(0);
	Check(!ExecuteHeapCommand(Heap, Bad, {}, 0).Succeeded(), "trailing descriptor byte accepted");
	Bad = Config; Bad[0] ^= 1;
	Check(ExecuteHeapCommand(Heap, Bad, {}, 0).Error == EHeapProtocolError::InvalidVersion, "bad magic accepted");
	Bad = Packet(static_cast<Abi::ECommand>(0xffffffff));
	Check(ExecuteHeapCommand(Heap, Bad, {}, 0).Error == EHeapProtocolError::UnknownCommand, "unknown operation accepted");
	RunPacket(Heap, Config);
	std::array<std::uint8_t, 8> Out; Out.fill(0xa5);
	auto P = Packet(Abi::ECommand::PushFrame);
	Check(ExecuteHeapCommand(Heap, P, std::span(Out).first(7), 0).Error == EHeapProtocolError::InvalidOutput, "short output accepted");
	Check(Heap.GetStats().ActiveFrames == 0 && Out[0] == 0xa5, "short output mutated heap or response");
	P = Packet(Abi::ECommand::Allocate); Wire(P, 1); Wire(P, 999, 8);
	Check(ExecuteHeapCommand(Heap, P, Out, 0).HeapError == EHeapError::InvalidRoot && Out[0] == 0xa5, "failed allocation changed response");
	P = Packet(Abi::ECommand::ReadBytes); Wire(P, 0, 8); Wire(P, 1); Wire(P, 0); Wire(P, 0xffffffff);
	Check(ExecuteHeapCommand(Heap, P, {}, 0).Error == EHeapProtocolError::LimitExceeded, "oversized byte count accepted");
}
void ProtocolRootsOnly()
{
	const auto Configuration = Packet(Abi::ECommand::ConfigureRootsOnly);
	FHeapLimits Limits; Limits.MaxFrames = 2; Limits.MaxRoots = 2;
	FHeap Heap(Limits);
	auto Bad = Configuration; Bad.push_back(0);
	Check(ExecuteHeapCommand(Heap, Bad, {}, 0).Error == EHeapProtocolError::InvalidPacket, "roots-only configure accepted trailing data");
	std::array<std::uint8_t, 8> Output{};
	Check(ExecuteHeapCommand(Heap, Configuration, Output, 0).Error == EHeapProtocolError::InvalidOutput, "roots-only configure accepted an output buffer");
	Bad = Packet(Abi::ECommand::Configure); Wire(Bad, 0);
	Check(ExecuteHeapCommand(Heap, Bad, {}, 0).Error == EHeapProtocolError::LimitExceeded, "legacy configure accepted empty layouts");
	RunPacket(Heap, Configuration);
	Check(Heap.Configure(Layouts) == EHeapError::AlreadyConfigured && Heap.ConfigureRootsOnly() == EHeapError::AlreadyConfigured,
		"roots-only configuration gained allocation authority or was reconfigured");
	const auto Outer = Frame(Heap), OuterRoot = Root(Heap, Outer), Inner = Frame(Heap), InnerRoot = Root(Heap, Inner);
	FToken Value = 123;
	Check(Heap.Allocate(1, InnerRoot, Value) == EHeapError::InvalidType && Value == 0, "roots-only heap allocated an object");
	Check(Heap.PushFrame(Value) == EHeapError::FrameLimit && Heap.CreateRoot(0, 0, Value) == EHeapError::RootLimit, "roots-only quotas ignored");
	FHeap Foreign; Ok(Foreign.Configure(Layouts)); const auto Object = Allocate(Foreign, Root(Foreign));
	Check(Heap.SetRoot(OuterRoot, Object) == EHeapError::InvalidObject, "roots-only heap imported a foreign object");
	Ok(Heap.SetRoot(OuterRoot, 0)); Ok(Heap.Collect());
	auto Pop = Packet(Abi::ECommand::PopFrame); Wire(Pop, Inner, 8);
	Check(ExecuteHeapCommand(Heap, Pop, {}, 2).Error == EHeapProtocolError::FrameBoundary, "roots-only call popped its caller frame");
	Ok(Heap.UnwindToDepth(1));
	Check(Heap.SetRoot(InnerRoot, 0) == EHeapError::InvalidRoot, "unwound roots remained usable");
	Ok(Heap.PopFrame(Outer));
	Check(Heap.GetStats().LiveRoots == 0 && Heap.GetStats().ActiveFrames == 0 && Heap.GetStats().Allocations == 0,
		"roots-only execution retained frames or allocated placeholder objects");
	Heap.Close(); Check(Heap.ConfigureRootsOnly() == EHeapError::Closed, "closed roots-only heap was resurrected");
}
void ProtocolRootAuthority()
{
	FHeapLimits Limits; Limits.MaxRoots = 8;
	FHeap Heap(Limits), Foreign; Ok(Heap.Configure(Layouts)); Ok(Foreign.Configure(Layouts));
	const auto Persistent = Root(Heap), Caller = Frame(Heap), Returned = Root(Heap, Caller), Private = Root(Heap, Caller);
	const auto Old = Allocate(Heap, Private);
	const std::array<FToken, 2> Aliased{Returned, Returned};
	Ok(Heap.ValidateRootTransfer(Aliased));
	const auto ForeignRoot = Root(Foreign), Stale = Root(Heap, Caller); Ok(Heap.ReleaseRoot(Stale));
	for (const auto Invalid : {FToken{0}, ForeignRoot, Stale})
		Check(Heap.ValidateRootTransfer({&Invalid, 1}) == EHeapError::InvalidRoot, "invalid root transfer accepted");
	Check(Heap.ValidateRootTransfer({&Persistent, 1}) == EHeapError::RootAuthority, "persistent root transferred");
	const std::array<FToken, 9> Oversized{};
	Check(Heap.ValidateRootTransfer(Oversized) == EHeapError::RootLimit, "unbounded root transfer accepted");
	const auto Callee = Frame(Heap), Local = Root(Heap, Callee), Fresh = Allocate(Heap, Local);
	WriteNumber(Heap, Fresh, 99);
	Check(Heap.ValidateRootTransfer({&Returned, 1}) == EHeapError::RootAuthority, "callee reloaned ancestor root");
	Ok(Heap.ValidateRootTransfer({&Local, 1}));
	Ok(Heap.ValidateRootTransfer({&Local, 1}, 1));
	Check(Heap.ValidateRootTransfer({&Local, 1}, 2) == EHeapError::RootAuthority,
		"unframed nested VM entry reloaned its caller root");
	auto Set = [&](FToken RootToken, FToken Object) {
		auto P = Packet(Abi::ECommand::SetRoot); Wire(P, RootToken, 8); Wire(P, Object, 8); return P;
	};
	for (const auto RootToken : {Returned, Private, Persistent})
		Check(ExecuteHeapCommand(Heap, Set(RootToken, Fresh), {}, 1).HeapError == EHeapError::RootAuthority,
			"nested call updated an ungranted root");
	Check(ExecuteHeapCommand(Heap, Set(Private, Fresh), {}, 1, Aliased).HeapError == EHeapError::RootAuthority,
		"loan authorized another root");
	std::array<std::uint8_t, 8> Output; Output.fill(0xa5); const auto Untouched = Output;
	const auto RootsBefore = Heap.GetStats().LiveRoots;
	const auto AllocationsBefore = Heap.GetStats().Allocations;
	for (const auto TargetFrame : {FToken{0}, Caller})
	{
		auto P = Packet(Abi::ECommand::CreateRoot); Wire(P, TargetFrame, 8); Wire(P, Fresh, 8);
		Check(ExecuteHeapCommand(Heap, P, Output, 1, Aliased).HeapError == EHeapError::RootAuthority,
			"callee created persistent or caller-owned root");
	}
	auto P = Packet(Abi::ECommand::Allocate); Wire(P, 1); Wire(P, Returned, 8);
	Check(ExecuteHeapCommand(Heap, P, Output, 1, Aliased).HeapError == EHeapError::RootAuthority, "loan allowed allocation");
	P = Packet(Abi::ECommand::ReleaseRoot); Wire(P, Returned, 8);
	Check(ExecuteHeapCommand(Heap, P, {}, 1, Aliased).HeapError == EHeapError::RootAuthority, "loan allowed release");
	Check(Output == Untouched && Heap.GetStats().LiveRoots == RootsBefore && Heap.GetStats().Allocations == AllocationsBefore,
		"rejected authority changed output or heap");
	auto Bad = Set(Returned, Fresh); Bad.push_back(0);
	Check(ExecuteHeapCommand(Heap, Bad, {}, 1, Aliased).Error == EHeapProtocolError::InvalidPacket, "loan bypassed packet validation");
	Check(ExecuteHeapCommand(Heap, Set(Returned, Fresh), Output, 1, Aliased).Error == EHeapProtocolError::InvalidOutput,
		"loan bypassed output validation");
	Check(ExecuteHeapCommand(Heap, Set(Returned, Fresh), {}, 1, Aliased).Succeeded(), "authorized return root update rejected");
	Check(ExecuteHeapCommand(Heap, Set(Local, Fresh), {}, 1).Succeeded(), "callee lost its own root authority");
	const auto Grandchild = Frame(Heap);
	Check(ExecuteHeapCommand(Heap, Set(Returned, Fresh), {}, 2, Aliased).HeapError == EHeapError::RootAuthority,
		"grandchild inherited an ancestor root loan");
	Check(ExecuteHeapCommand(Heap, Set(Local, Fresh), {}, 2, {&Local, 1}).Succeeded(), "immediate caller transfer rejected");
	Ok(Heap.PopFrame(Grandchild)); Ok(Heap.PopFrame(Callee)); Ok(Heap.Collect());
	Check(Heap.IsAlive(Old) && Heap.IsAlive(Fresh) && ReadNumber(Heap, Fresh) == 99,
		"callee unwind or host GC lost returned object or private caller state");
	Check(ExecuteHeapCommand(Heap, Set(Returned, 0), {}, 1).HeapError == EHeapError::RootAuthority,
		"a completed loan remained authorized without its scope");
	const auto ReusedFrame = Frame(Heap), ReusedRoot = Root(Heap, ReusedFrame);
	Check(ReusedFrame != Callee && ReusedRoot != Local, "frame/root slot reuse lost generation");
	Check(ExecuteHeapCommand(Heap, Set(Local, Fresh), {}, 1, {&Local, 1}).HeapError == EHeapError::InvalidRoot,
		"stale loan gained authority through slot reuse");
	Check(ExecuteHeapCommand(Heap, Set(ReusedRoot, Fresh), {}, 1).Succeeded(), "reused frame has incorrect depth");
	Ok(Heap.UnwindToDepth(0)); Ok(Heap.ReleaseRoot(Persistent)); Ok(Heap.Collect());
	Check(Heap.GetStats().LiveRoots == 0 && Heap.GetStats().LiveObjects == 0, "return transfer leaked roots or objects");
}
void PersistentRootsOwnGraphs()
{
	FHeap Heap; Ok(Heap.Configure(Layouts));
	const auto F = Frame(Heap), A = Allocate(Heap, Root(Heap, F)), B = Allocate(Heap, Root(Heap, F));
	Ok(Heap.WriteReference(A, 1, 0, B)); Ok(Heap.WriteReference(B, 1, 0, A));
	FPersistentRoots Lease;
	const std::array<FToken, 3> Objects{A, 0, A};
	Ok(Heap.RetainPersistent(Objects, Lease));
	Check(Lease.IsValidFor(Heap) && Lease.Count() == 2, "lease must retain non-null roots without copying objects");
	Ok(Heap.PopFrame(F)); Ok(Heap.Collect());
	Check(Heap.IsAlive(A) && Heap.IsAlive(B) && Heap.GetStats().ActiveFrames == 0, "lease lost cyclic graph after stack exit");
	FPersistentRoots Moved(std::move(Lease)); Lease.Reset();
	Check(!Lease.IsValidFor(Heap) && Moved.IsValidFor(Heap), "move did not transfer unique root ownership");
	FPersistentRoots Destination; Ok(Heap.RetainPersistent({&B, 1}, Destination));
	Destination = std::move(Moved);
	Check(Heap.GetStats().LiveRoots == 2, "move assignment leaked its previous roots");
	Destination.Reset(); Destination.Reset(); Ok(Heap.Collect());
	Check(Heap.GetStats().LiveRoots == 0 && Heap.GetStats().LiveObjects == 0, "lease release leaked cyclic graph");
	const auto AutoFrame = Frame(Heap), AutoObject = Allocate(Heap, Root(Heap, AutoFrame));
	{
		FPersistentRoots Scoped; Ok(Heap.RetainPersistent({&AutoObject, 1}, Scoped));
		Ok(Heap.PopFrame(AutoFrame)); Ok(Heap.Collect());
		Check(Heap.IsAlive(AutoObject), "scoped owner failed to retain object");
	}
	Ok(Heap.Collect());
	Check(Heap.GetStats().LiveRoots == 0 && !Heap.IsAlive(AutoObject), "owner destruction without Reset leaked roots");
	{ FPersistentRoots Empty; Ok(Heap.RetainPersistent({}, Empty)); Check(Empty.IsValidFor(Heap), "empty successful lease lost owner identity"); }
}
void PersistentRootAcquisitionIsAtomic()
{
	FHeapLimits Limits; Limits.MaxRoots = 4;
	FHeap Heap(Limits), Other; Ok(Heap.Configure(Layouts)); Ok(Other.Configure(Layouts));
	const auto F = Frame(Heap), A = Allocate(Heap, Root(Heap, F)), B = Allocate(Heap, Root(Heap, F));
	const auto Foreign = Allocate(Other, Root(Other));
	FPersistentRoots Lease; Ok(Heap.RetainPersistent({&A, 1}, Lease));
	const auto Before = Heap.GetStats().LiveRoots;
	const std::array<FToken, 2> TooMany{B, A}, WrongOwner{B, Foreign}, WrongKind{B, F};
	Check(Heap.RetainPersistent(TooMany, Lease) == EHeapError::RootLimit, "root budget ignored during atomic replacement");
	Check(Heap.RetainPersistent(WrongOwner, Lease) == EHeapError::InvalidObject, "foreign object retained");
	Check(Heap.RetainPersistent(WrongKind, Lease) == EHeapError::InvalidObject, "frame token retained as object");
	Check(Lease.IsValidFor(Heap) && Lease.Count() == 1 && Heap.GetStats().LiveRoots == Before,
		"failed acquisition leaked a partial root or replaced prior lease");
	Ok(Heap.PopFrame(F)); Ok(Heap.Collect());
	Check(Heap.IsAlive(A) && !Heap.IsAlive(B), "failed replacement changed retained graph");
	Check(Heap.RetainPersistent({&B, 1}, Lease) == EHeapError::InvalidObject, "stale object retained");
	Lease.Reset(); Ok(Heap.Collect()); Check(Heap.GetStats().LiveObjects == 0, "atomic failures leaked objects");
}
void PersistentRootsOutliveHeapSafely()
{
	FPersistentRoots Late;
	alignas(FHeap) std::array<std::byte, sizeof(FHeap)> Storage;
	FHeap* Original = std::construct_at(reinterpret_cast<FHeap*>(Storage.data()));
	Ok(Original->Configure(Layouts));
	const auto Object = Allocate(*Original, Root(*Original));
	Ok(Original->RetainPersistent({&Object, 1}, Late));
	Original->Close();
	Check(!Late.IsValidFor(*Original) && Original->GetStats().LiveRoots == 0, "close did not invalidate root lease");
	std::destroy_at(Original);
	FHeap* Replacement = std::construct_at(reinterpret_cast<FHeap*>(Storage.data()));
	Ok(Replacement->Configure(Layouts));
	const auto Fresh = Allocate(*Replacement, Root(*Replacement));
	Check(!Late.IsValidFor(*Replacement), "old lease became valid for same-address heap");
	Late.Reset(); Ok(Replacement->Collect());
	Check(Replacement->IsAlive(Fresh) && Replacement->GetStats().LiveRoots == 1, "late release corrupted replacement heap");
	Ok(Replacement->RetainPersistent({&Fresh, 1}, Late));
	std::destroy_at(Replacement); // destructor, without explicit Close
	Late.Reset();
}
void ProtocolLimitsAndRanges()
{
	Check(Abi::ValidateRanges(1, 8, 9, 8), "adjacent ranges rejected");
	Check(Abi::ValidateRanges(0xfffffff8, 8, 0, 0), "valid high unsigned address rejected");
	Check(!Abi::ValidateRanges(1, 8, 8, 8) && !Abi::ValidateRanges(0xfffffff9, 8, 0, 0), "overlap or wrap accepted");
	Check(!Abi::ValidateRanges(0, 8, 0, 0) && !Abi::ValidateRanges(1, -1, 0, 0)
		&& !Abi::ValidateRanges(1, 8, 5, 0) && !Abi::ValidateRanges(1, 8, 20, -1), "noncanonical range accepted");
	FHeap Heap; auto P = Packet(Abi::ECommand::Configure); Wire(P, Abi::MaxLayouts + 1);
	Check(ExecuteHeapCommand(Heap, P, {}, 0).Error == EHeapProtocolError::LimitExceeded, "layout quota ignored");
	P = Packet(Abi::ECommand::Configure); Wire(P, 257);
	for (unsigned I = 0; I < 257; ++I)
	{
		Wire(P, I + 1); Wire(P, 2048); Wire(P, 256);
		for (unsigned J = 0; J < 256; ++J) { Wire(P, J * 8); Wire(P, 0); }
	}
	Check(ExecuteHeapCommand(Heap, P, {}, 0).Error == EHeapProtocolError::LimitExceeded, "aggregate reference quota ignored");
	RunPacket(Heap, ConfigurePacket());
}
}
int main()
{
	try
	{
		SharedEscapingCells(); Cycles(); TokensAndGenerations(); FramesAndUnwind(); RootListReuse();
		TypedReferencesAndRanges(); AllocationLimits(); InvalidLayouts(); GenerationRetirement(); GraphOracle();
		ProtocolExecution(); ProtocolRejections(); ProtocolLimitsAndRanges(); ProtocolRootsOnly(); ProtocolRootAuthority();
		PersistentRootsOwnGraphs(); PersistentRootAcquisitionIsAtomic(); PersistentRootsOutliveHeapSafely();
		const int Total = 18 + RunManagedStaticTests();
		std::cout << "AvidScript.ManagedHeap.Tests: " << Total << '/' << Total
			<< " passed (6000 graph-oracle steps; full root generation retirement; wire protocol; persistent leases; static domain roots)\n";
		return 0;
	}
	catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
