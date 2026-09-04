#requires -Version 7.0
param([switch]$TimeoutOnly)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ContractTimer = [Diagnostics.Stopwatch]::StartNew()
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'InvokeAvidScriptUiSaveDemo.ps1')
$Passed = 0
$Total = 0
$Failures = [Collections.Generic.List[string]]::new()
$FixtureRoot = Join-Path ([IO.Path]::GetTempPath()) "AvidScriptUiSaveWorldContracts/$([Guid]::NewGuid().ToString('N'))"
New-AvidScriptUiSaveDirectory $FixtureRoot

function Assert-UiSaveWorldContract {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-UiSaveWorldCase {
    param([string]$Name, [scriptblock]$Body)
    ++$script:Total
    try { & $Body; ++$script:Passed }
    catch { $script:Failures.Add("$Name`: $($_.Exception.Message)") }
}

function Complete-UiSaveWorldContracts {
    $ContractTimer.Stop()
    $Receipt = [ordered]@{ result = $(if ($Failures.Count -eq 0) { 'avidscript_ui_save_world_contracts_passed' } else { 'avidscript_ui_save_world_contracts_failed' })
        passed = $Passed; total = $Total; failures = @($Failures); fixture_root = $FixtureRoot
        elapsed_ms = $ContractTimer.ElapsedMilliseconds }
    Write-AvidScriptUiSaveNewJson (Join-Path $FixtureRoot 'contracts.json') $Receipt
    [Console]::Out.WriteLine(($Receipt | ConvertTo-Json -Depth 8 -Compress))
    if ($Failures.Count -gt 0) { exit 1 }
    exit 0
}

Invoke-UiSaveWorldCase 'real public process helper accepts maximum budget' {
    $Ranges = @((Get-Command Invoke-AvidScriptAndroidProcess).Parameters['TimeoutSeconds'].Attributes |
        Where-Object { $_ -is [Management.Automation.ValidateRangeAttribute] })
    Assert-UiSaveWorldContract ($Ranges.Count -eq 1 -and $Ranges[0].MinRange -eq 1 -and $Ranges[0].MaxRange -eq 41630) 'Public process helper bound differs.'
    $Process = Invoke-AvidScriptAndroidProcess -Executable (Join-Path $PSHOME 'pwsh.exe') `
        -Arguments @('-NoProfile', '-NonInteractive', '-Command', "'world-timeout-contract'") `
        -WorkingDirectory $UiSavePluginRoot -TimeoutSeconds 41630
    Assert-UiSaveWorldContract ($Process.exit_code -eq 0 -and $Process.stdout.Trim() -ceq 'world-timeout-contract') 'Real public process invocation failed.'
}
if ($TimeoutOnly) { Complete-UiSaveWorldContracts }

function New-UiSaveWorldActionsFixture {
    param([array]$Steps, [string]$Before, [string]$After, [ref]$Frame)
    $Actions = @()
    $Hash = $Before
    foreach ($Step in $Steps) {
        $Previous = $Hash
        if ($Step[0] -ceq 'save') { $Hash = $After }
        $Frame.Value += 3
        $Actions += [ordered]@{ action = $Step[0]; expected_score = $Step[1]; observed_score = $Step[1]
            expected_status = $Step[2]; observed_status = $Step[2]; dispatch_frame = $Frame.Value
            check_frame = ($Frame.Value + 2); passed = $true; save_sha256_before = $Previous; save_sha256_after = $Hash }
    }
    return ,$Actions
}

function New-UiSaveWorldEngineMemoryFixture {
    param([long]$Frame, [switch]$LlmEnabled)
    return [ordered]@{ schema_version = 1; allocator_name = 'Mimalloc'; sample_frame = $Frame
        sample_consistency = 'owner_snapshots_not_atomic'
        trace = [ordered]@{ available = $true; memory_used_bytes = 4096; block_pool_bytes = 8192; fixed_buffer_bytes = 0
            shared_buffer_bytes = 128; important_cache_allocated_bytes = 512; important_cache_used_bytes = 1024; thread_registry_bytes = 32 }
        names = [ordered]@{ entry_bytes = 4096; table_bytes = 2048; ansi_count = 10; wide_count = 0 }
        llm = [ordered]@{ enabled = [bool]$LlmEnabled; sample_origin = 'live_totals_and_aggregated_tags'
            default_total_bytes = $(if ($LlmEnabled) { 4096 } else { $null })
            platform_total_bytes = $(if ($LlmEnabled) { 1024 } else { $null })
            platform_fmalloc_bytes = $(if ($LlmEnabled) { 8192 } else { $null })
            platform_overhead_bytes = $(if ($LlmEnabled) { -32 } else { $null })
            default_fmalloc_unused_bytes = $(if ($LlmEnabled) { -128 } else { $null })
            default_uobject_bytes = $(if ($LlmEnabled) { 8192 } else { $null })
            default_fname_bytes = $(if ($LlmEnabled) { 2048 } else { $null })
            default_untagged_bytes = $(if ($LlmEnabled) { -512 } else { $null })
            default_engine_misc_bytes = $(if ($LlmEnabled) { 0 } else { $null }) } }
}

function Assert-UiSaveWorldEngineFixtureRejected {
    param([scriptblock]$Mutate, [switch]$LlmEnabled)
    $Engine = New-UiSaveWorldEngineMemoryFixture 0 -LlmEnabled:$LlmEnabled
    [void](& $Mutate $Engine)
    $Snapshot = $Engine | ConvertTo-Json -Depth 8 -Compress | ConvertFrom-Json -Depth 8
    Assert-UiSaveWorldContract ($Snapshot -is [pscustomobject]) 'Engine fixture did not roundtrip to a JSON object.'
    $Rejected = $false
    try { Assert-AvidScriptUiSaveWorldEngineMemory $Snapshot $null }
    catch { $Rejected = $true }
    Assert-UiSaveWorldContract $Rejected 'Accepted invalid engine memory fixture.'
}

function New-UiSaveWorldReportFixture {
    param([string]$UserRoot, [string]$PackageId, [int]$Count, [int]$Seconds, [switch]$LlmEnabled)
    $SavePath = Join-Path $UserRoot 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $SavePath))
    $Cycles = @()
    $Hash = ''
    $Frame = 10L
    for ($Number = 1; $Number -le $Count; ++$Number) {
        $Before = $Hash
        $Content = [Text.UTF8Encoding]::new($false).GetBytes("fixture world saved score=$Number")
        $Hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Content)).ToLowerInvariant()
        $Steps = ,@('ready', '0', 'Ready')
        if ($Number -gt 1) { $Steps += ,@('load', [string]($Number - 1), 'Loaded') }
        $Steps += @(@('collect', [string]$Number, 'Collected'), @('save', [string]$Number, 'Saved'), @('gc', [string]$Number, 'Saved'))
        $Events = if ($Number -eq 1) { 2 } else { 3 }
        $Cycles += [ordered]@{ cycle = $Number; saved_score = $Number; save_sha256_before = $Before; save_sha256 = $Hash
            save_file_bytes = $Content.Length; events_before_travel = $Events; passed = $true
            actions = (New-UiSaveWorldActionsFixture $Steps $Before $Hash ([ref]$Frame))
            resources = [ordered]@{ active_subscriptions = 4; bound_buttons = 4; owned_entries = 0; borrowed_entries = 8
                pending_timers = 0; pending_continuations = 0; prepared_continuations = 0; prepared_subscriptions = 0
                backend = [ordered]@{ measured = $true; source = 'CaptureLiveSnapshot.BackendInfo'; backend_id = 'fixture.wasmtime'
                    backend_kind = 'wasmtime'; execution_mode = 'aot'; artifact_format = 'wasmtime_serialized'; runtime_version = 'fixture'
                    runtime_build_identity = 'fixture'; runtime_artifact_sha256 = ('b' * 64); target_triple = 'x86_64-pc-windows-msvc' } }
            cleanup = [ordered]@{ observed = $true; component_end_play = $true; guest_end_play = $true; session_released = $true
                owner_released = $true; buttons_unbound = $true; widget_removed = $true; saved_reference_cleared = $true
                events_before = $Events; events_after = $Events }
            gc = [ordered]@{ world_collected = $true; host_collected = $true; component_collected = $true; widget_collected = $true
                saved_object_collected = $true; new_world_identity = $true; new_host_identity = $true; new_component_identity = $true
                physical_bytes = (4GB + $Number); virtual_bytes = (8GB + $Number); uobject_count = 12345; active_sessions = 1
                attribution = [ordered]@{ schema_version = 1; backend_created = ($Number + 1); backend_destroyed = $Number; backend_live = 1
                    artifact_cache_entries = 1; artifact_cache_capacity = 32; artifact_cache_allocated_bytes = 2048
                    attestation_entries = 1; attestation_capacity = 32; attestation_allocated_bytes = 512
                    observer_retained_cycles = $Number; observer_retained_actions = ($Number * 5 - 1)
                    observer_json_estimated_bytes = ($Number * 4096); observer_estimate_kind = 'ue_json_memory_footprint'
                    engine_memory = (New-UiSaveWorldEngineMemoryFixture ($Number - 1) -LlmEnabled:$LlmEnabled) } } }
    }
    [IO.File]::WriteAllBytes($SavePath, $Content)
    $Elapsed = [Math]::Max($Seconds, $Count * 3) + 2
    $LongRun = $Elapsed -ge 3600 -and $Count -ge 10
    $Started = [DateTimeOffset]::UtcNow
    return [ordered]@{ schema_version = 1; result = 'avidscript_ui_save_probe_passed'; succeeded = $true; failure_category = ''
        mode = 'world'; process_mode = 'editor_binary_game'; input_kind = 'synthetic_ue_button_onclicked_broadcast'; process_id = 12345
        expected_module_id = 'avidscript.ui_save_demo'; expected_package_id = $PackageId; map = '/AvidScript/Demos/UiSave/L_UiSave'
        physical_click_verified = $false; visual_verified = $false; long_run_verified = $LongRun; gc_performed = $true
        runtime_snapshot_phase = 'final_active'; actions = @(); started_utc = $Started.ToString('o'); finished_utc = $Started.AddSeconds($Elapsed).ToString('o')
        elapsed_seconds = $Elapsed; timeout_seconds = ($Seconds + [Math]::Max(120, $Count * 20)); user_dir = $UserRoot
        save_path = $SavePath; save_file_exists = $true; save_file_bytes = $Content.Length; save_file_sha256 = $Hash
        initial_save_sha256 = ''; score_text = [string]$Count; status_text = 'Loaded'
        startup = [ordered]@{ active = $true; scenario_id = 'ui_save_demo'; error_category = ''; error_message = '' }
        runtime = [ordered]@{ module_id = 'avidscript.ui_save_demo'; package_id = $PackageId; error_message = ''; events = 1; dropped_events = 0
            resolved_from_package = $true; runtime_loaded = $true; begin_play = $true; owner_registered = $true; owner_handle_valid = $true }
        world_lifecycle = [ordered]@{ requested_cycles = $Count; requested_soak_seconds = $Seconds; completed_cycles = $Count
            activated_worlds = ($Count + 1); travel_count = $Count; cleanup_count = $Count; elapsed_seconds = $Elapsed; warmup_cycles = 3
            world_lifecycle_verified = $true; long_run_verified = $LongRun; memory_measured = $true; final_recovered_score = $Count
            final_save_sha256 = $Hash; cycles = $Cycles
            final_actions = (New-UiSaveWorldActionsFixture @(@('ready', '0', 'Ready'), @('load', [string]$Count, 'Loaded')) $Hash $Hash ([ref]$Frame))
            memory_summary = [ordered]@{ baseline_available = ($Count -ge 3)
                baseline_physical_bytes = $(if ($Count -ge 3) { 4GB + 3 } else { 0 })
                baseline_virtual_bytes = $(if ($Count -ge 3) { 8GB + 3 } else { 0 })
                final_physical_bytes = (4GB + $Count); final_virtual_bytes = (8GB + $Count)
                peak_physical_bytes = (4GB + $Count); peak_virtual_bytes = (8GB + $Count) } } }
}

