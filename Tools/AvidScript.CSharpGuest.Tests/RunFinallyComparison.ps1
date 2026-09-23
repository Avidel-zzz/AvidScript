param(
    [string]$DotNetPath = "dotnet",
    [string]$NodePath = "node",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepoRoot "Saved/AvidScript/P66CFinallyRuntime"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$ReferenceProject = Join-Path $RepoRoot "Fixtures/Phase66/FinallyCleanup.Reference.csproj"
$GuestProject = Join-Path $RepoRoot "Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj"
$WasmRunner = Join-Path $PSScriptRoot "RunFinallyWasm.cjs"
$ReferenceOutput = @(& $DotNetPath run --project $ReferenceProject -c Release)
if ($LASTEXITCODE -ne 0) { throw "The .NET finally reference run failed." }

$PreviousOutputDirectory = $env:AVIDSCRIPT_FINALLY_WASM_DIR
try {
    $env:AVIDSCRIPT_FINALLY_WASM_DIR = $OutputDirectory
    & $DotNetPath run --project $GuestProject -c Release -- --finally
    if ($LASTEXITCODE -ne 0) { throw "The Guest/WASM finally fixture build failed." }
}
finally {
    $env:AVIDSCRIPT_FINALLY_WASM_DIR = $PreviousOutputDirectory
}

$WasmOutput = @(& $NodePath $WasmRunner $OutputDirectory)
if ($LASTEXITCODE -ne 0) { throw "The finally WASM run failed." }
$ReferenceRows = @($ReferenceOutput | Where-Object { $_ -match '^finally_[a-z_]+: ' })
$WasmRows = @($WasmOutput | Where-Object { $_ -match '^finally_[a-z_]+: ' })
if ($ReferenceRows.Count -ne 6 -or $WasmRows.Count -ne 6 -or
    @(Compare-Object $ReferenceRows $WasmRows -SyncWindow 0).Count -ne 0) {
    throw ("The .NET and WASM finally results differ. .NET: {0}; WASM: {1}; raw .NET: {2}" -f
        ($ReferenceRows -join ", "), ($WasmRows -join ", "), ($ReferenceOutput -join " | "))
}
Write-Output "FinallyCleanupDotNetWasm: 6/6 matched"
