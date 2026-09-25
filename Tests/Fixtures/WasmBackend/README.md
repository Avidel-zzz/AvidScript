# WASM 后端测试工件

`P41_5_WamrSmoke.guestir.json` 是可读的 Guest IR 输入，`P41_5_WamrSmoke.wasm` 是由正式 `AvidScript.WasmBackend` 生成的对应 WASM 1.0 工件。对应的 UE 自动化测试直接读取这份二进制。

在插件根目录使用固定 .NET 8.0.416 环境后，可执行：

```powershell
<UserProfile>\.dotnet\dotnet.exe run --project Tools\AvidScript.WasmBackend\AvidScript.WasmBackend.csproj -- Tests\Fixtures\WasmBackend\P41_5_WamrSmoke.guestir.json Tests\Fixtures\WasmBackend\P41_5_WamrSmoke.wasm
```

当前生成物为 463 字节，SHA-256：`7babb00f52681ba01018774fee1327b7187a3495a39bd422e73c422e77d3f117`。

更新 fixture 时必须同时通过 `AvidScript.WasmBackend.Tests`、独立 WAMR load/execute 和 `AvidScript.Architecture.VM.GeneratedWasmBackendArtifactSmoke`，并更新本文件中的大小与哈希。

`P66_TaskLanguageError.guestir.json` 由 `AvidScript.GuestIr.Tests` 的组合 Task/语言错误样例生成，使用 Guest IR `20/1.19` 与 Semantic `40/1.49` 测试 provenance；它不是 C# 编译器产物。`P66_TaskLanguageError.wasm` 由正式 WASM backend 编译。`AvidScript.WasmBackend.Tests` 核对规范化 IR、确定性 WASM 字节、Host import 和语言错误目录；`AvidScript.Runtime.LanguageErrorCatalog.TaskFaultVmImport` 在 Win64 的 Wasmtime 与 WAMR 下加载该模块，并用单独的最小 WASM 探针调用新 import。WASM 为 4916 字节，SHA-256：`2ab8839282d00789e260ffbb74a1d28ef38354f7bfa94b4fa1562524c8274e68`。

从插件根重新生成这对 fixture：

```powershell
$env:AVIDSCRIPT_TASK_LANGUAGE_ERROR_IR_DIR = (Resolve-Path Tests/Fixtures/WasmBackend).Path
& (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe') run --project Tools/AvidScript.GuestIr.Tests/AvidScript.GuestIr.Tests.csproj -c Release
& (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe') run --project Tools/AvidScript.WasmBackend/AvidScript.WasmBackend.csproj -c Release -- Tests/Fixtures/WasmBackend/P66_TaskLanguageError.guestir.json Tests/Fixtures/WasmBackend/P66_TaskLanguageError.wasm
```
