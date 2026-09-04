#requires -Version 7.0
Set-StrictMode -Version Latest

function Assert-AvidScriptUiSaveWorldEqual {
    param([object]$Actual, [object]$Expected, [string]$Name)
    if ($Expected -is [bool]) { $Valid = $Actual -is [bool] -and $Actual -eq $Expected }
    elseif ($Expected -is [string]) { $Valid = $Actual -is [string] -and $Actual -ceq $Expected }
    else { $Valid = ($Actual -is [int] -or $Actual -is [long]) -and $Actual -eq $Expected }
    if (-not $Valid) { throw "World evidence mismatch: $Name" }
}

function Assert-AvidScriptUiSaveWorldNumber {
    param([object]$Actual, [string]$Name, [double]$Minimum = 0, [double]$Maximum = [long]::MaxValue, [switch]$Integer)
    $Numeric = $Actual -is [int] -or $Actual -is [long]
    if (-not $Integer) { $Numeric = $Numeric -or $Actual -is [double] -or $Actual -is [decimal] }
    if (-not $Numeric -or -not [double]::IsFinite([double]$Actual) -or $Actual -lt $Minimum -or $Actual -gt $Maximum) {
        throw "World evidence number is invalid: $Name"
    }
}

function Assert-AvidScriptUiSaveWorldHash {
    param([object]$Actual, [string]$Name)
    if ($Actual -isnot [string] -or $Actual -cnotmatch '\A[0-9a-f]{64}\z') { throw "World evidence hash is invalid: $Name" }
}

function Get-AvidScriptUiSaveWorldBudget {
    param([ValidateRange(2, 1000)][int]$Cycles, [ValidateRange(0, 21600)][int]$Seconds)
    return $Seconds + [Math]::Max(120, $Cycles * 20)
}

function Assert-AvidScriptUiSaveWorldResources {
    param([object]$Resources)
    foreach ($Field in @('active_subscriptions', 'bound_buttons')) { Assert-AvidScriptUiSaveWorldEqual $Resources.$Field 4 "resources.$Field" }
    foreach ($Field in @('owned_entries', 'pending_timers', 'pending_continuations', 'prepared_continuations', 'prepared_subscriptions')) {
        Assert-AvidScriptUiSaveWorldEqual $Resources.$Field 0 "resources.$Field"
    }
    Assert-AvidScriptUiSaveWorldNumber $Resources.borrowed_entries 'resources.borrowed_entries' 7 8 -Integer
    $Backend = $Resources.backend
    Assert-AvidScriptUiSaveWorldEqual $Backend.measured $true 'backend.measured'
    Assert-AvidScriptUiSaveWorldEqual $Backend.source 'CaptureLiveSnapshot.BackendInfo' 'backend.source'
    foreach ($Field in @('backend_id', 'backend_kind', 'execution_mode', 'artifact_format', 'runtime_version',
            'runtime_build_identity', 'runtime_artifact_sha256', 'target_triple')) {
        if ($Backend.$Field -isnot [string]) { throw "World backend field must be a string: $Field" }
    }
    if ([string]::IsNullOrWhiteSpace($Backend.backend_id) -or $Backend.backend_kind -cnotin @('wasmtime', 'wamr') -or
        $Backend.execution_mode -cnotin @('auto', 'interpreter', 'aot', 'jit') -or
        $Backend.artifact_format -cnotin @('wasm_bytecode', 'wamr_aot', 'wasmtime_serialized')) { throw 'World backend identity is invalid.' }
    if ($Backend.runtime_artifact_sha256 -cne '') { Assert-AvidScriptUiSaveWorldHash $Backend.runtime_artifact_sha256 'backend.runtime_artifact_sha256' }
}

