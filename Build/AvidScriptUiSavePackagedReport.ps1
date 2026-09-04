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
