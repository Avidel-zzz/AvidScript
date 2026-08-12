# P57.12 通用 UE Delegate/Event Subscription 设计

## 目标

P57.12 把 AvidScript 从三个 Actor 碰撞事件的手写桥接，推进到由 UE Reflection 描述、由 C# 声明处理器、由运行时自动订阅的通用事件链。第一批 P57.12A 聚焦可形成完整闭环的最小边界：脚本组件宿主对象上的动态多播委托。

该能力必须满足：

- 不为每个 UE 事件新增 host import、UFUNCTION 或 C++ 分支；
- profile、descriptor、生成 facade、C# 编译器和 runtime 使用同一稳定事件身份；
- 订阅跟随 runtime session 生命周期，失败热重载不破坏旧实例；
- 同步重入不递归进入 WASM，按非致命诊断丢弃；
- 事件参数通过预编译 codec plan 转换，热路径不做名称查找和类型推断。

## 冻结边界

### Profile schema 8

类规则新增 `include_events` 与 `exclude_events`。P57.12A 只接受 `FMulticastDelegateProperty`，且事件源固定为脚本宿主对象 `self`。发现式扫描、单播委托和任意对象订阅不进入本批。

### Descriptor schema 11

根对象新增独立的 `delegate_events` 表。事件不是函数，也不是普通属性：

```text
stable_id
canonical_identity
ordinal
owner_class
ue_member
script_name
delegate_kind = multicast
source_mode = self
export_name
parameters[]
```

`stable_id` 使用完整 SHA-256；`export_name` 为 `avid_on_delegate_` 加稳定 ID 前 16 位。`selection_hash` 覆盖排序后的 `owner + member` 选择，`package_hash` 覆盖事件完整签名与引用类型。

### C# 表面

生成 facade 提供：

```csharp
[AvidEvent(AvidEvents.OnScriptSignal)]
public static void OnScriptSignal(AActor owner, int value, float scale)
{
    // 游戏逻辑
}
```

`AvidEvents` 常量由 descriptor 生成；隐藏的 contract metadata 保存事件的稳定 ID 与参数类型。P57.12A 不开放 `event +=`、lambda 或 closure，避免绕过确定性语义投影和生命周期所有权。

### ABI

每个事件处理器生成一个类型化 WASM export。参数按既有 Guest value layout 递归展开，首批总计不得超过 `FAvidScriptVmCallFrame::MaxCells`：

- 标量与 enum：按 VM ABI cell 编码；
- UObject 引用：`slot + generation` 两个 cell；
- 固定布局 struct：按 descriptor 字段顺序递归展开；
- string、array、delegate、soft reference 和返回值：本批拒绝。

### Runtime 生命周期

`FAvidScriptRuntimeSession` 持有 subscription owner。加载候选 runtime 时先准备事件 plan 与代理，候选 `BeginPlay` 成功后才替换旧订阅；失败候选销毁其准备状态并保留旧订阅。`EndPlay`、Unload 和 Session 析构都先解绑，再释放对象所有权与 VM。

UE 动态委托要求目标对象上存在同签名 UFunction。运行时为每种签名缓存反射生成的桥接 UFunction，代理在 `ProcessEvent` 中取得原始参数帧，交给不可变 codec plan 编码，再通过当前 scheduler/runtime 调用对应 export。热路径不执行 `FindFProperty`、`FindFunction` 或 descriptor 文本解析。

## 本批验收

P57.12A 的完成标准：

1. 自定义动态多播委托可通过 profile 进入 schema 11 descriptor 和 C# facade；
2. C# `[AvidEvent]` 方法生成确定性的 WASM export；
3. UE fixture 广播真实委托后，WASM 处理器收到对象、整数与浮点参数；
4. 成功热重载替换订阅，失败热重载保留旧订阅；
5. 重入事件不卸载 runtime；
6. 阶段末统一通过 dotnet、UBT、Automation 与文档/安全检查。

## 后续批次

- P57.12B：token 化的任意 UObject 动态多播订阅与显式取消；
- P57.12C：latent/async continuation 与 world teardown；
- P57.12D：RPC/replication authority contract；
- P57.12E：事件热路径 benchmark、批量 payload 与移动端 AOT 校验。
