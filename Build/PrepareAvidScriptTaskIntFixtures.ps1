param(
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [string]$ProjectRoot = ''
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
}
$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
$DotNetPath = [System.IO.Path]::GetFullPath($DotNetPath)
$TestProject = Join-Path $PluginRoot 'Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj'
$ReferenceProject = Join-Path $PluginRoot 'Fixtures/Phase66/TaskIntIntegrated.Reference.csproj'
$FixtureRoot = Join-Path $ProjectRoot 'Saved/AvidScriptManagedHeapTests/GuestFixtures'
foreach ($Required in @($DotNetPath, $TestProject, $ReferenceProject)) {
    if (-not [System.IO.File]::Exists($Required)) {
        throw "Task<int> fixture input is missing: $Required"
    }
}

$PreviousCliHome = $env:DOTNET_CLI_HOME
$PreviousTelemetry = $env:DOTNET_CLI_TELEMETRY_OPTOUT
$PreviousFixtures = $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR
try {
    $env:DOTNET_CLI_HOME = Join-Path ([System.IO.Path]::GetTempPath()) 'avidscript-task-int-dotnet'
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR = $FixtureRoot
    $Sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $Sdk -cne '8.0.416') {
        throw "Task<int> fixtures require .NET SDK 8.0.416, got '$Sdk'."
    }
    $Reference = @(& $DotNetPath run --project $ReferenceProject --configuration Release)
    if ($LASTEXITCODE -ne 0 -or $Reference -notcontains 'result=253; cleanups=2') {
        throw "Integrated Task<int> .NET reference failed: $($Reference -join ' | ')"
    }
    Push-Location $PluginRoot
    try {
        & $DotNetPath run --project $TestProject --configuration Release -- --async-invocation
        if ($LASTEXITCODE -ne 0) { throw 'C# Task<int> fixture compilation failed.' }
    }
    finally {
        Pop-Location
    }
    foreach ($Scenario in @('immediate', 'deferred', 'chain', 'arguments', 'combined', 'cleanup', 'cancelled', 'cancelled-chain', 'local', 'parallel', 'integrated')) {
        $Stem = Join-Path $FixtureRoot "csharp-task-int-$Scenario"
        $Wasm = "$Stem.wasm"
        $OffsetFile = "$Stem.result-offset"
        if (-not [System.IO.File]::Exists($Wasm) -or
            ([System.IO.FileInfo]$Wasm).Length -le 8 -or
            -not [System.IO.File]::Exists($OffsetFile)) {
            throw "Task<int> fixture is incomplete: $Stem"
        }
        $Offset = 0
        $Text = [System.IO.File]::ReadAllText($OffsetFile).Trim()
        if (-not [int]::TryParse($Text, [ref]$Offset) -or
            $Offset -lt 0 -or $Offset -ge 65536) {
            throw "Task<int> fixture has invalid result offset: $OffsetFile"
        }
        if ($Scenario -eq 'integrated') {
            $CleanupFile = "$Stem.cleanup-offset"
            $CleanupOffset = 0
            if (-not [System.IO.File]::Exists($CleanupFile) -or
                -not [int]::TryParse([System.IO.File]::ReadAllText($CleanupFile).Trim(), [ref]$CleanupOffset) -or
                $CleanupOffset -lt 0 -or $CleanupOffset -ge 65536) {
                throw "Integrated Task<int> fixture has invalid cleanup offset: $CleanupFile"
            }
        }
    }
}
finally {
    $env:DOTNET_CLI_HOME = $PreviousCliHome
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = $PreviousTelemetry
    $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR = $PreviousFixtures
}

Write-Output "AvidScript.TaskIntFixtures: 11/11 generated; integrated .NET reference=253/2; directory=$FixtureRoot"
