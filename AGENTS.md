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

- After C++ or Build.cs changes, run the UE5.8 Editor target build:

```powershell
& "C:\UnrealEngine\Engine\Build\BatchFiles\Build.bat" AvidTPSTemplateEditor Win64 Development "-Project=C:\Users\user0\Documents\Unreal Projects\AvidTPSTemplate\AvidTPSTemplate.uproject" -WaitMutex -NoHotReloadFromIDE
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
