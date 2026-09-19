#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptWasmRuntime.h"
#include "AvidScriptManagedHeapAbi.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Tests/AvidScriptGeneratedTypeSessionTestTypes.h"
#include "Engine/World.h"
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
TArray<uint8> BuildFixture()
{
	using namespace AvidScript::Managed;
	TArray<uint8> Module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	Section(Module, 1, {5, 0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f,
		0x60, 1, 0x7e, 1, 0x7f, 0x60, 0, 1, 0x7f, 0x60, 0, 0, 0x60, 1, 0x7f, 1, 0x7f});
	TArray<uint8> Imports{3};
	for (const auto& Item : {TPair<const char*, uint8>{Abi::ImportName, 0}, {"context_reenter", 1}, {"owner_get_slot", 2}})
	{
		Name(Imports, "avidscript"); Name(Imports, Item.Key); Imports.Append({0, Item.Value});
	}
	Section(Module, 2, Imports); Section(Module, 3, {4, 3, 4, 2, 2});
	Section(Module, 5, {1, 1, 1, 1}); Section(Module, 6, {1, 0x7f, 1, 0x41, 0, 0x0b});
	TArray<uint8> Exports{5};
	Name(Exports, "memory"); Exports.Append({2, 0});
	for (const auto& Item : {TPair<const char*, uint8>{"avid_on_begin_play", 3}, {"step", 4}, {"trap", 5}, {"count", 6}})
	{
		Name(Exports, Item.Key); Exports.Append({0, Item.Value});
	}
	Section(Module, 7, Exports);
	TArray<uint8> Init{0}; HeapCall(Init, 64, 24); Init.Add(0x0b);
	TArray<uint8> Step{0};
	// Only the root creates the shared object; nested entries root that same token.
	Step.Append({0x20, 0, 0x41, 2, 0x46, 0x04, 0x40});
	Constant(Step, 528); Step.Append({0x42, 0, 0x37, 0, 0});
	Constant(Step, 560); Constant(Step, 0); Step.Append({0x36, 0, 0, 0x0b});
	HeapCall(Step, 96, 8, 512, 8);
	CopyToken(Step, 512, 120); CopyToken(Step, 528, 128); HeapCall(Step, 112, 24, 520, 8);
	Step.Append({0x20, 0, 0x41, 2, 0x46, 0x04, 0x40});
	CopyToken(Step, 520, 156); HeapCall(Step, 144, 20, 528, 8); Step.Add(0x0b);
	CopyToken(Step, 528, 184); CopyToken(Step, 528, 216);
	HeapCall(Step, 176, 28, 536, 4);
	Constant(Step, 236); Constant(Step, 536); Step.Append({0x28, 0, 0, 0x41, 1, 0x6a, 0x36, 0, 0});
	HeapCall(Step, 208, 32);
	// Read and write through the two aliased pointer values, not independent copies.
	for (int32 Pointer : {544, 548})
	{
		Constant(Step, Pointer); Step.Append({0x28, 0, 0});
		Constant(Step, Pointer); Step.Append({0x28, 0, 0, 0x28, 0, 0, 0x41, 1, 0x6a, 0x36, 0, 0});
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
	TArray<uint8> Code{4};
	const TArray<uint8>* Bodies[] = {&Init, &Step, &Trap, &Count};
	for (const auto* Body : Bodies) { U32(Code, Body->Num()); Code.Append(*Body); }
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

enum class EScenario { Normal, Trap, Unload, Mutate, ForeignRegistry, ForeignCode, World, Retire, Timer, Depth, Entries };
struct FProbe
{
	FAutomationTestBase& Test;
	FAvidScriptWasmRuntimeInstance& Runtime;
	FAvidScriptWasmHostContext A, B, Foreign;
	FAvidScriptContextualExportCall Step, Trap, OtherCode;
	EScenario Scenario = EScenario::Normal;
	int32 Calls = 0;
	TWeakObjectPtr<UWorld> WrongWorld;

	static EAvidScriptVmTypedHostStatus Reenter(void* Context, int64 Remaining, int32& Value)
	{
		auto& Self = *static_cast<FProbe*>(Context);
		const uint32 ParentSlot = static_cast<uint32>(Self.Runtime.HandleOwnerGetSlotImport());
		Self.Test.TestTrue(TEXT("subscription import uses current instance"), Self.Runtime.HandleEventUnsubscribeImport(1) == 1);
		if (++Self.Calls > 70) return EAvidScriptVmTypedHostStatus::Rejected;
		FAvidScriptWasmHostContext Target = Remaining == 1 ? Self.B : Self.A;
		FAvidScriptVmCallFrame Frame; Frame.CellCount = 1; Frame.Cells[0] = static_cast<uint32>(Remaining);
		const FAvidScriptContextualExportCall* Entry = &Self.Step;
		if (Self.Scenario == EScenario::Trap && Remaining == 0) { Entry = &Self.Trap; Frame = {}; }
		if (Self.Scenario == EScenario::ForeignRegistry) Target = Self.Foreign;
		if (Self.Scenario == EScenario::ForeignCode) Entry = &Self.OtherCode;
		if (Self.Scenario == EScenario::World) Target.World = Self.WrongWorld;
		if (Self.Scenario == EScenario::Depth) Frame.Cells[0] = 1;
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
		if (Self.Scenario == EScenario::Entries)
		{
			Frame.Cells[0] = 0;
			int32 Succeeded = 0;
			for (int32 I = 0; I < 4096; ++I)
				if (Self.Runtime.InvokeInContext(Self.Step, Target, Frame, Error, &Result)) ++Succeeded;
			Self.Test.TestEqual(TEXT("sequential calls consume the same root entry budget"), Succeeded, 4095);
			return EAvidScriptVmTypedHostStatus::Succeeded;
		}
		const uint32 Frames = Self.Runtime.GetManagedHeapForTesting()->GetStats().ActiveFrames;
		const bool bCalled = Self.Runtime.InvokeInContext(*Entry, Target, Frame, Error, &Result);
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
#endif
