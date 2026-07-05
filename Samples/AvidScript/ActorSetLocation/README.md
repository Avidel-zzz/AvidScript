# AvidScript ActorSetLocation Sample

This sample is the first minimal AvidScript DSL source.

```text
module actor_set_location

use actor_set_location

on begin_play {
    actor_set_location(1, 1, 123.0, 456.0, 789.0)
}

on tick(delta_seconds) {
}
```

The P6 frontend generator supports only this narrow syntax for now:

- one `module`
- one `use actor_set_location`
- one `on begin_play` block with one `actor_set_location` call
- one empty `on tick(delta_seconds)` block

Generate D source without compiling:

```powershell
powershell -ExecutionPolicy Bypass -File "Plugins\AvidScript\Build\BuildAvidScriptActor.ps1" -SourcePath "Plugins\AvidScript\Samples\AvidScript\ActorSetLocation\actor_set_location.avid" -SkipCompile
```

Build WASM and manifest with the verified portable LDC:

```powershell
powershell -ExecutionPolicy Bypass -File "Plugins\AvidScript\Build\BuildAvidScriptActor.ps1" -SourcePath "Plugins\AvidScript\Samples\AvidScript\ActorSetLocation\actor_set_location.avid" -Ldc2Path "C:\tmp\AvidScriptToolchains\ldc2-1.42.0-windows-x64\ldc2-1.42.0-windows-x64\bin\ldc2.exe"
```

Generated outputs are written under `Saved/AvidScriptGenerated/actor_set_location/` and are not committed to Git.
