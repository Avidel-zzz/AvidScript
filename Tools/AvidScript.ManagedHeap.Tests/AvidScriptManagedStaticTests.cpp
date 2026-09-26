#include "Memory/AvidScriptManagedHeap.h"
#include "Memory/AvidScriptManagedHeapProtocol.h"
#include <array>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>

using namespace AvidScript::Managed;
namespace
{
const std::array<FHeapLayout, 2> Layouts{{{1, 16, {{0, 1}}}, {2, 8, {}}}};
void Check(bool Condition, const char* Message) { if (!Condition) throw std::runtime_error(std::string("static storage: ") + Message); }
void Ok(EHeapError Error) { Check(Error == EHeapError::Ok, "unexpected heap error"); }
FToken Frame(FHeap& Heap) { FToken Value = 0; Ok(Heap.PushFrame(Value)); return Value; }
FToken Root(FHeap& Heap, FToken Owner = 0) { FToken Value = 0; Ok(Heap.CreateRoot(Owner, 0, Value)); return Value; }
FToken Allocate(FHeap& Heap, FToken Owner, std::uint32_t Type = 1) { FToken Value = 0; Ok(Heap.Allocate(Type, Owner, Value)); return Value; }
FToken Read(FHeap& Heap, std::uint32_t Slot, std::uint32_t Type = 1) { FToken Value = 0; Ok(Heap.ReadStaticSlot(Slot, Type, Value)); return Value; }

void StaticGraphsSurviveInvocations()
{
	FHeap Heap; Ok(Heap.Configure(Layouts));
	Ok(Heap.ConfigureStaticSlots(std::array<std::uint32_t, 2>{1, 0}));
	Check(Read(Heap, 1) == 0 && Read(Heap, 2, 0) == 0, "new slots must be null");
	const auto F = Frame(Heap), A = Allocate(Heap, Root(Heap, F)), B = Allocate(Heap, Root(Heap, F));
	Ok(Heap.WriteReference(A, 1, 0, B)); Ok(Heap.WriteReference(B, 1, 0, A));
	Ok(Heap.WriteStaticSlot(1, 1, A)); Ok(Heap.WriteStaticSlot(2, 0, A));
	Ok(Heap.PopFrame(F)); Ok(Heap.Collect());
	for (unsigned Invocation = 0; Invocation < 20; ++Invocation)
	{
		const auto Local = Frame(Heap), Pressure = Root(Heap, Local);
		Check(Read(Heap, 1) == A && Read(Heap, 2, 0) == A, "identity changed between invocations");
		for (unsigned I = 0; I < 24; ++I) { Allocate(Heap, Pressure); Ok(Heap.Collect()); }
		Ok(Heap.PopFrame(Local)); Ok(Heap.Collect());
		Check(Heap.IsAlive(A) && Heap.IsAlive(B) && Heap.GetStats().LiveObjects == 2
			&& Heap.GetStats().LiveRoots == 2 && Heap.GetStats().StaticRoots == 2
			&& Heap.GetStats().ActiveFrames == 0, "frame exit lost static graph or leaked temporary roots");
	}
	Ok(Heap.WriteStaticSlot(1, 1, 0)); Ok(Heap.Collect());
	Check(Heap.IsAlive(B), "clearing one alias lost the other alias");
	Ok(Heap.WriteStaticSlot(2, 0, 0)); Ok(Heap.Collect());
	Check(Heap.GetStats().LiveObjects == 0 && Heap.GetStats().LiveRoots == 2, "unreachable cycle was retained");
	Heap.Close(); Heap.Close();
	Check(Heap.GetStats().StaticRoots == 0 && Heap.GetStats().LiveRoots == 0, "close must release static roots");
}

void StaticFailuresPreserveValues()
{
	FHeap Heap, Other; FToken Value = 77;
	Check(Heap.ReadStaticSlot(1, 1, Value) == EHeapError::NotConfigured && Value == 0, "unconfigured heap read");
	Ok(Heap.Configure(Layouts)); Ok(Other.Configure(Layouts));
	Check(Heap.ReadStaticSlot(1, 1, Value) == EHeapError::StaticStorageNotConfigured, "missing static declaration accepted");
	Check(Heap.WriteStaticSlot(1, 1, 0) == EHeapError::StaticStorageNotConfigured, "unconfigured write accepted");
	Ok(Heap.ConfigureStaticSlots(std::array<std::uint32_t, 1>{1}));
	const auto F = Frame(Heap), R = Root(Heap, F), A = Allocate(Heap, R);
	const auto WrongType = Allocate(Heap, Root(Heap, F), 2), Stale = Allocate(Heap, Root(Heap, F));
	const auto Foreign = Allocate(Other, Root(Other));
	Ok(Heap.WriteStaticSlot(1, 1, A));
	Check(Heap.WriteStaticSlot(0, 1, A) == EHeapError::InvalidStaticSlot
		&& Heap.WriteStaticSlot(2, 1, A) == EHeapError::InvalidStaticSlot, "out of range write accepted");
	Check(Heap.WriteStaticSlot(1, 0, A) == EHeapError::ReferenceTypeMismatch, "erased access bypassed typed slot");
	Check(Heap.WriteStaticSlot(1, 1, WrongType) == EHeapError::ReferenceTypeMismatch, "wrong object layout accepted");
	Check(Heap.WriteStaticSlot(1, 1, Foreign) == EHeapError::InvalidObject
		&& Heap.WriteStaticSlot(1, 1, R) == EHeapError::InvalidObject, "foreign object or root token accepted");
	Value = 77;
	Check(Heap.ReadStaticSlot(1, 0, Value) == EHeapError::ReferenceTypeMismatch && Value == 0, "read mismatch leaked value");
	Check(Heap.ReadStaticSlot(0xffffffffu, 1, Value) == EHeapError::InvalidStaticSlot, "oversized slot accepted");
	Check(Read(Heap, 1) == A, "failed write changed previous value");
	Ok(Heap.UnwindToDepth(0)); Ok(Heap.Collect());
	Check(!Heap.IsAlive(Stale) && Heap.WriteStaticSlot(1, 1, Stale) == EHeapError::InvalidObject, "stale token accepted");
	Check(Read(Heap, 1) == A && Heap.GetStats().LiveObjects == 1, "unwind lost static value");
	Check(Heap.ConfigureStaticSlots(std::array<std::uint32_t, 1>{0}) == EHeapError::AlreadyConfigured
		&& Read(Heap, 1) == A, "reconfiguration reset a live slot");
}

void StaticRootBudgetIsAtomic()
{
	FHeapLimits Limits; Limits.MaxRoots = 4;
	FHeap Heap(Limits); Ok(Heap.Configure(Layouts));
	const auto F = Frame(Heap), R = Root(Heap, F), A = Allocate(Heap, R);
	Check(Heap.ConfigureStaticSlots(std::array<std::uint32_t, 4>{1, 1, 1, 1}) == EHeapError::RootLimit,
		"static storage exceeded remaining root budget");
	Check(Heap.ConfigureStaticSlots(std::array<std::uint32_t, 2>{1, 999}) == EHeapError::InvalidType,
		"unknown static type accepted");
	Check(Heap.ConfigureStaticSlots({}) == EHeapError::InvalidLayout, "empty configuration accepted");
	Check(Heap.GetStats().LiveRoots == 1 && Heap.GetStats().StaticRoots == 0, "failed config retained partial roots");
	Ok(Heap.ConfigureStaticSlots(std::array<std::uint32_t, 2>{1, 0}));
	const auto Last = Root(Heap, F); FToken Failed = 99;
	Check(Heap.CreateRoot(F, 0, Failed) == EHeapError::RootLimit && Failed == 0, "ordinary roots bypassed static quota");
	FPersistentRoots Lease;
	Check(Heap.RetainPersistent({&A, 1}, Lease) == EHeapError::RootLimit, "native lease bypassed static quota");
	Ok(Heap.ReleaseRoot(Last)); Ok(Heap.RetainPersistent({&A, 1}, Lease));
	Ok(Heap.WriteStaticSlot(1, 1, A)); Ok(Heap.PopFrame(F)); Lease.Reset(); Ok(Heap.Collect());
	Check(Heap.GetStats().LiveRoots == 2 && Heap.IsAlive(A), "root quota recovery damaged static storage");
	// Slot IDs cannot be used as ordinary root capabilities.
	Check(Heap.SetRoot(1, 0) == EHeapError::InvalidRoot && Heap.ReleaseRoot(1) == EHeapError::InvalidRoot
		&& Heap.Allocate(1, 1, Failed) == EHeapError::InvalidRoot, "static slot exposed a mutable root token");
}

void StaticDomainsAreIsolated()
{
	alignas(FHeap) std::array<std::byte, sizeof(FHeap)> Storage;
	FHeap* Original = std::construct_at(reinterpret_cast<FHeap*>(Storage.data()));
	Ok(Original->Configure(Layouts)); Ok(Original->ConfigureStaticSlots(std::array<std::uint32_t, 1>{1}));
	const auto F = Frame(*Original), A = Allocate(*Original, Root(*Original, F));
	Ok(Original->WriteStaticSlot(1, 1, A)); Ok(Original->PopFrame(F));
	FHeap Other; Ok(Other.Configure(Layouts)); Ok(Other.ConfigureStaticSlots(std::array<std::uint32_t, 1>{1}));
	Check(Read(Other, 1) == 0 && Other.WriteStaticSlot(1, 1, A) == EHeapError::InvalidObject, "domain state was shared");
	Original->Close(); Original->Close(); FToken Result = 99;
	Check(Original->ReadStaticSlot(1, 1, Result) == EHeapError::Closed && Result == 0
		&& Original->GetStats().LiveObjects == 0 && Original->GetStats().StaticRoots == 0, "closed domain retained state");
	std::destroy_at(Original);
	FHeap* Replacement = std::construct_at(reinterpret_cast<FHeap*>(Storage.data()));
	Ok(Replacement->Configure(Layouts)); Ok(Replacement->ConfigureStaticSlots(std::array<std::uint32_t, 1>{1}));
	Check(Read(*Replacement, 1) == 0 && Replacement->WriteStaticSlot(1, 1, A) == EHeapError::InvalidObject,
		"same-address heap accepted old domain token");
	std::destroy_at(Replacement);
}

using FPacket = std::vector<std::uint8_t>;
void Wire(FPacket& Bytes, std::uint64_t Value, unsigned Width = 4)
{ for (unsigned I = 0; I < Width; ++I) Bytes.push_back(static_cast<std::uint8_t>(Value >> (I * 8))); }
FPacket Packet(Abi::ECommand Command) { FPacket Bytes; Wire(Bytes, Abi::Magic); Wire(Bytes, static_cast<std::uint32_t>(Command)); return Bytes; }
FPacket StaticConfig() { auto P = Packet(Abi::ECommand::ConfigureStaticSlots); Wire(P, 1); Wire(P, 1); return P; }
FPacket StaticAccess(Abi::ECommand Command, FToken Object = 0)
{ auto P = Packet(Command); Wire(P, 1); Wire(P, 1); if (Command == Abi::ECommand::WriteStaticSlot) Wire(P, Object, 8); return P; }

void StaticProtocolSupportsReentry()
{
	FHeap Heap; Ok(Heap.Configure(Layouts));
	Check(ExecuteHeapCommand(Heap, StaticConfig(), {}, 0).Succeeded(), "wire config failed");
	const auto Outer = Frame(Heap), OuterRoot = Root(Heap, Outer), A = Allocate(Heap, OuterRoot);
	const auto Inner = Frame(Heap), InnerRoot = Root(Heap, Inner), B = Allocate(Heap, InnerRoot);
	auto P = Packet(Abi::ECommand::SetRoot); Wire(P, OuterRoot, 8); Wire(P, B, 8);
	Check(ExecuteHeapCommand(Heap, P, {}, 1).HeapError == EHeapError::RootAuthority, "nested static access weakened caller root authority");
	Check(ExecuteHeapCommand(Heap, StaticAccess(Abi::ECommand::WriteStaticSlot, B), {}, 1).Succeeded(), "nested static write rejected");
	std::array<std::uint8_t, 8> Bytes{};
	Check(ExecuteHeapCommand(Heap, StaticAccess(Abi::ECommand::ReadStaticSlot), Bytes, 1).Succeeded(), "nested static read rejected");
	FToken Result = 0; for (unsigned I = 0; I < 8; ++I) Result |= FToken(Bytes[I]) << (I * 8);
	Check(Result == B, "wire read lost object identity or high bits");
	Ok(Heap.PopFrame(Inner)); Ok(Heap.Collect());
	Check(Heap.IsAlive(A) && Heap.IsAlive(B), "nested return lost caller or static object");
	Ok(Heap.PopFrame(Outer)); Ok(Heap.Collect());
	Check(!Heap.IsAlive(A) && Heap.IsAlive(B) && Heap.GetStats().LiveRoots == 1, "static root did not survive invocation unwind");
}

void StaticProtocolRejectsMalformedPackets()
{
	FHeap Heap; Ok(Heap.Configure(Layouts));
	const auto Config = StaticConfig();
	for (std::size_t Size = Abi::HeaderBytes; Size < Config.size(); ++Size)
		Check(!ExecuteHeapCommand(Heap, std::span(Config).first(Size), {}, 0).Succeeded(), "truncated config accepted");
	auto Extra = Config; Extra.push_back(0);
	Check(ExecuteHeapCommand(Heap, Extra, {}, 0).Error == EHeapProtocolError::InvalidPacket, "extra config data accepted");
	auto TooMany = Packet(Abi::ECommand::ConfigureStaticSlots); Wire(TooMany, Abi::MaxStaticSlots + 1);
	Check(ExecuteHeapCommand(Heap, TooMany, {}, 0).Error == EHeapProtocolError::LimitExceeded, "wire slot quota ignored");
	auto Zero = Packet(Abi::ECommand::ConfigureStaticSlots); Wire(Zero, 0);
	Check(!ExecuteHeapCommand(Heap, Zero, {}, 0).Succeeded(), "zero slot count accepted");
	std::array<std::uint8_t, 8> Bytes{};
	Check(ExecuteHeapCommand(Heap, Config, Bytes, 0).Error == EHeapProtocolError::InvalidOutput, "config output accepted");
	Check(Heap.GetStats().StaticRoots == 0, "invalid config mutated heap");
	Check(ExecuteHeapCommand(Heap, Config, {}, 0).Succeeded(), "valid retry after rejected config failed");
	const auto F = Frame(Heap), A = Allocate(Heap, Root(Heap, F));
	const auto Write = StaticAccess(Abi::ECommand::WriteStaticSlot, A);
	for (std::size_t Size = Abi::HeaderBytes; Size < Write.size(); ++Size)
		Check(!ExecuteHeapCommand(Heap, std::span(Write).first(Size), {}, 0).Succeeded(), "truncated write accepted");
	Extra = Write; Extra.push_back(0);
	Check(!ExecuteHeapCommand(Heap, Extra, {}, 0).Succeeded()
		&& ExecuteHeapCommand(Heap, Write, Bytes, 0).Error == EHeapProtocolError::InvalidOutput
		&& Read(Heap, 1) == 0, "invalid write published value");
	Check(ExecuteHeapCommand(Heap, Write, {}, 0).Succeeded(), "valid write failed");
	Check(ExecuteHeapCommand(Heap, StaticAccess(Abi::ECommand::ReadStaticSlot), {}, 0).Error == EHeapProtocolError::InvalidOutput,
		"read with missing output accepted");
	Check(ExecuteHeapCommand(Heap, Config, {}, 0).HeapError == EHeapError::AlreadyConfigured && Read(Heap, 1) == A,
		"wire reconfiguration overwrote live slot");
	Check(ExecuteHeapCommand(Heap, Packet(static_cast<Abi::ECommand>(17)), {}, 0).Error == EHeapProtocolError::UnknownCommand,
		"future command accepted");
}

void StaticGraphOracle()
{
	FHeap Heap; Ok(Heap.Configure(Layouts)); Ok(Heap.ConfigureStaticSlots(std::array<std::uint32_t, 3>{1, 1, 1}));
	std::array<FToken, 3> Values{}; std::map<FToken, FToken> Edges; std::mt19937 Random(9273);
	for (unsigned Step = 0; Step < 2000; ++Step)
	{
		const auto F = Frame(Heap), R = Root(Heap, F);
		const unsigned Index = Random() % 3;
		if (Step % 3 == 0 || Edges.empty())
		{
			const auto Object = Allocate(Heap, R); Edges[Object] = 0; Values[Index] = Object;
			Ok(Heap.WriteStaticSlot(Index + 1, 1, Object));
		}
		else if (Step % 3 == 1)
		{
			auto Source = Edges.begin(), Target = Edges.begin();
			std::advance(Source, Random() % Edges.size()); std::advance(Target, Random() % Edges.size());
			Ok(Heap.WriteReference(Source->first, 1, 0, Target->first)); Source->second = Target->first;
		}
		else { Values[Index] = 0; Ok(Heap.WriteStaticSlot(Index + 1, 1, 0)); }
		Ok(Heap.PopFrame(F)); Ok(Heap.Collect());
		std::set<FToken> Live;
		for (auto Object : Values) while (Object && Live.insert(Object).second) Object = Edges.at(Object);
		for (auto It = Edges.begin(); It != Edges.end(); )
		{
			Check(Heap.IsAlive(It->first) == Live.contains(It->first), "static graph differs from reachability oracle");
			if (!Live.contains(It->first)) It = Edges.erase(It); else ++It;
		}
		Check(Heap.GetStats().LiveObjects == Live.size() && Heap.GetStats().LiveRoots == 3, "oracle found leaked roots");
	}
}
}

int RunManagedStaticTests()
{
	StaticGraphsSurviveInvocations(); StaticFailuresPreserveValues(); StaticRootBudgetIsAtomic();
	StaticDomainsAreIsolated(); StaticProtocolSupportsReentry(); StaticProtocolRejectsMalformedPackets(); StaticGraphOracle();
	return 7;
}