function Invoke-UiSaveWorldFixture {
    param([int]$Count = 2, [int]$Seconds = 0, [scriptblock]$Mutate, [string]$Fault = '', [int]$ExplicitTimeout = 0,
        [int]$CompletedCount = 0, [switch]$MeasureLlm)
    $CaseRoot = Join-Path $FixtureRoot ([Guid]::NewGuid().ToString('N'))
    New-AvidScriptUiSaveDirectory $CaseRoot
    $Mode = 'VerifyWorld'
    $WorldCycles = $Count
    $SoakSeconds = $Seconds
    $UiSaveTimeoutExplicit = $ExplicitTimeout -gt 0
    $TimeoutSeconds = if ($UiSaveTimeoutExplicit) { $ExplicitTimeout } else { 1200 }
    $ExpectedPackageId = if ($Fault -ceq 'missing package') { '' } else { 'd' * 64 }
    $VerifyUserRoot = Join-Path $CaseRoot 'User Data'
    $Context = [pscustomobject]@{ project = (Join-Path $CaseRoot 'Project/Fixture Project.uproject')
        editor = 'C:\fixture-engine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'; engine = 'C:\fixture-engine'
        output_root = (Join-Path $CaseRoot 'Project/Saved/World') }
    $Calls = [Collections.Generic.List[object]]::new()
    if ($Fault -ceq 'existing root') { New-AvidScriptUiSaveDirectory $VerifyUserRoot }
    if ($Fault -ceq 'project root') { $VerifyUserRoot = Join-Path (Split-Path -Parent $Context.project) 'User Data' }
    function Get-AvidScriptUiSaveContext { return $Context }
    function Invoke-AvidScriptAndroidProcess {
        param($Executable, $Arguments, $WorkingDirectory, $TimeoutSeconds, $Environment)
        $Calls.Add([pscustomobject]@{ executable = $Executable; arguments = $Arguments; timeout = $TimeoutSeconds })
        Assert-UiSaveWorldContract ($Executable.EndsWith('UnrealEditor-Cmd.exe')) 'World did not use Cmd editor.'
        foreach ($Argument in @('-game', '-ExecCmds=Module Load AvidScriptEditor', '-AvidScriptScenario=ui_save_demo',
                '-AvidScriptUiSaveProbe=world', "-AvidScriptUiSaveWorldCycles=$Count", "-AvidScriptUiSaveSoakSeconds=$Seconds")) {
            Assert-UiSaveWorldContract ($Arguments -ccontains $Argument) "Missing native argument: $Argument"
        }
        foreach ($Argument in @('-llm', '-AvidScriptUiSaveRequireLlm')) {
            $ExpectedCount = if ($MeasureLlm) { 1 } else { 0 }
            Assert-UiSaveWorldContract (@($Arguments | Where-Object { $_ -ceq $Argument }).Count -eq $ExpectedCount) "Conditional native argument differs: $Argument"
        }
        $ReportPath = ($Arguments | Where-Object { $_.StartsWith('-AvidScriptUiSaveReport=') }).Substring(24)
        $UserRoot = ($Arguments | Where-Object { $_.StartsWith('-UserDir=') }).Substring(9)
        Assert-UiSaveWorldContract (Test-AvidScriptUiSaveSamePath $UserRoot (Join-Path $VerifyUserRoot 'world')) 'World UserDir was not isolated.'
        Assert-UiSaveWorldContract (@(Get-ChildItem -LiteralPath $UserRoot -Force).Count -eq 0) 'World UserDir was not fresh.'
        Assert-UiSaveWorldContract (-not (Test-Path -LiteralPath $ReportPath)) 'World report path was reused.'
        $SampleCount = if ($CompletedCount -gt 0) { $CompletedCount } else { $Count }
        $Report = New-UiSaveWorldReportFixture $UserRoot $ExpectedPackageId $SampleCount $Seconds -LlmEnabled:$MeasureLlm
        $Report.world_lifecycle.requested_cycles = $Count
        $Report.timeout_seconds = $Seconds + [Math]::Max(120, $Count * 20)
        if ($null -ne $Mutate) { [void](& $Mutate $Report) }
        if ($Fault -cne 'missing report') { Write-AvidScriptUiSaveNewJson $ReportPath $Report }
        if ($Fault -ceq 'oversized report') {
            $Stream = [IO.File]::OpenWrite($ReportPath)
            try { $Stream.SetLength(64MB + 1) } finally { $Stream.Dispose() }
        }
        if ($Fault -ceq 'changed file') { [IO.File]::AppendAllText($Report.save_path, 'tampered') }
        return [pscustomobject]@{ exit_code = $(if ($Fault -ceq 'process failed') { 7 } else { 0 }); stdout = ''; stderr = ''
            elapsed_ms = $(if ($Fault -ceq 'forged process duration') { 1000 } else { ($Report.elapsed_seconds + 1) * 1000 }) }
    }
    $Summary = Invoke-AvidScriptUiSaveDemo
    Assert-UiSaveWorldContract ($Summary.llm_requested -is [bool] -and $Summary.llm_requested -eq [bool]$MeasureLlm) 'LLM request was not recorded as a boolean.'
    return [pscustomobject]@{ summary = $Summary; calls = $Calls }
}

