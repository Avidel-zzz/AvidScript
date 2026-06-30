#include "AvidScriptWasmRuntime.h"

#ifndef AVIDSCRIPT_WITH_WAMR
#define AVIDSCRIPT_WITH_WAMR 0
#endif

#if AVIDSCRIPT_WITH_WAMR
extern "C"
{
#include "wasm_export.h"
}
#endif

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWasmRuntime, Log, All);

namespace
{
constexpr uint32 AvidScriptWasmStackSize = 64 * 1024;
constexpr uint32 AvidScriptWasmHeapSize = 64 * 1024;
constexpr uint32 AvidScriptWasmErrorBufferSize = 512;

const uint8 GAvidScriptMinimalWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x07,
	0x02, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

#if AVIDSCRIPT_WITH_WAMR
FCriticalSection GWamrRuntimeCriticalSection;
int32 GWamrRuntimeRefCount = 0;
#endif

void PrepareResult(FAvidScriptWasmSmokeResult& OutResult, const FString& ModuleId)
{
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = ModuleId;
}

void SetFailure(
	FAvidScriptWasmSmokeResult& OutResult,
	const FString& ModuleId,
	const FString& ExportName,
	const FString& Category,
	const FString& Details,
	const FString& NextAction)
{
	OutResult.ModuleId = ModuleId;
	OutResult.ExportName = ExportName;
	OutResult.ErrorCategory = Category;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript WAMR error | backend=WAMR | module=%s | export=%s | category=%s | details=%s | next=%s"),
		ModuleId.IsEmpty() ? TEXT("<none>") : *ModuleId,
		ExportName.IsEmpty() ? TEXT("<none>") : *ExportName,
		*Category,
		*Details,
		*NextAction);

	UE_LOG(LogAvidScriptWasmRuntime, Warning, TEXT("%s"), *OutResult.ErrorMessage);
}

#if AVIDSCRIPT_WITH_WAMR
bool AcquireWamrRuntime(const FString& ModuleId, FAvidScriptWasmSmokeResult& OutResult)
{
	FScopeLock Lock(&GWamrRuntimeCriticalSection);

	if (GWamrRuntimeRefCount == 0 && !wasm_runtime_init())
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<runtime>"),
			TEXT("runtime_init_failed"),
			TEXT("wasm_runtime_init returned false"),
			TEXT("verify WAMR build artifacts and platform initialization"));
		return false;
	}

	++GWamrRuntimeRefCount;
	return true;
}

void ReleaseWamrRuntime()
{
	FScopeLock Lock(&GWamrRuntimeCriticalSection);

	if (GWamrRuntimeRefCount <= 0)
	{
		GWamrRuntimeRefCount = 0;
		return;
	}

	--GWamrRuntimeRefCount;
	if (GWamrRuntimeRefCount == 0)
	{
		wasm_runtime_destroy();
	}
}

FString GetWamrException(wasm_module_inst_t InModuleInstance)
{
	if (InModuleInstance == nullptr)
	{
		return TEXT("No WAMR module instance is available.");
	}

	const char* Exception = wasm_runtime_get_exception(InModuleInstance);
	return Exception != nullptr ? UTF8_TO_TCHAR(Exception) : TEXT("WAMR did not report an exception.");
}

bool CallWamrExport(
	wasm_module_inst_t InModuleInstance,
	wasm_exec_env_t InExecEnv,
	const FString& ModuleId,
	const char* ExportName,
	uint32 ArgCount,
	uint32* Args,
	FAvidScriptWasmSmokeResult& OutResult)
{
	const FString ExportNameText(UTF8_TO_TCHAR(ExportName));
	wasm_function_inst_t Function = wasm_runtime_lookup_function(InModuleInstance, ExportName);
	if (Function == nullptr)
	{
		SetFailure(
			OutResult,
			ModuleId,
			ExportNameText,
			TEXT("missing_export"),
			FString::Printf(TEXT("Required export '%s' was not found"), *ExportNameText),
			TEXT("skip this script instance and report the guest ABI mismatch"));
		return false;
	}

	if (!wasm_runtime_call_wasm(InExecEnv, Function, ArgCount, Args))
	{
		SetFailure(
			OutResult,
			ModuleId,
			ExportNameText,
			TEXT("trap"),
			GetWamrException(InModuleInstance),
			TEXT("stop ticking this script instance and surface the trap to UE logs"));
		return false;
	}

	return true;
}
#endif
} // namespace

