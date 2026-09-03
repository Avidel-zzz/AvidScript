#requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Runner = Join-Path (Split-Path -Parent $PSScriptRoot) 'InvokeAvidScriptUiSaveDemo.ps1'
. $Runner
$Passed = 0
$Total = 0
$Failures = [Collections.Generic.List[string]]::new()

function Assert-UiSaveContract {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-UiSaveCase {
    param([string]$Name, [scriptblock]$Body)
    ++$script:Total
    try { & $Body; ++$script:Passed }
    catch { $script:Failures.Add("$Name`: $($_.Exception.Message)") }
}

function Write-UiSaveFixtureJson {
    param([string]$Path, [object]$Object)
    [IO.File]::WriteAllText($Path, ($Object | ConvertTo-Json -Depth 32), [Text.UTF8Encoding]::new($false))
}

$EdgesFaults = [ordered]@{
    'missing edges' = { param($Report) $Report.Remove('edges') }
    'snapshot phase' = { param($Report) $Report.runtime_snapshot_phase = 'after_teardown' }
    'extra Guest event' = { param($Report) $Report.runtime.events = 10 }
    'GC claim' = { param($Report) $Report.gc_performed = $true }
    'missing action' = { param($Report) $Report.actions = @($Report.actions | Select-Object -SkipLast 1) }
    'action order' = { param($Report) $First = $Report.actions[1]; $Report.actions[1] = $Report.actions[2]; $Report.actions[2] = $First }
    'numeric score' = { param($Report) $Report.actions[1].observed_score = 1 }
    'lost saved object' = { param($Report) $Report.actions[8].saved_object_preserved = $false }
    'reset changed save' = { param($Report) $Report.actions[3].save_sha256_after = 'a' * 64 }
    'forged save preservation' = { param($Report) $Report.actions[15].save_sha256_before = 'a' * 64; $Report.actions[15].save_sha256_after = 'a' * 64 }
    'missing fixture' = { param($Report) $Report.edges.fixtures = @($Report.edges.fixtures | Select-Object -SkipLast 1) }
    'extra fixture' = { param($Report) $Report.edges.fixtures += $Report.edges.fixtures[-1] }
    'fixture order' = { param($Report) $First = $Report.edges.fixtures[1]; $Report.edges.fixtures[1] = $Report.edges.fixtures[2]; $Report.edges.fixtures[2] = $First }
    'broken fixture chain' = { param($Report) $Report.edges.fixtures[1].before_sha256 = 'a' * 64 }
    'empty hash' = { param($Report) $Report.edges.fixtures[3].sha256 = 'a' * 64; $Report.edges.fixtures[4].before_sha256 = 'a' * 64 }
    'empty bytes' = { param($Report) $Report.edges.fixtures[3].bytes = 1 }
    'string fixture bytes' = { param($Report) $Report.edges.fixtures[0].bytes = '10' }
    'final fixture bytes' = { param($Report) ++$Report.edges.fixtures[4].bytes }
    'missing teardown' = { param($Report) $Report.edges.Remove('teardown') }
    'fake cleanup' = { param($Report) $Report.edges.teardown.owner_resolves = $true }
    'wrong teardown kind' = { param($Report) $Report.edges.teardown.kind = 'runtime_unload' }
    'string teardown count' = { param($Report) $Report.edges.teardown.bound_buttons = '0' }
    'string boolean' = { param($Report) $Report.edges.save_lock_released = 'true' }
    'invalid loads count' = { param($Report) $Report.edges.invalid_loads_preserved_count = 3 }
    'late event entered Guest' = { param($Report) $Report.edges.late_event_events_after = 10 }
    'final file tamper' = { param($Report, $SavePath) [IO.File]::AppendAllText($SavePath, 'changed') }
}

function Invoke-UiSaveFixture {
    param([string]$Mode, [string]$Fault = '')
    $Root = Join-Path ([IO.Path]::GetTempPath()) "AvidScriptUiSaveContract_$([Guid]::NewGuid().ToString('N'))"
    [void][IO.Directory]::CreateDirectory($Root)
    try {
        $ExpectedPackageId = if ($Fault -ceq 'expected package missing') { '' } elseif ($Fault -ceq 'expected package malformed') { 'bad' } else { 'd' * 64 }
        $VerifyUserRoot = Join-Path $Root 'Users'
        $Context = [pscustomobject]@{
            project = (Join-Path $Root 'Project/Fixture Project.uproject'); editor = 'fixture-editor'
            output_root = $Root; engine = 'C:\UnrealEngine'; dotnet = 'fixture-dotnet'; pwsh = 'fixture-pwsh'
            profile = (Join-Path $Root 'Profile With Spaces.json'); source = (Join-Path $Root 'Script.cs')
            csharp_project = (Join-Path $Root 'Script.csproj')
        }
        $Calls = [Collections.Generic.List[object]]::new()
        $PlayCalls = [Collections.Generic.List[object]]::new()
        if ($Fault -ceq 'existing user root') { [void][IO.Directory]::CreateDirectory($VerifyUserRoot) }
        if ($Fault -ceq 'project user root') { $VerifyUserRoot = Join-Path $Root 'Project/Users' }
        if ($Fault -ceq 'engine user root') { $VerifyUserRoot = Join-Path $Context.engine 'VerifyUsers' }
        function Get-AvidScriptUiSaveContext { return $Context }
        function Start-AvidScriptUiSavePlay {
            param($Executable, $Arguments)
            $PlayCalls.Add([pscustomobject]@{ executable = $Executable; arguments = $Arguments })
            return 12345
        }
        function Invoke-AvidScriptAndroidProcess {
            param($Executable, $Arguments, $WorkingDirectory, $TimeoutSeconds, $Environment)
            $Calls.Add([pscustomobject]@{ executable = $Executable; arguments = $Arguments
                cwd = $WorkingDirectory; timeout = $TimeoutSeconds; environment = $Environment })
            if ($Fault -ceq 'timeout') { throw 'Fixture child exceeded its timeout.' }
            if (@($Arguments | Where-Object { $_ -like '-AvidScriptUiSaveProbe=*' }).Count -eq 1) {
                $ProbeMode = ($Arguments | Where-Object { $_.StartsWith('-AvidScriptUiSaveProbe=') }).Substring(23)
                $ReportPath = ($Arguments | Where-Object { $_.StartsWith('-AvidScriptUiSaveReport=') }).Substring(24)
                $UserRoot = ($Arguments | Where-Object { $_.StartsWith('-UserDir=') }).Substring(9)
                $Log = ($Arguments | Where-Object { $_.StartsWith('-abslog=') }).Substring(8)
                Assert-UiSaveContract (-not (Test-Path -LiteralPath $ReportPath)) 'Verify report was reused.'
                $SavePath = Join-Path $UserRoot 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
                $InitialHash = ''
                if ($ProbeMode -in @('read', 'gc')) {
                    Assert-UiSaveContract (Test-Path -LiteralPath $SavePath) 'Read/GC did not reuse the write save.'
                    $InitialHash = Get-AvidScriptBindingSha256Hex $SavePath
                } else { Assert-UiSaveContract (-not (Test-Path -LiteralPath $SavePath)) 'Write/missing/edges inherited a save.' }
                if ($ProbeMode -ceq 'write' -or ($ProbeMode -ceq 'missing' -and $Fault -ceq 'missing creates save')) {
                    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $SavePath))
                    [IO.File]::WriteAllText($SavePath, 'fixture score=3')
                }
                if ($ProbeMode -in @('read', 'gc') -and $Fault -ceq "$ProbeMode mutates save") { [IO.File]::AppendAllText($SavePath, 'changed') }
                if ($ProbeMode -ceq 'edges') {
                    Assert-UiSaveContract (@(Get-ChildItem -LiteralPath $UserRoot -Force).Count -eq 0) 'Edges UserDir was not fresh.'
                    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $SavePath))
                    [IO.File]::WriteAllText($SavePath, 'fixture script score=1')
                    $ScriptSaveHash = Get-AvidScriptBindingSha256Hex $SavePath
                    $Fixtures = @()
                    foreach ($Kind in @('wrong_type', 'negative', 'overflow', 'empty', 'valid')) {
                        $BeforeHash = Get-AvidScriptBindingSha256Hex $SavePath
                        $Content = if ($Kind -ceq 'empty') { '' } else { "fixture $Kind score=1" }
                        [IO.File]::WriteAllText($SavePath, $Content, [Text.UTF8Encoding]::new($false))
                        $Fixtures += [ordered]@{ kind = $Kind; before_sha256 = $BeforeHash
                            sha256 = (Get-AvidScriptBindingSha256Hex $SavePath); bytes = (Get-Item -LiteralPath $SavePath).Length }
                    }
                }
                $Steps = switch ($ProbeMode) {
                    'write' { ,@(@('ready', 'None', '0', 'Ready'), @('collect', 'CollectButton', '1', 'Collected'),
                        @('collect', 'CollectButton', '2', 'Collected'), @('collect', 'CollectButton', '3', 'Collected'), @('save', 'SaveButton', '3', 'Saved')) }
                    'read' { ,@(@('ready', 'None', '0', 'Ready'), @('load', 'LoadButton', '3', 'Loaded')) }
                    'missing' { ,@(@('ready', 'None', '0', 'Ready'), @('load', 'LoadButton', '0', 'No saved score')) }
                    'gc' { ,@(@('ready', 'None', '0', 'Ready'), @('load', 'LoadButton', '3', 'Loaded'),
                        @('collect_garbage', 'None', '3', 'Loaded'), @('collect', 'CollectButton', '4', 'Collected')) }
                    'edges' { ,@(@('ready', 'None', '0', 'Ready'), @('collect', 'CollectButton', '1', 'Collected'),
                        @('save', 'SaveButton', '1', 'Saved'), @('reset', 'ResetButton', '0', 'Reset'),
                        @('load', 'LoadButton', '1', 'Loaded'), @('fixture_wrong_type', 'None', '1', 'Loaded'),
                        @('load', 'LoadButton', '1', 'Wrong save type'), @('fixture_negative', 'None', '1', 'Wrong save type'),
                        @('load', 'LoadButton', '1', 'Invalid saved score'), @('fixture_overflow', 'None', '1', 'Invalid saved score'),
                        @('load', 'LoadButton', '1', 'Invalid saved score'), @('fixture_empty', 'None', '1', 'Invalid saved score'),
                        @('load', 'LoadButton', '1', 'Load failed'), @('fixture_valid', 'None', '1', 'Load failed'),
                        @('lock_save', 'None', '1', 'Load failed'), @('save_failed', 'SaveButton', '1', 'Save failed'),
                        @('teardown', 'None', '1', 'Save failed'), @('late_collect', 'CollectButton', '1', 'Save failed')) }
                }
                $Actions = @($Steps | ForEach-Object { [ordered]@{ action = $_[0]; button = $_[1]; expected_score = $_[2]
                    expected_status = $_[3]; observed_score = $_[2]; observed_status = $_[3]; passed = $true; synthetic_ue_event = ($_[1] -cne 'None') } })
                $Exists = Test-Path -LiteralPath $SavePath -PathType Leaf
                $Report = [ordered]@{ schema_version = 1; result = 'avidscript_ui_save_probe_passed'; succeeded = $true; failure_category = ''
                    mode = $ProbeMode; process_mode = 'editor_binary_game'; process_id = (10000 + $Calls.Count)
                    input_kind = 'synthetic_ue_button_onclicked_broadcast'; physical_click_verified = $false; visual_verified = $false; long_run_verified = $false
                    expected_module_id = 'avidscript.ui_save_demo'; expected_package_id = ('d' * 64); map = '/AvidScript/Demos/UiSave/L_UiSave'
                    user_dir = $UserRoot; save_path = $SavePath; initial_save_sha256 = $InitialHash
                    save_file_sha256 = $(if ($Exists) { Get-AvidScriptBindingSha256Hex $SavePath } else { '' })
                    save_file_bytes = $(if ($Exists) { (Get-Item -LiteralPath $SavePath).Length } else { 0 }); save_file_exists = $Exists
                    score_text = $Steps[-1][2]; status_text = $Steps[-1][3]; gc_performed = ($ProbeMode -ceq 'gc'); actions = $Actions
                    runtime = [ordered]@{ module_id = 'avidscript.ui_save_demo'; package_id = ('d' * 64); resolved_from_package = $true
                        runtime_loaded = $true; begin_play = $true; owner_registered = $true; owner_handle_valid = $true
                        error_message = ''; dropped_events = 0; events = 4 }
                    startup = @{ active = $true; scenario_id = 'ui_save_demo'; error_category = ''; error_message = '' } }
                if ($ProbeMode -ceq 'edges') {
                    $Report.runtime.events = 9
                    $Report.runtime_snapshot_phase = 'before_teardown'
                    $Report.edges = [ordered]@{ script_save_sha256 = $ScriptSaveHash; reset_preserved_save = $true
                        invalid_loads_preserved_count = 4; save_failure_preserved_save = $true; save_lock_released = $true
                        fixtures = $Fixtures; late_event_ignored = $true; late_event_events_before = 9; late_event_events_after = 9
                        teardown = [ordered]@{ kind = 'component_end_play'; component_end_play = $true; guest_end_play = $true
                            runtime_loaded = $false; owner_released = $true; owner_resolves = $false; session_present = $false
                            widget_in_viewport = $false; saved_object_present = $false; bound_buttons = 0
                            events_before = 9; events_after = 9; dropped_events = 0; error_message = '' } }
                    foreach ($Index in @(6, 8, 10, 12)) { $Report.actions[$Index].saved_object_preserved = $true }
                    $Report.actions[3].save_sha256_before = $ScriptSaveHash
                    $Report.actions[3].save_sha256_after = $ScriptSaveHash
                    $Report.actions[15].save_sha256_before = $Report.save_file_sha256
                    $Report.actions[15].save_sha256_after = $Report.save_file_sha256
                    if ($EdgesFaults.Contains($Fault)) { & $EdgesFaults[$Fault] $Report $SavePath }
                }
                switch ($Fault) {
                    'failed report exit zero' { $Report.result = 'avidscript_ui_save_probe_failed'; $Report.succeeded = $false; $Report.failure_category = 'missing avid_on_tick' }
                    'probe identity' { $Report.runtime.package_id = 'e' * 64 }
                    'probe mode' { $Report.mode = 'other' }
                    'duplicate pid' { $Report.process_id = 10001 }
                    'probe path' { $Report.save_path = Join-Path $Root 'wrong.sav' }
                    'runtime inactive' { $Report.runtime.runtime_loaded = $false }
                    'dropped events' { $Report.runtime.dropped_events = 1 }
                    'empty actions' { $Report.actions = @() }
                    'action failed' { $Report.actions[0].passed = $false }
                    'action score' { $Report.actions[-1].observed_score = '999' }
                    'final status' { $Report.status_text = 'Wrong' }
                    'physical claim' { $Report.physical_click_verified = $true }
                    'save hash' { $Report.save_file_sha256 = 'e' * 64 }
                    'save size' { ++$Report.save_file_bytes }
                    'extra save' { [IO.File]::WriteAllText((Join-Path (Split-Path -Parent $SavePath) 'other.sav'), 'other') }
                    'gc not performed' { $Report.gc_performed = $false }
                }
                if ($Fault -cne 'missing probe report') { Write-UiSaveFixtureJson $ReportPath $Report }
                [IO.File]::WriteAllText($Log, 'fixture probe')
                return [pscustomobject]@{ exit_code = $(if ($Fault -ceq 'probe exit') { 1 } else { 0 }); stdout = ''; stderr = '' }
            }
            if ($Arguments -contains '--version') {
                return [pscustomobject]@{ exit_code = 0; stdout = $(if ($Fault -ceq 'sdk') { '8.0.415' } else { '8.0.416' }); stderr = '' }
            }
            if ($Arguments -contains '-ExecCmds=AvidScript.PrepareUiSaveDemo exit') {
                return [pscustomobject]@{ exit_code = $(if ($Fault -ceq 'exit') { 1 } else { 0 })
                    stdout = $(if ($Fault -ceq 'marker') { 'No marker' } elseif ($Fault -ceq 'contradictory') {
                        'AVIDSCRIPT_UI_SAVE_DEMO_ASSETS_PASSED root=/AvidScript/Demos/UiSave assets=4 mode=created AVIDSCRIPT_UI_SAVE_DEMO_ASSETS_FAILED'
                    } else { 'AVIDSCRIPT_UI_SAVE_DEMO_ASSETS_PASSED root=/AvidScript/Demos/UiSave assets=4 mode=created' }); stderr = '' }
            }
            if ($Arguments -contains '-run=AvidScriptPublishProfileBindings') {
                $Output = ($Arguments | Where-Object { $_.StartsWith('-OutputRoot=') }).Substring(12)
                $ReportPath = ($Arguments | Where-Object { $_.StartsWith('-Report=') }).Substring(8)
                Assert-UiSaveContract (-not (Test-Path -LiteralPath $ReportPath)) 'Report path was not unique.'
                [void][IO.Directory]::CreateDirectory($Output)
                $PackageName = if ($Fault -ceq 'engine package') { 'avidscript.engine.gameplay' } else { 'avidscript.sample.ui_save_demo' }
                $DescriptorPath = Join-Path $Output 'bindings.json'
                $Reference = Join-Path $Output 'Bindings.cs'
                $ManifestPath = Join-Path $Output 'package.json'
                Write-UiSaveFixtureJson $DescriptorPath ([ordered]@{ schema_version = 2; package_name = $PackageName; package_hash = ('a' * 64) })
                [IO.File]::WriteAllText($Reference, '// fixture')
                Write-UiSaveFixtureJson $ManifestPath ([ordered]@{
                    schema_version = 1; descriptor_schema_version = 2; package_name = $PackageName; package_hash = ('a' * 64)
                    descriptor_sha256 = (Get-AvidScriptBindingSha256Hex $DescriptorPath)
                    reference_source_sha256 = (Get-AvidScriptBindingSha256Hex $Reference)
                    files = @{ descriptor = 'bindings.json'; reference_source = 'Bindings.cs' }
                    required_imports = @(@{ ordinal = 0; stable_id = ('b' * 64); module = 'avidscript'; name = 'fixture'; signature = '()i' })
                })
                $Report = [ordered]@{
                    schema_version = 1; result = 'avidscript_profile_bindings_published'; status = 'ok'
                    profile_path = $Context.profile; package_name = $PackageName; package_hash = ('a' * 64)
                    manifest_path = $ManifestPath; manifest_sha256 = (Get-AvidScriptBindingSha256Hex $ManifestPath)
                }
                switch ($Fault) {
                    'profile' { $Report.profile_path = Join-Path $Root 'AnotherProfile.json' }
                    'package hash' { $Report.package_hash = 'c' * 64 }
                    'manifest hash' { $Report.manifest_sha256 = 'c' * 64 }
                    'escape' { $Report.manifest_path = Join-Path $Root 'OutsideBindings.json' }
                    'descriptor tamper' { [IO.File]::AppendAllText($DescriptorPath, ' ') }
                    'reference tamper' { [IO.File]::AppendAllText($Reference, ' changed') }
                }
                if ($Fault -cne 'missing report') { Write-UiSaveFixtureJson $ReportPath $Report }
                return [pscustomobject]@{ exit_code = 0; stdout = ''; stderr = '' }
            }
            if ($Arguments -contains (Join-Path $UiSaveBuildRoot 'InvokeAvidScriptRelease.ps1')) {
                $ReleaseRoot = $Arguments[[Array]::IndexOf($Arguments, '-OutputRoot') + 1]
                $Release = [ordered]@{ schema_version = 1; result = 'avidscript_module_release_succeeded'
                    module_id = 'avidscript.ui_save_demo'; package_id = ('d' * 64); configuration = 'development'
                    target_platform = 'win64'; architecture = 'x86_64'; target_triple = 'x86_64-pc-windows-msvc'; build_output_root = $ReleaseRoot }
                switch ($Fault) {
                    'release module' { $Release.module_id = 'avidscript.pickup_rush' }
                    'release package' { $Release.package_id = 'not-a-hash' }
                    'release target' { $Release.target_platform = 'android' }
                    'release architecture' { $Release.architecture = 'arm64' }
                    'release root' { $Release.build_output_root = $Root }
                }
                return [pscustomobject]@{ exit_code = 0; stdout = ($Release | ConvertTo-Json -Compress); stderr = '' }
            }
            throw "Unexpected fixture process: $Executable $($Arguments -join ' ')"
        }
        $Result = $null
        $Failure = ''
        try { $Result = Invoke-AvidScriptUiSaveDemo } catch { $Failure = $_.Exception.Message }
        $Aggregate = $null
        $Retained = $false
        $RetainedEdges = $false
        if ($Mode -ceq 'Verify' -and $null -ne $Result) {
            $Aggregate = Get-Content -Raw -LiteralPath $Result.report_path | ConvertFrom-Json -Depth 32
            $Retained = Test-Path -LiteralPath (Join-Path $VerifyUserRoot 'write/Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav')
            $RetainedEdges = Test-Path -LiteralPath (Join-Path $VerifyUserRoot 'edges/Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav')
        }
        return [pscustomobject]@{ result = $Result; failure = $Failure; calls = $Calls.ToArray(); plays = $PlayCalls.ToArray()
            aggregate = $Aggregate; retained_save = $Retained; retained_edges_save = $RetainedEdges }
    }
    finally {
        $FullRoot = [IO.Path]::GetFullPath($Root)
        if (-not $FullRoot.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Fixture cleanup escaped the temporary root.'
        }
        Remove-Item -LiteralPath $FullRoot -Recurse -Force
    }
}

