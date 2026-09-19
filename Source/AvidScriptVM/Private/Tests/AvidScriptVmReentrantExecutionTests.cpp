#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptVmBackend.h"
#include "AvidScriptVmResultFixtureBuilder.h"
#include "Misc/AutomationTest.h"

namespace AvidScriptVmReentryTests
{
// Backend probe, not a Runtime receiver/authorization fixture. A single VM owns
// the global and memory location across logical A -> B -> A Host reentry.
TArray<uint8> BuildFixture()
{
	using namespace AvidScriptVmResultFixture;
	TArray<uint8> Module = { 0, 0x61, 0x73, 0x6d, 1, 0, 0, 0 };
	const TArray<uint8> Types = {
		3, 0x60, 1, 0x7f, 1, 0x7f, // (i32) -> i32
		0x60, 0, 1, 0x7f, // () -> i32
		0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f // heap ABI (unused; enables observer)
	};
	AppendSection(Module, 1, Types);
	TArray<uint8> Imports = { 2 };
	AppendString(Imports, "avidscript");
	AppendString(Imports, "host_add_i32");
	Imports.Append({ 0, 0 });
	AppendString(Imports, "avidscript");
	AppendString(Imports, "avid_managed_heap_v1");
	Imports.Append({ 0, 2 });
	AppendSection(Module, 2, Imports);
	AppendSection(Module, 3, TArray<uint8>{ 3, 0, 1, 1 });
	AppendSection(Module, 5, TArray<uint8>{ 1, 0, 1 });
	AppendSection(Module, 6, TArray<uint8>{ 1, 0x7f, 1, 0x41, 0, 0x0b });
	TArray<uint8> Exports = { 4 };
	AppendString(Exports, "step");
	Exports.Append({ 0, 2 });
	AppendString(Exports, "snapshot");
	Exports.Append({ 0, 3 });
	AppendString(Exports, "trap");
	Exports.Append({ 0, 4 });
	AppendString(Exports, "memory");
	Exports.Append({ 2, 0 });
	AppendSection(Module, 7, Exports);
	// code = (remaining depth << 1) | receiver bit; A=0, B=1.
	// memory[0] += receiver + 1; globals[0]++; next receiver toggles.
	const TArray<uint8> Step = {
		0,
		0x23, 0, 0x41, 1, 0x6a, 0x24, 0,
		0x41, 0, 0x41, 0, 0x28, 2, 0,
		0x20, 0, 0x41, 1, 0x71, 0x41, 1, 0x6a, 0x6a, 0x36, 2, 0,
		0x20, 0, 0x41, 1, 0x76, 0x04, 0x40,
		0x20, 0, 0x41, 2, 0x6b, 0x41, 1, 0x73, 0x10, 0, 0x1a, 0x0b,
		0x41, 0, 0x28, 2, 0, 0x0b
	};
	const TArray<uint8> Snapshot = {
		0, 0x41, 0, 0x28, 2, 0, 0x41, 0xe4, 0, 0x6c, 0x23, 0, 0x6a, 0x0b
	};
	const TArray<uint8> Trap = { 0, 0, 0x0b };
	TArray<uint8> Code = { 3 };
	for (const TArray<uint8>* Body : { &Step, &Snapshot, &Trap })
	{
		AppendU32Leb(Code, static_cast<uint32>(Body->Num()));
		Code.Append(*Body);
	}
	AppendSection(Module, 10, Code);
	return Module;
}

class FInvocationStack final : public IAvidScriptVmInvocationObserver
{
public:
	uint64 BeginVmInvocation() override
	{
		Stack.Add(++Serial);
		MaximumDepth = FMath::Max(MaximumDepth, Stack.Num());
		return Serial;
	}
	void EndVmInvocation(uint64 Token) override
	{
		bOrdered &= !Stack.IsEmpty() && Stack.Last() == Token;
		if (!Stack.IsEmpty()) Stack.Pop(EAllowShrinking::No);
		++Ends;
	}
	TArray<uint64> Stack;
	uint64 Serial = 0;
	uint64 Ends = 0;
	int32 MaximumDepth = 0;
	bool bOrdered = true;
};

enum class EScenario { Normal, Budget, Trap, Unload };

class FDispatcher final : public IAvidScriptHostDispatcher
{
public:
	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& Out) override
	{
		Out = {};
		if (Call.BindingId != EAvidScriptHostBindingId::HostAddI32 || ++HostCalls > 8)
		{
			Out.ErrorCategory = TEXT("probe_depth_limit");
			return false;
		}
		if (Call.IntArgs[0] == 0 && Scenario == EScenario::Unload)
		{
			Backend->Unload();
			bUnloadDeferred = Backend->IsLoaded();
			Out.bSucceeded = true;
			return true;
		}
		FAvidScriptVmCallFrame Frame;
		Frame.Cells[0] = static_cast<uint32>(Call.IntArgs[0]);
		Frame.CellCount = 1;
		FAvidScriptVmCallResult Result;
		FAvidScriptVmError Error;
		if (Call.IntArgs[0] == 0 && Scenario == EScenario::Trap)
		{
			Out.bSucceeded = Backend->Call(Trap, {}, Error, &Result);
		}
		else
		{
			Out.bSucceeded = bPrepared ? Prepared.Call(Frame, Error, &Result)
				: Backend->Call(Step, Frame, Error, &Result);
		}
		Out.ReturnValue = static_cast<int32>(Result.Cells[0]);
		Out.ErrorCategory = Error.Category;
		Out.Details = Error.Details;
		return Out.bSucceeded;
	}
	IAvidScriptVmBackend* Backend = nullptr;
	FAvidScriptVmExportHandle Step, Trap;
	FAvidScriptVmPreparedExportCall Prepared;
	EScenario Scenario = EScenario::Normal;
	bool bPrepared = false;
	bool bUnloadDeferred = false;
	int32 HostCalls = 0;
};

