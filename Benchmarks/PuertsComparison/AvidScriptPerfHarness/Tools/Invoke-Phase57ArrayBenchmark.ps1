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
    'avidscript_wasmtime_bulk'
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
if ([string]$profile.contract -cne 'phase57_array_profile.v1' -or
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

$projectRoot = Split-Path -Parent $resolvedProject
$puertsRuntimePath = Join-Path $projectRoot 'Plugins\Puerts\Binaries\Win64\UnrealEditor-JsEnv.dll'
$scriptPath = Join-Path $harnessRoot 'Content\JavaScript\phase57_array_reflection.js'
foreach ($requiredFile in @($puertsRuntimePath, $scriptPath, $resolvedWasm)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "ASP57A3008 required benchmark artifact is missing: $requiredFile"
    }
}

$profileSha256 = (Get-FileHash -LiteralPath $resolvedProfile -Algorithm SHA256).Hash.ToLowerInvariant()
$scriptSha256 = (Get-FileHash -LiteralPath $scriptPath -Algorithm SHA256).Hash.ToLowerInvariant()
$puertsRuntimeSha256 = (Get-FileHash -LiteralPath $puertsRuntimePath -Algorithm SHA256).Hash.ToLowerInvariant()
$wasmSha256 = (Get-FileHash -LiteralPath $resolvedWasm -Algorithm SHA256).Hash.ToLowerInvariant()
$processResultPaths = @()

for ($processRun = 1; $processRun -le [int]$profile.process_runs; ++$processRun) {
    $rotation = ($processRun - 1) % $canonicalLanes.Count
    $laneOrder = @($canonicalLanes[$rotation..($canonicalLanes.Count - 1)])
    if ($rotation -gt 0) {
        $laneOrder += @($canonicalLanes[0..($rotation - 1)])
    }
    $request = [ordered]@{
        contract = 'phase57_array_request.v1'
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
