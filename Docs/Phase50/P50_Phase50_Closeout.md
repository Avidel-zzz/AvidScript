# Phase 50 Typed Project API 收尾报告

> 状态：已通过最终 Gate 并完成 attestation。P50.1 至 P50.5 均已完成；本文与 `P50_Gate_Summary.json` 构成收尾提交，外部不可变 close evidence 由工作流 `close` 命令生成。

## 阶段成果

Phase 50 已把自定义 UE Actor 类型接入现有 C# -> Roslyn -> Guest IR -> WASM -> WAMR -> UE 链路，形成以下能力：

- profile v3 可声明 `self_class_path` 和 typed class reference；
- descriptor v6 发布确定性的 UObject 类型图、继承边、类型 ordinal、`self_type_id` 与 class-reference `result_type_id`；
- package load 阶段一次性解析并缓存 `UClass` plan，热路径不做 class path 或成员名查找；
- `UE.Self` 返回项目自定义 Actor handle，并通过 `avid_owner_get_handle` 一次 crossing 取得 slot/generation；
- `UE.SpawnActor` 根据 `TSubclassOf<T>` 静态类型返回 typed Actor handle；
- 派生 handle 到直接基类、派生 class reference 到基类 class reference 的 upcast 在 Guest 内完成，不进入 Host；
- 基类 handle 到派生 handle 通过通用 `avid_object_type_is_a` 做一次 checked cast crossing；
- 项目自定义 native `UFUNCTION` 继续通过 descriptor、统一 dynamic ABI 和 cached `ProcessEvent` plan 自动覆盖，没有增加逐 API VM wrapper；
- initial load 在 `BeginPlay` 前验证 owner 类型，reload candidate 类型错误时拒绝候选并保留旧 Runtime；
- manifest、prepared semantic、binding slice 和 reload loader 对 reflected、object-type、packed-owner capability 执行精确 provenance 校验。

## Gate 修复

本轮集中验证暴露并修复了以下跨层漂移：

1. PowerShell binding package、prepared semantic 和 Runtime reload loader 原先未同步 descriptor v6 与 packed owner intrinsic。
2. manifest import 数量原先只统计 reflected/lifecycle，未统一包含 object-type 与 packed owner capability。
3. direct ABI 脚本验证错误地要求六个生命周期导出全部存在，现改为只要求脚本声明的 allowlist 导出，并拒绝额外函数导出。
4. 无普通函数的 Self/property/class-reference profile 被错误判定为 `profile_empty`。
5. 生成式 class-reference upcast 未进入 Guest intrinsic lowering；现为同存储类型的零成本转换。
6. `i64` shift count、非 class-ref `default(T)` 与零初始化内存写入缺少完整 lowering/backend 支持。
7. C# emitter 通过版本感知 canonical serializer 与共享 Runtime package loader 保留 schema v2-v5 兼容；新 profile 统一生成 schema v6，旧 descriptor 不再被错误收窄或误报 `descriptor_not_canonical`。
8. generated facade、golden fixture、manifest 计数和 crossing 断言已迁移到当前 v6 合同。
9. package、prepared semantic、Runtime loader 与 Session 对 JSON 整数字段、import identity 和 packed-owner provenance 改为严格类型校验；脚本导入 packed owner 时，package capability 与非空 `ExpectedSelfClass` 必须共同授权，packed 脚本不能借用 legacy/selfless package；package 仍可作为脚本实际 imports 的授权超集。
10. class-reference intrinsic 不只校验 wrapper 类型，还会读取生成的 `ProjectClasses` getter 所发布的字面量 ordinal；伪造同形 wrapper、合法 wrapper 携带任意非负 ordinal 均不能通过 lowering。
11. Direct ABI 构建明确拒绝零导出 Guest IR/WASM，避免生成可构建但 Runtime 必然拒绝的 manifest。
12. schema v5 descriptor 的 class reference 通过 `base_class_path` 与旧 object type 映射重新生成 C# facade，不再错误依赖 v6 的 `result_type_id` 和 object ordinal。
13. packed-owner 校验区分 package 的授权能力与脚本的实际使用：未导入 `UE.Self` 的 DynamicProjectile 可以使用保留 `Self` 类型图的 schema v6 package；一旦脚本导入 owner，package 缺失对应能力则稳定拒绝。
14. WASM inspector 保留每个函数 import 的 module/name，Runtime 在 capability 判断前把真实 WASM import section 与 script manifest 做精确多集合比较；manifest 不能通过改名隐藏真实 packed-owner import，packed-owner handler 也会独立要求带 `ExpectedSelfClass` 的 binding package。
15. WAMR 动态 native registry 的进程全局状态不再被视为当前脚本的授权来源：真实 WASM 与 manifest 身份一致后，每个非静态导入还必须属于当前 immutable binding package；包可以是能力超集，但脚本无法借用另一个会话已注册的 `avid_ue_*`。VM 公开唯一静态导入策略供 registry 与 Runtime 共同使用。

