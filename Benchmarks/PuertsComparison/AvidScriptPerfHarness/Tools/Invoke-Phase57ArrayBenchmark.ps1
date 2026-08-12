[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EditorExecutable,

    [Parameter(Mandatory = $true)]
    [string]$ProjectPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$ProfilePath = '',

    [string]$WasmPath = '',

    [string]$CompilerWasmPath = '',

    [string]$DotNetPath = '',

    [string]$WatCompilerModuleRoot = '',

    [string]$PythonExecutable = '',

    [switch]$RequireInternalGate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$harnessRoot = Split-Path -Parent $PSScriptRoot
$comparisonRoot = Split-Path -Parent $harnessRoot
$profileRoot = Join-Path $comparisonRoot 'Profiles'
$expectedSizes = @(1, 4, 16, 64, 256, 1024)
$canonicalLanes = @(
    'puerts_v8_reflection_tarray',
    'avidscript_wasmtime_element',
    'avidscript_wasmtime_bulk',
    'avidscript_wasmtime_compiler_region'
)

if ([string]::IsNullOrWhiteSpace($ProfilePath)) {
    $ProfilePath = Join-Path $profileRoot 'Phase57Array.diagnostic.json'
}
$resolvedEditor = (Resolve-Path -LiteralPath $EditorExecutable).Path
$resolvedProject = (Resolve-Path -LiteralPath $ProjectPath).Path
$resolvedProfile = (Resolve-Path -LiteralPath $ProfilePath).Path
$resolvedOutput = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($resolvedOutput) | Out-Null

if ([IO.Path]::GetExtension($resolvedProject) -cne '.uproject') {
    throw 'ASP57A3001 ProjectPath must resolve to a .uproject file.'
}
$profile = Get-Content -LiteralPath $resolvedProfile -Raw | ConvertFrom-Json -Depth 100
if ([string]$profile.contract -cne 'phase57_array_profile.v2' -or
    @($profile.sizes).Count -ne $expectedSizes.Count) {
    throw 'ASP57A3002 invalid Phase57Array profile contract.'
}
for ($index = 0; $index -lt $expectedSizes.Count; ++$index) {
    if ([int]$profile.sizes[$index] -ne $expectedSizes[$index]) {
        throw 'ASP57A3003 Phase57Array profile size order is not frozen.'
    }
}
if ([string]$profile.alternate_js_representations.arraybuffer -cne 'diagnostic_only' -or
    [string]$profile.alternate_js_representations.typedarray -cne 'diagnostic_only') {
    throw 'ASP57A3004 ArrayBuffer and TypedArray may only be diagnostic.'
}

if ([string]::IsNullOrWhiteSpace($WasmPath)) {
    if ([string]::IsNullOrWhiteSpace($WatCompilerModuleRoot) -or
        [string]::IsNullOrWhiteSpace($PythonExecutable)) {
        throw 'ASP57A3005 provide WasmPath or both WatCompilerModuleRoot and PythonExecutable.'
    }
    $compilerRoot = (Resolve-Path -LiteralPath $WatCompilerModuleRoot).Path
    $resolvedPython = (Resolve-Path -LiteralPath $PythonExecutable).Path
    if (-not (Test-Path -LiteralPath (Join-Path $compilerRoot 'wasmtime\__init__.py') -PathType Leaf)) {
        throw 'ASP57A3006 WatCompilerModuleRoot does not contain wasmtime/__init__.py.'
    }
    $watPath = Join-Path $harnessRoot 'Content\Wasm\phase57_array_kernel.wat'
    $WasmPath = Join-Path $resolvedOutput 'phase57_array_kernel.wasm'
    $probe = @'
import base64
import pathlib
import sys
import wasmtime
print(base64.b64encode(wasmtime.wat2wasm(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))).decode("ascii"))
'@
    $previousPythonPath = $env:PYTHONPATH
    $env:PYTHONPATH = $compilerRoot
    try {
        $encoded = & $resolvedPython -c $probe $watPath
        if ($LASTEXITCODE -ne 0 -or @($encoded).Count -ne 1) {
            throw 'ASP57A3007 wasmtime.wat2wasm failed.'
        }
        [IO.File]::WriteAllBytes($WasmPath, [Convert]::FromBase64String([string]$encoded))
    }
    finally {
        $env:PYTHONPATH = $previousPythonPath
    }
}
$resolvedWasm = (Resolve-Path -LiteralPath $WasmPath).Path

