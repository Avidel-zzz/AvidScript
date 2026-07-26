#include "AvidScriptWasmtimeApi.h"

#include "wasmtime.h"

#include <stdlib.h>
#include <string.h>

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
	wasmtime_valkind_t parameter_kinds[AVIDSCRIPT_WASMTIME_MAX_VALUES];
};

struct AvidScriptWasmtimeFailure
{
	wasmtime_error_t* error;
	wasm_trap_t* trap;
	wasm_name_t message;
	wasm_frame_vec_t frames;
};

typedef struct AvidScriptWasmtimeHostBridge
{
	AvidScriptWasmtimeHostCallback callback;
	void* environment;
} AvidScriptWasmtimeHostBridge;

static AvidScriptWasmtimeFailure* avidscript_wasmtime_failure_new(
	wasmtime_error_t* error,
	wasm_trap_t* trap)
{
	AvidScriptWasmtimeFailure* failure;
	if (error == NULL && trap == NULL)
	{
		return NULL;
	}
	failure = (AvidScriptWasmtimeFailure*)calloc(1, sizeof(*failure));
	if (failure == NULL)
	{
		if (error != NULL)
		{
			wasmtime_error_delete(error);
		}
		if (trap != NULL)
		{
			wasm_trap_delete(trap);
		}
		return NULL;
	}
	failure->error = error;
	failure->trap = trap;
	if (error != NULL)
	{
		wasmtime_error_message(error, &failure->message);
	}
	else
	{
		wasm_trap_message(trap, &failure->message);
	}
	if (trap != NULL)
	{
		wasm_trap_trace(trap, &failure->frames);
	}
	return failure;
}

static wasm_valtype_t* avidscript_wasmtime_value_type(AvidScriptWasmtimeValueKind kind)
{
	switch (kind)
	{
	case AVIDSCRIPT_WASMTIME_I32:
		return wasm_valtype_new_i32();
	case AVIDSCRIPT_WASMTIME_I64:
		return wasm_valtype_new_i64();
	case AVIDSCRIPT_WASMTIME_F32:
		return wasm_valtype_new_f32();
	case AVIDSCRIPT_WASMTIME_F64:
		return wasm_valtype_new_f64();
	default:
		return NULL;
	}
}

static bool avidscript_wasmtime_from_native(
	const wasmtime_val_t* source,
	AvidScriptWasmtimeValue* destination)
{
	switch (source->kind)
	{
	case WASMTIME_I32:
		destination->kind = AVIDSCRIPT_WASMTIME_I32;
		destination->of.i32 = source->of.i32;
		return true;
	case WASMTIME_I64:
		destination->kind = AVIDSCRIPT_WASMTIME_I64;
		destination->of.i64 = source->of.i64;
		return true;
	case WASMTIME_F32:
		destination->kind = AVIDSCRIPT_WASMTIME_F32;
		destination->of.f32 = source->of.f32;
		return true;
	case WASMTIME_F64:
		destination->kind = AVIDSCRIPT_WASMTIME_F64;
		destination->of.f64 = source->of.f64;
		return true;
	default:
		return false;
	}
}

static bool avidscript_wasmtime_to_native(
	const AvidScriptWasmtimeValue* source,
	wasmtime_val_t* destination)
{
	switch (source->kind)
	{
	case AVIDSCRIPT_WASMTIME_I32:
		destination->kind = WASMTIME_I32;
		destination->of.i32 = source->of.i32;
		return true;
	case AVIDSCRIPT_WASMTIME_I64:
		destination->kind = WASMTIME_I64;
		destination->of.i64 = source->of.i64;
		return true;
	case AVIDSCRIPT_WASMTIME_F32:
		destination->kind = WASMTIME_F32;
		destination->of.f32 = source->of.f32;
		return true;
	case AVIDSCRIPT_WASMTIME_F64:
		destination->kind = WASMTIME_F64;
		destination->of.f64 = source->of.f64;
		return true;
	default:
		return false;
	}
}

