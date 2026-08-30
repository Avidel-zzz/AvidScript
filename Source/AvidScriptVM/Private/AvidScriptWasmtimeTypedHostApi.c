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

typedef struct AvidScriptWasmtimeSelfI32PairBridge
{
	AvidScriptWasmtimeSelfI32PairCallback callback;
	void* environment;
} AvidScriptWasmtimeSelfI32PairBridge;

typedef struct AvidScriptWasmtimeSelfI32PairGuestResultBridge
{
	AvidScriptWasmtimeSelfI32PairGuestResultCallback callback;
	void* environment;
} AvidScriptWasmtimeSelfI32PairGuestResultBridge;

typedef struct AvidScriptWasmtimeSelfF32TripleGuestVectorBridge
{
	AvidScriptWasmtimeSelfF32TripleGuestVectorCallback callback;
	void* environment;
} AvidScriptWasmtimeSelfF32TripleGuestVectorBridge;

typedef struct AvidScriptWasmtimeSelfPropertyI32GetBridge
{
	AvidScriptWasmtimeSelfPropertyI32GetCallback callback;
	void* environment;
} AvidScriptWasmtimeSelfPropertyI32GetBridge;

typedef struct AvidScriptWasmtimeSelfPropertyI32SetBridge
{
	AvidScriptWasmtimeSelfPropertyI32SetCallback callback;
	void* environment;
} AvidScriptWasmtimeSelfPropertyI32SetBridge;

typedef struct AvidScriptWasmtimeSelfPropertyF32GetBridge
{
	AvidScriptWasmtimeSelfPropertyF32GetCallback callback;
	void* environment;
} AvidScriptWasmtimeSelfPropertyF32GetBridge;

typedef struct AvidScriptWasmtimeSelfPropertyF32SetBridge
{
	AvidScriptWasmtimeSelfPropertyF32SetCallback callback;
	void* environment;
} AvidScriptWasmtimeSelfPropertyF32SetBridge;

typedef struct AvidScriptWasmtimePackedSelfPropertyI32GetBridge
{
	AvidScriptWasmtimePackedSelfPropertyI32GetCallback callback;
	void* environment;
} AvidScriptWasmtimePackedSelfPropertyI32GetBridge;

typedef struct AvidScriptWasmtimePackedSelfPropertyI32SetBridge
{
	AvidScriptWasmtimePackedSelfPropertyI32SetCallback callback;
	void* environment;
} AvidScriptWasmtimePackedSelfPropertyI32SetBridge;

typedef struct AvidScriptWasmtimePackedSelfPropertyI64GetBridge
{
	AvidScriptWasmtimePackedSelfPropertyI64GetCallback callback;
	void* environment;
} AvidScriptWasmtimePackedSelfPropertyI64GetBridge;

typedef struct AvidScriptWasmtimePackedSelfPropertyI64SetBridge
{
	AvidScriptWasmtimePackedSelfPropertyI64SetCallback callback;
	void* environment;
} AvidScriptWasmtimePackedSelfPropertyI64SetBridge;

typedef struct AvidScriptWasmtimePackedSelfPropertyF64GetBridge
{
	AvidScriptWasmtimePackedSelfPropertyF64GetCallback callback;
	void* environment;
} AvidScriptWasmtimePackedSelfPropertyF64GetBridge;

typedef struct AvidScriptWasmtimePackedSelfPropertyF64SetBridge
{
	AvidScriptWasmtimePackedSelfPropertyF64SetCallback callback;
	void* environment;
} AvidScriptWasmtimePackedSelfPropertyF64SetBridge;

typedef struct AvidScriptWasmtimeSelfGuestAddressBridge
{
	AvidScriptWasmtimeSelfGuestAddressCallback callback;
	void* environment;
} AvidScriptWasmtimeSelfGuestAddressBridge;

