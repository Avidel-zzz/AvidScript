#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptWasmRuntime.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptRuntimeArtifact.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "Session/AvidScriptRuntimeExecutionDomain.h"
#include "AvidScriptManagedHeapAbi.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Tests/AvidScriptGeneratedTypeSessionTestTypes.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Misc/ScopeExit.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace AvidScriptContextInvocationTests
{
void U32(TArray<uint8>& Out, uint32 Value)
{
	do { uint8 Byte = Value & 127; Value >>= 7; Out.Add(Byte | (Value ? 128 : 0)); } while (Value);
}
void I32(TArray<uint8>& Out, int32 Value)
{
	bool More;
	do { uint8 Byte = Value & 127; Value >>= 7;
		More = !((Value == 0 && !(Byte & 64)) || (Value == -1 && (Byte & 64)));
		Out.Add(Byte | (More ? 128 : 0)); } while (More);
}
void Constant(TArray<uint8>& Out, int32 Value) { Out.Add(0x41); I32(Out, Value); }
void Name(TArray<uint8>& Out, const char* Text)
{
	const int32 Size = FCStringAnsi::Strlen(Text); U32(Out, Size);
	Out.Append(reinterpret_cast<const uint8*>(Text), Size);
}
void Section(TArray<uint8>& Out, uint8 Id, const TArray<uint8>& Bytes)
{
	Out.Add(Id); U32(Out, Bytes.Num()); Out.Append(Bytes);
}
void HeapCall(TArray<uint8>& Out, int32 Input, int32 Size, int32 Output = 0, int32 Bytes = 0)
{
	Constant(Out, Input); Constant(Out, Size); Constant(Out, Output); Constant(Out, Bytes);
	Out.Append({0x10, 0, 0x1a});
}
void CopyToken(TArray<uint8>& Out, int32 Source, int32 Destination)
{
	Constant(Out, Destination); Constant(Out, Source);
	Out.Append({0x29, 0, 0, 0x37, 0, 0});
}
// Runtime/heap ABI probe, deliberately not a claim of C# cross-instance lowering.
// Both reference arguments point to memory[560]; one heap object at token[528]
// is rooted separately by A, B and nested A. Every entry forces collection.
TArray<uint8> BuildFixture(bool bProduction = false)
{
	using namespace AvidScript::Managed;
	TArray<uint8> Module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	TArray<uint8> Types{5, 0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f,
		0x60, 1, 0x7e, 1, 0x7f, 0x60, 0, 1, 0x7f, 0x60, 0, 0};
	if (bProduction) Types.Append({0x60, 6, 0x7f, 0x7e, 0x7f, 0x7f, 0x7c, 0x7f, 1, 0x7f});
	else Types.Append({0x60, 1, 0x7f, 1, 0x7f});
	Section(Module, 1, Types);
	TArray<uint8> Imports{3};
	for (const auto& Item : {TPair<const char*, uint8>{Abi::ImportName, 0}, {"context_reenter", 1}, {"owner_get_slot", 2}})
	{
		Name(Imports, "avidscript"); Name(Imports, Item.Key); Imports.Append({0, Item.Value});
	}
	Section(Module, 2, Imports);
	Section(Module, 3, bProduction ? TArray<uint8>{5, 3, 4, 2, 2, 1} : TArray<uint8>{4, 3, 4, 2, 2});
	Section(Module, 5, {1, 1, 1, 1});
	Section(Module, 6, bProduction ? TArray<uint8>{2, 0x7f, 1, 0x41, 0, 0x0b, 0x7f, 1, 0x41, 0, 0x0b}
		: TArray<uint8>{1, 0x7f, 1, 0x41, 0, 0x0b});
	TArray<uint8> Exports{static_cast<uint8>(bProduction ? 6 : 5)};
	Name(Exports, "memory"); Exports.Append({2, 0});
	for (const auto& Item : {TPair<const char*, uint8>{"avid_on_begin_play", 3}, {"step", 4}, {"trap", 5}, {"count", 6}})
	{
		Name(Exports, Item.Key); Exports.Append({0, Item.Value});
	}
	if (bProduction) { Name(Exports, "avid_ue_0123456789abcdef0123456789abcdef"); Exports.Append({0, 7}); }
	Section(Module, 7, Exports);
	TArray<uint8> Init{0};
	if (bProduction) Init.Append({0x23, 1, 0x45, 0x04, 0x40});
	HeapCall(Init, 64, 24);
	if (bProduction) Init.Append({0x41, 1, 0x24, 1, 0x0b});
	Init.Add(0x0b);
	TArray<uint8> Step{0};
	// Only the root creates the shared object; nested entries root that same token.
	Step.Append({0x20, 0, 0x41, 2, 0x46, 0x04, 0x40});
	Constant(Step, 528); Step.Append({0x42, 0, 0x37, 0, 0});
	Constant(Step, 560); Constant(Step, 0); Step.Append({0x36, 0, 0, 0x0b});
	HeapCall(Step, 96, 8, 512, 8);
	CopyToken(Step, 512, 120);
	if (bProduction) { Constant(Step, 128); Step.Append({0x20, 1, 0x37, 0, 0}); }
	else CopyToken(Step, 528, 128);
	HeapCall(Step, 112, 24, 520, 8);
	Step.Append({0x20, 0, 0x41, 2, 0x46, 0x04, 0x40});
	CopyToken(Step, 520, 156); HeapCall(Step, 144, 20, 528, 8); Step.Add(0x0b);
	CopyToken(Step, 528, 184); CopyToken(Step, 528, 216);
	HeapCall(Step, 176, 28, 536, 4);
	Constant(Step, 236); Constant(Step, 536); Step.Append({0x28, 0, 0, 0x41, 1, 0x6a, 0x36, 0, 0});
	HeapCall(Step, 208, 32);
	// Read and write through the two aliased pointer values, not independent copies.
	for (int32 Index = 0; Index < 2; ++Index)
	{
		for (int32 Read = 0; Read < 2; ++Read)
			if (bProduction) Step.Append({0x20, static_cast<uint8>(2 + Index)});
			else { Constant(Step, 544 + 4 * Index); Step.Append({0x28, 0, 0}); }
		Step.Append({0x28, 0, 0, 0x41, 1, 0x6a, 0x36, 0, 0});
	}
	if (bProduction)
	{
		// The same full eight-cell frame crosses every owner transition.
		Step.Append({0x20, 4, 0x44, 0, 0, 0, 0, 0, 0, 0x29, 0x40, 0x62, 0x04, 0x40, 0, 0x0b});
		Step.Append({0x20, 5, 0x41, 0xcd, 0, 0x47, 0x04, 0x40, 0, 0x0b});
	}
	Step.Append({0x23, 0, 0x41, 1, 0x6a, 0x24, 0});
	auto RecordOwner = [&](int32 Address)
	{
		Constant(Step, Address); Step.Append({0x20, 0, 0x41, 4, 0x6c, 0x6a, 0x10, 2, 0x36, 0, 0});
	};
	RecordOwner(576);
	HeapCall(Step, 248, 8);
	Step.Append({0x20, 0, 0x04, 0x40, 0x20, 0, 0x41, 1, 0x6b, 0xad, 0x10, 1, 0x1a, 0x0b});
	RecordOwner(592);
	HeapCall(Step, 248, 8); HeapCall(Step, 176, 28, 536, 4);
	Constant(Step, 536); Step.Append({0x28, 0, 0, 0x0b});
	const TArray<uint8> Trap{0, 0, 0x0b}, Count{0, 0x23, 0, 0x0b};
	TArray<uint8> Code{static_cast<uint8>(bProduction ? 5 : 4)};
	const TArray<uint8>* Bodies[] = {&Init, &Step, &Trap, &Count};
	for (const auto* Body : Bodies) { U32(Code, Body->Num()); Code.Append(*Body); }
	if (bProduction)
	{
		TArray<uint8> Root{0, 0x41, 2, 0x42, 0};
		Constant(Root, 560); Constant(Root, 560);
		Root.Append({0x44, 0, 0, 0, 0, 0, 0, 0x29, 0x40}); Constant(Root, 77);
		Root.Append({0x10, 4, 0x0b}); U32(Code, Root.Num()); Code.Append(Root);
	}
	Section(Module, 10, Code);
	TArray<uint8> Memory; Memory.SetNumZeroed(552);
	auto Put = [&](int32 Address, uint32 Value)
	{
		for (uint32 I = 0; I < 4; ++I) Memory[Address + I] = static_cast<uint8>(Value >> (I * 8));
	};
	auto Header = [&](int32 Address, Abi::ECommand Command)
	{
		Put(Address, Abi::Magic); Put(Address + 4, static_cast<uint32>(Command));
	};
	Header(64, Abi::ECommand::Configure); Put(72, 1); Put(76, 1); Put(80, 4); Put(84, 0);
	Header(96, Abi::ECommand::PushFrame); Header(112, Abi::ECommand::CreateRoot);
	Header(144, Abi::ECommand::Allocate); Put(152, 1);
	Header(176, Abi::ECommand::ReadBytes); Put(192, 1); Put(196, 0); Put(200, 4);
	Header(208, Abi::ECommand::WriteBytes); Put(224, 1); Put(228, 0); Put(232, 4);
	Header(248, Abi::ECommand::Collect); Put(544, 560); Put(548, 560);
	TArray<uint8> Data{1, 0}; Constant(Data, 0); Data.Add(0x0b); U32(Data, Memory.Num()); Data.Append(Memory);
	Section(Module, 11, Data);
	return Module;
}

class FSubscriptions final : public IAvidScriptEventSubscriptionHost
{
public:
	int64 Subscribe(UObject&, uint32, FString&) override { return 1; }
	bool Unsubscribe(int64, FString&) override { ++Unsubscribes; return true; }
	int32 Unsubscribes = 0;
};

enum class EScenario { Normal, Trap, Unload, Mutate, ForeignRegistry, ForeignCode, World, Retire, Timer, Depth, Entries, Frame, Suspended,
	RootReturn, RootRestore, RootNoGrant, RootWrongGrant, RootInvalid, RootRelease, RootAllocate, RootCreate, RootShadow, RootDepth, RootRegrant };
bool IsRootScenario(EScenario Scenario) { return Scenario >= EScenario::RootReturn; }
bool RootSucceeds(EScenario Scenario) { return Scenario == EScenario::RootReturn || Scenario == EScenario::RootRestore; }

// A real VM caller owns private and returned roots. The callee allocates a new
// object, transfers it with SetRoot, then its entire heap frame is unwound.
// Nested no-frame exports exercise equal heap floors at different VM depths.
TArray<uint8> BuildRootTransferFixture(EScenario Scenario)
{
	using namespace AvidScript::Managed;
	TArray<uint8> Module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	Section(Module, 1, {4, 0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f,
		0x60, 1, 0x7e, 1, 0x7f, 0x60, 0, 1, 0x7f, 0x60, 0, 0});
	TArray<uint8> Imports{3};
	for (const auto& Item : {TPair<const char*, uint8>{Abi::ImportName, 0}, {"context_reenter", 1}, {"owner_get_slot", 2}})
	{
		Name(Imports, "avidscript"); Name(Imports, Item.Key); Imports.Append({0, Item.Value});
	}
	Section(Module, 2, Imports); Section(Module, 3, {6, 3, 1, 2, 2, 1, 1});
	Section(Module, 5, {1, 1, 1, 1});
	Section(Module, 6, {2, 0x7f, 1, 0x41, 0, 0x0b, 0x7f, 1, 0x41, 0, 0x0b});
	TArray<uint8> Exports{7}; Name(Exports, "memory"); Exports.Append({2, 0});
	for (const auto& Item : {TPair<const char*, uint8>{"avid_on_begin_play", 3}, {"step", 4}, {"trap", 5}, {"count", 6},
		{"avid_ue_0123456789abcdef0123456789abcdef", 7}, {"leaf", 8}})
	{ Name(Exports, Item.Key); Exports.Append({0, Item.Value}); }
	Section(Module, 7, Exports);
	TArray<uint8> Init{0, 0x23, 1, 0x45, 0x04, 0x40}; HeapCall(Init, 64, 24);
	Init.Append({0x41, 1, 0x24, 1, 0x0b, 0x0b});
	TArray<uint8> Root{0};
	HeapCall(Root, 96, 8, 512, 8); CopyToken(Root, 512, 120); HeapCall(Root, 112, 24, 520, 8);
	CopyToken(Root, 520, 156); HeapCall(Root, 144, 20, 528, 8); HeapCall(Root, 112, 24, 608, 8);
	Root.Append({0x42, 1, 0x10, 1, 0x0b});
	TArray<uint8> Step{0, 0x23, 0, 0x41, 1, 0x6a, 0x24, 0};
	Constant(Step, 672); Constant(Step, 1); Step.Append({0x36, 0, 0});
	if (Scenario == EScenario::RootRestore || Scenario == EScenario::RootShadow || Scenario == EScenario::RootDepth || Scenario == EScenario::RootRegrant)
		Step.Append({0x42, 0, 0x10, 1, 0x1a});
	HeapCall(Step, 96, 8, 640, 8); CopyToken(Step, 640, 120); HeapCall(Step, 112, 24, 648, 8);
	CopyToken(Step, 648, 156); HeapCall(Step, 144, 20, 656, 8);
	CopyToken(Step, 656, 184); HeapCall(Step, 176, 32);
	if (Scenario == EScenario::RootRelease)
	{ Constant(Step, 248); Step.Append({0x20, 0, 0x37, 0, 0}); HeapCall(Step, 240, 16); }
	else if (Scenario == EScenario::RootAllocate)
	{ Constant(Step, 308); Step.Append({0x20, 0, 0x37, 0, 0}); HeapCall(Step, 296, 20, 664, 8); }
	else if (Scenario == EScenario::RootCreate)
	{ CopyToken(Step, 512, 272); HeapCall(Step, 264, 24, 664, 8); }
	else
	{ Constant(Step, 216); Step.Append({0x20, 0, 0x37, 0, 0}); CopyToken(Step, 656, 224); HeapCall(Step, 208, 24); }
	Constant(Step, 99); Step.Add(0x0b);
	TArray<uint8> Leaf{0}; Constant(Leaf, 216); Leaf.Append({0x20, 0, 0x37, 0, 0});
	CopyToken(Leaf, 528, 224); HeapCall(Leaf, 208, 24); Constant(Leaf, 99); Leaf.Add(0x0b);
	const TArray<uint8> Trap{0, 0, 0x0b}, Count{0, 0x23, 0, 0x0b};
	TArray<uint8> Code{6};
	const TArray<uint8>* Bodies[] = {&Init, &Step, &Trap, &Count, &Root, &Leaf};
	for (const auto* Body : Bodies) { U32(Code, Body->Num()); Code.Append(*Body); }
	Section(Module, 10, Code);
	TArray<uint8> Memory; Memory.SetNumZeroed(336);
	auto Put = [&](int32 Address, uint32 Value) { for (uint32 I = 0; I < 4; ++I) Memory[Address + I] = static_cast<uint8>(Value >> (I * 8)); };
	auto Header = [&](int32 Address, Abi::ECommand Command) { Put(Address, Abi::Magic); Put(Address + 4, static_cast<uint32>(Command)); };
	Header(64, Abi::ECommand::Configure); Put(72, 1); Put(76, 1); Put(80, 4); Put(84, 0);
	Header(96, Abi::ECommand::PushFrame); Header(112, Abi::ECommand::CreateRoot);
	Header(144, Abi::ECommand::Allocate); Put(152, 1);
	Header(176, Abi::ECommand::WriteBytes); Put(192, 1); Put(196, 0); Put(200, 4); Put(204, 99);
	Header(208, Abi::ECommand::SetRoot); Header(240, Abi::ECommand::ReleaseRoot);
	Header(264, Abi::ECommand::CreateRoot); Header(296, Abi::ECommand::Allocate); Put(304, 1);
	Header(328, Abi::ECommand::Collect);
	TArray<uint8> Data{1, 0}; Constant(Data, 0); Data.Add(0x0b); U32(Data, Memory.Num()); Data.Append(Memory);
	Section(Module, 11, Data); return Module;
}

struct FRootTransferProbe
{
	FAutomationTestBase& Test;
	FAvidScriptWasmRuntimeInstance& Runtime;
	FAvidScriptWasmHostContext A, B;
	FAvidScriptContextualExportCall Step, Count, Leaf;
	EScenario Scenario;
	int32 Calls = 0;
	bool bHostGcPreservedReturn = false;
	FString LastError;
	uint64 ReadToken(uint32 Offset)
	{
		TArray<uint8> Bytes; Bytes.SetNumZeroed(8); FString Error;
		Test.TestTrue(TEXT("read root transfer token from VM"), Runtime.ReadStateBytes(Offset, Bytes, Error));
		uint64 Token = 0; FMemory::Memcpy(&Token, Bytes.GetData(), 8); return Token;
	}
	static EAvidScriptVmTypedHostStatus Reenter(void* Context, int64 Remaining, int32& Value)
	{
		using namespace AvidScript::Managed;
		auto& Self = *static_cast<FRootTransferProbe*>(Context); ++Self.Calls;
		const auto ParentSlot = Self.Runtime.HandleOwnerGetSlotImport();
		const auto Returned = Self.ReadToken(608);
		FAvidScriptVmCallFrame Frame; Frame.CellCount = 2; FMemory::Memcpy(Frame.Cells, &Returned, 8);
		FAvidScriptVmError Error; FAvidScriptVmCallResult Result; bool bCalled;
		const auto Before = Self.Runtime.GetManagedHeapForTesting()->GetStats().ActiveFrames;
		if (Remaining == 0)
		{
			Self.Test.TestEqual(TEXT("nested loan probe enters before callee pushes a heap frame"), Before, 1u);
			if (Self.Scenario == EScenario::RootRestore)
				bCalled = Self.Runtime.InvokeGeneratedInstanceExport(Self.A.OwnerHandle, Self.Count, {}, Error, &Result);
			else if (Self.Scenario == EScenario::RootDepth)
				bCalled = Self.Runtime.InvokeInContext(Self.Leaf, Self.A, Frame, Error, &Result);
			else if (Self.Scenario == EScenario::RootRegrant)
				bCalled = Self.Runtime.InvokeGeneratedInstanceExport(Self.A.OwnerHandle, Self.Leaf, Frame, Error, &Result, {&Returned, 1});
			else bCalled = Self.Runtime.InvokeGeneratedInstanceExport(Self.A.OwnerHandle, Self.Leaf, Frame, Error, &Result);
			Self.Test.TestEqual(TEXT("unframed nested entry cannot inherit returned root authority"), bCalled, Self.Scenario == EScenario::RootRestore);
		}
		else
		{
			uint64 Loan = Self.Scenario == EScenario::RootWrongGrant ? Self.ReadToken(520)
				: Self.Scenario == EScenario::RootInvalid ? 0 : Returned;
			const TConstArrayView<uint64> Grants = Self.Scenario == EScenario::RootNoGrant ? TConstArrayView<uint64>{} : TConstArrayView<uint64>{&Loan, 1};
			bCalled = Self.Runtime.InvokeGeneratedInstanceExport(Self.B.OwnerHandle, Self.Step, Frame, Error, &Result, Grants);
			Self.Test.TestEqual(TEXT("root transfer route outcome"), bCalled, RootSucceeds(Self.Scenario));
			if (Self.Scenario == EScenario::RootInvalid)
				Self.Test.TestEqual(TEXT("invalid loan is rejected before target effects"), Self.ReadToken(672), uint64{0});
			if (bCalled)
			{
				auto* Heap = Self.Runtime.GetManagedHeapForTesting();
				Self.Test.TestTrue(TEXT("host forces GC before resuming caller"), Heap->Collect() == EHeapError::Ok);
				const auto Fresh = Self.ReadToken(656);
				Self.bHostGcPreservedReturn = Heap->IsAlive(Fresh) && Heap->IsAlive(Self.ReadToken(528));
				Self.Test.TestTrue(TEXT("returned and unrelated caller objects survive callee unwind and host GC"), Self.bHostGcPreservedReturn);
				uint32 Number = 0;
				Self.Test.TestTrue(TEXT("returned object remains readable"), Heap->ReadBytes(Fresh, 1, 0,
					{reinterpret_cast<uint8*>(&Number), sizeof(Number)}) == EHeapError::Ok);
				Self.Test.TestEqual(TEXT("returned object keeps callee mutation"), Number, 99u);
			}
		}
		if (!bCalled) Self.LastError = Error.Category;
		Self.Test.TestEqual(TEXT("root transfer restores caller frame floor"), Self.Runtime.GetManagedHeapForTesting()->GetStats().ActiveFrames, Before);
		Self.Test.TestEqual(TEXT("root transfer restores caller owner"), Self.Runtime.HandleOwnerGetSlotImport(), ParentSlot);
		Value = bCalled ? static_cast<int32>(Result.Cells[0]) : 0;
		return EAvidScriptVmTypedHostStatus::Succeeded; // Deliberately ignore failure; the root chain must still fail.
	}
};
struct FProbe
{
	FAutomationTestBase& Test;
	FAvidScriptWasmRuntimeInstance& Runtime;
	FAvidScriptWasmHostContext A, B, Foreign;
	FAvidScriptContextualExportCall Step, Trap, OtherCode;
	EScenario Scenario = EScenario::Normal;
	int32 Calls = 0;
	TWeakObjectPtr<UWorld> WrongWorld;
	bool bProduction = false;
	FAvidScriptObjectHandle OtherTarget;
	FAvidScriptRuntimeSession* SourceSession = nullptr;
	FAvidScriptRuntimeSession* PeerSession = nullptr;
	uint32 WritesBeforeFailure = 0;

	static EAvidScriptVmTypedHostStatus Reenter(void* Context, int64 Remaining, int32& Value)
	{
		auto& Self = *static_cast<FProbe*>(Context);
		const uint32 ParentSlot = static_cast<uint32>(Self.Runtime.HandleOwnerGetSlotImport());
		if (!Self.bProduction) Self.Test.TestTrue(TEXT("subscription import uses current instance"), Self.Runtime.HandleEventUnsubscribeImport(1) == 1);
		if (++Self.Calls > 70) return EAvidScriptVmTypedHostStatus::Rejected;
		FAvidScriptWasmHostContext Target = Remaining == 1 ? Self.B : Self.A;
		FAvidScriptVmCallFrame Frame; Frame.CellCount = 1; Frame.Cells[0] = static_cast<uint32>(Remaining);
		const FAvidScriptContextualExportCall* Entry = &Self.Step;
		if (Self.Scenario == EScenario::Trap && Remaining == 0) { Entry = &Self.Trap; Frame = {}; }
		if (Self.Scenario == EScenario::ForeignRegistry) Target = Self.Foreign;
		if (Self.Scenario == EScenario::ForeignCode) Entry = &Self.OtherCode;
		if (Self.Scenario == EScenario::World) Target.World = Self.WrongWorld;
		if (Self.Scenario == EScenario::Depth) Frame.Cells[0] = 1;
		if (Self.bProduction)
		{
			TArray<uint8> Token; Token.SetNumZeroed(8); FString ReadError;
			Self.Test.TestTrue(TEXT("pass original managed object token"), Self.Runtime.ReadStateBytes(528, Token, ReadError));
			Frame.CellCount = 8;
			FMemory::Memcpy(&Frame.Cells[1], Token.GetData(), 8);
			Frame.Cells[3] = 560; Frame.Cells[4] = 560;
			const double Mixed = 12.5; FMemory::Memcpy(&Frame.Cells[5], &Mixed, 8); Frame.Cells[7] = 77;
			if (Entry == &Self.Trap) Frame = {};
			if (Self.Scenario == EScenario::Frame) --Frame.CellCount;
		}
		if (Self.Scenario == EScenario::Unload)
		{
			FAvidScriptWasmSmokeResult Result;
			Self.Runtime.Unload(Result);
			Self.Test.TestFalse(TEXT("active unload is rejected"), Result.bUnloaded);
			Self.Test.TestTrue(TEXT("active heap remains owned"), Self.Runtime.IsLoaded() && Self.Runtime.GetManagedHeapForTesting());
			return EAvidScriptVmTypedHostStatus::Succeeded; // intentionally ignore the failure
		}
		if (Self.Scenario == EScenario::Mutate)
		{
			if (Self.bProduction)
			{
				FAvidScriptWasmSmokeResult Rejected; FString ClearError;
				Self.Test.TestFalse(TEXT("nested owner cannot stop source"), Self.SourceSession->StopAndUnload(Rejected));
				Self.Test.TestFalse(TEXT("nested owner cannot stop peer"), Self.PeerSession->StopAndUnload(Rejected));
				Self.Test.TestFalse(TEXT("nested owner cannot retire target registration"), Self.PeerSession->ClearGeneratedTypeInstance(ClearError));
			}
			Self.Runtime.SetHostContext(Self.B);
			Self.Runtime.ClearHostContext();
			FAvidScriptWasmSmokeResult Rejected;
			Self.Test.TestFalse(TEXT("active module lifecycle cannot end"), Self.Runtime.EndPlay(Rejected));
			Self.Test.TestFalse(TEXT("active module cannot be replaced"), Self.Runtime.LoadModule(nullptr, 0, TEXT("reentrant"), Rejected));
			Self.Test.TestEqual(TEXT("unscoped context mutation is rejected"), Self.Runtime.HandleOwnerGetSlotImport(), static_cast<int32>(ParentSlot));
			return EAvidScriptVmTypedHostStatus::Succeeded;
		}
		if (Self.Scenario == EScenario::Timer)
		{
			Self.Test.TestEqual(TEXT("legacy timer cannot lose its instance owner"), Self.Runtime.HandleTimerSetOnceImport(0, 1), 0);
			return EAvidScriptVmTypedHostStatus::Succeeded;
		}
		if (Self.Scenario == EScenario::Retire)
		{
			FAvidScriptObjectHandleResult Result;
			Self.Test.TestTrue(TEXT("owner retires during execution"), Self.A.ObjectRegistry->ReleaseHandle(Self.A.OwnerHandle, Result, false));
			return EAvidScriptVmTypedHostStatus::Succeeded;
		}
		FAvidScriptVmError Error;
		FAvidScriptVmCallResult Result;
		auto Invoke = [&]()
		{
			return Self.bProduction
				? Self.Runtime.InvokeGeneratedInstanceExport(Self.Scenario == EScenario::World ? Self.OtherTarget : Target.OwnerHandle,
					*Entry, Frame, Error, &Result)
				: Self.Runtime.InvokeInContext(*Entry, Target, Frame, Error, &Result);
		};
		if (Self.Scenario == EScenario::Entries)
		{
			Frame.Cells[0] = 0;
			int32 Succeeded = 0;
			for (int32 I = 0; I < 4096; ++I)
				if (Invoke()) ++Succeeded;
			Self.Test.TestEqual(TEXT("sequential calls consume the same root entry budget"), Succeeded, 4095);
			return EAvidScriptVmTypedHostStatus::Succeeded;
		}
		const uint32 Frames = Self.Runtime.GetManagedHeapForTesting()->GetStats().ActiveFrames;
		const bool bCalled = Invoke();
		if (Self.bProduction && !bCalled)
		{
			TArray<uint8> Bytes; Bytes.SetNumZeroed(4); FString ReadError;
			Self.Test.TestTrue(TEXT("inner failure retains shared memory until outer exit"), Self.Runtime.ReadStateBytes(560, Bytes, ReadError));
			FMemory::Memcpy(&Self.WritesBeforeFailure, Bytes.GetData(), 4);
		}
		Self.Test.TestEqual(TEXT("nested invocation never unwinds the outer heap roots"), Self.Runtime.GetManagedHeapForTesting()->GetStats().ActiveFrames, Frames);
		Self.Test.TestEqual(TEXT("target context returns to parent"), Self.Runtime.HandleOwnerGetSlotImport(), static_cast<int32>(ParentSlot));
		Value = bCalled ? static_cast<int32>(Result.Cells[0]) : 0;
		return EAvidScriptVmTypedHostStatus::Succeeded; // root must latch ignored inner errors
	}
};

bool Run(FAutomationTestBase& Test, EAvidScriptVmBackendKind Backend)
{
	using namespace AvidScript::Managed;
	const TArray<uint8> Wasm = BuildFixture();
	for (const auto Scenario : {EScenario::Normal, EScenario::Trap, EScenario::Unload, EScenario::Mutate,
		EScenario::ForeignRegistry, EScenario::ForeignCode, EScenario::World, EScenario::Retire, EScenario::Timer, EScenario::Depth, EScenario::Entries})
	{
		Test.AddInfo(FString::Printf(TEXT("context backend=%d scenario=%d"), static_cast<int32>(Backend), static_cast<int32>(Scenario)));
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Runtime(Selection), OtherRuntime(Selection);
		FAvidScriptObjectRegistry Registry, ForeignRegistry;
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(NewObject<UAvidScriptGeneratedTypeSessionTestObject>()), B(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		TStrongObjectPtr<UWorld> WrongWorld(NewObject<UWorld>());
		FAvidScriptObjectHandleResult HandleResult;
		FProbe Probe{Test, Runtime};
		FSubscriptions ASubscriptions, BSubscriptions;
		Probe.A.ObjectRegistry = &Registry;
		Probe.A.OwnerHandle = Registry.RegisterObject(A.Get(), HandleResult, false);
		Probe.A.EventSubscriptions = &ASubscriptions;
		Probe.B = Probe.A;
		Probe.B.OwnerHandle = Registry.RegisterObject(B.Get(), HandleResult, false);
		Probe.B.EventSubscriptions = &BSubscriptions;
		Probe.Foreign = Probe.A;
		Probe.Foreign.ObjectRegistry = &ForeignRegistry;
		Probe.Foreign.OwnerHandle = ForeignRegistry.RegisterObject(B.Get(), HandleResult, false);
		Probe.Scenario = Scenario; Probe.WrongWorld = WrongWorld.Get();
		Runtime.SetHostContext(Probe.A);
		FAvidScriptVmTypedHostImport Import;
		Import.StableId = TEXT("context_reentry_probe"); Import.ModuleName = TEXT("avidscript"); Import.ImportName = TEXT("context_reenter");
		Import.Signature = TEXT("(I)i"); Import.Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get;
		Import.bSupplementalRuntimeAuthority = true; Import.PreparedTarget.Context = &Probe;
		Import.PreparedTarget.PackedSelfPropertyI32Get = &FProbe::Reenter;
		FString Error;
		if (!Test.TestTrue(TEXT("context callback import configures"), Runtime.SetSupplementalTypedHostImports({&Import, 1}, Error))) return false;
		FAvidScriptWasmSmokeResult Load;
		if (!Test.TestTrue(TEXT("context fixture loads"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("context_heap"), Load))) { Test.AddError(Load.ErrorMessage); return false; }
		if (!Test.TestTrue(TEXT("heap layout initializes"), Runtime.BeginPlay(Load))) { Test.AddError(Load.ErrorMessage); return false; }
		FAvidScriptContextualExportCall Count;
		if (!Runtime.PrepareContextualExportCall(TEXT("step"), Probe.Step, Error)
			|| !Runtime.PrepareContextualExportCall(TEXT("trap"), Probe.Trap, Error)
			|| !Runtime.PrepareContextualExportCall(TEXT("count"), Count, Error)) { Test.AddError(Error); return false; }
		if (Scenario == EScenario::ForeignCode)
		{
			OtherRuntime.SetSupplementalTypedHostImports({&Import, 1}, Error);
			if (!OtherRuntime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("other_code"), Load)
				|| !OtherRuntime.PrepareContextualExportCall(TEXT("step"), Probe.OtherCode, Error)) return false;
		}
		FAvidScriptVmCallFrame Frame; Frame.CellCount = 1; Frame.Cells[0] = 2;
		FAvidScriptVmError Failure;
		FAvidScriptVmCallResult Result;
		const bool bCalled = Runtime.InvokeInContext(Probe.Step, Probe.A, Frame, Failure, &Result);
		Test.TestEqual(*FString::Printf(TEXT("only authorized complete chain succeeds: %s %s"), *Failure.Category, *Failure.Details), bCalled, Scenario == EScenario::Normal);
		if (Scenario == EScenario::Normal)
		{
			Test.TestEqual(TEXT("A observes shared managed object after B and nested A"), Result.Cells[0], 3u);
			Test.TestEqual(TEXT("subscriptions belong to their current owner"), ASubscriptions.Unsubscribes, 1);
			Test.TestEqual(TEXT("peer subscriptions use its own service"), BSubscriptions.Unsubscribes, 1);
			TArray<uint8> Bytes; Bytes.SetNumZeroed(44);
			Test.TestTrue(TEXT("read shared references and recorded owner identities"), Runtime.ReadStateBytes(560, Bytes, Error));
			auto Read = [&Bytes](int32 Offset) { uint32 Value; FMemory::Memcpy(&Value, Bytes.GetData() + Offset, 4); return Value; };
			Test.TestEqual(TEXT("two ref arguments alias one shared location"), Read(0), 6u);
			for (int32 Depth = 0; Depth < 3; ++Depth)
			{
				const uint32 Expected = Depth == 1 ? Probe.B.OwnerHandle.Slot : Probe.A.OwnerHandle.Slot;
				Test.TestEqual(TEXT("WASM owner import observes selected instance"), Read(16 + Depth * 4), Expected);
				Test.TestEqual(TEXT("WASM owner import restores caller after return"), Read(32 + Depth * 4), Expected);
			}
			Test.TestTrue(TEXT("shared static state remains readable"), Runtime.InvokeInContext(Count, Probe.A, {}, Failure, &Result));
			Test.TestEqual(TEXT("one module global counts all three instances entries"), Result.Cells[0], 3u);
		}
		else
		{
			Test.TestFalse(TEXT("ignored inner failure reaches root with a category"), Failure.Category.IsEmpty());
			const TCHAR* Expected = Scenario == EScenario::ForeignCode ? TEXT("context_invocation_code")
				: Scenario == EScenario::ForeignRegistry || Scenario == EScenario::World ? TEXT("context_invocation_authority")
				: Scenario == EScenario::Depth || Scenario == EScenario::Entries ? TEXT("context_invocation_budget")
				: Scenario == EScenario::Unload || Scenario == EScenario::Mutate || Scenario == EScenario::Timer ? TEXT("context_invocation_mutation") : nullptr;
			if (Expected) Test.TestEqual(TEXT("failure reports the precise boundary"), Failure.Category, FString(Expected));
		}
		auto* Heap = Runtime.GetManagedHeapForTesting();
		if (!Test.TestNotNull(TEXT("failed chains preserve Runtime ownership"), Heap)) return false;
		Test.TestEqual(TEXT("all frames unwind"), Heap->GetStats().ActiveFrames, 0u);
		Test.TestEqual(TEXT("all invocation roots unwind"), Heap->GetStats().LiveRoots, 0u);
		Test.TestTrue(TEXT("detached shared objects collect"), Heap->Collect() == EHeapError::Ok);
		Test.TestEqual(TEXT("no leaked shared objects"), Heap->GetStats().LiveObjects, 0u);
		if (Scenario != EScenario::Retire)
		{
			Test.TestEqual(TEXT("root exit restores original owner"), Runtime.HandleOwnerGetSlotImport(), static_cast<int32>(Probe.A.OwnerHandle.Slot));
			Test.TestTrue(TEXT("independent next chain is not poisoned"), Runtime.InvokeInContext(Count, Probe.A, {}, Failure, &Result));
		}
		else Test.TestTrue(TEXT("surviving peer can enter after the original owner retires"), Runtime.InvokeInContext(Count, Probe.B, {}, Failure, &Result));
		Runtime.Unload();
		Test.TestFalse(TEXT("retired code cannot execute"), Runtime.InvokeInContext(Count, Probe.A, {}, Failure, &Result));
		Test.TestEqual(TEXT("retired prepared code is diagnosed"), Failure.Category, FString(TEXT("context_invocation_code")));
		if (Scenario == EScenario::Normal)
		{
			FAvidScriptContextualExportCall FreshCount;
			if (!Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("new_code"), Load)
				|| !Runtime.PrepareContextualExportCall(TEXT("count"), FreshCount, Error)) return false;
			Test.TestFalse(TEXT("old prepared code cannot enter replacement VM"), Runtime.InvokeInContext(Count, Probe.A, {}, Failure, &Result));
			Test.TestTrue(TEXT("fresh prepared code enters replacement VM"), Runtime.InvokeInContext(FreshCount, Probe.A, {}, Failure, &Result));
			Test.TestEqual(TEXT("replacement gets distinct static storage"), Result.Cells[0], 0u);
		}
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptContextInvocationTest,
	"AvidScript.Runtime.GeneratedTypes.SharedRuntimeContext", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAvidScriptContextInvocationTest::RunTest(const FString& Parameters)
{
	return AvidScriptContextInvocationTests::Run(*this, EAvidScriptVmBackendKind::Wasmtime)
		&& AvidScriptContextInvocationTests::Run(*this, EAvidScriptVmBackendKind::Wamr);
}
namespace AvidScriptContextCallbackTests
{
TArray<uint8> BuildFixture()
{
	using namespace AvidScriptContextInvocationTests;
	TArray<uint8> Module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	Section(Module, 1, {6, 0x60, 1, 0x7e, 1, 0x7f, 0x60, 2, 0x7d, 0x7f, 1, 0x7e,
		0x60, 0, 0, 0x60, 0, 1, 0x7e, 0x60, 5, 0x7f, 0x7e, 0x7f, 0x7f, 0x7f, 0, 0x60, 1, 0x7f, 0});
	TArray<uint8> Imports{2};
	Name(Imports, "avidscript"); Name(Imports, "context_callback_probe"); Imports.Append({0, 0});
	Name(Imports, "avidscript"); Name(Imports, "continuation_delay"); Imports.Append({0, 1});
	Section(Module, 2, Imports);
	Section(Module, 3, {5, 2, 3, 4, 5, 2});
	TArray<uint8> Exports{5};
	for (const auto& Item : {TPair<const char*, uint8>{"avid_on_begin_play", 2}, {"schedule", 3},
		{"avid_on_continuation_v2", 4}, {"on_event", 5}, {"trap", 6}})
	{
		Name(Exports, Item.Key); Exports.Append({0, Item.Value});
	}
	Section(Module, 7, Exports);
	const TArray<uint8> Init{0, 0x0b}, Schedule{0, 0x43, 0x0a, 0xd7, 0x23, 0x3c, 0x41, 51, 0x10, 1, 0x0b},
		Continuation{0, 0x20, 1, 0x10, 0, 0x1a, 0x0b}, Event{0, 0x42, 0, 0x10, 0, 0x1a, 0x0b}, Trap{0, 0, 0x0b};
	TArray<uint8> Code{5};
	for (const auto* Body : {&Init, &Schedule, &Continuation, &Event, &Trap}) { U32(Code, Body->Num()); Code.Append(*Body); }
	Section(Module, 10, Code);
	return Module;
}

struct FProbe
{
	FAutomationTestBase& Test;
	FAvidScriptWasmRuntimeInstance& Runtime;
	FAvidScriptWasmHostContext A, B;
	FAvidScriptContextualExportCall Continuation, EventCall, Trap;
	FAvidScriptPreparedDelegateEvent Event;
	FAvidScriptContinuationCompletion CompletionB;
	bool bInjectFailure = false;
	int32 EventCalls = 0;
	int32 ContinuationCalls = 0;
	uint32 ExpectedBState = 222;

	static bool Encode(const void* Identity, const void*, const FAvidScriptBindingInvocationContext&,
		uint32, FAvidScriptVmCallFrame& Frame, TArray<FAvidScriptObjectHandle>&, FString&, FString&)
	{
		auto& Probe = *static_cast<const FProbe*>(Identity);
		Probe.Test.TestEqual(TEXT("delegate encoding sees target owner"), Probe.Runtime.HandleOwnerGetSlotImport(), static_cast<int32>(Probe.B.OwnerHandle.Slot));
		Frame.CellCount = 1; Frame.Cells[0] = 17;
		return true;
	}
	static EAvidScriptVmTypedHostStatus Observe(void* Opaque, int64 Token, int32& Result)
	{
		auto& P = *static_cast<FProbe*>(Opaque);
		Result = 1;
		const int32 Owner = P.Runtime.HandleOwnerGetSlotImport();
		uint32 State = 0;
		auto Bytes = MakeArrayView(reinterpret_cast<uint8*>(&State), sizeof(State));
		if (Token == 0)
		{
			++P.EventCalls;
			P.Test.TestEqual(TEXT("nested delegate executes for B"), Owner, static_cast<int32>(P.B.OwnerHandle.Slot));
			P.Test.TestEqual(TEXT("event cannot consume surrounding continuation state"), P.Runtime.HandleContinuationStateReadImport(P.CompletionB.Token, Bytes), 0);
			if (P.bInjectFailure)
			{
				FAvidScriptVmError Failure;
				P.Test.TestFalse(TEXT("nested trap fails"), P.Runtime.InvokeInContext(P.Trap, P.B, {}, Failure));
			}
			return EAvidScriptVmTypedHostStatus::Succeeded; // native code deliberately ignores an inner trap
		}
		++P.ContinuationCalls;
		if (Owner == static_cast<int32>(P.A.OwnerHandle.Slot))
		{
			FAvidScriptWasmSmokeResult Nested;
			P.Test.TestEqual(TEXT("nested event result follows the complete call chain"),
				P.Runtime.DispatchPreparedDelegateEventInContext(P.EventCall, P.B, P.Event, nullptr, Nested), !P.bInjectFailure);
			if (!P.bInjectFailure)
				P.Test.TestTrue(TEXT("B continuation may execute inside A continuation"),
					P.Runtime.DispatchContinuationInContext(P.Continuation, P.B, P.CompletionB, Nested));
			P.Test.TestEqual(TEXT("A owner restored after nested callbacks"), P.Runtime.HandleOwnerGetSlotImport(), Owner);
		}
		P.Test.TestEqual(TEXT("callback reads its own saved state once"), P.Runtime.HandleContinuationStateReadImport(Token, Bytes), 1);
		P.Test.TestEqual(TEXT("separate instance state is preserved"), State,
			Owner == static_cast<int32>(P.A.OwnerHandle.Slot) ? 111u : P.ExpectedBState);
		P.Test.TestEqual(TEXT("state cannot be consumed twice"), P.Runtime.HandleContinuationStateReadImport(Token, Bytes), 0);
		return EAvidScriptVmTypedHostStatus::Succeeded;
	}
};

bool Run(FAutomationTestBase& Test, EAvidScriptVmBackendKind Backend)
{
	if (GEngine == nullptr) return false;
	for (bool bFailure : {false, true})
	{
		Test.AddInfo(FString::Printf(TEXT("context callbacks backend=%d nestedFailure=%d"), static_cast<int32>(Backend), bFailure));
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptContextCallbacks"));
		if (World == nullptr) return false;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
		AActor* A = World->SpawnActor<AActor>();
		AActor* B = World->SpawnActor<AActor>();
		if (A == nullptr || B == nullptr) return false;
		FAvidScriptObjectRegistry Objects;
		FAvidScriptObjectHandleResult HandleResult;
		auto AContinuations = MakeShared<FAvidScriptSessionContinuations>();
		auto BContinuations = MakeShared<FAvidScriptSessionContinuations>();
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		FProbe Probe{Test, Runtime};
		Probe.bInjectFailure = bFailure;
		Probe.A.World = World; Probe.A.ObjectRegistry = &Objects;
		Probe.A.OwnerHandle = Objects.RegisterObject(A, HandleResult, false);
		Probe.A.Continuations = &AContinuations->ResetActive(World, &Objects, nullptr, Probe.A.OwnerHandle);
		Probe.B = Probe.A;
		Probe.B.OwnerHandle = Objects.RegisterObject(B, HandleResult, false);
		Probe.B.Continuations = &BContinuations->ResetActive(World, &Objects, nullptr, Probe.B.OwnerHandle);
		Runtime.SetHostContext(Probe.A);
		FAvidScriptVmTypedHostImport Import;
		Import.StableId = TEXT("context_callbacks"); Import.ModuleName = TEXT("avidscript"); Import.ImportName = TEXT("context_callback_probe");
		Import.Signature = TEXT("(I)i"); Import.Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get;
		Import.bSupplementalRuntimeAuthority = true; Import.PreparedTarget.Context = &Probe;
		Import.PreparedTarget.PackedSelfPropertyI32Get = &FProbe::Observe;
		FString Error;
		FAvidScriptWasmSmokeResult Result;
		const auto Wasm = BuildFixture();
		if (!Runtime.SetSupplementalTypedHostImports({&Import, 1}, Error)
			|| !Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("context_callbacks"), Result)
			|| !Runtime.ValidateRequiredExports({TEXT("avid_on_begin_play"), TEXT("avid_on_continuation_v2")}, Result)
			|| !Runtime.BeginPlay(Result)) { Test.AddError(Error + Result.ErrorMessage); return false; }
		FAvidScriptContextualExportCall Schedule;
		if (!Runtime.PrepareContextualExportCall(TEXT("schedule"), Schedule, Error)
			|| !Runtime.PrepareContextualExportCall(TEXT("avid_on_continuation_v2"), Probe.Continuation, Error)
			|| !Runtime.PrepareContextualExportCall(TEXT("on_event"), Probe.EventCall, Error)
			|| !Runtime.PrepareContextualExportCall(TEXT("trap"), Probe.Trap, Error)) { Test.AddError(Error); return false; }
		Probe.Event.StableId = TEXT("context_event"); Probe.Event.ExportName = TEXT("on_event");
		Probe.Event.Signature.ImmutableCodecIdentity = &Probe; Probe.Event.Signature.Encode = &FProbe::Encode;
		Probe.Event.Signature.ParameterCellCount = 1;
		auto Start = [&](const FAvidScriptWasmHostContext& Context, uint32 State)
		{
			FAvidScriptVmCallResult CallResult;
			FAvidScriptVmError Failure;
			if (!Runtime.InvokeInContext(Schedule, Context, {}, Failure, &CallResult)) { Test.AddError(Failure.Details); return int64(0); }
			int64 Token = 0; FMemory::Memcpy(&Token, CallResult.Cells, sizeof(Token));
			Test.TestTrue(TEXT("scoped Guest Timer await creates a continuation"), Token != 0);
			Test.TestTrue(TEXT("instance endpoint stores its state"), Context.Continuations->StoreState(Token,
				MakeArrayView(reinterpret_cast<const uint8*>(&State), sizeof(State))));
			return Token;
		};
		const int64 TokenA = Start(Probe.A, 111), TokenB = Start(Probe.B, 222);
		World->Tick(LEVELTICK_All, 0); ++GFrameCounter; World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
		TArray<FAvidScriptContinuationCompletion> ReadyA, ReadyB;
		AContinuations->DrainReady(ReadyA); BContinuations->DrainReady(ReadyB);
		if (!Test.TestEqual(TEXT("A Timer completes"), ReadyA.Num(), 1) || !Test.TestEqual(TEXT("B Timer completes"), ReadyB.Num(), 1)) return false;
		Probe.CompletionB = ReadyB[0];
		Test.TestEqual(TEXT("whole continuation chain result"), Runtime.DispatchContinuationInContext(Probe.Continuation, Probe.A, ReadyA[0], Result), !bFailure);
		if (bFailure) Test.TestFalse(TEXT("ignored nested failure has a diagnostic"), Result.ErrorCategory.IsEmpty());
		Test.TestEqual(TEXT("delegate called exactly once"), Probe.EventCalls, 1);
		Test.TestEqual(TEXT("continuation dispatch count"), Probe.ContinuationCalls, bFailure ? 1 : 2);
		Test.TestTrue(TEXT("A finalizes independently"), AContinuations->FinalizeDispatched(TokenA, !bFailure));
		Test.TestTrue(TEXT("B finalizes independently"), BContinuations->FinalizeDispatched(TokenB, !bFailure));
		Test.TestEqual(TEXT("A state released"), AContinuations->GetStateFrameByteCountForTesting(), 0);
		Test.TestEqual(TEXT("B state released"), BContinuations->GetStateFrameByteCountForTesting(), 0);
		Test.TestEqual(TEXT("base owner restored"), Runtime.HandleOwnerGetSlotImport(), static_cast<int32>(Probe.A.OwnerHandle.Slot));
		if (!bFailure)
		{
			Start(Probe.A, 333); Start(Probe.B, 444); Probe.ExpectedBState = 444;
			AContinuations->Teardown();
			Test.TestTrue(TEXT("A owner retires"), Objects.ReleaseHandle(Probe.A.OwnerHandle, HandleResult, false));
			World->Tick(LEVELTICK_All, 0); ++GFrameCounter; World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
			AContinuations->DrainReady(ReadyA); BContinuations->DrainReady(ReadyB);
			Test.TestEqual(TEXT("A teardown cancels only A"), ReadyA.Num(), 0);
			if (!Test.TestEqual(TEXT("B remains scheduled"), ReadyB.Num(), 1)) return false;
			Test.TestTrue(TEXT("B callback survives original owner retirement"), Runtime.DispatchContinuationInContext(Probe.Continuation, Probe.B, ReadyB[0], Result));
			Test.TestTrue(TEXT("B survivor finalizes"), BContinuations->FinalizeDispatched(ReadyB[0].Token, true));
			Test.TestFalse(TEXT("retired A event cannot enter Guest"), Runtime.DispatchPreparedDelegateEventInContext(Probe.EventCall, Probe.A, Probe.Event, nullptr, Result));
			Test.TestFalse(TEXT("different export cannot be used as continuation entry"), Runtime.DispatchContinuationInContext(Probe.EventCall, Probe.B, ReadyB[0], Result));
			Test.TestEqual(TEXT("callback contract mismatch diagnostic"), Result.ErrorCategory, FString(TEXT("context_callback_contract")));
		}
		AContinuations->Teardown(); BContinuations->Teardown();
		Runtime.Unload();
		if (!Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("replacement"), Result)
			|| !Runtime.ValidateRequiredExports({TEXT("avid_on_begin_play"), TEXT("avid_on_continuation_v2")}, Result)
			|| !Runtime.BeginPlay(Result)) return false;
		Test.TestFalse(TEXT("old callback route cannot enter replacement code"), Runtime.DispatchPreparedDelegateEventInContext(Probe.EventCall, Probe.B, Probe.Event, nullptr, Result));
		Test.TestEqual(TEXT("retired code callback diagnostic"), Result.ErrorCategory, FString(TEXT("context_invocation_code")));
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptContextCallbacksTest,
	"AvidScript.Runtime.GeneratedTypes.SharedRuntimeCallbacks", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAvidScriptContextCallbacksTest::RunTest(const FString& Parameters)
{
	return AvidScriptContextCallbackTests::Run(*this, EAvidScriptVmBackendKind::Wasmtime)
		&& AvidScriptContextCallbackTests::Run(*this, EAvidScriptVmBackendKind::Wamr);
}

namespace AvidScriptInstanceExecutionTests
{
TArray<uint8> BuildFixture()
{
	using namespace AvidScriptContextInvocationTests;
	TArray<uint8> Module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	Section(Module, 1, {7, 0x60, 1, 0x7e, 1, 0x7f, 0x60, 2, 0x7d, 0x7f, 1, 0x7f,
		0x60, 1, 0x7f, 1, 0x7f, 0x60, 0, 0, 0x60, 1, 0x7d, 0, 0x60, 2, 0x7f, 0x7f, 0, 0x60, 0, 1, 0x7f});
	TArray<uint8> Imports{3};
	Name(Imports, "avidscript"); Name(Imports, "instance_probe"); Imports.Append({0, 0});
	Name(Imports, "avidscript"); Name(Imports, "timer_set_once"); Imports.Append({0, 1});
	Name(Imports, "avidscript"); Name(Imports, "timer_cancel"); Imports.Append({0, 2});
	Section(Module, 2, Imports);
	Section(Module, 3, {7, 3, 4, 3, 5, 6, 6, 2});
	Section(Module, 6, {1, 0x7f, 1, 0x41, 0, 0x0b});
	TArray<uint8> Exports{7};
	for (const auto& Item : {TPair<const char*, uint8>{"avid_on_begin_play", 3}, {"avid_on_tick", 4},
		{"avid_on_end_play", 5}, {"avid_on_timer", 6}, {"schedule", 7}, {"get_shared", 8}, {"cancel", 9}})
	{
		Name(Exports, Item.Key); Exports.Append({0, Item.Value});
	}
	Section(Module, 7, Exports);
	const TArray<uint8> Begin{0, 0x23, 0, 0x41, 1, 0x6a, 0x24, 0,
		0x43, 0xcd, 0xcc, 0xcc, 0x3d, 0x41, 17, 0x10, 1, 0x1a, 0x42, 1, 0x10, 0, 0x1a, 0x0b},
		Tick{0, 0x42, 2, 0x10, 0, 0x1a, 0x0b}, End{0, 0x42, 3, 0x10, 0, 0x1a, 0x0b},
		Timer{0, 0x23, 0, 0x41, 1, 0x6a, 0x24, 0, 0x42, 4, 0x10, 0, 0x1a, 0x0b},
		Schedule{0, 0x43, 0xcd, 0xcc, 0xcc, 0x3d, 0x41, 17, 0x10, 1, 0x0b},
		Get{0, 0x23, 0, 0x0b}, Cancel{0, 0x20, 0, 0x10, 2, 0x0b};
	TArray<uint8> Code{7};
	for (const auto* Body : {&Begin, &Tick, &End, &Timer, &Schedule, &Get, &Cancel}) { U32(Code, Body->Num()); Code.Append(*Body); }
	Section(Module, 10, Code);
	return Module;
}

struct FProbe
{
	FAutomationTestBase& Test;
	FAvidScriptWasmRuntimeInstance& Runtime;
	FAvidScriptWasmHostContext A, B;
	int32 Calls[2][4] = {};
	bool bRejectActiveEnd = false;
	static EAvidScriptVmTypedHostStatus Observe(void* Opaque, int64 Phase, int32& Result)
	{
		auto& P = *static_cast<FProbe*>(Opaque);
		Result = 1;
		const int32 Owner = P.Runtime.HandleOwnerGetSlotImport();
		const int32 Index = Owner == static_cast<int32>(P.A.OwnerHandle.Slot) ? 0 : 1;
		P.Test.TestEqual(TEXT("instance callback uses its owner"), Owner,
			static_cast<int32>(Index == 0 ? P.A.OwnerHandle.Slot : P.B.OwnerHandle.Slot));
		if (Phase < 1 || Phase > 4) return EAvidScriptVmTypedHostStatus::Rejected;
		++P.Calls[Index][Phase - 1];
		if (Phase == 2 && Index == 0)
		{
			FString RetirementError;
			P.Test.TestFalse(TEXT("active instance state cannot retire"), P.Runtime.RetireInstanceExecutionState(P.A.InstanceExecutionState, RetirementError));
			FAvidScriptWasmSmokeResult Nested;
			if (P.bRejectActiveEnd)
				P.Test.TestFalse(TEXT("active A cannot EndPlay inside its Tick"), P.Runtime.EndPlayInContext(P.A, Nested));
			else
				P.Test.TestTrue(TEXT("A Tick may synchronously Tick idle B"), P.Runtime.TickInContext(P.B, 0.05f, Nested));
			P.Test.TestEqual(TEXT("A restored after nested lifecycle"), P.Runtime.HandleOwnerGetSlotImport(), Owner);
		}
		return EAvidScriptVmTypedHostStatus::Succeeded; // deliberately ignore a rejected nested lifecycle
	}
};

bool Run(FAutomationTestBase& Test, EAvidScriptVmBackendKind Backend)
{
	for (bool bFailure : {false, true})
	{
		Test.AddInfo(FString::Printf(TEXT("instance lifecycle backend=%d nestedFailure=%d"), static_cast<int32>(Backend), bFailure));
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptInstanceExecution"));
		if (!World || !GEngine) return false;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
		AActor* A = World->SpawnActor<AActor>(); AActor* B = World->SpawnActor<AActor>();
		if (!A || !B) return false;
		FAvidScriptObjectRegistry Objects;
		FAvidScriptObjectHandleResult HandleResult;
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		FProbe Probe{Test, Runtime}; Probe.bRejectActiveEnd = bFailure;
		Probe.A.World = World; Probe.A.ObjectRegistry = &Objects;
		Probe.A.OwnerHandle = Objects.RegisterObject(A, HandleResult, false);
		Probe.B = Probe.A; Probe.B.OwnerHandle = Objects.RegisterObject(B, HandleResult, false);
		Runtime.SetHostContext(Probe.A);
		FAvidScriptVmTypedHostImport Import;
		Import.StableId = TEXT("instance_probe"); Import.ModuleName = TEXT("avidscript"); Import.ImportName = TEXT("instance_probe");
		Import.Signature = TEXT("(I)i"); Import.Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get;
		Import.bSupplementalRuntimeAuthority = true; Import.PreparedTarget.Context = &Probe;
		Import.PreparedTarget.PackedSelfPropertyI32Get = &FProbe::Observe;
		FString Error;
		FAvidScriptWasmSmokeResult Result;
		const auto Wasm = BuildFixture();
		if (!Runtime.SetSupplementalTypedHostImports({&Import, 1}, Error)
			|| !Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("instance_lifecycle"), Result)
			|| !Runtime.ValidateRequiredExports({TEXT("avid_on_begin_play"), TEXT("avid_on_tick"), TEXT("avid_on_end_play"), TEXT("avid_on_timer")}, Result)
			|| !Runtime.CreateInstanceExecutionState(Probe.A, Probe.A.InstanceExecutionState, Error)
			|| !Runtime.CreateInstanceExecutionState(Probe.B, Probe.B.InstanceExecutionState, Error)) { Test.AddError(Error + Result.ErrorMessage); return false; }
		if (!Runtime.BeginPlayInContext(Probe.A, Result) || !Runtime.BeginPlayInContext(Probe.B, Result)) { Test.AddError(Result.ErrorMessage); return false; }
		Test.TestEqual(TEXT("module default lifecycle remains Loaded"), Runtime.GetLifecycleState(), EAvidScriptLifecycleState::Loaded);
		Test.TestEqual(TEXT("each BeginPlay runs once for A"), Probe.Calls[0][0], 1);
		Test.TestEqual(TEXT("each BeginPlay runs once for B"), Probe.Calls[1][0], 1);
		Test.TestFalse(TEXT("double BeginPlay rejected"), Runtime.BeginPlayInContext(Probe.B, Result));
		Test.TestEqual(TEXT("rejected duplicate does not stop B"), Probe.B.InstanceExecutionState->GetLifecycleState(), EAvidScriptLifecycleState::Running);
		Test.TestEqual(TEXT("A initial pending timer"), Probe.A.InstanceExecutionState->GetPendingTimerCount(), 1);
		Test.TestEqual(TEXT("B initial pending timer"), Probe.B.InstanceExecutionState->GetPendingTimerCount(), 1);
		Test.TestEqual(TEXT("A Tick follows complete nested chain"), Runtime.TickInContext(Probe.A, 0.2f, Result), !bFailure);
		if (bFailure)
		{
			Test.TestEqual(TEXT("failed Tick never dispatches A due timer"), Probe.Calls[0][3], 0);
			Test.TestEqual(TEXT("failed A state recorded"), Probe.A.InstanceExecutionState->GetLifecycleState(), EAvidScriptLifecycleState::Faulted);
			Test.TestEqual(TEXT("nested error propagated"), Result.ErrorCategory, FString(TEXT("context_instance_lifecycle")));
		}
		else
		{
			Test.TestEqual(TEXT("A timer fired"), Probe.Calls[0][3], 1);
			Test.TestEqual(TEXT("B clock advanced only by its nested Tick"), Probe.Calls[1][3], 0);
			Test.TestEqual(TEXT("nested B has independent tick count"), Probe.B.InstanceExecutionState->GetTickCallCount(), 1);
			Test.TestEqual(TEXT("A has independent tick count"), Probe.A.InstanceExecutionState->GetTickCallCount(), 1);
			FAvidScriptContextualExportCall Schedule, Get, Cancel;
			if (!Runtime.PrepareContextualExportCall(TEXT("schedule"), Schedule, Error)
				|| !Runtime.PrepareContextualExportCall(TEXT("get_shared"), Get, Error)
				|| !Runtime.PrepareContextualExportCall(TEXT("cancel"), Cancel, Error)) { Test.AddError(Error); return false; }
			FAvidScriptVmError Failure;
			FAvidScriptVmCallResult Value;
			Test.TestTrue(TEXT("schedule A before ending"), Runtime.InvokeInContext(Schedule, Probe.A, {}, Failure, &Value));
			Test.TestTrue(TEXT("A EndPlay succeeds"), Runtime.EndPlayInContext(Probe.A, Result));
			Test.TestTrue(TEXT("A EndPlay is idempotent"), Runtime.EndPlayInContext(Probe.A, Result));
			Test.TestEqual(TEXT("A guest EndPlay runs once"), Probe.Calls[0][2], 1);
			Test.TestEqual(TEXT("A timers cancelled"), Probe.A.InstanceExecutionState->GetPendingTimerCount(), 0);
			Test.TestEqual(TEXT("B timer retained"), Probe.B.InstanceExecutionState->GetPendingTimerCount(), 1);
			Test.TestTrue(TEXT("B runs after A stops"), Runtime.TickInContext(Probe.B, 0.06f, Result));
			Test.TestEqual(TEXT("B timer uses B owner"), Probe.Calls[1][3], 1);
			Test.TestTrue(TEXT("B can read shared globals"), Runtime.InvokeInContext(Get, Probe.B, {}, Failure, &Value));
			Test.TestEqual(TEXT("one VM retained both BeginPlay and Timer effects"), Value.Cells[0], 4u);
			Test.TestFalse(TEXT("stopped A cannot enter ordinary code"), Runtime.InvokeInContext(Get, Probe.A, {}, Failure, &Value));
			auto WrongOwner = Probe.A; WrongOwner.InstanceExecutionState = Probe.B.InstanceExecutionState;
			Test.TestFalse(TEXT("B execution state cannot authorize A"), Runtime.InvokeInContext(Get, WrongOwner, {}, Failure, &Value));
			Test.TestEqual(TEXT("wrong owner leaves B running"), Probe.B.InstanceExecutionState->GetLifecycleState(), EAvidScriptLifecycleState::Running);
			Test.TestTrue(TEXT("A state retires without unloading VM"), Runtime.RetireInstanceExecutionState(Probe.A.InstanceExecutionState, Error));
			Test.TestTrue(TEXT("A handle released"), Objects.ReleaseHandle(Probe.A.OwnerHandle, HandleResult, false));
			Test.TestTrue(TEXT("B survives retired original owner"), Runtime.InvokeInContext(Schedule, Probe.B, {}, Failure, &Value));
			FAvidScriptVmCallFrame CancelFrame; CancelFrame.CellCount = 1; CancelFrame.Cells[0] = Value.Cells[0];
			Test.TestTrue(TEXT("B cancels its timer"), Runtime.InvokeInContext(Cancel, Probe.B, CancelFrame, Failure, &Value));
			Test.TestEqual(TEXT("B cancellation succeeds"), Value.Cells[0], 1u);
			Test.TestEqual(TEXT("B cancellation affects only B queue"), Probe.B.InstanceExecutionState->GetPendingTimerCount(), 0);
			Test.TestTrue(TEXT("pending timer before module unload"), Runtime.InvokeInContext(Schedule, Probe.B, {}, Failure, &Value));
		}
		Runtime.Unload();
		Test.TestTrue(TEXT("module unload retires A"), Probe.A.InstanceExecutionState->IsRetired());
		Test.TestTrue(TEXT("module unload retires B"), Probe.B.InstanceExecutionState->IsRetired());
		Test.TestEqual(TEXT("module unload clears all instance timers"), Probe.B.InstanceExecutionState->GetPendingTimerCount(), 0);
		if (!Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("replacement"), Result)) return false;
		Test.TestFalse(TEXT("old instance lifecycle cannot enter replacement VM"), Runtime.BeginPlayInContext(Probe.B, Result));
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptInstanceExecutionStateTest,
	"AvidScript.Runtime.GeneratedTypes.SharedInstanceLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAvidScriptInstanceExecutionStateTest::RunTest(const FString& Parameters)
{
	return AvidScriptInstanceExecutionTests::Run(*this, EAvidScriptVmBackendKind::Wasmtime)
		&& AvidScriptInstanceExecutionTests::Run(*this, EAvidScriptVmBackendKind::Wamr);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptProductionInstanceEntryTest,
	"AvidScript.Runtime.GeneratedTypes.ProductionInstanceEntry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAvidScriptProductionInstanceEntryTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptContextInvocationTests;
	const FString Json = FString::Printf(TEXT(R"JSON({
"schema_version":6,"generator_version":"1.8","module_name":"AvidScriptRuntime",
"generation_key_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
"types":[{"type_ordinal":0,"stable_type_id":"type:production-entry","engine_name":"AvidScriptGeneratedTypeSessionTestObject",
"class_path":"%s","properties":[],"functions":[{"member_ordinal":0,"stable_member_id":"function:entry",
"native_name":"GetScriptValue","export_name":"avid_ue_0123456789abcdef0123456789abcdef","flags":[]}]}]})JSON"),
		*UAvidScriptGeneratedTypeSessionTestObject::StaticClass()->GetPathName());
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types; FString Error;
	if (!FAvidScriptGeneratedTypeRegistry::BuildFromJson(Json, Types, Error)) { AddError(Error); return false; }
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const auto Scenario : {EScenario::Normal, EScenario::Trap, EScenario::Mutate, EScenario::ForeignCode,
		EScenario::World, EScenario::Retire, EScenario::Depth, EScenario::Entries, EScenario::Frame, EScenario::Suspended,
		EScenario::RootReturn, EScenario::RootRestore, EScenario::RootNoGrant, EScenario::RootWrongGrant, EScenario::RootInvalid,
		EScenario::RootRelease, EScenario::RootAllocate, EScenario::RootCreate, EScenario::RootShadow, EScenario::RootDepth, EScenario::RootRegrant})
	{
		const bool bRootScenario = IsRootScenario(Scenario);
		const auto Wasm = bRootScenario ? BuildRootTransferFixture(Scenario) : BuildFixture(true);
		AddInfo(FString::Printf(TEXT("production entry backend=%d scenario=%d"), static_cast<int32>(Backend), static_cast<int32>(Scenario)));
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		auto Runtime = MakeShared<FAvidScriptWasmRuntimeInstance>(Selection);
		FAvidScriptWasmRuntimeInstance OtherRuntime(Selection);
		FAvidScriptObjectRegistry Registry;
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> A(NewObject<UAvidScriptGeneratedTypeSessionTestObject>()), B(NewObject<UAvidScriptGeneratedTypeSessionTestObject>());
		TStrongObjectPtr<UWorld> OtherWorld(NewObject<UWorld>());
		TStrongObjectPtr<UAvidScriptGeneratedTypeSessionTestObject> Other(NewObject<UAvidScriptGeneratedTypeSessionTestObject>(OtherWorld.Get()));
		FAvidScriptObjectHandleResult HandleResult;
		FProbe Probe{*this, *Runtime}; Probe.bProduction = true; Probe.Scenario = Scenario;
		FRootTransferProbe RootProbe{*this, *Runtime, {}, {}, {}, {}, {}, Scenario};
		Probe.A.ObjectRegistry = &Registry; Probe.A.OwnerHandle = Registry.RegisterObject(A.Get(), HandleResult, false);
		Probe.B = Probe.A; Probe.B.OwnerHandle = Registry.RegisterObject(B.Get(), HandleResult, false);
		Probe.OtherTarget = Registry.RegisterObject(Other.Get(), HandleResult, false);
		FAvidScriptRuntimeSession SA, SB;
		SA.SetHostContext(Probe.A); SB.SetHostContext(Probe.B);
		ON_SCOPE_EXIT { FAvidScriptWasmSmokeResult Stopped; SA.StopAndUnload(Stopped); SB.StopAndUnload(Stopped); };
		if (!SA.ConfigureGeneratedTypeInstance(*A, Probe.A.OwnerHandle, 0, Types, Error)
			|| !SB.ConfigureGeneratedTypeInstance(*B, Probe.B.OwnerHandle, 0, Types, Error)) { AddError(Error); return false; }
		Probe.SourceSession = &SA; Probe.PeerSession = &SB;
		Runtime->SetHostContext(Probe.A);
		FAvidScriptVmTypedHostImport Import;
		Import.StableId = TEXT("production_instance_probe"); Import.ModuleName = TEXT("avidscript"); Import.ImportName = TEXT("context_reenter");
		Import.Signature = TEXT("(I)i"); Import.Shape = EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get;
		Import.bSupplementalRuntimeAuthority = true; Import.PreparedTarget.Context = &Probe;
		Import.PreparedTarget.PackedSelfPropertyI32Get = &FProbe::Reenter;
		if (bRootScenario)
		{
			Import.PreparedTarget.Context = &RootProbe;
			Import.PreparedTarget.PackedSelfPropertyI32Get = &FRootTransferProbe::Reenter;
		}
		FAvidScriptWasmSmokeResult Loaded;
		if (!Runtime->SetSupplementalTypedHostImports({&Import, 1}, Error)
			|| !Runtime->LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("production_instance_entry"), Loaded)
			|| !Runtime->ValidateRequiredExports({TEXT("avid_on_begin_play")}, Loaded))
		{ AddError(Error + Loaded.ErrorMessage); return false; }
		FAvidScriptWasmReloadManifest Manifest;
		Manifest.ModuleId = TEXT("production_instance_entry"); Manifest.Language = TEXT("wasm");
		Manifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion;
		Manifest.RequiredExports = {TEXT("avid_on_begin_play")};
		const auto Artifact = FAvidScriptRuntimeArtifact::FromCanonicalWasm(Manifest, Wasm, Selection);
		// Inject only the native probe import before load. All instance activation,
		// prepared native entry, nested authority selection and cleanup use production paths.
		auto Domain = MakeShared<FAvidScriptRuntimeExecutionDomain>(Runtime, Probe.A, Types);
		FAvidScriptWasmReloadResult Joined;
		if (!SA.LoadGeneratedDomainArtifact(Artifact, Domain, Joined) || !SB.LoadGeneratedDomainArtifact(Artifact, Domain, Joined))
		{ AddError(Joined.ErrorMessage); return false; }
		Probe.A = SA.HostContext; Probe.B = SB.HostContext;
		RootProbe.A = Probe.A; RootProbe.B = Probe.B;
		if (bRootScenario && (!Runtime->PrepareContextualExportCall(TEXT("step"), RootProbe.Step, Error)
			|| !Runtime->PrepareContextualExportCall(TEXT("count"), RootProbe.Count, Error)
			|| !Runtime->PrepareContextualExportCall(TEXT("leaf"), RootProbe.Leaf, Error))) { AddError(Error); return false; }
		if (!Runtime->PrepareContextualExportCall(TEXT("step"), Probe.Step, Error)
			|| !Runtime->PrepareContextualExportCall(TEXT("trap"), Probe.Trap, Error)) { AddError(Error); return false; }
		if (Scenario == EScenario::ForeignCode)
		{
			if (!OtherRuntime.SetSupplementalTypedHostImports({&Import, 1}, Error)
				|| !OtherRuntime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("foreign_instance_entry"), Loaded)
				|| !OtherRuntime.PrepareContextualExportCall(TEXT("step"), Probe.OtherCode, Error)) return false;
		}
		FAvidScriptVmError Rejected; FAvidScriptVmCallResult NoResult;
		TestFalse(TEXT("instance route cannot start without an active caller"), Runtime->InvokeGeneratedInstanceExport(Probe.B.OwnerHandle, Probe.Step, {}, Rejected, &NoResult));
		TestEqual(TEXT("idle route rejection has source category"), Rejected.Category, FString(TEXT("generated_invocation_source")));
		if (Scenario == EScenario::Suspended) SB.SuspendForApplicationLifecycle(1);
		int32 Value = -1;
		const bool bCalled = FAvidScriptGeneratedTypeDispatcher::Invoke(A.Get(), 0, 0, {}, &Value);
		if (bRootScenario)
		{
			TestEqual(TEXT("production returned-root authorization result"), bCalled, RootSucceeds(Scenario));
			TestFalse(TEXT("returned-root chain has exited"), Runtime->IsContextInvocationActive());
			TestEqual(TEXT("expected returned-root call count"), RootProbe.Calls,
				Scenario == EScenario::RootRestore || Scenario == EScenario::RootShadow || Scenario == EScenario::RootDepth || Scenario == EScenario::RootRegrant ? 2 : 1);
			if (RootSucceeds(Scenario))
			{
				TestEqual(TEXT("returned-root callee completes"), Value, 99);
				TestTrue(TEXT("host GC verified the returned object"), RootProbe.bHostGcPreservedReturn);
				auto* Heap = Runtime->GetManagedHeapForTesting();
				TestEqual(TEXT("returned-root chain releases all frames"), Heap->GetStats().ActiveFrames, 0u);
				TestEqual(TEXT("returned-root chain releases all roots"), Heap->GetStats().LiveRoots, 0u);
				TestTrue(TEXT("returned graph collects after caller exits"), Heap->Collect() == AvidScript::Managed::EHeapError::Ok);
				TestEqual(TEXT("returned-root chain has no retained object"), Heap->GetStats().LiveObjects, 0u);
			}
			else
			{
				TestFalse(TEXT("root authority failure unloads shared VM after outer return"), Runtime->IsLoaded());
				TestTrue(TEXT("root authority failure quarantines whole domain"), SA.GetSnapshot().bFaultQuarantined && SB.GetSnapshot().bFaultQuarantined);
				TestFalse(TEXT("root authority failure is diagnosed"), RootProbe.LastError.IsEmpty());
				if (Scenario == EScenario::RootInvalid || Scenario == EScenario::RootRegrant)
					TestEqual(TEXT("invalid loan rejected by production entry"), RootProbe.LastError, FString(TEXT("generated_invocation_roots")));
			}
			continue;
		}
		TestEqual(TEXT("production call succeeds only for the authorized complete chain"), bCalled, Scenario == EScenario::Normal);
		TestFalse(TEXT("outer chain has fully returned"), Runtime->IsContextInvocationActive());
		if (Scenario == EScenario::Normal)
		{
			TestEqual(TEXT("production A observes shared object after B and nested A"), Value, 3);
			TestEqual(TEXT("exactly two owner switches"), Probe.Calls, 2);
			TArray<uint8> Bytes; Bytes.SetNumZeroed(44);
			if (!TestTrue(TEXT("inspect same-domain alias and context results"), Runtime->ReadStateBytes(560, Bytes, Error))) return false;
			auto Read = [&](int32 Offset) { uint32 V; FMemory::Memcpy(&V, Bytes.GetData() + Offset, 4); return V; };
			TestEqual(TEXT("two argument pointers preserve one location across all entries"), Read(0), 6u);
			for (int32 Depth = 0; Depth < 3; ++Depth)
			{
				uint32 Slot = Depth == 1 ? Probe.B.OwnerHandle.Slot : Probe.A.OwnerHandle.Slot;
				TestEqual(TEXT("selected Session supplies owner imports"), Read(16 + Depth * 4), Slot);
				TestEqual(TEXT("nested return restores calling Session"), Read(32 + Depth * 4), Slot);
			}
			auto* Heap = Runtime->GetManagedHeapForTesting();
			TestEqual(TEXT("production chain releases all frames"), Heap->GetStats().ActiveFrames, 0u);
			TestEqual(TEXT("production chain releases all roots"), Heap->GetStats().LiveRoots, 0u);
			TestTrue(TEXT("unrooted object is collectable"), Heap->Collect() == AvidScript::Managed::EHeapError::Ok);
			TestEqual(TEXT("no leaked object after production dispatch"), Heap->GetStats().LiveObjects, 0u);
		}
		else
		{
			TestTrue(TEXT("nested failure quarantines both Sessions"), SA.GetSnapshot().bFaultQuarantined && SB.GetSnapshot().bFaultQuarantined);
			TestFalse(TEXT("outermost return unloads the failed shared VM"), Runtime->IsLoaded());
			TestTrue(TEXT("fault cleanup retires both instance states"), Probe.A.InstanceExecutionState->IsRetired() && Probe.B.InstanceExecutionState->IsRetired());
			TestFalse(TEXT("fault diagnostic is retained"), SA.GetSnapshot().FaultCategory.IsEmpty());
			const TCHAR* Expected = Scenario == EScenario::ForeignCode ? TEXT("context_invocation_code")
				: Scenario == EScenario::World ? TEXT("generated_invocation_target")
				: Scenario == EScenario::Suspended ? TEXT("generated_invocation_state")
				: Scenario == EScenario::Depth || Scenario == EScenario::Entries ? TEXT("context_invocation_budget")
				: Scenario == EScenario::Frame ? TEXT("invalid_arguments")
				: Scenario == EScenario::Mutate ? TEXT("context_invocation_mutation") : nullptr;
			if (Expected) TestEqual(TEXT("root retains original nested failure category"), SA.GetSnapshot().FaultCategory, FString(Expected));
			if (Scenario == EScenario::Trap) TestEqual(TEXT("writes before nested trap retain alias order"), Probe.WritesBeforeFailure, 4u);
			TestFalse(TEXT("failed domain rejects peer entry"), FAvidScriptGeneratedTypeDispatcher::Invoke(B.Get(), 0, 0, {}, &Value));
		}
	}
	return true;
}
#endif
