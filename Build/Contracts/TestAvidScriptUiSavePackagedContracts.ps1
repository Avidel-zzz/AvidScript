#requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'InvokeAvidScriptUiSavePackaged.ps1')
$FixtureRoot = Join-Path ([IO.Path]::GetTempPath()) "AvidScriptPackagedUiContracts/$([Guid]::NewGuid().ToString('N'))"
New-AvidScriptUiSaveDirectory $FixtureRoot
$Passed = 0
$Total = 0
$Failures = [Collections.Generic.List[string]]::new()

function Invoke-PackagedUiCase {
    param([string]$Name, [scriptblock]$Body)
    ++$script:Total
    try { & $Body; ++$script:Passed }
    catch { $script:Failures.Add("$Name`: $($_.Exception.Message)") }
}

function New-PackagedUiFixture {
    param([string]$Mode = 'write', [string]$Configuration = 'Development')
    $Root = Join-Path $FixtureRoot ([Guid]::NewGuid().ToString('N'))
    $SavePath = Join-Path $Root 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $SavePath))
    [IO.File]::WriteAllBytes($SavePath, [byte[]]@(1, 2, 3, 4))
    $Hash = Get-AvidScriptBindingSha256Hex $SavePath
    $Steps = ,@('ready', '0', 'Ready')
    if ($Mode -ceq 'write') { $Steps += @(@('collect', '1', 'Collected'), @('collect', '2', 'Collected'),
            @('collect', '3', 'Collected'), @('save', '3', 'Saved')) }
    else { $Steps += ,@('load', '3', 'Loaded') }
    $Frame = 10
    $Actions = @($Steps | ForEach-Object {
        $Frame += 2
        [ordered]@{ action = $_[0]; expected_score = $_[1]; observed_score = $_[1]; expected_status = $_[2]
            observed_status = $_[2]; dispatch_frame = $Frame; check_frame = ($Frame + 1); passed = $true }
    })
    $Now = [DateTimeOffset]::UtcNow
    $RunId = [Guid]::NewGuid().ToString('N')
    $Report = [ordered]@{
        schema_version = 1; result = 'avidscript_ui_save_probe_passed'; succeeded = $true; failure_category = ''
        mode = $Mode; process_mode = 'packaged_game'; configuration = $Configuration; requires_cooked_data = $true
        validation_plugin = 'AvidScriptValidation'; run_id = $RunId; process_id = 12345
        input_kind = 'synthetic_ue_button_onclicked_broadcast'; physical_click_verified = $false; visual_verified = $false; long_run_verified = $false
        expected_module_id = 'avidscript.ui_save_demo'; expected_package_id = ('d' * 64); map = '/AvidScript/Demos/UiSave/L_UiSave'
        user_dir = $Root; effective_saved_dir = (Join-Path $Root 'Saved'); save_path = $SavePath
        initial_save_sha256 = $(if ($Mode -ceq 'read') { $Hash } else { '' }); save_file_sha256 = $Hash; save_file_bytes = 4; save_file_exists = $true
        score_text = '3'; status_text = $Steps[-1][2]; started_utc = $Now.ToString('o'); finished_utc = $Now.AddSeconds(2).ToString('o')
        elapsed_seconds = 2.0; timeout_seconds = 90; runtime_snapshot_phase = 'final_active'; actions = $Actions
        runtime = [ordered]@{ module_id = 'avidscript.ui_save_demo'; package_id = ('d' * 64); resolved_from_package = $true
            runtime_loaded = $true; begin_play = $true; owner_registered = $true; owner_handle_valid = $true
            error_message = ''; events = ($Steps.Count - 1); dropped_events = 0 }
        startup = [ordered]@{ scenario_id = 'ui_save_demo'; active = $true; error_category = ''; error_message = '' }
        backend = [ordered]@{ measured = $true; source = 'CaptureRuntimeDiagnostics.BackendInfo'; backend_id = 'wasmtime'
            backend_kind = 'wasmtime'; execution_mode = 'aot'; artifact_format = 'wasmtime_serialized'; runtime_version = '45.0.0'
            runtime_build_identity = 'fixture'; runtime_artifact_sha256 = ('a' * 64); target_triple = 'x86_64-pc-windows-msvc' }
        resources = [ordered]@{ active_subscriptions = 4; bound_buttons = 4; owned_entries = 0; borrowed_entries = 8
            pending_timers = 0; pending_continuations = 0; prepared_continuations = 0; prepared_subscriptions = 0 }
    }
    $ReportPath = Join-Path $Root 'Saved/probe.json'
    return [pscustomobject]@{ report = $Report; path = $ReportPath; save_path = $SavePath
        arguments = @{ ReportPath = $ReportPath; ProbeMode = $Mode; Configuration = $Configuration; RunId = $RunId
            UserRoot = $Root; PackageId = ('d' * 64); WriteHash = $Hash; ProcessIds = [Collections.Generic.HashSet[int]]::new()
            LaunchedUtc = $Now.AddSeconds(-1); ExitedUtc = $Now.AddSeconds(3) } }
}

