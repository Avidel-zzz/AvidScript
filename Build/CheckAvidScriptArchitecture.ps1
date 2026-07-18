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
foreach ($RequiredDependency in @('AvidScriptCore', 'CoreUObject', 'Engine', 'Json')) {
    if (-not $BindingsBuild.Contains('"' + $RequiredDependency + '"')) {
        Add-Violation "AvidScriptBindings is missing required dependency $RequiredDependency"
    }
}
foreach ($ForbiddenDependency in @('WAMR', 'UnrealEd', 'AvidScriptRuntime', 'AvidScriptEditor')) {
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

$VmContractHeader = Read-RequiredFile 'Source/AvidScriptVM/Public/AvidScriptVmBackend.h'
foreach ($RequiredBatchContract in @('ActorGetTransformBatch', 'InputCells', 'OutputFloats')) {
    if (-not $VmContractHeader.Contains($RequiredBatchContract)) {
        Add-Violation "VM batch contract is missing $RequiredBatchContract"
    }
}
$VmHostBindingsSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWamrHostBindings.cpp'
foreach ($RequiredVmPrimitive in @('TranslateGuestRange', 'actor_get_transform_batch')) {
    if (-not $VmHostBindingsSource.Contains($RequiredVmPrimitive)) {
        Add-Violation "VM guest-memory adapter is missing $RequiredVmPrimitive"
    }
}

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

$RuntimeHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptWasmRuntime.h'
$RuntimeSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp'
$GameplayEventHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptGameplayEvent.h'
$EventRouterSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Session/AvidScriptRuntimeEventRouter.cpp'
foreach ($RequiredTimerStructure in @('ActiveTimers', 'TimerHeap', 'DueTimerScratch')) {
    if (-not $RuntimeHeader.Contains($RequiredTimerStructure)) {
        Add-Violation "Runtime timer scheduler is missing required structure $RequiredTimerStructure"
    }
}
if ($RuntimeHeader -match '\bRemainingSeconds\b') {
    Add-Violation 'Runtime timers must use absolute deadlines instead of per-frame RemainingSeconds mutation'
}
if ($RuntimeSource -match 'PendingTimers\.IndexOfByPredicate') {
    Add-Violation 'Runtime timer lookup must use the active handle map instead of a linear PendingTimers scan'
}
if (-not $RuntimeHeader.Contains('CopyObservableStateToResult')) {
    Add-Violation 'Runtime result synchronization must use CopyObservableStateToResult at return boundaries'
}
foreach ($RequiredBatchRuntimeStructure in @(
    'HandleActorGetTransformBatchImport',
    'TransformBatchHandleScratch',
    'TransformBatchSnapshotScratch',
    'TransformBatchOutputScratch'
)) {
    if (-not $RuntimeHeader.Contains($RequiredBatchRuntimeStructure)) {
        Add-Violation "Runtime transform batch dispatcher is missing $RequiredBatchRuntimeStructure"
    }
}
if (-not $RuntimeSource.Contains('case EAvidScriptHostBindingId::ActorGetTransformBatch')) {
    Add-Violation 'Runtime dispatcher does not route ActorGetTransformBatch'
}
foreach ($RequiredGameplayEventContract in @(
    'EAvidScriptGameplayEventType',
    'FAvidScriptGameplayEvent',
    'BeginOverlap',
    'EndOverlap',
    'Hit',
    'Input'
)) {
    if (-not $GameplayEventHeader.Contains($RequiredGameplayEventContract)) {
        Add-Violation "Runtime gameplay event schema is missing $RequiredGameplayEventContract"
    }
}
foreach ($RequiredGameplayEventRoute in @('DispatchGameplayEvent', 'avid_on_gameplay_event')) {
    if (-not $RuntimeSource.Contains($RequiredGameplayEventRoute)) {
        Add-Violation "Runtime gameplay event dispatcher is missing $RequiredGameplayEventRoute"
    }
    if (-not $EventRouterSource.Contains($RequiredGameplayEventRoute)) {
        Add-Violation "Runtime event router is missing $RequiredGameplayEventRoute"
    }
}
foreach ($ForbiddenPerEventExport in @(
    'avid_on_begin_overlap',
    'avid_on_end_overlap',
    'avid_on_hit',
    'avid_on_input'
)) {
    if ($RuntimeSource.Contains($ForbiddenPerEventExport) -or $EventRouterSource.Contains($ForbiddenPerEventExport)) {
        Add-Violation "Runtime must route gameplay schemas through avid_on_gameplay_event instead of $ForbiddenPerEventExport"
    }
}
if ($RuntimeSource -match 'CopyHostImportStateToResult\(OutResult\);\s*CopyTimerStateToResult\(OutResult\);\s*CopyEventStateToResult\(OutResult\);') {
    Add-Violation 'Runtime result synchronization must not repeat the observable-state copy triplet'
}

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
$ComponentSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptComponent.cpp'
if ($ComponentHeader -match 'TUniquePtr\s*<\s*FAvidScriptWasmRuntimeInstance\s*>') {
    Add-Violation 'UAvidScriptComponent must own FAvidScriptRuntimeSession instead of a raw runtime instance'
}
if ($ComponentHeader -match '\bbPlayActive\b') {
    Add-Violation 'UAvidScriptComponent must derive active state from its RuntimeSession snapshot'
}
foreach ($RequiredCollisionLifecycle in @(
    'BindOwnerGameplayDelegates',
    'UnbindOwnerGameplayDelegates',
    'HandleOwnerBeginOverlap',
    'HandleOwnerEndOverlap',
    'HandleOwnerHit',
    'GameplayObjectHandleValues'
)) {
    if (-not $ComponentHeader.Contains($RequiredCollisionLifecycle) -or
        -not $ComponentSource.Contains($RequiredCollisionLifecycle)) {
        Add-Violation "UAvidScriptComponent collision lifecycle is missing $RequiredCollisionLifecycle"
    }
}
if ($ComponentSource.IndexOf('UnbindOwnerGameplayDelegates();') -gt
    $ComponentSource.IndexOf('RuntimeSession->StopAndUnload(')) {
    Add-Violation 'UAvidScriptComponent must unbind owner gameplay delegates before stopping the runtime session'
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

$BindingSelectionTypes = Read-RequiredFile 'Source/AvidScriptEditor/Public/AvidScriptEditorBindingSelectionTypes.h'
$BindingSelectionResolverHeader = Read-RequiredFile 'Source/AvidScriptEditor/Public/AvidScriptEditorBindingSelectionResolver.h'
$BindingSelectionResolverSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingSelectionResolver.cpp'
$ReflectedFunctionPolicySource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.cpp'
$BindingDescriptorGeneratorSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingDescriptorGenerator.cpp'
foreach ($RequiredSelectionContract in @(
    'FAvidScriptBindingSelectionProfile',
    'FAvidScriptBindingSelectionIssue',
    'FAvidScriptBindingSelectionResolveResult'
)) {
    if (-not $BindingSelectionTypes.Contains($RequiredSelectionContract)) {
        Add-Violation "binding selection types are missing $RequiredSelectionContract"
    }
}
if ($BindingSelectionTypes -match '\b(UClass|UFunction)\b') {
    Add-Violation 'binding selection types must remain reflection-free data contracts'
}
if (-not $BindingSelectionResolverHeader.Contains('FAvidScriptEditorBindingSelectionResolver') -or
    -not $BindingSelectionResolverSource.Contains('EFieldIterationFlags::None')) {
    Add-Violation 'binding selection resolver must own exact-class reflected function discovery'
}
if (-not $ReflectedFunctionPolicySource.Contains('FAvidScriptEditorReflectedFunctionPolicy::Evaluate') -or
    $BindingDescriptorGeneratorSource -match 'bool\s+IsFunctionAllowed\s*\(') {
    Add-Violation 'reflected function eligibility must remain in the shared function policy'
}
foreach ($RequiredProfileGeneratorContract in @('MakeEngineGameplayProfile', 'GenerateFromProfile')) {
    if (-not $BindingDescriptorGeneratorSource.Contains($RequiredProfileGeneratorContract)) {
        Add-Violation "binding descriptor generator is missing $RequiredProfileGeneratorContract"
    }
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

$CSharpBindingEmitterSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpBindingEmitter.cpp'
$CSharpBuildServiceSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/AvidScriptEditorCSharpBuildService.cpp'
$CSharpBuildInvokerSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBuildInvoker.cpp'
$CSharpBindingSliceSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBindingSliceService.cpp'
$CSharpPreparedSemanticSource = Read-RequiredFile 'Build/AvidScriptCSharpPreparedSemantic.ps1'
foreach ($RequiredGameplayPackageContract in @(
    'EmitEngineGameplay',
    'PublishEngineGameplay',
    'PublishGeneratedPackage'
)) {
    if (-not $CSharpBindingEmitterSource.Contains($RequiredGameplayPackageContract)) {
        Add-Violation "C# binding emitter is missing gameplay package contract $RequiredGameplayPackageContract"
    }
}
if (-not $CSharpBuildServiceSource.Contains('PublishEngineGameplay(BindingEmitResult)')) {
    Add-Violation 'custom C# builds must default to the generated engine gameplay package'
}
foreach ($RequiredBuildOrchestrationContract in @(
    'FAvidScriptEditorCSharpBuildInvoker::BuildOnce',
    'FAvidScriptEditorCSharpBindingSliceService::Publish',
    'CSharpBootstrap',
    'PreparedBuildReportPath',
    'FinalConfig.PreparedBuildReportPath = BootstrapConfig.ReportPath'
)) {
    if (-not $CSharpBuildServiceSource.Contains($RequiredBuildOrchestrationContract)) {
        Add-Violation "C# BuildService is missing orchestration contract $RequiredBuildOrchestrationContract"
    }
}
foreach ($ForbiddenBuildServiceConcern in @(
    'FPlatformProcess::ExecProcess',
    'FJsonSerializer',
    'FAvidScriptBindingDescriptorParser::Parse'
)) {
    if ($CSharpBuildServiceSource.Contains($ForbiddenBuildServiceConcern)) {
        Add-Violation "C# BuildService must not own invocation, JSON, or descriptor concern $ForbiddenBuildServiceConcern"
    }
}
if (-not $CSharpBuildInvokerSource.Contains('FPlatformProcess::ExecProcess') -or
    -not $CSharpBuildInvokerSource.Contains('-RuntimeBindingPackagePath') -or
    -not $CSharpBuildInvokerSource.Contains('-PreparedBuildReportPath') -or
    $CSharpBuildInvokerSource.Contains('PublishEngineGameplay') -or
    $CSharpBuildInvokerSource.Contains('BindingSliceService')) {
    Add-Violation 'C# BuildInvoker must execute one normalized build without selecting or slicing packages'
}
if (-not $CSharpBindingSliceSource.Contains('FAvidScriptEditorBindingDescriptorGenerator::Generate') -or
    -not $CSharpBindingSliceSource.Contains('FAvidScriptEditorCSharpBindingEmitter::PublishDescriptor') -or
    $CSharpBindingSliceSource.Contains('FPlatformProcess::ExecProcess')) {
    Add-Violation 'C# BindingSliceService must reuse the descriptor generator and package publisher without invoking builds'
}
$CSharpGuestContext = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpFunctionLoweringContext.cs'
$CSharpOperationLowerer = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpOperationLowerer.cs'
foreach ($RequiredCaptureAddressContract in @(
    'TrackCaptureAddressTarget',
    'TryGetCaptureAddressTarget',
    'EmitStorageAddress'
)) {
    if (-not $CSharpGuestContext.Contains($RequiredCaptureAddressContract) -and
        -not $CSharpOperationLowerer.Contains($RequiredCaptureAddressContract)) {
        Add-Violation "C# Guest lowering is missing flow-captured address contract $RequiredCaptureAddressContract"
    }
}
$SemanticAnalyzerSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticAnalyzer.cs'
$SemanticReachabilitySource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticReachabilityProjector.cs'
$CSharpGuestLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpGuestLowerer.cs'
$CSharpBuildScriptSource = Read-RequiredFile 'Build/BuildCSharpActorLifecycle.ps1'
foreach ($RequiredPreparedBuildContract in @(
    'AvidScriptCSharpPreparedSemantic.ps1',
    'Import-AvidScriptCSharpPreparedSemantic',
    'prepared_semantic_invalid',
    'frontend_reused',
    'semantic_reused'
)) {
    if (-not $CSharpBuildScriptSource.Contains($RequiredPreparedBuildContract)) {
        Add-Violation "C# build pipeline is missing prepared semantic contract $RequiredPreparedBuildContract"
    }
}
foreach ($RequiredPreparedHelperContract in @(
    'Import-AvidScriptCSharpPreparedSemantic',
    'ExpectedSourcePath',
    'ExpectedAuthorizationPackage',
    'Publish-AvidScriptBindingFilePairAtomic',
    'ASBI4401',
    'ASBI4402',
    'ASBI4403',
    'ASBI4404'
)) {
    if (-not $CSharpPreparedSemanticSource.Contains($RequiredPreparedHelperContract)) {
        Add-Violation "prepared semantic helper is missing validation contract $RequiredPreparedHelperContract"
    }
}
foreach ($ForbiddenPreparedHelperConcern in @(
    '\bdotnet(?:\.exe)?\b',
    '\bpowershell(?:\.exe)?\b',
    '\bStart-Process\b',
    '\bInvoke-AvidScriptPowerShell\b',
    '\bRuntimeBindingPackagePath\b',
    '\bOmitRuntimeBindingPackage\b',
    '\bPublishEngineGameplay\b',
    '\bBindingSliceService\b'
)) {
    if ($CSharpPreparedSemanticSource -match $ForbiddenPreparedHelperConcern) {
        Add-Violation "prepared semantic helper owns forbidden toolchain, source, or package-policy concern $ForbiddenPreparedHelperConcern"
    }
}
if ($CSharpPreparedSemanticSource -match 'Get-Content\s+-Raw\s+-LiteralPath\s+\$ExpectedSource' -or
    $CSharpPreparedSemanticSource -match 'ReadAllText\s*\(\s*\$ExpectedSource') {
    Add-Violation 'prepared semantic helper must hash the C# source without scanning its text'
}
foreach ($RequiredReachabilityContract in @(
    'SemanticReachabilityProjector.Project',
    'CurrentSchemaVersion = 5',
    'CurrentSemanticVersion = "1.5"'
)) {
    if (-not $SemanticAnalyzerSource.Contains($RequiredReachabilityContract)) {
        Add-Violation "C# Semantic analyzer is missing reachability contract $RequiredReachabilityContract"
    }
}
foreach ($RequiredReachabilityProjection in @('export_roots', 'all_callables_compatibility', 'AssociatedSymbolId')) {
    if (-not $SemanticReachabilitySource.Contains($RequiredReachabilityProjection)) {
        Add-Violation "C# Semantic reachability is missing projection contract $RequiredReachabilityProjection"
    }
}
if (-not $CSharpGuestLowererSource.Contains('GetReachableCallableIds') -or
    -not $CSharpBuildScriptSource.Contains('UsedAuthorizationBindingImports') -or
    -not $CSharpBuildScriptSource.Contains('UsedRuntimeBindingImports')) {
    Add-Violation 'C# Guest and build pipeline must consume semantic binding reachability'
}
foreach ($RequiredDualPackageContract in @('binding_authorization', 'RuntimeBindingPackagePath', 'OmitRuntimeBindingPackage', 'ASBI4303')) {
    if (-not $CSharpBuildScriptSource.Contains($RequiredDualPackageContract)) {
        Add-Violation "C# build pipeline is missing dual-package contract $RequiredDualPackageContract"
    }
}
if ($CSharpBuildScriptSource.Contains('MissingBindingImports')) {
    Add-Violation 'complete binding packages are authorization ceilings; unused imports must not be required in Guest IR'
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