Invoke-UiSaveWorldCase 'default ten travels and int64 memory' {
    $Result = Invoke-UiSaveWorldFixture -Count 10
    Assert-UiSaveWorldContract $Result.summary.succeeded $Result.summary.message
    Assert-UiSaveWorldContract ($Result.calls.Count -eq 1 -and $Result.calls[0].timeout -eq 230) 'World process budget/count differs.'
    Assert-UiSaveWorldContract ($Result.summary.evidence.world_lifecycle.activated_worlds -eq 11) 'Final World was not activated.'
    Assert-UiSaveWorldContract (-not $Result.summary.long_run_verified -and -not $Result.summary.packaged_verified -and
        -not $Result.summary.physical_click_verified -and -not $Result.summary.visual_verified) 'Unsupported acceptance was claimed.'
}
Invoke-UiSaveWorldCase 'minimum two travels no warmup baseline' {
    $Result = Invoke-UiSaveWorldFixture
    Assert-UiSaveWorldContract $Result.summary.succeeded $Result.summary.message
    Assert-UiSaveWorldContract (-not $Result.summary.evidence.world_lifecycle.memory_summary.baseline_available) 'Premature memory baseline.'
    Assert-UiSaveWorldContract ($Result.calls[0].timeout -eq 150) 'Minimum process budget differs.'
}
Invoke-UiSaveWorldCase 'one-hour receipt requires real process duration' {
    $Result = Invoke-UiSaveWorldFixture -Count 10 -Seconds 3600
    Assert-UiSaveWorldContract $Result.summary.succeeded $Result.summary.message
    Assert-UiSaveWorldContract ($Result.summary.long_run_verified -and $Result.calls[0].timeout -eq 3830) 'Soak used the legacy default timeout.'
}
Invoke-UiSaveWorldCase 'explicit caller timeout cap' {
    $Result = Invoke-UiSaveWorldFixture -ExplicitTimeout 80
    Assert-UiSaveWorldContract $Result.summary.succeeded $Result.summary.message
    Assert-UiSaveWorldContract ($Result.calls[0].timeout -eq 80) 'Explicit timeout cap was ignored.'
}
Invoke-UiSaveWorldCase 'soak continues beyond minimum requested cycles' {
    $Result = Invoke-UiSaveWorldFixture -Count 2 -CompletedCount 10 -Seconds 30
    Assert-UiSaveWorldContract $Result.summary.succeeded $Result.summary.message
    Assert-UiSaveWorldContract ($Result.summary.evidence.world_lifecycle.requested_cycles -eq 2 -and
        $Result.summary.evidence.world_lifecycle.completed_cycles -eq 10 -and $Result.summary.evidence.score_text -ceq '10') 'Soak did not keep the complete score/count chain.'
}
Invoke-UiSaveWorldCase 'maximum bounded budget' {
    Assert-UiSaveWorldContract ((Get-AvidScriptUiSaveWorldBudget 1000 21600) -eq 41600) 'Maximum budget differs.'
    foreach ($Values in @(@(1, 0), @(1001, 0), @(2, -1), @(2, 21601))) {
        $Rejected = $false
        try { [void](Get-AvidScriptUiSaveWorldBudget $Values[0] $Values[1]) } catch { $Rejected = $true }
        Assert-UiSaveWorldContract $Rejected 'Invalid World bounds were accepted.'
    }
}

