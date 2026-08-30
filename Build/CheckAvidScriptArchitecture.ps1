param(
    [string]$PluginRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$PluginRoot = [System.IO.Path]::GetFullPath($PluginRoot)
$Violations = [System.Collections.Generic.List[string]]::new()
$ArchitectureEvidencePaths = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)

function Add-Violation {
    param([string]$Message)
    $Violations.Add($Message)
}

function Read-RequiredFile {
    param([string]$RelativePath)
    [void]$ArchitectureEvidencePaths.Add($RelativePath.Replace('\', '/'))
    $Path = Join-Path $PluginRoot $RelativePath
    if (-not [System.IO.File]::Exists($Path)) {
        Add-Violation "missing required architecture file: $RelativePath"
        return ''
    }
    return [System.IO.File]::ReadAllText($Path)
}

function Get-SourceSlice {
    param(
        [string]$Source,
        [string]$StartToken,
        [string]$EndToken,
        [string]$Description
    )

    $StartIndex = $Source.IndexOf($StartToken, [System.StringComparison]::Ordinal)
    if ($StartIndex -lt 0) {
        Add-Violation "$Description start token is missing: $StartToken"
        return ''
    }
    $EndIndex = $Source.IndexOf(
        $EndToken,
        $StartIndex + $StartToken.Length,
        [System.StringComparison]::Ordinal)
    if ($EndIndex -lt 0) {
        Add-Violation "$Description end token is missing: $EndToken"
        return ''
    }
    return $Source.Substring($StartIndex, $EndIndex - $StartIndex)
}

function Test-RequiredTokenSequence {
    param(
        [string]$Source,
        [string[]]$Tokens,
        [string]$Description
    )

    $Cursor = 0
    foreach ($Token in $Tokens) {
        $TokenIndex = $Source.IndexOf($Token, $Cursor, [System.StringComparison]::Ordinal)
        if ($TokenIndex -lt 0) {
            Add-Violation "$Description is missing ordered token: $Token"
            return
        }
        $Cursor = $TokenIndex + $Token.Length
    }
}

function Get-CSharpCodeMask {
    param([string]$Source)

    $NonCodePattern = '(?ms)//[^\r\n]*|/\*.*?\*/|(?<raw>"{3,}).*?\k<raw>|' +
        '@"(?:""|[^"])*"|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
    return [regex]::Replace(
        $Source,
        $NonCodePattern,
        {
            param($Match)
            return [regex]::Replace($Match.Value, '[^\r\n]', ' ')
        })
}

function Get-LiteralDependencyNames {
    param(
        [string]$Method,
        [string]$ArgumentText
    )

    if ($Method -ceq 'Add') {
        $Literal = [regex]::Match(
            $ArgumentText,
            '^\s*"(?<name>[A-Za-z_][A-Za-z0-9_.]*)"\s*$')
        if (-not $Literal.Success) {
            return $null
        }
        return @($Literal.Groups['name'].Value)
    }

    $Range = [regex]::Match(
        $ArgumentText,
        '(?s)^\s*new\s*(?:string\s*)?\[\s*\]\s*\{\s*(?<items>.*?)\s*\}\s*$')
    if (-not $Range.Success) {
        return $null
    }
    $Items = $Range.Groups['items'].Value
    if ([string]::IsNullOrWhiteSpace($Items)) {
        return @()
    }
    $Parts = @($Items.Split(','))
    if ([string]::IsNullOrWhiteSpace($Parts[-1])) {
        $Parts = @($Parts | Select-Object -First ($Parts.Count - 1))
    }
    $Names = [System.Collections.Generic.List[string]]::new()
    foreach ($Part in $Parts) {
        $Literal = [regex]::Match(
            $Part,
            '^\s*"(?<name>[A-Za-z_][A-Za-z0-9_.]*)"\s*$')
        if (-not $Literal.Success) {
            return $null
        }
        $Names.Add($Literal.Groups['name'].Value)
    }
    return @($Names)
}

function Get-BuildDependencyAnalysis {
    param(
        [string]$Source,
        [ValidateSet('Public', 'Private')][string]$Visibility
    )

    $CodeMask = Get-CSharpCodeMask -Source $Source
    $OccurrencePattern = '\b' + [regex]::Escape($Visibility) +
        'DependencyModuleNames\b'
    $AllowedInvocationPattern = '^(?s)' + [regex]::Escape($Visibility) +
        'DependencyModuleNames\s*\.\s*(?<method>Add|AddRange)\s*\('
    $Names = [System.Collections.Generic.List[string]]::new()
    $UnresolvedCount = 0
    $UnsupportedCount = 0
    foreach ($Occurrence in [regex]::Matches($CodeMask, $OccurrencePattern)) {
        $Invocation = [regex]::Match(
            $CodeMask.Substring($Occurrence.Index),
            $AllowedInvocationPattern)
        if (-not $Invocation.Success) {
            ++$UnsupportedCount
            continue
        }
        $OpenParenthesis = $Occurrence.Index + $Invocation.Length - 1
        $Depth = 0
        $CloseParenthesis = -1
        for ($Index = $OpenParenthesis; $Index -lt $CodeMask.Length; ++$Index) {
            if ($CodeMask[$Index] -ceq '(') {
                ++$Depth
            }
            elseif ($CodeMask[$Index] -ceq ')') {
                --$Depth
                if ($Depth -eq 0) {
                    $CloseParenthesis = $Index
                    break
                }
            }
        }
        if ($CloseParenthesis -lt 0 -or
            -not $CodeMask.Substring($CloseParenthesis + 1) -match '^\s*;') {
            ++$UnresolvedCount
            continue
        }
        $ArgumentText = $Source.Substring(
            $OpenParenthesis + 1,
            $CloseParenthesis - $OpenParenthesis - 1)
        $LiteralNames = Get-LiteralDependencyNames `
            -Method $Invocation.Groups['method'].Value `
            -ArgumentText $ArgumentText
        if ($null -eq $LiteralNames) {
            ++$UnresolvedCount
            continue
        }
        foreach ($Name in @($LiteralNames)) {
            $Names.Add($Name)
        }
    }
    return [pscustomobject]@{
        Names = @($Names)
        UnresolvedCount = $UnresolvedCount
        UnsupportedCount = $UnsupportedCount
    }
}

function Test-NameAllowlist {
    param(
        [string[]]$ActualNames,
        [string[]]$ExpectedNames,
        [string]$Description
    )

    $MissingNames = @($ExpectedNames | Where-Object { $ActualNames -notcontains $_ })
    $UnexpectedNames = @($ActualNames | Where-Object { $ExpectedNames -notcontains $_ })
    $DuplicateNames = @(
        $ActualNames
        | Group-Object
        | Where-Object { $_.Count -ne 1 }
        | ForEach-Object { $_.Name })
    if ($MissingNames.Count -gt 0 -or
        $UnexpectedNames.Count -gt 0 -or
        $DuplicateNames.Count -gt 0 -or
        $ActualNames.Count -ne $ExpectedNames.Count) {
        $AllowlistViolation = "$Description differs from the static host import allowlist" `
            + " | missing=$($MissingNames -join ',')" `
            + " | unexpected=$($UnexpectedNames -join ',')" `
            + " | duplicate=$($DuplicateNames -join ',')"
        Add-Violation $AllowlistViolation
    }
}

$ArchitectureScriptPath = [System.IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)
$PluginRootPrefix = $PluginRoot.TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
if ($ArchitectureScriptPath.StartsWith(
    $PluginRootPrefix,
    [System.StringComparison]::OrdinalIgnoreCase)) {
    [void]$ArchitectureEvidencePaths.Add(
        [System.IO.Path]::GetRelativePath($PluginRoot, $ArchitectureScriptPath).Replace('\', '/'))
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

        $RelativePath = [System.IO.Path]::GetRelativePath($PluginRoot, $Path).Replace('\', '/')
        [void]$ArchitectureEvidencePaths.Add($RelativePath)
        $Text = [System.IO.File]::ReadAllText($Path)
        foreach ($Pattern in $Patterns) {
            if ([regex]::IsMatch($Text, $Pattern)) {
                Add-Violation "$RelativePath matches forbidden dependency pattern '$Pattern'"
            }
        }
    }
}

$CoreBuild = Read-RequiredFile 'Source/AvidScriptCore/AvidScriptCore.Build.cs'
foreach ($ForbiddenDependency in @('CoreUObject', 'Engine', 'WAMR', 'Wasmtime', 'Json', 'AvidScriptRuntime', 'AvidScriptBindings', 'AvidScriptEditor')) {
    if ($CoreBuild.Contains('"' + $ForbiddenDependency + '"')) {
        Add-Violation "AvidScriptCore must not depend on $ForbiddenDependency"
    }
}

Test-SourceTreeForbiddenPattern 'Source/AvidScriptCore' @(
    '#include\s+["<](Engine|GameFramework|Components|UObject)/',
    'wasm_runtime_|wasm_export\.h',
    '#include\s+["<]AvidScript(ActorBinding|ObjectRegistry|SceneComponentBinding|WasmRuntime|Component)\.h'
)

Test-SourceTreeForbiddenPattern 'Source' @(
    '\bCountByPredicate\s*\(',
    'FString::Printf\s*\(\s*\*'
)

$BindingsBuild = Read-RequiredFile 'Source/AvidScriptBindings/AvidScriptBindings.Build.cs'
foreach ($RequiredDependency in @('AvidScriptCore', 'CoreUObject', 'Engine', 'Json')) {
    if (-not $BindingsBuild.Contains('"' + $RequiredDependency + '"')) {
        Add-Violation "AvidScriptBindings is missing required dependency $RequiredDependency"
    }
}
foreach ($ForbiddenDependency in @('WAMR', 'Wasmtime', 'UnrealEd', 'AvidScriptRuntime', 'AvidScriptEditor')) {
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
}
$VmPrivateDependencyAnalysis = Get-BuildDependencyAnalysis -Source $VmBuild -Visibility Private
$VmPublicDependencyAnalysis = Get-BuildDependencyAnalysis -Source $VmBuild -Visibility Public
if ($VmPrivateDependencyAnalysis.UnresolvedCount -gt 0) {
    Add-Violation 'AvidScriptVM private dependencies must use literal-only Add/AddRange declarations'
}
if ($VmPublicDependencyAnalysis.UnresolvedCount -gt 0) {
    Add-Violation 'AvidScriptVM public dependencies must use literal-only Add/AddRange declarations'
}
if ($VmPrivateDependencyAnalysis.UnsupportedCount -gt 0) {
    Add-Violation 'AvidScriptVM private dependency-list occurrences must be literal-only Add/AddRange calls'
}
if ($VmPublicDependencyAnalysis.UnsupportedCount -gt 0) {
    Add-Violation 'AvidScriptVM public dependency-list occurrences must be literal-only Add/AddRange calls'
}
foreach ($RequiredVmBackendDependency in @('WAMR', 'Wasmtime')) {
    if ($VmPrivateDependencyAnalysis.Names -notcontains $RequiredVmBackendDependency) {
        Add-Violation "AvidScriptVM is missing its private $RequiredVmBackendDependency dependency"
    }
}
if ($VmPublicDependencyAnalysis.Names -contains 'Wasmtime') {
    Add-Violation 'AvidScriptVM must not expose Wasmtime through PublicDependencyModuleNames'
}
foreach ($ForbiddenDependency in @('CoreUObject', 'Engine', 'Json', 'UnrealEd', 'AvidScriptBindings', 'AvidScriptRuntime', 'AvidScriptEditor')) {
    if ($VmBuild.Contains('"' + $ForbiddenDependency + '"')) {
        Add-Violation "AvidScriptVM must not depend on $ForbiddenDependency"
    }
}
if ($VmBuild.Contains('bUseUnity = false')) {
    Add-Violation 'AvidScriptVM must preserve Unity builds; third-party headers require an explicit compile boundary'
}

Test-SourceTreeForbiddenPattern 'Source/AvidScriptVM/Public' @(
    'wasm_runtime_|wasm_export\.h',
    'wasmtime\.h|AVIDSCRIPT_WITH_WASMTIME',
    '#include\s+["<](Engine|GameFramework|Components|UObject)/',
    '\b(UObject|AActor|USceneComponent|FVector|FRotator|FTransform)\b'
)

Test-SourceTreeForbiddenPattern 'Source/AvidScriptVM' @(
    '\b(UFunction|FProperty|FAvidScriptObjectRegistry|FNameProperty|FStrProperty|FAvidScriptUtf8ValueHeap)\b',
    '\bFName\b',
    '\b(name_utf8|string_utf8)\b'
)

$WasmtimeBuild = Read-RequiredFile 'Source/ThirdParty/Wasmtime/Wasmtime.Build.cs'
$WasmtimeLock = Read-RequiredFile 'Source/ThirdParty/Wasmtime/WasmtimeDependency.lock.json'
$WasmtimeInstaller = Read-RequiredFile 'Build/InstallWasmtimeDependency.ps1'
foreach ($RequiredWasmtimeBuildContract in @(
    'ModuleType.External',
    'AVIDSCRIPT_WITH_WASMTIME=0',
    'AVIDSCRIPT_WITH_WASMTIME=1',
    'PublicIncludePaths.Add',
    'PublicAdditionalLibraries.Add',
    'wasmtime.dll.lib',
    'PublicDelayLoadDLLs.Add("wasmtime.dll")',
    'RuntimeDependencies.Add',
    '$(PluginDir)/Binaries/Win64/wasmtime.dll')) {
    if (-not $WasmtimeBuild.Contains($RequiredWasmtimeBuildContract)) {
        Add-Violation "Wasmtime external module is missing $RequiredWasmtimeBuildContract"
    }
}
if ($WasmtimeBuild -match '"wasmtime\.lib"') {
    Add-Violation 'Wasmtime external module must not link the static wasmtime.lib'
}
foreach ($RequiredWasmtimeLockIdentity in @(
    '"dependency": "Wasmtime Cranelift C API"',
    '"version": "v45.0.0"',
    '"size_bytes": 28820070',
    '"sha256": "d5ee516fc141576ccd6c43146aafee1074c3c26764cba73b3a97f599a3791f9c"',
    '"include_relative_path": "include"',
    '"dll_relative_path": "lib/wasmtime.dll"',
    '"relative_path": "Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0"')) {
    if (-not $WasmtimeLock.Contains($RequiredWasmtimeLockIdentity)) {
        Add-Violation "Wasmtime dependency lock is missing $RequiredWasmtimeLockIdentity"
    }
}
if (-not $WasmtimeInstaller.Contains('WASMTIME_FEATURE_CRANELIFT')) {
    Add-Violation 'Wasmtime dependency installer must reject layouts without Cranelift'
}

$VmContractHeader = Read-RequiredFile 'Source/AvidScriptVM/Public/AvidScriptVmBackend.h'
$VmArtifactHeader = Read-RequiredFile 'Source/AvidScriptVM/Public/AvidScriptVmArtifact.h'
$VmArtifactCompilerSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptVmArtifactCompiler.cpp'
$WasmtimeRuntimeSupportSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWasmtimeRuntimeSupport.cpp'
$VmWasmtimeTests = Read-RequiredFile 'Source/AvidScriptVM/Private/Tests/AvidScriptVmWasmtimeTests.cpp'
foreach ($RequiredBatchContract in @(
    'ActorGetTransformBatch',
    'InputCells',
    'OutputFloats',
    'BorrowReadOnlyBytes',
    'BorrowMutableBytes')) {
    if (-not $VmContractHeader.Contains($RequiredBatchContract)) {
        Add-Violation "VM batch contract is missing $RequiredBatchContract"
    }
}
foreach ($RequiredArtifactContract in @(
    'FAvidScriptVmOwnedArtifact',
    'CompileAvidScriptVmArtifact',
    'AuthorizeAvidScriptVmArtifact',
    'AttestationId')) {
    if (-not $VmArtifactHeader.Contains($RequiredArtifactContract)) {
        Add-Violation "VM owned-artifact contract is missing $RequiredArtifactContract"
    }
}
foreach ($RequiredArtifactCompilerContract in @(
    'ArtifactCacheCapacity = 32',
    'AttestationRegistryCapacity = 32',
    'avidscript_wasmtime_module_serialize',
    'RegisterArtifactAttestationLocked',
    'FAvidScriptHash::Sha256Hex')) {
    if (-not $VmArtifactCompilerSource.Contains($RequiredArtifactCompilerContract)) {
        Add-Violation "VM artifact compiler is missing $RequiredArtifactCompilerContract"
    }
}
foreach ($RequiredStartupDiagnosticContract in @(
    'AvidScript.VM.Wasmtime.PrecompiledStartupDiagnostic',
    'SampleCount = 9',
    'cache_miss_compile_ms=',
    'serialized_jit_module_load_ratio=',
    'ModuleLoadRatio <= 0.50',
    'every startup sample executes BeginPlay')) {
    if (-not $VmWasmtimeTests.Contains($RequiredStartupDiagnosticContract)) {
        Add-Violation "Wasmtime precompiled startup diagnostic is missing $RequiredStartupDiagnosticContract"
    }
}
foreach ($RequiredRuntimeIdentityContract in @(
    'ResolveAvidScriptWasmtimeRuntimeIdentity',
    'AVIDSCRIPT_WASMTIME_DLL_SHA256',
    'GWasmtimeDllHandle',
    'RuntimeBuildIdentity')) {
    if (-not $WasmtimeRuntimeSupportSource.Contains($RequiredRuntimeIdentityContract)) {
        Add-Violation "Wasmtime runtime identity owner is missing $RequiredRuntimeIdentityContract"
    }
}
$WamrBuildScript = Read-RequiredFile 'Build/BuildWAMRWin64.cmd'
$WamrCommonCMake = Read-RequiredFile 'Source/ThirdParty/WAMR/upstream/core/iwasm/common/iwasm_common.cmake'
$WamrRuntimeCMake = Read-RequiredFile 'Source/ThirdParty/WAMR/upstream/build-scripts/runtime_lib.cmake'
$WamrSimdeCMake = Read-RequiredFile 'Source/ThirdParty/WAMR/upstream/core/iwasm/libraries/simde/simde.cmake'
$WasmtimeShimHeader = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWasmtimeApi.h'
$WasmtimeShimSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWasmtimeApi.c'
$WasmtimeShimInternal = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWasmtimeApiInternal.h'
$WasmtimeBackendSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWasmtimeBackend.cpp'
$WasmtimeArtifactCompilerSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptVmArtifactCompiler.cpp'
$WasmtimeRuntimeSupportSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWasmtimeRuntimeSupport.cpp'
$WasmtimeCompilerProfileSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWasmtimeCompilerProfile.cpp'
$WasmtimeDynamicDispatchSlice = Get-SourceSlice `
    $WasmtimeBackendSource `
    'bool InvokeDynamicHostImport(' `
    'int32 InvokeTypedEmptyI32('
Test-RequiredTokenSequence $WasmtimeDynamicDispatchSlice @(
    'HostContext.PreparedTarget.IsBound()',
    'HostContext.PreparedTarget.Invoke(',
    'else',
    'HostDispatcher->DispatchDynamicHostCall') `
    'Wasmtime dynamic dispatch must prefer a prepared target and use the dispatcher only when unbound'
$DynamicDispatcherCalls = [regex]::Matches(
    $WasmtimeDynamicDispatchSlice,
    'HostDispatcher->DispatchDynamicHostCall').Count
if ($DynamicDispatcherCalls -ne 1) {
    Add-Violation 'Wasmtime prepared dynamic rejection must not replay through the dispatcher'
}
if ($WasmtimeBackendSource.Contains('AVIDSCRIPT_WASMTIME_DLL_SHA256') -or
    $WasmtimeBackendSource.Contains('EnsureWasmtimeDllLoaded') -or
    -not $WasmtimeBackendSource.Contains('ResolveAvidScriptWasmtimeCompilerProfile') -or
    -not $WasmtimeArtifactCompilerSource.Contains('ResolveAvidScriptWasmtimeCompilerProfile') -or
    -not $WasmtimeRuntimeSupportSource.Contains('FPlatformProcess::GetDllExport') -or
    -not $WasmtimeRuntimeSupportSource.Contains('BuildAvidScriptWasmtimeCompilerIdentity') -or
    -not $WasmtimeCompilerProfileSource.Contains('wasmtime-v%s+avidscript.1')) {
    Add-Violation 'Wasmtime backend must consume the shared runtime identity owner without duplicating DLL identity logic'
}
$StaticHostImportSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptVmStaticHostImports.cpp'
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
foreach ($RequiredWamrSimdToken in @(
    '-DWAMR_BUILD_SIMD=1',
    '-DWAMR_BUILD_LIB_SIMDE=1')) {
    if (-not $WamrBuildScript.Contains($RequiredWamrSimdToken)) {
        Add-Violation "Win64 WAMR SIMD support is missing $RequiredWamrSimdToken"
    }
}
foreach ($RequiredWamrSymbolIsolationToken in @(
    '-DWAMR_BUILD_WASM_C_API=0',
    'set "PATH=%SystemRoot%\System32;%PATH%"',
    '%SystemRoot%\System32\subst.exe',
    '%SystemRoot%\System32\findstr.exe',
    '/linkermember:1',
    'wasm_runtime_init$',
    'wasm_runtime_load$',
    'wasm_config_',
    'wasm_engine_',
    'wasm_functype_',
    'wasm_trap_')) {
    if (-not $WamrBuildScript.Contains($RequiredWamrSymbolIsolationToken)) {
        Add-Violation "Win64 WAMR symbol isolation is missing $RequiredWamrSymbolIsolationToken"
    }
}
foreach ($RequiredWamrCMakeIsolationToken in @(
    'WAMR_BUILD_WASM_C_API EQUAL 0',
    'list(REMOVE_ITEM c_source_all "${IWASM_COMMON_DIR}/wasm_c_api.c")')) {
    if (-not $WamrCommonCMake.Contains($RequiredWamrCMakeIsolationToken)) {
        Add-Violation "vendored WAMR CMake symbol isolation is missing $RequiredWamrCMakeIsolationToken"
    }
}
if ($WamrRuntimeCMake.Contains('SIMDe doesnt support platform') -or
    -not $WamrRuntimeCMake.Contains('include (${IWASM_DIR}/libraries/simde/simde.cmake)') -or
    -not $WamrRuntimeCMake.Contains('set (WAMR_BUILD_SIMDE 1)')) {
    Add-Violation 'vendored WAMR fast interpreter must enable pinned SIMDe for Win64 SIMD128'
}
if (-not $WamrFastInterpreterSource.Contains('simd_v128_to_simde_v128(V128 value)') -or
    -not $WamrFastInterpreterSource.Contains('#define SIMD_V128_TO_SIMDE_V128(s_v) simd_v128_to_simde_v128(s_v)') -or
    $WamrFastInterpreterSource.Contains('({')) {
    Add-Violation 'vendored WAMR SIMD value conversion must remain alias-safe and MSVC-compatible'
}
foreach ($RequiredWamrSimdeIdentity in @(
    '71fd833d9666141edcd1d3c109a80e228303d8d7.tar.gz',
    'SHA256=72b2c14a487560b7eb203795f2c2fead5c7499662e639944cca2a9bb19f09029')) {
    if (-not $WamrSimdeCMake.Contains($RequiredWamrSimdeIdentity)) {
        Add-Violation "vendored WAMR SIMDe dependency is missing $RequiredWamrSimdeIdentity"
    }
}
foreach ($RequiredWasmtimeShimToken in @(
    'avidscript_wasmtime_engine_new',
    'avidscript_wasmtime_linker_define_func',
    'const uint32_t* cells',
    'avidscript_wasmtime_memory_data')) {
    if (-not $WasmtimeShimHeader.Contains($RequiredWasmtimeShimToken) -or
        -not $WasmtimeShimSource.Contains($RequiredWasmtimeShimToken)) {
        Add-Violation "Wasmtime unique-prefix C shim is missing $RequiredWasmtimeShimToken"
    }
}
if (-not $WasmtimeShimInternal.Contains('#include "wasmtime.h"') -or
	$WasmtimeShimSource.Contains('#include "wasmtime.h"') -or
    $WasmtimeBackendSource.Contains('#include "wasmtime.h"')) {
    Add-Violation 'Wasmtime headers must remain isolated to the unique-prefix C shim'
}
if ($WasmtimeShimSource.Contains('GetProcAddress') -or $WasmtimeBackendSource.Contains('GetProcAddress')) {
    Add-Violation 'Wasmtime integration must not use dynamic per-symbol lookup'
}
foreach ($RequiredBorrowOverride in @('BorrowReadOnlyBytes', 'BorrowMutableBytes')) {
    if (-not $WasmtimeBackendSource.Contains($RequiredBorrowOverride)) {
        Add-Violation "Wasmtime guest memory is missing $RequiredBorrowOverride"
    }
}
foreach ($RequiredWasmtimeDynamicToken in @(
    'FAvidScriptWasmtimeDynamicHostContext',
    'DynamicHostContexts.Reserve',
    'DispatchDynamicHostCall',
    'uint64 ArgumentCells[64]',
    'PendingHostImportModuleName')) {
    if (-not $WasmtimeBackendSource.Contains($RequiredWasmtimeDynamicToken)) {
        Add-Violation "Wasmtime per-instance dynamic import path is missing $RequiredWasmtimeDynamicToken"
    }
}
if ($StaticHostImportSource.Contains('TArray<uint32> InputCells') -or
    $StaticHostImportSource.Contains('TArray<float> OutputFloats')) {
    Add-Violation 'transform batch adapter must borrow guest spans without per-call arrays'
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
$VmDynamicRegistrySource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWamrDynamicRegistry.cpp'
foreach ($RequiredVmPrimitive in @('BorrowReadOnlyBytes', 'BorrowMutableBytes', 'actor_get_transform_batch', 'avid_data_lane_submit')) {
    if (-not $StaticHostImportSource.Contains($RequiredVmPrimitive)) {
        Add-Violation "shared VM guest-memory adapter is missing $RequiredVmPrimitive"
    }
}
if (-not $VmHostBindingsSource.Contains('TranslateGuestRange')) {
    Add-Violation 'WAMR guest-memory adapter is missing TranslateGuestRange'
}

$RuntimeBuild = Read-RequiredFile 'Source/AvidScriptRuntime/AvidScriptRuntime.Build.cs'
foreach ($RequiredDependency in @('AvidScriptCore', 'AvidScriptBindings', 'AvidScriptVM')) {
    if (-not $RuntimeBuild.Contains('"' + $RequiredDependency + '"')) {
        Add-Violation "AvidScriptRuntime is missing required dependency $RequiredDependency"
    }
}
foreach ($ForbiddenRuntimeVmDependency in @('WAMR', 'Wasmtime')) {
    if ($RuntimeBuild.Contains('"' + $ForbiddenRuntimeVmDependency + '"')) {
        Add-Violation "AvidScriptRuntime must not depend directly on $ForbiddenRuntimeVmDependency"
    }
}
if ($RuntimeBuild.Contains('"AvidScriptGenerated"')) {
    Add-Violation 'AvidScriptRuntime must not depend on the project-specific AvidScriptGenerated module'
}

$GeneratedBuild = Read-RequiredFile 'Source/AvidScriptGenerated/AvidScriptGenerated.Build.cs'
foreach ($RequiredGeneratedDependency in @('AvidScriptRuntime', 'Core', 'CoreUObject', 'Engine')) {
    if (-not $GeneratedBuild.Contains('"' + $RequiredGeneratedDependency + '"')) {
        Add-Violation "AvidScriptGenerated is missing required dependency $RequiredGeneratedDependency"
    }
}
foreach ($ForbiddenGeneratedDependency in @('AvidScriptEditor', 'AvidScriptVM', 'WAMR', 'Wasmtime')) {
    if ($GeneratedBuild.Contains('"' + $ForbiddenGeneratedDependency + '"')) {
        Add-Violation "AvidScriptGenerated must not depend directly on $ForbiddenGeneratedDependency"
    }
}
$GeneratedDispatcherHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/ScriptTypes/AvidScriptGeneratedTypeDispatcher.h'
$GeneratedDispatcherSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/ScriptTypes/AvidScriptGeneratedTypeDispatcher.cpp'
$GeneratedTypeRouterHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/ScriptTypes/AvidScriptGeneratedTypeRouter.h'
$GeneratedTypeRouterSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/ScriptTypes/AvidScriptGeneratedTypeRouter.cpp'
$GeneratedTypeRegistryHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/ScriptTypes/AvidScriptGeneratedTypeRegistry.h'
$GeneratedTypeRegistrySource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/ScriptTypes/AvidScriptGeneratedTypeRegistry.cpp'
$GeneratedTypeSessionHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptRuntimeSession.h'
$GeneratedTypeSessionSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Session/AvidScriptRuntimeSession.cpp'
$GeneratedTypeSessionDispatchSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/ScriptTypes/AvidScriptGeneratedTypeSession.cpp'
$GeneratedTypeWasmRuntimeHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptWasmRuntime.h'
$RuntimeModuleSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptRuntimeModule.cpp'
$CSharpScriptTypeBuildSource = Read-RequiredFile 'Build/BuildCSharpScriptTypes.ps1'
$UeTypeShellRendererSource = Read-RequiredFile 'Tools/AvidScript.UeTypeGenerator/Generation/UhtShellRenderer.cs'
$SemanticUeRuntimeContractSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Model/SemanticUeTypeRuntimeContract.cs'
$CSharpUePropertyPlanSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpUePropertyAccessPlan.cs'
$CSharpGuestLowererSourceForUeTypes = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpGuestLowerer.cs'
$UeTypeManifestSource = Read-RequiredFile 'Tools/AvidScript.UeTypeGenerator/Model/UeTypeGenerationManifest.cs'
foreach ($RequiredGeneratedDispatchContract in @(
    'IAvidScriptGeneratedTypeDispatchTarget',
    'FAvidScriptGeneratedTypeDispatcher',
    'TypeOrdinal',
    'MemberOrdinal')) {
    if (-not $GeneratedDispatcherHeader.Contains($RequiredGeneratedDispatchContract)) {
        Add-Violation "generated type dispatcher contract is missing $RequiredGeneratedDispatchContract"
    }
}
foreach ($ForbiddenGeneratedHotPath in @('FName', 'FindFunction', 'FindObject', 'StaticFindObject')) {
    if ($GeneratedDispatcherSource.Contains($ForbiddenGeneratedHotPath)) {
        Add-Violation "generated type dispatcher hot path must not use $ForbiddenGeneratedHotPath"
    }
}
foreach ($RequiredGeneratedTypeRouterContract in @(
    'IAvidScriptGeneratedTypeInstance',
    'FAvidScriptGeneratedTypeInstanceRegistration',
    'FAvidScriptObjectHandle',
    'FObjectKey',
    'ActiveDispatchDepth')) {
    if (-not $GeneratedTypeRouterHeader.Contains($RequiredGeneratedTypeRouterContract) -and
        -not $GeneratedTypeRouterSource.Contains($RequiredGeneratedTypeRouterContract)) {
        Add-Violation "generated type instance router is missing $RequiredGeneratedTypeRouterContract"
    }
}
foreach ($ForbiddenGeneratedTypeRouterHotPath in @('FindFunction', 'FindObject', 'StaticFindObject', 'GetName')) {
    if ($GeneratedTypeRouterSource.Contains($ForbiddenGeneratedTypeRouterHotPath)) {
        Add-Violation "generated type instance router hot path must not use $ForbiddenGeneratedTypeRouterHotPath"
    }
}
foreach ($RequiredGeneratedTypeRegistryContract in @(
    'ManifestSchemaVersion = 5',
    'BuildFromJson',
    'FindTypeByOrdinal',
    'FindTypeByStableId',
    'FindTypeByClass',
    'FindFProperty<FProperty>',
    'FindFunctionByName')) {
    if (-not $GeneratedTypeRegistryHeader.Contains($RequiredGeneratedTypeRegistryContract) -and
        -not $GeneratedTypeRegistrySource.Contains($RequiredGeneratedTypeRegistryContract)) {
        Add-Violation "generated type immutable registry is missing $RequiredGeneratedTypeRegistryContract"
    }
}
foreach ($RequiredGeneratedTypeSessionContract in @(
    'public IAvidScriptGeneratedTypeInstance',
    'ConfigureGeneratedTypeInstance',
    'PrepareGeneratedTypeExports',
    'PrepareNamedExportCall',
    'ReceiverHandle.Slot',
    'ReceiverHandle.Generation',
    'FAvidScriptVmPreparedExportCall')) {
    if (-not $GeneratedTypeSessionHeader.Contains($RequiredGeneratedTypeSessionContract) -and
        -not $GeneratedTypeSessionSource.Contains($RequiredGeneratedTypeSessionContract) -and
        -not $GeneratedTypeSessionDispatchSource.Contains($RequiredGeneratedTypeSessionContract) -and
        -not $GeneratedTypeWasmRuntimeHeader.Contains($RequiredGeneratedTypeSessionContract)) {
        Add-Violation "generated type Session dispatch is missing $RequiredGeneratedTypeSessionContract"
    }
}
foreach ($RequiredGeneratedRouterModuleLifecycle in @(
    'FAvidScriptGeneratedTypeRouter::Get().Startup()',
    'FAvidScriptGeneratedTypeRouter::Get().Shutdown()')) {
    if (-not $RuntimeModuleSource.Contains($RequiredGeneratedRouterModuleLifecycle)) {
        Add-Violation "Runtime module does not own generated type router lifecycle: $RequiredGeneratedRouterModuleLifecycle"
    }
}
foreach ($RequiredUePropertyRuntimeContract in @(
    'BuildMemberOrdinals',
    'BuildPropertyPlans',
    'GetterImportName',
    'SetterImportName',
    'SemanticUePropertyInitializer')) {
    if (-not $SemanticUeRuntimeContractSource.Contains($RequiredUePropertyRuntimeContract) -and
        -not $UeTypeManifestSource.Contains($RequiredUePropertyRuntimeContract)) {
        Add-Violation "generated UE property runtime contract is missing $RequiredUePropertyRuntimeContract"
    }
}
foreach ($RequiredUePropertyGuestContract in @(
    'CSharpUePropertyAccessPlan.Build(document)',
    'AppendUePropertyImports',
    'calledTargets.Contains(importId)')) {
    if (-not $CSharpUePropertyPlanSource.Contains($RequiredUePropertyGuestContract) -and
        -not $CSharpGuestLowererSourceForUeTypes.Contains($RequiredUePropertyGuestContract)) {
        Add-Violation "C# Guest UE property lowering is missing $RequiredUePropertyGuestContract"
    }
}
foreach ($RequiredScriptTypeBuildContract in @(
    'Resolve-AvidScriptCSharpBindingPackage',
    'InvokeCSharpFrontend.ps1',
    'InvokeCSharpSemantic.ps1',
    'schema_version -ne 19',
    'semantic_version -cne "1.21"',
    'schema_version -ne 5',
    'generator_version -cne "1.4"',
    'semantic_artifact_sha256',
    'Get-FileHash')) {
    if (-not $CSharpScriptTypeBuildSource.Contains($RequiredScriptTypeBuildContract)) {
        Add-Violation "C# script type build pipeline is missing $RequiredScriptTypeBuildContract"
    }
}
if ($CSharpScriptTypeBuildSource -match '(?i)(?<![A-Za-z0-9])[0-9a-f]{64}(?![A-Za-z0-9])') {
    Add-Violation 'C# script type build pipeline must not hardcode a content-addressed binding package hash'
}
foreach ($RequiredGeneratedReflectionContract in @(
    'FAvidScriptGeneratedTypeDispatcher::Invoke(',
    'AvidScript.GeneratedTypes.Reflection',
    'FindFProperty<FProperty>',
    'FindFunctionByName',
    'RenderInitializer',
    'property.Initializer')) {
    if (-not $UeTypeShellRendererSource.Contains($RequiredGeneratedReflectionContract)) {
        Add-Violation "generated UE type renderer is missing $RequiredGeneratedReflectionContract"
    }
}

Test-SourceTreeForbiddenPattern 'Source/AvidScriptRuntime' @(
    'wasm_runtime_|wasm_export\.h',
    'AVIDSCRIPT_WITH_WAMR',
    'wasmtime\.h|AVIDSCRIPT_WITH_WASMTIME'
)

Test-SourceTreeForbiddenPattern 'Source/AvidScriptEditor' @(
    'wasm_runtime_|wasm_export\.h',
    'AVIDSCRIPT_WITH_WAMR',
    'wasmtime\.h|AVIDSCRIPT_WITH_WASMTIME'
)
$EditorBuild = Read-RequiredFile 'Source/AvidScriptEditor/AvidScriptEditor.Build.cs'
if ($EditorBuild.Contains('"Wasmtime"')) {
    Add-Violation 'AvidScriptEditor must not depend directly on Wasmtime'
}
$EditorArtifactPublisherSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorVmArtifactPublisher.cpp'
$EditorCSharpBuildPipelineSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBuildPipeline.cpp'
$CSharpGuestBuildScript = Read-RequiredFile 'Build/BuildCSharpActorLifecycle.ps1'
foreach ($RequiredEditorArtifactContract in @(
    'CompileAvidScriptVmArtifact',
    'wasmtime_serialized_v1',
    'canonical_sha256',
    'compiler_build_identity',
    'attestation_id',
    'RemoveStaleArtifact',
    'WriteManifestAtomic')) {
    if (-not $EditorArtifactPublisherSource.Contains($RequiredEditorArtifactContract)) {
        Add-Violation "Editor VM artifact publisher is missing $RequiredEditorArtifactContract"
    }
}
if ($CSharpGuestBuildScript.Contains('.cwasm') -or
    $CSharpGuestBuildScript.Contains('wasmtime compile')) {
    Add-Violation 'C# PowerShell build must not bypass the native Editor VM artifact publisher'
}
foreach ($RequiredEditorArtifactTransactionContract in @(
    'FAvidScriptEditorVmArtifactPublisher::MakeArtifactPath',
    'EAvidScriptEditorVmArtifactPolicy::JitOnly',
    'FAvidScriptEditorVmArtifactPublisher::Publish')) {
    if (-not $EditorCSharpBuildPipelineSource.Contains($RequiredEditorArtifactTransactionContract)) {
        Add-Violation "Editor C# artifact transaction is missing $RequiredEditorArtifactTransactionContract"
    }
}
$EditorCompleteFinalSlice = Get-SourceSlice `
    $EditorCSharpBuildPipelineSource `
    'bool FAvidScriptEditorCSharpBuildPipeline::CompleteFinal(' `
    'void FAvidScriptEditorCSharpBuildPipeline::Cleanup('
Test-RequiredTokenSequence $EditorCompleteFinalSlice @(
    'FAvidScriptEditorVmArtifactPublisher::Publish',
    'FinishArtifactTransaction(bCommit)') `
    'Editor must publish the VM artifact before committing the existing C# artifact transaction'

$RuntimeHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptWasmRuntime.h'
$RuntimeSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp'
$RuntimeArtifactHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptRuntimeArtifact.h'
$RuntimeArtifactSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptRuntimeArtifact.cpp'
$RuntimeSessionHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptRuntimeSession.h'
$RuntimeSessionSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Session/AvidScriptRuntimeSession.cpp'
$RuntimeBackendLaneHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Tests/AvidScriptRuntimeBackendTestLanes.h'
$RuntimeWasmTests = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Tests/AvidScriptWasmRuntimeTests.cpp'
$BindingInvocationHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptBindingInvocation.h'
$BindingInvocationSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptBindingInvocation.cpp'
$Utf8ValueHeapHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptUtf8ValueHeap.h'
$Utf8ValueHeapSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptUtf8ValueHeap.cpp'
$ValueCapabilityHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptValueCapability.h'
$ValueCapabilitySource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptValueCapability.cpp'
$BindingCodecProgramHeader = Read-RequiredFile 'Source/AvidScriptBindings/Private/Invocation/AvidScriptBindingCodecProgram.h'
$BindingCodecProgramSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/Invocation/AvidScriptBindingCodecProgram.cpp'
$BindingPreparedInvocationSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/Invocation/AvidScriptBindingPreparedInvocation.cpp'
$BindingCodecProgram = Read-RequiredFile 'Source/AvidScriptBindings/Private/Invocation/AvidScriptBindingCodecProgram.cpp'
$BindingPreparedInvocation = Read-RequiredFile 'Source/AvidScriptBindings/Private/Invocation/AvidScriptBindingPreparedInvocation.cpp'
foreach ($RequiredPreparedDynamicContract in @(
    'FAvidScriptVmPreparedDynamicHostTarget',
    'FAvidScriptVmDynamicImport',
    'PreparedTarget')) {
    if (-not $VmContractHeader.Contains($RequiredPreparedDynamicContract)) {
        Add-Violation "VM prepared dynamic contract is missing $RequiredPreparedDynamicContract"
    }
}
foreach ($RequiredPreparedBindingContract in @(
    'FAvidScriptPreparedDynamicBinding',
    'BuildPreparedDynamicBindings')) {
    if (-not $BindingInvocationHeader.Contains($RequiredPreparedBindingContract) -and
        -not $BindingInvocationSource.Contains($RequiredPreparedBindingContract)) {
        Add-Violation "Bindings prepared dynamic contract is missing $RequiredPreparedBindingContract"
    }
}
foreach ($RequiredCodecToken in @(
    'SetValueFromCells',
    'SetValueFromGuest',
    'WriteValueToGuest')) {
    if (-not $BindingCodecProgram.Contains($RequiredCodecToken)) {
        Add-Violation "Bindings codec program is missing $RequiredCodecToken"
    }
}
foreach ($RequiredPreparedExecutorToken in @(
    'InvokePreparedDynamicReflection',
    'ProcessEvent',
    'DispatchFastPath')) {
    if (-not $BindingPreparedInvocation.Contains($RequiredPreparedExecutorToken)) {
        Add-Violation "Bindings prepared executor is missing $RequiredPreparedExecutorToken"
    }
}
$PreparedDynamicBuildSlice = Get-SourceSlice `
    $RuntimeSource `
    'bool FAvidScriptWasmRuntimeInstance::BuildPreparedDynamicHostImports(' `
    'bool FAvidScriptWasmRuntimeInstance::ReadStateBytes('
Test-RequiredTokenSequence $PreparedDynamicBuildSlice @(
    'PreparedVmBindingPackage = BindingPackage->GetVmPackage()',
    'BuildPreparedDynamicBindings',
    'Import.Ordinal != Binding.BindingOrdinal',
    'Import.StableId != Binding.StableId',
    'Import.ModuleName != Binding.ModuleName',
    'Import.ImportName != Binding.ImportName',
    'Import.Signature != Binding.Signature',
    'Import.PreparedTarget.Context = Call.Get()',
    'Import.PreparedTarget.Invoke =') `
    'Runtime must bind prepared dynamic targets by immutable import identity'
if ($PreparedDynamicBuildSlice -match 'Import(Name|StableId)\s*==\s*TEXT\(') {
    Add-Violation 'Runtime prepared dynamic binding must not select targets by API name'
}
Test-RequiredTokenSequence $RuntimeWasmTests @(
    'PreparedTarget.Invoke(',
    'Instrumentation.PreparedDynamicHitCount',
    'Instrumentation.PreparedDynamicFallbackCount',
    'Instrumentation.PreparedDynamicRejectCount') `
    'Runtime prepared dynamic Automation must execute the target and prove hit fallback reject routing'
foreach ($RequiredRuntimeArtifactContract in @(
    'FAvidScriptRuntimeArtifact',
    'FAvidScriptRuntimeArtifactLoader',
    'FAvidScriptVmOwnedArtifact',
    'FAvidScriptVmBackendSelection')) {
    if (-not $RuntimeArtifactHeader.Contains($RequiredRuntimeArtifactContract)) {
        Add-Violation "Runtime artifact contract is missing $RequiredRuntimeArtifactContract"
    }
}
Test-RequiredTokenSequence $RuntimeArtifactSource @(
    'FAvidScriptWasmReloadManifestLoader::LoadFromFile',
    'RootObject->HasField(TEXT("execution"))',
    'AuthorizeAvidScriptVmArtifact') `
    'Runtime artifact loader must validate canonical WASM before authorizing serialized execution bytes'
Test-RequiredTokenSequence $RuntimeSource @(
    'Artifact.ArtifactFormat ==',
    'AuthorizeAvidScriptVmArtifact',
    'return LoadArtifactView(',
    'Artifact.MakeView(',
    'EAvidScriptVmArtifactTrust::VerifiedPackage',
    'EAvidScriptVmArtifactTrust::Untrusted') `
    'Runtime instance must reauthorize an owned serialized artifact before promoting its execution view to verified trust'
foreach ($RequiredRuntimeArtifactLoadContract in @(
    'LoadArtifactView',
    'VmBackend->LoadArtifact',
    'AuthorizeAvidScriptVmArtifact',
    'EAvidScriptVmArtifactTrust::Untrusted',
    'Artifact.VmArtifact.CanonicalWasmBytes',
    'Artifact.BackendSelection')) {
    if (-not $RuntimeSource.Contains($RequiredRuntimeArtifactLoadContract) -and
        -not $RuntimeSessionSource.Contains($RequiredRuntimeArtifactLoadContract)) {
        Add-Violation "Runtime artifact lifecycle is missing $RequiredRuntimeArtifactLoadContract"
    }
}
foreach ($RequiredRuntimeSessionToken in @(
    'SetBackendSelectionForTesting',
    'BackendSelection.BackendKind = EAvidScriptVmBackendKind::Wamr',
    'BackendSelection.ExecutionMode = EAvidScriptVmExecutionMode::Auto',
    'BackendSelection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode',
    'BackendSelection.bAllowFallback = true')) {
    if (-not $RuntimeSessionHeader.Contains($RequiredRuntimeSessionToken) -and
        -not $RuntimeSessionSource.Contains($RequiredRuntimeSessionToken)) {
        Add-Violation "Runtime session backend-selection contract is missing $RequiredRuntimeSessionToken"
    }
}
foreach ($RequiredRuntimeLaneToken in @(
    'EAvidScriptVmBackendKind::Wasmtime',
    'EAvidScriptVmExecutionMode::Jit',
    'EAvidScriptVmArtifactFormat::WasmBytecode',
    'Wasmtime.Selection.bAllowFallback = false')) {
    if (-not $RuntimeBackendLaneHeader.Contains($RequiredRuntimeLaneToken)) {
        Add-Violation "Runtime parity lane contract is missing $RequiredRuntimeLaneToken"
    }
}
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
$VmImportPolicySource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptVmImportPolicy.cpp'
$WamrBackendSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWamrBackend.cpp'
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
    -not $RuntimeReloadSource.Contains('wasm_layout_invalid') -or
    -not $RuntimeReloadSource.Contains('debug_map_wasm_layout_mismatch') -or
    -not $RuntimeReloadSource.Contains('LoadManifestDebugMap(*RootObject, ManifestFullPath, WasmLayout') -or
    -not $RuntimeReloadSource.Contains('DebugImportedFunctionCount') -or
    -not $RuntimeReloadSource.Contains('DebugDefinedFunctionCount')) {
    Add-Violation 'Runtime reload must resolve and validate the immutable debug map before candidate activation'
}
if (-not $RuntimeSource.Contains('DebugMap->MapFrames')) {
    Add-Violation 'Runtime must map VM trap frames through the validated debug map'
}

$ComponentHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptComponent.h'
$ComponentSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptComponent.cpp'
foreach ($RequiredComponentArtifactContract in @(
    'FAvidScriptRuntimeArtifactLoader::LoadFromFile',
    'RuntimeSession->LoadInitialArtifact',
    'RuntimeSession->ReloadArtifact')) {
    if (-not $ComponentSource.Contains($RequiredComponentArtifactContract)) {
        Add-Violation "UAvidScriptComponent artifact lifecycle is missing $RequiredComponentArtifactContract"
    }
}
if ($ComponentSource.Contains('FAvidScriptWasmReloadManifestLoader::LoadFromFile')) {
    Add-Violation 'UAvidScriptComponent must not bypass RuntimeArtifactLoader with the bytecode-only manifest loader'
}
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
$RuntimeSessionHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptRuntimeSession.h'
$WasmRuntimeHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptWasmRuntime.h'
$WasmRuntimeHotSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp'
foreach ($RequiredHotLifecycleContract in @(
    'TickHot',
    'DispatchEventHot',
    'DispatchGameplayEventHot',
    'CaptureLiveSnapshot',
    'GetLiveHotSnapshot'
)) {
    if (-not $RuntimeSessionHeader.Contains($RequiredHotLifecycleContract) -or
        -not $RuntimeSessionSource.Contains($RequiredHotLifecycleContract)) {
        Add-Violation "FAvidScriptRuntimeSession hot lifecycle contract is missing $RequiredHotLifecycleContract"
    }
}
if (-not $WasmRuntimeHeader.Contains('HotFailureOnly') -or
    -not $WasmRuntimeHotSource.Contains('InvokeVmExport(') -or
    -not $WasmRuntimeHotSource.Contains('if (!bHotFailureOnly)')) {
    Add-Violation 'WASM Runtime hot lifecycle path must avoid success result materialization'
}
if (-not $ComponentSource.Contains('RuntimeSession->TickHot(') -or
    -not $ComponentSource.Contains('RuntimeSession->GetLiveHotSnapshot()')) {
    Add-Violation 'UAvidScriptComponent Tick must use the hot lifecycle result and POD snapshot paths'
}

$BindingSelectionTypes = Read-RequiredFile 'Source/AvidScriptEditor/Public/AvidScriptEditorBindingSelectionTypes.h'
$BindingSelectionResolverHeader = Read-RequiredFile 'Source/AvidScriptEditor/Public/AvidScriptEditorBindingSelectionResolver.h'
$BindingSelectionResolverSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingSelectionResolver.cpp'
$PropertySelectionResolverHeader = Read-RequiredFile 'Source/AvidScriptEditor/Public/AvidScriptEditorBindingPropertySelectionResolver.h'
$PropertySelectionResolverSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingPropertySelectionResolver.cpp'
$ReflectedFunctionPolicySource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorReflectedFunctionPolicy.cpp'
$ReflectedPropertyPolicySource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorReflectedPropertyPolicy.cpp'
$ReflectedTypePolicyHeader = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorReflectedTypePolicy.h'
$ReflectedTypePolicySource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorReflectedTypePolicy.cpp'
$BindingDescriptorGeneratorSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingDescriptorGenerator.cpp'
$BindingDescriptorModelSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingDescriptorModel.cpp'
$EditorBindingDescriptorIdentityHeader = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingDescriptorIdentity.h'
$EditorBindingDescriptorIdentitySource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingDescriptorIdentity.cpp'
$BindingDescriptorHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptBindingDescriptor.h'
$BindingDescriptorSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptBindingDescriptor.cpp'
$BindingLatentHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptBindingLatent.h'
$BindingNetworkHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptBindingNetworkPolicy.h'
$BindingNetworkSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptBindingNetworkPolicy.cpp'
$NetworkRpcPlan = Read-RequiredFile 'Docs/Phase57/P57.12D1_Generated_RPC_Authority_Plan.md'
$ReplicatedPropertyPlan = Read-RequiredFile 'Docs/Phase57/P57.12D2_Replicated_Property_Plan.md'
$InboundHandlerPlan = Read-RequiredFile 'Docs/Phase57/P57.12D3_Inbound_Network_Handler_Plan.md'
$ChainedInboundHandlerPlan = Read-RequiredFile 'Docs/Phase57/P57.12D4_Chained_Blueprint_Inbound_Handler_Plan.md'
$NetworkTopologyPlan = Read-RequiredFile 'Docs/Phase57/P57.12D5_Real_Network_Topology_Plan.md'
$NetworkTopologyRunner = Read-RequiredFile 'Build/RunAvidScriptNetworkTopology.ps1'
$NetworkTopologyHarnessSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Tests/AvidScriptNetworkTopologyHarness.cpp'
$NetworkTopologyFixtureHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Tests/AvidScriptNetworkTopologyTestTypes.h'
$NetworkTopologyFixtureSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Tests/AvidScriptNetworkTopologyTestTypes.cpp'
$NetworkTopologyProfile = Read-RequiredFile 'Samples/CSharp/NetworkTopology/NetworkTopology.csharp-profile.json'
$NetworkTopologySample = Read-RequiredFile 'Samples/CSharp/NetworkTopology/NetworkTopologyScript.cs'
$NetworkTopologyEditorTest = Read-RequiredFile 'Source/AvidScriptEditor/Private/Tests/AvidScriptEditorNetworkTopologyTests.cpp'
$DelegateEventSelectionResolverSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorBindingDelegateEventSelectionResolver.cpp'
$FunctionHookRegistryHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Network/AvidScriptFunctionHookRegistry.h'
$FunctionHookRegistrySource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Network/AvidScriptFunctionHookRegistry.cpp'
$InboundHandlerSessionHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Session/AvidScriptSessionInboundHandlers.h'
$InboundHandlerSessionSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Session/AvidScriptSessionInboundHandlers.cpp'
$InboundHandlerWasmRuntimeSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp'
$InboundHandlerSampleProfile = Read-RequiredFile 'Samples/CSharp/InboundNetworkHandlers/InboundNetworkHandlers.csharp-profile.json'
$InboundHandlerSampleSource = Read-RequiredFile 'Samples/CSharp/InboundNetworkHandlers/InboundNetworkHandlersScript.cs'
$ObjectFactoryPolicyHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptObjectFactoryPolicy.h'
$ObjectLifecycleBindingHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptObjectLifecycleBinding.h'
$ObjectLifecycleBindingSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptObjectLifecycleBinding.cpp'
$BindingInvocationHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptBindingInvocation.h'
$BindingInvocationSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptBindingInvocation.cpp'
$BindingPreparedInvocationSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/Invocation/AvidScriptBindingPreparedInvocation.cpp'
$ObjectTypeBindingSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptObjectTypeBinding.cpp'
$ObjectRegistryHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptObjectRegistry.h'
$ObjectRegistrySource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptObjectRegistry.cpp'
$RuntimeComponentSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptComponent.cpp'
foreach ($RequiredSelectionContract in @(
    'FAvidScriptBindingSelectionProfile',
    'FAvidScriptBindingSelectionIssue',
    'FAvidScriptBindingSelectionResolveResult',
    'FAvidScriptReflectedPropertySelection',
    'IncludeProperties',
    'ExcludeProperties',
    'ExplicitProperties',
    'bWritable',
    'WritableProperties',
    'CandidatePropertyCount',
    'AcceptedPropertyCount',
    'RejectedPropertyCount',
    'CandidateWritablePropertyCount',
    'AcceptedWritablePropertyCount',
    'RejectedWritablePropertyCount'
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
foreach ($RequiredLatentBindingContract in @(
    'FAvidScriptEditorLatentFunctionContract',
    'FLatentActionInfo::StaticStruct()',
    'latent_completion_shape_unsupported',
    'ProjectLatentFunction',
    'LatentInfoParameter',
    'WorldContextParameter',
    'LatentGeneratorVersion',
    'TEXT("latent_process_event")',
    'EAvidScriptBindingReloadEffect::ContinuationProducer',
    'Package.SchemaVersion = 12'
)) {
    if (-not $ReflectedFunctionPolicySource.Contains($RequiredLatentBindingContract) -and
        -not $BindingSelectionResolverSource.Contains($RequiredLatentBindingContract) -and
        -not $ReflectedTypePolicyHeader.Contains($RequiredLatentBindingContract) -and
        -not $ReflectedTypePolicySource.Contains($RequiredLatentBindingContract) -and
        -not $BindingDescriptorHeader.Contains($RequiredLatentBindingContract) -and
        -not $BindingDescriptorGeneratorSource.Contains($RequiredLatentBindingContract)) {
        Add-Violation "schema v12 reflected latent generation is missing $RequiredLatentBindingContract"
    }
}
foreach ($RequiredLatentRuntimeContract in @(
    'IAvidScriptBindingLatentHost',
    'FAvidScriptBindingLatentReservation',
    'ResolveAvidScriptRuntimeLatentContract',
    'CallbackIdArgumentOffset',
    'BeginLatent(',
    'CommitLatent(',
    'AbortLatent(',
    'ReturnValueI64 = Reservation.Token'
)) {
    if (-not $BindingLatentHeader.Contains($RequiredLatentRuntimeContract) -and
        -not $BindingInvocationSource.Contains($RequiredLatentRuntimeContract) -and
        -not $BindingPreparedInvocationSource.Contains($RequiredLatentRuntimeContract)) {
        Add-Violation "generic latent invocation transaction is missing $RequiredLatentRuntimeContract"
    }
}
foreach ($RequiredNetworkBindingContract in @(
    'EAvidScriptBindingNetworkMode',
    'FAvidScriptBindingNetworkContract',
    'TryResolveAvidScriptBindingNetworkContract',
    'IsAvidScriptBindingNetworkOwnerClass',
    'NetworkGeneratorVersion',
    'Package.SchemaVersion = 15',
    'TEXT("network_mode")',
    'TEXT("network_reliable")',
    'TEXT("descriptor_selection_v15")',
    'TEXT("descriptor_package_v15")',
    'binding_network_contract_mismatch',
    'PreflightNetworkInvocation',
    'GetFunctionCallspace',
    'binding_network_call_absorbed',
    'binding_network_authority_denied',
    'binding_network_reload_effect_unsupported'
)) {
    if (-not $BindingNetworkHeader.Contains($RequiredNetworkBindingContract) -and
        -not $BindingNetworkSource.Contains($RequiredNetworkBindingContract) -and
        -not $ReflectedFunctionPolicySource.Contains($RequiredNetworkBindingContract) -and
        -not $BindingDescriptorGeneratorSource.Contains($RequiredNetworkBindingContract) -and
        -not $BindingDescriptorModelSource.Contains($RequiredNetworkBindingContract) -and
        -not $BindingDescriptorSource.Contains($RequiredNetworkBindingContract) -and
        -not $BindingInvocationSource.Contains($RequiredNetworkBindingContract) -and
        -not $BindingPreparedInvocationSource.Contains($RequiredNetworkBindingContract)) {
        Add-Violation "schema v15 generic RPC authority contract is missing $RequiredNetworkBindingContract"
    }
}
foreach ($RequiredNetworkPlanContract in @(
    'P57.12D2',
    'P57.12D3',
    'GetFunctionCallspace',
    '不为每个 RPC 手写宿主 API'
)) {
    if (-not $NetworkRpcPlan.Contains($RequiredNetworkPlanContract)) {
        Add-Violation "P57.12D1 network boundary plan is missing $RequiredNetworkPlanContract"
    }
}
foreach ($RequiredReplicatedPropertyContract in @(
    'FAvidScriptBindingPropertyReplicationContract',
    'TryResolveAvidScriptBindingPropertyReplicationContract',
    'ReplicatedPropertyGeneratorVersion',
    'Package.SchemaVersion = 16',
    'TEXT("property_replication")',
    'TEXT("rep_notify")',
    'TEXT("descriptor_selection_v16")',
    'TEXT("descriptor_package_v16")',
    'binding_property_replication_contract_mismatch',
    'PreflightReplicatedPropertyWrite',
    'binding_property_replication_authority_denied',
    'binding_property_replication_reload_effect_unsupported',
    'MarkPropertyDirtyFromRepIndex'
)) {
    if (-not $BindingNetworkHeader.Contains($RequiredReplicatedPropertyContract) -and
        -not $BindingNetworkSource.Contains($RequiredReplicatedPropertyContract) -and
        -not $ReflectedPropertyPolicySource.Contains($RequiredReplicatedPropertyContract) -and
        -not $BindingDescriptorGeneratorSource.Contains($RequiredReplicatedPropertyContract) -and
        -not $BindingDescriptorModelSource.Contains($RequiredReplicatedPropertyContract) -and
        -not $BindingDescriptorSource.Contains($RequiredReplicatedPropertyContract) -and
        -not $BindingInvocationSource.Contains($RequiredReplicatedPropertyContract) -and
        -not $BindingPreparedInvocationSource.Contains($RequiredReplicatedPropertyContract)) {
        Add-Violation "schema v16 replicated property contract is missing $RequiredReplicatedPropertyContract"
    }
}
foreach ($RequiredReplicatedPropertyPlanContract in @(
    'Push Model',
    '不把 UE 入站 RepNotify 自动分发到 C# export',
    '不把轮询结果冒充 UE RepNotify',
    'GetLifetimeReplicatedProps'
)) {
    if (-not $ReplicatedPropertyPlan.Contains($RequiredReplicatedPropertyPlanContract)) {
        Add-Violation "P57.12D2 replicated property plan is missing $RequiredReplicatedPropertyPlanContract"
    }
}
foreach ($RequiredInboundHandlerContract in @(
    'IncludeHandlers',
    'InboundHandlerGeneratorVersion',
	'OutPackage.SchemaVersion != 17',
    'TEXT("network_rpc")',
    'TEXT("rep_notify")',
    'TEXT("descriptor_selection_v17")',
    'TEXT("descriptor_package_v17")',
    'BuildPreparedInboundHandlers',
    'BuildPreparedCallbacks'
)) {
    if (-not $DelegateEventSelectionResolverSource.Contains($RequiredInboundHandlerContract) -and
        -not $BindingDescriptorGeneratorSource.Contains($RequiredInboundHandlerContract) -and
        -not $BindingDescriptorSource.Contains($RequiredInboundHandlerContract) -and
        -not $BindingInvocationHeader.Contains($RequiredInboundHandlerContract) -and
        -not $BindingInvocationSource.Contains($RequiredInboundHandlerContract) -and
        -not $InboundHandlerWasmRuntimeSource.Contains($RequiredInboundHandlerContract)) {
        Add-Violation "schema v17 inbound handler contract is missing $RequiredInboundHandlerContract"
    }
}
foreach ($RequiredFunctionHookContract in @(
    'FAvidScriptFunctionHookRegistry',
    'ValidateReplacement',
    'ReplaceRoutes',
    'RemoveRoutes',
    'GetNativeFunc',
    'SetNativeFunc',
    'CurrentNativeFunction',
    'Hook->Original(Context, Stack, RESULT_PARAM)',
    'FAvidScriptSessionInboundHandlers',
    'ValidatePreparedCommit',
    'DispatchPreparedDelegateEvent'
)) {
    if (-not $FunctionHookRegistryHeader.Contains($RequiredFunctionHookContract) -and
        -not $FunctionHookRegistrySource.Contains($RequiredFunctionHookContract) -and
        -not $InboundHandlerSessionSource.Contains($RequiredFunctionHookContract) -and
        -not $RuntimeSessionSource.Contains($RequiredFunctionHookContract)) {
        Add-Violation "P57.12D3 function hook lifecycle is missing $RequiredFunctionHookContract"
    }
}
foreach ($RequiredInboundHandlerPlanContract in @(
    'replace',
	'native thunk',
    'Prepare',
	'commit',
    '重入'
)) {
    if (-not $InboundHandlerPlan.Contains($RequiredInboundHandlerPlanContract)) {
        Add-Violation "P57.12D3 inbound handler plan is missing $RequiredInboundHandlerPlanContract"
    }
}
foreach ($RequiredInboundHandlerSampleContract in @(
    '"schema_version": 10',
    '"before_handlers"',
    '"after_handlers"',
    'ServerSubmitValue',
    'OnRep_ReplicatedScore',
    '[AvidEvent(AvidEvents.ServerSubmitValue)]',
    '[AvidEvent(AvidEvents.OnRep_ReplicatedScore)]'
)) {
    if (-not $InboundHandlerSampleProfile.Contains($RequiredInboundHandlerSampleContract) -and
        -not $InboundHandlerSampleSource.Contains($RequiredInboundHandlerSampleContract)) {
        Add-Violation "P57.12D4 C# sample is missing $RequiredInboundHandlerSampleContract"
    }
}
if (-not $ReflectedPropertyPolicySource.Contains('FAvidScriptEditorReflectedPropertyPolicy::EvaluateReadable') -or
    -not $ReflectedPropertyPolicySource.Contains('FAvidScriptEditorReflectedPropertyPolicy::EvaluateWritable') -or
    -not $ReflectedPropertyPolicySource.Contains('cached_property_set') -or
    -not $ReflectedPropertyPolicySource.Contains('cached_blueprint_setter') -or
    -not $ReflectedTypePolicyHeader.Contains('ProjectReadableProperty')) {
    Add-Violation 'reflected property eligibility and write routing must remain in the shared reflected property policy'
}
foreach ($RequiredStructWireDescriptorContract in @(
    'FAvidScriptBindingStructFieldModel',
	'FAvidScriptBindingDescriptorLayout',
    'StructFields',
    'ValidateAvidScriptBindingStructWireGraph',
	'IsAvidScriptBindingPowerOfTwo',
	'IsCanonicalAvidScriptBindingStructWireLeaf',
	'ExpectedWireOffset',
	'SchemaVersion >= 9 && OutType.Kind == TEXT("struct_wire")',
    'Depth > 8',
    'InOutNodes > 128',
    'Type.Size > 4096',
	'TEXT("struct_wire_type_size")',
	'TEXT("struct_wire_type_alignment")',
    'TEXT("descriptor_selection_v9")',
    'TEXT("descriptor_package_v9")'
)) {
    if (-not $BindingDescriptorHeader.Contains($RequiredStructWireDescriptorContract) -and
        -not $BindingDescriptorSource.Contains($RequiredStructWireDescriptorContract)) {
        Add-Violation "schema v9 recursive struct descriptor is missing $RequiredStructWireDescriptorContract"
    }
}
foreach ($RequiredStructWireIdentityContract in @(
	'WireSize',
	'WireAlignment'
)) {
	if (-not $BindingDescriptorHeader.Contains($RequiredStructWireIdentityContract) -or
		-not $EditorBindingDescriptorIdentityHeader.Contains($RequiredStructWireIdentityContract) -or
		-not $EditorBindingDescriptorIdentitySource.Contains($RequiredStructWireIdentityContract)) {
		Add-Violation "schema v9 struct-wire identity must bind and forward $RequiredStructWireIdentityContract"
	}
}
foreach ($RequiredStructWireProjectionContract in @(
    'TEXT("struct_wire:")',
    'StructDepth >= 8',
    '++InOutStructNodes > 128',
    'FieldValue.Type.Size > 4096 - WireOffset',
    'Package.SchemaVersion = 9',
    'TypeModel.StructFields = Type.StructFields'
)) {
    if (-not $ReflectedTypePolicySource.Contains($RequiredStructWireProjectionContract) -and
        -not $BindingDescriptorGeneratorSource.Contains($RequiredStructWireProjectionContract)) {
        Add-Violation "schema v9 recursive struct projection is missing $RequiredStructWireProjectionContract"
    }
}
if (-not $BindingDescriptorModelSource.Contains('FAvidScriptBindingDescriptorLayout::ValidateTypeGraph') -or
	-not $BindingDescriptorModelSource.Contains('Writer->WriteArrayStart(TEXT("fields"))') -or
    -not $BindingDescriptorModelSource.Contains('Writer->WriteValue(TEXT("wire_offset"), Field.WireOffset)')) {
    Add-Violation 'schema v9 serializer must validate and publish the immutable recursive struct field graph'
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
$CSharpProfileServiceHeader = Read-RequiredFile 'Source/AvidScriptEditor/Public/AvidScriptEditorCSharpProfileService.h'
$CSharpProfileServiceSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/AvidScriptEditorCSharpProfileService.cpp'
$ProjectBindingProfileHeader = Read-RequiredFile 'Source/AvidScriptEditor/Public/AvidScriptEditorProjectBindingProfile.h'
$ProjectBindingProfileSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/BindingGeneration/AvidScriptEditorProjectBindingProfile.cpp'
$CSharpBuildInvokerSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBuildInvoker.cpp'
$CSharpBuildPipelineSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBuildPipeline.cpp'
$CSharpBindingSliceSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBindingSliceService.cpp'
$CSharpOperationLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpOperationLowerer.cs'
$CSharpCallOperationLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpCallOperationLowerer.cs'
$BindingRuntimeIntegrationTestsSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/Tests/AvidScriptEditorBindingRuntimeIntegrationTests.cpp'
$BidirectionalPropertiesSampleSource = Read-RequiredFile 'Samples/CSharp/BidirectionalProperties/BidirectionalProperties.cs'
$LatentGameplaySampleSource = Read-RequiredFile 'Samples/CSharp/LatentGameplay/LatentGameplayScript.cs'
$LatentGameplayProfile = Read-RequiredFile 'Samples/CSharp/LatentGameplay/LatentGameplay.csharp-profile.json'
$NetworkRpcSampleSource = Read-RequiredFile 'Samples/CSharp/NetworkRpc/NetworkRpcScript.cs'
$NetworkRpcProfile = Read-RequiredFile 'Samples/CSharp/NetworkRpc/NetworkRpc.csharp-profile.json'
$NetworkRpcSampleReadme = Read-RequiredFile 'Samples/CSharp/NetworkRpc/README.md'
$ReplicatedPropertySampleSource = Read-RequiredFile 'Samples/CSharp/ReplicatedProperty/ReplicatedPropertyScript.cs'
$ReplicatedPropertyProfile = Read-RequiredFile 'Samples/CSharp/ReplicatedProperty/ReplicatedProperty.csharp-profile.json'
$ReplicatedPropertySampleReadme = Read-RequiredFile 'Samples/CSharp/ReplicatedProperty/README.md'
$LatentCancellationPayloadDesign = Read-RequiredFile 'Docs/Phase57/P57.12C6D_Cancellation_And_Payload_Design.md'
$CSharpAsyncBuildBackendSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildBackend.cpp'
$CSharpAsyncBuildJobSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildJob.cpp'
$CSharpLiveReloadServiceSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpLiveReload/AvidScriptEditorCSharpLiveReloadService.cpp'
$CSharpLiveReloadBuildStateSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpLiveReload/AvidScriptEditorCSharpLiveReloadServiceBuildState.cpp'
$CSharpLiveReloadCompletionSource = Read-RequiredFile 'Source/AvidScriptEditor/Private/CSharpLiveReload/AvidScriptEditorCSharpLiveReloadCompletion.cpp'
$CSharpPreparedSemanticSource = Read-RequiredFile 'Build/AvidScriptCSharpPreparedSemantic.ps1'
$CSharpBindingPackageSource = Read-RequiredFile 'Build/AvidScriptCSharpBindingPackage.ps1'
$CSharpSemanticCacheSource = Read-RequiredFile 'Build/AvidScriptCSharpSemanticCache.ps1'
foreach ($RequiredChainedHandlerProfileContract in @(
    'BeforeHandlers',
    'AfterHandlers',
    'HandlerMode',
    'handler_mode_conflict'
)) {
    if (-not $BindingSelectionTypes.Contains($RequiredChainedHandlerProfileContract) -and
        -not $CSharpProfileServiceSource.Contains($RequiredChainedHandlerProfileContract) -and
        -not $ProjectBindingProfileSource.Contains($RequiredChainedHandlerProfileContract) -and
        -not $DelegateEventSelectionResolverSource.Contains($RequiredChainedHandlerProfileContract) -and
		-not $CSharpBindingEmitterSource.Contains($RequiredChainedHandlerProfileContract) -and
        -not $CSharpBindingSliceSource.Contains($RequiredChainedHandlerProfileContract)) {
        Add-Violation "P57.12D4 chained handler profile contract is missing $RequiredChainedHandlerProfileContract"
    }
}
foreach ($RequiredChainedHandlerDescriptorContract in @(
    'Package.SchemaVersion = 18',
    'TEXT("handler_mode")',
    'TEXT("descriptor_selection_v18")',
    'TEXT("descriptor_package_v18")',
    'TEXT("event_handler_mode")'
)) {
    if (-not $BindingDescriptorGeneratorSource.Contains($RequiredChainedHandlerDescriptorContract) -and
        -not $BindingDescriptorModelSource.Contains($RequiredChainedHandlerDescriptorContract) -and
        -not $BindingDescriptorSource.Contains($RequiredChainedHandlerDescriptorContract) -and
        -not $BindingInvocationHeader.Contains($RequiredChainedHandlerDescriptorContract) -and
        -not $BindingInvocationSource.Contains($RequiredChainedHandlerDescriptorContract)) {
        Add-Violation "schema v18 chained handler descriptor contract is missing $RequiredChainedHandlerDescriptorContract"
    }
}
foreach ($RequiredChainedHandlerRuntimeContract in @(
    'EAvidScriptFunctionHookChainMode',
    'EAvidScriptInboundFunctionDispatch',
    'InvokeOriginal',
    'FStructOnScope',
    'MaxDeferredInboundHandlers',
    'CopyCompleteValue_InContainer',
    'PumpDeferred',
    'AddReferencedObjects'
)) {
    if (-not $FunctionHookRegistryHeader.Contains($RequiredChainedHandlerRuntimeContract) -and
        -not $FunctionHookRegistrySource.Contains($RequiredChainedHandlerRuntimeContract) -and
        -not $InboundHandlerSessionHeader.Contains($RequiredChainedHandlerRuntimeContract) -and
        -not $InboundHandlerSessionSource.Contains($RequiredChainedHandlerRuntimeContract) -and
        -not $RuntimeSessionSource.Contains($RequiredChainedHandlerRuntimeContract)) {
        Add-Violation "P57.12D4 chained/reentrant handler runtime is missing $RequiredChainedHandlerRuntimeContract"
    }
}
foreach ($RequiredChainedHandlerPlanContract in @(
    'before',
    'after',
    'replace',
    'FStructOnScope',
    '64',
    'ProcessInternal',
    'Session tick'
)) {
    if (-not $ChainedInboundHandlerPlan.Contains($RequiredChainedHandlerPlanContract)) {
        Add-Violation "P57.12D4 chained inbound handler plan is missing $RequiredChainedHandlerPlanContract"
    }
}
foreach ($RequiredNetworkTopologyHarnessContract in @(
    'WITH_DEV_AUTOMATION_TESTS',
    'AvidScriptNetworkTopologyRole=',
    'OnGameModePostLoginEvent',
    'AAvidScriptNetworkTopologyTestActor',
    'network_topology_timeout',
    'all_remote_clients_confirmed',
    'rep_notify_and_ack_dispatched'
)) {
    if (-not $NetworkTopologyHarnessSource.Contains($RequiredNetworkTopologyHarnessContract)) {
        Add-Violation "P57.12D5 network topology harness is missing $RequiredNetworkTopologyHarnessContract"
    }
}
foreach ($RequiredNetworkTopologyRunnerContract in @(
    "ValidateSet('All', 'Dedicated', 'Listen')",
    'Get-FreeTcpPort',
    'Start-Process',
    '-WindowStyle Hidden',
    'Stop-Process -Id',
    'GameNetDriver.*listening',
    "-Name 'dedicated'",
    "-Name 'listen'",
    'Assert-RoleResult',
    'aggregate.json'
)) {
    if (-not $NetworkTopologyRunner.Contains($RequiredNetworkTopologyRunnerContract)) {
        Add-Violation "P57.12D5 topology runner is missing $RequiredNetworkTopologyRunnerContract"
    }
}
foreach ($RequiredNetworkTopologyFixtureContract in @(
    'ServerSubmitValue',
    'ServerConfirmRepNotify',
    'OnRep_ReplicatedScore',
    'RecordScriptServerHandler',
    'RecordScriptRepNotify',
    'ReplicatedUsing',
    'bOnlyRelevantToOwner',
    'DOREPLIFETIME'
)) {
    if (-not $NetworkTopologyFixtureHeader.Contains($RequiredNetworkTopologyFixtureContract) -and
        -not $NetworkTopologyFixtureSource.Contains($RequiredNetworkTopologyFixtureContract)) {
        Add-Violation "P57.12D5 replicated fixture is missing $RequiredNetworkTopologyFixtureContract"
    }
}
foreach ($RequiredNetworkTopologySampleContract in @(
    'AvidContinuation',
    'ServerSubmitValue',
    'AvidEvents.ServerSubmitValue',
    'AvidEvents.OnRep_ReplicatedScore',
    'ServerConfirmRepNotify',
    '"before_handlers"',
    '"after_handlers"',
    '"writable_properties"'
)) {
    if (-not $NetworkTopologySample.Contains($RequiredNetworkTopologySampleContract) -and
        -not $NetworkTopologyProfile.Contains($RequiredNetworkTopologySampleContract)) {
        Add-Violation "P57.12D5 generated C# network sample is missing $RequiredNetworkTopologySampleContract"
    }
}
foreach ($RequiredNetworkTopologyPlanContract in @(
    'dedicated server + 2 clients',
    'listen server + 1 remote client',
    'UE NetDriver',
    'Push Model',
    'SHA-256',
    'P57-D06-ControlledLeadership'
)) {
    if (-not $NetworkTopologyPlan.Contains($RequiredNetworkTopologyPlanContract)) {
        Add-Violation "P57.12D5 topology plan is missing $RequiredNetworkTopologyPlanContract"
    }
}
foreach ($RequiredNetworkTopologyBuildContract in @(
    'AvidScript.Editor.NetworkTopology.BuildProfile',
    'BuildProfile',
    'NetworkTopology.csharp-profile.json'
)) {
    if (-not $NetworkTopologyEditorTest.Contains($RequiredNetworkTopologyBuildContract)) {
        Add-Violation "P57.12D5 production profile test is missing $RequiredNetworkTopologyBuildContract"
    }
}
foreach ($RequiredEmitterSchemaContract in @(
    'Package.SchemaVersion < 6',
    'FAvidScriptBindingPackage::LoadDescriptor',
    'FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical',
    'ValidateCanonicalDescriptor'
)) {
    if (-not $CSharpBindingEmitterSource.Contains($RequiredEmitterSchemaContract)) {
        Add-Violation "C# binding emitter is missing current descriptor schema contract $RequiredEmitterSchemaContract"
    }
}
foreach ($RequiredProjectProfileContract in @(
    'FAvidScriptProjectBindingProfileSpec',
    'FAvidScriptProjectBindingClassSpec',
    'FAvidScriptEditorProjectBindingProfile::Resolve',
    'TObjectIterator<UClass>',
    'Class->GetOutermost()->GetName() != ModulePath',
    'FModuleManifest::TryRead',
    'ModuleManifest.BuildId',
    'FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile',
    'FAvidScriptHash::Sha256HexUtf8',
    'bIncludeWritableProperties',
    'bHasGeneratedNativeProperties',
    'AppendRuleIdentity('
)) {
    if (-not $ProjectBindingProfileHeader.Contains($RequiredProjectProfileContract) -and
        -not $ProjectBindingProfileSource.Contains($RequiredProjectProfileContract)) {
        Add-Violation "project binding profile is missing stable discovery contract $RequiredProjectProfileContract"
    }
}
foreach ($ForbiddenProjectProfileConcern in @(
    'FAvidScriptEditorReflectedFunctionPolicy',
    'FAvidScriptEditorReflectedPropertyPolicy',
    'FBuildVersion',
    'FEngineVersion::Current',
    'ProcessEvent',
    'wasm_runtime_'
)) {
    if ($ProjectBindingProfileHeader.Contains($ForbiddenProjectProfileConcern) -or
        $ProjectBindingProfileSource.Contains($ForbiddenProjectProfileConcern)) {
        Add-Violation "project binding profile must not own member policy, invocation, or VM concern $ForbiddenProjectProfileConcern"
    }
}
foreach ($RequiredCSharpProfileContract in @(
    'SchemaVersion == 1',
    'SchemaVersion == 2',
    'binding_profile',
    'FAvidScriptEditorProjectBindingProfile::Resolve',
    'FAvidScriptEditorCSharpProfileService::MakeBuildRequest',
    'MakeEngineGameplayProfile',
    'Value->Type == EJson::String'
)) {
    if (-not $CSharpProfileServiceSource.Contains($RequiredCSharpProfileContract)) {
        Add-Violation "C# profile service is missing schema v2 project binding contract $RequiredCSharpProfileContract"
    }
}
foreach ($RequiredCSharpProfileResult in @(
    'bUsesEngineGameplayBindingProfile',
    'ResolvedBindingSelection',
    'BindingSelectionValidation',
    'ResolvedClassReferences',
    'BindingSelectionHash'
)) {
    if (-not $CSharpProfileServiceHeader.Contains($RequiredCSharpProfileResult)) {
        Add-Violation "C# profile result is missing project binding output $RequiredCSharpProfileResult"
    }
}
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
    'cached_property_get',
    'property_set',
    'write_policy',
    'cached_property_set',
    'cached_blueprint_setter',
    'MakePropertySetCanonicalIdentity',
    'ue_function'
)) {
    if (-not $BindingDescriptorSource.Contains($RequiredPropertyDescriptorParserContract)) {
        Add-Violation "binding descriptor v4 parser is missing $RequiredPropertyDescriptorParserContract"
    }
}
foreach ($RequiredPropertyDescriptorGeneratorContract in @(
    'GenerateWithReadableProperties',
    'property_get:',
    'cached_property_get',
    'ProjectReadableProperty',
    'MakePropertySetCanonicalIdentity',
    'WritablePropertyGeneratorVersion'
)) {
    if (-not $BindingDescriptorGeneratorSource.Contains($RequiredPropertyDescriptorGeneratorContract)) {
        Add-Violation "binding descriptor v4 generator is missing $RequiredPropertyDescriptorGeneratorContract"
    }
}
foreach ($RequiredPropertyRuntimeContract in @(
    'FindFProperty<FProperty>',
    'Plan.ReflectedProperty',
    'WriteValueToGuest',
    'binding_property_read_failed',
    'EAvidScriptBindingInvocationKind::ReflectedPropertyWrite',
    'cached_property_set',
    'cached_blueprint_setter',
    'PrepareReflectedProperty',
    'SetValueFromCells',
    'BlueprintSetter candidate reload is not reversible',
    'Binding.UeFunction != BlueprintSetterName',
    'binding_property_blueprint_setter_mismatch',
    'binding_property_write_policy_mismatch',
    'binding_property_write_failed'
)) {
    if (-not $BindingInvocationSource.Contains($RequiredPropertyRuntimeContract) -and
        -not $BindingPreparedInvocation.Contains($RequiredPropertyRuntimeContract)) {
        Add-Violation "cached property runtime is missing $RequiredPropertyRuntimeContract"
    }
}
foreach ($RequiredPropertyFacadeContract in @(
    'RenderPropertyGetter',
    'Binding.BindingKind == TEXT("property_get")',
    'BuildPropertySetterInterop',
    'Binding.BindingKind == TEXT("property_set")',
    'GenerateFromProfile',
    'public bool IsNull => Slot == 0 && Generation == 0;',
    'public bool HasHandle => Slot > 0 && Generation > 0;'
)) {
    if (-not $CSharpBindingRendererSource.Contains($RequiredPropertyFacadeContract) -and
        -not $CSharpBindingEmitterSource.Contains($RequiredPropertyFacadeContract) -and
        -not $CSharpBindingSliceSource.Contains($RequiredPropertyFacadeContract)) {
        Add-Violation "C# property facade pipeline is missing $RequiredPropertyFacadeContract"
    }
}
foreach ($RequiredPropertySliceContract in @(
    'Binding.BindingKind == TEXT("property_get")',
    'Binding.BindingKind == TEXT("property_set")'
)) {
    if (-not $CSharpBindingSliceSource.Contains($RequiredPropertySliceContract)) {
        Add-Violation "C# runtime slice is missing property accessor contract $RequiredPropertySliceContract"
    }
}
foreach ($RequiredPropertyLoweringContract in @(
    'LowerPropertySetter',
    'LowerPropertyReceiver',
    'GuestRegister receiver',
    'TryGetPropertySetter',
    'property setter'
)) {
    if (-not $CSharpCallOperationLowererSource.Contains($RequiredPropertyLoweringContract)) {
        Add-Violation "C# Guest lowering is missing property setter contract $RequiredPropertyLoweringContract"
    }
}
foreach ($RequiredPropertyOperationContract in @(
    'GuestRegister? propertyReceiver = null;',
    'LowerPropertyReceiver(',
    'LowerPropertySetter(',
    '? propertyValue',
    ': LowerValue(context, operation.Children[1]'
)) {
    if (-not $CSharpOperationLowererSource.Contains($RequiredPropertyOperationContract)) {
        Add-Violation "C# Guest property evaluation order is missing $RequiredPropertyOperationContract"
    }
}
foreach ($RequiredPropertyGameplayEvidence in @(
    'FORCENOINLINE void SetAvidScriptPropertyBenchmarkNative',
    'NativeChecksum',
    'BidirectionalPropertiesSample',
    'MakeBuildRequest(ProfileResult)',
    'BuildProfile(',
    'ReflectedVectorPropertySet'
)) {
    if (-not $BindingRuntimeIntegrationTestsSource.Contains($RequiredPropertyGameplayEvidence)) {
        Add-Violation "Phase 52 gameplay evidence is missing $RequiredPropertyGameplayEvidence"
    }
}
if (-not $BidirectionalPropertiesSampleSource.Contains('AActor self = UE.Self;') -or
    -not $BidirectionalPropertiesSampleSource.Contains('self.GetActorScale3D()') -or
    -not $BidirectionalPropertiesSampleSource.Contains('self.SetActorScale3D(') -or
    $BidirectionalPropertiesSampleSource.Contains('UE.Self.CustomTimeDilation =')) {
    Add-Violation 'Phase 52 property sample must cache the zero-allocation Self facade before property writes'
}
foreach ($RequiredClassReferenceDescriptorContract in @(
    'FAvidScriptBindingClassReferenceModel',
    'MakeClassReferenceStableId',
    'MakeSelectionHash',
    'MakePackageHash'
)) {
    if (-not $BindingDescriptorHeader.Contains($RequiredClassReferenceDescriptorContract) -and
        -not $BindingDescriptorSource.Contains($RequiredClassReferenceDescriptorContract)) {
        Add-Violation "binding descriptor v5 class table is missing $RequiredClassReferenceDescriptorContract"
    }
}
foreach ($RequiredClassReferenceGeneratorContract in @(
    'GenerateWithClassReferences',
    'Package.ClassReferences.Sort',
    'FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical'
)) {
    if (-not $BindingDescriptorGeneratorSource.Contains($RequiredClassReferenceGeneratorContract)) {
        Add-Violation "binding descriptor v5 generator is missing $RequiredClassReferenceGeneratorContract"
    }
}
if (-not $BindingDescriptorModelSource.Contains('Writer->WriteArrayStart(TEXT("class_references"))')) {
    Add-Violation 'canonical descriptor serializer must own class_references JSON emission'
}
foreach ($RequiredClassReferenceRuntimeContract in @(
    'ClassReferencePlans',
    'TryResolveClassReference',
    'binding_class_cook_missing',
    'CLASS_Abstract | CLASS_NotPlaceable'
)) {
    if (-not $BindingInvocationSource.Contains($RequiredClassReferenceRuntimeContract)) {
        Add-Violation "cached class reference runtime is missing $RequiredClassReferenceRuntimeContract"
    }
}
foreach ($RequiredClassReferenceFacadeContract in @(
    'public readonly struct TSubclassOfAActor',
    'public static class ProjectClasses',
    'class_reference_count',
    'AuthorizationModel.ClassReferences'
)) {
    if (-not $CSharpBindingRendererSource.Contains($RequiredClassReferenceFacadeContract) -and
        -not $CSharpBindingSliceSource.Contains($RequiredClassReferenceFacadeContract)) {
        Add-Violation "C# class reference facade pipeline is missing $RequiredClassReferenceFacadeContract"
    }
}
foreach ($RequiredLifecycleSpecContract in @(
    'EAvidScriptBindingInvocationKind',
    'ObjectSpawnActor',
    'ObjectDestroyActor',
    'ObjectIsA',
    'avid_object_spawn_actor',
    'avid_object_destroy_actor',
    'avid_object_is_a',
    'FAvidScriptHash::Sha256HexUtf8'
)) {
    if (-not $ObjectLifecycleBindingHeader.Contains($RequiredLifecycleSpecContract) -and
        -not $ObjectLifecycleBindingSource.Contains($RequiredLifecycleSpecContract)) {
        Add-Violation "shared object lifecycle binding specification is missing $RequiredLifecycleSpecContract"
    }
}
foreach ($RequiredLifecycleRuntimeContract in @(
    'DispatchAvidScriptObjectLifecycle',
    'Context.World.Get()',
    'binding_world_invalid',
    'binding_cross_world',
    'binding_destroy_owner_unsupported',
    'FAvidScriptObjectLifecycleBindings::GetSpecs()'
)) {
    if (-not $BindingInvocationSource.Contains($RequiredLifecycleRuntimeContract)) {
        Add-Violation "object lifecycle runtime is missing $RequiredLifecycleRuntimeContract"
    }
}
foreach ($RequiredLifecycleFacadeContract in @(
    'public static AActor SpawnActor',
    'public static bool DestroyActor',
    'public static bool IsA',
    'FAvidScriptObjectLifecycleBindings::GetSpecs()'
)) {
    if (-not $CSharpBindingRendererSource.Contains($RequiredLifecycleFacadeContract) -and
        -not $CSharpBindingSliceSource.Contains($RequiredLifecycleFacadeContract)) {
        Add-Violation "C# object lifecycle facade pipeline is missing $RequiredLifecycleFacadeContract"
    }
}
$DynamicProjectileSource = Read-RequiredFile 'Samples/CSharp/DynamicProjectile/DynamicProjectileScript.cs'
$DynamicProjectileProfile = Read-RequiredFile 'Samples/CSharp/DynamicProjectile/DynamicProjectile.csharp-profile.json'
$DynamicProjectileReloadFixture = Read-RequiredFile 'Tests/Fixtures/CSharp/P49_4_DynamicProjectileRejectedReload.cs'
$DynamicProjectileIntegrationTest = Read-RequiredFile 'Source/AvidScriptEditor/Private/Tests/AvidScriptEditorDynamicProjectileIntegrationTests.cpp'
foreach ($RequiredDynamicProjectileSourceContract in @(
    '[AvidStateContract(AvidStateMode.Explicit)]',
    '[AvidTransient]',
    'UE.SetTimer(SpawnDelaySeconds, SpawnTimerId)',
    'UE.SpawnActor(',
    'ProjectClasses.TwinStickProjectile',
    'Projectile.GetActorLocation()',
    'Projectile.SetActorScale3D(',
    'UE.DestroyActor(Projectile)'
)) {
    if (-not $DynamicProjectileSource.Contains($RequiredDynamicProjectileSourceContract)) {
        Add-Violation "dynamic projectile C# gameplay loop is missing $RequiredDynamicProjectileSourceContract"
    }
}
foreach ($RequiredDynamicProjectileProfileContract in @(
    '"schema_version": 2',
    '"package_name": "avidscript.sample.dynamic_projectile"',
    '"class_path": "/Script/Engine.Actor"',
    '"K2_GetActorLocation"',
    '"SetActorScale3D"',
    '"script_name": "TwinStickProjectile"',
    '"class_path": "/Game/Variant_TwinStick/Blueprints/BP_TwinStickProjectile.BP_TwinStickProjectile_C"',
    '"base_class_path": "/Script/AvidTPSTemplate.TwinStickProjectile"'
)) {
    if (-not $DynamicProjectileProfile.Contains($RequiredDynamicProjectileProfileContract)) {
        Add-Violation "dynamic projectile project profile is missing $RequiredDynamicProjectileProfileContract"
    }
}
if (-not $DynamicProjectileReloadFixture.Contains('UE.SpawnActor(') -or
    -not $DynamicProjectileReloadFixture.Contains('avid_on_begin_play')) {
    Add-Violation 'dynamic projectile rejected reload fixture must attempt SpawnActor from candidate BeginPlay'
}
foreach ($RequiredDynamicProjectileIntegrationContract in @(
    'AvidScript.Editor.DynamicProjectile.GameplayLoop',
    'FAvidScriptEditorCSharpProfileService::LoadProfile',
    'FAvidScriptEditorCSharpBuildService::BuildProfile',
    'FAvidScriptWasmReloadManifestLoader::LoadFromFile',
    'Session.LoadInitialModule(',
    'Session.ReloadModule(',
    'binding_reload_effect_unsupported',
    'Session.TickLive(',
    'generation_mismatch'
)) {
    if (-not $DynamicProjectileIntegrationTest.Contains($RequiredDynamicProjectileIntegrationContract)) {
        Add-Violation "dynamic projectile integration evidence is missing $RequiredDynamicProjectileIntegrationContract"
    }
}
$ObjectLifecycleBenchmarkHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptRuntimeBenchmark.h'
$ObjectLifecycleBenchmarkSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Benchmark/AvidScriptObjectLifecycleBenchmark.cpp'
$ObjectLifecycleBenchmarkTest = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Tests/AvidScriptObjectLifecycleBenchmarkTests.cpp'
$ActorBindingHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptActorBinding.h'
$ActorBindingSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptActorBinding.cpp'
$SceneComponentBindingHeader = Read-RequiredFile 'Source/AvidScriptBindings/Public/AvidScriptSceneComponentBinding.h'
$WasmRuntimeSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp'
$ContinuationOwnerHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Continuation/AvidScriptSessionContinuations.h'
$ContinuationOwnerSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Continuation/AvidScriptSessionContinuations.cpp'
$ContinuationResultCodecHeader = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Continuation/AvidScriptContinuationResultCodec.h'
$ContinuationResultCodecSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Continuation/AvidScriptContinuationResultCodec.cpp'
$AsyncObjectLoaderSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/Continuation/AvidScriptAsyncObjectLoader.cpp'
foreach ($RequiredLifecycleBenchmarkContract in @(
    'FAvidScriptObjectLifecycleBenchmarkResult',
    'RunObjectLifecycleBenchmark',
    'NativeSpawnActor',
    'BindingSpawnActor',
    'NativeDestroyActor',
    'BindingDestroyActor',
    'ClassOrdinalResolve',
    'RegistryResolveSpawnedActor',
    'BindingPackageClassLoadsDuringWarmLoop',
    'BindingPackageReflectedNameLookupsDuringWarmLoop',
    'SpawnImportsPerIteration',
    'DestroyImportsPerIteration',
    'WasmLifecycleImportsObserved'
)) {
    if (-not $ObjectLifecycleBenchmarkHeader.Contains($RequiredLifecycleBenchmarkContract) -and
        -not $ObjectLifecycleBenchmarkSource.Contains($RequiredLifecycleBenchmarkContract)) {
        Add-Violation "object lifecycle benchmark is missing $RequiredLifecycleBenchmarkContract"
    }
}
foreach ($RequiredLifecycleBenchmarkImplementation in @(
    'Package->TryResolveClassReference(0, ResolvedClass, ResolvedBaseClass)',
    'Registry.ResolveObject<AActor>(ResolveHandle, ResolveResult, false)',
    'DispatchLifecycleBenchmarkCall(',
    'RunLifecycleWasmCrossingProbe(',
    'const bool bNativeFirst = (RunIndex & 1) == 0',
    'Registry.GetLiveHandleCount() != 1',
    'Package->GetInstrumentation()',
    'InstrumentationAfterWarmLoop.ClassLoadCount',
    'InstrumentationAfterWarmLoop.ReflectedNameLookupCount'
)) {
    if (-not $ObjectLifecycleBenchmarkSource.Contains($RequiredLifecycleBenchmarkImplementation)) {
        Add-Violation "object lifecycle benchmark is missing fair warm-loop contract $RequiredLifecycleBenchmarkImplementation"
    }
}
if ($ObjectLifecycleBenchmarkSource -match '\b(LoadObject|StaticLoadObject|FindFunction)\s*\(') {
    Add-Violation 'object lifecycle benchmark timed owner must not perform class loads or reflected name lookup'
}
foreach ($RequiredLifecycleBenchmarkTestContract in @(
    'AvidScript.Performance.ObjectLifecycleBenchmarkSmoke',
    'Warm loop performs no binding package class loads',
    'Warm loop performs no binding package reflected name lookups',
    'Real WAMR probe observes Spawn and Destroy imports',
    'SpawnActor uses one host import',
    'DestroyActor uses one host import',
	'warm_binding_package_reflected_name_lookups',
    'Nearest-rank P95 uses the nineteenth of twenty samples'
)) {
    if (-not $ObjectLifecycleBenchmarkTest.Contains($RequiredLifecycleBenchmarkTestContract)) {
        Add-Violation "object lifecycle benchmark test is missing $RequiredLifecycleBenchmarkTestContract"
    }
}
if (-not $ObjectRegistryHeader.Contains('bool bIncludeObjectPath = true') -or
    -not $ObjectRegistrySource.Contains('SetSuccess(OutResult, Handle, Object, bIncludeObjectPath)') -or
    -not $BindingInvocationSource.Contains('ResolveObject<AActor>(Handle, ResolveResult, false)') -or
    -not $BindingInvocationSource.Contains('ReleaseHandle(Handle, ReleaseResult, false)') -or
    -not $RuntimeComponentSource.Contains('ObjectRegistry.RegisterObject(Owner, RegisterResult, true)')) {
    Add-Violation 'object registry must preserve diagnostic defaults and require explicit zero-diagnostic hot-path policy'
}
if (-not $ActorBindingHeader.Contains('EAvidScriptBindingDiagnosticsPolicy') -or
    -not $ActorBindingHeader.Contains('EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath') -or
    -not $SceneComponentBindingHeader.Contains('EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath') -or
    -not $ActorBindingSource.Contains('DiagnosticsPolicy == EAvidScriptBindingDiagnosticsPolicy::IncludeObjectPath') -or
    -not $WasmRuntimeSource.Contains('EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath')) {
    Add-Violation 'typed binding public APIs must preserve object-path diagnostics while WASM hot paths explicitly omit them'
}
foreach ($RequiredBindingInstrumentationContract in @(
    'FAvidScriptBindingPackageInstrumentation',
    'GetInstrumentation() const',
    'Instrumentation.ClassLoadCount',
    'Instrumentation.ReflectedNameLookupCount'
)) {
    if (-not $BindingInvocationHeader.Contains($RequiredBindingInstrumentationContract) -and
        -not $BindingInvocationSource.Contains($RequiredBindingInstrumentationContract)) {
        Add-Violation "binding package instrumentation is missing $RequiredBindingInstrumentationContract"
    }
}
if (-not $CSharpBindingArtifactHeader.Contains('EmitterVersion = TEXT("49.3.0")') -or
    -not $CSharpBindingArtifactHeader.Contains('DescriptorFileName = TEXT("bindings.v5.json")')) {
    Add-Violation 'C# binding artifact must identify the P49.3 schema-v5 object lifecycle surface'
}
foreach ($RequiredDescriptorSchemaVersion in 2..18) {
    $RequiredDescriptorSchemaToken = '$DescriptorSchemaVersion -ne ' + $RequiredDescriptorSchemaVersion
    if (-not $CSharpBindingPackageSource.Contains($RequiredDescriptorSchemaToken)) {
        Add-Violation "C# binding package resolver must preserve descriptor schema v2-v18 compatibility: $RequiredDescriptorSchemaToken"
    }
}
foreach ($PackedOwnerContract in @(
    "avidscript.owner_get_handle.v1",
    "avid_owner_get_handle",
    "()I",
    'Try-GetAvidScriptBindingJsonInt32',
	'$DescriptorSchemaVersion -lt 6',
	'[string]::IsNullOrWhiteSpace($SelfTypeId)',
	'packed owner capability requires descriptor schema v6 or newer with a non-empty self_type_id')) {
    if (-not $CSharpBindingPackageSource.Contains($PackedOwnerContract)) {
        Add-Violation "C# binding package resolver must validate the packed owner intrinsic exactly: $PackedOwnerContract"
    }
}
foreach ($RequiredValueCapabilityPackageContract in @(
    'avidscript.value_array_length.v1',
    'avidscript.value_array_load.v1',
    'avidscript.value_array_store.v1',
    'avidscript.value_release.v1',
    'avidscript.value_array_read_range.v1',
    'avidscript.value_array_write_range.v1',
    '$IsValueCapabilityImport',
    '$ValueCapabilityImports[$StableId]')) {
    if (-not $CSharpBindingPackageSource.Contains($RequiredValueCapabilityPackageContract)) {
        Add-Violation "C# binding package resolver must validate the shared value capability set exactly: $RequiredValueCapabilityPackageContract"
    }
}
foreach ($ActiveObjectTypePackageContract in @(
    'active_object_type_ordinals',
    '-isnot [System.Array]',
    'must be a JSON array',
    'HasActiveObjectTypeOrdinals',
    'ActiveObjectTypeOrdinals',
    'strictly increasing declared object ordinals'
)) {
    if (-not $CSharpBindingPackageSource.Contains(
            $ActiveObjectTypePackageContract)) {
        Add-Violation "C# binding package resolver is missing active object-type provenance $ActiveObjectTypePackageContract"
    }
}
if (-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 5') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 6') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 7') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 8') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 9') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 10') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 11') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 12') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 13') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 14') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 15') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 16') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 17') -or
	-not $RuntimeReloadSource.Contains('DescriptorSchemaVersion != 18')) {
	Add-Violation 'Runtime reload manifest loader must accept descriptor schema v5-v18 typed object packages'
}
foreach ($RequiredDelegateEventManifestContract in @(
    'delegate_event_count',
    'GetDelegateEventCount()',
    'DescriptorSchemaVersion >= 11'
)) {
    if (-not $CSharpBindingPackageSource.Contains($RequiredDelegateEventManifestContract) -and
        -not $RuntimeReloadSource.Contains($RequiredDelegateEventManifestContract) -and
        -not $BindingInvocationHeader.Contains($RequiredDelegateEventManifestContract)) {
        Add-Violation "schema v11-v12 manifest provenance is missing $RequiredDelegateEventManifestContract"
    }
}
foreach ($RequiredRuntimeManifestImportContract in @(
    'avidscript.owner_get_handle.v1',
    'bSeenPackedOwner',
    'FAvidScriptValueCapability::GetArrayImportSpecs()',
    'SeenValueCapabilities',
    'incomplete value capability set',
    'TryGetInt32Field(*ImportObject, TEXT("ordinal"), Ordinal)',
	'DescriptorSchemaVersion < 6',
    'GetExpectedSelfClass() == nullptr',
    '!bBindingPackageHasPackedOwnerCapability',
    'manifest_wasm_import_mismatch',
    'DeclaredImport->StableId != RuntimeImport.StableId',
    'DeclaredImport->Ordinal != RuntimeImport.Ordinal',
    'DeclaredImport->Signature != RuntimeImport.Signature')) {
    if (-not $RuntimeReloadSource.Contains($RequiredRuntimeManifestImportContract)) {
        Add-Violation "Runtime reload manifest must distinguish packed owner access and compare dynamic import identity exactly: $RequiredRuntimeManifestImportContract"
    }
}
if (-not $RuntimeSessionSource.Contains('Import.ImportName == TEXT("avid_owner_get_handle")') -or
    -not $RuntimeSessionSource.Contains('bRequiresPackedOwnerCapability') -or
    -not $RuntimeSessionSource.Contains('!Manifest.BindingPackage.IsValid()') -or
    -not $RuntimeSessionSource.Contains('Manifest.BindingPackage->GetExpectedSelfClass() == nullptr')) {
    Add-Violation 'Runtime activation must derive packed-owner use from exact actual imports and require a verified typed Self package'
}
foreach ($RequiredObjectFactoryModelContract in @(
    'FAvidScriptBindingObjectFactoryModel',
    'FAvidScriptBindingDescriptorTypeGraph',
    'IsDerivedFromClassPath',
    'MakeObjectFactoryStableId',
    'ValidateAvidScriptBindingV7ObjectFactories',
    'bFactoryClassReference == bActorLifecycleReference',
    'class_references.capability',
    'OutPackage.SchemaVersion != 8',
    'Root->TryGetArrayField(TEXT("object_factories")')) {
    if (-not $BindingDescriptorHeader.Contains($RequiredObjectFactoryModelContract) -and
        -not $BindingDescriptorSource.Contains($RequiredObjectFactoryModelContract)) {
        Add-Violation "descriptor v7 object-factory owner is missing $RequiredObjectFactoryModelContract"
    }
}
foreach ($RequiredObjectFactoryGeneratorContract in @(
    'GenerateWithObjectFactories',
    'Package.SchemaVersion = bHasWritableProperties',
	'WritablePropertyGeneratorVersion',
    'FAvidScriptEditorObjectTypeGraph::Build(',
    'Package.ObjectFactories.Sort',
    'FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical')) {
    if (-not $BindingDescriptorGeneratorSource.Contains($RequiredObjectFactoryGeneratorContract)) {
        Add-Violation "descriptor v7 generator owner is missing $RequiredObjectFactoryGeneratorContract"
    }
}
foreach ($RequiredObjectFactoryPlanContract in @(
    'FAvidScriptObjectFactoryPlan',
    'GetDescriptorSchemaVersion',
    'ObjectFactoryPlans.SetNum(Model.ObjectFactories.Num())',
    'FactoryClassesByReferenceId',
    'ObjectTypeOrdinalsByClassPath',
    'binding_class_capability_missing',
    'TryResolveObjectFactory')) {
    if (-not $ObjectFactoryPolicyHeader.Contains($RequiredObjectFactoryPlanContract) -and
        -not $BindingInvocationHeader.Contains($RequiredObjectFactoryPlanContract) -and
        -not $BindingInvocationSource.Contains($RequiredObjectFactoryPlanContract)) {
        Add-Violation "immutable object-factory package plan owner is missing $RequiredObjectFactoryPlanContract"
    }
}
foreach ($RequiredObjectFactoryProvenanceContract in @(
    'object_factory_count',
    "Properties['object_factories']",
    'DescriptorSchemaVersion -ge 7')) {
    if (-not $CSharpBindingPackageSource.Contains($RequiredObjectFactoryProvenanceContract)) {
        Add-Violation "C# binding package resolver is missing descriptor v7 factory provenance $RequiredObjectFactoryProvenanceContract"
    }
}
if (-not $RuntimeReloadSource.Contains('GetObjectFactoryCount()') -or
    -not $RuntimeReloadSource.Contains('GetDescriptorSchemaVersion()') -or
    -not $RuntimeReloadSource.Contains('package.json descriptor_schema_version does not match the descriptor') -or
    -not $RuntimeReloadSource.Contains('TEXT("object_factory_count")') -or
    -not $RuntimeReloadSource.Contains('DescriptorSchemaVersion >= 7')) {
    Add-Violation 'Runtime reload must validate descriptor schema and extensible factory provenance against the immutable package plan'
}
if (-not $CSharpBindingRendererSource.Contains(
        'FAvidScriptBindingDescriptorTypeGraph::IsDerivedFromClassPath(') -or
    -not $CSharpBindingRendererSource.Contains(
        'ResultType->ClassPath != Reference.BaseClassPath') -or
    -not $BindingInvocationSource.Contains(
        'FAvidScriptBindingDescriptorTypeGraph::IsDerivedFromClassPath(')) {
    Add-Violation 'renderer and runtime must validate and share descriptor type-graph capability classification'
}
foreach ($RequiredLegacyClassReferenceRendererContract in @(
    'bHasTypedClassReferenceSurface',
    '? Reference->ResultTypeId',
    ': Reference->BaseClassPath',
    'TEXT("object:") + Reference->BaseClassPath',
    'class_references.base_class_path')) {
    if (-not $CSharpBindingRendererSource.Contains($RequiredLegacyClassReferenceRendererContract)) {
        Add-Violation "C# binding renderer must preserve schema v5 class-reference emission from base_class_path: $RequiredLegacyClassReferenceRendererContract"
    }
}
foreach ($RequiredStructWireRendererContract in @(
    'AppendStructWireDeclarations',
    'FindRenderedTypeById',
	'FAvidScriptBindingDescriptorLayout::ValidateTypeGraph',
	'OutSource.Empty()',
    '[StructLayout(LayoutKind.Explicit, Size = %d)]',
    '[FieldOffset(%d)]',
	'__avidscript_bool_%d',
	'ChildType->Size == 4',
    'Type.Kind == TEXT("struct_wire")',
    'NativeParameters.Add(TEXT("in ") + OutPublicType + TEXT(" value"))'
)) {
    if (-not $CSharpBindingRendererSource.Contains($RequiredStructWireRendererContract)) {
        Add-Violation "C# schema v9 recursive struct renderer is missing $RequiredStructWireRendererContract"
    }
}
foreach ($ForbiddenStructWireRendererSpecialCase in @(
    'StructWireRoundTrip',
    'FAvidScriptStructWire',
    'test_struct_wire'
)) {
    if ($CSharpBindingRendererSource.Contains($ForbiddenStructWireRendererSpecialCase)) {
        Add-Violation "C# schema v9 recursive struct renderer must not select fixture or API names: $ForbiddenStructWireRendererSpecialCase"
    }
}
if ($BindingInvocationSource.Contains('CustomTimeDilation') -or
    $CSharpBindingRendererSource.Contains('CustomTimeDilation') -or
    $BindingInvocationSource.Contains('RootComponent') -or
    $CSharpBindingRendererSource.Contains('RootComponent')) {
    Add-Violation 'property runtime and renderer must stay data-driven without per-property API switches'
}

