# AvidScript Agent Notes

## Engine Baseline

- Use source-built Unreal Engine 5.8.
- Engine root: `C:\UnrealEngine`.
- Preferred Editor target validation command:

```powershell
& "C:\UnrealEngine\Engine\Build\BatchFiles\Build.bat" AvidTPSTemplateEditor Win64 Development "-Project=C:\Users\user0\Documents\Unreal Projects\AvidTPSTemplate\AvidTPSTemplate.uproject" -WaitMutex -NoHotReloadFromIDE
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
git config --global --add safe.directory "C:/Users/user0/Documents/Unreal Projects/AvidTPSTemplate/Plugins/AvidScript"
```

## Git Workflow

- Work from `Plugins/AvidScript` when running Git commands.
- Before edits, check status:

```powershell
git -C "C:\Users\user0\Documents\Unreal Projects\AvidTPSTemplate\Plugins\AvidScript" status --short --branch
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

## Documentation Workflow

- Project-level decision docs live under:

```text
C:\Users\user0\Documents\Unreal Projects\AvidTPSTemplate\Docs
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

- Do not use `Build.bat -Clean` just to make UBT notice new plugin `.cpp` or `.h` files. On 2026-07-02 this accidentally triggered a heavy `AvidTPSTemplateEditor` rebuild. Treat full target clean as destructive-to-time and run it only after explicit user approval.
- For normal AvidScript C++ iteration, prefer module-scoped validation first:

```powershell
& "C:\UnrealEngine\Engine\Build\BatchFiles\Build.bat" AvidTPSTemplateEditor Win64 Development "-Project=C:\Users\user0\Documents\Unreal Projects\AvidTPSTemplate\AvidTPSTemplate.uproject" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles -Module=AvidScriptRuntime
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
- After C++ or Build.cs changes, run the UE5.8 Editor target build:

```powershell
& "C:\UnrealEngine\Engine\Build\BatchFiles\Build.bat" AvidTPSTemplateEditor Win64 Development "-Project=C:\Users\user0\Documents\Unreal Projects\AvidTPSTemplate\AvidTPSTemplate.uproject" -WaitMutex -NoHotReloadFromIDE
```

- Run runtime automation after runtime behavior changes:

```powershell
& "C:\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\user0\Documents\Unreal Projects\AvidTPSTemplate\AvidTPSTemplate.uproject" -Unattended -NullRHI -NoSplash -NoSound -NoP4 -NoLiveCoding -stdout -FullStdOutLogOutput -FORCELOGFLUSH -CrashForUAT "-ExecCmds=Automation RunTests AvidScript.Runtime" "-TestExit=Automation Test Queue Empty" "-abslog=C:\tmp\AvidScript_Automation.log"
```

- For Windows packaged Development smoke on this UE5.8 source build, skip ZenStore during cook to avoid local Zen oplog staging instability:

```powershell
& "C:\UnrealEngine\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun "-project=C:\Users\user0\Documents\Unreal Projects\AvidTPSTemplate\AvidTPSTemplate.uproject" -noP4 -platform=Win64 -clientconfig=Development -skipbuild -cook -clean -stage -pak -archive "-archivedirectory=C:\tmp\AvidScript_Package" "-AdditionalCookerOptions=-SkipZenStore" "-ubtargs=-MaxParallelActions=4 -NoUBA" -unattended -utf8output
```

- Validate the packaged runtime smoke log with:

```powershell
& "C:\tmp\AvidScript_Package\Windows\AvidTPSTemplate.exe" -NullRHI -NoSplash -NoSound -stdout -FullStdOutLogOutput -FORCELOGFLUSH "-ExecCmds=quit" "-abslog=C:\tmp\AvidScript_Packaged_Run.log"
```

- Record successful builds in the active phase log.
- If a build fails before reaching AvidScript files, document it as an environment or project-level blocker.
- If a build fails inside AvidScript files, fix the plugin code first and rerun the same command.
- Generated build outputs must remain ignored by Git.

## C# Guest Toolchain Workflow

- P13.1 verified the user-local .NET 8 SDK at `C:\Users\user0\.dotnet\dotnet.exe` can install and list `wasi-experimental`. Prefer this SDK for C# WASI probes over `C:\Program Files\dotnet\dotnet.exe` on this machine.
- The Program Files .NET 9.0.306 CLI currently fails workload commands in `Microsoft.DotNet.Installer.Windows.InstallerBase`, and .NET 9 rejects `wasi-wasm` with `The 'wasi-experimental' workload is not supported in .NET 9.` Do not spend Phase time trying to force that path unless the toolchain has been repaired or upgraded.
- Current .NET 8 `wasi-experimental` output is a Mono/WASI runtime app: generated `dotnet.wasm` exports only `memory` and `_start`. It is not an AvidScript direct ABI module until it exports `avid_on_begin_play`, `avid_on_tick`, and `avid_on_end_play`.
- `PublishAot=true` with .NET 8 `wasi-wasm` currently fails with `NETSDK1203`; record this as toolchain unsupported, not as an AvidScript runtime failure.
- `BuildCSharpActorLifecycle.ps1` must isolate `DOTNET_CLI_HOME`, `APPDATA`, `LOCALAPPDATA`, and local NuGet config into `Saved/AvidScriptCSharpGuest/ActorLifecycle` so Codex sandbox runs do not try to read the user's blocked `%APPDATA%\NuGet\NuGet.Config`.
- If the user NuGet package cache is readable, it is acceptable for the C# diagnostic script to use `C:\Users\user0\.nuget\packages` as a package cache while keeping config and generated outputs in `Saved/`.
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
- Phase 38 complete automation baseline is 137/137 on UE5.8 Win64 Editor. Recount Success/Fail/performed lines; do not trust process exit code alone.

## Phase 39 Language Frontend Rules

- The formal C# frontend uses the Roslyn assemblies shipped with the selected .NET 8 SDK. Keep it offline and package-free; do not add a NuGet dependency for compiler services already present in the SDK.
- PowerShell may locate tools, orchestrate processes, and merge reports. It must not gain new C# lexical, syntactic, expression, or statement parsing responsibilities.
- The versioned AvidScript frontend JSON is the boundary consumed by later semantic analysis and Guest IR. Do not serialize Roslyn implementation objects or numeric enum values as the public contract.
- Frontend spans use UTF-16 offsets and zero-based line/column coordinates. Convert to one-based coordinates only at the Editor presentation boundary.
- Syntax errors must still produce a deterministic diagnostic artifact and must gate WASM generation. Never fall back to regex parsing after the formal frontend reports an error.
- 2026-07-13 P39.1 workflow mistake record: the first redirected .NET test command assigned to PowerShell's read-only `$HOME` variable, so the environment variables were never updated and NuGet attempted to read the sandbox-blocked user config. Prevention: use a task-specific name such as `$ToolHome`, set `DOTNET_CLI_HOME`, `APPDATA`, `LOCALAPPDATA`, and `NUGET_PACKAGES` before invoking dotnet, and check assignment errors before interpreting restore output.
- 2026-07-13 P39.1 edit mistake record: a controlled PowerShell fallback normalized an LF-only C# source anchor to CRLF, so exact validation rejected the intended atomic-writer edit before writing. Prevention: inspect or normalize the target text and anchor to LF before exact matching; preserve the file's existing newline style when writing it back.
