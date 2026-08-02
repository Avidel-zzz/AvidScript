# P57.11B2 变长 UTF-8 Value Heap 与字符串 Codec 设计

## 1. 目标

P57.11B2 在 P57.11A prepared dynamic target 与 P57.11B1 递归 codec program 之上，
补齐 `FName` 和 `FString` 的 value、const-ref、ref、out、return 以及 reflected property
get/set。生成的 C# facade 继续使用自然的 `string`，自定义 `UFUNCTION` 不需要手写 API。

本阶段还要建立可复用的 session-owned 变长值基础设施，后续 byte blob、数组和容器 codec
可以复用其代际 token、去重、事务回滚和生命周期边界，而不是为每种 UE 类型重复造堆。

## 2. 方案选择

采用 **tagged UTF-8 value reference + session-owned value heap**：

- `i32` 最高位为 `0`：值是现有 guest linear memory 地址，内容保持
  `[u32 byte_length][UTF-8 payload][NUL]`；字符串字面量继续走这一条零分配路径。
- `i32` 最高位为 `1`：值是 value heap token，剩余位编码 slot 与 generation；token
  只能由当前 runtime session 的 registry 解析。
- ref/out/return 的 guest storage 始终是一个 4 字节 value reference。宿主在
  `ProcessEvent` 前只需 preflight 固定地址，之后把动态结果 intern 到 value heap 并写回 token。
- `FName` 和 `FString` 共用 token/heap，但使用不同 codec 规则。`FName` 保留长度、无内嵌
  NUL 与 canonical UTF-8 约束；`FString` 接受完整长度驱动的 UTF-8 文本。

不采用 guest 固定容量 buffer，因为输出长度只能在 `ProcessEvent` 后确定；不采用宿主回调
guest allocator，因为会引入 VM 重入、memory growth 和 borrowed view 失效问题；不把裸 UE
`FString` 地址暴露给 Wasm，因为它破坏沙箱与跨 backend ABI。

## 3. Token 与 Heap 合同

### 3.1 Token

- token 为单个 `uint32`，按 `1-bit tag + 15-bit generation + 16-bit slot` 编码；guest/C#
  仍以 `string` 的 `i32` storage 搬运。同一 session 最多同时保存 `65535` 个 unique value。
- slot 与 generation 均不得为零；空字符串是合法 heap value，不使用零 token 表示。
- registry reset 必须推进 generation domain，使 unload/reload 前的 token fail closed。
- 无效 tag、越界 slot、generation 不匹配和未提交 slot 均返回稳定错误类别，不降级为 guest
  地址读取。

### 3.2 Heap

- heap 由 `FAvidScriptWasmRuntimeInstance` 持有，codec 仅通过
  `FAvidScriptBindingInvocationContext` 中的抽象服务访问。
- entry 保存 immutable canonical UTF-8 bytes；相同 bytes 在同一 session 内 intern 为同一
  token，避免 Tick 中重复读取名称造成线性增长。
- heap 设定明确的 slot 上限并在 host side effect 前预留输出 slot。若无法预留，调用不得进入
  `ProcessEvent`。
- output transaction 记录本次新建或预留的 token；后续任一输出失败时回滚，全部 guest write
  成功后统一 commit。
- unload、load replacement 和 runtime 销毁都 reset heap；VM backend 不感知 UE 字符串类型。

P57.11B2 暂不暴露手动 release。自动 interning 与 session reset 先保证常见游戏循环稳定；
跨帧精细引用计数和编译器插入 release 将作为后续 value-lifetime 批次处理。

## 4. 类型与 ABI

### FName

- canonical type：`name:fname`
- descriptor kind：`name_utf8`
- C++ type：`FName`
- storage：size `4`、alignment `4`、ABI `{ i32 }`
- 支持方向：value、const_ref、ref、out、return

### FString

- canonical type：`string:fstring`
- descriptor kind：`string_utf8`
- C++ type：`FString`
- storage：size `4`、alignment `4`、ABI `{ i32 }`
- 支持方向：value、const_ref、ref、out、return

普通 value/const-ref 参数的 ABI cell 直接携带 value reference；ref/out/return 的 ABI cell
携带 4 字节 storage 地址。property getter 也写回 value reference，setter 接收 value reference。
字符串暂不允许嵌入 `struct_wire`，保持固定宽度结构合同不被隐式所有权污染。