# Phase 50 typed-project API contracts are checked inside their production
# builders, initializers, and dispatch closure. Test fixtures and comments cannot
# satisfy these checks.
foreach ($RequiredTypedDescriptorField in @('ObjectTypeOrdinal', 'SelfTypeId', 'ResultTypeId')) {
    if (-not $BindingDescriptorHeader.Contains($RequiredTypedDescriptorField)) {
        Add-Violation "binding descriptor v6 model is missing $RequiredTypedDescriptorField"
    }
}

$DescriptorSelectionIdentitySource = Get-SourceSlice `
    $BindingDescriptorSource `
    'FString FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(' `
    'FString FAvidScriptBindingDescriptorIdentity::MakePackageHash(' `
    'descriptor selection identity builder'
$DescriptorPackageIdentitySource = Get-SourceSlice `
    $BindingDescriptorSource `
    'FString FAvidScriptBindingDescriptorIdentity::MakePackageHash(' `
    'bool FAvidScriptBindingDescriptorParser::Parse(' `
    'descriptor package identity builder'
$DescriptorGenerationSource = Get-SourceSlice `
    $BindingDescriptorGeneratorSource `
    'bool GenerateBindingDescriptor(' `
    '} // namespace' `
    'descriptor canonical generation path'

Test-RequiredTokenSequence $DescriptorSelectionIdentitySource @(
    'SelectionKeys.Sort(',
    'TEXT("descriptor_selection_v6")',
    'AppendAvidScriptBindingIdentityField(Identity, TEXT("self_type_id"), Package.SelfTypeId);',
    'TEXT("object_type_ordinal")',
    'FString::FromInt(Type.ObjectTypeOrdinal)',
    'AppendAvidScriptBindingIdentityField(Identity, TEXT("object_class_path"), Type.ClassPath);',
    'AppendAvidScriptBindingIdentityField(Identity, TEXT("object_base_type_id"), Type.BaseTypeId);',
    'AppendAvidScriptBindingIdentityField(Identity, TEXT("result_type_id"), Reference.ResultTypeId);',
    'return FAvidScriptHash::Sha256HexUtf8(Identity);'
) 'descriptor v6 selection canonical builder'
Test-RequiredTokenSequence $DescriptorPackageIdentitySource @(
    'TEXT("descriptor_package_v6")',
    'AppendAvidScriptBindingIdentityField(Identity, TEXT("self_type_id"), Package.SelfTypeId);',
    'TEXT("object_type_ordinal")',
    'FString::FromInt(Type.ObjectTypeOrdinal)',
    'AppendAvidScriptBindingIdentityField(Identity, TEXT("object_class_path"), Type.ClassPath);',
    'AppendAvidScriptBindingIdentityField(Identity, TEXT("object_base_type_id"), Type.BaseTypeId);',
    'AppendAvidScriptBindingIdentityField(Identity, TEXT("result_type_id"), Reference.ResultTypeId);',
    'return FAvidScriptHash::Sha256HexUtf8(Identity);'
) 'descriptor v6 package canonical builder'
Test-RequiredTokenSequence $DescriptorGenerationSource @(
    'Bindings.Sort(',
    'Package.ClassReferences.Sort(',
    'Package.Types.Sort(',
    'Package.SelfTypeId = SelfNode->TypeId;',
    'Reference.ResultTypeId = ResultNode->TypeId;',
    'Package.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);',
    'Package.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package);'
) 'descriptor stable ordering and hash publication path'

