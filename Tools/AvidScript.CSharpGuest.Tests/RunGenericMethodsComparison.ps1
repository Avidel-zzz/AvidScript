param(
    [string]$DotNetPath = "dotnet",
    [string]$NodePath = "node",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepoRoot "Saved/AvidScript/P66CGenericMethodsRuntime"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$ReferenceProject = Join-Path $RepoRoot "Fixtures/Phase66/GenericMethods.Reference.csproj"
$GuestProject = Join-Path $RepoRoot "Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj"
$WasmRunner = Join-Path $PSScriptRoot "RunGenericMethodsWasm.cjs"
$ReferenceOutput = @(& $DotNetPath run --project $ReferenceProject -c Release)
if ($LASTEXITCODE -ne 0) { throw "The .NET reference run failed." }

$PreviousOutputDirectory = $env:AVIDSCRIPT_GENERIC_METHOD_WASM_DIR
try {
    $env:AVIDSCRIPT_GENERIC_METHOD_WASM_DIR = $OutputDirectory
    & $DotNetPath run --project $GuestProject -c Release -- --generic-methods
    if ($LASTEXITCODE -ne 0) { throw "The Guest/WASM fixture build failed." }
}
finally {
    $env:AVIDSCRIPT_GENERIC_METHOD_WASM_DIR = $PreviousOutputDirectory
}

$WasmOutput = @(& $NodePath $WasmRunner $OutputDirectory)
if ($LASTEXITCODE -ne 0) { throw "The WASM run failed." }
$ReferenceRows = @($ReferenceOutput | Where-Object { $_ -match '^generic_[a-z_]+: ' })
$WasmRows = @($WasmOutput | Where-Object { $_ -match '^generic_[a-z_]+: ' })
if ($ReferenceRows.Count -ne 14 -or $WasmRows.Count -ne 14 -or @(Compare-Object $ReferenceRows $WasmRows -SyncWindow 0).Count -ne 0) {
    throw ("The .NET and WASM results differ. .NET: {0}; WASM: {1}" -f
        ($ReferenceRows -join ", "), ($WasmRows -join ", "))
}
Write-Output "GenericMethodsDotNetWasm: 14/14 matched"
