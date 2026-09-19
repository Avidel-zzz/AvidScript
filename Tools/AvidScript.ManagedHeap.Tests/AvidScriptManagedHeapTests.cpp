#include "Memory/AvidScriptManagedHeap.h"
#include <array>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

using namespace AvidScript::Managed;
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
}
int main()
{
	try
	{
		SharedEscapingCells(); Cycles(); TokensAndGenerations(); FramesAndUnwind(); RootListReuse();
		TypedReferencesAndRanges(); AllocationLimits(); InvalidLayouts(); GenerationRetirement(); GraphOracle();
		std::cout << "AvidScript.ManagedHeap.Tests: 10/10 passed (4000 graph-oracle steps; full root generation retirement)\n";
		return 0;
	}
	catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
