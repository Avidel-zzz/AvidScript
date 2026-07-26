# Wasmtime v45 C API 依赖

本目录锁定 Wasmtime `v45.0.0` 官方 Win64 C API archive 的 Cranelift 完整布局。仓库只跟踪依赖身份、许可证、安装器和 UBT 规则，不跟踪 archive、header、`.lib`、`.dll` 或下载 cache。

## 依赖身份

- 发布版本：`v45.0.0`
- 平台：`x86_64-windows-c-api`
- 许可：`Apache-2.0 WITH LLVM-exception`
- 链接方式：`wasmtime.dll` 与 `wasmtime.dll.lib`
- 禁止链接：静态 `wasmtime.lib`
- 编译器特性：`WASMTIME_FEATURE_CRANELIFT`

完整 URL、大小、SHA-256、archive root 和安装路径由 `WasmtimeDependency.lock.json` 固定，并由 JSON schema 与安装器共同校验。

## 管理命令

在插件根目录运行：

```powershell
& "Build/InstallWasmtimeDependency.ps1" -Mode ValidateLock
& "Build/InstallWasmtimeDependency.ps1" -Mode Install
& "Build/InstallWasmtimeDependency.ps1" -Mode Verify
& "Build/InstallWasmtimeDependency.ps1" -Mode Remove
```

`Install` 只发布完整 C API include、DLL、import library 与 archive 根许可证，不复制静态 `wasmtime.lib`。安装器还会验证 `wasmtime/conf.h` 明确启用 Cranelift；minimal 子布局不满足执行层 JIT 契约。`Verify` 会重算 lock 身份、marker 和完整内容摘要；`Remove` 仅删除通过完整验证的受管版本目录。

受管实物位于 `installed/Win64/v45.0.0/`，该目录由 Git 忽略。`Wasmtime.Build.cs` 仅在 Win64 受管布局完整时定义 `AVIDSCRIPT_WITH_WASMTIME=1`；否则定义为 `0`。
