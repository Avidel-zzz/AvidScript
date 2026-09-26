#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptLanguageErrorCatalog.h"
#include "AvidScriptWasmRuntime.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTaskErrorTransferTest,
	"AvidScript.Runtime.ManagedHeap.TaskErrorTransfer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskErrorTransferTest::RunTest(const FString& Parameters)
{
	using namespace AvidScript::Managed;
	// IR/backend fixtures exercise the production Host. C# source integration has its own gate.
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptTaskErrorTransferTests/GuestFixtures"));
	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const bool bCooperative : {false, true})
	for (const bool bCached : {false, true})
	{
		if (Backend == EAvidScriptVmBackendKind::Wamr && bCooperative) continue;
		AddInfo(FString::Printf(TEXT("Task error transfer: backend=%d cooperative=%d cached=%d"), static_cast<int32>(Backend), bCooperative, bCached));
		const FString Filename = FString(bCached ? TEXT("cached") : TEXT("local")) + (bCooperative ? TEXT("-cooperative.wasm") : TEXT(".wasm"));
		TArray<uint8> Wasm;
		if (!TestTrue(TEXT("Read generated transfer fixture"), FFileHelper::LoadFileToArray(Wasm, *(Directory / Filename)))) return false;
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime ? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		Selection.bAllowFallback = false;
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(TEXT("Transfer module loads"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), TEXT("csharp:Scripts/TaskErrorTransfer.cs"), Result)))
		{ AddError(Result.ErrorMessage); return false; }
		TestEqual(TEXT("Requested backend executes"), Runtime.GetActiveBackendInfo().Kind, Backend);
		TestEqual(TEXT("Requested mode executes"), Runtime.GetActiveBackendInfo().ExecutionMode, Selection.ExecutionMode);
		if (!TestTrue(TEXT("Outer IR retains its Task error profile"), Runtime.GetLanguageErrorCatalog()
			&& Runtime.GetLanguageErrorCatalog()->SupportsTaskLanguageErrorFault())) return false;
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		auto& Endpoint = Owner->ResetActive(World.Get());
		FAvidScriptWasmHostContext Context;
		Context.World = World.Get(); Context.Tasks = &Endpoint; Context.Continuations = &Endpoint;
		Runtime.SetHostContext(Context);
		FHeap* Heap = Runtime.GetManagedHeapForTesting();
		if (!TestNotNull(TEXT("Transfer domain has a heap"), Heap)) return false;
		const uint32 StaticRoots = bCached ? 1u : 0u;
		auto Call = [&](const TCHAR* Name, int64 Task, bool bFail, uint64 Expected)
		{
			FAvidScriptVmPreparedExportCall Prepared; FString Error;
			if (!Runtime.PrepareNamedExportCall(Name, Prepared, Error)) { AddError(Error); return false; }
			FAvidScriptVmCallFrame Frame;
			Frame.CellCount = Task ? (FString(Name) == TEXT("transfer") ? 3 : 2) : 0;
			FMemory::Memcpy(Frame.Cells, &Task, sizeof(Task));
			Frame.Cells[2] = bFail ? 1 : 0;
			FAvidScriptVmCallResult Value; FAvidScriptVmError VmError;
			const uint64 Invocation = Runtime.BeginVmInvocation();
			const bool bCalled = Prepared.Call(Frame, VmError, &Value);
			Runtime.EndVmInvocation(Invocation);
			if (!bCalled) { AddError(VmError.Details); return false; }
			uint64 Actual = Value.Cells[0];
			if (FString(Name) == TEXT("read")) Actual |= uint64(Value.Cells[1]) << 32;
			return TestEqual(TEXT("Generated export returns expected value"), Actual, Expected);
		};
		for (int32 Iteration = 0; Iteration < 16; ++Iteration)
		{
			if (!Call(TEXT("init"), 0, false, 1)) return false;
			const int64 Success = Endpoint.CreateTaskResult(TEXT("type:int32"));
			if (!TestTrue(TEXT("Create success-path task"), Success > 0) || !Call(TEXT("transfer"), Success, false, 41)) return false;
			FAvidScriptTaskResultSnapshot Snapshot;
			TestTrue(TEXT("Success branch keeps the pending task valid"), Endpoint.HasTaskResultType(Success, TEXT("type:int32")));
			TestFalse(TEXT("Success branch has no terminal error to read"), Endpoint.ReadTaskResult(Success, Snapshot));
			const uint8 SuccessBytes[] = {41, 0, 0, 0};
			TArray<int64> Waiters;
			TestTrue(TEXT("Caller can complete the normal result and release its producer reference"),
				Endpoint.SucceedTaskResult(Success, MakeArrayView(SuccessBytes), Waiters));
			TestTrue(TEXT("Normal result remains successful"), Endpoint.ReadTaskResult(Success, Snapshot)
				&& Snapshot.State == EAvidScriptTaskResultState::Succeeded && !Snapshot.LanguageError.IsSet());
			TestTrue(TEXT("Release success-path task"), Endpoint.ReleaseTaskResult(Success));
			const int64 First = Endpoint.CreateTaskResult(TEXT("type:int32"));
			const int64 Second = Endpoint.CreateTaskResult(TEXT("type:int32"));
			if (!TestTrue(TEXT("Create fault recipients"), First > 0 && Second > 0)
				|| !Call(TEXT("transfer"), First, true, 1) || !Call(TEXT("transfer"), Second, true, 1)) return false;
			FAvidScriptTaskResultSnapshot A, B;
			if (!TestTrue(TEXT("Tasks preserve fault state and typed payload"), Endpoint.ReadTaskResult(First, A)
				&& Endpoint.ReadTaskResult(Second, B) && A.LanguageError.IsSet() && B.LanguageError.IsSet()
				&& A.State == EAvidScriptTaskResultState::Faulted && B.State == EAvidScriptTaskResultState::Faulted)) return false;
			const FToken FirstObject = A.LanguageError->ObjectToken, SecondObject = B.LanguageError->ObjectToken;
			TestEqual(TEXT("Cached outcomes share identity; local producers allocate independently"), FirstObject == SecondObject, bCached);
			if (bCached)
			{
				FToken Cached = 0;
				TestTrue(TEXT("Task contains the original static object"), Heap->ReadStaticSlot(1, 1, Cached) == EHeapError::Ok && Cached == FirstObject);
			}
			TestTrue(TEXT("GC preserves both recipients after callee and caller exit"), Heap->Collect() == EHeapError::Ok
				&& Heap->IsAlive(FirstObject) && Heap->IsAlive(SecondObject) && Heap->GetStats().LiveObjects == (bCached ? 1u : 2u));
			TestEqual(TEXT("Each independent fault owns one root"), Heap->GetStats().LiveRoots, StaticRoots + 2);
			if (!Call(TEXT("read"), First, false, (uint64(1) << 32) | 1)
				|| !Call(TEXT("read"), Second, false, (uint64(1) << 32) | 1)) return false;
			TestTrue(TEXT("Release first recipient"), Endpoint.ReleaseTaskResult(First));
			TestTrue(TEXT("Other task remains rooted"), Heap->Collect() == EHeapError::Ok && Heap->IsAlive(SecondObject));
			TestEqual(TEXT("Releasing one task releases only its root"), Heap->GetStats().LiveRoots, StaticRoots + 1);
			if (!Call(TEXT("clear"), 0, false, 1) || !Call(TEXT("read"), Second, false, (uint64(1) << 32) | 1)) return false;
			TestTrue(TEXT("Task survives static cache clear"), Heap->IsAlive(SecondObject));
			TestTrue(TEXT("Release last recipient"), Endpoint.ReleaseTaskResult(Second));
			TestTrue(TEXT("Last release collects the original exception"), Heap->Collect() == EHeapError::Ok
				&& !Heap->IsAlive(FirstObject) && !Heap->IsAlive(SecondObject) && Heap->GetStats().LiveObjects == 0);
			TestEqual(TEXT("No transient roots leak"), Heap->GetStats().LiveRoots, StaticRoots);
			TestEqual(TEXT("No VM frames leak"), Heap->GetStats().ActiveFrames, 0u);
			TestEqual(TEXT("No Task token leaks"), Owner->GetTaskResultsForTesting().GetCount(), 0);
		}
		if (!Call(TEXT("init"), 0, false, 1)) return false;
		const int64 PendingRelease = Endpoint.CreateTaskResult(TEXT("type:int32"));
		if (!TestTrue(TEXT("Create teardown task"), PendingRelease > 0) || !Call(TEXT("transfer"), PendingRelease, true, 1)) return false;
		Owner->Teardown();
		TestEqual(TEXT("Owner teardown drops remaining Tasks"), Owner->GetTaskResultsForTesting().GetCount(), 0);
		TestTrue(TEXT("Owner teardown leaves only domain static storage"), Heap->Collect() == EHeapError::Ok
			&& Heap->GetStats().LiveRoots == StaticRoots && Heap->GetStats().LiveObjects == StaticRoots);
		Runtime.Unload();
		TestNull(TEXT("Domain unload releases its heap and static roots"), Runtime.GetManagedHeapForTesting());
	}
	return true;
}

#endif