static wasm_trap_t* avidscript_wasmtime_host_trampoline(
	void* environment,
	wasmtime_caller_t* caller,
	const wasmtime_val_t* arguments,
	size_t argument_count,
	wasmtime_val_t* results,
	size_t result_count)
{
	AvidScriptWasmtimeHostBridge* bridge = (AvidScriptWasmtimeHostBridge*)environment;
	AvidScriptWasmtimeValue converted_arguments[AVIDSCRIPT_WASMTIME_MAX_VALUES];
	AvidScriptWasmtimeValue converted_results[AVIDSCRIPT_WASMTIME_MAX_VALUES];
	size_t index;
	static const char invalid_bridge[] = "avidscript_host_bridge_unavailable";
	static const char invalid_values[] = "avidscript_host_bridge_value_invalid";
	static const char host_failure[] = "avidscript_host_import_failed";

	if (bridge == NULL || bridge->callback == NULL
		|| argument_count > AVIDSCRIPT_WASMTIME_MAX_VALUES
		|| result_count > AVIDSCRIPT_WASMTIME_MAX_VALUES)
	{
		return wasmtime_trap_new(invalid_bridge, sizeof(invalid_bridge) - 1);
	}
	for (index = 0; index < argument_count; ++index)
	{
		if (!avidscript_wasmtime_from_native(&arguments[index], &converted_arguments[index]))
		{
			return wasmtime_trap_new(invalid_values, sizeof(invalid_values) - 1);
		}
	}
	if (!bridge->callback(
		bridge->environment,
		(AvidScriptWasmtimeCaller*)caller,
		converted_arguments,
		argument_count,
		converted_results,
		result_count))
	{
		return wasmtime_trap_new(host_failure, sizeof(host_failure) - 1);
	}
	for (index = 0; index < result_count; ++index)
	{
		if (!avidscript_wasmtime_to_native(&converted_results[index], &results[index]))
		{
			return wasmtime_trap_new(invalid_values, sizeof(invalid_values) - 1);
		}
	}
	return NULL;
}

static void avidscript_wasmtime_host_bridge_delete(void* environment)
{
	free(environment);
}

AvidScriptWasmtimeEngine* avidscript_wasmtime_engine_new(void)
{
	wasm_config_t* config = wasm_config_new();
	AvidScriptWasmtimeEngine* engine;
	if (config == NULL)
	{
		return NULL;
	}
	wasmtime_config_strategy_set(config, WASMTIME_STRATEGY_CRANELIFT);
	wasmtime_config_cranelift_opt_level_set(config, WASMTIME_OPT_LEVEL_SPEED);
#ifdef WASMTIME_FEATURE_COMPONENT_MODEL
	wasmtime_config_wasm_component_model_set(config, false);
#endif
	engine = (AvidScriptWasmtimeEngine*)calloc(1, sizeof(*engine));
	if (engine == NULL)
	{
		wasm_config_delete(config);
		return NULL;
	}
	engine->value = wasm_engine_new_with_config(config);
	if (engine->value == NULL)
	{
		free(engine);
		return NULL;
	}
	return engine;
}