Invoke-UiSaveCase 'PowerShell syntax' {
    foreach ($Path in @($Runner, $PSCommandPath)) {
        $Errors = $null
        [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$null, [ref]$Errors) | Out-Null
        Assert-UiSaveContract ($Errors.Count -eq 0) "Parse failed: $Path"
    }
}

Invoke-UiSaveCase 'Prepare invokes only the asset command and checks its marker' {
    $Fixture = Invoke-UiSaveFixture Prepare
    Assert-UiSaveContract ($Fixture.failure -ceq '') $Fixture.failure
    Assert-UiSaveContract ($Fixture.result.status -ceq 'prepared' -and $Fixture.calls.Count -eq 1 -and $Fixture.plays.Count -eq 0) 'Prepare mode started unrelated work.'
    Assert-UiSaveContract ($Fixture.calls[0].arguments -contains '-ExecCmds=AvidScript.PrepareUiSaveDemo exit') 'Asset command is wrong.'
}

foreach ($Fault in @('marker', 'contradictory', 'exit', 'timeout')) {
    Invoke-UiSaveCase "Prepare rejects $Fault" {
        $Fixture = Invoke-UiSaveFixture Prepare $Fault
        Assert-UiSaveContract ($Fixture.failure -cne '' -and $null -eq $Fixture.result) 'Prepare failure was accepted.'
    }
}

