[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProjectFile,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$BuildEvidence,
    [Parameter(Mandatory)][string]$OutputRoot,
    [ValidateSet(1, 5)][int]$ProcessRuns = 1,
    [ValidateRange(30, 1800)][int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'AngelScriptValidation.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'AngelScriptTiming.psm1') -Force
$Prepared = & (Join-Path $PSScriptRoot 'Invoke-AngelScriptValidation.ps1') -ProjectFile $ProjectFile -Executable $Executable -BuildEvidence $BuildEvidence -OutputRoot $OutputRoot -PrepareOnly
$ProjectFile = [IO.Path]::GetFullPath($ProjectFile)
$Executable = $Prepared.executable
$Adapter = Split-Path -Parent $PSScriptRoot
$Comparison = [IO.Path]::GetFullPath((Join-Path $Adapter '../..'))
$Inputs = @($Prepared.inputs)
foreach ($Path in @($PSCommandPath, (Join-Path $PSScriptRoot 'AngelScriptTiming.psm1'))) {
    $Inputs += [pscustomobject]@{ path = $Path; sha256 = Get-As36Sha256 $Path }
}
foreach ($Suite in @('micro', 'gameplay')) {
    $Name = if ($Suite -ceq 'micro') { 'Phase56Micro.formal.json' } else { 'Phase56Gameplay.formal.json' }
    $Path = Join-Path $Comparison "Profiles/$Name"
    $Profile = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $Minimum = if ($Suite -ceq 'micro') { 1000 } else { 1 }
    $Maximum = if ($Suite -ceq 'micro') { 10000000 } else { 1048576 }
    if ($Profile.process_runs -ne 5 -or $Profile.warmup_samples -ne 5 -or $Profile.timed_samples -ne 30 -or
        $Profile.seed -ne 1397313 -or $Profile.calibration.confirmation_samples -ne 3 -or
        $Profile.calibration.minimum_sample_milliseconds -ne 5 -or $Profile.calibration.minimum_iterations -ne $Minimum -or
        $Profile.calibration.maximum_iterations -ne $Maximum -or $Profile.callback_result_mode -cne 'hot_failure_only' -or
        (($Profile.workloads -join ',') -cne ((Get-As38Workloads $Suite | ForEach-Object name) -join ','))) { throw 'Existing profile differs from the timing contract.' }
    $Inputs += [pscustomobject]@{ path = $Path; sha256 = Get-As36Sha256 $Path }
}
$SourceCommit = (& git -C $Adapter rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify adapter Git source.' }
$AdapterStatus = @(& git -C $Adapter status --porcelain -- .)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect adapter source status.' }
$Root = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('timing-' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$null = New-Item -ItemType Directory -Path $Root
$StatePath = Join-Path $Root 'run.json'
$Cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
$State = [ordered]@{ status = 'prepared'; passed = $false; started_utc = [DateTimeOffset]::UtcNow.ToString('o');
    source_commit = $SourceCommit; adapter_source_status = $AdapterStatus; build_id = $Prepared.build_id; inputs = $Inputs;
    process_runs_per_suite = $ProcessRuns; cpu = $Cpu; logical_processors = [Environment]::ProcessorCount;
    scope = 'Native/AngelScript Editor VM timings only; other framework matrix and packaged native remain incomplete';
    performance_leadership_claimed = $false; jobs = @() }
function Save-State { $State | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $StatePath -Encoding utf8 }
function Assert-Inputs {
    foreach ($Entry in $Inputs) { if ((Get-As36Sha256 $Entry.path) -cne $Entry.sha256) { throw "Timing input changed: $($Entry.path)" } }
}
function Invoke-TimingProcess {
    param([string]$Suite, [string]$Mode, [int]$ProcessRun, $Counts)
    Assert-Inputs
    $Id = [guid]::NewGuid().ToString('N')
    $Prefix = "$Suite-$Mode-$ProcessRun-$Id"
    $RequestPath = Join-Path $Root "$Prefix.request.json"
    $ResultPath = Join-Path $Root "$Prefix.result.json"
    $Log = Join-Path $Root "$Prefix.log"
    $Request = [ordered]@{ schema_version = 1; request_id = $Id; mode = $Mode; suite = $Suite; execution_mode = 'editor_vm';
        seed = 1397313; process_runs = $ProcessRuns; process_run = $ProcessRun; warmup_samples = 5; timed_samples = 30 }
    if ($Mode -ceq 'measure') { $Request.iteration_counts = $Counts }
    $Request | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $RequestPath -Encoding utf8
    $Request = Get-Content -LiteralPath $RequestPath -Raw | ConvertFrom-Json
    $Job = [ordered]@{ request = [IO.Path]::GetFileName($RequestPath); result = [IO.Path]::GetFileName($ResultPath);
        log = [IO.Path]::GetFileName($Log); request_sha256 = Get-As36Sha256 $RequestPath; status = 'prepared';
        mode = $Mode; suite = $Suite; process_run = $ProcessRun; started_utc = [DateTimeOffset]::UtcNow.ToString('o') }
    $State.jobs += $Job
    Save-State
    $Arguments = @($ProjectFile, '-run=As38LeadershipTiming', "-As38Request=$RequestPath", "-As38Result=$ResultPath",
        '-unattended', '-nullrhi', '-nosplash', '-nosound', '-nop4', '-NoLiveCoding', "-abslog=$Log")
    if (@($Arguments | Where-Object { $_ -match '["\r\n]' }).Count) { throw 'Unsupported quote or newline in process arguments.' }
    $Process = $null
    try {
        $ArgumentText = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
        $Process = Start-Process -FilePath $Executable -ArgumentList $ArgumentText -WorkingDirectory $Prepared.project_root -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $Root "$Prefix.stdout.log") -RedirectStandardError (Join-Path $Root "$Prefix.stderr.log")
        $Job.process_id = $Process.Id
        $Job.affinity_mask = $Process.ProcessorAffinity.ToInt64().ToString('x')
        $Job.priority_class = $Process.PriorityClass.ToString()
        $Job.status = 'running'
        Save-State
        Write-Host "Timing $Suite/$Mode/$ProcessRun started: PID=$($Process.Id), root=$Root"
        $Watch = [Diagnostics.Stopwatch]::StartNew()
        while (-not $Process.WaitForExit(1000)) {
            if ($Watch.Elapsed.TotalSeconds -gt $TimeoutSeconds) { throw 'Owned timing process timed out.' }
        }
        $Process.Refresh()
        $Job.exit_code = $Process.ExitCode
        if ($Process.ExitCode -ne 0) { throw "Timing process exited $($Process.ExitCode)" }
        $Report = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json
        Assert-As38TimingReport -Report $Report -Request $Request
        Assert-As38TimingLog -Text (Get-Content -LiteralPath $Log -Raw) -ExitCode $Process.ExitCode -Report $Report
        Assert-Inputs
        if ((Get-As36Sha256 $RequestPath) -cne $Job.request_sha256) { throw 'Request changed during execution.' }
        $Job.result_sha256 = Get-As36Sha256 $ResultPath
        $Job.log_sha256 = Get-As36Sha256 $Log
        $Job.sample_count = @($Report.samples).Count
        $Job.status = 'passed'
        return $Report
    } catch {
        $Job.status = 'failed'
        $Job.error = $_.Exception.Message
        if ($null -ne $Process -and -not $Process.HasExited) {
            & taskkill.exe /PID $Process.Id /T /F
            $Process.WaitForExit()
            $Process.Refresh()
            $Job.exit_code = $Process.ExitCode
        }
        throw
    } finally {
        $Job.finished_utc = [DateTimeOffset]::UtcNow.ToString('o')
        Save-State
    }
}
Save-State
try {
    $State.status = 'running'
    Save-State
    foreach ($Suite in @('micro', 'gameplay')) {
        $Calibration = Invoke-TimingProcess -Suite $Suite -Mode 'calibration' -ProcessRun -1
        for ($Index = 0; $Index -lt $ProcessRuns; ++$Index) {
            $null = Invoke-TimingProcess -Suite $Suite -Mode 'measure' -ProcessRun $Index -Counts $Calibration.iteration_counts
        }
    }
    if ($State.jobs.Count -ne 2 * ($ProcessRuns + 1) -or @($State.jobs | Where-Object { $_['status'] -cne 'passed' }).Count) { throw 'Incomplete process set.' }
    $State.status = 'passed'
    $State.passed = $true
} catch {
    $State.status = 'failed'
    $State.error = $_.Exception.Message
} finally {
    $State.finished_utc = [DateTimeOffset]::UtcNow.ToString('o')
    Save-State
}
Write-Output "AngelScript timing $($State.status): $StatePath"
if (-not $State.passed) { Write-Output $State.error; exit 1 }
