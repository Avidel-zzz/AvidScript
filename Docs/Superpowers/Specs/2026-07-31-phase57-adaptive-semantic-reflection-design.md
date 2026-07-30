# Phase 57 Adaptive Semantic Reflection 与全绿性能门禁设计

## 1. 背景

Phase 56 的正式证据共有 18 项性能门禁，其中 12 项通过、6 项失败。最主要的
失败项是：

- AvidScript semantic scalar：`649.9125 ns/op`；
- Puerts reflection scalar：`111.6875 ns/op`；
- semantic / Puerts reflection：`5.819026x`；
- 正式目标：`<= 0.80x`，按 Phase 56 denominator 对应 `<= 89.35 ns/op`。

两条路径执行相同整数 workload，但调用层级不同。AvidScript semantic 固定经过
`UObject::ProcessEvent`，Puerts 对安全的原生、非网络、非 Ubergraph UFUNCTION
会进入缓存后的 `UFunction::Invoke`。因此现有数据是产品热态动态绑定路径的真实
端到端比较，但不是相同 Unreal 调用层级的纯 reflection 成本比较。

Phase 57 不通过手写单个 UE API、benchmark fixture 特判或降低语义覆盖来制造领先。
本阶段把动态反射绑定编译成可复用的 prepared call-site，在可以证明调用层级安全时
自动使用 `UFunction::Invoke`，其余情况确定性回退 `ProcessEvent`。

## 2. 已批准的产品决策

Phase 57 采用 **Adaptive Semantic** 作为与 Puerts Reflection fast 对标的正式路径：

- 安全的原生 UFUNCTION 使用 prepared typed import 和 `UFunction::Invoke`；
- Blueprint event、RPC、interface、Ubergraph、脚本函数及其他不满足资格的函数继续
  使用 `ProcessEvent`；
- 严格 `SemanticProcessEvent` 模式永久保留，作为完整 UE 事件语义和独立诊断路径；
- benchmark 和文档必须公开实际 invocation tier，不能把 fallback 或 generated S1
  统计为 adaptive native hit；
- 自动资格判定基于函数 shape、flags、owner 和 descriptor provenance；
- session 显式选择 `AdaptiveSemantic` policy 即构成项目级授权，默认 policy 仍为
  `SemanticProcessEvent`；
  不维护逐 API 手写实现或 benchmark 名称白名单。

## 3. 方案比较

### 3.1 方案 A：Adaptive Semantic（采用）

加载期把 reflection descriptor 编译为不可变 call-site。支持的 shape 进入 Wasmtime
typed import，运行期按资格和失效状态选择 prepared native invocation 或 strict
ProcessEvent fallback。

优点：

- 与 Puerts Reflection fast 属于相同的 `UFunction::Invoke` 层级；
- 保留完整 fallback，覆盖能力不会因性能优化缩水；
- 可逐 shape 扩展，不需要逐 API 手写 wrapper；
- 能复用 Phase 56 fused callback frame 和 prepared typed host target。

代价：

- 必须明确区分 adaptive native 与 strict ProcessEvent 语义；
- 需要严谨处理对象失效、热重载、Blueprint reinstancing 和回调重入；
- 首批只对已证明的 POD shape 发布 typed call-site，其他 shape 仍走 fallback。

### 3.2 方案 B：只优化 ProcessEvent

继续减少 frame 初始化、句柄解析和参数循环，但每次调用仍进入 `ProcessEvent`。

优点是语义最保守。缺点是当前需要 `7.27x` 加速，现有 scalar 已经使用专用 POD
thunk，剩余主要成本是 `ProcessEvent` 本体，达到 `89.35 ns/op` 的概率很低。该方案
保留为 fallback 优化轴，不作为正式领先路径。

### 3.3 方案 C：把 generated S1 当作 reflection

Phase 56 generated S1 scalar 已达到 `27.0785 ns/op`，直接作为 numerator 很容易
通过门禁。但它属于生成式 C++ 强类型绑定，不是 reflection fast，同 Puerts
Reflection 比较会掩盖动态覆盖能力。本阶段禁止这样做。

