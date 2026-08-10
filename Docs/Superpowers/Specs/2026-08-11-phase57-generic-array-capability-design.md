# P57.11B3 通用数组 Value Capability 设计

## 1. 目标

P57.11B3 在 prepared dynamic executor、递归固定宽度 codec 和 UTF-8 value heap 之上，
建立第一套通用容器能力：将 UE Reflection 中满足安全约束的一维 `TArray<T>` 自动投影为
C# `T[]`，覆盖 UFUNCTION 的 value、const-ref、ref、out、return 与 UPROPERTY get/set。

实现必须保持 descriptor-driven，不允许为测试函数或项目 API 手写 VM wrapper。相同类型合同
应直接适用于用户自己声明的 `UFUNCTION` 和 `UPROPERTY`。

## 2. 方案

采用 **schema v10 element graph + session-owned array capability heap**：

- descriptor 中的 array type 只保存 `element_type_id`，元素类型仍由同一 type graph 描述；
- guest 与动态调用 ABI 继续使用单个 `i32` value reference；最高位为 1 时表示宿主值能力；
- array heap 保存 immutable type identity、元素数量、stride、alignment 与连续 canonical bytes；
- C# 编译器通过四个共享静态 import 实现 `Length`、索引读取、索引写入和显式释放；
- prepared executor 在 `ProcessEvent` 前预留全部输出，在调用后统一发布或回滚；
- WAMR 与 Wasmtime 只承载同一静态 host ABI，不包含 UE `TArray` 类型分支。

不采用把 UE `FScriptArray` 地址直接暴露给 Wasm 的方案，因为它会绕过 GC、对象句柄与
session ownership；不采用逐 API wrapper，因为那会重新形成手写 API 清单；本阶段也不把
数组复制隐藏成“零拷贝”，所有权与安全边界优先于未经验证的性能宣称。

## 3. Descriptor 与类型闭包

schema 从 9 升级到 10。array type 合同为：

```text
canonical_type = array:<element canonical type>
kind           = array
cpp_type       = TArray<<element cpp type>>
size/alignment = 4/4
abi_types      = [i]
element_type_id = <stable element type id>
```

序列化、反序列化、identity、Binding Slice 和 prepared semantic 都必须沿
`base_type_id`、`element_type_id` 与 `struct_fields` 计算完整类型闭包。缺失元素、循环数组、
非 canonical identity、错误 ABI 或错误 storage 一律 fail closed。

首批元素类型允许：

- bool、整数、浮点和 enum；
- `FVector`、`FRotator`、`FTransform`；
- UObject/Actor/Component handle；
- 已验证的递归固定宽度 `struct_wire`。

首批明确拒绝：

- nested array；
- `FName`、`FString`、`FText` 元素；
- `TSet`、`TMap`、delegate、soft/weak/lazy object；
- 超过现有 struct 深度、节点数、size 或 alignment 上限的元素。

## 4. Value Capability 与 Heap

数组和 UTF-8 值共享进程唯一 capability allocator。token 使用 high-bit tag 和单调分配的
31-bit ID；ID 不复用，避免不同 heap、runtime、reset 或 reload 后的 token 偶然撞权。

每个 runtime instance 持有独立 `FAvidScriptArrayValueHeap`：

- 最大元素数 `4096`；
- 单个数组最大 canonical payload `1 MiB`；
- 最多 `65535` 个 live/reserved slot；
- resolve 必须同时验证 token ownership 与 expected type id；
- read/write 必须验证元素 index、stride、buffer size 和整数溢出；
- release 后 token 立即 stale，再次访问或释放均 fail closed；
- unload、replacement load、clear 和析构重置整个 heap。

heap 暴露 live/reserved/bytes/peak/published/released 统计，供长期 session 的预算与泄漏门禁使用。

## 5. 共享 Host ABI

数组能力作为原子四元组进入生成 manifest、package parser、Binding Slice、Runtime manifest
loader、WAMR registration 与 Wasmtime static import catalog：

