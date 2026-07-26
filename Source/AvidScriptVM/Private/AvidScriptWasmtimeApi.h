#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct AvidScriptWasmtimeEngine AvidScriptWasmtimeEngine;
typedef struct AvidScriptWasmtimeModule AvidScriptWasmtimeModule;
typedef struct AvidScriptWasmtimeStore AvidScriptWasmtimeStore;
typedef struct AvidScriptWasmtimeLinker AvidScriptWasmtimeLinker;
typedef struct AvidScriptWasmtimeInstance AvidScriptWasmtimeInstance;
typedef struct AvidScriptWasmtimeFunction AvidScriptWasmtimeFunction;
typedef struct AvidScriptWasmtimeCaller AvidScriptWasmtimeCaller;
typedef struct AvidScriptWasmtimeFailure AvidScriptWasmtimeFailure;

typedef enum AvidScriptWasmtimeValueKind
{
	AVIDSCRIPT_WASMTIME_I32 = 0,
	AVIDSCRIPT_WASMTIME_I64 = 1,
	AVIDSCRIPT_WASMTIME_F32 = 2,
	AVIDSCRIPT_WASMTIME_F64 = 3
} AvidScriptWasmtimeValueKind;

typedef struct AvidScriptWasmtimeValue
{
	AvidScriptWasmtimeValueKind kind;
	union
	{
		int32_t i32;
		int64_t i64;
		float f32;
		double f64;
	} of;
} AvidScriptWasmtimeValue;

typedef bool (*AvidScriptWasmtimeHostCallback)(
	void* environment,
	AvidScriptWasmtimeCaller* caller,
	const AvidScriptWasmtimeValue* arguments,
	size_t argument_count,
	AvidScriptWasmtimeValue* results,
	size_t result_count);

AvidScriptWasmtimeEngine* avidscript_wasmtime_engine_new(void);
void avidscript_wasmtime_engine_delete(AvidScriptWasmtimeEngine* engine);

AvidScriptWasmtimeFailure* avidscript_wasmtime_module_new(
	AvidScriptWasmtimeEngine* engine,
	const uint8_t* bytecode,
	size_t bytecode_size,
	AvidScriptWasmtimeModule** out_module);
void avidscript_wasmtime_module_delete(AvidScriptWasmtimeModule* module);

AvidScriptWasmtimeStore* avidscript_wasmtime_store_new(AvidScriptWasmtimeEngine* engine);
void avidscript_wasmtime_store_delete(AvidScriptWasmtimeStore* store);

AvidScriptWasmtimeLinker* avidscript_wasmtime_linker_new(AvidScriptWasmtimeEngine* engine);
void avidscript_wasmtime_linker_delete(AvidScriptWasmtimeLinker* linker);
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
	void* environment);
AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_instantiate(
	AvidScriptWasmtimeLinker* linker,
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeModule* module,
	AvidScriptWasmtimeInstance** out_instance);

void avidscript_wasmtime_instance_delete(AvidScriptWasmtimeInstance* instance);
int avidscript_wasmtime_instance_resolve_event_export(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeInstance* instance,
	const char* export_name,
	size_t export_name_size,
	AvidScriptWasmtimeFunction** out_function,
	uint32_t* out_parameter_cell_count,
	uint32_t* out_result_cell_count);
void avidscript_wasmtime_function_delete(AvidScriptWasmtimeFunction* function);
AvidScriptWasmtimeFailure* avidscript_wasmtime_function_call_event(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	const uint32_t* cells,
	size_t cell_count,
	uint32_t* out_result_cells,
	size_t result_cell_capacity,
	size_t* out_result_cell_count);

bool avidscript_wasmtime_memory_data(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeInstance* instance,
	AvidScriptWasmtimeCaller* caller,
	uint8_t** out_data,
	size_t* out_size);

bool avidscript_wasmtime_failure_is_trap(const AvidScriptWasmtimeFailure* failure);
const char* avidscript_wasmtime_failure_message(
	const AvidScriptWasmtimeFailure* failure,
	size_t* out_size);
size_t avidscript_wasmtime_failure_frame_count(const AvidScriptWasmtimeFailure* failure);
bool avidscript_wasmtime_failure_frame(
	const AvidScriptWasmtimeFailure* failure,
	size_t index,
	uint32_t* out_function_index,
	size_t* out_function_offset,
	const char** out_function_name,
	size_t* out_function_name_size);
void avidscript_wasmtime_failure_delete(AvidScriptWasmtimeFailure* failure);

#ifdef __cplusplus
}
#endif