## 4. 性能合同

Phase 57 冻结 Phase 56 的六项失败门禁，不允许提高阈值：

| 门禁 | Phase 56 | Phase 57 目标 |
| --- | ---: | ---: |
| Adaptive semantic / Puerts reflection | `5.819026x` | `<= 0.80x` |
| Generated S1 scalar | `27.0785 ns` | `<= 25.0 ns` |
| Generated S1 property | `54.3086 ns` | `<= 50.0 ns` |
| Prepared / generic export | `0.995238x` | `<= 0.95x` |
| Wasmtime / V8 P95 geomean | `0.995801x` | `<= 0.95x` |
| Controlled kernel P95 win rate | `0.50` | `>= 0.60` |

正式 semantic gate 仍使用同轮 Puerts denominator：

```text
adaptive_semantic_ns <= 0.80 * puerts_reflection_ns
```

`89.35 ns/op` 只是 Phase 56 原始证据对应的参考绝对预算。Phase 57 正式结论必须来自
同一候选、同一机器、同一协议中的当前 denominator。

不得通过以下方式通过门禁：

- 删除、替换或修改 controlled-runtime kernel；
- 改变 workload、seed、oracle、operation count 或 host-call count；
- 缩短样本、改变 percentile 算法或只选有利进程；
- 把 generated S1、static binding 或 Native C++ 结果记为 adaptive reflection；
- 隐藏 fallback、trap、错误或不正确 observation；
- 修改阈值或在 evaluator 中对候选做特殊判断。

## 5. 架构

### 5.1 Binding Plan Compiler

在 `AvidScriptBindings` 内扩展现有 immutable invocation plan，不创建第二套反射缓存。
加载期新增 adaptive classifier，输出：

- `StrictProcessEvent`：完整语义 fallback；
- `PreparedNativeInvoke`：通过资格证明的 reflection fast；
- typed host shape；
- POD frame layout、参数与返回值 offsets；
- receiver mode、owner class 和 runtime guard；
- reload/effect policy；
- fallback thunk。

首批生产 shape 为：

```text
self + int32 + int32 -> int32
```

它是对架构的第一个垂直切片，不是 fixture 专用实现。classifier 只检查结构和
provenance，不读取函数名。

### 5.2 Adaptive 资格规则

首批 `PreparedNativeInvoke` 至少满足：

- `FUNC_Native`，native function pointer 非空；
- 非 network、event、BlueprintEvent、delegate、Ubergraph、interface 或 script；
- owner 和 target 满足 final 或 exact-class 证明，不能绕过 override；
- 参数和返回值为已证明的 trivial POD layout；
- 无 ref/out、默认参数、局部 frame 属性、构造或析构需求；
- descriptor identity、package hash、selection hash 与当前 reflection snapshot 一致；
- 调用位于 Game Thread，且不处于 GC、PostLoad 或 intra-frame debugging。

任一加载期条件失败时只生成 strict plan。加载期 classifier 可以为全部满足结构证明
的函数准备 candidate，但只有 session 显式选择 `AdaptiveSemantic` 时才能执行；
这项 session policy 是统一项目授权，不要求逐 API 白名单。运行期 guard 失败时执行
安全 fallback 或返回明确错误；不能继续使用失效 native pointer。

### 5.3 Prepared Reflection Call-Site

`AvidScriptRuntime` 为 adaptive plan 建立 prepared host call context，持有：

- binding ordinal 和 immutable plan lease；
- expected owner class 与 `UFunction`；
- typed frame codec；
- receiver/effect policy；
- strict fallback target；
- package、registry 和 reload revision token。

call-site 通过现有 `FAvidScriptVmPreparedTypedHostTarget` 风格接口发布给 VM。VM 层
只认识 typed ABI、opaque context 和 status，不依赖 UObject、UFunction 或 FProperty。