function New-PackagedUiWorldFixture {
    param([string]$Configuration = 'Development', [int]$WorldCycles = 4, [int]$SoakSeconds = 0,
        [int]$CompletedCycles = $WorldCycles)
    $Root = Join-Path $FixtureRoot ([Guid]::NewGuid().ToString('N'))
    $SavePath = Join-Path $Root 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $SavePath))
    [IO.File]::WriteAllBytes($SavePath, [byte[]]@(1, 2, 3, 4))
    $FinalHash = Get-AvidScriptBindingSha256Hex $SavePath
    $PackageId = 'd' * 64
    $RunId = [Guid]::NewGuid().ToString('N')
    $SampleCycles = @(Get-AvidScriptPackagedUiWorldSampleCycles $CompletedCycles)
    $Samples = [Collections.Generic.List[object]]::new()
    $Chain = Get-AvidScriptPackagedUiTextHash "AvidScriptPackagedWorld/v1|$RunId|$PackageId"
    $PeakPhysical = 0L
    $PeakVirtual = 0L
    $BaselinePhysical = 0L
    $BaselineVirtual = 0L
    for ($Cycle = 1; $Cycle -le $CompletedCycles; ++$Cycle) {
        $SaveHash = if ($Cycle -eq $CompletedCycles) { $FinalHash } else { Get-AvidScriptPackagedUiTextHash "fixture-save-$Cycle" }
        $ActionCount = if ($Cycle -eq 1) { 4 } else { 5 }
        $Physical = 100000L + $Cycle * 100L
        $Virtual = 200000L + $Cycle * 100L
        $UObjectCount = 500L + $Cycle
        $BackendCreated = 10L + $Cycle
        $BackendDestroyed = 9L + $Cycle
        $Canonical = @($Chain, $Cycle, $SaveHash, 4, $ActionCount, $Physical, $Virtual, $UObjectCount,
            $BackendCreated, $BackendDestroyed, 1, 1, 4096, 1, 2048) -join '|'
        $Chain = Get-AvidScriptPackagedUiTextHash $Canonical
        $PeakPhysical = [Math]::Max($PeakPhysical, $Physical)
        $PeakVirtual = [Math]::Max($PeakVirtual, $Virtual)
        if ($Cycle -eq 3) { $BaselinePhysical = $Physical; $BaselineVirtual = $Virtual }
        if ($SampleCycles -contains $Cycle) {
            $Samples.Add([pscustomobject][ordered]@{
                cycle = $Cycle; final_cycle = ($Cycle -eq $CompletedCycles); save_sha256 = $SaveHash; save_file_bytes = 4
                cycle_action_count = $ActionCount; physical_bytes = $Physical; virtual_bytes = $Virtual
                uobject_count = $UObjectCount; active_sessions = 1; active_subscriptions = 4; bound_buttons = 4
                owned_entries = 0; borrowed_entries = 7; pending_timers = 0; pending_continuations = 0
                prepared_continuations = 0; prepared_subscriptions = 0; backend_created = $BackendCreated
                backend_destroyed = $BackendDestroyed; backend_live = 1; artifact_cache_entries = 1
                artifact_cache_capacity = 32; artifact_cache_allocated_bytes = 4096; attestation_entries = 1
                attestation_capacity = 32; attestation_allocated_bytes = 2048; evidence_chain_sha256 = $Chain
                observer_json_estimated_bytes = 1024 + $Samples.Count * 128
            })
        }
    }
    $Now = [DateTimeOffset]::UtcNow
    $Elapsed = [double][Math]::Max($SoakSeconds, $CompletedCycles * 3)
    $Backend = [pscustomobject][ordered]@{ measured = $true; source = 'CaptureRuntimeDiagnostics.BackendInfo'; backend_id = 'wasmtime'
        backend_kind = 'wasmtime'; execution_mode = 'aot'; artifact_format = 'wasmtime_serialized'; runtime_version = '45.0.0'
        runtime_build_identity = 'fixture'; runtime_artifact_sha256 = ('a' * 64); target_triple = 'x86_64-pc-windows-msvc' }
    $Resources = [pscustomobject][ordered]@{ active_subscriptions = 4; bound_buttons = 4; owned_entries = 0; borrowed_entries = 8
        pending_timers = 0; pending_continuations = 0; prepared_continuations = 0; prepared_subscriptions = 0 }
    $Frame = 1000L
    $FinalActions = @(@('ready', '0', 'Ready'), @('load', [string]$CompletedCycles, 'Loaded')) | ForEach-Object {
        $Action = [pscustomobject][ordered]@{ action = $_[0]; expected_score = $_[1]; expected_status = $_[2]
            observed_score = $_[1]; observed_status = $_[2]; save_sha256_before = $FinalHash
            dispatch_frame = $Frame; check_frame = ($Frame + 2); passed = $true; save_sha256_after = $FinalHash }
        $Frame += 3
        $Action
    }
    $LongRun = $Elapsed -ge 3600 -and $CompletedCycles -ge 10
    $Report = [pscustomobject][ordered]@{
        schema_version = 1; result = 'avidscript_ui_save_world_probe_passed'; succeeded = $true; failure_category = ''
        mode = 'world'; process_mode = 'packaged_game'; configuration = $Configuration; requires_cooked_data = $true
        validation_plugin = 'AvidScriptValidation'; run_id = $RunId; process_id = 12345
        input_kind = 'synthetic_ue_button_onclicked_broadcast'; physical_click_verified = $false; visual_verified = $false
        long_run_verified = $LongRun; expected_module_id = 'avidscript.ui_save_demo'; expected_package_id = $PackageId
        map = '/AvidScript/Demos/UiSave/L_UiSave'; user_dir = $Root; effective_saved_dir = (Join-Path $Root 'Saved')
        save_path = $SavePath; initial_save_sha256 = ''; save_file_sha256 = $FinalHash; save_file_bytes = 4
        save_file_exists = $true; score_text = [string]$CompletedCycles; status_text = 'Loaded'; started_utc = $Now.ToString('o')
        finished_utc = $Now.AddSeconds($Elapsed).ToString('o'); elapsed_seconds = $Elapsed
        timeout_seconds = (Get-AvidScriptPackagedUiWorldBudget $WorldCycles $SoakSeconds); runtime_snapshot_phase = 'final_active'
        runtime = [pscustomobject][ordered]@{ module_id = 'avidscript.ui_save_demo'; package_id = $PackageId; error_message = ''
            resolved_from_package = $true; runtime_loaded = $true; begin_play = $true; owner_registered = $true
            owner_handle_valid = $true; events = 1; dropped_events = 0 }
        startup = [pscustomobject][ordered]@{ active = $true; scenario_id = 'ui_save_demo'; error_category = ''; error_message = '' }
        backend = $Backend; resources = $Resources
        world_lifecycle = [pscustomobject][ordered]@{
            requested_cycles = $WorldCycles; requested_soak_seconds = $SoakSeconds; completed_cycles = $CompletedCycles
            activated_worlds = $CompletedCycles + 1; travel_count = $CompletedCycles; cleanup_count = $CompletedCycles
            elapsed_seconds = $Elapsed; warmup_cycles = 3; world_lifecycle_verified = $true; long_run_verified = $LongRun
            final_recovered_score = $CompletedCycles; final_save_sha256 = $FinalHash; evidence_chain_sha256 = $Chain
            checks = [pscustomobject][ordered]@{ cycles_passed = $CompletedCycles; actions_passed = 5 * $CompletedCycles + 1
                cleanup_cycles_passed = $CompletedCycles; gc_cycles_passed = $CompletedCycles
                resource_snapshots_passed = $CompletedCycles + 1 }
            memory_summary = [pscustomobject][ordered]@{ baseline_available = ($CompletedCycles -ge 3)
                baseline_cycle = $(if ($CompletedCycles -ge 3) { 3 } else { 0 })
                baseline_physical_bytes = $BaselinePhysical; baseline_virtual_bytes = $BaselineVirtual
                final_physical_bytes = 100000L + $CompletedCycles * 100L; final_virtual_bytes = 200000L + $CompletedCycles * 100L
                peak_physical_bytes = $PeakPhysical; peak_virtual_bytes = $PeakVirtual }
            observer = [pscustomobject][ordered]@{ retention_policy = 'fixed_milestones_plus_final'; sample_count = $Samples.Count
                sample_capacity = 15; retained_completed_cycles = 0; retained_actions = 2; json_estimated_bytes = 4096 }
            samples = @($Samples); final_actions = @($FinalActions); failure_cycle = $null
        }
    }
    $ReportPath = Join-Path $Root 'Saved/AvidScript/PackagedUiWorld/world.json'
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $ReportPath))
    return [pscustomobject]@{ report = $Report; path = $ReportPath; save_path = $SavePath
        arguments = @{ Report = $Report; Configuration = $Configuration; RunId = $RunId; UserRoot = $Root; PackageId = $PackageId
            WorldCycles = $WorldCycles; SoakSeconds = $SoakSeconds; NativeBudget = (Get-AvidScriptPackagedUiWorldBudget $WorldCycles $SoakSeconds)
            ProcessId = 12345; LaunchedUtc = $Now.AddSeconds(-1); ExitedUtc = $Now.AddSeconds($Elapsed + 1) } }
}

