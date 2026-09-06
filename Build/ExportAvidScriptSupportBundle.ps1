[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [string]$PluginRoot = '',
    [string]$EngineRoot = '',
    [string]$DotNetPath = '',
    [string[]]$LogPath = @(),
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
. (Join-Path $ScriptRoot 'ReleaseEngineering/AvidScriptSupportBundle.ps1')

$Compatibility = Get-AvidScriptCompatibilityDoctorReport `
    -PluginRoot $PluginRoot `
    -ProjectRoot $ProjectRoot `
    -EngineRoot $EngineRoot `
    -DotNetPath $DotNetPath `
    -SkipExternalTools:$SkipExternalTools
Assert-AvidScriptCompatibilityDoctorReport $Compatibility
$Bundle = Publish-AvidScriptSupportBundle `
    -CompatibilityReport $Compatibility `
    -OutputRoot $OutputRoot `
    -LogPath $LogPath

[pscustomobject][ordered]@{
    schema_version = 1
    result = 'passed'
    compatibility_result = [string]$Compatibility.result
    compatibility_summary = $Compatibility.summary
    bundle_root = $Bundle.Root
    bundle_id = [string]$Bundle.Manifest.bundle_id
    manifest_sha256 = [string]$Bundle.ManifestSha256
    inventory_sha256 = [string]$Bundle.Manifest.payload.inventory_sha256
    file_count = [int]$Bundle.Manifest.payload.file_count
    total_bytes = [int64]$Bundle.Manifest.payload.total_bytes
} | ConvertTo-Json -Depth 8

if ($FailOnBlocked -and $Compatibility.result -ceq 'blocked') {
    exit 2
}
