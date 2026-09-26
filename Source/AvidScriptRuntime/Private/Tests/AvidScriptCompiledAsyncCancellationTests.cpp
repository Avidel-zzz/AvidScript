#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCompiledAsyncCancellationTest,
	"AvidScript.Runtime.Continuation.CompiledAsyncCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCompiledAsyncCancellationTest::RunTest(const FString& Parameters)
{
	if (!GEngine) return false;
	const FString Directory = FPlatformMisc::GetEnvironmentVariable(
		TEXT("AVIDSCRIPT_CSHARP_CANCELLATION_WASM_DIR"));
	TArray<uint8> Bytes;
	FString Json;
	TSharedPtr<FJsonObject> Ir;
	if (!TestFalse(TEXT("Compiled cancellation directory supplied"), Directory.IsEmpty())
		|| !TestTrue(TEXT("Compiler WASM exists"), FFileHelper::LoadFileToArray(
			Bytes, *(Directory / TEXT("cancellation.wasm"))))
		|| !TestTrue(TEXT("Compiler IR exists"), FFileHelper::LoadFileToString(
			Json, *(Directory / TEXT("cancellation.guestir.json"))))
		|| !TestTrue(TEXT("Compiler IR parses"), FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Json), Ir)) || !Ir.IsValid()) return false;
	if (!TestEqual(TEXT("Compiler cancellation IR version"),
		Ir->GetIntegerField(TEXT("schema_version")), 24)) return false;
	const FString ModuleId = Ir->GetStringField(TEXT("module_id"));
	TMap<FString, int32> Offsets;
	for (const auto& Value : Ir->GetObjectField(TEXT("memory_layout"))->GetArrayField(TEXT("state_slots")))
	{
		const auto Slot = Value->AsObject();
		if (Slot->GetStringField(TEXT("type_id")) == TEXT("type:int32")
			&& Slot->GetIntegerField(TEXT("size")) == 4)
			Offsets.Add(Slot->GetStringField(TEXT("global_id")), Slot->GetIntegerField(TEXT("offset")));
	}
	auto Offset = [&](const TCHAR* Owner, const TCHAR* Field) -> int32
	{
		const int32* Found = Offsets.Find(FString::Printf(
			TEXT("global:symbol:field:global::%s.%s:int32"), Owner, Field));
		if (!Found || *Found < 0 || *Found > 65532)
		{ AddError(FString::Printf(TEXT("Missing compiler state slot %s.%s"), Owner, Field)); return -1; }
		return *Found;
	};
	const int32 ModeOffset = Offset(TEXT("CancellationGuestEntry"), TEXT("TestCase"));
	const int32 ResultOffset = Offset(TEXT("CancellationGuestEntry"), TEXT("Result"));
	const int32 TraceOffset = Offset(TEXT("CancellationScript"), TEXT("Trace"));
	const int32 InnerOffset = Offset(TEXT("CancellationScript"), TEXT("InnerCatch"));
	const int32 OuterOffset = Offset(TEXT("CancellationScript"), TEXT("OuterCatch"));
	const int32 WrongOffset = Offset(TEXT("CancellationScript"), TEXT("WrongCatch"));
	if (HasAnyErrors()) return false;
	FString ExpectedCancellationDiagnostic;
	for (const auto& RouteValue : Ir->GetArrayField(TEXT("direct_await_routes")))
	{
		const auto Route = RouteValue->AsObject();
		if (!Route->GetStringField(TEXT("method_function_id")).Contains(TEXT("CancellationScript.InnerAsync("))) continue;
		const auto Cancellation = Route->GetObjectField(TEXT("cancellation"));
		const auto Catalog = Ir->GetObjectField(TEXT("language_error_catalog"));
		FString TypeId;
		for (const auto& TypeValue : Catalog->GetArrayField(TEXT("types")))
		{
			const auto Type = TypeValue->AsObject();
			if (Type->GetIntegerField(TEXT("token")) == Cancellation->GetIntegerField(TEXT("type_token")))
				TypeId = Type->GetStringField(TEXT("type_id"));
		}
		if (!TestEqual(TEXT("Original cancellation type"), TypeId,
			FString(TEXT("type:global::System.Threading.Tasks.TaskCanceledException")))) return false;
		for (const auto& SourceValue : Catalog->GetArrayField(TEXT("sources")))
		{
			const auto Source = SourceValue->AsObject();
			if (Source->GetIntegerField(TEXT("token")) == Cancellation->GetIntegerField(TEXT("source_token")))
				ExpectedCancellationDiagnostic = FString::Printf(
					TEXT("Uncaught %s at %s:%d:%d (UTF-16 span %d+%d)."), *TypeId,
					*Source->GetStringField(TEXT("source_id")), Source->GetIntegerField(TEXT("line")),
					Source->GetIntegerField(TEXT("column")), Source->GetIntegerField(TEXT("start")),
					Source->GetIntegerField(TEXT("length")));
		}
	}
	if (!TestFalse(TEXT("Original cancellation source is available"), ExpectedCancellationDiagnostic.IsEmpty())) return false;

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false,
		TEXT("AvidScriptCompiledAsyncCancellationWorld"));
	if (!TestNotNull(TEXT("Cancellation world created"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
	for (const bool bCancel : {false, true})
	for (int32 Case = 0; Case < 8; ++Case)
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(TEXT("Compiled cancellation module loads"),
			Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), ModuleId, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		if (!TestTrue(TEXT("Compiled cancellation exports exist"), Runtime.ValidateRequiredExports(
			{TEXT("avid_on_begin_play"), TEXT("avid_on_tick"), TEXT("avid_on_end_play"),
				TEXT("avid_on_continuation_v2")}, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		auto& Endpoint = Owner->ResetActive(World);
		ON_SCOPE_EXIT { Owner->Teardown(); };
		FAvidScriptWasmHostContext Context;
		Context.Tasks = &Endpoint;
		Context.Continuations = &Endpoint;
		Context.World = World;
		Runtime.SetHostContext(Context);
		uint8 ModeBytes[sizeof(int32)];
		FMemory::Memcpy(ModeBytes, &Case, sizeof(Case));
		FString Error;
		if (!TestTrue(TEXT("Set compiler fixture case"),
			Runtime.WriteStateBytes(ModeOffset, MakeArrayView(ModeBytes), Error)))
		{ AddError(Error); return false; }
		if (!TestTrue(TEXT("BeginPlay suspends"), Runtime.BeginPlay(Result)))
		{ AddError(Result.ErrorMessage); return false; }
		if (bCancel && !TestTrue(TEXT("Explicit cancellation request succeeds"), Runtime.Tick(-1.0f, Result)))
		{ AddError(Result.ErrorMessage); return false; }

		const bool bUnhandled = bCancel && (Case == 3 || Case == 6);
		const bool bVmTrap = Case == 7;
		int32 Failures = 0;
		int32 Cancelled = 0;
		int32 Resumes = 0;
		for (int32 Round = 0; Round < 12; ++Round)
		{
			// Even after cancellation, a handler can start an ordinary next-tick wait.
			World->Tick(LEVELTICK_All, 0.02f);
			++GFrameCounter;
			TArray<FAvidScriptContinuationCompletion> Ready;
			Owner->DrainReady(Ready);
			for (const auto& Completion : Ready)
			{
				if (Completion.Status == EAvidScriptContinuationStatus::Cancelled) ++Cancelled;
				const bool bDispatched = Runtime.DispatchContinuation(Completion, Result);
				if (!bDispatched)
				{
					++Failures;
					if (bUnhandled)
					{
						TestEqual(TEXT("Unhandled cancellation reports a language error"),
							Result.ErrorCategory, FString(TEXT("language_error_uncaught")));
						TestTrue(TEXT("Cancellation retains the original type and source span"),
							Result.ErrorMessage.Contains(ExpectedCancellationDiagnostic));
					}
					else if (bVmTrap)
						TestEqual(TEXT("Division trap remains a VM failure after a handled cancellation"), Result.ErrorCategory,
							Backend == EAvidScriptVmBackendKind::Wasmtime ? FString(TEXT("guest_trap")) : FString(TEXT("trap")));
					else AddError(Result.ErrorCategory + TEXT(": ") + Result.ErrorMessage);
				}
				TestTrue(TEXT("Compiler continuation finalizes"), Owner->FinalizeDispatched(Completion.Token, bDispatched));
				++Resumes;
			}
			if (Owner->GetActiveCount() == 0) break;
		}
		auto Read = [&](int32 Address) -> int32
		{
			uint8 Data[sizeof(int32)];
			if (!Runtime.ReadStateBytes(Address, MakeArrayView(Data), Error))
			{ AddError(Error); return MIN_int32; }
			int32 Value;
			FMemory::Memcpy(&Value, Data, sizeof(Value));
			return Value;
		};
		const int32 CancelResults[] = {18, 17, 20, 0, 21, 22, 0, 0};
		const int32 ExpectedResult = bCancel ? CancelResults[Case] : bVmTrap ? 0 : 16;
		TestEqual(TEXT("Same-source .NET result"), Read(ResultOffset), ExpectedResult);
		TestEqual(TEXT("Finally order and count"), Read(TraceOffset), Case == 5 ? 1 : 12);
		TestEqual(TEXT("First matching catch"), Read(InnerOffset), bCancel ? 1 : 0);
		TestEqual(TEXT("Rethrow reaches outer catch"), Read(OuterOffset), bCancel && (Case == 2 || Case == 4) ? 1 : 0);
		TestEqual(TEXT("Later or incompatible catch is skipped"), Read(WrongOffset), 0);
		TestEqual(TEXT("Only unhandled cancellation or the explicit VM trap fails"), Failures, bUnhandled || bVmTrap ? 1 : 0);
		TestEqual(TEXT("Cancelled Task state survives propagation"), Cancelled,
			bCancel ? (Case == 3 ? 3 : Case == 2 || Case == 6 ? 2 : 1) : 0);
		TestTrue(TEXT("Compiler callbacks actually executed"), Resumes >= 2);
		if (Case == 6) TestEqual(TEXT("Ready Task await does not schedule a sixth callback"), Resumes, 5);
		TestEqual(TEXT("Continuations retire before teardown"), Owner->GetActiveCount(), 0);
		TestEqual(TEXT("Tasks release before teardown"), Owner->GetTaskResultsForTesting().GetCount(), 0);
		const auto Stats = Runtime.GetManagedHeapForTesting()->GetStats();
		TestEqual(TEXT("Exception roots release before teardown"), Stats.LiveRoots, static_cast<uint32>(0));
		TestEqual(TEXT("VM call frames release"), Stats.ActiveFrames, static_cast<uint32>(0));
		if (!bUnhandled && !bVmTrap) TestTrue(TEXT("EndPlay releases cancellation source"), Runtime.EndPlay(Result));
		AddInfo(FString::Printf(TEXT("compiled cancellation backend=%d cancel=%d case=%d result=%d trace=%d resumes=%d cancelled=%d"),
			static_cast<int32>(Backend), bCancel ? 1 : 0, Case, Read(ResultOffset), Read(TraceOffset), Resumes, Cancelled));
		if (HasAnyErrors()) return false;
	}
	AddInfo(TEXT("CompiledAsyncCancellation: 32/32 passed"));
	return true;
}

#endif
