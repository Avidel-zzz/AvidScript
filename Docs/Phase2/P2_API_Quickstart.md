# Phase 2 API Quickstart

日期: 2026-07-02
状态: Verified In P2.5

本文档说明 Phase 2 已实现的 host-side API。它只描述当前插件中已经存在并被 automation 覆盖的能力, 不描述未来语言层的设计愿望。

## 1. UObject Handle Registry

头文件:

```cpp
#include "AvidScriptObjectRegistry.h"
```

最小用法:

```cpp
FAvidScriptObjectRegistry Registry;
FAvidScriptObjectHandleResult RegisterResult;
FAvidScriptObjectHandle Handle = Registry.RegisterObject(SomeActor, RegisterResult);

if (!RegisterResult.bSucceeded)
{
	UE_LOG(LogTemp, Warning, TEXT("%s"), *RegisterResult.ErrorMessage);
	return;
}

FAvidScriptObjectHandleResult ResolveResult;
AActor* Actor = Registry.ResolveObject<AActor>(Handle, ResolveResult);
if (Actor == nullptr)
{
	UE_LOG(LogTemp, Warning, TEXT("%s"), *ResolveResult.ErrorMessage);
	return;
}
```

关键规则:

- 不把 `UObject*` 暴露给 guest。
- guest-facing handle 可以用 `FAvidScriptObjectHandle::ToUInt64()` 传递。
- 释放 handle 后, 旧 handle 不应再命中新对象。
- `ResolveObject<T>` 类型不匹配时返回 `type_mismatch`。

## 2. Actor Binding

头文件:

```cpp
#include "AvidScriptActorBinding.h"
```

读取 Actor location:

```cpp
FVector Location;
FAvidScriptActorBindingResult Result;
const bool bOk = FAvidScriptActorBinding::GetActorLocation(
	Registry,
	ActorHandle,
	Location,
	Result);

if (!bOk)
{
	UE_LOG(LogTemp, Warning, TEXT("%s"), *Result.ErrorMessage);
}
```

写入 Actor location:

```cpp
FAvidScriptActorBindingResult Result;
const bool bOk = FAvidScriptActorBinding::SetActorLocation(
	Registry,
	ActorHandle,
	FVector(100.0, 0.0, 50.0),
	EAvidScriptActorWritePolicy::AllowWrites,
	Result);
```

只读策略:

```cpp
FAvidScriptActorBinding::SetActorLocation(
	Registry,
	ActorHandle,
	FVector::ZeroVector,
	EAvidScriptActorWritePolicy::ReadOnly,
	Result);
```

`ReadOnly` 会返回 `write_denied`, 用于把设计期或服务器权限策略前置到 host API。

## 3. AvidScript Component

头文件:

```cpp
#include "AvidScriptComponent.h"
```

当前 `UAvidScriptComponent` 是最小 runtime host component:

- BeginPlay 注册 owner Actor handle。
- BeginPlay 加载内置 smoke WASM module。
- TickComponent 调用 `avid_on_tick`。
- EndPlay unload runtime 并 release owner handle。

C++ 读取 stats:

```cpp
const UAvidScriptComponent* Component = Actor->FindComponentByClass<UAvidScriptComponent>();
if (Component != nullptr)
{
	const FAvidScriptComponentRuntimeStats& Stats = Component->GetRuntimeStats();
	UE_LOG(LogTemp, Log, TEXT("AvidScript ticks=%d owner=%s"), Stats.TickCallCount, *Stats.OwnerObjectPath);
}
```

解析 owner handle:

```cpp
AActor* OwnerActor = nullptr;
FAvidScriptObjectHandleResult Result;
const bool bResolved = Component->ResolveOwnerActor(OwnerActor, Result);
```

注意: 当前 component 加载的是内置 smoke module, 还没有 asset 指向、外部 WASM 文件或 Editor reload 按钮。

## 4. WASM Runtime Smoke

头文件:

```cpp
#include "AvidScriptWasmRuntime.h"
```

一次性 smoke:

```cpp
FAvidScriptWasmSmokeResult Result;
const bool bOk = FAvidScriptWasmRuntime::RunEmbeddedSmokeTest(Result);
```

手动 runtime instance:

```cpp
FAvidScriptWasmRuntimeInstance Runtime;
FAvidScriptWasmSmokeResult Result;

if (Runtime.LoadEmbeddedSmokeModule(Result) && Runtime.BeginPlay(Result))
{
	Runtime.Tick(1.0f / 60.0f, Result);
}

Runtime.Unload(Result);
```

`FAvidScriptWasmSmokeResult` 会记录:

- runtime/module/instance/exec env 状态。
- begin/tick/unload 状态。
- tick count。
- module id。
- error category、export name、next action、error message。
- runtime metrics。

## 5. Reload Session

头文件:

```cpp
#include "AvidScriptWasmReload.h"
```

最小流程:

```cpp
FAvidScriptWasmReloadSession Session;
FAvidScriptWasmReloadResult Result;

const FAvidScriptWasmReloadManifest ManifestV1 = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("module_v1"));
Session.LoadInitialModule(BytecodeV1, BytecodeV1Size, ManifestV1, Result);

const FAvidScriptWasmReloadManifest ManifestV2 = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("module_v2"));
if (Session.ReloadModule(BytecodeV2, BytecodeV2Size, ManifestV2, Result))
{
	UE_LOG(LogTemp, Log, TEXT("Reloaded module=%s"), *Result.ActiveModuleId);
}
else if (Result.bRollbackPreservedLiveRuntime)
{
	UE_LOG(LogTemp, Warning, TEXT("Reload rejected, still running=%s"), *Result.ActiveModuleId);
}
```

Tick live runtime:

```cpp
FAvidScriptWasmSmokeResult TickResult;
Session.TickLive(1.0f / 60.0f, TickResult);
```

当前 smoke manifest 要求:

```text
AbiVersion == 1
RequiredExports:
  avid_on_begin_play
  avid_on_tick
```

失败语义:

- ABI mismatch 返回 `abi_mismatch`。
- required export 缺失返回 `missing_export`。
- candidate load/instantiate/begin play 失败不会替换 live runtime。
- rejected reload 计入 `GetRejectedReloadCount()`。
- successful reload 计入 `GetSuccessfulReloadCount()`。

## 6. 当前不要这么用

Phase 2 还没有实现以下能力:

- 不要直接把外部 WASM asset 接到 component 上。
- 不要假设 reload 会迁移 guest state。
- 不要把 registry handle 当作永久存档 ID。
- 不要在 guest ABI 暴露 raw pointer 或 UE object address。
- 不要把 Actor binding 当成完整反射系统。
- 不要在移动端启用 WAMR, 移动端库和平台策略尚未验证。

## 7. 验证入口

针对所有 Phase 2 API 的完整回归:

```powershell
& "C:\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\user0\Documents\Unreal Projects\AvidTPSTemplate\AvidTPSTemplate.uproject" -Unattended -NullRHI -NoSplash -NoSound -NoP4 -NoLiveCoding -stdout -FullStdOutLogOutput -FORCELOGFLUSH -CrashForUAT "-ExecCmds=Automation RunTests AvidScript" "-TestExit=Automation Test Queue Empty" "-abslog=C:\tmp\AvidScript_P2_5_All_Automation.log"
```