if ([string]::IsNullOrWhiteSpace($CompilerWasmPath)) {
    if ([string]::IsNullOrWhiteSpace($DotNetPath)) {
        throw 'ASP57A3011 provide CompilerWasmPath or DotNetPath.'
    }
    $resolvedDotNet = (Resolve-Path -LiteralPath $DotNetPath).Path
    $pluginRoot = Split-Path -Parent (Split-Path -Parent $comparisonRoot)
    $sourcePath = Join-Path $harnessRoot 'Content\CSharp\phase57_array_compiler_region.cs'
    $referencePath = Join-Path $harnessRoot 'Content\CSharp\phase57_array_reference.cs'
    $compilerRoot = Join-Path $resolvedOutput 'compiler-managed'
    [IO.Directory]::CreateDirectory($compilerRoot) | Out-Null
    $frontendPath = Join-Path $compilerRoot 'frontend.json'
    $semanticPath = Join-Path $compilerRoot 'semantic.json'
    $guestIrPath = Join-Path $compilerRoot 'guest-ir.json'
    $debugMapPath = Join-Path $compilerRoot 'debug-map.json'
    $stateSchemaPath = Join-Path $compilerRoot 'state-schema.json'
    $CompilerWasmPath = Join-Path $compilerRoot 'phase57_array_compiler_region.wasm'
    $inspectionPath = Join-Path $compilerRoot 'wasm-inspection.json'
    & (Join-Path $pluginRoot 'Build\InvokeCSharpFrontend.ps1') `
        -DotNetPath $resolvedDotNet `
        -SourcePath $sourcePath `
        -SourceId 'Benchmarks/Phase57ArrayCompilerRegion.cs' `
        -OutputPath $frontendPath
    if ($LASTEXITCODE -ne 0) { throw 'ASP57A3012 C# frontend compilation failed.' }
    & (Join-Path $pluginRoot 'Build\InvokeCSharpSemantic.ps1') `
        -DotNetPath $resolvedDotNet `
        -SourcePath $sourcePath `
        -SourceId 'Benchmarks/Phase57ArrayCompilerRegion.cs' `
        -FrontendPath $frontendPath `
        -OutputPath $semanticPath `
        -ExecutableReferenceSourcePath $referencePath
    if ($LASTEXITCODE -ne 0) { throw 'ASP57A3013 C# semantic compilation failed.' }
    $frontendSha256 = (Get-FileHash -LiteralPath $frontendPath -Algorithm SHA256).Hash.ToLowerInvariant()
    & (Join-Path $pluginRoot 'Build\InvokeCSharpGuestCompiler.ps1') `
        -DotNetPath $resolvedDotNet `
        -SemanticPath $semanticPath `
        -FrontendArtifactSha256 $frontendSha256 `
        -GuestIrPath $guestIrPath `
        -DebugMapPath $debugMapPath `
        -StateSchemaPath $stateSchemaPath `
        -WasmPath $CompilerWasmPath `
        -InspectionPath $inspectionPath
    if ($LASTEXITCODE -ne 0) { throw 'ASP57A3014 C# Guest IR/Wasm compilation failed.' }
}
$resolvedCompilerWasm = (Resolve-Path -LiteralPath $CompilerWasmPath).Path
$compilerSourcePath = Join-Path $harnessRoot 'Content\CSharp\phase57_array_compiler_region.cs'
$compilerReferencePath = Join-Path $harnessRoot 'Content\CSharp\phase57_array_reference.cs'
if (-not (Get-Variable -Name guestIrPath -ErrorAction SilentlyContinue)) {
    throw 'ASP57A3015 direct CompilerWasmPath is diagnostic-only because formal compiler provenance requires Guest IR.'
}
$compilerGuestIr = Get-Content -LiteralPath $guestIrPath -Raw | ConvertFrom-Json -Depth 100
$compilerInspection = Get-Content -LiteralPath $inspectionPath -Raw | ConvertFrom-Json -Depth 100
$requiredRegionOps = @($compilerGuestIr.functions.blocks.instructions.op | Where-Object {
    $_ -in @('array_region_load', 'array_region_store')
})
$actualCompilerImports = @($compilerInspection.imports.name | Sort-Object)
$expectedCompilerImports = @(
    'avid_value_array_length',
    'avid_value_array_read_range',
    'avid_value_array_write_range'
)
if ($requiredRegionOps.Count -lt 2 -or
    (@($actualCompilerImports) -join ',') -cne (@($expectedCompilerImports | Sort-Object) -join ',') -or
    'phase57_array_run' -notin @($compilerInspection.exports.name)) {
    throw 'ASP57A3016 compiler-managed artifact contract mismatch.'
}

$projectRoot = Split-Path -Parent $resolvedProject
$puertsRuntimePath = Join-Path $projectRoot 'Plugins\Puerts\Binaries\Win64\UnrealEditor-JsEnv.dll'
$scriptPath = Join-Path $harnessRoot 'Content\JavaScript\phase57_array_reflection.js'
foreach ($requiredFile in @($puertsRuntimePath, $scriptPath, $resolvedWasm, $resolvedCompilerWasm)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "ASP57A3008 required benchmark artifact is missing: $requiredFile"
    }
}