## 可执行证据

真实 typed C# 重放已经生成 WASM：

| 项目 | 结果 |
| --- | --- |
| 输出目录 | `C:\tmp\AvidScript_P50_TypedReplay_R4` |
| 构建结果 | `direct_abi_built` |
| WASM SHA-256 | `2e9f2cd1725afd05894e4c83832a454d504547ffa848ef1bca989275a6e7fe75` |
| 链路 | typed source -> Roslyn semantic -> Guest IR -> WASM |

这证明当前 typed facade 可以被真实 C# 编译链消费；最终冻结 Gate 又通过 250 项完整 UE Automation 验证了 WAMR/UE 生命周期中的整体行为。

## 性能结果

最终 Gate 在 UE5.8 Win64 Development Editor / NullRHI 下的独立采样结果如下：

| 指标 | P50 / P95 |
| --- | --- |
| Native `UObject::IsA` | 0.000018 / 0.000019 ms |
| Binding ordinal checked cast | 0.000041 / 0.000047 ms |
| WAMR checked cast | 0.000890 / 0.001255 ms |
| 既有 typed binding | 0.000056 ms P50 |

20,000 次 typed upcast 的 Host import 为 0；20,000 次 WAMR checked cast 的 crossing delta 精确为 20,000；warm class load 与 reflected-name lookup 均为 0。Packed owner 使 PlayablePickup 单次 Tick crossing 从 6 降到 4，成功 BeginOverlap 从 8 降到 6。

Phase 49 没有同机、同 harness 的冻结 median，因此 `<= 5%` 回退项保持 `pending_same_machine_phase49_baseline`，不会使用异机或不同 workload 数据伪造通过结论。

## 最终 Gate

| 验证面 | 最终结果 | 说明 |
| --- | --- | --- |
| .NET test hosts | 163 / 163 | 固定 SDK 8.0.416，五个共享 graph 宿主串行执行 |
| PowerShell contracts | 108 / 108 | 七组合同宿主 |
| tracked PowerShell parser | 23 / 23 | 全部 tracked `.ps1` |
| Phase 50 architecture fixtures | 17 / 17 | canonical/token/closure/path 负例与正例 |
| 主架构检查 | 通过 | clean detached worktree，19 个冻结输入与主架构边界均通过 |
| `dotnet format` | 5 / 5 | `--verify-no-changes --no-restore` |
| no-clean UBT | 通过 | Bindings、VM、Runtime、Editor 四模块；最终候选 Target up to date；未清理 Editor Target |
| full Automation | 250 / 250 | found/completed/succeeded 均为 250，failed/not-run 均为 0，队列为空，进程退出码 0 |
| typed/object lifecycle benchmark | 通过 | warm class load/name lookup 均为 0，upcast 零 crossing，checked cast 计数精确 |

完整 Automation 的修复历史按原始日志记录：首次完整 Gate 暴露 legacy fixture 漂移并在重入安全审查后触发空 slot 断言；第二次完整 Gate 只剩 `TypedOwnerValidation` 的零 import fixture 错误声明 packed owner；修复为真实 `avid_owner_get_handle` import 后，最终候选完整重跑为 250/250。三次 full Gate、16 次 UBT 与 25 次 Automation 超过原预算，原因已写入不可变 Gate report，未通过省略失败轮次伪造预算合规。

