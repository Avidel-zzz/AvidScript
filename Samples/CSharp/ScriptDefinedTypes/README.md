# C# 脚本定义 UE 类型样例

`ScriptDefinedTypes.cs` 是 P59 的可执行样例：

- `[UClass]` 声明 Actor、ActorComponent、WorldSubsystem 与 GameInstanceSubsystem；
- `[UProperty]` 声明 Blueprint 属性、原生默认值与 RepNotify；
- `[UFunction]` 声明 BlueprintCallable、Server、Client 与 NetMulticast 函数；
- C# 继承、`override`、Blueprint 子类和生命周期进入同一 Runtime route；
- 生成链确定性发布 UHT native shell、WASM body 与 Runtime package。

方法体变化可自动热重载；类型、属性、函数、签名或反射 flag 变化需要 no-clean UBT 并重启 Editor。
RPC 壳层和 authority 内 C# 执行已验证，真实独立进程网络传输由 P59.D6b 验收。
