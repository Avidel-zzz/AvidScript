# Phase 57 Editor 预编译制品管线设计

## 1. 目标与范围

P57.7 已实现 `WasmtimeSerialized` 的 Runtime 消费能力，但制品仍由测试直接调用 C shim
生成。P57.8 要打通 PC Editor 的自动生产与热重载选择闭环：

```text
C# source -> canonical WASM -> VM artifact compiler
-> serialized Wasmtime artifact + provenance
-> Editor 原子发布
-> Runtime 验证来源并选择 precompiled/JIT
-> BeginPlay/Tick/事件沿用现有生命周期
```

本批只支持 UE 5.8 Win64 Editor。移动端 AOT、发布包离线签名、LLVM AOT 和自定义
Wasmtime inlining build 不在本批实现，但接口必须为它们保留后端无关扩展点。

本设计不修改当前用户正在编辑的 `AvidScriptWasmReloadTypes.h`。旧 manifest、旧
`LoadInitialModule(Bytecode, Manifest)` 和 WAMR 路径必须保持兼容。

## 2. 方案比较

### 方案 A：直接调用 Wasmtime CLI

Editor 启动 `wasmtime compile`，再读取 `.cwasm`。实现快，也能复用 CLI 的高级 flag；
但引入额外进程、路径发现、版本漂移与参数字符串合同，并且 Runtime C API engine 的完整
配置必须与 CLI 完全一致。它适合作为后续自定义工具链或 CI producer，不适合作为首个
Editor 内建 owner。

### 方案 B：PowerShell 构建脚本内后处理

`BuildCSharpActorLifecycle.ps1` 在生成 WASM 后调用编译器并直接扩展 manifest。它能够与
C# 构建一起落盘，但会把 VM 私有格式、目标平台和 Wasmtime 版本耦合到 frontend 脚本，
未来增加 LLVM/mobile lane 时会继续膨胀。

### 方案 C：VM 原生 artifact compiler（采用）

`AvidScriptVM` 提供后端无关的拥有型 artifact 与 compiler 服务，Wasmtime 实现在 VM
Private 层复用同一 C shim 和 engine 配置。Editor 只负责调用、事务发布与报告，Runtime
只负责加载、验证与选择。该方案模块边界最清楚，且后续可增加 LLVM AOT compiler 而不
改 C# frontend。

## 3. 模块设计

### 3.1 VM artifact 模型

新增独立公开头 `AvidScriptVmArtifact.h`，不继续扩大 `AvidScriptVmBackend.h`。主要类型：

- `FAvidScriptVmOwnedArtifact`：拥有 execution/canonical 两份字节、SHA-256、compiler
  identity、target triple、format 和 trust；可生成生命周期受自身约束的
  `FAvidScriptVmArtifactView`。
- `FAvidScriptVmArtifactCompileRequest`：canonical WASM、backend、format 与优化策略。
- `FAvidScriptVmArtifactCompileResult`：artifact、cache hit、compile timing、结构化错误。
- `CompileAvidScriptVmArtifact(...)`：通用编译入口；首个实现只接受
  `Wasmtime + WasmtimeSerialized + Aot`。

编译服务必须通过生产 `avidscript_wasmtime_engine_new` 创建 module，再调用 serialize；
不能复制另一份 engine flag。生成结果的 compiler identity 与 Runtime backend 使用同一
identity producer，避免“能生成但不能反序列化”的配置漂移。

### 3.2 进程内 attestation

Wasmtime 官方 serialized module 不能接收任意不可信输入。项目 manifest 与 SHA-256
可证明完整性，但本地攻击者可以同时替换文件和摘要，不能单独产生信任。

artifact compiler 在成功 serialize 后生成随机 128-bit attestation id，并在 VM 内部
有界 registry 中记录：

```text
attestation id
execution SHA-256
canonical SHA-256
compiler build identity
target triple
artifact format
```

Runtime 只能调用 `AuthorizeAvidScriptVmArtifact(...)` 校验完整 tuple；没有公开“注册任意
字节”的接口。Editor 重启后 registry 为空，因此磁盘上的旧 attestation 自动失效并回退
canonical JIT，绝不猜测 serialized 兼容性。

registry 使用互斥保护和固定容量 LRU；重复 canonical identity + compiler identity 的编译
可命中进程内 cache，避免同一热重载会话重复 Cranelift 编译。

### 3.3 Editor producer

新增 `CSharpBuild/AvidScriptEditorVmArtifactPublisher`，职责限定为：

1. 在 final C# build 成功且 canonical WASM/manifest 已验证后读取 WASM；
2. 调用 VM artifact compiler；
3. 原子写入 `<artifact>.wasmtime.cwasm`；
4. 原子扩展 manifest 的可选 `execution` 对象；
5. 把 compile/cache/timing/provenance 写入 `FAvidScriptEditorCSharpBuildResult`；
6. 失败时按策略回滚或保留 canonical JIT。

`execution` 对象采用向后兼容的 manifest schema v1 可选扩展：

```json
{
  "execution": {
    "format": "wasmtime_serialized_v1",
    "file": "actor_lifecycle.wasmtime.cwasm",
    "sha256": "...",
    "canonical_sha256": "...",
    "compiler_build_identity": "...",
    "target_triple": "x86_64-pc-windows-msvc",
    "attestation_id": "32 lowercase hex chars",
    "policy": "prefer_precompiled",
    "fallback": "wasmtime_jit"
  }
}
```