Invoke-UiSaveWorldCase 'backend growth remains measured evidence not an automatic no-leak claim' {
    $Result = Invoke-UiSaveWorldFixture -Mutate {
        param($R)
        ++$R.world_lifecycle.cycles[1].gc.attribution.backend_created
        ++$R.world_lifecycle.cycles[1].gc.attribution.backend_live
    }
    Assert-UiSaveWorldContract $Result.summary.succeeded $Result.summary.message
    Assert-UiSaveWorldContract ($Result.summary.evidence.world_lifecycle.cycles[1].gc.attribution.backend_live -eq 2) 'Growth evidence was discarded.'
}

Invoke-UiSaveWorldCase 'public MeasureLlm parameter is a switch' {
    $Entry = Get-Command (Join-Path (Split-Path -Parent $PSScriptRoot) 'InvokeAvidScriptUiSaveDemo.ps1')
    Assert-UiSaveWorldContract ($Entry.Parameters.ContainsKey('MeasureLlm') -and
        $Entry.Parameters['MeasureLlm'].ParameterType -eq [Management.Automation.SwitchParameter]) 'Public MeasureLlm switch is missing.'
}
foreach ($OtherMode in @('Prepare', 'Publish', 'Play', 'Verify', 'VerifyReload')) {
    Invoke-UiSaveWorldCase "MeasureLlm rejects $OtherMode before context lookup" {
        $Mode = $OtherMode
        $MeasureLlm = $true
        function Get-AvidScriptUiSaveContext { throw 'Unexpected context lookup.' }
        $Message = ''
        try { [void](Invoke-AvidScriptUiSaveDemo) } catch { $Message = $_.Exception.Message }
        Assert-UiSaveWorldContract ($Message -match 'MeasureLlm.*VerifyWorld') "MeasureLlm mode guard failed: $Message"
    }
}
Invoke-UiSaveWorldCase 'requested LLM adds both flags and retains signed independent snapshots' {
    $Result = Invoke-UiSaveWorldFixture -MeasureLlm
    Assert-UiSaveWorldContract $Result.summary.succeeded $Result.summary.message
    Assert-UiSaveWorldContract ($Result.summary.llm_requested -and $Result.calls.Count -eq 1) 'LLM request did not launch.'
    foreach ($Cycle in $Result.summary.evidence.world_lifecycle.cycles) {
        $Engine = $Cycle.gc.attribution.engine_memory
        Assert-UiSaveWorldContract ($Engine.llm.enabled -and $Engine.llm.default_fmalloc_unused_bytes -eq -128 -and
            $Engine.trace.important_cache_used_bytes -gt $Engine.trace.important_cache_allocated_bytes) 'Independent signed observations were not preserved.'
    }
}
Invoke-UiSaveWorldCase 'unrequested LLM may be enabled by other configuration' {
    $Result = Invoke-UiSaveWorldFixture -MeasureLlm:$false -Mutate {
        param($R)
        $R.world_lifecycle.cycles[1].gc.attribution.engine_memory = New-UiSaveWorldEngineMemoryFixture 1 -LlmEnabled
    }
    Assert-UiSaveWorldContract $Result.summary.succeeded $Result.summary.message
    Assert-UiSaveWorldContract (-not $Result.summary.llm_requested -and
        -not $Result.summary.evidence.world_lifecycle.cycles[0].gc.attribution.engine_memory.llm.enabled -and
        $Result.summary.evidence.world_lifecycle.cycles[1].gc.attribution.engine_memory.llm.enabled) 'LLM observation was replaced by request state.'
}
Invoke-UiSaveWorldCase 'unavailable Trace retains all seven nulls' {
    $Result = Invoke-UiSaveWorldFixture -Mutate {
        param($R)
        $Trace = $R.world_lifecycle.cycles[0].gc.attribution.engine_memory.trace
        $Trace.available = $false
        foreach ($Field in @($Trace.Keys | Where-Object { $_ -like '*_bytes' })) { $Trace[$Field] = $null }
    }
    Assert-UiSaveWorldContract $Result.summary.succeeded $Result.summary.message
    $Trace = $Result.summary.evidence.world_lifecycle.cycles[0].gc.attribution.engine_memory.trace
    Assert-UiSaveWorldContract (-not $Trace.available -and
        @($Trace.PSObject.Properties | Where-Object { $_.Name -like '*_bytes' -and $null -eq $_.Value }).Count -eq 7) 'Unavailable Trace was not retained as null.'
}
Invoke-UiSaveWorldCase 'safe integer endpoints are accepted without cross-owner sums' {
    $Result = Invoke-UiSaveWorldFixture -MeasureLlm -Mutate {
        param($R)
        for ($Index = 0; $Index -lt 2; ++$Index) {
            $Engine = $R.world_lifecycle.cycles[$Index].gc.attribution.engine_memory
            foreach ($Field in @($Engine.trace.Keys | Where-Object { $_ -like '*_bytes' })) {
                $Engine.trace[$Field] = if ($Index -eq 0) { 0L } else { 9007199254740991L }
            }
            foreach ($Field in @($Engine.llm.Keys | Where-Object { $_ -like '*_bytes' })) {
                $Engine.llm[$Field] = if ($Index -eq 0) { -9007199254740991L } else { 9007199254740991L }
            }
        }
    }
    Assert-UiSaveWorldContract $Result.summary.succeeded $Result.summary.message
    Assert-UiSaveWorldContract ($Result.summary.evidence.world_lifecycle.cycles[0].gc.attribution.engine_memory.llm.default_total_bytes -eq -9007199254740991L -and
        $Result.summary.evidence.world_lifecycle.cycles[1].gc.attribution.engine_memory.llm.default_total_bytes -eq 9007199254740991L) 'Signed endpoints lost precision.'
}
foreach ($DisabledCycle in @(0, 1)) {
    Invoke-UiSaveWorldCase "requested LLM must be enabled in cycle $DisabledCycle" {
        $Result = Invoke-UiSaveWorldFixture -MeasureLlm -Mutate {
            param($R)
            $R.world_lifecycle.cycles[$DisabledCycle].gc.attribution.engine_memory = New-UiSaveWorldEngineMemoryFixture $DisabledCycle
        }
        Assert-UiSaveWorldContract (-not $Result.summary.succeeded -and -not $Result.summary.world_lifecycle_verified -and
            $Result.summary.llm_requested -and $Result.summary.message -like '*requested llm.enabled*') 'Requested LLM silently degraded.'
    }
}

