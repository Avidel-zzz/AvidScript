# P57.11C 数组批量传输与性能协议设计

## 1. 目标

P57.11C 在 P57.11B3 的通用数组 value capability 之上，消除热循环中每个元素一次
host crossing 的主要固定成本。新增能力必须继续由元素类型图驱动，不能为某个 UFUNCTION、
项目类型或 benchmark fixture 增加专用分支。

本批次交付三件事：

- 为任意已支持固定宽度元素提供连续区间 read/write ABI；
- 在生成的 C# API 中提供显式 `Snapshot`/`Flush` 批量入口，并保留原逐元素 API；
- 建立独立的数组正确性与性能证据，分别报告 crossing 压缩、批量吞吐和 Puerts 对比。

## 2. ABI 冻结

数组共享 capability 从四项扩展为六项：

| stable id | import | signature | 语义 |
|---|---|---|---|
| `avidscript.value_array_length.v1` | `avid_value_array_length` | `(i)i` | 读取 capability 长度 |
| `avidscript.value_array_load.v1` | `avid_value_array_load` | `(iiii)i` | 读取一个元素 |
| `avidscript.value_array_store.v1` | `avid_value_array_store` | `(iiii)i` | 写入一个元素 |
| `avidscript.value_array_read_range.v1` | `avid_value_array_read_range` | `(iiiii)i` | capability 连续区间复制到 guest 线性数组 |
| `avidscript.value_array_write_range.v1` | `avid_value_array_write_range` | `(iiiii)i` | guest 线性数组连续区间写回 capability |
| `avidscript.value_release.v1` | `avid_value_release` | `(i)i` | 显式释放 capability |

`read_range` 参数依次为 capability token、capability 起始下标、guest array reference、
guest 起始下标、元素数量；`write_range` 保持相同物理参数顺序，只改变传输方向。成功返回
`1`，失败返回 `0` 并进入现有 fail-closed host import 错误通道。`count == 0` 是合法空操作，
但仍验证 token、两个起始下标和数组长度。

manifest 若使用 array type，必须精确授权完整六元组。旧 schema v10 package 的四元组只用于
旧制品加载兼容；新生成 package 不允许缺少 range ABI，也不允许同名不同 identity。

## 3. 内存与安全

Runtime 先 resolve capability，取得可信的 element count、stride 与 alignment，再解析 guest
线性数组的 4-byte little-endian 长度头。payload offset 使用与 Guest IR 完全一致的
`Align(4, max(4, element alignment))` 规则。

所有下列计算使用 64 位中间值并在借用 guest memory 前完成：

- capability 区间 `start + count`；
- guest 区间 `start + count`；
- `count * stride`；
- array reference、payload offset 与 byte offset 相加；
- 最终 guest address 不得超过 `uint32` 地址空间。

单次传输继续受 array heap 的 `4096` 元素和 `1 MiB` payload 上限约束。guest reference 带
high-bit capability tag、伪造 token、stale token、负下标、越界、错误对齐、坏长度头和地址
溢出全部拒绝。Host 不暴露 `FScriptArray`、`TArray` data pointer 或长期借用；borrow 只在同步
dispatch 内有效。

## 4. Heap 与 VM 分层

`FAvidScriptArrayValueHeap` 新增连续区间读写，负责 token ownership、区间和精确 byte width
验证；Runtime 负责 guest array layout 与 guest memory borrow；VM backend 只把五个 i32 参数
投影到统一 `FAvidScriptHostCall`：

- `IntArgs[0]`：token；
- `IntArgs[1]`：capability start；
- `GuestAddress`：guest array reference；
- `IntArgs[2]`：guest start；
- `IntArgs[3]`：count。

WAMR native registration 与 Wasmtime static catalog 使用同一 import identity 和参数映射。
range ABI 不进入 dynamic reflection ordinal，也不引入后端专属语义。

## 5. C# Surface

生成器为每个唯一 `T[]` 类型输出：

```csharp
bool ok = AvidScriptArray.Snapshot(hostArray, hostIndex, localArray, localIndex, count);
bool flushed = AvidScriptArray.Flush(localArray, localIndex, hostArray, hostIndex, count);
```

`Snapshot` 表示 capability 到 guest 线性数组，`Flush` 表示 guest 线性数组到 capability。
显式命名避免把 capability 伪装成 CLR GC array，也使性能关键代码能清楚控制 crossing 数量。
`AvidScriptValue.Release` 继续负责生命周期，不因 snapshot 自动释放来源 capability。

首版只要求现有受控 C# 子集能够构造的线性数组进入真实 WAMR/Wasmtime E2E。底层 range ABI
对 scalar、enum、常用 UE value、object handle 和固定宽度 user struct 使用同一连续 byte
协议；更完整的动态 `new T[n]`、自动 loop snapshot 和 capability-to-capability copy 依赖后续
guest allocator，不在本阶段用 1 MiB 固定 frame 模拟。

## 6. 性能协议

数组性能独立于既有 pure Wasmtime/V8 与 UE reflection 五 lane 结论，禁止跨族外推。至少记录：

- 逐元素 load、逐元素 store 的 host call 数与 ns/element；
- bulk read、bulk write 在 1、4、16、64、256、1024 元素下的 ns/element；
- 相同 payload 的 old/bulk crossing reduction，N 个元素应从 N 次降为 1 次；
- UFUNCTION roundtrip、property get/set、release 不被 bulk 优化改变正确性或 ownership；
- Puerts Reflection/JS Array 或 TypedArray 的同方向复制，明确容器表示和是否包含分配。

正确性 oracle 必须比较完整输出 bytes、写回后的 capability bytes、host call 计数和 release 后
live value 数。数组/Puerts 正式门禁只在同机、同候选、同轮数据存在后冻结；在此之前只发布
diagnostic 数字，不宣称数组全面领先。

## 7. 阶段 Gate

- 两个 range import 在 manifest、Binding Slice、VM catalog、WAMR、Wasmtime 与 Runtime 一致；
- forged/stale/cross-session token 和所有 guest/capability 越界均 fail closed；
- 真实 C# fixture 在 WAMR 与 Wasmtime 各完成 snapshot、修改、flush、逐元素复核与 release；
- 工具链 focused tests、完整 AvidScript Automation、UE5.8 no-clean Editor target 通过；
- 数组 benchmark 输出 correctness、crossing count、样本统计和 provenance；
- 旧数组逐元素 API 与既有非数组性能门禁无回退；
- 中文阶段报告、evidence、state、README 能力边界与 Git 推送完成。

## 8. 后续

P57.11D 优先建立 bounded guest allocator 和自动 snapshot/dirty flush 优化，使普通 C# 数组循环
无需手写 facade 也能进入批量路径。随后扩展 string element、nested container、`TSet/TMap`；
delegate、latent、RPC 与 interface dispatch 继续作为独立调用语义推进。
