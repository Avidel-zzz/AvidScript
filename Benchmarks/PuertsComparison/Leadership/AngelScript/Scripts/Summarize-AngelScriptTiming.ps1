[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunDirectory, [Parameter(Mandatory)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'AngelScriptValidation.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'AngelScriptTiming.psm1') -Force
$RunDirectory = [IO.Path]::GetFullPath($RunDirectory)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
Assert-As36PhysicalPath $OutputPath
if (-not $OutputPath.StartsWith($RunDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-Path -LiteralPath $OutputPath)) { throw 'Summary must be a fresh file inside its run directory.' }
$RunPath = Join-Path $RunDirectory 'run.json'
$Run = Get-Content -LiteralPath $RunPath -Raw | ConvertFrom-Json
if ($Run.passed -isnot [bool] -or -not $Run.passed -or $Run.status -cne 'passed' -or
    $Run.process_runs_per_suite -notin @(1, 5) -or @($Run.jobs).Count -ne 2 * ($Run.process_runs_per_suite + 1)) { throw 'Incomplete timing run.' }
$Expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($Suite in @('micro', 'gameplay')) {
    $null = $Expected.Add("$Suite/calibration/-1")
    for ($Index = 0; $Index -lt $Run.process_runs_per_suite; ++$Index) { $null = $Expected.Add("$Suite/measure/$Index") }
}
$RequestIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$Processes = @()
$CalibrationRows = 0
$WarmupRows = 0
$TimedRows = 0
$Calibrations = @{}
$Reports = @()
foreach ($Job in $Run.jobs) {
    if ($Job.status -cne 'passed' -or $Job.exit_code -ne 0 -or -not $Expected.Remove("$($Job.suite)/$($Job.mode)/$($Job.process_run)")) { throw 'Failed, duplicate or unexpected process.' }
    foreach ($Name in @('request', 'result', 'log')) {
        if ([IO.Path]::GetFileName($Job.$Name) -cne $Job.$Name) { throw 'Evidence path escaped run directory.' }
        $Field = $Name + '_sha256'
        if ((Get-As36Sha256 (Join-Path $RunDirectory $Job.$Name)) -cne $Job.$Field) { throw "Evidence hash changed: $Name" }
    }
    $Request = Get-Content -LiteralPath (Join-Path $RunDirectory $Job.request) -Raw | ConvertFrom-Json
    $Report = Get-Content -LiteralPath (Join-Path $RunDirectory $Job.result) -Raw | ConvertFrom-Json
    if (-not $RequestIds.Add($Request.request_id) -or $Request.suite -cne $Job.suite -or $Request.mode -cne $Job.mode -or
        $Request.process_run -ne $Job.process_run -or $Request.process_runs -ne $Run.process_runs_per_suite) { throw 'Process metadata mismatch.' }
    Assert-As38TimingReport -Report $Report -Request $Request
    Assert-As38TimingLog -Text (Get-Content -LiteralPath (Join-Path $RunDirectory $Job.log) -Raw) -ExitCode $Job.exit_code -Report $Report
    if ($Report.mode -ceq 'calibration') {
        $CalibrationRows += @($Report.samples).Count
        $Calibrations[$Report.suite] = $Report.iteration_counts
    } else {
        $Reports += $Report
        $WarmupRows += @($Report.samples | Where-Object phase -CEQ 'warmup').Count
        $TimedRows += @($Report.samples | Where-Object phase -CEQ 'timed').Count
    }
}
if ($Expected.Count -ne 0) { throw 'Missing processes.' }
function Get-NearestRank {
    param([double[]]$Values, [double]$Percentile)
    if ($Values.Count -eq 0) { throw 'Empty percentile input.' }
    $Sorted = @($Values | Sort-Object)
    $Sorted[[math]::Max(0, [int][math]::Ceiling($Percentile * $Sorted.Count) - 1)]
}
foreach ($Report in $Reports) {
    foreach ($Workload in (Get-As38Workloads $Report.suite)) {
        foreach ($Lane in @('native_cpp', 'angelscript_editor_vm')) {
            if ($Report.iteration_counts.($Workload.name).$Lane -ne $Calibrations[$Report.suite].($Workload.name).$Lane) { throw 'Process used different calibration.' }
            $Rows = @($Report.samples | Where-Object { $_.phase -ceq 'timed' -and $_.workload -ceq $Workload.name -and $_.lane -ceq $Lane })
            if ($Rows.Count -ne 30) { throw 'Incomplete timed sample set.' }
            $Values = @($Rows | ForEach-Object { $_.elapsed_cycles * $Report.seconds_per_cycle * 1000000000.0 / $_.iterations })
            $Processes += [pscustomobject][ordered]@{
                suite = $Report.suite; workload = $Workload.name; lane = $Lane; process_run = $Report.process_run;
                unit = $(if ($Report.suite -ceq 'micro') { 'ns_per_iteration' } else { 'ns_per_frame' });
                iterations = $Rows[0].iterations; samples = 30;
                p50 = Get-NearestRank $Values 0.5; p95 = Get-NearestRank $Values 0.95;
                minimum_sample_ms = ($Rows.milliseconds | Measure-Object -Minimum).Minimum;
                samples_below_calibration_threshold = @($Rows | Where-Object milliseconds -LT 5).Count
            }
        }
    }
}
$Statistics = @()
$Ratios = @()
foreach ($Suite in @('micro', 'gameplay')) {
    foreach ($Workload in (Get-As38Workloads $Suite)) {
        foreach ($Lane in @('native_cpp', 'angelscript_editor_vm')) {
            $Rows = @($Processes | Where-Object { $_.workload -ceq $Workload.name -and $_.lane -ceq $Lane })
            if ($Rows.Count -ne $Run.process_runs_per_suite) { throw 'Incomplete process statistics.' }
            $Statistics += [pscustomobject][ordered]@{ workload = $Workload.name; lane = $Lane; unit = $Rows[0].unit;
                process_count = $Rows.Count; median_process_p50 = Get-NearestRank $Rows.p50 0.5;
                maximum_process_p95 = ($Rows.p95 | Measure-Object -Maximum).Maximum;
                samples_below_calibration_threshold = ($Rows.samples_below_calibration_threshold | Measure-Object -Sum).Sum }
        }
        $Paired = @()
        for ($Index = 0; $Index -lt $Run.process_runs_per_suite; ++$Index) {
            $Native = @($Processes | Where-Object { $_.workload -ceq $Workload.name -and $_.lane -ceq 'native_cpp' -and $_.process_run -eq $Index })[0]
            $Script = @($Processes | Where-Object { $_.workload -ceq $Workload.name -and $_.lane -ceq 'angelscript_editor_vm' -and $_.process_run -eq $Index })[0]
            $Paired += [pscustomobject]@{ process_run = $Index; p50_ratio = $Script.p50 / $Native.p50; p95_ratio = $Script.p95 / $Native.p95 }
        }
        $Ratios += [pscustomobject]@{ workload = $Workload.name; numerator = 'angelscript_editor_vm'; denominator = 'native_cpp';
            median_paired_p50_ratio = Get-NearestRank $Paired.p50_ratio 0.5; maximum_paired_p95_ratio = ($Paired.p95_ratio | Measure-Object -Maximum).Maximum; paired_processes = $Paired }
    }
}
$Summary = [ordered]@{ schema_version = 1; scope = $Run.scope; source_commit = $Run.source_commit; build_id = $Run.build_id;
    run_sha256 = Get-As36Sha256 $RunPath; process_runs_per_suite = $Run.process_runs_per_suite;
    calibration_samples = $CalibrationRows; warmup_samples = $WarmupRows; timed_samples = $TimedRows;
    percentile = 'nearest_rank'; aggregate_p50 = 'median of process P50'; aggregate_p95 = 'maximum process P95; ratios use maximum paired process ratio';
    outliers_removed = $false; working_set_delta_is_allocation_count = $false; complete_leadership_claim = $false;
    processes = $Processes; statistics = $Statistics; native_reference_ratios = $Ratios }
$Summary | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $OutputPath -Encoding utf8
Write-Output "Timing summary verified: calibration=$CalibrationRows warmup=$WarmupRows timed=$TimedRows path=$OutputPath"
