# AvidScript / Puerts 性能对标

该目录保存 Phase 53 的可公开 benchmark 源码、固定依赖清单、采样配置和结果 schema。第三方源码、V8 二进制与原始大型日志不会提交到 AvidScript Git。

## 固定对比路径

- Native C++；
- Puerts V8 反射调用；
- Puerts V8 静态绑定；
- AvidScript C# / WASM / WAMR。

## 依赖安装

使用 PowerShell 7：

```powershell
pwsh -NoProfile -File .\Benchmarks\PuertsComparison\Scripts\Install-PuertsBenchmarkDependency.ps1 `
  -Mode Install `
  -ProjectRoot 'C:\Path\To\UnrealProject'
```

安装器只接受 lock 中的 Tencent 官方仓库、固定提交和固定 V8 归档。`Verify` 会重新核对 Git remote、commit/tree、归档长度、SHA-256 和工程插件 marker。

```powershell
pwsh -NoProfile -File .\Benchmarks\PuertsComparison\Scripts\Install-PuertsBenchmarkDependency.ps1 `
  -Mode Verify `
  -ProjectRoot 'C:\Path\To\UnrealProject'
```

`Remove` 只删除带匹配 managed marker 的 `Plugins/Puerts`，拒绝删除用户手工安装或身份不明的目录。

## 结果边界

正式同机 warm baseline 已于 2026-07-26 完成：5 个独立进程、每进程 5 次 warmup 与 30 次正式 sample，共 6,000 条正确样本。当前 WAMR interpreter 路径尚未快于 Puerts；十项 workload 的 AvidScript/Puerts Reflection P50 比率几何平均为 6.99x。

完整环境、分项结果和校准修正见 [P53.3 Puerts 同机 Warm Baseline 报告](../../Docs/Phase53/P53.3_Puerts_Warm_Baseline_Report.md)。

P53.4 已完成首项通用热路径优化：取消成功 handle resolve 中无人消费的 UObject path materialization。正式复测中，涉及 UObject target resolve 的 workload 提升 20.2% 到 34.9%，`vector_value` 相对 Puerts Reflection 达到 `0.92x`。优化前后、被拒绝候选与剩余执行层差距见 [P53.4 通用热路径优化报告](../../Docs/Phase53/P53.4_Generic_Hot_Path_Optimization_Report.md)。

只有正确性、版本、采样和 provenance 合同全部通过的 workload 才能进入正式报告。Cold 启动与稳定内存将单独报告，不与 warm 排名混合。
