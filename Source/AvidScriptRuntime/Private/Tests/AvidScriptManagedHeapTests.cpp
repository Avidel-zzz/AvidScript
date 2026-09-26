#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptWasmRuntime.h"
#include "AvidScriptRuntimeSession.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "AvidScriptManagedHeapAbi.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <array>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptManagedHeapOwnershipTest,
	"AvidScript.Runtime.ManagedHeap.Ownership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptManagedHeapOwnershipTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace AvidScript::Managed;
	FAvidScriptWasmRuntimeInstance Live, Candidate;
	FAvidScriptWasmSmokeResult Result;
	if (!TestTrue(TEXT("Live module loads"), Live.LoadEmbeddedSmokeModule(Result))) return false;
	FHeap* LiveHeap = Live.GetManagedHeapForTesting();
	if (!TestNotNull(TEXT("Loaded module owns heap"), LiveHeap)) return false;
	const std::array<FHeapLayout, 1> Layouts{{{1, 8, {}}}};
	FToken Root = 0, Object = 0;
	TestTrue(TEXT("Configure environment layout"), LiveHeap->Configure(Layouts) == EHeapError::Ok);
	TestTrue(TEXT("Create persistent root"), LiveHeap->CreateRoot(0, 0, Root) == EHeapError::Ok);
	TestTrue(TEXT("Allocate environment"), LiveHeap->Allocate(1, Root, Object) == EHeapError::Ok);
	TestFalse(TEXT("Persistent object has no invocation frame root"),
		LiveHeap->IsObjectRootedInCurrentFrame(Object));
	FToken InvocationFrame = 0, InvocationRoot = 0, InvocationObject = 0;
	TestTrue(TEXT("Create invocation frame"), LiveHeap->PushFrame(InvocationFrame) == EHeapError::Ok);
	TestTrue(TEXT("Create invocation root"),
		LiveHeap->CreateRoot(InvocationFrame, 0, InvocationRoot) == EHeapError::Ok);
	TestTrue(TEXT("Allocate invocation object"),
		LiveHeap->Allocate(1, InvocationRoot, InvocationObject) == EHeapError::Ok);
	TestTrue(TEXT("Current frame owns invocation object"),
		LiveHeap->IsObjectRootedInCurrentFrame(InvocationObject));
	TestFalse(TEXT("Invocation floor cannot claim an older frame"),
		LiveHeap->IsObjectRootedInCurrentFrame(InvocationObject, 1));
	TestFalse(TEXT("Persistent object is not a current-frame language error"),
		LiveHeap->IsObjectRootedInCurrentFrame(Object));
	TestTrue(TEXT("Close invocation frame"), LiveHeap->PopFrame(InvocationFrame) == EHeapError::Ok);
	TestFalse(TEXT("Retired invocation root is rejected"),
		LiveHeap->IsObjectRootedInCurrentFrame(InvocationObject));
	const uint8 InvalidModule[] = {0, 0, 0, 0};
	TestFalse(TEXT("Invalid candidate rejected"), Candidate.LoadModule(InvalidModule, UE_ARRAY_COUNT(InvalidModule), TEXT("invalid_heap_candidate"), Result));
	TestNull(TEXT("Failed candidate has no heap"), Candidate.GetManagedHeapForTesting());
	TestTrue(TEXT("Failed candidate preserves live environment"), LiveHeap->IsAlive(Object));
	if (!TestTrue(TEXT("Valid candidate loads"), Candidate.LoadEmbeddedSmokeModule(Result))) return false;
	FHeap* CandidateHeap = Candidate.GetManagedHeapForTesting();
	if (!TestNotNull(TEXT("Candidate owns separate heap"), CandidateHeap)) return false;
	TestTrue(TEXT("Candidate configure"), CandidateHeap->Configure(Layouts) == EHeapError::Ok);
	TestFalse(TEXT("Live token cannot enter candidate"), CandidateHeap->IsAlive(Object));
	Candidate.Unload();
	TestNull(TEXT("Candidate unload releases heap"), Candidate.GetManagedHeapForTesting());
	TestTrue(TEXT("Candidate unload preserves live heap"), LiveHeap->IsAlive(Object));
	Live.Unload(); Live.Unload();
	TestNull(TEXT("Repeated unload releases live heap"), Live.GetManagedHeapForTesting());
	if (!TestTrue(TEXT("Same runtime can load fresh module"), Live.LoadEmbeddedSmokeModule(Result))) return false;
	FHeap* FreshHeap = Live.GetManagedHeapForTesting();
	if (!TestNotNull(TEXT("Fresh module owns fresh heap"), FreshHeap)) return false;
	TestTrue(TEXT("Fresh configure"), FreshHeap->Configure(Layouts) == EHeapError::Ok);
	FToken FreshRoot = 0, FreshObject = 0;
	TestTrue(TEXT("Fresh root"), FreshHeap->CreateRoot(0, 0, FreshRoot) == EHeapError::Ok);
	TestTrue(TEXT("Fresh allocation"), FreshHeap->Allocate(1, FreshRoot, FreshObject) == EHeapError::Ok);
	TestTrue(TEXT("New module tokens differ"), FreshObject != Object);
	TestFalse(TEXT("Prior module token remains invalid"), FreshHeap->IsAlive(Object));

	// Exercise actual Session prepare/rollback/commit, including a BeginPlay trap.
	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult ReloadResult;
	if (!TestTrue(TEXT("Session loads live module"), Session.LoadEmbeddedSmoke(ReloadResult))) return false;
	FAvidScriptWasmRuntimeInstance* OriginalRuntime = Session.GetLiveRuntimeForTesting();
	FHeap* OriginalHeap = OriginalRuntime->GetManagedHeapForTesting();
	if (!TestNotNull(TEXT("Session owns module heap"), OriginalHeap)) return false;
	TestTrue(TEXT("Session heap configure"), OriginalHeap->Configure(Layouts) == EHeapError::Ok);
	FToken SessionRoot = 0, SessionObject = 0;
	TestTrue(TEXT("Session root"), OriginalHeap->CreateRoot(0, 0, SessionRoot) == EHeapError::Ok);
	TestTrue(TEXT("Session allocation"), OriginalHeap->Allocate(1, SessionRoot, SessionObject) == EHeapError::Ok);
	const std::array<std::uint32_t, 1> StaticTypes{1};
	TestTrue(TEXT("Session declares static storage"), OriginalHeap->ConfigureStaticSlots(StaticTypes) == EHeapError::Ok);
	TestTrue(TEXT("Session stores static object"), OriginalHeap->WriteStaticSlot(1, 1, SessionObject) == EHeapError::Ok);
	TestTrue(TEXT("Release temporary persistent root"), OriginalHeap->ReleaseRoot(SessionRoot) == EHeapError::Ok);
	TestTrue(TEXT("Collect before reload"), OriginalHeap->Collect() == EHeapError::Ok);
	TestEqual(TEXT("Only static root owns Session object"), OriginalHeap->GetStats().LiveRoots, 1u);
	TestEqual(TEXT("Session static root count"), OriginalHeap->GetStats().StaticRoots, 1u);
	TestTrue(TEXT("Static object survives before reload"), OriginalHeap->IsAlive(SessionObject));
	const uint8 BeginTrap[] = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
		0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
		0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
		0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
		0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
		0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
		0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x08,
		0x02, 0x03, 0x00, 0x00, 0x0b, 0x02, 0x00, 0x0b
	};
	if (!TestFalse(TEXT("Candidate BeginPlay trap rejects reload"), Session.ReloadModule(BeginTrap, UE_ARRAY_COUNT(BeginTrap),
		FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("heap_begin_trap")), ReloadResult))) return false;
	if (!TestTrue(TEXT("Rollback preserves live runtime identity"), ReloadResult.bRollbackPreservedLiveRuntime && Session.GetLiveRuntimeForTesting() == OriginalRuntime)) return false;
	TestTrue(TEXT("Rollback preserves live environment"), OriginalHeap->IsAlive(SessionObject));
	FToken StaticObject = 0;
	TestTrue(TEXT("Rollback retains static storage"), OriginalHeap->ReadStaticSlot(1, 1, StaticObject) == EHeapError::Ok);
	TestEqual(TEXT("Rollback retains exact static object identity"), StaticObject, SessionObject);
	TArray<uint8> Recover(BeginTrap, UE_ARRAY_COUNT(BeginTrap));
	Recover[UE_ARRAY_COUNT(BeginTrap) - 5] = 0x01; // Replace unreachable with nop in BeginPlay.
	if (!TestTrue(TEXT("Compatible candidate commits"), Session.ReloadModule(Recover.GetData(), Recover.Num(),
		FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("heap_recovered")), ReloadResult))) return false;
	FHeap* CommittedHeap = Session.GetLiveRuntimeForTesting()->GetManagedHeapForTesting();
	if (!TestNotNull(TEXT("Committed runtime owns heap"), CommittedHeap)) return false;
	TestTrue(TEXT("Committed heap configure"), CommittedHeap->Configure(Layouts) == EHeapError::Ok);
	TestFalse(TEXT("Committed module rejects old environment"), CommittedHeap->IsAlive(SessionObject));
	TestTrue(TEXT("Committed module declares fresh static storage"), CommittedHeap->ConfigureStaticSlots(StaticTypes) == EHeapError::Ok);
	TestTrue(TEXT("Read committed static storage"), CommittedHeap->ReadStaticSlot(1, 1, StaticObject) == EHeapError::Ok);
	TestEqual(TEXT("Commit does not migrate raw static token"), StaticObject, FToken(0));
	TestTrue(TEXT("Old static token cannot enter committed domain"),
		CommittedHeap->WriteStaticSlot(1, 1, SessionObject) == EHeapError::InvalidObject);
	TestTrue(TEXT("Session stop succeeds"), Session.StopAndUnload(Result));
	TestNull(TEXT("Session stop releases runtime and heap"), Session.GetLiveRuntimeForTesting());
	return true;
}

