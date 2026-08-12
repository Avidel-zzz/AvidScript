# P57.11D 编译器管理数组访问区设计

## 目标

让普通 C# `T[]` 的 `Length`、索引读取与索引写入在面对 UE `TArray<T>` capability 时，自动进入有界 Guest 线性内存区域。一个区域内只进行一次完整读取，并仅在发生写入时进行一次完整写回，避免逐元素 host crossing，同时保持现有 C# 语义和 P57.11C ABI。

## 冻结约束

- 不新增 UE API wrapper，不按 UFUNCTION 名称特化；
- 不新增 host ABI，复用 `avid_value_array_length`、`avid_value_array_read_range`、`avid_value_array_write_range`；
- capability token 仍为负 `i32`，Guest 线性数组仍为非负地址；
- 每个 Wasm 函数最多维护一个 active array region；
- region 上限为 `4096` 元素和 `1 MiB` payload，与 `FAvidScriptArrayValueHeap` 一致；
- 长度、stride、payload offset、地址或 page 计算溢出必须 trap；
- 任意非数组 intrinsic 调用前、不同 capability 首次访问前以及函数 return 前，必须 flush dirty region 并 invalidate；
- 同 token 的不同 C# 局部变量视为 alias，共享同一 active region；
- 本地正地址数组继续直接访问线性内存，不进入 capability region；
- 显式 `AvidScriptArray.Snapshot/Flush` 保持兼容，不作为常规代码要求。

## 分层设计

### Guest IR

新增 `array_region_load` 与 `array_region_store`。它们保留原 `array_load/store` 的操作数和元素类型，因此 Guest IR 验证器只扩展同一 shape，不引入 C# 或 UE 概念。C# lowering 对可达数组索引统一产生 region op；其他前端仍可继续使用基础 op。

### Wasm backend

当函数包含 region op 时，为函数增加以下局部状态：

- active capability token；
- materialized Guest array 地址；
- element count、stride、alignment；
- dirty flag。

第一次访问负 token 时，调用 length import，验证上限，按 array header/payload 对齐计算字节数，在当前函数动态栈顶分配并按需 `memory.grow`，写入 length header，再用 range read 一次性填充 payload。后续相同 token 的 load/store 转为普通线性数组访问；store 设置 dirty。

进入不同 token、普通 call 或 return 前执行 barrier。dirty 时调用 range write，成功后清空 active token；只读区域不写回。frame restore 释放 region 内存，不向 host 暴露 allocator。

### 生命周期与异常

- host import 返回 `0` 立即 trap；
- trap 不承诺写回尚未提交的 dirty region，与当前 Wasm trap 语义一致；
- normal return 必须先 barrier 再恢复 frame；
- region 地址只在当前函数调用存活，不进入状态迁移或 reload manifest；
- WAMR 与 Wasmtime 消费同一 Wasm 和同一 range ABI。

## 验收

- Guest IR 验证 region op 的类型与 shape；
- C# fixture 的普通 `result[i]` 循环不再要求手写 Snapshot/Flush；
- Wasm artifact 仍只导入现有六项 array capability ABI；
- WAMR/Wasmtime 结果一致、host capability 生命周期归零；
- N>=64 的 compiler-managed lane 相对逐元素 lane P50 不高于 `0.80`；
- 阶段末统一执行 .NET tests、UE5.8 no-clean build、完整 Automation、性能与 clean architecture gate。
