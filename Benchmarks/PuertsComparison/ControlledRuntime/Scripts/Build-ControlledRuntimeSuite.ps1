[CmdletBinding()]
param(
    [ValidateSet('Verify', 'Write')]
    [string]$Mode = 'Verify',

    [Parameter(Mandatory = $true)]
    [string]$WatCompilerModuleRoot,

    [Parameter(Mandatory = $true)]
    [string]$PythonExecutable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$controlledRoot = Split-Path -Parent $PSScriptRoot
$kernelRoot = Join-Path $controlledRoot 'Kernel'
$contractPath = Join-Path $kernelRoot 'phase54_suite.contract.json'
$compilerRoot = [IO.Path]::GetFullPath($WatCompilerModuleRoot)
$resolvedPython = (Resolve-Path -LiteralPath $PythonExecutable).Path

if (-not (Test-Path -LiteralPath (Join-Path $compilerRoot 'wasmtime\__init__.py') -PathType Leaf)) {
    throw "ASP54K2001 Wasmtime Python module root is invalid: $compilerRoot"
}

function Get-NormalizedWatSha256 {
    param([string]$Text)

    $normalized = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    return [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData(
            [Text.UTF8Encoding]::new($false).GetBytes($normalized))).ToLowerInvariant()
}

$contract = Get-Content -LiteralPath $contractPath -Raw | ConvertFrom-Json -Depth 100
if ([int]$contract.schema_version -ne 1 -or
    [string]$contract.compiler.package -cne 'wasmtime' -or
    [string]$contract.compiler.version -cne '45.0.0' -or
    @($contract.kernels).Count -ne 12) {
    throw 'ASP54K2002 Phase54 suite contract is invalid.'
}

$probe = @'
import base64
import importlib.metadata
import pathlib
import sys
import wasmtime
print(importlib.metadata.version("wasmtime"))
text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
print(base64.b64encode(wasmtime.wat2wasm(text)).decode("ascii"))
'@
$previousPythonPath = $env:PYTHONPATH
$env:PYTHONPATH = $compilerRoot
try {
    $results = @()
    foreach ($kernel in @($contract.kernels)) {
        $watPath = Join-Path $kernelRoot ([string]$kernel.wat_path)
        $wasmPath = Join-Path $kernelRoot ([string]$kernel.wasm_path)
        $watText = [IO.File]::ReadAllText($watPath)
        $watSha256 = Get-NormalizedWatSha256 -Text $watText
        if ($watSha256 -cne [string]$kernel.wat_normalized_sha256) {
            throw "ASP54K2003 WAT identity mismatch: $($kernel.kernel_id)"
        }

        $compilerOutput = @(& $resolvedPython -c $probe $watPath)
        if ($LASTEXITCODE -ne 0 -or
            $compilerOutput.Count -ne 2 -or
            [string]$compilerOutput[0] -cne '45.0.0') {
            throw "ASP54K2004 pinned compiler failed: $($kernel.kernel_id)"
        }
        $wasmBytes = [Convert]::FromBase64String([string]$compilerOutput[1])
        $wasmSha256 = [Convert]::ToHexString(
            [Security.Cryptography.SHA256]::HashData($wasmBytes)).ToLowerInvariant()
        if ($wasmSha256 -cne [string]$kernel.wasm_sha256) {
            throw "ASP54K2005 generated WASM identity mismatch: $($kernel.kernel_id)"
        }

        if ($Mode -ceq 'Write') {
            [IO.File]::WriteAllBytes($wasmPath, $wasmBytes)
        }
        elseif (-not (Test-Path -LiteralPath $wasmPath -PathType Leaf) -or
            -not [Linq.Enumerable]::SequenceEqual(
                [byte[]]$wasmBytes,
                [byte[]][IO.File]::ReadAllBytes($wasmPath))) {
            throw "ASP54K2006 tracked WASM bytes differ: $($kernel.kernel_id)"
        }

        $results += [pscustomobject]@{
            kernel_id = [string]$kernel.kernel_id
            wasm_sha256 = $wasmSha256
            wasm_size = $wasmBytes.Length
        }
    }
}
finally {
    $env:PYTHONPATH = $previousPythonPath
}

[pscustomobject]@{
    result = 'phase54_controlled_runtime_suite_verified'
    mode = $Mode
    kernel_count = $results.Count
    python_executable_sha256 =
        (Get-FileHash -LiteralPath $resolvedPython -Algorithm SHA256).
            Hash.ToLowerInvariant()
    kernels = $results
}
