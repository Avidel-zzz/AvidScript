param(
    [string]$DotNetPath = "dotnet",
    [string]$NodePath = "node",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepoRoot "Saved/AvidScript/P66CEnumeratorRuntime"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$ReferenceProject = Join-Path $RepoRoot "Fixtures/Phase66/EnumeratorCleanup.Reference.csproj"
$GuestProject = Join-Path $RepoRoot "Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj"
$WasmRunner = Join-Path $PSScriptRoot "RunEnumeratorWasm.cjs"
$ReferenceOutput = @(& $DotNetPath run --project $ReferenceProject -c Release)
if ($LASTEXITCODE -ne 0) { throw "The .NET enumerator reference run failed." }

$PreviousOutputDirectory = $env:AVIDSCRIPT_ENUMERATOR_WASM_DIR
try {
    $env:AVIDSCRIPT_ENUMERATOR_WASM_DIR = $OutputDirectory
    & $DotNetPath run --project $GuestProject -c Release -- --enumerator
    if ($LASTEXITCODE -ne 0) { throw "The Guest/WASM enumerator fixture build failed." }
}
finally {
    $env:AVIDSCRIPT_ENUMERATOR_WASM_DIR = $PreviousOutputDirectory
}

$WasmOutput = @(& $NodePath $WasmRunner $OutputDirectory)
if ($LASTEXITCODE -ne 0) { throw "The enumerator WASM run failed." }
$ReferenceRows = @($ReferenceOutput | Where-Object { $_ -match '^enumerator_[a-z_]+: ' })
$WasmRows = @($WasmOutput | Where-Object { $_ -match '^enumerator_[a-z_]+: ' })
if ($ReferenceRows.Count -ne 4 -or $WasmRows.Count -ne 4 -or
    @(Compare-Object $ReferenceRows $WasmRows -SyncWindow 0).Count -ne 0) {
    throw ("The .NET and WASM enumerator results differ. .NET: {0}; WASM: {1}; raw .NET: {2}" -f
        ($ReferenceRows -join ", "), ($WasmRows -join ", "), ($ReferenceOutput -join " | "))
}
Write-Output "EnumeratorCleanupDotNetWasm: 4/4 matched"
