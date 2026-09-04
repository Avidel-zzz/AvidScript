#requires -Version 7.0
Set-StrictMode -Version Latest

function Assert-AvidScriptPackagedUiJsonNames {
    param([System.Text.Json.JsonElement]$Element)
    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $Names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($Property in $Element.EnumerateObject()) {
            if (-not $Names.Add($Property.Name)) { throw "Duplicate JSON property: $($Property.Name)" }
            Assert-AvidScriptPackagedUiJsonNames $Property.Value
        }
    } elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        foreach ($Item in $Element.EnumerateArray()) { Assert-AvidScriptPackagedUiJsonNames $Item }
    }
}

function Read-AvidScriptPackagedUiJson {
    param([string]$Path)
    Assert-AvidScriptUiSaveSafePath $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -gt 4MB) {
        throw "Missing or oversized JSON report: $Path"
    }
    $Text = [IO.File]::ReadAllText($Path)
    $Document = [System.Text.Json.JsonDocument]::Parse($Text)
    try {
        if ($Document.RootElement.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) { throw 'Report must be a JSON object.' }
        Assert-AvidScriptPackagedUiJsonNames $Document.RootElement
    } finally { $Document.Dispose() }
    $Options = @{ Depth = 64; NoEnumerate = $true }
    if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')) { $Options.DateKind = 'String' }
    return ($Text | ConvertFrom-Json @Options)
}

function Assert-AvidScriptPackagedUiEqual {
    param([object]$Actual, [object]$Expected, [string]$Name)
    $Valid = if ($Expected -is [bool]) { $Actual -is [bool] -and $Actual -eq $Expected }
    elseif ($Expected -is [string]) { $Actual -is [string] -and $Actual -ceq $Expected }
    else { ($Actual -is [int] -or $Actual -is [long]) -and $Actual -eq $Expected }
    if (-not $Valid) { throw "Packaged UI evidence mismatch: $Name" }
}

function Assert-AvidScriptPackagedUiNumber {
    param([object]$Actual, [string]$Name, [double]$Minimum = 0, [double]$Maximum = [long]::MaxValue, [switch]$Integer)
    $Numeric = $Actual -is [int] -or $Actual -is [long]
    if (-not $Integer) { $Numeric = $Numeric -or $Actual -is [double] -or $Actual -is [decimal] }
    if (-not $Numeric -or -not [double]::IsFinite([double]$Actual) -or $Actual -lt $Minimum -or $Actual -gt $Maximum) {
        throw "Packaged UI evidence number is invalid: $Name"
    }
}

function Assert-AvidScriptPackagedUiHash {
    param([object]$Value, [string]$Name)
    if ($Value -isnot [string] -or $Value -cnotmatch '\A[0-9a-f]{64}\z') { throw "Invalid SHA256: $Name" }
}