Invoke-UiSaveCase 'Publish uses exact custom package, SDK and release entry' {
    $Fixture = Invoke-UiSaveFixture Publish
    Assert-UiSaveContract ($Fixture.failure -ceq '') $Fixture.failure
    Assert-UiSaveContract ($Fixture.result.status -ceq 'published' -and $Fixture.calls.Count -eq 3) 'Publish pipeline is incomplete.'
    foreach ($Call in $Fixture.calls) {
        Assert-UiSaveContract ($Call.cwd -ceq $UiSavePluginRoot -and $Call.timeout -eq $TimeoutSeconds) 'Child cwd/timeout differs from the controlled invocation.'
    }
    $Bindings = $Fixture.calls[1].arguments
    Assert-UiSaveContract ($Bindings -contains '-run=AvidScriptPublishProfileBindings' -and
        @($Bindings | Where-Object { $_ -like '-Profile=*Profile With Spaces.json' }).Count -eq 1) 'Custom profile was not one absolute argument.'
    $Release = $Fixture.calls[2].arguments
    foreach ($Flag in @('-BindingPackagePath', '-RuntimeBindingPackagePath')) {
        Assert-UiSaveContract ($Release[[Array]::IndexOf($Release, $Flag) + 1] -ceq $Fixture.result.binding_manifest) 'Release used an unverified or guessed binding manifest.'
    }
    Assert-UiSaveContract ($Fixture.result.package_id -ceq ('d' * 64) -and $Fixture.result.gameplay_acceptance -ceq 'not_run') 'Publication was mislabeled as gameplay acceptance.'
    Assert-UiSaveContract ($Fixture.calls[2].environment.DOTNET_CLI_HOME.StartsWith([IO.Path]::GetTempPath())) 'Dotnet state is not isolated outside the repository.'
}

