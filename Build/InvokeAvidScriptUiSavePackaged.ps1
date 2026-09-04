#requires -Version 7.0
<#
.SYNOPSIS
校验正式 BuildCookRun 结果，并在两个真实 Game 进程中验证 UI 写档与读回。
.EXAMPLE
pwsh -NoProfile -File Build/InvokeAvidScriptUiSavePackaged.ps1 -BuildResultPath <build-result.json>
#>
[CmdletBinding()]
param(
    [string]$BuildResultPath = '',
    [string]$UserRoot = '',
    [ValidateRange(100, 600)][int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PackagedUiBuildRoot = $PSScriptRoot
$PackagedUiPluginRoot = Split-Path -Parent $PSScriptRoot
$PackagedUiProjectRoot = Split-Path -Parent (Split-Path -Parent $PackagedUiPluginRoot)
. (Join-Path $PSScriptRoot 'Android/AvidScriptAndroidProcess.ps1')
. (Join-Path $PSScriptRoot 'AvidScriptCSharpBindingPackage.ps1')
. (Join-Path $PSScriptRoot 'AvidScriptUiSaveCommon.ps1')
. (Join-Path $PSScriptRoot 'AvidScriptUiSavePackagedReport.ps1')

function Get-AvidScriptPackagedUiContext {
    param([string]$BuildResultPath)
    if ([string]::IsNullOrWhiteSpace($BuildResultPath)) { throw 'BuildResultPath is required; use a completed InvokeAvidScriptBuildCookRun JSON result.' }
    $BuildResultPath = [IO.Path]::GetFullPath($BuildResultPath)
    $Build = Read-AvidScriptPackagedUiJson $BuildResultPath
    foreach ($Entry in ([ordered]@{ schema_version = 1; result = 'avidscript_build_cook_run_succeeded'; status = 'ok'
                platform = 'Win64'; packaged_oracle_mode = 'None' }).GetEnumerator()) {
        Assert-AvidScriptPackagedUiEqual $Build.($Entry.Key) $Entry.Value "build.$($Entry.Key)"
    }
    if ($Build.configuration -cnotin @('Development', 'Shipping') -or
        $Build.target_name -isnot [string] -or $Build.target_name -cnotmatch '\A[A-Za-z][A-Za-z0-9_]{0,63}\z' -or
        $Build.cook_maps -isnot [array] -or $Build.cook_maps.Count -ne 1 -or
        $Build.cook_maps[0] -cne '/AvidScript/Demos/UiSave/L_UiSave' -or
        $Build.enable_plugins -isnot [array] -or $Build.enable_plugins.Count -ne 1 -or
        $Build.enable_plugins[0] -cne 'AvidScriptValidation') { throw 'Build target, configuration, explicit map or validation plugin does not match.' }
    $Release = $Build.release
    foreach ($Entry in ([ordered]@{ schema_version = 1; result = 'avidscript_module_release_succeeded'
                module_id = 'avidscript.ui_save_demo'; configuration = $Build.configuration.ToLowerInvariant()
                target_platform = 'win64'; architecture = 'x86_64'; target_triple = 'x86_64-pc-windows-msvc' }).GetEnumerator()) {
        Assert-AvidScriptPackagedUiEqual $Release.($Entry.Key) $Entry.Value "release.$($Entry.Key)"
    }
    Assert-AvidScriptPackagedUiHash $Release.package_id 'release.package_id'
    Assert-AvidScriptUiSaveSafePath $Build.archive_root
    Assert-AvidScriptUiSaveSafePath $Build.receipt_path
    if (-not (Test-AvidScriptBindingPathContained -RootPath $PackagedUiProjectRoot -CandidatePath $Build.archive_root)) {
        throw 'ArchiveRoot must remain inside the originating project.'
    }
    $PackageRoot = Join-Path $Build.archive_root 'Windows'
    $BinaryName = if ($Build.configuration -ceq 'Shipping') { "$($Build.target_name)-Win64-Shipping.exe" } else { "$($Build.target_name).exe" }
    $Executable = Join-Path $PackageRoot "$($Build.target_name)/Binaries/Win64/$BinaryName"
    Assert-AvidScriptUiSaveSafePath $Executable
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Actual archived Game executable is missing: $Executable" }
    $Receipt = Read-AvidScriptPackagedUiJson $Build.receipt_path
    foreach ($Entry in ([ordered]@{ TargetName = $Build.target_name; TargetType = 'Game'; Platform = 'Win64'; Configuration = $Build.configuration }).GetEnumerator()) {
        Assert-AvidScriptPackagedUiEqual $Receipt.($Entry.Key) $Entry.Value "receipt.$($Entry.Key)"
    }
    $ValidationEntries = @($Receipt.RuntimeDependencies | Where-Object {
        $_.Path.Replace('\', '/') -cmatch '/AvidScriptValidation/AvidScriptValidation\.uplugin$'
    })
    if ($ValidationEntries.Count -ne 1) { throw 'Game receipt must include the explicitly enabled validation plugin.' }
    $ReceiptCheck = $Build.receipt_validation
    Assert-AvidScriptPackagedUiEqual $ReceiptCheck.result 'avidscript_package_receipt_valid' 'receipt_validation.result'
    Assert-AvidScriptPackagedUiEqual $ReceiptCheck.configuration $Build.configuration 'receipt_validation.configuration'
    Assert-AvidScriptPackagedUiEqual $ReceiptCheck.target_name $Build.target_name 'receipt_validation.target_name'
    return [pscustomobject]@{ build = $Build; build_path = $BuildResultPath; package_root = $PackageRoot
        executable = $Executable; project = (Join-Path $PackagedUiProjectRoot "$($Build.target_name).uproject")
        engine = 'C:\UnrealEngine'; package_id = $Release.package_id }
}

function New-AvidScriptPackagedUiStartupConfig {
    param([string]$UserRoot)
    Assert-AvidScriptUiSaveSafePath $UserRoot
    $Directory = Join-Path $UserRoot 'Saved/Config/Windows'
    New-AvidScriptUiSaveDirectory $Directory
    $Path = Join-Path $Directory 'Engine.ini'
    # Shipping ignores command-line map overrides; use the standard isolated user config hierarchy.
    $Bytes = [Text.UTF8Encoding]::new($false).GetBytes(
        "[/Script/EngineSettings.GameMapsSettings]`nGameDefaultMap=/AvidScript/Demos/UiSave/L_UiSave`n")
    $File = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $File.Write($Bytes, 0, $Bytes.Length) } finally { $File.Dispose() }
    return [pscustomobject]@{ mode = 'isolated_generated_engine_ini'; config_writes_disabled = $true; path = $Path
        sha256 = (Get-AvidScriptBindingSha256Hex $Path); map = '/AvidScript/Demos/UiSave/L_UiSave' }
}

function Invoke-AvidScriptPackagedUiVerification {
    param([string]$BuildResultPath, [string]$UserRoot, [int]$TimeoutSeconds)
    $Context = Get-AvidScriptPackagedUiContext $BuildResultPath
    $Build = $Context.build
    $RunId = [Guid]::NewGuid().ToString('N')
    if (-not $UserRoot) { $UserRoot = Join-Path ([IO.Path]::GetTempPath()) "AvidScriptPackagedUi/$RunId" }
    Assert-AvidScriptUiSaveUserRoot $UserRoot $Context
    $UserRoot = [IO.Path]::GetFullPath($UserRoot)
    New-AvidScriptUiSaveDirectory $UserRoot
    $EvidenceRoot = Join-Path $UserRoot 'Saved/AvidScript/PackagedUi'
    New-AvidScriptUiSaveDirectory $EvidenceRoot
    $SummaryPath = Join-Path $EvidenceRoot 'verify.json'
    $Summary = [ordered]@{ schema_version = 1; result = 'avidscript_packaged_ui_save_failed'; succeeded = $false
        run_id = $RunId; configuration = $Build.configuration; package_id = $Context.package_id
        instrumented_package = $true; physical_click_verified = $false; visual_verified = $false; long_run_verified = $false
        build_result_path = $Context.build_path; build_result_sha256 = (Get-AvidScriptBindingSha256Hex $Context.build_path)
        executable = $Context.executable; executable_sha256 = (Get-AvidScriptBindingSha256Hex $Context.executable)
        receipt_sha256 = (Get-AvidScriptBindingSha256Hex $Build.receipt_path)
        user_root = $UserRoot; report_path = $SummaryPath; probes = @(); error = '' }
    try {
        $ReceiptLog = Join-Path $EvidenceRoot 'receipt.log'
        $ReceiptProcess = Invoke-AvidScriptAndroidProcess -Executable (Join-Path $PSHOME 'pwsh.exe') `
            -Arguments @('-NoProfile', '-NonInteractive', '-File', (Join-Path $PackagedUiBuildRoot 'TestAvidScriptPackageReceipt.ps1'),
                '-ReceiptPath', $Build.receipt_path, '-ProjectRoot', $PackagedUiProjectRoot, '-PluginRoot', $PackagedUiPluginRoot,
                '-Configuration', $Build.configuration) -WorkingDirectory $PackagedUiPluginRoot -TimeoutSeconds $TimeoutSeconds
        [IO.File]::WriteAllText($ReceiptLog, $ReceiptProcess.stdout + "`n" + $ReceiptProcess.stderr)
        if ($ReceiptProcess.exit_code -ne 0 -or $ReceiptProcess.stderr.Trim()) { throw "Current package receipt validation failed; see $ReceiptLog" }
        $ReceiptReportPath = Join-Path $EvidenceRoot 'receipt.json'
        [IO.File]::WriteAllText($ReceiptReportPath, $ReceiptProcess.stdout)
        $ReceiptReport = Read-AvidScriptPackagedUiJson $ReceiptReportPath
        Assert-AvidScriptPackagedUiEqual $ReceiptReport.result 'avidscript_package_receipt_valid' 'fresh receipt result'
        Assert-AvidScriptPackagedUiEqual $ReceiptReport.configuration $Build.configuration 'fresh receipt configuration'
        Assert-AvidScriptPackagedUiEqual $ReceiptReport.target_name $Build.target_name 'fresh receipt target'
        $Summary.receipt_report = $ReceiptReportPath
        $Summary.startup_config = New-AvidScriptPackagedUiStartupConfig $UserRoot
        $ProcessIds = [Collections.Generic.HashSet[int]]::new()
        $WriteHash = ''
        foreach ($ProbeMode in @('write', 'read')) {
            Assert-AvidScriptUiSaveSafePath $Summary.startup_config.path
            Assert-AvidScriptPackagedUiEqual (Get-AvidScriptBindingSha256Hex $Summary.startup_config.path) $Summary.startup_config.sha256 'unchanged startup config'
            $ProbeRunId = [Guid]::NewGuid().ToString('N')
            $ReportPath = Join-Path $EvidenceRoot "$ProbeMode-$ProbeRunId.json"
            $LogPath = Join-Path $EvidenceRoot "$ProbeMode-$ProbeRunId.log"
            $Arguments = @($Build.target_name, '/AvidScript/Demos/UiSave/L_UiSave', '-game', '-unattended', '-nullrhi',
                '-nosplash', '-nosound', '-stdout', '-FullStdOutLogOutput', '-nowrite', '-EnablePlugins=AvidScriptValidation',
                '-AvidScriptScenario=ui_save_demo', "-UserDir=$UserRoot", "-abslog=$LogPath",
                "-AvidScriptUiSavePackagedProbe=$ProbeMode", "-AvidScriptUiSaveReport=$ReportPath",
                "-AvidScriptUiSaveExpectedPackage=$($Context.package_id)", "-AvidScriptUiSaveRunId=$ProbeRunId")
            $LaunchedUtc = [DateTimeOffset]::UtcNow
            $Process = Invoke-AvidScriptAndroidProcess -Executable $Context.executable -Arguments $Arguments `
                -WorkingDirectory $Context.package_root -TimeoutSeconds $TimeoutSeconds
            $ExitedUtc = [DateTimeOffset]::UtcNow
            [IO.File]::WriteAllText((Join-Path $EvidenceRoot "$ProbeMode-$ProbeRunId.process.log"), $Process.stdout + "`n" + $Process.stderr)
            if ($Process.exit_code -ne 0) { throw "Packaged $ProbeMode failed with exit $($Process.exit_code); see $LogPath" }
            $Report = Resolve-AvidScriptPackagedUiReport -ReportPath $ReportPath -ProbeMode $ProbeMode `
                -Configuration $Build.configuration -RunId $ProbeRunId -UserRoot $UserRoot -PackageId $Context.package_id `
                -WriteHash $WriteHash -ProcessIds $ProcessIds -LaunchedUtc $LaunchedUtc -ExitedUtc $ExitedUtc
            Assert-AvidScriptPackagedUiEqual $Report.process_id $Process.process_id 'actual process id'
            $WriteHash = $Report.save_file_sha256
            $Summary.probes += [pscustomobject]@{ mode = $ProbeMode; run_id = $ProbeRunId; process_id = $Process.process_id
                exit_code = $Process.exit_code; report_path = $ReportPath; report_sha256 = (Get-AvidScriptBindingSha256Hex $ReportPath)
                actions = $Report.actions.Count; events = $Report.runtime.events; backend = $Report.backend }
        }
        Assert-AvidScriptPackagedUiEqual (Get-AvidScriptBindingSha256Hex $Context.build_path) $Summary.build_result_sha256 'unchanged build result'
        Assert-AvidScriptPackagedUiEqual (Get-AvidScriptBindingSha256Hex $Context.executable) $Summary.executable_sha256 'unchanged game executable'
        Assert-AvidScriptPackagedUiEqual (Get-AvidScriptBindingSha256Hex $Build.receipt_path) $Summary.receipt_sha256 'unchanged receipt'
        Assert-AvidScriptUiSaveSafePath $Summary.startup_config.path
        Assert-AvidScriptPackagedUiEqual (Get-AvidScriptBindingSha256Hex $Summary.startup_config.path) $Summary.startup_config.sha256 'unchanged startup config'
        $Summary.save_file_sha256 = $WriteHash
        $Summary.result = 'avidscript_packaged_ui_save_passed'
        $Summary.succeeded = $true
    } catch { $Summary.error = $_.Exception.Message }
    Write-AvidScriptUiSaveNewJson $SummaryPath $Summary
    return [pscustomobject]$Summary
}

if ($MyInvocation.InvocationName -eq '.') { return }
try {
    $Summary = Invoke-AvidScriptPackagedUiVerification -BuildResultPath $BuildResultPath -UserRoot $UserRoot -TimeoutSeconds $TimeoutSeconds
    [Console]::Out.WriteLine(($Summary | ConvertTo-Json -Depth 32 -Compress))
    if (-not $Summary.succeeded) { exit 1 }
    exit 0
} catch {
    [Console]::Out.WriteLine(([ordered]@{ schema_version = 1; result = 'avidscript_packaged_ui_save_failed'
        succeeded = $false; error = $_.Exception.Message } | ConvertTo-Json -Compress))
    exit 1
}
