#requires -Version 7.0
<#
.SYNOPSIS
挂接或核对默认禁用的包内验证插件，不修改工程配置。
.EXAMPLE
pwsh -NoProfile -File Build/InstallAvidScriptValidation.ps1 -Mode Install
#>
[CmdletBinding()]
param(
    [ValidateSet('Install', 'Verify')][string]$Mode = 'Verify',
    [string]$ProjectRoot = (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AvidScriptCSharpBindingPackage.ps1')
. (Join-Path $PSScriptRoot 'AvidScriptUiSaveCommon.ps1')
$PluginRoot = Split-Path -Parent $PSScriptRoot
$Source = [IO.Path]::GetFullPath((Join-Path $PluginRoot 'Tests/Unreal/AvidScriptValidation'))
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$PluginsRoot = Join-Path $ProjectRoot 'Plugins'
$Target = Join-Path $PluginsRoot 'AvidScriptValidation'

Assert-AvidScriptUiSaveSafePath $Source
Assert-AvidScriptUiSaveSafePath $PluginsRoot
if (-not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $Source) '.ubtignore') -PathType Leaf)) {
    throw 'Tests/Unreal/.ubtignore is required to prevent duplicate UBT module discovery.'
}
if (@(Get-ChildItem -LiteralPath $ProjectRoot -Filter '*.uproject' -File).Count -ne 1) {
    throw 'ProjectRoot must contain exactly one .uproject.'
}
$Descriptor = Get-Content -LiteralPath (Join-Path $Source 'AvidScriptValidation.uplugin') -Raw | ConvertFrom-Json
if ($Descriptor.EnabledByDefault -isnot [bool] -or $Descriptor.EnabledByDefault -or
    $Descriptor.Modules.Count -ne 1 -or $Descriptor.Modules[0].Name -cne 'AvidScriptValidation' -or
    $Descriptor.Modules[0].Type -cne 'Runtime') { throw 'Validation plugin descriptor is not default-disabled Runtime.' }

if (-not (Test-Path -LiteralPath $Target) -and $Mode -ceq 'Install') {
    [void][IO.Directory]::CreateDirectory($PluginsRoot)
    New-Item -ItemType Junction -Path $Target -Target $Source -ErrorAction Stop | Out-Null
}
$Item = Get-Item -LiteralPath $Target -Force -ErrorAction Stop
if ($Item.LinkType -cne 'Junction' -or -not (Test-AvidScriptUiSaveSamePath ([string]$Item.Target) $Source)) {
    throw 'Existing validation plugin path is not the expected junction; it was not replaced.'
}
[pscustomobject]@{ schema_version = 1; succeeded = $true; mode = $Mode
    plugin_name = 'AvidScriptValidation'; source = $Source; target = $Target
    enabled_by_default = $false } | ConvertTo-Json -Compress
