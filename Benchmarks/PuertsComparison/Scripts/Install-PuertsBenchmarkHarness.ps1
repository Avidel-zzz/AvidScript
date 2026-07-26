[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Install', 'Verify', 'Remove')]
    [string]$Mode,

    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
$HarnessSource = [System.IO.Path]::GetFullPath((Join-Path $BenchmarkRoot 'AvidScriptPerfHarness'))
$ResolvedProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
$PluginsRoot = [System.IO.Path]::GetFullPath((Join-Path $ResolvedProjectRoot 'Plugins'))
$HarnessTarget = [System.IO.Path]::GetFullPath((Join-Path $PluginsRoot 'AvidScriptPerfHarness'))

if (-not (Test-Path -LiteralPath $ResolvedProjectRoot -PathType Container) -or
    @(Get-ChildItem -LiteralPath $ResolvedProjectRoot -Filter '*.uproject' -File).Count -ne 1) {
    throw 'ASP53H1000 ProjectRoot must contain exactly one .uproject'
}
if (-not (Test-Path -LiteralPath (Join-Path $HarnessSource 'AvidScriptPerfHarness.uplugin') -PathType Leaf)) {
    throw 'ASP53H1001 tracked benchmark harness source is incomplete'
}
if ($HarnessTarget -cne [System.IO.Path]::GetFullPath((Join-Path $PluginsRoot 'AvidScriptPerfHarness'))) {
    throw 'ASP53H1002 harness target escaped the project Plugins root'
}

function Assert-MatchingJunction {
    $Item = Get-Item -LiteralPath $HarnessTarget -Force -ErrorAction Stop
    if ($Item.LinkType -cne 'Junction') {
        throw 'ASP53H1100 harness target exists but is not a managed junction'
    }
    $ResolvedTarget = [System.IO.Path]::GetFullPath([string]$Item.Target)
    if ($ResolvedTarget -cne $HarnessSource) {
        throw "ASP53H1101 harness junction target mismatch: $ResolvedTarget"
    }
}

switch ($Mode) {
    'Install' {
        if (Test-Path -LiteralPath $HarnessTarget) {
            Assert-MatchingJunction
        }
        else {
            New-Item -ItemType Directory -Force -Path $PluginsRoot | Out-Null
            New-Item -ItemType Junction -Path $HarnessTarget -Target $HarnessSource | Out-Null
        }
        Assert-MatchingJunction
    }
    'Verify' {
        Assert-MatchingJunction
    }
    'Remove' {
        if (Test-Path -LiteralPath $HarnessTarget) {
            Assert-MatchingJunction
            Remove-Item -LiteralPath $HarnessTarget -Force
        }
    }
}

[pscustomobject][ordered]@{
    succeeded = $true
    mode = $Mode
    harness_source = $HarnessSource
    harness_target = $HarnessTarget
} | ConvertTo-Json -Depth 4