### 5.4 Wasmtime Typed Bridge

Wasmtime 加载时按 import shape 绑定固定 callback，正常路径不再逐 cell 遍历
`EAvidScriptVmValueKind`，也不构造通用 `uint64[64]` 参数数组。首批 callback 直接
接收：

```text
self_slot, self_generation, left, right -> int32
```

成功路径使用 prepared target，失败时才物化错误详情。`FallbackRequired` 必须进入
同一 binding 的 strict fallback，不允许丢失调用或改用不同 API。

### 5.5 Callback-Epoch Receiver Cache

每次 guest callback 第一次调用完成完整 registry resolve、generation 和 class
检查，后续相同 receiver 在当前 callback epoch 内复用。缓存不跨 callback 持有对象
有效性假设。

cache key 至少包含：

- slot 和 generation；
- registry/session/world epoch；
- expected class；
- package/reload revision。

对象销毁、registry reset、successful reload replacement、package revoke、Blueprint
reinstancing、world teardown 或 callback 结束都会使缓存失效。

### 5.6 Invocation

prepared native 路径：

1. typed Wasmtime callback 取得 prepared call-site；
2. receiver cache 命中，或执行一次完整 resolve；
3. 检查 runtime guard 和 host-effect policy；
4. 在栈上或预分配 scratch 中构造 12-byte POD frame；
5. 构造 compiled-in `FFrame`；
6. 调用缓存的 `UFunction::Invoke`；
7. 直接返回 scalar result cell；
8. 累加 adaptive native instrumentation。

strict fallback 路径继续调用 `ProcessEvent`，并累加独立 fallback instrumentation。

## 6. 组件边界

### `AvidScriptBindings`

- 负责 UFUNCTION 分类、frame 证明、prepared plan 和 strict fallback；
- 不负责 VM callback 生命周期；
- 不生成逐 API 手写代码。

### `AvidScriptRuntime`

- 负责 call-site 生命周期、receiver/effect cache、reload/revoke 失效；
- 把 binding plan 转换为 VM 可消费的 opaque prepared target；
- 不复制 reflection classifier。

### `AvidScriptVM`

- 负责 typed import ABI、Wasmtime callback 和成功/失败传播；
- 不包含 UObject、UFunction、FProperty 或项目类型；
- generic dynamic bridge 永久保留。

### Benchmark Harness

- 负责 lane 身份、同轮 Puerts denominator、计数、正确性和 provenance；
- 不拥有产品 fast path；
- diagnostic instrumentation 不能进入 production 默认热路径。

## 7. 其余五项门禁

Semantic 是 Phase 57 的优先主线，但完整关闭还要处理五项独立性能债务。

### 7.1 Generated S1 scalar/property

scalar 与 property 分别只差约 `7.7%` 和 `7.9%`。两项共享 typed host crossing：

- raw trampoline 直接持有 shape-specific prepared target；
- success path 跳过重复 shape 检查和错误容器清理；
- fused callback frame 只保留一个可失效 fast token；
- property set/get 复用 receiver validation。

诊断目标：scalar `<= 24.5 ns`，property `<= 49.0 ns`。

### 7.2 Prepared export

host-to-VM prepared export 与 guest-to-host typed import 是相反方向，单独优化：

- 稳定 call cell；
- active-call/reentrant-unload guard；
- stale token；
- failure-only diagnostic materialization。

产品 Runtime 和 benchmark 必须使用同一 prepared API。诊断目标 ratio `<= 0.94`。

### 7.3 Wasmtime / V8 controlled runtime

这条轴不受 UE reflection 优化影响。保持 12 个冻结 kernel 字节不变，分析 Wasmtime
Cranelift 生成码、调用选项和 P95 抖动。诊断目标：

- P95 geomean `<= 0.94`；
- 至少 `8/12` kernel 的 P95 胜出；
- 任一 kernel ratio `<= 1.05`；
- `mixed_gameplay` 不回退超过 `2%`。