function Assert-AvidScriptUiSaveWorldActions {
    param([object]$Actions, [array]$Steps, [string]$BeforeHash, [string]$SavedHash, [long]$PreviousFrame)
    if ($Actions -isnot [array] -or $Actions.Count -ne $Steps.Count) { throw 'World action count is incomplete.' }
    $Hash = $BeforeHash
    for ($Index = 0; $Index -lt $Steps.Count; ++$Index) {
        $Action = $Actions[$Index]
        $Step = $Steps[$Index]
        Assert-AvidScriptUiSaveWorldEqual $Action.action $Step[0] 'action order'
        Assert-AvidScriptUiSaveWorldEqual $Action.passed $true 'action.passed'
        foreach ($Field in @('expected_score', 'observed_score')) { Assert-AvidScriptUiSaveWorldEqual $Action.$Field $Step[1] $Field }
        foreach ($Field in @('expected_status', 'observed_status')) { Assert-AvidScriptUiSaveWorldEqual $Action.$Field $Step[2] $Field }
        Assert-AvidScriptUiSaveWorldNumber $Action.dispatch_frame 'dispatch_frame' ($PreviousFrame + 1) -Integer
        Assert-AvidScriptUiSaveWorldNumber $Action.check_frame 'check_frame' ($Action.dispatch_frame + 2) -Integer
        Assert-AvidScriptUiSaveWorldEqual $Action.save_sha256_before $Hash 'action.save_sha256_before'
        if ($Step[0] -ceq 'save') { $Hash = $SavedHash }
        Assert-AvidScriptUiSaveWorldEqual $Action.save_sha256_after $Hash 'action.save_sha256_after'
        $PreviousFrame = $Action.check_frame
    }
    return $PreviousFrame
}

function Assert-AvidScriptUiSaveWorldMemory {
    param([object]$World)
    $Memory = $World.memory_summary
    $HasBaseline = $World.completed_cycles -ge 3
    Assert-AvidScriptUiSaveWorldEqual $Memory.baseline_available $HasBaseline 'memory baseline_available'
    if ($Memory.PSObject.Properties.Name -contains 'memory_stable') { throw 'World memory is observation, not a memory_stable claim.' }
    foreach ($Kind in @('physical', 'virtual')) {
        $Field = "${Kind}_bytes"
        $Peak = 0L
        foreach ($Cycle in $World.cycles) { $Peak = [Math]::Max($Peak, [long]$Cycle.gc.$Field) }
        $Baseline = if ($HasBaseline) { $World.cycles[2].gc.$Field } else { 0L }
        Assert-AvidScriptUiSaveWorldEqual $Memory."baseline_$Field" $Baseline "memory.baseline_$Field"
        Assert-AvidScriptUiSaveWorldEqual $Memory."final_$Field" $World.cycles[-1].gc.$Field "memory.final_$Field"
        Assert-AvidScriptUiSaveWorldEqual $Memory."peak_$Field" $Peak "memory.peak_$Field"
    }
}

