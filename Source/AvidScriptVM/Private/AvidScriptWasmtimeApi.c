#include "AvidScriptWasmtimeApi.h"

#include "AvidScriptWasmtimeApiInternal.h"

#include <stdlib.h>
#include <string.h>

typedef struct AvidScriptWasmtimeHostBridge
{
	AvidScriptWasmtimeHostCallback callback;
	void* environment;
} AvidScriptWasmtimeHostBridge;

AvidScriptWasmtimeFailure* avidscript_wasmtime_failure_new(
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

AvidScriptWasmtimeFailure* avidscript_wasmtime_local_failure(
	const char* message)
{
	AvidScriptWasmtimeFailure* failure =
		(AvidScriptWasmtimeFailure*)calloc(1, sizeof(*failure));
	if (failure == NULL)
	{
		return NULL;
	}
	wasm_name_new_from_string_nt(&failure->message, message);
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
	const uint64_t wasm32_address_space_bytes = UINT64_C(1) << 32;
	wasm_config_t* config = wasm_config_new();
	AvidScriptWasmtimeEngine* engine;
	if (config == NULL)
	{
		return NULL;
	}
	wasmtime_config_strategy_set(config, WASMTIME_STRATEGY_CRANELIFT);
	wasmtime_config_cranelift_opt_level_set(config, WASMTIME_OPT_LEVEL_SPEED_AND_SIZE);
	wasmtime_config_memory_reservation_set(config, wasm32_address_space_bytes);
	wasmtime_config_memory_may_move_set(config, false);
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

AvidScriptWasmtimeEngine* avidscript_wasmtime_engine_new_with_profile(
	const AvidScriptWasmtimeEngineProfile* profile)
{
	wasm_config_t* config;
	AvidScriptWasmtimeEngine* engine;
	wasmtime_error_t* target_error;
	if (profile == NULL
		|| profile->SchemaVersion != 2
		|| profile->Strategy != AVIDSCRIPT_WASMTIME_ENGINE_STRATEGY_CRANELIFT
		|| (profile->Optimization != AVIDSCRIPT_WASMTIME_ENGINE_OPT_SPEED_AND_SIZE
			&& profile->Optimization != AVIDSCRIPT_WASMTIME_ENGINE_OPT_SPEED)
		|| profile->RegisterAllocator != AVIDSCRIPT_WASMTIME_ENGINE_REGALLOC_BACKTRACKING
		|| profile->Inlining != AVIDSCRIPT_WASMTIME_ENGINE_INLINING_ALL
		|| profile->CpuProfile != AVIDSCRIPT_WASMTIME_ENGINE_CPU_X86_64_V3
		|| profile->Wasm32MemoryReservationBytes == 0
		|| profile->MaxWasmStackBytes == 0
		|| !profile->bWasmGc
		|| profile->CompilerInliningSetter == NULL)
	{
		return NULL;
	}
	config = wasm_config_new();
	if (config == NULL)
	{
		return NULL;
	}
	wasmtime_config_strategy_set(config, WASMTIME_STRATEGY_CRANELIFT);
	wasmtime_config_cranelift_opt_level_set(
		config,
		profile->Optimization == AVIDSCRIPT_WASMTIME_ENGINE_OPT_SPEED
			? WASMTIME_OPT_LEVEL_SPEED
			: WASMTIME_OPT_LEVEL_SPEED_AND_SIZE);
	wasmtime_config_cranelift_regalloc_algorithm_set(
		config,
		WASMTIME_REGALLOC_BACKTRACKING);
	wasmtime_config_cranelift_debug_verifier_set(config, false);
	wasmtime_config_cranelift_nan_canonicalization_set(
		config,
		profile->bNanCanonicalization);
#ifdef WASMTIME_FEATURE_GC
	wasmtime_config_wasm_gc_set(config, profile->bWasmGc);
#else
	wasm_config_delete(config);
	return NULL;
#endif
	wasmtime_config_cranelift_flag_set(
		config,
		"enable_alias_analysis",
		"true");
	wasmtime_config_cranelift_flag_set(
		config,
		"enable_heap_access_spectre_mitigation",
		profile->bSpectreMitigation ? "true" : "false");
	wasmtime_config_cranelift_flag_set(
		config,
		"enable_table_access_spectre_mitigation",
		profile->bSpectreMitigation ? "true" : "false");
	target_error = wasmtime_config_target_set(
		config,
		"x86_64-pc-windows-msvc");
	if (target_error != NULL)
	{
		wasmtime_error_delete(target_error);
		wasm_config_delete(config);
		return NULL;
	}
	wasmtime_config_cranelift_flag_enable(config, "x86-64-v3");
	profile->CompilerInliningSetter(
		config,
		(uint8_t)profile->Inlining);
	wasmtime_config_memory_reservation_set(
		config,
		profile->Wasm32MemoryReservationBytes);
	wasmtime_config_max_wasm_stack_set(
		config,
		(size_t)profile->MaxWasmStackBytes);
	wasmtime_config_memory_may_move_set(config, profile->bMemoryMayMove);
	wasmtime_config_consume_fuel_set(config, profile->bConsumeFuel);
	wasmtime_config_epoch_interruption_set(config, profile->bEpochInterruption);
	wasmtime_config_signals_based_traps_set(config, true);
#ifdef WASMTIME_FEATURE_PARALLEL_COMPILATION
	wasmtime_config_parallel_compilation_set(
		config,
		profile->bParallelCompilation);
#endif
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

void avidscript_wasmtime_engine_increment_epoch(AvidScriptWasmtimeEngine* engine)
{
	if (engine != NULL && engine->value != NULL)
	{
		wasmtime_engine_increment_epoch(engine->value);
	}
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

AvidScriptWasmtimeFailure* avidscript_wasmtime_module_deserialize(
	AvidScriptWasmtimeEngine* engine,
	const uint8_t* serialized_bytes,
	size_t serialized_size,
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
	error = wasmtime_module_deserialize(
		engine->value,
		serialized_bytes,
		serialized_size,
		&module->value);
	if (error != NULL)
	{
		free(module);
		return avidscript_wasmtime_failure_new(error, NULL);
	}
	*out_module = module;
	return NULL;
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_module_serialize(
	const AvidScriptWasmtimeModule* module,
	uint8_t** out_serialized_bytes,
	size_t* out_serialized_size)
{
	wasm_byte_vec_t serialized;
	wasmtime_error_t* error;
	uint8_t* copied_bytes;
	*out_serialized_bytes = NULL;
	*out_serialized_size = 0;
	error = wasmtime_module_serialize(module->value, &serialized);
	if (error != NULL)
	{
		return avidscript_wasmtime_failure_new(error, NULL);
	}
	copied_bytes = (uint8_t*)malloc(serialized.size);
	if (copied_bytes == NULL)
	{
		wasm_byte_vec_delete(&serialized);
		return avidscript_wasmtime_local_failure(
			"Wasmtime serialized module allocation failed.");
	}
	memcpy(copied_bytes, serialized.data, serialized.size);
	*out_serialized_bytes = copied_bytes;
	*out_serialized_size = serialized.size;
	wasm_byte_vec_delete(&serialized);
	return NULL;
}

void avidscript_wasmtime_serialized_bytes_delete(uint8_t* serialized_bytes)
{
	free(serialized_bytes);
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

bool avidscript_wasmtime_store_set_limits(
	AvidScriptWasmtimeStore* store,
	uint64_t max_linear_memory_bytes)
{
	if (store == NULL
		|| store->value == NULL
		|| max_linear_memory_bytes == 0
		|| max_linear_memory_bytes > INT64_MAX)
	{
		return false;
	}
	wasmtime_store_limiter(
		store->value,
		(int64_t)max_linear_memory_bytes,
		-1,
		-1,
		-1,
		-1);
	return true;
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_store_set_fuel(
	AvidScriptWasmtimeStore* store,
	uint64_t fuel)
{
	wasmtime_error_t* error;
	if (store == NULL || store->context == NULL)
	{
		return avidscript_wasmtime_local_failure("Wasmtime store is unavailable.");
	}
	error = wasmtime_context_set_fuel(store->context, fuel);
	return error != NULL ? avidscript_wasmtime_failure_new(error, NULL) : NULL;
}

AvidScriptWasmtimeFailure* avidscript_wasmtime_store_get_fuel(
	const AvidScriptWasmtimeStore* store,
	uint64_t* out_fuel)
{
	wasmtime_error_t* error;
	if (store == NULL || store->context == NULL || out_fuel == NULL)
	{
		return avidscript_wasmtime_local_failure("Wasmtime store or fuel output is unavailable.");
	}
	error = wasmtime_context_get_fuel(store->context, out_fuel);
	return error != NULL ? avidscript_wasmtime_failure_new(error, NULL) : NULL;
}

bool avidscript_wasmtime_store_set_epoch_deadline(
	AvidScriptWasmtimeStore* store,
	uint64_t ticks_beyond_current)
{
	if (store == NULL || store->context == NULL || ticks_beyond_current == 0)
	{
		return false;
	}
	wasmtime_context_set_epoch_deadline(store->context, ticks_beyond_current);
	return true;
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
	{
		wasmtime_extern_t memory;
		if (wasmtime_instance_export_get(
				store->context,
				&instance->value,
				"memory",
				6,
				&memory))
		{
			if (memory.kind == WASMTIME_EXTERN_MEMORY)
			{
				instance->exported_memory = memory.of.memory;
				instance->has_exported_memory = true;
			}
			wasmtime_extern_delete(&memory);
		}
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
	uint32_t* out_parameter_cell_count,
	uint32_t* out_result_cell_count)
{
	wasmtime_extern_t item;
	wasm_functype_t* type;
	const wasm_valtype_vec_t* parameters;
	const wasm_valtype_vec_t* results;
	AvidScriptWasmtimeFunction* function;
	size_t parameter_index;
	size_t result_index;
	uint32_t cell_count = 0;
	uint32_t result_cell_count = 0;
	*out_function = NULL;
	*out_parameter_cell_count = 0;
	*out_result_cell_count = 0;
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
	if (parameters->size > AVIDSCRIPT_WASMTIME_MAX_VALUES
		|| results->size > AVIDSCRIPT_WASMTIME_MAX_VALUES)
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
	function->result_count = (uint32_t)results->size;
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
	for (result_index = 0; result_index < results->size; ++result_index)
	{
		const wasm_valkind_t kind = wasm_valtype_kind(results->data[result_index]);
		switch (kind)
		{
		case WASM_I32:
		case WASM_F32:
			result_cell_count += 1;
			break;
		case WASM_I64:
		case WASM_F64:
			result_cell_count += 2;
			break;
		default:
			free(function);
			wasm_functype_delete(type);
			wasmtime_extern_delete(&item);
			return 3;
		}
		function->result_kinds[result_index] = kind;
	}
	function->cell_count = cell_count;
	function->result_cell_count = result_cell_count;
	*out_parameter_cell_count = cell_count;
	*out_result_cell_count = result_cell_count;
	*out_function = function;
	wasm_functype_delete(type);
	wasmtime_extern_delete(&item);
	return 0;
}

void avidscript_wasmtime_function_delete(AvidScriptWasmtimeFunction* function)
{
	free(function);
}

AvidScriptWasmtimePreparedCallShape avidscript_wasmtime_function_prepared_call_shape(
	const AvidScriptWasmtimeFunction* function)
{
	if (function != NULL
		&& function->parameter_count == 2
		&& function->result_count == 1
		&& function->parameter_kinds[0] == WASM_I32
		&& function->parameter_kinds[1] == WASM_I32
		&& function->result_kinds[0] == WASM_I32)
	{
		return AVIDSCRIPT_WASMTIME_PREPARED_CALL_I32_I32_TO_I32;
	}
	return AVIDSCRIPT_WASMTIME_PREPARED_CALL_GENERIC;
}

AvidScriptWasmtimeCallStatus avidscript_wasmtime_function_call_event(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	const uint32_t* cells,
	size_t cell_count,
	uint32_t* out_result_cells,
	size_t result_cell_capacity,
	size_t* out_result_cell_count,
	AvidScriptWasmtimeFailure** out_failure)
{
	wasmtime_val_t arguments[AVIDSCRIPT_WASMTIME_MAX_VALUES];
	wasmtime_val_t results[AVIDSCRIPT_WASMTIME_MAX_VALUES];
	wasm_trap_t* trap = NULL;
	wasmtime_error_t* error;
	size_t parameter_index;
	size_t result_index;
	size_t cell_index = 0;
	size_t result_cell_index = 0;
	if (out_failure == NULL)
	{
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	*out_failure = NULL;
	if (store == NULL || function == NULL)
	{
		*out_failure = avidscript_wasmtime_local_failure(
			"Wasmtime store and resolved function are required.");
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	if (cell_count > 0 && cells == NULL)
	{
		*out_failure = avidscript_wasmtime_local_failure(
			"Wasmtime argument cells are required for a non-empty call frame.");
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	if (out_result_cell_count == NULL)
	{
		*out_failure = avidscript_wasmtime_local_failure(
			"Wasmtime result cell count output is required.");
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	*out_result_cell_count = 0;
	if (cell_count != function->cell_count)
	{
		*out_failure = avidscript_wasmtime_local_failure(
			"Wasmtime call cell count does not match the resolved export ABI.");
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	if (function->result_cell_count > result_cell_capacity
		|| (function->result_cell_count > 0 && out_result_cells == NULL))
	{
		*out_failure = avidscript_wasmtime_local_failure(
			"Wasmtime result buffer is smaller than the resolved export ABI.");
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
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
			*out_failure = avidscript_wasmtime_local_failure(
				"Wasmtime parameter kind is unsupported by the VM cell ABI.");
			return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
		}
	}
	error = wasmtime_func_call(
		store->context,
		&function->value,
		function->parameter_count == 0 ? NULL : arguments,
		function->parameter_count,
		function->result_count == 0 ? NULL : results,
		function->result_count,
		&trap);
	if (error != NULL || trap != NULL)
	{
		*out_failure = avidscript_wasmtime_failure_new(error, trap);
		return AVIDSCRIPT_WASMTIME_CALL_RUNTIME_FAILURE;
	}
	for (result_index = 0; result_index < function->result_count; ++result_index)
	{
		uint64_t wide_bits;
		switch (function->result_kinds[result_index])
		{
		case WASM_I32:
			out_result_cells[result_cell_index++] = (uint32_t)results[result_index].of.i32;
			break;
		case WASM_F32:
			memcpy(
				&out_result_cells[result_cell_index++],
				&results[result_index].of.f32,
				sizeof(float));
			break;
		case WASM_I64:
			wide_bits = (uint64_t)results[result_index].of.i64;
			out_result_cells[result_cell_index++] = (uint32_t)wide_bits;
			out_result_cells[result_cell_index++] = (uint32_t)(wide_bits >> 32);
			break;
		case WASM_F64:
			memcpy(&wide_bits, &results[result_index].of.f64, sizeof(double));
			out_result_cells[result_cell_index++] = (uint32_t)wide_bits;
			out_result_cells[result_cell_index++] = (uint32_t)(wide_bits >> 32);
			break;
		default:
			*out_failure = avidscript_wasmtime_local_failure(
				"Wasmtime result kind is unsupported by the VM cell ABI.");
			return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
		}
	}
	*out_result_cell_count = result_cell_index;
	return AVIDSCRIPT_WASMTIME_CALL_SUCCESS;
}

static AvidScriptWasmtimeCallStatus
avidscript_wasmtime_function_call_event_prepared_impl(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	const uint32_t* cells,
	uint32_t* out_result_cells,
	size_t result_cell_capacity,
	size_t* out_result_cell_count,
	AvidScriptWasmtimeFailure** out_failure)
{
	wasmtime_val_raw_t values[AVIDSCRIPT_WASMTIME_MAX_VALUES];
	wasm_trap_t* trap = NULL;
	wasmtime_error_t* error;
	size_t parameter_index;
	size_t result_index;
	size_t cell_index = 0;
	size_t result_cell_index = 0;
	size_t value_count;
	if (out_failure == NULL)
	{
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	*out_failure = NULL;
	if (store == NULL || function == NULL)
	{
		*out_failure = avidscript_wasmtime_local_failure(
			"Wasmtime store and resolved function are required.");
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	if (function->cell_count > 0 && cells == NULL)
	{
		*out_failure = avidscript_wasmtime_local_failure(
			"Wasmtime argument cells are required for a non-empty call frame.");
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	if (out_result_cell_count == NULL)
	{
		*out_failure = avidscript_wasmtime_local_failure(
			"Wasmtime result cell count output is required.");
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	*out_result_cell_count = 0;
	if (function->result_cell_count > result_cell_capacity
		|| (function->result_cell_count > 0 && out_result_cells == NULL))
	{
		*out_failure = avidscript_wasmtime_local_failure(
			"Wasmtime result buffer is smaller than the resolved export ABI.");
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	for (parameter_index = 0; parameter_index < function->parameter_count; ++parameter_index)
	{
		uint64_t wide_bits;
		switch (function->parameter_kinds[parameter_index])
		{
		case WASM_I32:
			values[parameter_index].i32 = (int32_t)cells[cell_index++];
			break;
		case WASM_F32:
			memcpy(&values[parameter_index].f32, &cells[cell_index], sizeof(float));
			++cell_index;
			break;
		case WASM_I64:
			wide_bits = (uint64_t)cells[cell_index]
				| ((uint64_t)cells[cell_index + 1] << 32);
			values[parameter_index].i64 = (int64_t)wide_bits;
			cell_index += 2;
			break;
		case WASM_F64:
			wide_bits = (uint64_t)cells[cell_index]
				| ((uint64_t)cells[cell_index + 1] << 32);
			memcpy(&values[parameter_index].f64, &wide_bits, sizeof(double));
			cell_index += 2;
			break;
		default:
			*out_failure = avidscript_wasmtime_local_failure(
				"Wasmtime parameter kind is unsupported by the VM cell ABI.");
			return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
		}
	}
	value_count = function->parameter_count > function->result_count
		? function->parameter_count
		: function->result_count;
	error = wasmtime_func_call_unchecked(
		store->context,
		&function->value,
		values,
		value_count,
		&trap);
	if (error != NULL || trap != NULL)
	{
		*out_failure = avidscript_wasmtime_failure_new(error, trap);
		return AVIDSCRIPT_WASMTIME_CALL_RUNTIME_FAILURE;
	}
	for (result_index = 0; result_index < function->result_count; ++result_index)
	{
		uint64_t wide_bits;
		switch (function->result_kinds[result_index])
		{
		case WASM_I32:
			out_result_cells[result_cell_index++] = (uint32_t)values[result_index].i32;
			break;
		case WASM_F32:
			memcpy(
				&out_result_cells[result_cell_index++],
				&values[result_index].f32,
				sizeof(float));
			break;
		case WASM_I64:
			wide_bits = (uint64_t)values[result_index].i64;
			out_result_cells[result_cell_index++] = (uint32_t)wide_bits;
			out_result_cells[result_cell_index++] = (uint32_t)(wide_bits >> 32);
			break;
		case WASM_F64:
			memcpy(&wide_bits, &values[result_index].f64, sizeof(double));
			out_result_cells[result_cell_index++] = (uint32_t)wide_bits;
			out_result_cells[result_cell_index++] = (uint32_t)(wide_bits >> 32);
			break;
		default:
			*out_failure = avidscript_wasmtime_local_failure(
				"Wasmtime result kind is unsupported by the VM cell ABI.");
			return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
		}
	}
	*out_result_cell_count = result_cell_index;
	return AVIDSCRIPT_WASMTIME_CALL_SUCCESS;
}

AvidScriptWasmtimeCallStatus avidscript_wasmtime_function_call_event_unchecked(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	const uint32_t* cells,
	size_t cell_count,
	uint32_t* out_result_cells,
	size_t result_cell_capacity,
	size_t* out_result_cell_count,
	AvidScriptWasmtimeFailure** out_failure)
{
	if (out_failure == NULL)
	{
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	*out_failure = NULL;
	if (function != NULL && cell_count != function->cell_count)
	{
		*out_failure = avidscript_wasmtime_local_failure(
			"Wasmtime call cell count does not match the resolved export ABI.");
		return AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE;
	}
	return avidscript_wasmtime_function_call_event_prepared_impl(
		store,
		function,
		cells,
		out_result_cells,
		result_cell_capacity,
		out_result_cell_count,
		out_failure);
}

AvidScriptWasmtimeCallStatus avidscript_wasmtime_function_call_event_prepared(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	const uint32_t* cells,
	uint32_t* out_result_cells,
	size_t result_cell_capacity,
	size_t* out_result_cell_count,
	AvidScriptWasmtimeFailure** out_failure)
{
	return avidscript_wasmtime_function_call_event_prepared_impl(
		store,
		function,
		cells,
		out_result_cells,
		result_cell_capacity,
		out_result_cell_count,
		out_failure);
}

AvidScriptWasmtimeCallStatus avidscript_wasmtime_function_call_i32_i32_to_i32_prepared_unchecked(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	int32_t first,
	int32_t second,
	int32_t* out_result,
	AvidScriptWasmtimeFailure** out_failure)
{
	wasmtime_val_raw_t values[2];
	wasm_trap_t* trap = NULL;
	wasmtime_error_t* error;

	*out_failure = NULL;
	values[0].i32 = first;
	values[1].i32 = second;
	error = wasmtime_func_call_unchecked(
		store->context,
		&function->value,
		values,
		2,
		&trap);
	if (error != NULL || trap != NULL)
	{
		*out_failure = avidscript_wasmtime_failure_new(error, trap);
		return AVIDSCRIPT_WASMTIME_CALL_RUNTIME_FAILURE;
	}
	*out_result = values[0].i32;
	return AVIDSCRIPT_WASMTIME_CALL_SUCCESS;
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
	if (store != NULL && instance != NULL && instance->has_exported_memory)
	{
		context = store->context;
		*out_data = wasmtime_memory_data(context, &instance->exported_memory);
		*out_size = wasmtime_memory_data_size(context, &instance->exported_memory);
		return *out_data != NULL;
	}
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

int32_t avidscript_wasmtime_failure_trap_code(
	const AvidScriptWasmtimeFailure* failure)
{
	wasmtime_trap_code_t code;
	if (failure == NULL
		|| failure->trap == NULL
		|| !wasmtime_trap_code(failure->trap, &code))
	{
		return -1;
	}
	return (int32_t)code;
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
