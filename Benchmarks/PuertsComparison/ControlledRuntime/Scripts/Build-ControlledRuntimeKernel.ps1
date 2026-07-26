[CmdletBinding()]
param(
    [ValidateSet('Verify', 'Write')]
    [string]$Mode = 'Verify',

    [Parameter(Mandatory = $true)]
    [string]$WatCompilerModuleRoot
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ControlledRoot = Split-Path -Parent $ScriptRoot
$KernelRoot = Join-Path $ControlledRoot 'Kernel'
$ContractPath = Join-Path $KernelRoot 'controlled_runtime_kernel.contract.json'

if (-not (Test-Path -LiteralPath $ContractPath -PathType Leaf)) {
    throw "ASP54K1001 kernel contract is missing: $ContractPath"
}
$Contract = Get-Content -LiteralPath $ContractPath -Raw | ConvertFrom-Json
if ([int]$Contract.schema_version -ne 1 -or
    [string]$Contract.compiler.package -cne 'wasmtime' -or
    [string]$Contract.compiler.version -cne '45.0.0' -or
    [string]$Contract.compiler.entrypoint -cne 'wasmtime.wat2wasm') {
    throw 'ASP54K1002 kernel compiler identity is not the frozen Wasmtime Python 45.0.0 contract'
}

$CompilerRoot = [System.IO.Path]::GetFullPath($WatCompilerModuleRoot)
if (-not (Test-Path -LiteralPath (Join-Path $CompilerRoot 'wasmtime/__init__.py') -PathType Leaf)) {
    throw "ASP54K1003 Wasmtime Python module root is invalid: $CompilerRoot"
}

$WatPath = Join-Path $KernelRoot ([string]$Contract.wat_path)
$WasmPath = Join-Path $KernelRoot ([string]$Contract.wasm_path)
if (-not (Test-Path -LiteralPath $WatPath -PathType Leaf)) {
    throw "ASP54K1004 tracked WAT is missing: $WatPath"
}

$WatText = [System.IO.File]::ReadAllText($WatPath)
$NormalizedWat = $WatText.Replace("`r`n", "`n").Replace("`r", "`n")
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$Sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $WatDigest = [Convert]::ToHexString(
        $Sha256.ComputeHash($Utf8NoBom.GetBytes($NormalizedWat))).ToLowerInvariant()
}
finally {
    $Sha256.Dispose()
}
if ($WatDigest -cne [string]$Contract.wat_normalized_sha256) {
    throw "ASP54K1005 WAT digest mismatch: expected=$($Contract.wat_normalized_sha256) actual=$WatDigest"
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
    $CompilerOutput = @(& python -c $CompilerProbe $WatPath)
    $CompilerExitCode = $LASTEXITCODE
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
if ($CompilerExitCode -ne 0 -or $CompilerOutput.Count -ne 2) {
    throw "ASP54K1006 wat2wasm failed with exit code $CompilerExitCode"
}
if ([string]$CompilerOutput[0] -cne [string]$Contract.compiler.version) {
    throw "ASP54K1007 compiler version mismatch: expected=$($Contract.compiler.version) actual=$($CompilerOutput[0])"
}
$GeneratedBytes = [Convert]::FromBase64String([string]$CompilerOutput[1])
$GeneratedDigest = [Convert]::ToHexString(
    [System.Security.Cryptography.SHA256]::HashData($GeneratedBytes)).ToLowerInvariant()
if ($GeneratedDigest -cne [string]$Contract.wasm_sha256) {
    throw "ASP54K1008 generated WASM digest mismatch: expected=$($Contract.wasm_sha256) actual=$GeneratedDigest"
}

if ($Mode -ceq 'Write') {
    [System.IO.File]::WriteAllBytes($WasmPath, $GeneratedBytes)
}
elseif (-not (Test-Path -LiteralPath $WasmPath -PathType Leaf)) {
    throw "ASP54K1009 tracked WASM is missing: $WasmPath"
}
else {
    $TrackedBytes = [System.IO.File]::ReadAllBytes($WasmPath)
    if (-not [System.Linq.Enumerable]::SequenceEqual([byte[]]$GeneratedBytes, [byte[]]$TrackedBytes)) {
        throw 'ASP54K1010 tracked WASM bytes differ from deterministic wat2wasm output'
    }
}

[pscustomobject]@{
    result = 'controlled_runtime_kernel_verified'
    kernel_id = [string]$Contract.kernel_id
    compiler = "wasmtime-python-$($Contract.compiler.version)"
    wat_sha256 = $WatDigest
    wasm_sha256 = $GeneratedDigest
    wasm_size = $GeneratedBytes.Length
}