function Assert-AvidScriptUiSaveWorldEngineMemory {
    param([object]$EngineMemory, [object]$Previous)
    if ($EngineMemory -isnot [pscustomobject]) { throw 'World engine_memory must be an object.' }
    Assert-AvidScriptUiSaveWorldEqual $EngineMemory.schema_version 1 'engine_memory.schema_version'
    if ($EngineMemory.allocator_name -isnot [string] -or [string]::IsNullOrWhiteSpace($EngineMemory.allocator_name)) {
        throw 'World engine_memory.allocator_name must be a nonempty string.'
    }
    Assert-AvidScriptUiSaveWorldNumber $EngineMemory.sample_frame 'engine_memory.sample_frame' 0 -Integer
    if ($null -ne $Previous -and $EngineMemory.sample_frame -le $Previous.sample_frame) {
        throw 'World engine_memory.sample_frame must increase each cycle.'
    }
    Assert-AvidScriptUiSaveWorldEqual $EngineMemory.sample_consistency 'owner_snapshots_not_atomic' 'engine_memory.sample_consistency'
    foreach ($Owner in @('trace', 'names', 'llm')) {
        if ($EngineMemory.$Owner -isnot [pscustomobject]) { throw "World engine_memory.$Owner must be an object." }
    }
    $Trace = $EngineMemory.trace
    if ($Trace.available -isnot [bool]) { throw 'World engine_memory.trace.available must be a boolean.' }
    foreach ($Field in @('memory_used_bytes', 'block_pool_bytes', 'fixed_buffer_bytes', 'shared_buffer_bytes',
            'important_cache_allocated_bytes', 'important_cache_used_bytes', 'thread_registry_bytes')) {
        if ($Trace.available) {
            Assert-AvidScriptUiSaveWorldNumber $Trace.$Field "engine_memory.trace.$Field" 0 9007199254740991 -Integer
        } elseif ($null -ne $Trace.$Field) { throw "World engine_memory.trace.$Field must be null when unavailable." }
    }
    foreach ($Field in @('entry_bytes', 'table_bytes', 'ansi_count', 'wide_count')) {
        Assert-AvidScriptUiSaveWorldNumber $EngineMemory.names.$Field "engine_memory.names.$Field" 0 -Integer
    }
    $Llm = $EngineMemory.llm
    if ($Llm.enabled -isnot [bool]) { throw 'World engine_memory.llm.enabled must be a boolean.' }
    Assert-AvidScriptUiSaveWorldEqual $Llm.sample_origin 'live_totals_and_aggregated_tags' 'engine_memory.llm.sample_origin'
    # Owners are independent snapshots; do not infer totals or Used/Allocated relationships.
    foreach ($Field in @('default_total_bytes', 'platform_total_bytes', 'platform_fmalloc_bytes', 'platform_overhead_bytes',
            'default_fmalloc_unused_bytes', 'default_uobject_bytes', 'default_fname_bytes', 'default_untagged_bytes', 'default_engine_misc_bytes')) {
        if ($Llm.enabled) {
            Assert-AvidScriptUiSaveWorldNumber $Llm.$Field "engine_memory.llm.$Field" -9007199254740991 9007199254740991 -Integer
        } elseif ($null -ne $Llm.$Field) { throw "World engine_memory.llm.$Field must be null when disabled." }
    }
}

function Assert-AvidScriptUiSaveWorldAttribution {
    param([object]$Cycle, [object]$Previous, [long]$RetainedActions)
    $Attribution = $Cycle.gc.attribution
    Assert-AvidScriptUiSaveWorldEqual $Attribution.schema_version 1 'attribution.schema_version'
    Assert-AvidScriptUiSaveWorldEqual $Attribution.observer_estimate_kind 'ue_json_memory_footprint' 'attribution.observer_estimate_kind'
    Assert-AvidScriptUiSaveWorldEqual $Attribution.observer_retained_cycles $Cycle.cycle 'attribution.observer_retained_cycles'
    Assert-AvidScriptUiSaveWorldEqual $Attribution.observer_retained_actions $RetainedActions 'attribution.observer_retained_actions'
    foreach ($Field in @('backend_created', 'backend_destroyed', 'backend_live', 'artifact_cache_entries',
            'artifact_cache_capacity', 'artifact_cache_allocated_bytes', 'attestation_entries', 'attestation_capacity',
            'attestation_allocated_bytes', 'observer_json_estimated_bytes')) {
        Assert-AvidScriptUiSaveWorldNumber $Attribution.$Field "attribution.$Field" 0 9007199254740991 -Integer
    }
    Assert-AvidScriptUiSaveWorldNumber $Attribution.backend_live 'attribution.backend_live' 1 -Integer
    Assert-AvidScriptUiSaveWorldEqual ($Attribution.backend_created - $Attribution.backend_destroyed) $Attribution.backend_live 'attribution.backend_balance'
    foreach ($Cache in @('artifact_cache', 'attestation')) {
        Assert-AvidScriptUiSaveWorldNumber $Attribution."${Cache}_capacity" "attribution.${Cache}_capacity" 1 -Integer
        Assert-AvidScriptUiSaveWorldNumber $Attribution."${Cache}_entries" "attribution.${Cache}_entries" 0 $Attribution."${Cache}_capacity" -Integer
    }
    Assert-AvidScriptUiSaveWorldNumber $Attribution.observer_json_estimated_bytes 'attribution.observer_json_estimated_bytes' 1 -Integer
    if ($null -ne $Previous) {
        foreach ($Field in @('backend_created', 'backend_destroyed', 'observer_json_estimated_bytes')) {
            Assert-AvidScriptUiSaveWorldNumber $Attribution.$Field "attribution.$Field monotonic" $Previous.$Field -Integer
        }
    }
    $PreviousEngineMemory = if ($null -ne $Previous) { $Previous.engine_memory } else { $null }
    Assert-AvidScriptUiSaveWorldEngineMemory $Attribution.engine_memory $PreviousEngineMemory
}

