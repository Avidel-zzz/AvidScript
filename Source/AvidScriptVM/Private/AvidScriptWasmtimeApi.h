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

typedef enum AvidScriptWasmtimeCompilerStrategy
{
	AVIDSCRIPT_WASMTIME_ENGINE_STRATEGY_CRANELIFT = 0
} AvidScriptWasmtimeCompilerStrategy;

typedef enum AvidScriptWasmtimeOptimization
{
	AVIDSCRIPT_WASMTIME_ENGINE_OPT_SPEED_AND_SIZE = 0,
	AVIDSCRIPT_WASMTIME_ENGINE_OPT_SPEED = 1
} AvidScriptWasmtimeOptimization;

typedef enum AvidScriptWasmtimeRegisterAllocator
{
	AVIDSCRIPT_WASMTIME_ENGINE_REGALLOC_BACKTRACKING = 0
} AvidScriptWasmtimeRegisterAllocator;

typedef enum AvidScriptWasmtimeInliningMode
{
	AVIDSCRIPT_WASMTIME_ENGINE_INLINING_NONE = 0,
	AVIDSCRIPT_WASMTIME_ENGINE_INLINING_ALL = 1
} AvidScriptWasmtimeInliningMode;

typedef enum AvidScriptWasmtimeCpuProfile
{
	AVIDSCRIPT_WASMTIME_ENGINE_CPU_X86_64_V3 = 0,
	AVIDSCRIPT_WASMTIME_ENGINE_CPU_ARM64_V8A = 1
} AvidScriptWasmtimeCpuProfile;

typedef enum AvidScriptWasmtimeTargetProfile
{
	AVIDSCRIPT_WASMTIME_ENGINE_TARGET_X86_64_WINDOWS = 0,
	AVIDSCRIPT_WASMTIME_ENGINE_TARGET_AARCH64_ANDROID = 1
} AvidScriptWasmtimeTargetProfile;

typedef void (*AvidScriptWasmtimeCompilerInliningSetter)(
	void* config,
	uint8_t mode);

typedef void* (*AvidScriptWasmtimeModulePrecompiler)(
	void* engine,
	const uint8_t* wasm,
	size_t wasm_size,
	void* out_serialized);

typedef struct AvidScriptWasmtimeEngineProfile
{
	uint32_t SchemaVersion;
	AvidScriptWasmtimeCompilerStrategy Strategy;
	AvidScriptWasmtimeOptimization Optimization;
	AvidScriptWasmtimeRegisterAllocator RegisterAllocator;
	AvidScriptWasmtimeInliningMode Inlining;
	AvidScriptWasmtimeTargetProfile TargetProfile;
	AvidScriptWasmtimeCpuProfile CpuProfile;
	uint64_t Wasm32MemoryReservationBytes;
	uint64_t MaxWasmStackBytes;
	bool bMemoryMayMove;
	bool bSpectreMitigation;
	bool bNanCanonicalization;
	bool bParallelCompilation;
	bool bWasmGc;
	bool bConsumeFuel;
	bool bEpochInterruption;
	AvidScriptWasmtimeCompilerInliningSetter CompilerInliningSetter;
	AvidScriptWasmtimeModulePrecompiler ModulePrecompiler;
} AvidScriptWasmtimeEngineProfile;

typedef enum AvidScriptWasmtimeCallStatus
{
	AVIDSCRIPT_WASMTIME_CALL_SUCCESS = 0,
	AVIDSCRIPT_WASMTIME_CALL_RUNTIME_FAILURE = 1,
	AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE = 2
} AvidScriptWasmtimeCallStatus;

typedef enum AvidScriptWasmtimeTrapCode
{
	AVIDSCRIPT_WASMTIME_TRAP_UNKNOWN = -1,
	AVIDSCRIPT_WASMTIME_TRAP_STACK_OVERFLOW = 0,
	AVIDSCRIPT_WASMTIME_TRAP_MEMORY_OUT_OF_BOUNDS = 1,
	AVIDSCRIPT_WASMTIME_TRAP_INTERRUPT = 10,
	AVIDSCRIPT_WASMTIME_TRAP_OUT_OF_FUEL = 11,
	AVIDSCRIPT_WASMTIME_TRAP_ALLOCATION_TOO_LARGE = 15
} AvidScriptWasmtimeTrapCode;