typedef struct AvidScriptWasmtimeStableObjectRoundtripBridge
{
	AvidScriptWasmtimeStableObjectRoundtripCallback callback;
	void* environment;
} AvidScriptWasmtimeStableObjectRoundtripBridge;

typedef char avidscript_wasmtime_raw_value_must_be_16_bytes[
	(sizeof(wasmtime_val_raw_t) == 16) ? 1 : -1];

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

static wasm_trap_t* avidscript_wasmtime_typed_raw_arity_invalid(void)
{
	static const char message[] = "avidscript_typed_host_raw_arity_invalid";
	return wasmtime_trap_new(message, sizeof(message) - 1);
}

static wasm_trap_t* avidscript_wasmtime_empty_i32_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeEmptyI32Bridge* bridge = (AvidScriptWasmtimeEmptyI32Bridge*)environment;
	int32_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 1)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_i32_pair_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeI32PairBridge* bridge = (AvidScriptWasmtimeI32PairBridge*)environment;
	int32_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 2)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i32, args_and_results[1].i32, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_self_i32_pair_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeSelfI32PairBridge* bridge = (AvidScriptWasmtimeSelfI32PairBridge*)environment;
	int32_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 4)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i32, args_and_results[1].i32, args_and_results[2].i32, args_and_results[3].i32, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_self_i32_pair_guest_result_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeSelfI32PairGuestResultBridge* bridge =
		(AvidScriptWasmtimeSelfI32PairGuestResultBridge*)environment;
	int32_t status = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 5)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(
			bridge->environment,
			args_and_results[0].i32,
			args_and_results[1].i32,
			args_and_results[2].i32,
			args_and_results[3].i32,
			args_and_results[4].i32,
			&status) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = status;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_self_f32_triple_guest_vector_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeSelfF32TripleGuestVectorBridge* bridge =
		(AvidScriptWasmtimeSelfF32TripleGuestVectorBridge*)environment;
	int32_t status = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 6)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(
			bridge->environment,
			args_and_results[0].i32,
			args_and_results[1].i32,
			args_and_results[2].f32,
			args_and_results[3].f32,
			args_and_results[4].f32,
			args_and_results[5].i32,
			&status) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = status;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_self_property_i32_get_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeSelfPropertyI32GetBridge* bridge = (AvidScriptWasmtimeSelfPropertyI32GetBridge*)environment;
	int32_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 2)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i32, args_and_results[1].i32, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_self_property_i32_set_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeSelfPropertyI32SetBridge* bridge = (AvidScriptWasmtimeSelfPropertyI32SetBridge*)environment;
	int32_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 3)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i32, args_and_results[1].i32, args_and_results[2].i32, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_self_property_i32_get_set_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeSelfGuestAddressBridge* bridge = (AvidScriptWasmtimeSelfGuestAddressBridge*)environment;
	int32_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 3)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i32, args_and_results[1].i32, args_and_results[2].i32, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_self_vector_value_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeSelfGuestAddressBridge* bridge = (AvidScriptWasmtimeSelfGuestAddressBridge*)environment;
	int32_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 3)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i32, args_and_results[1].i32, args_and_results[2].i32, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_stable_object_roundtrip_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeStableObjectRoundtripBridge* bridge = (AvidScriptWasmtimeStableObjectRoundtripBridge*)environment;
	int32_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 5)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i32, args_and_results[1].i32, args_and_results[2].i32, args_and_results[3].i32, args_and_results[4].i32, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_command_buffer_submit_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeI32PairBridge* bridge = (AvidScriptWasmtimeI32PairBridge*)environment;
	int32_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 2)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i32, args_and_results[1].i32, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = value;
	return NULL;
}

static void avidscript_wasmtime_typed_bridge_delete(void* environment)
{
	free(environment);
}

static AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_typed_i32(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	size_t parameter_count,
	wasmtime_func_unchecked_callback_t trampoline,
	void* bridge,
	const char* allocation_failure)
{
	wasm_valtype_vec_t parameters;
	wasm_valtype_vec_t results;
	wasm_functype_t* function_type;
	wasmtime_error_t* error;
	wasm_valtype_vec_new_uninitialized(&parameters, parameter_count);
	for (size_t index = 0; index < parameter_count; ++index)
		parameters.data[index] = wasm_valtype_new_i32();
	wasm_valtype_vec_new_uninitialized(&results, 1);
	results.data[0] = wasm_valtype_new_i32();
	function_type = wasm_functype_new(&parameters, &results);
	if (function_type == NULL)
	{
		free(bridge);
		return avidscript_wasmtime_local_failure(allocation_failure);
	}
	error = wasmtime_linker_define_func_unchecked(
		linker->value,
		module_name,
		module_name_size,
		import_name,
		import_name_size,
		function_type,
		trampoline,
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

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_empty_i32(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeEmptyI32Callback callback,
	void* environment)
{
	AvidScriptWasmtimeEmptyI32Bridge* bridge = (AvidScriptWasmtimeEmptyI32Bridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure("Could not allocate the typed empty-i32 bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	return avidscript_wasmtime_linker_define_typed_i32(linker, module_name, module_name_size, import_name, import_name_size, 0, avidscript_wasmtime_empty_i32_trampoline, bridge, "Could not allocate the typed empty-i32 function type.");
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
	AvidScriptWasmtimeI32PairBridge* bridge = (AvidScriptWasmtimeI32PairBridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure("Could not allocate the typed i32-pair bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	return avidscript_wasmtime_linker_define_typed_i32(linker, module_name, module_name_size, import_name, import_name_size, 2, avidscript_wasmtime_i32_pair_trampoline, bridge, "Could not allocate the typed i32-pair function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_i32_pair(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfI32PairCallback callback,
	void* environment)
{
	AvidScriptWasmtimeSelfI32PairBridge* bridge = (AvidScriptWasmtimeSelfI32PairBridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure("Could not allocate the typed self-i32-pair bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	return avidscript_wasmtime_linker_define_typed_i32(linker, module_name, module_name_size, import_name, import_name_size, 4, avidscript_wasmtime_self_i32_pair_trampoline, bridge, "Could not allocate the typed self-i32-pair function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_i32_pair_guest_result(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfI32PairGuestResultCallback callback,
	void* environment)
{
	AvidScriptWasmtimeSelfI32PairGuestResultBridge* bridge =
		(AvidScriptWasmtimeSelfI32PairGuestResultBridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure("Could not allocate the typed self-i32-pair guest-result bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	return avidscript_wasmtime_linker_define_typed_i32(
		linker,
		module_name,
		module_name_size,
		import_name,
		import_name_size,
		5,
		avidscript_wasmtime_self_i32_pair_guest_result_trampoline,
		bridge,
		"Could not allocate the typed self-i32-pair guest-result function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_f32_triple_guest_vector(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfF32TripleGuestVectorCallback callback,
	void* environment)
{
	AvidScriptWasmtimeSelfF32TripleGuestVectorBridge* bridge =
		(AvidScriptWasmtimeSelfF32TripleGuestVectorBridge*)calloc(
			1,
			sizeof(*bridge));
	wasm_valtype_vec_t parameters;
	wasm_valtype_vec_t results;
	wasm_functype_t* function_type;
	wasmtime_error_t* error;
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure(
			"Could not allocate the typed self-f32-triple guest-vector bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	wasm_valtype_vec_new_uninitialized(&parameters, 6);
	parameters.data[0] = wasm_valtype_new_i32();
	parameters.data[1] = wasm_valtype_new_i32();
	parameters.data[2] = wasm_valtype_new_f32();
	parameters.data[3] = wasm_valtype_new_f32();
	parameters.data[4] = wasm_valtype_new_f32();
	parameters.data[5] = wasm_valtype_new_i32();
	wasm_valtype_vec_new_uninitialized(&results, 1);
	results.data[0] = wasm_valtype_new_i32();
	function_type = wasm_functype_new(&parameters, &results);
	if (function_type == NULL)
	{
		free(bridge);
		return avidscript_wasmtime_local_failure(
			"Could not allocate the typed self-f32-triple guest-vector function type.");
	}
	error = wasmtime_linker_define_func_unchecked(
		linker->value,
		module_name,
		module_name_size,
		import_name,
		import_name_size,
		function_type,
		avidscript_wasmtime_self_f32_triple_guest_vector_trampoline,
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

static wasm_trap_t* avidscript_wasmtime_self_property_f32_get_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeSelfPropertyF32GetBridge* bridge =
		(AvidScriptWasmtimeSelfPropertyF32GetBridge*)environment;
	float value = 0.0f;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 1)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(
			bridge->environment,
			args_and_results[0].i64,
			&value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].f32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_self_property_f32_set_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimeSelfPropertyF32SetBridge* bridge =
		(AvidScriptWasmtimeSelfPropertyF32SetBridge*)environment;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 2)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(
			bridge->environment,
			args_and_results[0].i64,
			args_and_results[1].f32) != 0)
		return avidscript_wasmtime_typed_host_failed();
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_packed_self_property_i32_get_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimePackedSelfPropertyI32GetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyI32GetBridge*)environment;
	int32_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 1)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i64, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i32 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_packed_self_property_i32_set_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimePackedSelfPropertyI32SetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyI32SetBridge*)environment;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 2)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(
			bridge->environment,
			args_and_results[0].i64,
			args_and_results[1].i32) != 0)
		return avidscript_wasmtime_typed_host_failed();
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_packed_self_property_i64_get_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimePackedSelfPropertyI64GetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyI64GetBridge*)environment;
	int64_t value = 0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 1)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i64, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].i64 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_packed_self_property_i64_set_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimePackedSelfPropertyI64SetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyI64SetBridge*)environment;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 2)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(
			bridge->environment,
			args_and_results[0].i64,
			args_and_results[1].i64) != 0)
		return avidscript_wasmtime_typed_host_failed();
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_packed_self_property_f64_get_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimePackedSelfPropertyF64GetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyF64GetBridge*)environment;
	double value = 0.0;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 1)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(bridge->environment, args_and_results[0].i64, &value) != 0)
		return avidscript_wasmtime_typed_host_failed();
	args_and_results[0].f64 = value;
	return NULL;
}

