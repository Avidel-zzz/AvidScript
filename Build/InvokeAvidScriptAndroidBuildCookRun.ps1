[CmdletBinding()]
param(
    [ValidateSet('Preflight', 'BuildCookRun')][string]$Mode = 'Preflight',
    [ValidateSet('Development', 'Shipping')][string]$Configuration = 'Development',
    [string]$ArchiveRoot = '',
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$AndroidSdkRoot = '',
    [string]$NdkRoot = '',
    [string]$JavaHome = '',
    [ValidateRange(60, 7200)][int]$TimeoutSeconds = 3600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$BuildRoot = $PSScriptRoot
$PluginRoot = Split-Path -Parent $BuildRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
. (Join-Path $BuildRoot 'Android/AvidScriptAndroidProcess.ps1')

function New-AvidScriptAndroidUatArguments {
    param([string]$ProjectFile, [string]$TargetName, [string]$Configuration, [string]$ArchiveRoot)

    return @(
        'BuildCookRun', '-nop4', '-unattended', '-utf8output',
        "-project=$ProjectFile", "-target=$TargetName", '-targetplatform=Android',
        '-architectures=arm64', '-cookflavor=ASTC', "-clientconfig=$Configuration",
        '-build', '-skipbuildeditor', '-cook', '-stage', '-pak', '-package', '-archive',
        "-archivedirectory=$ArchiveRoot",
        '-AdditionalCookerOptions=-SkipZenStore -AvidScriptSuppressGeneratedTypeExecution')
}

function Invoke-AvidScriptAndroidBuildCookRun {
    $PowerShellPath = Join-Path $PSHOME 'pwsh.exe'
    $PreflightArguments = @(
        '-NoProfile', '-NonInteractive', '-File', (Join-Path $BuildRoot 'TestAvidScriptAndroidToolchain.ps1'),
        '-EngineRoot', $EngineRoot)
    foreach ($Pair in @(
            @('-AndroidSdkRoot', $AndroidSdkRoot), @('-NdkRoot', $NdkRoot), @('-JavaHome', $JavaHome))) {
        if (-not [string]::IsNullOrWhiteSpace($Pair[1])) { $PreflightArguments += $Pair }
    }
    $PreflightProcess = Invoke-AvidScriptAndroidProcess -Executable $PowerShellPath `
        -Arguments $PreflightArguments -WorkingDirectory $PluginRoot -TimeoutSeconds 60
    $Preflight = $PreflightProcess.stdout | ConvertFrom-Json -Depth 32
    $Result = [ordered]@{
        schema_version = 1
        result = 'avidscript_android_build_not_run'
        status = 'not_run'
        mode = $Mode
        configuration = $Configuration
        reason = 'toolchain_unavailable'
        toolchain = $Preflight
        build_cook_run = 'not_run'
        receipt = $null
        apks = @()
        obb_files = @()
        device = 'not_run'
    }
    if ($PreflightProcess.exit_code -ne 0 -or -not $Preflight.ready) { return [pscustomobject]$Result }
    if ($Mode -ceq 'Preflight') {
        $Result.result = 'avidscript_android_preflight_passed'
        $Result.status = 'ok'
        $Result.reason = 'preflight_only'
        return [pscustomobject]$Result
    }

    $TargetName = Split-Path -Leaf $ProjectRoot
    $ProjectFile = Join-Path $ProjectRoot "$TargetName.uproject"
    if (-not (Test-Path -LiteralPath $ProjectFile -PathType Leaf)) { throw 'UE project file is missing.' }
    $RunId = "$Configuration-$PID-$([Guid]::NewGuid().ToString('N'))"
    $EvidenceRoot = Join-Path $ProjectRoot 'Saved/AvidScript/Android'
    $ResolvedArchiveRoot = if ([string]::IsNullOrWhiteSpace($ArchiveRoot)) {
        Join-Path $EvidenceRoot "Packages/$RunId"
    } else {
        [System.IO.Path]::GetFullPath($ArchiveRoot)
    }
    if ($ResolvedArchiveRoot -match '[&|<>^%"\r\n]' -or $ProjectFile -match '[&|<>^%"\r\n]') {
        throw 'UAT paths contain unsupported command-host characters.'
    }
    if (Test-Path -LiteralPath $ResolvedArchiveRoot) {
        throw 'ArchiveRoot must be a new directory owned by this run.'
    }
    [void][System.IO.Directory]::CreateDirectory($EvidenceRoot)
    $LogPath = Join-Path $EvidenceRoot "BuildCookRun-$RunId.log"
    $RunUat = Join-Path $EngineRoot 'Engine/Build/BatchFiles/RunUAT.bat'
    $Arguments = @('/d', '/s', '/c', $RunUat) + @(New-AvidScriptAndroidUatArguments `
        -ProjectFile $ProjectFile -TargetName $TargetName -Configuration $Configuration -ArchiveRoot $ResolvedArchiveRoot)
    $ChildEnvironment = @{
        ANDROID_HOME = [string]$Preflight.toolchain.sdk_root
        ANDROID_SDK_ROOT = [string]$Preflight.toolchain.sdk_root
        NDKROOT = [string]$Preflight.toolchain.ndk_root
        NDK_ROOT = [string]$Preflight.toolchain.ndk_root
        JAVA_HOME = [string]$Preflight.toolchain.java_home
    }
    $StartedUtc = [DateTime]::UtcNow
    $Uat = Invoke-AvidScriptAndroidProcess `
        -Executable (Join-Path ([Environment]::GetFolderPath('System')) 'cmd.exe') `
        -Arguments $Arguments -WorkingDirectory $ProjectRoot -TimeoutSeconds $TimeoutSeconds `
        -Environment $ChildEnvironment
    [System.IO.File]::WriteAllText($LogPath, ($Uat.stdout + "`n--- stderr ---`n" + $Uat.stderr), [System.Text.UTF8Encoding]::new($false))
    if ($Uat.exit_code -ne 0) { throw "Android BuildCookRun failed ($($Uat.exit_code)). See $LogPath" }

    $ReceiptRoot = Join-Path $ProjectRoot 'Binaries/Android'
    $Receipts = @(Get-ChildItem -LiteralPath $ReceiptRoot -Filter '*.target' -File | Where-Object {
        $Receipt = [System.IO.File]::ReadAllText($_.FullName) | ConvertFrom-Json -Depth 64
        $Receipt.TargetName -ceq $TargetName -and $Receipt.Platform -ceq 'Android' -and
            $Receipt.Configuration -ceq $Configuration -and $Receipt.TargetType -ceq 'Game'
    })
    if ($Receipts.Count -ne 1) { throw 'Android BuildCookRun did not identify one matching Game receipt.' }
    $ReceiptArguments = @(
        '-NoProfile', '-NonInteractive', '-File', (Join-Path $BuildRoot 'TestAvidScriptPackageReceipt.ps1'),
        '-ReceiptPath', $Receipts[0].FullName, '-ProjectRoot', $ProjectRoot, '-PluginRoot', $PluginRoot,
        '-Configuration', $Configuration, '-TargetPlatform', 'Android')
    $ReceiptProcess = Invoke-AvidScriptAndroidProcess -Executable $PowerShellPath `
        -Arguments $ReceiptArguments -WorkingDirectory $PluginRoot -TimeoutSeconds 60
    if ($ReceiptProcess.exit_code -ne 0) { throw "Android package receipt failed: $($ReceiptProcess.stdout)" }
    $Apks = @(Get-ChildItem -LiteralPath $ResolvedArchiveRoot -Filter '*.apk' -File -Recurse | ForEach-Object {
        Get-AvidScriptAndroidApkIdentity -Path $_.FullName
    })
    if ($Apks.Count -eq 0) { throw 'Android archive contains no APK.' }
    $Result.result = 'avidscript_android_build_cook_run_passed'
    $Result.status = 'ok'
    $Result.reason = ''
    $Result.build_cook_run = 'passed'
    $Result.receipt = $ReceiptProcess.stdout | ConvertFrom-Json -Depth 64
    $Result.apks = $Apks
    $Result.obb_files = @(Get-ChildItem -LiteralPath $ResolvedArchiveRoot -Filter '*.obb' -File -Recurse | ForEach-Object {
        [pscustomobject]@{ path = $_.FullName; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $Result['archive_root'] = $ResolvedArchiveRoot
    $Result['uat_log'] = $LogPath
    $Result['elapsed_ms'] = $Uat.elapsed_ms
    $Result['receipt_freshness'] = if ($Receipts[0].LastWriteTimeUtc -ge $StartedUtc) { 'fresh' } else { 'reused_after_successful_uat' }
    return [pscustomobject]$Result
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        $Result = Invoke-AvidScriptAndroidBuildCookRun
        [Console]::Out.WriteLine(($Result | ConvertTo-Json -Depth 100 -Compress))
        if ($Result.status -ceq 'not_run') { exit 2 }
        exit 0
    }
    catch {
        [Console]::Out.WriteLine((@{
            schema_version = 1; result = 'avidscript_android_build_failed'; status = 'error'; message = $_.Exception.Message
        } | ConvertTo-Json -Compress))
        exit 1
    }
}
