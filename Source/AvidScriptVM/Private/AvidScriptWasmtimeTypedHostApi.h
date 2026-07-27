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

#ifdef __cplusplus
}
#endif
