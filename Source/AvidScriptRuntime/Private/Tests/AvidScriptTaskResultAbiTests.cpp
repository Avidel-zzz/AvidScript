#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
#include "AvidScriptTaskResultAbi.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace AvidScriptTaskResultAbiTests
{
enum class EFixture : uint8
{
	Valid, Immediate, Cancelled, ForgedRead, ProvidedRead, BadSignature
};

void U32(TArray<uint8>& Out, uint32 Value)
{
	do
	{
		uint8 Byte = Value & 0x7f;
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
		uint8 Byte = Value & 0x7f;
		Value >>= 7;
		More = !((Value == 0 && !(Byte & 0x40))
			|| (Value == -1 && (Byte & 0x40)));
		Out.Add(Byte | (More ? 0x80 : 0));
	} while (More);
}

void Name(TArray<uint8>& Out, const char* Value)
{
	const int32 Size = FCStringAnsi::Strlen(Value);
	U32(Out, Size);
	Out.Append(reinterpret_cast<const uint8*>(Value), Size);
}

void Section(TArray<uint8>& Out, uint8 Id, const TArray<uint8>& Bytes)
{
	Out.Add(Id);
	U32(Out, Bytes.Num());
	Out.Append(Bytes);
}

void Load64(TArray<uint8>& Out, int32 Address)
{
	I32(Out, Address);
	Out.Append({0x29, 3, 0});
}

void Store64(TArray<uint8>& Out, int32 Address)
{
	I32(Out, Address);
}

void TaskCall(TArray<uint8>& Out, AvidScript::TaskResult::Abi::ECommand Command,
	int32 TokenAddress, int32 Argument)
{
	I32(Out, static_cast<int32>(Command));
	if (TokenAddress == 0)
		Out.Append({0x42, 0});
	else
		Load64(Out, TokenAddress);
	I32(Out, Argument);
	I32(Out, 0);
	Out.Append({0x10, 0});
}

void StoreCall(TArray<uint8>& Out, int32 OutputAddress,
	AvidScript::TaskResult::Abi::ECommand Command,
	int32 TokenAddress, int32 Argument)
{
	Store64(Out, OutputAddress);
	TaskCall(Out, Command, TokenAddress, Argument);
	Out.Append({0x37, 3, 0});
}

TArray<uint8> Build(EFixture Fixture)
{
	using namespace AvidScript::TaskResult::Abi;
	TArray<uint8> Wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	TArray<uint8> Types{4,
		0x60, 4, Fixture == EFixture::BadSignature ? uint8(0x7e) : uint8(0x7f),
		0x7e, 0x7f, 0x7f, 1, 0x7e, // import: (i32, i64, i32, i32) -> i64
		0x60, 0, 0, // BeginPlay
		0x60, 1, 0x7d, 0, // Tick
		0x60, 3, 0x7f, 0x7e, 0x7f, 0 // continuation
	};
	Section(Wasm, 1, Types);
	TArray<uint8> Imports{1};
	Name(Imports, "avidscript");
	Name(Imports, Int32Import);
	Imports.Append({0, 0});
	Section(Wasm, 2, Imports);
	Section(Wasm, 3, {3, 1, 2, 3});
	Section(Wasm, 5, {1, 0, 1});
	TArray<uint8> Exports{4};
	Name(Exports, "memory"); Exports.Append({2, 0});
	Name(Exports, "avid_on_begin_play"); Exports.Append({0, 1});
	Name(Exports, "avid_on_tick"); Exports.Append({0, 2});
	Name(Exports, "avid_on_continuation"); Exports.Append({0, 3});
	Section(Wasm, 7, Exports);

	TArray<uint8> Begin{0};
	if (Fixture == EFixture::Valid || Fixture == EFixture::Immediate
		|| Fixture == EFixture::Cancelled)
	{
		StoreCall(Begin, 8, ECommand::Create, 0, 0);
		if (Fixture == EFixture::Immediate)
			StoreCall(Begin, 24, ECommand::Succeed, 8, -12);
		StoreCall(Begin, 16, ECommand::Await, 8, 77);
		if (Fixture == EFixture::Valid)
			StoreCall(Begin, 24, ECommand::Succeed, 8, -12);
		else if (Fixture == EFixture::Cancelled)
			StoreCall(Begin, 24, ECommand::Cancel, 8, 0);
		StoreCall(Begin, 32, ECommand::Read, 8, 0);
		StoreCall(Begin, 40, ECommand::Release, 8, 0);
	}
	else if (Fixture == EFixture::ForgedRead)
	{
		I32(Begin, static_cast<int32>(ECommand::Read));
		Begin.Append({0x42, 1});
		I32(Begin, 0); I32(Begin, 0);
		Begin.Append({0x10, 0, 0x1a});
	}
	else if (Fixture == EFixture::ProvidedRead)
	{
		StoreCall(Begin, 32, ECommand::Read, 8, 0);
	}
	Begin.Add(0x0b);
	TArray<uint8> Resume{0};
	if (Fixture == EFixture::Valid || Fixture == EFixture::Cancelled)
		StoreCall(Resume, 48, ECommand::Read, 8, 0);
	Resume.Add(0x0b);
	TArray<uint8> Code{3};
	U32(Code, Begin.Num()); Code.Append(Begin);
	Code.Append({2, 0, 0x0b});
	U32(Code, Resume.Num()); Code.Append(Resume);
	Section(Wasm, 10, Code);
	return Wasm;
}

TArray<uint8> BuildFailurePropagation()
{
	using namespace AvidScript::TaskResult::Abi;
	TArray<uint8> Wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
	Section(Wasm, 1, {2,
		0x60, 2, 0x7e, 0x7e, 1, 0x7f, // import: (i64, i64) -> i32
		0x60, 0, 0}); // BeginPlay
	TArray<uint8> Imports{1};
	Name(Imports, "avidscript");
	Name(Imports, PropagateFailureImport);
	Imports.Append({0, 0});
	Section(Wasm, 2, Imports);
	Section(Wasm, 3, {1, 1});
	Section(Wasm, 5, {1, 0, 1});
	TArray<uint8> Exports{2};
	Name(Exports, "memory"); Exports.Append({2, 0});
	Name(Exports, "avid_on_begin_play"); Exports.Append({0, 1});
	Section(Wasm, 7, Exports);
	TArray<uint8> Begin{0};
	I32(Begin, 24);
	Load64(Begin, 8);
	Load64(Begin, 16);
	Begin.Append({0x10, 0, 0x36, 2, 0, 0x0b}); // call import; i32.store
	TArray<uint8> Code{1};
	U32(Code, Begin.Num()); Code.Append(Begin);
	Section(Wasm, 10, Code);
	return Wasm;
}

int64 ReadI64(const uint8* Bytes, int32 Offset)
{
	int64 Value = 0;
	FMemory::Memcpy(&Value, Bytes + Offset, sizeof(Value));
	return Value;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTaskResultAbiTest,
	"AvidScript.Runtime.Continuation.TaskResultAbi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskResultAbiTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptTaskResultAbiTests;
	using namespace AvidScript::TaskResult::Abi;
	if (!GEngine) return false;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false,
		TEXT("AvidScriptTaskResultAbiWorld"));
	if (!TestNotNull(TEXT("Task ABI world created"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmSmokeResult Result;
		const TArray<uint8> Bytes = Build(EFixture::Valid);
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		if (!TestTrue(TEXT("Task ABI module loads in both VMs"),
			Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), TEXT("task_result_i32"), Result)))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		auto& Endpoint = Owner->ResetActive(World);
		FAvidScriptWasmHostContext Context;
		Context.Tasks = &Endpoint;
		Context.Continuations = &Endpoint;
		Context.World = World;
		Runtime.SetHostContext(Context);
		if (!TestTrue(TEXT("WASM creates, awaits and completes Task<int>"), Runtime.BeginPlay(Result)))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		uint8 Memory[56] = {};
		FString Error;
		if (!TestTrue(TEXT("Read Task<int> fixture memory"),
			Runtime.ReadStateBytes(0, MakeArrayView(Memory), Error))) return false;
		const int64 Task = ReadI64(Memory, 8);
		const int64 Waiter = ReadI64(Memory, 16);
		TestTrue(TEXT("Guest receives opaque task token"), Task > 0);
		TestTrue(TEXT("Guest receives Session continuation token"), Waiter > 0);
		TestEqual(TEXT("Completion accepted"), ReadI64(Memory, 24), 1LL);
		TestEqual(TEXT("Immediate read preserves signed i32 value"),
			ReadI64(Memory, 32), static_cast<int64>(PackRead(EState::Succeeded, -12)));
		TestEqual(TEXT("Producer reference releases"), ReadI64(Memory, 40), 1LL);
		TArray<FAvidScriptContinuationCompletion> Ready;
		Owner->DrainReady(Ready);
		if (!TestEqual(TEXT("Task resumes exactly one waiter"), Ready.Num(), 1)) return false;
		TestEqual(TEXT("Task callback ID"), Ready[0].CallbackId, 77);
		TestEqual(TEXT("Task continuation token"), Ready[0].Token, Waiter);
		TestTrue(TEXT("Task completion has completed status"),
			Ready[0].Status == EAvidScriptContinuationStatus::Completed);
		if (!TestTrue(TEXT("WASM continuation reads terminal Task<int>"),
			Runtime.DispatchContinuation(Ready[0], Result)))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		TestTrue(TEXT("Read resumed Task<int> result"),
			Runtime.ReadStateBytes(0, MakeArrayView(Memory), Error));
		TestEqual(TEXT("Resumed read preserves signed i32 value"),
			ReadI64(Memory, 48), static_cast<int64>(PackRead(EState::Succeeded, -12)));
		TestTrue(TEXT("Dispatch finalization releases waiter reference"),
			Owner->FinalizeDispatched(Waiter, true));
		TestEqual(TEXT("No task result survives dispatch"),
			Owner->GetTaskResultsForTesting().GetCount(), 0);
		Owner->Teardown();

		const TArray<uint8> ImmediateBytes = Build(EFixture::Immediate);
		FAvidScriptWasmRuntimeInstance Immediate(Selection);
		TestTrue(TEXT("Immediate-result fixture loads"),
			Immediate.LoadModule(ImmediateBytes.GetData(), ImmediateBytes.Num(),
				TEXT("task_result_immediate"), Result));
		const auto ImmediateOwner = MakeShared<FAvidScriptSessionContinuations>();
		auto& ImmediateEndpoint = ImmediateOwner->ResetActive(World);
		Context.Tasks = &ImmediateEndpoint;
		Context.Continuations = &ImmediateEndpoint;
		Immediate.SetHostContext(Context);
		TestTrue(TEXT("Already-completed task runs synchronously"), Immediate.BeginPlay(Result));
		TestTrue(TEXT("Read immediate-result fixture memory"),
			Immediate.ReadStateBytes(0, MakeArrayView(Memory), Error));
		TestEqual(TEXT("Already-ready await returns no continuation"),
			ReadI64(Memory, 16), 0LL);
		TestEqual(TEXT("Already-ready task preserves its value"),
			ReadI64(Memory, 32), static_cast<int64>(PackRead(EState::Succeeded, -12)));
		ImmediateOwner->DrainReady(Ready);
		TestEqual(TEXT("Already-ready task never queues a callback"), Ready.Num(), 0);
		TestEqual(TEXT("Immediate task releases at final reference"),
			ImmediateOwner->GetTaskResultsForTesting().GetCount(), 0);
		ImmediateOwner->Teardown();

		const TArray<uint8> CancelledBytes = Build(EFixture::Cancelled);
		FAvidScriptWasmRuntimeInstance Cancelled(Selection);
		TestTrue(TEXT("Cancelled-result fixture loads"),
			Cancelled.LoadModule(CancelledBytes.GetData(), CancelledBytes.Num(),
				TEXT("task_result_cancelled"), Result));
		const auto CancelledOwner = MakeShared<FAvidScriptSessionContinuations>();
		auto& CancelledEndpoint = CancelledOwner->ResetActive(World);
		Context.Tasks = &CancelledEndpoint;
		Context.Continuations = &CancelledEndpoint;
		Cancelled.SetHostContext(Context);
		TestTrue(TEXT("WASM cancellation reaches a terminal task"), Cancelled.BeginPlay(Result));
		TestTrue(TEXT("Read cancelled-result fixture memory"),
			Cancelled.ReadStateBytes(0, MakeArrayView(Memory), Error));
		TestEqual(TEXT("Cancelled task encodes its state"),
			ReadI64(Memory, 32), static_cast<int64>(PackRead(EState::Cancelled, 0)));
		CancelledOwner->DrainReady(Ready);
		if (!TestEqual(TEXT("Cancellation resumes one waiter"), Ready.Num(), 1)) return false;
		TestTrue(TEXT("Cancelled callback has distinct status"),
			Ready[0].Status == EAvidScriptContinuationStatus::Cancelled);
		TestTrue(TEXT("Cancelled continuation can read terminal state"),
			Cancelled.DispatchContinuation(Ready[0], Result));
		TestTrue(TEXT("Read cancelled continuation memory"),
			Cancelled.ReadStateBytes(0, MakeArrayView(Memory), Error));
		TestEqual(TEXT("Cancelled callback reads the same state"),
			ReadI64(Memory, 48), static_cast<int64>(PackRead(EState::Cancelled, 0)));
		TestTrue(TEXT("Cancelled callback finalizes"),
			CancelledOwner->FinalizeDispatched(Ready[0].Token, true));
		TestEqual(TEXT("Cancelled result is reclaimed"),
			CancelledOwner->GetTaskResultsForTesting().GetCount(), 0);
		CancelledOwner->Teardown();

		FAvidScriptWasmRuntimeInstance WithoutSession(Selection);
		TestTrue(TEXT("No-session fixture loads"),
			WithoutSession.LoadModule(Bytes.GetData(), Bytes.Num(), TEXT("task_result_no_session"), Result));
		TestFalse(TEXT("Task import rejects missing Session context"),
			WithoutSession.BeginPlay(Result));
		TestEqual(TEXT("Missing Session has a stable error category"),
			Result.ErrorCategory, FString(TEXT("task_result_context")));

		const TArray<uint8> ForgedBytes = Build(EFixture::ForgedRead);
		FAvidScriptWasmRuntimeInstance Forged(Selection);
		TestTrue(TEXT("Forged-token fixture loads"),
			Forged.LoadModule(ForgedBytes.GetData(), ForgedBytes.Num(), TEXT("task_result_forged"), Result));
		const auto ForgedOwner = MakeShared<FAvidScriptSessionContinuations>();
		auto& ForgedEndpoint = ForgedOwner->ResetActive(World);
		Context.Tasks = &ForgedEndpoint;
		Context.Continuations = &ForgedEndpoint;
		Forged.SetHostContext(Context);
		TestFalse(TEXT("Task import rejects forged token"), Forged.BeginPlay(Result));
		TestEqual(TEXT("Forged token has a stable error category"),
			Result.ErrorCategory, FString(TEXT("task_result_identity")));
		ForgedOwner->Teardown();

		const TArray<uint8> ProvidedBytes = Build(EFixture::ProvidedRead);
		FAvidScriptWasmRuntimeInstance WrongType(Selection);
		TestTrue(TEXT("Wrong-type fixture loads"),
			WrongType.LoadModule(ProvidedBytes.GetData(), ProvidedBytes.Num(),
				TEXT("task_result_wrong_type"), Result));
		const auto WrongTypeOwner = MakeShared<FAvidScriptSessionContinuations>();
		auto& WrongTypeEndpoint = WrongTypeOwner->ResetActive(World);
		const int64 ForeignTypeTask = WrongTypeEndpoint.CreateTaskResult(TEXT("type:float32"));
		TestTrue(TEXT("Fixture creates wrong-type task"), ForeignTypeTask > 0);
		Context.Tasks = &WrongTypeEndpoint;
		Context.Continuations = &WrongTypeEndpoint;
		WrongType.SetHostContext(Context);
		uint8 ProvidedMemory[16] = {};
		FMemory::Memcpy(ProvidedMemory + 8, &ForeignTypeTask, sizeof(ForeignTypeTask));
		TestTrue(TEXT("Fixture supplies wrong-type task token"),
			WrongType.WriteStateBytes(0, MakeArrayView(ProvidedMemory), Error));
		TestFalse(TEXT("Task<int> import rejects other result type"), WrongType.BeginPlay(Result));
		TestEqual(TEXT("Wrong type has a stable identity error"),
			Result.ErrorCategory, FString(TEXT("task_result_identity")));
		WrongTypeOwner->Teardown();

		FAvidScriptWasmRuntimeInstance Faulted(Selection);
		TestTrue(TEXT("Fault-read fixture loads"),
			Faulted.LoadModule(ProvidedBytes.GetData(), ProvidedBytes.Num(),
				TEXT("task_result_faulted"), Result));
		const auto FaultedOwner = MakeShared<FAvidScriptSessionContinuations>();
		auto& FaultedEndpoint = FaultedOwner->ResetActive(World);
		const int64 FaultedTask = FaultedEndpoint.CreateTaskResult(TEXT("type:int32"));
		TArray<int64> FaultWaiters;
		TestTrue(TEXT("Native fault creates a terminal task"),
			FaultedEndpoint.FaultTaskResult(
				FaultedTask, TEXT("script_error"), FaultWaiters));
		Context.Tasks = &FaultedEndpoint;
		Context.Continuations = &FaultedEndpoint;
		Faulted.SetHostContext(Context);
		FMemory::Memcpy(ProvidedMemory + 8, &FaultedTask, sizeof(FaultedTask));
		TestTrue(TEXT("Fixture supplies faulted task token"),
			Faulted.WriteStateBytes(0, MakeArrayView(ProvidedMemory), Error));
		TestTrue(TEXT("Guest can read faulted state without a Host trap"),
			Faulted.BeginPlay(Result));
		TestTrue(TEXT("Read faulted fixture memory"),
			Faulted.ReadStateBytes(0, MakeArrayView(Memory), Error));
		TestEqual(TEXT("Faulted task has a distinct state"),
			ReadI64(Memory, 32), static_cast<int64>(PackRead(EState::Faulted, 0)));
		TestTrue(TEXT("Faulted task releases"), FaultedEndpoint.ReleaseTaskResult(FaultedTask));
		FaultedOwner->Teardown();

		const TArray<uint8> PropagationBytes = BuildFailurePropagation();
		for (const bool bFaulted : {false, true})
		{
			FAvidScriptWasmRuntimeInstance Propagation(Selection);
			if (!TestTrue(TEXT("Failure propagation fixture loads in both VMs"),
				Propagation.LoadModule(PropagationBytes.GetData(), PropagationBytes.Num(),
					bFaulted ? TEXT("task_fault_propagation") : TEXT("task_cancel_propagation"), Result)))
			{ AddError(Result.ErrorMessage); return false; }
			const auto PropagationOwner = MakeShared<FAvidScriptSessionContinuations>();
			auto& PropagationEndpoint = PropagationOwner->ResetActive(World);
			const int64 SourceTask = PropagationEndpoint.CreateTaskResult(TEXT("type:int32"));
			const int64 TargetTask = PropagationEndpoint.CreateTaskResult(TEXT("type:int32"));
			if (!TestTrue(TEXT("Failure propagation creates distinct task tokens"),
				SourceTask > 0 && TargetTask > 0 && SourceTask != TargetTask)) return false;
			TArray<int64> PropagationWaiters;
			const bool bSourceTerminated = bFaulted
				? PropagationEndpoint.FaultTaskResult(SourceTask, TEXT("script_error_inner"), PropagationWaiters)
				: PropagationEndpoint.CancelTaskResult(SourceTask, PropagationWaiters);
			if (!TestTrue(TEXT("Failure propagation source terminates"), bSourceTerminated)) return false;
			int64 TargetWaiter = 0;
			if (!TestTrue(TEXT("Failure propagation target registers a waiter"),
				PropagationEndpoint.AwaitTaskResult(TargetTask, 91, TargetWaiter)
					== EAvidScriptTaskWaitRegistration::Queued)) return false;
			Context.Tasks = &PropagationEndpoint;
			Context.Continuations = &PropagationEndpoint;
			Propagation.SetHostContext(Context);
			uint8 PropagationMemory[28] = {};
			FMemory::Memcpy(PropagationMemory + 8, &SourceTask, sizeof(SourceTask));
			FMemory::Memcpy(PropagationMemory + 16, &TargetTask, sizeof(TargetTask));
			if (!TestTrue(TEXT("Failure propagation supplies task tokens"),
				Propagation.WriteStateBytes(0, MakeArrayView(PropagationMemory), Error))) return false;
			if (!TestTrue(TEXT("WASM propagates source failure without a trap"),
				Propagation.BeginPlay(Result)))
			{ AddError(Result.ErrorMessage); return false; }
			if (!TestTrue(TEXT("Read failure propagation result"),
				Propagation.ReadStateBytes(0, MakeArrayView(PropagationMemory), Error))) return false;
			int32 Accepted = 0;
			FMemory::Memcpy(&Accepted, PropagationMemory + 24, sizeof(Accepted));
			TestEqual(TEXT("Failure propagation import accepts terminal source"), Accepted, 1);
			FAvidScriptTaskResultSnapshot Propagated;
			if (!TestTrue(TEXT("Target has a readable terminal result"),
				PropagationEndpoint.ReadTaskResult(TargetTask, Propagated))) return false;
			TestTrue(TEXT("Target preserves source terminal state"),
				Propagated.State == (bFaulted
					? EAvidScriptTaskResultState::Faulted : EAvidScriptTaskResultState::Cancelled));
			TestEqual(TEXT("Target preserves source error code"), Propagated.ErrorCode,
				bFaulted ? FString(TEXT("script_error_inner")) : FString());
			PropagationOwner->DrainReady(Ready);
			if (!TestEqual(TEXT("Failure propagation wakes one target waiter"), Ready.Num(), 1)) return false;
			TestTrue(TEXT("Target waiter receives propagated status"),
				Ready[0].Status == (bFaulted
					? EAvidScriptContinuationStatus::Failed : EAvidScriptContinuationStatus::Cancelled));
			TestTrue(TEXT("Failure propagation waiter finalizes"),
				PropagationOwner->FinalizeDispatched(TargetWaiter, true));
			TestTrue(TEXT("Failure propagation source releases"),
				PropagationEndpoint.ReleaseTaskResult(SourceTask));
			TestTrue(TEXT("Failure propagation target releases"),
				PropagationEndpoint.ReleaseTaskResult(TargetTask));
			TestEqual(TEXT("Failure propagation leaves no task results"),
				PropagationOwner->GetTaskResultsForTesting().GetCount(), 0);
			PropagationOwner->Teardown();
		}
		FAvidScriptWasmRuntimeInstance RejectedPropagation(Selection);
		if (!TestTrue(TEXT("Invalid failure propagation fixture loads"),
			RejectedPropagation.LoadModule(PropagationBytes.GetData(), PropagationBytes.Num(),
				TEXT("task_success_not_failure"), Result)))
		{ AddError(Result.ErrorMessage); return false; }
		const auto RejectedOwner = MakeShared<FAvidScriptSessionContinuations>();
		auto& RejectedEndpoint = RejectedOwner->ResetActive(World);
		const int64 SucceededSource = RejectedEndpoint.CreateTaskResult(TEXT("type:int32"));
		const int64 RunningTarget = RejectedEndpoint.CreateTaskResult(TEXT("type:int32"));
		const uint8 SuccessValue[4] = {1, 0, 0, 0};
		TArray<int64> RejectedWaiters;
		if (!TestTrue(TEXT("Invalid propagation source succeeds"),
			RejectedEndpoint.SucceedTaskResult(SucceededSource,
				MakeArrayView(SuccessValue), RejectedWaiters))) return false;
		Context.Tasks = &RejectedEndpoint;
		Context.Continuations = &RejectedEndpoint;
		RejectedPropagation.SetHostContext(Context);
		uint8 RejectedMemory[28] = {};
		FMemory::Memcpy(RejectedMemory + 8, &SucceededSource, sizeof(SucceededSource));
		FMemory::Memcpy(RejectedMemory + 16, &RunningTarget, sizeof(RunningTarget));
		if (!TestTrue(TEXT("Invalid propagation supplies task tokens"),
			RejectedPropagation.WriteStateBytes(0, MakeArrayView(RejectedMemory), Error))) return false;
		TestFalse(TEXT("Succeeded source cannot propagate failure"),
			RejectedPropagation.BeginPlay(Result));
		TestEqual(TEXT("Invalid propagation has a stable error category"),
			Result.ErrorCategory, FString(TEXT("task_result_propagate")));
		TestTrue(TEXT("Rejected propagation leaves target running"),
			RejectedEndpoint.SucceedTaskResult(RunningTarget,
				MakeArrayView(SuccessValue), RejectedWaiters));
		TestTrue(TEXT("Rejected propagation source releases"),
			RejectedEndpoint.ReleaseTaskResult(SucceededSource));
		TestTrue(TEXT("Rejected propagation target releases"),
			RejectedEndpoint.ReleaseTaskResult(RunningTarget));
		TestEqual(TEXT("Rejected propagation leaves no task results"),
			RejectedOwner->GetTaskResultsForTesting().GetCount(), 0);
		RejectedOwner->Teardown();

		const TArray<uint8> BadBytes = Build(EFixture::BadSignature);
		FAvidScriptWasmRuntimeInstance BadSignature(Selection);
		TestFalse(TEXT("Task import rejects mismatched WASM signature"),
			BadSignature.LoadModule(BadBytes.GetData(), BadBytes.Num(),
				TEXT("task_result_bad_signature"), Result));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCompiledTaskIntTest,
	"AvidScript.Runtime.Continuation.CompiledTaskInt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCompiledTaskIntTest::RunTest(const FString& Parameters)
{
	if (!GEngine) return false;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false,
		TEXT("AvidScriptCompiledTaskIntWorld"));
	if (!TestNotNull(TEXT("Compiled Task<int> world created"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const TCHAR* Scenario : {TEXT("immediate"), TEXT("deferred"), TEXT("teardown"), TEXT("chain"), TEXT("arguments")})
	{
		const bool bTeardown = FCString::Strcmp(Scenario, TEXT("teardown")) == 0;
		const bool bChain = FCString::Strcmp(Scenario, TEXT("chain")) == 0;
		const bool bArguments = FCString::Strcmp(Scenario, TEXT("arguments")) == 0;
		const bool bDeferred = FCString::Strcmp(Scenario, TEXT("immediate")) != 0;
		const FString Stem = FPaths::Combine(FPaths::ProjectSavedDir(),
			TEXT("AvidScriptManagedHeapTests/GuestFixtures"),
			FString::Printf(TEXT("csharp-task-int-%s"), bTeardown ? TEXT("deferred") : Scenario));
		TArray<uint8> Bytes;
		FString OffsetText;
		int32 ResultOffset = -1;
		if (!TestTrue(TEXT("Run Build/PrepareAvidScriptTaskIntFixtures.ps1 before this test"),
			FFileHelper::LoadFileToArray(Bytes, *(Stem + TEXT(".wasm"))))
			|| !TestTrue(TEXT("Compiled Task<int> result offset exists"),
				FFileHelper::LoadFileToString(OffsetText, *(Stem + TEXT(".result-offset"))))
			|| !TestTrue(TEXT("Compiled Task<int> result offset is bounded"),
				LexTryParseString(ResultOffset, *OffsetText) && ResultOffset >= 0 && ResultOffset < 65536))
			return false;
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(TEXT("Compiled C# Task<int> module loads"),
			Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), Scenario, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		if (!TestTrue(TEXT("Compiled C# task continuation export exists"),
			Runtime.ValidateRequiredExports({TEXT("avid_on_continuation_v2")}, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		auto& Endpoint = Owner->ResetActive(World);
		FAvidScriptWasmHostContext Context;
		Context.Tasks = &Endpoint;
		Context.Continuations = &Endpoint;
		Context.World = World;
		Runtime.SetHostContext(Context);
		if (!TestTrue(TEXT("Compiled C# Task<int> BeginPlay runs"), Runtime.BeginPlay(Result)))
		{ AddError(Result.ErrorMessage); return false; }
		auto ReadResult = [&]() -> int32
		{
			uint8 ValueBytes[4] = {};
			FString Error;
			if (!Runtime.ReadStateBytes(ResultOffset, MakeArrayView(ValueBytes), Error))
			{
				AddError(Error);
				return MIN_int32;
			}
			int32 Value = 0;
			FMemory::Memcpy(&Value, ValueBytes, sizeof(Value));
			return Value;
		};
		TestEqual(TEXT("Immediate completion or deferred initial state"),
			ReadResult(), bDeferred ? 0 : 12);
		TestEqual(TEXT("Task ownership after initial entry"),
			Owner->GetTaskResultsForTesting().GetCount(), bChain ? 2 : bDeferred ? 1 : 0);
		if (bTeardown) Owner->Teardown();
		int32 Resumes = 0;
		for (int32 Round = 0; bDeferred && Round < 8; ++Round)
		{
			World->Tick(LEVELTICK_All, 0.02f);
			++GFrameCounter;
			TArray<FAvidScriptContinuationCompletion> Ready;
			Owner->DrainReady(Ready);
			if (bTeardown)
			{
				TestEqual(TEXT("Teardown suppresses pending C# task callbacks"), Ready.Num(), 0);
				continue;
			}
			for (const auto& Completion : Ready)
			{
				if (!TestTrue(TEXT("Compiled C# Task<int> resume runs"),
					Runtime.DispatchContinuation(Completion, Result)))
				{ AddError(Result.ErrorMessage); return false; }
				TestTrue(TEXT("Compiled C# Task<int> continuation finalizes"),
					Owner->FinalizeDispatched(Completion.Token, true));
				++Resumes;
			}
		}
		TestEqual(TEXT("Compiled C# Task<int> has the expected resume count"), Resumes,
			bTeardown ? 0 : bChain ? 3 : bDeferred ? 2 : 0);
		TestEqual(TEXT("Compiled C# Task<int> preserves result"), ReadResult(),
			bTeardown ? 0 : bChain ? 13 : bArguments ? 75 : 12);
		TestEqual(TEXT("Compiled C# Task<int> releases all result references"),
			Owner->GetTaskResultsForTesting().GetCount(), 0);
		Owner->Teardown();
	}
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const TCHAR* Scenario : {TEXT("cancelled"), TEXT("cancelled-chain")})
	{
		const bool bChain = FCString::Strcmp(Scenario, TEXT("cancelled-chain")) == 0;
		const FString Stem = FPaths::Combine(FPaths::ProjectSavedDir(),
			TEXT("AvidScriptManagedHeapTests/GuestFixtures"),
			FString::Printf(TEXT("csharp-task-int-%s"), Scenario));
		TArray<uint8> Bytes;
		FString OffsetText;
		int32 ResultOffset = -1;
		if (!TestTrue(TEXT("Compiled cancelled Task<int> fixture exists"),
			FFileHelper::LoadFileToArray(Bytes, *(Stem + TEXT(".wasm"))))
			|| !TestTrue(TEXT("Cancelled Task<int> result offset exists"),
				FFileHelper::LoadFileToString(OffsetText, *(Stem + TEXT(".result-offset"))))
			|| !TestTrue(TEXT("Cancelled Task<int> result offset is bounded"),
				LexTryParseString(ResultOffset, *OffsetText)
					&& ResultOffset >= 0 && ResultOffset < 65536))
			return false;
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(TEXT("Compiled cancelled Task<int> module loads"),
			Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), Scenario, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		if (!TestTrue(TEXT("Cancelled C# task continuation export exists"),
			Runtime.ValidateRequiredExports({TEXT("avid_on_continuation_v2")}, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		auto& Endpoint = Owner->ResetActive(World);
		FAvidScriptWasmHostContext Context;
		Context.Tasks = &Endpoint;
		Context.Continuations = &Endpoint;
		Context.World = World;
		Runtime.SetHostContext(Context);
		if (!TestTrue(TEXT("C# Task<int> producer suspends"), Runtime.BeginPlay(Result)))
		{ AddError(Result.ErrorMessage); return false; }
		TestEqual(TEXT("Suspended producers own their task results"),
			Owner->GetTaskResultsForTesting().GetCount(), bChain ? 2 : 1);
		if (!TestTrue(TEXT("C# cancellation source cancels producer"),
			Runtime.Tick(0.016f, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		TArray<FAvidScriptContinuationCompletion> Ready;
		Owner->DrainReady(Ready);
		if (!TestEqual(TEXT("Cancelled task wakes its C# awaiter"), Ready.Num(), 1))
			return false;
		TestTrue(TEXT("Cancelled C# await receives terminal status"),
			Ready[0].Status == EAvidScriptContinuationStatus::Cancelled);
		if (bChain)
		{
			if (!TestTrue(TEXT("Middle Task<int> propagates cancellation without VM trap"),
				Runtime.DispatchContinuation(Ready[0], Result)))
			{ AddError(Result.ErrorMessage); return false; }
			TestTrue(TEXT("Middle Task<int> continuation finalizes"),
				Owner->FinalizeDispatched(Ready[0].Token, true));
			Owner->DrainReady(Ready);
			if (!TestEqual(TEXT("Parent cancellation wakes the outer awaiter"),
				Ready.Num(), 1)) return false;
			TestTrue(TEXT("Outer awaiter sees propagated cancellation"),
				Ready[0].Status == EAvidScriptContinuationStatus::Cancelled);
		}
		TestFalse(TEXT("C# await rejects cancelled Task<int> result"),
			Runtime.DispatchContinuation(Ready[0], Result));
		TestTrue(TEXT("Cancelled C# awaiter finalizes"),
			Owner->FinalizeDispatched(Ready[0].Token, false));
		uint8 ResultBytes[4] = {};
		FString Error;
		TestTrue(TEXT("Cancelled script result remains readable"),
			Runtime.ReadStateBytes(ResultOffset, MakeArrayView(ResultBytes), Error));
		int32 Value = 0;
		FMemory::Memcpy(&Value, ResultBytes, sizeof(Value));
		TestEqual(TEXT("Cancelled C# await does not publish value"), Value, 0);
		TestEqual(TEXT("Cancelled Task<int> result is reclaimed"),
			Owner->GetTaskResultsForTesting().GetCount(), 0);
		Owner->Teardown();
	}
	return true;
}

#endif