foreach ($Fault in @('sdk', 'engine package', 'profile', 'package hash', 'manifest hash', 'escape', 'descriptor tamper', 'reference tamper', 'missing report')) {
    Invoke-UiSaveCase "Publish rejects $Fault before release" {
        $Fixture = Invoke-UiSaveFixture Publish $Fault
        Assert-UiSaveContract ($Fixture.failure -cne '' -and $null -eq $Fixture.result) 'Invalid binding publication passed.'
        Assert-UiSaveContract ($Fixture.calls.Count -lt 3 -and $Fixture.plays.Count -eq 0) 'Invalid input reached module release or Play.'
    }
}

foreach ($Fault in @('release module', 'release package', 'release target', 'release architecture', 'release root')) {
    Invoke-UiSaveCase "Publish rejects $Fault receipt" {
        $Fixture = Invoke-UiSaveFixture Publish $Fault
        Assert-UiSaveContract ($Fixture.failure.Contains('unexpected identity') -and $null -eq $Fixture.result) 'Invalid release identity was accepted.'
    }
}

Invoke-UiSaveCase 'Play is graphical, nonblocking and not acceptance' {
    $Fixture = Invoke-UiSaveFixture Play
    Assert-UiSaveContract ($Fixture.failure -ceq '') $Fixture.failure
    Assert-UiSaveContract ($Fixture.calls.Count -eq 0 -and $Fixture.plays.Count -eq 1 -and
        $Fixture.result.status -ceq 'launched' -and $Fixture.result.gameplay_acceptance -ceq 'not_run') 'Play built tools or claimed acceptance.'
    $Arguments = $Fixture.plays[0].arguments
    foreach ($Required in @('/AvidScript/Demos/UiSave/L_UiSave', '-game', '-windowed', '-AvidScriptScenario=ui_save_demo')) {
        Assert-UiSaveContract ($Arguments -contains $Required) "Missing interactive argument $Required"
    }
    Assert-UiSaveContract (-not ($Arguments -contains '-nullrhi') -and -not ($Arguments -contains '-unattended') -and
        @($Arguments | Where-Object { $_ -like '*ScenarioProbe*' }).Count -eq 0) 'Play is headless or a synthetic probe.'
}