FAvidScriptWasmRuntimeInstance::~FAvidScriptWasmRuntimeInstance()
{
	Unload();
}

bool FAvidScriptWasmRuntimeInstance::LoadEmbeddedSmokeModule(FAvidScriptWasmSmokeResult& OutResult)
{
	return LoadModule(
		GAvidScriptMinimalWasmModule,
		UE_ARRAY_COUNT(GAvidScriptMinimalWasmModule),
		TEXT("embedded_smoke"),
		OutResult);
}

bool FAvidScriptWasmRuntimeInstance::LoadModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FString& InModuleId,
	FAvidScriptWasmSmokeResult& OutResult)
{
	Unload();
	ModuleId = InModuleId;
	PrepareResult(OutResult, ModuleId);

#if !AVIDSCRIPT_WITH_WAMR
	SetFailure(
		OutResult,
		ModuleId,
		TEXT("<runtime>"),
		TEXT("backend_unavailable"),
		TEXT("WAMR backend is not available. Build WAMR and verify AVIDSCRIPT_WITH_WAMR=1."),
		TEXT("build the ThirdParty WAMR static library before running scripts"));
	return false;
#else
	if (Bytecode == nullptr || BytecodeSize <= 0)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<module>"),
			TEXT("invalid_bytecode"),
			TEXT("No WASM bytecode was provided"),
			TEXT("provide a non-empty WASM module buffer"));
		return false;
	}

	if (!AcquireWamrRuntime(ModuleId, OutResult))
	{
		return false;
	}

	bOwnsRuntimeLease = true;
	OutResult.bRuntimeInitialized = true;

	ModuleBuffer.Reset(BytecodeSize);
	ModuleBuffer.Append(Bytecode, BytecodeSize);

	char ErrorBuffer[AvidScriptWasmErrorBufferSize] = {};

	Module = wasm_runtime_load(ModuleBuffer.GetData(), static_cast<uint32>(ModuleBuffer.Num()), ErrorBuffer, sizeof(ErrorBuffer));
	if (Module == nullptr)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<module>"),
			TEXT("load_failed"),
			UTF8_TO_TCHAR(ErrorBuffer),
			TEXT("reject this script module and keep the runtime alive"));
		Unload();
		return false;
	}

	OutResult.bModuleLoaded = true;

	ModuleInstance = wasm_runtime_instantiate(
		static_cast<wasm_module_t>(Module),
		AvidScriptWasmStackSize,
		AvidScriptWasmHeapSize,
		ErrorBuffer,
		sizeof(ErrorBuffer));
	if (ModuleInstance == nullptr)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<module>"),
			TEXT("instantiate_failed"),
			UTF8_TO_TCHAR(ErrorBuffer),
			TEXT("reject this script module and inspect stack/heap limits"));
		Unload();
		return false;
	}

	OutResult.bModuleInstantiated = true;

	ExecEnv = wasm_runtime_create_exec_env(static_cast<wasm_module_inst_t>(ModuleInstance), AvidScriptWasmStackSize);
	if (ExecEnv == nullptr)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<exec_env>"),
			TEXT("exec_env_failed"),
			TEXT("wasm_runtime_create_exec_env returned null"),
			TEXT("reject this script module and inspect stack size"));
		Unload();
		return false;
	}

	return true;
#endif
}

bool FAvidScriptWasmRuntimeInstance::BeginPlay(FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId);
	OutResult.bRuntimeInitialized = bOwnsRuntimeLease;
	OutResult.bModuleLoaded = Module != nullptr;
	OutResult.bModuleInstantiated = ModuleInstance != nullptr;

#if !AVIDSCRIPT_WITH_WAMR
	SetFailure(
		OutResult,
		ModuleId,
		TEXT("avid_on_begin_play"),
		TEXT("backend_unavailable"),
		TEXT("WAMR backend is not available"),
		TEXT("build the ThirdParty WAMR static library before running scripts"));
	return false;