$DescriptorLoadSource = Get-SourceSlice `
    $BindingInvocationSource `
    'bool FAvidScriptBindingPackage::LoadDescriptor(' `
    'const FString& FAvidScriptBindingPackage::GetPackageName() const' `
    'binding package descriptor activation'
foreach ($RequiredObjectPlanContract in @(
    'Package->Impl->ObjectTypePlans[Type.ObjectTypeOrdinal]',
    'Model.SelfTypeId',
    'Reference.ResultTypeId',
    'Package->Impl->ExpectedSelfClass'
)) {
    if (-not $DescriptorLoadSource.Contains($RequiredObjectPlanContract)) {
        Add-Violation "typed object package activation is missing $RequiredObjectPlanContract"
    }
}

$ObjectTypeSpecFactorySource = Get-SourceSlice `
    $ObjectTypeBindingSource `
    'FAvidScriptObjectTypeBindingSpec MakeObjectTypeSpec(' `
    '} // namespace' `
    'object type capability initializer'
$ObjectTypeSpecsSource = Get-SourceSlice `
    ($ObjectTypeBindingSource + "`n<AVIDSCRIPT_EOF>") `
    'TConstArrayView<FAvidScriptObjectTypeBindingSpec> FAvidScriptObjectTypeBindings::GetSpecs()' `
    '<AVIDSCRIPT_EOF>' `
    'object type capability table'