正式目标仍使用原始 `<= 0.95` 与 `>= 0.60`。

## 8. 数据与诊断

新增分段统计只在 diagnostic timing policy 下开启：

- Wasmtime typed callback；
- prepared target dispatch；
- receiver resolve / cache hit；
- effect prepare；
- frame prepare；
- `UFunction::Invoke`；
- strict `ProcessEvent` fallback；
- result propagation。

正式 production 默认只保留无计时的整数计数。逐调用 cycles 采集不得污染正式结果。

需要报告：

- adaptive native plan/hit；
- strict fallback plan/hit；
- guard rejection；
- receiver revalidate/cache hit；
- generated S1 hit/fallback；
- observation、logical operation 和 host crossing count。

## 9. 错误处理

- stale package/lease：拒绝 prepared target，不访问 cached reflection pointer；
- receiver generation 或 class mismatch：执行 strict fallback；若 fallback 也无法安全解析，
  返回原有稳定错误 category；
- GC/PostLoad/debugging：prepared native 不可用，按现有线程和生命周期合同处理；
- UFUNCTION invoke abort：返回 `binding_adaptive_native_aborted`，不得报告成功；
- guest memory 或 result cell 错误：沿用 VM ABI 错误，不吞掉 trap；
- reload/revoke/reinstancing：先失效 call-site，再替换 package；
- fallback 不能递归回到同一个 adaptive target。

## 10. 验证策略

实现阶段按 60–120 分钟功能批次推进，阶段末统一构建和验证。

### 10.1 低成本批次反馈

- parser/schema 合同；
- descriptor 和 typed import identity 检查；
- `git diff --check`；
- 针对 call-site 与 invalidation 的小型非 UE harness；
- 只有 ABI、生命周期或 UAF 风险阻塞时运行最小 UE 探针。

### 10.2 集中审查

完成所有实现后执行一次集中代码与架构审查，重点检查：

- exact-class/final 证明；
- Blueprint、RPC、event 和 interface fallback；
- reload/revoke/reinstancing 失效；
- reentrant callback 与 deferred unload；
- VM 层没有 UE 类型泄漏；
- benchmark 没有修改冻结 workload。

审查后只安排一个集中修复批次。

### 10.3 阶段末 Gate

- 受影响 .NET harness；
- 四模块 no-clean UE5.8 Development Editor build；
- 完整 `Automation RunTests AvidScript`，成功数不得低于 Phase 56 的 317；
- architecture/parser/static contracts；
- diagnostic benchmark；
- 同一 clean candidate 的 5-process formal gameplay、micro 和 controlled-runtime；
- candidate commit/tree、raw SHA-256 和 evaluator revision attestation。

Phase 57 只有六项失败门禁全部通过、正确性与 fallback 合同通过、候选 commit/tree
干净且 evidence 与候选一致时才能关闭。某一优化机制完成但性能未达标时，继续保持
Phase 57 打开并记录债务，不能发布“全面领先”。

## 11. 交付分组

- `P57.0`：规格、状态机、冻结合同与 diagnostic attribution；
- `P57.1`：Adaptive plan classifier 和 prepared reflection call-site；
- `P57.2`：Wasmtime typed reflection bridge 与 receiver/effect fast path；
- `P57.3`：generated S1 scalar/property crossing；
- `P57.4`：prepared export 与 controlled-runtime backend；
- `P57.5`：集中审查、统一 Gate、中文实现报告和发布。

## 12. 非目标

- 不在本阶段完成所有 UE 类型 shape；
- 不手写 FVector、Actor 或其他单个 UE API wrapper；
- 不删除 `ProcessEvent`、generic dynamic bridge 或 reflection fallback；
- 不把 adaptive native 描述为完整 ProcessEvent 语义；
- 不把 generated S1 结果放入 reflection numerator；
- 不通过打包流程扩展阶段范围；PC Development Editor 是当前优先平台；
- 不做与六项门禁无关的大规模重构。