$EngineFaults = [ordered]@{
    'missing engine schema version' = { param($E) $E.Remove('schema_version') }
    'wrong schema version' = { param($E) $E.schema_version = 2 }
    'string schema version' = { param($E) $E.schema_version = '1' }
    'empty allocator name' = { param($E) $E.allocator_name = '' }
    'whitespace allocator name' = { param($E) $E.allocator_name = ' ' }
    'numeric allocator name' = { param($E) $E.allocator_name = 42 }
    'missing allocator name' = { param($E) $E.Remove('allocator_name') }
    'missing sample frame' = { param($E) $E.Remove('sample_frame') }
    'negative sample frame' = { param($E) $E.sample_frame = -1 }
    'fractional sample frame' = { param($E) $E.sample_frame = 0.5 }
    'string sample frame' = { param($E) $E.sample_frame = '0' }
    'wrong consistency' = { param($E) $E.sample_consistency = 'atomic' }
    'missing consistency' = { param($E) $E.Remove('sample_consistency') }
    'missing trace' = { param($E) $E.Remove('trace') }
    'missing trace availability' = { param($E) $E.trace.Remove('available') }
    'string trace availability' = { param($E) $E.trace.available = 'true' }
    'missing names' = { param($E) $E.Remove('names') }
    'missing llm' = { param($E) $E.Remove('llm') }
    'missing llm enabled' = { param($E) $E.llm.Remove('enabled') }
    'string llm enabled' = { param($E) $E.llm.enabled = 'false' }
    'wrong llm origin' = { param($E) $E.llm.sample_origin = 'fresh' }
    'missing llm origin' = { param($E) $E.llm.Remove('sample_origin') }
}
foreach ($EngineFault in $EngineFaults.Keys) {
    Invoke-UiSaveWorldCase $EngineFault {
        Assert-UiSaveWorldEngineFixtureRejected -Mutate $EngineFaults[$EngineFault]
    }
}
foreach ($Owner in @('trace', 'names', 'llm')) {
    $OwnerFixture = (New-UiSaveWorldEngineMemoryFixture 0)[$Owner]
    $Fields = @($OwnerFixture.Keys | Where-Object { $_ -like '*_bytes' -or $_ -like '*_count' })
    foreach ($Field in $Fields) {
        $Variants = if ($Owner -ceq 'names') { @('missing', 'invalid') } else { @('missing', 'invalid', 'inactive missing', 'inactive zero') }
        foreach ($Variant in $Variants) {
            Invoke-UiSaveWorldCase "$Owner.$Field $Variant" {
                Assert-UiSaveWorldEngineFixtureRejected -LlmEnabled:($Owner -ceq 'llm' -and $Variant -notlike 'inactive*') -Mutate {
                    param($E)
                    $Values = $E[$Owner]
                    if ($Variant -like 'inactive*') {
                        $Values[$(if ($Owner -ceq 'trace') { 'available' } else { 'enabled' })] = $false
                        foreach ($ByteField in @($Values.Keys | Where-Object { $_ -like '*_bytes' })) { $Values[$ByteField] = $null }
                    }
                    switch ($Variant) {
                        'missing' { $Values.Remove($Field) }
                        'invalid' { $Values[$Field] = '123' }
                        'inactive missing' { $Values.Remove($Field) }
                        'inactive zero' { $Values[$Field] = 0 }
                    }
                }
            }
        }
    }
    $NumericField = $Fields[0]
    $InvalidNumbers = @($null, $true, 1.5, 9007199254740992L)
    if ($Owner -ceq 'llm') { $InvalidNumbers += -9007199254740992L }
    elseif ($Owner -ceq 'trace') { $InvalidNumbers += -1 }
    else { $InvalidNumbers = @($null, $true, 1.5, -1) }
    foreach ($InvalidNumber in $InvalidNumbers) {
        Invoke-UiSaveWorldCase "$Owner rejects numeric value '$InvalidNumber'" {
            Assert-UiSaveWorldEngineFixtureRejected -LlmEnabled:($Owner -ceq 'llm') -Mutate {
                param($E)
                $E[$Owner][$NumericField] = $InvalidNumber
            }
        }
    }
}