bool Run(FAutomationTestBase& Test, EAvidScriptVmBackendKind Kind)
{
	for (bool bPrepared : { false, true })
	for (EScenario Scenario : { EScenario::Normal, EScenario::Budget, EScenario::Trap, EScenario::Unload })
	{
		FInvocationStack Observer;
		FDispatcher Dispatcher;
		FAvidScriptVmError Error;
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Kind;
		Selection.ExecutionMode = Kind == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		TUniquePtr<IAvidScriptVmBackend> Backend = CreateAvidScriptVmBackend(Selection, Error);
		if (!Test.TestTrue(TEXT("required backend is available"), Backend.IsValid())) return false;
		Dispatcher.Backend = Backend.Get();
		Dispatcher.Scenario = Scenario;
		Dispatcher.bPrepared = bPrepared;
		FAvidScriptVmLoadConfig Config;
		Config.HostDispatcher = &Dispatcher;
		Config.InvocationObserver = &Observer;
		Config.ExecutionBudget.MaxHostCallsPerEntry = Scenario == EScenario::Budget ? 1 : 8;
		const TArray<uint8> Bytes = BuildFixture();
		if (!Test.TestTrue(*FString::Printf(TEXT("probe loads: %s"), *Error.Details),
			Backend->Load(Bytes, TEXT("shared_domain_probe"), Config, Error))) return false;
		FAvidScriptVmExportHandle Snapshot;
		if (!Backend->ResolveExport(TEXT("step"), Dispatcher.Step, Error)
			|| !Backend->ResolveExport(TEXT("trap"), Dispatcher.Trap, Error)
			|| !Backend->ResolveExport(TEXT("snapshot"), Snapshot, Error)
			|| !Backend->PrepareExportCall(Dispatcher.Step, Dispatcher.Prepared, Error))
		{
			Test.AddError(Error.Details);
			return false;
		}
		FAvidScriptVmCallFrame Frame;
		Frame.CellCount = 1;
		Frame.Cells[0] = 4; // A -> B -> A
		FAvidScriptVmCallResult Result;
		const bool bCalled = bPrepared ? Dispatcher.Prepared.Call(Frame, Error, &Result)
			: Backend->Call(Dispatcher.Step, Frame, Error, &Result);
		Test.TestEqual(TEXT("only normal chain succeeds"), bCalled, Scenario == EScenario::Normal);
		Test.TestTrue(TEXT("all nested invocation scopes unwind in order"),
			Observer.bOrdered && Observer.Stack.IsEmpty() && Observer.Serial == Observer.Ends);
		Test.TestEqual(TEXT("nested call depth is observed"), Observer.MaximumDepth,
			Scenario == EScenario::Normal || Scenario == EScenario::Trap ? 3 : 2);
		if (Scenario == EScenario::Unload)
		{
			Test.TestTrue(TEXT("unload waits for the outer VM call"), Dispatcher.bUnloadDeferred && !Backend->IsLoaded());
			Test.TestEqual(TEXT("deferred unload propagates"), Error.Category, FString(TEXT("reentrant_unload")));
			continue;
		}
		if (Scenario == EScenario::Budget)
		{
			Test.TestEqual(TEXT("nested calls do not replenish the outer budget"), Error.Category, FString(TEXT("host_call_budget_exhausted")));
			Test.TestEqual(TEXT("second Host call is rejected before dispatcher effects"), Dispatcher.HostCalls, 1);
		}
		else if (Scenario == EScenario::Trap)
		{
			Test.TestTrue(TEXT("inner trap reaches the outer caller"), Error.Category == TEXT("trap") || Error.Category == TEXT("guest_trap"));
		}
		else
		{
			Test.TestEqual(TEXT("outer A observes B and nested A writes"), Result.Cells[0], 4u);
			Test.TestEqual(TEXT("two synchronous Host transitions execute"), Dispatcher.HostCalls, 2);
		}
		Test.TestTrue(TEXT("snapshot remains readable after the VM call"), Backend->Call(Snapshot, {}, Error, &Result));
		Test.TestEqual(TEXT("memory identity and static counter survive; pre-failure writes are retained"),
			Result.Cells[0], Scenario == EScenario::Normal ? 403u : 302u);
		if (Scenario == EScenario::Budget)
		{
			Frame.Cells[0] = 2; // new outer call gets exactly one fresh Host call
			Test.TestTrue(TEXT("new outer entry receives a fresh budget"), Backend->Call(Dispatcher.Step, Frame, Error, &Result));
			Test.TestEqual(TEXT("state is retained across outer entries"), Result.Cells[0], 6u);
		}
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptVmWamrReentrantExecutionTest,
	"AvidScript.VM.ReentrantExecution.Wamr", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAvidScriptVmWamrReentrantExecutionTest::RunTest(const FString& Parameters)
{
	return AvidScriptVmReentryTests::Run(*this, EAvidScriptVmBackendKind::Wamr);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptVmWasmtimeReentrantExecutionTest,
	"AvidScript.VM.ReentrantExecution.Wasmtime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAvidScriptVmWasmtimeReentrantExecutionTest::RunTest(const FString& Parameters)
{
	return AvidScriptVmReentryTests::Run(*this, EAvidScriptVmBackendKind::Wasmtime);
}

#endif
