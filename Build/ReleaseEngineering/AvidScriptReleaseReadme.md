# AvidScript 0.1.0 Developer Preview

AvidScript 是面向 Unreal Engine 5.8 的 C# + WebAssembly 游戏脚本框架。C# 在构建期编译为 WASM，
UE Runtime 不托管 CLR；Win64 使用 Wasmtime，移动端遵守平台 JIT 限制。

## 当前能力

- `BeginPlay`、`Tick`、`EndPlay`、Timer、Overlap、Gameplay Event 与受控 `async/await`。
- 由 Reflection/Profile 生成 `UFUNCTION`、`UPROPERTY`、Blueprint、网络与常用 UE 类型 facade。
- C# 定义 Actor、Component、World/GameInstance Subsystem，并生成对应 UE 类型。
- Win64 Development/Shipping 发布、热重载、UMG/SaveGame、RPC/RepNotify 与多进程网络闭环。
- `release.json` 同时绑定 Win64/Android 依赖 lock、Generated Type 与模块包生产器身份。

## 安装

此包是 thin source developer profile，不包含本机构建的第三方二进制。先将包安装到 UE 项目：

```powershell
pwsh -NoProfile -File .\payload\AvidScript\Build\InstallAvidScriptPluginRelease.ps1 `
  -PackageRoot "C:\Path\To\AvidScript-Release" `
  -ProjectRoot "C:\Path\To\YourProject" `
  -Mode Apply
```

安装后在插件目录准备 Win64 Wasmtime 性能工具链：

```powershell
pwsh -NoProfile -File Build/BuildAvidScriptWasmtimePerformanceToolchain.ps1 -Mode Verify
```

若本机尚未安装，按同一脚本的 `Build` 模式从锁定源码构建。Android 依赖使用：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Verify -Platform AndroidArm64
```

源码版 UE5.8 根目录与固定 .NET SDK 要求见 `global.json` 和发布包 `release.json`。安装前可先使用
`-Mode Plan`，安装后使用 `-Mode Verify`；安装器不会修改 `.uproject`、系统环境变量、游戏资产或
项目 `Saved/AvidScript` 数据。

安装后可从插件目录运行只读 Compatibility Doctor：

```powershell
pwsh -NoProfile -File Build/InvokeAvidScriptCompatibilityDoctor.ps1 `
  -ProjectRoot "C:\Path\To\YourProject" `
  -EngineRoot "C:\Path\To\UnrealEngine"
```

报告使用稳定 JSON code/status/remediation，并区分 blocked、warning 与 not-run，不会把缺失 Android
工具链或未执行设备测试写成通过。

需要提交问题材料时，可在插件目录一次完成诊断和脱敏导出：

```powershell
pwsh -NoProfile -File Build/ExportAvidScriptSupportBundle.ps1 `
  -ProjectRoot "C:\Path\To\YourProject" `
  -EngineRoot "C:\Path\To\UnrealEngine" `
  -OutputRoot "C:\Path\Outside\TheProject\AvidScriptSupport"
```

默认不包含日志；可用 `-LogPath` 显式添加最多 8 份 `.log/.txt`，每份仅保留脱敏后的 64 KiB/200 行尾部。
不要把原始项目、源码、WASM、PDB 或凭据当作日志传入。

## 边界

当前为 `0.1.0` 开发者预览。Android UBT/APK/真机、iOS、Shipping 人工视觉与完整 C#/.NET 兼容层
仍未宣称完成。MIT 协议见 `LICENSE`；Wasmtime 与 WAMR 继续遵守其各自随源码提供的许可证和 NOTICE。
