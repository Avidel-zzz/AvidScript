#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCompiledDirectAwaitCleanupTest,
	"AvidScript.Runtime.Continuation.CompiledDirectAwaitCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCompiledDirectAwaitCleanupTest::RunTest(const FString& Parameters)
{
	if (!GEngine) return false;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false,
		TEXT("AvidScriptCompiledDirectAwaitCleanupWorld"));
	if (!TestNotNull(TEXT("Direct await world created"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	const FString Fixture = FPlatformMisc::GetEnvironmentVariable(
		TEXT("AVIDSCRIPT_DIRECT_AWAIT_WASM_PATH"));
	const FString ModuleId = FPlatformMisc::GetEnvironmentVariable(
		TEXT("AVIDSCRIPT_DIRECT_AWAIT_MODULE_ID"));
	auto ReadOffset = [&](const TCHAR* Name, int32& OutOffset) -> bool
	{
		const FString Text = FPlatformMisc::GetEnvironmentVariable(Name);
		return LexTryParseString(OutOffset, *Text)
			&& OutOffset >= 0 && OutOffset < 65536;
	};
	int32 ResultOffset = -1;
	int32 CleanupOffset = -1;
	int32 CatchOffset = -1;
	if (!TestFalse(TEXT("Direct await WASM path set"), Fixture.IsEmpty())
		|| !TestFalse(TEXT("Direct await module id set"), ModuleId.IsEmpty())
		|| !TestTrue(TEXT("Result offset valid"),
			ReadOffset(TEXT("AVIDSCRIPT_DIRECT_AWAIT_RESULT_OFFSET"), ResultOffset))
		|| !TestTrue(TEXT("Cleanup offset valid"),
			ReadOffset(TEXT("AVIDSCRIPT_DIRECT_AWAIT_CLEANUP_OFFSET"), CleanupOffset))
		|| !TestTrue(TEXT("Catch offset valid"),
			ReadOffset(TEXT("AVIDSCRIPT_DIRECT_AWAIT_CATCH_OFFSET"), CatchOffset)))
		return false;
	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("Direct await WASM exists"),
		FFileHelper::LoadFileToArray(Bytes, *Fixture))) return false;

	for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime,
		EAvidScriptVmBackendKind::Wamr})
	for (const bool bCancel : {false, true})
	{
		FAvidScriptVmBackendSelection Selection;
		Selection.BackendKind = Backend;
		Selection.ExecutionMode = Backend == EAvidScriptVmBackendKind::Wasmtime
			? EAvidScriptVmExecutionMode::Jit : EAvidScriptVmExecutionMode::Interpreter;
		FAvidScriptWasmRuntimeInstance Runtime(Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(TEXT("Direct await module loads"),
			Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), ModuleId, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		if (!TestTrue(TEXT("Direct await exports exist"),
			Runtime.ValidateRequiredExports({TEXT("avid_on_begin_play"),
				TEXT("avid_on_tick"), TEXT("avid_on_continuation_v2")}, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
		auto& Endpoint = Owner->ResetActive(World);
		FAvidScriptWasmHostContext Context;
		Context.Tasks = &Endpoint;
		Context.Continuations = &Endpoint;
		Context.World = World;
		Runtime.SetHostContext(Context);
		if (!TestTrue(TEXT("Direct await BeginPlay suspends"), Runtime.BeginPlay(Result)))
		{ AddError(Result.ErrorMessage); return false; }
		if (bCancel && !TestTrue(TEXT("Script requests active cancellation"),
			Runtime.Tick(-1.0f, Result)))
		{ AddError(Result.ErrorMessage); return false; }

		int32 Resumes = 0;
		int32 Cancelled = 0;
		bool bSawUncaughtOuterCancellation = false;
		for (int32 Round = 0; Round < 8; ++Round)
		{
			if (!bCancel)
			{
				World->Tick(LEVELTICK_All, 0.02f);
				++GFrameCounter;
			}
			TArray<FAvidScriptContinuationCompletion> Ready;
			Owner->DrainReady(Ready);
			for (const FAvidScriptContinuationCompletion& Completion : Ready)
			{
				if (Completion.Status == EAvidScriptContinuationStatus::Cancelled)
					++Cancelled;
				const bool bDispatched = Runtime.DispatchContinuation(Completion, Result);
				if (bCancel && Resumes == 0)
					TestTrue(TEXT("Direct cancellation cleanup callback succeeds"),
						bDispatched);
				if (!bDispatched)
				{
					TestTrue(TEXT("Only the uncaught outer await may trap"), bCancel);
					TestEqual(TEXT("Uncaught outer cancellation remains a VM failure"),
						Result.ErrorCategory, Backend == EAvidScriptVmBackendKind::Wasmtime
							? FString(TEXT("guest_trap")) : FString(TEXT("trap")));
					bSawUncaughtOuterCancellation = true;
				}
				TestTrue(TEXT("Direct await completion finalizes"),
					Owner->FinalizeDispatched(Completion.Token, bDispatched));
				++Resumes;
			}
			if (Owner->GetActiveCount() == 0) break;
		}
		auto ReadInt32 = [&](int32 Offset) -> int32
		{
			uint8 Data[sizeof(int32)] = {};
			FString Error;
			if (!Runtime.ReadStateBytes(Offset, MakeArrayView(Data), Error))
			{
				AddError(Error);
				return MIN_int32;
			}
			int32 Value = 0;
			FMemory::Memcpy(&Value, Data, sizeof(Value));
			return Value;
		};
		TestEqual(TEXT("Direct await normal result"),
			ReadInt32(ResultOffset), bCancel ? 0 : 16);
		TestEqual(TEXT("Direct await finally runs exactly once"),
			ReadInt32(CleanupOffset), 1);
		TestEqual(TEXT("Direct await cancellation skips catch"),
			ReadInt32(CatchOffset), 0);
		TestEqual(TEXT("Direct await cancellation status"),
			Cancelled > 0, bCancel);
		TestEqual(TEXT("Uncaught outer await is isolated"),
			bSawUncaughtOuterCancellation, bCancel);
		TestEqual(TEXT("Direct await continuations retire"),
			Owner->GetActiveCount(), 0);
		TestEqual(TEXT("Direct await tasks release"),
			Owner->GetTaskResultsForTesting().GetCount(), 0);
		Owner->Teardown();
		AvidScript::Managed::FHeap* Heap = Runtime.GetManagedHeapForTesting();
		TestEqual(TEXT("Direct await managed roots release"),
			Heap->GetStats().LiveRoots, static_cast<uint32>(0));
		AddInfo(FString::Printf(TEXT("compiled direct await backend=%d cancel=%d result=%d cleanup=%d catch=%d resumes=%d cancelled=%d"),
			static_cast<int32>(Backend), bCancel ? 1 : 0,
			ReadInt32(ResultOffset), ReadInt32(CleanupOffset), ReadInt32(CatchOffset),
			Resumes, Cancelled));
	}
	return true;
}

#endif