void avidscript_wasmtime_engine_delete(AvidScriptWasmtimeEngine* engine)
{
	if (engine != NULL)
	{
		wasm_engine_delete(engine->value);
		free(engine);
	}
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_module_new(
	AvidScriptWasmtimeEngine* engine,
	const uint8_t* bytecode,
	size_t bytecode_size,
	AvidScriptWasmtimeModule** out_module)
{
	AvidScriptWasmtimeModule* module;
	wasmtime_error_t* error;
	*out_module = NULL;
	module = (AvidScriptWasmtimeModule*)calloc(1, sizeof(*module));
	if (module == NULL)
	{
		return NULL;
	}
	error = wasmtime_module_new(engine->value, bytecode, bytecode_size, &module->value);
	if (error != NULL)
	{
		free(module);
		return avidscript_wasmtime_failure_new(error, NULL);
	}
	*out_module = module;
	return NULL;
}

void avidscript_wasmtime_module_delete(AvidScriptWasmtimeModule* module)
{
	if (module != NULL)
	{
		wasmtime_module_delete(module->value);
		free(module);
	}
}

AvidScriptWasmtimeStore* avidscript_wasmtime_store_new(AvidScriptWasmtimeEngine* engine)
{
	AvidScriptWasmtimeStore* store = (AvidScriptWasmtimeStore*)calloc(1, sizeof(*store));
	if (store == NULL)
	{
		return NULL;
	}
	store->value = wasmtime_store_new(engine->value, NULL, NULL);
	store->context = store->value != NULL ? wasmtime_store_context(store->value) : NULL;
	if (store->value == NULL || store->context == NULL)
	{
		wasmtime_store_delete(store->value);
		free(store);
		return NULL;
	}
	return store;
}

void avidscript_wasmtime_store_delete(AvidScriptWasmtimeStore* store)
{
	if (store != NULL)
	{
		wasmtime_store_delete(store->value);
		free(store);
	}
}

AvidScriptWasmtimeLinker* avidscript_wasmtime_linker_new(AvidScriptWasmtimeEngine* engine)
{
	AvidScriptWasmtimeLinker* linker = (AvidScriptWasmtimeLinker*)calloc(1, sizeof(*linker));
	if (linker == NULL)
	{
		return NULL;
	}
	linker->value = wasmtime_linker_new(engine->value);
	if (linker->value == NULL)
	{
		free(linker);
		return NULL;
	}
	return linker;
}

void avidscript_wasmtime_linker_delete(AvidScriptWasmtimeLinker* linker)
{
	if (linker != NULL)
	{
		wasmtime_linker_delete(linker->value);
		free(linker);
	}
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_func(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	const AvidScriptWasmtimeValueKind* parameter_kinds,
	size_t parameter_count,
	const AvidScriptWasmtimeValueKind* result_kinds,
	size_t result_count,
	AvidScriptWasmtimeHostCallback callback,
	void* environment)
{
	wasm_valtype_vec_t parameters;
	wasm_valtype_vec_t results;
	wasm_functype_t* function_type;
	AvidScriptWasmtimeHostBridge* bridge;
	wasmtime_error_t* error;
	size_t index;

	wasm_valtype_vec_new_uninitialized(&parameters, parameter_count);
	for (index = 0; index < parameter_count; ++index)
	{
		parameters.data[index] = avidscript_wasmtime_value_type(parameter_kinds[index]);
	}
	wasm_valtype_vec_new_uninitialized(&results, result_count);
	for (index = 0; index < result_count; ++index)
	{
		results.data[index] = avidscript_wasmtime_value_type(result_kinds[index]);
	}
	function_type = wasm_functype_new(&parameters, &results);
	if (function_type == NULL)
	{
		return NULL;
	}
	bridge = (AvidScriptWasmtimeHostBridge*)calloc(1, sizeof(*bridge));
	if (bridge == NULL)
	{
		wasm_functype_delete(function_type);
		return NULL;
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
		avidscript_wasmtime_host_trampoline,
		bridge,
		avidscript_wasmtime_host_bridge_delete);
	wasm_functype_delete(function_type);
	if (error != NULL)
	{
		free(bridge);
		return avidscript_wasmtime_failure_new(error, NULL);
	}
	return NULL;
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_instantiate(
	AvidScriptWasmtimeLinker* linker,
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeModule* module,
	AvidScriptWasmtimeInstance** out_instance)
{
	AvidScriptWasmtimeInstance* instance;
	wasm_trap_t* trap = NULL;
	wasmtime_error_t* error;
	*out_instance = NULL;
	instance = (AvidScriptWasmtimeInstance*)calloc(1, sizeof(*instance));
	if (instance == NULL)
	{
		return NULL;
	}
	error = wasmtime_linker_instantiate(
		linker->value,
		store->context,
		module->value,
		&instance->value,
		&trap);
	if (error != NULL || trap != NULL)
	{
		free(instance);
		return avidscript_wasmtime_failure_new(error, trap);
	}
	*out_instance = instance;
	return NULL;
}

void avidscript_wasmtime_instance_delete(AvidScriptWasmtimeInstance* instance)
{
	free(instance);
}

int avidscript_wasmtime_instance_resolve_event_export(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeInstance* instance,
	const char* export_name,
	size_t export_name_size,
	AvidScriptWasmtimeFunction** out_function,
	uint32_t* out_cell_count)
{
	wasmtime_extern_t item;
	wasm_functype_t* type;
	const wasm_valtype_vec_t* parameters;
	const wasm_valtype_vec_t* results;
	AvidScriptWasmtimeFunction* function;
	size_t parameter_index;
	uint32_t cell_count = 0;
	*out_function = NULL;
	*out_cell_count = 0;
	if (!wasmtime_instance_export_get(
		store->context,
		&instance->value,
		export_name,
		export_name_size,
		&item))
	{
		return 1;
	}
	if (item.kind != WASMTIME_EXTERN_FUNC)
	{
		wasmtime_extern_delete(&item);
		return 2;
	}
	type = wasmtime_func_type(store->context, &item.of.func);
	if (type == NULL)
	{
		wasmtime_extern_delete(&item);
		return 4;
	}
	parameters = wasm_functype_params(type);
	results = wasm_functype_results(type);
	if (results->size != 0 || parameters->size > AVIDSCRIPT_WASMTIME_MAX_VALUES)
	{
		wasm_functype_delete(type);
		wasmtime_extern_delete(&item);
		return 3;
	}
	function = (AvidScriptWasmtimeFunction*)calloc(1, sizeof(*function));
	if (function == NULL)
	{
		wasm_functype_delete(type);
		wasmtime_extern_delete(&item);
		return 4;
	}
	function->value = item.of.func;
	function->parameter_count = (uint32_t)parameters->size;
	for (parameter_index = 0; parameter_index < parameters->size; ++parameter_index)
	{
		const wasm_valkind_t kind = wasm_valtype_kind(parameters->data[parameter_index]);
		switch (kind)
		{
		case WASM_I32:
		case WASM_F32:
			cell_count += 1;
			break;
		case WASM_I64:
		case WASM_F64:
			cell_count += 2;
			break;
		default:
			free(function);
			wasm_functype_delete(type);
			wasmtime_extern_delete(&item);
			return 3;
		}
		function->parameter_kinds[parameter_index] = kind;
	}
	function->cell_count = cell_count;
	*out_cell_count = cell_count;
	*out_function = function;
	wasm_functype_delete(type);
	wasmtime_extern_delete(&item);
	return 0;
}

void avidscript_wasmtime_function_delete(AvidScriptWasmtimeFunction* function)
{
	free(function);
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_function_call_event(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	const uint32_t* cells,
	size_t cell_count)
{
	wasmtime_val_t arguments[AVIDSCRIPT_WASMTIME_MAX_VALUES];
	wasm_trap_t* trap = NULL;
	wasmtime_error_t* error;
	size_t parameter_index;
	size_t cell_index = 0;
	if (cell_count != function->cell_count)
	{
		return NULL;
	}
	for (parameter_index = 0; parameter_index < function->parameter_count; ++parameter_index)
	{
		uint64_t wide_bits;
		arguments[parameter_index].kind = function->parameter_kinds[parameter_index];
		switch (function->parameter_kinds[parameter_index])
		{
		case WASM_I32:
			arguments[parameter_index].of.i32 = (int32_t)cells[cell_index++];
			break;
		case WASM_F32:
			memcpy(&arguments[parameter_index].of.f32, &cells[cell_index], sizeof(float));
			++cell_index;
			break;
		case WASM_I64:
			wide_bits = (uint64_t)cells[cell_index]
				| ((uint64_t)cells[cell_index + 1] << 32);
			arguments[parameter_index].of.i64 = (int64_t)wide_bits;
			cell_index += 2;
			break;
		case WASM_F64:
			wide_bits = (uint64_t)cells[cell_index]
				| ((uint64_t)cells[cell_index + 1] << 32);
			memcpy(&arguments[parameter_index].of.f64, &wide_bits, sizeof(double));
			cell_index += 2;
			break;
		default:
			return NULL;
		}
	}
	error = wasmtime_func_call(
		store->context,
		&function->value,
		function->parameter_count == 0 ? NULL : arguments,
		function->parameter_count,
		NULL,
		0,
		&trap);
	return avidscript_wasmtime_failure_new(error, trap);
}

bool avidscript_wasmtime_memory_data(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeInstance* instance,
	AvidScriptWasmtimeCaller* caller,
	uint8_t** out_data,
	size_t* out_size)
{
	wasmtime_extern_t item;
	wasmtime_context_t* context;
	bool found;
	*out_data = NULL;
	*out_size = 0;
	if (caller != NULL)
	{
		found = wasmtime_caller_export_get((wasmtime_caller_t*)caller, "memory", 6, &item);
		context = wasmtime_caller_context((wasmtime_caller_t*)caller);
	}
	else
	{
		if (store == NULL || instance == NULL)
		{
			return false;
		}
		found = wasmtime_instance_export_get(
			store->context,
			&instance->value,
			"memory",
			6,
			&item);
		context = store->context;
	}
	if (!found || context == NULL)
	{
		return false;
	}
	if (item.kind != WASMTIME_EXTERN_MEMORY)
	{
		wasmtime_extern_delete(&item);
		return false;
	}
	*out_data = wasmtime_memory_data(context, &item.of.memory);
	*out_size = wasmtime_memory_data_size(context, &item.of.memory);
	wasmtime_extern_delete(&item);
	return *out_data != NULL;
}

bool avidscript_wasmtime_failure_is_trap(const AvidScriptWasmtimeFailure* failure)
{
	return failure != NULL && failure->trap != NULL;
}

const char* avidscript_wasmtime_failure_message(
	const AvidScriptWasmtimeFailure* failure,
	size_t* out_size)
{
	if (failure == NULL)
	{
		*out_size = 0;
		return NULL;
	}
	*out_size = failure->message.size;
	return failure->message.data;
}

size_t avidscript_wasmtime_failure_frame_count(const AvidScriptWasmtimeFailure* failure)
{
	return failure != NULL ? failure->frames.size : 0;
}

bool avidscript_wasmtime_failure_frame(
	const AvidScriptWasmtimeFailure* failure,
	size_t index,
	uint32_t* out_function_index,
	size_t* out_function_offset,
	const char** out_function_name,
	size_t* out_function_name_size)
{
	const wasm_frame_t* frame;
	const wasm_name_t* function_name;
	if (failure == NULL || index >= failure->frames.size)
	{
		return false;
	}
	frame = failure->frames.data[index];
	*out_function_index = wasm_frame_func_index(frame);
	*out_function_offset = wasm_frame_func_offset(frame);
	function_name = wasmtime_frame_func_name(frame);
	*out_function_name = function_name != NULL ? function_name->data : NULL;
	*out_function_name_size = function_name != NULL ? function_name->size : 0;
	return true;
}

void avidscript_wasmtime_failure_delete(AvidScriptWasmtimeFailure* failure)
{
	if (failure != NULL)
	{
		wasm_byte_vec_delete(&failure->message);
		wasm_frame_vec_delete(&failure->frames);
		if (failure->error != NULL)
		{
			wasmtime_error_delete(failure->error);
		}
		if (failure->trap != NULL)
		{
			wasm_trap_delete(failure->trap);
		}
		free(failure);
	}
}