Invoke-PackagedUiCase 'isolated startup config selects cooked UI map' {
    $Root = Join-Path $FixtureRoot ([Guid]::NewGuid().ToString('N'))
    $Config = New-AvidScriptPackagedUiStartupConfig $Root
    if ($Config.mode -cne 'isolated_generated_engine_ini' -or
        -not (Test-AvidScriptUiSaveSamePath $Config.path (Join-Path $Root 'Saved/Config/Windows/Engine.ini')) -or
        $Config.sha256 -cne (Get-AvidScriptBindingSha256Hex $Config.path) -or
        [IO.File]::ReadAllText($Config.path) -cne "[/Script/EngineSettings.GameMapsSettings]`nGameDefaultMap=/AvidScript/Demos/UiSave/L_UiSave`n") {
        throw 'Startup fixture must use the isolated generated Engine.ini and a fixed cooked map.'
    }
}

Invoke-PackagedUiCase 'startup config never overwrites an existing user directory' {
    $Root = Join-Path $FixtureRoot ([Guid]::NewGuid().ToString('N'))
    $Config = New-AvidScriptPackagedUiStartupConfig $Root
    $Rejected = $false
    try { $null = New-AvidScriptPackagedUiStartupConfig $Root } catch { $Rejected = $true }
    if (-not $Rejected -or $Config.sha256 -cne (Get-AvidScriptBindingSha256Hex $Config.path)) {
        throw 'Existing startup config must be preserved and rejected.'
    }
}

