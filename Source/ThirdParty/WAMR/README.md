# WAMR ThirdParty Layout

This directory contains the AvidScript integration point for WAMR.

## Upstream

- Repository: https://github.com/bytecodealliance/wasm-micro-runtime
- First candidate tag: `WAMR-2.4.4`
- Integration mode: source-vendored snapshot

## Expected Layout

```text
WAMR/
  WAMR.Build.cs
  README.md
  upstream/
    core/iwasm/include/wasm_export.h
  lib/
    Win64/
      Release/
        vmlib.lib
  build/
  out/
```

## Build Guard

`WAMR.Build.cs` exports `AVIDSCRIPT_WITH_WAMR`.

- `AVIDSCRIPT_WITH_WAMR=1` when required headers and the Win64 static library are present.
- `AVIDSCRIPT_WITH_WAMR=0` when the upstream source or static library is missing.

This keeps the Unreal plugin buildable while WAMR vendoring and static-library generation are still in progress.

## Next Step

P1.2b should add the actual WAMR source snapshot, record tag/commit/license metadata, build the first Win64 static library, and verify that `AvidScriptRuntime` compiles with `AVIDSCRIPT_WITH_WAMR=1`.

