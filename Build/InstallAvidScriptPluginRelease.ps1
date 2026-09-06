[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [ValidateSet('Plan', 'Apply', 'Verify')][string]$Mode = 'Plan',
    [switch]$AllowDowngrade,
    [ValidateRange(1, 300)][int]$LockTimeoutSeconds = 30,
    [string]$ReportPath = ''
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptRoot 'ReleaseEngineering\AvidScriptPluginReleasePackage.ps1')
. (Join-Path $ScriptRoot 'ReleaseEngineering\AvidScriptPluginInstaller.ps1')

if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $Target = Get-AvidScriptPluginInstallTarget $ProjectRoot
    $FullReportPath = [System.IO.Path]::GetFullPath($ReportPath)
    if (Test-AvidScriptPluginReleasePathUnderRoot $FullReportPath $Target.PluginRoot) {
        Throw-AvidScriptPluginReleaseError `
            'ASRI2001' `
            'path_invalid' `
            'install report must remain outside the installed plugin directory.'
    }
}

$Result = Invoke-AvidScriptPluginInstallTransaction `
    -PackageRoot $PackageRoot `
    -ProjectRoot $ProjectRoot `
    -Mode $Mode `
    -AllowDowngrade:$AllowDowngrade `
    -LockTimeoutSeconds $LockTimeoutSeconds

if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = $FullReportPath
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $ReportPath)) | Out-Null
    $TemporaryPath = $ReportPath + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
    try {
        Write-AvidScriptPluginReleaseJson $TemporaryPath $Result
        [System.IO.File]::Move($TemporaryPath, $ReportPath, $true)
    }
    finally {
        if (Test-Path -LiteralPath $TemporaryPath) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
    }
}

$Result | ConvertTo-Json -Depth 16
