#pragma once

#include "AvidScriptWasmtimeApi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef int32_t (*AvidScriptWasmtimeEmptyI32Callback)(
	void* environment,
	int32_t* out_value);

typedef int32_t (*AvidScriptWasmtimeI32PairCallback)(
	void* environment,
	int32_t left,
	int32_t right,
	int32_t* out_value);

typedef int32_t (*AvidScriptWasmtimeSelfI32PairCallback)(
	void* environment,
	int32_t self_slot,
	int32_t self_generation,
	int32_t left,
	int32_t right,
	int32_t* out_value);

typedef int32_t (*AvidScriptWasmtimeSelfI32PairGuestResultCallback)(
	void* environment,
	int32_t self_slot,
	int32_t self_generation,
	int32_t left,
	int32_t right,
	int32_t guest_address,
	int32_t* out_status);

typedef int32_t (*AvidScriptWasmtimeSelfF32TripleGuestVectorCallback)(
	void* environment,
	int32_t self_slot,
	int32_t self_generation,
	float x,
	float y,
	float z,
	int32_t guest_address,
	int32_t* out_status);

typedef int32_t (*AvidScriptWasmtimeSelfPropertyI32GetCallback)(
	void* environment,
	int32_t self_slot,
	int32_t self_generation,
	int32_t* out_value);

typedef int32_t (*AvidScriptWasmtimeSelfPropertyI32SetCallback)(
	void* environment,
	int32_t self_slot,
	int32_t self_generation,
	int32_t value,
	int32_t* out_value);

typedef int32_t (*AvidScriptWasmtimeSelfGuestAddressCallback)(
	void* environment,
	int32_t self_slot,
	int32_t self_generation,
	int32_t guest_address,
	int32_t* out_value);

typedef int32_t (*AvidScriptWasmtimeStableObjectRoundtripCallback)(
	void* environment,
	int32_t self_slot,
	int32_t self_generation,
	int32_t object_slot,
	int32_t object_generation,
	int32_t guest_address,
	int32_t* out_value);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_empty_i32(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeEmptyI32Callback callback,
	void* environment);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_i32_pair(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeI32PairCallback callback,
	void* environment);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_i32_pair(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfI32PairCallback callback,
	void* environment);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_i32_pair_guest_result(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfI32PairGuestResultCallback callback,
	void* environment);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_f32_triple_guest_vector(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfF32TripleGuestVectorCallback callback,
	void* environment);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_property_i32_get(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfPropertyI32GetCallback callback,
	void* environment);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_property_i32_set(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfPropertyI32SetCallback callback,
	void* environment);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_property_i32_get_set(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfGuestAddressCallback callback,
	void* environment);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_self_vector_value(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeSelfGuestAddressCallback callback,
	void* environment);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_stable_object_roundtrip(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeStableObjectRoundtripCallback callback,
	void* environment);

AvidScriptWasmtimeFailure* avidscript_wasmtime_linker_define_command_buffer_submit(
	AvidScriptWasmtimeLinker* linker,
	const char* module_name,
	size_t module_name_size,
	const char* import_name,
	size_t import_name_size,
	AvidScriptWasmtimeI32PairCallback callback,
	void* environment);

#ifdef __cplusplus
}
#endif