Test-RequiredTokenSequence $ObjectTypeSpecFactorySource @(
    'Spec.Kind = Kind;',
    'Spec.ModuleName = TEXT("avidscript");',
    'Spec.ImportName = ImportName;',
    'Spec.Signature = Signature;',
    'return Spec;'
) 'object type capability initializer'
if (-not [regex]::IsMatch(
    $ObjectTypeSpecsSource,
    'MakeObjectTypeSpec\(\s*EAvidScriptBindingInvocationKind::ObjectTypeIsA,\s*TEXT\("avidscript\.object_type\.v1\|is_a\|object_handle,object_type_ordinal->i32"\),\s*TEXT\("avid_object_type_is_a"\),\s*TEXT\("\(iii\)i"\)\s*\)')) {
    Add-Violation 'object type capability must bind kind, identity, avid_object_type_is_a, and (iii)i in one initializer record'
}

$OwnerGetHandleSource = Get-SourceSlice `
    $VmHostBindingsSource `
    'int64_t OwnerGetHandle(' `
    'int32_t TimerSetOnce(' `
    'packed owner host function'
$StaticHostCatalogSource = Get-SourceSlice `
    $StaticHostImportSource `
    'const FAvidScriptVmStaticHostImport GStaticHostImports[] = {' `
    'static_assert(' `
    'static host import catalog'
