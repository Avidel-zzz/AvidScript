#include "AvidScriptWasmtimeTypedHostApi.h"

#include "AvidScriptWasmtimeApiInternal.h"

#include <stdlib.h>

typedef struct AvidScriptWasmtimeEmptyI32Bridge
{
	AvidScriptWasmtimeEmptyI32Callback callback;
	void* environment;
} AvidScriptWasmtimeEmptyI32Bridge;

typedef struct AvidScriptWasmtimeI32PairBridge
{
	AvidScriptWasmtimeI32PairCallback callback;
	void* environment;
} AvidScriptWasmtimeI32PairBridge;

static wasm_trap_t* avidscript_wasmtime_typed_bridge_unavailable(void)
{
	static const char message[] = "avidscript_typed_host_bridge_unavailable";
	return wasmtime_trap_new(message, sizeof(message) - 1);
}

static wasm_trap_t* avidscript_wasmtime_typed_host_failed(void)
{
	static const char message[] = "avidscript_host_import_failed";
	return wasmtime_trap_new(message, sizeof(message) - 1);
}

static wasm_trap_t* avidscript_wasmtime_empty_i32_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	const wasmtime_val_t* arguments,
	size_t argument_count,
	wasmtime_val_t* results,
	size_t result_count)
{
	AvidScriptWasmtimeEmptyI32Bridge* bridge =
		(AvidScriptWasmtimeEmptyI32Bridge*)environment;
	int32_t value = 0;
	(void)caller;
	(void)arguments;
	(void)argument_count;
	(void)result_count;
	if (bridge == NULL || bridge->callback == NULL)
	{
		return avidscript_wasmtime_typed_bridge_unavailable();
	}
	if (bridge->callback(bridge->environment, &value) != 0)
	{
		return avidscript_wasmtime_typed_host_failed();
	}
	results[0].kind = WASMTIME_I32;
	results[0].of.i32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_i32_pair_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	const wasmtime_val_t* arguments,
	size_t argument_count,
	wasmtime_val_t* results,
	size_t result_count)
{
	AvidScriptWasmtimeI32PairBridge* bridge =
		(AvidScriptWasmtimeI32PairBridge*)environment;
	int32_t value = 0;
	(void)caller;
	(void)argument_count;
	(void)result_count;
	if (bridge == NULL || bridge->callback == NULL)
	{
		return avidscript_wasmtime_typed_bridge_unavailable();
	}
	if (bridge->callback(
		bridge->environment,
		arguments[0].of.i32,
		arguments[1].of.i32,
		&value) != 0)
	{
		return avidscript_wasmtime_typed_host_failed();
	}
	results[0].kind = WASMTIME_I32;
	results[0].of.i32 = value;
	return NULL;
}

static void avidscript_wasmtime_typed_bridge_delete(void* environment)
{
	free(environment);
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_empty_i32(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeEmptyI32Callback callback,
	void* environment)
{
	wasm_valtype_vec_t parameters;
	wasm_valtype_vec_t results;
	wasm_functype_t* function_type;
	AvidScriptWasmtimeEmptyI32Bridge* bridge;
	wasmtime_error_t* error;
	wasm_valtype_vec_new_empty(&parameters);
	wasm_valtype_vec_new_uninitialized(&results, 1);
	results.data[0] = wasm_valtype_new_i32();
	function_type = wasm_functype_new(&parameters, &results);
	if (function_type == NULL)
	{
		return avidscript_wasmtime_local_failure(
			"Could not allocate the typed empty-i32 function type.");
	}
	bridge = (AvidScriptWasmtimeEmptyI32Bridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
	{
		wasm_functype_delete(function_type);
		return avidscript_wasmtime_local_failure(
			"Could not allocate the typed empty-i32 bridge.");
	}
	bridge->callback = callback;
	bridge->environment = environment;
	error = wasmtime_linker_define_func(
		linker->value,
		module_name,
		module_name_size,
		import_name,
		import_name_size,
		function_type,
		avidscript_wasmtime_empty_i32_trampoline,
		bridge,
		avidscript_wasmtime_typed_bridge_delete);
	wasm_functype_delete(function_type);
	if (error != NULL)
	{
		free(bridge);
		return avidscript_wasmtime_failure_new(error, NULL);
	}
	return NULL;
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_i32_pair(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeI32PairCallback callback,
	void* environment)
{
	wasm_valtype_vec_t parameters;
	wasm_valtype_vec_t results;
	wasm_functype_t* function_type;
	AvidScriptWasmtimeI32PairBridge* bridge;
	wasmtime_error_t* error;
	wasm_valtype_vec_new_uninitialized(&parameters, 2);
	parameters.data[0] = wasm_valtype_new_i32();
	parameters.data[1] = wasm_valtype_new_i32();
	wasm_valtype_vec_new_uninitialized(&results, 1);
	results.data[0] = wasm_valtype_new_i32();
	function_type = wasm_functype_new(&parameters, &results);
	if (function_type == NULL)
	{
		return avidscript_wasmtime_local_failure(
			"Could not allocate the typed i32-pair function type.");
	}
	bridge = (AvidScriptWasmtimeI32PairBridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
	{
		wasm_functype_delete(function_type);
		return avidscript_wasmtime_local_failure(
			"Could not allocate the typed i32-pair bridge.");
	}
	bridge->callback = callback;
	bridge->environment = environment;
	error = wasmtime_linker_define_func(
		linker->value,
		module_name,
		module_name_size,
		import_name,
		import_name_size,
		function_type,
		avidscript_wasmtime_i32_pair_trampoline,
		bridge,
		avidscript_wasmtime_typed_bridge_delete);
	wasm_functype_delete(function_type);
	if (error != NULL)
	{
		free(bridge);
		return avidscript_wasmtime_failure_new(error, NULL);
	}
	return NULL;
}
