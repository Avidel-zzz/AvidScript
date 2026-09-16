# AngelScript 同语义适配器

本目录为 P65.D37 的可复现 correctness adapter。已在 D36 的独立 UE5.8.0 移植引擎上编译并运行：264/264 用例与 17/17 拒绝检查通过。正式计时、打包生成原生代码模式和性能领先结论仍未完成。

## 范围与身份

- AngelScript 固定来源为 `f347f57f05e992976b6c2be6612303b7e8099502`；上游面向 UE5.8.2，本次使用隔离的 UE5.8.0 移植，不能称为上游 UE5.8.2 二进制。
- fixture、C# workload、gameplay oracle 来自 AvidScript `8c032e9fd9f0afe23c3b18a15057aa7a90d8774c`。冻结输入使用 UTF-8 无 BOM、CRLF；`contract.json` 是实际字节哈希，Git 等价而字节不同的输入也会被拒绝。
- `Native/As36FrozenNativeOracle.inl` 的微基准函数块原样来自冻结的 `AvidScriptPerfRunner.cpp`，外层使用独立命名空间避免 Unity Build 冲突。没有替换原始 `AAvidScriptPerfFixture` 或其 Puerts 依赖。
- `Integration/` 是已编译的完整 harness 配置，增加 `AngelscriptCode` 模块与 `Angelscript` 插件依赖。它只安装到显式指定的独立测试工程，普通 harness 和生产插件不增加此依赖。

## 接入和执行

前提是按 [D36](../../../../Docs/Phase65/P65.D36_AngelScript_Editor_Compatibility.md) 建好隔离引擎，并在独立工程中物理安装冻结的 AvidScript、Puerts 和完整 AvidScriptPerfHarness。安装器不下载或重建引擎，也不复制整个第三方依赖；它验证已有 workload 和依赖，再安装本目录的 10 个文件（D38 新增计时 Commandlet）。

从本目录执行，路径由调用方提供：

```powershell
$ProjectFile = Join-Path $env:AS_PROJECT 'As36Benchmark.uproject'
./Scripts/Install-AngelScriptAdapter.ps1 -ProjectFile $ProjectFile -Plan
./Scripts/Install-AngelScriptAdapter.ps1 -ProjectFile $ProjectFile
```

安装器先检查所有写入目标，再写文件；拒绝主工程、目录链接、错误 fixture 和已修改的适配器。只接受合同中已知的旧配置/适配器字节，保留工程其他字段和无关插件。重复安装不会改变文件内容。随后使用隔离引擎的 UBT 构建 `As36BenchmarkEditor Win64 Development`，保留完整命令、UBT 日志与成功进程退出证据，不清缓存。正式验证入口需要显式指定成功构建记录（JSON 的 `status="succeeded"`、`exit_code=0`），该记录应来自实际构建宿主，不能手工伪造。

```powershell
./Scripts/Invoke-AngelScriptValidation.ps1 `
    -ProjectFile $ProjectFile `
    -Executable (Join-Path $env:AS_PROJECT 'Binaries/Win64/As36BenchmarkEditor-Cmd.exe') `
    -BuildEvidence (Join-Path $env:AS_EVIDENCE 'harness-build-run.json') `
    -OutputRoot $env:AS_EVIDENCE
```

使用实际生成的 project Editor 可执行文件；不能误用隔离引擎目录中从旧引擎复制的 Editor。入口创建唯一证据目录，保留进程退出、日志、JSON 和输入哈希；运行前后核对模块、脚本、配置和全部 harness 源文件。成功构建记录与运行时报告各自提供证据，单独的 `status` 字段不证明完整构建来源。

验证器合同检查以真实成功报告作为基线，再注入缺失/重复样本、错误数值/计数、错误模式和失败日志：

```powershell
./Scripts/Test-AngelScriptValidationContracts.ps1 -CorrectnessReport $CorrectnessJson
./Scripts/Test-AngelScriptInstallContracts.ps1 -OutputRoot $env:AS_EVIDENCE
```

安装合同在外置小型 fixture 工程中检查 Plan 不写入、幂等、字段保留、输入冲突和链接拒绝；不启动引擎，也不删除测试目录。

## 语义和边界

脚本包含 10 项 micro 与 2 项 gameplay。240 个 micro 用例覆盖 6 个 seed（包含高位和 `uint32` 边界）以及 1/2/17/257 次迭代；24 个 gameplay 用例覆盖相同 seed 和 1/2 帧。每项比较结果校验和、最终 ScalarValue 和 13 类实际 native 调用计数；验证器还独立核对调度计数，避免两侧同时少做调用。property write 数量是逻辑调度计数，不是检测到的机器写入次数。

另外验证原生 64 位 FVector 精度、ref/out、对象/null 身份及两个 actor 的回调状态隔离。17 个拒绝检查覆盖未绑定、非法范围、重复执行/读取、原生 actor 冒充、跨 World、非 Game Thread、World teardown 和销毁后的对象。

`Bind` 缓存真实 `UASFunction`、类型和偏移，并分配参数结构；`PrepareSample` 在计时外设置状态，后续计时宿主只能测 `ExecutePrepared`，再在计时外调用 `ReadChecksum`。回调通过缓存的 `AActor::ProcessEvent`，不绕到原始 `asIScriptContext`。Tick 指缓存的 `ReceiveTick` 回调入口，不代表整个 World Tick 调度成本。

该 AngelScript 分支会让带输入函数的 `ParmsSize` 停在最后一个输入参数末端，返回值可能放在其后。桥接层使用 `GetStructureSize()`（与 `FStructOnScope` 的实际分配相同）校验返回值，并核对 `ReturnValueOffset`；输入仍受 `ParmsSize` 限制。并未放弃边界检查或修改引擎来适应测试。

宿主拥有 World、actor、fixture，并保证同步样本期间不发起 GC、reload 或 teardown。实际热重载缓存失效与错误反射签名的额外拒绝用例仍待补充。当前入口明确只支持 Editor VM；必须另外证明 packaged generated native code 确实被使用，再单独验证和计时。

正式 protocol 的 AngelScript 仍为 `not_frozen`。后续需在同一隔离引擎/构建配置下重跑其他框架，固定每组 5 进程、5 次 warmup、30 个计时样本与统计口径，不能直接用主引擎历史计时进行排名。

## 校准和计时

[D38](../../../../Docs/Phase65/P65.D38_AngelScript_Timing_Host.md) 提供 `Invoke-AngelScriptTiming.ps1`，在独立校准进程中确认 5 ms 样本长度，然后以冻结迭代数运行每组 1 或 5 个测量进程。每项 workload/lane 保留 5 次预热、30 次计时，交替 Native/AngelScript 的先后顺序。`Summarize-AngelScriptTiming.ps1` 校验全部原始数据并按进程计算 P50/P95；不会把单进程诊断或 Native 参照冒充完整跨框架排名。