## 5. 模块职责

### AvidScriptBindings

- 提供与 UObject registry 解耦的 UTF-8 value heap/token 服务。
- descriptor loader 校验两种字符串类型的 canonical identity、kind、storage 与方向。
- immutable codec program 增加 `String` kind；Name/String decode 同时接受 linear reference 与
  heap token，encode 统一产生 heap token。
- prepared executor 在 side effect 前 preflight guest output 与 heap slot，在 output
  transaction 中提交或回滚。

### AvidScriptRuntime

- 每个 runtime instance 持有一个 value heap，并在构造、host context 更新、clear 和 unload
  时维持正确注入与 reset。
- runtime 不解析 `FName/FString`，也不增加逐 API host import。

### AvidScriptEditor

- type policy 投影 `FName/FString` 全方向和 property get/set。
- C# renderer 将两类值都映射为 `string`，native storage 保持 `i32`/`out string` 合同。
- 默认值仅在能生成 canonical C# string literal 时发布；无法无损表达时不生成默认参数。

### C# Guest 与 Wasm Backend

- 保持现有 `string -> i32` lowering；字面量仍生成 linear UTF-8 data segment。
- out/ref local 继续是 4 字节 storage，现有 address lowering 可直接承载 heap token。
- 本阶段不加入专用运行时、GC 或 backend 分支。

## 6. 调用数据流

1. C# 字符串字面量以正地址传入；上一次 UE 调用返回的动态字符串以负 token 传入。
2. codec 按 tag 选择 guest memory decode 或当前 session heap resolve，并写入 UE frame。
3. executor preflight 所有 4 字节输出地址并预留所需 heap slot。
4. `ProcessEvent` 只执行一次。
5. Name/String output 转换为 canonical UTF-8，在预留 slot 中 intern/commit，并把 token 写回
   ref/out/return storage。
6. 任一输出失败时回滚本次 heap 变更和已借用 UObject handle；成功后统一 commit。

## 7. 安全与性能边界

- 所有长度与地址运算使用无符号溢出检查；linear payload 必须完整位于 guest memory。
- 单个 UTF-8 value 最大 `1 MiB`，linear input 与 heap output 使用同一上限，禁止长度前缀
  触发无界 guest read 或宿主分配。
- UTF-8 必须 canonical round-trip；`FName` 继续执行 `NAME_SIZE` 与内嵌 NUL 检查。
- token lookup 为 O(1)，字节去重使用稳定 hash 后再做 exact byte comparison，禁止仅凭 hash
  判等。
- literal input 不分配、不查 heap；重复动态输出命中 intern fast path，不重复保存 payload。
- codec program 在 package load 时冻结类型分支，热调用不按 canonical string 动态分派。
- 本阶段性能门禁增加 literal input、intern hit、unique output 与 ref round-trip 四类样本；不得
  让既有 scalar、object、struct Tier 1 门禁退化。

## 8. 验收

- 一个自定义 `UFUNCTION` 同时覆盖 `FName/FString` 的 value、const-ref、ref、out、return，
  C# facade 能编译并通过 Wasm 实际调用。
- `FName/FString` reflected property getter/setter 闭环可执行。
- 字面量输入、动态 token 回传、动态结果再次作为下一次调用输入均成功。
- Unicode、空字符串、`FName` 超长、内嵌 NUL、非法 UTF-8、伪造 token、stale token、heap
  slot 耗尽、输出地址重叠与 unload/reload invalidation 均有 fail-closed 覆盖。
- prepared-dynamic hit 保持生效，ordinal/package dispatcher fallback 为 `0`。
- 阶段末统一执行 UE 5.8 no-clean build、focused Automation、完整 AvidScript Automation、
  架构合同与 P57.10/P57.11B1 性能回归合同。

## 9. 非目标与后续

- 不在本阶段实现 `FText` 本地化语义、string concat/substring/format、容器内字符串、自动
  token release 或跨 runtime token 共享。
- P57.11B3 处理 value heap 显式/编译器辅助生命周期与字符串操作 intrinsic。
- P57.11C 将 mixed numeric、user struct、name/string workload 纳入统一性能量化，并继续
  对 Puerts Reflection 门禁。