function Assert-AvidScriptUiSaveWorldCycles {
    param([object]$World)
    $PreviousHash = ''
    $PreviousFrame = -1L
    $PreviousAttribution = $null
    $RetainedActions = 0L
    $Backend = $World.cycles[0].resources.backend
    for ($Index = 0; $Index -lt $World.cycles.Count; ++$Index) {
        $Cycle = $World.cycles[$Index]
        $Number = $Index + 1
        Assert-AvidScriptUiSaveWorldEqual $Cycle.cycle $Number 'cycle ordinal'
        Assert-AvidScriptUiSaveWorldEqual $Cycle.saved_score $Number 'saved_score'
        Assert-AvidScriptUiSaveWorldEqual $Cycle.save_sha256_before $PreviousHash 'cycle save hash chain'
        Assert-AvidScriptUiSaveWorldHash $Cycle.save_sha256 'cycle.save_sha256'
        if ($Cycle.save_sha256 -ceq $PreviousHash) { throw 'World Save did not change the persisted score/hash.' }
        Assert-AvidScriptUiSaveWorldNumber $Cycle.save_file_bytes 'cycle.save_file_bytes' 1 -Integer
        Assert-AvidScriptUiSaveWorldEqual $Cycle.passed $true 'cycle.passed'
        $Events = if ($Number -eq 1) { 2 } else { 3 }
        Assert-AvidScriptUiSaveWorldEqual $Cycle.events_before_travel $Events 'events_before_travel'
        Assert-AvidScriptUiSaveWorldResources $Cycle.resources
        foreach ($Field in @('backend_id', 'backend_kind', 'execution_mode', 'artifact_format', 'runtime_version',
                'runtime_build_identity', 'runtime_artifact_sha256', 'target_triple')) {
            Assert-AvidScriptUiSaveWorldEqual $Cycle.resources.backend.$Field $Backend.$Field "cycle backend.$Field"
        }
        foreach ($Field in @('observed', 'component_end_play', 'guest_end_play', 'session_released', 'owner_released',
                'buttons_unbound', 'widget_removed', 'saved_reference_cleared')) {
            Assert-AvidScriptUiSaveWorldEqual $Cycle.cleanup.$Field $true "cleanup.$Field"
        }
        foreach ($Field in @('events_before', 'events_after')) { Assert-AvidScriptUiSaveWorldEqual $Cycle.cleanup.$Field $Events "cleanup.$Field" }
        foreach ($Field in @('world_collected', 'host_collected', 'component_collected', 'widget_collected', 'saved_object_collected',
                'new_world_identity', 'new_host_identity', 'new_component_identity')) {
            Assert-AvidScriptUiSaveWorldEqual $Cycle.gc.$Field $true "gc.$Field"
        }
        foreach ($Field in @('physical_bytes', 'virtual_bytes', 'uobject_count')) {
            Assert-AvidScriptUiSaveWorldNumber $Cycle.gc.$Field "gc.$Field" 0 -Integer
        }
        Assert-AvidScriptUiSaveWorldEqual $Cycle.gc.active_sessions 1 'gc.active_sessions'
        $Steps = ,@('ready', '0', 'Ready')
        if ($Number -gt 1) { $Steps += ,@('load', [string]($Number - 1), 'Loaded') }
        $Steps += @(@('collect', [string]$Number, 'Collected'), @('save', [string]$Number, 'Saved'), @('gc', [string]$Number, 'Saved'))
        $PreviousFrame = Assert-AvidScriptUiSaveWorldActions $Cycle.actions $Steps $PreviousHash $Cycle.save_sha256 $PreviousFrame
        $RetainedActions += $Cycle.actions.Count
        Assert-AvidScriptUiSaveWorldAttribution $Cycle $PreviousAttribution $RetainedActions
        $PreviousAttribution = $Cycle.gc.attribution
        $PreviousHash = $Cycle.save_sha256
    }
    [void](Assert-AvidScriptUiSaveWorldActions $World.final_actions @(@('ready', '0', 'Ready'),
        @('load', [string]$World.completed_cycles, 'Loaded')) $PreviousHash $PreviousHash $PreviousFrame)
    Assert-AvidScriptUiSaveWorldMemory $World
}

