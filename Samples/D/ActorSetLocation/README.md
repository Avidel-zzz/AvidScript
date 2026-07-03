# AvidScript D Guest Actor Set Location Sample

Status: Phase 5.0 spike sample.

This sample is intentionally tiny. It documents the guest-side ABI shape that
Phase 5.0 wants from a D-to-WASM toolchain:

```text
import avidscript.actor_set_location(slot: i32, generation: i32, x: f32, y: f32, z: f32) -> i32
export avid_on_begin_play() -> void
export avid_on_tick(delta_seconds: f32) -> void
```

Constraints:

- Freestanding guest sample.
- No Phobos dependency is intended.
- No GC dependency is intended.
- PC Editor smoke only.
- The build script may generate a temporary D source file with test-specific
  handle constants before compiling to WASM.

The source declaration uses `extern(C)` to keep symbol names and call ABI simple.
The toolchain/build step remains responsible for proving that the emitted WASM
imports `actor_set_location` from the `avidscript` module expected by the UE host.
