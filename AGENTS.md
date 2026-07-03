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
- In Codex managed sandbox, `Build.bat` can fail before C++ with `UnrealBuildTool failed to check dependencies` when UBT tries to write `C:\UnrealEngine\Engine\Intermediate\Build`. Do not treat this as an AvidScript code failure; rerun the same command with explicit engine build permission.
- In Codex Windows sandbox, if `apply_patch` fails with `windows sandbox failed: helper_unknown_error`, first retry with workspace-relative paths. If the helper still fails, use a controlled PowerShell write only for the intended workspace files, then immediately inspect `git diff` and run the relevant build/automation verification. Do not skip diff review or tests after a fallback write.
- When editing Markdown through PowerShell, use single-quoted here-strings or explicit line arrays for text containing Markdown backticks. Do not put Markdown backticks inside double-quoted PowerShell strings, because they are escape characters and can corrupt commit hashes or inline code. After writing docs, verify the rendered source with `Get-Content` or `Select-String`.
- 2026-07-03 P4.2 mistake record: a PowerShell double-quoted Markdown replacement interpreted backticks as escape characters and briefly wrote corrupted inline code/control characters into root docs. Prevention: use literal single-quoted here-strings or line arrays for Markdown edits, then run a control-character scan (`[\x00-\x08\x0B\x0C\x0E-\x1F]`) over touched Markdown before considering docs done.
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
