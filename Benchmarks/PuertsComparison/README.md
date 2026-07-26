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

本目录尚未生成正式同机数据前，不代表 AvidScript 已快于 Puerts。只有正确性、版本、采样和 provenance 合同全部通过的 workload 才能进入最终报告。
