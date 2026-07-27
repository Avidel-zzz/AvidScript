[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectPath,

    [Parameter(Mandatory = $true)]
    [string]$EditorExecutable,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [Parameter(Mandatory = $true)]
    [string]$AvidScriptCommit,

    [Parameter(Mandatory = $true)]
    [string]$AvidScriptTreeSha,

    [Parameter(Mandatory = $true)]
    [bool]$AvidScriptDirty,

    [string]$ProfilePath = '',

    [switch]$AllowNonFormalProfile,

    [string[]]$AdditionalEditorArguments = @()
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$controlledRoot = Split-Path -Parent $scriptRoot
$comparisonRoot = Split-Path -Parent $controlledRoot
$runnerPluginRoot = Split-Path -Parent (Split-Path -Parent $comparisonRoot)
. (Join-Path $comparisonRoot 'Scripts/PuertsBenchmarkSidecar.Common.ps1')

$canonicalProfilePath = Join-Path $controlledRoot 'Config/PhysicalCostProfile.json'
if ([string]::IsNullOrWhiteSpace($ProfilePath)) {
    $ProfilePath = $canonicalProfilePath
}
$resolvedProfilePath = [IO.Path]::GetFullPath($ProfilePath)
if (-not (Test-Path -LiteralPath $resolvedProfilePath -PathType Leaf)) {
    throw 'ASP54L4701 physical cost profile is missing'
}
if (-not $AllowNonFormalProfile -and
    -not [Linq.Enumerable]::SequenceEqual(
        [byte[]][IO.File]::ReadAllBytes($canonicalProfilePath),
        [byte[]][IO.File]::ReadAllBytes($resolvedProfilePath))) {
    throw 'ASP54L4702 formal cost ladder requires the tracked profile bytes'
}
if ($AvidScriptDirty) {
    throw 'ASP54L4703 physical cost ladder rejects a dirty candidate'
}
$profile = Get-Content -LiteralPath $resolvedProfilePath -Raw |
    ConvertFrom-Json -Depth 100
$expectedStages = @(
    'native_no_op',
    'cached_export',
    'typed_empty_import',
    'generic_empty_import',
    'typed_i32_pair_import'
)
if ([string]$profile.benchmark_kind -cne 'physical_crossing_cost_ladder' -or
    [string]::Join('|', @($profile.stages)) -cne
        [string]::Join('|', $expectedStages)) {
    throw 'ASP54L4704 physical cost stage contract differs'
}

$resolvedProjectPath = [IO.Path]::GetFullPath($ProjectPath)
$resolvedEditorExecutable = [IO.Path]::GetFullPath($EditorExecutable)
$projectJunctions = Assert-SidecarBenchmarkProjectProvenance `
    -ProjectPath $resolvedProjectPath `
    -AvidScriptCommit $AvidScriptCommit `
    -AvidScriptTreeSha $AvidScriptTreeSha
if (-not $AllowNonFormalProfile) {
    Assert-SidecarRunnerCandidate `
        -PluginRoot $runnerPluginRoot `
        -CandidateRoot ([string]$projectJunctions.AvidScript)
}
$profileSha256 = Get-SidecarFileSha256 -Path $resolvedProfilePath
$engineSha256 = Get-SidecarFileSha256 -Path $resolvedEditorExecutable
$kernelPath = Join-Path ([string]$projectJunctions.AvidScript) (
    'Benchmarks/PuertsComparison/ControlledRuntime/Kernel/crossing_cost_kernel.wasm')
$contractPath = Join-Path ([string]$projectJunctions.AvidScript) (
    'Benchmarks/PuertsComparison/ControlledRuntime/Kernel/crossing_cost_kernel.contract.json')
$contract = Get-Content -LiteralPath $contractPath -Raw | ConvertFrom-Json
$kernelSha256 = Get-SidecarFileSha256 -Path $kernelPath
if (-not [bool]$contract.tracked_wasm -or
    $kernelSha256 -cne [string]$contract.wasm_sha256) {
    throw 'ASP54L4705 physical cost kernel identity differs from tracked contract'
}

$resolvedOutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (-not (Test-Path -LiteralPath $resolvedOutputRoot)) {
    New-Item -ItemType Directory -Path $resolvedOutputRoot | Out-Null
}
$attemptId = [Guid]::NewGuid().ToString()
$attemptPath = Join-Path $resolvedOutputRoot (
    'cost-{0}-{1}' -f
        [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ'),
        $attemptId.Replace('-', ''))
$requestRoot = Join-Path $attemptPath 'requests'
$resultRoot = Join-Path $attemptPath 'results'
$logRoot = Join-Path $attemptPath 'logs'
New-Item -ItemType Directory -Path $requestRoot -Force | Out-Null
New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
$requestSchemaPath = Join-Path $controlledRoot 'Schema/PhysicalCostRequest.schema.json'
$resultSchemaPath = Join-Path $controlledRoot 'Schema/PhysicalCostResult.schema.json'

function Write-Request {
    param($Value, [string]$Path)
    $json = $Value | ConvertTo-Json -Depth 100
    if (-not ($json | Test-Json -SchemaFile $requestSchemaPath)) {
        throw 'ASP54L4706 generated physical cost request does not match schema'
    }
    [IO.File]::WriteAllText(
        $Path,
        $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Invoke-CostProcess {
    param([string]$RequestPath, [string]$ResultPath, [int]$ProcessRun)
    $arguments = @(
        $resolvedProjectPath,
        '-run=AvidScriptPerfCost',
        '-Multiprocess',
        '-NoCompile',
        '-unattended',
        '-NoP4',
        '-NullRHI',
        '-NoSplash',
        '-NoSound',
        '-EnablePlugins=AvidScriptPerfHarness',
        "-AvidScriptPerfCostRequest=$RequestPath",
        "-AvidScriptPerfCostResult=$ResultPath"
    ) + @($AdditionalEditorArguments)
    $stdoutPath = Join-Path $logRoot "timed-$ProcessRun.stdout.log"
    $stderrPath = Join-Path $logRoot "timed-$ProcessRun.stderr.log"
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $resolvedEditorExecutable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $arguments) {
        [void]$startInfo.ArgumentList.Add([string]$argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'ASP54L4707 physical cost Editor process could not start'
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    [IO.File]::WriteAllText(
        $stdoutPath,
        $stdoutTask.GetAwaiter().GetResult(),
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText(
        $stderrPath,
        $stderrTask.GetAwaiter().GetResult(),
        [Text.UTF8Encoding]::new($false))
    $exitCode = $process.ExitCode
    $process.Dispose()
    if ($exitCode -ne 0) {
        throw "ASP54L4708 physical cost Editor process failed: run=$ProcessRun exit=$exitCode"
    }
}

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -lt 1) {
        throw 'ASP54L4709 percentile requires observations'
    }
    $rank = [Math]::Max(0, [Math]::Ceiling($Percentile * $sorted.Count) - 1)
    return [double]$sorted[[int]$rank]
}

function Get-Median {
    param([double[]]$Values)
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -lt 1) {
        throw 'ASP54L4710 median requires observations'
    }
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) {
        return [double]$sorted[$middle]
    }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

$results = @()
$resultPaths = @()
for ($processRun = 0; $processRun -lt [int]$profile.process_runs; ++$processRun) {
    $request = [ordered]@{
        schema_version = 1
        benchmark_kind = 'physical_crossing_cost_ladder'
        attempt_id = $attemptId
        profile_sha256 = $profileSha256
        candidate_commit = $AvidScriptCommit
        candidate_tree_sha = $AvidScriptTreeSha
        candidate_clean = $true
        engine_executable_sha256 = $engineSha256
        process_run = $processRun
        warmup_samples = [int]$profile.warmup_samples
        timed_samples = [int]$profile.timed_samples
        iterations = [int]$profile.iterations
        seed = [int]$profile.seed
        kernel_wasm_path = $kernelPath
        kernel_wasm_sha256 = $kernelSha256
    }
    $requestPath = Join-Path $requestRoot "timed-$processRun.request.json"
    $resultPath = Join-Path $resultRoot "timed-$processRun.result.json"
    Write-Request -Value $request -Path $requestPath
    Invoke-CostProcess `
        -RequestPath $requestPath `
        -ResultPath $resultPath `
        -ProcessRun $processRun
    if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        throw "ASP54L4711 physical cost result is missing: $processRun"
    }
    $resultJson = [IO.File]::ReadAllText($resultPath)
    if (-not ($resultJson | Test-Json -SchemaFile $resultSchemaPath)) {
        throw "ASP54L4712 physical cost result does not match schema: $processRun"
    }
    $result = $resultJson | ConvertFrom-Json -Depth 100
    $requestSha256 = Get-SidecarFileSha256 -Path $requestPath
    if ([string]$result.request_sha256 -cne $requestSha256 -or
        [string]$result.profile_sha256 -cne $profileSha256 -or
        [string]$result.candidate_commit -cne $AvidScriptCommit -or
        [string]$result.candidate_tree_sha -cne $AvidScriptTreeSha -or
        [string]$result.engine_executable_sha256 -cne $engineSha256 -or
        [int]$result.process_run -ne $processRun -or
        [int]$result.correctness_failures -ne 0 -or
        [bool]$result.fallback_used) {
        throw "ASP54L4713 physical cost result provenance differs: $processRun"
    }
    foreach ($stage in $expectedStages) {
        $stageSamples = @($result.samples | Where-Object {
            [string]$_.stage -ceq $stage
        })
        if ($stageSamples.Count -ne [int]$profile.timed_samples -or
            @($stageSamples.sample_index | Select-Object -Unique).Count -ne
                [int]$profile.timed_samples) {
            throw "ASP54L4714 physical cost sample matrix differs: $processRun/$stage"
        }
        $expectedCrossings = $stage -in @(
            'typed_empty_import',
            'generic_empty_import',
            'typed_i32_pair_import') ? [int]$profile.iterations : 0
        if (@($stageSamples | Where-Object {
            [int]$_.host_import_count -ne $expectedCrossings -or
            -not [bool]$_.correct
        }).Count -gt 0) {
            throw "ASP54L4715 physical cost path count differs: $processRun/$stage"
        }
    }
    $results += $result
    $resultPaths += $resultPath
}
if (@($results.pid | Select-Object -Unique).Count -ne [int]$profile.process_runs) {
    throw 'ASP54L4716 physical cost timed processes are not fresh and unique'
}