Invoke-UiSaveCase 'Each invocation has a unique evidence root' {
    $First = Invoke-UiSaveFixture Prepare
    $Second = Invoke-UiSaveFixture Prepare
    Assert-UiSaveContract ($First.result.run_id -cne $Second.result.run_id -and
        $First.result.editor_log -cne $Second.result.editor_log) 'Evidence paths can reuse old logs.'
}

Invoke-UiSaveCase 'Startup retains PickupRush and adds one existing UI host' {
    $Path = Join-Path $UiSavePluginRoot 'Content/AvidScript/Startup/scenarios.json'
    $Scenarios = (Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json -Depth 32).scenarios
    $Pickup = @($Scenarios | Where-Object scenario_id -CEQ 'pickup_rush')
    $Ui = @($Scenarios | Where-Object scenario_id -CEQ 'ui_save_demo')
    Assert-UiSaveContract ($Pickup.Count -eq 1 -and $Pickup[0].bindings[0].target.mode -ceq 'spawn_actor') 'PickupRush scenario was removed or replaced.'
    Assert-UiSaveContract ($Ui.Count -eq 1 -and $Ui[0].activation -ceq 'explicit' -and $Ui[0].bindings.Count -eq 1 -and
        $Ui[0].worlds.Count -eq 1 -and $Ui[0].worlds[0] -ceq '/AvidScript/Demos/UiSave/L_UiSave') 'UI scenario identity/world is incorrect.'
    $Binding = $Ui[0].bindings[0]
    Assert-UiSaveContract ($Binding.module_id -ceq 'avidscript.ui_save_demo' -and $Binding.target.mode -ceq 'existing_actor' -and
        $Binding.target.max_instances -eq 1 -and $Binding.target.class_path -ceq '/AvidScript/Demos/UiSave/BP_UiSaveHost.BP_UiSaveHost_C') 'UI scenario host is not uniquely constrained.'
}