Invoke-PackagedUiCase 'packaged entry defaults preserve Verify behavior' {
    $Command = Get-Command (Join-Path (Split-Path -Parent $PSScriptRoot) 'InvokeAvidScriptUiSavePackaged.ps1')
    $Modes = @($Command.Parameters.Mode.Attributes | Where-Object { $_ -is [Management.Automation.ValidateSetAttribute] }).ValidValues
    if ($Mode -cne 'Verify' -or $WorldCycles -ne 10 -or $SoakSeconds -ne 0 -or
        $Modes.Count -ne 2 -or $Modes[0] -cne 'Verify' -or $Modes[1] -cne 'VerifyWorld') {
        throw 'Default Verify mode or the explicit VerifyWorld surface drifted.'
    }
}

Invoke-PackagedUiCase 'packaged World budgets never shorten native duration' {
    if ((Get-AvidScriptPackagedUiWorldBudget 10 0) -ne 200 -or
        (Get-AvidScriptPackagedUiWorldBudget 1000 21600) -ne 41600 -or
        (Get-AvidScriptPackagedUiWorldProcessBudget 120 10 0) -ne 230 -or
        (Get-AvidScriptPackagedUiWorldProcessBudget 600 10 0) -ne 600 -or
        (Get-AvidScriptPackagedUiWorldProcessBudget 120 1000 21600) -ne 41630) {
        throw 'Packaged World native or outer timeout budget drifted.'
    }
}

