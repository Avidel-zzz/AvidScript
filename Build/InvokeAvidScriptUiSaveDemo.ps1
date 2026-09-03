#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Prepare', 'Publish', 'Play', 'Verify', 'VerifyReload')][string]$Mode = 'Prepare',
    [string]$ProfilePath = '',
    [string]$OutputRoot = '',
    [string]$ExpectedPackageId = '',
    [string]$VerifyUserRoot = '',
    [string]$ReloadBaselineManifestPath = '',
    [string]$ReloadChangedManifestPath = '',
    [string]$ReloadRejectedManifestPath = '',
    [ValidateRange(1, 100)][int]$ReloadCycles = 20,
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
        'edges' { $Steps += @(@('collect', 'CollectButton', '1', 'Collected'), @('save', 'SaveButton', '1', 'Saved'),
            @('reset', 'ResetButton', '0', 'Reset'), @('load', 'LoadButton', '1', 'Loaded'),
            @('fixture_wrong_type', 'None', '1', 'Loaded'), @('load', 'LoadButton', '1', 'Wrong save type'),
            @('fixture_negative', 'None', '1', 'Wrong save type'), @('load', 'LoadButton', '1', 'Invalid saved score'),
            @('fixture_overflow', 'None', '1', 'Invalid saved score'), @('load', 'LoadButton', '1', 'Invalid saved score'),
            @('fixture_empty', 'None', '1', 'Invalid saved score'), @('load', 'LoadButton', '1', 'Load failed'),
            @('fixture_valid', 'None', '1', 'Load failed'), @('lock_save', 'None', '1', 'Load failed'),
            @('save_failed', 'SaveButton', '1', 'Save failed'), @('teardown', 'None', '1', 'Save failed'),
            @('late_collect', 'CollectButton', '1', 'Save failed')) }
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

function Test-AvidScriptUiSaveEdgesHash {
    param([object]$Value)
    return $Value -is [string] -and $Value -cmatch '\A[0-9a-f]{64}\z'
}

