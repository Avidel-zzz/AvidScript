# Phase 63 收尾记录

状态：候选冻结前，待集中 Gate

## 交付结果

- catalog v2 以 `platform/architecture/configuration/backend/format` 选择唯一模块变体，
  同一逻辑 ModuleId 可并存 Win64 与 Android 产物。
- Android arm64 Wasmtime 45 静态依赖由 lock、archive hash、ELF AArch64 machine 和
  managed marker 共同约束，Android 不 staging Win64 DLL。
- Windows 受控 Wasmtime 工具链启用 `all-arch`，C# Release 可真实交叉生成
  `aarch64-linux-android` AOT artifact；目标 triple、CPU profile、runtime hash 和 policy
  进入内容身份与 attestation。
- Android Development/Shipping 均要求 `wasmtime_serialized_v1`，不允许设备端 JIT 或
  loose WASM fallback；多平台 catalog 不会阻塞 Win64 Editor staging。
- Runtime 统一处理后台暂停、前台恢复、低内存和 World teardown。恢复前校验 Runtime、
  module、World 与 owner generation；失效 Session 取消 Timer/continuation 后无 Guest 回调卸载。

## 集中 Gate 条件

- Agent Harness audit 与干净提交 architecture check；
- 固定 .NET 8.0.416 全套测试；
- 发布、package、Android dependency、Wasmtime toolchain、Cook/receipt 与 workflow 合同；
- UE5.8 Win64 Development no-clean UBT；
- 完整 `AvidScript.*` Automation，要求 Found、Completed、Succeeded 完全相等，
  `0 Failed`、`0 NotRun`、Queue Empty 和进程退出码 `0`。

## 平台边界与转移项

- Phase 63 只能声明 Android arm64 **host cross-AOT 发布路径与 UE 集成合同完成**。
- 当前主机的 UE5.8 Android SDK 校验仍为无效，Android UBT、APK、安装、启动时间、
  包体、输入/图形和真实后台恢复进入 Phase 64 设备 Gate。
- iOS AOT、远程 Mac toolchain、签名和真机验收尚未实现，继续保留为后续平台矩阵工作。
- 自动化不替代移动设备、真实玩法、网络拓扑和长时间运行的人类验收。
