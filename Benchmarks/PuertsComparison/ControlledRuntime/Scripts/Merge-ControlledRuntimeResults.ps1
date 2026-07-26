[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$ResultPaths,

    [Parameter(Mandatory = $true)]
    [string[]]$RequestPaths,

    [Parameter(Mandatory = $true)]
    [string]$CalibrationResultPath,

    [Parameter(Mandatory = $true)]
    [string]$CalibrationRequestPath,

    [Parameter(Mandatory = $true)]
    [string]$ProfilePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ControlledRoot = Split-Path -Parent $ScriptRoot
$PuertsComparisonRoot = Split-Path -Parent $ControlledRoot
. (Join-Path $PuertsComparisonRoot 'Scripts/PuertsBenchmarkSidecar.Common.ps1')
$ValidatorPath = Join-Path $ScriptRoot 'Test-ControlledRuntimeResult.ps1'
$AggregateSchemaPath = Join-Path $ControlledRoot (
    'Schema/ControlledRuntimeAggregate.schema.json')
$ResolvedProfilePath = [System.IO.Path]::GetFullPath($ProfilePath)
$Profile = Get-Content -LiteralPath $ResolvedProfilePath -Raw |
    ConvertFrom-Json
$ProfileSha256 = Get-SidecarFileSha256 -Path $ResolvedProfilePath
$TimedSamples = [int]$Profile.timed_samples
$ExpectedProcessRuns = [int]$Profile.process_runs

if ($ResultPaths.Count -ne $ExpectedProcessRuns -or
    $RequestPaths.Count -ne $ExpectedProcessRuns) {
    throw 'ASP54M4201 timed result/request process counts differ from profile'
}

& $ValidatorPath `
    -ResultPath $CalibrationResultPath `
    -RequestPath $CalibrationRequestPath `
    -ProfilePath $ResolvedProfilePath | Out-Null
$Calibration = Get-Content -LiteralPath $CalibrationResultPath -Raw |
    ConvertFrom-Json
$CalibrationSha256 = Get-SidecarFileSha256 -Path $CalibrationResultPath

$Results = @()
$Requests = @()
for ($Index = 0; $Index -lt $ExpectedProcessRuns; ++$Index) {
    & $ValidatorPath `
        -ResultPath $ResultPaths[$Index] `
        -RequestPath $RequestPaths[$Index] `
        -ProfilePath $ResolvedProfilePath `
        -CalibrationResultPath $CalibrationResultPath | Out-Null
    $Results += Get-Content -LiteralPath $ResultPaths[$Index] -Raw |
        ConvertFrom-Json
    $Requests += Get-Content -LiteralPath $RequestPaths[$Index] -Raw |
        ConvertFrom-Json
}

$Pids = @($Results | ForEach-Object { [int]$_.pid })
$AllPids = @([int]$Calibration.pid) + $Pids
if (@($AllPids | Select-Object -Unique).Count -ne
    $ExpectedProcessRuns + 1) {
    throw 'ASP54M4202 calibration/timed process PIDs are not fresh and unique'
}
$ProcessRuns = @(
    $Results |
        ForEach-Object { [int]$_.process_run } |
        Sort-Object
)
for ($Index = 0; $Index -lt $ExpectedProcessRuns; ++$Index) {
    if ($ProcessRuns[$Index] -ne $Index) {
        throw 'ASP54M4203 timed process_run sequence is incomplete'
    }
}

$ExpectedAttemptId = [string]$Calibration.attempt_id
$ExpectedCandidateIdentity = [string]::Join('|', @(
    [string]$Calibration.candidate_commit,
    [string]$Calibration.candidate_tree_sha,
    [string]$Calibration.candidate_clean
))
$ExpectedEngineIdentity = [string]::Join('|', @(
    [string]$Calibration.engine_version,
    [string]$Calibration.engine_build_id,
    [string]$Calibration.engine_executable_sha256
))
$ExpectedLaneIdentityJson = ConvertTo-SidecarCanonicalJson `
    -Value @($Calibration.lane_identities)
foreach ($Result in $Results) {
    $CandidateIdentity = [string]::Join('|', @(
        [string]$Result.candidate_commit,
        [string]$Result.candidate_tree_sha,
        [string]$Result.candidate_clean
    ))
    $EngineIdentity = [string]::Join('|', @(
        [string]$Result.engine_version,
        [string]$Result.engine_build_id,
        [string]$Result.engine_executable_sha256
    ))
    $LaneIdentityJson = ConvertTo-SidecarCanonicalJson `
        -Value @($Result.lane_identities)
    if ([string]$Result.attempt_id -cne $ExpectedAttemptId -or
        [string]$Result.profile_sha256 -cne $ProfileSha256 -or
        [string]$Result.calibration_sha256 -cne $CalibrationSha256 -or
        $CandidateIdentity -cne $ExpectedCandidateIdentity -or
        $EngineIdentity -cne $ExpectedEngineIdentity -or
        $LaneIdentityJson -cne $ExpectedLaneIdentityJson -or
        [bool]$Result.fallback_used) {
        throw 'ASP54M4204 process provenance, identity, or no-fallback mismatch'
    }
}

function Get-Percentile {
    param(
        [double[]]$Values,
        [double]$Percentile
    )
    if ($Values.Count -lt 1) {
        throw 'ASP54M4205 percentile requires observations'
    }
    $Sorted = @($Values | Sort-Object)
    $Rank = [Math]::Max(
        0,
        [Math]::Ceiling($Percentile * $Sorted.Count) - 1)
    return [double]$Sorted[[int]$Rank]
}

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -lt 1) {
        throw 'ASP54M4206 median requires observations'
    }
    $Sorted = @($Values | Sort-Object)
    $Middle = [int][Math]::Floor($Sorted.Count / 2)
    if (($Sorted.Count % 2) -eq 1) {
        return [double]$Sorted[$Middle]
    }
    return ([double]$Sorted[$Middle - 1] +
        [double]$Sorted[$Middle]) / 2.0
}

$ProcessMetrics = @()
$MetricsByProcessLane = @{}
$AllSamples = @($Results | ForEach-Object { @($_.samples) })
foreach ($Result in $Results) {
    $ProcessRun = [int]$Result.process_run
    foreach ($LaneId in @($Profile.lanes)) {
        [double[]]$Values = @(
            $Result.samples |
                Where-Object { [string]$_.lane_id -ceq [string]$LaneId } |
                ForEach-Object { [double]$_.ns_per_iteration }
        )
        if ($Values.Count -ne $TimedSamples) {
            throw "ASP54M4207 unequal process/lane sample count: process=$ProcessRun lane=$LaneId"
        }
        $P50 = Get-Median -Values $Values
        $P95 = Get-Percentile -Values $Values -Percentile 0.95
        [double[]]$AbsoluteDeviations = @(
            $Values | ForEach-Object { [Math]::Abs($_ - $P50) }
        )
        $Metric = [ordered]@{
            process_run = $ProcessRun
            lane_id = [string]$LaneId
            sample_count = $Values.Count
            p50_ns_per_iteration = $P50
            p95_ns_per_iteration = $P95
            mad_ns_per_iteration = Get-Median -Values $AbsoluteDeviations
        }
        $ProcessMetrics += $Metric
        $MetricsByProcessLane["$ProcessRun/$LaneId"] = $Metric
    }
}

$CrossProcessMetrics = @()
foreach ($LaneId in @($Profile.lanes)) {
    $LaneMetrics = @(
        $ProcessMetrics |
            Where-Object { [string]$_.lane_id -ceq [string]$LaneId }
    )
    if ($LaneMetrics.Count -ne $ExpectedProcessRuns) {
        throw "ASP54M4208 process metric count mismatch: $LaneId"
    }
    [double[]]$P50Values = @(
        $LaneMetrics | ForEach-Object { [double]$_.p50_ns_per_iteration }
    )
    [double[]]$P95Values = @(
        $LaneMetrics | ForEach-Object { [double]$_.p95_ns_per_iteration }
    )
    [double[]]$MadValues = @(
        $LaneMetrics | ForEach-Object { [double]$_.mad_ns_per_iteration }
    )
    $CrossProcessMetrics += [ordered]@{
        lane_id = [string]$LaneId
        process_count = $LaneMetrics.Count
        process_p50_median_ns_per_iteration = Get-Median -Values $P50Values
        process_p50_p95_ns_per_iteration = Get-Percentile `
            -Values $P50Values `
            -Percentile 0.95
        process_p95_median_ns_per_iteration = Get-Median -Values $P95Values
        process_p95_p95_ns_per_iteration = Get-Percentile `
            -Values $P95Values `
            -Percentile 0.95
        process_mad_median_ns_per_iteration = Get-Median -Values $MadValues
    }
}

$PerProcessRatios = @()
[double[]]$P50Ratios = @()
[double[]]$P95Ratios = @()
for ($ProcessRun = 0;
    $ProcessRun -lt $ExpectedProcessRuns;
    ++$ProcessRun) {
    $V8 = $MetricsByProcessLane["$ProcessRun/puerts_v8_wasm_jit"]
    $Wasmtime = $MetricsByProcessLane[
        "$ProcessRun/avidscript_wasmtime_cranelift_jit"]
    if ($null -eq $V8 -or $null -eq $Wasmtime -or
        [double]$V8.p50_ns_per_iteration -le 0 -or
        [double]$V8.p95_ns_per_iteration -le 0) {
        throw "ASP54M4209 paired process metrics invalid: $ProcessRun"
    }
    $P50Ratio = [double]$Wasmtime.p50_ns_per_iteration /
        [double]$V8.p50_ns_per_iteration
    $P95Ratio = [double]$Wasmtime.p95_ns_per_iteration /
        [double]$V8.p95_ns_per_iteration
    $P50Ratios += $P50Ratio
    $P95Ratios += $P95Ratio
    $PerProcessRatios += [ordered]@{
        process_run = $ProcessRun
        wasmtime_over_v8_p50 = $P50Ratio
        wasmtime_over_v8_p95 = $P95Ratio
    }
}
$PairedP50AcrossProcesses = Get-Median -Values $P50Ratios
$PairedP95AcrossProcesses = Get-Percentile `
    -Values $P95Ratios `
    -Percentile 0.95
$MaximumSlowdown = [double]$Profile.pc_stop_gate.maximum_slowdown_ratio
$Gate = if ($PairedP50AcrossProcesses -gt $MaximumSlowdown -or
    $PairedP95AcrossProcesses -gt $MaximumSlowdown) {
    'wasmtime_pc_default_rejected'
}
else {
    'wasmtime_pc_default_gate_passed'
}

$Aggregate = [ordered]@{
    schema_version = 1
    benchmark_kind = 'identical_wasm_kernel'
    attempt_id = $ExpectedAttemptId
    profile_sha256 = $ProfileSha256
    calibration_sha256 = $CalibrationSha256
    candidate = [ordered]@{
        commit = [string]$Calibration.candidate_commit
        tree_sha = [string]$Calibration.candidate_tree_sha
        clean = [bool]$Calibration.candidate_clean
    }
    engine = [ordered]@{
        version = [string]$Calibration.engine_version
        build_id = [string]$Calibration.engine_build_id
        executable_sha256 =
            [string]$Calibration.engine_executable_sha256
    }
    kernel_wasm_sha256 = [string]$Profile.kernel_wasm_sha256
    lane_schedule_id = [string]$Profile.lane_schedule_id
    lane_identities = @($Calibration.lane_identities)
    lane_identities_sha256 = Get-SidecarCanonicalJsonSha256 `
        -Value @($Calibration.lane_identities)
    calibration_pid = [int]$Calibration.pid
    timed_pids = $Pids
    process_runs = $ExpectedProcessRuns
    timed_samples_per_lane_per_process = $TimedSamples
    observation_count = $AllSamples.Count
    correctness_failures = 0
    fallback_used = $false
    result_sha256 = @(
        $ResultPaths | ForEach-Object {
            Get-SidecarFileSha256 -Path $_
        }
    )
    request_sha256 = @(
        $RequestPaths | ForEach-Object {
            Get-SidecarFileSha256 -Path $_
        }
    )
    process_metrics = $ProcessMetrics
    cross_process_metrics = $CrossProcessMetrics
    paired_ratios = [ordered]@{
        per_process = $PerProcessRatios
        wasmtime_over_v8_cross_process_p50 =
            $PairedP50AcrossProcesses
        wasmtime_over_v8_cross_process_p95 =
            $PairedP95AcrossProcesses
    }
    pc_default_gate = $Gate
}
$AggregateJson = $Aggregate | ConvertTo-Json -Depth 32
if (-not ($AggregateJson | Test-Json -SchemaFile $AggregateSchemaPath)) {
    throw 'ASP54M4210 generated aggregate does not match schema v1'
}
[System.IO.File]::WriteAllText(
    [System.IO.Path]::GetFullPath($OutputPath),
    $AggregateJson,
    [System.Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    result = 'controlled_runtime_aggregate_published'
    output_path = [System.IO.Path]::GetFullPath($OutputPath)
    observation_count = $AllSamples.Count
    wasmtime_over_v8_cross_process_p50 = $PairedP50AcrossProcesses
    wasmtime_over_v8_cross_process_p95 = $PairedP95AcrossProcesses
    pc_default_gate = $Gate
}