function Resolve-AvidScriptUiSaveWorldReport {
    param([string]$ReportPath, [string]$UserRoot, [string]$PackageId,
        [ValidateRange(2, 1000)][int]$Cycles, [ValidateRange(0, 21600)][int]$Seconds)
    Assert-AvidScriptUiSaveSafePath $ReportPath
    if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf)) { throw 'World probe report is missing.' }
    $Stream = [IO.File]::Open($ReportPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($Stream.Length -gt 64MB) { throw 'World probe report exceeds 64 MiB.' }
        $Reader = [IO.StreamReader]::new($Stream, [Text.UTF8Encoding]::new($false, $true), $true)
        try { $Json = $Reader.ReadToEnd() } finally { $Reader.Dispose() }
    } finally { $Stream.Dispose() }
    $JsonOptions = @{ Depth = 32 }
    if ($PSVersionTable.PSVersion -ge [version]'7.5') { $JsonOptions.DateKind = 'String' }
    $Report = $Json | ConvertFrom-Json @JsonOptions
    if (-not $JsonOptions.ContainsKey('DateKind')) {
        # Older PowerShell eagerly converts ISO timestamps; preserve their JSON string types.
        $Document = [Text.Json.JsonDocument]::Parse($Json)
        try {
            foreach ($Field in @('started_utc', 'finished_utc')) {
                $Element = $Document.RootElement.GetProperty($Field)
                if ($Element.ValueKind -ne [Text.Json.JsonValueKind]::String) { throw "World timestamp is not a JSON string: $Field" }
                $Report.$Field = $Element.GetString()
            }
        } finally { $Document.Dispose() }
    }
    $Expected = [ordered]@{ schema_version = 1; result = 'avidscript_ui_save_probe_passed'; succeeded = $true; failure_category = ''
        mode = 'world'; process_mode = 'editor_binary_game'; input_kind = 'synthetic_ue_button_onclicked_broadcast'
        expected_module_id = 'avidscript.ui_save_demo'; expected_package_id = $PackageId; map = '/AvidScript/Demos/UiSave/L_UiSave'
        physical_click_verified = $false; visual_verified = $false; gc_performed = $true; runtime_snapshot_phase = 'final_active'
        save_file_exists = $true; initial_save_sha256 = ''; status_text = 'Loaded' }
    foreach ($Field in $Expected.Keys) { Assert-AvidScriptUiSaveWorldEqual $Report.$Field $Expected[$Field] $Field }
    Assert-AvidScriptUiSaveWorldNumber $Report.process_id 'process_id' 1 ([int]::MaxValue) -Integer
    if ($Report.actions -isnot [array] -or $Report.actions.Count -ne 0) { throw 'World root actions must be empty.' }
    $Budget = Get-AvidScriptUiSaveWorldBudget $Cycles $Seconds
    Assert-AvidScriptUiSaveWorldEqual $Report.timeout_seconds $Budget 'timeout_seconds'
    Assert-AvidScriptUiSaveWorldNumber $Report.elapsed_seconds 'elapsed_seconds' 0 $Budget
    $Times = @()
    foreach ($Field in @('started_utc', 'finished_utc')) {
        $Time = [DateTimeOffset]::MinValue
        if ($Report.$Field -isnot [string] -or $Report.$Field -cnotmatch '(?:Z|\+00:00)\z' -or
            -not [DateTimeOffset]::TryParse($Report.$Field, [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::None, [ref]$Time)) { throw "World timestamp is invalid: $Field" }
        $Times += $Time
    }
    $WallSeconds = ($Times[1] - $Times[0]).TotalSeconds
    if ($WallSeconds -lt 0 -or [Math]::Abs($WallSeconds - $Report.elapsed_seconds) -gt 2) { throw 'World elapsed and timestamps disagree.' }
    foreach ($Field in @('resolved_from_package', 'runtime_loaded', 'begin_play', 'owner_registered', 'owner_handle_valid')) {
        Assert-AvidScriptUiSaveWorldEqual $Report.runtime.$Field $true "runtime.$Field"
    }
    foreach ($Field in @('module_id', 'package_id', 'error_message', 'events', 'dropped_events')) {
        $Value = switch ($Field) { 'module_id' { 'avidscript.ui_save_demo' }; 'package_id' { $PackageId }; 'error_message' { '' }; 'events' { 1 }; default { 0 } }
        Assert-AvidScriptUiSaveWorldEqual $Report.runtime.$Field $Value "runtime.$Field"
    }
    foreach ($Field in @('scenario_id', 'active', 'error_category', 'error_message')) {
        $Value = switch ($Field) { 'scenario_id' { 'ui_save_demo' }; 'active' { $true }; default { '' } }
        Assert-AvidScriptUiSaveWorldEqual $Report.startup.$Field $Value "startup.$Field"
    }
    $World = $Report.world_lifecycle
    Assert-AvidScriptUiSaveWorldEqual $World.requested_cycles $Cycles 'requested_cycles'
    Assert-AvidScriptUiSaveWorldEqual $World.requested_soak_seconds $Seconds 'requested_soak_seconds'
    Assert-AvidScriptUiSaveWorldNumber $World.completed_cycles 'completed_cycles' $Cycles 10000 -Integer
    $Completed = [int]$World.completed_cycles
    if ($Seconds -eq 0) { Assert-AvidScriptUiSaveWorldEqual $Completed $Cycles 'zero-soak completed_cycles' }
    Assert-AvidScriptUiSaveWorldEqual $World.activated_worlds ($Completed + 1) 'activated_worlds'
    foreach ($Field in @('travel_count', 'cleanup_count', 'final_recovered_score')) { Assert-AvidScriptUiSaveWorldEqual $World.$Field $Completed $Field }
    Assert-AvidScriptUiSaveWorldEqual $World.warmup_cycles 3 'warmup_cycles'
    foreach ($Field in @('world_lifecycle_verified', 'memory_measured')) { Assert-AvidScriptUiSaveWorldEqual $World.$Field $true $Field }
    Assert-AvidScriptUiSaveWorldNumber $World.elapsed_seconds 'world_lifecycle.elapsed_seconds' ([Math]::Max($Seconds, $Completed * 3)) $Report.elapsed_seconds
    $LongRun = $World.elapsed_seconds -ge 3600 -and $Completed -ge 10
    Assert-AvidScriptUiSaveWorldEqual $Report.long_run_verified $LongRun 'long_run_verified'
    Assert-AvidScriptUiSaveWorldEqual $World.long_run_verified $LongRun 'world_lifecycle.long_run_verified'
    Assert-AvidScriptUiSaveWorldEqual $Report.score_text ([string]$Completed) 'score_text'
    if ($World.cycles -isnot [array] -or $World.cycles.Count -ne $Completed) { throw 'World cycle count is incomplete.' }
    Assert-AvidScriptUiSaveWorldCycles $World
    $SavePath = Join-Path $UserRoot 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
    if (-not (Test-AvidScriptUiSaveSamePath $Report.user_dir $UserRoot) -or
        -not (Test-AvidScriptUiSaveSamePath $Report.save_path $SavePath)) { throw 'World report escaped the isolated UserDir/save path.' }
    Assert-AvidScriptUiSaveSaveDirectory $SavePath
    Assert-AvidScriptUiSaveWorldNumber $Report.save_file_bytes 'save_file_bytes' 1 -Integer
    Assert-AvidScriptUiSaveWorldHash $Report.save_file_sha256 'save_file_sha256'
    Assert-AvidScriptUiSaveWorldEqual $World.final_save_sha256 $Report.save_file_sha256 'final_save_sha256'
    Assert-AvidScriptUiSaveWorldEqual $World.cycles[-1].save_sha256 $Report.save_file_sha256 'last cycle hash'
    Assert-AvidScriptUiSaveWorldEqual $World.cycles[-1].save_file_bytes $Report.save_file_bytes 'last cycle bytes'
    if (-not (Test-Path -LiteralPath $SavePath -PathType Leaf) -or
        (Get-Item -LiteralPath $SavePath).Length -ne $Report.save_file_bytes -or
        (Get-AvidScriptBindingSha256Hex $SavePath) -cne $Report.save_file_sha256) { throw 'World final save bytes/hash differ from the actual file.' }
    return $Report
}

