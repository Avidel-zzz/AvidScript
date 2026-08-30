# UE Binding 策略

## 通用覆盖

- 一般 `UFunction`、`UProperty`、`UStruct`、`UClass` 和 delegate 通过 UE Reflection、descriptor、capability policy、codec program 与 C# facade 生成器覆盖。
- 禁止为常规 UE API 逐个手写绑定。手写层仅保留稳定 host ABI、生命周期入口和无法由反射表达的高价值特例。
- 类型支持以递归 type graph 建模；容器、ref/out、soft/weak object、delegate 和嵌套 struct 共享统一 capability。

## 契约

- descriptor schema、stable id、signature、flags、owner type 和 type closure 都有唯一 owner。
- 调用前验证函数身份、参数形状、对象 handle、权限、线程和 world/lifecycle 状态，失败时不进入 `ProcessEvent`。
- `FVector` 等 UE struct 按反射布局或规范化 wire layout 编解码，不能依赖宿主编译器偶然布局。
- Blueprint 与自定义 C++ `UFUNCTION` 只要满足导出策略和支持类型闭包，就应进入同一生成路径。

## 性能

- 冷路径完成反射发现、descriptor 解析、codec 编译和 route 选择；热路径复用 prepared invocation。
- 避免每次调用按名称查找、重复 `FProperty` 遍历、临时容器和字符串转换。
- direct/prepared/fused fast path 必须保持与 semantic fallback 等价，并在不满足前提时 fail-closed 回退。
