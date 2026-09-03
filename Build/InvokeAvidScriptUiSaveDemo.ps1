#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Prepare', 'Publish', 'Play', 'Verify')][string]$Mode = 'Prepare',
    [string]$ProfilePath = '',
    [string]$OutputRoot = '',
    [string]$ExpectedPackageId = '',
    [string]$VerifyUserRoot = '',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [ValidateRange(10, 7200)][int]$TimeoutSeconds = 1200
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$UiSaveBuildRoot = $PSScriptRoot
$UiSavePluginRoot = Split-Path -Parent $UiSaveBuildRoot
$UiSaveProjectRoot = Split-Path -Parent (Split-Path -Parent $UiSavePluginRoot)
. (Join-Path $UiSaveBuildRoot 'Android/AvidScriptAndroidProcess.ps1')
. (Join-Path $UiSaveBuildRoot 'AvidScriptCSharpBindingPackage.ps1')

function Get-AvidScriptUiSaveContext {
    if ($PSVersionTable.PSVersion.Major -ne 7) { throw 'PowerShell 7 is required.' }
    $EngineRoot = 'C:\UnrealEngine'
    $Version = Get-Content -Raw -LiteralPath (Join-Path $EngineRoot 'Engine/Build/Build.version') | ConvertFrom-Json
    if ($Version.MajorVersion -ne 5 -or $Version.MinorVersion -ne 8) { throw 'UE5.8 is required.' }
    $ProjectFile = Join-Path $UiSaveProjectRoot "$(Split-Path -Leaf $UiSaveProjectRoot).uproject"
    $Editor = Join-Path $EngineRoot $(if ($Mode -ieq 'Play') { 'Engine/Binaries/Win64/UnrealEditor.exe' } else { 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe' })
    foreach ($File in @($ProjectFile, $Editor)) {
        if (-not (Test-Path -LiteralPath $File -PathType Leaf)) { throw "Required file is missing: $File" }
    }
    $Root = if ($OutputRoot) { Resolve-AvidScriptBindingPath $UiSaveProjectRoot $OutputRoot } else { Join-Path $UiSaveProjectRoot 'Saved/AvidScript/UiSaveDemo' }
    if (-not (Test-AvidScriptBindingPathContained -RootPath $UiSaveProjectRoot -CandidatePath $Root) -or
        -not (Test-AvidScriptBindingPathContained -RootPath (Join-Path $UiSaveProjectRoot 'Saved') -CandidatePath $Root)) {
        throw 'OutputRoot must be below project Saved without reparse points.'
    }
    $Context = [ordered]@{ project = $ProjectFile; editor = $Editor; output_root = $Root; engine = $EngineRoot }
    if ($Mode -ieq 'Publish') {
        $Profile = if ($ProfilePath) { Resolve-AvidScriptBindingPath $UiSaveProjectRoot $ProfilePath } else { Join-Path $UiSavePluginRoot 'Samples/CSharp/UiSaveDemo/UiSaveDemo.csharp-profile.json' }
        $Settings = Get-Content -Raw -LiteralPath $Profile | ConvertFrom-Json -Depth 32
        if ($Settings.module_id -cne 'avidscript.ui_save_demo' -or $Settings.artifact_stem -cne 'ui_save_demo' -or
            $Settings.binding_profile.package_name -cne 'avidscript.sample.ui_save_demo') { throw 'UI/Save profile identity is invalid.' }
        $Context.profile = [IO.Path]::GetFullPath($Profile)
        $Context.source = Resolve-AvidScriptBindingPath $UiSaveProjectRoot $Settings.source_path
        $Context.csharp_project = Resolve-AvidScriptBindingPath $UiSaveProjectRoot $Settings.project_path
        $Context.dotnet = [IO.Path]::GetFullPath($DotNetPath)
        $Context.pwsh = Join-Path $PSHOME 'pwsh.exe'
        foreach ($File in @($Context.source, $Context.csharp_project)) {
            if (-not (Test-AvidScriptBindingPathContained -RootPath $UiSaveProjectRoot -CandidatePath $File)) { throw 'Profile source/project escaped the project.' }
        }
        foreach ($File in @($Context.source, $Context.csharp_project, $Context.dotnet, $Context.pwsh)) {
            if (-not (Test-Path -LiteralPath $File -PathType Leaf)) { throw "Required file is missing: $File" }
        }
    }
    return [pscustomobject]$Context
}

function Invoke-AvidScriptUiSaveTool {
    param([string]$Executable, [string[]]$Arguments, [string]$LogPath, [hashtable]$Environment = @{},
        [ValidateRange(1, 7200)][int]$ProcessTimeoutSeconds = $TimeoutSeconds)
    $Result = Invoke-AvidScriptAndroidProcess -Executable $Executable -Arguments $Arguments `
        -WorkingDirectory $UiSavePluginRoot -TimeoutSeconds $ProcessTimeoutSeconds -Environment $Environment
    [IO.File]::WriteAllText($LogPath, $Result.stdout + "`n" + $Result.stderr, [Text.UTF8Encoding]::new($false))
    if ($Result.exit_code -ne 0) { throw "Child process failed with exit code $($Result.exit_code). See $LogPath" }
    return $Result
}

function Resolve-AvidScriptUiSaveBindingReport {
    param([string]$ReportPath, [string]$ExpectedProfile, [string]$BindingRoot)
    $Report = Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json -Depth 32
    if ($Report.schema_version -ne 1 -or $Report.result -cne 'avidscript_profile_bindings_published' -or
        $Report.status -cne 'ok' -or $Report.package_name -cne 'avidscript.sample.ui_save_demo' -or
        -not [IO.Path]::IsPathFullyQualified($Report.profile_path) -or
        -not [IO.Path]::GetFullPath($Report.profile_path).Equals([IO.Path]::GetFullPath($ExpectedProfile), [StringComparison]::OrdinalIgnoreCase) -or
        $Report.manifest_sha256 -cnotmatch '\A[0-9a-f]{64}\z' -or $Report.package_hash -cnotmatch '\A[0-9a-f]{64}\z' -or
        -not [IO.Path]::IsPathFullyQualified($Report.manifest_path) -or
        -not (Test-AvidScriptBindingPathContained -RootPath $BindingRoot -CandidatePath $Report.manifest_path)) {
        throw 'Custom profile binding report identity/path is invalid.'
    }
    $Package = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $Report.manifest_path
    if ($Package.PackageName -cne 'avidscript.sample.ui_save_demo' -or
        $Package.ManifestSha256 -cne $Report.manifest_sha256 -or $Package.PackageHash -cne $Report.package_hash) {
        throw 'Custom profile binding report does not match the verified manifest/hash.'
    }
    return $Package
}

function Start-AvidScriptUiSavePlay {
    param([string]$Executable, [string[]]$Arguments)
    $Info = [Diagnostics.ProcessStartInfo]::new()
    $Info.FileName = $Executable
    $Info.WorkingDirectory = $UiSavePluginRoot
    $Info.UseShellExecute = $false
    foreach ($Argument in $Arguments) { [void]$Info.ArgumentList.Add($Argument) }
    $Process = [Diagnostics.Process]::Start($Info)
    if ($null -eq $Process) { throw 'UI/Save interactive session could not start.' }
    try { return $Process.Id } finally { $Process.Dispose() }
}

function Assert-AvidScriptUiSaveSafePath {
    param([string]$Path)
    if (-not [IO.Path]::IsPathFullyQualified($Path) -or
        [IO.Path]::GetPathRoot($Path) -notmatch '\A[A-Za-z]:[\\/]\z' -or
        -not (Test-AvidScriptBindingPathContained -RootPath ([IO.Path]::GetPathRoot($Path)) -CandidatePath $Path)) {
        throw "Path must be absolute, local and without reparse points: $Path"
    }
    $Current = [IO.Path]::GetFullPath($Path)
    while ($Current) {
        try {
            $Entry = Get-Item -LiteralPath $Current -Force -ErrorAction Stop
            if (($Entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point is forbidden: $Current" }
        } catch [Management.Automation.ItemNotFoundException] { }
        $Parent = Split-Path -Parent $Current
        if ($Parent -eq $Current) { break }
        $Current = $Parent
    }
}

function Assert-AvidScriptUiSaveUserRoot {
    param([string]$Path, [object]$Context)
    Assert-AvidScriptUiSaveSafePath $Path
    $Full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    foreach ($Protected in @((Split-Path -Parent $Context.project), $Context.engine)) {
        $Protected = [IO.Path]::GetFullPath($Protected).TrimEnd('\', '/')
        if ($Full.Equals($Protected, [StringComparison]::OrdinalIgnoreCase) -or
            $Full.StartsWith($Protected + '\', [StringComparison]::OrdinalIgnoreCase) -or
            $Protected.StartsWith($Full + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'VerifyUserRoot must be outside, and not an ancestor of, project and engine directories.'
        }
    }
}

function New-AvidScriptUiSaveDirectory {
    param([string]$Path)
    Assert-AvidScriptUiSaveSafePath $Path
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    Assert-AvidScriptUiSaveSafePath $Path
    if (-not ('AvidScriptUiSaveNativeDirectories' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class AvidScriptUiSaveNativeDirectories {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateDirectoryW(string path, IntPtr security);
    public static void CreateNew(string path) {
        if (!CreateDirectoryW(path, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Evidence directory must be new: " + path);
    }
}
'@
    }
    [AvidScriptUiSaveNativeDirectories]::CreateNew($Path)
    Assert-AvidScriptUiSaveSafePath $Path
}

function Write-AvidScriptUiSaveNewJson {
    param([string]$Path, [object]$Value)
    Assert-AvidScriptUiSaveSafePath $Path
    $Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 32 -Compress))
    $File = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $File.Write($Bytes, 0, $Bytes.Length) } finally { $File.Dispose() }
}

function Test-AvidScriptUiSaveSamePath {
    param([object]$Actual, [string]$Expected)
    return $Actual -is [string] -and [IO.Path]::IsPathFullyQualified($Actual) -and
        [IO.Path]::GetFullPath($Actual).TrimEnd('\', '/').Equals(
            [IO.Path]::GetFullPath($Expected).TrimEnd('\', '/'), [StringComparison]::OrdinalIgnoreCase)
}

function Get-AvidScriptUiSaveProbeSteps {
    param([string]$ProbeMode)
    $Steps = ,@('ready', 'None', '0', 'Ready')
    switch ($ProbeMode) {
        'write' { $Steps += @(@('collect', 'CollectButton', '1', 'Collected'), @('collect', 'CollectButton', '2', 'Collected'),
            @('collect', 'CollectButton', '3', 'Collected'), @('save', 'SaveButton', '3', 'Saved')) }
        'read' { $Steps += ,@('load', 'LoadButton', '3', 'Loaded') }
        'missing' { $Steps += ,@('load', 'LoadButton', '0', 'No saved score') }
        'gc' { $Steps += @(@('load', 'LoadButton', '3', 'Loaded'), @('collect_garbage', 'None', '3', 'Loaded'),
            @('collect', 'CollectButton', '4', 'Collected')) }
        default { throw 'Unknown probe mode.' }
    }
    return ,$Steps
}

function Assert-AvidScriptUiSaveSaveDirectory {
    param([string]$SavePath)
    Assert-AvidScriptUiSaveSafePath $SavePath
    $Directory = Split-Path -Parent $SavePath
    if (Test-Path -LiteralPath $Directory) {
        foreach ($Entry in Get-ChildItem -LiteralPath $Directory -Force) {
            if ($Entry.PSIsContainer -or -not (Test-AvidScriptUiSaveSamePath $Entry.FullName $SavePath)) {
                throw 'Isolated SaveGames directory contains an unexpected entry.'
            }
            Assert-AvidScriptUiSaveSafePath $Entry.FullName
        }
    }
}

function Resolve-AvidScriptUiSaveProbeReport {
    param([string]$ReportPath, [string]$ProbeMode, [string]$UserRoot, [string]$PackageId,
        [string]$WriteHash, [Collections.Generic.HashSet[int]]$ProcessIds)
    Assert-AvidScriptUiSaveSafePath $ReportPath
    if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf) -or (Get-Item -LiteralPath $ReportPath).Length -gt 4MB) {
        throw 'Probe report is missing or oversized.'
    }
    $Report = Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json -Depth 32
    $ProcessId = 0
    $Schema = 0
    if (-not (Try-GetAvidScriptBindingJsonInt32 $Report.schema_version ([ref]$Schema)) -or $Schema -ne 1 -or
        $Report.result -cne 'avidscript_ui_save_probe_passed' -or $Report.succeeded -isnot [bool] -or -not $Report.succeeded -or
        $Report.failure_category -cne '' -or $Report.mode -cne $ProbeMode -or
        $Report.process_mode -cne 'editor_binary_game' -or $Report.input_kind -cne 'synthetic_ue_button_onclicked_broadcast' -or
        $Report.expected_module_id -cne 'avidscript.ui_save_demo' -or $Report.expected_package_id -cne $PackageId -or
        $Report.map -cne '/AvidScript/Demos/UiSave/L_UiSave' -or
        -not (Try-GetAvidScriptBindingJsonInt32 $Report.process_id ([ref]$ProcessId)) -or $ProcessId -le 0 -or
        -not $ProcessIds.Add($ProcessId)) { throw 'Probe identity, mode, result or distinct process id is invalid.' }
    foreach ($Field in @('physical_click_verified', 'visual_verified', 'long_run_verified')) {
        if ($Report.$Field -isnot [bool] -or $Report.$Field) { throw "Synthetic probe claimed unsupported acceptance: $Field" }
    }
    $Runtime = $Report.runtime
    foreach ($Field in @('resolved_from_package', 'runtime_loaded', 'begin_play', 'owner_registered', 'owner_handle_valid')) {
        if ($Runtime.$Field -isnot [bool] -or -not $Runtime.$Field) { throw "Runtime evidence is incomplete: $Field" }
    }
    $Dropped = -1
    $Events = -1
    if ($Runtime.module_id -cne 'avidscript.ui_save_demo' -or $Runtime.package_id -cne $PackageId -or
        $Runtime.error_message -cne '' -or -not (Try-GetAvidScriptBindingJsonInt32 $Runtime.dropped_events ([ref]$Dropped)) -or $Dropped -ne 0 -or
        -not (Try-GetAvidScriptBindingJsonInt32 $Runtime.events ([ref]$Events)) -or
        $Report.startup.active -isnot [bool] -or -not $Report.startup.active -or
        $Report.startup.scenario_id -cne 'ui_save_demo' -or $Report.startup.error_category -cne '' -or $Report.startup.error_message -cne '') {
        throw 'Runtime or startup identity/lifecycle/event evidence is invalid.'
    }
    $Steps = Get-AvidScriptUiSaveProbeSteps $ProbeMode
    if ($Report.actions -isnot [array] -or $Report.actions.Count -ne $Steps.Count) { throw 'Probe actions are missing or incomplete.' }
    $RequiredEvents = 0
    for ($Index = 0; $Index -lt $Steps.Count; ++$Index) {
        $Step = $Steps[$Index]
        $Action = $Report.actions[$Index]
        $Synthetic = $Step[1] -cne 'None'
        if ($Synthetic) { ++$RequiredEvents }
        if ($Action.passed -isnot [bool] -or -not $Action.passed -or $Action.action -cne $Step[0] -or $Action.button -cne $Step[1] -or
            $Action.expected_score -cne $Step[2] -or $Action.observed_score -cne $Step[2] -or
            $Action.expected_status -cne $Step[3] -or $Action.observed_status -cne $Step[3] -or
            $Action.synthetic_ue_event -isnot [bool] -or $Action.synthetic_ue_event -ne $Synthetic) { throw "Probe action evidence is invalid: $Index" }
    }
    if ($Events -lt $RequiredEvents -or $Report.score_text -cne $Steps[-1][2] -or $Report.status_text -cne $Steps[-1][3] -or
        $Report.gc_performed -isnot [bool] -or $Report.gc_performed -ne ($ProbeMode -ceq 'gc')) { throw 'Probe final score/status, event count or GC evidence is invalid.' }
    $SavePath = Join-Path $UserRoot 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
    if (-not (Test-AvidScriptUiSaveSamePath $Report.user_dir $UserRoot) -or
        -not (Test-AvidScriptUiSaveSamePath $Report.save_path $SavePath)) { throw 'Probe user/save path differs from the isolated invocation.' }
    Assert-AvidScriptUiSaveSaveDirectory $SavePath
    $Bytes = -1
    if (-not (Try-GetAvidScriptBindingJsonInt32 $Report.save_file_bytes ([ref]$Bytes)) -or $Report.save_file_exists -isnot [bool]) {
        throw 'Probe save metadata types are invalid.'
    }
    if ($ProbeMode -ceq 'missing') {
        if ($Report.save_file_exists -or (Test-Path -LiteralPath $SavePath) -or $Bytes -ne 0 -or
            $Report.initial_save_sha256 -cne '' -or $Report.save_file_sha256 -cne '') { throw 'Missing-save probe created or claimed a save.' }
    } else {
        if (-not $Report.save_file_exists -or -not (Test-Path -LiteralPath $SavePath -PathType Leaf) -or $Bytes -le 0 -or
            $Bytes -ne (Get-Item -LiteralPath $SavePath).Length -or -not (Test-AvidScriptBindingSha256 $Report.save_file_sha256) -or
            $Report.save_file_sha256 -cne (Get-AvidScriptBindingSha256Hex $SavePath)) { throw 'Probe save hash/size does not match the actual file.' }
        if ($ProbeMode -ceq 'write') {
            if ($Report.initial_save_sha256 -cne '') { throw 'Write probe did not start with an empty save.' }
        } elseif ($Report.initial_save_sha256 -cne $WriteHash -or $Report.save_file_sha256 -cne $WriteHash) {
            throw 'Read/GC save hash differs from the preceding write process.'
        }
    }
    return $Report
}

function Invoke-AvidScriptUiSaveVerify {
    param([object]$Context, [string]$RunRoot, [string]$RunId, [string]$UserRoot)
    $Summary = [ordered]@{ schema_version = 1; result = 'avidscript_ui_save_verify_failed'; status = 'failed'; succeeded = $false
        mode = 'verify'; run_id = $RunId; module_id = 'avidscript.ui_save_demo'; package_id = $ExpectedPackageId.ToLowerInvariant()
        evidence_root = $RunRoot; verify_user_root = $UserRoot; report_path = (Join-Path $RunRoot 'verify.json')
        process_mode = 'editor_binary_game'; input_kind = 'synthetic_ue_button_onclicked_broadcast'
        physical_click_verified = $false; visual_verified = $false; long_run_verified = $false
        gameplay_acceptance = 'synthetic_probe_only'; probes = @(); message = '' }
    try {
        New-AvidScriptUiSaveDirectory $UserRoot
        $WriteRoot = Join-Path $UserRoot 'write'
        $MissingRoot = Join-Path $UserRoot 'missing'
        New-AvidScriptUiSaveDirectory $WriteRoot
        New-AvidScriptUiSaveDirectory $MissingRoot
        $ProcessIds = [Collections.Generic.HashSet[int]]::new()
        $WriteHash = ''
        foreach ($ProbeMode in @('write', 'read', 'missing', 'gc')) {
            $CurrentUserRoot = if ($ProbeMode -ceq 'missing') { $MissingRoot } else { $WriteRoot }
            Assert-AvidScriptUiSaveUserRoot $CurrentUserRoot $Context
            $SavePath = Join-Path $CurrentUserRoot 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
            Assert-AvidScriptUiSaveSaveDirectory $SavePath
            if ($ProbeMode -in @('write', 'missing')) {
                if (Test-Path -LiteralPath $SavePath) { throw 'Write/missing save must not preexist.' }
            } elseif ((Get-AvidScriptBindingSha256Hex $SavePath) -cne $WriteHash) { throw 'Saved bytes changed before read/GC.' }
            $ReportPath = Join-Path $RunRoot "$ProbeMode.json"
            $Log = Join-Path $RunRoot "$ProbeMode.editor.log"
            foreach ($Path in @($ReportPath, $Log)) {
                Assert-AvidScriptUiSaveSafePath $Path
                if (Test-Path -LiteralPath $Path) { throw 'Probe report/log must be new.' }
            }
            [void](Invoke-AvidScriptUiSaveTool -Executable $Context.editor -LogPath (Join-Path $RunRoot "$ProbeMode.process.log") `
                -ProcessTimeoutSeconds ([Math]::Min($TimeoutSeconds, 180)) `
                -Arguments @($Context.project, '/AvidScript/Demos/UiSave/L_UiSave', '-game', '-ExecCmds=Module Load AvidScriptEditor',
                    '-AvidScriptScenario=ui_save_demo', "-AvidScriptUiSaveProbe=$ProbeMode", "-AvidScriptUiSaveReport=$ReportPath",
                    "-AvidScriptUiSaveExpectedPackage=$($Summary.package_id)", "-UserDir=$CurrentUserRoot",
                    '-unattended', '-nullrhi', '-nosound', '-nop4', '-nosplash', '-stdout', '-FullStdOutLogOutput', "-abslog=$Log"))
            $Report = Resolve-AvidScriptUiSaveProbeReport -ReportPath $ReportPath -ProbeMode $ProbeMode -UserRoot $CurrentUserRoot `
                -PackageId $Summary.package_id -WriteHash $WriteHash -ProcessIds $ProcessIds
            if ($ProbeMode -ceq 'write') { $WriteHash = $Report.save_file_sha256 }
            $Summary.probes += [ordered]@{ mode = $ProbeMode; process_id = $Report.process_id; report_path = $ReportPath
                report_sha256 = (Get-AvidScriptBindingSha256Hex $ReportPath); editor_log = $Log; evidence = $Report }
        }
        $FinalSave = Join-Path $WriteRoot 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
        Assert-AvidScriptUiSaveSaveDirectory $FinalSave
        if ((Get-AvidScriptBindingSha256Hex $FinalSave) -cne $WriteHash) { throw 'Saved bytes changed across the process sequence.' }
        $Summary.save_file_sha256 = $WriteHash
        $Summary.result = 'avidscript_ui_save_verify_passed'
        $Summary.status = 'verified'
        $Summary.succeeded = $true
    } catch { $Summary.message = $_.Exception.Message }
    Write-AvidScriptUiSaveNewJson $Summary.report_path $Summary
    return [pscustomobject]$Summary
}

function Invoke-AvidScriptUiSaveDemo {
    if ($Mode -ieq 'Verify' -and $ExpectedPackageId -notmatch '\A[0-9a-fA-F]{64}\z') { throw 'Verify requires ExpectedPackageId (64hex).' }
    $Context = Get-AvidScriptUiSaveContext
    $RunId = [Guid]::NewGuid().ToString('N')
    $RunRoot = Join-Path $Context.output_root $RunId
    if ($Mode -ieq 'Verify') {
        $UserRoot = if ($VerifyUserRoot) { $VerifyUserRoot } else { Join-Path ([IO.Path]::GetTempPath()) "AvidScriptUiSaveVerify/$RunId" }
        Assert-AvidScriptUiSaveUserRoot $UserRoot $Context
        $UserRoot = [IO.Path]::GetFullPath($UserRoot)
    }
    New-AvidScriptUiSaveDirectory $RunRoot
    if ($Mode -ieq 'Verify') { return Invoke-AvidScriptUiSaveVerify $Context $RunRoot $RunId $UserRoot }
    $Log = Join-Path $RunRoot 'editor.log'
    $Result = [ordered]@{ schema_version = 1; result = 'avidscript_ui_save_demo_succeeded'; mode = $Mode.ToLowerInvariant()
        run_id = $RunId; module_id = 'avidscript.ui_save_demo'; evidence_root = $RunRoot
        editor_log = $Log; gameplay_acceptance = 'not_run' }
    if ($Mode -ieq 'Play') {
        $Result.process_id = Start-AvidScriptUiSavePlay -Executable $Context.editor -Arguments @($Context.project,
            '/AvidScript/Demos/UiSave/L_UiSave', '-game', '-windowed', '-AvidScriptScenario=ui_save_demo', "-abslog=$Log")
        $Result.status = 'launched'
        return [pscustomobject]$Result
    }
    $Common = @('-unattended', '-nop4', '-nosplash', '-nullrhi', '-stdout', '-FullStdOutLogOutput',
        '-AvidScriptSuppressGeneratedTypeExecution', "-abslog=$Log")
    if ($Mode -ieq 'Prepare') {
        $Process = Invoke-AvidScriptUiSaveTool -Executable $Context.editor -LogPath (Join-Path $RunRoot 'prepare.process.log') `
            -Arguments (@($Context.project, '-ExecCmds=AvidScript.PrepareUiSaveDemo exit') + $Common)
        $Evidence = $Process.stdout
        if (Test-Path -LiteralPath $Log) { $Evidence += "`n" + [IO.File]::ReadAllText($Log) }
        if ($Evidence -notmatch 'AVIDSCRIPT_UI_SAVE_DEMO_ASSETS_PASSED\s+root=/AvidScript/Demos/UiSave\s+assets=4\s+mode=(created|validated)(?:\s|$)' -or
            $Evidence.Contains('AVIDSCRIPT_UI_SAVE_DEMO_ASSETS_FAILED')) { throw 'Asset generator success marker is missing or contradictory.' }
        $Result.status = 'prepared'
        return [pscustomobject]$Result
    }
    $Environment = @{
        DOTNET_CLI_HOME = (Join-Path ([IO.Path]::GetTempPath()) "AvidScriptUiSaveDemo/$RunId/dotnet")
        NUGET_PACKAGES = (Join-Path ([IO.Path]::GetTempPath()) 'AvidScriptUiSaveDemo/nuget')
        DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'; DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    }
    $Version = Invoke-AvidScriptUiSaveTool -Executable $Context.dotnet -Arguments @('--version') `
        -LogPath (Join-Path $RunRoot 'dotnet.process.log') -Environment $Environment
    if ($Version.stdout.Trim() -cne '8.0.416') { throw '.NET SDK must resolve to 8.0.416 from plugin cwd.' }
    $BindingRoot = Join-Path $RunRoot 'Bindings'
    $ReportPath = Join-Path $RunRoot 'profile-bindings.json'
    if (Test-Path -LiteralPath $ReportPath) { throw 'Profile binding report must be new.' }
    [void](Invoke-AvidScriptUiSaveTool -Executable $Context.editor -LogPath (Join-Path $RunRoot 'bindings.process.log') `
        -Arguments (@($Context.project, '-run=AvidScriptPublishProfileBindings', "-Profile=$($Context.profile)",
            "-OutputRoot=$BindingRoot", "-Report=$ReportPath") + $Common))
    $Package = Resolve-AvidScriptUiSaveBindingReport -ReportPath $ReportPath -ExpectedProfile $Context.profile -BindingRoot $BindingRoot
    $ReleaseRoot = Join-Path $RunRoot 'Release'
    $ReleaseProcess = Invoke-AvidScriptUiSaveTool -Executable $Context.pwsh -LogPath (Join-Path $RunRoot 'release.process.log') `
        -Environment $Environment -Arguments @('-NoProfile', '-NonInteractive', '-File', (Join-Path $UiSaveBuildRoot 'InvokeAvidScriptRelease.ps1'),
            '-SourcePath', $Context.source, '-CSharpProjectPath', $Context.csharp_project, '-ModuleId', 'avidscript.ui_save_demo',
            '-ArtifactStem', 'ui_save_demo', '-OutputRoot', $ReleaseRoot, '-DotNetPath', $Context.dotnet,
            '-BindingPackagePath', $Package.ManifestPath, '-RuntimeBindingPackagePath', $Package.ManifestPath,
            '-Configuration', 'Development', '-TargetPlatform', 'Win64', '-EngineRoot', $Context.engine)
    $Release = $ReleaseProcess.stdout | ConvertFrom-Json -Depth 32
    if ($Release.schema_version -ne 1 -or $Release.result -cne 'avidscript_module_release_succeeded' -or
        $Release.module_id -cne 'avidscript.ui_save_demo' -or $Release.package_id -cnotmatch '\A[0-9a-f]{64}\z' -or
        $Release.configuration -ine 'Development' -or $Release.target_platform -cne 'win64' -or
        $Release.architecture -cne 'x86_64' -or $Release.target_triple -cne 'x86_64-pc-windows-msvc' -or
        -not [IO.Path]::GetFullPath($Release.build_output_root).Equals($ReleaseRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Module release returned an unexpected identity.'
    }
    $Result.status = 'published'
    $Result.binding_report = $ReportPath
    $Result.binding_manifest = $Package.ManifestPath
    $Result.binding_manifest_sha256 = $Package.ManifestSha256
    $Result.binding_package_hash = $Package.PackageHash
    $Result.package_id = $Release.package_id
    $Result.release = $Release
    return [pscustomobject]$Result
}

if ($MyInvocation.InvocationName -eq '.') { return }
try {
    $Summary = Invoke-AvidScriptUiSaveDemo
    [Console]::Out.WriteLine(($Summary | ConvertTo-Json -Depth 32 -Compress))
    if ($Mode -ieq 'Verify' -and -not $Summary.succeeded) { exit 1 }
    exit 0
}
catch {
    [Console]::Out.WriteLine(([ordered]@{ schema_version = 1; result = 'avidscript_ui_save_demo_failed'
        mode = $Mode.ToLowerInvariant(); message = $_.Exception.Message; gameplay_acceptance = 'not_run' } | ConvertTo-Json -Compress))
    exit 1
}
