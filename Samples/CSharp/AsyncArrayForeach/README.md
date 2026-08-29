# AsyncArrayForeach C# 游戏逻辑样例

这个样例展示 P57.12C16 的一维数组 `foreach + await` 闭环。Actor 在 BeginPlay 中创建三个缩放
百分比，每次等待一个安全 Tick 后应用下一档缩放，全部逻辑都写在 C# 中：

```csharp
int[] scalePercent = new[] { 100, 110, 125 };
foreach (int percent in scalePercent)
{
    await AvidContinuations.NextTickAsync();
    float scale = (float)percent / 100.0f;
    UE.Self.SetActorScale3D(new FVector(scale, scale, scale));
}
```

编译器会把 foreach 展开成普通 continuation CFG。集合表达式只求值一次，隐藏数组引用和索引由
Semantic 显式发布；每个 await 的精确活跃性决定是否将数组引用、索引和当前元素放入状态帧。
状态帧保存的是 4 字节数组引用，不会在每次暂停时复制整个数组，也不会引入逐元素 Host wrapper。

正数引用指向当前 WASM activation 的编译器托管数组区域，负数引用是当前 Session 拥有的 UE
`TArray<T>` capability。恢复受 owner generation、active/prepared lane、reload 和 teardown 栅栏约束；
Session capability 在 teardown 时统一回收，显式 Release 后继续使用仍会失败关闭。

当前 foreach 首批只接受同步求值的一维数组、普通值迭代和与数组完全一致的元素类型；常量数组
使用现有 `new[] { ... }` / `new T[] { ... }` array-creation 语法。非数组
enumerator、元素转换、解构、`ref/ref readonly foreach`、`await foreach`、嵌套数组和字符串元素
仍不开放。

在 Editor 中将 `AsyncArrayForeach.csharp-profile.json` 作为目标 profile，选中带 AvidScript Component
的 Actor，执行 `Tools > AvidScript > Build And Bind C# Profile Script` 后进入 PIE。Actor 会在三个
连续 Tick 中依次缩放为 `1.0`、`1.1` 和 `1.25`。
