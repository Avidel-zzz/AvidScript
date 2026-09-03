# UI 与存档 C# 样例

本样例用 C# 实现按钮事件、计分和 UE SaveGame 存取。模块为 `avidscript.ui_save_demo`，binding package 为
`avidscript.sample.ui_save_demo`。已通过资产生成、C# 到 WASM/AOT 发布，以及 Editor 游戏进程的
保存、重启读回、缺档、GC、存档异常与组件退出清理验证；物理输入与视觉体验仍待验收。

## 行为与生命周期

- Collect 增加分数，上限 999999；Reset 仅清零当前分数，不删除存档。
- Save 使用 UE `SaveGameToSlot`；Load 先检查存在性，再读取并验证类型与分数范围。
- 缺档、读取失败、错误类型、无效分数或保存失败显示独立状态，不把失败解释为成功。
- BeginPlay 订阅四个 Button；任意订阅失败取消整组。EndPlay 先禁用派发并取消订阅，再移除 UI，释放本 Session 创建的存档对象。
- 计分字段使用 `[AvidPersist]`，订阅和对象句柄使用 `[AvidTransient]`。热重载状态与磁盘存档是两条独立通路。
- 存档 slot 固定为 `AvidScript_UiSaveDemo_v1`，UserIndex 为 0。只操作本样例的 slot，不枚举、清理或覆盖其他 slot。
- 自动验收使用隔离的 UE `-UserDir`；跨进程读回复用同一个隔离目录，不操作用户实际样例存档。

## 冻结资产接口

资产根为 `/AvidScript/Demos/UiSave`，名称为 `WBP_UiSave`、`BP_UiSaveHost`、`BP_PlayerSave`、`L_UiSave`。

| 对象 | 公开属性 |
| --- | --- |
| BP_UiSaveHost | `WBP_UiSave RootWidget`、可写 `BP_PlayerSave SavedObject` |
| WBP_UiSave | `UButton CollectButton/SaveButton/LoadButton/ResetButton`、`UTextBlock ScoreText/StatusText` |
| BP_PlayerSave | 可写 `int32 Score` |

Blueprint 只负责标准 CreateWidget、布局、输入焦点和强引用持有，不放点击、计分或存取逻辑。
Host 必须先完成 Widget 初始化，startup coordinator 再激活脚本。
`RootWidget` 保留宿主强引用；脚本 EndPlay 移除挂载后，后续 BeginPlay 通过标准 `AddToViewport` 重新挂载。
`SavedObject` 持有读取/创建结果，不能把 Guest static handle 当作 GC 引用。
Load 先将 `USaveGame` 返回值 TryCast 为 `UPlayerSave` 并校验 Score，全部通过后才提交到强类型 `SavedObject`；失败保留原引用和分数。

源码中的 `AUiSaveHost/UUiSaveWidget/UPlayerSave` 是标准 C# using aliases，分别指向生成类型
`ABP_UiSaveHost_C/UWBP_UiSave_C/UBP_PlayerSave_C`，没有手写这些 facade 类型。

## 运行

先完成插件的 UE5.8 Editor 增量构建，再从插件根运行：

```powershell
pwsh -NoProfile -File Build/InvokeAvidScriptUiSaveDemo.ps1 -Mode Prepare
pwsh -NoProfile -File Build/InvokeAvidScriptUiSaveDemo.ps1 -Mode Publish
pwsh -NoProfile -File Build/InvokeAvidScriptUiSaveDemo.ps1 -Mode Play
```

Prepare 通过 UE 工厂创建四个资产；已有资产只校验，不覆盖自定义修改。Publish 使用本目录的 Profile，
生成类型化 facade，再通过正式 Release 链路发布 Win64 Wasmtime AOT 模块。Play 直接打开样例地图，
不触发构建，也不把进程启动成功当作验收。

每次命令返回 JSON 和唯一证据目录，发布结果带有 `package_id` 与验证过的 binding manifest。
修改 C# 后重新执行 Publish；修改反射数据壳则先更新 Profile 并重新生成 facade。不要手写 generated facade，
也不要用 EngineGameplay 的 binding package 替代本样例的包。

## 自动验证

将 Publish 返回的 `package_id` 传给独立验收入口：

```powershell
pwsh -NoProfile -File Build/InvokeAvidScriptUiSaveDemo.ps1 `
  -Mode Verify -ExpectedPackageId <发布返回的64位十六进制package_id>
```

Verify 依次启动五个独立游戏进程：写入 3 分、重启读回 3 分、独立空目录缺档、GC 后继续计分到 4，
以及另一个隔离目录中的错误类型/负数/越界/空文件、Reset、保存失败与组件退出清理。
读回与 GC 复用写入目录，异常夹具不修改该存档。每个进程有唯一日志/JSON，单进程等待最多 180 秒；
报告核对模块身份、UI 文本、UE 回调、存档哈希和失败字段，不只看退出码。

结果与存档保留在报告所列目录，不自动删除。默认存档根位于系统临时目录；可用 `-VerifyUserRoot`
指定项目及引擎目录外的**尚不存在**目录。此入口使用真实 `UButton.OnClicked` 广播，但输入由探针合成，
不是物理点击、画面可用性或长时间运行的验收。

## 事件来源

`UButton.OnClicked` 是无参数 multicast，四个实例共用一个事件合同和一个 C# handler。
BeginPlay 分别调用 `AvidSubscriptions.SubscribeOnClicked(source)`，handler 使用
`AvidSubscriptions.IsCurrentSource(source)` 路由 Collect/Save/Load/Reset，无需四份手写绑定。

通用 `AvidSubscriptions.IsCurrentSource(UButton source)` 已接入 facade/VM/Session：在当前委托
callback 内比较经验证的来源对象，callback 外返回 false，拒绝重入后恢复外层来源。
它不根据 hover、focus 或轮询推测点击者。Runtime 来源上下文和独立 UObject owner 的生成路径均已验证。

已读取 UE5.8 源码确认 `UKismetTextLibrary.Conv_IntToText/Conv_StringToText` 为 UFUNCTION，
`UTextBlock.SetText` 使用 FText；现有 type policy 将 FString/FText 投影为 string/FAvidText。
临时 `FAvidText` 在 `SetText` 复制后显式释放。集成进展与未完成项见
[P64.D 记录](../../../Docs/Phase64/P64.D_UI_Save_Integration.md)。

## 集成验收

已通过五个独立 UE 进程的 **31/31** 动作：正常存取和 GC 交互，以及四种读取失败、Reset、
写锁下的保存失败。失败 Load 保留原对象和分数；Reset 和保存失败不改存档字节。
组件 EndPlay/注销后，四个按钮解绑、UI 移除、Session 与 Owner 授权释放；迟到 Collect 不再进入 Guest。
报告中的 `runtime_snapshot_phase=before_teardown` 明确区分停止前 runtime 与最终 teardown 证据。

详见[异常流程验收](../../../Docs/Phase64/P64.D_UI_Save_Edges.md)。World 销毁、热重载后的清理/恢复、
包内 UI 与长稳仍待验证；空文件分支不代表任意损坏文件都可安全解析。合成事件不替代真实输入和视觉验收。