function Assert-AvidScriptUiSaveEdgesReport {
    param([object]$Report)
    foreach ($Field in @('runtime_snapshot_phase', 'initial_save_sha256', 'save_file_sha256', 'score_text', 'status_text',
        'result', 'failure_category', 'mode', 'process_mode', 'input_kind', 'expected_module_id', 'expected_package_id', 'map')) {
        if ($Report.$Field -isnot [string]) { throw "Edges report field must be a string: $Field" }
    }
    if ($Report.runtime_snapshot_phase -cne 'before_teardown' -or $Report.initial_save_sha256 -cne '' -or
        $Report.runtime -isnot [pscustomobject] -or $Report.startup -isnot [pscustomobject] -or
        $Report.edges -isnot [pscustomobject]) { throw 'Edges snapshot or object evidence is invalid.' }
    foreach ($Field in @('module_id', 'package_id', 'error_message')) {
        if ($Report.runtime.$Field -isnot [string]) { throw "Edges runtime field must be a string: $Field" }
    }
    foreach ($Field in @('scenario_id', 'error_category', 'error_message')) {
        if ($Report.startup.$Field -isnot [string]) { throw "Edges startup field must be a string: $Field" }
    }
    $Count = -1
    if (-not (Try-GetAvidScriptBindingJsonInt32 $Report.runtime.events ([ref]$Count)) -or $Count -ne 9) {
        throw 'Edges before-teardown runtime must contain exactly nine Guest events.'
    }
    $Edges = $Report.edges
    foreach ($Field in @('reset_preserved_save', 'save_failure_preserved_save', 'save_lock_released', 'late_event_ignored')) {
        if ($Edges.$Field -isnot [bool] -or -not $Edges.$Field) { throw "Edges assertion is missing or false: $Field" }
    }
    foreach ($Expected in @(@('invalid_loads_preserved_count', 4), @('late_event_events_before', 9), @('late_event_events_after', 9))) {
        if (-not (Try-GetAvidScriptBindingJsonInt32 $Edges.($Expected[0]) ([ref]$Count)) -or $Count -ne $Expected[1]) {
            throw "Edges count is invalid: $($Expected[0])"
        }
    }
    if (-not (Test-AvidScriptUiSaveEdgesHash $Edges.script_save_sha256) -or
        $Edges.fixtures -isnot [array] -or $Edges.fixtures.Count -ne 5) { throw 'Edges script save or fixture set is invalid.' }
    $Hash = $Edges.script_save_sha256
    $Kinds = @('wrong_type', 'negative', 'overflow', 'empty', 'valid')
    $EmptyHash = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
    for ($Index = 0; $Index -lt $Kinds.Count; ++$Index) {
        $Fixture = $Edges.fixtures[$Index]
        $Bytes = -1
        if ($Fixture -isnot [pscustomobject] -or $Fixture.kind -isnot [string] -or $Fixture.kind -cne $Kinds[$Index] -or
            -not (Test-AvidScriptUiSaveEdgesHash $Fixture.before_sha256) -or $Fixture.before_sha256 -cne $Hash -or
            -not (Test-AvidScriptUiSaveEdgesHash $Fixture.sha256) -or
            -not (Try-GetAvidScriptBindingJsonInt32 $Fixture.bytes ([ref]$Bytes))) { throw "Edges fixture identity/hash chain is invalid: $Index" }
        if ($Fixture.kind -ceq 'empty') {
            if ($Bytes -ne 0 -or $Fixture.sha256 -cne $EmptyHash) { throw 'Edges empty fixture must have the canonical empty SHA-256 and zero bytes.' }
        } elseif ($Bytes -le 0) { throw 'Edges nonempty fixture must contain bytes.' }
        $Hash = $Fixture.sha256
    }
    if ($Hash -cne $Report.save_file_sha256 -or $Edges.fixtures[-1].bytes -ne $Report.save_file_bytes) {
        throw 'Edges final valid fixture differs from the actual final save.'
    }
    foreach ($Action in $Report.actions) {
        if ($Action -isnot [pscustomobject]) { throw 'Edges action must be an object.' }
        foreach ($Field in @('action', 'button', 'expected_score', 'observed_score', 'expected_status', 'observed_status')) {
            if ($Action.$Field -isnot [string]) { throw "Edges action field must be a string: $Field" }
        }
    }
    foreach ($Index in @(6, 8, 10, 12)) {
        if ($Report.actions[$Index].saved_object_preserved -isnot [bool] -or -not $Report.actions[$Index].saved_object_preserved) {
            throw "Edges invalid load did not preserve the prior object: $Index"
        }
    }
    foreach ($Index in @(3, 15)) {
        $Action = $Report.actions[$Index]
        $ExpectedHash = if ($Index -eq 3) { $Edges.script_save_sha256 } else { $Hash }
        if (-not (Test-AvidScriptUiSaveEdgesHash $Action.save_sha256_before) -or
            -not (Test-AvidScriptUiSaveEdgesHash $Action.save_sha256_after) -or
            $Action.save_sha256_before -cne $ExpectedHash -or $Action.save_sha256_after -cne $ExpectedHash) {
            throw "Edges reset/failed save hash preservation is invalid: $Index"
        }
    }
    $Teardown = $Edges.teardown
    if ($Teardown -isnot [pscustomobject] -or $Teardown.kind -isnot [string] -or $Teardown.kind -cne 'component_end_play' -or
        $Teardown.error_message -isnot [string] -or $Teardown.error_message -cne '') { throw 'Edges teardown identity/error is invalid.' }
    foreach ($Field in @('component_end_play', 'guest_end_play', 'owner_released')) {
        if ($Teardown.$Field -isnot [bool] -or -not $Teardown.$Field) { throw "Edges teardown did not complete: $Field" }
    }
    foreach ($Field in @('runtime_loaded', 'owner_resolves', 'session_present', 'widget_in_viewport', 'saved_object_present')) {
        if ($Teardown.$Field -isnot [bool] -or $Teardown.$Field) { throw "Edges teardown retained live state: $Field" }
    }
    foreach ($Expected in @(@('bound_buttons', 0), @('events_before', 9), @('events_after', 9), @('dropped_events', 0))) {
        if (-not (Try-GetAvidScriptBindingJsonInt32 $Teardown.($Expected[0]) ([ref]$Count)) -or $Count -ne $Expected[1]) {
            throw "Edges teardown count is invalid: $($Expected[0])"
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
        if ($Synthetic -and -not ($ProbeMode -ceq 'edges' -and $Step[0] -ceq 'late_collect')) { ++$RequiredEvents }
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
        if ($ProbeMode -in @('write', 'edges')) {
            if ($Report.initial_save_sha256 -cne '') { throw 'Write/edges probe did not start with an empty save.' }
        } elseif ($Report.initial_save_sha256 -cne $WriteHash -or $Report.save_file_sha256 -cne $WriteHash) {
            throw 'Read/GC save hash differs from the preceding write process.'
        }
    }
    if ($ProbeMode -ceq 'edges') { Assert-AvidScriptUiSaveEdgesReport $Report }
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
        $EdgesRoot = Join-Path $UserRoot 'edges'
        New-AvidScriptUiSaveDirectory $WriteRoot
        New-AvidScriptUiSaveDirectory $MissingRoot
        New-AvidScriptUiSaveDirectory $EdgesRoot
        $ProcessIds = [Collections.Generic.HashSet[int]]::new()
        $WriteHash = ''
        foreach ($ProbeMode in @('write', 'read', 'missing', 'gc', 'edges')) {
            $CurrentUserRoot = switch ($ProbeMode) { 'missing' { $MissingRoot }; 'edges' { $EdgesRoot }; default { $WriteRoot } }
            Assert-AvidScriptUiSaveUserRoot $CurrentUserRoot $Context
            $SavePath = Join-Path $CurrentUserRoot 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
            Assert-AvidScriptUiSaveSaveDirectory $SavePath
            if ($ProbeMode -in @('write', 'missing', 'edges')) {
                if (Test-Path -LiteralPath $SavePath) { throw 'Write/missing/edges save must not preexist.' }
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

function Get-AvidScriptUiSaveReloadArtifacts {
    param([object]$Context)
    $Saved = Join-Path (Split-Path -Parent $Context.project) 'Saved'
    $Artifacts = [ordered]@{}
    $Inputs = [ordered]@{ baseline = $ReloadBaselineManifestPath; changed = $ReloadChangedManifestPath; rejected = $ReloadRejectedManifestPath }
    foreach ($Kind in $Inputs.Keys) {
        $Path = $Inputs[$Kind]
        Assert-AvidScriptUiSaveSafePath $Path
        if (-not (Test-AvidScriptBindingPathContained -RootPath $Saved -CandidatePath $Path) -or
            -not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -gt 4MB) {
            throw "Reload $Kind manifest must be a file below project Saved."
        }
        $Manifest = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json -Depth 32
        $Schema = 0
        $Abi = 0
        if (-not (Try-GetAvidScriptBindingJsonInt32 $Manifest.schema_version ([ref]$Schema)) -or $Schema -ne 1 -or
            -not (Try-GetAvidScriptBindingJsonInt32 $Manifest.abi_version ([ref]$Abi)) -or $Abi -ne 1 -or
            $Manifest.module_id -isnot [string] -or $Manifest.module_id -cne 'avidscript.ui_save_demo' -or
            $Manifest.wasm.file -isnot [string] -or [string]::IsNullOrWhiteSpace($Manifest.wasm.file) -or
            -not (Test-AvidScriptUiSaveEdgesHash $Manifest.wasm.sha256)) { throw "Reload $Kind manifest identity is invalid." }
        $WasmReference = $Manifest.wasm.file.Replace('\', '/')
        $WasmPath = Resolve-AvidScriptBindingPath (Split-Path -Parent $Path) $WasmReference
        if (-not [IO.Path]::IsPathFullyQualified($WasmReference)) {
            # Keep the same candidate order as FAvidScriptWasmReloadManifestLoader.
            $ProjectCandidate = Resolve-AvidScriptBindingPath (Split-Path -Parent $Context.project) $WasmReference
            $Candidates = if ($WasmReference -match '\A(?:Saved|Content|Plugins)/') { @($ProjectCandidate, $WasmPath) } else { @($WasmPath, $ProjectCandidate) }
            $WasmPath = $Candidates[0]
            foreach ($Candidate in $Candidates) {
                if (Test-Path -LiteralPath $Candidate -PathType Leaf) { $WasmPath = $Candidate; break }
            }
        }
        Assert-AvidScriptUiSaveSafePath $WasmPath
        if (-not (Test-AvidScriptBindingPathContained -RootPath $Saved -CandidatePath $WasmPath) -or
            -not (Test-Path -LiteralPath $WasmPath -PathType Leaf) -or (Get-Item -LiteralPath $WasmPath).Length -le 0 -or
            (Get-AvidScriptBindingSha256Hex $WasmPath) -cne $Manifest.wasm.sha256) { throw "Reload $Kind WASM path/hash is invalid." }
        $Artifacts[$Kind] = [pscustomobject][ordered]@{ manifest_path = [IO.Path]::GetFullPath($Path)
            manifest_sha256 = Get-AvidScriptBindingSha256Hex $Path; wasm_path = $WasmPath
            wasm_sha256 = $Manifest.wasm.sha256; module_id = $Manifest.module_id }
    }
    if (@($Artifacts.Values.wasm_sha256 | Sort-Object -Unique).Count -ne 3) { throw 'Reload requires three distinct WASM bodies.' }
    return [pscustomobject]$Artifacts
}

function Assert-AvidScriptUiSaveReloadArtifactsUnchanged {
    param([object]$Artifacts)
    foreach ($Kind in @('baseline', 'changed', 'rejected')) {
        $Artifact = $Artifacts.$Kind
        foreach ($Pair in @(@('manifest_path', 'manifest_sha256'), @('wasm_path', 'wasm_sha256'))) {
            Assert-AvidScriptUiSaveSafePath $Artifact.($Pair[0])
            if ((Get-AvidScriptBindingSha256Hex $Artifact.($Pair[0])) -cne $Artifact.($Pair[1])) { throw "Pinned reload $Kind artifact changed." }
        }
    }
}

function Assert-AvidScriptUiSaveReloadEqual {
    param([object]$Actual, [object]$Expected, [string]$Name)
    if ($Expected -is [bool]) { $Valid = $Actual -is [bool] -and $Actual -eq $Expected }
    elseif ($Expected -is [string]) { $Valid = $Actual -is [string] -and $Actual -ceq $Expected }
    else {
        $Number = -1
        $Valid = (Try-GetAvidScriptBindingJsonInt32 $Actual ([ref]$Number)) -and $Number -eq $Expected
    }
    if (-not $Valid) { throw "Reload evidence mismatch: $Name" }
}

function Assert-AvidScriptUiSaveReloadResources {
    param([object]$Resources, [object]$Baseline, [int]$Successes, [int]$Rejections)
    foreach ($Field in @('pending_timers', 'pending_continuations', 'prepared_continuations', 'prepared_subscriptions')) {
        Assert-AvidScriptUiSaveReloadEqual $Resources.$Field 0 $Field
    }
    foreach ($Field in @('active_subscriptions', 'bound_buttons')) { Assert-AvidScriptUiSaveReloadEqual $Resources.$Field 4 $Field }
    foreach ($Field in @('session_present', 'session_preserved')) { Assert-AvidScriptUiSaveReloadEqual $Resources.$Field $true $Field }
    foreach ($Field in @('owned_entries', 'borrowed_entries')) {
        $Count = -1
        $Limit = -1
        if (-not (Try-GetAvidScriptBindingJsonInt32 $Resources.$Field ([ref]$Count)) -or $Count -lt 0 -or
            -not (Try-GetAvidScriptBindingJsonInt32 $Baseline.$Field ([ref]$Limit)) -or $Limit -lt 0 -or $Count -gt $Limit) {
            throw "Reload resource entries grew: $Field"
        }
    }
    Assert-AvidScriptUiSaveReloadEqual $Resources.session_successful_reloads ($Baseline.session_successful_reloads + $Successes) 'session successes'
    Assert-AvidScriptUiSaveReloadEqual $Resources.session_rejected_reloads ($Baseline.session_rejected_reloads + $Rejections) 'session rejections'
}

function Resolve-AvidScriptUiSaveReloadReport {
    param([string]$ReportPath, [string]$UserRoot, [string]$PackageId, [object]$Artifacts, [int]$Cycles)
    Assert-AvidScriptUiSaveSafePath $ReportPath
    if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf) -or (Get-Item -LiteralPath $ReportPath).Length -gt 4MB) {
        throw 'Reload probe report is missing or oversized.'
    }
    $Report = Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json -Depth 32
    $Expected = [ordered]@{ schema_version = 1; result = 'avidscript_ui_save_probe_passed'; succeeded = $true; failure_category = ''
        mode = 'reload'; process_mode = 'editor_binary_game'; input_kind = 'synthetic_ue_button_onclicked_broadcast'
        expected_module_id = 'avidscript.ui_save_demo'; expected_package_id = $PackageId; map = '/AvidScript/Demos/UiSave/L_UiSave'
        physical_click_verified = $false; visual_verified = $false; long_run_verified = $false; gc_performed = $false
        runtime_snapshot_phase = 'before_teardown'; save_file_exists = $false; save_file_bytes = 0; initial_save_sha256 = ''; save_file_sha256 = '' }
    foreach ($Field in $Expected.Keys) { Assert-AvidScriptUiSaveReloadEqual $Report.$Field $Expected[$Field] $Field }
    $ProcessId = 0
    if (-not (Try-GetAvidScriptBindingJsonInt32 $Report.process_id ([ref]$ProcessId)) -or $ProcessId -le 0) { throw 'Reload process id is invalid.' }
    foreach ($Elapsed in @($Report.elapsed_seconds, $Report.reload.elapsed_seconds)) {
        if (($Elapsed -isnot [double] -and $Elapsed -isnot [long] -and $Elapsed -isnot [int] -and $Elapsed -isnot [decimal]) -or
            -not [double]::IsFinite([double]$Elapsed) -or $Elapsed -lt 0 -or $Elapsed -gt (30 + 6 * $Cycles)) { throw 'Reload elapsed time is invalid.' }
    }
    Assert-AvidScriptUiSaveReloadEqual $Report.timeout_seconds (30 + 6 * $Cycles) 'timeout'
    $SavePath = Join-Path $UserRoot 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
    if (-not (Test-AvidScriptUiSaveSamePath $Report.user_dir $UserRoot) -or
        -not (Test-AvidScriptUiSaveSamePath $Report.save_path $SavePath)) { throw 'Reload report escaped its isolated paths.' }
    Assert-AvidScriptUiSaveSaveDirectory $SavePath
    if (Test-Path -LiteralPath $SavePath) { throw 'Reload probe unexpectedly created a save.' }
    foreach ($Field in @('scenario_id', 'error_category', 'error_message')) {
        Assert-AvidScriptUiSaveReloadEqual $Report.startup.$Field $(if ($Field -eq 'scenario_id') { 'ui_save_demo' } else { '' }) "startup $Field"
    }
    Assert-AvidScriptUiSaveReloadEqual $Report.startup.active $true 'startup active'
    $Reload = $Report.reload
    foreach ($Field in @('requested_cycles', 'completed_cycles', 'successful_reloads', 'rejected_reloads')) {
        Assert-AvidScriptUiSaveReloadEqual $Reload.$Field $Cycles $Field
    }
    foreach ($Field in @('configuration_restored', 'artifacts_unchanged', 'late_event_ignored')) { Assert-AvidScriptUiSaveReloadEqual $Reload.$Field $true $Field }
    foreach ($Field in @('instance_pointer_identity_measured', 'gc_memory_measured')) { Assert-AvidScriptUiSaveReloadEqual $Reload.$Field $false $Field }
    Assert-AvidScriptUiSaveReloadEqual $Reload.object_count_kind 'registered_entries_not_live_gc_memory' 'object count scope'
    Assert-AvidScriptUiSaveReloadEqual $Reload.identity_evidence 'manifest_and_wasm_hashes_component_stats_session_identity_reload_result_and_observed_behavior' 'identity scope'
    Assert-AvidScriptUiSaveReloadEqual $Reload.startup_package_id $PackageId 'startup package'
    Assert-AvidScriptUiSaveReloadEqual $Reload.startup_wasm_sha256 $Artifacts.baseline.wasm_sha256 'startup body'
    foreach ($Kind in @('baseline', 'changed', 'rejected')) {
        foreach ($Field in @('module_id', 'manifest_sha256', 'wasm_sha256')) {
            Assert-AvidScriptUiSaveReloadEqual $Reload.artifacts.$Kind.$Field $Artifacts.$Kind.$Field "$Kind $Field"
        }
        foreach ($Field in @('manifest_path', 'wasm_path')) {
            if (-not (Test-AvidScriptUiSaveSamePath $Reload.artifacts.$Kind.$Field $Artifacts.$Kind.$Field)) { throw "Reload artifact path mismatch: $Kind $Field" }
        }
    }
    if ($Reload.cycles -isnot [array] -or $Reload.cycles.Count -ne $Cycles) { throw 'Reload cycle records are incomplete.' }
    Assert-AvidScriptUiSaveReloadEqual $Reload.resources_baseline.session_successful_reloads 0 'initial session reloads'
    Assert-AvidScriptUiSaveReloadEqual $Reload.resources_baseline.session_rejected_reloads 0 'initial session rejections'
    Assert-AvidScriptUiSaveReloadResources $Reload.resources_baseline $Reload.resources_baseline 0 0
    $Steps = @(@('ready', 'None', '0', 'Ready'), @('collect', 'CollectButton', '1', 'Collected'))
    $Score = 1
    $PreviousHash = $Artifacts.baseline.wasm_sha256
    for ($Index = 0; $Index -lt $Cycles; ++$Index) {
        $Cycle = $Reload.cycles[$Index]
        $Target = if ($Index % 2 -eq 0) { 'changed' } else { 'baseline' }
        $Delta = if ($Target -ceq 'changed') { 2 } else { 1 }
        foreach ($Pair in @(@('cycle', ($Index + 1)), @('target', $Target), @('delta', $Delta), @('before_score', "$Score"),
            @('ready_score', "$Score"), @('previous_wasm_sha256', $PreviousHash), @('active_wasm_sha256', $Artifacts.$Target.wasm_sha256),
            @('passed', $true), @('ready_after_tick', $true), @('rejection_tick_verified', $true))) {
            Assert-AvidScriptUiSaveReloadEqual $Cycle.($Pair[0]) $Pair[1] "cycle $Index $($Pair[0])"
        }
        foreach ($Field in @('returned', 'succeeded', 'applied', 'state_migrated', 'host_effects_attempted', 'host_effects_committed')) {
            Assert-AvidScriptUiSaveReloadEqual $Cycle.reload.$Field $true "commit $Field"
        }
        $Slots = 0
        if (-not (Try-GetAvidScriptBindingJsonInt32 $Cycle.reload.migrated_slots ([ref]$Slots)) -or $Slots -lt 1) { throw 'Reload did not migrate score state.' }
        foreach ($Field in @('error_category', 'error_message')) { Assert-AvidScriptUiSaveReloadEqual $Cycle.reload.$Field '' "commit $Field" }
        foreach ($Outcome in @($Cycle.reload, $Cycle.rejection)) {
            foreach ($Field in @('previous_module_id', 'candidate_module_id', 'active_module_id')) {
                Assert-AvidScriptUiSaveReloadEqual $Outcome.$Field 'avidscript.ui_save_demo' $Field
            }
        }
        foreach ($Field in @('returned', 'succeeded', 'applied', 'host_effects_committed')) { Assert-AvidScriptUiSaveReloadEqual $Cycle.rejection.$Field $false "rejection $Field" }
        Assert-AvidScriptUiSaveReloadEqual $Cycle.rejection.rollback_preserved_runtime $true 'rejection rollback'
        Assert-AvidScriptUiSaveReloadEqual $Cycle.rejection.host_effects_attempted $true 'rejection prepare'
        if ($Cycle.rejection.error_category -isnot [string] -or [string]::IsNullOrEmpty($Cycle.rejection.error_category) -or
            $Cycle.rejection.error_message -isnot [string] -or -not $Cycle.rejection.error_message.Contains('binding_reload_effect_unsupported') -or
            $Cycle.component_rejection_error -isnot [string] -or -not $Cycle.component_rejection_error.Contains('binding_reload_effect_unsupported')) {
            throw 'Rejected candidate failed for the wrong reason, or lost its host error details.'
        }
        $DispatchFrame = 0
        $CheckFrame = 0
        if (-not (Try-GetAvidScriptBindingJsonInt32 $Cycle.rejection_dispatch_frame ([ref]$DispatchFrame)) -or $DispatchFrame -lt 0 -or
            -not (Try-GetAvidScriptBindingJsonInt32 $Cycle.rejection_check_frame ([ref]$CheckFrame)) -or $CheckFrame -le ($DispatchFrame + 1)) {
            throw 'Rejected candidate was not observed across a safe Tick.'
        }
        Assert-AvidScriptUiSaveReloadEqual $Cycle.events_before_rejection 1 'events before rejection'
        Assert-AvidScriptUiSaveReloadEqual $Cycle.events_after_rejection 1 'events after rejection'
        Assert-AvidScriptUiSaveReloadResources $Cycle.resources_ready $Reload.resources_baseline ($Index + 1) $Index
        Assert-AvidScriptUiSaveReloadResources $Cycle.resources_after_rejection $Reload.resources_baseline ($Index + 1) ($Index + 1)
        Assert-AvidScriptUiSaveReloadResources $Cycle.resources_after_collect $Reload.resources_baseline ($Index + 1) ($Index + 1)
        $Steps += ,@("reload_$Target", 'None', "$Score", 'Ready')
        $Score += $Delta
        Assert-AvidScriptUiSaveReloadEqual $Cycle.collect_score "$Score" 'new body collect'
        Assert-AvidScriptUiSaveReloadEqual $Cycle.rejection_score "$Score" 'rejection preserves score'
        $Steps += @(@('collect', 'CollectButton', "$Score", 'Collected'), @('reject', 'None', "$Score", 'Collected'))
        $Score += $Delta
        Assert-AvidScriptUiSaveReloadEqual $Cycle.after_rejection_collect_score "$Score" 'old body collect after rejection'
        $Steps += ,@('collect_after_reject', 'CollectButton', "$Score", 'Collected')
        $PreviousHash = $Artifacts.$Target.wasm_sha256
    }
    $Steps += @(@('teardown', 'None', "$Score", 'Collected'), @('late_collect', 'CollectButton', "$Score", 'Collected'))
    if ($Report.actions -isnot [array] -or $Report.actions.Count -ne $Steps.Count) { throw 'Reload actions are incomplete.' }
    $PriorScore = '0'
    $PriorStatus = 'Ready'
    $PriorFrame = -1
    for ($Index = 0; $Index -lt $Steps.Count; ++$Index) {
        $Action = $Report.actions[$Index]
        $Step = $Steps[$Index]
        $Fields = [ordered]@{ action = $Step[0]; button = $Step[1]; expected_score = $Step[2]; observed_score = $Step[2]
            expected_status = $Step[3]; observed_status = $Step[3]; before_score = $PriorScore; before_status = $PriorStatus
            passed = $true; synthetic_ue_event = ($Step[1] -cne 'None') }
        foreach ($Field in $Fields.Keys) { Assert-AvidScriptUiSaveReloadEqual $Action.$Field $Fields[$Field] "action $Index $Field" }
        $StartFrame = 0
        $EndFrame = 0
        if (-not (Try-GetAvidScriptBindingJsonInt32 $Action.dispatch_frame ([ref]$StartFrame)) -or $StartFrame -le $PriorFrame -or
            -not (Try-GetAvidScriptBindingJsonInt32 $Action.check_frame ([ref]$EndFrame)) -or $EndFrame -le ($StartFrame + 1)) { throw 'Reload action frame order is invalid.' }
        if ($Step[0] -ceq 'reject') {
            $Record = $Reload.cycles[[int](($Index - 4) / 4)]
            Assert-AvidScriptUiSaveReloadEqual $Record.rejection_dispatch_frame $StartFrame 'rejection dispatch frame'
            Assert-AvidScriptUiSaveReloadEqual $Record.rejection_check_frame $EndFrame 'rejection check frame'
        }
        $PriorScore = $Step[2]; $PriorStatus = $Step[3]; $PriorFrame = $EndFrame
    }
    Assert-AvidScriptUiSaveReloadEqual $Report.score_text "$Score" 'final score'
    Assert-AvidScriptUiSaveReloadEqual $Report.status_text 'Collected' 'final status'
    foreach ($Field in @('runtime_loaded', 'begin_play', 'owner_registered', 'owner_handle_valid')) { Assert-AvidScriptUiSaveReloadEqual $Report.runtime.$Field $true $Field }
    Assert-AvidScriptUiSaveReloadEqual $Report.runtime.resolved_from_package $false 'loose runtime'
    Assert-AvidScriptUiSaveReloadEqual $Report.runtime.package_id '' 'loose package must not claim startup package'
    Assert-AvidScriptUiSaveReloadEqual $Report.runtime.module_id 'avidscript.ui_save_demo' 'live module'
    Assert-AvidScriptUiSaveReloadEqual $Report.runtime.events 2 'final active Guest events'
    Assert-AvidScriptUiSaveReloadEqual $Report.runtime.dropped_events 0 'dropped events'
    Assert-AvidScriptUiSaveReloadEqual $Report.runtime.error_message $Reload.cycles[-1].component_rejection_error 'retained rejection error'
    if (-not (Test-AvidScriptUiSaveSamePath $Report.runtime.script_manifest_path $Artifacts.$Target.manifest_path)) { throw 'Active loose runtime path is incorrect.' }
    Assert-AvidScriptUiSaveReloadResources $Reload.resources_before_teardown $Reload.resources_baseline $Cycles $Cycles
    $Teardown = $Reload.teardown
    Assert-AvidScriptUiSaveReloadEqual $Teardown.kind 'component_end_play' 'teardown kind'
    foreach ($Field in @('component_end_play', 'guest_end_play', 'owner_released', 'resource_owner_destroyed')) { Assert-AvidScriptUiSaveReloadEqual $Teardown.$Field $true "teardown $Field" }
    foreach ($Field in @('runtime_loaded', 'owner_resolves', 'session_present', 'widget_in_viewport', 'saved_object_present', 'resource_counts_measured')) {
        Assert-AvidScriptUiSaveReloadEqual $Teardown.$Field $false "teardown $Field"
    }
    foreach ($Field in @('bound_buttons', 'dropped_events')) { Assert-AvidScriptUiSaveReloadEqual $Teardown.$Field 0 "teardown $Field" }
    foreach ($Field in @('events_before', 'events_after')) { Assert-AvidScriptUiSaveReloadEqual $Teardown.$Field 2 "teardown $Field" }
    Assert-AvidScriptUiSaveReloadEqual $Teardown.error_message $Reload.cycles[-1].component_rejection_error 'teardown retained error'
    Assert-AvidScriptUiSaveReloadEqual $Reload.late_event_events_before 2 'late event before'
    Assert-AvidScriptUiSaveReloadEqual $Reload.late_event_events_after 2 'late event after'
    return $Report
}

function Invoke-AvidScriptUiSaveVerifyReload {
    param([object]$Context, [string]$RunRoot, [string]$RunId, [string]$UserRoot, [object]$Artifacts)
    $Summary = [ordered]@{ schema_version = 1; result = 'avidscript_ui_save_reload_verify_failed'; status = 'failed'; succeeded = $false
        mode = 'verifyreload'; run_id = $RunId; module_id = 'avidscript.ui_save_demo'; startup_package_id = $ExpectedPackageId.ToLowerInvariant()
        evidence_root = $RunRoot; verify_user_root = $UserRoot; report_path = (Join-Path $RunRoot 'verify-reload.json')
        probe_report_path = (Join-Path $RunRoot 'reload.json'); editor_log = (Join-Path $RunRoot 'reload.editor.log')
        process_mode = 'editor_binary_game'; input_kind = 'synthetic_ue_button_onclicked_broadcast'; gameplay_acceptance = 'bounded_editor_reload_probe_only'
        physical_click_verified = $false; visual_verified = $false; long_run_verified = $false; packaged_verified = $false
        reload_cycles = $ReloadCycles; artifacts = $Artifacts; artifacts_unchanged = $false; message = '' }
    try {
        New-AvidScriptUiSaveDirectory $UserRoot
        $ReloadRoot = Join-Path $UserRoot 'reload'
        New-AvidScriptUiSaveDirectory $ReloadRoot
        Assert-AvidScriptUiSaveUserRoot $ReloadRoot $Context
        Assert-AvidScriptUiSaveReloadArtifactsUnchanged $Artifacts
        Write-AvidScriptUiSaveNewJson (Join-Path $RunRoot 'reload-inputs.json') $Artifacts
        $Arguments = @($Context.project, '/AvidScript/Demos/UiSave/L_UiSave', '-game', '-ExecCmds=Module Load AvidScriptEditor',
            '-AvidScriptScenario=ui_save_demo', '-AvidScriptUiSaveProbe=reload', "-AvidScriptUiSaveReport=$($Summary.probe_report_path)",
            "-AvidScriptUiSaveExpectedPackage=$($Summary.startup_package_id)", "-AvidScriptUiSaveReloadCycles=$ReloadCycles", "-UserDir=$ReloadRoot",
            '-unattended', '-nullrhi', '-nosound', '-nop4', '-nosplash', '-stdout', '-FullStdOutLogOutput', "-abslog=$($Summary.editor_log)")
        foreach ($Kind in @('baseline', 'changed', 'rejected')) {
            $Arguments += @("-AvidScriptUiSaveReload$($Kind)Manifest=$($Artifacts.$Kind.manifest_path)",
                "-AvidScriptUiSaveReload$($Kind)ManifestSha256=$($Artifacts.$Kind.manifest_sha256)",
                "-AvidScriptUiSaveReload$($Kind)WasmSha256=$($Artifacts.$Kind.wasm_sha256)")
        }
        [void](Invoke-AvidScriptUiSaveTool -Executable $Context.editor -Arguments $Arguments `
            -LogPath (Join-Path $RunRoot 'reload.process.log') -ProcessTimeoutSeconds ([Math]::Min($TimeoutSeconds, (60 + 6 * $ReloadCycles))))
        $Summary.evidence = Resolve-AvidScriptUiSaveReloadReport $Summary.probe_report_path $ReloadRoot $Summary.startup_package_id $Artifacts $ReloadCycles
        $Summary.probe_report_sha256 = Get-AvidScriptBindingSha256Hex $Summary.probe_report_path
        $Summary.result = 'avidscript_ui_save_reload_verify_passed'
        $Summary.status = 'verified'
        $Summary.succeeded = $true
    } catch { $Summary.message = $_.Exception.Message }
    try {
        Assert-AvidScriptUiSaveReloadArtifactsUnchanged $Artifacts
        $Summary.artifacts_unchanged = $true
    } catch {
        $Summary.succeeded = $false; $Summary.status = 'failed'; $Summary.result = 'avidscript_ui_save_reload_verify_failed'
        $Summary.message += " Pinned artifacts verification: $($_.Exception.Message)"
    }
    Write-AvidScriptUiSaveNewJson $Summary.report_path $Summary
    return [pscustomobject]$Summary
}

function Invoke-AvidScriptUiSaveDemo {
    if ($Mode -in @('Verify', 'VerifyReload') -and $ExpectedPackageId -notmatch '\A[0-9a-fA-F]{64}\z') { throw 'Verify requires ExpectedPackageId (64hex).' }
    $Context = Get-AvidScriptUiSaveContext
    if ($Mode -ieq 'VerifyReload') { $ReloadArtifacts = Get-AvidScriptUiSaveReloadArtifacts $Context }
    $RunId = [Guid]::NewGuid().ToString('N')
    $RunRoot = Join-Path $Context.output_root $RunId
    if ($Mode -in @('Verify', 'VerifyReload')) {
        $UserRoot = if ($VerifyUserRoot) { $VerifyUserRoot } else { Join-Path ([IO.Path]::GetTempPath()) "AvidScriptUiSaveVerify/$RunId" }
        Assert-AvidScriptUiSaveUserRoot $UserRoot $Context
        $UserRoot = [IO.Path]::GetFullPath($UserRoot)
    }
    New-AvidScriptUiSaveDirectory $RunRoot
    if ($Mode -ieq 'Verify') { return Invoke-AvidScriptUiSaveVerify $Context $RunRoot $RunId $UserRoot }
    if ($Mode -ieq 'VerifyReload') { return Invoke-AvidScriptUiSaveVerifyReload $Context $RunRoot $RunId $UserRoot $ReloadArtifacts }
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
    if ($Mode -in @('Verify', 'VerifyReload') -and -not $Summary.succeeded) { exit 1 }
    exit 0
}
catch {
    [Console]::Out.WriteLine(([ordered]@{ schema_version = 1; result = 'avidscript_ui_save_demo_failed'
        mode = $Mode.ToLowerInvariant(); message = $_.Exception.Message; gameplay_acceptance = 'not_run' } | ConvertTo-Json -Compress))
    exit 1
}
