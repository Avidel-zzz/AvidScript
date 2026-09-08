[CmdletBinding()]
param(
    [string[]]$KernelIds = @(
        'scalar_float',
        'simd128',
        'mixed_gameplay',
        'data_branch'
    ),
    [ValidateRange(1, 1000)]
    [int]$TimedSamples = 30,
    [ValidateRange(1, 1000)]
    [int]$WarmupSamples = 5,
    [ValidateRange(0.1, 60000.0)]
    [double]$MinimumSampleMilliseconds = 5.0,
    [string]$OutputPath,
    [string]$BuildDirectory = 'C:\tmp\AvidScript\NativeProfileProbe'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$pluginRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..\..\..\..'))
$lockPath = Join-Path $pluginRoot (
    'Source\ThirdParty\Wasmtime\PerformanceToolchain\' +
    'WasmtimePerformanceToolchain.lock.json')
$lock = Get-Content -Raw -LiteralPath $lockPath | ConvertFrom-Json
$wasmtimeInstall = Join-Path $pluginRoot (
    [string]$lock.install.relative_path -replace '/', '\')
$wasmtimeDll = Join-Path $wasmtimeInstall 'lib\wasmtime.dll'
$wasmtimeImportLibrary = Join-Path $wasmtimeInstall 'lib\wasmtime.dll.lib'

foreach ($requiredPath in @($wasmtimeDll, $wasmtimeImportLibrary)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Missing locked Wasmtime artifact: $requiredPath"
    }
}

$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
[System.IO.Directory]::CreateDirectory($BuildDirectory) | Out-Null

$configureArguments = @(
    '-S', $PSScriptRoot,
    '-B', $BuildDirectory,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    "-DAVIDSCRIPT_ROOT=$pluginRoot",
    "-DAVIDSCRIPT_WASMTIME_INSTALL=$wasmtimeInstall"
)
& $cmake @configureArguments
if ($LASTEXITCODE -ne 0) {
    throw "Native profile probe configure failed with exit code $LASTEXITCODE"
}

& $cmake --build $BuildDirectory --config Release
if ($LASTEXITCODE -ne 0) {
    throw "Native profile probe build failed with exit code $LASTEXITCODE"
}

$executable = Join-Path $BuildDirectory (
    'Release\AvidScriptWasmtimeNativeProfileProbe.exe')
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Native profile probe executable is missing: $executable"
}

$kernelRoot = Join-Path $pluginRoot (
    'Benchmarks\PuertsComparison\ControlledRuntime\Kernel')
$kernelResults = @()
foreach ($kernelId in $KernelIds) {
    if ($kernelId -notmatch '^[a-z0-9_]+$') {
        throw "Invalid kernel id: $kernelId"
    }
    $kernelPath = Join-Path $kernelRoot "suite_$kernelId.wasm"
    if (-not (Test-Path -LiteralPath $kernelPath -PathType Leaf)) {
        throw "Kernel is missing: $kernelPath"
    }

    $probeArguments = @(
        '--kernel', $kernelPath,
        '--samples', $TimedSamples,
        '--warmup', $WarmupSamples,
        '--minimum-sample-ms', $MinimumSampleMilliseconds
    )
    $rawResultLines = & $executable @probeArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Native profile probe failed for $kernelId with exit code $LASTEXITCODE"
    }
    $parsedResult = ($rawResultLines -join [Environment]::NewLine) |
        ConvertFrom-Json
    $parsedResult | Add-Member -NotePropertyName kernel_id -NotePropertyValue $kernelId
    $parsedResult | Add-Member -NotePropertyName kernel_sha256 -NotePropertyValue (
        (Get-FileHash -Algorithm SHA256 -LiteralPath $kernelPath).Hash.ToLowerInvariant())
    $kernelResults += $parsedResult
}

$gitCommit = (& git -C $pluginRoot rev-parse HEAD).Trim()
$gitTree = (& git -C $pluginRoot rev-parse 'HEAD^{tree}').Trim()
$dirtyPaths = @(& git -C $pluginRoot status --porcelain=v1)
$result = [ordered]@{
    schema_version = 1
    evidence_class = 'diagnostic_attribution'
    generated_utc = [DateTime]::UtcNow.ToString('o')
    git_commit = $gitCommit
    git_tree = $gitTree
    repository_clean = $dirtyPaths.Count -eq 0
    dirty_path_count = $dirtyPaths.Count
    toolchain_id = [string]$lock.toolchain_id
    toolchain_lock_sha256 = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $lockPath
    ).Hash.ToLowerInvariant()
    wasmtime_dll_sha256 = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $wasmtimeDll
    ).Hash.ToLowerInvariant()
    timed_samples = $TimedSamples
    warmup_samples = $WarmupSamples
    minimum_sample_milliseconds = $MinimumSampleMilliseconds
    kernels = $kernelResults
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
    $OutputPath = "C:\tmp\AvidScript\P65D\d31-native-profile-probe-$stamp.json"
}
$resolvedOutputPath = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory(
    [System.IO.Path]::GetDirectoryName($resolvedOutputPath)) | Out-Null
$json = $result | ConvertTo-Json -Depth 12
[System.IO.File]::WriteAllText(
    $resolvedOutputPath,
    $json + [Environment]::NewLine,
    [System.Text.UTF8Encoding]::new($false))

Write-Output $resolvedOutputPath