#else
	if (!IsLoaded())
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_begin_play"),
			TEXT("invalid_state"),
			TEXT("No WASM module is loaded"),
			TEXT("load a module before calling BeginPlay"));
		return false;
	}

	if (!CallWamrExport(
		static_cast<wasm_module_inst_t>(ModuleInstance),
		static_cast<wasm_exec_env_t>(ExecEnv),
		ModuleId,
		"avid_on_begin_play",
		0,
		nullptr,
		OutResult))
	{
		return false;
	}

	bHasBegunPlay = true;
	OutResult.bBeginPlayCalled = true;
	OutResult.TickCallCount = TickCallCount;
	return true;
#endif
}

bool FAvidScriptWasmRuntimeInstance::Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId);
	OutResult.bRuntimeInitialized = bOwnsRuntimeLease;
	OutResult.bModuleLoaded = Module != nullptr;
	OutResult.bModuleInstantiated = ModuleInstance != nullptr;
	OutResult.bBeginPlayCalled = bHasBegunPlay;

#if !AVIDSCRIPT_WITH_WAMR
	SetFailure(
		OutResult,
		ModuleId,
		TEXT("avid_on_tick"),
		TEXT("backend_unavailable"),
		TEXT("WAMR backend is not available"),
		TEXT("build the ThirdParty WAMR static library before running scripts"));
	return false;
#else
	if (!IsLoaded())
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_tick"),
			TEXT("invalid_state"),
			TEXT("No WASM module is loaded"),
			TEXT("load a module before ticking"));
		return false;
	}

	uint32 TickArgs[1] = {};
	static_assert(sizeof(TickArgs[0]) == sizeof(DeltaSeconds), "WAMR f32 argument must fit in one cell.");
	FMemory::Memcpy(&TickArgs[0], &DeltaSeconds, sizeof(DeltaSeconds));

	if (!CallWamrExport(
		static_cast<wasm_module_inst_t>(ModuleInstance),
		static_cast<wasm_exec_env_t>(ExecEnv),
		ModuleId,
		"avid_on_tick",
		UE_ARRAY_COUNT(TickArgs),
		TickArgs,
		OutResult))
	{
		return false;
	}

	++TickCallCount;
	OutResult.bTickCalled = true;
	OutResult.TickCallCount = TickCallCount;
	return true;
#endif
}

void FAvidScriptWasmRuntimeInstance::Unload()
{
#if AVIDSCRIPT_WITH_WAMR
	if (ExecEnv != nullptr)
	{
		wasm_runtime_destroy_exec_env(static_cast<wasm_exec_env_t>(ExecEnv));
		ExecEnv = nullptr;
	}

	if (ModuleInstance != nullptr)
	{
		wasm_runtime_deinstantiate(static_cast<wasm_module_inst_t>(ModuleInstance));
		ModuleInstance = nullptr;
	}

	if (Module != nullptr)
	{
		wasm_runtime_unload(static_cast<wasm_module_t>(Module));
		Module = nullptr;
	}

	if (bOwnsRuntimeLease)
	{
		ReleaseWamrRuntime();
		bOwnsRuntimeLease = false;
	}
#endif

	Module = nullptr;
	ModuleInstance = nullptr;
	ExecEnv = nullptr;
	ModuleBuffer.Empty();
	ModuleId.Empty();
	bHasBegunPlay = false;
	TickCallCount = 0;
}

bool FAvidScriptWasmRuntimeInstance::IsLoaded() const
{
	return Module != nullptr && ModuleInstance != nullptr && ExecEnv != nullptr;
}

bool FAvidScriptWasmRuntime::RunEmbeddedSmokeTest(FAvidScriptWasmSmokeResult& OutResult)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	if (!Runtime.LoadEmbeddedSmokeModule(OutResult))
	{
		return false;
	}

	if (!Runtime.BeginPlay(OutResult))
	{
		return false;
	}

	if (!Runtime.Tick(1.0f / 60.0f, OutResult))
	{
		return false;
	}

	OutResult.bUnloaded = true;
	Runtime.Unload();
	return true;
}
