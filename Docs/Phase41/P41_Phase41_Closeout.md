# Phase 41 Guest IR 与 WASM 代码生成收尾

## 完成状态

Phase 41 的 P41.0 至 P41.6 已全部完成。C# 游戏脚本默认路径已经从过渡 AST 适配器迁移到正式的 semantic、Guest IR 和 WASM backend 架构。

## 阶段产物

| 小组 | 结果 | 文档 |
| --- | --- | --- |
| P41.0 | GuestIr、CSharpGuest、WasmBackend 三模块边界 | `P41.0_Guest_IR_Wasm_Architecture.md` |
| P41.1 | semantic callable ABI 与 type shape | `P41.1_Semantic_Callable_And_Type_Shapes.md` |
| P41.2 | Guest IR v1、serializer、validator、原子 artifact | `P41.2_Guest_IR_Core_And_Validator.md` |
| P41.3 | 状态、struct、string、enum、array 的确定性布局 | `P41.3_Deterministic_Guest_Memory_Layout.md` |
| P41.4 | C# semantic 到 Guest IR lowering | `P41.4_CSharp_Semantic_To_Guest_IR.md` |
| P41.5 | 确定性 WASM 1.0 backend | `P41.5_Deterministic_Wasm_Backend.md` |
| P41.6 | 默认构建链接入、UE 生命周期闭环与全量回归 | `P41.6_Formal_CSharp_Wasm_Toolchain_Closeout.md` |

## 最终架构

```text
C# / future language frontend
  -> versioned semantic artifact
  -> validated Guest IR v1
  -> deterministic WASM backend
  -> WAMR runtime + generated host bindings
```

Guest IR 与 WASM backend 都不依赖 Roslyn、UE Reflection 或手写 UE API 表。语言前端只负责产生稳定语义，后端只消费通过 validator 的 Guest IR。该边界允许未来增加 `.avid` 或其他语言前端，也允许在不改变语言语义契约的情况下引入结构化 CFG、AOT 与移动端后端。

## 当前可用能力

- C# 生命周期：BeginPlay、Tick、EndPlay；
- Timer、通用事件、碰撞事件与输入事件；
- `AActor`、`USceneComponent`、`FVector`、`FRotator`、`FTransform`；
- UObject handle registry 与 slot/generation 安全引用；
- 脚本状态、局部变量、分支、循环、函数调用、struct、string、enum、array 基础模型；
- manifest 驱动加载、ABI gate、热重载 rollback；
- 四阶段 artifact provenance 与 Editor 结构化诊断。

默认 ActorLifecycle 已通过 UE5.8 WAMR 真实执行和完整 AvidScript automation 141/141。

## 仍需完成

Phase 41 证明并接通了编译与运行闭环，但尚未让任意 C# 脚本自动获得整个 UE API 面。下一阶段必须完成 UE Reflection Binding Generator，从 Reflection 批量生成：

- C# reference assembly/facade；
- 类型与函数 binding descriptors；
- Runtime host stubs；
- UObject、UStruct、属性、参数与返回值 marshalling；
- Editor 增量生成与 schema/version 管理。

自定义源码 profile 在生成器接入前返回 `phase42_binding_required / ASBI4201` 并失败关闭。禁止用恢复旧适配器或逐个手写 API 的方式绕过该边界。

## 验证基线

- Frontend 7/7；
- Semantic 38/38；
- GuestIr 31/31；
- CSharpGuest 15/15；
- WasmBackend 11/11；
- BuildIntegration 3/3；
- BuildPublicationContracts 3/3；
- UE5.8 AvidScript automation 141/141。

Phase 42 从 Reflection schema、稳定 symbol identity 和首批 Actor/Component facade 生成开始。