Invoke-UiSaveCase 'Verify consumes five reports and reuses only the write UserDir' {
    $Fixture = Invoke-UiSaveFixture Verify
    Assert-UiSaveContract ($Fixture.failure -ceq '') $Fixture.failure
    Assert-UiSaveContract ($Fixture.result.succeeded -and $Fixture.calls.Count -eq 5) $Fixture.result.message
    Assert-UiSaveContract ($Fixture.result.result -ceq 'avidscript_ui_save_verify_passed' -and $Fixture.aggregate.succeeded -and
        $Fixture.result.probes.Count -eq 5 -and $Fixture.retained_save -and $Fixture.retained_edges_save) 'Verify did not retain machine-readable evidence/save.'
    $Roots = @()
    $Reports = @()
    $Logs = @()
    $Modes = @('write', 'read', 'missing', 'gc', 'edges')
    for ($Index = 0; $Index -lt 5; ++$Index) {
        $Arguments = $Fixture.calls[$Index].arguments
        Assert-UiSaveContract ($Fixture.calls[$Index].timeout -eq [Math]::Min($TimeoutSeconds, 180)) 'Verify child timeout is not bounded.'
        foreach ($Flag in @('-game', '/AvidScript/Demos/UiSave/L_UiSave', '-ExecCmds=Module Load AvidScriptEditor',
            '-AvidScriptScenario=ui_save_demo', '-unattended', '-nullrhi', '-nosound', "-AvidScriptUiSaveProbe=$($Modes[$Index])",
            "-AvidScriptUiSaveExpectedPackage=$('d' * 64)")) {
            Assert-UiSaveContract ($Arguments -contains $Flag) "Missing Verify argument: $Flag"
        }
        Assert-UiSaveContract (-not ($Arguments -contains '-AvidScriptSuppressGeneratedTypeExecution')) 'Verify suppresses runtime execution.'
        $Roots += @($Arguments | Where-Object { $_ -like '-UserDir=*' })
        $Reports += @($Arguments | Where-Object { $_ -like '-AvidScriptUiSaveReport=*' })
        $Logs += @($Arguments | Where-Object { $_ -like '-abslog=*' })
    }
    Assert-UiSaveContract ($Roots[0] -ceq $Roots[1] -and $Roots[0] -ceq $Roots[3] -and $Roots[2] -cne $Roots[0]) 'UserDir reuse contract is broken.'
    Assert-UiSaveContract ($Roots[4] -cne $Roots[0] -and $Roots[4] -cne $Roots[2] -and $Roots[4] -match '[\\/]edges$') 'Edges did not use an independent fresh UserDir.'
    Assert-UiSaveContract (@($Reports | Select-Object -Unique).Count -eq 5 -and @($Logs | Select-Object -Unique).Count -eq 5) 'Reports/logs are not distinct.'
    $Edges = $Fixture.result.probes[4].evidence
    Assert-UiSaveContract ($Edges.actions.Count -eq 18 -and $Edges.runtime.events -eq 9 -and
        $Edges.actions[-1].synthetic_ue_event -and $Edges.edges.late_event_ignored) 'Edges action/late-event contract is incomplete.'
    Assert-UiSaveContract ($Fixture.result.save_file_sha256 -ceq $Fixture.result.probes[0].evidence.save_file_sha256 -and
        $Edges.save_file_sha256 -cne $Fixture.result.save_file_sha256 -and $Edges.initial_save_sha256 -ceq '') 'Edges altered or inherited the write/read/GC hash chain.'
    Assert-UiSaveContract ($Fixture.result.gameplay_acceptance -ceq 'synthetic_probe_only' -and
        -not $Fixture.result.physical_click_verified -and -not $Fixture.result.visual_verified -and -not $Fixture.result.long_run_verified) 'Synthetic input was overstated.'
}

