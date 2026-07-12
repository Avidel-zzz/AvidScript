param(
    [string]$PluginRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$PluginRoot = [System.IO.Path]::GetFullPath($PluginRoot)
$Violations = [System.Collections.Generic.List[string]]::new()

function Add-Violation {
    param([string]$Message)
    $Violations.Add($Message)
}

function Read-RequiredFile {
    param([string]$RelativePath)
    $Path = Join-Path $PluginRoot $RelativePath
    if (-not [System.IO.File]::Exists($Path)) {
        Add-Violation "missing required architecture file: $RelativePath"
        return ''
    }
    return [System.IO.File]::ReadAllText($Path)
}

function Test-SourceTreeForbiddenPattern {
    param(
        [string]$RelativeDirectory,
        [string[]]$Patterns
    )

    $Directory = Join-Path $PluginRoot $RelativeDirectory
    if (-not [System.IO.Directory]::Exists($Directory)) {
        Add-Violation "missing required architecture directory: $RelativeDirectory"
        return
    }

    foreach ($Path in [System.IO.Directory]::GetFiles($Directory, '*.*', [System.IO.SearchOption]::AllDirectories)) {
        $Extension = [System.IO.Path]::GetExtension($Path)
        if ($Extension -notin @('.h', '.cpp', '.cs')) {
            continue
        }

        $Text = [System.IO.File]::ReadAllText($Path)
        foreach ($Pattern in $Patterns) {
            if ([regex]::IsMatch($Text, $Pattern)) {
                $RelativePath = [System.IO.Path]::GetRelativePath($PluginRoot, $Path)
                Add-Violation "$RelativePath matches forbidden dependency pattern '$Pattern'"
            }
        }
    }
}

$CoreBuild = Read-RequiredFile 'Source/AvidScriptCore/AvidScriptCore.Build.cs'
foreach ($ForbiddenDependency in @('CoreUObject', 'Engine', 'WAMR', 'Json', 'AvidScriptRuntime', 'AvidScriptBindings', 'AvidScriptEditor')) {
    if ($CoreBuild.Contains('"' + $ForbiddenDependency + '"')) {
        Add-Violation "AvidScriptCore must not depend on $ForbiddenDependency"
    }
}

Test-SourceTreeForbiddenPattern 'Source/AvidScriptCore' @(
    '#include\s+["<](Engine|GameFramework|Components|UObject)/',
    'wasm_runtime_|wasm_export\.h',
    '#include\s+["<]AvidScript(ActorBinding|ObjectRegistry|SceneComponentBinding|WasmRuntime|Component)\.h'
)

$BindingsBuild = Read-RequiredFile 'Source/AvidScriptBindings/AvidScriptBindings.Build.cs'
foreach ($RequiredDependency in @('AvidScriptCore', 'CoreUObject', 'Engine')) {
    if (-not $BindingsBuild.Contains('"' + $RequiredDependency + '"')) {
        Add-Violation "AvidScriptBindings is missing required dependency $RequiredDependency"
    }
}
foreach ($ForbiddenDependency in @('WAMR', 'Json', 'UnrealEd', 'AvidScriptRuntime', 'AvidScriptEditor')) {
    if ($BindingsBuild.Contains('"' + $ForbiddenDependency + '"')) {
        Add-Violation "AvidScriptBindings must not depend on $ForbiddenDependency"
    }
}

Test-SourceTreeForbiddenPattern 'Source/AvidScriptBindings' @(
    'wasm_runtime_|wasm_export\.h',
    '#include\s+["<]AvidScriptWasm(Runtime|Reload|ModuleLoader)\.h'
)

$VmBuild = Read-RequiredFile 'Source/AvidScriptVM/AvidScriptVM.Build.cs'
foreach ($RequiredDependency in @('AvidScriptCore', 'Core')) {
    if (-not $VmBuild.Contains('"' + $RequiredDependency + '"')) {
        Add-Violation "AvidScriptVM is missing required dependency $RequiredDependency"
    }
}if (-not $VmBuild.Contains('"WAMR"')) {
    Add-Violation 'AvidScriptVM is missing its private WAMR backend dependency'
}
foreach ($ForbiddenDependency in @('CoreUObject', 'Engine', 'Json', 'UnrealEd', 'AvidScriptBindings', 'AvidScriptRuntime', 'AvidScriptEditor')) {
    if ($VmBuild.Contains('"' + $ForbiddenDependency + '"')) {
        Add-Violation "AvidScriptVM must not depend on $ForbiddenDependency"
    }
}

Test-SourceTreeForbiddenPattern 'Source/AvidScriptVM/Public' @(
    'wasm_runtime_|wasm_export\.h',
    '#include\s+["<](Engine|GameFramework|Components|UObject)/',
    '\b(UObject|AActor|USceneComponent|FVector|FRotator|FTransform)\b'
)

$RuntimeBuild = Read-RequiredFile 'Source/AvidScriptRuntime/AvidScriptRuntime.Build.cs'
foreach ($RequiredDependency in @('AvidScriptCore', 'AvidScriptBindings', 'AvidScriptVM')) {
    if (-not $RuntimeBuild.Contains('"' + $RequiredDependency + '"')) {
        Add-Violation "AvidScriptRuntime is missing required dependency $RequiredDependency"
    }
}if ($RuntimeBuild.Contains('"WAMR"')) {
    Add-Violation 'AvidScriptRuntime must not depend directly on WAMR'
}

Test-SourceTreeForbiddenPattern 'Source/AvidScriptRuntime' @(
    'wasm_runtime_|wasm_export\.h',
    'AVIDSCRIPT_WITH_WAMR'
)

$ReloadTypesHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptWasmReloadTypes.h'
$RuntimeSessionHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptRuntimeSession.h'
$ReloadUmbrellaHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptWasmReload.h'
if ($ReloadTypesHeader -match '\bFAvidScriptRuntimeSession\b') {
    Add-Violation 'WASM reload types header must not declare RuntimeSession ownership'
}
if ($RuntimeSessionHeader -notmatch '\bclass\s+AVIDSCRIPTRUNTIME_API\s+FAvidScriptRuntimeSession\b') {
    Add-Violation 'AvidScriptRuntimeSession.h must own the RuntimeSession public declaration'
}
if ($ReloadUmbrellaHeader -match '\b(class|struct)\s+AVIDSCRIPTRUNTIME_API\b') {
    Add-Violation 'AvidScriptWasmReload.h must remain a compatibility umbrella without declarations'
}

$ComponentHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptComponent.h'
if ($ComponentHeader -match 'TUniquePtr\s*<\s*FAvidScriptWasmRuntimeInstance\s*>') {
    Add-Violation 'UAvidScriptComponent must own FAvidScriptRuntimeSession instead of a raw runtime instance'
}
if ($ComponentHeader -match '\bbPlayActive\b') {
    Add-Violation 'UAvidScriptComponent must derive active state from its RuntimeSession snapshot'
}

$WorldSubsystemHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptWorldSubsystem.h'
if ($WorldSubsystemHeader -match 'TUniquePtr\s*<\s*FAvidScriptWasmRuntimeInstance\s*>') {
    Add-Violation 'UAvidScriptWorldSubsystem must own FAvidScriptRuntimeSession instead of a raw runtime instance'
}
if ($WorldSubsystemHeader -match '\bbWorldPlayActive\b') {
    Add-Violation 'UAvidScriptWorldSubsystem must derive active state from its RuntimeSession snapshot'
}

foreach ($RuntimeServicePath in @(
    'Source/AvidScriptRuntime/Private/Session/AvidScriptRuntimeScheduler.h',
    'Source/AvidScriptRuntime/Private/Session/AvidScriptRuntimeScheduler.cpp',
    'Source/AvidScriptRuntime/Private/Session/AvidScriptRuntimeEventRouter.h',
    'Source/AvidScriptRuntime/Private/Session/AvidScriptRuntimeEventRouter.cpp'
)) {
    [void](Read-RequiredFile $RuntimeServicePath)
}

$RuntimeSessionSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Session/AvidScriptRuntimeSession.cpp'
if ($RuntimeSessionSource -match 'LiveRuntime->(Tick|DispatchEvent)\s*\(') {
    Add-Violation 'FAvidScriptRuntimeSession must route Tick and Event through Scheduler/EventRouter'
}
if (-not $RuntimeSessionSource.Contains('Scheduler->Tick(') -or
    -not $RuntimeSessionSource.Contains('EventRouter->Dispatch(')) {
    Add-Violation 'FAvidScriptRuntimeSession is missing Scheduler/EventRouter routing'
}

foreach ($LegacyBindingPath in @(
    'Source/AvidScriptRuntime/Public/AvidScriptObjectRegistry.h',
    'Source/AvidScriptRuntime/Public/AvidScriptActorBinding.h',
    'Source/AvidScriptRuntime/Public/AvidScriptSceneComponentBinding.h',
    'Source/AvidScriptRuntime/Private/AvidScriptObjectRegistry.cpp',
    'Source/AvidScriptRuntime/Private/AvidScriptActorBinding.cpp',
    'Source/AvidScriptRuntime/Private/AvidScriptSceneComponentBinding.cpp'
)) {
    if ([System.IO.File]::Exists((Join-Path $PluginRoot $LegacyBindingPath))) {
        Add-Violation "binding production file remains in Runtime module: $LegacyBindingPath"
    }
}

$PluginDescriptorPath = Join-Path $PluginRoot 'AvidScript.uplugin'
if (-not [System.IO.File]::Exists($PluginDescriptorPath)) {
    Add-Violation 'missing AvidScript.uplugin'
}
else {
    $Descriptor = [System.IO.File]::ReadAllText($PluginDescriptorPath) | ConvertFrom-Json
    $ModuleNames = @($Descriptor.Modules | ForEach-Object { $_.Name })
    foreach ($RequiredModule in @('AvidScriptCore', 'AvidScriptBindings', 'AvidScriptVM', 'AvidScriptRuntime', 'AvidScriptEditor')) {
        if ($ModuleNames -notcontains $RequiredModule) {
            Add-Violation "plugin descriptor is missing module $RequiredModule"
        }
    }
}

if ($Violations.Count -gt 0) {
    Write-Host "AvidScript architecture check failed with $($Violations.Count) violation(s):"
    foreach ($Violation in $Violations) {
        Write-Host " - $Violation"
    }
    exit 1
}

Write-Host 'AvidScript architecture check passed.'
Write-Host 'Core: Core-only dependency boundary.'
Write-Host 'Bindings: UE typed APIs without WAMR dependency.'
Write-Host 'VM: Core-only public contract without gameplay types.'
Write-Host 'Runtime: explicit Core + Bindings + VM composition.'
exit 0
