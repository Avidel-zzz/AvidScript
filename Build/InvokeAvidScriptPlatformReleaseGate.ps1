[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [ValidateSet('Inspect', 'Execute')][string]$Mode = 'Inspect',
    [string]$PlanPath = '',
    [string]$ProjectRoot = '',
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = 'dotnet',
    [string]$ReportPath = '',
    [ValidateRange(60, 7200)][int]$TimeoutSeconds = 3600
)

$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildRoot
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
}
. (Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptPlatformReleaseGate.ps1')

try {
    $Report = Invoke-AvidScriptPlatformReleaseGate `
        -PackageRoot $PackageRoot `
        -Mode $Mode `
        -PlanPath $PlanPath `
        -ProjectRoot $ProjectRoot `
        -EngineRoot $EngineRoot `
        -DotNetPath $DotNetPath `
        -ReportPath $ReportPath `
        -TimeoutSeconds $TimeoutSeconds
    [Console]::Out.WriteLine(($Report | ConvertTo-Json -Depth 100 -Compress))
    if ($Report.result -ceq 'passed') { exit 0 }
    if ($Report.result -ceq 'partial') { exit 2 }
    exit 1
}
catch {
    $Category = if ($_.Exception.Data.Contains('category')) {
        [string]$_.Exception.Data['category']
    }
    else {
        'unexpected_failure'
    }
    [Console]::Out.WriteLine((@{
            schema_version = 1
            result = 'avidscript_platform_release_gate_failed'
            status = 'error'
            category = $Category
            message = $_.Exception.Message
        } | ConvertTo-Json -Compress))
    exit 1
}
