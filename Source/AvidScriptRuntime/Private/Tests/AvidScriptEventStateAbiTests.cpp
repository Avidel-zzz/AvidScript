#if WITH_DEV_AUTOMATION_TESTS
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptEventStateAbi.h"
#include "AvidScriptManagedHeapAbi.h"
#include "Session/AvidScriptSessionDelegateSubscriptions.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"
#include <array>

namespace AvidScriptEventStateAbiTests
{
enum class EFault { None, SelfCancel, StoreType, StoreNull, StoreForeign, StoreStale, ZeroType,
	ReadType, ReadTwice, ReadOutside, Unauthorized, ForeignWorld, StaleSource, NegativeOrdinal, MissingEvent, NoHeapImport };
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
void Load(TArray<uint8>& Out, int32 Address, bool Wide)
{
	I32(Out, Address); Out.Append({static_cast<uint8>(Wide ? 0x29 : 0x28), 0, 0});
}
TArray<uint8> Build(EFault Fault, const char* Module = "avidscript", bool BadSignature = false)
{
	TArray<uint8> Wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	TArray<uint8> Types{6,
		0x60, 5, 0x7f, 0x7f, 0x7f, 0x7f, 0x7e, 1, 0x7e, // subscribe
		0x60, 1, 0x7f, 1, 0x7e, // read
		0x60, 1, 0x7e, 1, 0x7f, // unsubscribe
		0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f, // heap
		0x60, 0, 0, 0x60, 1, 0x7d, 0}; // BeginPlay, Tick/event
	if (BadSignature) Types[9] = 0x7f; // keep module valid, change subscribe return
	Section(Wasm, 1, Types);
	const bool HasHeap = Fault != EFault::NoHeapImport;
	const uint32 ImportCount = HasHeap ? 4 : 3;
	TArray<uint8> Imports; U32(Imports, ImportCount);
	Name(Imports, Module); Name(Imports, AvidScript::EventState::Abi::SubscribeImport); Imports.Append({0, 0});
	Name(Imports, Module); Name(Imports, AvidScript::EventState::Abi::ReadImport); Imports.Append({0, 1});
	Name(Imports, "avidscript"); Name(Imports, "event_unsubscribe"); Imports.Append({0, 2});
	if (HasHeap) { Name(Imports, "avidscript"); Name(Imports, AvidScript::Managed::Abi::ImportName); Imports.Append({0, 3}); }
	Section(Wasm, 2, Imports);
	Section(Wasm, 3, {3, 4, 5, 5});
	Section(Wasm, 5, {1, 1, 1, 1});
	TArray<uint8> Exports{4};
	Name(Exports, "memory"); Exports.Append({2, 0});
	Name(Exports, "avid_on_begin_play"); Exports.Add(0); U32(Exports, ImportCount);
	Name(Exports, "avid_on_tick"); Exports.Add(0); U32(Exports, ImportCount + 1);
	Name(Exports, "event_callback"); Exports.Add(0); U32(Exports, ImportCount + 2);
	Section(Wasm, 7, Exports);
	TArray<uint8> Tick{0};
	if (Fault == EFault::ReadOutside)
	{
		I32(Tick, 1); Tick.Append({0x10, 1, 0x1a});
	}
	else
	{
		I32(Tick, 24); Load(Tick, 0, false); Load(Tick, 4, false); Load(Tick, 8, false);
		I32(Tick, Fault == EFault::ZeroType ? 0 : Fault == EFault::StoreType ? 2 : 1);
		Load(Tick, 16, true); Tick.Append({0x10, 0});
		if (BadSignature) Tick.Add(0xad); // i64.extend_i32_u
		Tick.Append({0x37, 0, 0});
	}
	Tick.Add(0x0b);
	TArray<uint8> Event{0};
	if (Fault == EFault::SelfCancel) { Load(Event, 24, true); Event.Append({0x10, 2, 0x1a}); }
	auto Read = [&]() { I32(Event, Fault == EFault::ReadType ? 2 : 1); Event.Append({0x10, 1}); };
	I32(Event, 32); Read(); Event.Append({0x37, 0, 0});
	if (Fault == EFault::ReadTwice) { Read(); Event.Add(0x1a); }
	if (HasHeap)
	{
		I32(Event, 48); I32(Event, 8); I32(Event, 0); I32(Event, 0); Event.Append({0x10, 3, 0x1a});
	}
	I32(Event, 40); Load(Event, 40, false); I32(Event, 1); Event.Append({0x6a, 0x36, 0, 0});
	Event.Add(0x0b);
	TArray<uint8> Code{3, 2, 0, 0x0b}; U32(Code, Tick.Num()); Code.Append(Tick);
	U32(Code, Event.Num()); Code.Append(Event); Section(Wasm, 10, Code);
	return Wasm;
}
bool EncodeFrame(const void*, const void*, const FAvidScriptBindingInvocationContext&,
	uint32, FAvidScriptVmCallFrame& Frame, TArray<FAvidScriptObjectHandle>&, FString&, FString&)
{
	Frame.CellCount = 1; Frame.Cells[0] = 0; return true;
}

// Trusted endpoint probe isolates Runtime scope authorization from the real
// subscription owner exercised by ManagedStateAbi below.
class FNestedReadProbe final : public IAvidScriptEventSubscriptionHost
{
public:
	FAvidScriptWasmRuntimeInstance* Runtime = nullptr;
	FAvidScriptWasmHostContext Context;
	FAvidScriptContextualExportCall Nested;
	uint8 State[AvidScript::EventState::Abi::StateBytes] = {};
	bool bNestedReads = false;
	bool bNestedSucceeded = false;
	int32 Reads = 0;
	FAvidScriptVmError Error;
	int64 Subscribe(UObject&, uint32, FString&) override { return 0; }
	bool Unsubscribe(int64, FString&) override { return false; }
	bool ReadCurrentManagedState(const FAvidScriptWasmRuntimeInstance&, TArrayView<uint8> Out) override
	{
		if (++Reads != 1 || Out.Num() != sizeof(State)) return false;
		FAvidScriptVmCallFrame Frame;
		Frame.CellCount = bNestedReads ? 1 : 0;
		bNestedSucceeded = Runtime->InvokeInContext(Nested, Context, Frame, Error);
		if (!bNestedSucceeded) return false;
		FMemory::Memcpy(Out.GetData(), State, sizeof(State));
		return true;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptEventStateAbiTest,
	"AvidScript.Runtime.DelegateSubscription.ManagedStateAbi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEventStateAbiTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	using namespace AvidScriptEventStateAbiTests;
	if (!GEngine) return false;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptEventStateWorld"));
	UWorld* OtherWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptEventStateForeignWorld"));
	if (!TestNotNull(TEXT("World"), World) || !TestNotNull(TEXT("Other World"), OtherWorld)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(OtherWorld);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(OtherWorld); OtherWorld->DestroyWorld(false);
		GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	};
	AActor* Owner = World->SpawnActor<AActor>();
	AActor* Unauthorized = World->SpawnActor<AActor>();
	AActor* ForeignSource = OtherWorld->SpawnActor<AActor>();
	if (!Owner || !Unauthorized || !ForeignSource) return false;
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		for (int32 Index = 0; Index <= static_cast<int32>(EFault::NoHeapImport); ++Index)
		{
			const auto Fault = static_cast<EFault>(Index);
			AddInfo(FString::Printf(TEXT("Event state ABI backend=%d scenario=%d"), static_cast<int32>(Backend), Index));
			FAvidScriptObjectRegistry Registry;
			FAvidScriptObjectHandleResult HandleResult;
			const auto OwnerHandle = Registry.RegisterObject(Owner, HandleResult, false);
			const auto UnauthorizedHandle = Registry.RegisterObject(Unauthorized, HandleResult, false);
			const auto ForeignHandle = Registry.RegisterObject(ForeignSource, HandleResult, false);
			FAvidScriptSessionObjectOwnership Ownership;
			TestTrue(TEXT("Foreign source capability granted before World check"), Ownership.Borrow(Registry, *ForeignSource, HandleResult));
			FAvidScriptRuntimeSession Session;
			Session.SetBackendSelectionForTesting(Selection);
			const auto Wasm = Build(Fault);
			FAvidScriptWasmReloadResult Loaded;
			auto Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("event_state_abi"));
			Manifest.RequiredImports = {
				{TEXT("avidscript"), UTF8_TO_TCHAR(AvidScript::EventState::Abi::SubscribeImport)},
				{TEXT("avidscript"), UTF8_TO_TCHAR(AvidScript::EventState::Abi::ReadImport)},
				{TEXT("avidscript"), TEXT("event_unsubscribe")}};
			if (Fault != EFault::NoHeapImport) Manifest.RequiredImports.Add({TEXT("avidscript"), UTF8_TO_TCHAR(Abi::ImportName)});
			if (!Session.LoadInitialModule(Wasm.GetData(), Wasm.Num(), Manifest, Loaded))
			{ AddError(Loaded.ErrorMessage); return false; }
			auto* Runtime = Session.GetLiveRuntimeForTesting();
			FAvidScriptSessionDelegateSubscriptions Subscriptions(Session);
			FAvidScriptWasmHostContext Context;
			Context.ObjectRegistry = &Registry; Context.ObjectOwnership = &Ownership;
			Context.World = World; Context.OwnerHandle = OwnerHandle; Context.EventSubscriptions = &Subscriptions;
			Runtime->SetHostContext(Context);
			FAvidScriptPreparedDelegateEvent Event;
			Event.EventOrdinal = 7;
			Event.StableId = FString::ChrN(64, '6');
			Event.ExportName = TEXT("event_callback");
			Event.ExpectedSourceClass = AActor::StaticClass();
			Event.Signature.Kind = EAvidScriptPreparedDelegateKind::Multicast;
			Event.Signature.MulticastProperty = FindFProperty<FMulticastDelegateProperty>(AActor::StaticClass(), TEXT("OnActorBeginOverlap"));
			Event.Signature.SignatureFunction = Event.Signature.MulticastProperty->SignatureFunction;
			Event.Signature.ImmutableCodecIdentity = Event.Signature.SignatureFunction;
			Event.Signature.ParameterCellCount = 1; Event.Signature.Encode = &EncodeFrame;
			FString Error;
			if (!Subscriptions.Prepare(World, MakeArrayView(&Event, 1), Error, Runtime)) { AddError(Error); return false; }
			Subscriptions.CommitPrepared(); Subscriptions.SetDispatchEnabled(true);
			auto& Heap = *Runtime->GetManagedHeapForTesting();
			const std::array<FHeapLayout, 2> Layouts{{{1, 8, {{0, 1}}}, {2, 8, {}}}};
			TestTrue(TEXT("State layouts"), Heap.Configure(Layouts) == EHeapError::Ok);
			FToken Frame = 0, Root = 0, Object = 0;
			TestTrue(TEXT("Caller frame"), Heap.PushFrame(Frame) == EHeapError::Ok);
			TestTrue(TEXT("Caller root"), Heap.CreateRoot(Frame, 0, Root) == EHeapError::Ok);
			TestTrue(TEXT("State allocation"), Heap.Allocate(1, Root, Object) == EHeapError::Ok);
			TestTrue(TEXT("Cyclic capture"), Heap.WriteReference(Object, 1, 0, Object) == EHeapError::Ok);
			FHeap ForeignHeap;
			FToken ForeignRoot = 0, ForeignObject = 0;
			if (Fault == EFault::StoreForeign)
			{
				TestTrue(TEXT("Foreign layout"), ForeignHeap.Configure(Layouts) == EHeapError::Ok);
				TestTrue(TEXT("Foreign root"), ForeignHeap.CreateRoot(0, 0, ForeignRoot) == EHeapError::Ok);
				TestTrue(TEXT("Foreign state"), ForeignHeap.Allocate(1, ForeignRoot, ForeignObject) == EHeapError::Ok);
			}
			if (Fault == EFault::StoreStale)
			{
				TestTrue(TEXT("Stale root released"), Heap.SetRoot(Root, 0) == EHeapError::Ok);
				TestTrue(TEXT("Stale state collected"), Heap.Collect() == EHeapError::Ok);
			}
			uint8 Memory[64] = {};
			auto Handle = Fault == EFault::Unauthorized ? UnauthorizedHandle : Fault == EFault::ForeignWorld ? ForeignHandle : OwnerHandle;
			if (Fault == EFault::StaleSource) ++Handle.Generation;
			const int32 Ordinal = Fault == EFault::MissingEvent ? 99 : Fault == EFault::NegativeOrdinal ? -1 : 7;
			const uint64 StateObject = Fault == EFault::StoreNull ? 0 : Fault == EFault::StoreForeign ? ForeignObject : Object;
			FMemory::Memcpy(Memory, &Handle.Slot, 4); FMemory::Memcpy(Memory + 4, &Handle.Generation, 4);
			FMemory::Memcpy(Memory + 8, &Ordinal, 4); FMemory::Memcpy(Memory + 16, &StateObject, 8);
			const uint32 Packet[] = {Abi::Magic, static_cast<uint32>(Abi::ECommand::Collect)};
			FMemory::Memcpy(Memory + 48, Packet, sizeof(Packet));
			TestTrue(TEXT("Guest memory"), Runtime->WriteStateBytes(0, MakeArrayView(Memory), Error));
			FAvidScriptHostCall Outside; Outside.BindingId = EAvidScriptHostBindingId::EventManagedStateReadV1; Outside.IntArgs[0] = 1;
			FAvidScriptHostCallResult OutsideResult;
			TestFalse(TEXT("Native calls outside VM entry cannot read"), Runtime->DispatchHostCall(Outside, OutsideResult));
			const bool BadStore = (Fault >= EFault::StoreType && Fault <= EFault::ZeroType) || Fault == EFault::ReadOutside;
			const bool Denied = Fault >= EFault::Unauthorized && Fault <= EFault::MissingEvent;
			FAvidScriptWasmSmokeResult Result;
			TestEqual(TEXT("Actual Guest publish obeys type/context"), Runtime->Tick(0.0f, Result), !BadStore);
			TestTrue(TEXT("Caller frame exits"), Heap.PopFrame(Frame) == EHeapError::Ok);
			TestTrue(TEXT("Collect after caller exit"), Heap.Collect() == EHeapError::Ok);
			const bool Published = !BadStore && !Denied;
			TestEqual(TEXT("Atomic publication root count"), Heap.GetStats().LiveRoots, Published ? 1u : 0u);
			TestEqual(TEXT("Only published state survives"), Heap.IsAlive(Object), Published);
			TestEqual(TEXT("Rejected publication has no bridge"), Subscriptions.NumActive(), Published ? 1 : 0);
			if (Published)
			{
				TestTrue(TEXT("Read subscription token"), Runtime->ReadStateBytes(0, MakeArrayView(Memory), Error));
				int64 Token = 0; FMemory::Memcpy(&Token, Memory + 24, 8);
				TestTrue(TEXT("Full subscription token"), Token > 0);
				Owner->OnActorBeginOverlap.Broadcast(Owner, Unauthorized);
				const bool BadRead = Fault == EFault::ReadType || Fault == EFault::ReadTwice;
				if (BadRead)
				{
					TestTrue(TEXT("Bad read quarantines Session"), Session.GetSnapshot().bFaultQuarantined);
					TestNull(TEXT("Faulted Runtime unloaded"), Session.GetLiveRuntimeForTesting());
					Subscriptions.UnbindActive(); // late release into a retired heap is safe
					Ownership.Cleanup(Registry);
					continue;
				}
				TestFalse(TEXT("Valid callback does not quarantine"), Session.GetSnapshot().bFaultQuarantined);
				TestTrue(TEXT("Callback returned state"), Runtime->ReadStateBytes(0, MakeArrayView(Memory), Error));
				uint64 Restored = 0; FMemory::Memcpy(&Restored, Memory + 32, 8);
				TestEqual(TEXT("Exact state object identity restored"), Restored, Object);
				TestEqual(TEXT("Guest reaches past forced collection"), Memory[40], uint8(1));
				TestTrue(TEXT("Callback collection preserves state"), Heap.IsAlive(Object));
				Owner->OnActorBeginOverlap.Broadcast(Owner, Unauthorized);
				TestTrue(TEXT("Read repeated delivery"), Runtime->ReadStateBytes(0, MakeArrayView(Memory), Error));
				TestEqual(TEXT("Self cancel stops delivery, normal read resets per callback"), Memory[40], uint8(Fault == EFault::SelfCancel ? 1 : 2));
				TestEqual(TEXT("Unsubscribe uses existing API"), Runtime->HandleEventUnsubscribeImport(Token), Fault == EFault::SelfCancel ? 0 : 1);
			}
			Subscriptions.UnbindActive();
			TestTrue(TEXT("Final state collection"), Heap.Collect() == EHeapError::Ok);
			TestEqual(TEXT("No roots leak"), Heap.GetStats().LiveRoots, 0u);
			TestEqual(TEXT("No frames leak"), Heap.GetStats().ActiveFrames, 0u);
			TestFalse(TEXT("Subscription graph released"), Heap.IsAlive(Object));
			Session.StopAndUnload(Result);
			Ownership.Cleanup(Registry);
		}
		for (const bool WrongModule : {false, true})
		{
			FAvidScriptWasmRuntimeInstance Runtime(Selection); FAvidScriptWasmSmokeResult Result;
			const auto Wasm = Build(EFault::None, WrongModule ? "env" : "avidscript", !WrongModule);
			TestFalse(TEXT("Event state rejects env alias and incompatible signature"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("bad_event_state_abi"), Result));
		}
		FAvidScriptVmError ScopeError;
		auto Unscoped = CreateAvidScriptVmBackend(Selection, ScopeError);
		if (!TestNotNull(TEXT("VM without invocation observer"), Unscoped.Get())) return false;
		const auto WithoutHeapImport = Build(EFault::NoHeapImport);
		TestFalse(TEXT("Event-state-only module still requires invocation owner"), Unscoped->Load(MakeArrayView(WithoutHeapImport), TEXT("event_without_scope"), {}, ScopeError));
		TestEqual(TEXT("Missing event invocation owner diagnostic"), ScopeError.Category, FString(TEXT("managed_heap_scope_required")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptEventStateNestedScopeTest,
	"AvidScript.Runtime.DelegateSubscription.ManagedStateNestedScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEventStateNestedScopeTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	using namespace AvidScriptEventStateAbiTests;
	if (!GEngine) return false;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptEventNestedScope"));
	if (!World) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	AActor* Owner = World->SpawnActor<AActor>();
	if (!Owner) return false;
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const bool bNestedReads : {false, true})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		const auto Wasm = Build(EFault::NoHeapImport);
		FAvidScriptWasmSmokeResult Result;
		if (!Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("nested_event_state"), Result)) { AddError(Result.ErrorMessage); return false; }
		FAvidScriptObjectRegistry Registry;
		FAvidScriptObjectHandleResult HandleResult;
		FNestedReadProbe Probe;
		Probe.Runtime = &Runtime; Probe.bNestedReads = bNestedReads;
		Probe.Context.ObjectRegistry = &Registry;
		Probe.Context.OwnerHandle = Registry.RegisterObject(Owner, HandleResult, false);
		Probe.Context.World = World; Probe.Context.EventSubscriptions = &Probe;
		Runtime.SetHostContext(Probe.Context);
		if (!TestTrue(TEXT("Begin nested scope runtime"), Runtime.BeginPlay(Result))) return false;
		FString Error;
		FAvidScriptContextualExportCall EventCall;
		TestTrue(TEXT("Prepare event call"), Runtime.PrepareContextualExportCall(TEXT("event_callback"), EventCall, Error));
		TestTrue(TEXT("Prepare nested call"), Runtime.PrepareContextualExportCall(bNestedReads ? TEXT("event_callback") : TEXT("avid_on_begin_play"), Probe.Nested, Error));
		auto& Heap = *Runtime.GetManagedHeapForTesting();
		const std::array<FHeapLayout, 1> Layouts{{{1, 8, {}}}};
		TestTrue(TEXT("Nested state layout"), Heap.Configure(Layouts) == EHeapError::Ok);
		FToken Root = 0, Object = 0;
		TestTrue(TEXT("Native probe root"), Heap.CreateRoot(0, 0, Root) == EHeapError::Ok);
		TestTrue(TEXT("Native probe state"), Heap.Allocate(1, Root, Object) == EHeapError::Ok);
		Probe.State[0] = 1;
		for (unsigned I = 0; I < 8; ++I) Probe.State[4 + I] = static_cast<uint8>(Object >> (I * 8));
		FAvidScriptPreparedDelegateEvent Event;
		Event.StableId = FString::ChrN(64, '7'); Event.ExportName = TEXT("event_callback");
		Event.Signature.ImmutableCodecIdentity = &Probe;
		Event.Signature.ParameterCellCount = 1; Event.Signature.Encode = &EncodeFrame;
		TestEqual(TEXT("Nested export cannot impersonate the surrounding event"),
			Runtime.DispatchPreparedDelegateEventInContext(EventCall, Probe.Context, Event, nullptr, Result), !bNestedReads);
		TestEqual(TEXT("Nested read denied before contacting event owner"), Probe.Reads, 1);
		TestEqual(TEXT("Nested normal call succeeds but nested state read fails"), Probe.bNestedSucceeded, !bNestedReads);
		if (bNestedReads) TestTrue(TEXT("Rejection identifies event entry authority"), Probe.Error.Details.Contains(TEXT("Only the current event Guest entry")));
		else
		{
			uint8 Restored[8] = {};
			TestTrue(TEXT("Read outer restored state"), Runtime.ReadStateBytes(32, MakeArrayView(Restored), Error));
			uint64 Value = 0; FMemory::Memcpy(&Value, Restored, 8);
			TestEqual(TEXT("Successful nested return restores outer state authority"), Value, Object);
		}
		TestEqual(TEXT("Nested scopes leave no frames"), Heap.GetStats().ActiveFrames, 0u);
		Runtime.Unload();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptCompiledEventStateTest,
	"AvidScript.Runtime.DelegateSubscription.CompiledManagedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCompiledEventStateTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	using namespace AvidScriptEventStateAbiTests;
	if (!GEngine) return false;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptCompiledEventState"));
	if (!World) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	AActor* Owner = World->SpawnActor<AActor>();
	if (!Owner) return false;
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const FString Variant : {TEXT("normal"), TEXT("self-cancel"), TEXT("wrong"), TEXT("outside")})
	{
		AddInfo(FString::Printf(TEXT("Compiled event state backend=%d variant=%s"), static_cast<int32>(Backend), *Variant));
		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptManagedHeapTests/GuestFixtures"),
			FString::Printf(TEXT("event-state-%s.wasm"), *Variant));
		TArray<uint8> Bytes;
		if (!TestTrue(TEXT("WasmBackend.Tests generated the event-state fixture"), FFileHelper::LoadFileToArray(Bytes, *Path))) return false;
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptRuntimeSession Session;
		Session.SetBackendSelectionForTesting(Selection);
		auto Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("compiled_event_state"));
		Manifest.RequiredImports = {
			{TEXT("avidscript"), UTF8_TO_TCHAR(AvidScript::EventState::Abi::SubscribeImport)},
			{TEXT("avidscript"), UTF8_TO_TCHAR(AvidScript::EventState::Abi::ReadImport)},
			{TEXT("avidscript"), UTF8_TO_TCHAR(Abi::ImportName)},
			{TEXT("avidscript"), TEXT("event_unsubscribe")}, {TEXT("avidscript"), TEXT("owner_get_slot")},
			{TEXT("avidscript"), TEXT("owner_get_generation")}};
		FAvidScriptWasmReloadResult Loaded;
		if (!Session.LoadInitialModule(Bytes.GetData(), Bytes.Num(), Manifest, Loaded)) { AddError(Loaded.ErrorMessage); return false; }
		auto* Runtime = Session.GetLiveRuntimeForTesting();
		FAvidScriptObjectRegistry Registry;
		FAvidScriptObjectHandleResult HandleResult;
		FAvidScriptSessionDelegateSubscriptions Subscriptions(Session);
		FAvidScriptWasmHostContext Context;
		Context.World = World; Context.ObjectRegistry = &Registry;
		Context.OwnerHandle = Registry.RegisterObject(Owner, HandleResult, false);
		Context.EventSubscriptions = &Subscriptions;
		Runtime->SetHostContext(Context);
		FAvidScriptPreparedDelegateEvent Event;
		Event.EventOrdinal = 7; Event.StableId = FString::ChrN(64, '6'); Event.ExportName = TEXT("event_callback");
		Event.ExpectedSourceClass = AActor::StaticClass();
		Event.Signature.Kind = EAvidScriptPreparedDelegateKind::Multicast;
		Event.Signature.MulticastProperty = FindFProperty<FMulticastDelegateProperty>(AActor::StaticClass(), TEXT("OnActorBeginOverlap"));
		Event.Signature.SignatureFunction = Event.Signature.MulticastProperty->SignatureFunction;
		Event.Signature.ImmutableCodecIdentity = Event.Signature.SignatureFunction;
		Event.Signature.ParameterCellCount = 1; Event.Signature.Encode = &EncodeFrame;
		FString Error;
		if (!Subscriptions.Prepare(World, MakeArrayView(&Event, 1), Error, Runtime)) { AddError(Error); return false; }
		Subscriptions.CommitPrepared(); Subscriptions.SetDispatchEnabled(true);
		FAvidScriptWasmSmokeResult Result;
		const bool Outside = Variant == TEXT("outside");
		TestEqual(TEXT("Generated subscribe and read use the native invocation contract"), Runtime->Tick(0.0f, Result), !Outside);
		auto& Heap = *Runtime->GetManagedHeapForTesting();
		TestTrue(TEXT("Collect after generated publisher frame exits"), Heap.Collect() == EHeapError::Ok);
		TestEqual(TEXT("Only subscription lease survives publication"), Heap.GetStats().LiveRoots, Outside ? 0u : 1u);
		TestEqual(TEXT("Compiled state and cyclic child survive publication"), Heap.GetStats().LiveObjects, Outside ? 0u : 2u);
		TestEqual(TEXT("Generated publication frame cleaned on return or trap"), Heap.GetStats().ActiveFrames, 0u);
		if (!Outside)
		{
			Owner->OnActorBeginOverlap.Broadcast(Owner, Owner);
			if (Variant == TEXT("wrong"))
			{
				TestTrue(TEXT("Wrong generated state type quarantines Session"), Session.GetSnapshot().bFaultQuarantined);
				TestNull(TEXT("Wrong type unloads VM"), Session.GetLiveRuntimeForTesting());
				Subscriptions.UnbindActive();
				continue;
			}
			if (!TestFalse(TEXT("Compiled event executes without quarantine"), Session.GetSnapshot().bFaultQuarantined)) return false;
			uint8 State[16] = {};
			TestTrue(TEXT("Read compiler-produced observable state"), Runtime->ReadStateBytes(16, MakeArrayView(State), Error));
			int32 Count = 0, Value = 0; int64 Token = 0;
			FMemory::Memcpy(&Count, State, 4); FMemory::Memcpy(&Value, State + 4, 4); FMemory::Memcpy(&Token, State + 8, 8);
			TestEqual(TEXT("Generated callback executes once"), Count, 1);
			TestEqual(TEXT("Compiled state survives Guest collections and mutates"), Value, 43);
			TestTrue(TEXT("Subscription token is preserved"), Token > 0);
			const bool SelfCancel = Variant == TEXT("self-cancel");
			TestTrue(TEXT("Collect after first callback"), Heap.Collect() == EHeapError::Ok);
			TestEqual(TEXT("Self cancellation releases graph after callback frame exits"), Heap.GetStats().LiveObjects, SelfCancel ? 0u : 2u);
			TestEqual(TEXT("Callback temporary roots released"), Heap.GetStats().LiveRoots, SelfCancel ? 0u : 1u);
			Owner->OnActorBeginOverlap.Broadcast(Owner, Owner);
			TestTrue(TEXT("Read second generated callback result"), Runtime->ReadStateBytes(16, MakeArrayView(State), Error));
			FMemory::Memcpy(&Count, State, 4); FMemory::Memcpy(&Value, State + 4, 4);
			TestEqual(TEXT("Only live subscription receives second event"), Count, SelfCancel ? 1 : 2);
			TestEqual(TEXT("Second callback updates the same state graph"), Value, SelfCancel ? 43 : 44);
			TestEqual(TEXT("Existing unsubscribe releases compiled state"), Runtime->HandleEventUnsubscribeImport(Token), SelfCancel ? 0 : 1);
		}
		Subscriptions.UnbindActive();
		TestTrue(TEXT("Final compiled state collection"), Heap.Collect() == EHeapError::Ok);
		TestEqual(TEXT("No compiled event objects leak"), Heap.GetStats().LiveObjects, 0u);
		TestEqual(TEXT("No compiled event roots leak"), Heap.GetStats().LiveRoots, 0u);
		TestEqual(TEXT("No compiled event frames leak"), Heap.GetStats().ActiveFrames, 0u);
		Session.StopAndUnload(Result);
	}
	return true;
}
#endif
