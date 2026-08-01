# P57.11B1 递归固定宽度 USTRUCT Codec 设计

## 1. 目标

P57.11B1 让项目自定义 `UFUNCTION` 和 reflected property 在参数图只包含固定宽度安全
类型时，无需手写 wrapper 即可生成 C# API，并通过 P57.11A prepared dynamic target 执行。
支持 value、const-ref、ref、out 和 return，且调用成功路径不恢复 ordinal/package lookup。

首批允许的叶子类型为：全部现有 scalar、enum、`UObject` capability、`FVector`、
`FRotator`、`FTransform`，以及递归组合这些叶子的用户 `USTRUCT`。

## 2. 方案

采用 schema 9 的 descriptor type graph。用户结构类型使用 `struct_wire` kind，并发布：

- canonical struct path、稳定 type id；
- wire size/alignment；
- 按反射声明顺序冻结的字段名、child type id 和 wire offset；
- Core Wasm ABI `{ i32 guest_address }`。

用户结构不暴露 UE native size、padding 或 LWC 布局。Editor 按 child codec 的稳定 wire
size/alignment计算布局；Bindings 在加载期重新解析 `UScriptStruct/FProperty` 并验证字段图，
随后编译为不可变递归 codec program。C# 使用显式 wire layout，所有方向共享同一字节合同。

## 3. 安全边界

- 最大递归深度 `8`，每个根参数图最大字段节点 `128`，最大 wire size `4096` bytes。
- 字段必须是反射字段并具备 `CPF_BlueprintVisible`；拒绝 transient、editor-only、delegate、
  instanced reference、容器、soft/weak/lazy object 和任何未注册 child codec。
- offset、size 和地址计算使用溢出检查；字段区间不得重叠，最终 size 必须满足 alignment。
- descriptor 的 struct field graph 进入 type stable id 与 package hash；字段名、child type 或
  wire offset 被篡改时解析阶段 fail closed。
- schema 2-8 继续按旧合同加载；只有实际包含 `struct_wire` 的新 package 升级到 schema 9。
- Blueprint event、RPC、interface 和普通反射调用继续使用 `ProcessEvent`，不会因结构 codec
  自动升级到 native thunk。

## 4. 模块职责

### AvidScriptEditor

- `ReflectedTypePolicy` 递归投影固定宽度字段图并计算 wire layout。
- `BindingDescriptorGenerator` 递归收集 child types、生成 schema 9 和稳定 identity。
- `CSharpBindingRenderer` 为 `struct_wire` 生成 `[StructLayout(LayoutKind.Explicit)]` public
  readonly struct；value/const-ref 的 native 声明使用 `in T`，ref/out/return 使用现有地址 ABI。

### AvidScriptBindings

- descriptor parser 验证 schema 9 字段图、child type 引用、深度、节点数、区间和 identity。
- package load 把 `FStructProperty` 和字段 `FProperty` 编译为递归 immutable program。
- executor 按 wire offset 递归 decode/encode，不按 canonical string 或字段名称动态选择 codec。

### AvidScriptRuntime 与 VM

不新增 UE 类型逻辑。Runtime 继续提供 guest memory、object capability、scratch、host effect
和 package lifecycle；VM 继续只调用 opaque prepared target。

## 5. 数据流

1. Editor 从 UFunction 参数或 property 得到根 `FStructProperty`。
2. type policy 递归投影字段，先完成 child type，再确定父 wire offset、size 和 stable id。
3. C# facade 把 public struct 以固定 wire layout 放入 guest memory，并传递地址。
4. prepared codec 在 side effect 前验证地址和 object capability，并递归写入 UE frame。
5. `ProcessEvent` 执行一次。
6. ref/out/return 递归写回同一 wire contract；不会为探测容量重放 UFUNCTION。

## 6. 验收

- 一个嵌套两层、同时包含 scalar、enum、`FVector` 和 `UObject*` 的用户 `USTRUCT`，可用于
  自定义 UFUNCTION 的 value、const-ref、ref、out 和 return。
- 同一结构可用于 reflected property get/set。
- 生成 C# 类型字段 offset、总 size、方法 public signature 和 native `in/ref/out` ABI 与
  descriptor 完全一致，并能通过 C# 编译合同。
- Runtime fixture 实际执行 UFUNCTION，验证输入、ref/out、return、object capability 和
  prepared-dynamic hit，dispatcher fallback 为 `0`。
- 字段 type id、offset、递归深度、重叠区间和不安全字段篡改均 fail closed。
- 阶段末统一执行 UE 5.8 no-clean build、focused Automation、完整 AvidScript Automation、
  架构合同与 P57.10 性能回归合同。

## 7. 非目标与后续

- P57.11B1 不实现 `FName/FString/FText` 输出、容器、delegate、latent action 或 RPC facade。
- P57.11B2 引入 session-owned UTF-8 value heap，完成 `FName/FString` value/ref/out/return。
- P57.11C 冻结 mixed numeric、user struct 和 text workload，并统一量化 Tier 1 相对旧
  dispatcher 与 Puerts Reflection 的性能。
