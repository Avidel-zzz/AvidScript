#pragma once

#include "AvidScriptWasmtimeApi.h"

#include "wasmtime.h"

#define AVIDSCRIPT_WASMTIME_MAX_VALUES 64

struct AvidScriptWasmtimeEngine
{
	wasm_engine_t* value;
};

struct AvidScriptWasmtimeModule
{
	wasmtime_module_t* value;
};

struct AvidScriptWasmtimeStore
{
	wasmtime_store_t* value;
	wasmtime_context_t* context;
};

struct AvidScriptWasmtimeLinker
{
	wasmtime_linker_t* value;
};

struct AvidScriptWasmtimeInstance
{
	wasmtime_instance_t value;
};

struct AvidScriptWasmtimeFunction
{
	wasmtime_func_t value;
	uint32_t parameter_count;
	uint32_t cell_count;
	uint32_t result_count;
	uint32_t result_cell_count;
	wasmtime_valkind_t parameter_kinds[AVIDSCRIPT_WASMTIME_MAX_VALUES];
	wasmtime_valkind_t result_kinds[AVIDSCRIPT_WASMTIME_MAX_VALUES];
};

struct AvidScriptWasmtimeFailure
{
	wasmtime_error_t* error;
	wasm_trap_t* trap;
	wasm_name_t message;
	wasm_frame_vec_t frames;
};

AvidScriptWasmtimeFailure* avidscript_wasmtime_failure_new(
	wasmtime_error_t* error,
	wasm_trap_t* trap);

AvidScriptWasmtimeFailure* avidscript_wasmtime_local_failure(
	const char* message);