manifest 中的 `attestation_id` 只是 registry lookup key，不是签名或信任证明。

### 3.4 构建事务与策略

`GetAvidScriptCSharpCommittedArtifactPaths` 把 `.wasmtime.cwasm` 纳入现有 backup/rollback
事务。Publisher 必须在 `FinishAvidScriptCSharpArtifactTransaction(..., true)` 前执行，
因此 manifest patch、WASM、report 和 serialized artifact 要么一起提交，要么恢复旧版本。

新增策略枚举：

- `JitOnly`：不生成预编译制品；
- `PreferPrecompiled`：默认 Editor 策略；编译失败时发布 canonical manifest，并记录结构化
  warning；
- `RequirePrecompiled`：用于 CI/未来 packaged build；失败即整批回滚。

Bootstrap 构建固定 `JitOnly`，避免为了生成 binding slice 做无意义的双重预编译；只有
final artifact 进入 precompiler。Prefer 编译失败时必须删除本次输出位置上旧的 cwasm，
并发布不含 `execution` 的新 canonical manifest，不能让旧制品看起来仍属于新 WASM。
`execution.policy` 持久化为 `prefer_precompiled` 或 `require_precompiled`，Runtime 不从
Editor 默认值推断发布合同。

### 3.5 Runtime loader 与执行选择

新增 `AvidScriptRuntimeArtifact.h/.cpp`，不修改旧 manifest value object。Loader 先复用
`FAvidScriptWasmReloadManifestLoader` 完成 canonical WASM、import、binding package、
debug map 和 SHA 验证，再解析可选 `execution`：

- 无 `execution`：保持旧 backend selection；
- `execution` 合同合法且 attestation tuple 命中：返回拥有型 serialized artifact，选择
  `Wasmtime + Aot + WasmtimeSerialized`；
- attestation 失效、cwasm 缺失或 tuple 不匹配：不调用 deserialize，按 manifest 声明选择
  canonical `Wasmtime + Jit + WasmBytecode`；
- canonical WASM 本身失败：仍整体拒绝，不能用 cwasm 绕过 manifest/import 合同。

`FAvidScriptWasmRuntimeInstance` 增加拥有型 artifact 加载入口，内部与现有 `LoadModule`
共享 import、binding、metrics、lifecycle 和清理实现。`FAvidScriptRuntimeSession` 增加
artifact overload；`UAvidScriptComponent` 切换到新 loader。旧 bytecode overload 保留给
测试和嵌入模块。

## 4. 错误与回退合同

| 情况 | `PreferPrecompiled` | `RequirePrecompiled` |
| --- | --- | --- |
| canonical C# build 失败 | 失败并回滚 | 失败并回滚 |
| precompile 失败 | JIT + warning | 失败并回滚 |
| manifest patch/atomic write 失败 | 失败并回滚 | 失败并回滚 |
| cwasm hash 不符 | 不 deserialize，JIT | 不 deserialize，Runtime 拒绝 |
| attestation 不存在 | JIT | Runtime 拒绝 |
| compiler/target 不匹配 | JIT | Runtime 拒绝 |
| canonical import/layout 不合法 | Runtime 拒绝 | Runtime 拒绝 |

所有错误使用稳定 category，不把 Wasmtime 原始错误文本当控制流。回退必须记录
`requested_backend`、`selected_backend` 与 fallback reason，不能静默降低性能等级。

## 5. 性能与缓存

本批改善的是 compile/load 路径，不改变 frozen 12-kernel steady-state 字节。验收指标：

- 同进程第二次编译相同 canonical WASM 必须命中 cache；
- serialized `ModuleLoadMs` 相对 canonical JIT 的同进程配对 P50 目标 `<= 0.50`；
- BeginPlay/Tick 执行结果、imports、binding instrumentation 与 JIT 完全一致；
- cache key 必须包含 canonical SHA、compiler identity、target 和 format；
- registry/cache 固定容量，不允许热重载无界增长。

该启动收益不能替代 `P57-D06-ControlledLeadership` 的 steady-state p95 与 kernel win-rate
门禁。compiler inlining 和选择性 IR pass 在 producer 闭环稳定后单独接入。

## 6. 验证策略

集中 Gate 包含：

1. VM artifact compiler：真实 serialize、cache hit、attestation tuple、错误 format；
2. Editor publisher：atomic cwasm/manifest、Prefer fallback、Require rollback、bootstrap
   不预编译；
3. Runtime loader：verified serialized、过期 token JIT fallback、hash/target/compiler
   mismatch、canonical 合同不可绕过；
4. RuntimeSession：BeginPlay、Tick、事件、reload rollback 和状态迁移在两条执行 lane 一致；
5. UE 5.8 Win64 no-clean 构建；
6. 一次聚焦 Automation；
7. clean candidate 架构检查；
8. 同进程 JIT/serialized load-time 配对 diagnostic。

测试随功能编写，构建、Automation 与 benchmark 在批次集成后统一执行，不为每个小函数
重复启动 Editor。

## 7. 非目标与后续

- 不手写 UE API wrapper；本批只处理通用执行制品。
- 不修改 controlled benchmark kernel、seed、oracle 或门禁。
- 不把进程内 attestation 宣称为 packaged signature。
- 不在 Runtime 里调用 compiler。
- 不在移动端启用运行时 JIT。

P57.9 将为 packaged artifact 引入离线签名/公钥验证，并把自定义 Wasmtime compiler
identity（含 inlining/CPU feature policy）接入同一 producer/loader 合同。
