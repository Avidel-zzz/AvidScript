[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$ReportPath,
    [string[]]$PrivatePath = @()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseEngineering/AvidScriptBinaryPrivacy.ps1')
$report = Invoke-AvidScriptBinaryPrivacyScan -Root $Root -ReportPath $ReportPath -PrivatePath $PrivatePath
[ordered]@{result='binary_privacy_scan_passed'; files=$report.file_count; bytes=$report.byte_count} | ConvertTo-Json