function Resolve-AvidScriptPackagedUiReport {
    param([string]$ReportPath, [ValidateSet('write', 'read')][string]$ProbeMode,
        [string]$Configuration, [string]$RunId, [string]$UserRoot, [string]$PackageId,
        [string]$WriteHash, [Collections.Generic.HashSet[int]]$ProcessIds,
        [DateTimeOffset]$LaunchedUtc, [DateTimeOffset]$ExitedUtc)

    $Report = Read-AvidScriptPackagedUiJson $ReportPath
    $Expected = [ordered]@{
        schema_version = 1; result = 'avidscript_ui_save_probe_passed'; succeeded = $true; failure_category = ''
        mode = $ProbeMode; configuration = $Configuration; process_mode = 'packaged_game'; requires_cooked_data = $true
        validation_plugin = 'AvidScriptValidation'; run_id = $RunId
        input_kind = 'synthetic_ue_button_onclicked_broadcast'; physical_click_verified = $false
        visual_verified = $false; long_run_verified = $false; expected_module_id = 'avidscript.ui_save_demo'
        expected_package_id = $PackageId; map = '/AvidScript/Demos/UiSave/L_UiSave'
        runtime_snapshot_phase = 'final_active'; timeout_seconds = 90; save_file_exists = $true
    }
    foreach ($Entry in $Expected.GetEnumerator()) { Assert-AvidScriptPackagedUiEqual $Report.($Entry.Key) $Entry.Value $Entry.Key }
    Assert-AvidScriptPackagedUiNumber $Report.process_id 'process_id' 1 ([int]::MaxValue) -Integer
    if (-not $ProcessIds.Add([int]$Report.process_id)) { throw 'Write and read must use distinct processes.' }
    Assert-AvidScriptPackagedUiNumber $Report.elapsed_seconds 'elapsed_seconds' 0 90
    $Started = [DateTimeOffset]::MinValue
    $Finished = [DateTimeOffset]::MinValue
    if ($Report.started_utc -isnot [string] -or $Report.finished_utc -isnot [string] -or
        -not [DateTimeOffset]::TryParse($Report.started_utc, [ref]$Started) -or
        -not [DateTimeOffset]::TryParse($Report.finished_utc, [ref]$Finished) -or
        $Started -lt $LaunchedUtc.AddSeconds(-1) -or $Finished -gt $ExitedUtc.AddSeconds(1) -or
        $Finished -lt $Started -or [Math]::Abs(($Finished - $Started).TotalSeconds - $Report.elapsed_seconds) -gt 1) {
        throw 'Probe timestamps do not match this process invocation.'
    }
    $Runtime = $Report.runtime
    foreach ($Field in @('resolved_from_package', 'runtime_loaded', 'begin_play', 'owner_registered', 'owner_handle_valid')) {
        Assert-AvidScriptPackagedUiEqual $Runtime.$Field $true "runtime.$Field"
    }
    Assert-AvidScriptPackagedUiEqual $Runtime.module_id 'avidscript.ui_save_demo' 'runtime.module_id'
    Assert-AvidScriptPackagedUiEqual $Runtime.package_id $PackageId 'runtime.package_id'
    Assert-AvidScriptPackagedUiEqual $Runtime.error_message '' 'runtime.error_message'
    Assert-AvidScriptPackagedUiEqual $Runtime.dropped_events 0 'runtime.dropped_events'
    Assert-AvidScriptPackagedUiEqual $Report.startup.active $true 'startup.active'
    Assert-AvidScriptPackagedUiEqual $Report.startup.scenario_id 'ui_save_demo' 'startup.scenario_id'
    foreach ($Field in @('error_category', 'error_message')) { Assert-AvidScriptPackagedUiEqual $Report.startup.$Field '' "startup.$Field" }
    $Backend = $Report.backend
    foreach ($Entry in ([ordered]@{ measured = $true; source = 'CaptureRuntimeDiagnostics.BackendInfo'; backend_kind = 'wasmtime'
                execution_mode = 'aot'; artifact_format = 'wasmtime_serialized'; target_triple = 'x86_64-pc-windows-msvc' }).GetEnumerator()) {
        Assert-AvidScriptPackagedUiEqual $Backend.($Entry.Key) $Entry.Value "backend.$($Entry.Key)"
    }
    foreach ($Field in @('backend_id', 'runtime_version', 'runtime_build_identity')) {
        if ($Backend.$Field -isnot [string] -or [string]::IsNullOrWhiteSpace($Backend.$Field)) { throw "Missing backend identity: $Field" }
    }
    Assert-AvidScriptPackagedUiHash $Backend.runtime_artifact_sha256 'backend.runtime_artifact_sha256'
    foreach ($Field in @('active_subscriptions', 'bound_buttons')) { Assert-AvidScriptPackagedUiEqual $Report.resources.$Field 4 $Field }
    foreach ($Field in @('owned_entries', 'pending_timers', 'pending_continuations', 'prepared_continuations', 'prepared_subscriptions')) {
        Assert-AvidScriptPackagedUiEqual $Report.resources.$Field 0 $Field
    }
    Assert-AvidScriptPackagedUiNumber $Report.resources.borrowed_entries 'borrowed_entries' 7 8 -Integer
    $Steps = ,@('ready', '0', 'Ready')
    if ($ProbeMode -ceq 'write') { $Steps += @(@('collect', '1', 'Collected'), @('collect', '2', 'Collected'),
            @('collect', '3', 'Collected'), @('save', '3', 'Saved')) }
    else { $Steps += ,@('load', '3', 'Loaded') }
    if ($Report.actions -isnot [array] -or $Report.actions.Count -ne $Steps.Count) { throw 'Incomplete action count.' }
    $PreviousFrame = -1L
    for ($Index = 0; $Index -lt $Steps.Count; ++$Index) {
        $Step = $Steps[$Index]
        $Action = $Report.actions[$Index]
        Assert-AvidScriptPackagedUiEqual $Action.action $Step[0] 'action order'
        Assert-AvidScriptPackagedUiEqual $Action.passed $true 'action.passed'
        foreach ($Field in @('expected_score', 'observed_score')) { Assert-AvidScriptPackagedUiEqual $Action.$Field $Step[1] $Field }
        foreach ($Field in @('expected_status', 'observed_status')) { Assert-AvidScriptPackagedUiEqual $Action.$Field $Step[2] $Field }
        Assert-AvidScriptPackagedUiNumber $Action.dispatch_frame 'dispatch_frame' ($PreviousFrame + 1) -Integer
        Assert-AvidScriptPackagedUiNumber $Action.check_frame 'check_frame' ($Action.dispatch_frame + 1) -Integer
        $PreviousFrame = $Action.check_frame
    }
    Assert-AvidScriptPackagedUiEqual $Runtime.events ($Steps.Count - 1) 'runtime.events'
    Assert-AvidScriptPackagedUiEqual $Report.score_text $Steps[-1][1] 'score_text'
    Assert-AvidScriptPackagedUiEqual $Report.status_text $Steps[-1][2] 'status_text'
    $SavedRoot = Join-Path $UserRoot 'Saved'
    $SavePath = Join-Path $SavedRoot 'SaveGames/AvidScript_UiSaveDemo_v1.sav'
    foreach ($Entry in ([ordered]@{ user_dir = $UserRoot; effective_saved_dir = $SavedRoot; save_path = $SavePath }).GetEnumerator()) {
        if (-not (Test-AvidScriptUiSaveSamePath $Report.($Entry.Key) $Entry.Value)) { throw "Isolated path mismatch: $($Entry.Key)" }
    }
    Assert-AvidScriptUiSaveSaveDirectory $SavePath
    Assert-AvidScriptPackagedUiNumber $Report.save_file_bytes 'save_file_bytes' 1 16MB -Integer
    Assert-AvidScriptPackagedUiEqual $Report.save_file_bytes (Get-Item -LiteralPath $SavePath).Length 'actual save size'
    Assert-AvidScriptPackagedUiHash $Report.save_file_sha256 'save_file_sha256'
    Assert-AvidScriptPackagedUiEqual $Report.save_file_sha256 (Get-AvidScriptBindingSha256Hex $SavePath) 'actual save hash'
    if ($ProbeMode -ceq 'write') { Assert-AvidScriptPackagedUiEqual $Report.initial_save_sha256 '' 'write started without save' }
    else {
        Assert-AvidScriptPackagedUiEqual $Report.initial_save_sha256 $WriteHash 'read initial save'
        Assert-AvidScriptPackagedUiEqual $Report.save_file_sha256 $WriteHash 'read preserved save'
    }
    return $Report
}