foreach ($Fault in @('expected package missing', 'expected package malformed', 'existing user root', 'project user root', 'engine user root')) {
    Invoke-UiSaveCase "Verify rejects unsafe preflight: $Fault" {
        $Fixture = Invoke-UiSaveFixture Verify $Fault
        Assert-UiSaveContract ($Fixture.calls.Count -eq 0 -and
            ($Fixture.failure -cne '' -or -not $Fixture.result.succeeded)) 'Unsafe Verify preflight reached UE.'
    }
}

foreach ($Fault in @('failed report exit zero', 'missing probe report', 'probe identity', 'probe mode', 'duplicate pid', 'probe path',
    'runtime inactive', 'dropped events', 'empty actions', 'action failed', 'action score', 'final status', 'physical claim',
    'save hash', 'save size', 'extra save', 'read mutates save', 'gc mutates save', 'missing creates save', 'gc not performed', 'probe exit', 'timeout')) {
    Invoke-UiSaveCase "Verify rejects report/file evidence: $Fault" {
        $Fixture = Invoke-UiSaveFixture Verify $Fault
        Assert-UiSaveContract ($Fixture.failure -ceq '' -and -not $Fixture.result.succeeded -and -not $Fixture.aggregate.succeeded -and
            $Fixture.result.result -ceq 'avidscript_ui_save_verify_failed') 'Invalid Verify evidence passed or lost its aggregate.'
        $ExpectedCalls = switch ($Fault) { 'duplicate pid' { 2 }; 'read mutates save' { 2 }; 'missing creates save' { 3 }
            'gc mutates save' { 4 }; 'gc not performed' { 4 }; default { 1 } }
        Assert-UiSaveContract ($Fixture.calls.Count -eq $ExpectedCalls) "Verify did not stop at the failing process: $($Fixture.result.message)"
    }
}

