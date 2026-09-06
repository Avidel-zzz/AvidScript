[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [string]$PluginRoot = '',
    [string]$EngineRoot = '',
    [string]$DotNetPath = '',
    [string]$ReportPath = '',
    [switch]$SkipExternalTools,
    [switch]$FailOnBlocked
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($PluginRoot)) {
    $PluginRoot = Split-Path -Parent $ScriptRoot
}
if ([string]::IsNullOrWhiteSpace($EngineRoot)) {
    $EngineRoot = if (-not [string]::IsNullOrWhiteSpace($env:UE_ROOT)) {
        $env:UE_ROOT
    }
    else { 'C:\UnrealEngine' }
}
if ([string]::IsNullOrWhiteSpace($DotNetPath)) {
    $DotNetPath = Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
}
. (Join-Path $ScriptRoot 'ReleaseEngineering/AvidScriptCompatibilityDoctor.ps1')

$Report = Get-AvidScriptCompatibilityDoctorReport `
    -PluginRoot $PluginRoot `
    -ProjectRoot $ProjectRoot `
    -EngineRoot $EngineRoot `
    -DotNetPath $DotNetPath `
    -SkipExternalTools:$SkipExternalTools
Assert-AvidScriptCompatibilityDoctorReport $Report

if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = [System.IO.Path]::GetFullPath($ReportPath)
    $Parent = Split-Path -Parent $ReportPath
    [System.IO.Directory]::CreateDirectory($Parent) | Out-Null
    $TemporaryPath = $ReportPath + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        Write-AvidScriptPluginReleaseJson $TemporaryPath $Report
        Assert-AvidScriptPluginReleaseJsonSchema `
            -JsonPath $TemporaryPath `
            -SchemaPath (Join-Path $ScriptRoot 'ReleaseEngineering/AvidScriptCompatibilityReport.schema.json') `
            -Label 'compatibility report'
        [System.IO.File]::Move($TemporaryPath, $ReportPath, $true)
    }
    finally {
        if (Test-Path -LiteralPath $TemporaryPath) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
    }
}

$Report | ConvertTo-Json -Depth 16
if ($FailOnBlocked -and $Report.result -ceq 'blocked') {
    exit 2
}
