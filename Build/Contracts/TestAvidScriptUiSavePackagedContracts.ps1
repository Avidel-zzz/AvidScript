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

foreach ($Configuration in @('Development', 'Shipping')) {
    foreach ($Mode in @('write', 'read')) {
        Invoke-PackagedUiCase "$Configuration $Mode accepts complete report" {
            $Fixture = New-PackagedUiFixture $Mode $Configuration
            Write-AvidScriptUiSaveNewJson $Fixture.path $Fixture.report
            $Arguments = $Fixture.arguments
            $Result = Resolve-AvidScriptPackagedUiReport @Arguments
            if (-not $Result.succeeded) { throw 'Expected valid report.' }
        }
    }
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
