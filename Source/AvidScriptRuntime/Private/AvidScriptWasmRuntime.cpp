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

void SetSmokeFailure(FAvidScriptWasmSmokeResult& OutResult, const FString& Message)
{
	OutResult.ErrorMessage = Message;
}

#if AVIDSCRIPT_WITH_WAMR
FString GetWamrException(wasm_module_inst_t ModuleInstance)
{
	if (ModuleInstance == nullptr)
	{
		return TEXT("No WAMR module instance is available.");
	}

	const char* Exception = wasm_runtime_get_exception(ModuleInstance);
	return Exception != nullptr ? UTF8_TO_TCHAR(Exception) : TEXT("WAMR did not report an exception.");
}

bool CallWamrExport(
	wasm_module_inst_t ModuleInstance,
	wasm_exec_env_t ExecEnv,
	const char* ExportName,
	uint32 ArgCount,
	uint32* Args,
	FAvidScriptWasmSmokeResult& OutResult)
{
	wasm_function_inst_t Function = wasm_runtime_lookup_function(ModuleInstance, ExportName);
	if (Function == nullptr)
	{
		SetSmokeFailure(OutResult, FString::Printf(TEXT("Missing WASM export: %S"), ExportName));
		return false;
	}

	if (!wasm_runtime_call_wasm(ExecEnv, Function, ArgCount, Args))
	{
		SetSmokeFailure(
			OutResult,
			FString::Printf(TEXT("WASM export %S failed: %s"), ExportName, *GetWamrException(ModuleInstance)));
		return false;
	}

	return true;
}
#endif
} // namespace

bool FAvidScriptWasmRuntime::RunEmbeddedSmokeTest(FAvidScriptWasmSmokeResult& OutResult)
{
	OutResult = FAvidScriptWasmSmokeResult();

#if !AVIDSCRIPT_WITH_WAMR
	SetSmokeFailure(OutResult, TEXT("WAMR backend is not available. Build WAMR and verify AVIDSCRIPT_WITH_WAMR=1."));
	return false;
#else
	char ErrorBuffer[AvidScriptWasmErrorBufferSize] = {};

	if (!wasm_runtime_init())
	{
		SetSmokeFailure(OutResult, TEXT("wasm_runtime_init failed."));
		return false;
	}

	OutResult.bRuntimeInitialized = true;

	wasm_module_t Module = nullptr;
	wasm_module_inst_t ModuleInstance = nullptr;
	wasm_exec_env_t ExecEnv = nullptr;

	auto Cleanup = [&]()
	{
		if (ExecEnv != nullptr)
		{
			wasm_runtime_destroy_exec_env(ExecEnv);
			ExecEnv = nullptr;
		}

		if (ModuleInstance != nullptr)
		{
			wasm_runtime_deinstantiate(ModuleInstance);
			ModuleInstance = nullptr;
		}

		if (Module != nullptr)
		{
			wasm_runtime_unload(Module);
			Module = nullptr;
		}

		wasm_runtime_destroy();
	};

	TArray<uint8> ModuleBuffer;
	ModuleBuffer.Append(GAvidScriptMinimalWasmModule, UE_ARRAY_COUNT(GAvidScriptMinimalWasmModule));

	Module = wasm_runtime_load(
		ModuleBuffer.GetData(),
		static_cast<uint32>(ModuleBuffer.Num()),
		ErrorBuffer,
		sizeof(ErrorBuffer));
	if (Module == nullptr)
	{
		SetSmokeFailure(OutResult, FString::Printf(TEXT("wasm_runtime_load failed: %S"), ErrorBuffer));
		Cleanup();
		return false;
	}

	OutResult.bModuleLoaded = true;

	ModuleInstance = wasm_runtime_instantiate(
		Module,
		AvidScriptWasmStackSize,
		AvidScriptWasmHeapSize,
		ErrorBuffer,
		sizeof(ErrorBuffer));
	if (ModuleInstance == nullptr)
	{
		SetSmokeFailure(OutResult, FString::Printf(TEXT("wasm_runtime_instantiate failed: %S"), ErrorBuffer));
		Cleanup();
		return false;
	}

	OutResult.bModuleInstantiated = true;

	ExecEnv = wasm_runtime_create_exec_env(ModuleInstance, AvidScriptWasmStackSize);
	if (ExecEnv == nullptr)
	{
		SetSmokeFailure(OutResult, TEXT("wasm_runtime_create_exec_env failed."));
		Cleanup();
		return false;
	}

	if (!CallWamrExport(ModuleInstance, ExecEnv, "avid_on_begin_play", 0, nullptr, OutResult))
	{
		Cleanup();
		return false;
	}

	OutResult.bBeginPlayCalled = true;

	const float DeltaSeconds = 1.0f / 60.0f;
	uint32 TickArgs[1] = {};
	static_assert(sizeof(TickArgs[0]) == sizeof(DeltaSeconds), "WAMR f32 argument must fit in one cell.");
	FMemory::Memcpy(&TickArgs[0], &DeltaSeconds, sizeof(DeltaSeconds));

	if (!CallWamrExport(ModuleInstance, ExecEnv, "avid_on_tick", UE_ARRAY_COUNT(TickArgs), TickArgs, OutResult))
	{
		Cleanup();
		return false;
	}

	OutResult.bTickCalled = true;

	Cleanup();
	return true;
#endif
}