| stable id | import | signature | 语义 |
|---|---|---|---|
| `avidscript.value_array_length.v1` | `avid_value_array_length` | `(i)i` | 读取长度 |
| `avidscript.value_array_load.v1` | `avid_value_array_load` | `(iiii)i` | 拷贝一个元素到 guest scratch |
| `avidscript.value_array_store.v1` | `avid_value_array_store` | `(iiii)i` | 从 guest scratch 写回一个元素 |
| `avidscript.value_release.v1` | `avid_value_release` | `(i)i` | 显式释放值能力 |

package 若声明任意一个数组能力，就必须声明完整、无重复、身份完全匹配的四元组。shared
capability 不占动态 reflection ordinal，也不能通过同名未知 stable id 获得授权。

## 6. C# Surface 与编译

生成 facade 使用自然的 `T[]`：

```csharp
int[] result = self.IntArrayRoundTrip(input, ref inOut, out int[] output);
if (result.Length > 0)
{
    result[0] += 1;
}
self.ReadableIntArray = result;
AvidScriptValue.Release(result);
```

Roslyn semantic frontend 将一维数组构造、`Length`、load、store 与 release 降到 Guest IR；
Wasm backend 使用稳定 frame scratch 搬运元素，区分 linear array 与 high-bit capability token。
数组引用本身仍是 `i32`，元素类型的 encode/decode 继续复用 descriptor codec，不建立另一套
C# 类型系统。

本阶段的 C# array surface 是受控子集，不等于完整 .NET Array Runtime：不支持 LINQ、反射、
多维数组、协变、通用 GC 或任意 BCL collection。

## 7. 调用与事务

1. codec 根据 array type id 解析输入 capability，并把 canonical bytes 复制到 UE 参数 frame；
2. 在 host side effect 前检查所有 guest output range，拒绝重叠，并预留每个数组输出 slot；
3. `ProcessEvent` 只执行一次；
4. ref/out/return/property get 的 `TArray` 被编码到 staging bytes；
5. 所有输出编码成功后统一发布 capability token 并写回 guest；
6. 任一阶段失败时回滚 reservation、本次新 token 与对象 borrow；
7. guest 使用结束后调用 `AvidScriptValue.Release`，session 结束仍会做兜底 reset。

property setter 将 guest array capability 解码到 UE property；property getter产生新的 session
capability。对象元素继续使用 generational UObject handle，不把裸指针写入 payload。

## 8. 性能策略

本阶段只冻结正确性和生命周期，不生成数组性能领先结论。当前 `Length` 为一次 host call，
每次索引 load/store 也各为一次 host call；对于短数组和低频 UE 交互足够可用，但长数组热循环
还需要批量映射或借用视图。

后续性能批次必须分别测量：

- capability resolve 与 `Length`；
- scalar/vector/struct/object 元素 load/store；
- UFUNCTION 输入复制、ref/out/return 发布与 property get/set；
- 显式 release 和 session reset；
- 同形状 Puerts Reflection/ArrayBuffer 或 typed-array 路径。

只有冻结 workload、同机候选和完整 correctness oracle 通过后，才能更新 README 性能结论。

## 9. 验收

- 自定义 reflected `TArray<int32>` UFUNCTION 覆盖 value/ref/out/return；
- reflected `TArray<int32>` property get/set 可由真实 C# Guest 执行；
- `Length`、load、store 与 release 经 Guest IR/Wasm 实际调用；
- 同一 fixture 在 WAMR 与 Wasmtime 运行，得到 `[11, 1, 2]`，显式释放后 live value 为 0；
- scalar、enum、对象、固定 struct 与常用 UE value element 的 descriptor/renderer 合同通过；
- nested/string array、伪造 token、跨 session、stale token、越界、错误 element size、超预算与
  不完整 capability manifest 均 fail closed；
- UE5.8 no-clean target、工具链测试、完整 AvidScript Automation 与 clean architecture Gate 通过。

## 10. 后续

下一批应优先冻结数组批量访问 ABI 与 benchmark，减少逐元素 host crossing；随后扩展字符串
元素、nested container、`TSet/TMap`，并保持相同 descriptor graph、capability ownership 和
原子输出模型。delegate、latent、RPC 与 interface dispatch 继续作为独立调用语义推进。