Invoke-PackagedUiCase 'packaged World arguments select one native world probe' {
    $Build = [pscustomobject]@{ target_name = 'AvidTPSTemplate' }
    $Context = [pscustomobject]@{ package_id = 'd' * 64 }
    $Arguments = @(New-AvidScriptPackagedUiWorldArguments $Build $Context 'C:\WorldUser' 'C:\World.log' `
        'C:\World.json' ('a' * 32) 25 3600)
    foreach ($Expected in @('-game', '-unattended', '-nullrhi', '-nowrite', '-EnablePlugins=AvidScriptValidation',
            '-AvidScriptScenario=ui_save_demo', '-UserDir=C:\WorldUser', '-abslog=C:\World.log',
            '-AvidScriptUiSavePackagedProbe=world', '-AvidScriptUiSaveReport=C:\World.json',
            "-AvidScriptUiSaveExpectedPackage=$('d' * 64)", "-AvidScriptUiSaveRunId=$('a' * 32)",
            '-AvidScriptUiSaveWorldCycles=25', '-AvidScriptUiSaveSoakSeconds=3600')) {
        if (@($Arguments | Where-Object { $_ -ceq $Expected }).Count -ne 1) { throw "Missing or repeated World argument: $Expected" }
    }
    if (@($Arguments | Where-Object { $_ -cmatch 'AvidScriptUiSavePackagedProbe=' }).Count -ne 1) {
        throw 'Packaged World must launch exactly one native probe.'
    }
}

Invoke-PackagedUiCase 'packaged World sample schedule is fixed and bounded' {
    $FinalMilestone = @(Get-AvidScriptPackagedUiWorldSampleCycles 10000)
    $FinalBetween = @(Get-AvidScriptPackagedUiWorldSampleCycles 9999)
    if ($FinalMilestone.Count -ne 14 -or $FinalMilestone[-1] -ne 10000 -or
        $FinalBetween.Count -ne 14 -or $FinalBetween[-2] -ne 5000 -or $FinalBetween[-1] -ne 9999) {
        throw 'Fixed milestones plus nonduplicate final cycle drifted.'
    }
}

foreach ($Configuration in @('Development', 'Shipping')) {
    foreach ($ProbeMode in @('write', 'read')) {
        Invoke-PackagedUiCase "$Configuration $ProbeMode accepts complete report" {
            $Fixture = New-PackagedUiFixture $ProbeMode $Configuration
            Write-AvidScriptUiSaveNewJson $Fixture.path $Fixture.report
            $Arguments = $Fixture.arguments
            $Result = Resolve-AvidScriptPackagedUiReport @Arguments
            if (-not $Result.succeeded) { throw 'Expected valid report.' }
        }
    }
}

foreach ($Configuration in @('Development', 'Shipping')) {
    Invoke-PackagedUiCase "$Configuration World accepts bounded report file" {
        $Fixture = New-PackagedUiWorldFixture $Configuration
        Write-AvidScriptUiSaveNewJson $Fixture.path $Fixture.report
        $Arguments = @{} + $Fixture.arguments
        [void]$Arguments.Remove('Report')
        $Arguments.ReportPath = $Fixture.path
        $Result = Resolve-AvidScriptPackagedUiWorldReport @Arguments
        if (-not $Result.succeeded -or $Result.world_lifecycle.samples.Count -ne 4) { throw 'Expected valid bounded World report.' }
    }
}

Invoke-PackagedUiCase 'World accepts two cycles without a warmup baseline in memory' {
    $Fixture = New-PackagedUiWorldFixture -WorldCycles 2
    $Arguments = $Fixture.arguments
    $Result = Resolve-AvidScriptPackagedUiWorldReport @Arguments
    if ($Result.world_lifecycle.memory_summary.baseline_available -or $Result.world_lifecycle.samples.Count -ne 2) {
        throw 'Two-cycle evidence must remain valid without a cycle-three baseline.'
    }
}

Invoke-PackagedUiCase 'World accepts truthful long run fields' {
    $Fixture = New-PackagedUiWorldFixture -WorldCycles 10 -SoakSeconds 3600
    $Arguments = $Fixture.arguments
    $Result = Resolve-AvidScriptPackagedUiWorldReport @Arguments
    if (-not $Result.long_run_verified -or -not $Result.world_lifecycle.long_run_verified) { throw 'Truthful long-run evidence was rejected.' }
}

$WorldFaults = [ordered]@{
    'unexpected root history' = { param($f) $f.report | Add-Member -NotePropertyName actions -NotePropertyValue @() }
    'wrong process mode' = { param($f) $f.report.process_mode = 'editor_binary_game' }
    'string success' = { param($f) $f.report.succeeded = 'true' }
    'wrong package' = { param($f) $f.report.runtime.package_id = 'a' * 64 }
    'wrong process id' = { param($f) $f.report.process_id = 54321 }
    'escaped user path' = { param($f) $f.report.user_dir = $env:TEMP }
    'JIT backend' = { param($f) $f.report.backend.execution_mode = 'jit' }
    'bytecode artifact' = { param($f) $f.report.backend.artifact_format = 'wasm_bytecode' }
    'retained owner' = { param($f) $f.report.resources.owned_entries = 1; $f.report.world_lifecycle.samples[-1].owned_entries = 1 }
    'wrong completed count' = { param($f) --$f.report.world_lifecycle.completed_cycles }
    'wrong activated Worlds' = { param($f) --$f.report.world_lifecycle.activated_worlds }
    'incomplete action checks' = { param($f) --$f.report.world_lifecycle.checks.actions_passed }
    'incomplete cleanup checks' = { param($f) --$f.report.world_lifecycle.checks.cleanup_cycles_passed }
    'incomplete resource checks' = { param($f) --$f.report.world_lifecycle.checks.resource_snapshots_passed }
    'linear cycles field' = { param($f) $f.report.world_lifecycle | Add-Member -NotePropertyName cycles -NotePropertyValue @() }
    'missing sample capacity' = { param($f) $f.report.world_lifecycle.observer.PSObject.Properties.Remove('sample_capacity') }
    'wrong sample capacity' = { param($f) $f.report.world_lifecycle.observer.sample_capacity = 16 }
    'retained completed cycle' = { param($f) $f.report.world_lifecycle.observer.retained_completed_cycles = 1 }
    'retained cycle actions' = { param($f) $f.report.world_lifecycle.observer.retained_actions = 5 }
    'oversized observer estimate' = { param($f) $f.report.world_lifecycle.observer.json_estimated_bytes = 4MB + 1 }
    'extra sample' = { param($f) $f.report.world_lifecycle.samples += $f.report.world_lifecycle.samples[-1]; ++$f.report.world_lifecycle.observer.sample_count }
    'wrong milestone' = { param($f) $f.report.world_lifecycle.samples[1].cycle = 4 }
    'nonfinal milestone marked final' = { param($f) $f.report.world_lifecycle.samples[1].final_cycle = $true }
    'numeric sample hash' = { param($f) $f.report.world_lifecycle.samples[0].save_sha256 = 1 }
    'broken first chain link' = { param($f) $f.report.world_lifecycle.samples[0].evidence_chain_sha256 = 'b' * 64 }
    'wrong final chain' = { param($f) $f.report.world_lifecycle.evidence_chain_sha256 = 'b' * 64 }
    'wrong final sample hash' = { param($f) $f.report.world_lifecycle.samples[-1].save_sha256 = 'b' * 64 }
    'final ready sample has loaded borrow' = { param($f) $f.report.world_lifecycle.samples[-1].borrowed_entries = 8 }
    'backend lifetime imbalance' = { param($f) ++$f.report.world_lifecycle.samples[1].backend_live }
    'cache entries over capacity' = { param($f) $f.report.world_lifecycle.samples[1].artifact_cache_entries = 33 }
    'cache growth after warmup' = { param($f) ++$f.report.world_lifecycle.samples[-1].artifact_cache_allocated_bytes }
    'wrong memory baseline' = { param($f) ++$f.report.world_lifecycle.memory_summary.baseline_physical_bytes }
    'wrong final memory' = { param($f) ++$f.report.world_lifecycle.memory_summary.final_virtual_bytes }
    'peak below sample' = { param($f) $f.report.world_lifecycle.memory_summary.peak_physical_bytes = 1 }
    'failure cycle on success' = { param($f) $f.report.world_lifecycle.failure_cycle = [pscustomobject]@{ cycle = 4 } }
    'missing final load' = { param($f) $f.report.world_lifecycle.final_actions = @($f.report.world_lifecycle.final_actions[0]) }
    'final load changed save' = { param($f) $f.report.world_lifecycle.final_actions[1].save_sha256_after = 'b' * 64 }
    'same frame final check' = { param($f) $f.report.world_lifecycle.final_actions[1].check_frame = $f.report.world_lifecycle.final_actions[1].dispatch_frame }
    'forged long run' = { param($f) $f.report.long_run_verified = $true; $f.report.world_lifecycle.long_run_verified = $true }
    'lifecycle elapsed mismatch' = { param($f) ++$f.report.world_lifecycle.elapsed_seconds }
    'wrong native timeout' = { param($f) ++$f.report.timeout_seconds }
    'old timestamp' = { param($f) $f.report.started_utc = [DateTimeOffset]::UtcNow.AddDays(-1).ToString('o') }
}
foreach ($Fault in $WorldFaults.GetEnumerator()) {
    Invoke-PackagedUiCase "reject World $($Fault.Key) in memory" {
        $Fixture = New-PackagedUiWorldFixture
        & $Fault.Value $Fixture
        $Rejected = $false
        $Arguments = $Fixture.arguments
        try { $null = Resolve-AvidScriptPackagedUiWorldReport @Arguments } catch { $Rejected = $true }
        if (-not $Rejected) { throw 'Invalid in-memory World report was accepted.' }
    }
}

Invoke-PackagedUiCase 'World file validation rejects changed disk data' {
    $Fixture = New-PackagedUiWorldFixture
    Write-AvidScriptUiSaveNewJson $Fixture.path $Fixture.report
    [IO.File]::AppendAllText($Fixture.save_path, 'changed')
    $Arguments = @{} + $Fixture.arguments
    [void]$Arguments.Remove('Report')
    $Arguments.ReportPath = $Fixture.path
    $Rejected = $false
    try { $null = Resolve-AvidScriptPackagedUiWorldReport @Arguments } catch { $Rejected = $true }
    if (-not $Rejected) { throw 'Changed packaged World save was accepted.' }
}

$Faults = [ordered]@{
    'editor report' = { param($f) $f.report.process_mode = 'editor_binary_game' }
    'uncooked game' = { param($f) $f.report.requires_cooked_data = $false }
    'wrong configuration' = { param($f) $f.report.configuration = 'Shipping' }
    'stale run' = { param($f) $f.report.run_id = 'b' * 32 }
    'duplicate PID' = { param($f) [void]$f.arguments.ProcessIds.Add(12345) }
    'JIT not AOT' = { param($f) $f.report.backend.execution_mode = 'jit' }
    'wrong artifact' = { param($f) $f.report.backend.artifact_format = 'wasm_bytecode' }
    'different package' = { param($f) $f.report.runtime.package_id = 'a' * 64 }
    'missing action' = { param($f) $f.report.actions = @($f.report.actions | Select-Object -SkipLast 1) }
    'same frame check' = { param($f) $f.report.actions[1].check_frame = $f.report.actions[1].dispatch_frame }
    'missing event' = { param($f) $f.report.runtime.events = 3 }
    'extra event' = { param($f) $f.report.runtime.events = 5 }
    'late callback' = { param($f) $f.report.runtime.dropped_events = 1 }
    'pending continuation' = { param($f) $f.report.resources.pending_continuations = 1 }
    'string boolean' = { param($f) $f.report.succeeded = 'true' }
    'numeric text' = { param($f) $f.report.actions[1].observed_score = 1 }
    'forged visual pass' = { param($f) $f.report.visual_verified = $true }
    'escaped Saved' = { param($f) $f.report.effective_saved_dir = $env:TEMP }
    'changed disk data' = { param($f) [IO.File]::AppendAllText($f.save_path, 'changed') }
    'other save file' = { param($f) [IO.File]::WriteAllText((Join-Path (Split-Path -Parent $f.save_path) 'Other.sav'), 'other') }
    'old timestamp' = { param($f) $f.report.started_utc = [DateTimeOffset]::UtcNow.AddDays(-1).ToString('o') }
}
foreach ($Fault in $Faults.GetEnumerator()) {
    Invoke-PackagedUiCase "reject $($Fault.Key)" {
        $Fixture = New-PackagedUiFixture
        & $Fault.Value $Fixture
        Write-AvidScriptUiSaveNewJson $Fixture.path $Fixture.report
        $Arguments = $Fixture.arguments
        $Rejected = $false
        try { $null = Resolve-AvidScriptPackagedUiReport @Arguments } catch { $Rejected = $true }
        if (-not $Rejected) { throw 'Invalid report was accepted.' }
    }
}
Invoke-PackagedUiCase 'duplicate JSON keys rejected' {
    $Path = Join-Path $FixtureRoot 'duplicate.json'
    [IO.File]::WriteAllText($Path, '{"succeeded":true,"succeeded":false}')
    $Rejected = $false
    try { $null = Read-AvidScriptPackagedUiJson $Path } catch { $Rejected = $true }
    if (-not $Rejected) { throw 'Duplicate JSON was accepted.' }
}
Invoke-PackagedUiCase 'real process helper reports actual PID' {
    $Result = Invoke-AvidScriptAndroidProcess -Executable (Join-Path $PSHOME 'pwsh.exe') `
        -Arguments @('-NoProfile', '-NonInteractive', '-Command', '$PID') -WorkingDirectory $PackagedUiPluginRoot
    if ($Result.exit_code -ne 0 -or $Result.process_id -ne [int]$Result.stdout.Trim()) { throw 'Actual process identity was not preserved.' }
}

[Console]::Out.WriteLine(([ordered]@{ result = $(if ($Failures.Count) { 'avidscript_packaged_ui_contracts_failed' } else { 'avidscript_packaged_ui_contracts_passed' })
    passed = $Passed; total = $Total; failures = @($Failures); fixture_root = $FixtureRoot } | ConvertTo-Json -Depth 8 -Compress))
if ($Failures.Count) { exit 1 }
exit 0
