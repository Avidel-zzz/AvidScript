# 固定 WebAssembly Kernel

`controlled_runtime_kernel.wat` 是 Phase 54 Task 4C 的公开源码，导出：

```text
run(iterations: i32, seed: i32) -> i32
```

它只使用 WebAssembly MVP `i32` 指令，不包含 import、memory、engine object 或 frontend runtime。`controlled_runtime_kernel.wasm` 由 Wasmtime Python 45.0.0 的 `wat2wasm` 从该 WAT 生成；仓库合同同时固定 WAT 与 WASM 的 SHA-256，并验证四条 lane 读取同一个 `.wasm` 文件。

重新生成仅用于有意更新 kernel：

```powershell
& "Benchmarks/PuertsComparison/ControlledRuntime/Scripts/Build-ControlledRuntimeKernel.ps1" -Mode Write -WatCompilerModuleRoot "<wasmtime-python-45-root>" -PythonExecutable "<python-executable>"
```

普通验证不会写文件：

```powershell
& "Benchmarks/PuertsComparison/ControlledRuntime/Scripts/Build-ControlledRuntimeKernel.ps1" -Mode Verify -WatCompilerModuleRoot "<wasmtime-python-45-root>" -PythonExecutable "<python-executable>"
```
