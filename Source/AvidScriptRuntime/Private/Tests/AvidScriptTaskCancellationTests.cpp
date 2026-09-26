#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptManagedHeapAbi.h"
#include "AvidScriptRuntimeBackendTestLanes.h"
#include "AvidScriptTaskResultAbi.h"
#include "AvidScriptWasmRuntime.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"

#include <array>

namespace AvidScriptTaskCancellationTests
{
constexpr const TCHAR* ModuleId = TEXT("task_cancellation_abi");
// This is a native ABI fixture, not compiler-generated C# evidence.
enum EAddress : int32
{
	Source = 8, Target = 16, Frame = 24, Root = 32, Object = 40, Waiter = 48,
	Meta = 56, TargetObject = 64, SourceObject = 72, SourceState = 80,
	TargetState = 88, RepeatedObject = 96, Request = 256
};

void U32(TArray<uint8>& Out, uint32 Value)
{
	do
	{
		const uint8 Byte = Value & 0x7f;
		Value >>= 7;
		Out.Add(Byte | (Value ? 0x80 : 0));
	} while (Value);
}

void I32(TArray<uint8>& Out, int32 Value)
{
	Out.Add(0x41);
	bool More;
	do
	{
		const uint8 Byte = Value & 0x7f;
		Value >>= 7;
		More = !((Value == 0 && !(Byte & 0x40)) || (Value == -1 && (Byte & 0x40)));
		Out.Add(Byte | (More ? 0x80 : 0));
	} while (More);
}

void Name(TArray<uint8>& Out, const ANSICHAR* Value)
{
	const int32 Size = FCStringAnsi::Strlen(Value);
	U32(Out, Size);
	Out.Append(reinterpret_cast<const uint8*>(Value), Size);
}

void Section(TArray<uint8>& Out, uint8 Id, const TArray<uint8>& Payload)
{
	Out.Add(Id);
	U32(Out, Payload.Num());
	Out.Append(Payload);
}

void Custom(TArray<uint8>& Out, const ANSICHAR* Key, const FString& Value)
{
	TArray<uint8> Payload;
	Name(Payload, Key);
	const FTCHARToUTF8 Encoded(*Value);
	Payload.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	Section(Out, 0, Payload);
}

void Load64(TArray<uint8>& Out, int32 Address)
{
	I32(Out, Address);
	Out.Append({0x29, 3, 0});
}

void Store32(TArray<uint8>& Out, int32 Address, int32 Value)
{
	I32(Out, Address);
	I32(Out, Value);
	Out.Append({0x36, 2, 0});
}

void Copy64(TArray<uint8>& Out, int32 To, int32 From)
{
	I32(Out, To);
	Load64(Out, From);
	Out.Append({0x37, 3, 0});
}

void HeapCall(TArray<uint8>& Out, AvidScript::Managed::Abi::ECommand Command,
	int32 Bytes, int32 Output)
{
	Store32(Out, Request, AvidScript::Managed::Abi::Magic);
	Store32(Out, Request + 4, static_cast<int32>(Command));
	I32(Out, Request);
	I32(Out, Bytes);
	I32(Out, Output);
	I32(Out, 8);
	Out.Append({0x10, 0, 0x1a});
}

void TaskCall(TArray<uint8>& Out, AvidScript::TaskResult::Abi::ECommand Command,
	int32 TaskAddress, int32 Argument, int32 Output = 0)
{
	if (Output) I32(Out, Output);
	I32(Out, static_cast<int32>(Command));
	if (TaskAddress) Load64(Out, TaskAddress);
	else Out.Append({0x42, 0});
	I32(Out, Argument);
	I32(Out, 0);
	Out.Append({0x10, 1});
	if (Output) Out.Append({0x37, 3, 0});
	else Out.Add(0x1a);
}

void ReadCall(TArray<uint8>& Out, uint8 Import, int32 TaskAddress, int32 Output)
{
	I32(Out, Output);
	Load64(Out, TaskAddress);
	Out.Append({0x10, Import, 0x37, 3, 0});
}

FString Catalog()
{
	auto Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("schema_version"), 1);
	Document->SetNumberField(TEXT("guest_ir_schema_version"), 24);
	Document->SetStringField(TEXT("guest_ir_version"), TEXT("1.23"));
	Document->SetStringField(TEXT("module_id"), ModuleId);
	Document->SetStringField(TEXT("source_sha256"), FString::ChrN(64, 'a'));
	TArray<TSharedPtr<FJsonValue>> Types;
	for (const TCHAR* Id : {TEXT("type:global::System.Exception"),
		TEXT("type:global::System.OperationCanceledException"),
		TEXT("type:global::System.Threading.Tasks.TaskCanceledException")})
	{
		auto Type = MakeShared<FJsonObject>();
		Type->SetNumberField(TEXT("token"), Types.Num() + 1);
		Type->SetStringField(TEXT("type_id"), Id);
		Types.Add(MakeShared<FJsonValueObject>(Type));
	}
	Document->SetArrayField(TEXT("types"), Types);
	auto SourceSpan = MakeShared<FJsonObject>();
	SourceSpan->SetNumberField(TEXT("token"), 1);
	SourceSpan->SetStringField(TEXT("source_id"), TEXT("Tests/Cancellation.cs"));
	SourceSpan->SetNumberField(TEXT("source_length"), 32);
	SourceSpan->SetNumberField(TEXT("start"), 0);
	SourceSpan->SetNumberField(TEXT("length"), 12);
	SourceSpan->SetNumberField(TEXT("line"), 0);
	SourceSpan->SetNumberField(TEXT("column"), 0);
	SourceSpan->SetNumberField(TEXT("end_line"), 0);
	SourceSpan->SetNumberField(TEXT("end_column"), 12);
	Document->SetArrayField(TEXT("sources"), {MakeShared<FJsonValueObject>(SourceSpan)});
	FString Json;
	const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	check(FJsonSerializer::Serialize(Document, Writer));
	return Json;
}