$StaticHostImportPolicySource = Get-SourceSlice `
    $VmHostBindingsSource `
    'bool IsAvidScriptVmStaticHostImport(' `
    'bool RegisterAvidScriptWamrHostBindings()' `
    'static host import policy'
$StaticHostRegistrationSource = Get-SourceSlice `
    $VmHostBindingsSource `
    'bool RegisterAvidScriptWamrHostBindings()' `
    'void UnregisterAvidScriptWamrHostBindings()' `
    'static host import registration'

$CanonicalStaticImportNames = @(
    'host_add_i32',
    'host_fail_i32',
    'actor_get_location',
    'actor_set_location',
    'actor_add_location_offset',
    'actor_get_rotation',
    'actor_set_rotation',
    'actor_get_scale',
    'actor_set_scale',
    'actor_get_transform_batch',
    'actor_get_root_component',
    'scene_component_get_world_location',
    'scene_component_set_world_location',
    'owner_get_slot',
    'owner_get_generation',
    'avid_owner_get_handle',
    'timer_set_once',
    'timer_cancel',
    'continuation_delay',
    'continuation_cancel',
    'continuation_load_object',
    'continuation_result_read',
    'continuation_state_store',
    'continuation_state_read',
    'continuation_cancel_source_create',
    'continuation_cancel_source_cancel',
    'continuation_cancel_source_release',
    'continuation_bind_cancel',
	'event_subscribe',
	'event_unsubscribe',
	'avid_delegate_output_write',
	'avid_data_lane_epoch',
    'avid_data_lane_submit',
    'avid_value_array_length',
    'avid_value_array_load',
    'avid_value_array_store',
    'avid_value_array_read_range',
    'avid_value_array_write_range',
    'avid_value_release',
    'avid_value_text_to_string',
    'avid_value_container_count',
    'avid_value_container_read',
    'avid_value_container_write',
    'avid_value_container_resize',
    'avid_value_container_clear',
    'avid_value_container_find',
    'avid_value_container_upsert',
    'avid_value_container_remove'
)
$CompatibilityStaticImportNames = @(
    $CanonicalStaticImportNames | Where-Object {
        $_ -notin @(
			'avid_owner_get_handle',
			'avid_delegate_output_write',
			'avid_data_lane_epoch',
            'avid_data_lane_submit',
            'avid_value_array_length',
            'avid_value_array_load',
            'avid_value_array_store',
            'avid_value_array_read_range',
            'avid_value_array_write_range',
            'avid_value_release',
            'avid_value_text_to_string',
            'avid_value_container_count',
            'avid_value_container_read',
            'avid_value_container_write',
            'avid_value_container_resize',
            'avid_value_container_clear',
            'avid_value_container_find',
            'avid_value_container_upsert',
            'avid_value_container_remove')
    })

$StaticHostCatalogRecords = @(
    [regex]::Matches(
        $StaticHostCatalogSource,
        '\{\s*EAvidScriptHostBindingId::(?<binding>[A-Za-z0-9_]+)\s*,\s*"(?<name>[a-z0-9_]+)"\s*,\s*"(?<signature>[^"]+)"\s*,\s*(?<compatibility>true|false)\s*\}')
)
$CatalogStaticImportNames = @(
    $StaticHostCatalogRecords | ForEach-Object { $_.Groups['name'].Value })
$CompatibilityCatalogStaticImportNames = @(
    $StaticHostCatalogRecords
    | Where-Object { $_.Groups['compatibility'].Value -eq 'true' }
    | ForEach-Object { $_.Groups['name'].Value })
Test-NameAllowlist `
    $CatalogStaticImportNames `
    $CanonicalStaticImportNames `
    'canonical static host catalog'
Test-NameAllowlist `
    $CompatibilityCatalogStaticImportNames `
    $CompatibilityStaticImportNames `
    'compatibility static host catalog'
Test-RequiredTokenSequence $OwnerGetHandleSource @(
    'Call.BindingId = EAvidScriptHostBindingId::OwnerGetHandle;',
    'Dispatch(ExecEnv, StaticImportName(EAvidScriptHostBindingId::OwnerGetHandle), Call, Result)',
    'Result.ReturnValueI64'
) 'packed owner host function'
$RuntimeOwnerHandleSource = Get-SourceSlice `
    $RuntimeSource `
    'int64 FAvidScriptWasmRuntimeInstance::HandleOwnerGetHandleImport()' `
    'int32 FAvidScriptWasmRuntimeInstance::HandleActorGetLocationImport(' `
    'packed owner runtime handler'
$RuntimeHostDispatchSource = Get-SourceSlice `
    ($RuntimeSource + "`n<AVIDSCRIPT_EOF>") `
    'bool FAvidScriptWasmRuntimeInstance::DispatchHostCall(' `
    '<AVIDSCRIPT_EOF>' `
    'runtime static host dispatch'
Test-RequiredTokenSequence $RuntimeOwnerHandleSource @(
    'const FAvidScriptObjectHandle OwnerHandle = HostContext.OwnerHandle;',
    'const uint64 PackedHandle = static_cast<uint64>(OwnerHandle.Slot)',
    '(static_cast<uint64>(OwnerHandle.Generation) << 32)',
    'return static_cast<int64>(PackedHandle);'
) 'packed owner runtime handler'
Test-RequiredTokenSequence $RuntimeHostDispatchSource @(
    'case EAvidScriptHostBindingId::OwnerGetHandle:',
    'const int64 Value = HandleOwnerGetHandleImport();',
    'return FinishI64(Value, Value != 0);'
) 'packed owner runtime dispatch'
if (-not $StaticHostCatalogSource.Contains(
    '{ EAvidScriptHostBindingId::OwnerGetHandle, "avid_owner_get_handle", "()I", false }')) {
    Add-Violation 'packed owner name, binding, signature, and compatibility policy must share one catalog record'
}
if (-not $VmHostBindingsSource.Contains('constexpr const char* CanonicalModuleName = "avidscript";') -or
    -not $StaticHostRegistrationSource.Contains('BuildWamrStaticHostSymbolTables();') -or
    -not $StaticHostRegistrationSource.Contains(
        'wasm_runtime_register_natives(CanonicalModuleName, GNativeSymbols.GetData(), GNativeSymbols.Num())') -or
    -not $StaticHostRegistrationSource.Contains(
        'wasm_runtime_register_natives(') -or
    -not $VmHostBindingsSource.Contains('if (Import.bSupportsEnvCompatibility)') -or
    -not $StaticHostImportPolicySource.Contains('(ModuleName == TEXT("avidscript") || Import.bSupportsEnvCompatibility)')) {
    Add-Violation 'generated WAMR symbol tables must preserve canonical and compatibility catalog policy'
}

$ExpectedCapabilityImportNames = @(
    'avid_object_spawn_actor',
    'avid_object_destroy_actor',
    'avid_object_is_a',
    'avid_object_type_is_a'
)
$CapabilityImportNames = @(
    [regex]::Matches(
        $ObjectLifecycleBindingSource + "`n" + $ObjectTypeBindingSource,
        'TEXT\("(?<name>avid_[a-z0-9_]+)"\)')
    | ForEach-Object { $_.Groups['name'].Value }
    | Sort-Object -Unique)
$UnexpectedCapabilityImportNames = @(
    $CapabilityImportNames | Where-Object { $ExpectedCapabilityImportNames -notcontains $_ })
$MissingCapabilityImportNames = @(
    $ExpectedCapabilityImportNames | Where-Object { $CapabilityImportNames -notcontains $_ })
if ($UnexpectedCapabilityImportNames.Count -gt 0 -or
    $MissingCapabilityImportNames.Count -gt 0 -or
    $CapabilityImportNames.Count -ne $ExpectedCapabilityImportNames.Count) {
    $CapabilityAllowlistViolation = 'descriptor capability imports differ from the generic allowlist' `
        + " | missing=$($MissingCapabilityImportNames -join ',')" `
        + " | unexpected=$($UnexpectedCapabilityImportNames -join ',')"
    Add-Violation $CapabilityAllowlistViolation
}
$GeneratedBindingImportLiterals = @(
    [regex]::Matches($BindingDescriptorGeneratorSource, 'TEXT\("(?<name>avid_[a-z0-9_]*)"\)')
    | ForEach-Object { $_.Groups['name'].Value }
    | Sort-Object -Unique)
$ExpectedGeneratedBindingImportPrefixes = @(
    'avid_on_delegate_',
    'avid_s1_',
    'avid_ue_'
)
$UnexpectedGeneratedBindingImportPrefixes = @(
    $GeneratedBindingImportLiterals | Where-Object { $ExpectedGeneratedBindingImportPrefixes -cnotcontains $_ })
$MissingGeneratedBindingImportPrefixes = @(
    $ExpectedGeneratedBindingImportPrefixes | Where-Object { $GeneratedBindingImportLiterals -cnotcontains $_ })
if ($UnexpectedGeneratedBindingImportPrefixes.Count -gt 0 -or
    $MissingGeneratedBindingImportPrefixes.Count -gt 0 -or
    $GeneratedBindingImportLiterals.Count -ne $ExpectedGeneratedBindingImportPrefixes.Count) {
    Add-Violation ('reflected project symbols must use only descriptor-derived avid_ue_, avid_s1_, and avid_on_delegate_ namespaces' `
        + " | missing=$($MissingGeneratedBindingImportPrefixes -join ',')" `
        + " | unexpected=$($UnexpectedGeneratedBindingImportPrefixes -join ',')")
}
foreach ($RequiredDynamicRegistryContract in @(
    'Import.ModuleName != TEXT("avidscript")',
    'IsAvidScriptDynamicSafeToken(Import.ImportName)',
    'IsAvidScriptVmStaticHostImport(Import.ModuleName, Import.ImportName)',
    'InvokeAvidScriptDynamicRawImport'
)) {
    if (-not $VmDynamicRegistrySource.Contains($RequiredDynamicRegistryContract)) {
        Add-Violation "dynamic host registry is missing generic package contract $RequiredDynamicRegistryContract"
    }
}

$RenderMethodSource = Get-SourceSlice `
    $CSharpBindingRendererSource `
    'bool RenderMethod(' `
    'bool RenderPropertyGetter(' `
    'generated C# method renderer'
$RenderPropertySource = Get-SourceSlice `
    $CSharpBindingRendererSource `
    'bool RenderPropertyGetter(' `
    'void AppendVector(' `
    'generated C# property renderer'
$EmitReferenceSource = Get-SourceSlice `
    $CSharpBindingRendererSource `
    'bool FAvidScriptEditorCSharpBindingRenderer::EmitReferenceSource(' `
    'bool FAvidScriptEditorCSharpBindingRenderer::EmitManifest(' `
    'generated C# facade publisher'
$EmitManifestSource = Get-SourceSlice `
    ($CSharpBindingRendererSource + "`n<AVIDSCRIPT_EOF>") `
    'bool FAvidScriptEditorCSharpBindingRenderer::EmitManifest(' `
    '<AVIDSCRIPT_EOF>' `
    'generated C# manifest publisher'
foreach ($GenericRendererSource in @($RenderMethodSource, $RenderPropertySource)) {
    if (-not $GenericRendererSource.Contains('*EscapeCSharpString(Binding.HostImport.Module)') -or
        -not $GenericRendererSource.Contains('*EscapeCSharpString(Binding.HostImport.Name)')) {
        Add-Violation 'project method/property wrappers must derive imports from descriptor HostImport records'
    }
}
foreach ($RequiredGenericFacadeContract in @(
    'for (const FAvidScriptBindingTypeModel* Type : ObjectTypes)',
    'AppendObjectHandleProxy(',
    'for (const FAvidScriptBindingClassReferenceModel* Reference :',
    '*EscapeCSharpString(Spec.ModuleName)',
    '*EscapeCSharpString(Spec.ImportName)'
)) {
    if (-not $EmitReferenceSource.Contains($RequiredGenericFacadeContract)) {
        Add-Violation "typed facade must remain descriptor/spec driven: $RequiredGenericFacadeContract"
    }
}
foreach ($RequiredObjectTypeManifestContract in @(
    'FAvidScriptObjectTypeBindings::GetSpecs()',
    'Spec.StableId',
    'Spec.ImportName',
    'Spec.Signature')) {
    if (-not $EmitManifestSource.Contains($RequiredObjectTypeManifestContract)) {
        Add-Violation "generated C# manifest must publish the shared object-type capability exactly: $RequiredObjectTypeManifestContract"
    }
}
foreach ($RequiredSliceCapabilityContract in @(
    'FAvidScriptEditorCSharpBindingRenderer::GetManifestImportCount(AuthorizationModel)',
    'FAvidScriptObjectTypeBindings::GetSpecs()',
    'FAvidScriptValueCapability::GetArrayImportSpecs()',
    'avidscript.owner_get_handle.v1',
    'slice_import_identity_mismatch')) {
    if (-not $CSharpBindingSliceSource.Contains($RequiredSliceCapabilityContract)) {
        Add-Violation "C# binding slice identity must validate every shared manifest capability: $RequiredSliceCapabilityContract"
    }
}
if (-not $RuntimeReloadSource.Contains('bRequiresPackedOwnerCapability') -or
    -not $RuntimeReloadSource.Contains('!bBindingPackageHasPackedOwnerCapability') -or
    $RuntimeReloadSource.Contains('bRequiresPackedOwnerCapability != bBindingPackageHasPackedOwnerCapability')) {
    Add-Violation 'Runtime reload must treat package capabilities as an authorization superset while requiring every script packed-owner import to be authorized'
}
foreach ($RequiredWasmImportIdentityContract in @(
    'ActualLayout.FunctionImports',
    'ExpectedImports',
    'manifest_wasm_import_mismatch')) {
    if (-not $VmImportPolicySource.Contains($RequiredWasmImportIdentityContract)) {
        Add-Violation "VM import policy must compare expected imports with the actual WASM function import identities: $RequiredWasmImportIdentityContract"
    }
}
foreach ($RequiredDynamicImportAuthorizationContract in @(
    'IsAvidScriptVmStaticHostImport',
    'BindingPackage->Imports',
    'AuthorizedDynamicImports',
    'binding_package_import_mismatch')) {
    if (-not $VmImportPolicySource.Contains($RequiredDynamicImportAuthorizationContract)) {
        Add-Violation "VM import policy must authorize each non-static WASM import against the current immutable binding package: $RequiredDynamicImportAuthorizationContract"
    }
}
$ManifestLoadImportSlice = Get-SourceSlice `
    $RuntimeReloadSource `
    'bool FAvidScriptWasmReloadManifestLoader::LoadFromFile(' `
    'OutResult.ByteSize = OutBytecode.Num();' `
    'manifest loader import authorization'
Test-RequiredTokenSequence $ManifestLoadImportSlice @(
	'InspectAvidScriptWasmModuleLayout(',
	'TryGetArrayField(TEXT("required_imports")',
	'if (!ValidateAvidScriptWasmImportContract('
) 'manifest loader must inspect actual imports, parse expected imports, and apply the shared policy in order'

$RuntimeSessionImportSlice = Get-SourceSlice `
    $RuntimeSessionSource `
    'bool FAvidScriptRuntimeSession::BuildValidatedRuntime(' `
    'bool FAvidScriptRuntimeSession::ValidateExpectedOwner(' `
    'direct Runtime Session import authorization'
Test-RequiredTokenSequence $RuntimeSessionImportSlice @(
    'if (Artifact.VmArtifact.CanonicalWasmBytes.IsEmpty()',
    'if (!InspectAndValidateAvidScriptWasmImportContract(',
    'ImportContractResult.ErrorCategory',
    'CandidateRuntime->LoadArtifact('
) 'direct Runtime Session must reject invalid artifacts and authorize actual imports before VM load'

$WamrBackendLoadSlice = Get-SourceSlice `
    $WamrBackendSource `
    'bool Load(' `
    'bool ResolveExport(' `
    'WAMR backend load authorization'
