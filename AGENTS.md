# AvidScript Agent Notes

## Engine Baseline

- Use source-built Unreal Engine 5.8.
- Engine root: `C:\UnrealEngine`.
- Preferred Editor target validation command:

```powershell
& "C:\UnrealEngine\Engine\Build\BatchFiles\Build.bat" AvidTPSTemplateEditor Win64 Development "-Project=<ProjectRoot>\AvidTPSTemplate.uproject" -WaitMutex -NoHotReloadFromIDE
```

## Repository Policy

- Treat `Plugins/AvidScript` as the standalone Git-managed plugin repository.
- Do not assume the project root is a valid Git repository.
- Default branch: `main`.
- Keep generated Unreal outputs out of Git: `Binaries/`, `Intermediate/`, `Saved/`, `DerivedDataCache/`, and local IDE files.
- Keep source files, plugin metadata, docs, build scripts, third-party integration metadata, and intentional sample artifacts tracked.
- Do not commit downloaded caches, local build products, temporary logs, or machine-specific IDE settings.
- If Git reports dubious ownership in Codex, this plugin path is allowed as a Git safe directory:

```powershell
git config --global --add safe.directory "<PluginRoot>"
```

## Git Workflow

- Work from `Plugins/AvidScript` when running Git commands.
- Before edits, check status:

```powershell
git -C "<PluginRoot>" status --short --branch
```

- Make small commits aligned to phase groups, for example `P1.1 plugin skeleton` or `P1.2 WAMR third-party layout`.
- Commit only after verification has been run or a documented blocker has been recorded.
- Prefer clear commit messages:

```text
P1.1 add runtime module skeleton
P1.2 document WAMR third-party strategy
```

- Do not rewrite history, reset, or discard changes unless the user explicitly requests it.

## Phase Policy

- Work phase by phase and mark completed phase groups in the docs.
- For larger phases, split the work into small groups such as P1.0, P1.1, and P1.2.
- Document each implemented group with status, evidence, verification result, and remaining risk.
- AvidScript remains WASM-first, WAMR-first feasibility, C#-friendly, PC-first, and mobile-aware.

### Simplified Parallel Phase Workflow

- 架构、模块边界、公开接口、Schema、ABI 与验收指标冻结后，立即按独立写集拆分功能组并行开发；并行写任务必须使用独立 Git worktree/branch，或能够证明写集完全不重叠。
- 禁止把阶段拆成“小任务实现 -> 独立复审 -> 再复审”的串行链。普通 Task 完成后只记录状态和非阻塞债务，不单独启动审查；整个 Phase 集成后只执行一次集中代码与架构审查。
- 集中审查一次列全 findings，随后只安排一个集中修复批次。除修复引入新的 Blocker/Critical 外，不为单个 finding 再启动独立复审。
- 只有 ABI/Schema 兼容性、安全、资源所有权与生命周期、数据破坏、线程并发、跨平台或 packaged target 编译等阻塞性高风险，允许在实现批次中途打断并执行最小探针。普通测试补全、文档、样式、低风险边界和非关键优化统一留到阶段 Gate。
- 测试代码可随功能编写，但 UE 构建、Automation 和正式 benchmark 默认只在阶段末统一执行一次；不得用逐 Task 构建、重复全量回归或无意义 flag 组合测试替代工程判断。
- 接口冻结后的编码代理必须面向落盘结果工作：开始后 10 分钟内提交第一份代码 diff，或返回精确 blocker 与已检查证据。超过时限仍停留在开放式分析且没有文件变化时，立即中断、缩小任务或收回主线。
- 模型和代理按职责分配：架构冻结、跨层高风险判断和阶段末集中审查使用深度推理；边界明确的实现、测试补全和文档使用快速实现代理。禁止让高成本代理对已冻结的小接口重复开放式分析。
- 固定阶段顺序为：接口冻结 -> 并行功能组实现 -> 集成 -> 一次集中审查 -> 一次集中修复 -> 一次统一构建/测试/benchmark Gate -> 中文收尾与发布。

## Documentation Workflow

- Project-level decision docs live under:

```text
<ProjectRoot>\Docs
```

- Plugin-level implementation docs should live under:

```text
Plugins/AvidScript/Docs
```

- 面向用户或团队阅读的项目文档、阶段文档、实现日志默认使用中文；代码标识符、命令名、文件路径、API 名称和日志原文保持原语言。
- 2026-07-06 用户再次确认: 给人读的文档必须用中文写；后续 phase closeout、使用说明、实现日志和 tracker 默认中文优先。
- When a phase group is completed, update the phase tracker and the related implementation log.
- Each implementation note should include:
  - scope
  - files changed
  - verification command
  - result
  - remaining risk
  - next phase group

## Build And Verification Workflow

- 2026-08-28 P57.12C2 首次从暂存树创建 clean candidate 时把 `$tree` 通过管道传给 `git commit-tree`，但该命令要求 tree object 作为位置参数，导致临时 commit 创建失败、worktree 回落到旧 HEAD。Prevention：候选命令固定为 `$tree = git write-tree; $candidate = git commit-tree $tree -p HEAD -m <message>`，创建 worktree 后必须同时断言 `git rev-parse HEAD` 等于 candidate 且 `git status --porcelain` 为空，再运行架构门禁。
- 2026-08-28 P57.12C2 首轮完整队列修正后仍有 `SourceAdapterArtifactLifecycleSmoke` 一项失败：`FlushAsyncLoading` 加手动 `UWorld::Tick` 不能在同一个同步 Automation test 内驱动 `FStreamableDelegateDelayHelper` 的全局 `FTickableGameObject` 下一帧回调。Prevention：真实 Streamable 同步 oracle 先显式预加载目标资产，并用 scope guard 临时设置 `s.StreamableDelegateDelayFrames=0`、测试退出恢复原值；不要把 world tick 当作所有 engine-global tickable 的替代品。
- 2026-08-28 P57.12C2 首轮完整 Automation 在 `AsyncObjectProducer` fixture 直接调用 `NewObject<UObject>`，UE5.8 因抽象基类实例化触发 ensure 并以 255 退出。Prevention：Runtime Automation 需要通用对象时必须复用 `AvidScriptObjectRegistryTestTypes.h` 中的具体 `UAvidScriptObjectRegistryTestObject`，新增 `NewObject<T>` 前先检查 `T::StaticClass()->HasAnyClassFlags(CLASS_Abstract)` 或复制同模块既有 fixture 类型。
- 2026-08-28 P57.12C2 首次 Automation 包装先尝试删除固定日志而被安全策略拒绝，随后又误用 `-log=<absolute-path>`，使崩溃轮未留下指定文本日志，只能从 crash context 恢复。Prevention：Automation 每轮直接创建带时间戳的新证据文件，绝不删除旧日志；绝对日志路径统一使用 UE 的 `-abslog=<path>`，结束后立即验证文件存在再解析汇总。
- 2026-08-28 P57.12C2 首轮 no-clean Editor 构建发现新 async loader 将 UE5.8 的 `struct FSoftObjectPath` 前置声明成了 `class`，MSVC C4099 直接失败。Prevention：新增任何 UE 类型 forward declaration 前先在 `C:\UnrealEngine\Engine\Source` 用 `rg -n '<TypeName>'` 复制引擎原始 `class/struct/enum` 声明；集中审查不得只确认头文件路径与 API 名称。
- 2026-08-28 P57.12C2 首次并行探索同时传入 `agent_type=explorer` 与 `fork_context=true`，工具拒绝“带完整历史的 fork 不能覆盖 agent type”。Prevention：需要 explorer 角色时使用独立上下文并在 prompt 中写明 workspace 与边界；需要完整历史时省略 `agent_type`，两种模式不得混用。
- 2026-08-28 P57.12C2 在已经取得文件索引后仍猜测了不存在的 `Source/AvidScriptBindings/Private/AvidScriptObjectFactoryBinding.cpp` 与 `Tools/AvidScript.CSharpSemantic/Analysis/SemanticCompilationContext.cs`。Prevention：后续读取只能逐字复制本轮 `rg --files` 或 `rg -l` 的命中路径；类型定义位置未知时先 `rg -l '<TypeName>' <confirmed-parent>`，不得根据类名推导目录。

- 2026-08-23 P57.12B 首次 clean-candidate 架构门禁发现新增 `event_subscribe` / `event_unsubscribe` 后，static host catalog、renderer fixed import 与 framework facade struct 的 checker 白名单未同步，产生 4 项合同漂移。Prevention：新增任何共享 Host ABI 或框架级生成类型时，接口冻结清单必须同时覆盖 VM catalog、兼容性标记、renderer import、reserved/generated type 与 `CheckAvidScriptArchitecture.ps1` allowlist；集中 Gate 前执行 checker parser，但正式架构证据仍只在精确 clean candidate 上运行。
- 2026-08-23 P57.12B 首轮 C# Guest Gate 在新增 WASM 编译断言后漏加 `using AvidScript.WasmBackend`，测试项目在执行前以 CS0103 停止。Prevention：测试复用另一文件中的工具类型时，先读取原调用点顶部 namespace 与目标 `.csproj` 引用；集中 Gate 前的 owned-path 审查必须逐个解析新增外部类型的 namespace，不能只确认项目引用存在。
- 2026-08-23 P57.12B 并行代理在生成式 C# 写集完成后自行启动了 no-clean Editor 构建，早于阶段集成与统一 Gate；虽然提前发现 `TArray::CountByPredicate` 不存在，但仍造成一次可避免的碎片化构建。Prevention：实现代理任务必须逐字写明“禁止构建、Automation 与 benchmark，只提交 diff 和静态风险”；阶段构建命令只由主线在集中修复结束后发起一次，代理若发现疑似编译风险只返回文件/行号与建议探针。
- 2026-08-12 P57.12A Gate 首次按此前引擎输出版本推断 bundled dotnet 路径为 `10.0.203\win-x64`，实际目录是 `10.0\win-x64`；修正后又忽略仓库 `global.json` 固定要求用户级 SDK 8.0.416，造成两次环境级失败。Prevention：任何阶段 Gate 使用 dotnet 前必须先分别枚举候选 `dotnet.exe` 与执行 `--list-sdks`，选择同时满足字面路径和 `global.json` pin 的宿主；禁止根据 `dotnet --version` 输出反推安装目录，也不得把环境解析失败计为产品测试失败。

- 2026-08-11 P57.11C 文档收尾校验再次把“预期可能无匹配”的 README 旧值搜索与确定性 JSON/status 读取放入同一并行组，令无匹配 exit 1 吞掉其余输出。Prevention：从本记录起，任何用于证明“零命中”的 `rg` 都必须独立调用；并行编排只接受已经证明必定 exit 0 的命令，不再把无匹配搜索视为普通只读检查。
- 2026-08-11 P57.11C 收尾时把 clean worktree 的 CRLF checkout 文件 SHA 当成正式 benchmark profile SHA，而 runner 实际绑定的是主工作区 LF 字节，两个 hash 不同。Prevention：benchmark evidence 的 profile/result identity 必须直接读取正式 aggregate 内记录的 `profile_sha256`，再与 runner 当时使用的字面 profile 文件复核；clean candidate 文件 hash 只用于该 candidate 自身，不跨 checkout 代填 provenance。
- 2026-08-11 P57.11C 首版数组 benchmark 的静态合同把普通 JS `Array` 误认成 Puerts 的 UE `TArray` wrapper；contract smoke 通过，但真实 commandlet 在 `N=1` 返回空数组并被 full-hash oracle 拒绝。Prevention：Puerts container headline 必须使用固定版本公开的 `UE.NewArray(UE.Builtin*)` 与 `Num/Get/Add` API，并在任何计时数据发布前先通过每个 size 的真实 marshal/full-hash correctness。
- 2026-08-11 P57.11C 调查上述失败时再次猜测 Puerts checkout 存在 `README.md`、`doc` 与 `unreal` 路径，使并行搜索整体失败。Prevention：第三方 checkout 也适用路径索引规则；先枚举仓库根或 `rg --files`，只把已确认存在的目录交给并行搜索。
- 2026-08-11 P57.11C Gate 准备时把可能返回非零的 UE/dotnet 进程探测与内存、Git 读取放入同一个 `Promise.all`，再次丢失了其余有效输出。Prevention：进程、内容匹配和可选文件探测必须各自独立执行并显式处理“无结果”；只有合同保证 exit 0 的确定性读取才允许组成并行组。
- 2026-08-11 P57.11C 开始时再次按概念名猜测了不存在的工具、脚本和源码路径。Prevention：读取任何非刚创建的文件前，必须先对已确认父目录执行 `rg --files <parent>`，再逐字使用命中路径；不确定路径不得与确定性读取放入同一个并行失败域。
- 2026-08-11 P57.11C 首版 C# 数组夹具使用了当前 Guest lowerer 尚不支持的运行时长度 `new int[result.Length]`。Prevention：新增 C# 语法形态前先从 `Tools/AvidScript.GuestIr` 定位并核对 operation lowerer；当前数组分配只使用受支持的常量维度或初始化器，动态分配另立语言能力阶段。
- 2026-08-11 P57.11C 首版 VM range import 校验直接引用了 Bindings 层的 heap 常量，形成 `AvidScriptVM -> AvidScriptBindings` 反向依赖。Prevention：VM 只能依赖自身 ABI catalog 或中立 contract owner；中立 owner 建立前，跨层上限必须在 VM catalog 以带来源说明的 ABI 常量表达，禁止包含或引用 Bindings 实现类型。
- 2026-08-11 P57.11B3 首版 Runtime value-capability 去重把 `TSet::Add` 当作 `bool` 拼入校验表达式，UE5.8 实际返回 `FSetElementId`，导致集中增量编译失败。Prevention：需要“检查后插入”的 UE `TSet` 逻辑固定先用 `Contains` 形成布尔条件，校验通过后再单独调用 `Add`；容器 API 返回类型不能按 STL 习惯推断。

- 2026-08-02 P57.11B1 在 canonical 主工程构建之后又构建隔离 benchmark 工程，后者刷新了源码版 UE 的全局 `UnrealEditor.version/modules BuildId`；主工程 DLL 虽已存在，旧 manifest 仍被 Editor 判为 out-of-date，Automation 在发现测试前退出。Prevention：使用同一源码引擎完成任何隔离工程 target build 后，启动 canonical Automation 前必须再执行一次主工程 no-clean 增量 target build，并核对主工程 `UnrealEditor.modules` 的 `BuildId` 与引擎一致；禁止通过清理 Editor target 修复 metadata 漂移。
- 2026-08-02 P57.11B1 再次在一条 PowerShell `shell_command` 中用分号拼接 UE 进程与内存状态读取，违反“一调用一逻辑动作”规则。Prevention：即使两个读取都无副作用，也必须在 JavaScript 编排层分别调用；发送命令前扫描顶层 `;`、`&&`、`||`，只允许单个 PowerShell 脚本内部为同一计算目标组织多行语句。
- 2026-08-02 P57.11B1 新建正式 benchmark project 时把 `HarnessPluginPath` 指向主工程独立插件，sidecar 安全检查因 harness 不在候选 worktree 内而拒绝。Prevention：`New-PuertsBenchmarkProject.ps1` 的 harness 固定使用当前 clean candidate 下的 `Benchmarks/PuertsComparison/AvidScriptPerfHarness`；AvidScript commit/tree、harness 源码与 runner 必须来自同一候选。
- 2026-08-02 P57.11B1 隔离 benchmark 首次只复制三个 guest profile，没有复制 manifest 精确引用的 `Saved/AvidScriptGeneratedBindings/<package>/<hash>`，校准在计时前以 `binding_package_file_missing` fail closed。Prevention：从每个 `.avidscript.json` 的 `binding_package.manifest_file` 解析并复制唯一 package/hash 目录，逐项确认 package.json 存在后再创建新的空输出目录；失败目录不得复用为正式证据。
- 2026-08-02 P57.11B1 首次运行 bundled `dotnet build-server shutdown` 时把 workdir 留在含 `global.json` 8.0.416 pin 的候选仓库，只有引擎 SDK 10.0.203 的命令因此在 shutdown 前被 SDK resolver 拒绝。Prevention：使用引擎 bundled dotnet 关闭 UBT/MSBuild server 时固定从不受仓库 `global.json` 约束的 `C:\tmp` 启动，成功输出必须同时确认 MSBuild 与 VB/C# server 已关闭。
- 2026-08-02 P57.11B1 把 `Merge-PuertsBenchmarkResults.ps1` 直接用于 `Invoke-Phase54GameplayBenchmark.ps1` 的 Micro `external_raw_evidence` 输出；该 runner 按合同不生成 `attempt.json`，merge 在读取前即拒绝。Prevention：Phase56 Micro 回归从 5 个 process result 按冻结规则计算“每进程 nearest-rank p50，再跨进程 nearest-rank p50”；只有由 sidecar attempt workflow 生成并含 `attempt.json` 的目录才调用通用 Merge 脚本。
- 2026-08-02 P57.11B1 证据哈希脚本从 result 文件名构造 request 文件名时把 `Join-Path`、`-replace` 和 `+ '.json'` 写在同一表达式中，PowerShell 运算符绑定生成了不存在的路径；`Get-FileHash` 又是非终止错误，使脚本仍以 exit 0 输出不完整 JSON。Prevention：先用独立变量把 `.result.json` 替换为 `.request.json`，再调用 `Join-Path`；证据脚本顶部固定 `$ErrorActionPreference = 'Stop'`，任何 hash 缺失都必须使整个证据计算失败。
- 2026-08-02 P57.11B1 在主工程 Automation 启动前看到约 3.98 GB 空闲内存，仍把它当作 4 GB 边界可用；Editor 分配基础工作集后只剩 3.33 GB，并再次在项目模块加载前停滞。Prevention：canonical 主工程 Editor 冷启动门槛提高到至少 5 GB 未四舍五入可用物理内存；不足时不通过切换 culture/DDC/Multiprocess 参数反复试跑，继续静态、文档和候选工作，直到资源窗口明确满足门槛。
- 2026-08-02 P57.11B1 受限并发的隔离 UBT 构建完成后仍留下 19 个 `/nodeReuse:true` MSBuild `dotnet` worker，父 UBT 已退出但约占 2.2 GB working set，直接压低后续 UE 冷启动内存。Prevention：大批量 UBT/UBT 自编译结束并准备运行 Editor Gate 前，检查同批次 bundled SDK MSBuild worker；确认 command line、同一启动时间且父 PID 已退出后，使用 bundled `dotnet build-server shutdown` 正常关闭，禁止按进程名粗暴结束用户的其他 dotnet 进程。
- 2026-08-02 P57.11B1 新 detached benchmark candidate 首次构建时尚未安装 ignored Wasmtime SDK，UBT 生成了 `AVIDSCRIPT_WITH_WASMTIME=0` 的 makefile；随后 lockfile 安装成功，但普通增量构建和 `-gather` 都继续复用旧 ModuleRules 结果，仍报告 `wasmtime.h` 缺失。Prevention：干净 candidate 必须先安装并 Verify 官方/性能 Wasmtime managed layout，再首次生成 target makefile；若无 SDK makefile 已经生成，使用 `-NoUBTMakefiles` 重新执行 ModuleRules，禁止 clean target 或盲目重复同一构建。
- 2026-08-02 P57.11B1 为修正完整 Automation 的 9 个过期断言，又单独冷启动包含多个 C# workspace build 的聚焦集合；两次 UE 进程都在测试发现前因系统仅剩约 0.8-1.8 GB 物理内存停滞，父命令超时后还留下 Editor 子进程，造成纯等待。Prevention：完整队列已经定位为断言/计数更新时，先增量编译，阶段末只重跑一次完整 Automation；需要冷启动前先检查可用物理内存，低于 4 GB 时先做不依赖 Editor 的工作，命令超时后必须独立检查并终止自己创建的残留 UE 进程，禁止用重型 workspace 聚焦集合重复支付冷启动成本。
- 2026-08-02 P57.11B1 统计 candidate tree 时把未引用的 PowerShell 参数 `HEAD^{tree}` 放入并行 Git 读取组，`{tree}` 被 PowerShell 当作脚本块并编码成额外参数，整组有效输出再次丢弃。Prevention：所有带 `^{...}` 的 git revision 必须使用单引号并先独立执行，例如 `git rev-parse 'HEAD^{tree}'`；只有该 revision 已成功解析后，才并行后续只读统计。
- 2026-08-02 P57.11B1 focused Automation 首次漏传 runtime 复数开关 `-EnablePlugins=AvidScriptPerfHarness`，导致依赖 Harness 的 `AvidScriptGeneratedBindings` 在测试发现前加载失败；修正后进程 exit 0 但 4 项中仍有 1 项 `Result={Fail}`，证明进程码不能替代结果解析。失败夹具还直接 `NewObject<UObject>()`，UE5.8 把抽象 UObject 实例化记录为 ensure。Prevention：所有主工程 Automation 从 tracked runtime 模板复制复数插件开关；验收同时解析 found/completed/每项 Result/Queue Empty/TestExit；需要普通 UObject capability 的测试使用插件内具体 UCLASS，禁止实例化 UObject 基类。
- 2026-08-02 P57.11B1 Gate 入口把可能抛出 junction mismatch 的 Harness `Verify`、可能无进程的 `Get-Process` 与 Git 确定性读取放入同一并行组，再次造成有效状态输出丢弃。Prevention：环境前置条件与进程探测都按“可失败探测”独立执行；只有 Harness Verify 明确成功后才并行纯 Git/文件读取，禁止为了少一次 tool call 扩大失败域。
- 2026-08-02 P57.11B1 首次统一构建发现 recursive compiler 在 `AvidScriptBindingInvocation.cpp` 匿名命名空间中直接使用 `FValueCodecProgram/EValueCodecKind`，但该 translation unit 只通过 `FAvidScriptRuntimeBindingValuePlan/EAvidScriptRuntimeBindingKind` alias 暴露私有类型，造成级联未声明错误；测试同时把 `WriteBytes` 参数命名为成员 `Bytes` 并使用了错误的非 const package shared pointer。Prevention：跨私有命名空间实现必须复用 translation unit 已定义 alias 或写完整限定名；测试 override 参数不得遮蔽成员；调用公开签名前先逐字核对 const/shared-pointer 类型，统一构建的首轮 error list 按首个根错误修复，不逐条追级联诊断。
- 2026-08-02 P57.11B1 在已知存在 protected dirty baseline 的主工作树上仍执行无 pathspec 的 `git diff --check`，用户文件中的既有尾随空格使并行检查整体返回非零。Prevention：主工作树只对本批次 owned path 清单执行 scoped `git diff --check -- <paths>`；全树 diff/check 与 architecture evidence 仅在 detached clean candidate 执行，禁止修改受保护文件来消除噪声。
- 2026-08-02 P57.11B1 并行实现首版在 production codec 尾部保留了约百行 `#if 0` 旧写回实现，同时 C# renderer 只输出 `[FieldOffset]`，未先核对自研 C# Guest lowerer 会忽略该 attribute 并按自然字段布局重算；`bool` 因 Guest 为 1 byte、wire 为 4 bytes 会使后续 byte 字段偏移漂移。Prevention：集中审查必须拒绝 production `#if 0` 实现墓地；任何生成的布局 attribute 都要追到实际 lowerer consumer，若 consumer 不读取 attribute，则生成字段的自然 size/alignment 必须与 wire 一致。schema 9 bool 固定使用 private i32 wire storage 加 public bool 只读投影，并用 bool 后接 byte 的 fixture 验证偏移不会塌缩。
- 2026-08-02 P57.11B1 C# layout 审查时在 PowerShell 命令字符串里用 `\"` 试图转义 `rg` 正则中的双引号，外层 shell 把表达式解析为语法错误。Prevention：PowerShell 中的 `rg` 正则参数固定使用单引号，模式需要字面单引号时才改用反引号或 `--%`；禁止套用 C/JSON 的反斜杠引号习惯。
- 2026-08-02 P57.11B1 C# Guest layout owner 探索再次猜测了不存在的 `Tools/AvidScript.CSharpGuest/Model/GuestDataLayout.cs`，并与确定读取并行，导致同批输出丢弃；实际 owner 是 `Tools/AvidScript.GuestIr/Layout/GuestDataLayout.cs` 与 `GuestTypeLayoutResolver.cs`。Prevention：跨工具项目定位类型 owner 时先执行 `rg -l '<TypeName>' Tools -g '*.cs'`，只读取命中的字面文件；命中前不得根据 namespace 猜目录。
- 2026-08-02 P57.11B1 PowerShell parser 预检首次把含 `$errors` 的脚本放进双引号 `pwsh -Command`，变量被外层 PowerShell 提前展开，产生空 `[ref]` 与 empty pipe ParserError。Prevention：从 PowerShell 启动嵌套 `pwsh -Command` 时，整个子脚本使用单引号保护，内部字面字符串使用双引号；Parser API 固定初始化 `$tokens/$errors` 并传 `[ref]$tokens`、`[ref]$errors`。
- 2026-08-02 P57.11B1 Gate 准备再次把可能以 exit 1 表示“无匹配”的 `rg` 与三个确定性 `Get-Content` 放进同一并行编排，导致有效输出整体丢弃，复发了仓库已有的探测隔离错误。Prevention：`rg`/`Select-String` 等内容探测一律独立调用；只有已知存在且预期 exit 0 的字面文件读取可组成 `Promise.all`，发送并行组前机械检查每个命令的合法非零语义。
- 2026-08-02 P57.11B1 计划首次写入了不存在的 `Source/AvidScriptBindings/Private/Tests/AvidScriptBindingDescriptorTests.cpp`，并在查找既有 C# golden 时猜测了不存在的 Editor 私有 Fixtures 目录；实际 Bindings 测试 owner 是 `AvidScriptBindingsBoundaryTests.cpp`，golden 位于仓库级 `Tests/Fixtures/BindingGeneration`。Prevention：阶段计划中的每个 Modify/Test 路径必须先由对应父目录的 `rg --files` 输出证明存在；计划新增文件必须明确标为 Create，禁止把概念 owner 或历史目录结构写成已存在路径。
- 2026-08-01 P57.11A 收口时再次猜测文档/源码文件名，先后使用了不存在的旧设计名、Runtime 根级 session/types 路径和错误的 P57.10 evidence 文件名。Prevention：读取任何非刚创建文件前，先以已确认父目录执行 `rg --files <parent>`，再从输出逐字复制路径；恢复摘要里的概念名也不能替代当前工作树索引。
- 2026-08-01 P57.11A benchmark Harness junction 一度仍指向旧 worktree，使 UBT 验证了 Phase53 源码而非当前候选。Prevention：每次 benchmark build 或 Automation 前先运行 Harness installer `Verify`，并断言 junction source 等于当前 canonical `Benchmarks/PuertsComparison/AvidScriptPerfHarness`；验证失败时先修挂载，不解释构建结果。
- 2026-08-01 P57.11A 混用了 UBT 与 Editor runtime 的插件开关：UBT 使用单数 `-EnablePlugin=`，Editor runtime 使用复数 `-EnablePlugins=`；错误的 runtime 单数参数导致目标测试数量为 0。Prevention：构建和运行命令分别从 tracked harness 命令模板复制开关，启动后首先核对插件加载日志和 `Found N`，`Found 0` 视为配置失败而非测试通过。
- 2026-08-01 P57.11A 仅构建可选 Harness module 虽生成 DLL，却没有把依赖模块写入完整 target receipt，运行时仍加载失败。Prevention：module-scoped build 只承担快速编译诊断；阶段 Gate 必须再执行一次不带 `-Module` 的 no-clean `AvidTPSTemplateEditor` target build，严禁通过 clean Editor target 解决 receipt/staging 问题。
- 2026-08-01 P57.11A five-lane lane id 保留了历史 `adaptive` 名称，但 correctness smoke 明确配置 strict `SemanticProcessEvent`；按 lane 名推断策略会得出错误结论。Prevention：判断实际执行模式必须读取初始化配置和 `BackendInfo.binding_invocation_mode`，历史显示名只用于结果兼容，不作为运行语义证据。
- 2026-08-01 P57.11A 首版 prepared-dynamic oracle 把所有 Semantic 调用都要求为 dynamic hit，忽略 Wasmtime 会让窄 typed host import 优先占用同一精确 identity。Prevention：路由期望按 workload ABI shape 和 linker precedence 推导；typed shape 期望 dynamic hit 为 0，只有 generic ABI workload 才要求 hit 精确等于 iteration，receiver-cache 计数使用独立 oracle。
- 2026-08-01 P57.11A workload enum 已增加到 12 项，但 five-lane smoke 仍硬编码 `WorkloadCount = 10`，导致两个 gameplay workload 没有实际进入循环。Prevention：所有 workload 数组、循环和结果表统一从 enum `Count` 或 catalog size 派生；新增 workload 后静态合同必须同时验证总数和 gameplay 子集数量。
- 2026-08-01 P57.11A Runtime direct prepared-target fixture 首版没有复现生产 load 的 scratch 分配、guest memory、object registry 和 receiver owner capability，造成可避免的重复构建。Prevention：prepared target 集成测试在首次运行前必须逐项镜像生产前置条件：package scratch requirement、有效 guest memory、registry handle/generation、owner policy、instrumentation；Automation 进程 exit 0 仍需解析 `Result={Fail}`、成功数和最终 `TEST COMPLETE` 标记。
- 2026-07-31 P57.10 设计冻结时采纳了“semantic FVector 可以复用 generated `SelfVectorValue (iii)i`”的只读分析结论，随后读取实际物化的 binding descriptor 才确认 semantic `const FVector& -> FVector` ABI 是 `(iifffi)i`；generated packed guest-address ABI 与 semantic expanded f32 ABI 被错误混为同一 shape。同时 profile 中声明的 benchmark `Saved` 相对路径和不存在的源码模块路径被当作当前工作树已物化路径读取。Prevention：任何 typed shape 冻结前必须同时核对 renderer 规则、实际 descriptor `host_import.signature` 与 VM linker signature，generated/semantic 即使 UE 类型相同也不得推定 ABI 相同；配置中的输出路径先通过当前候选 `rg --files` 证明存在，源码 owner 只能从 `Get-ChildItem Source` 的实际模块清单选择。
- 2026-07-31 P57.10 热路径探索首次把未确认存在的 `Source/AvidScriptBindings/Public/AvidScriptBindingFastPath.h` 放进 `Promise.all`，单项读取失败使同批其余确定性结果没有返回；随后又把不存在的 `Source/AvidScriptPerfHarness` 与 `Benchmarks/PuertsComparison/AvidScriptCSharpGuest` 加入检索，并在一次 Git 状态命令中用分号串联三个动作。Prevention：并行读取必须逐项转为 settled result，任一失败不得取消同批成功输出；新路径只能从刚取得的 `rg --files` 或父目录字面清单复制；一条 `shell_command` 只执行一个逻辑动作，禁止 `;`、`&&`、`||`，已知规则复发时在下一批命令发送前机械检查路径与分隔符。
- 2026-08-01 P57.9 compiler profile resolver 取代旧 runtime identity resolver 后，首次干净架构检查仍只匹配 `ResolveAvidScriptWasmtimeRuntimeIdentity`，误报 backend 重复 DLL identity；实现没有重复逻辑，但阶段计划在架构检查真正通过前已把该 checklist 标记完成。Prevention：共享 owner 接口演进必须原子更新架构门禁，检查 backend 与 artifact compiler 都消费新 resolver、RuntimeSupport 独占 DLL export/identity wiring、CompilerProfile 独占 identity 字符串；checklist 只能在干净候选实际通过后勾选，不能根据预期提前完成。
- 2026-08-01 P57.9 正式 controlled-runtime 隔离工程首次只执行 `-Module=AvidScriptPerfHarness`；UBT 为 AvidScript 依赖模块生成了 `.lib`，但没有在候选插件 `Binaries/Win64` 链接其 DLL，calibration 进程在 commandlet 注册前以 exit 1 退出且没有 result。Prevention：新 benchmark 工程先用 Harness module build 暴露局部编译错误，随后必须无 `-Module` 构建完整 `AvidTPSTemplateEditor` target；正式 runner 前逐项确认 Harness 与候选 VM/Runtime/Bindings/Editor DLL 均存在，不得把依赖 `.lib` 成功当成可启动目标闭环，仍禁止清理 Editor target。
- 2026-08-01 P57.9 GC 版受控 DLL 重建后，即使用 `-NoUBTMakefiles` 编译 VM，Wasmtime 外部模块先前生成的 `Definitions*.h` 仍保留旧 `AVIDSCRIPT_WASMTIME_DLL_SHA256=8033...`，runtime 正确拒绝新 `725f...` DLL。Prevention：`Wasmtime.Build.cs` 把 managed marker、DLL 与 import library 声明为 `ExternalDependencies`，让工具链重建自动使 UBT makefile/definitions 失效；验证必须读取生成 definitions 或运行 identity Automation，不能把 C/C++ 重新编译等同于外部模块规则已重新求值，禁止以清理 Editor target 规避失效依赖建模。
- 2026-08-01 P57.9 首版 `WASMTIME_DISABLE_ALL_FEATURES=ON` 只恢复 Cranelift/并行编译/禁日志，遗漏现有 `ResultAbi` fixture 依赖的 Wasm GC 类型支持；受控 runtime 通过其余 26 项但以 `gc types are disallowed` 拒绝该模块。Prevention：性能工具链裁剪前先从完整 VM Automation 建立所需 Wasm proposal 清单；`gc + gc-drc` 与 engine 的显式 `wasm_gc=true` 共同进入 lock、schema、编译器 identity 和 sidecar provenance，不能把语言兼容面当成无关二进制体积优化。
- 2026-08-01 P57.9 受控 SDK 首次接入后，UBT 已链接新 import library，但编辑器开发目录仍残留旧 `Binaries/Win64/wasmtime.dll`；runtime resolver 对首个存在但摘要不匹配的候选立即失败，未继续检查正确的 performance managed install，27 项 Automation 全部被 provenance Gate 拒绝。Prevention：DLL resolver 遍历所有已授权候选并只加载匹配编译期摘要的制品，过期候选被记录后跳过；只有没有任何匹配候选时才汇总严格失败。阶段 Automation 前同时核对 linked expected SHA、实际加载 SHA 与候选顺序，不能假设 `RuntimeDependencies` 会在普通编辑器增量构建中同步 staging 目标。
- 2026-08-01 P57.9 自定义 Wasmtime `config.h` 扩展与内部 `AvidScriptWasmtimeApi.h` 首次都声明 `AVIDSCRIPT_WASMTIME_INLINING_*` 枚举常量；两个 C 头同处一个 translation unit 时全局枚举命名空间碰撞，UBT 以 C2365/C2086 拒绝。Prevention：内部桥接 profile 枚举固定使用 `AVIDSCRIPT_WASMTIME_ENGINE_*` 前缀，第三方扩展保留其导出 ABI 前缀；静态合同检查两套头可共存的命名边界，新增 C ABI 标识前先检索完整 translation unit include closure。
- 2026-08-01 P57.9 Wasmtime CMake 安装阶段首次把 `CARGO_TARGET_DIR` 设为 `Join-Path` 产生的 Windows 反斜杠路径；上游把该值直接嵌入 `cmake_install.cmake`，其中 `\U` 被 CMake 当作非法转义，导致 Rust 编译完成后安装失败。Prevention：所有可能进入 CMake source/generated script 的环境路径先规范化为 `/`；工具链静态合同固定检查该规范化，真实构建仍保留 Cargo target cache 以免重复编译。
- 2026-08-01 P57.9 benchmark identity helper 首次从目标插件目录读取 performance lock，但 sidecar fixture/安装目标只承诺复制运行时二进制，导致 provenance helper 抢先以 `ASP57S2057` 失败并破坏原有制品错误合同。Prevention：benchmark 的冻结配置与 schema 从 helper 自身所在的 tracked AvidScript 源树读取，目标插件根只用于观测实际 DLL/模块摘要；新增 provenance 依赖后必须运行 sidecar fixture 合同，不能只跑脚本 parser。
- 2026-08-01 P57.9 多文件 PowerShell parser 检查把多条 statement 用分号压进单个 `shell_command` 字符串，违反本仓库“一条命令一个逻辑动作、禁止 `;`/`&&`/`||` 链接”的稳定执行规则。Prevention：多文件 parser 预检写成已有 tracked 合同脚本中的循环，或每个文件独立调用；不得为了减少 tool call 把循环初始化、解析和退出判断压成分号链。
- 2026-08-01 P57.9 首次 Wasmtime 源码构建强制使用 Ninja，却没有先进入 MSVC developer environment，CMake 在任何 Cargo 编译前以 `No CMAKE_C_COMPILER could be found` 拒绝。Prevention：Windows Rust `*-pc-windows-msvc` 第三方构建默认使用 `Visual Studio 17 2022` + `-A x64` 生成器，让 CMake/MSBuild 提供 cl/link 环境；若确需 Ninja，必须先由 `vswhere` 定位并导入受控 VsDevCmd 环境。切换生成器前删除工具链自有 CMake build tree，但保留独立 Cargo target cache。
- 2026-08-01 P57.9 零上下文 upstream patch 首次 dry-run 漏掉 `git apply --unidiff-zero`，普通解析器按默认 context 规则拒绝本可应用的补丁。Prevention：tracked patch 如果用 `git diff -U0` 消除空 context 的尾随空格，builder 的 `--check`、实际 apply 和独立正反 dry-run 必须全部显式携带 `--unidiff-zero`，合同测试固定该参数组合。
- 2026-08-01 P57.9 上游 sparse checkout 路径再次依赖猜测：未先列出 `C:\tmp\wasmtime-v45-meta\crates` 就读取推测的 `crates/c-api/src`，导致路径不存在。Prevention：即使熟悉上游仓库，也必须先对已确认父目录运行一次字面目录清单或 `git ls-tree`；只有实际输出中的 child 才能加入 sparse checkout 或后续读取命令。
- 2026-08-01 P57.8 状态快照只做通用 JSON 解析便提交：`documents` 被加入 `p57_8_spec`、`p57_8_plan`、`latest_evidence` 三个 schema 外字段，`next_action` 也被手写成自然语言，随后 `InvokePhaseWorkflow.ps1 status` 以 `ASPW1002` 拒绝。Prevention：任何 `Phase*_State.json` 手工修改在暂存和提交前必须运行 `pwsh -NoProfile -File Build/InvokePhaseWorkflow.ps1 status -Phase <N>`；阶段演进只使用合同内的 `architecture.path/version/sha256/revision_reason` 与 `documents.plan/closeout`，`next_action` 必须逐字采用状态机计算结果，通用 `ConvertFrom-Json` 不能替代 schema Gate。
- 2026-08-01 P57.9 Windows wildcard 路径禁令再次复发：检索已确认的 `Build/PhaseWorkflow` 时又把 `Build/PhaseWorkflow*` 作为 `rg` 路径参数，导致 Win32 路径错误并污染有效命中。Prevention：`rg` 路径位只允许刚由目录清单确认的字面路径；文件筛选一律使用 `-g`，发送前机械检查所有路径参数不含 `*` 或 `?`。
- 2026-08-01 P57.8 字面目录表规则写入后立即猜测子目录：尚未列出 `Source/AvidScriptRuntime/Private` 就假定存在 `Private/Reload`，并把该命令加入确定性并行组，导致整组结果被路径错误丢弃。Prevention：目录探索强制串行两步：第一条命令只能 `Get-ChildItem -Name <已确认父目录>`；模型必须从该条实际输出复制子目录或文件名后，下一条才可读取，任何未出现在刚刚输出中的子路径禁止发送。
- 2026-08-01 P57.8 owner 探索再次混入猜测根并输出过宽：检索 precompiler/manifest owner 时把不存在的仓库根 `Contracts` 与已确认目录并列，实际合同目录是 `Build/Contracts`；同时 pattern 与文档范围过多，输出超过可审阅规模。Prevention：新批次 owner 探索先用已确认模块的 `Get-ChildItem -Depth 2` 建立字面目录表，再按单一责任分别检索 producer、manifest loader、runtime consumer；每次只允许一个已确认根和一个概念组，禁止把猜测根或多个历史 Docs phase 混入源码 owner 查询。
- 2026-08-01 P57.7 集中安全复审晚于首次统一构建：WasmtimeSerialized 首次构建与 Automation 通过后，文档化接口时才发现手工构造的 `WasmBytecode` artifact 可令 execution bytes 与 canonical bytes 分离，必须补一次安全修复和增量验证。Prevention：涉及“双制品/来源/被验证输入与被执行输入”的接口，集中审查清单必须在首次阶段 Gate 前逐项核对 identity binding、byte ownership、trust producer 和实际 execution source；确认被检查字节与被执行字节不可分离后才启动统一构建。
- 2026-08-01 P57.7 并行探测隔离规则写入后立即复发：对真实 `wasmtime.h` 执行可能无匹配的 `rg` 时，仍与 `Get-Item`、`git status` 放在同一编排脚本，`rg` exit 1 再次使整体结果丢失。Prevention：规则写入后的下一条命令必须先做一次机械自检；任何内容匹配探测都作为独立 `shell_command` 执行，确定性查询才允许并行，不能因路径已确认就忽略“无匹配”也是合法结果。
- 2026-08-01 P57.7 已知路径规则复发并扩大并行失败面：检查 Wasmtime C API 声明时把不存在的根级 `ThirdParty` 与已确认的 `Source` 一并传给 `rg`，且该探测与四个确定性静态检查放在同一并行脚本中，单项 exit 1 使其余结果没有被完整回传。Prevention：未知 owner 必须先单独执行仓库级 `rg --files` 索引并从返回值选取字面路径；可能因“无匹配/路径不存在”返回非零的探测不得与确定性 Gate 共用一个会整体失败的编排单元。
- 2026-08-01 P57.5 PowerShell statement pipeline 解析错误复发：两次把 `foreach (...) { ... } | Format-Table` 直接写成顶层语句，PowerShell 报 `An empty pipe element is not allowed`。Prevention：需要把 `foreach` 结果送入管道时，固定先写 `$rows = @(foreach (...) { ... })`，下一行再执行 `$rows | ...`；禁止依赖顶层 statement 后直接接管道。
- 2026-08-01 P57.5 批量外部工具退出码被 `ForEach-Object` 掩盖：批量运行 `wasm-opt -O4` 时 SIMD 项因缺少 `--enable-simd` 失败，但后续文件成功使整个 PowerShell 命令最终 exit 0。Prevention：批量调用 native tool 后立即检查 `$LASTEXITCODE` 并在非零时 `throw`；批次结束还必须按预期文件数、每项摘要或独立 validator 验证产物，不能把 PowerShell 顶层 exit 0 当成每个子进程成功。
- 2026-08-01 P57.5 已确认源码 owner 后仍加入猜测根：检索 Wasmtime 配置时同时传入不存在的仓库根 `ThirdParty`，随后检索上游源码又加入不存在的 `crates/runtime`，有效命中伴随路径错误退出。Prevention：首次由 `rg --files` 或 `git ls-tree` 确认 owner 后，后续命令只逐字复用已存在路径；未知范围从已确认父目录配合 `-g` 收窄，禁止再附加“可能存在”的并列根。
- 2026-08-01 P57.5 controlled-runtime 冻结输入越界：在分析 Cranelift 循环回边后，先修改了受控 kernel 的 WAT/WASM 并跑正式矩阵，随后复核 Phase 57 性能合同才发现该阶段明确禁止修改 kernel 字节；相关提交已完整 revert，结果仅保留为前端研究证据。Prevention：任何 benchmark 优化开始前必须先逐字读取该 Gate 的冻结输入、允许写集和禁止项；formal 候选只允许修改产品运行时、编译器或通用生成管线，kernel/workload/seed/oracle/operation count 不得进入候选 diff。
- 2026-08-01 P57.5 VM build identity 与 benchmark provenance 不同步：SSE-only 探针先修改 VM build identity，未在同一不可变提交中同步所有 benchmark-side expected identity，第一次运行在测量前被 provenance 合同拒绝。Prevention：runtime identity、producer、sidecar expected identity、schema/contract test 必须作为一个原子写集提交；提交前检索旧 identity 的全部 owner，再允许启动 benchmark。
- 2026-08-01 P57.5 上游 Wasmtime 研究 clone 过重：首次使用普通 clone 获取大仓库，长时间下载且没有形成可用工作树，最终被终止。Prevention：只读上游源码研究默认使用 pinned tag 的 `--filter=blob:none --sparse --no-checkout`，再按精确目录展开；只有确需完整历史或构建全部 workspace 时才使用普通 clone。
- 2026-08-01 P57.5 正式 shootout 外层 timeout 小于真实矩阵耗时：单 kernel 五进程测量已完成全部 timed result，但 120 秒外层命令在 merger 落盘前终止，需手动恢复聚合。Prevention：五进程 formal 单 kernel 外层 timeout 至少 180 秒，12-kernel suite 至少 2400 秒；timeout 必须覆盖 UE 启动、校准、采样、merger 和证据写入，不按纯采样时间估算。
- 2026-08-01 P57.5 工具定位宽递归搜索超时：为查找 `wasm-opt` 递归扫描大型 Engine/temp 根，产生无效等待。Prevention：第三方工具优先查 `PATH`、仓库已声明 tool root 和已知缓存；不存在时直接从固定版本官方发布获取并校验摘要，禁止递归扫描整个 Engine、用户目录或 temp 树。
- 2026-07-31 P57.3 diagnostic profile 误用 formal evaluator：在 Phase56 Micro diagnostic runner 已成功后，直接调用要求 profile 存在 `gates` 对象的 `Evaluate-Phase54PerformanceGates.ps1`，脚本在生成输出前按合同拒绝。Prevention：运行 evaluator 前必须读取其 profile 前置条件；diagnostic 只使用 runner correctness 与同轮 nearest-rank 比值，formal evaluator 只接收具有完整 Gate 配置的 formal profile，禁止为了复用脚本临时补造 `gates`。
- 2026-07-31 P57.2 PowerShell 双引号破坏 `rg` 正则：检索脚本中的 `switch ($Mode)` 时把含 `$Mode` 的 regex 放在双引号命令中，PowerShell 先展开空变量，导致 `rg` 收到未闭合分组。Prevention：包含 `$`、反斜线、括号或 PowerShell token 的 `rg` pattern 固定使用单引号；复杂 alternation 优先拆成多个字面检索，禁止依靠多层反斜线转义。
- 2026-07-31 P57.2 Adaptive benchmark 共享 instrumentation 漏审：首次运行 Micro diagnostic 时，校验器错误地把 `TryResolveFusedCallbackReceiver` 的共享接收者缓存计数视为仅属于 Generated/Data lane，导致 `adaptive_native=10` 且业务结果完全正确时仍被判失败。Prevention：新增执行 lane 时必须逐项审计其复用的跨路径 instrumentation，并让 correctness oracle 明确声明允许值，不能仅检查新加的专属计数器。
- 2026-07-31 P57.2 Adaptive correctness 首次修正范围过窄：根据第一个失败样本把 prepared-native 期望硬编码为 `ScalarAddInt32`，下一次运行才暴露 `BatchScalar` 及 gameplay 也复用相同 typed shape。Prevention：benchmark oracle 必须从 guest workload 映射和 binding eligibility 推导统一的 expected-count 函数，禁止根据首个失败 workload 名逐项放行。
- 2026-07-31 P57.2 runtime oracle 与外部 Gate 审计不同步：修正 runner correctness 后才发现 `Evaluate-Phase54PerformanceGates.ps1` 仍只验证 adaptive scalar 且拒绝共享 receiver-cache 计数。Prevention：benchmark 计数合同变更必须同时检索并更新 runtime oracle、外部 evaluator、schema/contract test 四个 owner，阶段诊断通过不等于 Gate 口径已同步。
- 2026-07-31 P57.3 在未提交 production 输入上运行 architecture evidence checker：`CheckAvidScriptArchitecture.ps1` 按设计要求输入与报告 Git tree 完全一致，因此只报告 dirty evidence，不能提供有意义的架构判断。Prevention：实现中先运行 scoped diff/static/build；需要 tree-bound 的 architecture checker 时，先提交拥有的改动，再在 clean candidate 上运行，不能把 dirty-input 拒绝消耗成一次架构审查。
- 2026-07-31 P57.2 benchmark harness 构建 owner 假设：把仓库内 `AvidScriptPerfHarness` 源目录直接当成项目目标的常驻 module，执行 target-plus-`-Module=AvidScriptPerfHarness`；UBT 在编译前以 `Unable to find output items for module` 拒绝。Prevention：benchmark harness 必须先按其 tracked prepare/fixture 流程挂载到目标工程，再由该流程构建；`-Module` 只用于当前 `.uproject` 已启用且 action graph 可解析的模块，不能从源码目录名推断。
- 2026-07-31 P57.2 跨 benchmark schema 指标名误用：给 P53 sidecar 加 adaptive hit 校验时，直接沿用 Phase54 aggregate 的 `logical_operation_count`，但该 process-result 合同的对应字段是 `operation_call_count`。Prevention：跨结果层新增校验前必须读取目标 schema 的 `required` 与 `properties`，逐字复用该层字段名；不能从相邻 runner、aggregate 或 phase contract 推导名称。
- 2026-07-31 P57.1 `rg` 连字符 pattern 规则复发：检索 `-Module=...` 时再次遗漏 `--`，`rg` 在读取文件前把 pattern 解析为参数并拒绝。Prevention：发送任何 `rg` 命令前检查 pattern 首字符；以 `-` 开头时固定写成 `rg <options> -- '<pattern>' <paths>`，即使只是文档或命令示例检索也不例外。
- 2026-07-31 P57.1 宽补丁问题复发：首次把 adaptive reflection 的多个函数改动合并进一个跨区段 patch，其中一个上下文漂移后整包被原子拒绝；没有产生部分写入，但浪费了实现时间。Prevention：超过一个 owner 函数或跨越多个非连续区段的修改，固定按函数边界拆分；每个 patch 后立即复读该函数，再进入下一处，不以同属一个功能为由合并宽 patch。
- 2026-07-31 P57.0 static contract schema assumption: the first frozen-threshold assertion addressed the controlled-runtime profile through a guessed `gates` property, while the tracked schema stores those values under `pc_leadership_gate`; the low-cost contract failed before any product build. Prevention: before adding JSON field assertions, read the tracked producer/profile and use its literal object path; do not infer a shared envelope from a different benchmark profile.
- 2026-07-29 P56.5 发布型架构检查运行时机错误：在集成分支仍有未提交的 Runtime/Bindings 修改时调用 `CheckAvidScriptArchitecture.ps1`，检查器按设计因 evidence commit/tree 与输入字节不一致而拒绝，未产生架构结论。Prevention：该检查器只在候选提交完成且 `git status` 干净后运行；dirty 实现期使用 `git diff --check`、parser 与合同测试，禁止把发布身份检查当作 working-tree linter。
- 2026-07-29 P56.5 Windows wildcard 路径禁令复发：检查 Phase 56 diagnostic profile 时把 `Profiles/Phase56*.diagnostic.json` 直接作为 `rg` 路径参数，Win32 在读取前以非法路径拒绝。Prevention：Windows 下 `rg` 的路径参数只允许已确认存在的字面目录或文件；文件名筛选固定使用 `-g 'Phase56*.diagnostic.json'`，提交命令前机械拒绝路径位置中的 `*` 和 `?`。
- 2026-07-29 P56.5 benchmark harness 根目录再次猜测：已经通过仓库索引得到 `Benchmarks/PuertsComparison/AvidScriptPerfHarness/Source`，后续检索仍额外传入不存在的根级 `Source/AvidScriptPerfHarness`，导致有效结果伴随路径错误退出。Prevention：一次索引确认 owner 后，后续命令逐字复用返回路径；benchmark harness 固定从 `Benchmarks/PuertsComparison/AvidScriptPerfHarness` 起查，禁止再拼接根级 `Source` 候选。
- 2026-07-29 P56.5 Automation 汇报先于精确结果解析：完整进程以 exit 0 结束并报告 `315 tests performed` 后，先口头汇报为全部成功，随后统计日志才发现 `314 Success / 1 Fail`，失败项为 `HotLifecycleResultContract`。Prevention：Automation 完成后固定按顺序核对 performed、`Result={Success}`、`Result={Fail}`、Queue Empty 和 TestExit 五项，在五项统计完成前只能汇报“进程结束/队列执行完”，禁止把 exit 0 或 performed 数等同于全部通过。
- 2026-07-29 P56.5 多路径脚本检索再次加入猜测目录：准备 Automation 入口时把不存在的仓库根 `Scripts` 与已确认的 `Build`、`Tools` 一起传给 `rg --files`，导致命令在返回部分命中的同时以路径错误退出。Prevention：未知脚本入口固定从仓库根单独执行 `rg --files` 后按文件名过滤，或先分别确认每个搜索根存在；禁止把推测目录混入多路径命令。
- 2026-07-28 P54.6 `rg` 模式以连字符开头未加参数终止符：检索 `->Load(` 时 `rg` 把 pattern 当成 flag，在读取前失败。Prevention：任何可能以 `-` 开头的 pattern 固定使用 `rg ... -- '<pattern>' <paths>`；复杂符号模式优先单引号，`--` 必须位于 pattern 前。
- 2026-07-28 P54.6 Console 测试项目误用 `dotnet test`：五个 `*.Tests.csproj` 实际是自带断言入口的 `OutputType=Exe`，`dotnet test` 以 exit 0 返回却没有执行任何用例；随后又在全新 worktree 无 `obj/project.assets.json` 时先加 `--no-restore`。Prevention：首次运行测试项目前读取 csproj；`Microsoft.NET.Test.Sdk` 项目用 `dotnet test`，console harness 固定用固定 SDK 的 `dotnet run --project ... --configuration Release` 完成必要 restore，只有已探测 assets 存在才用 `--no-restore`；证据必须包含 harness 自报用例数。
- 2026-07-28 P54.6 Wasmtime license 合同直接哈希工作树字节：Windows `core.autocrlf` 把合法 LF 文本展开为 CRLF，合同错误报告 license drift。Prevention：文本依赖身份统一去 BOM 并规范化 CRLF/CR 为 LF 后再按 UTF-8 哈希；只有 archive、DLL、LIB、WASM 等二进制制品使用原始字节哈希。
- 2026-07-28 P54.6 集中 Gate 检索仍夹带猜测路径：一次 `rg` 多路径调用加入不存在的 `AvidScriptEditorCSharpBindingEmitter.cpp`，使已有命中也以 exit 1 返回。Prevention：多路径检索的每个路径先由 `rg --files` 确认；未知 owner 只以已确认目录配合 `-g` 检索，禁止把推测 basename 加入读取白名单。
- 2026-07-28 P54.6 单 kernel 构建器用宽泛合同 glob：新增 suite 合同后，旧脚本把所有 `*.contract.json` 都按单 kernel schema 读取，因空 `wat_path` 在 Gate 前失败。Prevention：合同发现必须使用显式 allowlist，或先按 schema/kind 判别再读取 shape-specific 字段；新增合同类型必须运行同目录全部消费者的静态合同。
- 2026-07-28 P54.6 Python 工具环境身份假设：依赖生成任务只留下可导入 module root，控制器一度假定系统 `python` 已安装相同 Wasmtime 包。Prevention：依赖任务必须报告并探测精确 executable 与 module root；否则把固定版本安装到声明的外部缓存，并在调用前同时验证解释器、包版本和入口点。
- 2026-07-28 P54.6 集中修复补丁跨度过大：首次修改 benchmark runner 时把 include、helper、两段 parser、热路径校验和调用点塞进一个多区段 patch；其中一个上下文未匹配，`apply_patch` 正确原子拒绝，未产生部分写入。Prevention：长 owner 的集中修复仍按可独立复读的函数边界拆成小 patch；每次只改一个连续区段并立即复读目标函数，不能因 findings 属于同一批次就合并为单个巨型 hunk。
- 2026-07-27 P54.6 Windows 长路径历史读取再次误用 `rev:path`：集中审查时先执行未引用的 `git show <sha>^:<long-path>`，随后即使改为引用参数，Windows Git 仍把长 `rev:path` 错误落入文件系统路径处理并失败。Prevention：历史长路径内容固定先由机器解析父提交，再使用 `git diff '<parent>' '<child>' -- '<path>'` 或临时 archive 读取；所有包含 `^`、`~`、`{}`、`:` 的 revision 参数始终单引号引用，不再在 Windows 上使用长 `rev:path`。
- 2026-07-27 P54.6 恢复核对再次用分号连接 Git 查询：在一条 `shell_command` 中串联 `git status --short -- Tools` 与 `git log -3 --oneline`，重复破坏“一次调用一个逻辑命令”的审计边界。Prevention：所有 shell 调用在发送前对完整命令字符串做字面扫描；出现 `;`、`&&`、`||` 或多个顶层命令时直接拆分，Git 状态与历史即使属于同一次恢复核对也必须分别调用。
- 2026-07-27 P54.6 ignored SDD 报告被代理强制跟踪复发：Task 2/3 提交把已被 `.gitignore` 排除的 `.superpowers/sdd/.../task-*-report.md` 纳入产品历史，违背仓库洁净度和既有 P53.3 规则。Prevention：编码代理提交前必须执行 `git ls-files --cached .superpowers` 并要求空结果；机械禁止 `git add -f`、`git add --force` 和任何被忽略路径，执行报告仅留本地，产品实现文档写入 `Docs/PhaseXX`。
- 2026-07-27 P54.6 子代理提交 SHA 报告手误：代理最终消息把实际可达的 `6aaff016` 写成 `6aaff015`，控制器首次按错误对象检查而得到 `bad object`。Prevention：阶段状态不得直接采用消息中的手录 SHA；先在 owning worktree 执行 `git rev-parse HEAD`，再用 `git cat-file -e '<sha>^{commit}'` 验证对象可达，只有机器输出可写入 tracker、brief 或发布证据。
- 2026-07-27 P54.6 typed C 窄探针错误选择不完整系统工具链：先后用 `VsDevCmd.bat` 和 `vcvars64.bat` 启动独立 MSVC `/Zs`，随后确认本机 VS 对应 Windows SDK 只有 UCRT 片段且缺少 `um`，两个 translation unit 均在标准头阶段停止，未检查产品代码。Prevention：启动独立 MSVC 探针前先验证同一 toolset 的 VC include 与 Windows SDK `ucrt/shared/um` 四项完整；任一缺失就不再重试临时宿主，C/C++ ABI 统一由仓库既定 UE5.8 UBT/AutoSDK Gate 编译，宿主初始化失败不得算产品失败或编译证据。
- 2026-07-27 P54.6 探索型 `rg` fail-fast 三次连续复发：marker、aggregate initializer 与 Runtime epoch 三次查询都允许零匹配，却仍通过会把退出码 1 转成脚本异常的 `functions.exec -> tools.shell_command` 包装执行。Prevention：探索型 `rg` 机械禁止嵌套在 `functions.exec`；固定直接调用 `functions.shell_command`，由模型读取退出码 0/1。只有 tracked contract 明确要求至少一项匹配时，才允许使用会把 1 视为失败的编排层。
- 2026-07-27 P54.6 计划提交暂存区核对再次混合命令：在一条 `shell_command` 中用分号连接 `git diff --cached --stat` 与 `git status --short`，重复违反一调用一逻辑命令规则。Prevention：Git 暂存内容、工作区状态、提交和身份查询固定为四类独立调用；发送前对完整 shell 字符串做字面扫描，包含 `;`、`&&`、`||` 时不允许执行，即使只是只读核对。
- 2026-07-27 P54.6 上下文恢复未先执行状态机：恢复后先读取 memory、skill、计划和 Git 状态，未把主插件 `Build/InvokePhaseWorkflow.ps1 status -Phase 54` 作为首个项目相关命令。Prevention：每次上下文压缩、网络重连或自动续跑后设置恢复门闩；只有 Phase status 成功返回后才允许读取计划、源码、Git 或外部 evidence，memory/skill 读取不能被误当作已经恢复项目状态。
- 2026-07-27 P54.3 Task 4C fix round 1 只读命令再次混合：读取 profile、validator 与 merger 时在一条 PowerShell `shell_command` 中使用分号连接三个 `Get-Content`，重复违反已记录的单逻辑命令规则。Prevention：同轮已把多文件 parser 固化进 tracked `Test-ControlledRuntimeContracts.ps1`；后续多文件读取必须拆成独立调用，命令提交前机械扫描完整字符串，出现语句分隔符即拒绝发送。
- 2026-07-27 P54.3 Task 4C 结果写回补丁落点错误：为 WAMR `Call` 增加返回 cell 时，宽泛 patch 只锚定通用 `return true`，实际把 `OutResult/Cells` 写回插入后续 `BorrowReadOnlyBytes`；首轮 UBT 才发现未声明标识符。Prevention：同文件存在多个同形返回块时，patch context 必须包含目标函数签名和紧邻语义；静态合同不能只搜索 token 存在，还要隔离目标函数 slice 并拒绝 token 出现在其他 owner。
- 2026-07-27 P54.3 Task 4C parser 探测再次使用分号：一次 PowerShell `shell_command` 用分号串联文件枚举、循环 parser 与输出，违反一调用一逻辑命令。Prevention：shell 调用发送前继续机械拒绝 `;`、`&&`、`||`；多文件 parser 应写入受审合同脚本后单独调用，不能把临时循环压成单行。
- 2026-07-27 P54.3 上下文压缩恢复顺序遗漏：恢复后先读取外部 formal attempt 与 worktree Task 4B 报告，之后才在主插件运行 `Build/InvokePhaseWorkflow.ps1 status -Phase 54`。这些读取没有修改状态，但外部 evidence 延续仍属于当前 Phase 的恢复动作，不能绕过状态机先行规则。Prevention：网络重连、上下文压缩或自动续跑后的首个项目相关工具调用固定为主插件当前最高 Phase 的 `status`；外部 attempt、SDD ledger、agent 报告、Git 和源码读取全部排在其后，不能以路径位于仓库外为例外。
- 2026-07-27 P54.3 compact evidence 生成命令仍含 PowerShell 分号：Task 4B 的一次外部 evidence 生成调用在 hashtable 项之间使用 `;`，虽然只有一个逻辑目的、没有修改冻结候选或 raw attempt，仍违反 shell 字符串字面禁令。Prevention：`shell_command` 的分隔符预检作用于完整命令文本，不区分分号位于 statement、hashtable、script block 或只读表达式；需要结构化临时逻辑时写入已审查的 tracked/外部 `.ps1` 脚本后以单一 call operator 调用，或拆成不含分号的独立命令，禁止以内层语法为例外。
- 2026-07-27 P54.3 WSL review-package 无法读取 Windows linked-worktree gitdir：从 PowerShell 工作目录调用 Superpowers 的 WSL `review-package` 时，linked worktree 的 `.git` 文件包含 `C:/.../.git/worktrees/...` Windows 绝对路径；WSL Git 把它追加到当前 `/mnt/c/...` 路径后形成无效 gitdir，命令在生成审查包前以 `not a git repository` 失败。Prevention：Windows linked worktree 的审查包固定使用原生 Windows Git 生成；需要运行 Bash helper 时只用于不读取 Git metadata 的纯文本提取，除非先在独立临时目录建立 WSL 原生 checkout，禁止假定 WSL Git 能解析 Windows worktree gitdir 指针。
- 2026-07-27 P54.1 共享 worktree 暂存区污染提交：实现代理实际与控制器共享 Phase worktree，并在验证完成前暂存 Task 1 文件；控制器随后只对计划文档执行精确 `git add`，但直接 `git commit` 仍把暂存区内代理代码一并纳入 `04ea016`，导致文档和未完成验收的实现混在同一提交。Prevention：任何代理运行期间控制器禁止提交共享 worktree；每次 commit 前必须执行并人工核对 `git diff --cached --name-only` 与本次 owned path 清单，即使刚执行的是精确 `git add` 也不能假设暂存区此前为空；并行编码代理默认使用独立 Git worktree，无法证明隔离时只允许只读 sidecar。
- 2026-07-27 P54.0 worktree 探测命令再次使用分号连接：为一次读取 `git-dir`、`common-dir`、branch 与 superproject，把四个只读 Git 查询放进同一 PowerShell 命令。虽然没有修改仓库，但再次破坏“一次调用一个逻辑命令”的审计边界。Prevention：每次发送 `shell_command` 前执行字面扫描，命令字符串不得包含 `;`、`&&`、`||`；即使属于同一 worktree 探测步骤，也拆成独立调用，不能以“只读”或“同一目的”为例外。
- 2026-07-26 P53.5 最终 Gate 临时工程路径预算遗漏：隔离工程位于描述性长目录下，生成 binding reference source 的完整路径达到 261 字符；`pwsh 7 Test-Path` 为 true，但 C# build service 使用的 Windows PowerShell 5 返回 false，prepare 以 `ASBI4202` 拒绝。Prevention：会生成深层 Saved/Intermediate 制品的 UE Gate 工程使用短且唯一的根目录，并在 UBT 前计算最长已知制品路径，Windows PowerShell 5 消费链固定保留低于 240 字符的预算；不能只验证工程根可创建。
- 2026-07-26 P53.5 最终 Gate 全仓 parser 宿主错误：Gate 外层继续由 Windows PowerShell 5 执行，并用 5.1 parser 扫描包含合法 PowerShell 7 管道续行的 `CheckAvidScriptArchitecture.ps1`，在架构门禁执行前产生假失败。Prevention：最终 Gate 外层、全仓 parser 和架构脚本统一使用 `pwsh 7 -NoProfile`；只有明确验证 Windows PowerShell 5 行为的合同才启动独立 `powershell.exe` 子进程，不能让旧宿主定义全仓语法上限。
- 2026-07-26 P53.5 最终 Gate PowerShell 宿主选错：新增的 Phase 53 benchmark 合同使用 `Test-Json`，一次性驱动却沿用 PhaseWorkflow 合同的 `powershell.exe` 宿主，Windows PowerShell 5 在执行合同前以 command-not-found 失败。Prevention：PhaseWorkflow 拒绝路径合同保持隔离 `powershell.exe -NoProfile`；使用 PowerShell 7 API 的 benchmark/dependency 合同固定使用已确认的 `pwsh.exe -NoProfile`，Gate 清单按脚本运行时要求分组，不能共用单一宿主。
- 2026-07-26 P53.5 最终 Gate 预期 stderr 误判：Windows PowerShell 5 的 native logging helper 在 `$ErrorActionPreference = 'Stop'` 下把 Guest 负例故意写出的 `ASCG1001` stderr 转成终止异常，尽管测试为 68/68 且进程退出码 0。Prevention：需要保留 native stdout/stderr 的 Gate helper 在最小调用作用域使用 `Continue` 采集两条流，恢复外层偏好后只按捕获的 `$LASTEXITCODE` 判定；stderr 文本本身不能替代退出码。
- 2026-07-26 P53.5 最终 Gate SDK 探测 cwd 错误：一次性驱动在项目根而不是候选插件目录调用固定 `$env:USERPROFILE\.dotnet\dotnet.exe --version`，因此没有读取候选 `global.json` 并命中 10.0.301；Gate 在测试前失败。Prevention：SDK 版本探测与所有 .NET 命令必须在候选插件 cwd 内执行，先验证输出恰为 8.0.416，再启动测试；固定 host 路径不能替代 cwd 下的 `global.json` 解析。
- 2026-07-26 P53.5 PhaseWorkflow 入口路径猜测：恢复主分支状态时直接调用不存在的 `Build/PhaseWorkflow/InvokePhaseWorkflow.ps1`，真实入口为 `Build/InvokePhaseWorkflow.ps1`。Prevention：状态机命令统一使用仓库已确认入口 `Build/InvokePhaseWorkflow.ps1`；若入口无法执行，先用 `rg --files | rg 'InvokePhaseWorkflow\.ps1$'` 定位，不从目录名推导路径。
- 2026-07-26 P53.5 状态检查分号禁令再次复发：恢复收尾后把 `git status`、`git log` 与 `git diff --check` 放进同一 PowerShell 调用；命令虽只读且成功，但再次破坏“一次调用一个逻辑命令”的审计边界。Prevention：发送任何 `shell_command` 前机械扫描 `;`、`&&`、`||`；状态、身份和差异检查分别调用，不能因同属 Git 检查而合并。
- 2026-07-26 P53.5 发布证据不可变性误判：P53.3/P53.4 报告把 `C:\tmp` attempt 称为不可变证据，但没有发布 aggregate SHA 或可分发的机器摘要；本机目录可变且其他机器无法审计表格。Prevention：正式性能结论只引用带 SHA-256 的 raw aggregate，并在仓库发布去路径、去 raw sample 的 compact statistics/provenance 摘要；阶段外目录只能称为本机原始证据，不能称为发布制品。
- 2026-07-26 P53.5 formal provenance 信任边界错误：首版 runner 只验证命令行 commit/hash 格式并原样写入结果，Puerts Verify 也只检查 marker 和少量文件存在；调用者可把陈旧或被改动的 candidate/artifact 标成锁定身份。Prevention：正式 runner 在启动进程前重验 clean-project marker、candidate HEAD/tree/clean、tracked canonical profile/lock、Puerts 安装内容、WASM/manifest 与 Editor executable identity；命令行值必须和实物及 tracked lock 三方一致。
- 2026-07-26 P53.5 委派基线完整 SHA 抄写错误：未执行 `git rev-parse HEAD` 就把不存在的 `71753da50e...` 写入 SDD ledger 和三个 agent brief；agents 被迫回退到本地真实 `71753dad890...`。Prevention：任何委派 brief、candidate marker 或 Gate identity 中的完整 Git 对象都从当前仓库命令输出逐字复制，禁止由短 SHA 补全。
- 2026-07-26 P53.5 Saved artifact 路径猜测：准备复跑时直接读取猜测的 `Saved/AvidScript/PerformanceComparison`，真实制品位于 `Saved/AvidScriptCSharpGuest/Profiles/profile_phase53_perf`。Prevention：首次读取生成目录先枚举现有 `Saved` 子树或检索生产 owner 中的固定路径常量，再使用确认路径；阶段摘要中的概念名不能当磁盘路径。
- 2026-07-26 P53.5 Git 只读命令再次用分号连接：审查 agent commit 时把 `git show --stat` 与完整 `git show` 放进同一 shell 调用。Prevention：即使目标 commit 相同，规模检查和内容检查仍是两个逻辑命令；发送前机械扫描 `;`、`&&`、`||`。
- 2026-07-26 P53.5 Puerts 计时合同 owner 漏审：首轮 lane-parity patch 已让 JS workload 不再返回 checksum，但 `AAvidScriptPerfFixture::RunPuertsWorkload` 仍调用 `FJsObject::Func<int32>`，静态合同只检查 runner/JS token而没有读取实际 invocation owner。Prevention：跨语言计时边界变更必须同时复读注册点、存储 owner、调用 owner 和结果读取 owner；合同明确断言 `Action`/`Func` 形态，UE non-unity 可见性由直接 include 验证。
- 2026-07-26 P53.3 关键路径委派错误：把 10 项 warm matrix 这一立即阻塞任务交给子代理后等待，代理只扩展静态契约测试而未进入实现，用户可见进度停滞。Prevention：当前下一步直接依赖的实现由主代理本地完成；子代理只承担不阻塞主线的独立 sidecar，30 秒内无产品代码变化时立即要求状态并收回任务，禁止用更多测试代替已明确要求的实现。
- 2026-07-26 P53.3 集中验收前遗留合同未同步：warm core 首次真实 UE5.8 构建暴露旧裸 include `JsonSerializer.h`/`JsonWriter.h`，10 项正确性首次运行又因 Automation 仍写死 workload count 7 失败。Prevention：扩展稳定枚举时全仓检索固定 count 和旧终值；UE5.8 Json include 固定使用 `Serialization/JsonSerializer.h` 与 `Serialization/JsonWriter.h`；集中验收前执行该模块的 include owner 与枚举消费者扫描。
- 2026-07-26 P53.3 长构建短 timeout 错误复发：首次 no-clean UBT 再次给 `shell_command` 1 秒 timeout，包装进程在 5 秒下限退出但子 UBT 继续运行，必须额外追踪进程与引擎日志。Prevention：UBT、Automation 和 C# profile 首次调用固定使用至少 600000 ms timeout；只有工具明确返回 running cell 才使用 `wait`，不再用短 timeout 模拟后台执行。
- 2026-07-26 P53.3 SDD 报告跟踪错误：实现代理使用强制暂存，把已被忽略的 `.superpowers/sdd/.../task-P53.3-clean-project-report.md` 纳入产品提交。Prevention：`.superpowers` 必须持续保持忽略；每次 Phase 提交前用显式 commit path allowlist 校验全部 staged paths，并机械拒绝任何 `.superpowers/` 路径，禁止 `git add -f`。
- 2026-07-26 P53.3 正式 benchmark 共享校准错误：首版用最快 Native lane 达到 5 ms 所需的迭代数驱动同一 workload 的全部 lane，WAMR 因而被迫执行数百万次 crossing，单个 timed process 运行 16 分钟仍未完成。Prevention：每个 workload 与 lane 独立校准，timed request 冻结二维 iteration matrix，正确性按各 lane 自身迭代数与 Native oracle 比较，最终只用 ns/op 等归一化指标跨 lane 对比；正式运行前先用 1 process x 5 samples 诊断 profile 验证总时长。
- 2026-07-26 P53.3 可选插件参数错误复发：已有 P53 记录明确 UE5.8 UBT 使用单数 `-EnablePlugin=`，本轮仍误用运行时形式 `-EnablePlugins=`，UBT 成功但没有编译 Harness，形成无效证据。Prevention：Harness UBT 固定使用 `-EnablePlugin=AvidScriptPerfHarness` 或由候选 `.uproject` 显式启用；有效证据必须出现 `Compile/Link ... AvidScriptPerfHarness`，仅 target succeeded 或命令行含插件名不计数。
- 2026-07-26 P53.3 全局非 Unity 构建错误复发：为绕过 Harness 遗留 unity 产物再次向完整 Editor target 传入 `-DisableUnity`，触发 23,503 个源码引擎动作并留下子进程；终止后恢复普通配置又产生 997 个 relink 动作。Prevention：项目级与模块级 UBT 机械禁止全局 `-DisableUnity`；可选插件派生产物异常时只隔离该插件的 `Intermediate/Binaries`，随后用正确 `-EnablePlugin=` 普通增量构建；包装进程超时后立即按启动时间与命令行核查并结束对应 UBT/MSBuild 子进程。
- 2026-07-26 P53.4 候选 BuildId 顺序错误：在较新的 benchmark 候选上运行项目 target UBT 后，直接复用较早 `2ce7a9b` 候选启动正式矩阵；UE5.8 因其 Harness module manifest BuildId 落后于当前 Editor target 而跳过模块，校准进程在采样前退出。Prevention：正式 attempt 前最后一次 UBT 必须针对即将运行的同一候选；构建其他候选会使旧候选 manifest 失效，runner 启动前检查 target receipt 与 Harness `UnrealEditor.modules` BuildId 相等。
- 2026-07-26 P53.4 Windows `rg` 通配路径禁令复发：读取五个 run 的 `stdout.log` 时再次把 `runs\*\stdout.log` 作为 Windows 路径参数，`rg` 在读取前返回 OS error 123。Prevention：递归搜索只传字面目录并使用 `-g stdout.log`；命令发送前机械拒绝 path position 中的 `*` 和 `?`。
- 2026-07-26 P53.4 热路径契约结束锚点猜测错误：新增静态契约用不存在的 `ReadAvidScriptRuntimeObjectHandle` 作为 `ResolveAvidScriptRuntimeHandle` 的结束边界，测试在检查生产优化前即以 `ASP53H1001` 失败。Prevention：源码区段契约的起止符号必须先由当前 `rg` 结果确认；结束锚点固定选同文件中真实相邻定义，并保留“无法隔离区段”的稳定拒绝诊断。
- 2026-07-26 P53.4 benchmark 表格半位舍入不一致：手工转录 `puerts_v8_static/vector_value=1032.175` 时写为 `1032.17`，而同表其他半位值采用 away-from-zero。Prevention：正式表格由 `aggregate.json` 机械提取，并以 decimal `MidpointRounding.AwayFromZero` 统一保留两位；发布前逐 lane/workload 对照原始聚合结果。
- 2026-07-26 P53 首版 benchmark 公平性设计错误：共享 fixture 最初是 `UObject`，但 AvidScript profile Self 合同要求 `AActor`；三条 lane 又各自创建对象，Puerts static 通过额外 UObject 参数的全静态 proxy 调用，属性 workload 实际调用 getter/setter 函数，且 AvidScript 用额外 UFUNCTION crossing 发布 checksum。Prevention：跨框架 benchmark 在首次 UBT 前完成独立语义复审；所有 lane 复用同一 Actor fixture，static lane 使用实例 `.Method/.Property`，property workload 必须走正式属性表面，guest 结果通过计时外 state/memory slot读取，不允许某一 lane 独占额外 crossing。
- 2026-07-26 P53 事件 seed 精度错误：首版准备把任意 int32 seed 数值传入 `avid_on_event` 的 float 参数，固定 seed `1397313073` 无法被 float 精确表示，会让 C# checksum 与 Native/JS 确定性分叉。Prevention：复用 float 事件 ABI 的整数 benchmark seed 固定在有符号 24 位精确范围并由 profile validator 断言；需要完整 int32 输入时新增通用 int32 export/call contract，禁止依靠 float 数值往返或 NaN bit-cast。
- 2026-07-26 P53 PowerShell 单行输出解包错误：dependency installer 的 Git helper 返回单行字符串时被 pipeline 自动解包，调用方直接 `(...)[-1]` 取得最后一个 `Char`，随后 `Trim()` 失败；安装在 stage 创建前停止。Prevention：所有可能返回一行或多行的 native helper 消费点先用 `@(...)` 捕获，再把末项显式转为 `[string]`；合同至少覆盖 remote、commit 和 tree 三个单行命令。
- 2026-07-26 P53 第三方安装无条件 fetch 错误：bare cache 已含固定 commit/tree，安装器仍每次执行网络 fetch，第二次安装因 GitHub TLS close 中断而在写工程前失败。Prevention：固定依赖先用 `git cat-file -e '<sha>^{commit}'` 验证本地对象，只在对象缺失时 fetch；archive、hash 和 install verify 必须支持完全离线重放。
- 2026-07-26 P53 默认禁用 benchmark plugin 的 UBT 参数错误：为避免普通项目构建依赖 Puerts，harness 正确设置 `EnabledByDefault=false`；首次 module build 未启用插件，第二次又误用不存在的复数参数 `-EnablePlugins=AvidScriptPerfHarness`，两次均以 `Unable to find output items` 很快停止。UE5.8 `TargetRules` 的实际命令行合同是单数 `-EnablePlugin=` / `-BuildPlugin=`。Prevention：可选 benchmark plugin 的 UBT 命令显式传 `-EnablePlugin=AvidScriptPerfHarness`，需要编译但不启用运行时功能时可传 `-BuildPlugin=AvidScriptPerfHarness`；Editor-Cmd/Automation 使用引擎可识别的运行时插件启用方式，并在启动前先 Verify Puerts lock/marker。普通 AvidScript Gate 不启用该插件。
- 2026-07-26 P53 benchmark Unity 常量重名：fixture 与 runner 两个 `.cpp` 的匿名命名空间都声明了通用 `MixMultiplier` / `MixIncrement`，首次真实 Harness 编译在 Unity translation unit 中报 C2374/C2086。Prevention：benchmark 模块的匿名 helper 与常量使用 owner 前缀；新增源码在集中 UBT 前对同模块 `.cpp` 做符号碰撞扫描，并保留 Unity 构建作为最终证据。
- 2026-07-26 P53 dependency Verify 缓存根遗漏：固定 V8 资产位于 `C:\tmp\AvidScriptPhase53`，复验命令却省略 `-CacheRoot`，安装器按 `%TEMP%\AvidScriptPhase53` 查找并以稳定分类 `ASP53D1300` 拒绝。Prevention：Phase 53 的 install/verify/runner manifest 必须显式记录并传递同一 cache root；不得依靠机器默认临时目录推断固定资产位置。
- 2026-07-26 P53 Puerts producer DLL 范围遗漏：首次 UBT 指定 `JsEnv` 与 Harness，依赖图编译了 `WasmCore` 并只生成 import library，却没有链接 `UnrealEditor-WasmCore.dll`；Editor-Cmd 挂载 Puerts 后在测试发现前拒绝加载。Prevention：第三方插件运行前从固定 `.uplugin` 枚举目标平台会加载的 Runtime/Editor module，并在 no-clean UBT 中显式构建全部 producer DLL；不能把依赖对象编译或 `.lib` 生成当成可加载插件证据。
- 2026-07-26 P53 Puerts module loader 根目录遗漏：Harness 首版自定义 loader 只允许 benchmark `Content/JavaScript`，V8 初始化需要的 `puerts/first_run.js`、`argv.js` 等官方 runtime module 全部查找失败。Prevention：自定义第三方脚本 loader 必须组合 workload root 与固定第三方 runtime root，对每个候选做规范化和根目录边界检查；不得通过复制上游 runtime 文件或放宽到任意磁盘路径来绕过。
- 2026-07-26 P53 首次可选插件运行缺少 module manifest：模块级 UBT 已链接所有 DLL，但不会为首次启用的项目插件生成 `Binaries/Win64/UnrealEditor.modules`，PluginManager 仍报告找不到模块。Prevention：固定依赖首次安装后执行一次带 `-EnablePlugin=AvidScriptPerfHarness` 的 no-clean 项目 target build 生成 target receipt 与各插件 module manifest；后续源码迭代恢复 module-scoped UBT，禁止 clean target。
- 2026-07-26 P53 Puerts 入口模块格式错误：Harness 用 `FJsEnv::Start()` 启动的 workload 首版使用 ESM `import`，Puerts 默认 `modular.js` 以 CommonJS `require` 执行入口并报 `Cannot use import statement outside a module`。Prevention：Puerts Unreal benchmark 入口遵循固定版本的默认 CommonJS 合同，使用 `require("ue")` 与 `require("puerts")`；若未来对标 ESM，必须作为独立 lane 并记录 loader/runtime 配置。
- 2026-07-26 P53 commandlet 模块加载阶段错误：`UAvidScriptPerfPrepareCommandlet` 已进入 Harness DLL，但模块 descriptor 使用 `PostEngineInit`，`-run=AvidScriptPerfPrepare` 在该阶段之前解析 commandlet class 并报告找不到。Prevention：包含 `UCommandlet` 的可选 Editor 模块使用 `Default` 或更早且经过真实 `-run=` 验证的 LoadingPhase；不能以 DLL/UHT 编译成功代替命令行类发现证据。
- 2026-07-26 P53 C# workload 语义子集错误：首个 headless profile build 到达正式 Roslyn semantic 后，以 `ASCS2001 roslyn:Switch` 拒绝 benchmark 源码；现有 AvidScript C# 子集尚未支持 switch operation。Prevention：benchmark workload 必须先通过当前正式 frontend 支持矩阵；workload 分派放在计时循环外并使用受支持的 `if/else`，避免把分派分支混入每次 crossing。`switch` 支持作为语言成熟度缺口单独规划，不在 benchmark 中伪装为已支持。
- 2026-07-26 P53 生成 facade 多级隐式转换假设：C# workload 直接把 `AAvidScriptPerfFixture` 赋给 `UObject`，但生成 facade 只定义相邻 `AAvidScriptPerfFixture -> AActor` 与 `AActor -> UObject` 转换，C# 不串联两个用户转换，编译失效后 semantic 以 `ASCS3004` 关闭。Prevention：跨两个以上生成对象层级时显式逐级转换；profile artifact 必须通过真实 C# compilation，不能只验证 facade 文本含目标类型。
- 2026-07-26 P53 C# semantic/lowering 支持矩阵断层：semantic 将内建 unary、`++/--` 与 conditional flow capture 标记为 supported，但 Guest lowering 没有对应规则，正式 profile 直到 `guest_ir_failed` 才失败。Prevention：`SemanticOperationProjector` 每新增或放行一种 operation kind，必须在 `AvidScript.CSharpGuest.Tests` 使用真实 Roslyn source 覆盖 lowering 与 Guest IR validation；上层 wrapper 必须保留底层编译器诊断，不能只返回阶段级错误。
- 2026-07-26 P53 Guest IR/backend 算子词汇漂移：semantic 投影 canonical `bitwise_and/or/xor`，WASM backend 只识别旧名 `and/or/exclusive_or`，导致有效 Guest IR 在 backend 才以 `ASWB1002` 失败。Prevention：canonical operator vocabulary 由 semantic model 定义；backend 对每个 canonical 整数算子同时覆盖 i32/i64 的编译测试，历史别名只做兼容，不得成为唯一实现名称。
- 2026-07-26 P53 第三方缓存设计错误：首次为阅读 Puerts 源码对 partial clone 执行完整 checkout，网络下载超时后留下数千条 staged deletion；该目录位于临时区且未影响产品仓库。Prevention：固定第三方源码使用 bare/filter clone，只通过 immutable commit 的 `git show`、`git archive` 或临时纯导出目录读取；安装器不得依赖可变 checkout，也不得用 checkout dirty state 代表已固定对象的身份。
- 2026-07-26 P53 backend 完整性路径错误：安装前复审发现 verifier 把 V8 header 写成 `Inc/include/v8.h`，而 Puerts `JsEnv.Build.cs` 直接把 `Inc` 加入 include path，正确身份文件是 `Inc/v8.h`。Prevention：第三方完整性 marker 必须由固定提交的真实 Build.cs/include contract 推导；大型归档首次解压前先对照上游消费路径，不能凭常见目录布局猜测。
- 2026-07-26 P53 schema 错误分类遗漏：dependency installer 在全局 `ErrorActionPreference=Stop` 下直接调用 `Test-Json`，无效 lock 先被 PowerShell 自身异常终止，未返回稳定的 `ASP53D1003`。Prevention：预期以布尔值判定的 schema 校验在最小作用域使用 `-ErrorAction SilentlyContinue`，随后由产品脚本统一抛出稳定分类；合同必须覆盖非法 remote、commit 和 asset URL。
- 2026-07-26 P53 隐私扫描自匹配错误：architecture test 把待检测的用户目录和私钥头字面量写在自身源码中，再扫描整个 benchmark 目录，导致两次假阳性。Prevention：安全扫描规则 token 运行时分段构造，或从扫描集合显式排除规则定义文件；规则合同必须先证明正常 tracked tree 可通过，再用独立 fixture 证明恶意内容会被拒绝。
- 2026-07-26 P53 恢复顺序错误复发：网络/上下文恢复后先执行了目录枚举与 `git status`，之后才调用最高 tracked Phase 的状态机。没有修改产品文件，但破坏了“状态机先行”的可审计顺序。Prevention：恢复后的首个仓库操作只允许从已知最高 state 执行 `Build/InvokePhaseWorkflow.ps1 status -Phase <N>`；目录、Git、计划和第三方探测全部排在该命令之后。若新 Phase 尚未自举，先检查上一 Phase 状态，再读取新 Phase 文档并立即 `start`。
- 2026-07-26 P52 Gate 包装器错误记录：外部脚本没有先做 parser preflight，`"$RelativePath:"` 在任何产品测试启动前触发变量作用域语法错误；随后又用原始文件 SHA 比较两个 checkout，把内容相同的 CRLF/LF 脚本误判为不一致；主架构脚本还被错误交给 Windows PowerShell 5.1，无法解析 PowerShell 7 跨行管道。Prevention：Gate orchestrator 落盘后先用 PowerShell parser 做纯语法检查；tracked 文本跨 worktree 等价性使用 Git blob 或统一 LF 后比较，不用原始字节 hash；生产 `Build/*.ps1` 默认使用 `pwsh -NoProfile`，只有明确验证 Windows PowerShell 5.1 的合同宿主才使用 `powershell.exe`。Attempt 日志必须保留，最终 attestation 只引用通过日志。
- 2026-07-26 P52 Gate 证据 schema 错误记录：首次报告给 Static、DotNet、PowerShell、Build 和 Performance 检查附加了扩展 `counts`，工作流以 `ASPW3023` 拒绝；状态未改变。Prevention：Phase Gate v1 只有 `Automation` category 可以发布 `counts`，其他 category 固定为 `null`，数字写入 `completion_marker` 或 tracked Gate summary；报告生成后先调用证据 validator/`attest`，拒绝报告另存 Attempt，不覆盖或伪装为已验证证据。
- 2026-07-26 P52 Git revision 与 SDK 证据错误记录：PowerShell 中未引用 `HEAD^{tree}` 会让花括号参与 shell 解析；Gate 元数据又从 wrapper 根目录查询 `dotnet --version`，显示 10.0.301，而实际测试在具有 pinned `global.json` 的候选 cwd 下使用 8.0.416。Prevention：所有 revision expression 固定写成单引号参数，例如 `git rev-parse 'HEAD^{tree}'`；工具链版本必须在与被测命令完全相同的 working directory 和环境中采集，并在启动测试前断言期望版本。
- 2026-07-25 P51.3 路径探测错误复发：上下文恢复后依据摘要中的计划简称直接拼写两个 `Docs/Phase51` 文件名，读取命令因文件不存在失败；没有写盘，但重复违反了已知的路径发现规则。Prevention：摘要中的文件描述不得当作路径证据；即使目录与 Phase 已知，也必须先用 `rg --files <dir>` 取得精确文件名，再复制该输出用于后续读取，禁止凭阶段编号补全路径。
- 2026-07-25 P51.3 验证流程错误记录：实现仍只存在于插件内的 Git worktree 时，直接从项目根运行 `-Module=AvidScriptEditor`；UE 项目固定引用主插件目录，因此实际编译并加载了主分支旧源码，聚焦自动化继续执行旧的 factory import 断言。Prevention：worktree 批次进入 UE 验证前，必须先提交候选、核验主目录受保护文件、以 `--ff-only` 同步主插件，再从项目根构建；自动化失败若行号/断言文本与 worktree 不符，先核对主目录 `HEAD` 和加载 DLL 来源，不盲目修改实现。

- Do not use `Build.bat -Clean` just to make UBT notice new plugin `.cpp` or `.h` files. On 2026-07-02 this accidentally triggered a heavy `AvidTPSTemplateEditor` rebuild. Treat full target clean as destructive-to-time and run it only after explicit user approval.
- For normal AvidScript C++ iteration, prefer module-scoped validation first:

```powershell
& "C:\UnrealEngine\Engine\Build\BatchFiles\Build.bat" AvidTPSTemplateEditor Win64 Development "-Project=<ProjectRoot>\AvidTPSTemplate.uproject" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles -Module=AvidScriptRuntime
```

- 如果 Runtime 的公开头文件改变跨模块可见的 struct/class 布局、虚函数表或 inline API，不得只重编 `AvidScriptRuntime` 就启动完整 Editor 自动化。同步对每个直接消费者执行模块级增量构建，当前至少包括 `-Module=AvidScriptEditor`；这不需要也不允许借机 clean Editor Target。
- 2026-07-10 P25.3 mistake record: `FAvidScriptWasmSmokeResult` / `FAvidScriptWasmRuntimeMetrics` 增加公开字段后只编译了 `AvidScriptRuntime`，旧 `AvidScriptEditor.dll` 继续按旧结构布局调用 runtime，导致 `AvidScript.Editor.CommandLauncher.BuiltManifestReloadSmoke` 在读取 Actor 时访问冲突。单独增量编译 `AvidScriptEditor` 后原用例和完整 95 项回归通过。Prevention: 公开 runtime ABI/布局变化后先列出并重编直接消费者模块，再运行跨模块自动化；若出现看似无关的指针损坏，先检查 DLL/头文件 ABI 是否同步。
- If UBT says the target is up to date after adding plugin source files, do not clean the Editor target. First retry with `-NoUBTMakefiles -Module=AvidScriptRuntime`, inspect UBT output for makefile invalidation, and only escalate to broader rebuilds with user approval.
- On 2026-07-02, P3.1 exposed a stale unity-file trap: `-NoUBTMakefiles -Module=AvidScriptRuntime` can succeed while a newly added `.cpp` is not yet included in `Intermediate/.../Module.AvidScriptRuntime.cpp`. If a new source file should have failed or changed behavior but the build unexpectedly succeeds, inspect the generated module unity file or `LiveCodingInfo.json`. Then run the same module-scoped build once without `-NoUBTMakefiles` so UBT can report `Invalidating makefile ... (source file added)`. This is not a target clean. If the first refreshed build links against a new test but not a new implementation `.cpp`, rerun the same module-scoped build once more before considering broader rebuilds.
- 2026-07-05 P6.3 mistake record: a newly added automation `.cpp` was not registered, and `Automation RunTests AvidScript.Guest.AvidScript` returned `0 tests performed`. Root cause was the cached UBT unity file not including `AvidScriptFrontendTests.cpp`; staging the new source file and rerunning the module-scoped build without target clean produced `Invalidating makefile ... (source file added)`. Prevention: after adding any new plugin `.cpp`, verify `Intermediate/.../Module.AvidScriptRuntime.cpp` includes it before treating a `0 tests matched` automation result as a test filter problem.
- 2026-07-05 P9.2 workflow note: the same stale source-list trap applies to `AvidScriptEditor`. A full Editor target build can report `Target is up to date` after adding a new Editor module `.cpp`; verify `Intermediate/.../Module.AvidScriptEditor.cpp` includes the new file or run the module-scoped build with `-Module=AvidScriptEditor` until UBT reports `Invalidating makefile ... (source file added)`. Do not use target clean for this.
- 2026-07-05 P12.1 workflow note: a RED build after adding a new `AvidScriptEditor` test `.cpp` first reported `Target is up to date` until rerun with `-NoUBTMakefiles -Module=AvidScriptEditor`; after adding the matching implementation `.cpp`, the first GREEN build linked the test but not the implementation and required one more module-scoped source rescan. Prevention: when TDD adds test and implementation `.cpp` files in separate steps, verify the generated `Intermediate/.../Module.AvidScriptEditor.cpp` contains both files before trusting build results. This is still not a reason to clean the Editor target.
- 2026-07-05 P12.2 mistake record: after `apply_patch` failed, a PowerShell string replacement intended to insert `AvidScriptEditorSourceConfig.h` into `AvidScriptEditorModule.cpp` did not match the CRLF-shaped text, and the next build failed on missing source config symbols. Prevention: after fallback PowerShell edits, read the touched include/function block with `Get-Content` or `Select-String` before building, not only after a failure.
- 2026-07-05 P12.3 mistake record: a root-doc commit-hash backfill script passed multiple child paths through `Join-Path`, causing PowerShell to fail before writing files. Prevention: when updating multiple docs, build the path array as complete literal paths or call `Join-Path` once per path, then read back the touched lines before continuing.
- In Codex managed sandbox, `Build.bat` can fail before C++ with `UnrealBuildTool failed to check dependencies` when UBT tries to write `C:\UnrealEngine\Engine\Intermediate\Build`. Do not treat this as an AvidScript code failure; rerun the same command with explicit engine build permission.
- In Codex managed sandbox, `UnrealEditor-Cmd.exe` automation can fail during startup with `Unable to use cache graph 'Default' because it has no writable nodes available`. Prefer adding `-DDC-ForceMemoryCache` to automation commands before escalating or changing project settings.
- In Codex Windows sandbox, if `apply_patch` fails with `windows sandbox failed: helper_unknown_error`, first retry with workspace-relative paths. If the helper still fails, use a controlled PowerShell write only for the intended workspace files, then immediately inspect `git diff` and run the relevant build/automation verification. Do not skip diff review or tests after a fallback write.
- When editing Markdown through PowerShell, use single-quoted here-strings or explicit line arrays for text containing Markdown backticks. Do not put Markdown backticks inside double-quoted PowerShell strings, because they are escape characters and can corrupt commit hashes or inline code. After writing docs, verify the rendered source with `Get-Content` or `Select-String`.
- 2026-07-03 P4.2 mistake record: a PowerShell double-quoted Markdown replacement interpreted backticks as escape characters and briefly wrote corrupted inline code/control characters into root docs. Prevention: use literal single-quoted here-strings or line arrays for Markdown edits, then run a control-character scan (`[\x00-\x08\x0B\x0C\x0E-\x1F]`) over touched Markdown before considering docs done.
- 2026-07-05 P8.0 mistake record: a PowerShell single-quoted replacement used the literal text `` `r`n`` while trying to insert a Markdown newline, briefly placing that marker in the phase tracker. Prevention: when replacing Markdown with line breaks, use a single-quoted here-string that contains real newlines, line arrays joined with `[System.Environment]::NewLine`, or string concatenation with `[System.Environment]::NewLine`; after writing docs, scan for literal `` `r`n`` / `` `n`` markers in addition to control characters.
- 2026-07-05 P10.4 mistake record: root docs were updated with double-quoted PowerShell strings containing Markdown backticks around commit hashes. PowerShell treated backtick-zero as a NUL control character and treated backtick-dollar as an escaped variable, briefly writing `$Commit` and a control character into docs. Prevention: for commit-hash or Markdown table row updates, never put Markdown backticks in double-quoted PowerShell strings; build backticks with `[char]96` or use single-quoted templates with placeholders, then scan touched docs for `$Commit`, control characters, literal newline markers, and lost inline-code backticks before moving on.
- 2026-07-05 P11.1 mistake record: `BuildAvidScriptActor.ps1` depended on `Get-FileHash`, but PowerShell launched from `UnrealEditor-Cmd.exe` did not expose that cmdlet, causing the wrapper to abort before writing the frontend report. Prevention: avoid nonessential PowerShell cmdlet dependencies in UE-launched build wrappers; prefer .NET APIs or explicit tool paths, and include child process exit/stdout/stderr in Editor failure summaries.
- 2026-07-05 P11.1 mistake record: while backfilling a commit hash into the root implementation plan, a single-quoted PowerShell replacement inserted the literal text `\n` instead of a real newline. Prevention: never use `\n` as a Markdown line break in PowerShell replacements; use `[System.Environment]::NewLine`, a real here-string newline, or a line array join, then scan touched docs for literal backslash-n before moving on.
- 2026-07-05 P11.2 mistake record: `UE::ToolMenus::FToolMenuTestInstanceScoped` is declared in UE ToolMenus headers but is not linkable/exported for this plugin automation module, causing an unresolved external during `AvidScriptEditor` link. Prevention: do not depend on UE internal test helpers from plugin tests unless export/linkage has been proven; prefer public ToolMenus APIs plus unique test menu names for automation.
- 2026-07-04 P5.1 mistake record: a Chinese plugin Markdown file was first written with ASCII encoding, replacing non-ASCII text with `?`. Prevention: when a Markdown/doc file contains Chinese or other non-ASCII text, write it as UTF-8 without BOM and inspect the rendered source with `Get-Content` before committing; a control-character scan alone is not sufficient.
- 2026-07-05 P5.2c mistake record: `FPlatformMisc::GetSHA256Signature` asserted with `No SHA256 Platform implementation` during Editor-Cmd automation. Prevention: for runtime manifest hash validation, use OpenSSL SHA256 or another already-proven implementation in UE automation before relying on platform SHA helpers.
- 2026-07-05 P5.2c mistake record: `FPaths::ProjectSavedDir()` can be relative, for example `../../../../Users/...`, when running under `UnrealEditor-Cmd.exe`. Prevention: normalize test fixture directories to full paths before writing manifests, and keep real manifest wasm paths project-relative such as `Saved/...` so build script output and runtime loader behavior stay aligned.
- After completing a coherent C++ or Build.cs development batch, run the affected UE5.8 modules. Do not rebuild the Editor target after every individual file edit:

```powershell
& "C:\UnrealEngine\Engine\Build\BatchFiles\Build.bat" AvidTPSTemplateEditor Win64 Development "-Project=<ProjectRoot>\AvidTPSTemplate.uproject" -WaitMutex -NoHotReloadFromIDE
```

- 2026-07-21 Codex transport disconnect 记录：用 `tty=true` 直接流式运行 UE/UBT，并同时启用 `-stdout -FullStdOutLogOutput`，一次命令会向会话传输数万行启动日志和终端控制码，触发 `stream disconnected before completion` / `error decoding response body`。Prevention：Codex 内运行 UE/UBT 时禁止把完整 stdout 持续回传；使用非 TTY 子进程，把 stdout/stderr 重定向到 `C:\tmp` 日志并等待退出，只读取退出码、`Result`、测试发现/完成数、失败行、`Queue Empty` 和 `RequestExitWithStatus` 摘要。含空格的 `-Project` 参数不得交给 `Start-Process -ArgumentList` 重新拼接，首个探针因此把路径截断在 `<UserProfile>\Documents\Unreal`；固定使用 PowerShell call operator 保留参数边界，例如 `& $Executable @Arguments *> $Log`，再读取 `$LASTEXITCODE`。`-abslog` 仍保留作为自动化审计日志。这是执行通道稳定性规则，不得通过减少测试覆盖规避。

- Run runtime automation after runtime behavior changes:

```powershell
& "C:\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<ProjectRoot>\AvidTPSTemplate.uproject" -Unattended -NullRHI -NoSplash -NoSound -NoP4 -NoLiveCoding -stdout -FullStdOutLogOutput -FORCELOGFLUSH -CrashForUAT "-ExecCmds=Automation RunTests AvidScript.Runtime" "-TestExit=Automation Test Queue Empty" "-abslog=C:\tmp\AvidScript_Automation.log"
```

- For Windows packaged Development smoke on this UE5.8 source build, skip ZenStore during cook to avoid local Zen oplog staging instability:

```powershell
& "C:\UnrealEngine\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun "-project=<ProjectRoot>\AvidTPSTemplate.uproject" -noP4 -platform=Win64 -clientconfig=Development -skipbuild -cook -clean -stage -pak -archive "-archivedirectory=C:\tmp\AvidScript_Package" "-AdditionalCookerOptions=-SkipZenStore" "-ubtargs=-MaxParallelActions=4 -NoUBA" -unattended -utf8output
```

- Validate the packaged runtime smoke log with:

```powershell
& "C:\tmp\AvidScript_Package\Windows\AvidTPSTemplate.exe" -NullRHI -NoSplash -NoSound -stdout -FullStdOutLogOutput -FORCELOGFLUSH "-ExecCmds=quit" "-abslog=C:\tmp\AvidScript_Packaged_Run.log"
```

- Record successful builds in the active phase log.
- If a build fails before reaching AvidScript files, document it as an environment or project-level blocker.
- If a build fails inside AvidScript files, fix the plugin code first and rerun the same command.
- Generated build outputs must remain ignored by Git.

## Batched Verification Workflow

- `Docs/Workflow/Phase_Development_Workflow_Design.md` 是阶段执行的规范合同。每次开始新 Phase、恢复中断任务或发生上下文压缩后，必须先读取本节、当前 Phase 计划、债务清单和最近一次 Gate 证据，再确定唯一下一步；不得仅依赖聊天记录或记忆判断阶段状态。
- 默认以一个完整功能组或 60–120 分钟工作量作为开发批次。批次内连续完成相关 production code、测试代码和文档，不在每个小步骤后重复启动 UE、运行完整模块构建或完整 automation。
- 批次内只使用低成本反馈：源码检索、结构化产物检查、编译器静态诊断、`git diff --check`，以及确实会阻塞后续实现的最小 .NET 测试。不得为了逐个 checklist 制造碎片化 RED/GREEN 循环。
- 实现批次中默认不运行 UE/UBT 门禁。只有 Blocker、Critical 或经设计文档风险升级规则判定需要中点集成时，才执行一次受影响模块的 no-clean 增量构建或 broader focused filter；失败后只重跑失败组及其直接依赖。
- 大 Phase 收尾门禁集中执行：所有相关 .NET 测试宿主与 format、一次四模块 no-clean scoped build、architecture/parser gate、一次完整 `Automation RunTests AvidScript`。完整 automation 默认不在同一大 Phase 中重复运行。
- 独立代码审查安排在最终全量 automation 之前。审查修复后先运行受影响 focused tests，随后只运行一次最终全量 automation，避免“全量测试 -> 审查修改 -> 再全量测试”的重复成本。
- 只有 ABI、guest memory、状态迁移、生命周期事务、崩溃或数据损坏风险，以及需要先确认根因的真实回归，才允许在批次中途立即编译或运行聚焦测试。即使属于例外，也应合并同一风险面的修改后再执行。
- 批量验证不等于把反馈无限推迟到数小时任务末尾。单个批次不得跨越多个相互独立的架构层，也不得以减少测试时间为理由跳过最终门禁、清理 Editor Target 或降低成功判定标准。
- 每次开始新任务、网络重连或上下文压缩恢复后，第一条仓库命令必须是 `Build/InvokePhaseWorkflow.ps1 status -Phase <当前Phase>`；当前 Phase 由最高编号且未关闭的 tracked state 决定，不得把旧 Phase 编号硬编码在规范中。只有目标 state 尚不存在时才允许读取上一 Phase closeout 与新 Phase 架构/计划，并在文档提交后立即通过 `start` 自举状态。
- 状态创建后只通过 `Build/InvokePhaseWorkflow.ps1` 执行 batch、debt、architecture revision、freeze、attest、close 和 reopen。完整 Gate 验证 `gate_ready` 候选提交；Gate 后只允许一个受限 attestation commit，没有匹配 commit/tree/state hash 的不可变 Gate 报告时不得宣布 Phase 完成。
- 2026-07-24 P50 工作流探测错误记录：`Build/InvokePhaseWorkflow.ps1` 没有 `help` 子命令，直接执行 `help` 会先因缺少有效 `-Phase` 触发 `ASPW1101`，不提供使用说明。Prevention：恢复状态使用 `status -Phase <N>`；需要确认命令参数时读取脚本 `param`/`switch` 块或既有流程文档，不再用不存在的 `help` 探测。

## C# Guest Toolchain Workflow

- 2026-07-25 P51.3 编译错误记录：新增 `TryGetKindFromTypeId(..., out kind)` 时直接用短路 `&&` 转发另一个 `TryGet`，当前缀不匹配时 `out kind` 未赋值，触发 CS0177。Prevention：所有带 `out` 参数的布尔短路 helper 在表达式前先显式初始化输出，或改写为完整分支；新增 Tools helper 后先执行受影响 .NET 测试宿主的集中编译再进入 UE Gate。

- P13.1 verified the user-local .NET 8 SDK at `<UserProfile>\.dotnet\dotnet.exe` can install and list `wasi-experimental`. Prefer this SDK for C# WASI probes over `C:\Program Files\dotnet\dotnet.exe` on this machine.
- The Program Files .NET 9.0.306 CLI currently fails workload commands in `Microsoft.DotNet.Installer.Windows.InstallerBase`, and .NET 9 rejects `wasi-wasm` with `The 'wasi-experimental' workload is not supported in .NET 9.` Do not spend Phase time trying to force that path unless the toolchain has been repaired or upgraded.
- Current .NET 8 `wasi-experimental` output is a Mono/WASI runtime app: generated `dotnet.wasm` exports only `memory` and `_start`. It is not an AvidScript direct ABI module until it exports `avid_on_begin_play`, `avid_on_tick`, and `avid_on_end_play`.
- `PublishAot=true` with .NET 8 `wasi-wasm` currently fails with `NETSDK1203`; record this as toolchain unsupported, not as an AvidScript runtime failure.
- `BuildCSharpActorLifecycle.ps1` must isolate `DOTNET_CLI_HOME`, `APPDATA`, `LOCALAPPDATA`, and local NuGet config into `Saved/AvidScriptCSharpGuest/ActorLifecycle` so Codex sandbox runs do not try to read the user's blocked `%APPDATA%\NuGet\NuGet.Config`.
- If the user NuGet package cache is readable, it is acceptable for the C# diagnostic script to use `<UserProfile>\.nuget\packages` as a package cache while keeping config and generated outputs in `Saved/`.
- 2026-07-06 P13.1 mistake record: passing `BaseOutputPath` or `BaseIntermediateOutputPath` to MSBuild with a trailing Windows backslash inside a quoted argument can break paths with spaces and produce `MSB1008: Only one project can be specified`. Prevention: pass these MSBuild property paths with forward slashes and a trailing `/`.
- 2026-07-06 P13.1 mistake record: `--configfile` alone did not stop NuGet targets from reading `%APPDATA%\NuGet\NuGet.Config`. Prevention: redirect `APPDATA` and `LOCALAPPDATA` for the script process before invoking `dotnet publish`.

- P14.1 changed the normal C# sample build route: `BuildCSharpActorLifecycle.ps1` now tries `avidscript-csharp-source-adapter` first and exits with `direct_abi_built` when the source is inside the supported subset. The .NET/WASI publish path remains a fallback diagnostic route, not the default success path.
- Current C# source adapter subset `actor_lifecycle_v11`: `BeginPlay()`, `Tick(float deltaSeconds)`, optional `EndPlay()` / `OnTimer(int callbackId, int timerHandle)` / `OnEvent(int eventId, float value)`, `UE.SetTimer`, `UE.CancelTimer`, `private static float` state, handle-backed `UE.Self`/`AActor`/`USceneComponent`, typed Actor location/rotation/scale and RootComponent world-location reads/writes, shared `FVector`/`FRotator` three-component locals and addition, read-only `FTransform` snapshot projection, legacy `Actor.*` facade, numeric literals, lifecycle parameters, field references, multiplication, and addition.
- 2026-07-11 P25 review mistake record: the first EndPlay slice covered explicit component/session unload but did not model successful reload replacement, cached failure idempotency, component-observed versus guest-called statistics, or an explicitly empty `EndPlay(){}` body. Prevention: every lifecycle callback addition must test initial load, successful replacement, rejected replacement, explicit unload/destruction, success/no-export/trap results, and adapter empty-body syntax before merge. Runtime transitions must validate the candidate before ending the old guest, then end/unload the old guest before beginning the candidate.
- P26 typed self binding uses `FAvidScriptWasmHostContext.OwnerHandle` plus `owner_get_slot` / `owner_get_generation`. New typed APIs must never hardcode slot `1` / generation `1`; preserve the non-slot-1 end-to-end test when extending UObject bindings.
- P27 typed read uses `env.actor_get_location(slot, generation, out_ptr)` with guest linear-memory scratch storage. Future struct-return bindings must validate guest memory ranges, keep raw host pointers out of the ABI, and preserve non-slot-1 read/modify/write coverage.
- P28 rotation binding extends the same rule to `FRotator` in Pitch/Yaw/Roll order through `actor_get_rotation` / `actor_set_rotation`. `FVector` and `FRotator` adapter locals share one three-component codegen path; preserve type checks when adding new value structs.
- 2026-07-11 P28 workflow mistake record: a text insertion anchored on `int32_t AvidScriptOwnerGetSlot(...)` matched the forward declaration instead of the later function definition, placing rotation wrappers before required helper definitions and causing C3861 errors. Prevention: before region insertion, count matching anchors and require exactly one; when declarations and definitions share a name, anchor on the full definition including the opening brace or on a unique neighboring implementation block, then inspect the resulting line range before building.
- P29 scale binding extends typed Actor Transform coverage with `actor_get_scale` / `actor_set_scale`; `FTransform` is currently a read-only C# snapshot projection, not an adapter lifecycle local or atomic setter.
- P30 SceneComponent object graph uses `actor_get_root_component` to return an 8-byte `{slot, generation}` guest value, then `scene_component_get_world_location` / `scene_component_set_world_location` for typed component access. Never expose or reconstruct `UObject*` values in guest code; returned UObject-derived values must stay generation-checked handles.
- 2026-07-12 P30 test mistake record: the first invalid guest output pointer used `65532`, assuming a declared one-page WASM memory ended at 65536. WAMR instance heap allocation made that address valid. Prevention: never infer the runtime app-address boundary from the module's declared minimum pages; use `wasm_runtime_validate_app_addr` in production and a clearly unreachable positive address such as `INT32_MAX - 3` in negative tests, then verify the structured import failure.
- 2026-07-12 P30 editing mistake record: concatenating a PowerShell anchor directly with a here-string did not insert the expected leading newline and temporarily joined two C++ assertions on one line. Prevention: add an explicit newline between concatenated fragments, inspect the exact edited line range immediately, and run `git diff --check` before compiling.
- 2026-07-12 P30 documentation mistake record: a double-quoted PowerShell replacement interpreted Markdown backticks as escape prefixes, inserting a control character and expanding the intended literal `` `n ``. Prevention: use single-quoted here-strings for Markdown containing backticks, then scan touched text for control characters before continuing.
- P31 reflection schema reads `UClass` / `UFunction` / `FProperty` only in the Editor generation and validation path. Do not add dynamic reflection lookup to BeginPlay or Tick; generated/allowlisted static imports remain the runtime ABI.
- P31 manifest validation must reject duplicate `(module, name)` declarations, non-object `required_imports` entries, missing fields, and imports outside the reflected allowlist before emitting a usable contract.
- 2026-07-12 P31 test mistake record: a PowerShell/JavaScript command string consumed C++ JSON escape characters and produced an invalid embedded JSON fixture. Prevention: use C++ raw string literals for embedded JSON in tests, or verify the exact written line before compiling when text passes through multiple string parsers.
- 2026-07-12 P31 test mistake record: `DuplicateSpecs.Add(DuplicateSpecs[0])` triggered UE `TArray` alias protection when Add reallocated the same container. Prevention: copy an element to a local value before adding it back to the same `TArray`.
- 2026-07-12 P31 Git workflow mistake record: PowerShell continued to `git commit` after `git diff --cached --check` reported trailing blank lines. Prevention: after every Git quality gate, explicitly inspect `$LASTEXITCODE` and throw before commit when it is non-zero; amend only the immediately created agent commit if cleanup is required.
- P32 Timer state belongs to one `FAvidScriptWasmRuntimeInstance`. Rejected Reload must preserve the old runtime and its pending Timer state; successful Reload must unload old Timers before the candidate BeginPlay creates fresh state.
- P32 Timer Tick semantics are snapshot based: collect due handles before guest Tick, execute guest Tick, then invoke due callbacks in handle order. A zero-delay Timer created during Tick or callback must wait until the next frame.
- 2026-07-12 P32 test mistake record: `TNumericLimits<float>::QuietNaN()` was assumed to exist, but UE's numeric limits API did not provide that member. Prevention: use `std::numeric_limits<float>::quiet_NaN()` and include `<limits>` when a portable NaN fixture is needed.
- 2026-07-12 P32 editing mistake record: JavaScript string escaping consumed PowerShell/C++ regex `\s`, and a dynamic PowerShell array insertion briefly collapsed multiple imports onto one line. Prevention: use `String.raw` for regex-bearing tool commands, prefer structured line arrays for multi-line insertion, and read back the exact parser/import block before running the build.
- 2026-07-12 P32 test mistake record: repeated `UWorld::Tick` calls inside one automation frame did not repeatedly tick the ActorComponent fixture even though the WorldSubsystem advanced. Prevention: use the first real World Tick to verify routing, then call `TickComponent` directly for deterministic repeated component-frame tests.
- 2026-07-12 P32 compatibility mistake record: the first v10 adapter required every script to define `OnTimer`, breaking older C# profiles. Making it optional then reused a parameterless-only optional-method regex, silently generating a no-op callback for `OnTimer(int, int)`. Prevention: lifecycle additions must test scripts both with and without the optional method, and optional method detection must accept the declared parameter list before extracting the body.
- 2026-07-12 P32 automation mistake record: `TestNotNull` recorded a failed component binding but the test immediately dereferenced the null result, turning a source-adapter compatibility failure into an Editor access violation. Prevention: automation assertions do not short-circuit; guard pointers before dereference so the original failure remains visible.
- P33 gameplay event ABI is `avid_on_event(i32 eventId, f32 value)`. Host argument validation failures reject only the current dispatch and must not unload a healthy guest; missing exports and guest traps are guest failures and do disable the component runtime.
- P33 event ingress stays generic at the Runtime boundary. Enhanced Input, Blueprint, overlap, hit, and gameplay systems should adapt into component events or later typed callbacks instead of making WAMR depend on a specific UE input plugin.
- 2026-07-12 P33 compatibility rule: every newly added optional lifecycle callback must have a no-op generated export when absent, plus tests for both old custom profiles and the new method shape.
- 2026-07-12 P33 parser rule: lifecycle parameter identifiers are context-sensitive. `value` is legal only in `OnEvent`; reject it elsewhere during source adaptation so invalid C# cannot silently compile to a wrong WASM local.
- 2026-07-11 P29 mistake record: the first scale missing-context implementation initialized the out value to identity scale `(1,1,1)`, but host ABI failure paths require deterministic zero initialization. Prevention: do not use semantic identity defaults for failed ABI out parameters; initialize failed struct reads to zero and add missing/invalid/stale tests for every getter.
- 2026-07-11 P29 documentation mistake record: a temporary PowerShell replacement helper named `R` collided with the built-in `r` alias for `Invoke-History`, so only later direct replacements were written. Prevention: use descriptive helper names such as `Replace-Required`, never one-letter PowerShell function names, and read back the complete touched document before continuing.
- 2026-07-11 P26 workflow mistake record: building `-Module=AvidScriptEditor` compiled the changed Runtime unity object and Editor DLL but did not relink `UnrealEditor-AvidScriptRuntime.dll`; the next automation run loaded the previous Runtime test binary and reported an impossible source-shape failure. Prevention: after changing Runtime production or test sources, always finish with an explicit `-Module=AvidScriptRuntime` build and confirm a Runtime DLL link action before running automation, even if an Editor consumer build compiled Runtime objects.
- 2026-07-06 P14.1 mistake record: returning an empty .NET `List[byte]` from a PowerShell function without a unary comma enumerates the empty collection and assigns `$null`, causing later parameter binding errors such as `Cannot bind argument to parameter 'Bytes' because it is null`. Prevention: return collection objects with `return ,$List` / `return ,$Body`, and use `return ,([byte[]]...)` for byte arrays that must stay intact.
- P15.2 组件级 C# manifest 路径已经接入 `UAvidScriptComponent`: manifest 路径为空时继续使用 embedded smoke module, 路径非空时通过 manifest loader 加载 WASM, 并在 BeginPlay 前注入组件 owner registry 与 `AllowWrites` actor 写策略。
- 2026-07-06 P15.2 mistake record: UE5.8 的 `FFilePath` 声明在 `UObject/SoftObjectPath.h`, 不是 `Misc/FilePath.h`; 错误 include 会导致 `fatal error C1083`. Prevention: 新增 UE struct include 前先在 `C:\UnrealEngine\Engine\Source` 搜索声明位置或参考同引擎版本的工作示例。
- P16.2 Editor 侧 C# report/manifest 组件绑定已经接入 `FAvidScriptEditorComponentBindingService`: 该服务可读取 C# build report 的 `artifacts.manifest_file`, 绑定到显式 Actor 或当前选中 Actor, 并在缺少组件时创建 `UAvidScriptComponent`。
- P17.2 Editor 菜单入口已经接入 C# ActorLifecycle 绑定: `Tools > AvidScript > Bind C# ActorLifecycle Script` 会读取 `Saved/AvidScriptCSharpGuest/ActorLifecycle/actor_lifecycle.csharp.report.json`, 并调用组件绑定服务绑定到当前选中 Actor。
- P18.2 Editor 菜单入口已经接入 C# ActorLifecycle 构建并绑定: `Tools > AvidScript > Build And Bind C# ActorLifecycle Script` 会调用 `BuildCSharpActorLifecycle.ps1`, 验证 report 存在, 再复用组件绑定服务绑定到当前选中 Actor。
- P19 profile 化 C# 构建已经接入: `BuildCSharpActorLifecycle.ps1` 与 `FAvidScriptEditorCSharpBuildService::BuildProfile(...)` 接收 `SourcePath`, `ProjectPath`, `ModuleId`, `ArtifactStem`, `OutputRoot`, `ReportPath`, `ManifestPath`; 默认 ActorLifecycle 参数保持兼容。
- P19 自定义 profile 自动化使用 `csharp_custom_mover` / `custom_mover` 验证 report、manifest、WASM artifact 命名与组件绑定。后续 UI/profile 持久化应复用 `BuildProfile(...)`, 不要再硬编码 ActorLifecycle 文件名。
- 2026-07-06 P19 workflow note: 新增 `AvidScriptEditor` 测试 `.cpp` 时仍可能遇到 UBT cached source list; 如果模块构建意外 up to date, 先触发 source-list invalidation 并保持 `-Module=AvidScriptEditor` 范围, 不要清 Editor target。
- P20 C# profile JSON 服务已经接入: `FAvidScriptEditorCSharpProfileService::LoadProfile(...)` 可读取 schema_version 1 / language csharp profile, 并映射到 `FAvidScriptEditorCSharpBuildConfig`。
- P20 默认 profile 路径为 `Saved/AvidScriptCSharpProfiles/default.csharp-profile.json`; Editor 菜单入口 `Build And Bind C# Profile Script` 当前固定读取该路径, 后续 UI/profile 列表应复用这个默认约定。
- P20 profile 入口成功路径为: profile JSON -> `BuildProfile(...)` -> C# report -> `FAvidScriptEditorComponentBindingService::ApplyCSharpReportToSelectedActor(...)` -> `UAvidScriptComponent` manifest path。
- P21 默认 C# profile 模板已经接入: `FAvidScriptEditorCSharpProfileService::WriteDefaultProfileTemplate(...)` 会生成 `Saved/AvidScriptCSharpProfiles/default.csharp-profile.json`, 默认 source 指向 `Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs`, module/artifact 为 `csharp_profile_actor_lifecycle` / `profile_actor_lifecycle`, output root 为 `Saved/AvidScriptCSharpGuest/Profiles/profile_actor_lifecycle`。
- P21 Editor 入口已经接入: `Tools > AvidScript > Create Default C# Profile` 默认只确保 profile 存在, 不覆盖用户已编辑 JSON; 之后继续使用 `Build And Bind C# Profile Script` 走 profile 构建和绑定。
- 2026-07-06 P21 mistake record: PowerShell 行插入曾把制表符写成字面量 `` `t ``。Prevention: 对包含 PowerShell escape 字符的插入, 使用单引号字符串、显式 `[char]9` 或行数组, 写后用读回检查确认不存在字面量反引号标记。
- 2026-07-06 P21 mistake record: 重写 `AvidScriptEditorModule.h` 时曾遗漏既有 `CoreMinimal.h`、`Logging/LogMacros.h`、`DECLARE_LOG_CATEGORY_EXTERN` 和 `MakeCommandConfigForSource(...)` overload, 导致后续构建失败。Prevention: 编辑公共头文件时优先做局部 patch, 如需重写, 先读取完整原文件并列出必须保留的 include、宏、既有 public API, 写后立即 diff 检查被删除声明。
- 2026-07-06 P21 workflow note: C++ 文件行尾可能是 LF, 用 `[Environment]::NewLine` 做多行字符串替换会错过匹配; 常量插入脚本还可能误把第一次使用当作声明位置。Prevention: 对 C++ 插入优先按 `ReadAllLines`/行号/邻近锚点处理, 并用 `Select-String` 验证声明位置。
- 2026-07-06 P21 mistake record: 更新中文 tracker 时对 `C:\tmp` 路径做了多轮临时替换, 一度把路径写成包含制表符的 `C:` + tab + `mp`。Prevention: Markdown 中的 Windows 路径不要做反斜杠转义占位替换; 用单引号 here-string 直接写字面量, 写后扫描 `[char]9`、控制字符和 `C:` + tab 片段。
- 2026-07-06 P21 mistake record: commit hash 回填时再次把 Markdown 反引号放进 PowerShell 双引号字符串, 导致 inline code 反引号丢失并写入字面量 `` `r ``。Prevention: 所有包含 Markdown 反引号的文档回填都用单引号模板、行数组直接赋值或 `[char]96`, 写后扫描字面量 `` `r `` / `` `n ``。
- 2026-07-06 P20 mistake record: 用 PowerShell 双引号直接拼复杂 C++ 替换片段时容易被引号、反斜杠或 UE 宏文本打断解析; 本次未写坏文件但浪费了验证时间。Prevention: 复杂 C++/Markdown 替换优先用单引号 here-string、逐行数组或小范围读写函数, 写后立即 `Select-String`/`git diff --check` 核对。

- 2026-07-12 P37 测试编辑错误记录：受控 PowerShell fallback 用 LF here-string 匹配 CRLF 测试文件，因换行不一致触发 missing anchor；脚本在写盘前停止，文件未损坏。Prevention：fallback 多行替换必须先在内存中把源文本和锚点统一为 LF，完成全部必需锚点检查后才允许一次性写盘。

- 2026-07-12 P37 文档命令错误记录：把含 Markdown 反引号的 PowerShell here-string 直接嵌入 functions.exec JavaScript template literal，导致外层 JavaScript 在命令执行前 SyntaxError；文件未改动。Prevention：通过 functions.exec 发送含反引号的文档时，使用不含 template literal 的安全字符串构造，或先移除/显式转义外层反引号。
- 2026-07-12 P37 文档路径错误记录：把项目根 Docs 下的 Phase 37 实施计划误按插件相对路径读取，脚本在实现记录写盘后因 FileNotFound 停止。Prevention：项目决策/plan/tracker 使用项目根绝对路径，插件实现记录才从插件仓库使用 Docs 相对路径；多目录文档更新前先逐项 Test-Path。

- 2026-07-13 P37.3 计划自检错误记录：placeholder scan 匹配了自检说明中的占位符单词本身并误报。Prevention：自检说明使用“没有占位项”等自然语言，不在被扫描文档里复述扫描 token。
- 2026-07-13 P37.3 编辑事务错误记录：多文件 PowerShell fallback 先写 Runtime header，随后 source 锚点失败，留下短暂不可编译中间态。Prevention：多文件编辑必须先在内存完成所有锚点验证，再统一写盘；不能边验证边写。
- 2026-07-13 P37.3 regex 错误记录：PowerShell replacement 中的捕获组写法被错误生成成字面 1CopyObservableStateToResult；架构脚本通过但源码不可编译。Prevention：复杂缩进迁移优先 ReadAllLines；使用 regex capture 时先在临时字符串断言输出不存在异常 token，并始终以真实编译为最终 gate。
- 2026-07-13 P37.3 IWYU 重复错误记录：新建 AvidScriptActorTransformBatch.cpp 时首 include 不是同名 header，UBT 给出非致命诊断后仍链接。Prevention：创建每个 UE cpp 时第一行立即写同名 header，并把非致命 UBT diagnostics 视为失败处理。
- 2026-07-13 P37.3 benchmark 编辑错误记录：对重复字段片段做全局 Replace，把 TransformBatchSize 错加到 Timer result；读回时在构建前修复。Prevention：包含重复结构的 public header 只能用带 struct 名的唯一上下文锚点或逐行范围编辑，禁止无上下文全局 Replace。

- 2026-07-13 P37.3 文档路径错误复发：functions.exec JavaScript template literal 再次把 PowerShell here-string 中的 Windows C colon backslash tmp 路径解释成制表符并移除斜杠。Prevention：经 functions.exec 生成的 Markdown 路径统一写成 C:/tmp/... 正斜杠形式；写后扫描 tab 控制字符。

## Product Maturity Direction

- The project goal is a production-grade modern UE scripting platform comparable in practical scope to Puerts, UnLua, and Unreal AngelScript, not a collection of isolated demo bindings.
- From Phase 39 onward, the primary roadmap is a real language frontend, generated reflection bindings, broad typed UE interoperability, debugging, packaging, and cross-platform hardening.
- Do not expand UE coverage by hand-writing one host import per gameplay method as the default strategy. A manual binding is acceptable only when it establishes or validates a reusable ABI, generator rule, ownership policy, or performance primitive that subsequent APIs can share.
- Every phase must state how it improves generated coverage, language expressiveness, tooling, runtime guarantees, or production readiness. Reject work that only increases a sample version number without advancing one of those dimensions.
- Tests must detect a plausible regression at an ownership, ABI, memory-safety, lifecycle, code-generation, or user-workflow boundary. Do not add tests that merely assign fields and immediately read the same fields back, assert implementation text without a behavioral contract, or duplicate coverage without a distinct failure mode.
- Prefer end-to-end generated artifacts and real WAMR guest-memory/event paths for public behavior. Keep narrow unit tests for algorithms and state machines where isolation provides a meaningful failure signal.
- Feature parity and production maturity are separate gates: implementation is not mature until it has packaged-build, stress, compatibility, and real-project evidence.
- 2026-07-13 P38 test edit mistake record: an in-memory insertion required the test file to end with a newline after the final preprocessor directive, so the exact end anchor failed before write. Prevention: end-of-file edits must use LastIndexOf on the directive or structured lines and must not assume a trailing newline; keep all validation before WriteAllText.
- 2026-07-13 P38 edit validation mistake record: Select-String was used as if pipeline output preserved direct match counting, then the replacement was incorrectly expected to contain three lowercase import-name tokens when it correctly contained two. Both checks stopped before write. Prevention: deterministic validation uses String.Split or regex Matches with a count derived from the actual intended occurrences; compilation remains the final gate.
- 2026-07-13 P38 PowerShell syntax mistake record: C-style backslash quote escaping was used inside a PowerShell double-quoted edit string, causing ParserError before execution. Prevention: C++ source anchors containing quotes use single-quoted PowerShell here-strings; do not compress them into escaped double-quoted literals.
- 2026-07-13 P38 PowerShell syntax mistake recurrence: while adding typed event tests, a double-quoted include replacement again used C-style backslash escaping and failed at parse time before write. Prevention strengthened: any edit text containing C++ quotes must be declared as a single-quoted here-string, even for a two-line replacement; do not use double-quoted compression.
- 2026-07-13 P38 WAMR fixture mistake record: the first invalid-output case used address 65532 assuming a fixed one-page memory, but WAMR instance heap growth made that range valid and the dispatcher was called. Prevention: invalid guest-pointer fixtures use a deterministic near-MAX_int32 address or derive the actual memory bound; validation loops should retain distinct case names.
- 2026-07-13 P38 documentation formatting mistake record: an inserted here-string did not preserve the intended separator before Module Architecture Workflow, joining a note and heading on one line. Prevention: after inserting variable-length notes, assert heading lines start at line boundaries and read back the surrounding section before continuing.

## Module Architecture Workflow

- Runtime dependency direction is `AvidScriptCore <- AvidScriptVM/AvidScriptBindings <- AvidScriptRuntime <- AvidScriptEditor`.
- `AvidScriptCore` may depend only on UE `Core`; it must not include Engine, CoreUObject, WAMR, Binding, Runtime, or Editor APIs.
- `AvidScriptVM` owns backend-specific resources. WAMR headers and libraries must remain private to this module, and VM public contracts must not mention `UObject`, `AActor`, `FVector`, or other gameplay types.
- `AvidScriptBindings` owns the UObject handle registry and typed UE operations. It may depend on CoreUObject/Engine but must not include WAMR or Runtime.
- `AvidScriptRuntime` is the composition and session layer; gameplay integration must not be added directly to a VM backend.
- Run `Build/CheckAvidScriptArchitecture.ps1` after module, Build.cs, descriptor, or ownership changes.
- Adding a new plugin module and building its DLL does not necessarily update `Binaries/Win64/UnrealEditor.modules`. After adding the module to `AvidScript.uplugin`, run one normal no-clean incremental `AvidTPSTemplateEditor` target build to write target metadata. Verify the action list; Phase 34 required only `WriteMetadata AvidTPSTemplateEditor.target`. Never use this as a reason to clean the target.
- 2026-07-12 P34 editing mistake record: a PowerShell substring-based insertion briefly produced malformed `""AvidScriptBindings"` text in a Build.cs dependency list. Prevention: use `apply_patch`, structured line arrays, or exact whole-line replacement for Build.cs; immediately read back the dependency block before building.
- 2026-07-12 P34 test mistake record: the first Bindings fixture called `NewObject<UObject>()`, but `UObject` is abstract and UE automation raised an ensure. Prevention: UObject registry tests must instantiate a concrete test `UCLASS`, and pointer assertions must remain guarded because automation assertions do not short-circuit.
- 2026-07-12 P34 automation workflow note: `UnrealEditor-Cmd.exe` can exit with code 0 while an automation case reports `Result={Fail}`. Completion requires checking the log for each `Test Completed. Result={Success}`, absence of `Result={Fail}`, and the expected performed-test count; process exit code alone is insufficient.
- 2026-07-12 P35.1 workflow mistake record: a successful PowerShell architecture script inherited a stale non-zero `$LASTEXITCODE` from an earlier external process because its success path did not explicitly exit. Prevention: executable quality-gate scripts must `exit 0` on success and `exit 1` on failure; callers should not interpret a stale process code as the script result.
- 2026-07-12 P35 editing mistake recurrence: despite an existing rule, a helper was again named `R`, which PowerShell resolved to the `Invoke-History` alias. Prevention: one-letter helper names are prohibited in project edit scripts; use names such as `Replace-Required` and set `$ErrorActionPreference = 'Stop'` before any multi-file mutation.
- 2026-07-12 P35 recovery mistake record: PowerShell's case-insensitive read-only `$Host` variable collided with a local `$host`; the assignment failed non-terminatingly, but a later write still ran and replaced `AvidScriptWamrHostBindings.cpp` with one line. The pre-corruption staged blob was recovered with `git fsck --unreachable` and verified as 16 symbols before rebuilding. Prevention: multi-file edit scripts must stop on the first error, must not use automatic-variable names, and must verify changed file line counts before staging.
- 2026-07-12 P35 ABI mistake record: WAMR user data stored a multiply inherited concrete backend pointer and later cast the `void*` directly to its second base interface, skipping the required base offset and causing an invalid virtual call during the first C# host import. Prevention: store the already adjusted interface pointer (`static_cast<IAvidScriptWamrHostBridge*>(this)`) and preserve the imported-WASM dispatcher regression test.
- 2026-07-12 P35 workflow mistake record: a combined Git/build command ran Git from the project root instead of the plugin repository and stopped before compilation. Prevention: never combine plugin Git gates and project-level UE builds in one command; Git runs from `Plugins/AvidScript`, while Build.bat runs from the project root.
- Phase 35 moved all WAMR APIs, native symbols, guest memory access, and global lease ownership into `AvidScriptVM` Private. Runtime must remain free of `WAMR`, `wasm_export.h`, `wasm_runtime_*`, and `AVIDSCRIPT_WITH_WAMR`; enforce this with `Build/CheckAvidScriptArchitecture.ps1`.
- Phase 36 still needs to remove compatibility lifecycle booleans and make `FAvidScriptRuntimeSession` the unique owner. Do not add new gameplay callbacks directly to the Runtime façade during this migration.
- 2026-07-12 P36.2 编译错误记录：`AvidScriptComponent.cpp` 与 `AvidScriptWorldSubsystem.cpp` 在匿名命名空间中使用了相同的 `CopySessionLoadResult` helper 名；UE unity build 合并两个 `.cpp` 后触发 C2084 重定义。Prevention：Runtime 模块匿名命名空间 helper 使用带所属文件语义的唯一名称（如 `CopyWorldSessionLoadResult`），新增 helper 后必须经过实际 unity 模块编译，不能只依赖单文件阅读。
- 2026-07-12 P36.3 编辑错误记录：修改架构脚本时把包含 `$ComponentHeader` 的 PowerShell 源码锚点放进双引号字符串，变量被提前插值，短暂生成 `$ComponentHeader$ReloadTypesHeader` ParserError。Prevention：编辑 PowerShell 源码时，凡锚点或替换文本包含 `$`，必须使用单引号字面量或单引号 here-string；写后立即执行脚本语法/结果检查。
- 2026-07-12 P36.3 IWYU 记录：拆分 reload types 后把 `AvidScriptWasmReload.cpp` 的首 include 改为 types 头，UBT 报 `Expected AvidScriptWasmReload.h to be first header included`，但仍完成链接。Prevention：UE `.cpp` 保持同名 public/private header 为首 include；细分类型通过同名 header 的 umbrella 间接引入，并把非致命 UBT diagnostics 也视为必须修复。
- 2026-07-12 P36.3 生命周期设计错误记录：事务式 reload 初版在 candidate `BeginPlay` 成功后调用旧实例 `EndPlay`，旧脚本 cleanup 覆盖了 candidate 刚写入的 Actor 状态，完整回归 `SourceAdapterArtifactLifecycleSmoke` 失败。Prevention：UE `BeginPlay/EndPlay` 只对应真实 gameplay 生命周期；热重载成功时替换 owner 并直接卸载旧实例，不伪造 `EndPlay`。未来 cleanup/state migration 使用独立 reload callback/协议，并保留成功 reload 的 C# Timer/Actor 回归。
- 2026-07-12 P36.3 测试编辑错误记录：修改 reload 最终位置期望值时全局替换 `FVector(200...)`，同时误改了 fixture 的旧 EndPlay 写入值；读回检查在编译前发现并恢复。Prevention：重复测试常量不得用无上下文全局替换，必须以测试名/邻近语句组成唯一锚点，并读回 fixture 与 assertion 两处。

## D Guest Toolchain Workflow

- P5.1 proved official LDC 1.42.0 Windows x64 can compile the minimal D guest to freestanding wasm32 using LDC's internal LLD. Do not require a standalone `wasm-ld.exe` for this path.
- If only `ldc2.exe` is present, `BuildDGuestActorSetLocation.ps1` should continue and report `linker=ldc2-internal-lld`.
- The current verified portable toolchain location is outside the plugin repository:

```text
C:\tmp\AvidScriptToolchains\ldc2-1.42.0-windows-x64\ldc2-1.42.0-windows-x64\bin\ldc2.exe
```

- Do not commit downloaded LDC archives, extracted toolchains, or generated D/WASM artifacts under `Saved/`.
- P5.2 D reload artifacts under `Saved/AvidScriptDGuest/Reload/...` are generated test inputs, not source-controlled assets. Rebuild v1/v2 with `BuildDGuestActorSetLocation.ps1` before relying on `AvidScript.Reload.DGuestActorHostContextSmoke` as a true D reload smoke.
- LDC freestanding `extern(C)` undefined imports currently arrive as `env.<name>`. AvidScript's canonical Host ABI remains `avidscript.<name>`, but runtime must keep the `env` compatibility alias until a cleaner import-module mapping or artifact postprocess is implemented.
- Before removing the `env` alias, prove D artifacts can import `avidscript.actor_set_location` directly and rerun `AvidScript.Guest.D` plus the full `AvidScript` automation suite.

## ThirdParty Runtime Workflow

- Prefer a source-vendored WAMR layout for the first feasibility spike unless the user decides otherwise.
- Keep third-party runtime files under:

```text
Plugins/AvidScript/Source/ThirdParty
```

- Current WAMR snapshot: `WAMR-2.4.4`, commit `8c18e3f68b16c4bcaf05996b2636f6ed2b4cf629`.
- WAMR upstream source lives in `Source/ThirdParty/WAMR/upstream`.
- Win64 static library lives in `Source/ThirdParty/WAMR/lib/Win64/Release/libiwasm.lib`.
- Rebuild Win64 WAMR with:

```powershell
cmd /c Plugins\AvidScript\Build\BuildWAMRWin64.cmd
```

- Keep WAMR source/configuration tracked only when it is intentionally vendored.
- Keep WAMR build outputs ignored through `.gitignore`.
- Separate PC Editor support from future Android/iOS support in Build.cs and documentation.

## Safety And Architecture Rules

- Do not expose raw `UObject*` pointers to guest code.
- All guest object access must go through host-owned handles.
- Every host call must be designed to fail closed.
- Runtime failures should become deterministic diagnostics, not Editor or packaged-game crashes.
- Hot reload must use staging load, ABI validation, migration rules, and rollback.
- High-frequency gameplay APIs should prefer generated typed calls and batching over fine-grained dynamic reflection calls.
- 2026-07-13 P38.3 fallback 换行错误复发：受控 PowerShell 编辑再次用 LF here-string 直接匹配 CRLF C++ 文件，唯一锚点计数为 0；脚本在写盘前停止。Prevention：所有 fallback 锚点在计数和 Replace 前必须显式转换为目标文件的换行格式。
- 2026-07-13 P38.3 命令封装错误复发：functions.exec 的 JavaScript template literal 再次包含 PowerShell 反引号，导致外层 SyntaxError 且命令未执行。Prevention：通过 functions.exec 发送 PowerShell fallback 时禁止在命令中使用反引号；换行改用 [char]13/[char]10。
- 2026-07-13 P38.4 混合换行错误记录：碰撞实现 fallback 把同模块 Header 与 Source 都按 CRLF 转换，但 Source 实际为 LF，导致 source 锚点为 0；事务在写盘前停止。Prevention：多文件 fallback 必须逐文件探测换行，统一到 LF 匹配后再分别恢复，禁止按目录推断。
- 2026-07-13 P38.5 fallback helper 错误：C# RED 编辑脚本调用 Normalize-Lf 但漏定义该函数，PowerShell 在任何写盘前停止。Prevention：每个自包含 fallback 命令必须在顶部定义并立即使用所需 helper，不能假设前一条 exec 的函数仍存在。
- 2026-07-13 P38.5 命令封装错误再次复发：functions.exec 的 JavaScript template literal 中出现 PowerShell 反引号，外层解析在执行前失败，未写盘。Prevention：受控 fallback 命令禁止出现 PowerShell 反引号；包含美元符号的替换锚点一律使用单引号 here-string。
- 2026-07-13 P38.5 测试设计错误：Editor 兼容性测试用精确 JSON 文本匹配断言空数组，因 ConvertTo-Json 的缩进换行产生假失败；读回同时发现空 static_float_fields 被序列化为 [null]。Prevention：JSON 语义必须通过 FJsonObject/FJsonValue 结构化断言，生成器中的可空流水线在序列化前过滤 null，并覆盖空集合契约。
- 2026-07-13 P38.5 集合返回错误：Get-CSharpStaticFloatFields 使用 return ,$Fields，调用方再次数组化后形成嵌套集合；零字段被误计为一个 global，并产生 [null] 元数据。Prevention：供 @(... ) 调用的集合 helper 直接输出元素，不用一元逗号保护集合；至少覆盖零元素与正常非空元素的结构化契约。
- 2026-07-13 P38.5 文档命令封装错误复发：Markdown 反引号直接出现在 functions.exec 的 JavaScript template literal 中，外层解析在写盘前失败。Prevention：fallback 文档内容禁止包含字面反引号，统一使用占位符并在 PowerShell 内以 [char]96 还原；优先继续尝试 apply_patch。
- 2026-07-13 P38.6 生成器命令封装错误复发：为批量构造 PowerShell 源码锚点而在字符串中使用反引号转义美元符号，functions.exec 外层 JavaScript 在执行前失败。Prevention：源码中的变量调用点逐个使用单引号 here-string 替换，禁止用可插值字符串数组生成锚点。
- 2026-07-13 P38.7 验证汇总脚本错误：带内部 exit 的 foreach 语句被直接连接到 Format-Table 管道，PowerShell 报 empty pipe element，未执行日志检查。Prevention：验证脚本先把结果收集到 rows 数组，循环内完成断言，循环结束后再单独 Format-Table。

## Phase 38 Gameplay Contract

- New gameplay callbacks are Schema/descriptor entries routed through the single optional avid_on_gameplay_event export. Do not add one WASM export per UE callback.
- The fixed event envelope is type, primary id, secondary id, object slot/generation, and vector xyz. VM public contracts stay POD-only and UE-type-free.
- Component gameplay ingress must use the shared DispatchGameplayEvent policy so collision, input, invalid arguments, traps, metrics, and teardown remain consistent.
- AvidScriptRuntime must not depend on EnhancedInput. Project input systems call the typed DispatchScriptInput ingress.
- C# callback support is generated from descriptors. Phase 39-42 must replace the temporary source adapter with a real frontend and Reflection Binding Generator rather than expanding handwritten APIs.
- Phase 39 complete automation baseline is 138/138 on UE5.8 Win64 Editor. Recount Success/Fail/performed lines; do not trust process exit code alone.

## Phase 39 Language Frontend Rules

- The formal C# frontend uses the Roslyn assemblies shipped with the selected .NET 8 SDK. Keep it offline and package-free; do not add a NuGet dependency for compiler services already present in the SDK.
- PowerShell may locate tools, orchestrate processes, and merge reports. It must not gain new C# lexical, syntactic, expression, or statement parsing responsibilities.
- The versioned AvidScript frontend JSON is the boundary consumed by later semantic analysis and Guest IR. Do not serialize Roslyn implementation objects or numeric enum values as the public contract.
- Frontend spans use UTF-16 offsets and zero-based line/column coordinates. Convert to one-based coordinates only at the Editor presentation boundary.
- Syntax errors must still produce a deterministic diagnostic artifact and must gate WASM generation. Never fall back to regex parsing after the formal frontend reports an error.
- 2026-07-13 P39.1 workflow mistake record: the first redirected .NET test command assigned to PowerShell's read-only `$HOME` variable, so the environment variables were never updated and NuGet attempted to read the sandbox-blocked user config. Prevention: use a task-specific name such as `$ToolHome`, set `DOTNET_CLI_HOME`, `APPDATA`, `LOCALAPPDATA`, and `NUGET_PACKAGES` before invoking dotnet, and check assignment errors before interpreting restore output.
- 2026-07-13 P39.1 edit mistake record: a controlled PowerShell fallback normalized an LF-only C# source anchor to CRLF, so exact validation rejected the intended atomic-writer edit before writing. Prevention: inspect or normalize the target text and anchor to LF before exact matching; preserve the file's existing newline style when writing it back.
- P39.2 AST is an AvidScript-owned, versioned projection of Roslyn syntax. Map only syntax whose structure downstream phases can consume; preserve every other node as explicit `is_supported=false` with kind, text, and span instead of partially guessing it.
- Declaration, parameter, and AST node spans must be nested inside their owning node. Keep the real ActorLifecycle sample span-containment test when adding syntax mappings.
- 2026-07-13 P39.2 edit mistake record: a PowerShell single-quoted replacement represented newlines as literal backtick-n text, so exact validation rejected the `FrontendSyntax` edit before writing. Prevention: use a real here-string/newline or replace a unique single-line field anchor; never expect escape processing in single-quoted PowerShell strings.
- 2026-07-13 P39.2 compile mistake record: the first Roslyn mapper build omitted `using Microsoft.CodeAnalysis.CSharp`, so `SyntaxNode.Kind()` was unavailable. Prevention: every mapper calling CSharp extension methods must import the CSharp namespace; the .NET build remains the gate before AST behavior tests.
- 2026-07-13 P39.2 review edit record: an exact replacement assumed a property/event declaration anchor was unique when it intentionally appeared twice, then a trimmed here-string removed indentation from three inserted C# lines. Validation prevented the first write and readback caught the formatting issue. Prevention: declare expected occurrence counts for repeated syntax and never trim leading whitespace from code replacements.
- Every C# WASM build must run the formal frontend first. A syntax-error frontend artifact is a hard gate: write diagnostics, return failure, remove stale manifest/WASM artifacts, and never fall back to regex or .NET/WASI publish.
- The transitional AST adapter may extract source slices only from validated Roslyn spans. Script type, fields, and method bodies must come from the frontend model; PowerShell regex remains temporary only inside the statement emitter until Guest IR replaces it.
- 2026-07-13 P39.3 test mistake record: the first build-integration test derived PluginRoot from the test directory with one parent traversal instead of two, so it attempted to execute Tools/Build/BuildCSharpActorLifecycle.ps1. Prevention: derive and assert TestDir, ToolsRoot, and PluginRoot separately before launching integration scripts.
- 2026-07-13 P39.3 runtime mistake record: recursive frontend declaration traversal marked the declarations array mandatory without AllowEmptyCollection, so leaf nodes failed normal builds and triggered the diagnostic fallback. Prevention: recursive collection APIs must explicitly accept empty children, and the valid real-sample build is part of the permanent integration gate.
- 2026-07-13 P39.3 artifact mistake record: syntax gating initially left a stale same-stem adapter WASM from a previous run. Prevention: remove report, manifest, frontend, adapter WASM, and fallback WASM before the new build begins; failure tests must start from a directory containing stale artifacts.
- 2026-07-13 P39.3 verification mistake record: the first UE automation filter used `AvidScript.CSharp`, which matched 0 tests; the actual guest suite is `AvidScript.Guest.CSharp`. Prevention: confirm automation names with source registration or log `Found N automation tests`, and treat `0 tests performed` as failure even when the Editor process exits 0.
- Editor report readers must accept the structured C# `source` object and the legacy string `source` field. Preserve UTF-16 offsets and zero-based coordinates in the data model; convert coordinates only in presentation code.
- 2026-07-13 P39.4 edit mistake record: a multi-file fallback validated and wrote the header before discovering that the source anchor appeared twice. Prevention: validate every target path, occurrence count, newline style, and replacement result before writing any file; multi-file fallback edits must be transactional.
- 2026-07-13 P39.4 contract mistake record: the AST adapter manifest advanced to `actor_lifecycle_v13`, but Runtime/Editor tests and a report message still asserted v12. Prevention: when a generated artifact contract version changes, search production code, tests, reports, samples, and current docs for the old identifier before considering the migration complete; run both producer integration tests and UE consumers.
- 2026-07-13 P39.4 fallback newline mistake record: a PowerShell here-string appended after an existing line did not contribute a leading newline, joining two AGENTS bullets. Prevention: when appending a block, concatenate an explicit Environment.NewLine before the block and verify the surrounding lines after write.
- 2026-07-13 P39.4 verification environment mistake record: the final .NET test reused a C:\tmp CLI-home path whose existing ACL was not writable, so dotnet failed before loading the project. Prevention: create and probe a task-local ignored directory under the writable plugin Saved tree, then redirect DOTNET_CLI_HOME, APPDATA, LOCALAPPDATA, and NUGET_PACKAGES there before invoking dotnet.

## Phase 40 Semantic Analysis Rules

- Use Roslyn Compilation, IOperation, and ControlFlowGraph as the C# semantic truth. Do not hand-write a C# binder or reconstruct control flow from syntax text.
- Project Roslyn results into versioned AvidScript symbols, canonical types, typed operations, conversions, and CFG records. Public artifacts must not contain numeric Roslyn enums, object identities, SDK paths, or nondeterministic symbol keys.
- PowerShell remains orchestration only. It must not acquire type inference, overload resolution, conversion, or branch analysis responsibilities.
- Semantic analysis must verify the source hash recorded by the frontend artifact before consuming it. A mismatch is a hard failure, not a warning.
- Instance fields and methods may be represented in Phase 40, but guest memory offsets and object layout belong to Phase 41.
- UE facade/reference assemblies, C# proxies, and host stubs must be generated from reflection schema in Phase 42; do not expand the temporary handwritten sample facade.
- 2026-07-13 P40.0 fallback newline recurrence: the AGENTS append again relied on a here-string leading blank line and produced only one line break before a heading. Prevention: never infer block separation from here-string layout; construct the boundary with two explicit local newline sequences and inspect the preceding line plus heading.
- Roslyn diagnostic messages written to stable artifacts must use CultureInfo.InvariantCulture; otherwise identical compiler errors differ across developer locales.
- Semantic CLI exit codes are 0 for success, 1 for semantic failure with an artifact, and 2 for argument/IO/frontend-artifact failure. Output replacement must remain atomic.
- P40.2 semantic test baseline is 22/22 and Phase 39 frontend regression is 7/7. The real ActorLifecycle semantic artifact currently contains 15 types, 186 symbols, 38 executable method bodies, 656 typed operations, and 0 diagnostics.
- Operation projection must bind Roslyn-selected symbols directly. Stable method signatures retain ref/out/in parameter kinds; local IDs retain containing method and declaration position; explicit accessors and expression-bodied property getters are executable methods.
- Semantic schema v3 / semantic version 1.3 is the current typed-operation, CFG, instance-identity, and support-policy contract. Binary, unary, compound, and increment/decrement operations retain stable operator_kind plus checked/lifted/postfix flags.
- Stable member IDs normalize Roslyn constructed symbols to OriginalDefinition, include generic method arity, and include indexer parameter signatures. Expression-bodied property and indexer getters are executable methods.
- Statement conditional, loop, and branch operations are supported only because P40.3 projects stable Roslyn CFGs. Coalesce remains unsupported until its value conversion is modeled; ternary conditional expressions remain supported.
- Conversion operations retain checked and try-cast semantics. Generic invocation/method-reference operations retain concrete type_argument_ids in addition to their OriginalDefinition symbol ID.
- 2026-07-13 P40.2 schema edit mistake record: a fallback PowerShell command used C-style backslash quote escaping inside a double-quoted string and failed at parse time before writing. Prevention: use literal here-string anchors for multiline PowerShell replacements; do not mix C# escaping rules into PowerShell.
- Unsupported operations must remain in the semantic tree with kind, span, type, and children, emit a stable ASCS2xxx error, and force succeeded=false.
- 2026-07-13 P40.2 verification toolchain mistake record: the first RED command used system .NET 9, whose SDK Roslyn targets System.Runtime 9 and cannot compile the net8.0 tools. Prevention: invoke <UserProfile>\.dotnet\dotnet.exe 8.0.416 explicitly and redirect DOTNET_CLI_HOME, APPDATA, LOCALAPPDATA, and NUGET_PACKAGES to a writable task-local Saved directory before every frontend/semantic build or test.
- 2026-07-13 P40.2 fallback edit mistake record: a PowerShell fallback here-string accidentally retained patch-style plus markers and added a non-constant Replace call to const raw test sources. Readback caught it before RED. Prevention: fallback file content must be plain source without patch markers or cleanup expressions; always read back the exact inserted region before compiling.
- P40.3 semantic test baseline is 28/28 and Phase 39 frontend regression is 7/7. ActorLifecycle schema v3 contains 38 executable methods, 38 CFGs, 120 blocks, 84 successor edges, and 0 diagnostics.
- Operation trees and CFGs must share SemanticExecutableBodyResolver. Expression-bodied property/indexer operations must walk Roslyn Parent links to the implicit method/block root before CFG creation; keep the ActorLifecycle method-to-CFG one-to-one test.
- CFG capture IDs are method-local AvidScript IDs assigned by SemanticCaptureRegistry in deterministic traversal order. Never serialize CaptureId.ToString(), hash codes, reflection-only Value fields, or other Roslyn identity details; keep the multi-capture distinct-ID regression.
- Any exception region, async method, iterator/yield method, invalid compilation, CFG creation failure, or unsupported lowered operation must emit ASCS3001-ASCS3004 and clear the entire control_flow_graphs collection. Inspect destinationless throw/rethrow branches before filtering edges for serializable destinations.
- The plugin root global.json pins exact .NET SDK 8.0.416 with rollForward=disable. Every .NET verification must also redirect DOTNET_CLI_HOME, APPDATA, LOCALAPPDATA, and NUGET_PACKAGES into a writable task-local Saved directory.
- 2026-07-13 P40.3 verification mistake record: the first test command omitted APPDATA/LOCALAPPDATA and, after SDK 10 was added to the same user-local host, selected 10.0.301 and read the blocked user NuGet.Config. Prevention: keep global.json under version control, print dotnet --version before verification, and set all four task-local environment variables.
- 2026-07-13 P40.3 CFG mistake record: the first expression-bodied getter implementation passed a property declaration/expression leaf to ControlFlowGraph.Create, so five real ActorLifecycle getters failed CFG creation. Prevention: resolve the operation root through Parent links and assert all real executable method IDs have matching CFG IDs.
- 2026-07-13 P40.3 capture mistake record: CaptureId.ToString() serialized the type name and collapsed every capture identity. Prevention: use CaptureId only as an in-process dictionary key, assign AvidScript-owned numeric IDs per graph, scan artifacts for Roslyn identity leaks, and require numeric capture IDs in tests.
- 2026-07-13 P40.3 regression observation: the first Phase 39 concurrent atomic-writer regression failed once, followed by five consecutive 7/7 passes. Do not hide a recurrence with a single rerun; capture child stderr and repeat stress before changing the writer.
- 2026-07-13 P40.3 review exception mistake record: exception detection reused the serializable-edge helper, which dropped destinationless throw/rethrow branches before checking semantics and could retain a partial CFG. Prevention: inspect all Roslyn branches for unsupported semantics first, filter null destinations only while serializing edges, and test both block-bodied and expression-bodied throw.
- 2026-07-13 P40.3 review determinism mistake record: global.json initially used latestPatch and therefore still allowed the SDK-shipped Roslyn build to drift. Prevention: use rollForward=disable while semantic artifacts depend on SDK Roslyn lowering, and verify dotnet --version before final tests.
- 2026-07-13 P40.3 fallback wrapper mistake record: nested JavaScript templates containing raw Markdown/PowerShell backticks and a PowerShell variable immediately followed by a colon caused parser failures before writing. Prevention: prefer Environment.NewLine, escape template backticks, and use PowerShell format expressions for diagnostic strings; confirm failed wrappers made no file changes.
- 2026-07-13 P40.3 review workflow mistake record: resuming the original isolated reviewer after local fixes repeated findings from its pre-fix snapshot, and a replacement reviewer was stopped before producing evidence. Prevention: treat subagent workspaces as snapshots, use a fresh post-edit review when available, and resolve conflicts against current file readback plus reproducible tests rather than stale review text.
- P40.4 semantic test baseline is 33/33 and Phase 39 frontend regression is 7/7. ActorLifecycle semantic 1.3 contains 15 types, 186 symbols, 38 methods, 38 CFGs, 120 blocks, 84 successor edges, 86 bound instance references, and 0 diagnostics.
- InstanceReference operations must bind both type_id and symbol_id to the containing type. Constructor, field, property, parameter, and helper-method symbols must retain complete containing_symbol_id and is_static state; guest offsets remain Phase 41 work.
- SemanticSupportPolicy owns ASCS4001 lambda/delegate/closure/local-function, ASCS4002 dynamic, and ASCS4003 unsafe diagnostics. Operation projection owns stable data shape and reports whether a complete projection exists; do not scatter language-profile decisions across emitters.
- Any document-level or operation-level support error clears the entire control_flow_graphs collection, including otherwise safe methods in the same source. Keep methods/operations for diagnostics, but Phase 41 must require succeeded=true and a matching CFG.
- 2026-07-13 P40.4 verification mistake record: the first RED command resolved a new Saved/DotNetCli/P40.4 path before creating its CLI-home, AppData, LocalAppData, and Packages directories, producing null environment paths. Prevention: create and probe every task-local directory before Resolve-Path and dotnet invocation.
- 2026-07-13 P40.4 Roslyn API mistake record: the first unsafe policy referenced IPointerIndirectionReferenceOperation after reflection found the type name, but the interface is internal and failed compilation. Prevention: inspect public accessibility, depend only on public Roslyn APIs, and use public type symbols plus syntax policy for pointer dereference.
- 2026-07-13 P40.4 fail-closed mistake record: document-level dynamic diagnostics initially made succeeded=false but still published a safe method CFG. Prevention: gate the final graph collection on both document and operation support errors, and keep the dynamic-field plus safe-method regression.
- 2026-07-13 P40.4 diagnostic self-review record: the first deduplication grouped the complete compiler, policy, operation, and CFG diagnostic stream by code/span, which could hide distinct compiler messages. Prevention: deduplicate only overlapping document/operation support diagnostics; preserve compiler and CFG diagnostics exactly before final stable ordering.

## Phase 40 Build Integration Rules

- The C# build order is syntax frontend, semantic CLI, semantic hard gate, then emitter. Never let the transitional AST emitter or WASM publication run when the semantic artifact is absent, invalid, or succeeded=false.
- Every build attempt removes stale report, manifest, frontend artifact, semantic artifact, adapter WASM, and dotnet WASM before analysis. Semantic failure keeps diagnostic frontend/semantic artifacts but must leave no loadable manifest or WASM.
- C# report and manifest provenance must carry the semantic artifact path, schema/version, and matching source/frontend hashes. Editor readers must keep Phase 39 and legacy reports loadable with default semantic fields.
- Semantic reference sources participate only in Roslyn Compilation; public projection remains rooted in the primary script source. Reference source ids must be stable and must not serialize absolute paths.
- The current non-default-profile reference to the ActorLifecycle sample facade is transitional. Phase 42 must replace it with Reflection Binding Generator output; do not expand the sample facade by hand.
- P40.5 baselines are Semantic 34/34, Frontend 7/7, BuildIntegration 3/3, Report 5/5, C# Guest 4/4, C# BuildService 2/2, and full UE5.8 automation 139/139.
- 2026-07-14 P40.5 edit-tool mistake record: apply_patch failed twice in the Windows sandbox, then the first controlled PowerShell fallback nested a single-quoted here-string inside another and failed to parse. Prevention: use different here-string delimiters for nested source, validate parser-safe anchors, and confirm git status before retrying.
- 2026-07-14 P40.5 verification mistake record: the first final .NET regression command again omitted task-local APPDATA/LOCALAPPDATA/NUGET_PACKAGES despite the existing rule and failed on the blocked user NuGet.Config. Prevention: every .NET command starts from the shared four-variable task-local preamble; do not issue ad hoc dotnet verification commands.
- 2026-07-14 P40.5 integration mistake record: the first semantic gate compiled only the custom primary source, so BuildService CustomProfileSmoke failed with CS0103 for the SDK Actor facade. Prevention: custom profiles must supply explicit semantic reference sources, and both CLI binding tests and UE BuildService automation are required before closeout.
- 2026-07-14 P40.5 fallback transaction mistake record: a multi-file fallback created SemanticReferenceSource.cs before a CRLF-specific removal anchor failed. Prevention: validate all target anchors independent of newline style before the first write, then perform writes; always inspect status after any fallback exception.
- 2026-07-14 P40.5 AGENTS append mistake recurrence: TrimEnd plus a here-string again left the new heading without a blank separator. Prevention: construct heading boundaries with two explicit newline sequences and read back the preceding line, blank line, and heading after every append.

## Phase 41 Guest IR And Codegen Rules

- Guest IR is language-neutral and must not reference Roslyn, C#, UE, WAMR, or PowerShell types. CSharpGuest lowering may depend on the semantic artifact; WasmBackend may depend only on validated Guest IR.
- PowerShell is orchestration only. Do not add expression parsing, control-flow reconstruction, state layout, container lowering, or WASM opcode emission to BuildCSharpActorLifecycle.ps1.
- Every external call must originate from stable callable ABI metadata or a generated binding descriptor. Never guess UE bindings from method names and do not add another handwritten UE API table before Phase 42.
- Guest IR uses typed register CFG with explicit terminators. IDs, ordering, serialization, data layout, and diagnostics must be AvidScript-owned and byte deterministic.
- Strings are immutable UTF-8 data, arrays use a length header and mandatory bounds checks, enums use their projected underlying integer type, and UObject references remain slot/generation handles.
- The initial general CFG backend may use a deterministic block-dispatch loop. Keep that choice isolated behind WasmBackend so Phase 53 can add structuralization/AOT without changing Guest IR.
- 2026-07-14 P41.0 edit-tool record: apply_patch created new Phase 41 documents but failed twice while opening the existing root tracker in the Windows sandbox. Prevention: after two helper failures, use the controlled fallback only after validating every file, unique anchor, newline style, and replacement before the first write; inspect both readbacks immediately.
- 2026-07-14 P41.0 fallback newline recurrence: the controlled here-string again contributed only one boundary newline, leaving the Phase 41 headings attached to the preceding paragraph. Prevention: build the exact old and new boundary from explicit newline variables, assert one occurrence, and verify the preceding line plus blank line after every heading insertion.
- Semantic schema v4 / semantic version 1.4 adds callables and type_shapes. Guest lowering must consume ordered callable parameters and array/enum shape fields; never parse SemanticSymbol.Signature or display names to recover ABI structure.
- DllImport and UnmanagedCallersOnly metadata must come from Roslyn AttributeData. Empty import metadata uses ASCS5001, missing export EntryPoint uses ASCS5002, and duplicate export names use ASCS5003; any callable ABI error clears all CFGs.
- P41.1 baseline is Semantic 38/38 and BuildIntegration 3/3. ActorLifecycle has 15 types, 186 symbols, 52 callables, 38 bodies/CFGs, 14 imports, 5 exports, and 0 diagnostics.
- Run dotnet format directly on every changed production project and test project. Formatting a test project skips referenced production projects and is not evidence that production sources were checked.
- 2026-07-14 P41.1 patch-path mistake record: the first RED patch used plugin-relative Tools paths while apply_patch was rooted at the workspace, so it targeted the nonexistent root Tools directory and failed before writing. Prevention: apply_patch paths are always workspace-relative and must start with Plugins/AvidScript for plugin files.
- 2026-07-14 P41.1 fallback tuple mistake record: a PowerShell array of two-element arrays was flattened, concatenating old/new analyzer anchors. Transaction validation stopped all writes. Prevention: use explicit named anchors or PSCustomObject records; never rely on nested PowerShell arrays preserving tuple boundaries.
- 2026-07-14 P41.1 TDD mistake record: the first callable projector included ASCS5002 behavior before a dedicated failing test had been observed. Prevention: do not implement adjacent error branches speculatively; remove untested behavior, run the focused RED, then restore only the behavior required by that test.
- 2026-07-14 P41.1 fallback source mistake record: a controlled here-string copied patch-style plus prefixes into a C# test and attempted to hide them with Replace. Readback caught it before RED. Prevention: fallback content is plain source, never patch text; reject leading plus markers and runtime cleanup expressions before writing.
- 2026-07-14 P41.1 automation command mistake record: the first Report command used -log and appended ; Quit, so Editor exited before discovering tests and wrote no requested absolute log while returning 0. Prevention: use -abslog, omit Quit, let -TestExit=Automation Test Queue Empty terminate the run, and require both Found N automation tests and zero Result={Fail} in the log.
- 2026-07-14 P41.1 verification regex mistake record: a combined rg pattern was mangled by PowerShell escaping and failed with an unclosed group. Prevention: run simple leading-marker and cleanup-expression scans as separate fixed patterns instead of composing a shell-escaped alternation.

- Guest IR v1 is the language-neutral contract at schema 1 / IR 1.0. Every lowering producer must pass GuestModuleValidator before publishing; every backend must reject unvalidated or invalid IR.
- Guest IR artifact writes are validate-first and same-directory atomic replacement. Never let a failed generation overwrite the last known-good artifact.
- P41.2 baseline is GuestIr 15/15: 13 validator cases plus 2 deterministic serializer/atomic-writer cases.
- 2026-07-14 P41.2 toolchain mistake recurrence: verification first used the system dotnet host and then guessed an outdated UE-bundled 8.0.416 path, despite the repository rule naming the user-local 8.0.416 host. Prevention: read global.json, run the selected host with --list-sdks, and invoke <UserProfile>\.dotnet\dotnet.exe explicitly before all Phase 41 .NET commands.
- 2026-07-14 P41.2 sandbox mistake recurrence: the first verification omitted the complete task-local environment preamble; NuGet also recreates its zero-byte first-use migration marker under the plugin root in this sandbox even after correct isolation. Prevention: precreate and probe DOTNET_CLI_HOME, APPDATA, LOCALAPPDATA, USERPROFILE, and NUGET_PACKAGES, ignore only NuGet/Migrations rather than the whole NuGet namespace, and confirm git status immediately afterward.
- 2026-07-14 P41.2 fallback source mistake recurrence: a controlled PowerShell test edit again contained patch-style plus prefixes. The explicit marker scan stopped the transaction before write. Prevention: fallback here-strings must be plain source, and the leading-plus rejection remains mandatory before any temporary file is moved into place.
- 2026-07-14 P41.2 verification command mistake: comma-separated Join-Path calls inside @() were parsed as extra arguments to one command; the non-terminating error left isolation variables empty and a later --no-restore build failed reading assets. Prevention: assign each isolation path to an explicit named variable, set ErrorActionPreference=Stop for verification scripts, probe all directories, and restore before retrying after any environment setup failure.

- Final Guest IR v1 modules carry canonical memory_layout and data_segments. Type lowering, data encoding, region placement, and final validation must use the shared GuestIr Layout subsystem; do not recompute offsets in CSharpGuest or WasmBackend.
- Linear memory reserves [0,16) as a null guard, caps alignment at 16 bytes, sorts state/data by stable IDs, and uses checked 32-bit arithmetic. Runtime array access must emit a bounds check before address calculation.
- String data is [utf8 byte length:i32][bytes][0]. Array data is [count:i32][alignment padding][payload], with stride AlignUp(element size, element alignment).
- P41.3 baseline is GuestIr 28/28, including 13 layout tests for struct/enum/string/array/state/data/heap, overflow, zero initialization, and artifact tamper rejection.
- 2026-07-14 P41.3 fallback source mistake recurrence: several controlled test edits again included patch-style plus prefixes; the mandatory marker scan rejected every transaction before write. Prevention: compose fallback here-strings as standalone source, run the marker scan before any anchor replacement, and prefer new-file apply_patch for large additions.
- 2026-07-14 P41.3 fallback anchor mistake record: a method insertion searched for only the beginning of a signature followed immediately by a newline, but parameters continued on the same line, so unique-anchor validation stopped the write. Prevention: read the exact full signature line before constructing insertion anchors; never infer the newline position.

### P41.4 CSharp Semantic Lowering Rules

- CSharpGuest consumes only succeeded semantic schema v4 / semantic 1.4 artifacts. It must not reread C# source, parse signature strings, guess bindings from method names, or add handwritten UE API tables.
- Mutable locals/state, aggregates, pointers, and constant data use explicit Guest IR instructions with independent validator rules. New opcodes require lowering, validator, backend, and behavior tests together.
- Bound property accessors and user-defined operators resolve through SemanticCallable metadata. ref/out/in parameters use type:address plus typed indirect_load/indirect_store.
- UTF-8 strings and constant arrays use the shared P41.3 layout and content-addressed data segments. Dynamic arrays and bounds-checked element access remain later backend/container work.
- P41.4 baselines are CSharpGuest 14/14 and GuestIr 29/29. Real ActorLifecycle lowers to 12 types, 14 imports, 1 global, 38 functions, 120 blocks, 357 instructions, and 5 exports with deterministic SHA-256 f1da58e9816a93068197df8d03254fe4d000ef51b98900a09c3acaa44e35f7b4.
- 2026-07-15 P41.4 verification environment mistake record: the first format command resolved Saved/DotNetCli/P41.4 before creating it, then running from the UE project root bypassed the plugin global.json and selected SDK 10; a follow-up relative path also climbed one parent too far. Prevention: create every isolation directory first, run dotnet from the plugin root, assert version 8.0.416, and derive absolute paths from resolved plugin/project roots instead of hand-counting parents.
- 2026-07-15 P41.4 diagnostic aggregation mistake record: CSharpControlFlowLowerer initially treated any diagnostic from an earlier function as failure for every later function, hiding real errors behind missing-export cascades. Prevention: capture a function-local diagnostic baseline and compare only diagnostics added while lowering that function.
- 2026-07-15 P41.4 fallback source mistake recurrence: large controlled here-strings again contained patch-style leading plus markers; marker guards prevented writes, but one later edit attempted an in-memory cleanup expression before writing. Prevention: never clean patch markers at runtime; use apply_patch for new files and small exact-anchor edits for existing files, reject contaminated drafts, and read back before compile.
- 2026-07-15 P41.4 PowerShell inspection mistake record: a CFG summary pipeline was attached directly after a closing foreach block and produced an empty summary. Prevention: assign traversal results to a named collection, end the loop, then run the pipeline as a separate statement and assert the collection count.
- 2026-07-15 P41.4 final hash mistake record: final verification asserted the pre-review ActorLifecycle hash after user-defined operators had been correctly changed from binary opcodes to callable instructions. Prevention: after any semantic lowering change, generate two fresh artifacts, compare them to each other first, inspect opcode deltas, and only then update the documented golden hash.

### P41.5 Deterministic WASM Backend Rules

- WasmBackend consumes only validated Guest IR. It must not depend on Roslyn, C# source text, UE Reflection, WAMR internals, PowerShell semantics, or handwritten per-API binding tables.
- Module/function indices, type deduplication, sections, locals, data segments, custom provenance, LEB encodings, and diagnostics must be AvidScript-owned and byte deterministic.
- Array element access emits an unsigned bounds check before address arithmetic and traps with unreachable on failure. Storage representation is not enough for semantic validation; validators must also check the Guest IR type kind.
- Defined memory-struct returns use caller-owned hidden sret. Linear-memory frames use an explicit stack pointer, check memory bounds before use, and restore the pointer on every normal return path.
- P41.5 baselines are CSharpGuest 14/14, GuestIr 31/31, WasmBackend 10/10, WAMR CLI load/execute plus bounds trap, and UE GeneratedWasmBackendArtifactSmoke Success. Real ActorLifecycle compiles to 8,849 bytes with deterministic SHA-256 a76cef5c06cbdcc4eb06020fe0cfed4ce29d3a7f69f38f8f89adbc0ef49ec75d.
- 2026-07-15 P41.5 nested-source escape mistake record: a nested JavaScript/PowerShell write interpreted the C# backslash-n literal as a real newline and broke the provenance string.Join. Prevention: do not embed backslash escape literals through multiple language parsers; use char 10, apply_patch, or a single-parser source file and read it back before compile.
- 2026-07-15 P41.5 concatenation-boundary mistake record: controlled here-string replacements repeatedly joined break and the following case on one line. Prevention: add Environment.NewLine explicitly at concatenation boundaries and scan changed C# for break followed by case before format/build.
- 2026-07-15 P41.5 marker-guard mistake record: a whitespace-plus regex rejected legitimate indented C# increment expressions such as ++count. Prevention: patch-marker guards reject only column-zero plus signs; rely on compile, format, and diff review for source correctness.
- 2026-07-15 P41.5 semantic-kind mistake record: the first array validator accepted any 4-byte i32 storage type as an index, including array handles. Prevention: validate semantic kind and width independently; never infer numeric eligibility from WASM storage alone.
- 2026-07-15 P41.5 PowerShell environment mistake record: the first final format command assigned a lowercase home variable, which is case-insensitively identical to PowerShell's read-only HOME. Prevention: use explicit names such as cliHomePath for isolation variables and keep ErrorActionPreference=Stop so setup failures cannot fall through into dotnet.
- 2026-07-15 P41.5 boundary-test fixture mistake record: the first runtime-stack RED directly changed heap_start and therefore failed Guest IR canonical layout validation before reaching the backend assertion. Prevention: construct boundary fixtures through GuestDataLayout and GuestLayoutBuilder, assert the fixture is valid, and only then test backend behavior.

### P41.6 Formal CSharp To WASM Toolchain Rules

- The default C# build chain is source -> frontend -> semantic -> Guest IR -> WasmBackend -> manifest/report. PowerShell is orchestration only and must never regain AST interpretation, control-flow reconstruction, UE API discovery, or WASM opcode emission.
- Every build removes stale Guest IR, manifest, WASM, and legacy adapter artifacts before analysis. A failed stage may keep diagnostic frontend/semantic artifacts and its report, but it must not leave anything loadable by the runtime.
- Custom C# source profiles fail closed with phase42_binding_required / ASBI4201 until generated UE facade and binding descriptors are available. Packaging identity such as ModuleId, ArtifactStem, and output paths remains independent. Do not restore the AST adapter or bypass the source gate with a handwritten API table.
- Editor build failures prefer the structured report result and first error diagnostic over a generic process exit. Binding commands must stop immediately after a failed build and must never inspect or bind a null component.
- P41.6 baselines are Frontend 7/7, Semantic 38/38, GuestIr 31/31, CSharpGuest 15/15, WasmBackend 11/11, BuildIntegration 3/3, BuildPublicationContracts 3/3, Editor Report 5/5, Editor SuccessContract 1/1, and full UE5.8 AvidScript automation 141/141. Formal ActorLifecycle has 39 functions, 130 blocks, 14 imports, 6 exports, and a 9,546-byte WASM SHA-256 d8bea6beb5bc5f30a313c66b2b81ce53d7a1a242e0a80d747de9c42592f47a6e.
- 2026-07-15 P41.6 control-flow bug record: every Roslyn exit block without successors was lowered to trap, so legal void fallthrough reached unreachable during BeginPlay. Prevention: distinguish void fallthrough return from impossible non-void exit, keep a focused regular-edge-to-exit regression, and require the real UE lifecycle smoke before closeout.
- 2026-07-15 P41.6 test-ID assumption mistake record: the first focused exit test guessed that Guest block IDs ended in `:block:2`; the canonical ID actually ends in the block ordinal only. Prevention: derive expected IDs from the documented ID contract or inspect the ID builder before writing exact identity assertions.
- 2026-07-15 P41.6 PowerShell portability bug recurrence: despite the P11.1 rule, report hashing again used Get-FileHash, which was unavailable in the PowerShell environment launched by UE even though it worked interactively. Prevention: build scripts use framework APIs for required primitives such as SHA-256 and must be exercised through the real Editor process, not only an interactive shell.
- 2026-07-15 P41.6 automation crash record: a legacy profile test ignored the failed build result and dereferenced a null binding component, causing EXCEPTION_ACCESS_VIOLATION. Prevention: tests and production commands branch on failure before object access; every failed build asserts no binding, no component, and no stale manifest.
- 2026-07-15 P41.6 stale-contract test record: runtime tests still inspected obsolete AST adapter manifest prose and a sample test required the removed AllowEmptyString marker. Prevention: validate structured artifact contracts, hashes, imports/exports, and live behavior; never treat implementation text as a compiler contract.
- 2026-07-15 P41.6 PowerShell collection mistake record: a cleanup helper combined comma-separated string expressions with `+`, flattening all include names into one value and stopping before write. Prevention: build edit lists from plain literal elements or PSCustomObject entries, then assert each target independently before any write.
- 2026-07-15 P41.6 verification regex recurrence: a combined rg expression for several C++ symbols was malformed by nested escaping and failed with an unclosed group after edits had succeeded. Prevention: verification scans use separate fixed-string rg calls unless regex grouping is essential.
- P41.6 final WASM success is defined by the inspected binary, not Guest IR declarations alone. `observed_exports` comes from `WasmArtifactInspector`, the inspection SHA-256 must match the published WASM, and both declared and observed direct ABI exports must pass before manifest publication.
- Manifest/report publication is transactional with respect to loadable artifacts. Any report or manifest write failure removes both manifest and WASM; Editor also rejects exit-0 builds unless report schema/result/succeeded, error diagnostics, manifest, and WASM satisfy the success contract.
- Source/facade semantics define the Phase 42 binding gate. ModuleId, ArtifactStem, report path, manifest path, and output root are packaging/deployment identity and must not be treated as UE API authorization; the existing default profile intentionally uses custom module/stem values.
- 2026-07-15 P41.6 review-gate correction: an external review suggestion proposed treating ModuleId, ArtifactStem, and output paths as custom-binding evidence. Repository inspection showed the default profile intentionally customizes those fields, so that part was rejected. Prevention: search profile templates and consumers before widening a semantic gate to packaging metadata.
- 2026-07-15 P41.6 fallback interpolation mistake record: a double-quoted PowerShell replacement expanded `$DefaultArtifactStem` while generating source and briefly wrote ` = "actor_lifecycle"`. The PowerShell parser accepted it as a command, so parse-only verification did not catch the damage. Prevention: source-generating helpers use single-quoted here-strings, read back assignment blocks, scan for assignment lines missing a variable, and run a functional build smoke.
- 2026-07-15 P41.6 generated-fixture mistake record: the fake Guest compiler preamble and body were concatenated without a newline, producing `"Stop"$Model` and a parser failure. Prevention: generated scripts join sections with an explicit platform newline and are parsed before invocation.
- 2026-07-15 P41.6 child-stderr bug record: with `ErrorActionPreference=Stop`, redirected native stderr became a terminating `NativeCommandError` before the parent could classify a valid `succeeded=false` Guest IR. Prevention: child PowerShell stages run through `Invoke-AvidScriptPowerShell`, temporarily capture native stderr under Continue, preserve the real exit code, then restore the caller preference.
- 2026-07-15 P41.6 verification-command interpolation mistake: a diagnostic string used `$path:` in a double-quoted PowerShell string, which is parsed as an invalid scoped variable. Prevention: delimit variables before punctuation with `${path}:` in verification helpers.

## Phase 42 Reflection Binding Generator Rules

- Phase 42 uses versioned Binding Descriptor + WAMR raw native trampoline + cached reflected invocation plans. Adding an ordinary supported UE API must not require a new `EAvidScriptHostBindingId`, a handwritten `NativeSymbol` wrapper, or a Runtime dispatch switch case.
- Reflection selection specs are authorization policy only. Import names, WASM signatures, C# proxy methods, ordinals, parameter directions, type layouts, and marshalling plans must be generated from reflected metadata and a shared type policy.
- WAMR remains private to AvidScriptVM. VM Public may expose language-neutral module/name/signature/ordinal/frame descriptors, but never UObject, UFunction, FProperty, FVector, or WAMR types. AvidScriptBindings must not depend on WAMR.
- Descriptor canonical identity includes owner path, function name, return type, and ordered parameter name/direction/type. Stable IDs use the complete SHA-256; compact ordinals are valid only together with the matching package hash.
- Package load performs all JSON parsing, name lookup, UFunction/FProperty resolution, default-value materialization, and invocation-plan construction. BeginPlay/Tick/Event calls use ordinal indexing and immutable cached plans; do not add string lookup or reflection traversal to the hot path.
- Custom C# binding authorization is based on generated descriptor/reference-source/package hashes and required-import membership, not on SourcePath, ModuleId, ArtifactStem, report path, manifest path, or output root alone.
- Phase 42 v1 supports scalar, enum, UObject handle, FVector, FRotator, FTransform, and safe non-latent UFunction projections first. Unsupported properties, latent/custom-thunk/delegate/editor-only functions, ambiguous identities, frame mismatches, stale handles, or package hash mismatches fail closed with stable diagnostics.
- A transitional P42.1/P42.2 slice may generate facade code that reuses the existing ten host imports to unlock the first custom C# script. It must be descriptor-generated, must not expand the handwritten sample facade, and must yield to raw dynamic registration as the default extension path in P42.3/P42.4.
- P42.1 baseline is Core Hash 1/1, Editor BindingDescriptor 4/4, legacy BindingSchema 3/3, architecture check passed, and full UE5.8 AvidScript automation 146/146.
- 2026-07-15 P42.1 unity-build helper mistake record: the new descriptor generator reused `GetScriptFunctionName` inside an anonymous namespace, but UE unity compilation merged it with the Phase 31 helper and produced C2084. Prevention: file-private helpers in UE modules still use subsystem-specific prefixes, and every new cpp is verified with the real unity-enabled module build.
- 2026-07-15 P42.1 platform-hash crash recurrence: despite the existing P5.2c warning, descriptor hashing reused `FPlatformMisc::GetSHA256Signature`; UE5.8 asserted with `No SHA256 Platform implementation` in the first Editor automation. Prevention: search this mistake ledger before selecting platform helpers, route all package identity through portable `FAvidScriptHash`, preserve standard known-vector tests, and exercise hashing inside the real command-line Editor.
- 2026-07-15 P42.1 TArray alias mistake recurrence: despite the P31 test rule, `DuplicateSelections.Add(DuplicateSelections[0])` was reused and crashed the failure-path automation when `Add` reallocated. Prevention: copy container elements to independent local values before mutating the same `TArray`, and search prior mistake records before writing structurally similar tests.
- 2026-07-15 P42.1 producer-module build mistake record: adding exported `FAvidScriptHash` APIs and compiling only the consumer `AvidScriptEditor` refreshed its import library but left the deployed `AvidScriptCore.dll` stale, causing Windows loader error 127. Prevention: after adding or changing exported APIs, explicitly module-build the producer DLL before its consumers, verify the expected export when loader diagnostics are ambiguous, and never use an Editor Target clean as a shortcut.
- 2026-07-15 P42.1 PowerShell collection mistake recurrence: while recording mistakes, comma-separated concatenation expressions intended as three array elements flattened into one Markdown line, repeating the P41.6 collection trap. Prevention: assign each composed line to a named scalar first, join only those scalars with an explicit newline, and inspect the exact AGENTS tail immediately after every fallback write.
- 2026-07-15 P42.1 inspection interpolation mistake recurrence: a PowerShell line-number command again placed a colon directly after a variable despite the P41.6 punctuation rule, so it was parsed as an invalid scoped variable. Prevention: use the format operator for path/line output, or delimit every variable before punctuation.
- 2026-07-15 P42.1 nested-command escape mistake recurrence: JavaScript template tool commands embedded PowerShell newline backticks and later a dollar-brace literal, so the outer JavaScript parser failed before any file write. Prevention: nested tool commands construct special characters from character codes or use a single-parser script; do not embed inner-language escape or interpolation delimiters inside an outer template.
- 2026-07-15 P42.1 newline-readback mistake record: an AGENTS fallback write succeeded, but its verification assumed only the host newline sequence while the file temporarily contained mixed LF/CRLF, reporting a false failure. Prevention: validate semantic lines with `Get-Content` or accept both newline forms, then run `git diff --check`.
- P42.2 generated C# has a public typed facade and a separate internal Native layer. Public object proxies never expose raw slot/generation constructors; generated imports, ordinals, ABI flattening, and package constants remain internal implementation details.
- P42.2 binding packages are immutable and content addressed. Default publication is under `Saved/AvidScriptGeneratedBindings/<package-name>/<manifest-hash>` so descriptor, generated source, and emitter-version changes cannot alias; identical bytes are reused, mismatched existing bytes fail with `package_conflict`, and staging stays under a short sibling root before same-volume commit.
- P42.2 enum definitions are descriptor data: visible member names and int32 values participate in the type stable id and package identity. The emitter must generate those members and typed enum defaults; an empty facade enum is not valid enum support.
- Numeric default metadata is accepted only after full-string grammar, range, and finite-value validation. Never use permissive `LexTryParseString` alone for generated C# defaults.
- Roslyn compiler diagnostics from generated reference sources are part of semantic success. Reference-source spans must not be projected against the primary script, and any reference compiler error clears CFG output and fails closed.
- Generated object proxy members (`Slot`, `Generation`, `AvidScriptSlot`, `AvidScriptGeneration`, `IsValid`) are reserved names. A reflected collision fails with `generated_member_collision`; an owner used as an object parameter or return remains a handle struct even when all selected owner methods are static.
- P42.2 baseline is Frontend 7/7, Semantic 39/39, Editor CSharpBindingEmitter 3/3, architecture check passed, and full UE5.8 AvidScript automation 149/149. Roslyn semantic schema 4 / version 1.4 accepts `GeneratedProjectionSmoke.cs` with 5 methods, 5 callables, and 0 diagnostics; the default smoke remains 3 methods, 3 callables, and 0 diagnostics.
- 2026-07-15 P42.2 Windows search mistake record: an exploration command passed `Tools/AvidScript.CSharp*` as a literal Windows path and `rg` failed with OS error 123 because PowerShell did not expand it. Prevention: enumerate literal directories or use `rg --glob`; never pass an unexpanded wildcard path as a Windows directory argument.
- 2026-07-15 P42.2 Unreal container API mistake record: descriptor parsing treated `TSet::Add` as a boolean uniqueness result, but UE returns `FSetElementId`, causing C2678/C2088. Prevention: validate uniqueness with `Contains`, fail before mutation, then call `Add` explicitly.
- 2026-07-15 P42.2 publication API and path mistake record: the first publisher assumed `IPlatformFile::MoveDirectory` existed, then placed a full package hash plus GUID in the staging directory and exceeded practical Windows path limits. Prevention: inspect UE5.8 platform-file APIs, keep staging under a short output-root sibling, and use the editor-host same-volume rename path only after all bytes are written.
- 2026-07-15 P42.2 SDK selection recurrence: `InvokeCSharpFrontend.ps1` invoked dotnet from the UE project root, bypassed the plugin `global.json`, selected SDK 10.0.301, and failed offline net8 targeting-pack restore. Prevention: resolve source/output paths first, push to the plugin root, read the expected SDK from `global.json`, assert the selected version, then build and invoke Roslyn.
- 2026-07-15 P42.2 nested path escape recurrence: a JavaScript template carrying PowerShell exact anchors consumed single backslashes before PowerShell saw them, so transaction validation reported a false missing anchor. Prevention: avoid backslashes in nested anchors, use forward-slash paths or doubled escaping, and keep unique-anchor validation before every write.
- 2026-07-15 P42.2 automation command recurrence: intermediate focused tests reused `-log` plus a queued `Quit` despite the P41.1 rule, weakening log-path and completion evidence even though those runs happened to execute. Prevention: final and future automation use absolute `-abslog`, omit `Quit`, terminate only through `-TestExit=Automation Test Queue Empty`, and verify both the discovered count and zero failures.
- 2026-07-16 P42.2 .NET host recurrence: the first new semantic RED command invoked bare `dotnet` despite the pinned-host rule and selected the incompatible system host. Prevention: every direct .NET command uses `<UserProfile>\.dotnet\dotnet.exe`, asserts 8.0.416, and sets the complete task-local environment before build or test.
- 2026-07-16 P42.2 low-memory build mistake record: the first review build used default 20-action UBA while committed virtual memory was near the machine limit, causing an unproductive compile retry loop. Prevention: inspect free virtual memory before UE builds; below 10 GB use `-MaxParallelActions=1 -NoUBA`, keep the build incremental, and never clean the Editor Target.
- 2026-07-16 P42.2 fallback transaction recurrence: a two-file PowerShell fallback wrote the first header before discovering that the second file anchor was ambiguous. Prevention: resolve every target and validate every unique anchor for the whole transaction before the first temporary file is written or moved.
- 2026-07-16 P42.2 automation `Quit` recurrence: the review-focused run again queued `Quit` despite the rule immediately above. Prevention: use only `-TestExit=Automation Test Queue Empty`; the final 149/149 run followed this corrected form and verified discovery, success count, zero failures, and exit code.
- P42.2 source organization rule: descriptor identity/model, C# syntax/default formatting, source rendering, and package publication are separate private services. Keep WAMR registration, UE reflection invocation, and generated C# text out of the same implementation file.
- 2026-07-16 P42.2 unity split mistake record: after extracting the renderer and default formatter, both cpp files defined a generic anonymous-namespace `FindType`; UE unity compilation merged them and produced C2084. Prevention: every file-private helper in a UE module uses a subsystem-specific name even inside anonymous namespaces, and every structural split is verified with the real unity-enabled incremental build.
- P42.3 dynamic raw imports are registered only inside AvidScriptVM. Global WAMR symbols are reference counted by module/name, while each backend keeps its own attachment-to-ordinal map. Register before module load, unload the module before releasing registrations, and never expose WAMR types through VM Public.
- P42.4 descriptor parsing and invocation-plan construction occur once at package load. Dynamic calls use immutable ordinal-indexed plans, host-owned UObject handles, preallocated aligned full-function scratch frames, and explicit write policy; do not add reflection traversal, JSON parsing, or per-call heap allocation to the hot path.
- P42.3/P42.4 baseline is DynamicRawRegistry 2/2, BindingRuntime 2/2, BindingDescriptor 4/4, CSharpBindingEmitter 3/3, and full UE5.8 AvidScript automation 153/153. Final automation must verify discovered count, success count, zero failures, and queue completion from the absolute log.
- 2026-07-17 P42.4 automation command recurrence: the first diagnostic run used `-log` and queued `Quit`, completed only platform validation, and produced no requested log. Prevention: use `-abslog`, omit `Quit`, and terminate only with `-TestExit=Automation Test Queue Empty`.
- 2026-07-17 P42.4 test fixture mistake record: an abstract `UObject` was used only to reserve registry slot one and triggered an engine ensure. Prevention: registry ordering tests use concrete existing UObjects such as the Actor root component; never instantiate abstract engine classes as padding.
- 2026-07-17 P42.4 ABI mistake record: void return was initially counted as a guest output pointer because direction was checked before semantic type. Prevention: canonical void always has zero argument width; only non-void returns require guest memory and an output address.
- 2026-07-17 P42.4 authorization propagation mistake record: the first dynamic reflection dispatcher did not copy `ActorWritePolicy` from Runtime host context, so non-const reflected functions could not enforce the established write gate correctly. Prevention: every new generic dispatch context explicitly carries object registry, owner handle, and write policy end to end.
- 2026-07-17 P42.4 dependency mistake record: a test-only no-op `IAvidScriptVmGuestMemory` subclass made AvidScriptEditor directly require VM interface constructor/destructor symbols and failed at link time. Prevention: model whether guest memory is semantically required in the cached invocation plan; do not fabricate interface implementations to satisfy an incorrect frame contract.
- 2026-07-17 P42.4 UE callspace mistake record: a reflected `SetActorScale3D` call returned through `ProcessEvent` without changing the Actor because the test world had not called `InitializeActorsForPlay`; the authority-only UFunction was absorbed while direct C++ invocation still worked. Prevention: reflected gameplay tests create a real initialized Game world, assert the Actor role/callspace precondition, and preserve UE authority/network semantics instead of bypassing ProcessEvent.
- 2026-07-17 P42.4 frame-layout hardening: using `ParmsSize` scratch with `UStruct::InitializeStruct` can under-allocate functions whose complete structure includes additional properties, and raw `TArray<uint8>` start addresses cannot be assumed to satisfy every future platform alignment. Prevention: cache `GetStructureSize` and `GetMinAlignment`, reserve size plus alignment padding once, align inside the reusable scratch, and initialize/destroy the complete frame.
- 2026-07-17 P42.4 fallback newline recurrence: the first exact PowerShell edit assumed CRLF for an LF C++ file and correctly stopped at anchor count zero before writing. Prevention: inspect newline style before constructing every fallback transaction, validate every anchor before the first temp write, then read back the changed region.
- 2026-07-17 P42.4 PowerShell punctuation recurrence: a diagnostic helper used a variable immediately followed by a colon and hit scoped-variable parsing. Prevention: use the format operator or `${name}:` whenever punctuation follows a variable.
- 2026-07-17 P42.4 PowerShell escaping recurrence: a command used C-style escaped quotes and failed before execution. Prevention: PowerShell source generation uses literal strings/here-strings or doubled quotes according to PowerShell rules, never C/C# backslash escaping.
- 2026-07-17 P42.4 search scope mistake record: a broad recursive search under the full engine source produced excessive output while investigating WAMR raw argument layout. Prevention: locate the owning subsystem first, then search the smallest relevant WAMR or UE source directory with focused symbols.
- P42.5 generated facades enter the compiler through explicit executable reference sources. Ordinary reference sources remain compile-only; never concatenate generated facade text with user source in PowerShell.
- Custom C# builds authorize generated UE imports only through a verified content-addressed Binding Package. Editor may publish the default package when a custom profile omits `binding_package_path`, but the resolved `package.json` must still be passed explicitly into the formal build.
- Reload manifest loading verifies package.json, descriptor, package identity, artifact hashes, contained descriptor path, and dynamic import membership before constructing a candidate VM. Runtime Session receives only the immutable validated package and must reject generated imports with `binding_package_missing` otherwise.
- P42.5 baseline is Frontend 7/7, Semantic 41/41, GuestIr 31/31, CSharpGuest 16/16, WasmBackend 11/11, BuildIntegration 3/3, BuildPublicationContracts 3/3, and full UE5.8 AvidScript automation 155/155.
- 2026-07-17 P42.5 .NET concurrency mistake record: an early verification launched dotnet builds that shared project obj directories in parallel and hit CS2012 file locking. Prevention: all dotnet format/build/test operations that share a project graph run sequentially under one isolated environment.
- 2026-07-17 P42.5 multi-source semantic mistake record: the first executable-reference projection treated repeated namespace symbols as duplicate stable IDs. Prevention: merge only namespace symbols as legal C# partial namespace declarations; methods, fields, properties, parameters, and types keep strict duplicate-id rejection.
- 2026-07-17 P42.5 discard-lowering mistake record: generated wrappers use `_ = Native.Invoke(...)`, but Guest lowering initially treated discard as writable storage. Prevention: evaluate a discard assignment RHS exactly once, return its value, and emit no store.
- 2026-07-17 P42.5 PowerShell edit-boundary mistake record: two generated insertions omitted a newline and left adjacent parameter/map fields on one line. Prevention: preserve explicit boundary newlines and run the PowerShell parser before every functional build invocation.
- 2026-07-17 P42.5 child-process mistake record: a nested Windows PowerShell process remained alive after an unhandled dotnet exception. Prevention: terminate only the exact process tree launched by the current task, then rerun the failing stage directly rather than nesting more wrappers.
- 2026-07-17 P42.5 source-engine build mistake record: after `dotnet build-server shutdown`, `Build.bat` failed in the UBT dependency self-check before plugin compilation. Prevention: use the existing source-engine `UnrealBuildTool.dll` directly for low-concurrency incremental module builds with `-MaxParallelActions=1 -NoUBA`; never clean the Editor Target as a workaround.
- 2026-07-17 P42.5 verification-wrapper mistake record: an outer PowerShell wrapper read `$LASTEXITCODE` left by an intentionally failing child case after a script had already reported 3/3 success. Prevention: verify such PowerShell contract suites in a fresh `powershell.exe -File` process and use that process exit code.
- 2026-07-17 P42.5 review-format mistake record: a valid but unreadable `); if (...)` statement boundary survived a format verification run. Prevention: format verification does not replace direct diff/readback review around every edited control-flow boundary.
- 2026-07-17 P42.5 architecture-gate drift mistake record: the P42.4 descriptor parser added a legitimate private `Json` dependency to AvidScriptBindings without updating `CheckAvidScriptArchitecture.ps1`. Prevention: every capability ownership or module dependency change updates and runs the architecture gate in the same change; required infrastructure dependencies are encoded explicitly instead of left as stale forbidden entries.
- 2026-07-17 P42.5 PowerShell punctuation recurrence record: a controlled fallback command repeated the already documented `$variable:` parsing mistake and stopped before writing. Prevention: fallback diagnostics use the `-f` format operator exclusively; do not interpolate variables into punctuation-adjacent diagnostic strings.
- 2026-07-17 P42.5 PowerShell replacement-shape mistake record: a fallback represented replacement pairs as nested arrays, which PowerShell flattened and reduced the first anchor to one character. Prevention: controlled replacement transactions use named `Old` and `New` objects and assert each complete anchor count before writing.
- 2026-07-17 P42.5 automation-result parsing mistake record: a broad `Test Failed` search treated the successful test name `FailedCompileRejectedSmoke` as a failure. Prevention: count every `Test Completed. Result={...}` record and classify failures only by a non-`Success` result field; test names are never failure evidence.
- 2026-07-17 P42.5 EOF-newline review mistake record: `SemanticCompilationFactory.cs` still lacked its final newline even after `dotnet format --verify-no-changes` passed. Prevention: submission review uses `git diff` EOF markers in addition to formatter output and fixes every `No newline at end of file` marker.
- P43.1 binding selection profiles declare class boundaries and filters; they do not enumerate runtime API implementations. Class discovery rejects unsupported signatures into a deterministic compatibility report, while explicit function selections remain strict by default.
- P43.1 exact-class discovery uses `TFieldIterator<UFunction>(Class, EFieldIterationFlags::None)` so inherited functions are not duplicated across class rules. The final accepted selections and compatibility issues are both sorted by stable textual identity.
- P43.2 keeps the eight-function `PublishDefault()` package as a compatibility entry, while custom C# builds without an explicit package now default to the verified 115-function `avidscript.engine.gameplay` package. Full-facade collision probes must pass before any expanded profile becomes a build default.
- P43.1 UE5.8 baseline is gameplay profile candidates 221, accepted 115, rejected 106; focused BindingSelection 4/4; full AvidScript automation 159/159 with zero non-success results.
- 2026-07-17 P43.1 PowerShell escaping recurrence record: an existing-file fallback again used C-style escaped quotes and stopped at parser validation before writing. Prevention: fallback transformations use single-quoted literals or symbol-boundary `IndexOf` operations; never reconstruct quoted C++ blocks with backslash escaping.
- 2026-07-17 P43.1 Unity Build naming mistake record: generic anonymous-namespace helpers named `MakeSelectionKey` collided when UE combined generator, resolver, and tests into one unity translation unit. Prevention: file-local helpers still use ownership-prefixed names that remain unique under Unity Build.
- 2026-07-17 P43.1 field-iteration mistake record: the first implementation assumed `EFieldIterationFlags::ExcludeSuper`, but UE5.8 exposes exclusion as `EFieldIterationFlags::None`; `ExcludeSuper` exists only in the legacy `EFieldIteratorFlags` namespace. Prevention: inspect the active source-engine enum before using reflection iterator flags.
- 2026-07-17 P43.1 PowerShell pipeline mistake record: a diagnostic attempted to pipe directly from a bare `foreach` statement and failed parsing. Prevention: collect statement output into `$rows` or wrap the statement in `$()` before applying a pipeline.
- UE5.8 commandlet startup currently emits 13 `LogAutomationTest: Error: Condition failed` records from UnifiedError self-tests before `Ready to start automation`; P42.5 baselines contain the same records. Classify AvidScript results only from discovered tests and `Test Completed. Result={...}` after the automation queue starts.
- 2026-07-17 P43.1 cleanup-reporting mistake record: the first temporary-script cleanup omitted `$ErrorActionPreference = 'Stop'`, so access-denied errors were non-terminating and the command falsely reported 19 removals. Prevention: cleanup commands use terminating errors, verify the exact contained paths before deletion, and assert the post-delete remaining count is zero before reporting success.
- P43.2 Gameplay packages and default packages share one `PublishGeneratedPackage` transaction. New profiles must not copy staging, byte-conflict, content-address, or race-reuse logic.
- P43.2 C# ref/out calls preserve Roslyn flow-capture address provenance. A captured ref/out parameter forwards its caller address; it must not be lowered as an address to a copied capture value.
- P43.2 UE5.8 baseline is CSharpBindingEmitter 4/4, CSharpGuest 17/17, Frontend 7/7, GuestIr 31/31, WasmBackend 11/11, Semantic 41/41, BuildIntegration 3/3, BuildPublicationContracts 3/3, and full AvidScript automation 160/160 with zero non-success results.
- 2026-07-18 P43.2 test-registration mistake record: the flow-captured ref regression method was added but the first edit did not invoke it from the custom suite `Run()`, so the unchanged 16/16 count falsely looked healthy. Prevention: every custom console-test addition must increase the expected count and direct verification must confirm the new count before broader tests.
- 2026-07-18 P43.2 PowerShell pipeline recurrence record: a log-summary command again piped directly from a bare `foreach` statement and failed parsing. Prevention: every PowerShell loop that feeds a pipeline first assigns its output to a named collection; do not type `foreach (...) { ... } |` in verification commands.
- 2026-07-18 P43.2 artifact-path mistake record: a new Phase 43 document was initially created relative to the project root instead of the nested plugin repository. Prevention: new plugin files use a `Plugins/AvidScript/...` patch path and are immediately confirmed by plugin-level `git status --short`.
- 2026-07-18 P43.2 final-format SDK recurrence record: the first final format command incorrectly used the UE5.8-bundled .NET 10 host even though `global.json` pins 8.0.416 and the repository already documents the user-local host. Prevention: before every repository .NET command, invoke `<UserProfile>/.dotnet/dotnet.exe --version`, require exactly 8.0.416, and only then run the command with the complete task-local environment.
- P43.3A Semantic schema v5 owns export-root call reachability. Guest IR lowers only reachable functions/imports; schema v4 artifacts retain all-callable compatibility.
- P43.3A complete binding packages are authorization ceilings, not mandatory import sets. Every observed dynamic import must exist in the verified package, while unused package imports remain absent from Guest IR and WASM.
- P43.3A build provenance records used binding stable ID, ordinal, module, name, and signature. P43.3B must consume this structure instead of scanning C# or generated facade text.
- 2026-07-18 P43.3A generated-patch count mistake record: a manually assembled unified diff declared one extra line in a hunk and `git apply --check` rejected it before any source write. Prevention: generated patch transactions always use `git apply --recount --check --whitespace=error-all` before the matching `git apply --recount`.
- 2026-07-18 P43.3A PowerShell punctuation recurrence record: the first parser-validation wrapper interpolated `$file:` and failed before parsing target scripts despite this syntax already being documented. Prevention: all diagnostic wrappers use the `-f` format operator; punctuation-adjacent interpolation is forbidden even in one-off verification commands.
- 2026-07-18 P43.3A UE container API mistake record: a test assumed `CountByPredicate` was a `TArray` member and failed the real UE5.8 compile. Prevention: inspect the active UE container API before using convenience algorithms; use an explicit loop when it avoids a new dependency and keeps a one-off test count clear.
- P43.3A UE5.8 baseline is Frontend 7/7, GuestIr 31/31, Semantic 43/43, CSharpGuest 18/18, WasmBackend 11/11, BuildIntegration 3/3, BuildPublicationContracts 3/3, GeneratedBindingLifecycle 115 authorized / 2 dynamic imports, and full AvidScript automation 160/160 with zero non-success results.
- 2026-07-18 P43.3A stale-contract test mistake record: SourceAdapterArtifactLifecycleSmoke still required `timer_cancel` merely because the sample declared `UE.CancelTimer`, so the first full reachability baseline reported 159/160 even though no lifecycle export could reach that API. Prevention: source API availability tests and emitted-manifest usage tests remain separate; manifest assertions follow semantic reachability and explicitly verify unreachable imports are absent.
- 2026-07-18 P43.3A patch-tab escape mistake record: the first follow-up unified diff contained literal `\t` text instead of tab characters and failed `git apply --check`. Prevention: nested patch generators use real whitespace, inspect the generated patch when preflight rejects valid-looking context, and never bypass the preflight.
- 2026-07-18 P43.3A automation-process mistake record: the command wrapper returned while the spawned UnrealEditor-Cmd process still had only 72/160 results in its log. Prevention: full automation verification waits for the exact process to exit and requires both 160 completed result records and the Queue Empty/TestExit markers before reporting a baseline.
- 2026-07-18 P43.3B zero-context patch mistake record: a minimal unified diff used a zero-context hunk but the first preflight omitted `--unidiff-zero`, so valid exact content was reported as not applicable. Prevention: prefer at least one stable context line; when a truly zero-context hunk is required, use `--unidiff-zero` consistently for both check and apply.
- 2026-07-18 P43.3B null-trim inspection mistake record: worktree detection called `.Trim()` directly on the empty output of `git rev-parse --show-superproject-working-tree`, causing a non-terminating PowerShell error. Prevention: capture optional command output first and normalize null to an empty string before invoking string methods.
- 2026-07-18 P43.3B contract-suite exit-code recurrence: a wrapper exited with `$LASTEXITCODE` after BuildPublicationContracts had passed 3/3, so the last intentionally failing fixture leaked exit code 1 despite the earlier P42.5 warning. Prevention: contract suites that invoke expected failures end with explicit `exit 0` after all assertions, and verification uses that process exit code.
- 2026-07-18 P43.3B sandbox temp-root mistake record: a direct test run assumed `C:\tmp` was writable, but the active Windows sandbox denied creation before the suite started. Prevention: repository .NET task homes stay under the plugin-owned `Saved/AvidScriptFrontendDotNet/DotNetEnv` root unless a candidate path has first been proven writable.
- 2026-07-18 P43.3B verification-command recurrence record: one search again passed a literal Windows wildcard path to `rg`, and one report summary again piped directly from a bare `foreach`, repeating documented P42.2/P43.1 failures. Prevention: `rg` scopes use literal directories plus `-g`, and every loop-to-pipeline command first assigns loop output to a named collection.
- P43.3B dual-package rule: the complete generated package is the C# authorization ceiling, while only the stable-ID runtime slice enters the final script manifest and immutable Runtime package. Automatic custom builds use bootstrap plus final invocation; explicit packages remain single-pass; zero dynamic imports omit `binding_package` entirely.
- P43.3B ownership rule: BuildService owns defaults and package strategy, BuildInvoker owns exactly one PowerShell invocation and artifact contract, and BindingSliceService reuses the reflection descriptor generator plus content-addressed publisher. Do not move process execution, C# parsing, descriptor serialization, or package policy across these boundaries.
- P43.3B UE5.8 baseline is Frontend 7/7, GuestIr 31/31, Semantic 43/43, CSharpGuest 18/18, WasmBackend 11/11, BuildIntegration 8/8, BuildPublicationContracts 3/3, PowerShell parser 4/4, CSharpBindingSlice 1/1, CSharpBuildService 4/4, GeneratedCSharpLifecycle 1/1, authorization 115 / runtime 2 / WASM dynamic imports 2, and full AvidScript automation 163/163 with zero non-success results.

## Phase 43.4 Prepared Semantic Reuse Rules

- 自动自定义 C# 构建继续执行 bootstrap/final 两次 BuildOnce，但 Roslyn Frontend 与 Semantic 各只执行一次；显式 package 保持单遍 `1/1/1`。Guest IR、WASM Backend、runtime slice 子集校验和正式发布仍在 final 重跑。
- `build_reuse.prepared_report_file` 与 SHA 是一次 BuildService 调用内的审计来源。final report 的 `artifacts.*` 和 manifest 只引用 final output；Runtime 不得依赖结束后删除的 bootstrap 路径。
- Prepared Semantic helper 只验证 report/source/authorization/artifact provenance 并事务发布 Frontend/Semantic。禁止启动 dotnet/powershell、扫描 C# 源文本或决定 binding package 策略。
- Prepared 验证失败必须返回 `prepared_semantic_invalid` 和 `ASBI4401` 到 `ASBI4404`，不得静默降级，也不得留下可加载 manifest/WASM。
- Prepared artifact 路径必须同时通过词法包含和物理路径逐段校验；prepared root 下任何目录联接、符号链接或其他 reparse point 一律拒绝。
- Frontend/Semantic artifact 的 `source.source_id` 必须与 prepared report 的 `source.file` 完全一致，源码哈希相同不能替代源身份校验。
- 双文件发布在两个 destination 都替换成功后进入提交态。提交前失败必须反向回滚；提交后的备份清理失败只能告警并保留新产物，不得触发回滚。
- P43.4 UE5.8 baseline is Frontend 7/7, GuestIr 31/31, Semantic 43/43, CSharpGuest 18/18, WasmBackend 11/11, PreparedSemanticContracts 11/11, BuildIntegration 9/9, BuildPublicationContracts 4/4, PowerShell parser 14/14, architecture passed, CSharpBuildService 4/4, GeneratedCSharpLifecycle 1/1, authorization 115 / runtime 2 / WASM dynamic imports 2, and full AvidScript automation 163/163 with zero non-success results.
- 2026-07-18 P43.4 delegation mistake record: Task 1 的关键阻塞实现交给 worker 后，主流程多次等待而没有推进非重叠工作。Prevention：主流程始终拥有下一步立即依赖的 blocker；只把独立 sidecar 工作交给 subagent，并在其运行时推进本地非重叠任务。
- 2026-07-18 P43.4 internal-artifact mistake record: worker 曾把 `.superpowers/sdd/task-1-report.md` 纳入提交。Prevention：委派前先把 `.superpowers/` 等内部工作流产物写入 `.gitignore`，暂存后逐项审计文件列表。
- 2026-07-18 P43.4 RED-fixture mistake record: 首次 Prepared contract RED 在写 fixture 前没有创建根目录。Prevention：测试写盘前显式创建并断言 fixture root，再验证预期失败来自被测契约而不是夹具 IO。
- 2026-07-18 P43.4 diff-scope mistake record: 一次宽泛 `git diff --check` 扫到用户自有 Phase 42 文档的既有尾随空格。Prevention：工作树有受保护文件时只对本任务路径或 staged diff 运行 whitespace gate，禁止用无范围结果判断本任务质量。
- 2026-07-18 P43.4 source-engine permission mistake record: 首次 UBT 调用未直接申请源码引擎写权限，在写 Trace 备份前被 sandbox 拒绝。Prevention：`C:\UnrealEngine` 的 UBT/Editor 命令首次执行即使用已限定前缀的 escalation；失败后不通过清理 target 绕过权限问题。
- 2026-07-18 P43.4 prelaunch-fixture mistake record: 测试曾假设“普通文件作为目录”会让 UE `MakeDirectory` 返回 false，但它返回 true 并启动了 PowerShell。Prevention：启动前失败使用平台明确拒绝的非法路径，并同时断言稳定错误类别与 `Build/Frontend/Semantic=0/0/0`，不能只看总结果失败。
- 2026-07-18 P43.4 checklist-verification mistake record: 收尾命令按字面搜索 `- [ ]`，误把计划开头的 checkbox 语法说明当成未完成任务。Prevention：计划完成度检查只匹配行首真实 checkbox，使用 `^\s*-\s+\[ \]`，并在失败时先读回命中行再判断阶段状态。
- 2026-07-18 P43.4 containment security mistake record: prepared artifact 首版只用 `GetFullPath` 与字符串前缀判断，目录联接可把词法根目录内路径指向外部。Prevention：不可信 artifact 路径必须逐段拒绝 reparse point，并用真实 junction fixture 覆盖。
- 2026-07-18 P43.4 source-identity mistake record: 首版只核对源码 SHA-256，遗漏 Frontend/Semantic `source_id`，同内容异路径可被误接纳。Prevention：provenance 校验同时固定内容哈希和规范化源身份，并保留同内容异路径回归用例。
- 2026-07-18 P43.4 transaction-state mistake record: 双文件发布把旧备份删除放在提交事务内，第二次删除失败会错误回滚已发布的新组合。Prevention：事务必须显式区分提交前回滚与提交后尽力清理，并注入第二个备份清理故障验证。
- 2026-07-18 P43.4 residual-regex mistake record: 回滚测试使用 `\.(tmp|bak)\.`，没有匹配真实以 `.tmp/.bak` 结尾的事务文件。Prevention：夹具必须使用 `\.(tmp|bak)$` 并验证故障注入确实到达预期调用次数。
- 2026-07-18 P43.4 junction-fixture cleanup mistake record: 首次用 Windows PowerShell `Remove-Item` 删除测试 junction 触发内部 `NullReferenceException`。Prevention：只删除已验证的精确 junction 路径，并使用 `[System.IO.Directory]::Delete`，不得递归删除 target。
- 2026-07-18 P43.4 patch-context mistake record: 一次补丁凭记忆使用 `$RootPrefix`，实际代码变量是 `$ContainedPrefix`，预检正确拒绝且未写盘。Prevention：生成补丁前读回精确目标块，并继续执行 `git apply --check`。
- 2026-07-18 P43.4 Windows-PowerShell compatibility mistake record: 首版路径加固使用 `Path.GetRelativePath`，但 Windows PowerShell 5.1 的 .NET Framework 不提供该 API。Prevention：Build helper 修改后必须由 `powershell.exe -NoProfile` 实跑；已完成词法包含时用兼容的前缀切片计算相对段。
- 2026-07-18 P43.4 `rg` wildcard recurrence record: .NET 验证前再次把 Windows 通配符路径 `Tools\*Tests\*.csproj` 直接交给 `rg`，命令被系统拒绝且未写盘。Prevention：搜索命令只能传字面目录，并通过 `-g "*Tests.csproj"` 过滤；该规则同样适用于一次性核查命令。
- 2026-07-18 P43.4 long-process session mistake record: 首次 UE 聚焦测试把大量 stdout 与 session id 一起输出，结果 session id 被截断，主流程只看到测试启动片段而无法继续轮询。Prevention：长任务首次调用只输出 `session_id/exit_code/output_tail` 元数据，立即保存 session id，并持续用 `write_stdin` 轮询到退出后再解析完整日志。
- 2026-07-18 P43.4 rollback-deletion mistake record: 双文件发布回滚曾用 `Remove-Item -ErrorAction SilentlyContinue` 删除已发布 destination；删除失败会被吞掉，随后清理未发布 staged 文件，留下半组合且无恢复材料。Prevention：提交前回滚的每个删除/恢复操作都必须计入 rollback failure；回滚不完整时保留 `.tmp/.bak` 并通过故障注入验证恢复材料。

## Phase 43.5 Content-Addressed Semantic Cache Rules

- P43.5 cache root 固定在项目 `Saved/AvidScript/CSharpSemanticCache/v1`；条目使用 64 位小写 SHA-256 内容地址和首两位分片，不得写入插件源码、Content、bootstrap Intermediate 或最终可加载 artifact 目录。
- Cache key 必须覆盖 source/project/reference source 实际哈希、authorization identity、configuration、net8.0、SDK 8.0.416 和 Frontend/Semantic 源码闭包指纹；禁止时间、机器名、绝对 cache root 或 output root 进入键。
- Semantic Cache helper 只拥有 key、指纹、entry lookup/publication 和损坏隔离。禁止解析用户 C# 文本、启动工具进程、解释 Roslyn 语义或决定 binding package 策略。
- 命中后仍须执行 Guest IR、WASM、authorization/runtime 子集校验和正式发布；Runtime manifest 不得引用 cache entry。
- Build report 的 `tool_invocations` 是 Editor 调用计数唯一事实来源；BuildInvoker 不得再根据 prepared 参数推断 Frontend/Semantic 次数。
- 2026-07-18 P43.5 verification-regex mistake record: 首次 Markdown 尾随空格扫描在 PowerShell 单引号 regex 中写了 `[ `t]`，把反引号和字母 `t` 当成普通字符并产生大量假阳性。Prevention：regex tab 使用 `[ \t]`，或在双引号表达式中使用实际 tab；核查失败后先读回命中行，未确认前不得改文件。
- 2026-07-18 P43.5 cache-fixture root mistake record: Task 1 首次 GREEN 把 cache fixture 放在插件 `Saved`，不属于设计规定的项目 `<Project>/Saved`，正确触发 `ASBI4503`。Prevention：cache fixture 与正式 cache 使用项目级 Saved 专属子目录；插件 Saved 只保存被忽略的 source/toolchain 测试输入。
- 2026-07-18 P43.5 patch-terminator mistake record: 一次生成的 patch 文件把 `*** End Patch` 误加 diff 前缀，apply_patch 校验拒绝且未写盘。Prevention：外层 patch 结束标记必须独占最后一行，创建后仍执行 `git apply --check` 再写生产文件。
- 2026-07-18 P43.5 mixed-case extension mistake record: the first semantic-cache key helper compared `.cs` and `.csproj` case-sensitively, although Windows MSBuild accepts mixed-case source extensions. Prevention: fingerprint extension checks must follow Windows casing and contract-test `.CS` invalidation.
- 2026-07-18 P43.5 toolchain-containment mistake record: the first toolchain closure used lexical containment only, allowing a reparse-point source to escape the plugin root. Prevention: every fingerprinted toolchain file must pass `Test-AvidScriptBindingPathContained`, with a real junction contract.
- 2026-07-18 P43.5 cache-ownership mistake record: the first cache-root contract accepted every project `Saved` descendant, including final artifact ownership. Prevention: cache roots must be physically contained by the project `Saved/AvidScript` namespace; production remains fixed at `Saved/AvidScript/CSharpSemanticCache/v1`.
- 2026-07-18 P43.5 reparse-fixture mistake record: the first toolchain junction test put a junction below a source root, but Windows PowerShell did not recurse into it, so no escaped file entered the closure. Prevention: reparse contracts must prove the escaped file was enumerated; use the source root itself as the junction when validating closure containment.
- 2026-07-18 P43.5 parser-command quoting mistake record: an ad hoc nested `powershell -Command` used outer double quotes, so the parent shell expanded inner `$` variables and the verification command produced its own ParserError. Prevention: preserve nested PowerShell source with outer single quotes or run a checked script file; distinguish command-parser failures from repository parser failures.
- 2026-07-18 P43.5 entry-helper syntax mistake record: the first Task 2 entry helper omitted the closing parenthesis around a three-part context validation expression. Prevention: run the PowerShell parser immediately after applying each generated helper hunk and before executing behavioral contracts.
- 2026-07-18 P43.5 cache-temp path-budget mistake record: the first entry publisher repeated the 64-character cache key in validation and staging directory names, pushing atomic temp files beyond the Windows path budget in the real project location. Prevention: content identity belongs only in the final entry path; transaction directories use short role prefixes plus PID/GUID and must be exercised under the actual project path depth.
- 2026-07-18 P43.5 artifact-escape fixture mistake record: the first path-escape contract copied an artifact before creating its outside parent directory, so the fixture failed before exercising cache validation. Prevention: every negative fixture must establish its complete invalid state first and assert the intended diagnostic rather than accepting setup exceptions.
- 2026-07-18 P43.5 generated-test patch mistake record: the first extended entry-contract patch accidentally added a leading space to a removed line, so `git apply --check` rejected the behavioral hunk. Prevention: inspect generated patch failures at the exact rejected line and never bypass the check; keep large test replacements in smaller hunks.
- 2026-07-18 P43.5 zero-context placement mistake record: a zero-context patch inserted `$CanIsolate = $true` between a backtick-continued command name and its parameters. The script parsed but passed the assignment result as command input, producing a misleading `= True` runtime error. Prevention: after zero-context edits, read the surrounding command block as well as running the parser; prefer anchored context for statement-order changes.
- 2026-07-18 P43.5 publication-provenance mistake record: Task 2 初版发布器只信任调用方传入的 cache context，可能把其他 project/configuration/toolchain 产生的合法 artifact 盖上当前 key。Prevention：源 build report 必须携带 key/toolchain fingerprint，发布时从当前 source/project/reference/authorization/configuration/toolchain 全量重算并与 report、context 三方一致后才允许写 entry。
- 2026-07-18 P43.5 default-authorization coercion mistake record: Task 2 初版用 PowerShell 类型强转检查默认授权，缺字段对象和标量可被强制转换成空值后通过。Prevention：先要求完整属性集合与 bool/string/integer/array 精确类型，再检查 required=false 和空 identity。
- 2026-07-18 P43.5 cache-destination ownership mistake record: Task 2 初版 import 未拒绝指向 cache namespace 的 destination，命中复制可覆盖不可变 entry。Prevention：复制前对两个 destination 做项目物理包含、互异和 cache root 不相交校验，失败时不得进入损坏隔离。
- 2026-07-18 P43.5 corrupt-isolation race mistake record: Task 2 初版 lookup、损坏隔离和 publisher winner 选择之间没有每-key 跨进程序列化，存在 ABA 竞态。Prevention：import 与 publish 共享 cache-root Locks 下的独占 FileStream，并在验证、隔离、原子发布及最终 winner 验证完成后才释放。
- 2026-07-18 P43.5 staging-validation mistake record: Task 2 初版在完整 Prepared Semantic 校验之前就把 staging 目录移动到最终 entry，且部分 staging/shard IO 会泄漏原始异常。Prevention：暂存副本先走正式 provenance/model/hash 门禁，所有非结构化发布 IO 统一映射 ASBI4504，清理故障保持 ASBI4505。
- 2026-07-18 P43.5 validation-namespace mistake record: 加入 destination ownership 防护后，发布器原有 cache-root 内部 validation destination 会被自身规则正确拒绝。Prevention：可变验证事务固定在项目 Intermediate/AvidScript，cache root 只容纳 locks、短 staging 与不可变内容地址 entry。
- 2026-07-18 P43.5 patch-hunk count recurrence record: 两个生成补丁再次声明了错误 hunk 行数，普通 `git apply --check` 报 corrupt patch。Prevention：所有生成补丁从第一次预检开始就使用 `git apply --recount --check --whitespace=error-all`，零上下文补丁同时固定 `--unidiff-zero`，不得先尝试不带 recount 的命令。
- 2026-07-19 P43.5 external-output compatibility mistake record: destination 防护初版要求输出物理位于 Unreal ProjectRoot，破坏了构建 API 原有的任意绝对 OutputRoot 契约。Prevention：cache import 只拒绝目标互相覆盖、路径 reparse 和 cache namespace 重叠；不得把项目根所有权误加到普通构建产物。
- 2026-07-19 P43.5 lock-file reparse mistake record: per-key 锁初版只检查 `CacheRoot/Locks` 目录，未检查可预测的 `<key>.lock` 自身，文件 symlink 可把独占打开引向外部。Prevention：创建父目录后、打开句柄前，对完整 lock path 同时执行 cache 物理包含和逐段 reparse 检查。
- 2026-07-19 P43.5 staged-report surrogate mistake record: full-staging 校验初版从内存 EntryReport 另行序列化验证副本，没有重读真正写盘的 `entry.csharp.report.json`。Prevention：写盘后重读真实 bytes，核对完整 identity/path/hash，再从该实读对象生成仅替换 staging 路径的 Prepared 验证投影。
- 2026-07-19 P43.5 transient-scan drift mistake record: validation transaction 迁到 Project `Intermediate/AvidScript` 后，cleanup contract 仍只扫描 CacheRoot，无法发现注入故障留下的真实 validation 目录。Prevention：每次迁移事务 ownership root 时同步更新清理、残留扫描和故障注入断言的全部根集合。
- 2026-07-19 P43.5 symlink-privilege fixture mistake record: lock escape 回归用例直接创建 Windows file symbolic link，在普通权限环境中因需要管理员权限而停在夹具阶段。Prevention：有权限时使用真实 file symlink；无权限时在同一精确 lock path 使用真实 directory junction，并分别用 File.Delete/Directory.Delete 只删除链接本身。
- 2026-07-19 P43.5 external-root ACL fixture mistake record: external output hit 用例假设 `C:/tmp` 可写，但当前沙箱 ACL 在复制阶段拒绝访问。Prevention：外部目标边界放行必须无条件验证；完整字节复制只在写入探针成功时执行，不把宿主 ACL 失败误判为 cache contract 失败。
- 2026-07-19 P43.5 `rg` wildcard recurrence record: 调试路径 helper 时再次把 `Build/*.ps1` 作为 Windows 字面路径交给 `rg`，重复 OS error 123。Prevention：所有搜索只传字面目录并使用 `-g "*.ps1"` 过滤，临时诊断也不例外。
- P43.5 build report `semantic_cache` identity is schema-versioned and records the producing key plus toolchain fingerprint before publication. Cache provenance never enters the runtime manifest; `tool_invocations` remains the only invocation-count source consumed by Editor.
- `-DisableSemanticCache` is a supported diagnostic path: lookup stays disabled, key/fingerprint remain empty, no entry is published, and the normal Frontend/Semantic/Guest IR/WASM chain still runs with counts `1/1/1/1`.
- 2026-07-19 P43.5 report-schema mistake record: Task 3 首次接入 hardened publisher 时写出了 key 与 toolchain fingerprint，却遗漏 `semantic_cache.schema_version=1`，导致所有身份哈希都相同仍被正确拒绝。Prevention：每个新增 report 子契约先固定 schema，再用集成测试同时断言 schema、key 和 fingerprint 三元组。
- 2026-07-19 P43.5 Editor cache-fixture namespace recurrence: Task 4 首版 UE fixture 再次把自定义 cache root 放到项目 `Saved/AvidScriptTests`，被正式 ownership contract 正确标记为 `rejected`。Prevention：所有 UE cache fixture 固定在项目 `Saved/AvidScript/Tests/<case>/CSharpSemanticCache/v1`，输出目录与 cache namespace 分离。
- 2026-07-19 UE5.8 UBT runtime-entry mistake record: 直接启动 `UnrealBuildTool.exe` 选择了系统 .NET，缺少 UBT 所需的 .NET 10。Prevention：源码版 UE5.8 增量编译固定用引擎 `ThirdParty/DotNet/10.0/win-x64/dotnet.exe` 执行 `UnrealBuildTool.dll`，并保持 `-MaxParallelActions=1 -NoUBA`，禁止清理 Editor Target。
- 2026-07-19 verification-scope recurrence: Task 4 核查再次向 `rg` 传入 Windows wildcard，并让宽泛 `git diff --check` 扫到受保护用户文档。Prevention：`rg` 只接收字面目录配合 `-g`；whitespace gate 必须显式列出本任务文件，禁止在脏工作树运行无范围 gate。
- 2026-07-19 nested-command string mistake record: 自动化日志解析首次把正则反斜杠放进普通 JavaScript 字符串，工具调用在进入 PowerShell 前失败。Prevention：含 PowerShell 路径或正则的嵌套命令统一使用 `String.raw`，并先构造命名 `$rows` 再进入 pipeline。
- 2026-07-19 P43.5 staged-report complete-shape review finding: Task 2 hardened 版重读真实 staged report 后只核对选择字段，合法 JSON 仍可篡改 `module_id` 或注入 loadable/unknown 字段并到达 atomic move。Prevention：移动前对重读对象与完整预期 report 做 canonical JSON 哈希等价校验，再执行路径、artifact byte 和 Prepared 验证；回归测试注入 well-formed tampering 并断言 move 调用次数为零。
- P43.5 Editor report rule: 旧 report 可由 reader 读取，但 BuildInvoker 只接受完整有效的 `tool_invocations` 与 `semantic_cache` schema；启动前失败计数保持 0，启动后的成功或结构化失败只透传 report 事实，禁止参数推断。
- P43.5 automatic-build audit rule: BuildService 对 bootstrap/final 的 Build、Frontend、Semantic、Guest IR、WASM Backend 五类次数求和；final prepared lookup 为 disabled 时保留 bootstrap 的 cache miss/hit/rejected 审计字段。
- P43.5 immutable publication rule: staged entry report atomic move 前必须与完整预期 report canonical JSON 哈希相等，并通过路径、artifact byte 和 Prepared provenance 验证；Runtime manifest 永不引用 cache entry。
- P43.5 UE5.8 baseline is SemanticCacheEntry 25/25, SemanticCache 16/16, Prepared 11/11, BuildCacheIntegration 8/8, BuildIntegration 11/11, BuildPublication 4/4, Editor Report 6/6, CSharpBuildService 4/4, GeneratedCSharpLifecycle 1/1, and full AvidScript automation 163/163 with zero non-success results.
- 2026-07-19 Markdown literal-marker scan mistake record: Task 4 首次用 `Contains('`n')` 检查字面换行标记，错误命中合法 inline code `` `net8.0` ``。Prevention：只匹配完整 `` `r`n ``，或位于行尾/空白边界的独立 `` `n ``，不得把 Markdown 反引号后的普通字母 `n` 当成异常。
- 2026-07-19 JavaScript raw-template delimiter recurrence: 修正 Markdown 扫描时仍把 PowerShell 反引号字面量直接放进 `String.raw` 模板，JavaScript 在调用工具前终止解析。Prevention：嵌套命令中的反引号统一由 PowerShell `[char]96` 构造；`String.raw` 只解决反斜杠，不保护模板分隔符本身。
- 2026-07-18 P43.5 cold-artifact acceptance review finding: 生命周期测试在 warm build 覆盖同一输出目录前只检查 cold 计数，没有独立加载 cold manifest/WASM/package，也只断言 authorization 文件存在却声称验证 115 bindings。Prevention：cold 产物必须在任何后续构建前完成 package 结构、115/2/2、真实 WAMR BeginPlay/Tick 验收；warm 产物重复同一验收。
- 2026-07-18 P43.5 pre-Frontend count review finding: 结构化失败 fixture 只有四类工具均为 1 的情况，不能防止 Invoker 重新按进程启动推断 Frontend 已调用。Prevention：固定 `Build=1 / Frontend=0 / Semantic=0 / GuestIR=0 / WASM=0` 的 Frontend 前失败 report contract，并要求计数逐项来自 report。
- 2026-07-18 tool-script reserved-word mistake record: nested JavaScript used `package` as a variable name and failed strict-mode parsing before reading a binding manifest. Prevention：工具编排变量使用 `pkgResult` 等非保留标识符，并把 parser failure 与目标命令失败分开记录。
- 2026-07-18 multi-file plan-patch recurrence: AGENTS insertion context was valid, but a line-ending-sensitive Phase 43 hunk made the combined patch preflight fail atomically. Prevention：对处于 line-ending-only dirty 状态的 Markdown 使用精确行号 zero-context hunk，并把独立文件拆成独立 checked patches。

## Phase 44 Project C# Workspace Rules

- Project-authored C# source, project, profile, and `global.json` live under `<Project>/Scripts/AvidScript`; create-or-refresh preserves them unless overwrite is explicitly requested. Generated IDE facades live under `Intermediate/AvidScript/CSharpWorkspace`, and loadable artifacts live under project Saved.
- WorkspaceService owns template materialization and IDE facade refresh only. It must not invoke builds, parse reflection descriptors, bind Actors, or introduce Runtime dependencies on Editor/profile/workspace types.
- 2026-07-18 P44.1 nested JavaScript path recurrence: ordinary JavaScript command strings containing Windows backslashes failed before the tool call. Prevention: construct every nested Windows path or regex command with `String.raw`; construct literal PowerShell backticks with `[char]96` instead of placing them inside JavaScript template literals.
- 2026-07-18 P44.1 verification-pattern mistake record: a Markdown scan included `\u0000` in an `rg` pattern, which ripgrep correctly rejected because binary NUL cannot be matched in normal text mode. Prevention: validate NUL/control bytes through a byte-aware PowerShell read and keep text placeholder searches separate.
- 2026-07-18 P44.1 diagnostic-regex mistake record: an ad hoc `rg` alternation for `IPluginManager` contained an unclosed escaped group. Prevention: use fixed-string search for literal C++ symbols unless regex structure is required, and treat diagnostic parser failures as command errors rather than repository evidence.
- 2026-07-18 P44.1 path-containment mistake record: normalized project root retained a trailing slash, then containment appended another slash and rejected every valid project-owned path. Prevention: collapse relative segments and duplicate separators, trim non-volume-root trailing separators, and exercise both inside/outside roots before any write.
- 2026-07-18 P44.1 UE file API mistake record: atomic publication treated `IFileManager::Move` as a copy result code, so successful `true` was classified as failure. Prevention: inspect the active UE5.8 declaration and keep `Move` boolean handling distinct from APIs that return `COPY_OK`.
- P44.1 UE5.8 baseline is WorkspaceService 2/2, CSharpBuildService 4/4, GeneratedCSharpLifecycle 1/1 with cold and warm authorization/runtime/dynamic import counts 115/2/2, and full AvidScript automation 165/165 with zero non-success results.
- 2026-07-18 P44.1 tail-context mistake record: a baseline patch anchored at the three lines before the actual AGENTS tail and omitted the already appended plan-patch rule, so preflight rejected it. Prevention：read the literal tail immediately before every append and prefer a checked zero-context EOF insertion when concurrent append-only rules are present.
- 2026-07-18 P44.1 plan-patch context mistake record: a broad Markdown hunk anchored above the actual Task 1 lines did not apply after line-ending/stat drift. Prevention: read exact numbered target lines first and use small context hunks; when line-ending-only status is present, use the same checked `--unidiff-zero` mode for both preflight and apply.
- P44.2 Editor command 只编排 WorkspaceService 与既有 profile Build/Bind 链；菜单 handler 不复制 workspace、编译、切片、发布或 Actor 绑定逻辑。
- 2026-07-19 P44.2 生成补丁 hunk 行号错误：首次 module header 补丁的上下文起始位置不正确，预检即失败。Prevention：补丁从实际 `git diff`/编号读回结果生成，并始终先执行 `git apply --recount --check --whitespace=error-all`。
- 2026-07-19 P44.2 zero-context 声明边界错误：按裸行号插入 API，曾把 public 方法放入 `private` 并把 handler 放到 class 外。Prevention：多行声明、class 访问区和宏附近禁止裸行号插入；使用稳定上下文，并在 apply 后读回完整 owner block。
- 2026-07-19 P44.2 apply_patch workspace helper failure：直接修改已有 production header 时 sandbox helper 失败。Prevention：通过 `apply_patch` 创建 ignored patch artifact，再用 checked `git apply` 写入目标文件；不得改用未审计的 shell 重写。
- 2026-07-19 P44.2 patch artifact root assumption：`apply_patch` 创建的 `Saved/CodexPatch` 位于当前 project workspace root，而首次 `git apply` 从 nested plugin repo 查找相对路径，因而找不到文件。Prevention：生成后先读取 patch artifact 的绝对路径，nested repo 的 apply 命令始终使用该绝对路径。
- 2026-07-19 P44.2 RegisterMenus whitespace mismatch：组合补丁假定一行空白，源码实际有两行，导致整个 hunk 预检失败。Prevention：拆分独立 hunk，并以立即读回的精确上下文为锚点。
- 2026-07-19 P44.2 presenter zero-context placement：workspace presenter 曾插入 profile presenter 的多行函数签名内部。Prevention：函数级新增必须锚定完整相邻签名或闭合花括号，apply 后读回前后两个完整函数。
- 2026-07-19 P44.2 automation macro placement：三条 presentation 断言曾插入 `IMPLEMENT_SIMPLE_AUTOMATION_TEST` 多行宏参数内部。Prevention：测试断言只锚定 `RunTest` 函数体内的稳定语句，宏与函数体必须一起读回。
- P44.2 UE5.8 baseline is WorkspaceBuildAndBind 1/1, Presentation 9/9, and SampleCommandConfig 1/1; the formal command produced workspace source/project/profile/facade, report, manifest, WASM, runtime package, and an Actor-bound component.
- P44.3 runtime Bind/BeginPlay rule：在已经运行的 world 上创建 `UAvidScriptComponent` 时，必须先写入 manifest path，再调用 `RegisterComponent()`；注册会立即进入 BeginPlay，顺序反转会加载 embedded smoke。
- P44.3 C# const lowering rule：带 `SemanticConstant` 的 field reference 直接生成 typed Guest constant，不得读取零初始化 global；mutable/static readonly field 才走 projected global state。
- 2026-07-19 P44.3 manifest assertion overreach：首版测试把“不引用 generated facade 路径”错误收紧为“不出现 facade 文件名”，误伤合法 provenance 文本。Prevention：安全断言固定 ownership/path 泄漏边界，不把文件身份与绝对路径引用混为一谈。
- 2026-07-19 P44.3 world initialization fixture mistake：首版 world helper 在 spawn Actor/Component 前执行 `InitializeActorsForPlay`，后注册组件没有收到预期 BeginPlay。Prevention：生命周期 fixture 复用仓库已验证的 world 启动与动态注册顺序，并从 module id/RuntimeStats 证明实际分发。
- 2026-07-19 P44.3 delayed BeginPlay assumption：第一次修正仍假设 synthetic Game world 会在稍后 `World->BeginPlay()` 时分发到已动态注册组件，结果 RuntimeStats 为空。Prevention：运行中绑定场景先启动 world，再 spawn Actor 并注册 Component；禁止手工调用 Component BeginPlay 伪造证据。
- 2026-07-19 P44.3 binding registration defect：`ComponentBindingService` 曾先注册新组件、后写 manifest，运行中绑定稳定加载了 `embedded_smoke`。Prevention：创建、配置、注册是单一有序事务，端到端测试同时断言 `ModuleId=csharp_project_gameplay` 与实际 Actor 行为。
- 2026-07-19 P44.3 const-field compiler defect：Roslyn 已输出 `float32=90`，Guest lowering 却无条件生成 `global_load`，使旋转速度变成零。Prevention：保留 typed constant provenance，增加 const field 回归，并用真实 Tick 行为验收。
- 2026-07-19 P44.3 diagnostic command mistakes：一次 `rg` 组合 regex 括号未闭合；一次 PowerShell 把自动变量 `$Matches` 当普通 list；一次从 nested plugin cwd 读取 project Saved 相对路径。Prevention：符号搜索优先 `-F`，PowerShell 临时集合使用非自动变量名，跨 repo artifact 使用绝对路径。
- 2026-07-19 P44.3 documentation tool-string recurrence：中文文档 patch 把 Markdown backtick 直接放进 JavaScript template literal，调用前 SyntaxError。Prevention：嵌套 patch 中的 backtick 统一使用占位符并由 `[char]96`/`String.fromCharCode(96)` 替换。
- P44.3 baseline is CSharpGuest 19/19, ComponentBinding 2/2, ProjectCSharpGameplayWorkspace 1/1, cold/warm tool counts `2/1/1/2/2` and `2/0/0/2/2`, authorization/runtime/dynamic imports `115/3/3`, with real Component/WAMR BeginPlay scale and Tick yaw behavior.
- 2026-07-19 P44.3 backtick-prevention recurrence：记录 template-literal 规则后，紧接着的 implementation-plan patch 又直接包含 Markdown backtick 并再次触发 SyntaxError。Prevention：任何 Markdown patch 在构造 JavaScript 字符串前先默认启用占位符转换，不再靠目视判断内容是否含 backtick。
- 2026-07-19 P44.3 plugin-document path recurrence：新中文文档再次按 project workspace root 创建到顶层 Docs，而非 nested Plugins/AvidScript repo。Prevention：新增插件文件的 apply_patch 路径必须以 Plugins/AvidScript 开头，创建后第一条命令固定执行 plugin-level git status 并核对目标绝对路径。
- 2026-07-19 P44.3 PowerShell-continuation backtick recurrence：计划修正 patch 已替换 Markdown backtick，却遗漏新增 git add 命令中的 PowerShell continuation backtick，第三次在工具调用前 SyntaxError。Prevention：patch source 中禁止任何直接 backtick；多行命令优先改成单行，否则所有 backtick 都必须经过占位符。
- 2026-07-19 P44.3 plan context mismatch：准确性修正使用普通上下文 hunk，在 line-ending-sensitive dirty Markdown 上预检失败。Prevention：此类已知文件先读精确编号行，并从第一次预检起对单行替换使用 checked --unidiff-zero；不重复尝试宽上下文。
- P44.4 live reload rule：`UAvidScriptComponent::ReloadConfiguredScript` 只通过 `FAvidScriptRuntimeSession::ReloadModule` 激活候选；manifest load 或候选 lifecycle 失败不得调用 `ReleaseRuntime`、关闭 Tick、解绑 gameplay delegates 或覆盖 active `RuntimeStats.ScriptManifestPath`。
- P44.4 Editor binding transaction rule：live Component 应用 manifest 前保存配置 path；成功后同时提交 runtime 与 path 并 dirty package，失败则恢复旧 path、返回 `reload_rejected`，且不得新增 package dirty。
- P44.4 active-path rule：`RuntimeStats.ScriptManifestPath` 表示已提交 active runtime 的 manifest；`GetScriptManifestPath()` 是可编辑候选配置。Editor transaction 在拒绝时恢复配置，直接 Component API 则允许保留候选以便修复后重试。
- 2026-07-19 P44.4 patch-protocol mistake record：首个 test patch artifact 把 tool 的 `*** End Patch` 误当成 git diff 内容并遗漏外层终止标记，修正后又使用裸 `@@` hunk header，`git apply` 报 garbage。Prevention：artifact 内容只包含标准 unified diff，每个 hunk 必须有 `@@ -old,count +new,count @@`；tool protocol terminator 始终是不带前缀的最后一行。
- 2026-07-19 P44.4 plan-patch context recurrence：已知 Phase 44 Markdown 对上下文敏感后，Task 4 checkbox 更新仍先尝试了宽上下文 patch 并在第二个 hunk 失败。Prevention：计划 checkbox、单行命令与 line-ending-sensitive Markdown 从第一次预检起固定使用逐行 checked `--unidiff-zero --recount`。
- P44.4 UE5.8 baseline is Component 7/7, RuntimeSession 2/2, ComponentBinding 3/3, incremental Runtime+Editor compile and architecture check passed; successful reload switches module and rejected manifest/BeginPlay candidate preserves the previous live Tick and clean package state.
- 2026-07-19 P44.4 verification backtick recurrence：静态门禁命令再次把 Markdown fence 的三个反引号直接写入 JavaScript raw template，导致工具调用前 SyntaxError。Prevention：任何经 `functions.exec` 发送的 PowerShell 都禁止直接出现反引号；Markdown fence 检查必须在 PowerShell 内由 `[char]96` 拼接。
- 2026-07-19 P44.4 staging backtick recurrence：精确 staged-file 集合比较仍在 JavaScript raw template 中直接使用 PowerShell newline escape，第三次于工具调用前解析失败。Prevention：跨层字符串拼接一律使用 `[Environment]::NewLine` 或 `[char]`，禁止任何 PowerShell escape backtick 进入 JavaScript template source。
- P44.5 generated-binding discovery rule：同一 content-addressed package root 可同时包含完整 authorization package 与最小 runtime slice；测试和工具必须按 required descriptor capabilities 与 import coverage 选择角色匹配的 package，禁止按最后修改时间选择“最新 package”。
- 2026-07-19 P44.5 diagnostic root-depth mistake record：首次诊断从 plugin root 使用 `..\..\..` 推导 project Saved，实际落到 `Unreal Projects\Saved`。Prevention：artifact discovery 前显式解析并输出 plugin root 与 project root，跨 repo 路径统一使用已验证的绝对路径；本项目 plugin 到 project root 是 `..\..`。
- 2026-07-19 P44.5 PowerShell patch backtick recurrence：BuildIntegration 修正 patch 再次把 continuation backtick 直接放入 JavaScript template，导致工具调用前解析失败。Prevention：任何修改 `.ps1` 的 patch 默认启用占位符替换，patch source 中不得出现直接 backtick。
- P44.5 UE5.8 baseline is Frontend 7/7, Semantic 43/43, GuestIr 31/31, CSharpGuest 19/19, WasmBackend 11/11, SemanticCache key/entry/prepared/integration 16/16, 25/25, 11/11, 8/8, BuildIntegration 11/11, BuildPublication 4/4, parser 18/18, architecture passed, focused UE automation 15/15, and full AvidScript automation 170/170 with zero non-success results.
- Phase 44 closeout rule：当前 C#+WASM 闭环可承载 Actor lifecycle 游戏逻辑与显式事务 reload；自动文件监听、guest state migration、host-write rollback、debug mapping、Cook/Shipping 与移动端仍属于后续阶段，文档和对外状态不得把这些能力表述为已完成。

## Phase 45 C# Auto Live Reload Rules
- 2026-07-19 P45.1 read-command chaining mistake record：Task 1 检查时把 `git diff --stat` 与 `git status --short` 用分号串入同一个 shell 调用，违反独立只读命令并行执行规范。Prevention：多个无依赖读取必须作为独立 `exec_command` 调用交给统一并行编排，不使用 shell separator 拼接。
- 2026-07-19 P45.1 UBT source-discovery false-RED mistake record：Task 2 在既有 `Private/CSharpLiveReload` 与 `Private/Tests` 目录新增未跟踪 cpp 后，普通增量 UBT 返回 `Target is up to date`；`-NoUBTMakefiles` 仍复用旧 source set，而当前 UE5.8 分支并未解析 `-gather`，它首次看似生效只是命令行变化碰巧触发 makefile 失效。Prevention：仅在新增 cpp 未被 action graph 发现时，先解析并验证项目内精确目标 `Intermediate/Build/Win64/x64/AvidTPSTemplateEditor/Development/Makefile.bin`，只删除该单一缓存后执行普通增量 UBT；禁止清理 Editor target，并要求日志出现新 cpp compile action。
- P45.1 coordinator/build-executor rule：文件变化调度与正式 Profile/Build/Binding 分离；executor 始终接收显式 Actor，阶段 category 与底层 cause category 分字段保存，任一阶段失败在下一阶段前短路。
- 2026-07-19 P45.1 Unity header-name collision mistake record：公开 watch-host 接口与 private adapter 最初同名 `AvidScriptEditorCSharpLiveReloadWatchHost.h`，Unity include 搜索命中 private 头自身并跳过真实接口，造成整组未声明级联错误。Prevention：同一模块的 public contract 与 private adapter 使用不同 basename；本实现固定为 `...WatchHost.h` 与 `...DirectoryWatchHost.h`，结构拆分后必须使用真实 unity-enabled UBT 验证。
- 2026-07-19 P45.1 ticker handle API mistake record：service 最初用通用 `FDelegateHandle` 保存 `FTSTicker` 注册结果，而 UE5.8 `RemoveTicker` 要求 `FTSTicker::FDelegateHandle`。Prevention：封装具体 Engine subsystem 前先读取该版本注册/注销签名，句柄类型与拥有它的 API 保持一致。
- 2026-07-19 P45.1 default dependency construction mistake record：默认 executor 捕获写成 `MakeShared<...>` 而遗漏调用括号，并假设 UE `TUniquePtr<Derived>` 可隐式转换为 `TUniquePtr<Interface>`，导致构造器不匹配。Prevention：工厂表达式在编译前读回，接口所有权边界显式构造 `TUniquePtr<Interface>`，共享实例必须写成完整 `MakeShared<Type>()`。
- 2026-07-19 P45.1 read-command chaining recurrence：Task 3 验证时再次把两个独立 `git diff` 用分号放入同一 shell 调用。Prevention：即使只是临时查看新增文件，多个读取也必须作为独立调用交给并行编排；此规则不因命令短小而放宽。
- P45.1 watch-service rule：DirectoryWatcher callback 只捕获 thread-safe pending state 并写入 MPSC queue；Ticker 在 game thread 执行 coordinator 与同步 build；Stop 先关闭 accepting，再注销 watcher、移除 ticker并使旧 generation completion 失效。
- 2026-07-19 P45.1 stale-completion review mistake record：service 最初忽略 coordinator `CompleteBuild` 的拒绝结果，构建期间若重入 Stop，旧 generation 仍会把 stopped result 覆盖为 running。Prevention：所有 generation/request completion 都是提交门禁；返回 false 后禁止写状态或解引用旧 target，并用 Stop-during-build 回归固定。
- 2026-07-19 P45.1 raw watcher callback review mistake record：DirectoryWatcher adapter 最初以 raw `this` 注册并在 Stop 无同步清空 `TFunction`，对 callback in-flight 作了未经证实的串行假设。Prevention：外部 watcher delegate 捕获独立 thread-safe shared state；Close 在锁内先拒绝并清空 callback，再注销 handle，adapter 对象不进入 delegate。
- P45.1 Task 3 baseline is incremental UE5.8 Editor build passed, architecture gate passed, service 6/6 and full CSharpLiveReload 12/12 with discovered count, zero failures, queue empty, and TestExit verified from absolute logs.
- P45.1 module lifecycle rule：`StartupModule` 只创建 live reload service，不自动编译或监听；显式 Start 必须先完成 Workspace -> Build -> Bind，并从 `BindingResult.Component->GetOwner()` 固定 Actor，后续 reload 禁止重新读取 Editor selection；`ShutdownModule` 在 ToolMenus 与模块资源释放前先 Stop service。
- 2026-07-19 P45.1 repeated-start review mistake record：重复 Start 最初复制旧 `LastResult` 后只覆盖 error 字段，导致 `StartFailed` 与旧 `Watching/BuildSucceeded/BuildFailed` 状态、旧 request/build payload 混杂。Prevention：拒绝重复 Start 时构造全新结果，只复制 active workspace/profile/target identity，显式设置 `bRunning=true` 与 `Status=StartFailed`；回归必须证明不 rebuild、不重复注册 watcher。
- P45.1 Task 4 baseline is incremental UE5.8 Editor build and architecture gate passed, fixed-target command scenario 1/1, Module 8/8, Presentation 9/9, and CSharpWorkspaceService 2/2; the command scenario covers initial real build/bind, repeated Start, selection drift, idempotent Stop, real C# build failure, binding failure, and outside-project rejection with no extra watcher.
- 2026-07-19 P45.1 debounce deadline test mistake record：真实自动重载测试用字面量 `1.70` 重建 `1.35 + 0.35` 的截止时间，浮点表示使其略早于 coordinator 保存的 deadline，坏源码构建尚未触发。Prevention：确定性时钟测试直接读取 `PendingDeadlineSeconds`，不得用十进制字面量重新计算边界；早于边界的覆盖另用明确小于 deadline 的值。
- 2026-07-19 P45.1 PowerShell foreach pipeline mistake record：日志汇总脚本把 statement-form `foreach (...) { ... }` 直接接到管道，PowerShell 报 `An empty pipe element is not allowed`。Prevention：需要继续管道时使用 `ForEach-Object`，或先把 foreach 输出赋给变量再进入 pipeline。
- 2026-07-19 P45.1 Markdown fence verification recurrence：首次按 `[Environment]::NewLine` 切分 LF 文档导致 fence 计数为 0，随后又把三个反引号直接写进 PowerShell 双引号参数而静默搜索了错误 pattern。Prevention：文本门禁用 multiline regex 直接扫描原文；fence token 必须由 `[char]96` 构造，跨层命令中禁止直接反引号。
- P45.1 final UE5.8 baseline is Frontend 7/7, Semantic 43/43, GuestIr 31/31, CSharpGuest 19/19, WasmBackend 11/11, SemanticCache key/entry/prepared/integration 16/16, 25/25, 11/11, 8/8, BuildIntegration 11/11, BuildPublication 4/4, PowerShell parser 18/18, incremental UBT and architecture passed, real auto reload 1/1, CSharpLiveReload 12/12, Component 7/7, ComponentBinding 3/3, and full AvidScript automation 182/182 with zero non-success results, Queue Empty, TestExit, and exit status 0.
- Phase 45.1 closeout rule：当前支持 Win64 Editor 中显式启动的单固定 Actor C# 自动热重载、350ms debounce、single-flight/trailing build 与事务 runtime 保活；同步构建会短暂阻塞 Editor。不得宣称异步取消、guest state migration、host-write rollback、Cook/Shipping 或移动端已经完成；这些属于 P45.2 及后续阶段。
- 2026-07-19 P45.1 unscoped diff gate mistake record：最终 `git diff --check` 未限定本阶段路径，因受保护的用户 Phase 42 文档中既有尾随空格而返回失败。Prevention：dirty worktree 的 pre-stage 门禁必须传入精确 owned path set；暂存后再以 `git diff --cached --check` 覆盖提交集合，受保护文件只确认未暂存，不代替用户修正。

## Phase 45.2 Async C# Live Reload Rules
- 2026-07-19 P45.2 implementation-path assumption mistake record：首次读取构建服务实现时根据 invoker 所在目录推断为 `Private/CSharpBuild/AvidScriptEditorCSharpBuildService.cpp`，实际文件位于 `Private/AvidScriptEditorCSharpBuildService.cpp`，造成无效读取。Prevention：读取或修改未确认的实现文件前必须先用 `rg --files` 精确定位，禁止从相邻类型目录推断路径。
- P45.2 thread-affinity rule：`FMonitoredProcess` 的 output/completed/canceled delegate 在监控线程执行，只能写入独立 thread-safe state；反射授权包生成、bootstrap report 验证、runtime binding slice 发布、Actor 有效性检查和事务 reload 必须由 Editor 主线程 ticker 驱动。
- P45.2 completion rule：异步进程完成只代表 build invocation 可结算，不代表请求可提交；service 必须先验证 session generation、request id、固定 target 与 active job identity，过期或取消 completion 不得解析为可绑定结果。
- P45.2 shared-invocation rule：同步 `BuildOnce` 与异步 process 必须共用 `Prepare`/`Finalize`；PowerShell 参数、输出目录、结构化 report、manifest/WASM 与 semantic cache metadata 校验只能有一份实现。
- P45.2 Task 1 baseline is incremental UE5.8 Editor build passed, architecture gate passed, and CSharpBuildService 4/4 with zero non-success results, Queue Empty, TestExit, and exit status 0.
- P45.2 process-adapter rule：`FMonitoredProcess` delegate 只捕获 thread-safe pending state，不捕获 adapter `this` 或 process；主线程 `Poll` 才组合 stdout/progress，`Cancel` 始终使用 kill-tree，并以 Canceled delegate 作为唯一取消终态。
- P45.2 Task 2 baseline is incremental UE5.8 Editor build passed with new cpp discovery, architecture gate passed, and CSharpBuildProcess 3/3 covering output/success, non-zero exit, and process-tree cancel with zero non-success results, Queue Empty, TestExit, and exit status 0.
- P45.2 pipeline ownership rule：`FAvidScriptEditorCSharpBuildService` 只保留默认路径与同步 façade；normalization、authorization package、bootstrap provenance、runtime slice、result aggregation 和 bootstrap cleanup 统一归 `FAvidScriptEditorCSharpBuildPipeline`，pipeline 禁止执行进程或持有 Actor/binding service。
- P45.2 Task 3A baseline is incremental UE5.8 Editor build passed, migrated architecture gate passed, and existing CSharpBuildService 4/4 preserved automatic slice, zero-binding, explicit package, semantic cache, structured failure, Queue Empty, TestExit, and exit status 0.
- P45.2 async-job ownership rule：`AsyncBuildBackend` 只拥有 profile、pipeline 与 invoker 编排；`AsyncBuildJob` 只拥有 process、stage、progress、cancel 与单次 result consumption；两者都禁止持有 Actor 或 component binding。公开 progress stage 必须至少跨一个调用边界可观察，不能在同一次 `Tick` 内赋值后立即覆盖。
- 2026-07-19 P45.2 transient-progress mistake record：首版 job 在 bootstrap completion 的同一次 `Tick` 内先写入 `PublishingBindingSlice`、随后立即启动 final process 并覆盖为 `FinalRunning`，导致外部 UI 永远观察不到公开阶段。Prevention：两阶段完成先缓存 invocation/snapshot 并返回，下一 Tick 才执行 slice/final preparation；回归同时断言阶段值与 final process 尚未创建。
- 2026-07-19 P45.2 staged-transition test migration mistake record：引入跨 Tick 的 `PublishingBindingSlice` 后，旧 bootstrap-failure 回归仍只调用一次 `Tick` 就断言终态，造成 1 个预期不匹配。Prevention：状态机测试必须逐个推进并断言每个公开阶段，禁止沿用旧的一帧结算假设。
- P45.2 Task 3B baseline is incremental UE5.8 Editor build passed, architecture gate enforces backend/job ownership, CSharpBuildService 4/4, CSharpAsyncBuildJob 7/7, CSharpBindingSlice 1/1, and CSharpLiveReload.BuildExecutor 3/3 all passed with zero non-success results, Queue Empty, TestExit, and exit status 0.
- P45.2 async-service ownership rule：`LiveReloadService.cpp` 只拥有 watcher/ticker/session/Stop 生命周期，`LiveReloadServiceBuildState.cpp` 只拥有 request/job/progress/completion gate 与注入式 `ApplyReport`，`LiveReloadCompletion` 只映射 profile/build/binding 结果；三层都禁止直接执行 BuildPipeline、Invoker 或 `FMonitoredProcess`。
- P45.2 async completion gate rule：只有 active job serial、coordinator session generation、request id、固定 `TWeakObjectPtr<AActor>`、Actor path 与有效性全部匹配，`ReadyToBind` 才能调用 `ApplyReport`；Stop、Actor 销毁、取消或过期 completion 均不得绑定。
- 2026-07-19 P45.2 duplicate-cancel review mistake record：首版 `AsyncBuildJob::Cancel` 在 process 仍处于 running stage 时每次都会转发，Service 显式取消后 Job 析构再次取消可能重复发送 kill-tree。Prevention：`Progress.bCancelRequested` 是 process cancel 的幂等门禁；回归连续调用两次 `Cancel` 并断言 adapter 只收到一次。
- 2026-07-19 subagent full-history fork invocation mistake record：首次启动独立审查时同时传入 `fork_context=true` 与 `agent_type`，工具拒绝 full-history fork 的显式 agent 参数。Prevention：完整历史 fork 必须省略 `agent_type`、`model` 与 `reasoning_effort`；只有无历史 spawn 才显式选择类型。
- 2026-07-19 P45.2 self-comparing job-serial review mistake record：首版 completion 在函数入口才从 `ActiveBuildJobSerial` 复制局部值并立即与原字段比较，serial gate 恒真，且 Tick 后未验证原 job 指针仍为 active。Prevention：调用 job `Tick` 前捕获 expected pointer/serial，返回后先验证 identity 再解引用，并把两者传入 completion gate；architecture gate 固定 `TickingJob`、`ExpectedJob` 与 `ExpectedJobSerial` contract。
- 2026-07-19 P45.2 rejected-completion stuck-state review mistake record：首版在 Actor path 或 request gate 失败时先释放 job 后直接返回，Coordinator 仍保持 `Building`，Actor 构建中改名可永久阻塞 trailing reload。Prevention：target identity 失配以 `TargetUnavailable/actor_identity_changed_during_build` 停止 session；同 session request 失配以结构化 completion rejection 停止；回归必须断言服务不再运行且未绑定。
- 2026-07-19 P45.2 terminal-payload trust review mistake record：首版只根据 async/build 两个成功位调用 `ApplyReport`，Canceled/Failed 终态若携带陈旧 success payload 仍会先绑定再报告取消。Prevention：绑定必须同时满足 `Stage == ReadyToBind`、async success 与 build success；Canceled+success payload 回归必须保持 apply count 为零。
- 2026-07-19 P45.2 architecture-token migration mistake record：review hardening 把轮询 owner 从 `ActiveBuildJob->Tick` 改为捕获的 `TickingJob->Tick`，首次门禁复验仍要求旧字面 token，产生 1 个静态 violation。Prevention：每次 ownership 调用点重命名或迁移后，在同一 patch 中搜索并更新 architecture gate 的 required/forbidden token，再立即运行 gate。
- P45.2 Task 4 baseline is incremental UE5.8 Editor build and architecture gate passed, AsyncBuildJob 7/7, Service 9/9, CSharpLiveReload 15/15, Module 8/8, Presentation 9/9, Component 7/7, and ComponentBinding 3/3 passed with zero non-success results, Queue Empty, TestExit, and exit status 0; the real Module scenario exercised asynchronous two-stage C# compilation, fixed-Actor transactional binding, updated Tick behavior, and old-runtime preservation after a real build failure.
- P45.2 正式产物事务规则：BuildPipeline 在任何 final invocation 前备份已提交的 report、manifest 与 WASM；只有经过共享 Invoker/Finalize 验证的成功构建才提交新文件，构建失败、launch failure、显式取消或 job 析构 cleanup 都必须恢复旧字节。Service、Job、Invoker 与 Actor binding 禁止复制或持有该文件事务。
- P45.2 显式取消规则：运行中的 AsyncBuildJob 收到 Cancel 后先向 FMonitoredProcess 发送一次 kill-tree，再释放 process 并同步进入 Canceled 终态，随后执行 backend cleanup；Service Stop 即使立即销毁 job，也必须能观察到 Canceled，并且不能等待下一次 Tick 才完成状态结算。
- 2026-07-19 P45.2 Task 5 路径假设错误记录：执行计划把 `AvidScriptEditorCSharpAutoLiveReloadTests.cpp` 写成“新建或修改”，首次探索却直接按已存在文件读取；随后又从类型名推断 WorkspaceService 与模板目录，产生多次无效读取。Prevention：计划中带“新建或修改”的路径先用 `rg --files` 判定存在性；任何 Workspace/Template 实现都先定位再读取，不从命名或相邻目录推断。
- 2026-07-19 P45.2 source graph 遗留错误记录：P45.1 把 private watcher header 改名为 `DirectoryWatchHost.h`，但 `.cpp` 仍保留 `LiveReloadWatchHost.cpp`，旧 UBT makefile 未收录该源文件而暂时掩盖了 UE 首头文件检查错误。Prevention：C++ owner 类型或首头文件改名时，header/cpp basename 必须同批重命名；新增 cpp 触发 source graph 重建后必须关注之前未编译文件的诊断。
- 2026-07-19 P45.2 Task 5 门禁命令错误记录：首次架构检查使用了不存在的 `<UserProfile>/Documents/WindowsPowerShell/Scripts/pwsh.exe`。Prevention：本仓库门禁直接由当前 PowerShell 执行 `Build/CheckAvidScriptArchitecture.ps1`；仅使用经过 `Test-Path` 验证的显式 shell 路径。
- 2026-07-19 P45.2 Task 5 读取编排错误复发：检查 diff 时再次用分号把 `git diff --stat` 与 `git status --short` 串在同一 shell 调用。Prevention：所有无依赖只读命令继续保持独立 `exec_command` 并由统一并行编排，不因命令短小而例外。
- P45.2 Task 5 focused baseline：真实工作区自动重载 1/1 通过，证明 CoreTicker 在实际双阶段 C# 构建中持续心跳、编译失败保留 manifest/WASM 字节、候选绑定拒绝后旧 Tick 继续；真实取消 1/1 通过，证明 PowerShell -> dotnet MSBuild -> PowerShell 进程树退出、job=Canceled、旧 runtime 与正式三件套均保留；AsyncBuildJob 7/7、增量 UE5.8 Editor 编译与 architecture gate 通过。
- 2026-07-19 P45.2 并行只读命令拼接错误记录：首次把已逐项加引号的 argv 片段直接拼成 PowerShell command，导致 `'git' 'status'` 等命令在解析阶段失败。Prevention：`exec_command` 接收 shell source，不接收 argv 数组；并行编排仍传独立 command string，复杂参数只在目标 shell 内按其语法引用。
- 2026-07-19 P45.2 .NET 测试宿主误判记录：首次对 `OutputType=Exe` 的自有测试宿主执行 `dotnet test`，命令返回 0 但没有运行任何用例。Prevention：新增或不熟悉的 csproj 先检查 SDK/Test SDK 与 OutputType；本仓库五组自有测试宿主统一使用固定 .NET 8 的 `dotnet run --project`，并要求输出明确的 `N/N passed`。
- P45.2 final baseline：Frontend 7/7、Semantic 43/43、GuestIr 31/31、CSharpGuest 19/19、WasmBackend 11/11、SemanticCache key/entry/prepared/integration 16/16、25/25、11/11、8/8、BuildIntegration 11/11、BuildPublication 4/4、PowerShell parser 18/18、incremental UE5.8 Editor build 与 architecture gate 通过；focused UE automation 为 BuildService 4/4、BuildProcess 3/3、AsyncBuildJob 7/7、CSharpLiveReload 16/16、Component 7/7、ComponentBinding 3/3，完整 AvidScript automation 196/196，非成功 0、Queue Empty、TestExit 与 exit status 0。
- 2026-07-19 P45.2 process adapter 重复取消终审记录：AsyncBuildJob 已用 `bCancelRequested` 阻止重复转发，但立即释放 process adapter 时其析构再次调用 `Cancel()`，adapter 只检查 Running 而未检查自身 cancel flag，仍可能第二次发送 kill-tree。Prevention：取消幂等必须在真正拥有外部进程的 adapter 层成立；Running 且尚未请求取消时才调用 `FMonitoredProcess::Cancel(true)`，上层门禁只负责业务状态。
- 2026-07-19 P45.2 收尾补丁 hunk 格式错误记录：多文件 `apply_patch` 在第一个 hunk 后多写了一个孤立的 `@@`，预检拒绝整个补丁。Prevention：每个 `Update File` 只包含完整 hunk，切换文件前不得留下空 hunk header；多文件补丁失败时确认零写入后再拆成稳定上下文重试。

## Phase 45.3 C# Guest State Migration Rules
- 2026-07-19 P45.3 apply_patch 基准目录误判记录：首次新增阶段文档时使用了相对 `Docs/...`，但工具以 UE 工程根而非插件 Git 根为基准，文件被写到工程根；随后只检查插件路径，又错误判断为零写入。Prevention：本任务所有 `apply_patch` 路径必须显式以 `Plugins/AvidScript/` 开头；任何补丁结果异常都从 workspace root 定位目标文件，确认实际路径和内容后才能判断写入状态。
- 2026-07-19 P45.3 CLI namespace/catch-order 编译疏漏记录：新增 `SemanticDocument` 局部变量时遗漏 `AvidScript.CSharpSemantic` using，并把派生自 `IOException` 的 `InvalidDataException` catch 放在通用 IO catch 后。Prevention：跨项目模型首次进入文件时同步检查 namespace；新增异常分支按最具体到最通用排序，并在第一次编译前核对继承关系。
- 2026-07-19 P45.3 Windows 原子发布句柄错误记录：状态 schema writer 首版使用覆盖整个 try 作用域的 using declaration，`File.Move` 执行时临时文件流尚未 Dispose，Windows 返回 file in use。Prevention：WriteThrough 临时流必须使用显式 using block，在退出 block 确认句柄关闭后才能执行同卷原子替换；CLI 回归必须真实重复发布同一路径。
- 2026-07-19 P45.3 PowerShell parser 门禁插值错误记录：临时语法检查命令在双引号中写 `$File:`，冒号被解析为变量作用域分隔符，命令在读取仓库脚本前失败。Prevention：变量后紧跟冒号时固定使用 `${File}:`，并区分“门禁命令解析失败”和“被测脚本解析失败”。
- 2026-07-19 P45.3 UE automation 成功格式误判记录：首次汇总聚焦日志时匹配 `Test Completed. Result=Succeeded`，但 UE5.8 controller 实际输出 `Result={Success}`，导致已通过的 1/1 用例被误报为失败。Prevention：自动化成功计数固定匹配 `Test Completed. Result=\{Success\}`，非成功匹配同一大括号格式；同时核对 BeginEvents/EndEvents、Queue Empty、`RequestExitWithStatus(1, 0)` 与进程退出码。
- 2026-07-19 P45.3 多文件 hunk 归属错误记录：VM capability 补丁末尾的 Reload overlap 测试 hunk 未切换 `Update File`，预检在 VM 测试文件中找不到上下文并拒绝整批写入。Prevention：多模块补丁按 ownership 拆分；每个 hunk 前核对当前 `Update File`，测试修订不得附着到上一个模块文件段。
- 2026-07-19 P45.3 RuntimeSession 重复上下文插入错误记录：状态迁移块以通用 `BuildValidatedRuntime`/`ActivateValidatedRuntime` 相邻文本定位，首次命中 `LoadInitialModule` 而非目标 `ReloadModule`。Prevention：同文件存在重复调用序列时，patch hunk 必须包含完整函数签名锚点；应用后立即读回两个函数，确认初次加载和 reload 语义未串位。
- 2026-07-19 P45.3 WAMR memory 上界测试假设错误记录：VM 回归用地址 65535 作为一页 memory 的越界点，但 WAMR instantiate 的 64 KiB heap 会扩展实际线性内存，该地址合法。Prevention：不得用模块 initial page 推断实例化后的 memory 上界；越界回归使用接近 `MAX_uint32` 且长度必然溢出的范围，并继续由 WAMR `validate_app_addr` 判定。
- P45.3 state migration rule：禁止复制整块 WASM state/linear memory；只迁移 manifest 明确声明、稳定 ID 匹配且类型指纹和尺寸完全一致的安全值槽位。字符串、数组、引用、对象句柄、堆地址、计时器、协程和 VM 栈默认不迁移。
- P45.3 activation order rule：候选 runtime 必须在旧 runtime 仍存活时完成状态迁移，并在迁移成功后执行候选 BeginPlay；迁移、BeginPlay 或后续候选验证失败都卸载候选并保留旧 runtime、scheduler 和 manifest。
- 2026-07-19 P45.3 WAMR 校验异常污染记录：`wasm_runtime_validate_app_addr` 越界失败会在实例上留下 sticky exception，导致随后合法的 `BeginPlay`/`Tick` 调用也失败，破坏 reload 回滚后的旧实例可用性。Prevention：宿主侧 guest-memory capability 在校验前拒绝已有脚本异常，只清理由本次宿主范围校验产生的异常；越界回归后必须继续调用合法 export 证明 VM 未被污染。
- 2026-07-19 P45.3 假编译器契约漂移记录：状态 schema 成为 Guest 编译必填产物后，BuildPublication fixture 未同步 `StateSchemaPath`，使缺失 export 用例提前误分类为 `guest_ir_failed`。Prevention：共享编译 invoker 新增必填参数时，搜索全部注入式 compiler fixture；需要进入后续阶段的 fixture 必须发布结构完整的前置产物，并运行完整 PowerShell 契约组。
- 2026-07-19 P45.3 WAMR 路径脱敏方案错误记录：首次重建只向 MSVC 传入 `/pathmap`，编译器因缺少 `/experimental:deterministic` 忽略该参数，且 MASM 本来也不会继承 CL 的路径映射。Prevention：第三方静态库公开发布统一从未占用的临时盘符访问 plugin/source/build 根，覆盖 C、C++、ASM 和 CMake；脚本所有退出路径必须解除映射，产物提交前按二进制字节扫描本机用户名和绝对工作区路径。
- 2026-07-19 P45.3 安全审计读取命令拼接错误记录：检查两个上游 `.env` 时用分号把独立 `Get-Content` 放进同一个 shell 调用，再次违反只读命令独立编排规范。Prevention：即使文件很小，每个无依赖读取仍使用独立工具调用；安全审计不因只读而放宽命令结构。
- 2026-07-19 P45.3 parser 插值错误复发记录：完整 PowerShell 语法门禁再次在诊断字符串中写 `$File:`，复发已知作用域解析错误。Prevention：脚本变量后接冒号的唯一允许形式为 `${File}:`；门禁命令保存为可复用规范片段，不再临时手写同类插值。
- 2026-07-19 P45.3 文档占位符破坏 diff 协议记录：阶段成果补丁用 `@@` 表示 Markdown 反引号，随后全局替换同时改坏 unified diff 的 hunk header，预检拒绝且零写入。Prevention：patch 内容占位符必须使用不会出现在工具协议或 diff 语法中的命名 token，并只替换该精确 token；禁止使用 `@@`、`***`、`+++`、`---` 等协议片段。
- P45.3 final baseline：.NET 113/113、PowerShell contracts 75/75、PowerShell parser 18/18、普通增量 UE5.8 Editor build 与 architecture gate 通过；聚焦 manifest/VM/migration/session/C# artifact/真实工作区热重载均通过，完整 AvidScript automation 199/199、非成功 0、Queue Empty、TestExit 与 exit status 0。
- P45.3 public publication rule：公开 Git 历史不得包含用户邮箱、用户名、凭据、本地产物或带私有构建路径的二进制；WAMR 静态库必须从脱敏盘符构建并执行字节扫描。公开发布必须保留 phase 提交链；若旧提交含个人信息，应在临时镜像中清洗 author/committer 与历史对象，验证提交顺序、说明、时间和最终 tree 后，经用户明确授权使用 `--force-with-lease` 更新远端。禁止仅为了脱敏把既有历史压缩为单个无父快照。
- 2026-07-19 P45.3 最终审阅 stable-ID 设计缺陷记录：首版 schema 直接复用含字段类型的 Guest global ID，字段类型变化会被误判为删除加新增并绕过指纹不兼容拒绝。Prevention：迁移 stable ID 固定为 owner type identity + 字段名且不含类型；Guest global ID 仅用于定位 layout，类型完整性只由递归指纹和尺寸负责，回归显式断言 stable ID 不含 `int32`。
- 2026-07-19 P45.3 只读命令拼接再次复发：最终审阅状态迁移服务时又用分号串联 header/cpp 两个 `Get-Content`。Prevention：禁止在任何 `exec_command` 中用分号组织多文件读取；每次工具调用提交前先搜索命令 source 是否含 shell separator，多文件只读必须拆成独立调用。
- 2026-07-19 P45.3 stable-ID 回归夹具名称错误记录：新增测试未先读回 `CSharpGuestSemanticFixture`，把实际字段 `Score` 写成 `Counter`，使实现修复后的首次 GREEN 仍失败。Prevention：断言 fixture identity 前先读取其定义，优先引用现有常量或从 document symbol 取 name，禁止凭印象手写测试身份。
- 2026-07-19 P45.3 安全扫描自动变量误用复发：staged privacy scan 再次把 PowerShell 自动变量 `$Matches` 当作普通结果集合赋值。Prevention：所有临时扫描结果统一使用带语义前缀的普通变量名（如 `$ScanMatches`）；`$Matches`、`$Error`、`$Args` 等自动变量禁止用于任务状态。
- 2026-07-19 P45.3 公开历史压缩错误记录：首次推送为了避开旧提交中的个人邮箱和绝对路径，直接创建无父快照并只发布 1 条提交，导致 205 条 phase 开发记录在 GitHub 不可见。Prevention：安全发布的默认方案是清洗并保留历史；任何 squash、orphan root 或远端历史改写都必须先说明影响并获得用户明确授权。旧链恢复前必须保留本地 archive ref，恢复后验证提交数、线性结构、历史记录摘要、对象隐私与最终 tree。
- 2026-07-19 P45.3 filter-repo 工作目录错误记录：首次从仓库外调用临时 Python `git_filter_repo`，虽然其他检查使用了 `git -C`，过滤器本身仍因当前目录不是 Git 仓库而在写入前退出。Prevention：第三方 Git 工具是否支持 `-C` 必须分别确认；`git-filter-repo` 固定以临时 mirror 根作为 process working directory，失败后先核对待改写 ref tip 和 commit count 未变化。
- 2026-07-19 P45.3 跨解释器转义错误记录：预检编排把包含 PowerShell 反引号的命令放入 JavaScript template literal，导致外层脚本在 Git 启动前解析失败。Prevention：跨 JavaScript/PowerShell 传递含反引号内容时使用逐行普通字符串或避免 PowerShell 转义字符；执行失败后区分 outer parser、PowerShell parser 和目标命令三层。
- 2026-07-19 P45.3 PowerShell 条件拼接复发记录：本地 ref 同步脚本尝试在条件表达式中同时执行 `git show-ref` 并读取 `$LASTEXITCODE`，产生解析错误。Prevention：外部命令与退出码读取必须是两个独立语句，禁止把 shell 命令、分号和状态判断塞进同一个 `if` 表达式。
- 2026-07-19 P45.3 SSH fetch 会话管理错误记录：远端已验证后又启动两个长时间 `git fetch origin`，外层工具等待结束时没有保留首个 session id，造成重复挂起进程。Prevention：长命令首次 yield 必须立即保存并轮询唯一 session；本地已有经过远端回抓验证的 mirror 时，优先从该 mirror 导入对象，禁止并发启动相同 fetch。终止卡住会话前必须按完整命令行核对根进程并只结束其进程树。

## Phase 45.4 Explicit State Contract Rules
- 2026-07-20 P45.4 审查节奏错误记录：P45.4B 对每个小修复串行启动新的独立审查，并重复运行完整 publication/integration 套件，导致一个 Guest schema task 消耗远超实现本身。Prevention：每个 task 只做一次集中审查并一次列全 findings；批量修复后只跑受影响的聚焦回归，完整套件只在 task 或 phase 收尾运行。Critical 行为问题仍必须修复，但不得用重复全量验证替代工程判断。
- 2026-07-20 P45.4 跨层反引号错误复发：启动 P45.4C 代理的 JavaScript template literal 含 Markdown inline backtick，外层解析在代理启动前失败。Prevention：`functions.exec` 中的长 prompt 使用普通双引号字符串和显式换行，禁止直接嵌入反引号；需要展示代码标识时使用无 Markdown 的纯文本。
- 2026-07-20 P45.4 只读命令拼接复发：阶段核对曾再次在单个 `exec_command` 中用分号串联 status/log/diff 或多个文件读取。Prevention：每个 shell 调用只承担一个独立检查；多文件和多 Git gate 拆成独立工具调用，不因只读或输出短而放宽。
- P45.4 runtime migration rule：candidate primary stable ID 优先于 alias；同一次迁移中一个 candidate slot 只能被一个 previous slot claim。多个旧 ID 汇聚到同一目标必须在写入前 fail closed，禁止按遍历顺序覆盖。
- P45.4 rollback rule：candidate 写失败后逆序尝试恢复全部已写槽；单个恢复失败不得阻止其余恢复尝试，并以首个恢复失败槽作为诊断主键。candidate 只在完整迁移成功后激活。
- 2026-07-20 P45.4 最终夹具版本漂移记录：Semantic schema 升级到 6/1.6 后，`SemanticCacheEntryContractTests.ps1` 的 seed semantic 与 report 仍保留 5/1.5，导致完整 PowerShell contract 首轮失败。Prevention：任何 schema/version 常量升级都必须搜索生产验证器、缓存键、prepared artifact、seed report 和注入式 fixture；阶段收尾继续运行全部 contract，不能仅依赖新功能聚焦组。
- 2026-07-20 P45.4 PowerShell JSON 数值类型错误记录：`Try-GetJsonInt32` 首版只接受 `[int]`，但 PowerShell 7 的 `ConvertFrom-Json` 会把普通 JSON 整数表示为 `[long]`，使合法 prepared semantic 被拒绝。Prevention：JSON Int32 helper 接受 `[int]` 或 Int32 范围内的 `[long]`，并明确拒绝字符串、浮点数和溢出值；所有数值边界由共享 helper 处理。
- P45.4 final baseline：.NET 自有测试宿主 129/129、PowerShell contracts 83/83、PowerShell parser 18/18、architecture gate 与普通增量 UE5.8 Editor build 通过；P45.4 聚焦 manifest、migration/session rollback、binding facade、workspace、auto reload 与真实 rename reload 全部通过，完整 AvidScript automation 199/199、非成功 0、Queue Empty、TestExit 与进程退出码 0。
- 2026-07-20 P45.4 发布前历史扫描错误记录：未发布提交中误跟踪 `.superpowers/sdd/task-2-report.md`，且阶段验证文档保留了本机用户绝对路径。Prevention：生成审查文件前确认 `.superpowers/` 已忽略；发布门禁必须扫描 `origin/main..HEAD` 的全部新增文本和 tracked path，而不只扫描最后一次 staged diff。若问题只存在于未发布链，保留提交数量、顺序和说明进行内容清洗，再以普通 fast-forward push 发布。

## Phase 45.5 Transactional Host Effect Rules

- 2026-07-21 P45.5 路径猜测错误记录：探索 binding selection profile 时直接按推断路径读取 `Private/BindingGeneration/AvidScriptEditorBindingSelectionProfile.h`，实际合同位于 `Public/AvidScriptEditorBindingSelectionTypes.h`。Prevention：读取计划或推断出的新路径前先用 `rg --files` 按类型名/关键词定位；不得从相邻模块命名习惯推断文件存在。
- 2026-07-21 P45.5 只读正则命令错误记录：首次搜索 descriptor schema 写入点时把含嵌套双引号和括号的 `rg` pattern 放进 PowerShell 双引号，shell 在 `rg` 启动前报 `Unexpected token ')'`。Prevention：复杂 `rg` 正则固定使用 PowerShell 单引号；若 pattern 自身含单引号则改用参数数组，不用多层双引号临时转义。
- P45.5 candidate effect rule：只有 descriptor 明确声明且 runtime 已注册 transaction adapter 的宿主写入能在候选 `BeginPlay` 执行；未知非 const reflected binding 必须在 `ProcessEvent` 前以 `binding_reload_effect_unsupported` fail closed。live runtime 调用不受 candidate journal 限制。
- P45.5 transaction ownership rule：`RuntimeSession` 是候选 HostEffectTransaction 唯一 owner；runtime instance 和 binding package 只持有调用期非 owning journal 指针。候选 commit/rollback 后必须在成为 live runtime 前清除该指针，禁止跨 Tick 或事件保存。
- 2026-07-21 P45.5 模块增量构建错误记录：新增 `AvidScriptBindings` 导出符号后只用 `-Module=AvidScriptEditor` 构建，UBT 更新了依赖模块 import library 并成功链接 Editor DLL，却没有重新链接 Bindings DLL；自动化启动时因此以 Windows 127 拒绝加载 Editor 模块。Prevention：跨模块 public ABI 变化后，在同一增量命令显式列出所有受影响的生产模块，或构建完整项目 target；编译成功后必须实际加载模块，不能把 import library 更新视为依赖 DLL 已更新。
- 2026-07-21 P45.5 自动化多 filter 命令错误记录：把两个 `Automation RunTests` 用分号放入同一 `-ExecCmds`，UE5.8 只排入第一组并把第二段报告为 unknown command。Prevention：每个 automation 进程只运行一个 filter；需要多组聚焦验证时分别启动、分别核对 found/completed/Queue Empty/TestExit，禁止依赖 console 分号串联测试队列。
- 2026-07-21 P45.5 Windows rg glob 错误记录：把 `Docs/Phase45/P45.5*` 作为普通搜索路径传给 `rg`，Windows 未展开通配符并返回路径语法错误。Prevention：内容过滤使用 `rg -g 'P45.5*'`，文件定位先用 `rg --files` 再筛选；不得依赖 PowerShell 为 native command 展开路径 glob。
- 2026-07-21 P45.5 依赖读取并行错误记录：同一并行批次一边执行 `rg --files`，一边按尚未验证的猜测读取 Bindings 模块 Actor/SceneComponent 测试文件；真实测试位于 Runtime 模块，两个猜测读取失败。Prevention：用于决定后续路径的定位命令必须先完成，再基于其输出发起下一批读取；有数据依赖的工具调用禁止伪并行。
- 2026-07-21 P45.5 新增 Runtime cpp 注册规则复发：RED 构建已因新增测试刷新 makefile，随后新增 transaction 实现 cpp 后未先进入 Git index；下一次模块编译只编入测试声明并在链接阶段缺少实现。Prevention：每次新增任意 plugin `.cpp` 后，在首次编译前单独确认该文件已 staged 且 `Intermediate/.../Module.<Module>.cpp` 包含它；同一 task 先后新增多个 cpp 也必须逐次执行该检查。
- 2026-07-20 P45.5 读取命令拼接错误记录：两个无关的 `Get-Content` 读取被 PowerShell 分号拼在同一命令中，输出截断后文件边界不清晰。Prevention：每次执行只读取一个文件或一个连续范围；已确认相互独立的读取使用分离的并行工具调用，禁止用 shell 分隔符拼接检查命令。
- **2026-07-20 - Do not use a module name as a UBT target:** `AvidScriptEditor` is a module, not a target, so `Build.bat AvidScriptEditor ...` fails with `Couldn't find target rules file`. Build it through the real project target: `Build.bat AvidTPSTemplateEditor Win64 Development -Project=<uproject> -Module=AvidScriptEditor ...`. Apply the same target-plus-`-Module` form to other plugin modules.
- **2026-07-20 - Invoke the pinned .NET SDK explicitly:** P45.5 final verification repeated the known mistake of calling the system `dotnet`, which only exposed SDK 9.0.306 and could not satisfy `global.json` 8.0.416. All repository .NET hosts must use `$env:USERPROFILE\.dotnet\dotnet.exe` (after confirming `--list-sdks` contains 8.0.416); do not infer that the shell default or UE bundled SDK matches the repository pin.
- **2026-07-20 - Do not parallelize build-oriented PowerShell contract hosts:** P45.5 ran PreparedSemantic, BuildIntegration, BuildPublication, and SemanticCacheBuildIntegration together; their nested builds share repository tool output and the batch produced fixture build failures. The same hosts passed when run one at a time with explicit `-DotNetPath`. Semantic cache pure contract readers may be independent, but any host that invokes the C# build pipeline must run serially.
- 2026-07-20 P45.5 阶段清单扫描误报记录：最终门禁直接搜索文档中的任意字面量 `- [ ]`，把说明段落展示的复选框语法误判成未完成任务。Prevention：清单完成度检查必须将正则锚定到 Markdown 列表项行首（`^\s*-\s+\[ \]`）；示例、引用和说明文字不得参与任务状态判断。
- P45.5 candidate host-effect rule：reload transaction 只存在于候选 `BeginPlay`；初次加载和 live Tick/Event/Input 不得持有 journal。静态与动态 binding 必须在实际 UE 写入或 `ProcessEvent` 前 prepare effect，未知非 const binding 在候选阶段按 `binding_reload_effect_unsupported` fail closed。
- P45.5 API 扩展规则：新增 UE 函数继续由 descriptor v3 的中央精确 policy 分类并复用动态 ABI；只有出现新的副作用语义时才新增 effect-domain adapter，禁止回到逐个函数新增 VM enum、Runtime switch 或回滚 import。
- P45.5 final baseline：.NET 自有测试 129/129、PowerShell contracts 83/83、PowerShell parser 18/18、architecture gate 与普通增量 UE5.8 Editor build 通过；四条指定真实 reflected/C#/workspace 闭环均为 1/1，完整 AvidScript automation 202/202、非成功 0、Queue Empty、TestExit 与进程退出码 0。

## Phase 45.6 Runtime Diagnostic Rules

- 2026-07-21 P45.6 第三方构建路径猜测错误记录：定位 WAMR call-stack 配置时直接传入推测的根级 `CMakeLists.txt`、`cmake`、`Build-Win64.ps1` 与 glob 路径，产生多个 path-not-found；真实入口是 `Build/BuildWAMRWin64.cmd`。Prevention：第三方源码和构建脚本同样先用 `rg --files` 按关键词定位，再读取已确认路径；native command 的 Windows 路径不得依赖未展开 glob。
- 2026-07-21 P45.6 Markdown 行尾空格错误记录：架构文档用两个行尾空格实现引用块强制换行，`git diff --cached --check` 将其判定为 trailing whitespace。Prevention：仓库 Markdown 的段落换行使用空引用行或独立段落，禁止依赖行尾空格；每次文档暂存前运行 scoped `git diff --check`。
- 2026-07-21 P45.6 source span 单位假设错误记录：首版架构文档未核对 `SemanticSpanFactory` 就把 debug artifact 行列写成一基；源码实现实际保存 Roslyn 零基行列。Prevention：跨层位置合同必须先读取 span producer，再固定 artifact 与 presentation 各自单位；debug map 保留零基/end-exclusive span，Runtime frame 统一转换为一基，Editor 禁止重复加一。
- 2026-07-21 P45.6 Roslyn span 测试手算错误记录：debug map 首次 GREEN 手工按方法标识符位置预期 `15:22`，实际 `SemanticSymbolProjector` 保存带 attribute 的完整 declaration syntax span `14:4`。Prevention：跨层透传测试直接比较 producer 的结构化 `SemanticSpan` 与 consumer 输出；只有专门测试 span producer 时才硬编码源码坐标，禁止按排版目测计算。
- 2026-07-21 P45.6 module identity 设计混淆记录：架构示例把 profile 的 runtime manifest `module_id` 写进 CSharpGuest debug map，但该工具真实拥有的是 `GuestModule.ModuleId`，两者在项目构建中不同。Prevention：debug map root 绑定 Guest module identity；manifest 的 `guest_ir.module_id` 显式携带并由 Runtime 比较，runtime `ModuleId` 继续只标识加载实例，禁止跨 provenance 域复用同名字段含义。
- 2026-07-21 P45.6 build-host 并行错误复发：已知 `PreparedSemanticContractTests.ps1` 与 `SemanticCacheBuildIntegrationTests.ps1` 会共享仓库内 .NET/NuGet 工具目录，仍被并行启动，冷构建因 NuGet 文件创建竞争失败；串行重跑用于区分产品回归。Prevention：任何会调用 C# build pipeline 的 PowerShell contract host 必须逐个运行并等待退出，只有纯读取合同测试允许并行；执行前先按 P45.5 既有规则分类。
- 2026-07-21 P45.6 parser gate 路径错误记录：实施计划写了不存在的 `Build/TestAvidScriptPowerShellSyntax.ps1`，执行前未先用 `rg --files`/`git ls-files` 定位，导致门禁命令本身失败。Prevention：PowerShell parser 门禁以 `git ls-files '*.ps1'` 得到当前 18 个受控脚本，再逐个调用 `System.Management.Automation.Language.Parser.ParseFile`；计划中的命令路径也必须和源码路径一样先验证存在性。
- 2026-07-21 P45.6 external-lib 增量重链接错误记录：替换 `libiwasm.lib` 后直接运行 `-Module=AvidScriptVM`，UBT 报 target up to date，没有重新链接消费新静态库的 DLL。Prevention：第三方静态库更新后只刷新其直接 owner 源文件时间戳并执行 scoped module build，确认输出包含 Link action；不得把 0 actions 当成新库已生效，也不得清理完整 Editor target。
- 2026-07-21 P45.6 PowerShell 自动变量误用复发：原始 WAMR dump 日志扫描再次把 `$Matches` 用作普通结果数组；本次没有依赖 regex 自动赋值且结果正确，但违反既有安全扫描规则。Prevention：所有 `Select-String`/`rg` 结果固定命名为 `$ScanMatches` 或领域专名，命令提交前检查不得给 `$Matches`、`$Error`、`$Args` 等自动变量赋值。
- 2026-07-21 P45.6 非唯一补丁锚点错误记录：想修改 `ParseAvidScriptWamrCallStack` 末尾的 `return true`，补丁却匹配到同文件更早的 `TryParseDecimal`，导致 scoped build 报未声明 `OutFrames`。Prevention：修改常见语句时补丁上下文必须包含目标函数签名或相邻唯一代码；应用后立即用带行号局部读取和 `git diff` 确认命中位置，再进入编译。
- 2026-07-21 P45.6 UE FString 构造误判记录：Runtime diagnostics 测试夹具按标准容器习惯使用不存在的 `FString(count, character)` 构造，首次 RED 编译先失败在夹具而非待实现链接。Prevention：重复字符字符串统一使用 UE5.8 已验证的 `FString::ChrN(count, character)`；新 UE 容器 API 写法先在 `C:\UnrealEngine\Engine\Source` 搜索现有调用。
- 2026-07-21 P45.6 Unity helper 重名复发：新增 Debug Map parser 在匿名命名空间中定义通用 `IsLowercaseSha256`、`BytesToLowerHex`，与同模块 reload cpp 合并后 C2084；此前 P36.2/P42.1/P42.2/P43.1 已记录同类规则。Prevention：开始新增 Runtime cpp 前先搜索目标模块所有 helper 名；匿名命名空间中的每个 helper 和测试常量也必须带 `DebugMap`/`Diagnostics` 等 owner 前缀，不能只修编译器已报告的两个名称。
- 2026-07-21 P45.6 public ABI 增量构建错误复发：给 Runtime public result/manifest struct 增加诊断字段后只重链 `AvidScriptRuntime`，Editor 仍加载按旧布局编译的代码，后续 Editor-Cmd 在 `DirectoryWatcher` 与 `UClass::AssembleReferenceTokenStreams` 等随机启动点访问冲突。Prevention：修改跨模块 public 类型后必须在同一 no-clean 项目 target 命令显式列出所有消费者模块；至少实际重编并重链直接消费者，再启动 Editor-Cmd 验证，禁止把 owner module 单独 build 成功视为 ABI 同步完成。
- 2026-07-21 P45.6 Automation filter 命名猜测错误记录：实施计划和首次回归使用不存在的 `AvidScript.Runtime.Reload`，Editor-Cmd 以退出码 0 完成但报告 0 tests。Prevention：运行聚焦组前先从 `IMPLEMENT_*_AUTOMATION_TEST` 或上一份成功日志确认真实 filter；验证必须同时要求 found 大于 0、found 等于 completed、全部 Success、Queue Empty、TestExit 和进程退出码 0，不能只看退出码。
- 2026-07-21 P45.6 只读命令拼接再次复发：阶段恢复后的 status/diff、skill/log 和源码范围读取仍用 PowerShell 分号放在同一个 `exec_command`，违反 P45.3-P45.5 已有规则。Prevention：即使上下文压缩或恢复后也先读取 `AGENTS.md` 相关 phase 规则；每个 shell 调用只运行一个逻辑命令，多项检查拆成独立工具调用，命令提交前检查 source 不含 shell separator。
- P45.6 diagnostic ownership rule：VM 只公开通用 WASM function index/offset frame，不包含 C# 或源码路径；CSharpGuest 生成 debug map，Runtime 验证并映射，Editor 只负责展示。WAMR 文本格式必须封装在 VM Private parser。
- P45.6 performance rule：调用栈只在 trap 路径采集；健康 BeginPlay/Tick/Event/Timer 不解析 debug map、不抓 frame、不新增 host crossing。v1 只承诺函数声明 span，禁止把未经验证的 fast-interpreter offset 表述为精确 C# 指令行号。
- 2026-07-21 P45.6 debug function index 连续性假设错误记录：Runtime 首版把映射条目数量当成全部 defined function 数量，要求 `wasm_function_index` 从 import count 连续递增；真实 C# 模块中的构造函数会占用 WASM function slot，但 projector 按设计不为非方法符号伪造源码位置，导致合法 debug map 被拒绝。Prevention：debug map root 与 manifest 必须显式发布 `imported_function_count` 和 `defined_function_count`；方法映射只要求唯一、严格递增且位于完整 index range，允许构造函数等非方法产生空洞；所有 index-space 测试必须含真实 constructor fixture，禁止从映射条目数反推定义函数数。
- 2026-07-21 P45.6 `TSet::Add` 返回值误用记录：WASM layout inspector 的首次编译把 `TSet::Add` 返回的 `FSetElementId` 当作 bool 取反，导致 C2678/C2088。Prevention：UE `TSet` 重复检测固定使用 `Contains` 后再 `Add`，只有已确认返回 bool 的容器 API 才直接进入逻辑表达式；新增 UE 容器写法在首次 scoped build 前搜索引擎或仓库现有调用。
- 2026-07-21 P45.6 嵌入 NUL 检查错误记录：WASM export name 首版校验使用 `FString.Contains(TEXT("\0"))`，但 C++ 字符串字面量在首个 NUL 处终止，该表达式实际检查空字符串并拒绝全部合法名称。Prevention：二进制 name section 的 NUL 与 UTF-8 合法性必须在带显式长度的原始字节 view 上检查，禁止用终止字符串 API 表达嵌入零字节。
- 2026-07-21 P45.6 WASM export 夹具字段错改记录：为双 import 负例参数化 BeginPlay function index 时，补丁命中同段更早的 export vector count，把固定 `2` 错改为 `importCount + 1`，inspector 因非法 section 7 正确拒绝但未到达预期三方 mismatch。Prevention：WASM 二进制夹具中 count、kind、index 等相邻同类型 LEB 写入必须用字段语义注释或唯一上下文定位；应用后逐字段读取完整 section，不能只看 diff 中出现了目标表达式。
- 2026-07-21 P45.6 架构门禁符号臆测记录：为新 WASM layout inspector 补门禁时，未先读取实现便要求不存在的 `ImportSectionId`、`ReadUnsignedLeb128` 等推测符号，使正确实现被门禁误报。Prevention：架构门禁只能锁定已存在的公共合同和关键实现边界；新增字符串检查前先用源码定位核对真实符号，禁止为了通过门禁反向添加无意义别名。
- 2026-07-21 P45.6 Markdown 双空格换行复发记录：成果文档首版再次使用两个行尾空格连接引用行，暂存差异门禁正确拒绝。Prevention：中文文档引用块多行固定插入独立的 `>` 空行；新建文档必须在提交前纳入 `git diff --cached --check`，既有错误记录不能替代实际门禁。
- P45.6 final baseline：.NET 自有测试主机 132/132、PowerShell contracts 84/84、PowerShell parser、architecture gate 与 UE5.8 no-clean 增量 target build 全部通过；完整 `AvidScript` automation 209/209、非成功 0、Queue Empty、TestExit、RequestExit status 0 与进程退出码 0。C# trap 已能通过经过 provenance 和真实 WASM layout 验证的 debug map 映射到函数声明，并由 Editor 展示。

## Phase 46 Reflected Property Binding Rules

- P46 property ownership rule：属性是第一等 reflected member binding，不允许把 `FProperty` 伪装成不存在的 `UFunction`。Selection/Profile 只保存 reflection-free 数据；Editor property policy 负责资格，shared type policy 负责 ABI 类型，Bindings Runtime 后续只消费经过验证的 descriptor。
- P46 property write rule：首个切片只生成 getter。setter 必须显式处理 BlueprintSetter、replication/OnRep、config/editor-only/instanced policy 与 candidate reload transaction；在这些合同完成前禁止直接用 `FProperty` 写入 UObject。
- 2026-07-21 P46.1 Unity helper 重名复发记录：property resolver 与 descriptor generator 分别新增同名匿名 `MakePropertySelectionKey`，Unity 合并后 C2084。Prevention：Editor BindingGeneration 下所有匿名 helper 从首次提交起必须带 owner 前缀，例如 `MakeResolved...`、`MakeDescriptor...`；新增 cpp 编译前按目标模块全局搜索候选 helper 名，不能只依赖单文件作用域判断。
- 2026-07-21 P46.2 guest-memory 跨模块 ABI 错误记录：Runtime property getter 测试首次在 Editor 模块实现 `IAvidScriptVmGuestMemory` 时，先暴露该导出接口没有 owner 提供的显式构造/析构定义；补定义后导出表已有符号，但 Editor 仍因只通过 Runtime/Bindings 间接看到 VM 类型、Build.cs 未直接链接 `AvidScriptVM` 而失败。Prevention：任何带模块 API 宏且允许消费者实现的接口必须由 owner 模块提供显式导出构造/虚析构定义，实际构造或派生该接口的消费者也必须声明 owner 为直接依赖；用 `dumpbin /exports` 区分缺失导出与缺失模块链接，再以 owner 和全部消费者同一 no-clean target 命令验证。
- 2026-07-21 P46.2 emitter 测试格式臆测记录：首版属性 facade 回归猜测内部 native 方法名为 `Binding0`，并猜测 manifest 存在带空格的 `"binding_count": 1`；真实 renderer 使用不同私有命名，manifest 的合同字段是 `required_imports`。Prevention：生成物回归只锁定公开 C# surface 与结构化互操作合同；EntryPoint 从 descriptor 读取，manifest 必须解析 JSON 后比较 stable id/name/signature，禁止断言私有 helper 名或 JSON 空白格式。
- 2026-07-21 P46.2 UE absolute log 参数错误记录：首次 automation 命令把绝对日志路径传给 `-log=`，进程虽返回 0 但预期审计文件未生成。Prevention：UE automation 的绝对日志固定使用 `-abslog=<absolute-path>`；测试结论必须来自该文件中的 found/completed/Success/TEST COMPLETE/RequestExit 与进程退出码，日志缺失时不得视为通过。
- 2026-07-21 P46.3 重复初始化补丁误命中记录：为真实 C# lifecycle Actor 设置 `CustomTimeDilation` 时，短 hunk 只锚定同文件重复出现的 `SetActorScale3D`，实际命中后面的 diagnostics 场景。Prevention：大型集成测试文件中的初始化、`return true`、通用断言等重复语句不得单独作为补丁锚点；上下文必须包含唯一测试名、helper 参数或场景文案，应用后按目标与全文件关键词两个维度确认出现位置。
- 2026-07-21 P46.3 descriptor schema 消费者遗漏记录：EngineGameplay 首次发布 schema v4 后，shared descriptor parser 与 Emitter 已支持 v4，但 PowerShell `Resolve-AvidScriptCSharpBindingPackage` 和 Runtime reload manifest loader 仍分别只接受 v2/v3；真实 C# bootstrap 先以 `ASBI4202` 拒绝包，修复后最终 WASM 又在 Runtime package load 被拒绝。Prevention：descriptor schema 升级必须全局搜索 `schema_version` 与 `DescriptorSchemaVersion` 的 producer、C++/PowerShell/.NET parser、manifest loader、fixture 与 architecture gate；真实两阶段 C# build 加 Runtime load 是完成前的必要验证，不能用 Editor-only emitter 成功替代。
- 2026-07-21 P46.3 gate 变量与搜索插值错误记录：新增 Runtime schema 门禁时未搜索已有定义，臆写 `$WasmReloadSource`，真实变量是 `$RuntimeReloadSource`；随后用 PowerShell 双引号搜索 `$WasmReloadSource` 又被 shell 展开为空并形成残缺正则。Prevention：architecture gate 新检查必须复用已通过 `rg` 确认的现有 owner 变量；搜索包含 `$` 的源码标识符固定使用 PowerShell 单引号或 `-F`，禁止让 shell 展开。
- 2026-07-21 P46.3 automation 名称搜索 quoting 复发记录：组合搜索多个双引号 C++ 测试名时，在 PowerShell 双引号参数中写 `\"`，shell 提前终止字符串并把 pattern 后半段当作命令。Prevention：`rg` 的 alternation/引号 pattern 固定放在 PowerShell 单引号中；只需要名称片段时不把 C++ 双引号纳入 pattern，复杂搜索先输出最终 argv 或拆成简单关键词。
- P46 final baseline：EngineGameplay 授权 profile 为 115 个函数加 1 个 `AActor.CustomTimeDilation` getter，真实 C# lifecycle 经过 Roslyn 三 import reachability、schema v4 最小 slice、WASM backend 与 WAMR `BeginPlay`/`Tick` 驱动 Actor；UE5.8 no-clean build、architecture gate、PowerShell parser 18/18、PreparedSemantic 11/11 和完整 AvidScript Automation 214/214 全部通过，非成功 0、TEST COMPLETE 0、RequestExit 0 与进程退出码 0。

## Phase 47 Object Reference Property Rules

- P47 object identity rule：UObject 引用属性只能通过 `FAvidScriptObjectRegistry` 投影为 `(slot, generation)`，禁止把裸指针、地址整数或长期缓存的 UObject 内存暴露给 WASM。
- P47 composability rule：属性返回的对象包装器必须能直接作为下一条自动生成实例 binding 的 receiver；Selection、descriptor、marshaller、renderer 与 Runtime 都不得出现 `RootComponent` 专用分支。
- P47 load/hot-path rule：`FObjectPropertyBase` 与目标类型验证在 package load 完成，热路径只读取缓存的 `FProperty*`、注册或复用对象句柄并写 guest memory，不解析 JSON 或按名称查找。
- 2026-07-21 P47.1 补丁缩进漂移记录：一次多文件补丁在 `GenerateFromProfile` 调用中把最后一个参数少缩进一级，虽然编译前 diff 审查发现并修复，但产生了无意义格式噪声。Prevention：多文件补丁应用后必须先读每个 hunk 的局部上下文，并运行 `git diff --check`，再进入编译。
- 2026-07-21 P47.1 搜索范围过宽记录：首次硬编码计数搜索未排除 `Source/ThirdParty/WAMR`，导致二进制与图形夹具输出淹没有效结果。Prevention：产品合同搜索默认排除 `Source/ThirdParty`，并使用目标目录白名单；只有审计 vendored 代码时才扩大范围。
- 2026-07-21 P47.1 空路径搜索复发记录：查找最新生成 descriptor 时文件筛选条件未命中，却仍把空变量传给 `rg`，使工具退化为扫描整个仓库并再次产生巨量输出。Prevention：任何动态路径搜索必须先用 `Test-Path -LiteralPath` 或结构化文件枚举确认唯一结果；路径为空时立即失败，禁止让 `rg` 使用隐式当前目录。
- P47 error visibility rule：native binding 返回 0 只表示 host 已拒绝调用；动态 WAMR raw import 必须同时设置 module exception，并由 VM 投影为带 import/details 的 `host_import_failed`，禁止让生成 C# 带默认 storage 继续执行。该合同不得增加成功热路径 host crossing。
- P47.2 error visibility result：动态 WAMR raw import 在 host dispatcher 返回失败时记录 import/details 并对 module instance 设置 exception；VM 将其投影为 `host_import_failed`，因此生成 C# 即使不分支检查整数返回值，也不会带默认 storage 继续执行。可恢复的可选对象分支必须另行设计结构性空句柄 API，不能吞掉 stale generation 错误。
- P47.2 final baseline：真实五 import C# WASM 在无 `RootComponent` 的 Actor 上于 `USceneComponent.GetWorldLocation()` 统一 trap，保留动态 import、`binding_target_invalid`、反射成员路径与无效句柄细节，且 Actor 未被后续默认值写入；scoped no-clean build、聚焦 lifecycle 与完整 AvidScript Automation 214/214 全部通过，非成功 0、TEST COMPLETE 0、RequestExit 0 与进程退出码 0。
- P47.3 structural handle rule：生成对象代理的 `IsNull` 只表示 `(slot, generation) == (0, 0)`，`HasHandle` 只表示两个字段为正；两者都不得宣称 UObject 存活或 generation 当前有效。历史 `IsValid` 暂时保留兼容，但不能用于跳过 Runtime registry 校验。
- 2026-07-21 P47.3 gate 变量先用后声明复发记录：为 emitter version 新增架构门禁时再次先引用不存在的 `$CSharpBindingArtifactHeader`，本次在运行 gate 前通过搜索发现并补上 `Read-RequiredFile`。Prevention：新增 gate 条件前必须先定位并修改同一变量加载区，先读后查再写；禁止把“运行前发现”视为没有犯错。
- 2026-07-21 P47.3 manifest 字段路径猜测记录：生成包审计把 `reference_source_sha256` 猜在 `files` 子对象，实际位于 manifest 根部，首次输出为空。Prevention：结构化产物审计先读取 producer 或输出对象键，再写字段访问；空 hash 必须视为验证命令错误并立即核对，不能当成产品成功证据。
- P47.3 final baseline：所有生成 UObject 代理新增零 host crossing 的 `IsNull` 与 `HasHandle`，保留 `IsValid` 兼容并把 emitter version 升为 47.3.0；新 manifest hash 发布到独立内容寻址目录。scoped no-clean build、architecture gate、facade 与真实 C# build 聚焦测试以及完整 AvidScript Automation 214/214 全部通过，非成功 0、TEST COMPLETE 0、RequestExit 0 与进程退出码 0。
- P47.1 final baseline：EngineGameplay 为 115 个函数加 `CustomTimeDilation` 与 `RootComponent` 两个只读属性，共 117 个 binding；真实 C# lifecycle 从授权包裁剪 5 个 import，经对象属性句柄继续调用 `USceneComponent.GetWorldLocation()`。UE5.8 scoped no-clean build、architecture gate、对象 schema 聚焦测试与完整 AvidScript Automation 214/214 全部通过，非成功 0、TEST COMPLETE 0、RequestExit 0 与进程退出码 0。

## Phase 48 Natural Gameplay Authoring Rules

- 2026-07-21 P48.2 pinned SDK 复发记录：调试映射修复后的首个聚焦测试再次调用裸 `dotnet`，系统 host 只有 SDK 9.0.306，无法满足仓库锁定的 8.0.416。Prevention：本仓库任何 .NET 命令在构造测试命令前必须先把 host 写成 `$env:USERPROFILE\.dotnet\dotnet.exe`，执行 `--version` 并要求精确等于 `8.0.416`；同时设置任务本地的 `DOTNET_CLI_HOME`、`APPDATA`、`LOCALAPPDATA` 与 `NUGET_PACKAGES`，禁止把裸 `dotnet` 作为可执行命令留到运行阶段才发现。
- 2026-07-21 P48.2 review gate 变量复发记录：为 hostile semantic artifact 防御补 architecture gate 时，先引用了未在加载区声明的 `$CSharpSemanticInputValidator`。Prevention：任何新 gate owner 必须先在同一加载区用 `Read-RequiredFile` 声明带 `Source` 后缀的变量，再在检查区复用该精确变量名；应用补丁后、运行 gate 前固定用 `rg` 同时确认声明和引用各自存在。
- 2026-07-21 P48.3 bool 状态槽尺寸猜测记录：真实 WAMR 状态迁移 RED 首版把 `bool` 按 4 字节估算，错误期待三个状态槽共 12 字节；正式 Guest layout 中 `float32=4`、`bool=1`、`int32=4`，实际为 9 字节。Prevention：任何状态迁移 byte-count 断言必须先读取生成的 state schema 或 Guest type layout，按每个 slot 的实际 `size` 求和，禁止按 C++ 对齐习惯猜测 C# Guest ABI。
- 2026-07-21 P48.3 状态槽断言遗漏记录：更新 reload migration 的槽数和字节数后，漏掉同一自动化前半段仍断言 manifest 只有一个槽，导致首轮 GREEN 在旧快照断言失败。Prevention：样例状态字段变化后必须在目标测试文件中全局搜索 `StateMigration`、`Slots.Num`、`MigratedSlotCount`、`MigratedByteCount` 和旧数字文案，统一更新加载、执行、迁移三个观察点后再编译。
- 2026-07-22 P48.4 FName ABI 计数错误记录：预检把非静态 bool `ActorHasTag(FName)` 的 host signature 误写为 `(iii)i`，漏算非 void 返回值使用的 guest storage 地址；正确组成是 self slot、self generation、name data address、return address，因此为 `(iiii)i`。Prevention：函数 ABI 断言必须逐项列出 receiver、每个参数和 hidden return storage，再与 `MakeExpectedAbiSignature`/descriptor 实际输出核对，禁止仅凭参数表心算。
- 2026-07-22 P48.4 RED fixture 越界记录：FName emitter RED 在 descriptor 生成失败后仍索引空 binding/parameter 数组，导致 Editor assertion，掩盖了预期的 capability failure。Prevention：RED tests 也必须 fail safely；任何解析或生成前置条件失败后先 guard 数组数量和对象有效性，禁止为了观察 RED 继续索引缺失产物。
- 2026-07-22 P48.4 只读命令拼接规则复发：阶段恢复后再次用 PowerShell 分号把 status、diff、多个文件读取放进同一 `exec_command`，违反已记录的单命令规范。Prevention：从本条起每个 shell 调用只执行一个逻辑命令；状态、日志、源码读取、Git 检查分别调用，不以输出较短或等待代理为理由拼接。
- 2026-07-22 P48.4 测试 guard 复查遗漏：修复 descriptor 生成失败后的空 binding 访问时，仍漏掉 JSON `abi_types[0]` 和 parse 失败后的 package 数组访问。Prevention：新增 descriptor/renderer 测试时逐个审计所有 `[]`、`GetArrayField()[N]`、`Bindings[N]`、`Parameters[N]`，每一处都必须由同一控制流中的数量检查支配；只修复最先触发的 assertion 不算完成。
- 2026-07-22 P48.4 renderer 闭环证据错误：最初的 C# source-to-WASM 测试使用手写的等价 facade，并非 C++ renderer 的真实输出，因此两边漂移时仍可能同时通过。Prevention：跨进程生成链路必须共享一个受版本管理的 renderer fixture；Editor 测试逐字节比较实时输出，C# 测试读取同一 fixture，不复制生成代码字符串。
- 2026-07-22 P48 推送前隐私遗漏：阶段文档和执行记录最初写入了本机用户目录绝对路径，直到全范围推送检查才发现。Prevention：给人读的文档从创建时固定使用 `<UserProfile>`、`<ProjectRoot>`、`<PluginRoot>`；推送前不仅扫描 staged diff，还必须扫描 `git log -p origin/main..main`，确保待推送历史中的旧提交也没有账户名、凭据或私钥。
- 2026-07-22 P48 本地历史脱敏脚本错误：对 worktree 文件执行 `git grep ... HEAD --` 会返回 `HEAD:path`，直接交给 Windows 文件 API 会形成非法路径并暂停 rebase。Prevention：读取当前 worktree 时省略 tree-ish；只有读取 object database 时才保留 tree-ish 并显式拆分 `<tree>:<path>`。
- 2026-07-22 P48 PowerShell stash 引用错误：未加引号的 `stash@{0}` 被 PowerShell 解析而未原样传给 Git。Prevention：PowerShell 中所有 reflog/stash selector 均以字符串参数传递，例如 `"stash@{0}"`，并在 pop 后核对 stash object 与 `git status`，确认用户工作已恢复。
- 2026-07-22 P48.5 UHT 类型前缀错误：首版 ProcessEvent 计数测试 Actor 继承 `AActor` 却命名为 `UAvidScriptBindingRuntimeProcessEventTestActor`，UHT 在编译前拒绝并要求 `A` 前缀。Prevention：新增 reflected fixture 时先按直接基类核对 Unreal 前缀，`UObject=U`、`AActor=A`、`FStruct=F`，再运行 UHT。
- 2026-07-22 P48.5 UE memory API 猜测错误：FName guest payload 首版使用了不存在的 `FMemory::Memchr`。Prevention：使用 UE memory helper 前先在 UE5.8 `FMemory` 声明中确认 API；对最多 4096 字节且本就需要验证的 payload，优先使用边界明确的线性扫描，不引入不必要的查找依赖。
- 2026-07-22 P48.4 授权上限回归遗漏：Gameplay profile 已由旧阶段的 117 项扩展到 342 项，但 P48.4 只更新 BindingSelection/Emitter 基线，没有搜索 BindingRuntime 与 BuildService 中相同的旧上限，导致 P48.5 首次完整 BindingRuntime 为 5/7。Prevention：profile binding count 变化后在整个 `Source/AvidScriptEditor/Private/Tests` 搜索旧数字和 `authorization ceiling`，并至少运行 BindingSelection、CSharpBindingEmitter、BindingRuntime、CSharpBuildService 四组消费者回归。
- 2026-07-22 P48.5 地址宽度测试 helper 漏改：为验证 guest address 高 32 位拒绝而把 dispatch lambda 改为 `uint64`，但外层 rejection lambda 仍是 `uint32`，编译器先报常量截断。Prevention：ABI cell 宽度变化后用函数名和参数名搜索完整调用链，逐层核对 fixture、wrapper、dispatch 与 production helper 的类型，不能只改最内层入口。
- 2026-07-22 P48.6 PowerShell 正则转义复发：首次检索生成 facade 时在 PowerShell 双引号中混用反斜杠转义，传给 `rg` 的 alternation 形成未闭合分组。Prevention：PowerShell 中的 `rg` 正则默认使用单引号；只需定位符号时去掉源码引号等非必要字符，不在 shell 层拼复杂转义。
- 2026-07-22 P48.6 输出目录猜测错误：检索既有 C# 生成物时直接使用了不存在的 `Saved/AvidScriptCSharp`，没有先枚举实际输出目录。Prevention：Saved 下的动态工具输出必须先用结构化目录枚举确认根路径，再对已存在的窄目录执行 `rg`；禁止把历史名称或推测名称直接交给递归搜索。
- 2026-07-22 P48.6 fixture 路径推测错误：修改 FName renderer 共享 fixture 时根据 Editor 测试源码位置推测它位于 `Source/.../Tests/Fixtures`，实际资产位于插件根目录 `Tests/Fixtures`，补丁预检因此失败。Prevention：任何测试资产在编辑前先用 `rg --files` 确认唯一受控路径；不能从加载方源码目录反推数据文件目录。
- 2026-07-22 P48.6 `rg` 搜索根目录猜测复发：查询 Runtime stats 时把不存在的 `Source/AvidScript` 与真实目录一起传给 `rg`，有效结果虽已返回但命令仍以错误退出。Prevention：多根搜索前先用受控文件索引确认每个根目录存在；模块搜索只列真实 owner 目录，不能把插件名当作默认源码模块名。
- 2026-07-22 P48.6 自然事件与低层导出混用错误：首版 PlayablePickup 同时声明自然 `OnBeginOverlap` 和显式 `avid_on_gameplay_event`，Guest IR 以 `ASCG1007` 拒绝编译器生成路由与手写导出冲突。Prevention：脚本采用自然 gameplay callback 时不得再声明统一 gameplay event export；只有完全自行解码低层事件时才实现 `avid_on_gameplay_event`，两种 authoring 模式必须互斥。
- 2026-07-22 P48.6 emitter version 门禁漂移：P48.4 已把 artifact emitter version 升为 `48.4.0`，architecture gate 却仍要求 `47.3.0`，直到 P48.6 收尾才暴露。Prevention：任何生成 C# public surface 的变更必须在同一提交同步 `AvidScriptEditorCSharpBindingArtifact.h`、architecture gate 的精确版本与阶段文案，并实际运行 gate；禁止只更新 producer 版本。
- 2026-07-22 流程设计提交命令封装错误：`functions.exec` 中的 `shell_command` 首次使用转义双引号包裹 Git commit message，外层 JavaScript 在 Git 执行前报 `SyntaxError`。Prevention：经 `functions.exec` 调用 PowerShell 的 Git commit message 固定使用 shell 单引号，避免在 JavaScript 字符串内嵌套转义双引号。
- 2026-07-22 P48.7 .NET 测试宿主类型误判：看到 `*.Tests.csproj` 后直接运行 `dotnet test`，但五个项目都是自有 `OutputType=Exe` 测试宿主，命令只 restore、执行 0 个测试仍返回 0。Prevention：运行 .NET 测试前先读取 csproj；`Exe` 合同宿主统一用 `dotnet run --project ... -c Release` 并要求输出明确 passed/total，只有引用 Microsoft.NET.Test.Sdk 的项目才使用 `dotnet test`。
- 2026-07-22 P48.7 workspace 文本断言行尾错误：完整 automation 在 Windows checkout 上仅失败于 `[AvidPersist]\n` 字面匹配，模板实际是正确的 CRLF，测试把行尾形式误当成 authoring 合同。Prevention：跨文件内容断言先将 `\r\n` 规范化为 `\n`；只有专门验证字节产物与编码时才锁定物理行尾，普通语义测试不得依赖 Git checkout 的 EOL 策略。
- 2026-07-22 P48.6 成功热重载覆盖遗漏：PlayablePickup 的首版 `BeginPlay` 无条件清空持久化状态并恢复 Actor 外观，同时用例只验证失败回滚，没有覆盖已拾取状态下的成功 reload，导致候选运行时会覆盖迁移结果且丢失运行时本地计时器。Prevention：持久化脚本的候选 `BeginPlay` 必须在迁移后运行的语义下审计，禁止无条件初始化持久字段；运行时本地资源要按迁移状态重建，并为成功与失败 reload 各保留一条端到端用例。
- 2026-07-22 P48.7 热重载身份建模错误：成功 reload 回归最初把候选 `ModuleId` 改成 `csharp_playable_pickup_reload`，而状态迁移只适用于前后 `ModuleId` 相同的同一脚本身份，测试因此按设计跳过迁移。Prevention：热重载候选可使用独立输出目录与 artifact stem，但必须保持逻辑 `ModuleId` 不变；测试必须显式断言 migration attempted/applied、迁移槽数以及迁移后 BeginPlay 的资源重建调用。
- 2026-07-22 P48.7 不存在搜索根复发：查询 architecture/parser 命令时再次把不存在的 `Scripts` 目录交给多根 `rg`，命令在返回有效结果的同时以错误退出，重复了 P48.6 已记录的问题。Prevention：多根检索前固定先用 `rg --files` 或 `Test-Path` 建立真实根列表；若任务只需查插件全树，直接从仓库根使用受限 glob，不再手写推测目录。
- 2026-07-23 P49 规划期 PowerShell 通配路径误用：首次检索 C# 工具源码时把 `Tools/AvidScript.CSharp*` 直接作为 `rg` 路径参数，Windows 下该通配路径未展开，命令在读取源码前失败。Prevention：`rg` 搜索从已确认存在的字面目录开始，文件范围使用 `-g`；禁止把 shell 通配符放进路径参数。
- 2026-07-23 P49 规划自审单命令规范复发：恢复工作后把 `git status`、branch 和 HEAD 查询用分号塞进一次 shell 调用，违反每个 shell 调用只执行一个逻辑命令的规则。Prevention：即使查询只读且相关，也必须拆成独立 `shell_command` 并通过并行调度聚合；不得以减少调用为由拼接命令。
- 2026-07-23 P49 暂存隐私扫描退出码误判：把“无匹配即成功”的 `rg` 隐私扫描直接放进并行检查，`rg` 的正常 no-match 退出码 1 使调度层丢弃了整组展示结果。Prevention：否定式扫描使用无匹配仍返回成功的结构化包装或 `Select-String`，并单独断言匹配集合为空；不能把搜索工具的 no-match 直接当命令失败传播。
- 2026-07-23 P49 PowerShell 5 解析命令变量展开复发：用外层 PowerShell 双引号传递 `powershell.exe -Command` 脚本，`$files`、`$errors` 等变量在子进程启动前被外层展开为空，导致解析器命令自身报语法错误且没有读取目标文件。Prevention：嵌套 PowerShell 脚本整体使用外层单引号参数，或写入受版本管理的测试入口后用 `-File` 调用；包含 `$` 的子脚本禁止放在外层双引号中。
- 2026-07-23 P49 合同测试变量冒号插值错误：测试失败文案写成 `$ExpectedCode:`，PowerShell 将冒号解释为变量作用域/drive 语法，脚本在执行前解析失败。Prevention：双引号字符串中变量后紧跟冒号或其他可参与变量名解析的字符时固定使用 `${ExpectedCode}:` 形式，并在运行合同前解析全部新增 PowerShell 文件。
- 2026-07-23 P49 临时 Git 夹具继承换行配置错误：PhaseWorkflow 合同仓库继承本机 `core.autocrlf`，`git add` 的 LF/CRLF 警告在 Windows PowerShell 5 的 `ErrorActionPreference=Stop` 下被升级为终止错误，测试未进入状态机。Prevention：所有自建 Git 合同夹具在写入文件前固定本地 `core.autocrlf=false`，同时配置夹具身份，避免测试结果依赖用户全局 Git 设置。
- 2026-07-23 P49 CLI 数组与错误输出进程边界误判：合同宿主通过 `powershell.exe -File` 传递多个 `-BatchId` 裸值时，后续值被当作无归属位置参数；同时 `Write-Error` 在父进程中转成格式化 ErrorRecord，截断了稳定诊断。Prevention：跨进程数组参数使用单个逗号分隔值并由 CLI 明确拆分校验；预期业务拒绝输出单行普通文本并用退出码 1 表达，不依赖 PowerShell 错误流格式。
- 2026-07-23 P49 Windows PowerShell 原子替换重载误判：状态写入首版调用 `.NET File.Replace(temp, destination, $null)`，在 Windows PowerShell 5/.NET Framework 中空 backup 路径被判为非法，首次状态更新失败。Prevention：跨 PowerShell 5/7 的同目录原子替换使用真实随机 backup 路径和四参数重载，成功后删除 backup，`finally` 同时清理 temp/backup；新增文件仍使用同目录原子 move。
- 2026-07-23 P49 确定性 JSON 行尾遗漏：状态写入首版直接使用 Windows PowerShell 5 `ConvertTo-Json` 输出，内部为 CRLF；Git 提交内容读取按 LF 重组后，verified state SHA-256 与工作区原始字节不一致。Prevention：所有参与哈希、提交和证据验证的 JSON 在 UTF-8 无 BOM 写入前先把 `CRLF/CR` 统一为 LF，哈希必须针对最终落盘字节语义。
- 2026-07-23 P49 Git porcelain 前导空格破坏：通用 Git 包装器对全部输出调用 `.Trim()`，把 `git status --porcelain` 首条记录的前导索引列空格删除，protected dirty 解析随即拒绝合法的 ` M path`。Prevention：通用进程/Git 适配器不得修剪结构化输出；保留字节/行语义，由 `rev-parse` 等具体消费者在明确协议下处理结尾。
- 2026-07-23 P49 Git AllowFailure 错误流遗漏：`git show HEAD:<untracked>` 的预期非零本应返回空 head baseline，但 Windows PowerShell 5 在 `ErrorActionPreference=Stop` 下先把 native stderr 升级成异常，绕过了退出码分支。Prevention：native 进程适配器在最小作用域内临时用 `Continue` 捕获合并输出与 `$LASTEXITCODE`，立即恢复调用方策略，再由 `AllowFailure` 合同决定返回或抛出。
- 2026-07-23 P49 架构门禁补丁上下文误判：首次插入 PhaseWorkflow 门禁时把相邻的双条件 `if` 记成单条件，`apply_patch` 预检未找到锚点并安全拒绝。Prevention：对千行级门禁文件追加区块前先用唯一诊断文案和目标变量执行 `rg -C`，按读取到的完整条件块作为补丁上下文，不依赖先前输出片段。
- 2026-07-23 P49 否定式 `rg` 并行检查再次复发：P49.0A 暂存前审查又把预期无 TODO 命中的 `rg` 放进并行工具组，使正常退出码 1 再次让调度层丢弃整组输出。Prevention：本仓库所有“应无匹配”的检查固定使用 `Get-ChildItem | Select-String` 并检查空结果；`rg` 只用于预期至少一个结果或单独显式处理退出码的查询。
- 2026-07-23 P49 Close 状态推导与身份复核遗漏：P49.0A 首轮实现让 `status` 只凭关闭文件存在就报告 closed，并且 Close 只比较报告 hash/run id，没有再次逐项比较 state 中的 verified commit/tree。Prevention：任何终态必须解析并验证完整外部证据、当前 HEAD/tree 和 state identity；文件存在不能代表有效，冗余身份字段必须全部一致后才允许关闭。
- 2026-07-23 P49 严格字段校验对象模型误判：新增 additional-properties 防御时只读取 `PSObject.Properties`，但初始化阶段的 `[ordered]` 是 `IDictionary`，其键不会作为普通属性枚举，导致所有合法 batch 被误判缺少 `id`。Prevention：跨 JSON/内存对象校验必须同时支持 `IDictionary.Keys` 与 `PSCustomObject.PSObject.Properties`，并用完整 start 合同验证两种表示。
- 2026-07-23 P49 CLI 默认仓库根求值时机错误：`RepositoryRoot` 参数默认值直接使用 `$PSScriptRoot`，Windows PowerShell 5 在参数默认表达式求值时该变量为空；合同夹具始终显式传 root，直到真实 Phase 49 自举才暴露。Prevention：可直接执行的脚本在参数绑定后通过 `$MyInvocation.MyCommand.Path` 解析脚本目录和默认仓库根，并保留一次不传 `RepositoryRoot` 的真实入口验收。
- 2026-07-23 P49 tracked state 行尾属性遗漏：状态写入已经规范化 LF，但首轮自举暂存时仓库 `.gitattributes` 没有锁定 Phase state，Windows `core.autocrlf` 警告表明后续 checkout 可能恢复 CRLF 并破坏 state SHA-256。Prevention：所有参与 Git 身份和 Gate hash 的 tracked state/summary JSON 必须声明 `text eol=lf`，架构门禁同步检查该属性。
- 2026-07-23 P49.1 UE 源码检索根误判：接口核对时从 `C:/UnrealEngine` 直接搜索不存在的 `Runtime`、`Source` 相对目录，导致并行读取在取得证据前失败。Prevention：源码版 UE5.8 的检索根固定从 `C:/UnrealEngine/Engine/Source` 开始；提交多根 `rg` 前先确认每个字面目录存在，禁止按模块简称猜测引擎根布局。
- 2026-07-23 P49.1 受保护脏基线门禁范围遗漏：已知 Phase 42 用户文档受保护且含既有尾随空格，仍运行无 pathspec 的 `git diff --check`，使 P49.1 检查被无关差异阻断。Prevention：存在受保护 dirty baseline 时，阶段中间的 diff gate 必须显式列出本阶段 ownership 文件；最终 staged gate 只检查索引内容，禁止修改用户文件来迎合无范围检查。
- 2026-07-23 P49.1 跨 JavaScript/PowerShell 路径转义复发：并行检查命令在 JavaScript 普通字符串中嵌入未转义的 Windows 反斜杠绝对路径，外层脚本在 shell 启动前报 `SyntaxError`。Prevention：`functions.exec` 中传给 PowerShell 的绝对路径统一使用 `/`，或在 JavaScript 层逐个双写反斜杠；提交调用前先区分外层字符串语法与目标 shell 语法。
- 2026-07-23 P49.1 UE flags getter 对称性猜测：看到 `UClass::GetClassFlags()` 后推断 `UFunction` 也有 `GetFunctionFlags()`，但 UE5.8 `UFunction` 只公开 `FunctionFlags` 字段。Prevention：访问新的 UE 反射 API 前必须在 `C:/UnrealEngine/Engine/Source` 精确检索声明；相邻类型或命名对称不能作为接口存在的证据。
- 2026-07-23 P49.1 架构门禁合同同步遗漏：Project Binding Profile 已切换到模块 manifest BuildId 与通用 `PublishProfile`，但首轮补丁仍让门禁要求旧 `FApp::GetBuildVersion` 和 `PublishEngineGameplay`，产生两个可预防的违规。Prevention：owner API、版本身份或发布入口变更必须在同一补丁同步 producer、consumer 与 architecture gate token，应用后立即运行门禁，不能把合同更新留到阶段末。
- 2026-07-23 P49.1 BuildId 弱回退风险：首版在 `UnrealEditor.modules` 缺失时回退到 `Build.version` 或 engine version 字符串，这些身份不能证明当前 Editor 二进制兼容，可能错误复用旧生成包。Prevention：影响反射产物身份的 BuildId 只接受当前 modules manifest；读取失败以稳定类别失败关闭，禁止降级到版本号猜测二进制身份。
- 2026-07-23 P49.1 构建等待窗口配置错误：首次集中 no-clean 模块构建给同步 shell 设置了约 5 秒的有效超时，外层先报告超时但子 `Build.bat` 仍继续运行，第二次调用只能等待同一脚本，状态一度不透明。Prevention：UE Build.bat 必须直接使用覆盖真实增量构建时长的超时；超时后先检查进程/互斥状态，不得假设子进程已终止并立即重复启动；只有返回可恢复 cell id 的执行器才使用短 yield 后 wait。
- 2026-07-23 P49.1 `rg --` 参数边界错误：检索以 `-Module` 开头的 pattern 时先漏写 `--`，修正后又把 `-g` glob 放在 `--` 之后，导致 glob 被当作路径。Prevention：以连字符开头的 pattern 使用 `rg -g '<glob>' -- '<pattern>' <roots>`，所有选项必须位于 `--` 前；复杂检索优先缩短为固定字符串。
- 2026-07-23 P49.1 PowerShell 正则引号复发：复核 `HasField(TEXT("load_policy"))` 时再次把带括号和源码引号的复杂正则放进 shell 参数，传给 `rg` 后形成未闭合分组。Prevention：源码合同复核默认拆成多个 `rg -F` 固定字符串查询；只有确实需要模式匹配时才使用单引号正则，并先去掉可由文件范围表达的语法字符。
- 2026-07-23 P49.2 并行读取失败传播错误：首轮只读检查使用 `Promise.all`，其中预期可能无匹配的 memory `rg` 返回 1 后整组结果被拒绝。Prevention：彼此独立且允许单项 no-match 的只读检查统一使用 `Promise.allSettled`，逐项展示并判断结果；否定式搜索仍优先使用不会以 no-match 失败的结构化包装。
- 2026-07-23 P49.2 源码路径猜测复发：在定位前直接读取了三个推测路径，实际 header 位于 `Public`、Guest 文件位于 `Layout` 子目录、CSharp slice 位于 `Private/CSharpBuild`。Prevention：首次读取未知 owner 或 basename 前固定先执行 `rg --files | rg -F <basename>`，只对索引确认的路径发起读取。
- 2026-07-23 P49.2 单命令规范再次复发：一次只读检查用分号连接两个 `Get-Content`。Prevention：每个 shell 调用只承载一个逻辑命令；无依赖读取用统一并行编排，不因文件相关或命令短小而拼接。
- 2026-07-23 P49.2 固定 .NET SDK 规则再次复发：Guest IR 集中验证首次调用裸 `dotnet`，系统 host 只有 9.0.306，无法满足仓库锁定的 8.0.416，测试未启动。Prevention：为本仓库构造任何 .NET 命令时，第一步就把可执行文件固定为 `$env:USERPROFILE/.dotnet/dotnet.exe`，先要求 `--version` 精确返回 8.0.416，并同步隔离 `DOTNET_CLI_HOME`、`APPDATA`、`LOCALAPPDATA` 与 `NUGET_PACKAGES`；禁止把裸 `dotnet` 留到执行阶段。
- 2026-07-23 P49.2 已知路径定位规则即时复发：读取 `SemanticOperation.cs` 已看到 `SemanticMethodBody` 同文件声明后，仍额外猜测并读取不存在的 `SemanticMethodBody.cs`。Prevention：类型声明位置以刚读到的源码证据为准；需要新文件时仍先用 `rg --files | rg -F <basename>`，不得因常见一类型一文件习惯覆盖当前证据。
- 2026-07-23 P49.2 PowerShell 固定字符串引号复发：定位 `TryGetArrayField(TEXT("bindings"))` 时把含源码双引号的 pattern 放进 shell 双引号，PowerShell 拆坏参数并让 `rg` 把尾部当路径。Prevention：含源码引号、括号或反斜杠的固定字符串查询在 PowerShell 层一律使用单引号参数，并保持 `rg -F`；不要在双引号中叠加跨 JavaScript/PowerShell 转义。
- 2026-07-23 P49.2 并行调用编排错误：一次 `Promise.allSettled` 误传了两个 shell 参数对象而不是 `tools.shell_command(...)` promise，结果只回显输入且没有执行检查。Prevention：并行工具调用先构造明确的 `tools.shell_command` promise 数组，再交给 `Promise.allSettled`；输出前逐项确认返回对象含执行结果字段。
- 2026-07-23 P49.2 PowerShell 变量搜索展开错误：检索 architecture gate 变量名时把 `$BindingDescriptor` 等 pattern 放在 PowerShell 双引号中，变量被展开为空并导致 `rg` 输出整份脚本。Prevention：搜索源码中的 PowerShell 变量名必须使用单引号固定字符串，例如 `rg -F '$BindingDescriptor'`，不得让目标 shell 插值被搜索文本。
- 2026-07-23 P49.2 architecture gate token 推测错误：新增 class-ref 门禁时按语义写了 `type.Kind == "class_ref"`，而实际防御代码使用 `type.Kind != "class_ref"`；同时 property pipeline 已统一到 `GenerateWithClassReferences`，旧入口 token 失效。Prevention：门禁 token 必须从应用补丁后的真实 owner 源码复制，不按等价语义或旧调用路径推测；新增门禁后立即执行并逐项核对违规来源。
- 2026-07-23 P49.2 集中构建边界遗漏：首次批次 UE 构建成功后才给已知会漂移的字节级 FName fixture 补充 actual artifact 诊断输出，造成一次可避免的测试源码增量重编。Prevention：descriptor schema 或 renderer identity 变更的批次，在首次构建前必须检索所有 byte-for-byte fixture，并预先准备确定性 actual 输出/迁移路径；阶段构建清单确认 fixture、门禁、测试与文档代码均已落盘后再启动 UBT。
- 2026-07-23 P49.2 functions.exec JavaScript 字符串错误：聚焦日志并行解析首次在对象字段中写入无效的反斜杠转义双引号，外层 JavaScript 在任何命令执行前报 SyntaxError。Prevention：含 PowerShell 正则和多行脚本的 command 使用 `String.raw` 模板；提交前先保证 JavaScript 字符串自身合法，再考虑 PowerShell 转义。
- 2026-07-23 P49.2 防御纵深测试假设漂移：schema v5 扩展 package hash 后，三个旧 metadata tamper 用例仍期待直接到达反射合同 gate，实际先被更外层 hash gate 拒绝。Prevention：扩大身份哈希覆盖面时审计全部深层 tamper 测试；需要验证内层防御的用例在篡改后用共享 identity API 重算合法 hash，并分别保留外层 hash mismatch 与内层 reflection mismatch 证据。
- 2026-07-23 P49.2 文本卫生扫描换行转义错误：两次临时扫描都让换行模式经过 JavaScript/PowerShell 多层转义，CRLF 的 CR 未被可靠移除并被误报为 146 处行尾空白。Prevention：跨工具文本扫描不再传递换行正则，固定按字符码 13/10 规范化后逐行检查，并用 ``git diff --check`` 作为 staged 空白合同；扫描器异常时禁止据此批量改写源码。
- 2026-07-23 P49.3 工具脚本路径猜测复发：检索 C# authorization 链路时直接传入不存在的 ``Build/AvidScriptCSharpGuest.ps1`` 和 ``Tools/AvidScript.CSharpSemanticArtifact``，使两个只读查询以路径错误退出。Prevention：跨 Build/Tools 模块检索前先枚举真实目录和 ``rg --files``；脚本入口与程序集目录均以索引结果为准，不从产品名称推测文件名。
- 2026-07-23 P49.3 手写 descriptor 来源字段漂移：ObjectLifecycle Automation 夹具把 ``source`` 写成测试专用值，违反共享 parser 对 ``ue_reflection`` 的固定合同，首次 focused 运行在生命周期派发前失败。Prevention：测试需要最小 descriptor 时必须逐项复用 ``FAvidScriptBindingDescriptorParser`` 的生产 schema 合同或由现有 generator 生成；禁止为测试语义自创受约束 metadata 值，运行前至少核对 schema/source/hash 三组字段。
- 2026-07-23 P49.3 Phase state 路径再次猜测：提交前读取状态时直接使用不存在的 ``Docs/Workflow/Phase49_State.json``，实际受控文件位于 ``Docs/Phase49/Phase49_State.json``。Prevention：任何阶段状态、Gate 或 evidence 文件在读取和暂存前先用 ``rg --files | rg -F <PhaseId>`` 确认唯一真实路径；不得根据 schema 文档目录推断实例状态目录。
- 2026-07-23 P49.3 pwsh 日期自动转换破坏状态确定性：用 PowerShell 7 执行 ``batch-complete`` 时，``ConvertFrom-Json`` 把既有 ISO 时间字符串转成 ``DateTime``，重写 state 时将旧 UTC 字节改为本地 ``+08:00``。Prevention：PhaseWorkflow 的全部生产 JSON 读取统一通过 ``ConvertFrom-AvidScriptJson``，在支持 ``-DateKind String`` 的 host 上强制保留日期字符串；跨 shell 合同必须让 pwsh 更新后断言旧时间文本逐字不变。
- 2026-07-23 P49.3 可空检索并行编排再次复发：读取 workflow 合同时仍用 ``Promise.all`` 包含可能无匹配的 ``rg``，正常退出码 1 丢弃了同组读取结果。Prevention：只读并行组只要含一个允许 no-match 的搜索就必须使用 ``Promise.allSettled``，并逐项展示状态；已知应命中的固定合同才可进入 ``Promise.all``。
- 2026-07-23 P49.4 Saved profile 路径猜测复发：在枚举产物前直接读取了不存在的 ``Saved/AvidScriptCSharpProfiles/default.csharp-profile.json``，实际 profile 分布在样例与测试生成目录。Prevention：Saved、Intermediate 等动态目录中的 profile/manifest/report 必须先用受限 ``rg --files`` 或 ``Get-ChildItem`` 枚举，再读取唯一真实路径；不得从产品名推测输出层级。
- 2026-07-23 P49.4 owner 路径与可空并行规则再次复发：直接猜测 ``AvidScriptObjectRegistry.h`` 位于 Runtime/Public，且先前会话接口读取再次让可能无匹配的 ``rg`` 进入失败传播并行组。Prevention：首次读取未知 owner 前必须先用 ``rg --files | rg -F <basename>`` 定位；任何包含可空搜索的读取组只使用 ``Promise.allSettled`` 并逐项保留结果。
- 2026-07-23 P49.4 子代理提示词外层语法错误：首次 spawn prompt 使用 JavaScript template literal，同时在中文说明中嵌入 Markdown 反引号，外层字符串被提前终止并触发 ``ReferenceError``。Prevention：多行代理提示词使用普通字符串数组加 ``join("\n")``，或确保反引号全部转义；先验证编排语言字符串，再发送任务。
- 2026-07-23 P49.4 公开头文件 owner 再次猜测：未索引就读取不存在的 ``AvidScriptBindings/Public/AvidScriptBindingPackage.h``，而 package 实际由 ``AvidScriptBindingInvocation.h`` 拥有；同时按类型名猜测 manifest loader 文件名没有结果。Prevention：新增 include 或读取 owner 前固定以类型符号执行 ``rg -l -F``，接受同文件多 owner 设计，不再从类型名推导头文件名。
- 2026-07-23 P49.4 验证入口路径猜测复发：按旧命名读取不存在的 ``Build/TestArchitecture.ps1`` 和 ``Build/TestPhaseWorkflow.ps1``，实际架构入口为 ``Build/CheckAvidScriptArchitecture.ps1``，workflow 合同由专用目录和测试夹具拥有。Prevention：运行任何门禁前先用 ``rg --files Build Tests`` 定位已跟踪入口；文档中的概念名不得直接映射成脚本路径。
- 2026-07-23 P49.4 恢复入口合同遗漏：网络恢复后的首条仓库命令直接读取 RuntimeSession 与 git status，没有先执行 ``Build/InvokePhaseWorkflow.ps1 status -Phase 49``；后续虽读取 state，但已违反恢复顺序。Prevention：任何重连、上下文压缩或任务恢复后，把 PhaseWorkflow status 作为单独的第一条仓库命令，不与源码探查并行；只有 state 明确不存在才回退到计划文档。
- 2026-07-23 P49.4 unity 产物根路径猜测：UBT 成功后从项目根 ``Intermediate/Build`` 读取 Editor unity 文件失败，真实插件模块产物位于 ``Plugins/AvidScript/Intermediate/Build``。Prevention：核验 UBT unity/source registration 时先从目标模块仓库的 ``Intermediate`` 执行受限 ``rg --files``，再读取命中的编号 unity 文件；项目 Target 不代表插件模块产物归项目根所有。
- 2026-07-23 P49.4 失败 bootstrap report 生命周期误判：从 Automation 日志复制 ``Intermediate/AvidScript/CSharpBootstrap/<guid>`` report 路径后直接读取，但 pipeline cleanup 已删除临时根。Prevention：动态 bootstrap 路径先 ``Test-Path``；需要深层 compiler 诊断时，用已发布的内容寻址 authorization package 在 ``C:/tmp`` 重放 Build 脚本并保留 report，不依赖事务清理后的临时文件。
- 2026-07-23 P49.4 C# CFG 可写捕获限制：动态投射物首版把 ``Projectile.HasHandle && UE.IsA(...)`` 直接赋给静态字段，Roslyn CFG 将左值建模为 ``flow_capture_reference``，Guest IR 只支持捕获值读取而不支持捕获位置写入，bootstrap 以 ``ASCG1004`` 失败。Prevention：在 writable flow-capture 位置模型正式设计前，脚本对带短路控制流的字段赋值使用显式分支和简单字段写；不得把捕获寄存器误当 lvalue 快速放宽 lowerer。
- 2026-07-23 P49.4 Phase state 路径猜测错误：恢复工作后直接读取不存在的 `Build/PhaseWorkflowState.json`，实际状态由流程工具管理在 `Docs/Phase49/Phase49_State.json`。Prevention：阶段状态只通过 `Build/InvokePhaseWorkflow.ps1 status -Phase <N>` 查询；需要审计原始 JSON 时先从工具输出或 `rg --files` 确认路径，不再猜测固定文件名。
- 2026-07-23 P49.4 只读命令拼接规则再次复发：收拢状态时把 `git diff --stat` 与未跟踪文件查询用分号放入同一 shell 调用。Prevention：每个 `shell_command` 保持一个逻辑命令；需要并行读取时只在 JavaScript 编排层使用 `Promise.all`，PowerShell 字符串内不得使用分号拼接多个命令。
- 2026-07-23 P49.4 Windows `rg` 通配路径复发：查找历史 benchmark baseline 时再次把 `Docs/Phase4*` 作为路径参数传给 `rg`，Windows 未展开通配符并返回非法路径。Prevention：`rg` 的搜索根必须是已确认存在的字面目录；阶段范围使用仓库根加 `-g 'Docs/Phase4*/**'` 或先由 `rg --files` 筛选，禁止把通配符放进 Windows 路径参数。
- 2026-07-23 P49.4 scoped UBT producer DLL 覆盖错误：修改 Bindings 公开 ABI 后只使用 `-Module=AvidScriptRuntime`，UBT 重建了 Bindings import library 并成功链接 Runtime，却没有重链接磁盘上的 Bindings DLL；Runtime 启动时需要三参数 `RegisterObject`，旧 DLL 仅导出两参数符号，Windows loader 以 `GetLastError=127` 拒绝。Prevention：公开模块 ABI 变化时一次 no-clean UBT 必须显式包含 producer 与全部本批次直接 consumer，例如同时传入 `-Module=AvidScriptBindings -Module=AvidScriptRuntime`；构建后用 DLL 时间或 `dumpbin /imports`、`/exports` 核对符号一致，再启动 Automation。
- 2026-07-23 P49.4 公开 ABI consumer 枚举不完整：修复 Bindings producer DLL 后只补了 Runtime consumer，遗漏 Editor 自动化源码也直接调用 `RegisterObject`，第二次启动转为 `AvidScriptEditor.dll` 的 `GetLastError=127`。Prevention：公开符号变化后先用全 Source 调用点搜索和旧 DLL `dumpbin /imports` 建立完整 consumer 模块集合，再构造一次 no-clean UBT；不得只从生产依赖图推断而漏掉测试代码所属模块。
- 2026-07-23 P49.4 只读命令拼接规则再次违反：为查看 BindingInvocation 两段源码，在同一 shell 字符串中用分号拼接两个 `Get-Content`。Prevention：即使读取同一文件的不同区段，也必须拆成两个并行 shell 调用，或一次读取后在编排层切片；禁止在 shell 内用分号组合。
- 2026-07-23 P49.4 大补丁上下文猜测错误：一次跨多文件 `apply_patch` 使用了未从当前源码复制的 lifecycle `RegisterObject` 行，补丁因上下文不匹配整体拒绝。Prevention：跨文件补丁前对每个目标位置读取精确上下文；行为/API/测试/门禁分成可独立验证的小补丁，避免一个猜错位置使整组改动失败。
- 2026-07-23 P49.4 Git diff 选项位置错误：差异汇总命令把 `--stat` 放在 `-- <pathspec>` 之后，Git 将其按 pathspec 处理并输出整份 diff，产生大量无用上下文。Prevention：所有 Git 选项必须位于 `--` 前，例如 `git diff --stat -- <paths>`；`--` 之后只允许真实路径。
- 2026-07-23 P49.4 typed binding 结果自别名覆盖遗漏：`SetSuccess(OutResult, OutResult.ObjectResult, ...)` 与同类失败路径先重置 `OutResult`，导致引用其内部成员的 `ObjectResult` 同时被清空，公开默认 `ObjectPath` 没有真正保留。Prevention：结果转换函数不得在读取嵌套输入引用前修改其 owner；统一先构造独立结果对象、复制输入合同，再用移动赋值替换输出，并用默认完整诊断与显式轻量诊断各一条 Automation 断言锁定行为。
- 2026-07-23 P49.4 Windows `rg` 通配路径规则又一次复发：已经读取过禁止把 `Tools/*/*.csproj` 作为 Windows 路径参数的记录，仍用同类 glob 直接调用 `rg`。Prevention：所有 `rg` 文件类型筛选统一写成 `rg -g '*.csproj' <pattern> Tools`，搜索根只允许已确认存在的字面目录；执行前看到 path 参数含 `*` 就必须改为 `-g`。
- 2026-07-23 P49.4 PhaseWorkflow shell 选择错误：`batch-complete` 误用 Windows PowerShell 5.1，旧 `ConvertTo-Json` 把 state 展开成宽缩进整文件噪声；随后又猜测两个不存在的 pwsh 安装路径。Prevention：PhaseWorkflow 的所有写操作固定通过已验证的 `Get-Command pwsh` 命令运行，禁止回退到 `powershell.exe`；首次会话只定位一次 pwsh 并复用命令名，不猜测 Program Files 或用户安装目录。state 暂存前必须审计其 diff 只包含预期字段。
- 2026-07-23 P49 最终 Gate .NET 并行规则再次违反：明知五个 Exe test host 共享 Frontend、Semantic 与 GuestIr ProjectReference 的 `obj/Release`，仍并行执行 `dotnet run`，四组以 `CS2012` 文件占用失败。Prevention：共享 project graph 的 build/run/format 永远串行；只有先完成单一 graph build 并为各宿主使用 `--no-build`、且确认无共享生成步骤时才允许并发运行测试进程。Gate 报告把锁竞争记录为无效 invocation，不计为产品测试失败。
- 2026-07-23 P49 最终 Gate callable fixture 漂移：P48.3 已为 ActorLifecycle 增加 `AActor.Matches(AActor)`，但 `BuildIntegrationTests.ps1` 仍保留 P48.2 的 52 callable 总数，直到 P49 全合同门禁才发现。Prevention：样例 helper 或生成 reference surface 变化时必须检索所有 artifact 总数断言；更新计数时同时断言新增 symbol id，避免只把数字改到当前值而失去语义证据。
- 2026-07-23 P49 最终 Gate package fixture 选择不稳定：PreparedSemantic 与 SemanticCacheBuildIntegration 按修改时间选择任意最新 binding package，P49 DynamicProjectile 三项 profile 覆盖了旧 EngineGameplay package，夹具随后找不到 `UE.Self.SetActorScale3D`。Prevention：测试和构建入口不得用“latest package”代替能力选择；统一解析已验证 manifest/descriptor，并按 required UE function 与 authorized stable id 选择兼容 package，再用 import 数和稳定路径作确定性排序。
- 2026-07-23 P49 恢复入口顺序第二次复发：上下文压缩恢复后先执行了 `git status`，随后才运行 `Build/InvokePhaseWorkflow.ps1 status -Phase 49`，再次违反恢复入口合同。Prevention：恢复后的第一条工具调用必须直接执行 PhaseWorkflow status；禁止把 git、memory、终端或源码探查放在它之前，状态确认后才允许展开其他检查。

## Phase 50 Typed Project API Rules

- 2026-07-23 P50.0 重构后源码路径猜测复发：架构调研时先后直接读取了不存在的 `Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingModel.h` 和 `Source/AvidScriptRuntime/Private/AvidScriptRuntimeSession.cpp`；真实文件分别是 `AvidScriptEditorBindingDescriptorModel.h` 与 `Private/Session/AvidScriptRuntimeSession.cpp`。Prevention：重构过的模块不得根据类型名或旧目录结构猜路径；首次读取 basename 或 owner 前必须执行受限 `rg --files <module> | rg -F <keyword>` 或 `rg -l -F <symbol> <module>`，只读取索引确认的路径。
- 2026-07-24 P50.2 checked-downcast 方向错误记录：renderer 与 golden test 曾共同生成并接受派生 wrapper 上的实例 `TryCast()` 返回直接基类；该方法只重复了零 crossing upcast，无法表达架构要求的 `Derived.TryCast(Base)`，直到真实 C# gameplay 样例编译才暴露。Prevention：checked downcast 固定生成为目标派生类型上的 `public static Derived TryCast(DirectBase value)`，检查目标派生 ordinal，成功复制 handle、mismatch 返回 default；golden 必须断言参数/返回方向，且每个 typed surface 批次必须至少有一个真实 Roslyn -> Guest IR -> WASM 调用用例，禁止仅凭 renderer 与同源字符串测试相互证明。
- 2026-07-24 P50 集中 UBT 测试 API 错误记录：deferred-build 批次首次编译发现测试把不存在的 `TArray::CountByPredicate` 当成 UE5.8 API，lambda 返回类型失败后又产生多条 `TestEqual` 级联错误；另一处把 `int32` dynamic Host return 与 `uint64` expected 传给 `TestEqual`，触发重载歧义。Prevention：新增 UE 容器 helper 前先在 `C:\UnrealEngine\Engine\Source` 或仓库 working example 验证精确 API；计数使用显式 `int32` 循环时声明 lambda 返回类型；Automation `TestEqual` 的 expected 必须与 actual 字段类型一致。deferred-build 阶段在集中 UBT 前仍执行编译 API 静态审查，但不以增加碎片化构建替代集中策略。
- 2026-07-24 P50 最终 Gate 抽象 fixture 错误记录：`ObjectTypeDispatch` 与 `TypedOwnerImports` 直接使用 `NewObject<UObject>`，但 UE5.8 的 `UObject` 已标记为 abstract，完整 Automation 在 `StaticAllocateObjectErrorTests` 触发 ensure。Prevention：需要普通非 Actor 对象的 Runtime 测试统一实例化既有具体类型 `UAvidScriptObjectRegistryTestObject`；新增 `NewObject<T>` 前检查 `T::StaticClass()->HasAnyClassFlags(CLASS_Abstract)` 或复用已编译的 concrete fixture。
- 2026-07-24 P50 最终 Gate descriptor fixture 漂移记录：`TypedOwnerImports` 仍期待只有空 `types/class_references/bindings` 的 schema v6 descriptor 可加载，但共享 loader 已明确要求 `bindings` 或 `class_references` 至少存在一个，导致 `descriptor_contract_invalid`。Prevention：descriptor fixture 必须从当前共享合同和同版本 working example 构造；空 capability descriptor 应断言 `bindings|class_references` fail-closed，需要加载的 v6 fixture必须提供合法 `SelfTypeId`、object type graph、typed class reference 与 `ResultTypeId`。
- 2026-07-24 P50 最终 Gate detached-worktree 环境错误记录：首次运行 `BuildIntegrationTests.ps1` 时使用纯源码 detached worktree，该目录不包含 Git ignored 的已发布 Phase 42 binding package，合同在启动时正确失败。Prevention：需要 `Saved` 发布产物的集成合同在冻结 HEAD 的主工作区执行，或显式传入受验证的外部 package 路径；只有 parser/architecture 等纯源码静态 Gate 放在 clean detached worktree，不能假设 ignored 产物会随 worktree 复制。
- 2026-07-24 P50 Gate 准备阶段 Windows `rg` 通配路径再次复发：搜索 PowerShell contract host 时把 `Tools/AvidScript.CSharpFrontend.Tests/*.ps1` 直接作为路径参数，Windows 未展开通配符并返回非法路径；命令只读且未写盘。Prevention：`rg` 搜索根只传已确认存在的字面目录，文件筛选统一使用 `-g "*.ps1"`；构造命令时只要路径参数包含 `*`，必须在执行前改写为 `-g` filter。
- 2026-07-24 P50 Gate 失败计数摘要错误：恢复摘要把首次完整 Automation 归纳为仅 2 个失败，但原始日志实际有 20 个失败，其中 18 个 Editor 失败在第二次 Gate 稳定复现。Prevention：任何 Gate 修复决策都先对每份原始日志执行失败计数和失败名称集合对比；会话摘要只作为导航，不得替代 `Test Completed. Result={Fail}` 的日志事实。
- 2026-07-24 P50 v6 包解析合同漂移：C# emitter 已发布 descriptor schema v6 和 packed owner intrinsic，但 `AvidScriptCSharpBindingPackage.ps1` 仍只接受 v2-v5、SHA-256 stable id 和非负 ordinal，导致所有真实 C# build 在编译前以 `ASBI4202` 失败。Prevention：新增 descriptor schema 或 manifest intrinsic 时必须同步审计 C++ parser、PowerShell resolver、runtime loader、architecture gate 和真实 build integration；内建 import 只按完整 stable id/module/name/signature/ordinal 合同精确放行。
- 2026-07-24 P50 Gate 修复阶段 shell 拼接规则再次违反：两次只读检查在同一 PowerShell 命令中用分号串联多个逻辑动作，虽然没有写盘，仍造成输出边界含混。Prevention：每个 `shell_command` 只保留一个逻辑命令；需要多个读取时拆成独立调用，不因“只读”而例外。
- 2026-07-24 P50 Gate 修复阶段固定 SDK 规则复发：Guest 集中验证首次又调用了裸 `dotnet`，忽略仓库已锁定的用户 SDK 8.0.416。Prevention：AvidScript 的全部 .NET build/run 命令从构造阶段就固定使用 `$env:USERPROFILE\.dotnet\dotnet.exe`，禁止先尝试 PATH host 再回退。
- 2026-07-24 P50 Gate 修复阶段复杂正则错误：源码计数检索把多个带括号的 token 合并成未闭合的 `rg` 正则，产生无效查询。Prevention：合同定位默认使用多个 `rg -F` 固定字符串；仅在必须匹配结构时使用正则，并先缩小文件范围、验证括号成对。
- 2026-07-24 P50 no-clean UBT 包装错误：首次尝试用 `cmd /c` 重定向构建日志时，项目绝对路径含空格且嵌套引号不符合 `cmd` 的 `/s /c` 解析规则，构建在 UBT 启动前被拒绝。Prevention：带空格路径的 UE 构建固定复用已验证的 PowerShell `& <Build.bat> ... *> <log>; exit $LASTEXITCODE` 包装，不临时改换 `cmd /c`；日志文件和退出码仍作为同一外部构建调用的执行包装合同。
- 2026-07-24 P50 聚焦 Automation 汇总错误：首次只读取日志末尾 80 条匹配就把 10 个失败误报为 3 个，重复了 Gate 失败计数摘要错误。Prevention：任何 Automation 结论先对整份日志统计全部 `Test Completed. Result={Fail}` 与 `Success`，输出唯一失败名称集合，再按局部上下文分类；尾部日志只用于确认队列结束和退出状态，不能用于全局计数。
- 2026-07-24 P50 PowerShell 变量名检索复发：定位 `$RequiredExports` 时首次仍在 PowerShell 双引号中调用 `rg -F`，变量被展开为空并输出整份脚本。Prevention：检索源码中的 `$` token 必须使用单引号固定字符串；执行前看到 pattern 以 `$` 开头就拒绝双引号命令。
- 2026-07-24 P50 跨文件补丁上下文再次猜测：manifest import 合同补丁假定 `EmitManifest` 使用 `OutManifest.Reset()` 和独立 `Writer->Close()`，实际源码使用 `Empty()` 并直接返回 Close，导致整组补丁预检拒绝。Prevention：跨文件行为补丁先分别读取每个目标函数的精确首尾上下文，再按 owner 拆成小补丁；不得从相邻 renderer 风格推断未读取行。
- 2026-07-24 P50 typed golden 锚点错误：迁移 class-reference upcast 时补丁只锚定公共 `internal int AvidScriptOrdinal`，把 `TSubclassOfAStaticMeshActor -> TSubclassOfAActor` 转换插进了基类 `TSubclassOfAActor` struct。Prevention：重复成员附近的 golden 补丁必须把完整类型声明和下一类型边界纳入上下文；应用后立即用 `git diff --no-index` 对 actual artifact 确认转换所属类型。
- 2026-07-24 P50 全仓 PowerShell parser 包装规则复发：临时汇总错误文案再次写成 `$file:`，解析器把冒号当成变量作用域语法并在扫描任何文件前失败。Prevention：PowerShell 双引号字符串内变量后紧跟冒号统一写为 `${file}:`；构造 parser/contract wrapper 时先对 wrapper 自身做最小解析，不因它是临时只读命令而放宽。
- 2026-07-24 P50 Gate 审查阶段兼容性收窄错误：为避免旧 descriptor 在 v6 canonical 重建中误报 `descriptor_not_canonical`，一度直接让 C# emitter 拒绝 v2-v5，违反 Phase 50 明确的旧 schema 兼容要求。Prevention：遇到多版本诊断漂移时先实现版本感知 serializer/validator，并复用 Runtime package load 校验 hash 与反射；不得以缩小已承诺输入范围代替兼容实现。
- 2026-07-24 P50 Gate 审查阶段只读命令拼接再次复发：读取 generator 与 emitter 文件头时在同一 shell 字符串中用分号拼接两个 `Get-Content`。Prevention：即使只读且输出很短，每个 `shell_command` 仍只执行一个逻辑命令；多个文件读取只在 JavaScript 编排层并行。
- 2026-07-24 P50 零导出回归测试前置条件错误：首版测试改用未提供 binding package 的自定义 C# 源码，构建在 Phase 42 授权门禁以 `phase42_binding_required` 提前失败，没有抵达目标 Direct ABI 校验。Prevention：深层负向测试必须保留此前所有生产前置条件，优先从成功 seed 派生单变量伪编译产物；断言失败分类与目标诊断，禁止只看非零退出码。
- 2026-07-24 P50 PowerShell 空数组序列化错误：零导出伪编译器把 `PSCustomObject.exports` 赋为未定型 `@()`，属性接收枚举后的 `$null`，Backend 因 schema 非法提前拒绝。Prevention：JSON schema 要求数组且测试需要空集合时显式赋值 `[object[]]@()`，写盘前断言序列化文本包含目标 `[]`，避免 PowerShell 管道枚举语义改变 JSON 类型。
- 2026-07-24 P50 严格 Guest IR 的 `PSCustomObject` round-trip 错误：把 seed Guest IR 通过 `ConvertFrom-Json`/`ConvertTo-Json` 全量重写后，即使空数组类型已修正，强类型 Backend 仍因其他 token 类型漂移拒绝 schema。Prevention：修改严格 JSON 产物时使用 `System.Text.Json.Nodes` 定点替换目标节点并保留其余 token；伪编译器必须先用真实 Backend 编译该产物，再进入上层合同断言。
- 2026-07-24 P50 `apply_patch` 文件边界遗漏：一次同时修改测试与 `AGENTS.md` 的补丁在第二组 hunk 前漏写 `*** Update File`，预检把 AGENTS 上下文错误应用到测试脚本并整体拒绝。Prevention：跨文件补丁每个目标都必须有独立 file header；错误记录与行为修复优先拆成两个小补丁，预检失败后不重复猜上下文。
- 2026-07-24 P50 合同脚本 PowerShell 宿主假设错误：手动在新 PowerShell 验证 `System.Text.Json.Nodes` 成功后，直接把该 API 写入会被 Windows PowerShell 5.1 调用的伪编译器，测试以 `TypeNotFound` 提前失败。Prevention：PowerShell 合同依赖必须以仓库支持的最低宿主能力为准；跨宿主 API 先在真实调用链验证。严格 JSON fixture 的局部变更可按规范化顶层字段双锚点替换，并必须由真实强类型 consumer 立即验证。
- 2026-07-24 P50 嵌套 PowerShell here-string 转义错误：生成伪编译器时用外层 backtick 转义内层 JSON 双引号，外层展开后转义符被消费，生成脚本出现 `"  "exports"` 解析错误。Prevention：双引号 here-string 内需要生成含 JSON 双引号的 PowerShell 字面量时，优先让生成脚本使用单引号字符串；生成后先执行 parser 检查再跑完整合同。
- 2026-07-24 P50 Editor unity 匿名 helper 重名：新 descriptor serializer 在匿名命名空间定义通用 `WriteStringArray`，与 generator 同名 helper 在 unity translation unit 合并后触发 C2084；单文件阅读无法暴露。Prevention：UE 模块 `.cpp` 的匿名 helper 仍使用 owner 前缀的唯一名称，尤其是 `Write*`/`Parse*`/`Validate*` 等高碰撞命名；集中 no-clean UBT 必须保留 unity 编译证据。
- 2026-07-24 P50 复审发现 packed owner 能力借用：首次实现只检查 package 指针有效，且 package manifest 只在自身声明 owner 时校验 schema v6 Self；脚本可以声明 packed owner、package 漏报 owner，再借用合法 v2-v5 package。Prevention：安全能力必须由实际脚本 imports、package manifest capability 和加载后的 immutable package 三方精确匹配；loader 在解析实际 imports 后交叉校验，Session 在激活前再次要求非空 `ExpectedSelfClass`，禁止把“package 有效”等同于“package 授权了某项能力”。
- 2026-07-24 P50 复审发现 legacy 兼容夹具覆盖空洞：v2-v5 emitter 兼容测试从没有 class reference 的默认 descriptor 投影，因此 schema v5 合法 class reference 的空 `result_type_id` 从未进入 renderer。Prevention：每个跨 schema 兼容承诺必须使用该旧版本独有字段形状的真实非空 fixture；schema v5 class reference 固定从 `base_class_path` 恢复 legacy nominal wrapper，不得复用要求 v6 ordinal/type graph 的 typed 分支。
- 2026-07-24 P50 复审发现 class-reference ordinal 可重标：lowerer 验证了 wrapper 形状和非负 literal，却没有验证 ordinal 是否由兼容的 `ProjectClasses` property 发布，导致合法 wrapper 可包装其他类型的 ordinal。Prevention：class reference 构造授权必须绑定 `(nominal wrapper, published ordinal)`；允许集合从生成 `ProjectClasses` getter 与受验证的 upcast 图推导，Runtime ordinal 不能单独承担 nominal 类型证明。
- 2026-07-24 P50 复审发现 manifest ordinal 宽松解析：package manifest 的 import ordinal 仍调用 `TryGetNumberField(..., int32&)`，小数可能在 UE JSON 转换时被舍入。Prevention：manifest 中所有 int32 合同字段统一先读 double，再检查 finite、边界和 `Number == Trunc(Number)`；负向测试至少覆盖动态 import 小数 ordinal。
- 2026-07-24 P50 复审修复引入 owner capability 过度约束：为阻止 packed-owner 能力借用，loader 改为要求脚本与 package 的 owner 标志精确相等，误把 package 的授权能力超集当成 script 的实际 import 集合；未使用 `UE.Self` 的合法 DynamicProjectile 因此会被拒绝。Prevention：安全 capability 校验必须先区分“package 授权”和“script 使用”；使用方声明能力时必须由 package 与 immutable plan 共同授权，package 的额外授权不构成不一致。对 capability present/absent 两个方向分别保留拒绝与正向真实加载回归。
- 2026-07-25 P50 复审发现 script manifest 可隐藏真实 WASM import：loader 只按 manifest 的 `required_imports` 判断 packed-owner 使用，WASM inspector 虽读取 module/name 却只保留函数数量，攻击者可改名 manifest 项并绕过 package/Session 能力门禁。Prevention：任何影响安全 capability 的 script manifest 字段都不能自证；loader 必须从已校验 hash 的 WASM import section提取完整函数 import identity，与 manifest 做精确多集合比较后再进行 capability 授权。负向回归必须保留真实 WASM import，只篡改 manifest 身份。
- 2026-07-25 P50 architecture checker 变量别名猜测：新增 WASM import identity 门禁时使用了未定义的 `$WasmReloadSource`、`$WasmLayoutSource` 和 `$WasmLayoutHeader`，实际既有变量为 `$RuntimeReloadSource` 与 `$VmModuleLayout*`。Prevention：向长 PowerShell checker 插入合同前必须从顶部 owner 读取已定义变量名并原样复用；应用后先执行 parser 和主 checker，不能只运行不覆盖主分支的 fixture mode。
- 2026-07-24 P50 定向 Automation 多命令包装错误：把三条 `Automation RunTests` 用分号放进同一个 `ExecCmds`，UE5.8 只接受第一条，后两条报告 unknown command，虽然进程以 0 退出但仅执行 1 个测试。Prevention：每个定向 Editor-Cmd invocation 只传一个已经从测试注册名确认的 filter；需要多组时使用共同父 filter或分别执行，证据必须核对 found/completed 的精确测试集合，不能只看进程退出码。
- 2026-07-24 P50 恢复后只读 shell 拼接再次复发：本轮首次状态与源码检索仍在同一 PowerShell 字符串内用分号串联两个逻辑动作。Prevention：上下文恢复不豁免 shell 规则；每次 `shell_command` 只执行一个逻辑任务，同文件多区段读取可在一个脚本内完成，但 status、search、read 等不同任务必须拆开。
- 2026-07-24 P50 Gate runner 路径猜测：流程设计文档展示了 `Build/RunPhaseGate.ps1` 的目标接口，但仓库尚未实现该文件；恢复时直接读取推测路径，产生一次无效命令。Prevention：文档中的规划接口不能视为已存在实现；调用构建或流程入口前先用 `rg --files Build` 确认受控路径，找不到时按已实现的状态机和证据 schema 执行，不临时假造入口。
- 2026-07-24 P50 Phase 49 证据读取又一次混合逻辑：一次 shell 调用同时执行两个 `git show` 和一次历史文档读取，重复了已记录的单命令违规。Prevention：历史 commit 元数据、不同 commit diff 与文件内容是独立逻辑查询，必须拆开；即使为同一 Gate 调研也不能合并。
- 2026-07-25 P50 复审发现全局动态导入注册表可跨包借权：WASM 与 script manifest 的 import identity 已能精确匹配，但 loader 只对 packed owner 做包级授权；WAMR 的动态 native registry 是进程全局状态，脚本仍可声明并链接另一个会话已注册的 `avid_ue_*`，未执行时甚至可完成激活。Prevention：hash-verified WASM import 必须先与 script manifest 做精确多集合比较；随后把每个非静态 import 作为当前 binding package immutable imports 的成员验证，关系固定为“script actual imports 是 package authorization 的子集”。静态 import 判定由 VM 公共接口唯一发布，Runtime 与 registry 禁止各写一份白名单。
- 2026-07-25 P50 Editor 启动期 AutoSDK 规避参数误判：定向 Automation 首次启动遇到 VisionOS `MainVersion` 校验失败后，误以为 `-NoCompile` 会跳过 TargetPlatform 的 AutoSDK 全平台探测，导致重复等待一次；源码实际只在 `-Multiprocess` 时跳过这条启动路径。Prevention：源码版 UE5.8 的自动化命令固定携带 `-Multiprocess -NoCompile`；出现平台 SDK 探测失败时先读取 `TargetPlatformManagerModule.cpp` 的真实开关，不连续猜测启动参数，也不清理 Editor Target。
- 2026-07-25 P50 受保护文件 hash 审计 PowerShell 语法错误：首次把 `foreach (...) { ... }` 语句块直接接到格式化管道，PowerShell 在解析期以 empty pipe element 停止；命令没有触碰文件。Prevention：小规模文件 hash 审计直接使用 `Get-FileHash -LiteralPath <paths> | ForEach-Object`；必须从循环收集对象时先赋给数组变量，再把变量送入管道，禁止在提交边界临时拼写未验证的复合管道。
- 2026-07-25 P50 staged 隐私扫描方向错误：首次对完整 `git diff --cached` 文本执行敏感路径匹配，删除旧个人路径的 `-` 行被误当成待推送泄露，形成假阳性。Prevention：提交隐私门禁只扫描 unified-zero diff 中以单个 `+` 开头且不是 `+++` header 的新增行；删除行用于证明清理但不作为泄露失败。推送前另对 `git diff origin/main..HEAD` 的新增行执行同一规则，避免只看最后一个 commit。
- 2026-07-25 P50 freeze 提交 shell 拼接复发：gate-ready state 提交时把 `git add` 与 `git commit` 用分号放进同一条 `shell_command`，内容虽正确但再次违反单命令规范，最终 Gate 尚未开始即主动 reopen。Prevention：状态暂存、暂存核对、提交必须是三个独立 shell 调用；即使没有用户等待成本，也不得为减少工具消息合并写操作。freeze 后提交前固定检查待执行字符串不含命令分隔符。
- 2026-07-25 P50 最终静态 Gate 包装字符串插值错误：parser 汇总文案写成 `"$file:$line"`，PowerShell 把冒号视为变量作用域语法并在执行任何检查前 ParserError；本轮 0 项测试、0 份 Gate 日志，不计 invocation。Prevention：变量后紧邻冒号时一律写成 `"${file}:$line"` 或格式化运算符；最终 Gate 包装先用 `[scriptblock]::Create()` 做纯解析预检，再运行受控命令，避免候选冻结后才发现 orchestrator 语法错误。
- 2026-07-25 P50 静态 Gate 工作树选择规则复发：已经存在“纯静态 Gate 使用 clean detached worktree”的规则，仍在含两个 protected dirty 文件的主工作树运行 `TestPhase50Architecture.ps1 -Mode Gate`；parser 23/23 与 fixtures 17/17 已通过，Gate 只因 clean-tree 合同拒绝。Prevention：freeze 后先检查主工作树是否存在 protected baseline；若存在，architecture/parser/frozen-hash Gate 固定在 `C:\tmp` 下由 verified HEAD 创建的 detached worktree 执行并核对 HEAD，依赖 ignored `Saved` 产物的合同、UBT 与 Automation 留在主工作区。不得先在主工作树试跑再切换。
- 2026-07-25 P50 freeze 人读证据语言偏差：一次 `ReviewEvidence` 使用英文，状态尚未提交即发现，但违反“给人读的文档使用中文”并再次触发 reopen。Prevention：PhaseWorkflow 的 `Evidence`、`Reason`、`ReviewEvidence` 以及 closeout 文案在执行写操作前先做中文口径检查；协议字段名、诊断 category 和命令可保留英文，叙述证据必须使用中文。
- P50 object-type rule：自定义 UObject 类型必须由 descriptor v6 的稳定类型图和 package-load immutable `UClass` plan 驱动；Runtime 热路径只能按 ordinal 索引缓存类型，禁止根据 C# 名称或 class path 执行字符串反射查找。
- P50 conversion rule：派生 handle 到基类 handle 的 upcast 必须完全在 Guest 内复制两个 `i32`，不得新增 Host import；基类 handle 到派生 handle 的 checked downcast 统一使用一个 object-type import，类型不匹配返回 invalid handle，stale/cross-world handle 继续失败关闭。
- P50 API-growth rule：自定义项目 `UFUNCTION` 继续通过 descriptor、统一 dynamic ABI 和 cached `ProcessEvent` plan 扩展；禁止为单个项目类或函数手写 VM import、Runtime switch、WAMR wrapper 或 renderer 特判。
- 2026-07-25 P50 网络重连恢复顺序错误：本轮恢复后的第一条仓库命令执行了 `git status`，没有先运行状态机规定的 `Build/InvokePhaseWorkflow.ps1 status -Phase 50`；随后读取状态文件并 `reopen`，未造成内容损失。Prevention：每次收到网络重连后的首条用户消息，先把状态机 status 命令作为唯一仓库调用，再检查 Git 和日志；聊天摘要或磁盘 checkpoint 都不能替代该顺序。
- 2026-07-25 P50 Gate 失败调查路径假设错误：一次 `rg` 使用了 Windows 不展开的测试目录通配路径，恢复后又猜测 `AvidScriptWamrHostBindings.cpp` 位于不存在的 `Private/Wamr` 子目录，两次均只读失败。Prevention：源码搜索先用 `rg --files <literal-root> | rg <filename>` 确认真实路径；Windows 文件过滤只使用 `-g`，不得把通配符或推测目录直接作为搜索路径。
- 2026-07-25 P50 legacy reload fixture 合同漂移：安全门禁升级为精确比较 hash-verified WASM function imports 与 manifest `required_imports` 后，三个旧正向 fixture 仍生成零 import WASM 却声明 `env.actor_set_location`；首次完整 Automation 因 `manifest_wasm_import_mismatch` 失败，`StateMigrationManifestContract` 随后继续索引空 `Slots[0]` 并触发进程断言。Prevention：正向 manifest fixture 必须让真实 WASM import identity 与 manifest 精确一致，fixture hash 从实际字节计算；任何正向加载断言失败后必须立即短路，数组索引前独立验证数量，完整 Gate 失败也不能演变为测试宿主崩溃。
- 2026-07-25 P50 `rg` 固定字符串引用错误：检索 C++ 字符串中的 `\"required_imports\"` 时把 pattern 放进 PowerShell 双引号，导致参数被拆分、输出大量无关匹配并追加一个不存在路径错误；命令只读。Prevention：包含反斜线、`$` 或 C++ 转义引号的 `rg -F` pattern 固定使用 PowerShell 单引号，先在命令文本层确认 pattern 是一个参数。
- 2026-07-25 P50 Windows 通配路径禁令即时复发：刚记录路径规则后，检查 gameplay fixture 仍把 `AvidScriptGameplayEventFixture.*` 作为 `rg` 路径传入并触发 OS error 123；第二次改用字面目录和 `-g` 才成功。Prevention：每条 `rg` 执行前对所有 path position 做机械扫描，只要出现 `*` 或 `?` 就拒绝执行并改写为 `-g`；已写入 AGENTS 的规则不能只靠临场记忆。
- 2026-07-25 P50 fixture 身份更新漏改断言：将 reload manifest fixture 从虚假的 `env.actor_set_location` 改为真实 `avidscript.host_add_i32` 后，没有同步同一测试末尾对 parsed import 的两个期望值，导致聚焦 `AvidScript.Reload` 10 项中 1 项失败；生产加载和 State Migration 均已成功。Prevention：修改测试 fixture 的 module/name/hash/export 等身份字段后，先对旧值做同文件与全仓固定字符串检索，清零所有非负向用例引用后再编译；不能只审查构造器和二进制数组。
- 2026-07-25 P50 断言修复补丁上下文猜测：修复上述期望值时没有先读取精确成员名，补丁假定 `RequiredImports` 元素字段为 `Module`/`Name`，实际为 `ModuleName`/`ImportName`，导致 `apply_patch` 预检拒绝且未写盘。Prevention：即使日志已给出行号，补丁前仍读取目标行的精确文本；行为修复与 AGENTS 记录拆成独立补丁，禁止从相邻类型命名推断成员名。
- 2026-07-25 P50 VM backend 文件名再次猜测：检查 direct Session 授权路径时直接假定实现文件名为 `AvidScriptWamrVmBackend.cpp`，实际为 `AvidScriptWamrBackend.cpp`，产生一次只读路径错误。Prevention：即使目录已确认，文件名仍必须先通过 `rg --files <literal-root>` 定位；同一轮已经出现路径假设错误后，后续所有源码文件调用都禁止手写未验证 basename。
- 2026-07-25 P50 复审发现公开 Session 可绕过 import 身份合同：文件 manifest loader 已精确比较 hash-verified WASM imports 并做当前 package 成员授权，但公开 `FAvidScriptRuntimeSession::LoadInitialModule/ReloadModule` 接受内存 bytecode + manifest 时只验证 manifest 字段，直接调用者可绕过 exact multiset 检查，并可能重新暴露全局 WAMR registry 跨包借权。Prevention：actual imports、manifest imports 与 immutable package 授权统一由 `AvidScriptWasmImportPolicy` 实现；文件 loader 和 direct Session 都必须调用同一策略，architecture gate 固定检查两个入口，direct Session 回归必须证明 mismatch 在激活前失败。
- 2026-07-25 P50 diagnostics 负向分类漂移：`ManifestRuntimeIntegration` 的 fixture 明确生成 manifest 1 个 import、WASM 2 个 imports，安全门禁升级后应先返回 `manifest_wasm_import_mismatch`，旧断言仍期待后续 `debug_map_wasm_layout_mismatch`，直到前序 Gate 崩溃修复后才被完整执行。Prevention：新增更早的 fail-closed 门禁时审计下游负向 fixture 的首个预期错误分类；聚焦测试必须跑完完整相关父组，不能只验证新正向路径。
- 2026-07-25 P50 PowerShell `$` pattern 引用规则再次复发：定位 checker 中 `$VmContractHeader` 赋值时又把 pattern 放进双引号，PowerShell 展开变量后首次检索无结果；命令只读。Prevention：构造任何 `rg` 命令时，只要 pattern 文本包含 `$`，在工具调用前执行字符串级拒绝并改为 PowerShell 单引号；不再依靠“已经记录过”来替代机械检查。
- 2026-07-25 P50 复审继续发现 VM backend 仍可跨包借权：只在 Runtime/Session 共享 import policy 仍不足以保护直接 `IAvidScriptVmBackend::Load`；Backend 原先先注册 package 全集，再让 WAMR 从进程全局 registry 链接 actual imports，未授权 VM 可借用已注册 attachment，授权 VM 卸载后存在 attachment 悬空和潜在 UAF。Prevention：唯一 `ValidateAvidScriptVmImportContract` 下沉至 AvidScriptVM；Backend 必须在取得 WAMR lease、注册 native 和 `wasm_runtime_load` 前解析 actual layout 并验证每个非静态 import 属于当前 package；Runtime exact manifest 校验委托同一 VM policy，调用时 ordinal 检查继续作为纵深防御。
- 2026-07-25 P50 VM 前置授权改变 legacy 分类：`host_missing_i32` 过去依赖 WAMR 链接后返回 `missing_import`，底层 policy 现在在 WAMR 前识别为未带 binding package 的非静态 import，稳定分类变为 `binding_package_missing`；旧测试期望需要同步。Prevention：把安全校验下沉到更早层级时，固定搜索原错误 category 的全部断言，并为新的 pre-link category 保留 import module/name 诊断，避免只验证新增攻击回归。
- 2026-07-25 P50 PhaseWorkflow 接口探测错误：恢复后直接执行不存在的 `InvokePhaseWorkflow.ps1 help` 子命令，脚本先按必填 Phase 合同返回 `ASPW1101`，没有提供帮助信息。Prevention：PhaseWorkflow 命令只从 `InvokePhaseWorkflow.ps1` 的已实现 switch 或既有阶段记录读取；需要了解参数时读取脚本 param/switch，不再通过猜测子命令探测。
- 2026-07-25 P50 parser 包装变量展开规则复发：手动检查 architecture checker 时再次把含 `$tokens`、`$errors` 的内层 PowerShell 放进外层双引号，变量在错误层提前展开并生成 empty pipe ParserError；随后改为单引号命令才完成真实解析。Prevention：`pwsh -Command` 包装包含 PowerShell 变量时，外层固定使用单引号脚本文本；禁止在已知 `$` 引用规则上继续依赖人工转义。
- 2026-07-25 P50 UE guard include 路径假设错误：Session operation lease 首版直接 include 不存在的 `Misc/GuardValue.h`，集中 UBT 才发现 UE5.8 的 `TGuardValue` 位于 `Templates/UnrealTemplate.h` 且已由 Core 头提供。Prevention：新增 UE helper 的 include owner 必须先在 `C:\UnrealEngine\Engine\Source\Runtime\Core\Public` 用符号检索确认；CoreMinimal 已提供的模板不添加猜测路径 include。
- 2026-07-25 P50 测试 observer 自销毁：重入 reload 回归在 `CandidateBeginPlayObserverForTesting` 回调内部把拥有当前闭包的 `TFunction` 清空；生产 guard 已正确返回 `reentrant_operation`，但闭包执行中销毁自身导致捕获布尔值被污染并形成假失败。Prevention：一次性 observer 固定由 owner 在调用前 `MoveTemp` 到局部变量，再执行局部副本；回调不得修改或销毁当前正在执行的 callable owner。
- 2026-07-25 P50 Component 重入销毁后的 Super Tick：安全回归让 guest 执行期间调用 `DestroyComponent()`，Runtime release 已正确延迟到 guest call 返回，但外层 `UAvidScriptComponent::TickComponent` 仍无条件进入 `UActorComponent::TickComponent`，对已取消注册组件触发 UE5.8 `bRegistered` 断言。Prevention：任何允许宿主回调改变 UObject/Component 生命周期的外层调用，在回调返回后必须重新验证对象注册/有效状态；Component Tick 仅在仍 registered 时调用 Super。
- 2026-07-25 P50 独立复审发现重入卸载 UAF：`wasm_runtime_call_wasm` 期间的动态 Host callback 可以经自定义 `UFUNCTION` 重入 reload/unload；旧 Backend 会立即销毁 exec env、module 和全局 registry attachment，Session 也会替换正在执行的 LiveRuntime。Prevention：WAMR Backend 固定维护 active-call depth 并将物理 unload 延迟到最外层返回；Runtime Session 用 mutation/guest-execution lease 拒绝 `reentrant_operation`；Component release 在 Session operation 活跃时延迟，三层回归分别验证回调内 unload、reload 和组件销毁。
- 2026-07-25 P50 最终 Gate typed-owner fixture 漂移：`TypedOwnerValidation` 仍用零 import `GSessionCompatibleModule` 搭配声明 `avid_owner_get_handle` 的 manifest，却期待后续 legacy package capability 拒绝；统一 exact import policy 正确地先返回 `manifest_wasm_import_mismatch`，250 项完整 Automation 因此 1 项失败。Prevention：测试某个 package/capability 后置门禁时，WASM 必须真实导入目标能力且 manifest 精确匹配；新增安全前置门禁后固定检索所有 `RequiredImports` 手工赋值，并验证对应 bytecode import section。
- 2026-07-25 P50 close 隐私扫描误报：`Test-AvidScriptPhasePrivacy` 扫描了完整 diff，已删除的 Windows 账户绝对路径仍会触发；secret 正则又缺少标识符左边界，把 `RequiredDescriptorSchemaToken` 后缀误判为明文 token，导致已通过 Gate 的 attestation 无法 close。Prevention：内容隐私扫描只检查 `--unified=0` 的新增行并排除 `+++` header；password/secret/token 必须按独立或 snake_case 标识符匹配；合同测试同时覆盖删除旧路径、SchemaToken 合法标识符和新增真实私有路径拒绝。
- 2026-07-25 P50 Git 对象身份猜测：回滚失败 attestation 时根据短 hash 手工补写了错误的 40 位对象 ID，`git revert` 以 `bad object` 拒绝且未改动仓库。Prevention：Git 对象完整身份只通过 `git rev-parse <short-hash>` 获取；命令本身接受无歧义短 hash 时直接使用短 hash，禁止人工扩写。
- 2026-07-25 P50 parser 验证包装插值错误：汇总 PowerShell parser 错误时在双引号内写 `$file:`，冒号被解析成变量作用域语法，导致包装命令本身 ParserError，目标脚本尚未被检查。Prevention：诊断文本统一用 `-f` 格式化（`'{0}:{1}: {2}' -f ...`）或 `${file}` 明确变量边界；验证包装器必须先成功解析，不能把 wrapper 失败计入产品 parser 结果。
- 2026-07-25 P50 隐私回归测试自触发：新增“删除旧账户路径应允许”用例时，把扫描器应拒绝的路径形态直接写进 tracked 测试源码；阶段扫描正确拒绝了测试文件本身。Prevention：隐私扫描负例在测试运行时由多个不敏感片段拼成，仓库 diff 不保存完整敏感形态；新增 privacy fixture 后必须立即对真实 phase commit range 调用生产扫描器，不能只在隔离 fixture 内断言。
- 2026-07-25 P50 复审发现 `+++` 内容绕过：新增行若原始内容以 `++` 开头，unified diff 数据行同样以 `+++` 开头；首版用 `StartsWith('+++')` 排除文件 header，会连同这类真实新增内容一起跳过。Prevention：通过 Git `--output-indicator-new=>` 给新增数据行使用独立指示符，不再按 `+++` 猜测 header；安全合同必须覆盖以 `++` 开头的私有路径或 secret 内容并确认仍被拒绝。
- 2026-07-25 P50 复审发现彩色 diff 绕过：独立新增行指示符修复后仍未显式关闭 Git 颜色；仓库配置 `color.ui=always` 时 ANSI 转义位于 `>` 前，`StartsWith('>')` 选中 0 行并静默绕过全部内容隐私模式。Prevention：所有用于机器解析的 Git diff 固定传入 `--no-color`；合同测试在 fixture 内强制 `color.ui=always`，仍必须拒绝新增账户路径。
- 2026-07-25 P50 隔离工作树复制路径重复：shell 已在 `phase50-finalize` 工作树内运行，却再次相对解析 `.worktrees/phase50-finalize`，形成重复目录并让受保护基线复制命令在写入前失败。Prevention：切换 `workdir` 后把 `(Get-Location).Path` 作为 target；source 使用已验证的主仓库绝对路径，复制前分别打印并断言 source/target 不相等且目标位于当前 worktree。
- 2026-07-25 P50 复审发现 binary/textconv 隐藏：即使新增行指示符和彩色输出已固定，tracked `.gitattributes` 仍可用 `-diff` 把普通文本标记为 binary，默认 textconv 也可替换内容，导致新增敏感文本没有任何数据行可扫描。Prevention：隐私 diff 固定组合 `--text --no-textconv --no-ext-diff --no-color`，强制读取原始 blob 文本；合同测试必须证明 `-diff` 文件中的账户路径仍被拒绝。
- 2026-07-25 P50 PowerShell Git revision 表达式未引用：执行 `git rev-parse HEAD^{tree}` 时没有把 revision expression 作为单引号参数，PowerShell 拆解花括号并向 Git 传入额外垃圾参数，commit 已成功但 tree 查询失败。Prevention：包含 `^`、`{}`、`:` 或 `~` 的 Git revision expression 在 PowerShell 中统一使用单引号，例如 `git rev-parse 'HEAD^{tree}'`；身份输出再用 `git show -s --format='%H %T'` 交叉确认。
- 2026-07-25 P50 Gate evidence 验证工作树选择错误：在完全 clean 的 static Gate worktree 调用 `Test-AvidScriptGateEvidence`，但 phase state 明确要求两项 protected dirty baseline，验证器因缺失未跟踪技术状态文档而拒绝。Prevention：纯 static checker 在 clean detached worktree 执行；Gate report 的 state/protected baseline 验证固定回到复现 protected dirty 的 finalize worktree，两个职责不能混用。
- 2026-07-25 P50 ignored `Saved` 宿主位置规则复发：在 clean Gate worktree 直接启动 `BuildIntegrationTests.ps1`，宿主因缺少 ignored binding package 在执行测试前退出。Prevention：PhaseWorkflow/parser/architecture 等纯 tracked 合同运行于 exact clean candidate；依赖 `Saved` 发布产物的 Build/Publication/Prepared/Cache 宿主只在主项目运行，并先证明候选没有修改对应 tracked 脚本，Gate 日志记录等价范围与未启动的首次尝试。
- 2026-07-25 P51 同名 foreign plugin 构建证据错误：在项目已启用主目录 AvidScript 时向 UBT 追加隔离工作树的 `-Plugin=<worktree>/AvidScript.uplugin -BuildPluginAsLocal`，命令虽成功，但 action 与产物仍落在项目主插件目录，日志只有 foreign plugin 参数而没有工作树源码输入，不能证明隔离分支已编译。Prevention：同名插件工作树不使用 `-Plugin` 作为源码切换机制；实现批次在工作树执行纯源码合同，阶段批次提交合并到主插件后再按标准项目 target 做一次 no-clean producer/consumer 构建和 Automation，并以 action source path 或编译诊断证明输入归属。
- 2026-07-25 P51 factory 输入切片复审问题：首版 profile v4 resolver 允许非 Actor factory class reference，却把同一完整 class reference 数组继续交给旧 SpawnActor descriptor 路径；旧 overload 还可静默丢弃 resolved factory，BuildRequest 也未传播 factory，形成“profile 成功但产物缺能力或暴露错误 SpawnActor API”的假闭环。Prevention：新增 capability 必须从 profile -> request -> plan -> descriptor -> renderer 建立显式 owner 链；链未闭合时在第一处缺失 owner 失败关闭。Spawn class reference 与 factory class reference 按 capability 分流，禁止仅按共享类型表推断操作权限。
- 2026-07-25 P51 factory Outer 约束遗漏：首版只验证 Outer 是可加载 UClass，并对 Component 检查 Actor 派生，未检查 `ObjectClass->ClassWithin`；带 `Within` 的 UObject 可能通过生成期校验后在 `NewObject` 触发失败。Prevention：所有 generic UObject 构造计划在发布前同时验证 profile Outer constraint、UE `ClassWithin` 和操作 kind 的额外 Outer 规则；回归至少包含一个非默认 `Within` 类。
- 2026-07-25 P51 旧 profile hash 兼容遗漏：resolver version 从 50.1.0 无条件升级到 51.1.0 会让未使用 factory 的 v1-v3 profile selection hash 全部变化。Prevention：新增可选 schema capability 时，legacy 输入继续使用旧 identity domain；只有实际声明新字段的 profile 才进入新 resolver/hash version，固定兼容测试覆盖空 capability 表。
- 2026-07-25 P51 PowerShell 正则包装引号复发：descriptor v7 静态扫描把包含 `\"` 和右括号的 `rg` pattern 放进 PowerShell 双引号，包装器先报 `Unexpected token ')'`，目标扫描未执行。Prevention：所有含反斜线、引号、美元符或括号的机器检索 pattern 固定使用 PowerShell 单引号；复杂 pattern 拆成多个 `rg`，执行前先确认包装器文本可解析。
- 2026-07-25 P51 恢复入口顺序再次违反：收到恢复后的“怎么样了”先读取 skill、Git 与源码，之后才运行 `Build/InvokePhaseWorkflow.ps1 status -Phase 51`。Prevention：任何网络恢复、上下文压缩或用户状态追问后的第一条仓库调用固定为 PhaseWorkflow status；skill 与 memory 可先读，但 Git、源码和测试必须等状态机输出后再执行。
- 2026-07-25 P51 shell 逻辑拼接规则再次违反：为并行读取 Build.cs 与生命周期文件，单个 `shell_command` 内再次用分号连接两个 `Get-Content`，违背已记录的“一调用一逻辑命令”。Prevention：编排层 `Promise.all` 的每个元素只读取一个文件或一个连续区段；构造命令字符串时若出现分号，除单个外部命令的退出码包装外必须拒绝执行并拆分。
- 2026-07-25 P51 SDD Bash 路径假设错误：直接假定 Git Bash 位于 `C:\Program Files\Git\bin\bash.exe`，本机只有 WSL `bash.exe`，辅助脚本在执行前失败。Prevention：跨平台辅助脚本先用 `Get-Command bash` 定位并辨别 Git Bash/WSL；没有已验证的 POSIX host 时用 `apply_patch` 创建 ignored ledger/brief，不猜测安装路径，也不为流程辅助启动额外 WSL 环境。
- 2026-07-25 P51 长构建 timeout 配置错误：真实 UE5.8 no-clean UBT 首次只给 `shell_command` 1 秒 timeout，包装器在 5 秒下限退出并关闭日志管道，子 UBT 虽完成编译但只留下 SDK 启动行，无法作为完整 action 证据。Prevention：UBT、Automation 和 .NET build 从首次调用就使用至少 600000 ms timeout；只有工具返回 running cell 时通过 `wait` 续接，禁止用短 timeout 模拟异步。
- 2026-07-25 P51 Automation 多过滤器语法错误：试图在 `ExecCmds` 中重复 `Automation RunTests`，UE5.8 把首个 `Automation` 后的分号内容作为 Automation 内部命令，后两段因此报告 Unknown Automation command；随后读取引擎源码才确认多过滤器应在单个 `RunTests` 参数中用 `+` 分隔。Prevention：多个 Automation filter 固定写成 `Automation RunTests FilterA+FilterB;Quit`；阶段模板保存该语法，不再通过重复 console command 猜测。
- 2026-07-25 P51 descriptor v7 serializer fixture 缺 Self：生产 canonical serializer 正向测试构造了非空 `class_references`，却只发布 UObject 根且遗漏 `self_type_id`，共享 v6+ parser 按既有 lifecycle 合同在 `self_type_id` 拒绝。Prevention：descriptor 正向 fixture 从当前 working generator 结构派生；只要包含 class reference，至少发布 UObject -> Actor 图与 Actor Self，除非目标测试明确验证 capability 分流后的 factory-only 无 Self 合同。
- 2026-07-25 P51 prepared semantic 工作树产物前置条件遗漏：在隔离工作树直接运行 `PreparedSemanticContractTests.ps1`，该宿主依赖主项目 `Saved` 下已发布的 gameplay binding package，测试在进入合同断言前以 missing package 退出。Prevention：依赖 ignored 发布产物的 PowerShell 宿主固定在主项目候选源码合入并发布 package 后运行，或显式传入已验证的 `-BindingPackagePath`；隔离工作树只运行纯 tracked 合同。
- 2026-07-25 P51 architecture gate 提交时机与 token 漂移：在候选仍 dirty 时运行 evidence-aware architecture checker，除预期 dirty evidence 拒绝外，还暴露 generator canonical serializer 和 pointer-filtered class reference 重构后旧 token 合同未同步。Prevention：实现期间先用 parser/定向静态检查；每次 owner 重构同步更新 architecture token 到新的生产 owner，正式 architecture evidence 只在候选提交形成精确 tree 后执行。
- 2026-07-25 P51 Editor test header 路径猜测复发：根据 include 名直接读取不存在的 `Source/AvidScriptEditor/Public/AvidScriptEditorCSharpBindingEmitterTestTypes.h`，真实 owner 位于 `Private/Tests`。Prevention：即使源码已经出现 basename include，首次读取 owner 仍先用 `rg --files <module> | rg -F <basename>` 定位，不从 include 可见性猜 Public/Private。
- 2026-07-25 P51 正向 UObject factory 夹具绕过 resolver：generator/emitter/slice 正向测试直接传 resolved spec 时使用 `UConsole` 与 UObject Outer，但 UE5.8 的 `UConsole` 声明 `Within=GameViewportClient`；只有 profile resolver 入口会提前拒绝，直接 generator 路径可生成随后在 runtime activation 失败的 descriptor。Prevention：普通 UObject 正向夹具统一使用已有 concrete、默认 `Within=UObject` 的测试类；`UConsole` 仅用于 ClassWithin 负例；任何正向 factory class 在写测试前读取 UE `UCLASS` 声明确认 abstract、Within 和 kind。
- 2026-07-25 P51 class reference 能力重叠：首版 v7 parser 只要求 class reference 满足“factory-owned 或 Actor-derived”，没有禁止两者同时成立；renderer 会为重叠项发布 Actor lifecycle，runtime 却优先按 factory 处理并最终拒绝 Actor class，形成可解析、可发射但不可激活的 descriptor。Prevention：descriptor v7 对每个 class reference 强制 Actor lifecycle 与 object factory capability 恰好一个成立，并在 parser/runtime 共用类型图判定；架构门禁固定检查 XOR 条件，回归覆盖能力重叠。
- 2026-07-25 P51 Runtime 测试 helper 的 unity 冲突：新增 `AvidScriptObjectFactoryPlanTests.cpp` 在匿名命名空间定义通用 `MakeObjectType`，与同模块旧测试 helper 同名；独立阅读看似文件私有，但 UE adaptive unity 把两个 `.cpp` 合并后报重复定义。Prevention：Automation `.cpp` 的匿名命名空间 helper 仍使用测试域唯一前缀；新增测试文件在集中 UBT 前先检索同模块 helper 名，且最终必须保留 unity build 验证。
- 2026-07-25 P51 负例同时破坏上游合同：集中 Automation 中 TypedProjectApi 通过移除 Self 具体类型 ordinal 来测试 class-reference 错误，实际先命中 `self_type_id`；factory 继承错配又把 result type 改成 Actor，先命中新加入的 capability XOR。Prevention：每个负例只改变目标字段且保持所有前置合同有效；需要测试 runtime 反射约束时先构造 parser 合法的 descriptor，并按 parser -> immutable plan -> activation 的真实校验顺序审查预期 category。
- 2026-07-25 P51 renderer 验证顺序与手工 type graph 排序遗漏：class reference 的未知 `result_type_id` 先经类型图查询返回非 Actor，renderer 因而静默跳过该引用；runtime 继承负例手工把 Console type 追加到末尾，又违反 object type ordinal 必须按 class path 严格递增的 descriptor 合同。Prevention：renderer 在 capability 分类前先验证 result type 存在、为 object handle、有 ordinal 且 class path 对应；手工构造 v6+ type graph 后统一按 canonical class path 排序并重编连续 ordinal。
- 2026-07-25 P51.2 并行只读路径未先定位：读取 ownership 相关测试类型时直接猜测其位于 Bindings 模块，实际 owner 在 Runtime；一个 `Get-Content` 失败又通过 `Promise.all` 使整组证据丢失。Prevention：首次读取未知 basename 必须先用 `rg --files` 定位；允许单项路径未命中的探索组逐项捕获结果，不能让独立读取互相取消。
- 2026-07-25 P51.2 单命令规范再次违反：核对工作树时把 `git status` 与 `git log` 用分号放进同一 `shell_command`，重复了已记录的流程错误。Prevention：只读 Git 状态、分支、日志也分别作为独立调用并由编排层并行；提交命令前检查字符串中的命令分隔符，除命令自身语法外出现分号即拆分。
- 2026-07-25 P51.2 UE collector header 路径猜测：核对 `FReferenceCollector` API 时直接猜测 `CoreUObject/Public/UObject/ReferenceCollector.h`，该文件不存在，导致该项查询失败。Prevention：引擎 basename 或类型 owner 不确定时先从 `C:/UnrealEngine/Engine/Source` 用 `rg --files` 或类型声明检索定位；不能根据命名空间和相邻 `GCObject.h` 推测物理文件。
- 2026-07-25 P51.2 导出纯接口特殊成员遗漏：把 `IAvidScriptObjectOwnershipDomain` 标记为 `AVIDSCRIPTBINDINGS_API`，却保留隐式构造与 inline default 析构；Runtime 作为 DLL consumer 因而引用了未由 Bindings 导出的构造/析构并在链接时报 LNK2019。Prevention：跨模块导出的非 UObject 接口显式声明构造/虚析构，并在 owner 模块 `.cpp` 提供 out-of-line 定义；新增 public ABI 后必须执行 producer+consumer 链接验证。
- 2026-07-25 P51.2 增量 GC 强引用类型过时：首版 `FGCObject` 账本用裸 `UObject*` 调用 `FReferenceCollector::AddReferencedObject`，UE5.8 明确警告 incremental GC 下可能随机崩溃。Prevention：UE5.8 的长期 GC bridge 使用 `TObjectPtr<UObject>` 并调用对应 collector overload；涉及 GC 的新代码把 C4996 当作阻塞错误，不能作为普通 warning 延后。
- 2026-07-25 P51.2 producer/consumer 模块构建范围误判：新增 Bindings 导出实现后只用 `-Module=AvidScriptRuntime` 重建 consumer，UBT 没有自动编译新增的 Bindings `.cpp`，Runtime 因旧 DLL 继续报同一 LNK2019。Prevention：跨模块 ABI 变更按依赖顺序显式 no-clean 构建 producer 再构建 consumer；不能把 consumer module build 当成依赖模块源码构建证据。
- 2026-07-25 P51.2 已知路径错误复发：恢复后已经有“未知 basename 先 `rg --files`”规则，仍直接猜测 Runtime Session 位于 `Private` 根目录，且用未逐项容错的 `Promise.all` 让其余成功读取结果一并丢失。Prevention：任何恢复摘要给出文件职责但未给精确路径时，读取前仍先索引；并行探索读取逐项捕获失败，只有全部路径已由索引确认后才允许 fail-fast 批次。
- 2026-07-25 P51.2 路径防错规则同轮未机械执行：记录上一条后又按概念名猜测 `AvidScriptBindingPackage.h` 与 `AvidScriptBindingInvoker.h`，两个路径均不存在。Prevention：未知 owner 的读取命令不得手工录入路径；必须直接复制本轮 `rg --files` 输出中的完整路径，若索引没有目标 basename，则先按符号检索 owner，不能把概念名转换成文件名尝试。
- 2026-07-25 P51.2 Phase state 路径再次猜测：已经索引出 `Docs/Phase51/Phase51_State.json`，仍额外尝试不存在的 `Build/PhaseState/phase-51.json`。Prevention：同轮索引已返回唯一目标时禁止再构造候选路径；后续命令参数必须逐字使用索引结果，状态读取统一由 `InvokePhaseWorkflow.ps1 status` 或该已确认文档承担。
- 2026-07-25 P51.2 索引外扩展名补全复发：`rg --files` 只返回 inline 定义的 `AvidScriptObjectRegistryTestTypes.h`，仍尝试读取并不存在的同名 `.cpp`。Prevention：索引结果是允许读取路径的白名单；不得根据已确认 basename 继续补全另一扩展名，只有符号检索明确返回新 owner 时才加入读取批次。
- 2026-07-25 P51.2 重复结构补丁锚点过宽：factory import 追加块只锚定通用的 object-type 尾部与相邻大括号，被应用到 `TryResolveObjectFactory`，首次 Bindings UBT 因 `Package` 与计数变量越界失败。Prevention：长实现文件中的块移动或插入必须同时锚定目标函数签名和唯一尾部字段（本例 `LoadDescriptor` 与 `OutResult.BindingCount`）；应用后用函数级源码切片确认新增块位于预期作用域，再进入 UBT。
- 2026-07-25 P51.4 C# slice owner 路径猜测复发：按类名直接读取不存在的 `Private/BindingGeneration/AvidScriptEditorCSharpBindingSliceService.cpp`，实际 owner 位于 `Private/CSharpBuild`，导致并行检索批次提前失败。Prevention：未知实现文件首次访问只能逐字使用当前轮 `rg --files` 返回的路径；批次构造前检查每个文件参数都来自索引结果，禁止按相邻职责目录推断。
- 2026-07-25 P51.4 Guest lowerer 文件名推断失败：为检查 enum/控制流支持直接读取不存在的 `CSharpFunctionLowerer.cs`，实际 lowering 职责分布在 `CSharpOperationLowerer`、`CSharpControlFlowLowerer` 等文件。Prevention：未知符号先用 `rg -n` 定位 owner，未知文件先用 `rg --files` 建立路径白名单；概念职责名不得转换成候选文件名。
- 2026-07-25 P51.4 profile header 目录猜测第三次复发：已知实现位于 `Private/BindingGeneration` 后仍据此读取同目录下不存在的 `AvidScriptEditorProjectBindingProfile.h`，真实 header 位于模块 `Public`。Prevention：首次读取任何未由本轮索引确认的文件都先执行独立 `rg --files`；shell 命令中的每个源码路径必须能回溯到本轮索引输出，无法回溯则禁止调用并先定位。
- 2026-07-25 P51.4 新增 Bindings `.cpp` 未进入缓存 action graph：标准 no-clean `-Module=AvidScriptBindings` 只重编旧 unity translation unit，新 `AvidScriptSceneAttachmentBinding.cpp` 没有进入缓存 UBT makefile，链接因此报告三个新增导出符号未解析。Prevention：模块新增或删除 `.cpp` 后首次 no-clean producer build 固定追加 `-NoUBTMakefiles` 重新 gather 源文件图；它不是 Target clean，后续未改变文件集合的构建恢复普通增量参数。
- 2026-07-25 P51.4 attachment renderer 混用 class-reference result/base 语义：SceneComponent capability 分类使用 `Reference.ResultTypeId`，但该字段有意指向公开 base return type；具体 SceneComponent 若公开 ActorComponent base 会被误判为非场景组件，导致 attachment imports 缺失并触发 `slice_binding_missing`。Prevention：factory 具体能力分类固定从 `ClassReference.ClassPath` 对应的 concrete object-type 节点开始遍历；`ResultTypeId` 只用于 class-reference 的公开返回合同，不能代替具体 factory class 身份。
- 2026-07-25 P51.4 P51.3 factory slice 能力链未闭合：runtime slice 会保留完整 factory table，但 import provenance 验证只识别 lifecycle/object-type/owner，真实 C# 首次调用 `ObjectConstruct` 即以 `slice_binding_missing` 失败；先前回归只传入未使用 factory 的 manifest，未覆盖实际调用。Prevention：新增 capability family 必须同步 renderer、runtime package、slice ordinal 验证和“真实 used import”回归；slice 固定按 reflected、lifecycle、object type、factory、attachment 顺序逐族校验稳定身份。
- 2026-07-25 P51.5 测试模块目录猜测复发：按对象工厂测试的概念名直接读取 Bindings 模块下不存在的测试文件，真实 owner 位于 Runtime 模块。Prevention：即使恢复摘要已经给出测试职责，首次读取仍必须逐字使用本轮 `rg --files` 输出；概念所属模块不能替代物理路径索引。
- 2026-07-25 P51.5 benchmark 类型图简化过度：首版基准 descriptor 把 `UStaticMeshComponent` 的直接父类写成 `USceneComponent`，遗漏 `UMeshComponent -> UPrimitiveComponent`，生产 parser 以 `binding_object_type_base_mismatch` 正确拒绝。Prevention：性能夹具与功能夹具使用相同的真实反射类型图合同；新增引擎类型前从 `UClass::GetSuperClass()` 链或现有生成产物确认每一层直接父类，不以常用公开基类替代 descriptor 的 direct-super 语义。
- 2026-07-25 P51.5 Runtime 重复条件补丁锚点过宽：为 root-component import 增加 ownership 前置检查时只锚定通用 `HostContext.ObjectRegistry` 条件，补丁误插入更早的 Actor location handler。Prevention：长 Runtime 文件中重复前置条件的修改必须同时锚定完整函数签名和目标 import 名；应用后立即按函数区段复读，禁止仅用新增字符串检索确认位置。
- 2026-07-25 P51.5 恢复摘要路径层级沿用错误：复核派生切片补丁时使用了不存在的 `Private/AvidScriptEditorCSharpBindingEmitter.cpp` 与 `Private/AvidScriptEditorCSharpBindingSliceService.cpp`，实际文件分别位于 `Private/BindingGeneration` 与 `Private/CSharpBuild`。Prevention：恢复后也必须先把 `rg --files` 结果作为路径白名单；摘要中的职责或 basename 不能替代当前工作树的精确路径索引。
- 2026-07-25 P51.5 恢复后的状态机顺序遗漏：本轮先在隔离工作树检查补丁并提交，之后才运行 `InvokePhaseWorkflow.ps1 status -Phase 51`，没有遵守网络/上下文恢复后的首命令规则。Prevention：任何恢复摘要后的首个仓库动作固定只允许 PhaseWorkflow status；状态输出确认下一动作后才检查 Git 或源码，即使摘要已给出 stage 也不例外。
- 2026-07-25 P51.5 `rg` 连字符 pattern 参数边界错误：检索 `-Multiprocess` 时没有先写 `--`，修正时又把 `-g` 放在 `--` 后，造成两次只读失败。Prevention：`rg` 参数顺序固定为 options、glob、`--`、pattern、paths；pattern 以连字符开头时不得临时调整该顺序，命令模板先在文本层检查一次。
- 2026-07-25 P51 状态基线工作区选择错误：Phase 51 从干净隔离工作树执行 `start`，导致 `protected_dirty` 为空，未记录主插件中持续存在的三份用户改动。P51.5 在 freeze 前用 PhaseWorkflow 同一 helper 从主插件重建 path/status/worktree/head hash 基线。Prevention：新 Phase 的 `start` 固定在用户实际项目插件根执行；实现可在 worktree 进行，但阶段状态的 protected baseline 必须代表最终 build/Automation/close 所在工作区。
- 2026-07-25 P51 protected baseline 回填后的校验工作区错误：把主插件三份用户文件写入状态后，仍在隔离工作树执行 status；该工作树没有主插件的未跟踪技术状态文档，状态 schema 因路径不存在而正确拒绝。Prevention：状态一旦绑定实际主工作区 protected baseline，后续 status、freeze、attest、close 只在该主工作区执行；隔离工作树只负责源码和文档提交，不再运行阶段状态机。
- 2026-07-25 P51.5 Windows 通配路径错误再次复发：查找 BuildPipeline 与 object factory 测试时两次把 `AvidScriptEditorCSharp*Tests.cpp` / `AvidScriptObjectFactory*Tests.cpp` 作为 `rg` 路径参数，触发 Win32 path error。Prevention：源码路径位置机械禁止 `*` 和 `?`；文件过滤只能放在路径前的 `-g`，命令提交前逐个扫描 path 参数。
- 2026-07-25 P51.5 HostEffect 接口 header 猜测：符号检索已返回 `AvidScriptBindingReloadEffect.h`，同一命令仍继续读取猜测的 `AvidScriptBindingHostEffects.h` 并失败。Prevention：一个探索命令不得把“搜索未知 owner”和“读取猜测 owner”合并；先接收 `rg` 结果，下一条命令只复制确认路径。
- 2026-07-25 P51.5 Git 只读命令再次混合：审查复修后把 `git diff --check`、`git diff --stat` 与 `git status --short` 放进同一 shell 调用，违反仓库单逻辑命令约定。Prevention：即使三者都只读，whitespace、规模与状态仍分别调用；执行前按换行拆分外部命令，循环只用于同一类文件读取。
- 2026-07-25 P51.5 紧凑切片误作 C# 编译引用：为压缩 object type 表，一度让 final build 直接针对 runtime slice 重新执行 Frontend/Semantic；源码中不可达 helper 仍需完整授权 API 才能通过 Roslyn，完整 Automation 因而在编译期失败。Prevention：授权 package 始终承担完整 C# 编译表面并保持稳定 ordinal；runtime slice 只裁剪动态 binding，并通过 Guest IR provenance 发布 `active_object_type_ordinals`，加载器仅激活可达类型闭包，不用缩窄编译引用换取运行时规模。
- 2026-07-25 P51.5 HostContext 所有权合同夹具遗漏：root-component import 升级为必须经过 ownership domain 后，两个直接 Runtime fixture 仍只注入 registry，完整 Automation 才发现正向用例失败。Prevention：`FAvidScriptWasmHostContext` 新增或收紧 capability 时，用符号检索审计全部直接构造夹具；会产生 borrowed/owned handle 的正向夹具必须注入真实 session ownership 并显式 Cleanup，缺失域的 fail-closed 行为留给独立负例。
- 2026-07-25 P51.5 Actor fixture 补丁锚点过宽复发：给 `DirectHandlerSmoke` 增加 ownership 时只锚定通用 HostContext 初始化，首次补丁落入更早的 external-file 测试，随后 cleanup 引用了目标函数中不存在的变量。Prevention：同文件重复 fixture 修改必须把完整 Automation 测试函数签名纳入 patch 上下文；应用后先按 ownership 声明、注入、cleanup 三元组检索并确认都位于同一函数，再进入编译。
- 2026-07-25 P51.5 重复声明锚点规则未机械执行：记录上一条后，新增 `InactiveObjectType` 仍只锚定重复的 `LoadedSlice` 声明，变量落入后面的 factory 测试并导致集中 UBT 失败。Prevention：重复声明周围的补丁禁止只使用一行上下文；至少包含当前 `IMPLEMENT_*` 测试类名或前后唯一断言，提交前用符号检索核对声明行号必须早于且接近所有使用点。
- 2026-07-25 P51.5 UE 容器 API 未验证：descriptor parser 直接使用了并不存在于当前 UE5.8 `TArray` 的 `CountByPredicate`，直到 UBT 才发现。Prevention：使用不在仓库既有代码中的 UE 容器便利 API 前先检索引擎 5.8 header；简单计数优先写显式 range-for，避免为一行代码引入版本假设。
- 2026-07-25 P51.5 全局 unity 开关扩大构建范围：为刷新公共 model ABI 使用 `-DisableUnity` 并同时指定三个插件模块，UBT 仍把该开关应用到完整 Editor 依赖图，生成 4065 个 actions；终止外层工具会话后 UBT 子进程还继续运行。Prevention：窄范围 ABI 刷新只更新已确认插件 consumer `.cpp` 的时间戳并执行普通 no-clean 模块构建；禁止使用全局 unity 开关。终止构建包装器后必须按精确 target 命令行复查并结束对应子进程树，不能假定父会话退出等于 UBT 已退出。
- 2026-07-25 P51.5 通用 TypeId 被误当作对象 TypeId：运行时激活闭包对每个 binding 返回值和参数无条件调用对象类型解析，`void` 稳定 ID 按描述符合同不进入 `types` 表，导致所有包含 void 返回的切片以 `binding_object_type_required_missing` 失败。Prevention：对象激活闭包只消费 `kind == object_handle` 的 binding value；struct、enum、scalar 与 void 仍参与 ABI/描述符校验，但不得进入 `UClass` 激活图。正向回归必须包含 void 返回与对象参数/返回两类函数。
- 2026-07-25 P51.5 隔离工作树补丁未进入项目构建：修复只存在于 Phase worktree 时，直接从 `AvidTPSTemplate.uproject` 运行 UBT；项目实际加载主插件目录，Bindings 增量构建成功却仍是旧源码，随后完整重复了同一组五项失败。Prevention：项目级 UBT/Automation 前先核对待验证 commit 已快进到实际 `Plugins/AvidScript` 工作区；未合入的 worktree 候选只能运行自有静态/.NET/PowerShell 验证，不能把主项目构建结果当作该候选证据。
- 2026-07-25 P51.5 派生描述符绕过 canonical serializer：切片闭包重算身份后用通用 `FJsonSerializer` 把新字段写回原对象，`active_object_type_ordinals` 因插入顺序落在对象末尾，随后统一 emitter 以 `descriptor_not_canonical` 拒绝。Prevention：任何 descriptor 字段增删或身份重算后都必须由唯一 `FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical` 生成最终字节；禁止用 DOM 局部改写承担发布路径，通用 JSON parser 只用于读取或明确的负向 fixture。
- 2026-07-25 P51.5 最终 Guest IR 与 runtime slice 未交叉绑定：bootstrap 已把可达 object type ordinal 写入 slice，final lowering 也重新提取 ordinal，但 build pipeline 只比较 imports，未比较两份对象类型集合。Prevention：binding package resolver 必须读取 hash 保护的 `active_object_type_ordinals`；final build 对显式激活集执行严格有序的精确集合比较，不一致以稳定 category 失败并删除 WASM/manifest；架构门禁与负向合同固定覆盖该跨制品关系。
- 2026-07-25 P51.5 许可证只读审计命令混合：确认 MIT 文件时在一条 shell 字符串中用分号连接 `git ls-files` 与 `Get-Content`，再次违反单逻辑命令规则。Prevention：即使两个查询属于同一审计主题，Git 跟踪状态与文件内容读取仍拆为独立工具调用；提交 shell 前机械拒绝命令分隔符。
- 2026-07-25 P51.5 PowerShell 函数调用与布尔运算混写：active ordinal resolver 首版在 `if` 条件中直接把多行函数调用接到 `-and`，parser 将运算符归入命令参数并报告缺少括号。Prevention：PowerShell 条件内的命令调用先赋给命名布尔变量，再用纯表达式组合；所有修改过的脚本在运行前统一执行 AST parser 预检。
- 2026-07-25 P51.5 active ordinal 负例破坏上游 schema：BuildIntegration 自动选中的通用 gameplay 授权包仍为 schema v3，首版 fixture 直接添加 `active_object_type_ordinals`，测试先命中 schema v6+ 门禁而非目标 `ASBI4304`。Prevention：跨 schema 负例必须显式构造满足所有上游 parser 合同的最小目标 schema，再只改变待测字段；失败后核对首个稳定 category，不能只看退出码。
- 2026-07-25 P51.5 PowerShell 数组强制转换掩盖 JSON shape：resolver 用 `@($Value)` 遍历 `active_object_type_ordinals`，使 JSON 标量 `0` 被静默包装为单元素数组，而 UE runtime parser 会拒绝同一制品。Prevention：所有 JSON array 合同必须先用 `-is [System.Array]` 验证原始 `ConvertFrom-Json` 值，再执行 PowerShell 数组包装和元素校验；跨工具链解析器必须对 JSON shape 保持一致。
- 2026-07-25 P51.5 非 canonical 负例无法证明跨制品合同：首版 object-type provenance 测试把 legacy schema 手工升级并重写 descriptor，只更新文件 hash，既没有保持真实 generator canonical 输出，也只覆盖 Runtime 多激活方向。Prevention：跨生成制品的一致性回归必须选取真实发布且 hash 完整的 canonical authorization/runtime package，两个集合差异方向分别覆盖；手工 fixture 仅用于单一 malformed shape，并明确验证首个稳定 category。
- 2026-07-25 P51.5 PowerShell cmdlet 参数压缩错误：只读检索把 `Select-Object -First` 压成不存在的 `Select-Object-First`，导致 package 探索无结果。Prevention：PowerShell cmdlet 名与参数始终保留空格，复杂 pipeline 按既有格式分行；不要为缩短一次性命令压缩标准参数边界。
- 2026-07-25 P51.5 TryCast ordinal provenance 漏检：真实 Guest IR 已保留 `avid_object_type_is_a` 与 ordinal 常量，但首版提取器只扫描 `object_type_ref` nominal constant，导致 generated `TryCast` 所需 UClass 不进入 runtime slice 激活集。Prevention：最终 Guest IR provenance 同时追踪 typed object capability constant 与 `avid_object_type_is_a` 的第三个直接非负 int32 常量；bootstrap/final 共用同一提取器，真实 cast 负例必须证明 Guest 多用方向被拒绝。
- 2026-07-25 P51.5 `rg` 复合正则未闭合：检索 call lowering 时手写带转义括号的 regex，包装字符串遗漏闭合组，`rg` 在读取源码前退出。Prevention：只查固定语法 token 时优先 `rg -F`；需要复合 regex 时先缩小为多个简单 pattern，避免在 PowerShell 包装层同时维护两套转义。
- 2026-07-25 P51.5 已有 Phase 文档名再次猜测：目录尚未索引时直接读取不存在的 `P51.1_Component_Ownership_Plan.md`，真实文件名为 `P51.1_Object_Component_Ownership_Implementation_Plan.md`。Prevention：Phase 文档首次读取固定先列出 `Docs/PhaseNN` 或使用 `rg --files`，后续命令只复制索引返回的完整文件名，不按标题缩写路径。
- 2026-07-25 P51.5 Guest provenance 失败路径不一致：首版 TryCast ordinal 修复让 malformed intrinsic 抛出未处理异常，却仍静默忽略 malformed `object_type_ref`；提取又发生在 WASM 生成后，异常路径没有统一清理或稳定报告。Prevention：两类 ordinal 来源必须严格解析并共用异常出口；调用点捕获后执行 `Remove-LoadableArtifacts`，发布稳定 `ASBI4305 / guest_object_type_provenance_invalid` 报告，再以 1 退出。
- 2026-07-25 P51.5 architecture source slice 锚点不唯一：Guest provenance failure gate 用 `$RequiredExports = @(` 起始，先匹配到文件前部空数组初始化，并被紧邻的 `$ObservedExports = @()` 提前截断。Prevention：`Get-SourceSlice` 的 start/end token 必须包含目标 owner 的唯一语义标识，例如 `$GuestIrModel.exports`；新增 slice 后先检索 token 出现次数，不能只依赖前缀看似明确。
- 2026-07-25 P51.5 许可证审计查询再次混合：检查 README 尾部、Git 跟踪状态与 LICENSE 内容时把三个不同查询放入同一 shell 调用，违反一调用一逻辑命令。Prevention：文档内容、Git metadata 与许可证文本分别调用；即使都属于发布审计，也不能以同主题为由合并外部命令。
- 2026-07-25 P51 最终 Static Gate 基线滞后：Phase 51 已审查并验证 factory-aware renderer、borrowed object registry 与 profile regeneration，但 `TestPhase50Architecture.ps1` 的 frozen hash 和调用序列仍停在 Phase 50，首次冻结候选因此 5 项失败并必须 reopen。Prevention：每个 Phase 修改冻结闭包后，在 batch-complete 前运行 checker 的 `Hashes` 与 `Gate` 两种模式；新 hash 只能在审查实际 diff、确认行为属于本阶段且聚焦/完整测试通过后更新，禁止仅按失败输出机械替换。
- 2026-07-26 P51 最终 Gate Git revision 表达式未引用：PowerShell 把未引用的 `HEAD^{tree}` 拆成 revision 与 script block，随后生成无关的 encoded command 并让已成功提交后的只读命令返回失败。Prevention：PowerShell 中所有包含 `{}`、`^`、`~` 或 `:` 的 Git revision/pathspec 都作为独立单引号参数传入，例如 `git rev-parse 'HEAD^{tree}'`；提交与身份读取分开执行，不能让后者掩盖提交结果。
- 2026-07-26 P51 最终 Gate 预期非零子进程被外层偏好改写：PowerShell Gate 包装器启用 `$PSNativeCommandUseErrorActionPreference = $true`，PhaseWorkflow 合同用于验证拒绝路径的非零 `powershell.exe` 被提前转为异常，形成 18 个假阴性，且包装器未检查脚本 `$LASTEXITCODE` 仍错误写出 passed marker。Prevention：包含预期非零子进程的合同宿主固定在隔离 `powershell.exe -NoProfile -File` 进程执行，外层保持 native error preference 关闭、逐宿主检查退出码；完成 marker 只能在全部退出码为 0 后写出。
- 2026-07-26 P51 最终 Gate 日志初始化使用删除操作：唯一 Gate 目录本可先验证目标不存在，命令仍加入 `Remove-Item` 清理日志并被安全策略在执行前拒绝。Prevention：Gate run 使用不可复用的 commit/run-id 目录，创建前 `Test-Path` 断言不存在；不删除或覆盖既有证据，重试使用 `AttemptN` 文件名并保留审计链。
- 2026-07-26 P51 protected baseline 校验工作区错误复发：Gate report 首次用 detached candidate 调用 evidence helper，而状态绑定的未跟踪用户文档只存在于主插件，正确触发 `ASPW4006`。Prevention：state/status/freeze/attest/close 以及任何会读取 `protected_dirty` 的 evidence helper 只在主插件根运行；detached candidate 仅运行不读取阶段状态的静态和自有测试。
- 2026-07-26 P52 规划阶段 reflected type policy 路径猜测：未先索引就读取不存在的 `AvidScriptEditorBindingTypePolicy.*`，真实 owner 是 `AvidScriptEditorReflectedTypePolicy.*`。Prevention：即使概念 owner 明确，首次读取 basename 仍先执行 `rg --files` 并逐字复制结果；架构探索不得把概念名转换成文件名。
- 2026-07-26 P52 start 前代理文件未先提交：架构、计划、closeout 与 AGENTS 仍未提交时执行 `start`，PhaseWorkflow 正确把当前全部 dirty path 记录为 protected baseline，导致 7 项而不是用户原有 3 项。无效 state 在提交前删除并重建。Prevention：新 Phase 固定两步启动：先提交代理创建的 architecture/plan/closeout/流程规则并确认 status 只剩用户改动，再在主插件执行 `start` 并单独提交 state；不得把代理未提交文件混入 protected baseline。
- 2026-07-26 P52.4 Git 写操作再次用分号连接：记录实现批次时把精确 `git add` 与 `git commit` 放在同一 shell 字符串，虽然只暂存了 Phase 状态文件，但破坏了一调用一逻辑命令的审计边界。Prevention：暂存、提交、身份读取各自使用独立工具调用；执行 shell 前机械扫描 `;`、`&&`、`||`，Git 工作流命令不得包含这些分隔符。
- 2026-07-26 P52.4 .NET SDK 启动器选择错误：首次使用系统 `dotnet`，而仓库 `global.json` 固定 8.0.416，系统安装仅有 9.0.306，测试在编译前失败。Prevention：AvidScript 的 .NET 构建与测试固定使用 `$env:USERPROFILE\.dotnet\dotnet.exe`；仅在显式验证 SDK 探测失败行为时使用 PATH 中的 `dotnet`。
- 2026-07-26 P52.4 `rg` 单行正则误含 `\n`：检索 C# 属性语法时在默认单行模式的 pattern 中放入换行转义，`rg` 在读取文件前拒绝。Prevention：源码结构检索拆成多个单行 token；确需跨行时显式使用 `-U` 并先验证正则，否则不在 pattern 中写 `\n`。
- 2026-07-26 P52.4 Automation 启动器超时过短：首次聚焦 UE Automation 把外层工具超时设为 1 秒，工具在 5 秒最小窗口终止等待，但 `UnrealEditor-Cmd` 子进程继续运行。随后先按精确项目命令行确认唯一子进程并等待结束，没有并发启动第二份。Prevention：UE Automation 首次调用固定使用至少 10 分钟工具超时；若外层意外退出，先枚举精确 `UnrealEditor-Cmd` 命令行并等待或读取现有日志，禁止直接重启。
- 2026-07-26 P52.5 恢复协议顺序再次偏移：上下文恢复后的第一条仓库命令先递归查找 `AGENTS.md`，之后才执行 `InvokePhaseWorkflow.ps1 status -Phase 52`。虽然没有修改仓库，但破坏了状态机唯一下一步的机械保证。Prevention：恢复后允许先查非仓库 memory；进入仓库后的第一条命令固定直接在已知插件根运行 PhaseWorkflow status，`AGENTS.md`、Git status 与文件索引均排在其后。
- 2026-07-26 P52.5 PowerShell 临时 parser 包装器变量边界错误：错误输出模板写成 `"$path:$line"`，冒号被 parser 解释为变量名的一部分，使包装器在读取目标脚本前失败。Prevention：PowerShell 双引号字符串中变量后紧跟冒号时固定使用 `${path}:...` 或格式化运算符；合同脚本 parser 结果只以修正后包装器的真实输出为准。
- 2026-07-26 P52.5 module-scoped UBT 链接目标遗漏：修改 `AvidScriptBindings.cpp` 和 Editor 测试后只指定 `-Module=AvidScriptEditor`；UBT 编译了 Bindings unity 并生成 `.lib`，却没有链接 `UnrealEditor-AvidScriptBindings.dll`，首轮 Automation 因而加载旧实现并产生 6 项假失败。单独执行 `-Module=AvidScriptBindings` 链接 DLL 后同一测试通过。Prevention：模块实现发生变化时，UBT 命令必须显式包含并最终链接该 producer module；不能把依赖对象已编译或 consumer DLL 已链接当作 producer DLL 已刷新，启动 Automation 前从日志逐个确认所有变更模块的 `Link ... .dll` action。
- 2026-07-26 P52.5 `rg` 上下文参数再次混用：引擎头检索把 PowerShell `Select-String -Context` 的长参数写给 `rg`，命令在读文件前失败。Prevention：`rg` 上下文只使用 `-C <n>`、`-B <n>` 或 `-A <n>`；命令提交前按所选工具校验参数集合，不在两个检索器之间凭记忆搬参数。
- 2026-07-26 P52.5 reload manifest 头文件名猜测：为确认 load result 字段直接读取不存在的 `AvidScriptWasmReloadManifest.h`，真实 owner 为索引返回的 `AvidScriptWasmReloadTypes.h`。Prevention：任何首次出现的类型 owner 先用 `rg -n <Type> Source -g '*.h'` 或 `rg --files` 定位，禁止从类型名推导文件名。
- 2026-07-26 P52.5 UE 容器 API 禁令复发：真实样例回归首版又对 `TArray` 使用当前 UE5.8 不存在的 `CountByPredicate`，尽管 P51 已记录相同错误。Prevention：本仓库机械禁止新增 `CountByPredicate` token；计数统一使用显式 range-for，静态架构检查在冻结前检索并拒绝该 token。
- 2026-07-26 P52.5 并行 agent 隔离失效：三个声明为 disjoint write set 的 worker 实际共享同一个 Phase worktree；其中一个执行提交时把另一个 worker 的 descriptor/profile 未暂存改动一并纳入临时提交 `10d5bfc`，导致提交边界失真。worker 在收到禁止重写消息前已把这个未推送提交改写为范围正确的 `b89459b`，descriptor/profile 随后由 owner 独立提交为 `330e8c2`；代码未丢失，但产生了不必要的历史竞争。Prevention：后续并行编码 worker 默认各自使用独立 Git worktree；确需共享 worktree 时，任务提示和验收都必须要求 `git add -- <owned paths>`，提交前用 `git diff --cached --name-only` 与 ownership 清单精确比对，禁止 `git add -A`、`git commit -a` 和任何全仓暂存。
- 2026-07-26 P52.5 Phase worktree 构建指向错误：在实现仍只存在于 `.worktrees/phase52-property-binding` 时直接以主项目 `.uproject` 调用 UBT；项目只发现主插件目录，因此返回 `Target is up to date`、零 action，未编译 Phase 候选。该调用不计入有效 UBT 证据。Prevention：执行 UE 构建前必须先用 `git -C <project-plugin> rev-parse HEAD` 与候选 commit 对比；不一致时先确认 protected-dirty 无路径重叠，再 fast-forward 集成候选，或使用明确加载候选插件的隔离项目。只有变更 producer module 出现实际 `Compile`/`Link ...dll` action，或能证明对应 binary 已由同一候选生成时，才计入构建证据。
- 2026-07-26 P52.5 UE5.8 checked-format 编译失败：profile identity fixture 把运行时 `FString` 解引用后传给 `FString::Printf` 作为格式串；UE5.8 要求 format 在编译期可检查，因此 Editor 模块编译失败。Prevention：`FString::Printf` 的第一个参数只允许 `TEXT(...)` 字面量或编译期拼接字面量；需要动态模板时使用结构化 JSON API，测试内的小型固定 JSON fixture 直接保留字面量格式串。静态检索在冻结前拒绝 `Printf(` 后以 `*` 开头的动态格式参数。
- 2026-07-26 P52.5 C# 样例误用临时 struct setter：首版双向属性样例写成 `UE.Self.CustomTimeDilation = ...`；`UE.Self` 是零分配 `readonly struct AActor` 的属性返回值，Roslyn 按 C# `CS1612` 将该赋值投影为 `IInvalidOperation`，真实 profile build 因 `ASCS2001` 失败。Prevention：当前 facade 模型下每个事件入口先缓存 `AActor self = UE.Self`，属性读写和函数调用复用局部 receiver；样例文档必须说明该约束。ref-return Self 只有在 Guest IR、静态状态与热重载语义完整设计后才能单独立项，不能在阶段收尾时临时改对象模型。
- 2026-07-26 P52.5 profile 测试调用错误重载：真实样例测试先成功加载 schema v5 profile，却把 `ProfileResult.BuildConfig` 传给 config-only `BuildProfile`；解析出的 writable selection、class reference 与 profile identity 因而被丢弃，pipeline 退回旧 EngineGameplay 只读授权包，局部 receiver 写属性仍以 `ASCS2001` 失败。Prevention：从 profile load result 发起构建必须先调用 `FAvidScriptEditorCSharpProfileService::MakeBuildRequest(ProfileResult)`，测试输出路径只修改 `Request.Config`；架构门禁要求真实 profile gameplay 证据包含该调用。
- 2026-07-26 P52.5 样例误选未支持 UFunction：双向属性 profile 选择 `K2_SetActorLocation`，但该函数含 sweep 与 `FHitResult` out 参数，当前通用投影策略会拒绝它；profile 仍可生成其余授权包，直到 Roslyn 编译调用点才暴露缺少方法。Prevention：阶段样例只声明由 canonical descriptor 实际生成并经 facade 检查的函数；本样例改用完整支持的 `GetActorScale3D/SetActorScale3D`。显式选择被策略拒绝时的早期诊断，以及 `FHitResult`/out-struct ABI，必须作为后续通用 API 覆盖目标，不能在文档中宣称已支持。
- 2026-07-26 P53 四通道测试世界未初始化 Actor：Harness 创建 `EWorldType::Game` 后直接 SpawnActor，未调用 `InitializeActorsForPlay`；`AActor::ProcessEvent` 因 `AreActorsInitialized()==false` 静默跳过 native UFUNCTION，造成返回帧始终为零。Prevention：临时 Game World 的统一生命周期为 CreateWorld、CreateNewWorldContext、SetCurrentWorld、InitializeActorsForPlay，再创建测试 Actor；反射调用失败时同时核对输入帧、目标、Function 与 World actor initialization。
- 2026-07-26 P53 裸 Runtime 缺失对象所有权服务：性能 Lane 直接使用 `FAvidScriptWasmRuntimeInstance` 并只注入 ObjectRegistry，UObject 返回在绑定写回时正确拒绝空 ObjectOwnership。Prevention：需要 UObject 借用、生命周期、事件路由或热重载语义的端到端 Lane 固定使用 `FAvidScriptRuntimeSession`；裸 Runtime 只用于明确隔离 VM 层的测试，并必须显式声明所需 host services。
- 2026-07-26 P53 flow capture 测试只统计 opcode：原测试证明 capture 可被 `local_store` 覆盖，却未验证赋值左侧 capture 应回写其捕获的原 local/parameter；条件表达式拆 CFG 后循环累加值因此停留在初始种子。Prevention：flow capture 回归必须区分 value capture 与 address capture，并断言 store 的精确 TargetId；真实 Guest 工作负载保留条件表达式循环作为端到端执行证据。
- 2026-07-26 P53 固定 SDK 规则再次复发：新增 Guest 回归后首次调用 PATH `dotnet`，因仓库锁定 8.0.416 而系统只有 9.0.306，在编译前失败。Prevention：AvidScript 的所有 .NET 命令从构造时即使用 `%USERPROFILE%\.dotnet\dotnet.exe`，不得先尝试 PATH host。
- 2026-07-26 P53 frame lifecycle 优化局部诊断变量遗漏：同一 helper 在 BlueprintSetter 分支复用了已有 `ValueDetails`，普通 UFunction 分支复制调用时也引用该名称，但该分支只在后续参数循环内声明局部 `Details`，集中 UBT 以未声明标识符失败。Prevention：跨分支复制 package-build 校验时，每个分支在调用点声明语义专用诊断变量；提交候选前对新增标识符执行作用域检索，集中 UBT 仍作为最终编译证据。
- 2026-07-26 P53 Puerts 锁身份受工作树换行影响：受管标记最初在 LF 锁文件上生成，Windows `core.autocrlf` 工作树把同一 Git 内容展开为 CRLF 后，Remove/Verify 以原始文件字节重算 SHA-256 并错误拒绝合法安装。Prevention：文本锁身份统一使用去 BOM、CRLF/CR 规范化为 LF 后的 UTF-8 SHA-256，并用 `.gitattributes` 固定锁文件 `eol=lf`；跨换行 Verify 合同必须通过，不能把平台工作树编码差异当成依赖篡改。
- 2026-07-26 P53 Puerts 内容摘要遗漏真实构建生成目录：安装摘要只排除顶层生成目录，首次 UBT 又在唯一 C# 辅助项目的 `Source/CSharpParamDefaultValueMetas/obj` 写入 MSBuild 状态，导致正式 runner 在采样前把合法构建判为依赖篡改。Prevention：仅豁免该已确认 `.csproj` 的精确 `bin/obj` 前缀，并保持其他嵌套 `Binaries/Intermediate` 纳入摘要；依赖合同必须覆盖“已安装、完成真实构建后仍可 Verify”。
- 2026-07-26 P53 `pwsh -File` 布尔参数绑定误判：正式 runner 的 `[bool]$AvidScriptDirty` 通过新 `pwsh -File` 进程调用时，`$false` 与 `0` 都作为字符串传入并在脚本主体前失败。Prevention：需要布尔或复杂类型参数的 PowerShell 脚本在当前 PowerShell 会话用 call operator `& <script> -Flag $false` 调用；只有纯字符串参数脚本使用 `pwsh -File`。
- 2026-07-26 P53 benchmark profile 文件名猜测：检查 runner 默认 profile 时直接读取不存在的 `PuertsBenchmarkProfile.json`，真实跟踪文件为 `Config/BenchmarkProfile.json`。Prevention：benchmark 配置首次读取也必须先用 `rg --files Benchmarks/PuertsComparison` 定位，不能根据脚本职责推导文件名。
- 2026-07-26 P53 installer/sidecar 摘要分隔符协议漂移：installer 用双引号格式串产生真实 tab，sidecar 用单引号格式串把 `` `t `` 保留成字面字符；各自的本地合同都能自洽，真实正式链却对同一 1077 个文件生成不同 SHA-256。Prevention：跨进程/跨脚本 manifest 必须通过 installer 生成 marker、sidecar 原样重算的交叉合同；PowerShell 需要转义符语义的字符串禁止使用单引号。
- 2026-07-26 P53 TypedProjectApi 测试依赖 LF：测试从跟踪 profile 读取文本后用包含 `\n` 的整块字面量替换，Windows clean worktree 展开为 CRLF 时替换数量为零，最终 Automation 才暴露四项连锁失败。Prevention：测试只要对仓库文本执行精确多行匹配，读取后先把 CRLF/CR 规范化为 LF；发布 Gate 必须在全新 Windows 工作树运行完整 Automation，不能只依赖原工作区已有换行状态。
- 2026-07-26 P53 最终 Gate benchmark 脚本路径猜测：已经存在 `Benchmarks/PuertsComparison/Scripts/New-PuertsBenchmarkProject.ps1`，仍按职责猜成不存在的 `Build/New-PuertsBenchmarkProject.ps1`，产生一次无效读取。Prevention：阶段专用脚本首次调用固定先用 `rg --files` 定位，后续路径逐字复制索引结果；通用 `Build` 目录不能作为 benchmark/release 脚本的默认猜测位置。
- 2026-07-26 P53 最终 Gate 长哈希人工补全错误：只读取了短提交 `ad6190f` 后手工补写其余字符传给 benchmark 工程身份门禁，门禁以 `ASP53B1103` 正确拒绝，未创建工程。Prevention：commit/tree/SHA-256 等发布身份必须直接使用 `git rev-parse` 或哈希工具的完整输出并由变量机器传递；禁止从短标识人工补全或重录长身份。
- 2026-07-26 P53 隔离工程 C# 产物生成入口错误：首次从物理 worktree 路径调用 `BuildCSharpActorLifecycle.ps1`，脚本据自身路径推导的项目根不属于短路径 Gate 工程，完整 Automation 因 debug-map provenance 拒绝该产物；改从隔离工程 `Plugins/AvidScript` junction 入口调用后定向与全量测试均通过。Prevention：脚本通过自身位置推导 ProjectRoot 时，隔离工程必须从该工程内的插件 junction 路径调用；生成后先验证 manifest 中所有 artifact path 都是相对隔离项目根的 canonical 路径。
- 2026-07-27 P54.3 受控运行时校准卡在临界线：首版校准以一组三次中位数刚达到 5 ms 就冻结迭代数，再用另一组 seed 复测；V8 和 Wasmtime 均可能因正常计时抖动跌回门槛下，且 1000 万上限不给现代 JIT 留余量。Prevention：冻结复测未达门槛时必须加倍迭代并重新执行两组确认，直到稳定通过或明确耗尽上限；PC JIT 对照 profile 至少保留 1 亿次迭代上限，正式采样前必须先完成独立校准进程的真实编排冒烟。
- 2026-07-27 P54.3 诊断 profile 与 aggregate schema 矛盾：runner 明确提供 `-AllowNonFormalProfile`，但 aggregate schema 把 5 process、30 sample、600 observation 与数组长度全部写成常量，导致 1 process 冒烟完成真实计时后仍无法汇总。Prevention：JSON Schema 只约束诊断与正式数据共同的安全边界和结构关系，正式样本规模由 tracked profile、request/result validator 与静态 formal contract 三方强制；每个正式 benchmark runner 在冻结前必须用缩小 profile 完成端到端 aggregate 冒烟。
- 2026-07-27 P54.4 只读命令再次使用分号：读取 static host import 头源文件时把两个 `Get-Content` 用分号放进同一 `shell_command`，同时其中一个路径未经索引确认而不存在。Prevention：shell 调用提交前机械扫描 `;`、`&&`、`||`，每个外部读取独立调用；未知 owner 先用 `rg --files` 定位，下一条命令只复制已确认路径。
- 2026-07-27 P54.4 Git 身份查询重复违反两项既有规则：在同一 `shell_command` 用分号连接 commit、tree、status 三个查询，并再次未引用 `HEAD^{tree}`；commit 查询成功，但 PowerShell 把 tree expression 拆解为额外 encoded command，后续查询失败且不能作为证据。Prevention：Git commit、tree、status 各自独立调用；tree 固定使用 `git rev-parse 'HEAD^{tree}'`，执行前拒绝含命令分隔符的 shell 字符串。
- 2026-07-27 P54.5 WSL Git 无法解析 Windows worktree 元数据：确认 Ubuntu 已安装后直接在 WSL 调用 SDD shell helper，但该 worktree 的 `.git` 文件包含 Windows 盘符 `gitdir`，WSL Git 将其拼接为无效路径并在创建账本前失败。Prevention：POSIX helper 依赖 Git 时，先在同一路径执行最小 `git rev-parse --show-toplevel` 兼容性探测；Windows worktree 若失败，使用等价 PowerShell 步骤创建同样的自忽略目录、brief、ledger 与 review package，不再把“WSL 可启动”视为“可解析 Windows Git worktree”。
- 2026-07-27 P54.5 Puerts 安装路径沿用旧摘要猜测：审计 `FBlueprintContextTracker` 时直接检索当前 worktree 中不存在的 `Benchmarks/.../Plugins/Puerts/Source` 与 `Plugins/Puerts/Source`，命令在读取前失败。Prevention：第三方 managed dependency 的物理位置可能位于短路径 Gate 工程或阶段证据目录；每轮首次访问必须先用 `rg --files` 或受管依赖 marker 定位当前安装根，摘要中的旧工程相对路径只能作为搜索关键词，不能直接作为 shell 路径。
- 2026-07-27 P54.5 Windows 通配路径禁令复发：定位 dynamic host-call result 时再次把 `Source/AvidScript*` 作为 `rg` 路径参数，Win32 在检索前以非法路径拒绝。Prevention：`rg` 的路径位置机械只允许已确认存在的字面目录或文件；模块范围使用 `Source` 加 `-g 'AvidScript*'`/文件扩展过滤，提交命令前扫描所有 path 参数并拒绝 `*`、`?`。
- 2026-07-27 P54.5 独立只读区段再次合并：审查 descriptor identity 与 parser 时把两个 `Get-Content` 作为换行分隔的两条命令放进同一次 `shell_command`；命令成功且未修改文件，但绕过了一调用一逻辑读取的审计边界。Prevention：不仅扫描 `;`、`&&`、`||`，还要按 shell 换行拆分顶层命令；即使读取同一文件的两个区段，也分别调用并让每次输出只对应一个审查问题。
- 2026-07-27 P54.5 Runtime classifier 直接访问 editor-only 字段：qualified-native classifier 无条件读取 `UClass::ClassGeneratedBy`，该字段在 UE5.8 仅由 `WITH_EDITORONLY_DATA` 提供，独立审查发现 packaged/mobile target 会编译失败。Prevention：Runtime 模块首次使用 `UClass`/`UFunction` 数据成员前必须核对 UE5.8 声明周围的 build guards；editor-only 辅助证明封装成带相同宏的 load-time helper，非 editor target 返回保守且由 runtime class flags 继续约束的结果。
- 2026-07-27 P54.5 阶段报告文件名再次凭记忆补全：检索 P54.4 命令时直接读取不存在的 `P54.4_Crossing_Fast_Path_Implementation_Report.md`，实际 tracked 文件名为 `P54.4_Crossing_Fast_Path_Report.md`。Prevention：摘要只给出文档职责或不确定 basename 时，首次访问必须先用 `rg --files <directory>` 定位，并把返回路径作为后续读取白名单；禁止按相邻阶段命名惯例补全 `Implementation` 等词。
- 2026-07-27 P54.5 benchmark direct 证据由名称猜测和 workload 推算：五通道实现用 SHA-256 `StableId.Contains(FunctionName)` 查找计划，初始化必然找不到；样本 direct/fallback 计数又按 workload 和迭代数推算，没有读取真实 dispatch。集中 Gate 同时发现缩小 sidecar profile 未包含 `scalar_add_int32`，新增 direct 负例因此选择空 sample。Prevention：binding package 提供通用 `UClass + FName -> ordinal` 不可变查询；direct/fallback 证据只来自 dispatcher 成功路径的可选调用方计数器；benchmark fixture 至少包含一个 qualified workload，并从 workload 数公式同步矩阵规模，禁止以字段存在或预期规则代替真实执行证据。
- 2026-07-27 P54.5 PowerShell regex 与多路径读取未遵守输入白名单：一次双引号 `rg` pattern 中的 `$Profile` 被 PowerShell 展开为 profile 路径并破坏 regex；随后又在一个多路径检索中加入未经 `rg --files` 确认的 `AvidScriptWasmReloadManifest.h`。Prevention：包含 `$` 的 regex 固定使用单引号；多路径读取的每个 path 都必须来自本轮索引结果，任一未确认路径都不得加入调用。
- 2026-07-27 P54.5 依赖布局在首次 UBT 后改变：先生成 UBT makefile，再安装 Wasmtime managed dependency，后续增量构建没有可靠感知新库布局。Prevention：阶段候选在首次 UBT 前完成并验证全部受管依赖；依赖安装器输出稳定 stamp 并作为 Build.cs 的真实输入，布局变化后只失效相关 target makefile，不清理 Editor target。
- 2026-07-27 P54.5 模块构建证据不足以启动正式项目：module-scoped UBT 刷新了插件 DLL，但项目 target receipt 与 `UnrealEditor.modules` 的 BuildId 仍可能陈旧，commandlet 因而加载旧模块。Prevention：实现期可使用 module-scoped 构建；阶段末运行 commandlet、Automation 或 benchmark 前，对同一候选执行一次 no-clean project target build，并核对 receipt 与全部 producer module 的 BuildId。
- 2026-07-27 P54.5 读取了陈旧 UBT 日志：一次诊断使用 `%LOCALAPPDATA%\UnrealBuildTool\Log.txt`，该文件来自旧 UE5.6，导致分析偏离当前构建。Prevention：只读取当前 UBT invocation 明确输出的日志路径；本项目默认核对 `C:\UnrealEngine\Engine\Programs\UnrealBuildTool\Log.txt` 的时间戳、引擎根与命令行，拒绝跨引擎版本的缓存日志。
- 2026-07-27 P54.5 新 benchmark 工程未显式启用 Harness：工程生成器复制插件后没有把 `AvidScriptPerfHarness` 写入 `.uproject`，后续 target build 无法保证生成其 receipt。Prevention：`New-PuertsBenchmarkProject.ps1` 幂等添加并启用 Harness，project contract 同时验证 JSON、源工程不被修改和二次执行稳定。
- 2026-07-27 P54.5 canonical 再生成丢失 direct 授权：Prepare 重新解析 profile 时只保留普通函数选择，没有重建 native-direct class rule，生成 descriptor 后 direct binding 被降级。Prevention：所有 canonical regeneration 必须从同一 selection profile 模型重建普通选择与 direct 子集；正式 Prepare 保留真实 direct descriptor，并由 profile/descriptor contract 覆盖。
- 2026-07-27 P54.5 binding slice 丢失 dispatch mode：切片服务只按显式函数列表重新生成 descriptor，native-direct binding 变为 semantic，触发 `slice_selection_mismatch`。Prevention：切片闭包重新生成必须使用完整 `FAvidScriptBindingSelectionProfile`，保留 class rule、direct function subset 与 canonical identity；合同同时包含一个 direct getter 和一个 semantic setter。
- 2026-07-27 P54.5 Puerts 摘要纳入 UBT 生成属性文件：正式 runner 在构建后重验依赖时，把 `Source/CSharpParamDefaultValueMetas/CSharpParamDefaultValueMetas.ubtplugin.csproj.props` 当成上游源码篡改。Prevention：installer 与 sidecar 共享精确、最小的生成文件排除规则，只排除已确认由 UBT 产生的 props 和既有受控 bin/obj；交叉合同覆盖真实构建后的再次 Verify。
- 2026-07-27 P54.5 正式构建外层 timeout 过短：包装进程提前返回时子 UBT 仍继续执行，增加了重复探测和误启动第二份构建的风险。Prevention：UE5.8 target build、Automation 和正式 benchmark 首次调用固定使用覆盖最坏增量构建的长 timeout；只有工具返回可恢复 cell 才等待，外层异常后先按精确命令行确认既有子进程。
- 2026-07-27 P54.6 evaluator 跨行命令解析失败且被后续成功掩盖：hashtable value 中把 `Get-FileHash -LiteralPath` 与实参拆行却未添加续行符，AST parser 正确报错；同一 shell 调用末尾的合同测试成功又让总退出码成为 0。Prevention：PowerShell named-argument 命令跨行必须使用反引号、splatting 或完整子表达式；修改后的每个脚本先在独立进程执行 AST parser，检查该进程退出码后才运行其他合同，禁止把 parser 与后续命令串在同一 shell 调用。
- 2026-07-27 P54.6 Phase 50 Gate 模式名猜测：更新冻结架构合同后按习惯调用不存在的 `-Mode SelfTest`，参数绑定在测试开始前正确拒绝。Prevention：首次调用带 `ValidateSet` 的仓库脚本前先读取 param block 或帮助输出，只使用脚本声明的模式；本脚本的静态夹具入口固定为 `-Mode Fixtures`。
- 2026-07-27 P54.6 Wasmtime C ABI 断言误用 C11 语法：新增 `.c` 桥接层直接使用 `_Static_assert`，但 UE5.8 的 MSVC C 编译模式未接受该语法，首次 producer build 失败。Prevention：需要兼容 UE Windows C translation unit 的编译期 ABI 断言固定使用负数组长度 typedef，或先由同一 toolchain 的最小构建证明语法可用；不得把 C++ `static_assert` 或其他编译器的 C11 支持外推到 MSVC C 模式。
- 2026-07-27 P54.6 Unity 编译匿名辅助符号重名：两个 Editor `.cpp` 的匿名命名空间分别定义了通用名 `IsSafeIdentifier`、`IsLowerHexSha256`，源文件独立检查无冲突，但 UE Unity 合并后发生重定义；本地 `EscapeJsonString` 还与 UE JSON header 的全局辅助函数形成 ADL 歧义。Prevention：会参与 Unity 的私有辅助函数使用职责限定的文件唯一名，尤其拒绝 `IsSafeIdentifier`、`EscapeJsonString` 等通用名称；阶段末必须保留标准 Unity project-target build 作为唯一编译证据。
- 2026-07-27 P54.6 UE5.8 目录移动 API 与引擎头路径再次猜测：实现时假定 `IPlatformFile::MoveDirectory` 存在，诊断时又把 wildcard 和不存在的候选头路径直接交给 shell；真实 UE5.8 公开接口是可同时移动文件与目录的 `IFileManager::Move`。Prevention：引擎 API 首次使用先由 `C:\UnrealEngine\Engine\Source` 的符号索引确认声明与签名，搜索路径只允许已确认存在的字面目录；不得把方法职责、候选文件名或 wildcard 当作 API 证据。
- 2026-07-27 P54.6 `rg` 无匹配被编排层误判为检查失败：负向残留检索按设计返回退出码 1，却与后续 `git diff --check`、status 串在同一 JavaScript 顺序脚本中，首个调用抛错后跳过其余检查。Prevention：`rg` 负向断言不与必须执行的检查共享编排调用；发布检查优先执行显式正向符号清单与独立 `git diff --check`，需要证明零匹配时单独解释退出码 1，不能让它阻断后续门禁。
- 2026-07-27 P54.6 UE5.8 Automation AutoSDK 规则复发：仓库 P50 已明确源码版 Editor 自动化必须携带 `-Multiprocess -NoCompile`，本轮仍按通用参数直接启动，导致测试队列前再次被 VisionOS `MainVersion` 全平台校验阻断。Prevention：构造任何 `UnrealEditor-Cmd.exe` Automation 命令前先匹配 `AGENTS.md` 的固定模板，强制包含 `-Multiprocess -NoCompile -NoLiveCoding -DDC-ForceMemoryCache`；命令审计缺少任一固定参数时不得启动进程。
- 2026-07-27 P54.6 property-only Automation 夹具误解空函数选择：测试 profile 只添加 property，却把空 `IncludeFunctions` 当成“不选择函数”；真实 resolver 合同中空集表示遍历该类全部声明 UFunction，descriptor 因而合法包含三个额外 semantic binding，测试又按数组首项误报 generated property 失败。Prevention：property-only 夹具必须显式排除测试类全部声明函数，后续断言按 owner、member、binding kind 与 dispatch mode 联合定位，不能用 package 总数或首个 setter 代替目标身份。
- 2026-07-27 P54.6 descriptor 篡改测试依赖 JSON 空白格式：已有 `ParseDescriptor`/`SerializeDescriptor` 结构化 helper，仍用无空格字符串片段替换 pretty-printed JSON，替换次数为零后把原 descriptor 送回 parser，产生假失败。Prevention：JSON/manifest 篡改测试一律解析对象、修改明确字段并重新序列化；执行拒绝断言前必须先证明目标字段确实发生变化。
- 2026-07-27 P54.6 Runtime 夹具实例化抽象 `UObject`：Self capability 测试直接调用 `NewObject<UObject>()`，UE5.8 Automation 以 handled ensure 拒绝抽象类实例化。Prevention：对象生命周期测试使用最小具体类型（本例 `AActor`），首次选择引擎基类前检查 `CLASS_Abstract`，handled ensure 也必须按测试失败处理。
- 2026-07-27 P54.6 typed host package 使用非稠密 ordinal：夹具把唯一 dynamic import 标为 ordinal 7，违反共享 VM package 的 `Import.Ordinal == array index` 合同，导致 Wasmtime typed 路径在实例化前被正确拒绝。Prevention：需要验证非零 binding ordinal 时补齐每个较低 ordinal 的唯一合法 padding import；所有 backend 夹具先通过 `ValidateAvidScriptVmBindingPackage` 的稠密性、身份和签名合同，再验证专用 trampoline。
- 2026-07-27 P54.6 concrete owner 修复遗漏 Automation 比较类型统一：Self capability 夹具从抽象 `UObject` 改为 `AActor` 后，两处 `TestEqual` 仍以 `UObject*` actual 对比 `AActor*` expected，模板参数推导歧义导致增量 UBT 失败。Prevention：替换测试对象具体类型时同步审查注册、解析和比较处的静态指针类型；基类 API 的期望值显式上转为同一基类指针再交给 Automation 模板。
- 2026-07-27 P54.6 property-only profile 的空函数接管遗漏：首次修复仅在测试中排除 UFunction，却未先核对 `GenerateFromProfile` 对 resolver 失败类别的接管条件；class rule 存在时函数 resolver 返回 `selection_empty`，代码只允许 `profile_empty` 继续到 property resolver，合法 property-only profile 因而失败。Prevention：多 surface pipeline 的空选择必须按职责区分，函数 surface 的 `profile_empty` 与 `selection_empty` 在确有 property/self/class/factory surface 时都可继续；回归必须覆盖 class property-only profile，而不是保留无关 control function 掩盖耦合。
- 2026-07-27 P54.6 typed padding 身份未复用已验证 fixture：为 ordinal 7 临时生成七个零填充 SHA/import 名，没有先用共享 package validator 的已知通过布局作最小验证，首个 padding 仍被合同拒绝。Prevention：typed trampoline 只需证明非零 ordinal 时使用其他 backend 已验证的 ordinal 1 加固定 `2` padding；扩大 ordinal 矩阵前先为 padding factory 建独立 package-validator 合同，不能在端到端测试中同时引入新的身份生成方式。
- 2026-07-27 P54.6 data-lane renderer 使用非规范 int32 token：setter annotation 条件写成 `CanonicalType == "int32"`，而同文件类型解析和 descriptor 合同均使用 `scalar:i32`，导致真实 generated property 不发布 `buffered_write` 元数据。Prevention：类型分支只引用 shared canonical type 常量或与同 owner 的 `ResolveValueType` token 保持一致；端到端 reachability 测试必须从 profile、descriptor、IR 一直断言到 C# attribute，不能只检查 attribute class 声明。
- 2026-07-27 P54.6 PowerShell 双引号正则转义复发：检索 `CanonicalType` 时在双引号 shell 字符串内混用反斜杠转义 C++ 引号，PowerShell 先改写参数，`rg` 收到未闭合分组并失败。Prevention：包含 `$`、括号或双引号的 `rg` pattern 固定使用 PowerShell 单引号，先用最小关键词正向检索再收窄；不得按 C/JSON 的反斜杠规则推断 PowerShell 引号。
- 2026-07-27 P54.6 主插件 Gate 未编译外置 Perf Harness：项目 target 首次启用 `AvidScriptPerfHarness` 后才发现 correctness smoke 仍按旧四参数调用 `FPuertsLane::Initialize`，而正式 runner 已改为六参数实物身份接口。Prevention：任何修改 benchmark harness 公共或内部接口的阶段，在正式采样前必须用隔离 benchmark 工程编译 Harness；主插件 build 不能替代外置插件编译证据，正确性 smoke 与正式 runner 必须同时进入调用点检索。
- 2026-07-27 P54.6 候选 worktree 缺少受管 Wasmtime 安装：主插件目录已有 ignored SDK，隔离 benchmark 工程却指向干净 Phase worktree；首次 UBT 因该 worktree 没有 `wasmtime.h` 失败。Prevention：创建隔离 benchmark 工程后、首次 UBT 前，固定对工程 junction 指向的候选根运行 `InstallWasmtimeDependency.ps1 -Mode Verify`；若仅缺安装，则使用已校验缓存执行 `Install` 后再次 `Verify`，不得把主插件目录的 ignored 依赖状态外推到其他 worktree。
- 2026-07-27 P54.6 Frontend JSON 默认深度无法承载真实脚本：现有 7 项 Frontend 测试只覆盖浅语法树，475 行 benchmark C# 的合法 AST 超过 `System.Text.Json` 默认 64 层，Frontend 在发布 artifact 时抛未处理 `JsonException`；即使单独放宽 writer，Semantic 的 `JsonDocument.Parse` 也会在同一默认深度拒绝。Prevention：Frontend artifact 的 writer 与所有 reader 共享一个显式、受限的最大深度；回归同时覆盖深控制流序列化和 Semantic CLI 消费，阶段 benchmark 在 UE Prepare 前先用真实 workload 执行一次独立 Frontend 冒烟。
- 2026-07-27 P54.6 跨工具项目共享常量遗漏命名空间：Semantic 已有 Frontend project reference，但新增 `FrontendSerializer.MaximumDepth` 时未在 `SemanticCommandLine.cs` 导入 `AvidScript.CSharpFrontend`，聚焦 .NET 测试在运行前编译失败。Prevention：跨命名空间复用类型时补丁前先检查目标文件现有 using 与类型全名；改动后先构建最小 producer/consumer 项目，再启动较重的 UE Prepare。
- 2026-07-27 P54.6 共享 project reference 的 .NET 测试错误并行：Frontend、Semantic 与 Guest 三个 `dotnet run` 同时构建 Release 依赖图，多个进程争写 `AvidScript.CSharpSemantic.dll`，产生与代码无关的 `CS2012` 文件锁失败。Prevention：只读且无共享输出的测试可并行；共享 project reference 和同一 configuration/obj 目录的 .NET build/run 固定按 producer 到 consumer 串行执行，除非显式分配独立输出目录。
- 2026-07-27 P54.6 Guest IR 生产者与消费者版本漂移：Guest IR 已升级为 schema 2 / IR 1.1，但 C# build 发布门、PowerShell 集成测试和 UE Automation 仍硬编码 1 / 1.0，真实 benchmark 编译完成后被错误判为 `direct_abi_contract_invalid`。Prevention：制品版本升级必须用符号检索同步所有当前产物消费者；底层 validator 可显式保留 legacy 兼容，但发布门、manifest 和正式验收固定断言当前版本，并在真实样例 Prepare 前运行一次 BuildIntegration。
- 2026-07-27 P54.6 Windows wildcard 路径再次传给 `rg`：一次合同检索把 `Content/CSharp/*.json` 作为路径参数，Win32 在读取前拒绝。Prevention：Windows 下 `rg` 路径参数只使用已确认存在的字面目录，扩展名筛选统一放入 `-g '*.json'`；提交命令前机械拒绝路径位置中的 `*` 与 `?`。
- 2026-07-27 P54.6 集成夹具错误依赖 junction 递归：为让短路径 benchmark 工程复用主工程 canonical binding packages，先在生成目录下创建子目录 junction，但 `Get-ChildItem -Recurse` 没有把它们当作普通目录遍历，夹具仍报告 package missing。Prevention：需要跨工程复用只读制品的测试暴露显式 root 参数，并继续验证每个制品 hash；不依赖 shell 对 reparse point 的隐式递归语义，也不复制或改写 canonical 制品。
- 2026-07-27 P54.6 Guest compiler 装饰器接口漂移：BuildIntegration 的 provenance mutation wrapper 使用 `@PSBoundParameters` 透传正式编译器，但正式调用新增 `DataLaneFusion` 后 wrapper 的 param block 未同步，负例在变异 Guest IR 前以未知参数失败。Prevention：测试装饰器必须镜像被包装命令的完整公开参数合同；正式编译器新增参数时用调用点检索同时更新所有 wrapper，并要求负例先证明目标变异已发生再断言稳定失败类别。
- 2026-07-27 P54.6 Windows wildcard 路径规则记录后立即复发：检索 profile 测试时又把 `AvidScriptEditorCSharpProfile*` 放在 `rg` 路径位置，命令部分输出后仍以非法路径失败。Prevention：执行每条 `rg` 前不只检查主目录，还机械检查全部尾部 path token；模块名过滤只允许 `-g '*Profile*.cpp'`，绝不把通配符附在目录或文件路径上。
- 2026-07-27 P54.6 generated S1 门禁与已实现 shape 自相矛盾：通用前置检查只接受 `value`，因此合法的 `FVector Function(const FVector&)` 在到达已实现的 `vector_value` 分支前被拒绝。Prevention：输入方向门禁明确接受 `value/const_ref`、拒绝 `ref/out`；每个新增 generated shape 都用真实 UHT 反射签名覆盖资格判断、descriptor shape 和生成 thunk，正式 profile Prepare 是阶段末必要验证。
- 2026-07-27 P54.6 canonical regeneration 遗漏 generated S1 标记：C# emitter 从 descriptor 重建 reflection profile 时只恢复 `qualified_native_direct`，generated 函数和属性被降回 semantic，合法 descriptor 因而被自身判为 `descriptor_not_canonical`。Prevention：所有特殊 dispatch mode 由统一 class-rule 重建器恢复 include 集与专用子集；generated function、generated property 和 native direct 都必须有 generator 到 emitter 的 canonical roundtrip 回归。
- 2026-07-27 P54.6 Windows PowerShell 5.1 长路径被误判为制品缺失：generated package 已完整发布，PowerShell 7 可读取，但 UE 启动的 `powershell.exe` 5.1 对 262 字符 reference source 路径返回 `Test-Path=false`，Build 报 `ASBI4202`。Prevention：Windows benchmark/CI 使用短路径工程，并限制正式 package identity 长度；错误为 missing 时同时记录绝对路径长度并用与 UE 相同的 Windows PowerShell 版本复验，不能按外层 PowerShell 7 结果推断子进程可见性。
- 2026-07-27 P54.6 runtime slice 再生成遗漏 generated S1：切片服务在 P54.5 修复 direct dispatch 后仍把特殊规则命名和实现限定为 native-direct，导致完整包中的 generated function/property 在最小授权切片里降回 semantic 并触发 `slice_selection_mismatch`。Prevention：descriptor 的 canonical regeneration、runtime slice 与 emitter 共用同一特殊 dispatch 语义清单；切片必须按请求 stable ID 同时恢复 native-direct、generated function、generated property、writable property 与完整 include 集，真实 generated/data profile Prepare 作为阶段末闭环门禁。
### 2026-07-27: generated S1 facade must follow the frozen typed ABI

- Mistake: the runtime typed-host contract had already frozen compact signatures, but the descriptor generator and C# facade continued deriving the older generic reflected ABI. This produced `(iiiii)i` for an integer pair and `(iifffi)i` for `FVector`, so a real generated profile could not attach its runtime plan.
- Prevention: treat each `generated_native_s1` shape as one end-to-end ABI contract shared by descriptor generation, C# facade rendering, VM linking, and runtime dispatch. Integer pair results return directly; vector calls use one 24-byte in-place guest buffer. Every new generated shape must assert its exact descriptor signature and emitted C# interop surface.

### 2026-07-27: runtime reflection validation must recognize generated ABI shapes

- Mistake: after fixing descriptor and facade generation, runtime reflection validation still recomputed every function signature with the generic reflected ABI and rejected the compact generated S1 signature as `binding_function_contract_mismatch`.
- Prevention: generated bindings remain reflection-validated for owner, function, flags, parameters and canonical identity, but their host signature must be validated from the frozen generated shape contract. Keep the descriptor generator, facade renderer, VM linker and runtime reflection validator covered by the same exact-signature tests.

### 2026-07-27: runtime slices need tamper-evident generated-code provenance

- Mistake: generated C++ entries were registered under the complete authorization package hash, while automatic runtime slices had a distinct package hash and no explicit way to identify their generated-code source.
- Prevention: schema v8 runtime slices that contain generated S1 bindings carry `generated_source_package_hash`; the field participates in the slice package hash and runtime registry acquisition. Never use registry-wide stable-id fallback or an unverified package search.

### 2026-07-27: generated S1 source emission belongs in the product build pipeline

- Mistake: the generated binding service existed only as a test-facing API, so `BuildProfile` could compile a generated S1 guest but never emitted or loaded the C++ call-site module needed by the runtime registry.
- Prevention: automatic project profiles detect generated S1 descriptors, deterministically emit or reuse `AvidScriptGeneratedBindings`, and fail once with `generated_binding_build_required` until the Editor target is rebuilt and restarted. Do not hide this transition in benchmark-only commandlets.

### 2026-07-27: normalize UHT module-relative headers for cross-module includes

- Mistake: generated source copied `ModuleRelativePath` verbatim, producing `#include "Public/AvidScriptPerfFixture.h"`; Unreal module public include roots already point inside `Public`, so the real project module could not compile.
- Prevention: generated binding IR strips leading `Public/` and legacy `Classes/` source-root prefixes before validating and emitting owner includes. Validate this with a real project module build, not only Engine headers or source-text tests.

### 2026-07-27: benchmark plugin binaries live under the plugin root

- Mistake: the six-lane benchmark orchestrator resolved the harness plugin root but hashed `UnrealEditor-AvidScriptPerfHarness.dll` under the project `Binaries` directory, so a valid isolated plugin build failed before calibration.
- Prevention: derive the harness module artifact from `Plugins/AvidScriptPerfHarness/Binaries/Win64` and keep that root choice in the benchmark contract test. Evidence identity paths must match Unreal's actual module ownership.

### 2026-07-27: the host owner handle is a root capability

- Mistake: generated stable-object dispatch required every resolved handle to belong to session-created object ownership. `UE.Self` is host-injected and registry-valid but intentionally not session-owned, so passing Self as a `UObject` was rejected despite matching the active owner, class and world.
- Prevention: stable borrow accepts the exact active `OwnerHandle` as a root capability; all other handles still require session ownership. Preserve exact slot and generation comparison so stale or foreign handles cannot use this exemption.

### 2026-07-27: avoid broad recursive searches across the entire engine tree

- Mistake: a recursive `Get-ChildItem` search across `C:\UnrealEngine` for optional Wasm tools timed out and produced no useful evidence.
- Prevention: search known tool directories or use `rg --files` within a bounded subtree. Do not recursively enumerate the whole source engine for a non-blocking utility lookup.

### 2026-07-27: keep PowerShell search patterns literal when grouping is unnecessary

- Mistake: a broad `rg` expression embedded in a double-quoted PowerShell command contained unmatched grouping and failed in the shell before the search ran.
- Prevention: use single-quoted literal patterns or separate simple `rg` searches for code discovery; only introduce regex grouping when it is required and locally testable.

### 2026-07-27: generated imports must update the common host-call counter

- Mistake: the generated S1 path updated dedicated hit/fallback/reject counters but not `HostImportCallCount`, so correct generated execution appeared to make only its final generic import.
- Prevention: every Wasm-to-host ABI family updates the common crossing counter exactly once at its shared dispatch/status boundary; route-specific counters remain additional evidence rather than replacements for the common metric.
- Validation: generated-route runtime tests assert both dedicated invocation evidence and the common host-call count before benchmark contracts are accepted.

### 2026-07-27: data-lane fusion must cover the generated property facade

- Mistake: the fusion pass was tested only with direct extern setter calls, while the product facade exposes a property setter wrapper around the generated import; enabled profiles therefore emitted byte-identical non-fused Wasm.
- Prevention: generated facades place matching buffered-write metadata on the property wrapper and underlying import. The semantic layer admits wrapper metadata only from non-primary reference sources, and Guest IR validates the wrapper as an exact side-effect-free forwarding shape before fusion.
- Validation: compile a product-shaped property facade and require distinct fused Guest IR/Wasm plus epoch and submit imports; direct-import unit fixtures alone are insufficient.

### 2026-07-27: data-lane imports are not generated S1 invocations

- Mistake: command-buffer submit returned through `RecordGeneratedStatus`, double-counting the common host call already recorded by the data-lane handler and polluting generated S1 hit metrics.
- Prevention: typed data-lane imports own their host-call and data-bridge metrics; generated S1 route counters are reserved for generated binding thunks.

### 2026-07-27: black-box test projects must not depend on internal ID helpers

- Mistake: a Guest compiler test referenced the internal `CSharpGuestIds` helper from a separate test assembly and failed to compile.
- Prevention: cross-assembly tests assert serialized public ID contracts such as the stable `function:` prefix, unless the production assembly explicitly grants internals access for a justified white-box test.

### 2026-07-27: compiler-injected imports need a separate exact authorization contract

- Mistake: binding-package authorization treated data-lane epoch and submit imports like user-selected UE bindings, so the first real fused profile was rejected after Guest IR generation.
- Prevention: compiler-injected imports are admitted only when the profile enables the feature and the complete frozen Guest IR identity matches: internal ID, module, name, dispatch class, optimization metadata, binding ordinal, parameters and return type. They are not added to user binding packages or accepted by name alone.

### 2026-07-27: gameplay benchmark output directories are caller-owned

- Mistake: `Invoke-Phase54GameplayBenchmark.ps1` was invoked with a fresh path that had not been created, so its fail-closed preflight rejected the run before calibration.
- Prevention: create and verify an empty unique evidence directory before invoking the orchestrator; the script writes sidecars but intentionally does not create or overwrite the evidence root.

### 2026-07-27: expected metrics must cover optimized routes

- Mistake: the data-oriented benchmark published and validated exact command counts but left its expected common host-call count at zero, weakening the evidence contract for the optimized route.
- Prevention: every optimized benchmark lane publishes an independently derived expected host-call count, validates the observed count in the runner, and repeats the route-specific derivation in the external gate evaluator.

### 2026-07-27: calibration ceilings must cover the fastest lane

- Mistake: the diagnostic micro profile capped calibration at 100,000 iterations, so the native empty-callback lane could not reach the 0.25 ms timing floor on a fast desktop CPU.
- Prevention: size diagnostic and formal calibration ceilings from the fastest zero-work lane, and freeze a contract that the formal ceiling is never below the validated diagnostic ceiling.

### 2026-07-27: each benchmark route needs its own expected-hit model

- Mistake: micro validation reused generated S1 expected-hit counts for the semantic lane, although supported route shapes differ; `scalar_noop` is semantic but not generated S1.
- Prevention: derive and externally repeat separate semantic, generated S1, and data-oriented hit contracts. Shared workload correctness does not imply identical route instrumentation.

### 2026-07-27: prepared-artifact integration tests need an explicit project root

- Mistake: the phase-end integration host was launched from the physical plugin worktree without pointing it at the main project's prepared generated-binding root, so it failed before product assertions with a missing package prerequisite.
- Prevention: tests that consume ignored project `Saved` artifacts run through a project-local plugin junction or receive the verified project artifact root explicitly; physical worktree execution is reserved for self-contained tracked tests.

### 2026-07-27: PowerShell host success is not the last native exit code

- Mistake: a phase-end wrapper checked `$LASTEXITCODE` after successful PowerShell contract scripts; one script intentionally ran rejected native child cases and left a stale non-zero value, so the wrapper stopped after reporting a pass.
- Prevention: invoke PowerShell contract hosts under stop-on-error semantics and treat thrown errors or an explicit host result as failure. Inspect `$LASTEXITCODE` only immediately after a native executable whose exit code is the contract under test.

### 2026-07-27: generated ABI surface changes must update the architecture allowlist

- Mistake: the reviewed generated FVector in/out ABI buffer was added to the renderer without updating Phase 50's exact generated-declaration allowlist, so the stage gate correctly treated it as an unreviewed bespoke wrapper.
- Prevention: every intentional generated declaration is added to both the literal-stream and exact-multiset allowlists in the same reviewed batch, followed by fixture tests before canonical hashes are refreshed.

### 2026-07-27: generated slice fixtures must model registry lifecycle

- Mistake: the binding-slice Automation fixture dynamically published a generated S1 authorization descriptor but did not register its matching complete generated package before runtime-slice publication, so the production loadability check correctly rejected it.
- Prevention: generated-slice fixtures register exact package/stable-id/descriptor/shape entries before publishing a derived slice and revoke them with scope-bound cleanup. Production validation remains fail-closed.

### 2026-07-27: runtime import-count tests must derive from route composition

- Mistake: the slice fixture kept a pre-generated-route literal count for VM imports, so it failed after the generated S1 binding became loadable even though the resulting package was correct.
- Prevention: import-count assertions derive from reflected bindings plus lifecycle and capability families, then independently assert typed-host import identity and ordinal instead of hiding route composition behind a stale integer.

### 2026-07-28: formal calibration ceilings must match the timing floor

- Mistake: the formal gameplay profile retained a 65,536-iteration ceiling while raising the timing floor to 5 ms, so the native small-frame lane could not complete calibration on a fast desktop CPU.
- Prevention: formal profiles size their ceiling from the fastest lane and timing floor, and contract tests freeze a formal gameplay ceiling above the diagnostic ceiling.

### 2026-07-28: do not terminate Unreal build wrappers with short tool timeouts

- Mistake: an initial one-second shell timeout terminated the `Build.bat` wrapper while its child build continued, causing the next invocation to wait noisily on the existing script.
- Prevention: launch Unreal builds once with a full command timeout and use the execution cell's yield/wait mechanism for progress; never use a short process timeout as a polling mechanism.

### 2026-07-28: contract assertions must load every referenced fixture

- Mistake: a new gameplay calibration assertion referenced formal and diagnostic profile variables before the test fixture loaded those JSON files.
- Prevention: add fixture loading in the same patch as assertions that consume it, then run the focused contract host before committing the benchmark candidate.

### 2026-07-28: every formal kernel feature must be loadable by every required lane

- Mistake: the twelve-kernel controlled-runtime suite required `simd128`, but the required WAMR lane was still built with SIMD disabled, so the formal suite failed only after completing ten expensive kernels.
- Prevention: derive runtime build features from the formal kernel contract before starting the suite, freeze SIMD and SIMDe flags in architecture checks and backend identity, and run the SIMD kernel as a focused blocker probe before the full formal matrix.

### 2026-07-28: Windows build scripts must not assume System32 is on PATH

- Mistake: the WAMR rebuild was launched in a valid non-interactive environment whose reduced `PATH` omitted System32, so bare `subst` failed before configuration.
- Prevention: repository Windows build scripts prepend `%SystemRoot%\System32`, call critical operating-system tools such as `subst.exe` and `findstr.exe` by absolute path, and express fetched CMake dependencies as URL plus SHA-256 so builds do not depend on an incidental Git executable in caller PATH.

### 2026-07-28: cross-platform SIMD builds must audit compiler extensions

- Mistake: enabling WAMR fast-interpreter SIMDe on Win64 exposed an upstream GCC statement expression in the `V128` conversion macro, so configuration succeeded but MSVC compilation failed deep in the SIMD opcode switch.
- Prevention: keep the conversion in an alias-safe file-level inline function, reject GCC statement expressions in the vendored fast interpreter through the architecture gate, and complete a real Win64 SIMD build before starting the formal runtime matrix.

### 2026-07-28: orchestration layers must not parse domain artifacts

- Mistake: the C# build pipeline directly loaded and parsed the generated binding descriptor so it could decide whether to emit an S1 module and inspect its package hash, violating the existing orchestration boundary.
- Prevention: descriptor file loading, parsing, empty-package decisions and package identity stay behind `FAvidScriptEditorGeneratedBindingService`; the build pipeline consumes only the service result and owns orchestration messages.

### 2026-07-28: quote Git revision expressions in PowerShell

- Mistake: an unquoted `HEAD^{tree}` revision expression was parsed by the PowerShell command layer instead of being passed literally to Git, so a post-commit identity command failed after the commit itself succeeded.
- Prevention: quote revision expressions containing braces, for example `git rev-parse 'HEAD^{tree}'`, and keep identity verification separate from state-changing Git commands.

### 2026-07-28: pass explicit inputs to ad-hoc evidence parsers

- Mistake: an ad-hoc PowerShell parser read `$args[0]` even though the command did not invoke a script with positional arguments, so the aggregate path was null.
- Prevention: bind evidence paths to named variables in the command, or invoke a checked script with explicit named parameters. Do not assume `shell_command` populates `$args`.

### 2026-07-28: discover benchmark tool paths before invoking them

- Mistake: a phase-end inspection assumed merge and evaluator scripts lived under the repository-root `Scripts` directory, while they are owned by `Benchmarks/PuertsComparison`; both reads failed before inspection.
- Prevention: resolve unfamiliar tools with bounded `rg --files` first, then invoke the returned repository path. Do not infer ownership from a script filename.

### 2026-07-28: preserve complete benchmark invocation parameters

- Mistake: the first formal micro invocation omitted the mandatory `EditorExecutable` parameter and was rejected before Unreal started.
- Prevention: derive formal invocations from one reviewed parameter block containing editor, project, profile, template and fresh output root. When repeating a sibling benchmark, change only profile and output identity.

### 2026-07-28: exercise the successful formal evaluator path

- Mistake: the centralized evaluator had only source-contract coverage for its formal path. A valid five-process result exposed three PowerShell representation bugs: equal `Compare-Object` returned null, nullable doubles were auto-unwrapped, and `OrderedDictionary` records collapsed multi-property grouping into one bucket.
- Prevention: normalize command results with `@(...)`, cast present metrics to scalar `double`, store grouping records as `PSCustomObject`, and fail unless process and cross-process matrix cardinalities exactly match the profile. Keep a focused contract for all four invariants and run the evaluator on formal evidence before publishing conclusions.

### 2026-07-28: inspect structured allocation evidence before aggregation

- Mistake: an ad-hoc path-counter summary passed the structured `allocations` object to `Measure-Object -Sum`, producing many non-terminating conversion errors even though unrelated counters were calculated.
- Prevention: inspect sample property types before aggregating; sum only numeric leaves such as `allocations.count` when the status declares them available, and omit unavailable metrics instead of coercing their wrapper object.

### 2026-07-28: run phase workflow commands in the protected-dirty owner worktree

- Mistake: the workflow CLI was first called as `help` without its mandatory `-Phase`, then `status` was run in an isolated worktree that intentionally does not contain the main worktree's protected dirty files; both preflights rejected the call.
- Prevention: read the CLI parameter block before invocation, always pass `-Phase`, and execute protected-dirty state transitions from the worktree that owns the recorded baseline after fast-forwarding reviewed commits. Use isolated worktrees for implementation and clean evidence, not for validating another worktree's local-only files.

### 2026-07-28: next actions must match every freeze-blocking debt severity

- Mistake: phase next-action derivation routed only Open/Fixing Blocker and Critical debt to `debt-update`, while the freeze transition also rejects Important debt. A valid state therefore advertised a `freeze` command guaranteed to fail.
- Prevention: derive next actions from the same severity set used by the target transition guard. Phase workflow contracts add an Important debt after all batches complete and require `debt-update` to be the unique next action.

### 2026-07-29: resume with phase status before other repository probes

- Mistake: after a resumed performance-planning turn, repository inspection started before the mandatory phase status preflight, so the session did not establish the authoritative workflow state first.
- Prevention: the first project command after every resume or context transition is `Build/InvokePhaseWorkflow.ps1 status -Phase <Number>` in the workflow owner worktree. Skill reads and non-project context may precede it, but no repository Git, file, build or test probe may do so.

### 2026-07-29: keep shell calls to one logical command

- Mistake: several read-only probes were packed into separator-heavy PowerShell invocations, making failure attribution and command audit unnecessarily ambiguous.
- Prevention: every `shell_command` contains one logical command. Use separate tool calls for independent Git, file, status and test operations; pipelines that transform one command's output remain allowed, while `;`, `&&` and `||` composition remains prohibited.

### 2026-07-29: reject shell separators before dispatch

- Mistake: Phase 55 recovery again joined independent read-only Git and file probes with PowerShell semicolons despite the existing one-command rule.
- Prevention: before dispatching any `shell_command`, scan the complete command string and reject it when it contains `;`, `&&` or `||`; status, diff, history and multi-file reads always use separate tool calls.

### 2026-07-29: verify the project-visible candidate before UBT

- Mistake: the first Phase 55 candidate UBT ran before the isolated worktree commit was integrated into the plugin directory referenced by the project, so it built the older main checkout and could not count as candidate evidence.
- Prevention: before project UBT, compare the project-visible plugin `HEAD` with the intended evidence commit and integrate the reviewed candidate first; an isolated worktree build only counts when the project explicitly resolves that worktree.

### 2026-07-29: audit every ABI consumer before formal sampling

- Mistake: the split property ABI passed generator and primary runtime checks, but the real generated profile exposed a hard-coded legacy getter signature and the Data-Oriented lane still accepted only the legacy combined setter shape.
- Prevention: every ABI shape change must enumerate generator, loader, prepared-call, semantic compatibility, Data-Oriented and lifecycle consumers, then load the semantic, generated and data-oriented C# profiles before formal sampling begins.

### 2026-07-29: never infer a full commit identity from abbreviated output

- Mistake: a follow-up command used a guessed expansion of an abbreviated commit hash and failed even though the intended commit existed.
- Prevention: obtain candidate identity with `git rev-parse HEAD` and `git rev-parse 'HEAD^{tree}'`; copy those exact values into build, benchmark and evidence commands.

### 2026-07-29: commit the previous phase before starting the next phase

- Mistake: Phase 55 started while the completed Phase 54 state file was still dirty, so the new phase captured that workflow-owned file as protected user work; the same startup commit then made it clean and rendered the Phase 55 freeze guard permanently unsatisfiable.
- Prevention: close and commit the previous phase state first, verify that only genuine user-owned changes remain in `git status`, and only then run the next phase `start` command. Phase state files must never enter another phase's protected dirty baseline.

### 2026-07-29: benchmark preflight must prepare identity, directories and every guest lane

- Mistake: early Phase 56 benchmark attempts used an abbreviated commit, referenced an output directory before creating it, and started before all semantic, generated and data-oriented C# artifacts were ready.
- Prevention: one reviewed preflight resolves the full 40-character commit and tree, creates fresh output roots, generates every required guest artifact, completes any generated-binding build, and only then launches diagnostic or formal sampling.

### 2026-07-29: owner-bound runtime fixtures must use the owner handle

- Mistake: a runtime test invoked a `SelfBound` generated route through a non-owner fixture handle, so production ownership validation correctly rejected the test.
- Prevention: tests for owner-bound routes use the session owner handle and keep non-owner handles only for explicit rejection coverage.

### 2026-07-29: timing evidence must tolerate sub-tick measurements

- Mistake: a physical timing test assumed every valid clock delta was greater than zero, which is false for extremely short operations at timer resolution.
- Prevention: timing conversion uses a one-cycle floor where a positive duration is contractually required, and tests cover zero raw delta without changing measured ordering.

### 2026-07-29: PowerShell aggregation records must be PSCustomObject values

- Mistake: physical aggregation fed `OrderedDictionary` records to `Measure-Object`, so property grouping and numeric aggregation observed dictionary entries instead of records.
- Prevention: emit `PSCustomObject` records at aggregation boundaries and keep a focused successful-path regression test for cardinality and numeric types.

### 2026-07-29: generated binding hits are not automatically fused hits

- Mistake: the Phase 56 evidence checker treated every generated S1 hit as fused, even though vector and object shapes intentionally use the dynamic path.
- Prevention: derive expected fused calls from the exact typed ABI shape and workload composition; keep vector and object calls in generated totals without adding them to fused counters.

### 2026-07-29: fused ratios must filter to samples that observed fused work

- Mistake: the evaluator divided fused revalidations by callbacks from all generated samples, diluting the ratio with vector and object workloads that cannot enter the fused typed path.
- Prevention: compute fused ratios only from samples whose fused call counters are present and positive, then assert the expected sample cardinality before evaluating the ratio.

### 2026-07-29: evidence hashes must use bytes from the measured worktree

- Mistake: a profile hash was first taken from another worktree whose line-ending materialization differed from the clean candidate, even though the logical JSON content matched.
- Prevention: all profile and source hashes come from the exact candidate worktree used for sampling; another checkout may inspect content but must not substitute its materialized bytes in provenance.

### 2026-07-31: public module layouts require producer and consumer relinks

- Mistake: a public Bindings structure gained frozen fast-path fields, but the first scoped build targeted only Runtime. UBT recompiled the dependency library without relinking the Bindings DLL, so the focused test loaded mismatched structure layouts and reported unrelated identity failures.
- Prevention: when a public cross-module type changes size or field layout, enumerate its producer and every binary consumer before testing. Build each affected module target in the same batch and verify their DLL timestamps before launching Automation.

### 2026-07-31: benchmark output-root preflight recurrence

- Mistake: a Phase 57 diagnostic invocation repeated the documented error of passing a fresh output path before creating the directory. The benchmark preflight rejected it before Unreal launched.
- Prevention: treat output-root creation as part of the immutable benchmark parameter block. Verify the fresh directory exists immediately before invoking the runner, rather than relying on the runner to create it.

### 2026-07-31: generated project modules are public ABI consumers

- Mistake: after extending `FAvidScriptGeneratedBindingEntry`, the candidate build relinked Bindings, Runtime, and Editor but launched the project with an older `AvidScriptGeneratedBindings.dll`. Its static entry array used the previous stride and crashed while registering the package before the headless generator could run.
- Prevention: public generated-binding layout changes must include every project-generated module in the consumer relink set. Rebuild the existing generated module before the first Editor launch, run regeneration, then rebuild it again from the new source before tests or benchmarks.

### 2026-07-31: separator rejection also applies inside orchestration scripts

- Mistake: a read-only Git identity probe embedded two commands with a semicolon inside a `shell_command` dispatched by the JavaScript orchestration layer, repeating the one-logical-command violation.
- Prevention: inspect every nested `shell_command.command` string before dispatch. Commit and tree identity use separate calls such as `git rev-parse HEAD` and `git show -s --format=%T HEAD`; orchestration does not relax the separator ban.

### 2026-07-31: generate native bindings from the declared source package

- Mistake: the first trusted-thunk regeneration used a runtime-slice descriptor whose package hash was `8a58...`, even though its `generated_source_package_hash` declared the complete source package `4ac4...`. The generated DLL registered the slice identity, so runtime correctly rejected it as unavailable.
- Prevention: before generating C++, read `generated_source_package_hash`. When present, resolve and generate from that complete package descriptor; derived runtime slices consume the registered source package and must never replace its native module identity.

### 2026-07-31: formal multiprocess benchmarks need a phase-scale timeout

- Mistake: the five-process Phase 56 formal micro benchmark was launched with a 120-second shell timeout. The orchestration host timed out while its final Editor child was still completing, invalidating the attempt before aggregation.
- Prevention: diagnostic one-process runs may use a short bound, but formal calibration plus five-process runs use at least a 10-minute outer timeout. After any host timeout, wait for owned Editor children to exit and restart from a fresh output directory.

### 2026-07-31: never infer a full Git object ID from its short form

- Mistake: while updating a temporary benchmark-project marker, a short commit ID was expanded with an assumed suffix before the exact object ID had been queried. The benchmark had not started and provenance would have rejected the marker, but the value was still fabricated.
- Prevention: every candidate marker, evidence file, or attestation must obtain the full commit with `git rev-parse HEAD` and the tree with `git show -s --format=%T HEAD` in separate commands. Never type or infer the remaining characters of a Git object ID.

### 2026-07-31: do not pass Git pathspec exclusions to ripgrep

- Mistake: a Runtime owner search passed Git's `:!path` exclusion syntax as an `rg` positional path, so ripgrep found the intended symbols but then exited with an invalid Windows path error.
- Prevention: ripgrep exclusions use `-g '!relative/path'`; Git pathspecs such as `:!path` are valid only for Git commands. Keep an exclusion probe standalone until its syntax succeeds.

### 2026-07-31: architecture checker PowerShell 7 host recurrence

- Mistake: the Phase 57.8 working-tree architecture probe launched `CheckAvidScriptArchitecture.ps1` with Windows PowerShell 5.1 even though the repository already records that its leading-pipe syntax requires PowerShell 7; parsing failed before any architecture assertion ran.
- Prevention: resolve `Get-Command pwsh` once and run architecture/parser gates with `pwsh -NoProfile`. Reserve `powershell.exe` for explicit Windows PowerShell compatibility contracts only; a 5.1 parser failure is never product evidence.

### 2026-07-31: Phase 57.8 Windows rg wildcard recurrence

- Mistake: a parallel source probe passed `Source/AvidScriptVM/Private/Tests/*.h` as an `rg` path argument; Win32 rejected that branch with OS error 123 and the orchestration call reported failure despite useful sibling output.
- Prevention: before dispatching parallel probes, mechanically reject `*` or `?` in every `rg` path position. Search literal roots such as `Source/AvidScriptVM/Private/Tests` and apply filename filters only through `-g '*.h'`; any failed sibling makes the whole probe non-evidence.

### 2026-07-31: Phase 57.8 unquoted Git tree expression recurrence

- Mistake: a parallel candidate identity probe again passed unquoted `HEAD^{tree}` through PowerShell; the shell emitted an encoded script-block argument and Git rejected the synthetic revision, invalidating the whole parallel result.
- Prevention: stop typing brace revision expressions in phase workflows. Read the commit with `git rev-parse HEAD` and the tree with `git show -s --format=%T HEAD` as separate commands; treat any failed sibling in a parallel identity probe as no evidence.

### 2026-07-31: Phase 57.8 Runtime Unity helper collision recurrence

- Mistake: the new Runtime artifact loader introduced generic anonymous helpers `IsLowercaseSha256` and `IsLowercaseAttestationId`; the first collided with the existing reload loader when UBT merged both `.cpp` files into one Unity translation unit, failing the concentrated build with C2084.
- Prevention: every new Runtime `.cpp` anonymous helper uses an owner-qualified name from its first draft, such as `IsRuntimeArtifactLowercaseSha256`. Before staging a new source file, search its helper names across the entire owning module with literal-root `rg`; the standard Unity-enabled project build remains the compilation gate.

### 2026-08-01: Phase 57.8 Automation PowerShell module assumption

- Mistake: new Editor fallback fixtures called `Get-FileHash` inside `powershell.exe` launched by Unreal Automation. That child process could not auto-load `Microsoft.PowerShell.Utility`, so both fixtures exited before reaching the VM artifact publisher even though an interactive shell exposed the cmdlet.
- Prevention: generated Automation scripts depend only on PowerShell language primitives and explicitly constructed .NET APIs. Prefer computing deterministic fixture hashes in C++ with `FAvidScriptHash`; every external-process assertion logs exit code, category, stdout and stderr on failure.

### 2026-08-01: verify declarations after multi-function replacements

- Mistake: a large Runtime replacement retained the previous function's `EAvidScriptVmTypedHostStatus` token while defining `ResolvePreparedReflectionCallMode`, whose declaration and boolean contract require `bool`.
- Prevention: after every multi-function patch, compare each touched definition against its declaration with a focused symbol search before adding callers; do not defer signature consistency to the concentrated build.

### 2026-08-01: Windows rg wildcard recurrence during Gate discovery

- Mistake: a parallel Gate-history probe passed `Docs/Phase57/P57.9*` and `P57.8*` as positional paths, so Win32 rejected the wildcard path and invalidated every sibling result.
- Prevention: Gate discovery uses the literal `Docs/Phase57` root with `-g 'P57.9*'` and `-g 'P57.8*'`; inspect every positional `rg` path for `*` or `?` before dispatching a parallel group.

### 2026-08-01: adaptive fallback tests must respect ProcessEvent guards

- Mistake: the P57.1 adaptive fallback fixture expected `ProcessEvent` to execute while `GIntraFrameDebuggingGameThread` was true, but UE 5.8 deliberately returns before native dispatch while the game thread is paused at a breakpoint.
- Prevention: exercise semantic fallback with a condition that permits `ProcessEvent`, such as an exact-class native guard rejection on a compatible derived receiver. Debugger, GC, PostLoad, and thread guards must be tested against their actual engine semantics rather than used as result-preserving fallback fixtures.

### 2026-08-01: decode UE reflection flags from the active engine source

- Mistake: the first P57.10 BroadShape diagnosis read the `0x00800000` function-flag delta as `FUNC_HasOutParms`; UE 5.8 defines it as `FUNC_HasDefaults`, while `FUNC_HasOutParms` is `0x00400000`.
- Prevention: before changing a reflection safety guard from generated hexadecimal metadata, decode every differing bit against `C:\UnrealEngine\Engine\Source\Runtime\CoreUObject\Public\UObject\Script.h`. Keep true out-parameter rejection independent from fully supplied trivial struct parameters that carry `FUNC_HasDefaults`.

### 2026-08-01: isolated benchmark worktrees need ignored SDK restoration

- Mistake: the first P57.10 clean benchmark-project build started before the ignored `Source/ThirdParty/Wasmtime/installed` payload was restored into the candidate worktree, so the Wasmtime C bridge failed on missing `wasmtime.h` after unrelated modules had already compiled.
- Prevention: immediately after creating or switching a benchmark worktree, restore each lock-governed ignored SDK from the verified local install, run its dependency/hash contracts, and only then create or build the junction-based benchmark project. A clean Git status proves tracked provenance, not local SDK completeness.

### 2026-08-01: use the active benchmark profile runner

- Mistake: the first P57.10 formal attempt used the legacy Phase 53 sidecar with the current Phase 56 harness, so its request omitted the required `callback_result_mode` field and calibration exited before timing.
- Prevention: resolve the runner from the frozen profile family before launch. Phase 56 micro/gameplay profiles use `Invoke-Phase54GameplayBenchmark.ps1` with the tracked six-lane request template; run the matching contract before treating a sidecar as formal.

### 2026-08-01: isolated benchmark projects must load generated modules

- Mistake: the fresh benchmark `.uproject` initially omitted the already-generated `AvidScriptGeneratedBindings` module, so generated package registration never ran even though the source package hash and guest descriptor matched.
- Prevention: before the first benchmark launch, verify every manifest-required generated source module is declared in the isolated `.uproject`, linked for the candidate, and loaded. Copying Saved guest/package artifacts alone is insufficient.

### 2026-08-01: direct property cells need explicit native eligibility

- Mistake: prepared `FIntProperty` getter/setter cells performed direct reflected memory access but inherited eligibility only from function fast-path state, causing valid direct calls to be counted as strict ProcessEvent fallback.
- Prevention: each non-function prepared shape owns an explicit safety guard and eligibility proof. Performance correctness tests assert both observable values and invocation-mode counters before formal sampling.

### 2026-08-01: precompute values in temporary Build.cs diagnostics

- Mistake: a temporary interpolated Build.cs probe escaped string literals inside an interpolation expression, producing a C# rules syntax error before the intended path diagnostics ran.
- Prevention: compute diagnostic booleans in local variables and interpolate only identifiers; immediately restore temporary rules instrumentation after the single evidence run.

### 2026-08-01: extend benchmark route oracles with prepared coverage

- Mistake: P57.10 enabled adaptive direct property cells, but the frozen benchmark correctness oracle still declared only scalar and batch workloads native-eligible, so it rejected correct values and zero fallbacks before timing.
- Prevention: a new prepared shape updates the C++ calibration oracle, the PowerShell evidence evaluator, and their parity contract in the same source commit. Workloads and thresholds remain frozen; only the expected route counters change to match implemented behavior.

### 2026-08-01: re-read arithmetic expressions after term removal

- Mistake: while removing the property-write subtraction from the PowerShell route oracle, the first patch left the adjacent subtraction operator in front of the event term.
- Prevention: after deleting a term from a multiline arithmetic expression, read the complete expression from disk and run the contract immediately; never infer the resulting operator sequence from patch context alone.

### 2026-08-01: static contracts follow the current code owner

- Mistake: the warm-core contract still searched `AvidScriptWasmtimeBackend.cpp` for observed DLL hashing after load-boundary identity moved to `AvidScriptWasmtimeRuntimeSupport.cpp`.
- Prevention: when production ownership moves, update static source contracts in the same change. Assert the delegating call at the consumer and the security-critical behavior at the new owner instead of weakening or deleting the check.

### 2026-08-01: Windows rg wildcard recurrence during Wasmtime lookup

- Mistake: a Wasmtime source lookup again passed `AvidScriptWasmtimeApi.*` as a positional Windows path, producing OS error 123 and invalidating the probe.
- Prevention: positional paths are always literal directories or files. Filename patterns belong only in `-g 'AvidScriptWasmtimeApi.*'`, even for quick diagnostics already covered by an earlier recurrence note.

### 2026-08-01: performance fixtures must preserve UFUNCTION parameter qualifiers

- Mistake: the prepared FVector automation fixture used a by-value parameter while the formal benchmark fixture used `const FVector&`, so the test passed without exercising UE 5.8's `CPF_OutParm | CPF_ReferenceParm | CPF_ConstParm` layout and its required `FOutParmRec` chain.
- Prevention: reflection performance fixtures mirror the formal UFUNCTION signature exactly, including const/reference/out qualifiers. A newly qualified prepared shape must first fail through the real generated thunk, then prove both ProcessEvent and native results before formal sampling.

### 2026-08-01: quote Git peel expressions in PowerShell

- Mistake: `git rev-parse HEAD^{tree}` was passed to PowerShell without quoting, so PowerShell interpreted the brace expression and Git received an encoded stray argument.
- Prevention: always single-quote Git peel expressions in PowerShell, for example `git rev-parse 'HEAD^{tree}'` and `git rev-parse 'HEAD^{commit}'`.

### 2026-08-01: create an empty formal benchmark output directory

- Mistake: the first P57.10 formal runner invocation used a fresh path that did not yet exist, while the sidecar contract requires an existing output directory and rejected the run before Editor startup.
- Prevention: create a unique empty formal output directory immediately before invoking `Invoke-Phase54GameplayBenchmark.ps1`; never reuse a directory containing requests or results.

### 2026-08-01: Micro results are supplemental evaluator input

- Mistake: `Phase56Micro.formal.json` was passed as the primary profile to `Evaluate-Phase54PerformanceGates.ps1`, but the evaluator's primary profile is Gameplay and requires a `gates` object; Micro evidence is consumed by `Get-Phase54MicroStatistics` through supplemental inputs.
- Prevention: use the frozen Micro runner for raw sampling, then aggregate Micro-only shape statistics with `Get-Phase54MicroStatistics`. Invoke the overall evaluator only with the matching Gameplay profile plus all candidate-matched supplemental evidence.

### 2026-08-01: full Automation requires the canonical project fixtures

- Mistake: the first P57.10 full `AvidScript` Automation run used the minimal benchmark project. That project intentionally contained only performance artifacts, so lifecycle manifests, binding-schema inputs, and TPS content tests produced environment failures unrelated to the candidate code.
- Prevention: use the isolated benchmark project only for performance and benchmark-specific focused tests. Run the complete plugin Automation suite from the canonical project after confirming its generated lifecycle manifests and required content assets exist; a fixture-incomplete run is diagnostic only and must not be reported as a regression or final Gate.

### 2026-08-01: assign PowerShell foreach output before piping

- Mistake: two diagnostic one-liners piped directly after a `foreach (...) { ... }` statement, which PowerShell parsed as an empty pipeline element.
- Prevention: capture statement output first (`$Rows = foreach (...) { ... }`) and pipe `$Rows` in a separate statement. Reserve direct pipelines for expressions that PowerShell accepts as pipeline elements.

### 2026-08-01: patch contracts must tolerate checkout line endings

- Mistake: the Wasmtime toolchain contract canonicalized CRLF for the locked patch hash but used an LF-only `diff --git` regex. A clean worktree created under `core.autocrlf` therefore rejected the same tracked patch blob before running the real contract assertions.
- Prevention: every parser for canonicalized text accepts both CRLF and LF, and clean detached worktree Gate coverage is the portability regression test. Hash normalization and structural parsing must use the same line-ending model.

### 2026-08-02: reflected Actor fixtures require a play-initialized world

- Mistake: the P57.11B2 end-to-end fixture repeated the known uninitialized-world error: it spawned an Actor after `CreateWorld` and `SetCurrentWorld` but omitted `InitializeActorsForPlay`, so `AActor::ProcessEvent` silently left ref/out/return frames at their defaults in both VM backends.
- Prevention: every reflected Actor oracle uses the fixed lifecycle `CreateWorld -> CreateNewWorldContext -> SetCurrentWorld -> InitializeActorsForPlay -> SpawnActor`. Before debugging codecs or guest memory, invoke one native return UFUNCTION through `ProcessEvent` and verify a non-default result.

### 2026-08-02: architecture checks always use PowerShell 7

- Mistake: the P57.11B2 clean architecture run repeated the Windows PowerShell 5.1 launcher error even though the checker uses PowerShell 7 leading-pipe syntax; parsing failed before any architecture assertion ran.
- Prevention: architecture and parser gates are launched only with `pwsh -NoProfile`. `powershell.exe` is reserved for an explicit 5.1 compatibility contract and its parser result never counts as product evidence.

### 2026-08-02: repository .NET tests use the pinned SDK launcher

- Mistake: the first P57.11B2 C# Guest test used UE 5.8's bundled .NET 10 launcher, while `global.json` disables roll-forward and pins 8.0.416; execution stopped before compilation.
- Prevention: before any repository .NET build or test, resolve and print `$env:USERPROFILE\.dotnet\dotnet.exe --version`, require 8.0.416, and use that executable for the whole producer-to-consumer test sequence.

### 2026-08-02: Automation leaf names cannot also be parent groups

- Mistake: `AvidScript.Bindings.Utf8ValueHeap` was registered as a runnable test and as the parent of `.CrossInvocation`; UE Automation discovered only the child, leaving the core heap contract outside the focused gate.
- Prevention: every Automation test has a leaf suffix such as `.Core`, `.Lifecycle`, or `.CrossInvocation`. Before phase closeout, run the intended parent filter once and verify the found count includes every expected leaf.

### 2026-08-11: descriptor type closure does not belong in selection identity

- Mistake: the first P57.11B3 schema 10 implementation appended array type and element IDs to `selection_hash`. Runtime binding slices intentionally preserve the authorization selection identity while pruning inactive type closure, so this made valid slices fail with `slice_package_identity_mismatch`.
- Prevention: `selection_hash` binds reflected member selection and class/factory policy only. Type layout, array element edges, and pruned runtime closure belong in type stable IDs and `package_hash`. Before extending either hash, verify authorization package, runtime slice, and reload identity invariants together.

### 2026-08-11: C# profile self classes must be Actor-derived

- Mistake: the first P57.11B3 end-to-end profile used the convenient UObject emitter fixture as `binding_profile.self_class_path`; profile validation correctly rejected it with `self_class_not_actor` before producing the C# report.
- Prevention: profile-driven lifecycle fixtures select an `AActor`-derived self class before source or report paths are added. UObject fixtures remain valid for descriptor and codec unit boundaries, but do not serve as project profile owners.
### 2026-08-12: every probe path must come from repository enumeration

- Mistake: the first P57.11D exploration guessed a nonexistent `Scripts` directory and then guessed `GuestArrayCapabilityIntrinsics.cs` from its type name. Both read-only probes failed before returning the intended evidence.
- Prevention: before any multi-path search or file read, obtain directories with `Get-ChildItem` and exact files with `rg --files` or `rg -l`. Never place an unverified path in a parallel probe; a path inferred from a symbol or class name is not verified.

### 2026-08-12: shared .NET dependency builds are sequential

- Mistake: the first P57.11D .NET gate launched Guest IR, C# Guest, and Wasm backend test projects in parallel. They share `AvidScript.GuestIr/obj/Release`, so one compiler lost the output DLL write race. The wrapper also checked only PowerShell Job state instead of each reported child exit code and incorrectly returned process exit `0`.
- Prevention: repository .NET test projects that share references run sequentially unless each process has isolated `BaseIntermediateOutputPath` and `OutputPath`. Gate wrappers collect every child `ExitCode` and fail when any value is nonzero; a completed PowerShell Job does not prove its command passed.

### 2026-08-12: benchmark generated modules are restored before canonical gates

- Mistake: P57.11C's benchmark publication left the canonical project's `Source/AvidScriptGeneratedBindings` dependent on `AvidScriptPerfHarness`. The later no-clean target warned about the undeclared plugin, and P57.11D Automation then stopped before test discovery because Windows could not resolve `UnrealEditor-AvidScriptPerfHarness.dll` from the canonical loader paths.
- Prevention: any benchmark that emits the project-level generated module owns a mandatory finally-step: regenerate the module from the latest full EngineGameplay descriptor, run one canonical no-clean target build, and cold-start the canonical Editor before reporting benchmark completion. A benchmark DLL existing in its plugin directory does not make it a canonical project dependency.

### 2026-08-12: Unreal ExecCmds uses comma-separated commands

- Mistake: the first generated-module recovery command lost a quoted descriptor path, and the second used a semicolon between `GenerateBindings` and `Quit`; UE treated the semicolon as part of the descriptor filename.
- Prevention: stage descriptor inputs at a verified no-space temporary path and separate `-ExecCmds` commands with commas. Before waiting on Editor shutdown, verify the log contains the complete parsed `Cmd:` line and the expected success marker.

### 2026-08-12: benchmark crossing contracts follow observed counters

- Mistake: the first P57.11D compiler-region benchmark froze four host crossings from the intended load/store sequence, but the generated export reused the explicit array length and the runtime counter correctly observed only three. The runner rejected the valid execution before producing samples.
- Prevention: a new benchmark lane first records one diagnostic run from the runtime-owned crossing counter. Freeze that observed value in the runner, evaluator, and synthetic contract together only after correctness and import inspection agree; never infer the count from a conceptual lowering diagram alone.

### 2026-08-12: long-running gates use one waitable invocation

- Mistake: P57.11D twice launched a build or Automation process through an outer command with a one-second timeout. The tool terminated the launcher, discarded its final exit code, and in one case stopped Editor before an evidence log was created.
- Prevention: build, benchmark, and Automation commands always receive a timeout longer than their expected wall time and stay in one waitable tool session until completion. Never use a short timeout as a background-process launcher; a process timestamp or disappearance is not substitute evidence for the command exit code and final log markers.

### 2026-08-13: index source paths before reading concept-named files

- Mistake: P57.12A review guessed an `AvidScriptObjectHandle.h` path even though the handle is declared in `AvidScriptObjectRegistry.h`, causing an avoidable read-only command failure.
- Prevention: before reading any source file that was not already listed in the current turn, enumerate its confirmed parent with `rg --files` or locate the symbol with `rg -n`; use the exact returned path and never infer filenames from type names.

### 2026-08-13: validation pathspecs exclude protected local edits

- Mistake: P57.12A passed the whole `Source` tree to `git diff --check`, so a protected pre-existing reload-file change appeared as phase validation noise.
- Prevention: every scoped diff, whitespace gate, staging command, and candidate file list must use explicit Git pathspec exclusions for all protected local files; broad directory validation is not an acceptable substitute for an owned-file allowlist.

### 2026-08-13: TestExit zero does not prove every Automation test passed

- Mistake: P57.12A initially called a full Automation run successful from process exit 0 and Queue Empty before counting the per-test results; the log actually contained four `Result={Fail}` completions.
- Prevention: a UE Automation gate passes only after parsing every `Test Completed` record and proving `found = completed = Result{Success}`, `Result{Fail}=0`, Queue Empty, TestExit, RequestExitWithStatus 0, and process exit 0. Never infer suite success from the exit markers alone.

### 2026-08-13: use Git-native shortstat for commit totals

- Mistake: P57.12A attempted to feed a calculated hashtable expression to PowerShell `Measure-Object -Property`, which rejected the expression and discarded an otherwise valid identity-read group.
- Prevention: use `git show --shortstat` for commit-level file and line totals. Custom PowerShell aggregation must first materialize typed objects and named properties; it may not be grouped with required identity reads until independently proven exit 0.

### 2026-08-13: event replacement fixtures must use concrete UObject classes

- Mistake: the first P57.12A replacement-lifecycle fixture used `NewObject<UObject>()` as an incompatible source, but `UObject` is abstract and UE raised an ensure before the intended type check.
- Prevention: negative UObject fixtures must use a known concrete reflected class that is intentionally incompatible with the expected source class. A failed assertion is not attributed to the target contract until the log is checked for engine ensures.

### 2026-08-13: dynamic bridge compatibility is an ABI-shape check

- Mistake: the first reusable delegate bridge collision check called `UFunction::IsSignatureCompatibleWith`; after the duplicated function was linked into the bridge class, its parameter offsets differed from the original delegate signature and a valid second preparation was rejected.
- Prevention: cached dynamic bridge functions compare parameter type, size, array dimension, and calling-convention property flags while deliberately ignoring class-relative offsets. Stable-ID collisions still fail closed on any ABI-shape difference.

### 2026-08-26: resume with the authoritative phase status command

- Mistake: after resuming P57.12C work, the first project commands inspected Git and repository guidance before running the required `Build/InvokePhaseWorkflow.ps1 status -Phase 57` checkpoint.
- Prevention: after every resume, reconnect, or context compaction, the first command executed in the plugin repository is the authoritative phase workflow status command. Git identity, dirty-state, and source probes follow only after that checkpoint succeeds.

### 2026-08-26: enumerate source paths before every targeted read

- Mistake: the first continuation-lowering probe inferred a nonexistent `GuestControlFlow.cs` filename from a concept name and paired it with an invalid search pattern.
- Prevention: obtain each unfamiliar source path from `rg --files` or a symbol-producing `rg -n` result before reading it. Concept names, class names, and remembered paths are not path evidence.

### 2026-08-26: continuation ownership is a Session transaction boundary

- Mistake: the first P57.12C1 draft placed continuation ownership inside each Runtime instance and began implementation before checking candidate-lane, guest-depth, and teardown-fence requirements together.
- Prevention: cross-producer asynchronous work is owned by the Runtime Session. The Runtime only forwards host ABI calls and invokes guest exports; active/prepared lanes, activation identity, cancellation, delivery fencing, and deferred teardown are designed and reviewed as one Session transaction before implementation begins.

### 2026-08-26: keep repository probes to one PowerShell statement

- Mistake: one P57.12C source read joined two `Get-Content` statements with a semicolon even though the repository shell contract requires one logical command per invocation.
- Prevention: read one file or one verified slice per shell invocation. When multiple independent reads are needed, issue separate tool calls; do not join PowerShell statements with semicolons or command-chain operators.

### 2026-08-26: verify UE container APIs and endpoint lifetime before tests

- Mistake: the first continuation owner draft used a nonexistent `TArray::CountByPredicate` helper, and its transaction test invoked a prepared endpoint reference after `DiscardPrepared` had released the endpoint.
- Prevention: verify unfamiliar UE container methods against engine headers or established repository use before writing them. Raw host endpoint references are valid only while their Runtime HostContext owns that lane; tests must not call them after discard, and production swaps retain retired endpoints until the old Runtime has explicitly rebound or unloaded.

### 2026-08-26: enumerate the Tools root before selecting test projects

- Mistake: the first centralized .NET gate probe guessed a nonexistent `Tools/AvidScript.GuestWasm.Tests` directory instead of enumerating the actual test projects.
- Prevention: discover repository test projects with `rg --files Tools | rg 'Tests.*\.csproj$'` before constructing any test command. Never infer a test-project directory from a compiler or artifact name.

### 2026-08-26: test public artifacts across assembly boundaries

- Mistake: the first continuation debug-map regression referenced the internal `CSharpGuestIds` helper from the separate test assembly, causing a compile failure before the behavior could be exercised.
- Prevention: cross-assembly tests use public contracts or protocol literals already exposed by serialized artifacts. Internal implementation helpers are referenced only when the production assembly explicitly grants test visibility.

### 2026-08-26: synthetic-router fixtures declare their complete ABI type set

- Mistake: the first source-less continuation-router debug fixture declared only a callback attribute, so its semantic type registry lacked the `i64` and `f32` primitives required by the generated router ABI.
- Prevention: isolated synthetic-function fixtures explicitly exercise every implicit ABI type before lowering. A semantic success result alone does not prove that the type registry is sufficient for generated Guest functions.

### 2026-08-26: read fixture constants before updating exact assertions

- Mistake: the first schema-10 BuildIntegration update guessed the ActorLifecycle continuation callback id instead of reading its declared constant and wrote `1001` where the source contract uses `9`.
- Prevention: exact fixture IDs, counts, and signatures are copied only from the current source or generated artifact after a successful build. Remembered or illustrative values are never promoted into acceptance assertions.

### 2026-08-26: architecture evidence runs from a clean candidate tree

- Mistake: the first P57.12C1 architecture check was launched in the implementation worktree even though its evidence contract intentionally rejects dirty architecture inputs.
- Prevention: implementation-time checks use scoped source and contract tests. The evidence architecture checker runs only after the candidate commit exists, from a detached clean worktree at that exact commit.
