[CmdletBinding()]
param(
    [ValidateSet('Verify', 'Write')]
    [string]$Mode = 'Verify',

    [Parameter(Mandatory = $true)]
    [string]$WatCompilerModuleRoot,

    [Parameter(Mandatory = $true)]
    [string]$PythonExecutable
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ControlledRoot = Split-Path -Parent $ScriptRoot
$KernelRoot = Join-Path $ControlledRoot 'Kernel'
$CompilerRoot = [System.IO.Path]::GetFullPath($WatCompilerModuleRoot)
$ResolvedPython = (Resolve-Path -LiteralPath $PythonExecutable).Path

if (-not (Test-Path -LiteralPath (Join-Path $CompilerRoot 'wasmtime/__init__.py') -PathType Leaf)) {
    throw "ASP54K1003 Wasmtime Python module root is invalid: $CompilerRoot"
}

function Get-NormalizedWatSha256 {
    param([string]$WatText)
    $NormalizedWat = $WatText.Replace("`r`n", "`n").Replace("`r", "`n")
    $Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    return [Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData(
            $Utf8NoBom.GetBytes($NormalizedWat))).ToLowerInvariant()
}

$ContractPaths = @(
    'controlled_runtime_kernel.contract.json',
    'crossing_cost_kernel.contract.json'
) | ForEach-Object { Get-Item -LiteralPath (Join-Path $KernelRoot $_) }
if ($ContractPaths.Count -ne 2) {
    throw 'ASP54K1001 both controlled kernel contracts are required'
}

$PreviousPythonPath = $env:PYTHONPATH
$env:PYTHONPATH = $CompilerRoot
try {
    $CompilerProbe = @'
import base64
import importlib.metadata
import pathlib
import sys
import wasmtime

version = importlib.metadata.version("wasmtime")
wat = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
print(version)
print(base64.b64encode(wasmtime.wat2wasm(wat)).decode("ascii"))
'@

    $Results = @()
    foreach ($ContractPath in $ContractPaths) {
        $Contract = Get-Content -LiteralPath $ContractPath.FullName -Raw | ConvertFrom-Json
        if ([int]$Contract.schema_version -ne 1 -or
            [string]$Contract.compiler.package -cne 'wasmtime' -or
            [string]$Contract.compiler.version -cne '45.0.0' -or
            [string]$Contract.compiler.entrypoint -cne 'wasmtime.wat2wasm') {
            throw "ASP54K1002 invalid compiler identity in $($ContractPath.Name)"
        }

        $WatPath = Join-Path $KernelRoot ([string]$Contract.wat_path)
        $WasmPath = Join-Path $KernelRoot ([string]$Contract.wasm_path)
        if (-not (Test-Path -LiteralPath $WatPath -PathType Leaf)) {
            throw "ASP54K1004 tracked WAT is missing: $WatPath"
        }

        $WatDigest = Get-NormalizedWatSha256 -WatText ([System.IO.File]::ReadAllText($WatPath))
        if ($Contract.PSObject.Properties.Name -contains 'wat_normalized_sha256' -and
            $WatDigest -cne [string]$Contract.wat_normalized_sha256) {
            throw "ASP54K1005 WAT digest mismatch for $($Contract.kernel_id)"
        }

        $CompilerOutput = @(& $ResolvedPython -c $CompilerProbe $WatPath)
        $CompilerExitCode = $LASTEXITCODE
        if ($CompilerExitCode -ne 0 -or $CompilerOutput.Count -ne 2) {
            throw "ASP54K1006 wat2wasm failed for $($Contract.kernel_id) with exit code $CompilerExitCode"
        }
        if ([string]$CompilerOutput[0] -cne [string]$Contract.compiler.version) {
            throw "ASP54K1007 compiler version mismatch for $($Contract.kernel_id)"
        }

        $GeneratedBytes = [Convert]::FromBase64String([string]$CompilerOutput[1])
        $GeneratedDigest = [Convert]::ToHexString(
            [System.Security.Cryptography.SHA256]::HashData($GeneratedBytes)).ToLowerInvariant()
        $TracksWasm = -not ($Contract.PSObject.Properties.Name -contains 'tracked_wasm') -or [bool]$Contract.tracked_wasm
        if ($TracksWasm -and $GeneratedDigest -cne [string]$Contract.wasm_sha256) {
            throw "ASP54K1008 generated WASM digest mismatch for $($Contract.kernel_id)"
        }

        if ($Mode -ceq 'Write') {
            [System.IO.File]::WriteAllBytes($WasmPath, $GeneratedBytes)
        }
        elseif ($TracksWasm) {
            if (-not (Test-Path -LiteralPath $WasmPath -PathType Leaf)) {
                throw "ASP54K1009 tracked WASM is missing: $WasmPath"
            }
            $TrackedBytes = [System.IO.File]::ReadAllBytes($WasmPath)
            if (-not [System.Linq.Enumerable]::SequenceEqual([byte[]]$GeneratedBytes, [byte[]]$TrackedBytes)) {
                throw "ASP54K1010 tracked WASM bytes differ for $($Contract.kernel_id)"
            }
        }

        $Results += [pscustomobject]@{
            kernel_id = [string]$Contract.kernel_id
            wat_sha256 = $WatDigest
            wasm_sha256 = $GeneratedDigest
            wasm_size = $GeneratedBytes.Length
            tracked_wasm = $TracksWasm
        }
    }
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}

[pscustomobject]@{
    result = 'controlled_runtime_kernels_verified'
    mode = $Mode
    kernel_count = $Results.Count
    python_executable_sha256 =
        (Get-FileHash -LiteralPath $ResolvedPython -Algorithm SHA256).
            Hash.ToLowerInvariant()
    kernels = $Results
}
