[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$ResultPaths,

    [Parameter(Mandatory = $true)]
    [string]$ProfilePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ControlledRoot = Split-Path -Parent $ScriptRoot
$ValidatorPath = Join-Path $ScriptRoot 'Test-ControlledRuntimeResult.ps1'
$AggregateSchemaPath = Join-Path $ControlledRoot 'Schema/ControlledRuntimeAggregate.schema.json'
$Profile = Get-Content -LiteralPath $ProfilePath -Raw | ConvertFrom-Json
$TimedSamples = [int]$Profile.timed_samples
$ExpectedProcessRuns = [int]$Profile.process_runs

if ($ResultPaths.Count -ne $ExpectedProcessRuns) {
    throw "ASP54M4201 expected $ExpectedProcessRuns timed process results, received $($ResultPaths.Count)"
}

$Results = @()
foreach ($Path in $ResultPaths) {
    & $ValidatorPath -ResultPath $Path -ExpectedTimedSamples $TimedSamples | Out-Null
    $Results += Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}
$Pids = @($Results | ForEach-Object { [int]$_.pid })
if (@($Pids | Select-Object -Unique).Count -ne $ExpectedProcessRuns) {
    throw 'ASP54M4202 timed process PIDs are not fresh and unique'
}
$ProcessRuns = @($Results | ForEach-Object { [int]$_.process_run } | Sort-Object)
for ($Index = 0; $Index -lt $ExpectedProcessRuns; ++$Index) {
    if ($ProcessRuns[$Index] -ne $Index) {
        throw 'ASP54M4203 timed process_run sequence is incomplete'
    }
}
$KernelDigests = @($Results | ForEach-Object { [string]$_.kernel_wasm_sha256 } | Select-Object -Unique)
if ($KernelDigests.Count -ne 1 -or
    $KernelDigests[0] -cne [string]$Profile.kernel_wasm_sha256) {
    throw 'ASP54M4204 timed processes did not use the frozen kernel digest'
}

function Get-Percentile {
    param(
        [double[]]$Values,
        [double]$Percentile
    )
    $Sorted = @($Values | Sort-Object)
    $Rank = [Math]::Max(0, [Math]::Ceiling($Percentile * $Sorted.Count) - 1)
    return [double]$Sorted[[int]$Rank]
}

function Get-Median {
    param([double[]]$Values)
    $Sorted = @($Values | Sort-Object)
    $Middle = [int][Math]::Floor($Sorted.Count / 2)
    if (($Sorted.Count % 2) -eq 1) {
        return [double]$Sorted[$Middle]
    }
    return ([double]$Sorted[$Middle - 1] + [double]$Sorted[$Middle]) / 2.0
}

$AllSamples = @($Results | ForEach-Object { @($_.samples) })
$Metrics = @()
$MetricsByLane = @{}
foreach ($LaneId in @($Profile.lanes)) {
    [double[]]$Values = @(
        $AllSamples |
            Where-Object { [string]$_.lane_id -ceq [string]$LaneId } |
            ForEach-Object { [double]$_.ns_per_iteration }
    )
    $ExpectedLaneSamples = $ExpectedProcessRuns * $TimedSamples
    if ($Values.Count -ne $ExpectedLaneSamples) {
        throw "ASP54M4205 aggregate sample count mismatch for lane $LaneId"
    }
    $P50 = Get-Median -Values $Values
    $P95 = Get-Percentile -Values $Values -Percentile 0.95
    [double[]]$AbsoluteDeviations = @($Values | ForEach-Object { [Math]::Abs($_ - $P50) })
    $Metric = [ordered]@{
        lane_id = [string]$LaneId
        sample_count = $Values.Count
        p50_ns_per_iteration = $P50
        p95_ns_per_iteration = $P95
        mad_ns_per_iteration = Get-Median -Values $AbsoluteDeviations
    }
    $Metrics += $Metric
    $MetricsByLane[[string]$LaneId] = $Metric
}

$V8 = $MetricsByLane['puerts_v8_wasm_jit']
$Wasmtime = $MetricsByLane['avidscript_wasmtime_cranelift_jit']
$P50Ratio = [double]$Wasmtime.p50_ns_per_iteration / [double]$V8.p50_ns_per_iteration
$P95Ratio = [double]$Wasmtime.p95_ns_per_iteration / [double]$V8.p95_ns_per_iteration
$MaximumSlowdown = [double]$Profile.pc_stop_gate.maximum_slowdown_ratio
$Gate = if ($P50Ratio -gt $MaximumSlowdown -or $P95Ratio -gt $MaximumSlowdown) {
    'wasmtime_pc_default_rejected'
}
else {
    'wasmtime_pc_default_gate_passed'
}

$Aggregate = [ordered]@{
    schema_version = 1
    benchmark_kind = 'identical_wasm_kernel'
    kernel_wasm_sha256 = $KernelDigests[0]
    process_runs = $ExpectedProcessRuns
    timed_samples_per_lane_per_process = $TimedSamples
    timed_pids = $Pids
    observation_count = $AllSamples.Count
    correctness_failures = 0
    fallback_used = $false
    metrics = $Metrics
    ratios = [ordered]@{
        wasmtime_over_v8_p50 = $P50Ratio
        wasmtime_over_v8_p95 = $P95Ratio
    }
    pc_default_gate = $Gate
}
$AggregateJson = $Aggregate | ConvertTo-Json -Depth 16
if (-not ($AggregateJson | Test-Json -SchemaFile $AggregateSchemaPath)) {
    throw 'ASP54M4206 generated aggregate does not match schema v1'
}
[System.IO.File]::WriteAllText(
    [System.IO.Path]::GetFullPath($OutputPath),
    $AggregateJson,
    [System.Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    result = 'controlled_runtime_aggregate_published'
    output_path = [System.IO.Path]::GetFullPath($OutputPath)
    observation_count = $AllSamples.Count
    wasmtime_over_v8_p50 = $P50Ratio
    wasmtime_over_v8_p95 = $P95Ratio
    pc_default_gate = $Gate
}
