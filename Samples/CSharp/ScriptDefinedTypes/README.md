# C# 脚本定义 UE 类型样例

`ScriptDefinedTypes.cs` 是 P59 的可执行样例：

- `[UClass]` 声明 Actor、ActorComponent、WorldSubsystem 与 GameInstanceSubsystem；
- `[UProperty]` 声明 Blueprint 属性、原生默认值与 RepNotify；
- `[UFunction]` 声明 BlueprintCallable、Server、Client 与 NetMulticast 函数；
- C# 继承、`override`、Blueprint 子类和生命周期进入同一 Runtime route；
- 生成链确定性发布 UHT native shell、WASM body 与 Runtime package。

方法体变化可自动热重载；类型、属性、函数、签名或反射 flag 变化需要 no-clean UBT 并重启 Editor。

例如 `Projectile` 的 `LaunchSpeed` 是新声明的蓝图可读写属性，默认值为 `1200`；调用
`ConfigureLaunch(900.0f, false, 3)` 后，C# 方法会把它更新为 `900`，`GetLaunchSpeed()`
可从生成的 UE 函数返回新值。新增这两个反射成员时，构建链会报告
`native_rebuild_required`，需要重新编译 Editor 并冷启动验证。

生成的函数也能等待下一帧。`Projectile` 中的实际写法是：

```csharp
[UFunction(BlueprintCallable = true, Category = "Projectile")]
public async void SetLaunchSpeedNextTick(float speed)
{
    await AvidContinuations.NextTickAsync();
    LaunchSpeed = speed;
}
```

调用后，`LaunchSpeed` 本帧保持原值，下一帧才写入传入的 `speed`。两个 Actor 同时等待时，销毁其中一个只取消它自己的续执行，另一个继续完成。

`Projectile` 还能订阅 UE 自带的碰撞事件，并在事件处理函数中等待下一帧：

```csharp
[UFunction(BlueprintCallable = true, Category = "Projectile")]
public void StartOverlapAwait()
{
    UE.Self.OnActorBeginOverlap += OnOverlapAsync;
}

private async void OnOverlapAsync(AActor overlappedActor, AActor otherActor)
{
    int bonus = 2;
    OverlapScore += bonus;
    await AvidContinuations.NextTickAsync();
    OverlapScore += bonus * 10;
}
```

调用 `StartOverlapAwait()` 后，碰撞广播会立即把 `OverlapScore` 加到 `2`，下一帧变为 `22`。Win64 Editor 自动化从本文件正式生成原生类型壳和 WASM 包，验证了两个 Actor 各自订阅、其中一个销毁后取消等待、另一个继续恢复。这里订阅的是 UE 现有事件；从 C# **新声明** UE 事件仍不在本例范围内。

RPC 壳层与真实独立进程网络传输均已验证：客户端 Server RPC、服务器 Client/NetMulticast、replicated
property 和客户端 C# RepNotify 在 dedicated/listen 拓扑中形成闭环。

构建链还会生成 `Content/AvidScriptGenerated/<package-id>/` 可移植 bundle，并由 UBT 只暂存当前
`current.json` 指向的六个 NonUFS 文件。完整 BuildCookRun/Shipping 验收仍在后续阶段。