Test-RequiredTokenSequence $WamrBackendLoadSlice @(
    'InspectAvidScriptWasmModuleLayout(Bytecode, ModuleLayout, LayoutError)',
    'if (!ValidateAvidScriptVmImportContract(',
    'AcquireWamrLease(OutError)',
    'AcquireAvidScriptWamrDynamicImports(',
    'Module = wasm_runtime_load('
) 'VM backend must authorize actual imports before acquiring WAMR, registering natives, or loading the module'
foreach ($RequiredWamrCallLeaseContract in @(
    '++ActiveCallDepth;',
    'const bool bUnloadRequestedDuringCall = bUnloadDeferred;',
    'if (ActiveCallDepth == 0 && bUnloadDeferred)',
    'PerformUnload();',
    'reentrant_unload')) {
    if (-not $WamrBackendSource.Contains($RequiredWamrCallLeaseContract)) {
        Add-Violation "WAMR backend must defer physical unload until active guest calls unwind: $RequiredWamrCallLeaseContract"
    }
}
foreach ($RequiredSessionOperationLeaseContract in @(
    'TGuardValue<bool> MutationGuard(bMutationInProgress, true);',
    'TGuardValue<int32> GuestCallGuard(ActiveGuestCallDepth, ActiveGuestCallDepth + 1);',
    'TEXT("reentrant_operation")')) {
    if (-not $RuntimeSessionSource.Contains($RequiredSessionOperationLeaseContract)) {
        Add-Violation "Runtime Session must reject destructive or nested reentry while a guest call is active: $RequiredSessionOperationLeaseContract"
    }
}
if (-not $VmContractHeader.Contains('AVIDSCRIPTVM_API bool IsAvidScriptVmStaticHostImport')) {
    Add-Violation 'VM must publish one canonical static-host-import policy for registry and Runtime authorization checks'
}
foreach ($RequiredPackedOwnerRuntimeDefense in @(
    'BindingPackage.IsValid()',
    'BindingPackage->GetExpectedSelfClass()',
    'Packed owner access requires a binding package with ExpectedSelfClass')) {
    if (-not $WasmRuntimeSource.Contains($RequiredPackedOwnerRuntimeDefense)) {
        Add-Violation "packed owner runtime handler must independently require an authorized Self package: $RequiredPackedOwnerRuntimeDefense"
    }
}
foreach ($RequiredWasmInspectorImportContract in @(
    'FAvidScriptWasmFunctionImport',
    'OutLayout.FunctionImports',
    'FunctionImport.ModuleName',
    'FunctionImport.ImportName')) {
    if (-not $VmModuleLayoutSource.Contains($RequiredWasmInspectorImportContract) -and
        -not $VmModuleLayoutHeader.Contains($RequiredWasmInspectorImportContract)) {
        Add-Violation "WASM module layout inspector must preserve function import identity: $RequiredWasmInspectorImportContract"
    }
}
$AllowedFixedRendererImports = @(
    'avid_owner_get_handle',
    'avid_object_type_is_a',
    'timer_set_once',
    'timer_cancel',
    'continuation_delay',
    'continuation_cancel',
    'continuation_load_object',
    'continuation_result_read',
    'continuation_state_store',
    'continuation_state_read',
    'continuation_cancel_source_create',
    'continuation_cancel_source_cancel',
    'continuation_cancel_source_release',
    'continuation_bind_cancel',
    'event_subscribe',
    'event_unsubscribe',
    'avid_value_array_read_range',
    'avid_value_array_write_range',
    'avid_value_release',
    'avid_value_text_to_string',
    'avid_value_container_count',
    'avid_value_container_read',
    'avid_value_container_write',
    'avid_value_container_resize',
    'avid_value_container_clear',
    'avid_value_container_find',
    'avid_value_container_upsert',
    'avid_value_container_remove'
)
foreach ($RequiredGeneratedOutcomeFacadeContract in @(
    'AvidLatentAttribute(string module, string importName, int bindingOrdinal, string payloadTypeId)',
    'Binding.Completion.PayloadTypeId',
    'AvidOutcomeAwaitable<',
    'public readonly struct AvidOutcome<T>',
    'public readonly struct AvidOutcomeAwaitable<T>',
    'public readonly struct AvidOutcomeAwaiter<T>',
	'public bool Cancelled => StatusValue == AvidContinuationStatus.Cancelled;',
    'ContinuationResultRead(int bindingOrdinal, int resultSlot, int resultGeneration, int outputAddress, int byteCount)'
)) {
    if (-not $CSharpBindingRendererSource.Contains($RequiredGeneratedOutcomeFacadeContract)) {
        Add-Violation "generated C# outcome facade is missing $RequiredGeneratedOutcomeFacadeContract"
    }
}
$FixedRendererImports = @(
    [regex]::Matches(
        $CSharpBindingRendererSource,
        'EntryPoint = \\"(?<name>[a-z0-9_]+)\\"')
    | ForEach-Object { $_.Groups['name'].Value }
    | Sort-Object -Unique)
$UnexpectedFixedRendererImports = @(
    $FixedRendererImports | Where-Object { $AllowedFixedRendererImports -notcontains $_ })
$MissingFixedRendererImports = @(
    $AllowedFixedRendererImports | Where-Object { $FixedRendererImports -notcontains $_ })
if ($UnexpectedFixedRendererImports.Count -gt 0 -or
    $MissingFixedRendererImports.Count -gt 0 -or
    $FixedRendererImports.Count -ne $AllowedFixedRendererImports.Count) {
    $RendererAllowlistViolation = 'renderer fixed imports differ from the generic facade allowlist' `
        + " | missing=$($MissingFixedRendererImports -join ',')" `
        + " | unexpected=$($UnexpectedFixedRendererImports -join ',')"
    Add-Violation $RendererAllowlistViolation
}
$AllowedLiteralFacadeStructs = @(
    'FVector',
    'FRotator',
    'FTransform',
    'InputEvent',
    'TSubclassOfAActor',
    'AvidSubscription',
    'AvidContinuation',
    'AvidLoadedObject',
    'AvidCancellationToken',
    'AvidCancellationSource',
    'AvidDelayAwaitable',
    'AvidObjectAwaitable',
    'FAvidText'
)
$LiteralFacadeStructs = @(
    [regex]::Matches(
        $CSharpBindingRendererSource,
        'TEXT\("public readonly struct (?<name>[A-Za-z0-9_]+)"\)')
    | ForEach-Object { $_.Groups['name'].Value }
    | Sort-Object -Unique)
$UnexpectedLiteralFacadeStructs = @(
    $LiteralFacadeStructs | Where-Object { $AllowedLiteralFacadeStructs -notcontains $_ })
