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
- Current .NET 8 `wasi-experimental` output is a Mono/WASI runtime app: generated `dotnet.wasm` exports only `memory` and `_start`. It is not an AvidScript direct ABI module until it exports both `avid_on_begin_play` and `avid_on_tick`.
- `PublishAot=true` with .NET 8 `wasi-wasm` currently fails with `NETSDK1203`; record this as toolchain unsupported, not as an AvidScript runtime failure.
- `BuildCSharpActorLifecycle.ps1` must isolate `DOTNET_CLI_HOME`, `APPDATA`, `LOCALAPPDATA`, and local NuGet config into `Saved/AvidScriptCSharpGuest/ActorLifecycle` so Codex sandbox runs do not try to read the user's blocked `%APPDATA%\NuGet\NuGet.Config`.
- If the user NuGet package cache is readable, it is acceptable for the C# diagnostic script to use `C:\Users\user0\.nuget\packages` as a package cache while keeping config and generated outputs in `Saved/`.
- 2026-07-06 P13.1 mistake record: passing `BaseOutputPath` or `BaseIntermediateOutputPath` to MSBuild with a trailing Windows backslash inside a quoted argument can break paths with spaces and produce `MSB1008: Only one project can be specified`. Prevention: pass these MSBuild property paths with forward slashes and a trailing `/`.
- 2026-07-06 P13.1 mistake record: `--configfile` alone did not stop NuGet targets from reading `%APPDATA%\NuGet\NuGet.Config`. Prevention: redirect `APPDATA` and `LOCALAPPDATA` for the script process before invoking `dotnet publish`.

- P14.1 changed the normal C# sample build route: `BuildCSharpActorLifecycle.ps1` now tries `avidscript-csharp-source-adapter` first and exits with `direct_abi_built` when the source is inside the supported subset. The .NET/WASI publish path remains a fallback diagnostic route, not the default success path.
- Current C# source adapter subset: `BeginPlay()`, `Tick(float deltaSeconds)`, `Actor.SetLocation(float x, float y, float z)`, numeric float literals, `deltaSeconds`, multiplication by numeric literals, and addition of supported expressions.
- Current adapter self binding assumes the per-script owner actor is registered as slot `1`, generation `1`. Before expanding to multi-object gameplay, replace this with an explicit self/owner host binding or runtime context injection and cover it with automation.
- 2026-07-06 P14.1 mistake record: returning an empty .NET `List[byte]` from a PowerShell function without a unary comma enumerates the empty collection and assigns `$null`, causing later parameter binding errors such as `Cannot bind argument to parameter 'Bytes' because it is null`. Prevention: return collection objects with `return ,$List` / `return ,$Body`, and use `return ,([byte[]]...)` for byte arrays that must stay intact.
- P15.2 组件级 C# manifest 路径已经接入 `UAvidScriptComponent`: manifest 路径为空时继续使用 embedded smoke module, 路径非空时通过 manifest loader 加载 WASM, 并在 BeginPlay 前注入组件 owner registry 与 `AllowWrites` actor 写策略。
- 2026-07-06 P15.2 mistake record: UE5.8 的 `FFilePath` 声明在 `UObject/SoftObjectPath.h`, 不是 `Misc/FilePath.h`; 错误 include 会导致 `fatal error C1083`. Prevention: 新增 UE struct include 前先在 `C:\UnrealEngine\Engine\Source` 搜索声明位置或参考同引擎版本的工作示例。
- P16.2 Editor 侧 C# report/manifest 组件绑定已经接入 `FAvidScriptEditorComponentBindingService`: 该服务可读取 C# build report 的 `artifacts.manifest_file`, 绑定到显式 Actor 或当前选中 Actor, 并在缺少组件时创建 `UAvidScriptComponent`。
- P17.2 Editor 菜单入口已经接入 C# ActorLifecycle 绑定: `Tools > AvidScript > Bind C# ActorLifecycle Script` 会读取 `Saved/AvidScriptCSharpGuest/ActorLifecycle/actor_lifecycle.csharp.report.json`, 并调用组件绑定服务绑定到当前选中 Actor。

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
