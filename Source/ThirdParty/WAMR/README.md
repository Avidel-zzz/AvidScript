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

This is intentionally minimal for the first UE embedding spike.