独立复审曾发现一项 Critical：动态 native 回调内重入 unload 会让正在执行的 WAMR module instance 被释放，形成 UAF。Backend 现使用 active-call depth 与 deferred physical unload，Session 拒绝 guest 执行期间的 load/reload/unload 和嵌套执行，Component 也延迟释放 Runtime/Owner；对应 unload、reload、nested call 与 component destroy during tick 回归均已通过。其他跨层风险包括 legacy/selfless package 借用、schema v5 class-reference 重建、合法 wrapper ordinal 重标、Runtime package manifest 的非整数 ordinal、package 授权超集被误当成 script import 精确集合、script manifest 隐藏真实 WASM owner import，以及跨会话借用全局动态导入注册表，均已修复并纳入回归。

## 验收映射

| 验收项 | 当前状态 | 最终证据 |
| --- | --- | --- |
| typed `UE.Self` 与一次 packed-owner crossing | Passed | 最终 Automation 与 crossing 计数 |
| typed Blueprint/native class reference 与 typed Spawn | Passed | 最终 Automation |
| 自定义 native `UFUNCTION` 通用闭环 | Passed | descriptor、真实 C# replay 与最终 Automation |
| upcast 零 crossing | Passed | 最终 benchmark |
| checked downcast 一次 crossing | Passed | 最终 benchmark |
| wrong owner 在 `BeginPlay` 前拒绝 | Passed | 最终 Automation |
| reload candidate 错误时旧 Runtime 保活 | Passed | 最终 Automation |
| warm path 零 class load/name lookup | Passed | 最终 benchmark |
| 无项目专用 wrapper | Passed | clean candidate 架构 checker |
| 中文样例、性能与收尾文档 | Passed | 本收尾提交 |

## 已知边界

- 当前目标是 UE5.8 Win64 Editor；Cook、Shipping、Android 和 iOS 尚未进入本阶段。
- 已覆盖 descriptor 能表达的普通 `UFUNCTION` 与当前 ABI 类型集合，不等于完整 UE API 类型系统。
- Blueprint 子类可以作为 typed class reference 创建并调用 native base 暴露的方法；Blueprint 图中新声明函数、delegate、容器与任意 `UStruct` 尚未覆盖。
- 新 profile 与 descriptor 统一生成 schema v6；当前 emitter 仍可消费合法 schema v2-v5 descriptor 并重建对应旧 facade。schema v5 class reference 可以恢复旧式 `TSubclassOf<T>` 表面，但没有 v6 的 `result_type_id`、object ordinal 与完整 typed inheritance/provenance 图；schema v2-v4 也不具备 typed `Self` 等 v6 能力。

## Gate 身份

| 字段 | 值 |
| --- | --- |
| verified commit | `45a80c5ec91582729eb986b8952329578b9e58b0` |
| verified tree | `180e0092e6ab4a7c9c552e74f9e0ec019cc18f46` |
| gate report | `C:\tmp\AvidScriptPhase50Gate-45a80c5-final\Phase50_Gate.json` |
| gate report SHA-256 | `2ce61fbce2dfbd92133fdef25ef1b83c0985ce38aa0579d0e24b7d424d1c4ac6` |
| Automation | 250 / 250 |
| attestation commit | 本收尾提交 |
| close evidence | `C:\tmp\AvidScriptPhase50Gate-45a80c5-final\Phase50_Close.json` |

## 下一阶段

Phase 51 建议以 UObject/Component 游戏开发闭环为主：通用对象与组件工厂、Outer/ownership policy、组件查询与 Attach/Detach，并复用 Phase 50 的 object type graph、typed handle、checked cast 和统一 descriptor binding，不引入逐 API 手写桥接。

## 当前结论

**Phase 50 已完成。** 冻结候选、集中 Gate、不可变报告 attestation、单独收尾提交与外部 close evidence 共同形成可复核闭环；后续功能演进从 Phase 51 开始。