$profileSha256 = (Get-FileHash -LiteralPath $resolvedProfile -Algorithm SHA256).Hash.ToLowerInvariant()
$scriptSha256 = (Get-FileHash -LiteralPath $scriptPath -Algorithm SHA256).Hash.ToLowerInvariant()
$puertsRuntimeSha256 = (Get-FileHash -LiteralPath $puertsRuntimePath -Algorithm SHA256).Hash.ToLowerInvariant()
$wasmSha256 = (Get-FileHash -LiteralPath $resolvedWasm -Algorithm SHA256).Hash.ToLowerInvariant()
$compilerWasmSha256 = (Get-FileHash -LiteralPath $resolvedCompilerWasm -Algorithm SHA256).Hash.ToLowerInvariant()
$compilerSourceSha256 = (Get-FileHash -LiteralPath $compilerSourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
$compilerReferenceSha256 = (Get-FileHash -LiteralPath $compilerReferencePath -Algorithm SHA256).Hash.ToLowerInvariant()
$compilerGuestIrSha256 = (Get-FileHash -LiteralPath $guestIrPath -Algorithm SHA256).Hash.ToLowerInvariant()
$compilerInspectionSha256 = (Get-FileHash -LiteralPath $inspectionPath -Algorithm SHA256).Hash.ToLowerInvariant()
$processResultPaths = @()

for ($processRun = 1; $processRun -le [int]$profile.process_runs; ++$processRun) {
    $rotation = ($processRun - 1) % $canonicalLanes.Count
    $laneOrder = @($canonicalLanes[$rotation..($canonicalLanes.Count - 1)])
    if ($rotation -gt 0) {
        $laneOrder += @($canonicalLanes[0..($rotation - 1)])
    }
    $request = [ordered]@{
        contract = 'phase57_array_request.v2'
        profile_id = [string]$profile.profile_id
        profile_sha256 = $profileSha256
        measurement_level = [string]$profile.measurement_level
        process_run = $processRun
        warmup_samples = [int]$profile.warmup_samples
        timed_samples = [int]$profile.timed_samples
        seed = [int]$profile.seed
        sizes = $expectedSizes
        logical_calls_by_size = $profile.logical_calls_by_size
        lane_order = $laneOrder
        puerts_script_sha256 = $scriptSha256
        puerts_runtime_sha256 = $puertsRuntimeSha256
        avidscript_wasm_path = $resolvedWasm
        avidscript_wasm_sha256 = $wasmSha256
        avidscript_compiler_wasm_path = $resolvedCompilerWasm
        avidscript_compiler_wasm_sha256 = $compilerWasmSha256
        avidscript_compiler_source_sha256 = $compilerSourceSha256
        avidscript_compiler_reference_sha256 = $compilerReferenceSha256
        avidscript_compiler_guest_ir_sha256 = $compilerGuestIrSha256
        avidscript_compiler_inspection_sha256 = $compilerInspectionSha256
    }
    $requestPath = Join-Path $resolvedOutput ("process-{0:D2}.request.json" -f $processRun)
    $processResultPath = Join-Path $resolvedOutput ("process-{0:D2}.result.json" -f $processRun)
    if ((Test-Path -LiteralPath $requestPath) -or
        (Test-Path -LiteralPath $processResultPath)) {
        throw "ASP57A3009 refusing to overwrite process sidecars for run $processRun."
    }
    [IO.File]::WriteAllText(
        $requestPath,
        ($request | ConvertTo-Json -Depth 100) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))

    $arguments = @(
        $resolvedProject,
        '-run=AvidScriptPerfRun',
        '-unattended',
        '-nop4',
        '-nosplash',
        '-nullrhi',
        '-EnablePlugins=AvidScriptPerfHarness',
        "-AvidScriptPerfArrayRequest=$requestPath",
        "-AvidScriptPerfArrayResult=$processResultPath"
    )
    & $resolvedEditor @arguments
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $processResultPath -PathType Leaf)) {
        throw "ASP57A3010 Unreal process run $processRun failed with exit code $LASTEXITCODE."
    }
    $processResultPaths += $processResultPath
}

$resultPath = Join-Path $resolvedOutput 'Phase57Array.result.json'
$evaluate = Join-Path $PSScriptRoot 'Evaluate-Phase57ArrayPerformance.ps1'
$evaluateArguments = @{
    ProfilePath = $resolvedProfile
    ProcessResultPaths = $processResultPaths
    ResultPath = $resultPath
    RequireInternalGate = $RequireInternalGate
}
& $evaluate @evaluateArguments
