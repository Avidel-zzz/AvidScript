# C# 前端策略

## 目标边界

- C# 是当前首选用户语言；Roslyn 只负责解析、语义和诊断，不是运行时。
- 前端输出规范化 Guest IR，再由 WASM backend 生成模块。UE Runtime 不嵌入 CLR，也不依赖 Roslyn。
- C# 语义、Guest IR、WASM ABI 与 UE binding 是四个可版本化合同，不能用跨层特例绕过。

## 实现规则

- 语义能力先落入统一 capability/contract，再扩 lowerer、validator、emitter、fixture 和版本 owner。
- 不把平台或 UE 反射知识塞入通用 C# 语义层；UE 类型通过生成 facade 和稳定 descriptor 暴露。
- 新语法必须更新 symbol projector、控制流、async state、诊断和兼容性读取者。
- 生成 C# API 应可重建、确定性排序、跨 assembly 编译；不逐个手写 UE API wrapper。
- 自执行 `*.Tests.csproj` 使用固定 SDK 的 `dotnet run --configuration Release`，证据包含显式 passed/total。

## 性能

- 编译缓存键包含源码、引用、语义 Schema、生成器版本和目标 ABI。
- 热路径避免运行时字符串查找、重复 descriptor 解析和每次调用分配。
- async/continuation 由 Session 生命周期拥有，取消与 reload 必须能清理挂起状态。