static wasm_trap_t* avidscript_wasmtime_packed_self_property_f64_set_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	wasmtime_val_raw_t* args_and_results,
	size_t count)
{
	AvidScriptWasmtimePackedSelfPropertyF64SetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyF64SetBridge*)environment;
	(void)caller;
	if (bridge == NULL || bridge->callback == NULL || args_and_results == NULL)
		return avidscript_wasmtime_typed_bridge_unavailable();
	if (count != 2)
		return avidscript_wasmtime_typed_raw_arity_invalid();
	if (bridge->callback(
			bridge->environment,
			args_and_results[0].i64,
			args_and_results[1].f64) != 0)
		return avidscript_wasmtime_typed_host_failed();
	return NULL;
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_property_i32_get(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfPropertyI32GetCallback callback,
	void* environment)
{
	AvidScriptWasmtimeSelfPropertyI32GetBridge* bridge = (AvidScriptWasmtimeSelfPropertyI32GetBridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure("Could not allocate the typed self-property-i32-get bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	return avidscript_wasmtime_linker_define_typed_i32(linker, module_name, module_name_size, import_name, import_name_size, 2, avidscript_wasmtime_self_property_i32_get_trampoline, bridge, "Could not allocate the typed self-property-i32-get function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_property_i32_set(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfPropertyI32SetCallback callback,
	void* environment)
{
	AvidScriptWasmtimeSelfPropertyI32SetBridge* bridge = (AvidScriptWasmtimeSelfPropertyI32SetBridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure("Could not allocate the typed self-property-i32-set bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	return avidscript_wasmtime_linker_define_typed_i32(linker, module_name, module_name_size, import_name, import_name_size, 3, avidscript_wasmtime_self_property_i32_set_trampoline, bridge, "Could not allocate the typed self-property-i32-set function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_property_f32_get(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfPropertyF32GetCallback callback,
	void* environment)
{
	AvidScriptWasmtimeSelfPropertyF32GetBridge* bridge =
		(AvidScriptWasmtimeSelfPropertyF32GetBridge*)calloc(1, sizeof(*bridge));
	wasm_valtype_vec_t parameters;
	wasm_valtype_vec_t results;
	wasm_functype_t* function_type;
	wasmtime_error_t* error;
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure(
			"Could not allocate the typed self-property-f32-get bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	wasm_valtype_vec_new_uninitialized(&parameters, 1);
	parameters.data[0] = wasm_valtype_new_i64();
	wasm_valtype_vec_new_uninitialized(&results, 1);
	results.data[0] = wasm_valtype_new_f32();
	function_type = wasm_functype_new(&parameters, &results);
	if (function_type == NULL)
	{
		free(bridge);
		return avidscript_wasmtime_local_failure(
			"Could not allocate the typed self-property-f32-get function type.");
	}
	error = wasmtime_linker_define_func_unchecked(
		linker->value,
		module_name,
		module_name_size,
		import_name,
		import_name_size,
		function_type,
		avidscript_wasmtime_self_property_f32_get_trampoline,
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

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_property_f32_set(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfPropertyF32SetCallback callback,
	void* environment)
{
	AvidScriptWasmtimeSelfPropertyF32SetBridge* bridge =
		(AvidScriptWasmtimeSelfPropertyF32SetBridge*)calloc(1, sizeof(*bridge));
	wasm_valtype_vec_t parameters;
	wasm_valtype_vec_t results;
	wasm_functype_t* function_type;
	wasmtime_error_t* error;
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure(
			"Could not allocate the typed self-property-f32-set bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	wasm_valtype_vec_new_uninitialized(&parameters, 2);
	parameters.data[0] = wasm_valtype_new_i64();
	parameters.data[1] = wasm_valtype_new_f32();
	wasm_valtype_vec_new_empty(&results);
	function_type = wasm_functype_new(&parameters, &results);
	if (function_type == NULL)
	{
		free(bridge);
		return avidscript_wasmtime_local_failure(
			"Could not allocate the typed self-property-f32-set function type.");
	}
	error = wasmtime_linker_define_func_unchecked(
		linker->value,
		module_name,
		module_name_size,
		import_name,
		import_name_size,
		function_type,
		avidscript_wasmtime_self_property_f32_set_trampoline,
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

static AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_packed_property_get(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	void* bridge,
	wasm_valtype_t* result_type,
	wasmtime_func_unchecked_callback_t trampoline,
	const char* allocation_failure)
{
	wasm_valtype_vec_t parameters;
	wasm_valtype_vec_t results;
	wasm_functype_t* function_type;
	wasmtime_error_t* error;
	if (bridge == NULL || result_type == NULL)
	{
		free(bridge);
		if (result_type != NULL)
			wasm_valtype_delete(result_type);
		return avidscript_wasmtime_local_failure(allocation_failure);
	}
	wasm_valtype_vec_new_uninitialized(&parameters, 1);
	parameters.data[0] = wasm_valtype_new_i64();
	wasm_valtype_vec_new_uninitialized(&results, 1);
	results.data[0] = result_type;
	function_type = wasm_functype_new(&parameters, &results);
	if (function_type == NULL)
	{
		free(bridge);
		return avidscript_wasmtime_local_failure(allocation_failure);
	}
	error = wasmtime_linker_define_func_unchecked(
		linker->value,
		module_name,
		module_name_size,
		import_name,
		import_name_size,
		function_type,
		trampoline,
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

static AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_packed_property_set(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	void* bridge,
	wasm_valtype_t* value_type,
	wasmtime_func_unchecked_callback_t trampoline,
	const char* allocation_failure)
{
	wasm_valtype_vec_t parameters;
	wasm_valtype_vec_t results;
	wasm_functype_t* function_type;
	wasmtime_error_t* error;
	if (bridge == NULL || value_type == NULL)
	{
		free(bridge);
		if (value_type != NULL)
			wasm_valtype_delete(value_type);
		return avidscript_wasmtime_local_failure(allocation_failure);
	}
	wasm_valtype_vec_new_uninitialized(&parameters, 2);
	parameters.data[0] = wasm_valtype_new_i64();
	parameters.data[1] = value_type;
	wasm_valtype_vec_new_empty(&results);
	function_type = wasm_functype_new(&parameters, &results);
	if (function_type == NULL)
	{
		free(bridge);
		return avidscript_wasmtime_local_failure(allocation_failure);
	}
	error = wasmtime_linker_define_func_unchecked(
		linker->value,
		module_name,
		module_name_size,
		import_name,
		import_name_size,
		function_type,
		trampoline,
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

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_packed_self_property_i32_get(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimePackedSelfPropertyI32GetCallback callback,
	void* environment)
{
	AvidScriptWasmtimePackedSelfPropertyI32GetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyI32GetBridge*)calloc(1, sizeof(*bridge));
	if (bridge != NULL)
	{
		bridge->callback = callback;
		bridge->environment = environment;
	}
	return avidscript_wasmtime_linker_define_packed_property_get(
		linker, module_name, module_name_size, import_name, import_name_size,
		bridge, wasm_valtype_new_i32(),
		avidscript_wasmtime_packed_self_property_i32_get_trampoline,
		"Could not allocate the packed self-property-i32-get bridge or function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_packed_self_property_i32_set(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimePackedSelfPropertyI32SetCallback callback,
	void* environment)
{
	AvidScriptWasmtimePackedSelfPropertyI32SetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyI32SetBridge*)calloc(1, sizeof(*bridge));
	if (bridge != NULL)
	{
		bridge->callback = callback;
		bridge->environment = environment;
	}
	return avidscript_wasmtime_linker_define_packed_property_set(
		linker, module_name, module_name_size, import_name, import_name_size,
		bridge, wasm_valtype_new_i32(),
		avidscript_wasmtime_packed_self_property_i32_set_trampoline,
		"Could not allocate the packed self-property-i32-set bridge or function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_packed_self_property_i64_get(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimePackedSelfPropertyI64GetCallback callback,
	void* environment)
{
	AvidScriptWasmtimePackedSelfPropertyI64GetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyI64GetBridge*)calloc(1, sizeof(*bridge));
	if (bridge != NULL)
	{
		bridge->callback = callback;
		bridge->environment = environment;
	}
	return avidscript_wasmtime_linker_define_packed_property_get(
		linker, module_name, module_name_size, import_name, import_name_size,
		bridge, wasm_valtype_new_i64(),
		avidscript_wasmtime_packed_self_property_i64_get_trampoline,
		"Could not allocate the packed self-property-i64-get bridge or function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_packed_self_property_i64_set(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimePackedSelfPropertyI64SetCallback callback,
	void* environment)
{
	AvidScriptWasmtimePackedSelfPropertyI64SetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyI64SetBridge*)calloc(1, sizeof(*bridge));
	if (bridge != NULL)
	{
		bridge->callback = callback;
		bridge->environment = environment;
	}
	return avidscript_wasmtime_linker_define_packed_property_set(
		linker, module_name, module_name_size, import_name, import_name_size,
		bridge, wasm_valtype_new_i64(),
		avidscript_wasmtime_packed_self_property_i64_set_trampoline,
		"Could not allocate the packed self-property-i64-set bridge or function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_packed_self_property_f64_get(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimePackedSelfPropertyF64GetCallback callback,
	void* environment)
{
	AvidScriptWasmtimePackedSelfPropertyF64GetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyF64GetBridge*)calloc(1, sizeof(*bridge));
	if (bridge != NULL)
	{
		bridge->callback = callback;
		bridge->environment = environment;
	}
	return avidscript_wasmtime_linker_define_packed_property_get(
		linker, module_name, module_name_size, import_name, import_name_size,
		bridge, wasm_valtype_new_f64(),
		avidscript_wasmtime_packed_self_property_f64_get_trampoline,
		"Could not allocate the packed self-property-f64-get bridge or function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_packed_self_property_f64_set(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimePackedSelfPropertyF64SetCallback callback,
	void* environment)
{
	AvidScriptWasmtimePackedSelfPropertyF64SetBridge* bridge =
		(AvidScriptWasmtimePackedSelfPropertyF64SetBridge*)calloc(1, sizeof(*bridge));
	if (bridge != NULL)
	{
		bridge->callback = callback;
		bridge->environment = environment;
	}
	return avidscript_wasmtime_linker_define_packed_property_set(
		linker, module_name, module_name_size, import_name, import_name_size,
		bridge, wasm_valtype_new_f64(),
		avidscript_wasmtime_packed_self_property_f64_set_trampoline,
		"Could not allocate the packed self-property-f64-set bridge or function type.");
}

static AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_guest_address(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfGuestAddressCallback callback,
	void* environment,
	wasmtime_func_unchecked_callback_t trampoline,
	const char* allocation_failure)
{
	AvidScriptWasmtimeSelfGuestAddressBridge* bridge = (AvidScriptWasmtimeSelfGuestAddressBridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure(allocation_failure);
	bridge->callback = callback;
	bridge->environment = environment;
	return avidscript_wasmtime_linker_define_typed_i32(linker, module_name, module_name_size, import_name, import_name_size, 3, trampoline, bridge, allocation_failure);
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_property_i32_get_set(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfGuestAddressCallback callback,
	void* environment)
{
	return avidscript_wasmtime_linker_define_self_guest_address(linker, module_name, module_name_size, import_name, import_name_size, callback, environment, avidscript_wasmtime_self_property_i32_get_set_trampoline, "Could not allocate the typed self-property bridge or function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_vector_value(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfGuestAddressCallback callback,
	void* environment)
{
	return avidscript_wasmtime_linker_define_self_guest_address(linker, module_name, module_name_size, import_name, import_name_size, callback, environment, avidscript_wasmtime_self_vector_value_trampoline, "Could not allocate the typed self-vector bridge or function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_stable_object_roundtrip(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeStableObjectRoundtripCallback callback,
	void* environment)
{
	AvidScriptWasmtimeStableObjectRoundtripBridge* bridge = (AvidScriptWasmtimeStableObjectRoundtripBridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure("Could not allocate the typed stable-object bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	return avidscript_wasmtime_linker_define_typed_i32(linker, module_name, module_name_size, import_name, import_name_size, 5, avidscript_wasmtime_stable_object_roundtrip_trampoline, bridge, "Could not allocate the typed stable-object function type.");
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_command_buffer_submit(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeI32PairCallback callback,
	void* environment)
{
	AvidScriptWasmtimeI32PairBridge* bridge = (AvidScriptWasmtimeI32PairBridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
		return avidscript_wasmtime_local_failure("Could not allocate the typed command-buffer bridge.");
	bridge->callback = callback;
	bridge->environment = environment;
	return avidscript_wasmtime_linker_define_typed_i32(linker, module_name, module_name_size, import_name, import_name_size, 2, avidscript_wasmtime_command_buffer_submit_trampoline, bridge, "Could not allocate the typed command-buffer function type.");
}