$Faults = [ordered]@{
    'missing engine memory object' = { param($R) $R.world_lifecycle.cycles[0].gc.attribution.Remove('engine_memory') }
    'null engine memory object' = { param($R) $R.world_lifecycle.cycles[0].gc.attribution.engine_memory = $null }
    'array engine memory object' = { param($R) $R.world_lifecycle.cycles[0].gc.attribution.engine_memory = @($R.world_lifecycle.cycles[0].gc.attribution.engine_memory) }
    'sample frame repeated' = { param($R) $R.world_lifecycle.cycles[1].gc.attribution.engine_memory.sample_frame = 0 }
    'sample frame regressed' = { param($R) $R.world_lifecycle.cycles[0].gc.attribution.engine_memory.sample_frame = 2 }
    'missing attribution is not zero' = { param($R) $R.world_lifecycle.cycles[0].gc.Remove('attribution') }
    'attribution byte string' = { param($R) $R.world_lifecycle.cycles[0].gc.attribution.artifact_cache_allocated_bytes = '2048' }
    'unbalanced backend lifetime counts' = { param($R) ++$R.world_lifecycle.cycles[0].gc.attribution.backend_live }
    'cache entries exceed capacity' = { param($R) $R.world_lifecycle.cycles[0].gc.attribution.attestation_entries = 33 }
    'observer history count differs' = { param($R) ++$R.world_lifecycle.cycles[0].gc.attribution.observer_retained_actions }
    'lifetime counters went backwards' = { param($R) $R.world_lifecycle.cycles[1].gc.attribution.backend_created = 1; $R.world_lifecycle.cycles[1].gc.attribution.backend_destroyed = 0 }
    'failed report despite exit zero' = { param($R) $R.succeeded = $false; $R.failure_category = 'fixture_native_failure' }
    'wrong package' = { param($R) $R.runtime.package_id = 'a' * 64 }
    'wrong runtime snapshot' = { param($R) $R.runtime_snapshot_phase = 'before_teardown' }
    'nonempty root actions' = { param($R) $R.actions = @($R.world_lifecycle.cycles[0].actions[0]) }
    'missing final recovery' = { param($R) $R.world_lifecycle.final_actions = @($R.world_lifecycle.final_actions[0]) }
    'wrong final recovery score' = { param($R) $R.world_lifecycle.final_actions[1].observed_score = '0' }
    'wrong final Guest events' = { param($R) $R.runtime.events = 3 }
    'wrong World counts' = { param($R) $R.world_lifecycle.activated_worlds = 2 }
    'missing travel cycle' = { param($R) $R.world_lifecycle.cycles = @($R.world_lifecycle.cycles[0]) }
    'wrong prior saved score' = { param($R) $R.world_lifecycle.cycles[1].actions[1].observed_score = '2' }
    'numeric action score' = { param($R) $R.world_lifecycle.cycles[0].actions[1].observed_score = 1 }
    'broken hash chain' = { param($R) $R.world_lifecycle.cycles[1].save_sha256_before = 'a' * 64 }
    'GC changed file hash' = { param($R) $R.world_lifecycle.cycles[0].actions[-1].save_sha256_after = 'a' * 64 }
    'final load changed file hash' = { param($R) $R.world_lifecycle.final_actions[1].save_sha256_after = 'a' * 64 }
    'action frame did not cross safe tick' = { param($R) $R.world_lifecycle.cycles[0].actions[0].check_frame = $R.world_lifecycle.cycles[0].actions[0].dispatch_frame }
    'cleanup still owns session' = { param($R) $R.world_lifecycle.cycles[0].cleanup.session_released = $false }
    'cleanup boolean string' = { param($R) $R.world_lifecycle.cycles[0].cleanup.widget_removed = 'true' }
    'cleanup delivered event' = { param($R) ++$R.world_lifecycle.cycles[0].cleanup.events_after }
    'old world survived GC' = { param($R) $R.world_lifecycle.cycles[0].gc.world_collected = $false }
    'extra active session' = { param($R) $R.world_lifecycle.cycles[0].gc.active_sessions = 2 }
    'retained ownership' = { param($R) $R.world_lifecycle.cycles[0].resources.owned_entries = 1 }
    'borrowed registry growth' = { param($R) $R.world_lifecycle.cycles[1].resources.borrowed_entries = 9 }
    'pending continuation' = { param($R) $R.world_lifecycle.cycles[0].resources.pending_continuations = 1 }
    'unmeasured backend' = { param($R) $R.world_lifecycle.cycles[0].resources.backend.measured = $false }
    'memory byte string' = { param($R) $R.world_lifecycle.cycles[0].gc.physical_bytes = '4294967297' }
    'forged memory summary' = { param($R) ++$R.world_lifecycle.memory_summary.peak_virtual_bytes }
    'false memory stable claim' = { param($R) $R.world_lifecycle.memory_summary.memory_stable = $true }
    'too short dwell duration' = { param($R) $R.world_lifecycle.elapsed_seconds = 1 }
    'false long-run claim' = { param($R) $R.long_run_verified = $true; $R.world_lifecycle.long_run_verified = $true }
    'timestamp contradicts elapsed' = { param($R) $R.finished_utc = $R.started_utc }
    'wrong isolated save path' = { param($R) $R.save_path = 'C:\fixture-engine\Saved\SaveGames\AvidScript_UiSaveDemo_v1.sav' }
}
foreach ($WorldFault in $Faults.Keys) {
    Invoke-UiSaveWorldCase $WorldFault {
        $Result = Invoke-UiSaveWorldFixture -Mutate $Faults[$WorldFault]
        Assert-UiSaveWorldContract (-not $Result.summary.succeeded -and $Result.summary.status -ceq 'failed' -and
            $Result.summary.message.Length -gt 0 -and -not $Result.summary.world_lifecycle_verified) "Accepted forged evidence: $WorldFault"
    }
}
foreach ($ProcessFault in @('changed file', 'missing report', 'oversized report', 'process failed', 'forged process duration', 'existing root')) {
    Invoke-UiSaveWorldCase $ProcessFault {
        $Result = Invoke-UiSaveWorldFixture -Fault $ProcessFault
        Assert-UiSaveWorldContract (-not $Result.summary.succeeded) "Accepted invalid process/file evidence: $ProcessFault"
    }
}
foreach ($InputFault in @('missing package', 'project root')) {
    Invoke-UiSaveWorldCase $InputFault {
        $Rejected = $false
        try { [void](Invoke-UiSaveWorldFixture -Fault $InputFault) } catch { $Rejected = $true }
        Assert-UiSaveWorldContract $Rejected "Accepted unsafe input: $InputFault"
    }
}

Complete-UiSaveWorldContracts
