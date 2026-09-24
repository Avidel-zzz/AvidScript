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
$FixtureRoot = Join-Path $ProjectRoot 'Saved/AvidScriptManagedHeapTests/GuestFixtures'
foreach ($Required in @($DotNetPath, $TestProject)) {
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
    Push-Location $PluginRoot
    try {
        & $DotNetPath run --project $TestProject --configuration Release -- --async-invocation
        if ($LASTEXITCODE -ne 0) { throw 'C# Task<int> fixture compilation failed.' }
    }
    finally {
        Pop-Location
    }
    foreach ($Scenario in @('immediate', 'deferred', 'chain', 'arguments', 'cancelled')) {
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
    }
}
finally {
    $env:DOTNET_CLI_HOME = $PreviousCliHome
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = $PreviousTelemetry
    $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR = $PreviousFixtures
}

Write-Output "AvidScript.TaskIntFixtures: 5/5 generated; directory=$FixtureRoot"
