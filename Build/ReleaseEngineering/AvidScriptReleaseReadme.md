# AvidScript 0.1.0 Developer Preview

AvidScript 是面向 Unreal Engine 5.8 的 C# + WebAssembly 游戏脚本框架。C# 在构建期编译为 WASM，
UE Runtime 不托管 CLR；Win64 使用 Wasmtime，移动端遵守平台 JIT 限制。

## 当前能力

- `BeginPlay`、`Tick`、`EndPlay`、Timer、Overlap、Gameplay Event 与受控 `async/await`。
- 由 Reflection/Profile 生成 `UFUNCTION`、`UPROPERTY`、Blueprint、网络与常用 UE 类型 facade。
- C# 定义 Actor、Component、World/GameInstance Subsystem，并生成对应 UE 类型。
- Win64 Development/Shipping 发布、热重载、UMG/SaveGame、RPC/RepNotify 与多进程网络闭环。
- `release.json` 绑定所选平台的依赖 lock、Generated Type 与模块包生产器身份。

## 安装

以 `release.json.profile` 为准：`source-developer` 是需要另行准备 Wasmtime 的源码包；
`win64-offline` 是包含 Win64 Wasmtime headers、DLL/import library、完整声明及 WAMR 静态库的源码包。
后者安装和 UE 编译时无需下载这些运行库，但不包含 UE、Visual Studio、.NET SDK 或 NuGet 缓存；
这些工具链和 C# 构建依赖仍需预先准备。先将包安装到 UE 项目：

```powershell
pwsh -NoProfile -File .\payload\AvidScript\Build\InstallAvidScriptPluginRelease.ps1 `
  -PackageRoot "C:\Path\To\AvidScript-Release" `
  -ProjectRoot "C:\Path\To\YourProject" `
  -Mode Apply
```

安装后可在插件目录验证 Win64 Wasmtime 性能工具链：

```powershell
pwsh -NoProfile -File Build/BuildAvidScriptWasmtimePerformanceToolchain.ps1 -Mode Verify
```

`win64-offline` 已携带运行库，不需要运行 `Build`。`source-developer` 若缺少运行库，按同一脚本的
`Build` 模式从锁定源码构建。Android 仅属于源码包的独立平台范围，依赖使用：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Verify -Platform AndroidArm64
```

源码版 UE5.8 根目录与固定 .NET SDK 要求见 `global.json` 和发布包 `release.json`。安装前可先使用
`-Mode Plan`，安装后使用 `-Mode Verify`；安装器不会修改 `.uproject`、系统环境变量、游戏资产或
项目 `Saved/AvidScript` 数据。

离线包使用 release schema v2；源码包继续使用 v1。安装器在写入前重新核对包文件清单、来源锁文件、
运行库 managed marker、全部内容哈希及声明与 DLL 的绑定，缺失或篡改会拒绝安装。
当前 preview 不提供数字签名；内容身份和完整性校验不等同于发布者身份认证。

安装后可从插件目录运行只读 Compatibility Doctor：

```powershell
pwsh -NoProfile -File Build/InvokeAvidScriptCompatibilityDoctor.ps1 `
  -ProjectRoot "C:\Path\To\YourProject" `
  -EngineRoot "C:\Path\To\UnrealEngine"
```

报告使用稳定 JSON code/status/remediation，并区分 blocked、warning 与 not-run，不会把缺失 Android
工具链或未执行设备测试写成通过。

发布候选可使用同一 `release.json` 运行分层平台 Gate：

```powershell
pwsh -NoProfile -File Build/InvokeAvidScriptPlatformReleaseGate.ps1 `
  -PackageRoot "C:\Path\To\AvidScript-Release" `
  -Mode Inspect `
  -ProjectRoot "C:\Path\To\YourProject" `
  -EngineRoot "C:\Path\To\UnrealEngine"
```

`Inspect` 不启动构建或设备任务；`Execute` 仅执行显式计划文件启用的层。Shipping 视觉与物理输入始终保留为
独立人工 Gate。

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