$processMetrics = @()
foreach ($result in $results) {
    foreach ($stage in $expectedStages) {
        [double[]]$values = @($result.samples | Where-Object {
            [string]$_.stage -ceq $stage
        } | ForEach-Object { [double]$_.ns_per_iteration })
        [double]$p50 = Get-Median -Values $values
        [double]$p95 = Get-Percentile -Values $values -Percentile 0.95
        [double[]]$deviations = @($values | ForEach-Object {
            [Math]::Abs($_ - $p50)
        })
        $processMetrics += [ordered]@{
            process_run = [int]$result.process_run
            stage = $stage
            sample_count = $values.Count
            p50_ns_per_iteration = $p50
            p95_ns_per_iteration = $p95
            mad_ns_per_iteration = Get-Median -Values $deviations
        }
    }
}

$crossProcessMetrics = @()
foreach ($stage in $expectedStages) {
    $metrics = @($processMetrics | Where-Object { [string]$_.stage -ceq $stage })
    [double[]]$p50Values = @($metrics | ForEach-Object { [double]$_.p50_ns_per_iteration })
    [double[]]$p95Values = @($metrics | ForEach-Object { [double]$_.p95_ns_per_iteration })
    [double[]]$madValues = @($metrics | ForEach-Object { [double]$_.mad_ns_per_iteration })
    $crossProcessMetrics += [ordered]@{
        stage = $stage
        process_count = $metrics.Count
        process_p50_median_ns_per_iteration = Get-Median -Values $p50Values
        process_p50_p95_ns_per_iteration = Get-Percentile -Values $p50Values -Percentile 0.95
        process_p95_median_ns_per_iteration = Get-Median -Values $p95Values
        process_p95_p95_ns_per_iteration = Get-Percentile -Values $p95Values -Percentile 0.95
        process_mad_median_ns_per_iteration = Get-Median -Values $madValues
    }
}
function Find-StageMetric {
    param([string]$Stage)
    return @($crossProcessMetrics | Where-Object { [string]$_.stage -ceq $Stage })[0]
}
$cached = Find-StageMetric -Stage 'cached_export'
$typed = Find-StageMetric -Stage 'typed_empty_import'
$generic = Find-StageMetric -Stage 'generic_empty_import'
$typedPair = Find-StageMetric -Stage 'typed_i32_pair_import'
$first = $results[0]
$aggregate = [ordered]@{
    schema_version = 1
    benchmark_kind = 'physical_crossing_cost_ladder'
    attempt_id = $attemptId
    profile_sha256 = $profileSha256
    candidate = [ordered]@{
        commit = $AvidScriptCommit
        tree_sha = $AvidScriptTreeSha
        clean = $true
    }
    engine_executable_sha256 = $engineSha256
    kernel_wasm_sha256 = $kernelSha256
    runtime = [ordered]@{
        id = [string]$first.runtime_id
        version = [string]$first.runtime_version
        build_identity = [string]$first.runtime_build_identity
        artifact_sha256 = [string]$first.runtime_artifact_sha256
    }
    process_runs = [int]$profile.process_runs
    timed_samples_per_stage_per_process = [int]$profile.timed_samples
    observation_count = @($results.samples).Count
    process_metrics = $processMetrics
    cross_process_metrics = $crossProcessMetrics
    cost_deltas = [ordered]@{
        typed_empty_over_cached_p95 =
            [double]$typed.process_p95_p95_ns_per_iteration /
            [double]$cached.process_p95_p95_ns_per_iteration
        generic_minus_typed_empty_observed_p95_ns =
            [double]$generic.process_p95_p95_ns_per_iteration -
            [double]$typed.process_p95_p95_ns_per_iteration
        typed_i32_pair_minus_typed_empty_observed_p95_ns =
            [double]$typedPair.process_p95_p95_ns_per_iteration -
            [double]$typed.process_p95_p95_ns_per_iteration
    }
    correctness_failures = 0
    fallback_used = $false
    result_sha256 = @($resultPaths | ForEach-Object {
        Get-SidecarFileSha256 -Path $_
    })
}
$aggregateJson = $aggregate | ConvertTo-Json -Depth 100
$aggregateSchemaPath = Join-Path $controlledRoot 'Schema/PhysicalCostAggregate.schema.json'
if (-not ($aggregateJson | Test-Json -SchemaFile $aggregateSchemaPath)) {
    throw 'ASP54L4717 generated physical cost aggregate does not match schema'
}
$aggregatePath = Join-Path $attemptPath 'aggregate.json'
[IO.File]::WriteAllText(
    $aggregatePath,
    $aggregateJson + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    result = 'physical_cost_ladder_complete'
    aggregate_path = $aggregatePath
    aggregate_sha256 = Get-SidecarFileSha256 -Path $aggregatePath
    typed_empty_p95_ns = [double]$typed.process_p95_p95_ns_per_iteration
    generic_minus_typed_empty_observed_p95_ns =
        [double]$aggregate.cost_deltas.generic_minus_typed_empty_observed_p95_ns
    typed_i32_pair_minus_typed_empty_observed_p95_ns =
        [double]$aggregate.cost_deltas.typed_i32_pair_minus_typed_empty_observed_p95_ns
}
