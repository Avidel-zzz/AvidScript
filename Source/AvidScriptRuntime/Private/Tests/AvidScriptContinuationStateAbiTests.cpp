#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptWasmRuntime.h"
#include "AvidScriptContinuationStateAbi.h"
#include "AvidScriptManagedHeapAbi.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include <array>

namespace AvidScriptContinuationStateAbiTests
{
enum class EFault { None, StoreType, StoreNull, StoreForeign, StoreStale, ReadType, ReadToken, ReadTwice, ReadOutside, DuplicateStore, NoHeapImport };
void U32(TArray<uint8>& Out, uint32 Value)
{
	do { uint8 Byte = Value & 127; Value >>= 7; Out.Add(Byte | (Value ? 128 : 0)); } while (Value);
}
void Name(TArray<uint8>& Out, const char* Value)
{
	const int32 Size = FCStringAnsi::Strlen(Value); U32(Out, Size);
	Out.Append(reinterpret_cast<const uint8*>(Value), Size);
}
void Section(TArray<uint8>& Out, uint8 Id, const TArray<uint8>& Bytes)
{
	Out.Add(Id); U32(Out, Bytes.Num()); Out.Append(Bytes);
}
void I32(TArray<uint8>& Out, int32 Value)
{
	Out.Add(0x41);
	bool More;
	do
	{
		uint8 Byte = Value & 127; Value >>= 7;
		More = !((Value == 0 && !(Byte & 64)) || (Value == -1 && (Byte & 64)));
		Out.Add(Byte | (More ? 128 : 0));
	} while (More);
}
void Load64(TArray<uint8>& Out, int32 Address) { I32(Out, Address); Out.Append({0x29, 0, 0}); }
TArray<uint8> Build(EFault Fault, const char* ImportModule = "avidscript", bool BadSignature = false)
{
	TArray<uint8> Wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	TArray<uint8> Types{6,
		0x60, 3, 0x7e, 0x7f, 0x7e, 1, 0x7f, // store
		0x60, 2, 0x7e, 0x7f, 1, 0x7e, // read
		0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f, // heap
		0x60, 0, 0, 0x60, 1, 0x7d, 0, // BeginPlay, Tick
		0x60, 3, 0x7f, 0x7e, 0x7f, 0}; // continuation
	if (BadSignature) Types[7] = 0x7e;
	Section(Wasm, 1, Types);
	const bool HasHeap = Fault != EFault::NoHeapImport;
	const uint32 ImportsCount = HasHeap ? 3 : 2;
	TArray<uint8> Imports; U32(Imports, ImportsCount);
	Name(Imports, ImportModule); Name(Imports, AvidScript::ContinuationState::Abi::StoreImport); Imports.Append({0, 0});
	Name(Imports, ImportModule); Name(Imports, AvidScript::ContinuationState::Abi::ReadImport); Imports.Append({0, 1});
	if (HasHeap) { Name(Imports, "avidscript"); Name(Imports, AvidScript::Managed::Abi::ImportName); Imports.Append({0, 2}); }
	Section(Wasm, 2, Imports);
	Section(Wasm, 3, {3, 3, 4, 5});
	Section(Wasm, 5, {1, 1, 1, 1});
	TArray<uint8> Exports{4};
	Name(Exports, "memory"); Exports.Append({2, 0});
	Name(Exports, "avid_on_begin_play"); Exports.Add(0); U32(Exports, ImportsCount);
	Name(Exports, "avid_on_tick"); Exports.Add(0); U32(Exports, ImportsCount + 1);
	Name(Exports, "avid_on_continuation"); Exports.Add(0); U32(Exports, ImportsCount + 2);
	Section(Wasm, 7, Exports);
	TArray<uint8> Begin{0};
	if (Fault == EFault::ReadOutside)
	{
		Load64(Begin, 8); I32(Begin, 1); Begin.Append({0x10, 1, 0x1a});
	}
	else
	{
		auto Store = [&]()
		{
			Load64(Begin, 8); I32(Begin, Fault == EFault::StoreType ? 2 : 1);
			if (Fault == EFault::StoreNull) Begin.Append({0x42, 0}); else Load64(Begin, 16);
			Begin.Append({0x10, 0});
			if (BadSignature) Begin.Add(0xa7); // Keep WASM valid; only the Host import signature is incompatible.
		};
		I32(Begin, 24); Store(); Begin.Append({0x36, 0, 0});
		if (Fault == EFault::DuplicateStore) { I32(Begin, 28); Store(); Begin.Append({0x36, 0, 0}); }
	}
	Begin.Add(0x0b);
	TArray<uint8> Resume{0};
	auto Read = [&]()
	{
		Resume.Append({0x20, 1}); // current i64 continuation token
		if (Fault == EFault::ReadToken) Resume.Append({0x42, 1, 0x7c});
		I32(Resume, Fault == EFault::ReadType ? 2 : 1);
		Resume.Append({0x10, 1});
	};
	I32(Resume, 32); Read(); Resume.Append({0x37, 0, 0});
	if (Fault == EFault::ReadTwice) { Read(); Resume.Add(0x1a); }
	if (HasHeap)
	{
		// Force collection after returning the object but before dispatch finalizes.
		I32(Resume, 48); I32(Resume, 8); I32(Resume, 0); I32(Resume, 0); Resume.Append({0x10, 2, 0x1a});
	}
	Resume.Add(0x0b);
	TArray<uint8> Code{3}; U32(Code, Begin.Num()); Code.Append(Begin);
	Code.Append({2, 0, 0x0b}); U32(Code, Resume.Num()); Code.Append(Resume); Section(Wasm, 10, Code);
	return Wasm;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptContinuationStateAbiTest,
	"AvidScript.Runtime.Continuation.ManagedStateAbi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationStateAbiTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	using namespace AvidScriptContinuationStateAbiTests;
	if (!GEngine) return false;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptStateAbiWorld"));
	if (!TestNotNull(TEXT("ABI World created"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		for (const EFault Fault : {EFault::None, EFault::StoreType, EFault::StoreNull, EFault::StoreForeign, EFault::StoreStale, EFault::ReadType,
			EFault::ReadToken, EFault::ReadTwice, EFault::ReadOutside, EFault::DuplicateStore, EFault::NoHeapImport})
		{
			FAvidScriptWasmRuntimeInstance Runtime(Selection);
			FAvidScriptWasmSmokeResult Result;
			const auto Bytes = Build(Fault);
			if (!TestTrue(TEXT("Actual state ABI WASM loads"), Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), TEXT("continuation_state_abi"), Result)))
			{ AddError(Result.ErrorMessage); return false; }
			const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
			auto& Endpoint = Owner->ResetActive(World);
			FAvidScriptWasmHostContext Context;
			Context.Continuations = &Endpoint;
			Runtime.SetHostContext(Context);
			FHeap& Heap = *Runtime.GetManagedHeapForTesting();
			const std::array<FHeapLayout, 2> Layouts{{{1, 8, {{0, 1}}}, {2, 8, {}}}};
			TestTrue(TEXT("Concrete state layouts configure"), Heap.Configure(Layouts) == EHeapError::Ok);
			FToken Frame = 0, Root = 0, Object = 0;
			TestTrue(TEXT("Fixture frame"), Heap.PushFrame(Frame) == EHeapError::Ok);
			TestTrue(TEXT("Fixture root"), Heap.CreateRoot(Frame, 0, Root) == EHeapError::Ok);
			TestTrue(TEXT("Fixture state object"), Heap.Allocate(1, Root, Object) == EHeapError::Ok);
			TestTrue(TEXT("Shared cyclic reference"), Heap.WriteReference(Object, 1, 0, Object) == EHeapError::Ok);
			const int64 Token = Endpoint.ScheduleDelay(0.01f, 7);
			FHeap ForeignHeap;
			FToken ForeignRoot = 0, ForeignObject = 0;
			if (Fault == EFault::StoreForeign)
			{
				TestTrue(TEXT("Foreign layout"), ForeignHeap.Configure(Layouts) == EHeapError::Ok);
				TestTrue(TEXT("Foreign root"), ForeignHeap.CreateRoot(0, 0, ForeignRoot) == EHeapError::Ok);
				TestTrue(TEXT("Foreign object"), ForeignHeap.Allocate(1, ForeignRoot, ForeignObject) == EHeapError::Ok);
			}
			if (Fault == EFault::StoreStale)
			{
				TestTrue(TEXT("Release stale fixture root"), Heap.SetRoot(Root, 0) == EHeapError::Ok);
				TestTrue(TEXT("Collect stale object"), Heap.Collect() == EHeapError::Ok);
			}
			uint8 Memory[64] = {};
			const FToken StoredObject = Fault == EFault::StoreForeign ? ForeignObject : Object;
			FMemory::Memcpy(Memory + 8, &Token, 8); FMemory::Memcpy(Memory + 16, &StoredObject, 8);
			const uint32 Packet[] = {Abi::Magic, static_cast<uint32>(Abi::ECommand::Collect)};
			FMemory::Memcpy(Memory + 48, Packet, sizeof(Packet));
			FString Error;
			TestTrue(TEXT("Fixture memory initialized"), Runtime.WriteStateBytes(0, MakeArrayView(Memory), Error));
			const bool BadBegin = Fault == EFault::StoreType || Fault == EFault::StoreNull
				|| Fault == EFault::StoreForeign || Fault == EFault::StoreStale || Fault == EFault::ReadOutside;
			TestEqual(TEXT("WASM store respects type/context"), Runtime.BeginPlay(Result), !BadBegin);
			TestTrue(TEXT("Fixture frame released"), Heap.PopFrame(Frame) == EHeapError::Ok);
			TestTrue(TEXT("Collect while suspended"), Heap.Collect() == EHeapError::Ok);
			TestEqual(TEXT("Published state survives caller exit"), Heap.IsAlive(Object), !BadBegin);
			if (!BadBegin)
			{
				TestTrue(TEXT("Read store acceptance"), Runtime.ReadStateBytes(0, MakeArrayView(Memory), Error));
				TestEqual(TEXT("First store accepted"), Memory[24], uint8(1));
				if (Fault == EFault::DuplicateStore)
				{
					TestEqual(TEXT("Duplicate store returns recoverable rejection"), Memory[28], uint8(0));
					TestEqual(TEXT("Duplicate acquisition does not leak roots"), Heap.GetStats().LiveRoots, uint32(1));
				}
				World->Tick(LEVELTICK_All, 0); ++GFrameCounter;
				World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
				TArray<FAvidScriptContinuationCompletion> Ready;
				Owner->DrainReady(Ready);
				if (!TestEqual(TEXT("Timer ready for actual WASM resume"), Ready.Num(), 1)) return false;
				const bool BadRead = Fault == EFault::ReadType || Fault == EFault::ReadToken || Fault == EFault::ReadTwice;
				TestEqual(TEXT("Resume enforces type, current token and consume-once"), Runtime.DispatchContinuation(Ready[0], Result), !BadRead);
				if (!BadRead)
				{
					TestTrue(TEXT("Read resumed object"), Runtime.ReadStateBytes(0, MakeArrayView(Memory), Error));
					uint64 Restored = 0; FMemory::Memcpy(&Restored, Memory + 32, 8);
					TestEqual(TEXT("WASM restores exact object identity"), Restored, Object);
					TestTrue(TEXT("Post-read Guest collection keeps state"), Heap.IsAlive(Object));
				}
				TestTrue(TEXT("Native owner finalizes Guest success/trap"), Owner->FinalizeDispatched(Token, !BadRead));
			}
			Owner->Teardown();
			TestTrue(TEXT("Final collection"), Heap.Collect() == EHeapError::Ok);
			TestFalse(TEXT("No state retained after dispatch/cancel"), Heap.IsAlive(Object));
			TestEqual(TEXT("All native state roots released"), Heap.GetStats().LiveRoots, uint32(0));
		}
		for (const bool WrongModule : {false, true})
		{
			FAvidScriptWasmRuntimeInstance Runtime(Selection); FAvidScriptWasmSmokeResult Result;
			const auto Bytes = Build(EFault::None, WrongModule ? "env" : "avidscript", !WrongModule);
			TestFalse(TEXT("Managed state rejects env alias and wrong signature"), Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), TEXT("bad_state_abi"), Result));
		}
	}
	return true;
}
#endif