TArray<uint8> Module()
{
	using namespace AvidScript::TaskResult::Abi;
	namespace HeapAbi = AvidScript::Managed::Abi;
	TArray<uint8> Wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	Section(Wasm, 1, {8,
		0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f, // heap packet
		0x60, 4, 0x7f, 0x7e, 0x7f, 0x7f, 1, 0x7e, // Task<int>
		0x60, 4, 0x7e, 0x7f, 0x7f, 0x7e, 1, 0x7f, // cancellation
		0x60, 2, 0x7e, 0x7e, 1, 0x7f, // propagation
		0x60, 1, 0x7e, 1, 0x7e, // terminal metadata / root
		0x60, 0, 0, // BeginPlay / EndPlay
		0x60, 1, 0x7d, 0, // Tick
		0x60, 3, 0x7f, 0x7e, 0x7f, 0}); // continuation
	TArray<uint8> Imports{6};
	uint8 ImportIndex = 0;
	for (const ANSICHAR* Import : {HeapAbi::ImportName, Int32Import, CancelLanguageErrorImport,
		PropagateFailureImport, TerminalErrorMetaImport, TerminalErrorRootImport})
	{
		Name(Imports, "avidscript");
		Name(Imports, Import);
		Imports.Add(0);
		Imports.Add(FMath::Min<uint8>(ImportIndex++, 4));
	}
	Section(Wasm, 2, Imports);
	Section(Wasm, 3, {4, 5, 6, 5, 7});
	Section(Wasm, 5, {1, 0, 1});
	TArray<uint8> Exports{5};
	Name(Exports, "memory");
	Exports.Append({2, 0});
	uint8 FunctionIndex = 6;
	for (const ANSICHAR* Export : {"avid_on_begin_play", "avid_on_tick",
		"avid_on_end_play", "avid_on_continuation"})
	{
		Name(Exports, Export);
		Exports.Append({0, FunctionIndex++});
	}
	Section(Wasm, 7, Exports);
	TArray<uint8> Begin{0};
	HeapCall(Begin, HeapAbi::ECommand::PushFrame, 8, Frame);
	Copy64(Begin, Request + 8, Frame);
	HeapCall(Begin, HeapAbi::ECommand::CreateRoot, 24, Root);
	Store32(Begin, Request + 8, 1);
	Copy64(Begin, Request + 12, Root);
	HeapCall(Begin, HeapAbi::ECommand::Allocate, 20, Object);
	TaskCall(Begin, ECommand::Create, 0, 0, Source);
	TaskCall(Begin, ECommand::Create, 0, 0, Target);
	TaskCall(Begin, ECommand::Await, Target, 41, Waiter);
	Load64(Begin, Source);
	I32(Begin, 3);
	I32(Begin, 1);
	Load64(Begin, Object);
	Begin.Append({0x10, 2, 0x1a});
	Load64(Begin, Source);
	Load64(Begin, Target);
	Begin.Append({0x10, 3, 0x1a});
	ReadCall(Begin, 4, Target, Meta);
	ReadCall(Begin, 5, Target, TargetObject);
	ReadCall(Begin, 5, Source, SourceObject);
	TaskCall(Begin, ECommand::Read, Source, 0, SourceState);
	TaskCall(Begin, ECommand::Read, Target, 0, TargetState);
	TaskCall(Begin, ECommand::Release, Source, 0);
	Begin.Add(0x0b);
	TArray<uint8> Read{0};
	HeapCall(Read, HeapAbi::ECommand::PushFrame, 8, Frame);
	ReadCall(Read, 5, Target, RepeatedObject);
	Read.Add(0x0b);
	TArray<uint8> Code{4};
	for (const TArray<uint8>& Body : {Begin, Read, TArray<uint8>{0, 0x0b}, Read})
	{
		U32(Code, Body.Num());
		Code.Append(Body);
	}
	Section(Wasm, 10, Code);
	Custom(Wasm, "avidscript.provenance", FString::Printf(
		TEXT("module_id=%s\nsource_id=Tests/Cancellation.cs\nsource_sha256=%s\n")
		TEXT("frontend_sha256=%s\nsemantic_sha256=%s\nguest_ir=24/1.23"),
		ModuleId, *FString::ChrN(64, 'a'), *FString::ChrN(64, 'b'), *FString::ChrN(64, 'c')));
	Custom(Wasm, "avidscript.language_errors", Catalog());
	return Wasm;
}

