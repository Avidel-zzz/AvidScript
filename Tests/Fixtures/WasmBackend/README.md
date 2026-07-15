# WASM 后端测试工件

`P41_5_WamrSmoke.guestir.json` 是可读的 Guest IR 输入，`P41_5_WamrSmoke.wasm` 是由正式 `AvidScript.WasmBackend` 生成的对应 WASM 1.0 工件。UE 自动化测试直接读取这份二进制，不在 C++ 测试中手写 WASM bytes。

在插件根目录使用固定 .NET 8.0.416 环境后，可执行：

```powershell
C:\Users\user0\.dotnet\dotnet.exe run --project Tools\AvidScript.WasmBackend\AvidScript.WasmBackend.csproj -- Tests\Fixtures\WasmBackend\P41_5_WamrSmoke.guestir.json Tests\Fixtures\WasmBackend\P41_5_WamrSmoke.wasm
```

当前生成物为 463 字节，SHA-256：`7babb00f52681ba01018774fee1327b7187a3495a39bd422e73c422e77d3f117`。

更新 fixture 时必须同时通过 `AvidScript.WasmBackend.Tests`、独立 WAMR load/execute 和 `AvidScript.Architecture.VM.GeneratedWasmBackendArtifactSmoke`，并更新本文件中的大小与哈希。