function Invoke-AvidScriptUiSaveVerifyWorld {
    param([object]$Context, [string]$RunRoot, [string]$RunId, [string]$UserRoot)
    $Budget = Get-AvidScriptUiSaveWorldBudget $WorldCycles $SoakSeconds
    $ProcessBudget = $Budget + 30
    if ($UiSaveTimeoutExplicit) { $ProcessBudget = [Math]::Min($TimeoutSeconds, $ProcessBudget) }
    $Summary = [ordered]@{ schema_version = 1; result = 'avidscript_ui_save_world_verify_failed'; status = 'failed'; succeeded = $false
        mode = 'verifyworld'; run_id = $RunId; module_id = 'avidscript.ui_save_demo'; package_id = $ExpectedPackageId.ToLowerInvariant()
        evidence_root = $RunRoot; verify_user_root = $UserRoot; report_path = (Join-Path $RunRoot 'verify-world.json')
        probe_report_path = (Join-Path $RunRoot 'world.json'); editor_log = (Join-Path $RunRoot 'world.editor.log')
        process_mode = 'editor_binary_game'; input_kind = 'synthetic_ue_button_onclicked_broadcast'
        gameplay_acceptance = 'editor_world_synthetic_probe_only'; physical_click_verified = $false; visual_verified = $false
        packaged_verified = $false; world_lifecycle_verified = $false; long_run_verified = $false
        world_cycles = $WorldCycles; soak_seconds = $SoakSeconds; timeout_seconds = $ProcessBudget
        llm_requested = [bool]$MeasureLlm; memory_trace_requested = [bool]$TraceMemory; memory_trace = $null; message = '' }
    try {
        New-AvidScriptUiSaveDirectory $UserRoot
        $WorldRoot = Join-Path $UserRoot 'world'
        New-AvidScriptUiSaveDirectory $WorldRoot
        Assert-AvidScriptUiSaveUserRoot $WorldRoot $Context
        $SavePath = Join-Path $WorldRoot 'Saved/SaveGames/AvidScript_UiSaveDemo_v1.sav'
        Assert-AvidScriptUiSaveSaveDirectory $SavePath
        if (Test-Path -LiteralPath $SavePath) { throw 'World verification must start without a save.' }
        foreach ($Path in @($Summary.probe_report_path, $Summary.editor_log, (Join-Path $RunRoot 'world.process.log'))) {
            Assert-AvidScriptUiSaveSafePath $Path
            if (Test-Path -LiteralPath $Path) { throw 'World report/log must be new.' }
        }
        $TracePath = Join-Path $RunRoot 'world.memory.utrace'
        if ($TraceMemory) {
            Assert-AvidScriptUiSaveSafePath $TracePath
            if (Test-Path -LiteralPath $TracePath) { throw 'World memory trace must be new.' }
        }
        $Arguments = @($Context.project, '/AvidScript/Demos/UiSave/L_UiSave',
                '-game', '-ExecCmds=Module Load AvidScriptEditor', '-AvidScriptScenario=ui_save_demo', '-AvidScriptUiSaveProbe=world',
                "-AvidScriptUiSaveReport=$($Summary.probe_report_path)", "-AvidScriptUiSaveExpectedPackage=$($Summary.package_id)",
                "-AvidScriptUiSaveWorldCycles=$WorldCycles", "-AvidScriptUiSaveSoakSeconds=$SoakSeconds", "-UserDir=$WorldRoot",
                '-unattended', '-nullrhi', '-nosound', '-nop4', '-nosplash', '-stdout', '-FullStdOutLogOutput', "-abslog=$($Summary.editor_log)")
        if ($MeasureLlm) { $Arguments += @('-llm', '-AvidScriptUiSaveRequireLlm') }
        if ($TraceMemory) { $Arguments += @('-trace=memory,bookmark', "-tracefile=$TracePath") }
        $Process = Invoke-AvidScriptUiSaveTool -Executable $Context.editor -ProcessTimeoutSeconds $ProcessBudget `
            -LogPath (Join-Path $RunRoot 'world.process.log') -Arguments $Arguments
        $Summary.evidence = Resolve-AvidScriptUiSaveWorldReport $Summary.probe_report_path $WorldRoot $Summary.package_id $WorldCycles $SoakSeconds
        if ($MeasureLlm) {
            foreach ($Cycle in $Summary.evidence.world_lifecycle.cycles) {
                Assert-AvidScriptUiSaveWorldEqual $Cycle.gc.attribution.engine_memory.llm.enabled $true "cycle $($Cycle.cycle) requested llm.enabled"
            }
        }
        if ($TraceMemory) {
            if (-not (Test-Path -LiteralPath $TracePath -PathType Leaf) -or (Get-Item -LiteralPath $TracePath).Length -eq 0) {
                throw 'Requested World memory trace is missing or empty.'
            }
            $Summary.memory_trace = [ordered]@{ path = $TracePath; bytes = (Get-Item -LiteralPath $TracePath).Length
                sha256 = (Get-AvidScriptBindingSha256Hex $TracePath); analyzed = $false
                bookmark_prefix = 'AvidScript.WorldGC.' }
        }
        Assert-AvidScriptUiSaveWorldNumber $Process.elapsed_ms 'process.elapsed_ms' 0
        if ($Summary.evidence.elapsed_seconds -gt ($Process.elapsed_ms / 1000 + 1)) { throw 'World elapsed exceeds the measured child process duration.' }
        $Summary.probe_report_sha256 = Get-AvidScriptBindingSha256Hex $Summary.probe_report_path
        $Summary.world_lifecycle_verified = $true
        $Summary.long_run_verified = $Summary.evidence.world_lifecycle.long_run_verified
        $Summary.result = 'avidscript_ui_save_world_verify_passed'; $Summary.status = 'verified'; $Summary.succeeded = $true
    } catch { $Summary.message = $_.Exception.Message }
    Write-AvidScriptUiSaveNewJson $Summary.report_path $Summary
    return [pscustomobject]$Summary
}