function Assert-AvidScriptPackagedUiProperties {
    param([object]$Value, [string[]]$Expected, [string]$Name)
    if ($Value -is [Collections.IDictionary]) { $Actual = @($Value.Keys) }
    elseif ($Value -is [pscustomobject]) { $Actual = @($Value.PSObject.Properties.Name) }
    else { throw "Packaged UI evidence object is invalid: $Name" }
    $Names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($Property in $Expected) { [void]$Names.Add($Property) }
    if ($Actual.Count -ne $Expected.Count) { throw "Packaged UI evidence property count is invalid: $Name" }
    foreach ($Property in $Actual) {
        if ($Property -isnot [string] -or -not $Names.Contains($Property)) {
            throw "Packaged UI evidence property is invalid: $Name.$Property"
        }
    }
}

function Get-AvidScriptPackagedUiTextHash {
    param([string]$Text)
    $Bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

function Assert-AvidScriptPackagedUiWorldBackend {
    param([object]$Backend)
    Assert-AvidScriptPackagedUiProperties $Backend @('measured', 'source', 'backend_id', 'backend_kind', 'execution_mode',
        'artifact_format', 'runtime_version', 'runtime_build_identity', 'runtime_artifact_sha256', 'target_triple') 'backend'
    foreach ($Entry in ([ordered]@{ measured = $true; source = 'CaptureRuntimeDiagnostics.BackendInfo'; backend_kind = 'wasmtime'
                execution_mode = 'aot'; artifact_format = 'wasmtime_serialized'; target_triple = 'x86_64-pc-windows-msvc' }).GetEnumerator()) {
        Assert-AvidScriptPackagedUiEqual $Backend.($Entry.Key) $Entry.Value "backend.$($Entry.Key)"
    }
    foreach ($Field in @('backend_id', 'runtime_version', 'runtime_build_identity')) {
        if ($Backend.$Field -isnot [string] -or [string]::IsNullOrWhiteSpace($Backend.$Field)) {
            throw "Missing packaged World backend identity: $Field"
        }
    }
    Assert-AvidScriptPackagedUiHash $Backend.runtime_artifact_sha256 'backend.runtime_artifact_sha256'
}

function Assert-AvidScriptPackagedUiWorldResources {
    param([object]$Resources, [string]$Name = 'resources')
    $Fields = @('active_subscriptions', 'bound_buttons', 'owned_entries', 'borrowed_entries', 'pending_timers',
        'pending_continuations', 'prepared_continuations', 'prepared_subscriptions')
    Assert-AvidScriptPackagedUiProperties $Resources $Fields $Name
    foreach ($Field in @('active_subscriptions', 'bound_buttons')) {
        Assert-AvidScriptPackagedUiEqual $Resources.$Field 4 "$Name.$Field"
    }
    foreach ($Field in @('owned_entries', 'pending_timers', 'pending_continuations', 'prepared_continuations', 'prepared_subscriptions')) {
        Assert-AvidScriptPackagedUiEqual $Resources.$Field 0 "$Name.$Field"
    }
    Assert-AvidScriptPackagedUiNumber $Resources.borrowed_entries "$Name.borrowed_entries" 7 8 -Integer
}

function Assert-AvidScriptPackagedUiWorldFinalActions {
    param([object]$Actions, [int]$CompletedCycles, [string]$SaveHash)
    if ($Actions -isnot [array] -or $Actions.Count -ne 2) { throw 'Packaged World final action count is invalid.' }
    $Steps = @(@('ready', '0', 'Ready'), @('load', [string]$CompletedCycles, 'Loaded'))
    $PreviousFrame = -1L
    for ($Index = 0; $Index -lt $Steps.Count; ++$Index) {
        $Action = $Actions[$Index]
        $Step = $Steps[$Index]
        Assert-AvidScriptPackagedUiProperties $Action @('action', 'expected_score', 'expected_status', 'observed_score',
            'observed_status', 'save_sha256_before', 'dispatch_frame', 'check_frame', 'passed', 'save_sha256_after') "final_actions[$Index]"
        Assert-AvidScriptPackagedUiEqual $Action.action $Step[0] 'final action order'
        Assert-AvidScriptPackagedUiEqual $Action.passed $true 'final action passed'
        foreach ($Field in @('expected_score', 'observed_score')) { Assert-AvidScriptPackagedUiEqual $Action.$Field $Step[1] "final action $Field" }
        foreach ($Field in @('expected_status', 'observed_status')) { Assert-AvidScriptPackagedUiEqual $Action.$Field $Step[2] "final action $Field" }
        foreach ($Field in @('save_sha256_before', 'save_sha256_after')) { Assert-AvidScriptPackagedUiEqual $Action.$Field $SaveHash "final action $Field" }
        Assert-AvidScriptPackagedUiNumber $Action.dispatch_frame 'final action dispatch_frame' ($PreviousFrame + 1) -Integer
        Assert-AvidScriptPackagedUiNumber $Action.check_frame 'final action check_frame' ($Action.dispatch_frame + 2) -Integer
        $PreviousFrame = [long]$Action.check_frame
    }
}

function Get-AvidScriptPackagedUiWorldSampleCycles {
    param([int]$CompletedCycles)
    $Cycles = @(@(1, 2, 3, 5, 10, 25, 50, 100, 200, 500, 1000, 2000, 5000, 10000) |
        Where-Object { $_ -le $CompletedCycles })
    if ($Cycles.Count -eq 0 -or $Cycles[-1] -ne $CompletedCycles) { $Cycles += $CompletedCycles }
    return @($Cycles)
}

function Resolve-AvidScriptPackagedUiWorldReport {
    [CmdletBinding(DefaultParameterSetName = 'Path')]
    param(
        [Parameter(Mandatory, ParameterSetName = 'Path')][string]$ReportPath,
        [Parameter(Mandatory, ParameterSetName = 'Object')][object]$Report,
        [Parameter(Mandatory)][ValidateSet('Development', 'Shipping')][string]$Configuration,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][string]$UserRoot,
        [Parameter(Mandatory)][string]$PackageId,
        [Parameter(Mandatory)][ValidateRange(2, 1000)][int]$WorldCycles,
        [Parameter(Mandatory)][ValidateRange(0, 21600)][int]$SoakSeconds,
        [Parameter(Mandatory)][int]$NativeBudget,
        [Parameter(Mandatory)][ValidateRange(1, 2147483647)][int]$ProcessId,
        [Parameter(Mandatory)][DateTimeOffset]$LaunchedUtc,
        [Parameter(Mandatory)][DateTimeOffset]$ExitedUtc
    )
    Assert-AvidScriptUiSaveSafePath $UserRoot
    if ($PSCmdlet.ParameterSetName -ceq 'Path') {
        $EvidenceRoot = Join-Path $UserRoot 'Saved/AvidScript/PackagedUiWorld'
        if (-not (Test-AvidScriptBindingPathContained -RootPath $EvidenceRoot -CandidatePath $ReportPath)) {
            throw 'Packaged World report must remain inside its isolated evidence directory.'
        }
        $Report = Read-AvidScriptPackagedUiJson $ReportPath
    }
    Assert-AvidScriptPackagedUiEqual $NativeBudget ($SoakSeconds + [Math]::Max(120, $WorldCycles * 20)) 'native World timeout budget'
    Assert-AvidScriptPackagedUiProperties $Report @('schema_version', 'result', 'succeeded', 'failure_category', 'mode',
        'process_mode', 'configuration', 'requires_cooked_data', 'validation_plugin', 'run_id', 'process_id', 'input_kind',
        'physical_click_verified', 'visual_verified', 'long_run_verified', 'expected_module_id', 'expected_package_id', 'map',
        'user_dir', 'effective_saved_dir', 'save_path', 'initial_save_sha256', 'save_file_sha256', 'save_file_bytes',
        'save_file_exists', 'score_text', 'status_text', 'started_utc', 'finished_utc', 'elapsed_seconds', 'timeout_seconds',
        'runtime_snapshot_phase', 'runtime', 'startup', 'backend', 'resources', 'world_lifecycle') 'report'
    foreach ($Entry in ([ordered]@{ schema_version = 1; result = 'avidscript_ui_save_world_probe_passed'; succeeded = $true
                failure_category = ''; mode = 'world'; process_mode = 'packaged_game'; configuration = $Configuration
                requires_cooked_data = $true; validation_plugin = 'AvidScriptValidation'; run_id = $RunId
                input_kind = 'synthetic_ue_button_onclicked_broadcast'; physical_click_verified = $false; visual_verified = $false
                expected_module_id = 'avidscript.ui_save_demo'; expected_package_id = $PackageId
                map = '/AvidScript/Demos/UiSave/L_UiSave'; initial_save_sha256 = ''; save_file_exists = $true
                status_text = 'Loaded'; timeout_seconds = $NativeBudget; runtime_snapshot_phase = 'final_active' }).GetEnumerator()) {
        Assert-AvidScriptPackagedUiEqual $Report.($Entry.Key) $Entry.Value $Entry.Key
    }
    if ($RunId -cnotmatch '\A[0-9a-f]{32}\z') { throw 'Packaged World RunId must be lowercase 32hex.' }
    Assert-AvidScriptPackagedUiHash $PackageId 'expected PackageId'
    Assert-AvidScriptPackagedUiNumber $Report.process_id 'process_id' 1 ([int]::MaxValue) -Integer
    Assert-AvidScriptPackagedUiEqual $Report.process_id $ProcessId 'actual process id'
    Assert-AvidScriptPackagedUiNumber $Report.elapsed_seconds 'elapsed_seconds' 0 $NativeBudget
    $Times = [Collections.Generic.List[DateTimeOffset]]::new()
    foreach ($Field in @('started_utc', 'finished_utc')) {
        $Time = [DateTimeOffset]::MinValue
        if ($Report.$Field -isnot [string] -or $Report.$Field -cnotmatch '(?:Z|\+00:00)\z' -or
            -not [DateTimeOffset]::TryParse($Report.$Field, [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::None, [ref]$Time)) { throw "Packaged World timestamp is invalid: $Field" }
        $Times.Add($Time)
    }
    if ($Times[0] -lt $LaunchedUtc.AddSeconds(-1) -or $Times[1] -gt $ExitedUtc.AddSeconds(1) -or
        $Times[1] -lt $Times[0] -or [Math]::Abs(($Times[1] - $Times[0]).TotalSeconds - $Report.elapsed_seconds) -gt 2) {
        throw 'Packaged World timestamps do not match this process invocation.'
    }

    Assert-AvidScriptPackagedUiProperties $Report.runtime @('module_id', 'package_id', 'error_message', 'resolved_from_package',
        'runtime_loaded', 'begin_play', 'owner_registered', 'owner_handle_valid', 'events', 'dropped_events') 'runtime'
    foreach ($Entry in ([ordered]@{ module_id = 'avidscript.ui_save_demo'; package_id = $PackageId; error_message = ''
                resolved_from_package = $true; runtime_loaded = $true; begin_play = $true; owner_registered = $true
                owner_handle_valid = $true; events = 1; dropped_events = 0 }).GetEnumerator()) {
        Assert-AvidScriptPackagedUiEqual $Report.runtime.($Entry.Key) $Entry.Value "runtime.$($Entry.Key)"
    }
    Assert-AvidScriptPackagedUiProperties $Report.startup @('active', 'scenario_id', 'error_category', 'error_message') 'startup'
    foreach ($Entry in ([ordered]@{ active = $true; scenario_id = 'ui_save_demo'; error_category = ''; error_message = '' }).GetEnumerator()) {
        Assert-AvidScriptPackagedUiEqual $Report.startup.($Entry.Key) $Entry.Value "startup.$($Entry.Key)"
    }
    Assert-AvidScriptPackagedUiWorldBackend $Report.backend
    Assert-AvidScriptPackagedUiWorldResources $Report.resources
    Assert-AvidScriptPackagedUiEqual $Report.resources.borrowed_entries 8 'resources.borrowed_entries after final load'

    $World = $Report.world_lifecycle
    Assert-AvidScriptPackagedUiProperties $World @('requested_cycles', 'requested_soak_seconds', 'completed_cycles', 'activated_worlds',
        'travel_count', 'cleanup_count', 'elapsed_seconds', 'warmup_cycles', 'world_lifecycle_verified', 'long_run_verified',
        'final_recovered_score', 'final_save_sha256', 'evidence_chain_sha256', 'checks', 'memory_summary', 'observer', 'samples',
        'final_actions', 'failure_cycle') 'world_lifecycle'
    Assert-AvidScriptPackagedUiEqual $World.requested_cycles $WorldCycles 'world_lifecycle.requested_cycles'
    Assert-AvidScriptPackagedUiEqual $World.requested_soak_seconds $SoakSeconds 'world_lifecycle.requested_soak_seconds'
    Assert-AvidScriptPackagedUiNumber $World.completed_cycles 'world_lifecycle.completed_cycles' $WorldCycles 10000 -Integer
    $Completed = [int]$World.completed_cycles
    if ($SoakSeconds -eq 0) { Assert-AvidScriptPackagedUiEqual $Completed $WorldCycles 'zero-soak completed cycles' }
    foreach ($Field in @('travel_count', 'cleanup_count', 'final_recovered_score')) {
        Assert-AvidScriptPackagedUiEqual $World.$Field $Completed "world_lifecycle.$Field"
    }
    Assert-AvidScriptPackagedUiEqual $World.activated_worlds ($Completed + 1) 'world_lifecycle.activated_worlds'
    Assert-AvidScriptPackagedUiEqual $World.warmup_cycles 3 'world_lifecycle.warmup_cycles'
    Assert-AvidScriptPackagedUiEqual $World.world_lifecycle_verified $true 'world_lifecycle.world_lifecycle_verified'
    Assert-AvidScriptPackagedUiNumber $World.elapsed_seconds 'world_lifecycle.elapsed_seconds' ([Math]::Max($SoakSeconds, $Completed * 3)) $NativeBudget
    if ([Math]::Abs([double]$World.elapsed_seconds - [double]$Report.elapsed_seconds) -gt 0.001) {
        throw 'Packaged World root and lifecycle elapsed durations disagree.'
    }
    $LongRun = $Report.elapsed_seconds -ge 3600 -and $Completed -ge 10
    Assert-AvidScriptPackagedUiEqual $Report.long_run_verified $LongRun 'long_run_verified'
    Assert-AvidScriptPackagedUiEqual $World.long_run_verified $LongRun 'world_lifecycle.long_run_verified'
    Assert-AvidScriptPackagedUiEqual $Report.score_text ([string]$Completed) 'score_text'
    Assert-AvidScriptPackagedUiHash $Report.save_file_sha256 'save_file_sha256'
    Assert-AvidScriptPackagedUiEqual $World.final_save_sha256 $Report.save_file_sha256 'world_lifecycle.final_save_sha256'
    Assert-AvidScriptPackagedUiHash $World.evidence_chain_sha256 'world_lifecycle.evidence_chain_sha256'
    if ($null -ne $World.failure_cycle) { throw 'Successful packaged World report must not retain a failure cycle.' }

    Assert-AvidScriptPackagedUiProperties $World.checks @('cycles_passed', 'actions_passed', 'cleanup_cycles_passed',
        'gc_cycles_passed', 'resource_snapshots_passed') 'world_lifecycle.checks'
    foreach ($Field in @('cycles_passed', 'cleanup_cycles_passed', 'gc_cycles_passed')) {
        Assert-AvidScriptPackagedUiEqual $World.checks.$Field $Completed "world_lifecycle.checks.$Field"
    }
    Assert-AvidScriptPackagedUiEqual $World.checks.actions_passed (5 * $Completed + 1) 'world_lifecycle.checks.actions_passed'
    Assert-AvidScriptPackagedUiEqual $World.checks.resource_snapshots_passed ($Completed + 1) 'world_lifecycle.checks.resource_snapshots_passed'

    $Observer = $World.observer
    Assert-AvidScriptPackagedUiProperties $Observer @('retention_policy', 'sample_count', 'sample_capacity',
        'retained_completed_cycles', 'retained_actions', 'json_estimated_bytes') 'world_lifecycle.observer'
    Assert-AvidScriptPackagedUiEqual $Observer.retention_policy 'fixed_milestones_plus_final' 'observer.retention_policy'
    Assert-AvidScriptPackagedUiEqual $Observer.sample_capacity 15 'observer.sample_capacity'
    Assert-AvidScriptPackagedUiEqual $Observer.retained_completed_cycles 0 'observer.retained_completed_cycles'
    Assert-AvidScriptPackagedUiEqual $Observer.retained_actions 2 'observer.retained_actions'
    Assert-AvidScriptPackagedUiNumber $Observer.json_estimated_bytes 'observer.json_estimated_bytes' 1 4MB -Integer

    if ($World.samples -isnot [array]) { throw 'Packaged World samples must be an array.' }
    $ExpectedCycles = @(Get-AvidScriptPackagedUiWorldSampleCycles $Completed)
    Assert-AvidScriptPackagedUiEqual $Observer.sample_count $ExpectedCycles.Count 'observer.sample_count'
    if ($World.samples.Count -ne $ExpectedCycles.Count -or $World.samples.Count -gt 15) {
        throw 'Packaged World samples are not the fixed milestone set.'
    }
    $SampleFields = @('cycle', 'final_cycle', 'save_sha256', 'save_file_bytes', 'cycle_action_count', 'physical_bytes',
        'virtual_bytes', 'uobject_count', 'active_sessions', 'active_subscriptions', 'bound_buttons', 'owned_entries',
        'borrowed_entries', 'pending_timers', 'pending_continuations', 'prepared_continuations', 'prepared_subscriptions',
        'backend_created', 'backend_destroyed', 'backend_live', 'artifact_cache_entries', 'artifact_cache_capacity',
        'artifact_cache_allocated_bytes', 'attestation_entries', 'attestation_capacity', 'attestation_allocated_bytes',
        'evidence_chain_sha256', 'observer_json_estimated_bytes')
    $PreviousSample = $null
    $Baseline = $null
    $MaximumSamplePhysical = 0L
    $MaximumSampleVirtual = 0L
    for ($Index = 0; $Index -lt $World.samples.Count; ++$Index) {
        $Sample = $World.samples[$Index]
        $Cycle = $ExpectedCycles[$Index]
        Assert-AvidScriptPackagedUiProperties $Sample $SampleFields "world_lifecycle.samples[$Index]"
        Assert-AvidScriptPackagedUiEqual $Sample.cycle $Cycle 'sample.cycle'
        Assert-AvidScriptPackagedUiEqual $Sample.final_cycle ($Cycle -eq $Completed) 'sample.final_cycle'
        Assert-AvidScriptPackagedUiHash $Sample.save_sha256 'sample.save_sha256'
        Assert-AvidScriptPackagedUiHash $Sample.evidence_chain_sha256 'sample.evidence_chain_sha256'
        Assert-AvidScriptPackagedUiNumber $Sample.save_file_bytes 'sample.save_file_bytes' 1 16MB -Integer
        Assert-AvidScriptPackagedUiEqual $Sample.cycle_action_count $(if ($Cycle -eq 1) { 4 } else { 5 }) 'sample.cycle_action_count'
        foreach ($Field in @('physical_bytes', 'virtual_bytes', 'uobject_count')) {
            Assert-AvidScriptPackagedUiNumber $Sample.$Field "sample.$Field" 0 9007199254740991 -Integer
        }
        Assert-AvidScriptPackagedUiEqual $Sample.active_sessions 1 'sample.active_sessions'
        $FlatResources = [pscustomobject][ordered]@{}
        foreach ($Field in @('active_subscriptions', 'bound_buttons', 'owned_entries', 'borrowed_entries', 'pending_timers',
                'pending_continuations', 'prepared_continuations', 'prepared_subscriptions')) {
            $FlatResources | Add-Member -NotePropertyName $Field -NotePropertyValue $Sample.$Field
        }
        Assert-AvidScriptPackagedUiWorldResources $FlatResources 'sample.resources'
        Assert-AvidScriptPackagedUiEqual $Sample.borrowed_entries 7 'sample.borrowed_entries before final load'
        foreach ($Field in @('backend_created', 'backend_destroyed', 'backend_live', 'artifact_cache_entries',
                'artifact_cache_capacity', 'artifact_cache_allocated_bytes', 'attestation_entries', 'attestation_capacity',
                'attestation_allocated_bytes')) {
            Assert-AvidScriptPackagedUiNumber $Sample.$Field "sample.$Field" 0 9007199254740991 -Integer
        }
        Assert-AvidScriptPackagedUiNumber $Sample.backend_live 'sample.backend_live' 1 9007199254740991 -Integer
        Assert-AvidScriptPackagedUiEqual ($Sample.backend_created - $Sample.backend_destroyed) $Sample.backend_live 'sample backend balance'
        Assert-AvidScriptPackagedUiNumber $Sample.artifact_cache_entries 'sample.artifact_cache_entries' 0 $Sample.artifact_cache_capacity -Integer
        Assert-AvidScriptPackagedUiNumber $Sample.attestation_entries 'sample.attestation_entries' 0 $Sample.attestation_capacity -Integer
        Assert-AvidScriptPackagedUiNumber $Sample.observer_json_estimated_bytes 'sample.observer_json_estimated_bytes' 1 4MB -Integer
        if ($null -ne $PreviousSample) {
            foreach ($Field in @('backend_created', 'backend_destroyed')) {
                Assert-AvidScriptPackagedUiNumber $Sample.$Field "sample.$Field monotonic" $PreviousSample.$Field 9007199254740991 -Integer
            }
        }
        if ($Cycle -eq 3) { $Baseline = $Sample }
        elseif ($Cycle -gt 3 -and $null -ne $Baseline) {
            Assert-AvidScriptPackagedUiEqual $Sample.backend_live $Baseline.backend_live 'sample backend_live after warmup'
            foreach ($Field in @('artifact_cache_entries', 'artifact_cache_allocated_bytes', 'attestation_entries', 'attestation_allocated_bytes')) {
                Assert-AvidScriptPackagedUiNumber $Sample.$Field "sample.$Field after warmup" 0 $Baseline.$Field -Integer
            }
        }
        $PreviousChain = if ($null -eq $PreviousSample) {
            Get-AvidScriptPackagedUiTextHash "AvidScriptPackagedWorld/v1|$RunId|$PackageId"
        } else { $PreviousSample.evidence_chain_sha256 }
        if ($null -eq $PreviousSample -or $Cycle -eq ($PreviousSample.cycle + 1)) {
            $Canonical = @($PreviousChain, $Cycle, $Sample.save_sha256, $Sample.save_file_bytes, $Sample.cycle_action_count,
                $Sample.physical_bytes, $Sample.virtual_bytes, $Sample.uobject_count, $Sample.backend_created,
                $Sample.backend_destroyed, $Sample.backend_live, $Sample.artifact_cache_entries,
                $Sample.artifact_cache_allocated_bytes, $Sample.attestation_entries, $Sample.attestation_allocated_bytes) -join '|'
            Assert-AvidScriptPackagedUiEqual $Sample.evidence_chain_sha256 (Get-AvidScriptPackagedUiTextHash $Canonical) 'sample evidence chain'
        }
        $MaximumSamplePhysical = [Math]::Max($MaximumSamplePhysical, [long]$Sample.physical_bytes)
        $MaximumSampleVirtual = [Math]::Max($MaximumSampleVirtual, [long]$Sample.virtual_bytes)
        $PreviousSample = $Sample
    }
    $FinalSample = $World.samples[-1]
    Assert-AvidScriptPackagedUiEqual $FinalSample.save_sha256 $Report.save_file_sha256 'final sample save hash'
    Assert-AvidScriptPackagedUiEqual $FinalSample.save_file_bytes $Report.save_file_bytes 'final sample save bytes'
    Assert-AvidScriptPackagedUiEqual $FinalSample.evidence_chain_sha256 $World.evidence_chain_sha256 'final sample evidence chain'
    foreach ($Field in @('active_subscriptions', 'bound_buttons', 'owned_entries', 'pending_timers',
            'pending_continuations', 'prepared_continuations', 'prepared_subscriptions')) {
        Assert-AvidScriptPackagedUiEqual $Report.resources.$Field $FinalSample.$Field "final sample resources.$Field"
    }

    $Memory = $World.memory_summary
    Assert-AvidScriptPackagedUiProperties $Memory @('baseline_available', 'baseline_cycle', 'baseline_physical_bytes',
        'baseline_virtual_bytes', 'final_physical_bytes', 'final_virtual_bytes', 'peak_physical_bytes', 'peak_virtual_bytes') 'world_lifecycle.memory_summary'
    $HasBaseline = $Completed -ge 3
    Assert-AvidScriptPackagedUiEqual $Memory.baseline_available $HasBaseline 'memory_summary.baseline_available'
    Assert-AvidScriptPackagedUiEqual $Memory.baseline_cycle $(if ($HasBaseline) { 3 } else { 0 }) 'memory_summary.baseline_cycle'
    $CycleThree = if ($HasBaseline) { @($World.samples | Where-Object { $_.cycle -eq 3 })[0] } else { $null }
    foreach ($Kind in @('physical', 'virtual')) {
        $BaselineValue = if ($HasBaseline) { $CycleThree."${Kind}_bytes" } else { 0 }
        Assert-AvidScriptPackagedUiEqual $Memory."baseline_${Kind}_bytes" $BaselineValue "memory_summary.baseline_${Kind}_bytes"
        Assert-AvidScriptPackagedUiEqual $Memory."final_${Kind}_bytes" $FinalSample."${Kind}_bytes" "memory_summary.final_${Kind}_bytes"
        $SampleMaximum = if ($Kind -ceq 'physical') { $MaximumSamplePhysical } else { $MaximumSampleVirtual }
        Assert-AvidScriptPackagedUiNumber $Memory."peak_${Kind}_bytes" "memory_summary.peak_${Kind}_bytes" $SampleMaximum 9007199254740991 -Integer
    }
    Assert-AvidScriptPackagedUiWorldFinalActions $World.final_actions $Completed $Report.save_file_sha256

    $SavedRoot = Join-Path $UserRoot 'Saved'
    $SavePath = Join-Path $SavedRoot 'SaveGames/AvidScript_UiSaveDemo_v1.sav'
    foreach ($Entry in ([ordered]@{ user_dir = $UserRoot; effective_saved_dir = $SavedRoot; save_path = $SavePath }).GetEnumerator()) {
        if (-not (Test-AvidScriptUiSaveSamePath $Report.($Entry.Key) $Entry.Value)) { throw "Packaged World isolated path mismatch: $($Entry.Key)" }
    }
    Assert-AvidScriptUiSaveSaveDirectory $SavePath
    Assert-AvidScriptPackagedUiNumber $Report.save_file_bytes 'save_file_bytes' 1 16MB -Integer
    if (-not (Test-Path -LiteralPath $SavePath -PathType Leaf) -or
        (Get-Item -LiteralPath $SavePath).Length -ne $Report.save_file_bytes -or
        (Get-AvidScriptBindingSha256Hex $SavePath) -cne $Report.save_file_sha256) {
        throw 'Packaged World final save bytes/hash differ from the actual file.'
    }
    return $Report
}
