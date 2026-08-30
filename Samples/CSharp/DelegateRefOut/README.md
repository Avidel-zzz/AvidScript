# C# Delegate `ref/out` 示例

这个示例验证 C# 脚本可以处理 UE 动态多播委托的 `ref/out` 参数，并在脚本正常返回后把结果一次性写回 UE 参数帧。

```csharp
[AvidEvent(AvidEvents.OnRefOutSignal)]
public static void HandleRefOutSignal(ref int value, out int doubled)
{
    value += 3;
    doubled = value * 2;
}
```

对应的 UE 签名使用 `UPARAM(ref)` 区分输入输出引用与纯输出参数：

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FRefOutSignal,
    UPARAM(ref) int32&, Value,
    int32&, Doubled);
```

当前 profile 绑定插件内的 Editor 验证 Actor。接入游戏项目时，把 `self_class_path`、`class_path` 和 `include_events` 替换为自己的 `AActor` 与 `BlueprintAssignable` 委托即可，不需要手写 host API。

输出采用事务语义：错误 token、重复写入、漏写任一输出、WASM trap 或回调重入都会使整次写回失败，原生参数保持调用前状态。
