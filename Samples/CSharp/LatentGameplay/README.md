# LatentGameplay C# 生命周期样例

这个样例展示 P57.12C7 的生成式 UE latent UFUNCTION 与显式取消闭环。Profile 只授权
`AActor.SetActorScale3D` 与 `UKismetSystemLibrary.Delay`，generator 会从 UE Reflection 生成
`UKismetSystemLibrary.DelayAsync(float)`，不需要为 `Delay` 手写 VM switch 或 C# wrapper。

核心脚本：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    LifetimeCancellation = AvidCancellationSource.Create();
    UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
    await UKismetSystemLibrary.DelayAsync(0.25f)
        .WithCancellation(LifetimeCancellation.Token);
    UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
}

[UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
public static void EndPlay()
{
    LifetimeCancellation.Cancel();
    LifetimeCancellation.Release();
}
```

构建链从 profile 生成 descriptor schema v12、C# facade 与动态 WASM import。Runtime 为每次调用
预留一个 Session continuation token 和独占 callback proxy，将隐藏的 `WorldContextObject` 与
`FLatentActionInfo` 注入 UFunction frame，再通过 `ProcessEvent` 注册到真实 `FLatentActionManager`。
action 完成后只进入 Session ready queue，并在安全 Tick 边界恢复后续 C# segment。

`AvidCancellationSource` 属于当前 Session 和 active/prepared lane。一个 source 可以绑定多个 await；
`Cancel()` 会同步移除仍处于 Pending/Ready 的 continuation，已经进入 Dispatching 的完成则获胜。
`Release()` 只释放 source 的取消能力，不取消仍在运行的 continuation，因此生命周期结束时应先取消再释放。
reload、candidate rollback 与 Session teardown 会自动回收遗留 source 和绑定。

当前生成 C# facade 已支持 bool、enum、UObject capability、FVector 等固定 struct 与递归固定 USTRUCT
的 value 输入，并通过共享 storage plan 降为 WASM ABI。仍不支持 latent ref/out、返回 payload 与
delegate payload；这些会 fail closed，计划在 schema v13 的显式 completion provider 中实现。

在 Editor 中把 `LatentGameplay.csharp-profile.json` 设为目标 profile，选中带 AvidScript Component
的 Actor，执行 `Tools > AvidScript > Build And Bind C# Profile Script`，然后进入 PIE。Actor 会在
BeginPlay 后约 0.25 秒从 `1.0` 缩放到 `1.25`；若 Actor 在等待期间 EndPlay，后续 segment 不会执行。
