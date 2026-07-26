# WAMR ThirdParty Layout

This directory contains the AvidScript integration point for WAMR.

## Upstream Snapshot

- Repository: https://github.com/bytecodealliance/wasm-micro-runtime
- Tag: `WAMR-2.4.4`
- Commit: `8c18e3f68b16c4bcaf05996b2636f6ed2b4cf629`
- License: Apache-2.0 WITH LLVM-exception, see `upstream/LICENSE`.
- Integration mode: source-vendored snapshot.

## Current Layout

```text
WAMR/
  WAMR.Build.cs
  README.md
  upstream/
    LICENSE
    core/iwasm/include/wasm_export.h
    product-mini/platforms/windows/CMakeLists.txt
  lib/
    Win64/
      Release/
        libiwasm.lib
  build/                 # ignored generated output
  out/                   # ignored generated output
```

## Build Guard

`WAMR.Build.cs` exports `AVIDSCRIPT_WITH_WAMR`.

- `AVIDSCRIPT_WITH_WAMR=1` when required headers and a Win64 static library are present.
- `AVIDSCRIPT_WITH_WAMR=0` when the upstream source or static library is missing.

The Win64 library candidates are checked in this order:

1. `lib/Win64/Release/iwasm.lib`
2. `lib/Win64/Release/libiwasm.lib`
3. `lib/Win64/Release/vmlib.lib`

## Rebuild Command

From the project root:

```powershell
cmd /c Plugins\AvidScript\Build\BuildWAMRWin64.cmd
```

The script calls Visual Studio `vcvars64.bat`, configures WAMR with CMake + Ninja, builds target `vmlib`, and copies the generated static library into `lib/Win64/Release`.

## Current Build Configuration

- Platform: Windows x64
- Runtime mode: interpreter + fast interpreter
- AOT: disabled
- JIT / Fast JIT: disabled
- libc builtin: enabled
- libc WASI: disabled
- multi-module: disabled
- SIMD: disabled
- mini loader: disabled
- trap call-stack snapshot: enabled
- standard wasm-c-api: disabled

This is intentionally minimal for the first UE embedding spike.

## AvidScript 本地集成补丁

### 标准 wasm-c-api 符号隔离

`core/iwasm/common/iwasm_common.cmake` 增加本地 `WAMR_BUILD_WASM_C_API` 开关。Win64 生产构建固定传入 `WAMR_BUILD_WASM_C_API=0`，从 `vmlib` 源列表中移除 `wasm_c_api.c`。AvidScript 的 WAMR backend 只使用 `wasm_runtime_*` API；排除此成员可防止 `wasm_config_*`、`wasm_engine_*`、`wasm_functype_*` 与 `wasm_trap_*` 同 Wasmtime v45 import library 的标准 C API 符号发生静态链接歧义。

`BuildWAMRWin64.cmd` 在复制 tracked `libiwasm.lib` 前后执行 `dumpbin /linkermember:1` 契约：`wasm_runtime_init`、`wasm_runtime_load` 必须存在，上述标准 wasm-c-api 符号族必须不存在。不得用链接顺序替代该产物边界；升级 WAMR snapshot 后必须重跑构建脚本并检查同一符号契约。

### Trap 调用栈

WAMR 2.4.4 在启用 `WAMR_BUILD_DUMP_CALL_STACK` 后，会在解释器异常出口同时创建快照并直接打印调用栈。AvidScript 将三个解释器异常出口调整为只调用 `wasm_interp_create_call_stack`，随后由 `AvidScriptVM/Private` 通过 bounded buffer API 读取并结构化解析。这样保留上游快照生命周期和 function index/offset，同时避免第三方文本绕过 Runtime/Editor 诊断管线污染游戏日志。

升级 WAMR snapshot 时必须重新核对 `wasm_interp_fast.c`、`wasm_interp_classic.c` 与 `wasm_runtime.c` 的异常出口，并运行 `AvidScript.VM.Diagnostics.TrapCallStack` 证明 trap 有结构化帧且日志中没有原始 `#<ordinal>` dump。
