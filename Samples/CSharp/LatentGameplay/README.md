# LatentGameplay C# 样例

这个样例展示 P57.12C5 的第一条生成式 UE latent UFUNCTION 闭环。Profile 只授权
`AActor.SetActorScale3D` 与 `UKismetSystemLibrary.Delay`，generator 会从 UE Reflection 生成
`UKismetSystemLibrary.DelayAsync(float)`，不需要为 `Delay` 手写 VM switch 或 C# wrapper。

核心脚本：

```csharp
[UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
public static async void BeginPlay()
{
    UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));
    await UKismetSystemLibrary.DelayAsync(0.25f);
    UE.Self.SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f));
}
```

构建链从 profile 生成 descriptor schema v12、C# facade 与动态 WASM import。Runtime 为每次调用
预留一个 Session continuation token 和独占 callback proxy，将隐藏的 `WorldContextObject` 与
`FLatentActionInfo` 注入 UFunction frame，再通过 `ProcessEvent` 注册到真实 `FLatentActionManager`。
action 完成后只进入 Session ready queue，并在安全 Tick 边界恢复后续 C# segment。

当前生成 C# facade 只接受静态、无返回、completion-only latent UFUNCTION，以及可直接映射为单个
WASM cell 的 value 参数。复杂 struct、对象、数组、bool、ref/out、delegate payload 与实例 latent
facade 会 fail closed；Runtime descriptor/executor 已保持通用，后续扩展这些类型不需要按 API 名称添加分支。

在 Editor 中把 `LatentGameplay.csharp-profile.json` 设为目标 profile，选中带 AvidScript Component
的 Actor，执行 `Tools > AvidScript > Build And Bind C# Profile Script`，然后进入 PIE。Actor 会在
BeginPlay 后约 0.25 秒从 `1.0` 缩放到 `1.25`。