typedef enum AvidScriptWasmtimePreparedCallShape
{
	AVIDSCRIPT_WASMTIME_PREPARED_CALL_GENERIC = 0,
	AVIDSCRIPT_WASMTIME_PREPARED_CALL_I32_I32_TO_I32 = 1
} AvidScriptWasmtimePreparedCallShape;

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
AvidScriptWasmtimeEngine* avidscript_wasmtime_engine_new_with_profile(
	const AvidScriptWasmtimeEngineProfile* profile);
void avidscript_wasmtime_engine_increment_epoch(AvidScriptWasmtimeEngine* engine);
void avidscript_wasmtime_engine_delete(AvidScriptWasmtimeEngine* engine);

AvidScriptWasmtimeFailure* avidscript_wasmtime_module_new(
	AvidScriptWasmtimeEngine* engine,
	const uint8_t* bytecode,
	size_t bytecode_size,
	AvidScriptWasmtimeModule** out_module);
AvidScriptWasmtimeFailure* avidscript_wasmtime_module_deserialize(
	AvidScriptWasmtimeEngine* engine,
	const uint8_t* serialized_bytes,
	size_t serialized_size,
	AvidScriptWasmtimeModule** out_module);
AvidScriptWasmtimeFailure* avidscript_wasmtime_module_serialize(
	const AvidScriptWasmtimeModule* module,
	uint8_t** out_serialized_bytes,
	size_t* out_serialized_size);
AvidScriptWasmtimeFailure* avidscript_wasmtime_module_precompile(
	AvidScriptWasmtimeEngine* engine,
	const uint8_t* wasm,
	size_t wasm_size,
	uint8_t** out_serialized_bytes,
	size_t* out_serialized_size);
void avidscript_wasmtime_serialized_bytes_delete(uint8_t* serialized_bytes);
void avidscript_wasmtime_module_delete(AvidScriptWasmtimeModule* module);

AvidScriptWasmtimeStore* avidscript_wasmtime_store_new(AvidScriptWasmtimeEngine* engine);
bool avidscript_wasmtime_store_set_limits(
	AvidScriptWasmtimeStore* store,
	uint64_t max_linear_memory_bytes);
AvidScriptWasmtimeFailure* avidscript_wasmtime_store_set_fuel(
	AvidScriptWasmtimeStore* store,
	uint64_t fuel);
AvidScriptWasmtimeFailure* avidscript_wasmtime_store_get_fuel(
	const AvidScriptWasmtimeStore* store,
	uint64_t* out_fuel);
bool avidscript_wasmtime_store_set_epoch_deadline(
	AvidScriptWasmtimeStore* store,
	uint64_t ticks_beyond_current);
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
AvidScriptWasmtimePreparedCallShape avidscript_wasmtime_function_prepared_call_shape(
	const AvidScriptWasmtimeFunction* function);
AvidScriptWasmtimeCallStatus avidscript_wasmtime_function_call_event(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	const uint32_t* cells,
	size_t cell_count,
	uint32_t* out_result_cells,
	size_t result_cell_capacity,
	size_t* out_result_cell_count,
	AvidScriptWasmtimeFailure** out_failure);
AvidScriptWasmtimeCallStatus avidscript_wasmtime_function_call_event_unchecked(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	const uint32_t* cells,
	size_t cell_count,
	uint32_t* out_result_cells,
	size_t result_cell_capacity,
	size_t* out_result_cell_count,
	AvidScriptWasmtimeFailure** out_failure);
AvidScriptWasmtimeCallStatus avidscript_wasmtime_function_call_event_prepared(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	const uint32_t* cells,
	uint32_t* out_result_cells,
	size_t result_cell_capacity,
	size_t* out_result_cell_count,
	AvidScriptWasmtimeFailure** out_failure);
AvidScriptWasmtimeCallStatus avidscript_wasmtime_function_call_i32_i32_to_i32_prepared_unchecked(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeFunction* function,
	int32_t first,
	int32_t second,
	int32_t* out_result,
	AvidScriptWasmtimeFailure** out_failure);

bool avidscript_wasmtime_memory_data(
	AvidScriptWasmtimeStore* store,
	AvidScriptWasmtimeInstance* instance,
	AvidScriptWasmtimeCaller* caller,
	uint8_t** out_data,
	size_t* out_size);

bool avidscript_wasmtime_failure_is_trap(const AvidScriptWasmtimeFailure* failure);
int32_t avidscript_wasmtime_failure_trap_code(
	const AvidScriptWasmtimeFailure* failure);
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
