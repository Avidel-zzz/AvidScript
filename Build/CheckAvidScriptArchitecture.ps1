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
$WamrBuildScript = Read-RequiredFile 'Build/BuildWAMRWin64.cmd'
$WamrCallStackSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWamrCallStack.cpp'
$WamrFastInterpreterSource = Read-RequiredFile 'Source/ThirdParty/WAMR/upstream/core/iwasm/interpreter/wasm_interp_fast.c'
$WamrClassicInterpreterSource = Read-RequiredFile 'Source/ThirdParty/WAMR/upstream/core/iwasm/interpreter/wasm_interp_classic.c'
$WamrInterpreterRuntimeSource = Read-RequiredFile 'Source/ThirdParty/WAMR/upstream/core/iwasm/interpreter/wasm_runtime.c'
foreach ($RequiredVmDiagnosticContract in @(
    'FAvidScriptVmStackFrame',
    'TArray<FAvidScriptVmStackFrame> StackFrames',
    'StackFrames.Reset()'
)) {
    if (-not $VmContractHeader.Contains($RequiredVmDiagnosticContract)) {
        Add-Violation "VM diagnostic contract is missing $RequiredVmDiagnosticContract"
    }
}
foreach ($RequiredWamrDiagnosticPrimitive in @(
    'MaxCallStackTextLength = 64 * 1024',
    'MaxCallStackFrames = 128',
    'wasm_runtime_get_call_stack_buf_size',
    'wasm_runtime_dump_call_stack_to_buf',
    'ParseAvidScriptWamrCallStack'
)) {
    if (-not $WamrCallStackSource.Contains($RequiredWamrDiagnosticPrimitive)) {
        Add-Violation "WAMR diagnostic adapter is missing $RequiredWamrDiagnosticPrimitive"
    }
}
if (-not $WamrBuildScript.Contains('-DWAMR_BUILD_DUMP_CALL_STACK=1')) {
    Add-Violation 'Win64 WAMR build must enable bounded trap call-stack capture'
}
foreach ($WamrInterpreterSource in @(
    $WamrFastInterpreterSource,
    $WamrClassicInterpreterSource,
    $WamrInterpreterRuntimeSource
)) {
    if (-not $WamrInterpreterSource.Contains('(void)wasm_interp_create_call_stack(exec_env);') -or
        $WamrInterpreterSource.Contains('wasm_interp_dump_call_stack(exec_env, true, NULL, 0);')) {
        Add-Violation 'vendored WAMR trap exits must snapshot call stacks without printing upstream text'
    }
}
foreach ($ForbiddenVmDiagnosticConcern in @('CSharp', 'SemanticSpan', 'SourcePath', 'wasm_export.h')) {
    if ($VmContractHeader.Contains($ForbiddenVmDiagnosticConcern)) {
        Add-Violation "VM public diagnostics must remain language and WAMR neutral: $ForbiddenVmDiagnosticConcern"
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

Test-SourceTreeForbiddenPattern 'Source/AvidScriptEditor' @(
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
$RuntimeDiagnosticsHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptWasmDiagnostics.h'
$RuntimeDebugMapHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Diagnostics/AvidScriptWasmDebugMap.h'
$RuntimeDebugMapSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Diagnostics/AvidScriptWasmDebugMap.cpp'
$RuntimeReloadSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptWasmReload.cpp'
$VmModuleLayoutHeader = Read-RequiredFile 'Source/AvidScriptVM/Public/AvidScriptWasmModuleLayout.h'
$VmModuleLayoutSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWasmModuleLayout.cpp'
if ($ReloadTypesHeader -match '\bFAvidScriptRuntimeSession\b') {
    Add-Violation 'WASM reload types header must not declare RuntimeSession ownership'
}
if ($RuntimeSessionHeader -notmatch '\bclass\s+AVIDSCRIPTRUNTIME_API\s+FAvidScriptRuntimeSession\b') {
    Add-Violation 'AvidScriptRuntimeSession.h must own the RuntimeSession public declaration'
}
if ($ReloadUmbrellaHeader -match '\b(class|struct)\s+AVIDSCRIPTRUNTIME_API\b') {
    Add-Violation 'AvidScriptWasmReload.h must remain a compatibility umbrella without declarations'
}
foreach ($RequiredRuntimeDiagnosticContract in @(
    'FAvidScriptWasmDebugProvenance',
    'FAvidScriptWasmDiagnosticFrame',
    'FunctionIndex',
    'FunctionOffset',
    'RawFunctionToken',
    'bSourceMapped',
    'FrontendArtifactSha256',
    'ImportedFunctionCount',
    'DefinedFunctionCount'
)) {
    if (-not $RuntimeDiagnosticsHeader.Contains($RequiredRuntimeDiagnosticContract)) {
        Add-Violation "Runtime diagnostic contract is missing $RequiredRuntimeDiagnosticContract"
    }
}
foreach ($RequiredDebugMapContract in @(
    'LoadAndValidate',
    'MapFrames',
    'FunctionIndicesByExportName',
    'TSharedPtr<const FAvidScriptWasmDebugMap>'
)) {
    if (-not $RuntimeDebugMapHeader.Contains($RequiredDebugMapContract)) {
        Add-Violation "Runtime debug-map interface is missing $RequiredDebugMapContract"
    }
}
foreach ($RequiredDebugMapValidation in @(
    'MaxDebugMapByteSize = 4 * 1024 * 1024',
    'MaxDebugFunctionCount = 65536',
    'IsCanonicalDebugMapSource',
    'debug_map_hash_mismatch',
    'debug_map_module_mismatch',
    'debug_map_guest_ir_mismatch',
    'debug_map_duplicate_function_index',
    'debug_map_function_index_range_mismatch',
    'frontend_artifact_sha256',
    'imported_function_count',
    'defined_function_count'
)) {
    if (-not $RuntimeDebugMapSource.Contains($RequiredDebugMapValidation)) {
        Add-Violation "Runtime debug-map validator is missing $RequiredDebugMapValidation"
    }
}
foreach ($RequiredWasmLayoutContract in @(
    'FAvidScriptWasmModuleLayout',
    'ImportedFunctionCount',
    'DefinedFunctionCount',
    'FunctionExports',
    'InspectAvidScriptWasmModuleLayout'
)) {
    if (-not $VmModuleLayoutHeader.Contains($RequiredWasmLayoutContract)) {
        Add-Violation "VM WASM module-layout interface is missing $RequiredWasmLayoutContract"
    }
}
foreach ($RequiredWasmLayoutParserContract in @(
    'ReadU32Leb',
    'ParseWasmImportSection',
    'ParseWasmFunctionSection',
    'ParseWasmExportSection',
    'FunctionIndexLimit'
)) {
    if (-not $VmModuleLayoutSource.Contains($RequiredWasmLayoutParserContract)) {
        Add-Violation "VM WASM module-layout parser is missing $RequiredWasmLayoutParserContract"
    }
}
if (-not $RuntimeReloadSource.Contains('LoadManifestDebugMap') -or
    -not $RuntimeReloadSource.Contains('TryResolveDebugMapPathFromManifest') -or
    -not $RuntimeReloadSource.Contains('InspectAvidScriptWasmModuleLayout') -or
    -not $RuntimeReloadSource.Contains('debug_map_wasm_layout_invalid') -or
    -not $RuntimeReloadSource.Contains('debug_map_wasm_layout_mismatch') -or
    -not $RuntimeReloadSource.Contains('DebugImportedFunctionCount') -or
    -not $RuntimeReloadSource.Contains('DebugDefinedFunctionCount')) {
    Add-Violation 'Runtime reload must resolve and validate the immutable debug map before candidate activation'
}
if (-not $RuntimeSource.Contains('DebugMap->MapFrames')) {
    Add-Violation 'Runtime must map VM trap frames through the validated debug map'
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
$PropertySelectionResolverHeader = Read-RequiredFile 'Source/AvidScriptEditor/Public/AvidScriptEditorBindingPropertySelectionResolver.h'
$PropertySelectionResolverSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingPropertySelectionResolver.cpp'
$ReflectedFunctionPolicySource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.cpp'
$ReflectedPropertyPolicySource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorReflectedPropertyPolicy.cpp'
$ReflectedTypePolicyHeader = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorReflectedTypePolicy.h'
$BindingDescriptorGeneratorSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingDescriptorGenerator.cpp'
$BindingDescriptorHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptBindingDescriptor.h'
$BindingDescriptorSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptBindingDescriptor.cpp'
$BindingInvocationSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptBindingInvocation.cpp'
foreach ($RequiredSelectionContract in @(
    'FAvidScriptBindingSelectionProfile',
    'FAvidScriptBindingSelectionIssue',
    'FAvidScriptBindingSelectionResolveResult',
    'FAvidScriptReflectedPropertySelection',
    'IncludeProperties',
    'ExcludeProperties',
    'ExplicitProperties',
    'CandidatePropertyCount',
    'AcceptedPropertyCount',
    'RejectedPropertyCount'
)) {
    if (-not $BindingSelectionTypes.Contains($RequiredSelectionContract)) {
        Add-Violation "binding selection types are missing $RequiredSelectionContract"
    }
}
if ($BindingSelectionTypes -match '\b(UClass|UFunction|FProperty)\b') {
    Add-Violation 'binding selection types must remain reflection-free data contracts'
}
if (-not $BindingSelectionResolverHeader.Contains('FAvidScriptEditorBindingSelectionResolver') -or
    -not $BindingSelectionResolverSource.Contains('EFieldIterationFlags::None')) {
    Add-Violation 'binding selection resolver must own exact-class reflected function discovery'
}
if (-not $PropertySelectionResolverHeader.Contains('FAvidScriptEditorBindingPropertySelectionResolver') -or
    -not $PropertySelectionResolverSource.Contains('EFieldIterationFlags::IncludeSuper') -or
    -not $PropertySelectionResolverSource.Contains('EvaluateReadable')) {
    Add-Violation 'property selection resolver must own bounded reflected property discovery and policy evaluation'
}
if (-not $ReflectedFunctionPolicySource.Contains('FAvidScriptEditorReflectedFunctionPolicy::Evaluate') -or
    $BindingDescriptorGeneratorSource -match 'bool\s+IsFunctionAllowed\s*\(') {
    Add-Violation 'reflected function eligibility must remain in the shared function policy'
}
if (-not $ReflectedPropertyPolicySource.Contains('FAvidScriptEditorReflectedPropertyPolicy::EvaluateReadable') -or
    -not $ReflectedTypePolicyHeader.Contains('ProjectReadableProperty')) {
    Add-Violation 'reflected property eligibility must reuse the shared reflected type policy'
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
$CSharpBindingRendererSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpBindingRenderer.cpp'
$CSharpBindingArtifactHeader = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorCSharpBindingArtifact.h'
$CSharpBuildServiceSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/AvidScriptEditorCSharpBuildService.cpp'
$CSharpBuildInvokerSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBuildInvoker.cpp'
$CSharpBuildPipelineSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBuildPipeline.cpp'
$CSharpBindingSliceSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBindingSliceService.cpp'
$CSharpAsyncBuildBackendSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildBackend.cpp'
$CSharpAsyncBuildJobSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildJob.cpp'
$CSharpLiveReloadServiceSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpLiveReload/AvidScriptEditorCSharpLiveReloadService.cpp'
$CSharpLiveReloadBuildStateSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpLiveReload/AvidScriptEditorCSharpLiveReloadServiceBuildState.cpp'
$CSharpLiveReloadCompletionSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpLiveReload/AvidScriptEditorCSharpLiveReloadCompletion.cpp'
$CSharpPreparedSemanticSource = Read-RequiredFile 'Build/AvidScriptCSharpPreparedSemantic.ps1'
$CSharpBindingPackageSource = Read-RequiredFile 'Build/AvidScriptCSharpBindingPackage.ps1'
$CSharpSemanticCacheSource = Read-RequiredFile 'Build/AvidScriptCSharpSemanticCache.ps1'
foreach ($RequiredGameplayPackageContract in @(
    'EmitEngineGameplay',
    'PublishEngineGameplay',
    'PublishGeneratedPackage'
)) {
    if (-not $CSharpBindingEmitterSource.Contains($RequiredGameplayPackageContract)) {
        Add-Violation "C# binding emitter is missing gameplay package contract $RequiredGameplayPackageContract"
    }
}
foreach ($RequiredPropertyDescriptorContract in @(
    'BindingKind',
    'UeMember'
)) {
    if (-not $BindingDescriptorHeader.Contains($RequiredPropertyDescriptorContract)) {
        Add-Violation "binding descriptor model is missing $RequiredPropertyDescriptorContract"
    }
}
foreach ($RequiredPropertyDescriptorParserContract in @(
    'SchemaVersion != 4',
    'binding_kind',
    'ue_member',
    'property_get',
    'cached_property_get'
)) {
    if (-not $BindingDescriptorSource.Contains($RequiredPropertyDescriptorParserContract)) {
        Add-Violation "binding descriptor v4 parser is missing $RequiredPropertyDescriptorParserContract"
    }
}
foreach ($RequiredPropertyDescriptorGeneratorContract in @(
    'GenerateWithReadableProperties',
    'property_get:',
    'cached_property_get',
    'ProjectReadableProperty'
)) {
    if (-not $BindingDescriptorGeneratorSource.Contains($RequiredPropertyDescriptorGeneratorContract)) {
        Add-Violation "binding descriptor v4 generator is missing $RequiredPropertyDescriptorGeneratorContract"
    }
}
foreach ($RequiredPropertyRuntimeContract in @(
    'FindFProperty<FProperty>',
    'Plan.ReflectedProperty',
    'WriteAvidScriptRuntimeValueToGuest',
    'binding_property_read_failed'
)) {
    if (-not $BindingInvocationSource.Contains($RequiredPropertyRuntimeContract)) {
        Add-Violation "cached property runtime is missing $RequiredPropertyRuntimeContract"
    }
}
foreach ($RequiredPropertyFacadeContract in @(
    'RenderPropertyGetter',
    'Binding.BindingKind == TEXT("property_get")',
    'GenerateWithReadableProperties',
    'public bool IsNull => Slot == 0 && Generation == 0;',
    'public bool HasHandle => Slot > 0 && Generation > 0;'
)) {
    if (-not $CSharpBindingRendererSource.Contains($RequiredPropertyFacadeContract) -and
        -not $CSharpBindingEmitterSource.Contains($RequiredPropertyFacadeContract) -and
        -not $CSharpBindingSliceSource.Contains($RequiredPropertyFacadeContract)) {
        Add-Violation "C# property facade pipeline is missing $RequiredPropertyFacadeContract"
    }
}
if (-not $CSharpBindingArtifactHeader.Contains('EmitterVersion = TEXT("47.3.0")')) {
    Add-Violation 'C# binding emitter version must identify the P47.3 object proxy surface'
}
if (-not $CSharpBindingPackageSource.Contains('[int]$Descriptor.schema_version -ne 4')) {
    Add-Violation 'C# binding package resolver must accept descriptor schema v4 property packages'
}
if (-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 4')) {
    Add-Violation 'Runtime reload manifest loader must accept descriptor schema v4 property packages'
}
if ($BindingInvocationSource.Contains('CustomTimeDilation') -or
    $CSharpBindingRendererSource.Contains('CustomTimeDilation') -or
    $BindingInvocationSource.Contains('RootComponent') -or
    $CSharpBindingRendererSource.Contains('RootComponent')) {
    Add-Violation 'property runtime and renderer must stay data-driven without per-property API switches'
}
$CSharpWorkspaceHeader = Read-RequiredFile 'Source/AvidScriptEditor/Public/AvidScriptEditorCSharpWorkspaceService.h'
$CSharpWorkspaceSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpWorkspace/AvidScriptEditorCSharpWorkspaceService.cpp'
foreach ($RequiredWorkspaceTemplate in @(
    'Templates/CSharp/ProjectWorkspace/GameplayScript.cs',
    'Templates/CSharp/ProjectWorkspace/AvidScript.Gameplay.csproj.template',
    'Templates/CSharp/ProjectWorkspace/default.csharp-profile.json.template',
    'Templates/CSharp/ProjectWorkspace/global.json'
)) {
    [void](Read-RequiredFile $RequiredWorkspaceTemplate)
}
foreach ($RequiredWorkspaceContract in @('CreateOrRefresh', 'GetDefaultWorkspaceRoot', 'GetDefaultFacadePath')) {
    if (-not $CSharpWorkspaceHeader.Contains($RequiredWorkspaceContract)) {
        Add-Violation "C# WorkspaceService is missing contract $RequiredWorkspaceContract"
    }
}
if (-not $CSharpWorkspaceSource.Contains('FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay') -or
    -not $CSharpWorkspaceSource.Contains('Templates') -or
    $CSharpWorkspaceSource.Contains('FAvidScriptEditorCSharpBuildService::BuildProfile') -or
    $CSharpWorkspaceSource.Contains('FAvidScriptEditorComponentBindingService') -or
    $CSharpWorkspaceSource.Contains('FPlatformProcess')) {
    Add-Violation 'C# WorkspaceService must own files and IDE facade refresh without build, binding, or process concerns'
}
foreach ($RequiredBuildServiceContract in @(
    'FAvidScriptEditorCSharpBuildInvoker::BuildOnce',
    'FAvidScriptEditorCSharpBuildPipeline::Prepare',
    'FAvidScriptEditorCSharpBuildPipeline::CompleteBootstrap',
    'FAvidScriptEditorCSharpBuildPipeline::CompleteFinal',
    'FAvidScriptEditorCSharpBuildPipeline::Cleanup'
)) {
    if (-not $CSharpBuildServiceSource.Contains($RequiredBuildServiceContract)) {
        Add-Violation "C# BuildService is missing pipeline delegation contract $RequiredBuildServiceContract"
    }
}
foreach ($ForbiddenBuildServiceConcern in @(
    'FPlatformProcess::ExecProcess',
    'FJsonSerializer',
    'FAvidScriptBindingDescriptorParser::Parse',
    'PublishEngineGameplay',
    'BindingSliceService',
    'FAvidScriptFrontendReportReader'
)) {
    if ($CSharpBuildServiceSource.Contains($ForbiddenBuildServiceConcern)) {
        Add-Violation "C# BuildService must not own invocation, reflection, report, or slicing concern $ForbiddenBuildServiceConcern"
    }
}
foreach ($RequiredBuildPipelineContract in @(
    'FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay',
    'FAvidScriptEditorCSharpBindingSliceService::Publish',
    'CSharpBootstrap',
    'CSharpBuildTransactions',
    'BeginAvidScriptCSharpArtifactTransaction',
    'FinishAvidScriptCSharpArtifactTransaction',
    'PreparedBuildReportPath',
    'Plan.FinalConfig.PreparedBuildReportPath',
    'FAvidScriptFrontendReportReader::LoadFromFile'
)) {
    if (-not $CSharpBuildPipelineSource.Contains($RequiredBuildPipelineContract)) {
        Add-Violation "C# BuildPipeline is missing orchestration contract $RequiredBuildPipelineContract"
    }
}
foreach ($ForbiddenBuildPipelineConcern in @(
    'FPlatformProcess::ExecProcess',
    'FJsonSerializer',
    'FAvidScriptBindingDescriptorParser::Parse',
    'FAvidScriptEditorComponentBindingService',
    'AActor'
)) {
    if ($CSharpBuildPipelineSource.Contains($ForbiddenBuildPipelineConcern)) {
        Add-Violation "C# BuildPipeline must not own process, JSON, descriptor, or Actor binding concern $ForbiddenBuildPipelineConcern"
    }
}
foreach ($ForbiddenArtifactTransactionOwner in @(
    $CSharpBuildServiceSource,
    $CSharpBuildInvokerSource,
    $CSharpAsyncBuildJobSource,
    $CSharpLiveReloadServiceSource,
    $CSharpLiveReloadBuildStateSource
)) {
    if ($ForbiddenArtifactTransactionOwner.Contains('BeginAvidScriptCSharpArtifactTransaction') -or
        $ForbiddenArtifactTransactionOwner.Contains('FinishAvidScriptCSharpArtifactTransaction') -or
        $ForbiddenArtifactTransactionOwner.Contains('CSharpBuildTransactions')) {
        Add-Violation 'C# committed artifact transaction must be owned only by BuildPipeline'
    }
}
foreach ($RequiredAsyncBuildBackendContract in @(
    'FAvidScriptEditorCSharpProfileService::LoadProfile',
    'FAvidScriptEditorCSharpBuildPipeline::Prepare',
    'FAvidScriptEditorCSharpBuildPipeline::CompleteBootstrap',
    'FAvidScriptEditorCSharpBuildPipeline::CompleteFinal',
    'FAvidScriptEditorCSharpBuildPipeline::Cleanup',
    'FAvidScriptEditorCSharpBuildInvoker::Prepare',
    'FAvidScriptEditorCSharpBuildInvoker::Finalize'
)) {
    if (-not $CSharpAsyncBuildBackendSource.Contains($RequiredAsyncBuildBackendContract)) {
        Add-Violation "C# AsyncBuildBackend is missing orchestration contract $RequiredAsyncBuildBackendContract"
    }
}
foreach ($ForbiddenAsyncBuildBackendConcern in @(
    'FMonitoredProcess',
    'IAvidScriptEditorCSharpBuildProcess',
    'FAvidScriptEditorComponentBindingService',
    'AActor'
)) {
    if ($CSharpAsyncBuildBackendSource.Contains($ForbiddenAsyncBuildBackendConcern)) {
        Add-Violation "C# AsyncBuildBackend must not own process or Actor binding concern $ForbiddenAsyncBuildBackendConcern"
    }
}
foreach ($RequiredAsyncBuildJobContract in @(
    'Process->Launch',
    'Process->Poll',
    'Process->Cancel',
    'Backend->CompleteInvocation',
    'PublishingBindingSlice',
    'FAvidScriptEditorCSharpMonitoredBuildProcess'
)) {
    if (-not $CSharpAsyncBuildJobSource.Contains($RequiredAsyncBuildJobContract)) {
        Add-Violation "C# AsyncBuildJob is missing process/state contract $RequiredAsyncBuildJobContract"
    }
}
foreach ($ForbiddenAsyncBuildJobConcern in @(
    'FAvidScriptEditorCSharpProfileService',
    'FAvidScriptEditorCSharpBuildPipeline',
    'FAvidScriptEditorComponentBindingService',
    'FPlatformProcess::ExecProcess',
    'AActor'
)) {
    if ($CSharpAsyncBuildJobSource.Contains($ForbiddenAsyncBuildJobConcern)) {
        Add-Violation "C# AsyncBuildJob must not own profile, pipeline, process invocation, or Actor binding concern $ForbiddenAsyncBuildJobConcern"
    }
}
foreach ($RequiredLiveReloadServiceContract in @(
    'FAvidScriptEditorCSharpAsyncBuildJobFactory::Create',
    'FAvidScriptEditorComponentBindingService::',
    'WatchHost->Start',
    'AddTicker',
    'ActiveBuildJob->Cancel',
    'Coordinator.Stop'
)) {
    if (-not $CSharpLiveReloadServiceSource.Contains($RequiredLiveReloadServiceContract)) {
        Add-Violation "C# LiveReloadService is missing lifecycle contract $RequiredLiveReloadServiceContract"
    }
}
foreach ($ForbiddenLiveReloadServiceConcern in @(
    'FAvidScriptEditorCSharpBuildService::BuildProfile',
    'FAvidScriptEditorCSharpBuildPipeline',
    'FAvidScriptEditorCSharpBuildInvoker',
    'FMonitoredProcess',
    'FPlatformProcess::ExecProcess'
)) {
    if ($CSharpLiveReloadServiceSource.Contains($ForbiddenLiveReloadServiceConcern)) {
        Add-Violation "C# LiveReloadService lifecycle must not own build pipeline or process concern $ForbiddenLiveReloadServiceConcern"
    }
}
foreach ($RequiredLiveReloadBuildStateContract in @(
    'TickingJob',
    'ExpectedJob',
    'ExpectedJobSerial',
    'ActiveBuildJob->Start',
    'TickingJob->Tick',
    'ActiveBuildJob->IsFinished',
    'ActiveBuildJob->ConsumeResult',
    'IsActiveRequestCurrent',
    'Coordinator.CompleteBuild',
    'ApplyReport',
    'bReadyToBind',
    'actor_identity_changed_during_build',
    'StopInternal(true)'
)) {
    if (-not $CSharpLiveReloadBuildStateSource.Contains($RequiredLiveReloadBuildStateContract)) {
        Add-Violation "C# LiveReloadServiceBuildState is missing request/job contract $RequiredLiveReloadBuildStateContract"
    }
}
foreach ($ForbiddenLiveReloadBuildStateConcern in @(
    'FAvidScriptEditorCSharpProfileService',
    'FAvidScriptEditorCSharpBuildPipeline',
    'FAvidScriptEditorCSharpBuildInvoker',
    'FAvidScriptEditorComponentBindingService',
    'FMonitoredProcess',
    'FPlatformProcess::ExecProcess'
)) {
    if ($CSharpLiveReloadBuildStateSource.Contains($ForbiddenLiveReloadBuildStateConcern)) {
        Add-Violation "C# LiveReloadServiceBuildState must not own profile, pipeline, process, or concrete binding concern $ForbiddenLiveReloadBuildStateConcern"
    }
}
if (-not $CSharpLiveReloadCompletionSource.Contains('FromAsyncBuild') -or
    -not $CSharpLiveReloadCompletionSource.Contains('FromBinding') -or
    $CSharpLiveReloadCompletionSource.Contains('AActor') -or
    $CSharpLiveReloadCompletionSource.Contains('Coordinator') -or
    $CSharpLiveReloadCompletionSource.Contains('FMonitoredProcess')) {
    Add-Violation 'C# LiveReloadCompletion must only map async build and binding results without Actor, coordinator, or process ownership'
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
$SemanticGameplayEventSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticGameplayEventProjector.cs'
$CSharpGuestLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpGuestLowerer.cs'
$CSharpGameplayEventLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpGameplayEventLowerer.cs'
$CSharpGuestDebugMapProjectorSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Diagnostics/CSharpGuestDebugMapProjector.cs'
$CSharpBuildScriptSource = Read-RequiredFile 'Build/BuildCSharpActorLifecycle.ps1'
foreach ($RequiredPreparedBuildContract in @(
    'AvidScriptCSharpSemanticCache.ps1',
    'Import-AvidScriptCSharpPreparedSemantic',
    'prepared_semantic_invalid',
    'frontend_reused',
    'semantic_reused'
)) {
    if (-not $CSharpBuildScriptSource.Contains($RequiredPreparedBuildContract)) {
        Add-Violation "C# build pipeline is missing prepared semantic contract $RequiredPreparedBuildContract"
    }
}
foreach ($RequiredSemanticCacheBuildContract in @(
    'SemanticCacheRoot',
    'DisableSemanticCache',
    'Get-AvidScriptCSharpSemanticCacheContext',
    'Import-AvidScriptCSharpSemanticCacheEntry',
    'Publish-AvidScriptCSharpSemanticCacheEntry',
    'semantic_cache',
    'toolchain_fingerprint',
    'tool_invocations',
    'frontend',
    'semantic',
    'guest_ir',
    'wasm_backend'
)) {
    if (-not $CSharpBuildScriptSource.Contains($RequiredSemanticCacheBuildContract)) {
        Add-Violation "C# build pipeline is missing semantic cache contract $RequiredSemanticCacheBuildContract"
    }
}
foreach ($RequiredDebugArtifactBuildContract in @(
    'imported_function_count',
    'defined_function_count',
    'DebugIndexSpaceValid',
    'DebugImportedFunctionCount',
    'DebugDefinedFunctionCount'
)) {
    if (-not $CSharpBuildScriptSource.Contains($RequiredDebugArtifactBuildContract)) {
        Add-Violation "C# build pipeline is missing debug artifact index-space contract $RequiredDebugArtifactBuildContract"
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
    'ASBI4404',
    'source.source_id'
)) {
    if (-not $CSharpPreparedSemanticSource.Contains($RequiredPreparedHelperContract)) {
        Add-Violation "prepared semantic helper is missing validation contract $RequiredPreparedHelperContract"
    }
}
foreach ($RequiredPreparedPublicationContract in @(
    '[System.IO.FileAttributes]::ReparsePoint',
    '$Committed = $false',
    'Atomic pair publication committed, but backup cleanup failed',
    '$PreserveRecoveryMaterial = $false',
    'Failed to remove published destination'
)) {
    if (-not $CSharpBindingPackageSource.Contains($RequiredPreparedPublicationContract)) {
        Add-Violation "prepared semantic publisher is missing trust or transaction contract $RequiredPreparedPublicationContract"
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
foreach ($RequiredSemanticCacheContract in @(
    'Get-AvidScriptCSharpSemanticCacheContext',
    'Import-AvidScriptCSharpSemanticCacheEntry',
    'Publish-AvidScriptCSharpSemanticCacheEntry',
    'Assert-AvidScriptSemanticCachePublicationContext',
    'Enter-AvidScriptSemanticCacheKeyLock',
    '[System.IO.FileShare]::None',
    'Corrupt',
    'semantic_cache',
    'Get-AvidScriptCSharpToolchainFingerprint',
    'global.json',
    'Build\InvokeCSharpFrontend.ps1',
    'Build\InvokeCSharpSemantic.ps1',
    'AvidScript.CSharpFrontend',
    'AvidScript.CSharpSemantic',
    'ConvertTo-Json -Compress -Depth 32',
    'Sort-Object -Property RelativePath',
    'Test-AvidScriptBindingPathContained',
    'ASBI4501',
    'ASBI4502',
    'ASBI4503',
    'ASBI4504',
    'ASBI4505'
)) {
    if (-not $CSharpSemanticCacheSource.Contains($RequiredSemanticCacheContract)) {
        Add-Violation "C# semantic cache helper is missing deterministic key contract $RequiredSemanticCacheContract"
    }
}
foreach ($ForbiddenSemanticCacheConcern in @(
    '\bStart-Process\b',
    '\bInvoke-AvidScriptPowerShell\b',
    '\bGuestCompilerPath\b',
    '\bGuestIrArtifactPath\b',
    '\bWasmArtifactPath\b',
    '\bRuntimeBindingPackagePath\b',
    '\bOmitRuntimeBindingPackage\b'
)) {
    if ($CSharpSemanticCacheSource -match $ForbiddenSemanticCacheConcern) {
        Add-Violation "C# semantic cache helper owns forbidden process, backend, or package-policy concern $ForbiddenSemanticCacheConcern"
    }
}
if ($CSharpSemanticCacheSource -match 'Get-Content\s+-Raw\s+-LiteralPath\s+\$Source' -or
    $CSharpSemanticCacheSource -match 'ReadAllText\s*\(\s*\$Source') {
    Add-Violation 'C# semantic cache helper must hash user source without parsing its text'
}
foreach ($RequiredReachabilityContract in @(
    'SemanticReachabilityProjector.Project',
    'SemanticStateContractProjector.Project',
    'SemanticGameplayEventProjector.Project',
    'CurrentSchemaVersion = 7',
    'CurrentSemanticVersion = "1.7"'
)) {
    if (-not $SemanticAnalyzerSource.Contains($RequiredReachabilityContract)) {
        Add-Violation "C# Semantic analyzer is missing reachability contract $RequiredReachabilityContract"
    }
}
foreach ($RequiredReachabilityProjection in @('export_roots', 'entrypoint_roots', 'all_callables_compatibility', 'AssociatedSymbolId', 'gameplayEventCallbacks')) {
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
$CSharpStateSchemaProjectorSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/StateMigration/CSharpGuestStateSchemaProjector.cs'
$CSharpStateContractResolverSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/StateMigration/CSharpGuestStateContractResolver.cs'
$CSharpStateSchemaSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/StateMigration/CSharpGuestStateSchema.cs'
$RuntimeStateMigrationSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/StateMigration/AvidScriptRuntimeStateMigration.cpp'
$RuntimeSessionSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Session/AvidScriptRuntimeSession.cpp'
$HostEffectTransactionHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Private/HostEffects/AvidScriptHostEffectTransaction.h'
$HostEffectTransactionSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/HostEffects/AvidScriptHostEffectTransaction.cpp'
$WasmRuntimeHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptWasmRuntime.h'
$BindingInvocationHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptBindingInvocation.h'
$BindingInvocationSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptBindingInvocation.cpp'
$VmBackendContractSource = Read-RequiredFile 'Source/AvidScriptVM/Public/AvidScriptVmBackend.h'
$WamrBackendSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWamrBackend.cpp'
foreach ($RequiredStateSchemaContract in @(
    'CSharpGuestStateSchemaProjector',
    'CSharpGuestStateContractResolver',
    'TryFingerprintType',
    'host_snapshot',
    'ASSTATE1005',
    'SemanticDocument',
    'GuestModule'
)) {
    if (-not $CSharpStateSchemaProjectorSource.Contains($RequiredStateSchemaContract)) {
        Add-Violation "C# state schema projector is missing contract $RequiredStateSchemaContract"
    }
}
foreach ($RequiredStateSchemaRecordContract in @('ContractVersion', 'Aliases', 'JsonPropertyOrder')) {
    if (-not $CSharpStateSchemaSource.Contains($RequiredStateSchemaRecordContract)) {
        Add-Violation "C# state schema record is missing v2 field contract $RequiredStateSchemaRecordContract"
    }
}
foreach ($RequiredStateResolverContract in @(
    'CSharpGuestResolvedStateContract',
    'StateContracts',
    'SemanticSymbol',
    'ASSTATE1002'
)) {
    if (-not $CSharpStateContractResolverSource.Contains($RequiredStateResolverContract)) {
        Add-Violation "C# state contract resolver is missing semantic state contract $RequiredStateResolverContract"
    }
}
foreach ($ForbiddenStateResolverConcern in @('Attribute', 'Syntax', 'Microsoft.CodeAnalysis')) {
    if ($CSharpStateContractResolverSource.Contains($ForbiddenStateResolverConcern)) {
        Add-Violation "C# state contract resolver must consume semantic artifacts without C# source or Attribute parsing: $ForbiddenStateResolverConcern"
    }
}
foreach ($RequiredRuntimeMigrationContract in @(
    'FAvidScriptRuntimeStateMigration::Migrate',
    'ReadStateBytes',
    'WriteStateBytes',
    'CandidatePrimarySlots',
    'CandidateAliasSlots',
    'CandidateOriginalBytes',
    'state_migration_version_regression',
    'state_migration_incompatible',
    'state_migration_read_failed',
    'state_migration_write_failed'
)) {
    if (-not $RuntimeStateMigrationSource.Contains($RequiredRuntimeMigrationContract)) {
        Add-Violation "runtime state migration service is missing contract $RequiredRuntimeMigrationContract"
    }
}
foreach ($RequiredSessionMigrationContract in @(
    'FAvidScriptRuntimeStateMigration::Migrate',
    'bStateMigrationAttempted',
    'bStateMigrationApplied',
    'StateMigrationAliasedSlotCount'
)) {
    if (-not $RuntimeSessionSource.Contains($RequiredSessionMigrationContract)) {
        Add-Violation "RuntimeSession is missing state migration orchestration contract $RequiredSessionMigrationContract"
    }
}
foreach ($ForbiddenSessionMigrationConcern in @('ReadStateBytes', 'WriteStateBytes', 'IAvidScriptVmGuestMemory')) {
    if ($RuntimeSessionSource.Contains($ForbiddenSessionMigrationConcern)) {
        Add-Violation "RuntimeSession must not own guest memory migration concern $ForbiddenSessionMigrationConcern"
    }
}
foreach ($RequiredGameplayEventContract in @('OnBeginOverlap', 'OnEndOverlap', 'OnHit', 'OnInput', 'ASCS5105')) {
    if (-not $SemanticGameplayEventSource.Contains($RequiredGameplayEventContract)) {
        Add-Violation "C# Semantic gameplay event projector is missing contract $RequiredGameplayEventContract"
    }
}
foreach ($RequiredGameplayEventLowering in @('avid_on_gameplay_event', 'stack_alloc', 'field_store', 'ASCG1007')) {
    if (-not $CSharpGameplayEventLowererSource.Contains($RequiredGameplayEventLowering)) {
        Add-Violation "C# Guest gameplay event lowerer is missing contract $RequiredGameplayEventLowering"
    }
}
foreach ($RequiredGameplayDebugMapContract in @('SourceLessGeneratedFunctionIds', 'CSharpGuestIds.GameplayEventFunctionId')) {
    if (-not $CSharpGuestDebugMapProjectorSource.Contains($RequiredGameplayDebugMapContract)) {
        Add-Violation "C# Guest debug map projector is missing generated gameplay router contract $RequiredGameplayDebugMapContract"
    }
}
foreach ($RequiredHostEffectContract in @(
    'IAvidScriptBindingHostEffectJournal',
    'EAvidScriptHostEffectTransactionState',
    'FAvidScriptHostEffectTransactionResult',
    'TWeakObjectPtr<UObject>',
    'TSet<FEntryKey>',
    'FirstPrepareErrorSource'
)) {
    if (-not $HostEffectTransactionHeader.Contains($RequiredHostEffectContract)) {
        Add-Violation "host effect transaction header is missing contract $RequiredHostEffectContract"
    }
}
foreach ($RequiredHostEffectBehavior in @(
    'EAvidScriptBindingReloadEffect::ActorTransform',
    'EAvidScriptBindingReloadEffect::SceneComponentTransform',
    'binding_reload_effect_unsupported',
    'host_effect_restore_failed'
)) {
    if (-not $HostEffectTransactionSource.Contains($RequiredHostEffectBehavior)) {
        Add-Violation "host effect transaction source is missing behavior $RequiredHostEffectBehavior"
    }
}
foreach ($ForbiddenHostEffectOwner in @($WasmRuntimeHeader, $BindingInvocationHeader)) {
    if ($ForbiddenHostEffectOwner.Contains('FAvidScriptHostEffectTransaction')) {
        Add-Violation 'WasmRuntime and BindingPackage contracts must not own the host effect transaction service'
    }
}
foreach ($RequiredSessionHostEffectContract in @(
    'TOptional<FAvidScriptHostEffectTransaction> HostEffectTransaction',
    'HostEffectTransaction.Emplace()',
    'CandidateHostContext.HostEffectJournal = &HostEffectTransaction.GetValue()',
    'HostEffectTransaction->Rollback(',
    'HostEffectTransaction->Commit(',
    'CandidateRuntime->SetHostContext(HostContext)',
    'HostContext.HostEffectJournal = nullptr'
)) {
    if (-not $RuntimeSessionSource.Contains($RequiredSessionHostEffectContract)) {
        Add-Violation "RuntimeSession is missing candidate host effect transaction contract $RequiredSessionHostEffectContract"
    }
}
if ($RuntimeSessionHeader.Contains('FAvidScriptHostEffectTransaction')) {
    Add-Violation 'RuntimeSession public contract must not expose the private host effect transaction service'
}
if (-not $WasmRuntimeHeader.Contains('IAvidScriptBindingHostEffectJournal* HostEffectJournal = nullptr')) {
    Add-Violation 'Wasm host context must expose only the non-owning host effect journal interface'
}
if (-not $RuntimeSource.Contains('InvocationContext.HostEffectJournal = HostContext.HostEffectJournal')) {
    Add-Violation 'Wasm dynamic host calls must forward the candidate host effect journal into binding invocation'
}
$DynamicPrepareIndex = $BindingInvocationSource.IndexOf('Context.HostEffectJournal->PrepareEffect(')
$DynamicProcessEventIndex = $BindingInvocationSource.IndexOf('Target->ProcessEvent(Plan.Function, Frame)')
if (-not $BindingInvocationSource.Contains('binding_reload_effect_unsupported') -or
    $DynamicPrepareIndex -lt 0 -or
    $DynamicProcessEventIndex -lt 0 -or
    $DynamicPrepareIndex -ge $DynamicProcessEventIndex) {
    Add-Violation 'dynamic binding candidate policy must reject unsupported writes and prepare reversible effects before ProcessEvent'
}
if (-not $VmBackendContractSource.Contains('IAvidScriptVmGuestMemory* GetGuestMemory()') -or
    -not $WamrBackendSource.Contains('IAvidScriptVmGuestMemory* GetGuestMemory() override')) {
    Add-Violation 'VM guest memory must be exposed through an optional backend capability implemented by WAMR'
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