foreach ($Fault in $EdgesFaults.Keys) {
    Invoke-UiSaveCase "Verify rejects edges evidence: $Fault" {
        $Fixture = Invoke-UiSaveFixture Verify $Fault
        Assert-UiSaveContract ($Fixture.failure -ceq '' -and -not $Fixture.result.succeeded -and -not $Fixture.aggregate.succeeded -and
            $Fixture.result.result -ceq 'avidscript_ui_save_verify_failed') 'Invalid edges evidence passed or lost its aggregate.'
        Assert-UiSaveContract ($Fixture.calls.Count -eq 5 -and $Fixture.result.probes.Count -eq 4 -and $Fixture.retained_save) `
            "Edges failure did not preserve the four completed reports/write save: $($Fixture.result.message)"
    }
}

Invoke-UiSaveCase 'Evidence directory creation rejects existing directories and reparse ancestors' {
    $Root = Join-Path ([IO.Path]::GetTempPath()) "AvidScriptUiSavePaths_$([Guid]::NewGuid().ToString('N'))"
    [void][IO.Directory]::CreateDirectory($Root)
    try {
        $Existing = Join-Path $Root 'existing'
        New-AvidScriptUiSaveDirectory $Existing
        $Rejected = $false
        try { New-AvidScriptUiSaveDirectory $Existing } catch { $Rejected = $true }
        Assert-UiSaveContract $Rejected 'Existing evidence directory was reused.'
        $Link = Join-Path $Root 'link'
        New-Item -ItemType Junction -Path $Link -Target $Existing | Out-Null
        try {
            $Rejected = $false
            try { New-AvidScriptUiSaveDirectory (Join-Path $Link 'child') } catch { $Rejected = $true }
            Assert-UiSaveContract $Rejected 'Reparse ancestor was accepted.'
        } finally { Remove-Item -LiteralPath $Link -Force }
    } finally {
        if (-not [IO.Path]::GetFullPath($Root).StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Path fixture cleanup escaped Temp.'
        }
        Remove-Item -LiteralPath $Root -Recurse -Force
    }
}

[Console]::Out.WriteLine(([ordered]@{ result = $(if ($Failures.Count) { 'avidscript_ui_save_demo_contracts_failed' } else { 'avidscript_ui_save_demo_contracts_passed' })
    passed = $Passed; total = $Total; failures = $Failures.ToArray() } | ConvertTo-Json -Depth 8 -Compress))
if ($Failures.Count) { exit 1 }
exit 0