if ($UnexpectedLiteralFacadeStructs.Count -gt 0) {
    $FacadeStructViolation = 'renderer contains a hard-coded project wrapper instead of a descriptor-derived object handle' `
        + " | unexpected=$($UnexpectedLiteralFacadeStructs -join ',')"
    Add-Violation $FacadeStructViolation
}
foreach ($RequiredStaticCheckedCastContract in @(
    'public static %s TryCast(%s value)',
    'public static %s TryCast(AvidLoadedObject value)',
    'AvidScriptNative.ObjectTypeIsA(value.Slot, value.Generation, %d)',
    'internal static extern int ObjectTypeIsA(int slot, int generation, int targetOrdinal);'
)) {
    if (-not $EmitReferenceSource.Contains($RequiredStaticCheckedCastContract) -and
        -not $CSharpBindingRendererSource.Contains($RequiredStaticCheckedCastContract)) {
        Add-Violation "typed facade is missing static Derived.TryCast(Base) contract $RequiredStaticCheckedCastContract"
    }
}
if ($CSharpBindingRendererSource.Contains('public %s TryCast()')) {
    Add-Violation 'typed facade must not regress to the obsolete inverse instance TryCast shape'
}

$ConversionFunctionSource = Get-SourceSlice `
    $CSharpOperationLowererSource `
    'private static GuestRegister? LowerConversion(' `
    'private static GuestRegister? LowerFieldLoad(' `
    'C# conversion lowering function'
$UserDefinedConversionSource = Get-SourceSlice `
    $ConversionFunctionSource `
    'if (operation.Conversion.IsUserDefined)' `
    'GuestRegister? result = context.CreateTemporary(' `
    'C# user-defined conversion branch'
Test-RequiredTokenSequence $UserDefinedConversionSource @(
    'if (operation.Conversion.IsUserDefined)',
    'operation.Conversion.MethodSymbolId',
    'context.TryGetCallTarget(',
    '!callable.IsStatic',
    'callable.IsConstructor',
    '!callable.HasBody',
    'callable.Import is not null',
    'callable.Parameters.Count != 1',
    'callable.Parameters[0].RefKind',
    'callable.Parameters[0].TypeId',
    'callable.ReturnTypeId',
    'return EmitCall('
) 'validated SemanticConversion.IsUserDefined to Guest EmitCall path'

$ObjectTypeDispatchSource = Get-SourceSlice `
    $BindingInvocationSource `
    'bool DispatchAvidScriptObjectType(' `
    '} // namespace' `
    'checked object type dispatch'
$ObjectRegistryResolveSource = Get-SourceSlice `
    $ObjectRegistrySource `
    'UObject* FAvidScriptObjectRegistry::ResolveObject(' `
    'bool FAvidScriptObjectRegistry::ReleaseHandle(' `
    'object registry UObject resolver'
$ObjectTypePlanResolveSource = Get-SourceSlice `
    $BindingInvocationSource `
    'bool FAvidScriptBindingPackage::TryResolveObjectType(' `
    'UClass* FAvidScriptBindingPackage::GetExpectedSelfClass() const' `
    'cached object type plan resolver'
$TypedCastDispatchClosure = $ObjectTypeDispatchSource `
    + "`n" + $ObjectRegistryResolveSource `
    + "`n" + $ObjectTypePlanResolveSource
foreach ($ForbiddenCheckedCastLookup in @('FindObject', 'LoadObject', 'StaticLoadObject', 'GetPathName')) {
    if ($TypedCastDispatchClosure.Contains($ForbiddenCheckedCastLookup)) {
        Add-Violation "typed cast dispatch/helper closure must use cached plans instead of $ForbiddenCheckedCastLookup"
    }
}
foreach ($RequiredGenericDispatchContract in @(
    'UObject* Object = Context.ObjectRegistry->ResolveObject(Handle, ResolveResult, false);',
    'Package.TryResolveObjectType(static_cast<uint32>(Call.Arguments[2]), CachedClass)',
    'Object->IsA(CachedClass)'
)) {
    if (-not $ObjectTypeDispatchSource.Contains($RequiredGenericDispatchContract)) {
        Add-Violation "checked object type dispatch is missing UObject/cached-plan contract $RequiredGenericDispatchContract"
    }
}
if ($ObjectTypeDispatchSource -match '\bAActor\b' -or
    -not $ObjectRegistryResolveSource.Contains('UObject* FAvidScriptObjectRegistry::ResolveObject(') -or
    -not $ObjectRegistryResolveSource.Contains('UObject* Object = Slot.Object.Get();') -or
    -not $ObjectTypePlanResolveSource.Contains('UClass*& OutClass')) {
    Add-Violation 'checked object type dispatch and direct helpers must preserve UObject input and UClass plan contracts'
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
    'FAvidScriptEditorCSharpBindingEmitter::PublishProfile',
    'Request.AuthorizationBindingProfile',
    'Request.BindingSelectionHash',
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
if (-not $CSharpBindingSliceSource.Contains('FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile') -or
    -not $CSharpBindingSliceSource.Contains('FAvidScriptEditorBindingDescriptorModelSerializer::SerializeCanonical') -or
    -not $CSharpBindingSliceSource.Contains('FAvidScriptEditorCSharpBindingEmitter::PublishDerivedSliceDescriptor') -or
    $CSharpBindingSliceSource.Contains('FPlatformProcess::ExecProcess')) {
    Add-Violation 'C# BindingSliceService must reuse the factory-aware descriptor generator, canonical serializer, and derived package publisher without invoking builds'
}
$CSharpGuestContext = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpFunctionLoweringContext.cs'
$CSharpOperationLowerer = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpOperationLowerer.cs'
$CSharpClassReferencePolicySource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpClassReferencePolicy.cs'
$CSharpClassReferenceLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpClassReferenceLowerer.cs'
$CSharpTypeLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpTypeLowerer.cs'
$GuestInstructionValidatorSource = Read-RequiredFile 'Tools/AvidScript.GuestIr/Validation/GuestInstructionValidator.cs'
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
foreach ($RequiredClassReferenceGuestContract in @(
    'CanonicalName = "global::AvidScript.TSubclassOfAActor"',
    'TryLowerObjectCreation',
    'new GuestConstant("class_ref"',
    'type.Kind != "class_ref"',
    'class_ref_ordinal'
)) {
    if (-not $CSharpClassReferencePolicySource.Contains($RequiredClassReferenceGuestContract) -and
        -not $CSharpClassReferenceLowererSource.Contains($RequiredClassReferenceGuestContract) -and
        -not $CSharpTypeLowererSource.Contains($RequiredClassReferenceGuestContract) -and
        -not $GuestInstructionValidatorSource.Contains($RequiredClassReferenceGuestContract)) {
        Add-Violation "nominal C# Guest class reference lowering is missing $RequiredClassReferenceGuestContract"
    }
}
foreach ($RequiredClassReferenceAuthorizationContract in @(
    'ProjectClassesSymbolId',
    'UeSymbolId',
    'symbol.Name == "SpawnActor"',
    'symbol.Name == "actorClass"',
    'IsAuthorizedType(sourceType.Id, document.Symbols)',
    'IsAuthorizedType(targetType.Id, document.Symbols)')) {
    if (-not $CSharpClassReferencePolicySource.Contains($RequiredClassReferenceAuthorizationContract)) {
        Add-Violation "C# Guest class references must be authorized by the exact generated public API surface: $RequiredClassReferenceAuthorizationContract"
    }
}
foreach ($RequiredClassReferenceOrdinalContract in @(
    'IsAuthorizedOrdinal',
    'TryReadPublishedOrdinal',
    'IsCompatibleClassReferenceType',
    'publishedOrdinal == ordinal',
    'CSharpClassReferencePolicy.IsAuthorizedOrdinal')) {
    if (-not $CSharpClassReferencePolicySource.Contains($RequiredClassReferenceOrdinalContract) -and
        -not $CSharpClassReferenceLowererSource.Contains($RequiredClassReferenceOrdinalContract)) {
        Add-Violation "C# Guest class reference ordinals must be bound to compatible generated ProjectClasses provenance: $RequiredClassReferenceOrdinalContract"
    }
}
$SemanticAnalyzerSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticAnalyzer.cs'
$SemanticCallableProjectorSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticCallableProjector.cs'
$SemanticSymbolProjectorSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticSymbolProjector.cs'
$SemanticSymbolModelSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Model/SemanticSymbol.cs'
$SemanticContractSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Model/SemanticContract.cs'
$SemanticReachabilitySource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticReachabilityProjector.cs'
$SemanticGameplayEventSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticGameplayEventProjector.cs'
$SemanticContinuationSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticContinuationProjector.cs'
$SemanticAsyncSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticAsyncProjector.cs'
$SemanticAsyncStructuredSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticAsyncStructuredStatementProjector.cs'
$SemanticAsyncCfgSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticAsyncControlFlowProjector.cs'
$SemanticAsyncStateFlowSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Analysis/SemanticAsyncStateFlowAnalyzer.cs'
$SemanticAsyncModelSource = Read-RequiredFile 'Tools/AvidScript.CSharpSemantic/Model/SemanticAsync.cs'
$CSharpGuestLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpGuestLowerer.cs'
$CSharpGameplayEventLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpGameplayEventLowerer.cs'
$CSharpContinuationLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpContinuationLowerer.cs'
$CSharpAsyncLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpAsyncLowerer.cs'
$CSharpAsyncControlFlowLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpAsyncControlFlowLowerer.cs'
$CSharpAsyncCfgLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpAsyncCfgLowerer.cs'
$CSharpOutcomeOperationLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpOutcomeOperationLowerer.cs'
$CSharpLatentStoragePlannerSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpLatentStoragePlanner.cs'
$CSharpTypeLowererSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpTypeLowerer.cs'
$CSharpCompositeCapabilityPolicySource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Lowering/CSharpCompositeValueCapabilityPolicy.cs'
$CSharpSemanticInputValidatorSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Validation/CSharpSemanticInputValidator.cs'
$CSharpGuestDebugMapProjectorSource = Read-RequiredFile 'Tools/AvidScript.CSharpGuest/Diagnostics/CSharpGuestDebugMapProjector.cs'
$GuestArrayCapabilityIntrinsicsSource = Read-RequiredFile 'Tools/AvidScript.GuestIr/Model/GuestArrayCapabilityIntrinsics.cs'
$WasmFunctionCompilerSource = Read-RequiredFile 'Tools/AvidScript.WasmBackend/Codegen/WasmFunctionCompiler.cs'
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
foreach ($RequiredDirectAbiBuildContract in @(
    '$RequiredExports.Count -eq 0',
    'required_export_count = $RequiredExports.Count',
    'direct_abi_contract_invalid')) {
    if (-not $CSharpBuildScriptSource.Contains($RequiredDirectAbiBuildContract)) {
        Add-Violation "C# build pipeline must reject an empty or invalid Direct ABI export surface: $RequiredDirectAbiBuildContract"
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
foreach ($RequiredOptionalExportContract in @(
    '$SemanticModel.delegate_event_callbacks',
    '$UnexpectedDeclaredExports = @($RequiredExports | Where-Object { $DirectAbiExports -notcontains $_ })',
    '$MissingObservedExports = @($RequiredExports | Where-Object { $ObservedExports -notcontains $_ })',
    '$UnexpectedObservedExports = @($ObservedExports | Where-Object { $RequiredExports -notcontains $_ })')) {
    if (-not $CSharpBuildScriptSource.Contains($RequiredOptionalExportContract)) {
        Add-Violation "C# direct ABI must validate only the event hooks declared by the script: $RequiredOptionalExportContract"
    }
}
if ($CSharpBuildScriptSource.Contains(
        '$MissingDeclaredExports = @($DirectAbiExports | Where-Object { $RequiredExports -notcontains $_ })')) {
    Add-Violation 'C# direct ABI must not require every optional event hook in every script'
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
foreach ($RequiredPreparedSharedCapabilityContract in @(
    '$Ordinal -eq -1',
    'Try-GetAvidScriptBindingJsonInt32',
	'$IsDeclaredSharedCapability',
	'$ExpectedImportsByKey.ContainsKey($Key)',
	'$ExpectedImport.StableId -ceq $StableId',
	'$ExpectedImport.Module -ceq $Module',
	'$ExpectedImport.Name -ceq $Name',
	'$ExpectedImport.Signature -ceq $Signature')) {
    if (-not $CSharpPreparedSemanticSource.Contains($RequiredPreparedSharedCapabilityContract)) {
        Add-Violation "prepared semantic helper must validate declared shared capability provenance exactly: $RequiredPreparedSharedCapabilityContract"
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
    'SemanticUeTypeProjector.Project',
    'SemanticGameplayEventProjector.Project',
    'SemanticDelegateEventProjector.Project',
    'SemanticContinuationProjector.Project'
)) {
    if (-not $SemanticAnalyzerSource.Contains($RequiredReachabilityContract)) {
        Add-Violation "C# Semantic analyzer is missing reachability contract $RequiredReachabilityContract"
    }
}
foreach ($RequiredSemanticContract in @(
    'CurrentSchemaVersion = 19',
    'CurrentSemanticVersion = "1.21"'
)) {
    if (-not $SemanticContractSource.Contains($RequiredSemanticContract)) {
        Add-Violation "C# Semantic contract is missing current version token $RequiredSemanticContract"
    }
}
if (-not $SemanticAnalyzerSource.Contains('UeTypeDeclarations = ueTypeProjection.Declarations')) {
    Add-Violation 'C# Semantic analyzer is missing the versioned UE type declaration output contract'
}
foreach ($RequiredCompositeCapabilityProvenanceContract in @(
    'IsExecutableReferenceSource',
    'IsExecutableReferenceSource = isExecutableReferenceSource',
    'declaration is { IsExecutableReferenceSource: true }',
    '&& field.IsExecutableReferenceSource',
    '&& symbol.IsExecutableReferenceSource'
)) {
    if (-not $SemanticSymbolModelSource.Contains($RequiredCompositeCapabilityProvenanceContract) -and
        -not $SemanticSymbolProjectorSource.Contains($RequiredCompositeCapabilityProvenanceContract) -and
        -not $CSharpCompositeCapabilityPolicySource.Contains($RequiredCompositeCapabilityProvenanceContract)) {
        Add-Violation "C# composite capabilities are missing executable-reference provenance contract $RequiredCompositeCapabilityProvenanceContract"
    }
}
foreach ($RequiredControlledAsyncSemanticContract in @(
    'SemanticAsyncProjector.Project',
    'AsyncMethods = asyncProjection.Methods'
)) {
    if (-not $SemanticAnalyzerSource.Contains($RequiredControlledAsyncSemanticContract)) {
        Add-Violation "C# Semantic analyzer is missing controlled async contract $RequiredControlledAsyncSemanticContract"
    }
}
foreach ($RequiredControlledAsyncProjectionContract in @(
    'MaximumAwaitsPerMethod = 16',
    'MaximumAwaitsPerModule = 64',
    'CompilerCallbackIdStart',
    'AvidLatentAttribute',
    'binding_latent|',
    'DelayAsync',
    'NextTickAsync',
    'LoadObjectAsync',
    'TryUnwrapCancellationMarker',
    'CancellationTokenTypeName',
    'projectedCancellationToken,',
    'producer.PayloadDescriptorTypeId',
    'producer.PayloadValueType',
    'TryAttachStateFrames',
    'TryProjectEarlyReturnGuard',
    'EarlyReturnGuardOperationKind',
    'requiresControlFlowCfg',
    'MaximumStateSlotsPerAwait = 64',
    'SemanticAsyncStateFlowAnalyzer.Analyze',
    'SlotsByAwaitSegment',
    'type:synthetic:async_state:',
    'ASCS5410',
    'ASCS5401',
    'ASCS5408'
)) {
    if (-not $SemanticAsyncSource.Contains($RequiredControlledAsyncProjectionContract)) {
        Add-Violation "C# Semantic controlled async projector is missing $RequiredControlledAsyncProjectionContract"
    }
}
foreach ($RequiredStructuredAsyncProjectionContract in @(
    'SemanticAsyncStructuredStatementProjector',
    'MaximumStructuredFlowNodes',
    'MaximumStructuredFlowDepth',
    'IfStatementSyntax',
    'WhileStatementSyntax',
    'DoStatementSyntax',
    'ForStatementSyntax',
    'BreakStatementSyntax',
    'ContinueStatementSyntax',
    'LocalDeclarationOperationKind'
)) {
    if (-not $SemanticAsyncStructuredSource.Contains($RequiredStructuredAsyncProjectionContract)) {
        Add-Violation "C# Semantic structured async projector is missing $RequiredStructuredAsyncProjectionContract"
    }
}
foreach ($RequiredAsyncCfgProjectionContract in @(
    'SemanticAsyncControlFlowProjector',
    'SemanticAsyncControlFlowProjection',
    'MaximumControlFlowSegments',
    'GotoTransferKind',
    'BranchTransferKind',
    'AwaitTransferKind',
    'ReturnTransferKind',
    'CollectReachable',
    'callbackByDraft',
    'TryProjectAwaitSite',
    'SwitchStatementSyntax',
    'CaseSwitchLabelSyntax',
    'BuildSwitch',
    'ASCS5418',
    'ASCS5417'
)) {
    if (-not $SemanticAsyncCfgSource.Contains($RequiredAsyncCfgProjectionContract)) {
        Add-Violation "C# Semantic continuation CFG projector is missing $RequiredAsyncCfgProjectionContract"
    }
}
foreach ($RequiredAsyncStateFlowContract in @(
    'SemanticAsyncStateFlowAnalyzer',
    'SlotsByAwaitSegment',
    'AnalyzeControlFlow',
    'TransferControlFlowSegment',
    'TransferAwait',
    'TransferWhile',
    'TransferDoWhile',
    'TransferFor',
    'TryGetDirectLocal',
    'live.Remove(assignedLocal',
    'Break = liveAfter',
    'Continue = header'
)) {
    if (-not $SemanticAsyncStateFlowSource.Contains($RequiredAsyncStateFlowContract)) {
        Add-Violation "C# Semantic async state-flow analyzer is missing $RequiredAsyncStateFlowContract"
    }
}
foreach ($RequiredControlledAsyncModelContract in @(
    'SemanticAsyncMethod',
    'SemanticAsyncSegment',
    'SemanticAsyncAwaitSite',
    'SemanticOperation? CancellationToken = null',
    'SemanticAsyncStateFrame? StateFrame = null',
    'SemanticAsyncStateFrame',
    'SemanticAsyncStateSlot',
    'reentrant_zero_heap_cps',
    'async_early_return_guard',
    'async_local_declaration',
    'async_if',
    'async_while',
    'async_do_while',
    'async_for',
    'async_break',
    'async_continue',
    'async_return',
    'continuation_cfg',
    'SemanticAsyncControlTransfer',
    'EntrySegmentOrdinal = 0',
    'MaximumControlFlowSegments = 64',
    'MaximumStructuredFlowNodes = 256',
    'MaximumStructuredFlowDepth = 8'
)) {
    if (-not $SemanticAsyncModelSource.Contains($RequiredControlledAsyncModelContract)) {
        Add-Violation "C# Semantic controlled async model is missing $RequiredControlledAsyncModelContract"
    }
}
foreach ($RequiredControlledAsyncReachabilityContract in @(
    'IReadOnlyList<SemanticAsyncMethod> asyncMethods',
    'EnumerateAsyncOperations',
    'segment.Transfer?.Condition',
    'awaitSite.CancellationToken'
)) {
    if (-not $SemanticReachabilitySource.Contains($RequiredControlledAsyncReachabilityContract)) {
        Add-Violation "C# Semantic reachability is missing async segment contract $RequiredControlledAsyncReachabilityContract"
    }
}
foreach ($RequiredReachabilityProjection in @(
    'export_roots',
    'entrypoint_roots',
    'all_callables_compatibility',
    'AssociatedSymbolId',
    'gameplayEventCallbacks',
    'continuationCallbacks',
    'QueuePropertyAccessors',
    'PropertyAccess.Write',
    'PropertyAccess.ReadWrite'
)) {
    if (-not $SemanticReachabilitySource.Contains($RequiredReachabilityProjection)) {
        Add-Violation "C# Semantic reachability is missing projection contract $RequiredReachabilityProjection"
    }
}
if (-not $CSharpGuestLowererSource.Contains('GetReachableCallableIds') -or
    -not $CSharpBuildScriptSource.Contains('UsedAuthorizationBindingImports') -or
    -not $CSharpBuildScriptSource.Contains('UsedRuntimeBindingImports')) {
    Add-Violation 'C# Guest and build pipeline must consume semantic binding reachability'
}
foreach ($RequiredCompilerInjectedDelegateOutputContract in @(
    'Test-CompilerInjectedBindingImport',
    'avidscript.delegate_output_write.v1',
    'avid_delegate_output_write',
    '$ParameterTypes[2] -ceq "type:address"'
)) {
    if (-not $CSharpBuildScriptSource.Contains($RequiredCompilerInjectedDelegateOutputContract)) {
        Add-Violation "C# build authorization is missing compiler-injected delegate output contract $RequiredCompilerInjectedDelegateOutputContract"
    }
}
foreach ($RequiredAsyncContinuationFacadeContract in @(
    'public enum AvidContinuationStatus',
    'public readonly struct AvidLoadedObject',
    'public static class AvidAssets',
    'LoadObjectAsync(string assetPath, int callbackId)',
    'ContinuationLoadObject(string assetPath, int callbackId)',
    'TryCast(AvidLoadedObject value)'
)) {
    if (-not $CSharpBindingRendererSource.Contains($RequiredAsyncContinuationFacadeContract)) {
        Add-Violation "generated C# async continuation facade is missing $RequiredAsyncContinuationFacadeContract"
    }
}
foreach ($RequiredControlledAsyncFacadeContract in @(
    'public sealed class AvidLatentAttribute',
    'IsLatentStorageCompatible',
    'IsLatentParameterShapeSupported',
    'public readonly struct AvidDelayAwaitable',
    'public readonly struct AvidDelayAwaiter : INotifyCompletion',
    'DelayAsync(float delaySeconds)',
    'NextTickAsync()',
    'public readonly struct AvidObjectAwaitable',
    'public readonly struct AvidObjectAwaiter : INotifyCompletion',
    'LoadObjectAsync(string assetPath)',
    'public readonly struct AvidCancellationToken',
    'public readonly struct AvidCancellationSource',
    'WithCancellation(AvidCancellationToken token)',
    'ContinuationCancelSourceCreate()',
    'ContinuationBindCancel(long sourceToken, long continuationToken)'
)) {
    if (-not $CSharpBindingRendererSource.Contains($RequiredControlledAsyncFacadeContract)) {
        Add-Violation "generated C# controlled async facade is missing $RequiredControlledAsyncFacadeContract"
    }
}
foreach ($RequiredLatentGameplaySampleContract in @(
    'UKismetSystemLibrary.DelayAsync(0.25f)',
    '.WithCancellation(LifetimeCancellation.Token)',
    'LifetimeCancellation = AvidCancellationSource.Create()',
    'LifetimeCancellation.Cancel()',
    'LifetimeCancellation.Release()',
    'SetActorScale3D(new FVector(1.25f, 1.25f, 1.25f))'
)) {
    if (-not $LatentGameplaySampleSource.Contains($RequiredLatentGameplaySampleContract)) {
        Add-Violation "LatentGameplay sample is missing generated latent await contract $RequiredLatentGameplaySampleContract"
    }
}
foreach ($RequiredLatentGameplayProfileContract in @(
    '"class_path": "/Script/Engine.KismetSystemLibrary"',
    '"Delay"',
    '"SetActorScale3D"'
)) {
    if (-not $LatentGameplayProfile.Contains($RequiredLatentGameplayProfileContract)) {
        Add-Violation "LatentGameplay profile is missing reflected binding authorization $RequiredLatentGameplayProfileContract"
    }
}
foreach ($RequiredNetworkRpcSampleContract in @(
    'actor.HasAuthority()',
    'self.ServerSubmitValue(10)',
    'self.ClientApplyValue(20)',
    'self.MulticastAnnounceValue(30)'
)) {
    if (-not $NetworkRpcSampleSource.Contains($RequiredNetworkRpcSampleContract)) {
        Add-Violation "NetworkRpc sample is missing authority-directed call $RequiredNetworkRpcSampleContract"
    }
}
foreach ($RequiredNetworkRpcProfileContract in @(
    '"self_class_path": "/Script/AvidScriptEditor.AvidScriptBindingRuntimeNetworkTestActor"',
    '"HasAuthority"',
    '"ServerSubmitValue"',
    '"ClientApplyValue"',
    '"MulticastAnnounceValue"'
)) {
    if (-not $NetworkRpcProfile.Contains($RequiredNetworkRpcProfileContract)) {
        Add-Violation "NetworkRpc profile is missing reflected RPC authorization $RequiredNetworkRpcProfileContract"
    }
}
if (-not $NetworkRpcSampleReadme.Contains('脚本接收 RPC') -or
    -not $NetworkRpcSampleReadme.Contains('Replicated Property/RepNotify')) {
    Add-Violation 'NetworkRpc README must preserve the P57.12D1/D2/D3 boundary'
}
foreach ($RequiredReplicatedPropertySampleContract in @(
    'actor.HasAuthority()',
    'self.ReplicatedScore = self.ReplicatedScore + 10',
    'self.ReplicatedRoutedValue = 20'
)) {
    if (-not $ReplicatedPropertySampleSource.Contains($RequiredReplicatedPropertySampleContract)) {
        Add-Violation "ReplicatedProperty sample is missing authority-directed property use $RequiredReplicatedPropertySampleContract"
    }
}
foreach ($RequiredReplicatedPropertyProfileContract in @(
    '"self_class_path": "/Script/AvidScriptEditor.AvidScriptBindingRuntimeNetworkTestActor"',
    '"HasAuthority"',
    '"ReplicatedScore"',
    '"ReplicatedRoutedValue"'
)) {
    if (-not $ReplicatedPropertyProfile.Contains($RequiredReplicatedPropertyProfileContract)) {
        Add-Violation "ReplicatedProperty profile is missing reflected authorization $RequiredReplicatedPropertyProfileContract"
    }
}
if (-not $ReplicatedPropertySampleReadme.Contains('不会伪造 OnRep') -or
    -not $ReplicatedPropertySampleReadme.Contains('不使用 Tick 轮询')) {
    Add-Violation 'ReplicatedProperty README must preserve native RepNotify semantics and the no-polling boundary'
}
foreach ($RequiredAsyncContinuationSemanticContract in @(
    'MaximumAssetPathUtf8Bytes = 1024',
    'LoadObjectAsync',
    'ObjectPayloadKind',
    'ASCS5309',
    'ASCS5310'
)) {
    if (-not $SemanticContinuationSource.Contains($RequiredAsyncContinuationSemanticContract)) {
        Add-Violation "C# Semantic async continuation projection is missing $RequiredAsyncContinuationSemanticContract"
    }
}
foreach ($RequiredAsyncContinuationGuestContract in @(
    'ContinuationV2ExportName',
    'ObjectPayloadKind',
    'object_slot',
    'object_generation'
)) {
    if (-not $CSharpContinuationLowererSource.Contains($RequiredAsyncContinuationGuestContract)) {
        Add-Violation "C# Guest v2 continuation lowerer is missing $RequiredAsyncContinuationGuestContract"
    }
}
foreach ($RequiredControlledAsyncGuestContract in @(
    'CSharpAsyncLowerer.Lower',
    'asyncMethods.ResumeRoutes'
)) {
    if (-not $CSharpGuestLowererSource.Contains($RequiredControlledAsyncGuestContract)) {
        Add-Violation "C# Guest entry pipeline is missing controlled async contract $RequiredControlledAsyncGuestContract"
    }
}
foreach ($RequiredControlledAsyncLoweringContract in @(
    'CSharpGuestIds.AsyncResumeFunction',
    'binding_latent|',
    'FindImport(document',
    'LowerLatentArgument',
    'CSharpLatentStoragePlanner.TryBuild',
    'LowerFieldPath',
    'LowerStorageAddress',
    'continuation_delay',
    'continuation_load_object',
    'continuation_bind_cancel',
    'continuation_result_read',
    'continuation_state_store',
    'continuation_state_read',
    'EmitIncomingState',
    'EmitOutgoingState',
    'EmitIncomingResult',
    'result_accepted',
    'result_rejected',
    'ResultSlotPayloadKind',
    'OutcomeStatusField',
    'TryBuildSingleValue',
    'CancellationToken',
    'bitwise_and',
    'not_equals',
    'new GuestTerminator("trap"',
    'EmitEarlyReturnGuard',
    'guard_return',
    'branch_if',
    'CSharpAsyncControlFlowLowerer',
    'CSharpAsyncResumeRoute'
)) {
    if (-not $CSharpAsyncLowererSource.Contains($RequiredControlledAsyncLoweringContract)) {
        Add-Violation "C# Guest controlled async lowerer is missing $RequiredControlledAsyncLoweringContract"
    }
}
foreach ($RequiredStructuredAsyncLoweringContract in @(
    'CSharpAsyncControlFlowLowerer',
    'EmitIf',
    'EmitWhile',
    'EmitDoWhile',
    'EmitFor',
    'EmitLoopTransfer',
    'new GuestTerminator("branch"',
    'new GuestTerminator("branch_if"',
    'MaximumStructuredFlowNodes'
)) {
    if (-not $CSharpAsyncControlFlowLowererSource.Contains($RequiredStructuredAsyncLoweringContract) -and
        -not $CSharpSemanticInputValidatorSource.Contains($RequiredStructuredAsyncLoweringContract)) {
        Add-Violation "C# Guest structured async pipeline is missing $RequiredStructuredAsyncLoweringContract"
    }
}
foreach ($RequiredAsyncCfgLoweringContract in @(
    'CSharpAsyncCfgLowerer',
    'CSharpAsyncAbi',
    'TryLowerEntry',
    'CollectSynchronousReachable',
    'EmitIncomingState',
    'EmitIncomingResult',
    'EmitProducer',
    'EmitOutgoingState',
    'state_rejected',
    'result_rejected',
    'schedule_rejected',
    'new GuestTerminator("branch"',
    '"branch_if"'
)) {
    if (-not $CSharpAsyncCfgLowererSource.Contains($RequiredAsyncCfgLoweringContract)) {
        Add-Violation "C# Guest continuation CFG lowerer is missing $RequiredAsyncCfgLoweringContract"
    }
}
foreach ($RequiredAsyncCfgValidationContract in @(
    'ValidateAsyncControlFlowMethod',
    'AnalyzeControlFlow',
    'ContinuationCfgLowering',
    'MaximumControlFlowSegments',
    'reachable.Count != method.Segments.Count',
    '(15, "1.16") => true'
)) {
    if (-not $CSharpSemanticInputValidatorSource.Contains($RequiredAsyncCfgValidationContract)) {
        Add-Violation "C# Guest continuation CFG validator is missing $RequiredAsyncCfgValidationContract"
    }
}
foreach ($RequiredOutcomeLoweringContract in @(
    'CSharpOutcomeOperationLowerer',
    'OutcomeStatusField',
	'"Cancelled" => "3"',
    'OutcomeValueField',
    'property?.Name is not ("Status" or "Value" or "Succeeded" or "Failed" or "Cancelled")',
    '"field_load"',
    '"equals"'
)) {
    if (-not $CSharpOutcomeOperationLowererSource.Contains($RequiredOutcomeLoweringContract) -and
        -not $CSharpOperationLowererSource.Contains($RequiredOutcomeLoweringContract)) {
        Add-Violation "C# Guest outcome lowering is missing $RequiredOutcomeLoweringContract"
    }
}
if (-not $CSharpSemanticInputValidatorSource.Contains('CSharpLatentStoragePlanner.TryBuild')) {
    Add-Violation 'generated latent parameters must validate the shared public-to-storage plan'
}
foreach ($RequiredCancellationValidationContract in @(
    'awaitSite.CancellationToken',
    'AvidCancellationToken',
    'CSharpLatentStoragePlanner.TryBuildSingleValue',
    'continuation_bind_cancel'
)) {
    if (-not $CSharpSemanticInputValidatorSource.Contains($RequiredCancellationValidationContract)) {
        Add-Violation "C# Semantic input cancellation validation is missing $RequiredCancellationValidationContract"
    }
}
foreach ($RequiredAsyncGuardValidationContract in @(
    'ValidateAsyncStatement',
    'EarlyReturnGuardOperationKind',
    'schemaVersion >= 14',
    'statement.Operation.Children[0].TypeId == "type:bool"'
)) {
    if (-not $CSharpSemanticInputValidatorSource.Contains($RequiredAsyncGuardValidationContract)) {
        Add-Violation "C# Semantic input async guard validation is missing $RequiredAsyncGuardValidationContract"
    }
}
foreach ($RequiredLatentStoragePlanContract in @(
    'MaximumAggregateDepth',
    'MaximumStorageCells',
    'EnumUnderlyingTypeId',
    'nextParameter.RefKind == "in"',
    'TryCollectAggregateLeaves',
    'CSharpLatentStorageCellKind.Field',
    'CSharpLatentStorageCellKind.Address')) {
    if (-not $CSharpLatentStoragePlannerSource.Contains($RequiredLatentStoragePlanContract)) {
        Add-Violation "generated latent storage planner is missing $RequiredLatentStoragePlanContract"
    }
}
foreach ($RequiredLatentCancellationPayloadContract in @(
    'continuation_bind_cancel',
    'Pending -> Ready -> Dispatching -> Terminal',
    'IAvidScriptLatentCompletionProvider',
    '"provider_id"',
    'Session-owned result slot',
    'P57-D06-ControlledLeadership')) {
    if (-not $LatentCancellationPayloadDesign.Contains($RequiredLatentCancellationPayloadContract)) {
        Add-Violation "latent cancellation/payload design is missing $RequiredLatentCancellationPayloadContract"
    }
}
if (-not $CSharpContinuationLowererSource.Contains('IReadOnlyList<CSharpAsyncResumeRoute> asyncRoutes')) {
    Add-Violation 'C# Guest continuation v2 router must merge compiler-owned async resume routes'
}
if (-not $CSharpTypeLowererSource.Contains('IsCompilerAsyncScaffoldType')) {
    Add-Violation 'C# Guest type lowering must isolate compiler-only async awaiter scaffold types'
}
foreach ($RequiredAsyncStateTypeContract in @(
    'MaximumAsyncStateFrameBytes = 4096',
    'IsStateTypeSupported',
    'Async state frame',
    'bounded fixed-value layout'
)) {
    if (-not $CSharpTypeLowererSource.Contains($RequiredAsyncStateTypeContract)) {
        Add-Violation "C# Guest async state-frame type lowering is missing $RequiredAsyncStateTypeContract"
    }
}
if (-not $CSharpGuestDebugMapProjectorSource.Contains('BuildAsyncResumeTargets')) {
    Add-Violation 'C# Guest debug maps must project async resume functions back to source segments'
}
foreach ($RequiredAsyncContinuationRuntimeContract in @(
    'continuation_load_object',
    'avid_on_continuation_v2',
    'HandleContinuationLoadObjectImport',
    'DecodeAvidScriptUtf8ValueReference'
)) {
    if (-not $WasmRuntimeSource.Contains($RequiredAsyncContinuationRuntimeContract)) {
        Add-Violation "Runtime async continuation dispatch is missing $RequiredAsyncContinuationRuntimeContract"
    }
}
foreach ($RequiredCancellationRuntimeContract in @(
    'EAvidScriptHostBindingId::ContinuationCancelSourceCreate',
    'EAvidScriptHostBindingId::ContinuationCancelSourceCancel',
    'EAvidScriptHostBindingId::ContinuationCancelSourceRelease',
    'EAvidScriptHostBindingId::ContinuationBindCancel',
    'HandleContinuationCancelSourceCreateImport',
    'HandleContinuationBindCancelImport'
)) {
    if (-not $WasmRuntimeSource.Contains($RequiredCancellationRuntimeContract)) {
        Add-Violation "Runtime cancellation-source dispatch is missing $RequiredCancellationRuntimeContract"
    }
}
foreach ($RequiredContinuationStateRuntimeContract in @(
    'EAvidScriptHostBindingId::ContinuationStateStore',
    'EAvidScriptHostBindingId::ContinuationStateRead',
    'HandleContinuationStateStoreImport',
    'HandleContinuationStateReadImport',
    'bContinuationDispatchActive',
    '!bContinuationStateConsumed',
    'ContinuationToken == ActiveContinuationToken'
)) {
    if (-not $WasmRuntimeSource.Contains($RequiredContinuationStateRuntimeContract)) {
        Add-Violation "Runtime continuation state-frame dispatch is missing $RequiredContinuationStateRuntimeContract"
    }
}
foreach ($RequiredAsyncContinuationOwnershipContract in @(
    'MaximumRetainedLoadedObjects',
    'ScheduleObjectLoad',
    'TStrongObjectPtr<UObject>',
    'RollbackBorrowedHandles',
    'FinalizeDispatched'
)) {
    if (-not $ContinuationOwnerHeader.Contains($RequiredAsyncContinuationOwnershipContract) -and
        -not $ContinuationOwnerSource.Contains($RequiredAsyncContinuationOwnershipContract)) {
        Add-Violation "Session async continuation ownership is missing $RequiredAsyncContinuationOwnershipContract"
    }
}
foreach ($RequiredCancellationOwnershipContract in @(
    'MaximumCancellationSources = 1024',
    'MaximumCancellationBindings = 4096',
    'CancellationSourceKindMask',
    'CreateCancellationSource',
    'BindCancellationSource',
    'UnbindEntryFromCancellationSource',
	'CancelEntry(uint32 SlotIndex, bool bDeliverTerminal)',
	'Entry.LatentCompletion.ResumesOutcomeOnCancel()',
	'EAvidScriptContinuationStatus::Cancelled',
    'ReleaseCancellationSourcesForLane'
)) {
    if (-not $ContinuationOwnerHeader.Contains($RequiredCancellationOwnershipContract) -and
        -not $ContinuationOwnerSource.Contains($RequiredCancellationOwnershipContract)) {
        Add-Violation "Session cancellation-source ownership is missing $RequiredCancellationOwnershipContract"
    }
}
foreach ($RequiredContinuationResultOwnershipContract in @(
    'MaximumResultSlots = 1024',
    'MaximumFixedResultBytes = 4096',
    'ConsumeResult(',
    'ExpectedTypeId',
    'AllocateResult(',
    'ReleaseResultSlot(',
    'Entry.ResultSlot',
    'Slot.Entry->ContinuationToken == Entry.Token'
)) {
    if (-not $ContinuationOwnerHeader.Contains($RequiredContinuationResultOwnershipContract) -and
        -not $ContinuationOwnerSource.Contains($RequiredContinuationResultOwnershipContract)) {
        Add-Violation "Session continuation result ownership is missing $RequiredContinuationResultOwnershipContract"
    }
}
foreach ($RequiredContinuationStateOwnershipContract in @(
    'MaximumStateFrameBytes = 4096',
    'StoreState(',
    'ReadState(',
    'TArray<uint8, TInlineAllocator<128>> StateFrame',
    'Slot.Entry->ContinuationToken == Entry.Token',
    'Slot.Entry->StateFrame.Num() != OutStateBytes.Num()'
)) {
    if (-not $ContinuationOwnerHeader.Contains($RequiredContinuationStateOwnershipContract) -and
        -not $ContinuationOwnerSource.Contains($RequiredContinuationStateOwnershipContract)) {
        Add-Violation "Session continuation state-frame ownership is missing $RequiredContinuationStateOwnershipContract"
    }
}
foreach ($RequiredContinuationResultCodecContract in @(
    'FAvidScriptContinuationResultCodecTransaction',
    'CreatedUtf8Tokens',
    'CreatedArrayTokens',
    'EAvidScriptBindingLatentPayloadKind::AbiCells',
    'EAvidScriptBindingLatentPayloadKind::FixedWire',
    'EAvidScriptBindingLatentPayloadKind::Object',
    'EAvidScriptBindingLatentPayloadKind::Utf8',
    'EAvidScriptBindingLatentPayloadKind::Array',
    'ObjectOwnership->Borrow(',
    'Transaction.CreatedUtf8Tokens.Add(Token)',
    'Transaction.CreatedArrayTokens.Add(Token)',
    'continuation_result_descriptor_mismatch'
)) {
    if (-not $ContinuationResultCodecHeader.Contains($RequiredContinuationResultCodecContract) -and
        -not $ContinuationResultCodecSource.Contains($RequiredContinuationResultCodecContract)) {
        Add-Violation "continuation result codec is missing $RequiredContinuationResultCodecContract"
    }
}
foreach ($RequiredContinuationResultRuntimeContract in @(
    'HandleContinuationResultReadImport',
    'bContinuationResultConsumed',
    'TryGetLatentCompletionResultType',
	'ActiveContinuationStatus == EAvidScriptContinuationStatus::Cancelled',
    'HostContext.Continuations->ConsumeResult(',
    'FAvidScriptContinuationResultCodec::Encode(',
    'ResultTransaction.Rollback(Utf8ValueHeap, ArrayValueHeap)',
    'ResultTransaction.Commit()'
)) {
    if (-not $WasmRuntimeSource.Contains($RequiredContinuationResultRuntimeContract)) {
        Add-Violation "Runtime continuation result dispatch is missing $RequiredContinuationResultRuntimeContract"
    }
}
foreach ($RequiredAsyncObjectLoaderContract in @(
    'FStreamableManager',
    'RequestAsyncLoad',
    'CancelHandle'
)) {
    if (-not $AsyncObjectLoaderSource.Contains($RequiredAsyncObjectLoaderContract)) {
        Add-Violation "async object-loader adapter is missing $RequiredAsyncObjectLoaderContract"
    }
}
foreach ($RequiredAsyncContinuationVmContract in @(
    'ContinuationLoadObject',
    'continuation_load_object',
    '(ii)I'
)) {
    if (-not $StaticHostImportSource.Contains($RequiredAsyncContinuationVmContract)) {
        Add-Violation "VM async continuation catalog is missing $RequiredAsyncContinuationVmContract"
    }
}
foreach ($RequiredContinuationResultVmContract in @(
    'ContinuationResultRead',
    'continuation_result_read',
    '(iiiii)i'
)) {
    if (-not $StaticHostImportSource.Contains($RequiredContinuationResultVmContract) -and
        -not $VmHostBindingsSource.Contains($RequiredContinuationResultVmContract)) {
        Add-Violation "VM continuation result catalog is missing $RequiredContinuationResultVmContract"
    }
}
foreach ($RequiredContinuationStateVmContract in @(
    'ContinuationStateStore',
    'ContinuationStateRead',
    'continuation_state_store',
    'continuation_state_read',
    '(Iii)i'
)) {
    if (-not $StaticHostImportSource.Contains($RequiredContinuationStateVmContract) -and
        -not $VmHostBindingsSource.Contains($RequiredContinuationStateVmContract)) {
        Add-Violation "VM continuation state-frame catalog is missing $RequiredContinuationStateVmContract"
    }
}
foreach ($RequiredCancellationVmContract in @(
    'ContinuationCancelSourceCreate',
    'ContinuationCancelSourceCancel',
    'ContinuationCancelSourceRelease',
    'ContinuationBindCancel',
    'continuation_bind_cancel',
    '(II)i'
)) {
    if (-not $StaticHostImportSource.Contains($RequiredCancellationVmContract)) {
        Add-Violation "VM cancellation-source catalog is missing $RequiredCancellationVmContract"
    }
}
foreach ($RequiredCompilerManagedArrayContract in @(
    '"array_region_load"',
    '"array_region_store"')) {
    if (-not $CSharpOperationLowererSource.Contains($RequiredCompilerManagedArrayContract) -or
        -not $GuestInstructionValidatorSource.Contains($RequiredCompilerManagedArrayContract) -or
        -not $WasmFunctionCompilerSource.Contains($RequiredCompilerManagedArrayContract)) {
        Add-Violation "compiler-managed C# array regions are missing $RequiredCompilerManagedArrayContract"
    }
}
foreach ($RequiredArrayRegionImport in @(
    @{ Abi = 'avid_value_array_length'; Constant = 'GuestArrayCapabilityIntrinsics.LengthImportName' },
    @{ Abi = 'avid_value_array_read_range'; Constant = 'GuestArrayCapabilityIntrinsics.ReadRangeImportName' },
    @{ Abi = 'avid_value_array_write_range'; Constant = 'GuestArrayCapabilityIntrinsics.WriteRangeImportName' })) {
    if (-not $GuestArrayCapabilityIntrinsicsSource.Contains($RequiredArrayRegionImport.Abi) -or
        -not $CSharpGuestLowererSource.Contains($RequiredArrayRegionImport.Constant)) {
        Add-Violation "compiler-managed array region import reuse is missing $($RequiredArrayRegionImport.Abi)"
    }
}
foreach ($RequiredBoundedArrayRegionContract in @(
    'MaxArrayRegionElements = 4096',
    'MaxArrayRegionBytes = 1024 * 1024',
    'EnsureArrayRegion(body',
    'GetArrayElementStride',
    'WriteTrapWhenHostReturnsZero')) {
    if (-not $WasmFunctionCompilerSource.Contains($RequiredBoundedArrayRegionContract)) {
        Add-Violation "bounded Wasm array region compiler is missing $RequiredBoundedArrayRegionContract"
    }
}
foreach ($RequiredAvidExportContract in @(
    'AvidExportAttributeName',
    'global::AvidScript.AvidExportAttribute',
    'A callable must use either AvidExport or UnmanagedCallersOnly, not both.')) {
    if (-not $SemanticCallableProjectorSource.Contains($RequiredAvidExportContract)) {
        Add-Violation "AvidExport semantic projection is missing $RequiredAvidExportContract"
    }
}
$WasmRuntimeHeaderForTesting = Read-RequiredFile 'Source/AvidScriptRuntime/Public/AvidScriptWasmRuntime.h'
foreach ($RequiredBenchmarkExportContract in @(
    'InvokeI32PairExportHotForTesting',
    'TestingI32PairExport',
    'TestingI32PairExportName')) {
    if (-not $WasmRuntimeHeaderForTesting.Contains($RequiredBenchmarkExportContract)) {
        Add-Violation "benchmark-only prepared export path is missing $RequiredBenchmarkExportContract"
    }
}
$WasmCompileCallSlice = Get-SourceSlice `
    $WasmFunctionCompilerSource `
    'private void CompileCall(' `
    'private void CompileLocalLoad('
Test-RequiredTokenSequence `
    $WasmCompileCallSlice `
    @('FlushArrayRegion(body);', 'body.WriteByte(0x10);') `
    'Wasm calls must flush compiler-managed array regions before dispatch'
$WasmCompileReturnSlice = Get-SourceSlice `
    $WasmFunctionCompilerSource `
    'private void CompileReturn(' `
    'private void RestoreFrame('
Test-RequiredTokenSequence `
    $WasmCompileReturnSlice `
    @('FlushArrayRegion(body);', 'RestoreFrame(body);', 'body.WriteByte(0x0f);') `
    'Wasm returns must flush compiler-managed array regions before restoring the frame'
foreach ($RequiredDualPackageContract in @(
    'binding_authorization',
    'RuntimeBindingPackagePath',
    'OmitRuntimeBindingPackage',
    'ASBI4303',
    'ASBI4304',
    'ASBI4305',
    'HasActiveObjectTypeOrdinals',
    'binding_runtime_object_type_mismatch',
    'guest_object_type_provenance_invalid'
)) {
    if (-not $CSharpBuildScriptSource.Contains($RequiredDualPackageContract)) {
        Add-Violation "C# build pipeline is missing dual-package contract $RequiredDualPackageContract"
    }
}
$ObjectTypeProvenanceValidationSource = Get-SourceSlice `
    -Source $CSharpBuildScriptSource `
    -StartToken 'if ([bool]$BindingPackageInfo.HasActiveObjectTypeOrdinals)' `
    -EndToken '$DirectAbiExports = @(' `
    -Description 'C# runtime object-type provenance validation'
Test-RequiredTokenSequence `
    -Source $ObjectTypeProvenanceValidationSource `
    -Tokens @(
        '$RuntimeActiveObjectTypeOrdinals =',
        '@($BindingPackageInfo.ActiveObjectTypeOrdinals)',
        '$ObjectTypeOrdinalsMatch =',
        '$RuntimeActiveObjectTypeOrdinals.Count -eq $UsedObjectTypeOrdinals.Count',
        'for ($Index = 0; $Index -lt $UsedObjectTypeOrdinals.Count; ++$Index)',
        '[int]$RuntimeActiveObjectTypeOrdinals[$Index] -ne',
        '[int]$UsedObjectTypeOrdinals[$Index]',
        '$ObjectTypeOrdinalsMatch = $false',
        'if (-not $ObjectTypeOrdinalsMatch)',
        'Remove-LoadableArtifacts',
        'ASBI4304',
        'binding_runtime_object_type_mismatch',
        'exit 1') `
    -Description 'C# runtime object-type provenance validation'
foreach ($RequiredGuestObjectTypeExtractionContract in @(
    'avid_object_type_is_a',
    '$ConstantsByResultId.TryGetValue(',
    '$Operands.Count -eq 3',
    'direct non-negative int32 constant ordinal',
    'object_type_ref constants must contain a direct non-negative int32 ordinal'
)) {
    if (-not $CSharpBuildScriptSource.Contains(
            $RequiredGuestObjectTypeExtractionContract)) {
        Add-Violation "C# final Guest IR object-type extraction is missing $RequiredGuestObjectTypeExtractionContract"
    }
}
$GuestObjectTypeExtractionFailureSource = Get-SourceSlice `
    -Source $CSharpBuildScriptSource `
    -StartToken '$RequiredExports = @($GuestIrModel.exports' `
    -EndToken '$ObservedExports = @(' `
    -Description 'C# Guest object-type extraction failure path'
Test-RequiredTokenSequence `
    -Source $GuestObjectTypeExtractionFailureSource `
    -Tokens @(
        'Get-UsedObjectTypeOrdinals -Model $GuestIrModel',
        'catch {',
        'Remove-LoadableArtifacts',
        'ASBI4305',
        'guest_object_type_provenance_invalid',
        'exit 1') `
    -Description 'C# Guest object-type extraction failure path'
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
$BindingFastPathSource = Read-RequiredFile 'Source/AvidScriptBindings/Private/AvidScriptBindingFastPath.cpp'
$WasmRuntimeSource = Read-RequiredFile 'Source/AvidScriptRuntime/Private/AvidScriptWasmRuntime.cpp'
$VmTypedHostHeader = Read-RequiredFile 'Source/AvidScriptVM/Public/AvidScriptVmTypedHostImport.h'
$WasmtimeBackendSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWasmtimeBackend.cpp'
$WasmtimeTypedHostApiSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWasmtimeTypedHostApi.c'
$VmBackendContractSource = Read-RequiredFile 'Source/AvidScriptVM/Public/AvidScriptVmBackend.h'
$WamrBackendSource = Read-RequiredFile 'Source/AvidScriptVM/Private/AvidScriptWamrBackend.cpp'
foreach ($PreparedReflectionContract in @(
    'FAvidScriptPreparedReflectionNativeGuard',
    'FAvidScriptPreparedReflectionVectorCall',
    'FAvidScriptPreparedReflectionObjectCall',
    'FAvidScriptPreparedReflectionPropertyI32Get',
    'ImmutablePlanIdentity'
)) {
    if (-not $BindingInvocationHeader.Contains($PreparedReflectionContract)) {
        Add-Violation "prepared reflection public call cell is missing $PreparedReflectionContract"
    }
}
foreach ($PreparedShapeContract in @(
    'SelfI32PairToGuestI32',
    'SelfF32TripleToGuestVector',
    'SelfPropertyI32GetSet',
    'StableObjectRoundtrip'
)) {
    if (-not $VmTypedHostHeader.Contains($PreparedShapeContract)) {
        Add-Violation "VM prepared reflection shape is missing $PreparedShapeContract"
    }
}
$PreparedReflectionRuntimeSlice = Get-SourceSlice `
    -Source $WasmRuntimeSource `
    -StartToken 'FAvidScriptWasmRuntimeInstance::ResolvePreparedReflectionCallMode(' `
    -EndToken 'FAvidScriptWasmRuntimeInstance::InvokePreparedSelfPropertyI32Get(' `
    -Description 'prepared reflection Runtime hot path'
foreach ($RequiredPreparedRuntimeContract in @(
    'Call.Binding.I32PairCall(',
    'Call.Binding.PropertyI32Get(',
    'Call.Binding.PropertyI32Set(',
    'Call.Binding.VectorCall(',
    'Call.Binding.ObjectCall(',
    'RecordPreparedReflectionInvocation('
)) {
    if (-not $PreparedReflectionRuntimeSlice.Contains($RequiredPreparedRuntimeContract)) {
        Add-Violation "prepared reflection Runtime hot path is missing $RequiredPreparedRuntimeContract"
    }
}
foreach ($ForbiddenPreparedRuntimeLookup in @(
    'InvokePreparedReflectionI32Pair(',
    'BindingOrdinal',
    'Impl->Plans[',
    'FindByPredicate'
)) {
    if ($PreparedReflectionRuntimeSlice.Contains($ForbiddenPreparedRuntimeLookup)) {
        Add-Violation "prepared reflection Runtime hot path must not perform package or ordinal lookup: $ForbiddenPreparedRuntimeLookup"
    }
}
foreach ($RequiredPreparedBindingContract in @(
    'InvokePreparedScalarI32PairCallCell',
    'InvokePreparedVectorCallCell',
    'InvokePreparedObjectCallCell',
    'ReadAvidScriptPreparedReflectionPropertyI32',
    'WriteAvidScriptPreparedReflectionPropertyI32'
)) {
    if (-not $BindingInvocationSource.Contains($RequiredPreparedBindingContract) -and
        -not $BindingFastPathSource.Contains($RequiredPreparedBindingContract)) {
        Add-Violation "Bindings prepared reflection compiler is missing $RequiredPreparedBindingContract"
    }
}
foreach ($RequiredStructWireExecutorContract in @(
    'EValueCodecKind::StructWire',
    'BuildAvidScriptStructWireProgram',
    'SetStructValueFromGuest',
    'ResolveGuestAddress',
    'PreflightValueOutput',
    'FCodecOutputTransaction',
    'OutputTransaction.Rollback',
    'CopyCompleteValue'
)) {
    if (-not $BindingInvocationSource.Contains($RequiredStructWireExecutorContract) -and
        -not $BindingCodecProgramSource.Contains($RequiredStructWireExecutorContract) -and
        -not $BindingPreparedInvocationSource.Contains($RequiredStructWireExecutorContract)) {
        Add-Violation "Bindings schema v9 recursive struct executor is missing $RequiredStructWireExecutorContract"
    }
}
foreach ($ForbiddenStructWireExecutorSpecialCase in @(
    'RecursiveStructRoundtrip',
    'FAvidScriptBindingsRecursiveStruct',
    'RecursiveStructProperty'
)) {
    if ($BindingInvocationSource.Contains($ForbiddenStructWireExecutorSpecialCase) -or
        $BindingCodecProgramSource.Contains($ForbiddenStructWireExecutorSpecialCase) -or
        $BindingPreparedInvocationSource.Contains($ForbiddenStructWireExecutorSpecialCase)) {
        Add-Violation "Bindings schema v9 recursive struct executor must not select fixture or API names: $ForbiddenStructWireExecutorSpecialCase"
    }
}
if ($BindingCodecProgramSource.Contains('#if 0')) {
    Add-Violation 'Bindings codec production source must not retain disabled implementation blocks'
}
foreach ($RequiredValueCapabilityCatalogContract in @(
    'FAvidScriptValueCapabilityImportSpec',
    'FAvidScriptValueCapability::GetArrayImportSpecs()',
    'avidscript.value_array_length.v1',
    'avid_value_array_length',
    '(i)i',
    'avidscript.value_array_load.v1',
    'avid_value_array_load',
    '(iiii)i',
    'avidscript.value_array_store.v1',
    'avid_value_array_store',
    'avidscript.value_array_read_range.v1',
    'avid_value_array_read_range',
    '(iiiii)i',
    'avidscript.value_array_write_range.v1',
    'avid_value_array_write_range',
    'avidscript.value_release.v1',
    'avid_value_release')) {
    if (-not $ValueCapabilityHeader.Contains($RequiredValueCapabilityCatalogContract) -and
        -not $ValueCapabilitySource.Contains($RequiredValueCapabilityCatalogContract)) {
        Add-Violation "Bindings shared value capability catalog is missing $RequiredValueCapabilityCatalogContract"
    }
}
foreach ($RequiredUtf8HeapContract in @(
    'MaxValueBytes = 1024u * 1024u',
    'MaxSlots = MAX_uint16',
    'TokenToSlots',
    'FAvidScriptValueCapability::AllocateToken',
    'utf8_value_token_space_exhausted',
    'IsCanonicalUtf8'
)) {
    if (-not $Utf8ValueHeapHeader.Contains($RequiredUtf8HeapContract) -and
        -not $Utf8ValueHeapSource.Contains($RequiredUtf8HeapContract)) {
        Add-Violation "Bindings variable UTF-8 heap is missing $RequiredUtf8HeapContract"
    }
}
foreach ($RequiredUtf8CodecContract in @(
    'EValueCodecKind::Name',
    'EValueCodecKind::String',
    'ReadLinearUtf8Payload',
    'SetUtf8Value',
    'InternNextUtf8Value',
    'FPreparedValueOutput',
    'PublishValueOutput'
)) {
    if (-not $BindingCodecProgramHeader.Contains($RequiredUtf8CodecContract) -and
        -not $BindingCodecProgramSource.Contains($RequiredUtf8CodecContract) -and
        -not $BindingPreparedInvocationSource.Contains($RequiredUtf8CodecContract)) {
        Add-Violation "Bindings variable UTF-8 codec is missing $RequiredUtf8CodecContract"
    }
}
foreach ($RequiredRuntimeUtf8OwnershipContract in @(
    'FAvidScriptUtf8ValueHeap Utf8ValueHeap',
    'BindingInvocationContext.Utf8ValueHeap = &Utf8ValueHeap',
    'Utf8ValueHeap.Reset()'
)) {
    if (-not $RuntimeHeader.Contains($RequiredRuntimeUtf8OwnershipContract) -and
        -not $RuntimeSource.Contains($RequiredRuntimeUtf8OwnershipContract)) {
        Add-Violation "Runtime UTF-8 heap ownership is missing $RequiredRuntimeUtf8OwnershipContract"
    }
}
$AtomicGuestOutputSlice = Get-SourceSlice `
    -Source $BindingPreparedInvocationSource `
    -StartToken 'TArray<uint32, TInlineAllocator<16>> ParameterGuestAddresses;' `
    -EndToken '} // namespace UE::AvidScript::BindingPrivate' `
    -Description 'prepared reflected atomic guest output path'
Test-RequiredTokenSequence `
    -Source $AtomicGuestOutputSlice `
    -Tokens @(
        'PreflightValueOutput(',
        'PrepareHostEffect()',
        'Receiver.ProcessEvent(',
        'WriteValueToGuest(',
        'PublishValueOutput(',
        'OutputTransaction.Commit()') `
    -Description 'prepared reflected atomic guest output path'
if ($BindingCodecProgramSource.Contains('GuestMemory.WriteBytes(')) {
    Add-Violation 'Bindings output publication must use secured mutable ranges instead of fallible WriteBytes calls'
}
foreach ($RequiredPreparedWasmtimeContract in @(
    'SelfF32TripleToGuestVector',
    'TypedSelfF32TripleGuestVectorCallback',
    'avidscript_wasmtime_linker_define_self_f32_triple_guest_vector'
)) {
    if (-not $WasmtimeBackendSource.Contains($RequiredPreparedWasmtimeContract) -and
        -not $WasmtimeTypedHostApiSource.Contains($RequiredPreparedWasmtimeContract)) {
        Add-Violation "Wasmtime prepared reflection bridge is missing $RequiredPreparedWasmtimeContract"
    }
}
foreach ($ForbiddenVmPreparedDependency in @(
    'AvidScriptBindingInvocation',
    'FAvidScriptBindingPackage',
    'FAvidScriptWasmRuntimeInstance'
)) {
    if ($WasmtimeBackendSource.Contains($ForbiddenVmPreparedDependency) -or
        $WasmtimeTypedHostApiSource.Contains($ForbiddenVmPreparedDependency)) {
        Add-Violation "VM prepared reflection bridge crosses its ownership boundary: $ForbiddenVmPreparedDependency"
    }
}
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
foreach ($RequiredGameplayEventContract in @(
    'OnBeginOverlap',
    'OnEndOverlap',
    'OnHit',
    'OnInput',
    'ASCS5105',
    'ASCS5107',
    'exportedOwnerIds'
)) {
    if (-not $SemanticGameplayEventSource.Contains($RequiredGameplayEventContract)) {
        Add-Violation "C# Semantic gameplay event projector is missing contract $RequiredGameplayEventContract"
    }
}
foreach ($RequiredGameplayArtifactValidation in @('GameplayCallbackContracts', 'IsSupportedContract', 'entrypoint_roots')) {
    if (-not $CSharpSemanticInputValidatorSource.Contains($RequiredGameplayArtifactValidation)) {
        Add-Violation "C# semantic input validator is missing gameplay artifact contract $RequiredGameplayArtifactValidation"
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
    'FirstPrepareErrorSource',
    'TStrongObjectPtr<UObject>'
)) {
    if (-not $HostEffectTransactionHeader.Contains($RequiredHostEffectContract)) {
        Add-Violation "host effect transaction header is missing contract $RequiredHostEffectContract"
    }
}
foreach ($RequiredHostEffectBehavior in @(
    'EAvidScriptBindingReloadEffect::ActorTransform',
    'EAvidScriptBindingReloadEffect::SceneComponentTransform',
    'EAvidScriptBindingReloadEffect::ReflectedProperty',
    'FAvidScriptHostEffectTransaction::PrepareReflectedProperty',
    'Property->InitializeValue(Data)',
    'Property->CopyCompleteValue(',
    'CastField<FObjectPropertyBase>(Property)',
    'StrongObjectReferences.Emplace',
    'Property->DestroyValue(Data)',
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
$DynamicPrepareIndex = $BindingPreparedInvocation.IndexOf('InvocationContext.HostEffectJournal->PrepareEffect(')
$ReflectedPropertyPrepareIndex = $BindingPreparedInvocation.IndexOf('->PrepareReflectedProperty(')
$DynamicProcessEventIndex = $BindingPreparedInvocation.IndexOf('Receiver.ProcessEvent(Program->Function, Frame)')
if (-not $BindingPreparedInvocation.Contains('binding_reload_effect_unsupported') -or
    $DynamicPrepareIndex -lt 0 -or
    $ReflectedPropertyPrepareIndex -lt 0 -or
    $DynamicProcessEventIndex -lt 0 -or
    $DynamicPrepareIndex -ge $DynamicProcessEventIndex -or
    $ReflectedPropertyPrepareIndex -ge $DynamicProcessEventIndex) {
    Add-Violation 'dynamic binding candidate policy must reject unsupported writes and prepare reversible effects before ProcessEvent'
}
if (-not $VmBackendContractSource.Contains('IAvidScriptVmGuestMemory* GetGuestMemory()') -or
    -not $WamrBackendSource.Contains('IAvidScriptVmGuestMemory* GetGuestMemory() override')) {
    Add-Violation 'VM guest memory must be exposed through an optional backend capability implemented by WAMR'
}

$PhaseWorkflowCli = Read-RequiredFile 'Build/InvokePhaseWorkflow.ps1'
$PhaseWorkflowState = Read-RequiredFile 'Build/PhaseWorkflow/AvidScriptPhaseState.ps1'
$PhaseWorkflowEvidence = Read-RequiredFile 'Build/PhaseWorkflow/AvidScriptPhaseEvidence.ps1'
$PhaseStateSchemaText = Read-RequiredFile 'Docs/Workflow/Phase_State.schema.json'
$PhaseGateSchemaText = Read-RequiredFile 'Docs/Workflow/Phase_Gate_Evidence.schema.json'
$PhaseWorkflowContractTests = Read-RequiredFile 'Tools/AvidScript.CSharpFrontend.Tests/PhaseWorkflowContractTests.ps1'
$GitAttributes = Read-RequiredFile '.gitattributes'
foreach ($RequiredWorkflowCliContract in @(
    'PhaseWorkflow\AvidScriptPhaseState.ps1',
    'PhaseWorkflow\AvidScriptPhaseEvidence.ps1',
    "'start'",
    "'status'",
    "'freeze'",
    "'attest'",
    "'close'",
    'Write-Output "ERROR $Message"'
)) {
    if (-not $PhaseWorkflowCli.Contains($RequiredWorkflowCliContract)) {
        Add-Violation "phase workflow CLI is missing contract $RequiredWorkflowCliContract"
    }
}
$PhaseWorkflowDotSources = [regex]::Matches($PhaseWorkflowCli, '(?m)^\.\s+\(Join-Path\s+\$ScriptDirectory')
if ($PhaseWorkflowDotSources.Count -ne 2) {
    Add-Violation 'phase workflow CLI must dot-source exactly the state and evidence domain helpers'
}
if (-not $PhaseWorkflowCli.Contains('$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path')) {
    Add-Violation 'phase workflow CLI must resolve its default repository root after parameter binding'
}
foreach ($ForbiddenWorkflowDependency in @('UnrealEditor', 'Build.bat', 'RunUAT', 'BuildCookRun')) {
    if ($PhaseWorkflowCli.Contains($ForbiddenWorkflowDependency) -or
        $PhaseWorkflowState.Contains($ForbiddenWorkflowDependency) -or
        $PhaseWorkflowEvidence.Contains($ForbiddenWorkflowDependency)) {
        Add-Violation "phase workflow state/evidence layer must not invoke UE: $ForbiddenWorkflowDependency"
    }
}
foreach ($RequiredStateContract in @(
    'Write-AvidScriptPhaseStateAtomic',
    'Test-AvidScriptPhaseState',
    'Get-AvidScriptPhaseNextAction',
    'ConvertFrom-AvidScriptJson',
    "ConvertFrom-Json -DateKind String",
    'File]::Replace',
    'protected_dirty'
)) {
    if (-not $PhaseWorkflowState.Contains($RequiredStateContract)) {
        Add-Violation "phase workflow state domain is missing contract $RequiredStateContract"
    }
}
foreach ($RequiredEvidenceContract in @(
    'Test-AvidScriptGateEvidence',
    'Test-AvidScriptAttestationDiff',
    'Test-AvidScriptPhasePrivacy',
    'Invoke-AvidScriptPhaseClose',
    'attestation commit parent is not the Gate verified commit'
)) {
    if (-not $PhaseWorkflowEvidence.Contains($RequiredEvidenceContract)) {
        Add-Violation "phase workflow evidence domain is missing contract $RequiredEvidenceContract"
    }
}
if (-not $PhaseWorkflowContractTests.Contains('Evidence.ValidAttestAndClose') -or
    -not $PhaseWorkflowContractTests.Contains('Evidence.AttestationSourceChangeRejected') -or
    -not $PhaseWorkflowContractTests.Contains('Transitions.ProtectedDirtyBaseline') -or
    -not $PhaseWorkflowContractTests.Contains('State.PwshPreservesTimestampStrings')) {
    Add-Violation 'phase workflow contract tests do not cover close, source rejection, protected dirty, and cross-shell timestamp behavior'
}
foreach ($RequiredPhaseEolContract in @(
    'Docs/Phase*/Phase*_State.json text eol=lf',
    'Docs/Phase*/P*_Gate_Summary.json text eol=lf'
)) {
    if (-not $GitAttributes.Contains($RequiredPhaseEolContract)) {
        Add-Violation "phase workflow Git attributes are missing $RequiredPhaseEolContract"
    }
}
try {
    $PhaseStateSchema = $PhaseStateSchemaText | ConvertFrom-Json
    $PhaseGateSchema = $PhaseGateSchemaText | ConvertFrom-Json
    if ([int]$PhaseStateSchema.properties.schema_version.const -ne 1 -or
        [int]$PhaseGateSchema.properties.schema_version.const -ne 1) {
        Add-Violation 'phase workflow schemas must publish schema version 1'
    }
    if ([string]$PhaseStateSchema.title -cne 'AvidScript Phase State v1' -or
        [string]$PhaseGateSchema.title -cne 'AvidScript Phase Gate Evidence v1') {
        Add-Violation 'phase workflow schema titles differ from the v1 contract'
    }
}
catch {
    Add-Violation 'phase workflow schemas must be valid JSON'
}
$PhaseStateFiles = [System.IO.Directory]::GetFiles(
    (Join-Path $PluginRoot 'Docs'),
    'Phase*_State.json',
    [System.IO.SearchOption]::AllDirectories)
foreach ($PhaseStateFile in $PhaseStateFiles) {
    $PhaseStateText = [System.IO.File]::ReadAllText($PhaseStateFile)
    if ($PhaseStateText -match '(?i)[A-Z]:[\\/]+Users[\\/]+|-----BEGIN [A-Z ]*PRIVATE KEY-----') {
        Add-Violation 'tracked phase state contains a private account path or private key marker'
    }
    try {
        $PhaseStateJson = $PhaseStateText | ConvertFrom-Json
        if ([int]$PhaseStateJson.schema_version -ne 1) {
            Add-Violation 'tracked phase state does not use schema version 1'
        }
    }
    catch {
        Add-Violation 'tracked phase state is not valid JSON'
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

$EvidenceCommitOutput = @(& git -C $PluginRoot rev-parse HEAD 2>&1)
if ($LASTEXITCODE -ne 0) {
    Add-Violation 'architecture evidence requires a readable Git HEAD commit'
    $EvidenceCommit = 'unavailable'
}
else {
    $EvidenceCommit = ($EvidenceCommitOutput -join '').Trim()
}
$EvidenceTreeOutput = @(& git -C $PluginRoot rev-parse 'HEAD^{tree}' 2>&1)
if ($LASTEXITCODE -ne 0) {
    Add-Violation 'architecture evidence requires a readable Git HEAD tree'
    $EvidenceTree = 'unavailable'
}
else {
    $EvidenceTree = ($EvidenceTreeOutput -join '').Trim()
}
$TrackedDirtyOutput = @(& git -C $PluginRoot diff --name-only HEAD -- 2>&1)
if ($LASTEXITCODE -ne 0) {
    Add-Violation 'architecture evidence could not inspect tracked worktree changes'
    $TrackedDirtyOutput = @()
}
$UntrackedOutput = @(& git -C $PluginRoot ls-files --others --exclude-standard 2>&1)
if ($LASTEXITCODE -ne 0) {
    Add-Violation 'architecture evidence could not inspect untracked worktree inputs'
    $UntrackedOutput = @()
}
$DirtyArchitectureInputs = [System.Collections.Generic.List[string]]::new()
foreach ($DirtyPath in @($TrackedDirtyOutput) + @($UntrackedOutput)) {
    $NormalizedDirtyPath = ([string]$DirtyPath).Trim().Replace('\', '/')
    if ($ArchitectureEvidencePaths.Contains($NormalizedDirtyPath)) {
        $DirtyArchitectureInputs.Add($NormalizedDirtyPath)
    }
}
if ($DirtyArchitectureInputs.Count -gt 0) {
    $DirtyEvidenceViolation = 'architecture evidence inputs differ from the reported Git tree: ' `
        + ($DirtyArchitectureInputs -join ', ')
    Add-Violation $DirtyEvidenceViolation
}
$CheckerSha256Algorithm = [System.Security.Cryptography.SHA256]::Create()
try {
    $CheckerSha256 = [System.BitConverter]::ToString(
        $CheckerSha256Algorithm.ComputeHash(
            [System.IO.File]::ReadAllBytes($ArchitectureScriptPath))).Replace('-', '').ToLowerInvariant()
}
finally {
    $CheckerSha256Algorithm.Dispose()
}

Write-Host "Evidence commit: $EvidenceCommit"
Write-Host "Evidence tree: $EvidenceTree"
Write-Host "Evidence checker SHA-256: $CheckerSha256"
$EvidenceInputState = if ($DirtyArchitectureInputs.Count -eq 0) { 'clean' } else { 'dirty' }
Write-Host "Evidence architecture inputs: $EvidenceInputState"

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