uint64 Read64(TConstArrayView<uint8> Bytes, int32 Offset)
{
	uint64 Value = 0;
	for (int32 Index = 0; Index < 8; ++Index)
		Value |= uint64(Bytes[Offset + Index]) << (8 * Index);
	return Value;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTaskCancellationAbiTest,
	"AvidScript.Runtime.Continuation.TaskCancellationAbi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskCancellationAbiTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptTaskCancellationTests;
	using namespace AvidScript::Managed;
	using namespace AvidScript::TaskResult::Abi;
	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	const TArray<uint8> Wasm = Module();
	for (const auto& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("cancellation ABI WASM loads")),
			Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), ModuleId, Result)))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		auto* Heap = Runtime.GetManagedHeapForTesting();
		const std::array<FHeapLayout, 1> Layouts{{{1, 8, {}}}};
		if (!TestTrue(TEXT("Cancellation heap configures"), Heap
			&& Heap->Configure(Layouts) == EHeapError::Ok)) return false;
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		auto& Endpoint = Owner->ResetActive(World.Get());
		FAvidScriptWasmHostContext Context;
		Context.World = World.Get();
		Context.Tasks = &Endpoint;
		Context.Continuations = &Endpoint;
		Runtime.SetHostContext(Context);
		if (!TestTrue(TEXT("WASM allocates, cancels, propagates and roots the same error"),
			Runtime.BeginPlay(Result)))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		uint8 Memory[104] = {};
		FString Error;
		if (!TestTrue(TEXT("ABI result memory is readable"),
			Runtime.ReadStateBytes(0, MakeArrayView(Memory), Error))) return false;
		const uint64 ErrorObject = Read64(Memory, Object);
		const int64 TargetTask = static_cast<int64>(Read64(Memory, Target));
		const int64 WaiterToken = static_cast<int64>(Read64(Memory, Waiter));
		TestTrue(TEXT("Task token retains high bits across both bridges"),
			static_cast<uint64>(TargetTask) > MAX_uint32);
		TestEqual(TEXT("Source stays cancelled"), Read64(Memory, SourceState), PackRead(EState::Cancelled, 0));
		TestEqual(TEXT("Propagation stays cancelled"), Read64(Memory, TargetState), PackRead(EState::Cancelled, 0));
		TestEqual(TEXT("Metadata preserves type and source"), Read64(Memory, Meta), (uint64(3) << 32) | 1);
		TestEqual(TEXT("Target returns original error object"), Read64(Memory, TargetObject), ErrorObject);
		TestEqual(TEXT("Source returns original error object"), Read64(Memory, SourceObject), ErrorObject);
		TestTrue(TEXT("Task lease survives invocation exit and GC"),
			Heap->Collect() == EHeapError::Ok && Heap->IsAlive(ErrorObject)
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 1);
		for (int32 Repeat = 0; Repeat < 2; ++Repeat)
		{
			TestTrue(TEXT("A later VM invocation can read the same cancellation"), Runtime.Tick(0.01f, Result));
			TestTrue(TEXT("Repeated read result is available"), Runtime.ReadStateBytes(0, MakeArrayView(Memory), Error));
			TestEqual(TEXT("Repeated await preserves object identity"), Read64(Memory, RepeatedObject), ErrorObject);
			TestEqual(TEXT("Repeated read frame roots do not leak"), Heap->GetStats().LiveRoots, 1u);
		}
		int64 AlreadyReady = 123;
		TestTrue(TEXT("Await after cancellation is immediately ready"),
			Endpoint.AwaitTaskResult(TargetTask, 42, AlreadyReady) == EAvidScriptTaskWaitRegistration::Ready);
		TestEqual(TEXT("Ready await creates no continuation"), AlreadyReady, 0LL);
		TestTrue(TEXT("Target producer releases its reference"), Endpoint.ReleaseTaskResult(TargetTask));
		TArray<FAvidScriptContinuationCompletion> Ready;
		Owner->DrainReady(Ready);
		if (!TestEqual(TEXT("Cancellation wakes exactly one queued waiter"), Ready.Num(), 1)) return false;
		TestEqual(TEXT("Correct waiter receives cancellation"), Ready[0].Token, WaiterToken);
		TestTrue(TEXT("Waiter receives Cancelled status"), Ready[0].Status == EAvidScriptContinuationStatus::Cancelled);
		TestTrue(TEXT("Waiter lease keeps payload readable in actual WASM"), Runtime.DispatchContinuation(Ready[0], Result));
		TestTrue(TEXT("Finalizing last waiter releases task"), Owner->FinalizeDispatched(WaiterToken, true));
		TestTrue(TEXT("Last Task reference releases cancellation object"),
			Heap->Collect() == EHeapError::Ok && !Heap->IsAlive(ErrorObject)
			&& Heap->GetStats().LiveRoots == 0 && Heap->GetStats().ActiveFrames == 0);
		TestEqual(TEXT("No Task survives final waiter"), Owner->GetTaskResultsForTesting().GetCount(), 0);
		Owner->Teardown();
		Runtime.Unload();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTaskCancellationAdmissionTest,
	"AvidScript.Runtime.Continuation.TaskCancellationAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskCancellationAdmissionTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptTaskCancellationTests;
	using namespace AvidScript::Managed;
	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	const TArray<uint8> Wasm = Module();
	for (const auto& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult LoadResult;
		if (!TestTrue(TEXT("Cancellation admission module loads"),
			Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), ModuleId, LoadResult))) return false;
		auto* Heap = Runtime.GetManagedHeapForTesting();
		const std::array<FHeapLayout, 1> Layouts{{{1, 8, {}}}};
		if (!TestTrue(TEXT("Admission heap configures"), Heap
			&& Heap->Configure(Layouts) == EHeapError::Ok)) return false;
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		const auto ForeignOwner = MakeShared<FAvidScriptSessionContinuations>();
		auto& Active = Owner->ResetActive(World.Get());
		auto& Foreign = ForeignOwner->ResetActive(World.Get());
		const int64 Task = Active.CreateTaskResult(TEXT("type:int32"));
		const int64 OtherTask = Foreign.CreateTaskResult(TEXT("type:int32"));
		const int64 WrongTypeTask = Active.CreateTaskResult(TEXT("type:float32"));
		FAvidScriptWasmHostContext Context;
		Context.World = World.Get();
		Context.Tasks = &Active;
		Context.Continuations = &Active;
		Runtime.SetHostContext(Context);
		FAvidScriptHostCallResult Result;
		auto Complete = [&Runtime, &Result](int64 Token, int32 Type, uint64 ObjectToken,
			int32 SourceToken = 1, bool bFault = false)
		{
			FAvidScriptHostCall Call;
			Call.BindingId = bFault ? EAvidScriptHostBindingId::TaskFaultLanguageErrorV1
				: EAvidScriptHostBindingId::TaskCancelLanguageErrorV1;
			Call.Int64Args[0] = Token;
			Call.Int64Args[1] = static_cast<int64>(ObjectToken);
			Call.IntArgs[0] = Type;
			Call.IntArgs[1] = SourceToken;
			return Runtime.DispatchHostCall(Call, Result);
		};
		auto Read = [&Runtime, &Result](EAvidScriptHostBindingId Binding, int64 Token)
		{
			FAvidScriptHostCall Call;
			Call.BindingId = Binding;
			Call.Int64Args[0] = Token;
			return Runtime.DispatchHostCall(Call, Result);
		};
		auto Allocate = [this, Heap]()
		{
			FToken FrameToken = 0, RootToken = 0, ObjectToken = 0;
			TestTrue(TEXT("Admission frame starts"), Heap->PushFrame(FrameToken) == EHeapError::Ok);
			TestTrue(TEXT("Admission root creates"), Heap->CreateRoot(FrameToken, 0, RootToken) == EHeapError::Ok);
			TestTrue(TEXT("Admission object allocates"), Heap->Allocate(1, RootToken, ObjectToken) == EHeapError::Ok);
			return ObjectToken;
		};
		TestFalse(TEXT("Cancellation outside a VM invocation is rejected"), Complete(Task, 3, 1));
		TestEqual(TEXT("Cancellation needs invocation context"), Result.ErrorCategory, FString(TEXT("task_result_context")));
		const FToken OlderObject = Allocate();
		const uint64 Invocation = Runtime.BeginVmInvocation();
		const FToken ErrorObject = Allocate();
		const uint32 InitialRoots = Heap->GetStats().LiveRoots;
		TestFalse(TEXT("Foreign Session cancellation is rejected"), Complete(OtherTask, 3, ErrorObject));
		TestEqual(TEXT("Foreign task rejection is identity failure"), Result.ErrorCategory, FString(TEXT("task_result_identity")));
		TestFalse(TEXT("Wrong Task value type is rejected"), Complete(WrongTypeTask, 3, ErrorObject));
		TestFalse(TEXT("Forged task token is rejected"), Complete(1, 3, ErrorObject));
		TestFalse(TEXT("Unknown type is rejected"), Complete(Task, 4, ErrorObject));
		TestFalse(TEXT("Unknown source span is rejected"), Complete(Task, 3, ErrorObject, 2));
		TestEqual(TEXT("Invalid source has catalog error"), Result.ErrorCategory, FString(TEXT("task_language_error_catalog")));
		TestFalse(TEXT("Ordinary Exception cannot masquerade as cancellation"), Complete(Task, 1, ErrorObject));
		TestEqual(TEXT("Wrong cancellation type is diagnosed"), Result.ErrorCategory, FString(TEXT("task_language_error_type")));
		TestFalse(TEXT("Missing object root is rejected"), Complete(Task, 3, 0));
		TestFalse(TEXT("Older invocation root is not producer authority"), Complete(Task, 3, OlderObject));
		TestEqual(TEXT("Wrong root has stable category"), Result.ErrorCategory, FString(TEXT("task_language_error_root")));
		TestEqual(TEXT("Rejected admissions retain no roots"), Heap->GetStats().LiveRoots, InitialRoots);
		FAvidScriptTaskResultSnapshot Snapshot;
		TestFalse(TEXT("Rejected admissions leave Task running"), Active.ReadTaskResult(Task, Snapshot));
		TestTrue(TEXT("TaskCanceledException completes cancellation"), Complete(Task, 3, ErrorObject));
		TestTrue(TEXT("Cancelled state retains the actual error payload"), Active.ReadTaskResult(Task, Snapshot)
			&& Snapshot.State == EAvidScriptTaskResultState::Cancelled && Snapshot.ErrorCode.IsEmpty()
			&& Snapshot.LanguageError.IsSet() && Snapshot.LanguageError->ObjectToken == ErrorObject);
		const uint32 CompletedRoots = Heap->GetStats().LiveRoots;
		TestFalse(TEXT("Terminal cancellation cannot complete twice"), Complete(Task, 3, ErrorObject));
		TestEqual(TEXT("Failed terminal write releases attempted lease"), Heap->GetStats().LiveRoots, CompletedRoots);
		TestFalse(TEXT("Fault cannot replace cancellation"), Complete(Task, 1, ErrorObject, 1, true));
		TestEqual(TEXT("Replacement fault also releases its attempted lease"), Heap->GetStats().LiveRoots, CompletedRoots);
		for (const auto Binding : {EAvidScriptHostBindingId::TaskLanguageErrorMetaV1,
			EAvidScriptHostBindingId::TaskLanguageErrorRootV1})
		{
			TestFalse(TEXT("Original fault-only reads reject cancellation"), Read(Binding, Task));
			TestEqual(TEXT("Old read fails on payload state"), Result.ErrorCategory, FString(TEXT("task_language_error_read")));
		}
		TestTrue(TEXT("Terminal metadata exposes cancellation type and source"),
			Read(EAvidScriptHostBindingId::TaskTerminalErrorMetaV1, Task)
			&& static_cast<uint64>(Result.ReturnValueI64) == ((uint64(3) << 32) | 1));
		TestEqual(TEXT("Metadata reads do not acquire roots"), Heap->GetStats().LiveRoots, CompletedRoots);
		TestTrue(TEXT("Terminal root acquires original object"),
			Read(EAvidScriptHostBindingId::TaskTerminalErrorRootV1, Task)
			&& static_cast<uint64>(Result.ReturnValueI64) == ErrorObject);
		const int64 BareTask = Active.CreateTaskResult(TEXT("type:int32"));
		TArray<int64> Woken;
		TestTrue(TEXT("Legacy bare cancellation still works"), Active.CancelTaskResult(BareTask, Woken));
		TestFalse(TEXT("Bare cancellation does not invent an error"), Read(EAvidScriptHostBindingId::TaskTerminalErrorMetaV1, BareTask));
		const int64 FaultTask = Active.CreateTaskResult(TEXT("type:int32"));
		TestTrue(TEXT("IR 24 retains ordinary language faults"), Complete(FaultTask, 1, ErrorObject, 1, true));
		TestTrue(TEXT("New terminal reads also accept fault payloads"),
			Read(EAvidScriptHostBindingId::TaskTerminalErrorRootV1, FaultTask)
			&& static_cast<uint64>(Result.ReturnValueI64) == ErrorObject);
		TestTrue(TEXT("Old fault metadata still works in IR 24"), Read(EAvidScriptHostBindingId::TaskLanguageErrorMetaV1, FaultTask));
		Runtime.EndVmInvocation(Invocation);
		Heap->UnwindToDepth(0);
		TestTrue(TEXT("Task error survives producer frame retirement"), Heap->Collect() == EHeapError::Ok && Heap->IsAlive(ErrorObject));
		const uint64 ReadInvocation = Runtime.BeginVmInvocation();
		TestFalse(TEXT("Root read requires an awaiter frame"), Read(EAvidScriptHostBindingId::TaskTerminalErrorRootV1, Task));
		TestEqual(TEXT("Missing awaiter frame is diagnosed"), Result.ErrorCategory, FString(TEXT("task_language_error_root")));
		Runtime.EndVmInvocation(ReadInvocation);

		// Rollback releases only candidate roots; publication retires the previous
		// activation. Neither path may enter Guest cleanup on a retired endpoint.
		auto& Prepared = Owner->BeginPrepared(World.Get());
		const int64 Candidate = Prepared.CreateTaskResult(TEXT("type:int32"));
		const uint64 CandidateInvocation = Runtime.BeginVmInvocation();
		const FToken CandidateObject = Allocate();
		TestFalse(TEXT("Active endpoint cannot cancel a prepared Task"), Complete(Candidate, 2, CandidateObject));
		Context.Tasks = &Prepared;
		Context.Continuations = &Prepared;
		Runtime.SetHostContext(Context);
		TestFalse(TEXT("Prepared endpoint cannot read active cancellation"), Read(EAvidScriptHostBindingId::TaskTerminalErrorMetaV1, Task));
		TestTrue(TEXT("OperationCanceledException is admitted in candidate"), Complete(Candidate, 2, CandidateObject));
		Runtime.EndVmInvocation(CandidateInvocation);
		Owner->DiscardPrepared();
		TestTrue(TEXT("Rollback releases candidate error but retains active error"),
			Heap->Collect() == EHeapError::Ok && !Heap->IsAlive(CandidateObject) && Heap->IsAlive(ErrorObject));
		auto& Published = Owner->BeginPrepared(World.Get());
		const int64 PublishedTask = Published.CreateTaskResult(TEXT("type:int32"));
		Context.Tasks = &Published;
		Context.Continuations = &Published;
		Runtime.SetHostContext(Context);
		const uint64 PublishedInvocation = Runtime.BeginVmInvocation();
		const FToken PublishedObject = Allocate();
		int64 PublishedWaiter = 0;
		TestTrue(TEXT("Candidate waiter queues before cancellation"),
			Published.AwaitTaskResult(PublishedTask, 51, PublishedWaiter) == EAvidScriptTaskWaitRegistration::Queued);
		TestTrue(TEXT("Published candidate owns a cancellation error"), Complete(PublishedTask, 3, PublishedObject));
		Runtime.EndVmInvocation(PublishedInvocation);
		Owner->CommitPrepared();
		TestTrue(TEXT("Publication releases old error and keeps candidate error"),
			Heap->Collect() == EHeapError::Ok && !Heap->IsAlive(ErrorObject) && Heap->IsAlive(PublishedObject));
		TestFalse(TEXT("Retired endpoint cannot read old cancellation"), Active.ReadTaskResult(Task, Snapshot));
		TestTrue(TEXT("Published Task remains cancelled"), Published.ReadTaskResult(PublishedTask, Snapshot)
			&& Snapshot.State == EAvidScriptTaskResultState::Cancelled);
		Owner->Teardown();
		Owner->Teardown();
		TArray<FAvidScriptContinuationCompletion> Ready;
		Owner->DrainReady(Ready);
		TestTrue(TEXT("Teardown drops queued cancellation without reentering Guest"), Ready.IsEmpty());
		TestTrue(TEXT("Teardown releases last cancellation owner"),
			Heap->Collect() == EHeapError::Ok && !Heap->IsAlive(PublishedObject)
			&& Heap->GetStats().LiveRoots == 0 && Heap->GetStats().ActiveFrames == 0);
		TestEqual(TEXT("Teardown leaves no tasks"), Owner->GetTaskResultsForTesting().GetCount(), 0);
		ForeignOwner->Teardown();
		Runtime.Unload();
	}
	return true;
}

#endif