namespace ManagedHeapFixture
{
enum class EFault { None, Trap, OutputBounds, Overlap, ShortOutput, ForeignObject, BadVersion };
void U32(TArray<uint8>& Out, uint32 Value)
{
	do { uint8 Byte = Value & 127; Value >>= 7; Out.Add(Byte | (Value ? 128 : 0)); } while (Value);
}
void I32(TArray<uint8>& Out, int32 Value)
{
	bool More;
	do
	{
		uint8 Byte = Value & 127; Value >>= 7;
		More = !((Value == 0 && !(Byte & 64)) || (Value == -1 && (Byte & 64)));
		Out.Add(Byte | (More ? 128 : 0));
	} while (More);
}
void Name(TArray<uint8>& Out, const char* Text)
{
	const int32 Size = FCStringAnsi::Strlen(Text); U32(Out, Size);
	Out.Append(reinterpret_cast<const uint8*>(Text), Size);
}
void Section(TArray<uint8>& Out, uint8 Id, const TArray<uint8>& Bytes)
{
	Out.Add(Id); U32(Out, Bytes.Num()); Out.Append(Bytes);
}
void Constant(TArray<uint8>& Out, int32 Value) { Out.Add(0x41); I32(Out, Value); }
void Host(TArray<uint8>& Out, int32 Input, int32 InputBytes, int32 Output = 0, int32 OutputBytes = 0)
{
	Constant(Out, Input); Constant(Out, InputBytes); Constant(Out, Output); Constant(Out, OutputBytes);
	Out.Append({0x10, 0x00, 0x1a}); // call import; drop success i32 (errors trap).
}
void CopyToken(TArray<uint8>& Out, int32 Source, int32 Destination)
{
	Constant(Out, Destination); Constant(Out, Source);
	Out.Append({0x29, 0x00, 0x00, 0x37, 0x00, 0x00}); // unaligned i64.load/store
}
TArray<uint8> Build(EFault Fault, const char* ImportModule = "avidscript", bool BadSignature = false, bool StaticStorage = false)
{
	using namespace AvidScript::Managed;
	TArray<uint8> Module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	TArray<uint8> Types{5, 0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f,
		0x60, 0, 0, 0x60, 1, 0x7d, 0, 0x60, 2, 0x7f, 0x7f, 1, 0x7f, 0x60, 2, 0x7f, 0x7d, 0};
	if (BadSignature) Types[8] = 0x7e; // Valid WASM returning i64; incompatible with the registered i32 ABI.
	Section(Module, 1, Types);
	TArray<uint8> Imports{1}; Name(Imports, ImportModule); Name(Imports, Abi::ImportName); Imports.Append({0, 0});
	Section(Module, 2, Imports); Section(Module, 3, {4, 1, 2, 3, 4}); Section(Module, 5, {1, 1, 1, 1});
	TArray<uint8> Exports{5};
	Name(Exports, "memory"); Exports.Append({2, 0});
	Name(Exports, "avid_on_begin_play"); Exports.Append({0, 1});
	Name(Exports, "avid_on_tick"); Exports.Append({0, 2});
	Name(Exports, "heap_pair"); Exports.Append({0, 3});
	Name(Exports, "heap_event"); Exports.Append({0, 4}); Section(Module, 7, Exports);
	TArray<uint8> Init{0}; Host(Init, 64, 24);
	if (StaticStorage) Host(Init, 320, 16);
	Init.Add(0x0b);
	TArray<uint8> Run{0};
	Host(Run, 128, 8, Fault == EFault::OutputBounds ? 0x7ffffffc : Fault == EFault::Overlap ? 132 : 512,
		Fault == EFault::ShortOutput ? 7 : 8);
	CopyToken(Run, 512, 168); Host(Run, 160, 24, 520, 8);
	CopyToken(Run, 520, 204); Host(Run, 192, 20, 528, 8);
	if (Fault != EFault::ForeignObject) CopyToken(Run, 528, 232);
	Host(Run, 224, 32);
	if (StaticStorage) { CopyToken(Run, 528, 400); Host(Run, 384, 24); }
	CopyToken(Run, 528, 280); Host(Run, 272, 28, 536, 4);
	if (Fault == EFault::Trap) Run.Add(0x00);
	TArray<uint8> Void = Run; Void.Add(0x0b);
	TArray<uint8> Pair = StaticStorage ? TArray<uint8>{0} : Run;
	if (StaticStorage)
	{
		Host(Pair, 352, 16, 544, 8); CopyToken(Pair, 544, 280); Host(Pair, 272, 28, 536, 4);
	}
	Constant(Pair, 536); Pair.Append({0x28, 0, 0, 0x0b});
	TArray<uint8> Event = Void;
	if (StaticStorage) { Event = {0}; Host(Event, 384, 24); Event.Add(0x0b); }
	TArray<uint8> Code{4};
	for (const auto* Body : {&Init, &Void, &Pair, &Event}) { U32(Code, Body->Num()); Code.Append(*Body); }
	Section(Module, 10, Code);
	TArray<uint8> Memory; Memory.SetNumZeroed(StaticStorage ? 408 : 320);
	auto Put = [&Memory](int32 Address, uint32 Value)
	{
		for (unsigned I = 0; I < 4; ++I) Memory[Address + I] = static_cast<uint8>(Value >> (I * 8));
	};
	auto Header = [&Put](int32 Address, Abi::ECommand Command)
	{
		Put(Address, Abi::Magic); Put(Address + 4, static_cast<uint32>(Command));
	};
	Header(64, Abi::ECommand::Configure); Put(72, 1); Put(76, 1); Put(80, 16); Put(84, 0);
	Header(128, Abi::ECommand::PushFrame);
	if (Fault == EFault::BadVersion) Put(128, Abi::Magic + 1);
	Header(160, Abi::ECommand::CreateRoot); Header(192, Abi::ECommand::Allocate); Put(200, 1);
	Header(224, Abi::ECommand::WriteBytes); Put(240, 1); Put(244, 0); Put(248, 4); Put(252, 42);
	Header(272, Abi::ECommand::ReadBytes); Put(288, 1); Put(292, 0); Put(296, 4);
	if (StaticStorage)
	{
		Header(320, Abi::ECommand::ConfigureStaticSlots); Put(328, 1); Put(332, 1);
		Header(352, Abi::ECommand::ReadStaticSlot); Put(360, 1); Put(364, 1);
		Header(384, Abi::ECommand::WriteStaticSlot); Put(392, 1); Put(396, 1);
	}
	TArray<uint8> Data{1, 0}; Constant(Data, 0); Data.Add(0x0b); U32(Data, Memory.Num()); Data.Append(Memory);
	Section(Module, 11, Data); return Module;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptManagedHeapHostAbiTest,
	"AvidScript.Runtime.ManagedHeap.HostAbi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptManagedHeapHostAbiTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace AvidScript::Managed;
	using namespace ManagedHeapFixture;
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		for (const auto Fault : {EFault::None, EFault::Trap, EFault::OutputBounds, EFault::Overlap, EFault::ShortOutput, EFault::ForeignObject, EFault::BadVersion})
		{
			FAvidScriptWasmRuntimeInstance Runtime(Selection); FAvidScriptWasmSmokeResult Result;
			const auto Wasm = Build(Fault);
			if (!TestTrue(TEXT("Heap ABI fixture loads"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("heap_abi"), Result)))
			{ AddError(Result.ErrorMessage); return false; }
			if (!TestTrue(TEXT("WASM registers layout"), Runtime.BeginPlay(Result))) { AddError(Result.ErrorMessage); return false; }
			FHeap& Heap = *Runtime.GetManagedHeapForTesting();
			FToken Persistent = 0, ProtectedObject = 0, OuterFrame = 0, OuterRoot = 0;
			TestTrue(TEXT("Persistent root"), Heap.CreateRoot(0, 0, Persistent) == EHeapError::Ok);
			TestTrue(TEXT("Persistent allocation"), Heap.Allocate(1, Persistent, ProtectedObject) == EHeapError::Ok);
			TestTrue(TEXT("Outer invocation frame"), Heap.PushFrame(OuterFrame) == EHeapError::Ok);
			TestTrue(TEXT("Outer root"), Heap.CreateRoot(OuterFrame, ProtectedObject, OuterRoot) == EHeapError::Ok);
			if (Fault == EFault::ForeignObject)
			{
				FHeap Other; const std::array<FHeapLayout, 1> Layouts{{{1, 16, {}}}}; Other.Configure(Layouts);
				FToken R = 0, O = 0; Other.CreateRoot(0, 0, R); Other.Allocate(1, R, O);
				uint8 Token[8]; for (unsigned I = 0; I < 8; ++I) Token[I] = static_cast<uint8>(O >> (I * 8));
				FString Error; TestTrue(TEXT("Inject foreign heap token"), Runtime.WriteStateBytes(232, MakeArrayView(Token), Error));
			}
			const bool bSuccess = Runtime.Tick(0.01f, Result);
			TestEqual(TEXT("WASM operation outcome"), bSuccess, Fault == EFault::None);
			TestEqual(TEXT("Invocation clears only its frames"), Heap.GetStats().ActiveFrames, 1u);
			TestEqual(TEXT("Invocation clears only its roots"), Heap.GetStats().LiveRoots, 2u);
			TestTrue(TEXT("Persistent object survived"), Heap.IsAlive(ProtectedObject));
			if (Fault == EFault::None)
			{
				uint8 Value[4]{}; FString Error;
				TestTrue(TEXT("Read Guest result"), Runtime.ReadStateBytes(536, MakeArrayView(Value), Error));
				TestEqual(TEXT("WASM allocated, wrote and read environment"), Value[0], uint8(42));
			}
			else
			{
				const FString Category = Fault == EFault::Trap ? (Backend == EAvidScriptVmBackendKind::Wasmtime ? TEXT("guest_trap") : TEXT("trap"))
					: Fault == EFault::ForeignObject ? TEXT("managed_heap_rejected")
					: Fault == EFault::ShortOutput || Fault == EFault::BadVersion ? TEXT("managed_heap_protocol") : TEXT("managed_heap_range");
				TestEqual(TEXT("Precise failure category"), Result.ErrorCategory, Category);
				if (Fault != EFault::Trap && Fault != EFault::ForeignObject)
					TestEqual(TEXT("Rejected packet did not allocate"), Heap.GetStats().Allocations, uint64(1));
			}
			TestTrue(TEXT("Collect after invocation"), Heap.Collect() == EHeapError::Ok);
			TestEqual(TEXT("Only protected object remains"), Heap.GetStats().LiveObjects, 1u);
			if (Backend == EAvidScriptVmBackendKind::Wasmtime && (Fault == EFault::None || Fault == EFault::Trap))
			{
				for (const FString Export : {FString(TEXT("heap_pair")), FString(TEXT("heap_event"))})
				{
					FAvidScriptVmPreparedExportCall Prepared; FString Error;
					if (!TestTrue(TEXT("Prepare specialized export"), Runtime.PrepareNamedExportCall(Export, Prepared, Error))) return false;
					FAvidScriptVmCallFrame Frame; Frame.CellCount = 2;
					FAvidScriptVmCallResult Return; FAvidScriptVmError VmError;
					TestEqual(TEXT("Prepared fast path outcome"), Prepared.Call(Frame, VmError, &Return), Fault == EFault::None);
					TestEqual(TEXT("Prepared fast path cleans frames"), Heap.GetStats().ActiveFrames, 1u);
					TestEqual(TEXT("Prepared fast path cleans roots"), Heap.GetStats().LiveRoots, 2u);
					if (Fault == EFault::None && Export == TEXT("heap_pair")) TestEqual(TEXT("Prepared return value"), Return.Cells[0], 42u);
				}
			}
		}
		for (const bool BadSignature : {false, true})
		{
			FAvidScriptWasmRuntimeInstance Runtime(Selection); FAvidScriptWasmSmokeResult Result;
			const auto Wasm = Build(EFault::None, BadSignature ? "avidscript" : "env", BadSignature);
			TestFalse(TEXT("Wrong module or signature rejected"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("heap_bad_import"), Result));
			TestNull(TEXT("Rejected import does not retain heap"), Runtime.GetManagedHeapForTesting());
		}
		FAvidScriptVmError Error;
		auto Unscoped = CreateAvidScriptVmBackend(Selection, Error);
		if (!TestNotNull(TEXT("Create unscoped VM"), Unscoped.Get())) return false;
		const auto Wasm = Build(EFault::None);
		TestFalse(TEXT("Heap ABI requires invocation cleanup owner"), Unscoped->Load(MakeArrayView(Wasm), TEXT("heap_without_scope"), {}, Error));
		TestEqual(TEXT("Missing owner diagnostic"), Error.Category, FString(TEXT("managed_heap_scope_required")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptManagedHeapStaticStorageTest,
	"AvidScript.Runtime.ManagedHeap.StaticStorage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptManagedHeapStaticStorageTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace AvidScript::Managed;
	using namespace ManagedHeapFixture;
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		AddInfo(Backend == EAvidScriptVmBackendKind::Wasmtime ? TEXT("Static storage: Wasmtime JIT") : TEXT("Static storage: WAMR interpreter"));
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Runtime(Selection); FAvidScriptWasmSmokeResult Result;
		const auto Wasm = Build(EFault::None, "avidscript", false, true);
		if (!TestTrue(TEXT("Static fixture loads"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("heap_static"), Result)))
		{ AddError(Result.ErrorMessage); return false; }
		TestEqual(TEXT("Static fixture uses requested backend"), Runtime.GetActiveBackendInfo().Kind, Backend);
		TestEqual(TEXT("Static fixture uses requested execution mode"), Runtime.GetActiveBackendInfo().ExecutionMode, Selection.ExecutionMode);
		if (!TestTrue(TEXT("WASM declares static storage"), Runtime.BeginPlay(Result))) { AddError(Result.ErrorMessage); return false; }
		FHeap& Heap = *Runtime.GetManagedHeapForTesting();
		FToken Current = 99, Previous = 0;
		TestTrue(TEXT("Read default static value"), Heap.ReadStaticSlot(1, 1, Current) == EHeapError::Ok);
		TestEqual(TEXT("Initial static value is null"), Current, FToken(0));
		TestEqual(TEXT("Null static slot reserves root budget"), Heap.GetStats().StaticRoots, 1u);
		FAvidScriptVmPreparedExportCall Read, Write;
		FString Error;
		if (!TestTrue(TEXT("Prepare static reader"), Runtime.PrepareNamedExportCall(TEXT("heap_pair"), Read, Error))
			|| !TestTrue(TEXT("Prepare static writer"), Runtime.PrepareNamedExportCall(TEXT("heap_event"), Write, Error)))
		{ AddError(Error); return false; }
		FAvidScriptVmCallFrame CallFrame; CallFrame.CellCount = 2;
		FAvidScriptVmError VmError;
		for (int32 Invocation = 0; Invocation < 8; ++Invocation)
		{
			FToken CallerFrame = 0, CallerRoot = 0;
			TestTrue(TEXT("Caller frame"), Heap.PushFrame(CallerFrame) == EHeapError::Ok);
			TestTrue(TEXT("Caller root"), Heap.CreateRoot(CallerFrame, 0, CallerRoot) == EHeapError::Ok);
			if (!TestTrue(TEXT("WASM stores a new static object"), Runtime.Tick(0.01f, Result))) { AddError(Result.ErrorMessage); return false; }
			TestEqual(TEXT("Tick preserves caller frame"), Heap.GetStats().ActiveFrames, 1u);
			TestEqual(TEXT("Tick retains only caller and static roots"), Heap.GetStats().LiveRoots, 2u);
			TestTrue(TEXT("Return from caller"), Heap.PopFrame(CallerFrame) == EHeapError::Ok);
			TestTrue(TEXT("Collect between exports"), Heap.Collect() == EHeapError::Ok);
			TestTrue(TEXT("Read static object identity"), Heap.ReadStaticSlot(1, 1, Current) == EHeapError::Ok);
			TestTrue(TEXT("Only current static object survives"), Heap.IsAlive(Current) && !Heap.IsAlive(Previous));
			TestEqual(TEXT("Replacement does not retain old objects"), Heap.GetStats().LiveObjects, 1u);
			FAvidScriptVmCallResult Return;
			if (!TestTrue(TEXT("Separate WASM export reads retained object"), Read.Call(CallFrame, VmError, &Return)))
			{ AddError(VmError.Details); return false; }
			TestEqual(TEXT("Retained object still contains its value"), Return.Cells[0], 42u);
			TestEqual(TEXT("Reader allocates no replacement object"), Heap.GetStats().Allocations, uint64(Invocation + 1));
			TestEqual(TEXT("Static access leaves no invocation frames"), Heap.GetStats().ActiveFrames, 0u);
			TestEqual(TEXT("Static access leaves only domain root"), Heap.GetStats().LiveRoots, 1u);
			if (Invocation != 7) Previous = Current;
		}
		uint8 Bytes[8];
		for (unsigned I = 0; I < 8; ++I) Bytes[I] = static_cast<uint8>(Previous >> (I * 8));
		TestTrue(TEXT("Inject stale static assignment"), Runtime.WriteStateBytes(400, MakeArrayView(Bytes), Error));
		TestFalse(TEXT("Host rejects stale static object"), Write.Call(CallFrame, VmError));
		TestEqual(TEXT("Stale static write diagnostic"), VmError.Category, FString(TEXT("managed_heap_rejected")));
		FToken AfterFailure = 0;
		TestTrue(TEXT("Static value remains readable after rejection"), Heap.ReadStaticSlot(1, 1, AfterFailure) == EHeapError::Ok);
		TestEqual(TEXT("Failed write preserves prior identity"), AfterFailure, Current);
		TestTrue(TEXT("Collect after rejected write"), Heap.Collect() == EHeapError::Ok);
		TestTrue(TEXT("Rejected call does not retire domain static root"), Heap.IsAlive(Current));
		for (auto& Byte : Bytes) Byte = 0;
		TestTrue(TEXT("Write null static assignment packet"), Runtime.WriteStateBytes(400, MakeArrayView(Bytes), Error));
		if (!TestTrue(TEXT("WASM clears static reference"), Write.Call(CallFrame, VmError))) { AddError(VmError.Details); return false; }
		TestTrue(TEXT("Collect cleared static object"), Heap.Collect() == EHeapError::Ok);
		TestEqual(TEXT("Clear releases last object"), Heap.GetStats().LiveObjects, 0u);
		TestEqual(TEXT("Clear keeps declared static storage"), Heap.GetStats().LiveRoots, 1u);
		Runtime.Unload(); Runtime.Unload();
		TestNull(TEXT("Unload releases static heap"), Runtime.GetManagedHeapForTesting());
		if (!TestTrue(TEXT("Static module reloads fresh"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("heap_static_fresh"), Result))
			|| !TestTrue(TEXT("Fresh WASM declares static storage"), Runtime.BeginPlay(Result))) { AddError(Result.ErrorMessage); return false; }
		FHeap& Fresh = *Runtime.GetManagedHeapForTesting();
		TestTrue(TEXT("Fresh storage is null"), Fresh.ReadStaticSlot(1, 1, AfterFailure) == EHeapError::Ok && AfterFailure == 0);
		TestTrue(TEXT("Prior domain token cannot enter fresh slot"), Fresh.WriteStaticSlot(1, 1, Current) == EHeapError::InvalidObject);

		FAvidScriptWasmRuntimeInstance Trapping(Selection);
		const auto TrapWasm = Build(EFault::Trap, "avidscript", false, true);
		if (!TestTrue(TEXT("Static trap fixture loads"), Trapping.LoadModule(TrapWasm.GetData(), TrapWasm.Num(), TEXT("heap_static_trap"), Result))
			|| !TestTrue(TEXT("Trap fixture initializes"), Trapping.BeginPlay(Result))) { AddError(Result.ErrorMessage); return false; }
		TestFalse(TEXT("Tick traps after storing static object"), Trapping.Tick(0.01f, Result));
		FHeap& TrapHeap = *Trapping.GetManagedHeapForTesting();
		TestTrue(TEXT("Collect after trap"), TrapHeap.Collect() == EHeapError::Ok);
		TestEqual(TEXT("Trap unwinds function frames"), TrapHeap.GetStats().ActiveFrames, 0u);
		TestEqual(TEXT("Trap preserves domain static storage"), TrapHeap.GetStats().StaticRoots, 1u);
		TestEqual(TEXT("Static object survives function trap until domain closes"), TrapHeap.GetStats().LiveObjects, 1u);
		Trapping.Unload();
		TestNull(TEXT("Trapped domain unload releases heap"), Trapping.GetManagedHeapForTesting());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptManagedHeapGeneratedGuestTest,
	"AvidScript.Runtime.ManagedHeap.GeneratedGuest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptManagedHeapGeneratedGuestTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace AvidScript::Managed;
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		for (const FString File : {FString(TEXT("managed.wasm")), FString(TEXT("managed-trap.wasm")), FString(TEXT("managed-cooperative.wasm")),
			FString(TEXT("managed-erased.wasm")), FString(TEXT("managed-erased-wrong.wasm")), FString(TEXT("framed-managed.wasm"))})
		{
			if (Backend == EAvidScriptVmBackendKind::Wamr && File == TEXT("managed-cooperative.wasm")) continue;
			TArray<uint8> Wasm;
			if (!TestTrue(TEXT("Load current compiler fixture; generate with Build/TestAvidScriptManagedHeap.ps1 -RuntimeAutomation"),
				FFileHelper::LoadFileToArray(Wasm, *FPaths::Combine(Directory, File)))) return false;
			FAvidScriptVmBackendSelection Selection;
			Selection.BackendKind = Backend;
			Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
			FAvidScriptWasmRuntimeInstance Runtime(Selection); FAvidScriptWasmSmokeResult Result;
			if (!TestTrue(TEXT("Generated Guest loads"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), File, Result)))
			{ AddError(Result.ErrorMessage); return false; }
			const bool bWrongCast = File == TEXT("managed-erased-wrong.wasm");
			const bool bTrap = File == TEXT("managed-trap.wasm") || bWrongCast;
			const bool bExecuted = Runtime.BeginPlay(Result);
			if (!TestEqual(TEXT("Generated program outcome"), bExecuted, !bTrap)) { AddError(Result.ErrorMessage); return false; }
			if (bWrongCast)
			{
				TestEqual(TEXT("Wrong downcast reports managed type rejection"), Result.ErrorCategory, FString(TEXT("managed_heap_rejected")));
				FHeap* RejectedHeap = Runtime.GetManagedHeapForTesting();
				if (!TestNotNull(TEXT("Rejected cast retains diagnostic heap"), RejectedHeap)) return false;
				TestEqual(TEXT("Rejected cast unwinds frames"), RejectedHeap->GetStats().ActiveFrames, 0u);
				TestEqual(TEXT("Rejected cast unwinds roots"), RejectedHeap->GetStats().LiveRoots, 0u);
				TestTrue(TEXT("Rejected cast graph collects"), RejectedHeap->Collect() == EHeapError::Ok);
				TestEqual(TEXT("Rejected cast leaves no objects"), RejectedHeap->GetStats().LiveObjects, 0u);
				continue;
			}
			uint8 Value[4]{}; FString Error;
			TestTrue(TEXT("Read generated program result"), Runtime.ReadStateBytes(16, MakeArrayView(Value), Error));
			TestEqual(TEXT("Reference/aggregate return, recursive/indirect calls and value-copy parameters survive GC"),
				uint32(Value[0]) | (uint32(Value[1]) << 8) | (uint32(Value[2]) << 16) | (uint32(Value[3]) << 24),
				File == TEXT("framed-managed.wasm") ? 644u : 49u);
			FHeap* Heap = Runtime.GetManagedHeapForTesting();
			if (!TestNotNull(TEXT("Generated module owns heap"), Heap)) return false;
			const auto Stats = Heap->GetStats();
			TestTrue(TEXT("Actual repeated allocation and collection"), Stats.Allocations >= 128 && Stats.Collections >= 128);
			TestTrue(TEXT("Loop roots do not retain allocation history"), Stats.PeakLiveBytes <= 512);
			TestEqual(TEXT("All generated frames unwind"), Stats.ActiveFrames, 0u);
			TestEqual(TEXT("All generated roots unwind"), Stats.LiveRoots, 0u);
			TestTrue(TEXT("Post-invocation cycle collection"), Heap->Collect() == EHeapError::Ok);
			TestEqual(TEXT("Escaped/shared graph and self-cycle reclaimed"), Heap->GetStats().LiveObjects, 0u);
		}
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptManagedHeapCSharpClosuresTest,
	"AvidScript.Runtime.ManagedHeap.CSharpClosures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptManagedHeapCSharpClosuresTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace AvidScript::Managed;
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		for (const FString File : {FString(TEXT("csharp-closures.wasm")), FString(TEXT("csharp-closures-stress.wasm")),
			FString(TEXT("csharp-delegate-identity.wasm")), FString(TEXT("csharp-delegate-identity-stress.wasm")),
			FString(TEXT("csharp-delegate-identity-static.wasm")),
			FString(TEXT("csharp-delegate-list.wasm")), FString(TEXT("csharp-delegate-list-stress.wasm")),
			FString(TEXT("csharp-delegate-list-static.wasm")), FString(TEXT("csharp-delegate-list-static-stress.wasm")),
			FString(TEXT("csharp-bound-delegate.wasm")), FString(TEXT("csharp-bound-delegate-stress.wasm")),
			FString(TEXT("csharp-bound-delegate-plain.wasm")), FString(TEXT("csharp-bound-delegate-plain-stress.wasm")),
			FString(TEXT("csharp-reference-object.wasm")), FString(TEXT("csharp-reference-object-stress.wasm")),
			FString(TEXT("csharp-reference-object-fault-method.wasm")), FString(TEXT("csharp-reference-object-fault-bind.wasm")),
			FString(TEXT("csharp-reference-object-fault-field.wasm")), FString(TEXT("csharp-reference-object-fault-compound.wasm")),
			FString(TEXT("csharp-reference-object-fault-borrow.wasm")), FString(TEXT("csharp-reference-object-fault-nested.wasm")),
			FString(TEXT("csharp-reference-object-fault-cast.wasm")),
			FString(TEXT("csharp-receiver-capture.wasm")), FString(TEXT("csharp-receiver-capture-stress.wasm")),
			FString(TEXT("csharp-receiver-capture-fault.wasm"))})
		{
			TArray<uint8> Wasm;
			if (!TestTrue(TEXT("Load current CSharp closure fixture"), FFileHelper::LoadFileToArray(Wasm, *FPaths::Combine(Directory, File)))) return false;
			FAvidScriptVmBackendSelection Selection;
			Selection.BackendKind = Backend;
			Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
			FAvidScriptWasmRuntimeInstance Runtime(Selection); FAvidScriptWasmSmokeResult Result;
			if (!TestTrue(TEXT("CSharp closures load"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), File, Result)))
			{ AddError(Result.ErrorMessage); return false; }
			const bool bReferenceObject = File.Contains(TEXT("reference-object"));
			const bool bReceiverCapture = File.Contains(TEXT("receiver-capture"));
			const bool bReferenceFault = (bReferenceObject || bReceiverCapture) && File.Contains(TEXT("-fault"));
			if (!TestEqual(TEXT("CSharp object/closure execution or expected failure"), Runtime.BeginPlay(Result), !bReferenceFault))
			{ AddError(Result.ErrorMessage); return false; }
			if (bReferenceFault) TestFalse(TEXT("Reference failure is diagnostic"), Result.ErrorMessage.IsEmpty());
			uint8 Value[4]{}; FString Error;
			if (!TestTrue(TEXT("Read CSharp closure result"), Runtime.ReadStateBytes(16, MakeArrayView(Value), Error))) return false;
			const bool bStaticIdentity = File == TEXT("csharp-delegate-identity-static.wasm");
			const bool bDelegateList = File.Contains(TEXT("delegate-list"));
			const bool bStaticList = File.Contains(TEXT("delegate-list-static"));
			const bool bBoundDelegate = File.Contains(TEXT("bound-delegate"));
			const bool bPlainBoundDelegate = File.Contains(TEXT("bound-delegate-plain"));
			const uint32 Expected = bReferenceFault ? (bReceiverCapture ? 7u : File.EndsWith(TEXT("-method.wasm")) || File.EndsWith(TEXT("-field.wasm")) ? 1u : 0u)
				: bReferenceObject ? 32767u : bReceiverCapture ? 8191u : bPlainBoundDelegate ? 650u : bBoundDelegate ? 4095u
				: bStaticIdentity || bStaticList ? 7u : bDelegateList ? 65535u : File.Contains(TEXT("identity")) ? 8191u : 1147395u;
			TestEqual(TEXT("CSharp closure execution and callable/environment equality match reference results"),
				uint32(Value[0]) | (uint32(Value[1]) << 8) | (uint32(Value[2]) << 16) | (uint32(Value[3]) << 24), Expected);
			FHeap* Heap = Runtime.GetManagedHeapForTesting();
			if (bStaticIdentity)
			{
				if (!TestNotNull(TEXT("Loaded module owns its standard heap manager"), Heap)) return false;
				TestTrue(TEXT("Static-only equality performs no managed allocation"), Heap->GetStats().Allocations == 0);
				TestEqual(TEXT("Static-only equality retains no roots"), Heap->GetStats().LiveRoots, 0u);
				TestEqual(TEXT("Static-only equality retains no frames"), Heap->GetStats().ActiveFrames, 0u);
				continue;
			}
			if (!TestNotNull(TEXT("CSharp module owns heap"), Heap)) return false;
			const auto Stats = Heap->GetStats();
			const uint64 MinimumAllocations = bReferenceFault ? (bReceiverCapture ? 3u : File.EndsWith(TEXT("-cast.wasm")) ? 1u : 0u)
				: bReferenceObject ? 8u : bPlainBoundDelegate ? 1u : bBoundDelegate ? 12u : bStaticList ? 2u : 16u;
			TestTrue(TEXT("Closures, lists and bound values use actual managed allocations"), Stats.Allocations >= MinimumAllocations);
			if (File.Contains(TEXT("stress")))
				TestTrue(TEXT("Every allocation followed by collection"), Stats.Collections >= Stats.Allocations);
			TestEqual(TEXT("CSharp frames unwind"), Stats.ActiveFrames, 0u);
			TestEqual(TEXT("CSharp roots unwind"), Stats.LiveRoots, 0u);
			TestTrue(TEXT("Collect detached closure graphs"), Heap->Collect() == EHeapError::Ok);
			TestEqual(TEXT("All closure environments reclaimed"), Heap->GetStats().LiveObjects, 0u);
		}
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptManagedHeapGenericMembersTest,
	"AvidScript.Runtime.ManagedHeap.GenericMembers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptManagedHeapGenericMembersTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace AvidScript::Managed;
	const FString File = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AvidScriptManagedHeapTests/GuestFixtures/generic-methods.wasm"));
	TArray<uint8> Wasm;
	if (!TestTrue(TEXT("Load current generic fixture; generate with Build/TestAvidScriptManagedHeap.ps1 -RuntimeAutomation"),
		FFileHelper::LoadFileToArray(Wasm, *File))) return false;
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		Selection.bAllowFallback = false;
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(TEXT("Generic member fixture loads"),
			Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("generic_members"), Result)))
		{ AddError(Result.ErrorMessage); return false; }
		TestEqual(TEXT("Generic member fixture uses requested VM backend"),
			Runtime.GetActiveBackendInfo().Kind, Backend);
		TestEqual(TEXT("Generic member fixture uses requested execution mode"),
			Runtime.GetActiveBackendInfo().ExecutionMode, Selection.ExecutionMode);
		if (!TestTrue(TEXT("Generic member constructors and methods execute in BeginPlay"), Runtime.BeginPlay(Result)))
		{ AddError(Result.ErrorMessage); return false; }
		uint8 Value[4]{};
		FString Error;
		if (!TestTrue(TEXT("Read generic member result"), Runtime.ReadStateBytes(16, MakeArrayView(Value), Error)))
		{ AddError(Error); return false; }
		TestEqual(TEXT("Closed int and nested value members retain .NET results"),
			uint32(Value[0]) | (uint32(Value[1]) << 8) | (uint32(Value[2]) << 16) | (uint32(Value[3]) << 24), 102u);
		FHeap* Heap = Runtime.GetManagedHeapForTesting();
		if (!TestNotNull(TEXT("Generic member module owns managed heap"), Heap)) return false;
		const auto Stats = Heap->GetStats();
		TestTrue(TEXT("Generic class instances allocate through Host ABI"), Stats.Allocations >= 3);
		TestEqual(TEXT("Generic member frames unwind"), Stats.ActiveFrames, 0u);
		TestEqual(TEXT("Generic member roots unwind"), Stats.LiveRoots, 0u);
		TestTrue(TEXT("Collect detached generic class instances"), Heap->Collect() == EHeapError::Ok);
		TestEqual(TEXT("Generic member objects reclaimed"), Heap->GetStats().LiveObjects, 0u);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptEnumeratorCleanupTest,
	"AvidScript.Runtime.EnumeratorCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEnumeratorCleanupTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace AvidScript::Managed;
	const FString File = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AvidScriptEnumeratorTests/GuestFixtures/enumerator-cleanup.wasm"));
	TArray<uint8> Wasm;
	if (!TestTrue(TEXT("Load current enumerator fixture; generate with Build/TestAvidScriptEnumeratorCleanup.ps1"),
		FFileHelper::LoadFileToArray(Wasm, *File))) return false;
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		Selection.bAllowFallback = false;
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(TEXT("Enumerator fixture loads"),
			Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("enumerator_cleanup"), Result)))
		{ AddError(Result.ErrorMessage); return false; }
		TestEqual(TEXT("Enumerator fixture uses requested VM backend"),
			Runtime.GetActiveBackendInfo().Kind, Backend);
		TestEqual(TEXT("Enumerator fixture uses requested execution mode"),
			Runtime.GetActiveBackendInfo().ExecutionMode, Selection.ExecutionMode);
		if (!TestTrue(TEXT("Enumerator fixture executes in BeginPlay"), Runtime.BeginPlay(Result)))
		{ AddError(Result.ErrorMessage); return false; }
		uint8 Values[24]{};
		FString Error;
		if (!TestTrue(TEXT("Read enumerator counts and results"), Runtime.ReadStateBytes(16, MakeArrayView(Values), Error)))
		{ AddError(Error); return false; }
		auto ReadValue = [&Values](int32 Index)
		{
			const int32 Offset = Index * 4;
			return uint32(Values[Offset]) | (uint32(Values[Offset + 1]) << 8)
				| (uint32(Values[Offset + 2]) << 16) | (uint32(Values[Offset + 3]) << 24);
		};
		// Static slots use stable symbol order, not C# declaration order.
		TestEqual(TEXT("Collection acquired once per loop"), ReadValue(0), 3u);
		TestEqual(TEXT("Enumerator disposed once per loop"), ReadValue(1), 3u);
		TestEqual(TEXT("Break and continue result"), ReadValue(2), 2u);
		TestEqual(TEXT("Final acquire/dispose count"), ReadValue(3), 33u);
		TestEqual(TEXT("Early return evaluates before Dispose"), ReadValue(4), 22u);
		TestEqual(TEXT("Normal completion result"), ReadValue(5), 12u);
		FHeap* Heap = Runtime.GetManagedHeapForTesting();
		if (!TestNotNull(TEXT("Enumerator fixture owns managed heap"), Heap)) return false;
		const auto Stats = Heap->GetStats();
		TestTrue(TEXT("Collections and enumerators allocate through Host ABI"), Stats.Allocations >= 6);
		TestEqual(TEXT("Enumerator frames unwind"), Stats.ActiveFrames, 0u);
		TestEqual(TEXT("Enumerator roots unwind"), Stats.LiveRoots, 0u);
		TestTrue(TEXT("Collect detached enumerators"), Heap->Collect() == EHeapError::Ok);
		TestEqual(TEXT("Enumerator objects reclaimed"), Heap->GetStats().LiveObjects, 0u);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptManagedHeapBorrowedReferencesTest,
	"AvidScript.Runtime.ManagedHeap.BorrowedReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptManagedHeapBorrowedReferencesTest::RunTest(const FString& Parameters)
{
	static_cast<void>(Parameters);
	using namespace AvidScript::Managed;
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"));
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		for (const FString File : {FString(TEXT("borrowed.wasm")), FString(TEXT("borrowed-trap.wasm")),
			FString(TEXT("csharp-borrowed.wasm")), FString(TEXT("csharp-borrowed-stress.wasm")), FString(TEXT("framed-borrowed.wasm"))})
		{
			TArray<uint8> Wasm;
			if (!TestTrue(TEXT("Load current borrowed reference fixture"), FFileHelper::LoadFileToArray(Wasm, *FPaths::Combine(Directory, File)))) return false;
			FAvidScriptVmBackendSelection Selection;
			Selection.BackendKind = Backend;
			Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
			FAvidScriptWasmRuntimeInstance Runtime(Selection); FAvidScriptWasmSmokeResult Result;
			if (!TestTrue(TEXT("Borrowed references load"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), File, Result)))
			{ AddError(Result.ErrorMessage); return false; }
			const bool bTrap = File == TEXT("borrowed-trap.wasm");
			const bool bCSharp = File.StartsWith(TEXT("csharp-"));
			if (!TestEqual(TEXT("Borrowed references execute"), Runtime.BeginPlay(Result), !bTrap))
			{ AddError(Result.ErrorMessage); return false; }
			uint8 Value[4]{}; FString Error;
			if (!TestTrue(TEXT("Read borrowed reference result"), Runtime.ReadStateBytes(16, MakeArrayView(Value), Error))) return false;
			TestEqual(TEXT("Aliased writes, reentrant reads, nested fields and ref updates survive callee collection"),
                uint32(Value[0]) | (uint32(Value[1]) << 8) | (uint32(Value[2]) << 16) | (uint32(Value[3]) << 24), bCSharp ? 111897u : 223u);
			FHeap* Heap = Runtime.GetManagedHeapForTesting();
			if (!TestNotNull(TEXT("Borrowed module owns heap"), Heap)) return false;
			TestTrue(TEXT("Borrowed fixture actually allocates"), Heap->GetStats().Allocations >= 3);
			if (!bCSharp) TestTrue(TEXT("IR borrowed fixture actually collects"), Heap->GetStats().Collections >= 4);
			if (File.Contains(TEXT("stress"))) TestTrue(TEXT("CSharp borrowed references survive collection after every allocation"),
				Heap->GetStats().Collections >= Heap->GetStats().Allocations);
			TestEqual(TEXT("Borrowed frames unwind"), Heap->GetStats().ActiveFrames, 0u);
			TestEqual(TEXT("Borrowed roots unwind"), Heap->GetStats().LiveRoots, 0u);
			TestTrue(TEXT("Collect after borrowed invocation"), Heap->Collect() == EHeapError::Ok);
			TestEqual(TEXT("Borrowed owners and updated objects reclaimed"), Heap->GetStats().LiveObjects, 0u);
		}
	}
	return true;
}
#endif
