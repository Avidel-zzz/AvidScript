[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$AttemptPath,

    [ValidateSet('Validate', 'Aggregate')]
    [string]$Mode = 'Aggregate'
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
. (Join-Path $ScriptRoot 'PuertsBenchmarkSidecar.Common.ps1')

function Get-DistributionSummary {
    param([Parameter(Mandatory = $true)][double[]]$Values)

    $P50 = Get-SidecarNearestRankPercentile -Values $Values -Percentile 0.50
    $P95 = Get-SidecarNearestRankPercentile -Values $Values -Percentile 0.95
    [double[]]$AbsoluteDeviations = @($Values | ForEach-Object { [Math]::Abs($_ - $P50) })
    return [pscustomobject][ordered]@{
        sample_count = $Values.Count
        p50 = $P50
        p95 = $P95
        mad = Get-SidecarNearestRankPercentile -Values $AbsoluteDeviations -Percentile 0.50
        geometric_mean = Get-SidecarGeometricMean -Values $Values
    }
}

function Test-IterationMap {
    param(
        [Parameter(Mandatory = $true)]$Actual,
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)]$Profile,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $ExpectedWorkloads = @($Profile.workloads | ForEach-Object { [string]$_ })
    $ActualWorkloads = @($Actual.PSObject.Properties.Name | ForEach-Object { [string]$_ })
    if ($ActualWorkloads.Count -ne $ExpectedWorkloads.Count) {
        throw "ASP53S2046 iteration_counts 数量不一致：$Label"
    }
    foreach ($Workload in $ExpectedWorkloads) {
        if ($ActualWorkloads -cnotcontains $Workload -or
            [int64]$Actual.$Workload -ne [int64]$Expected.$Workload) {
            throw "ASP53S2046 iteration_counts 不一致：$Label workload=$Workload"
        }
    }
}

function Test-RequestProfileContract {
    param(
        [Parameter(Mandatory = $true)]$Request,
        [Parameter(Mandatory = $true)]$Profile,
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][string]$ExpectedMode,
        [Parameter(Mandatory = $true)][int]$ExpectedProcessRun,
        [Parameter(Mandatory = $true)]$ExpectedLaneOrder,
        [Parameter(Mandatory = $true)][int]$ExpectedResultSchemaVersion,
        [Parameter(Mandatory = $true)][string]$ExpectedResultSchemaSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedResultPath,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $ExpectedTemporaryPath = "$ExpectedResultPath.$($Manifest.attempt_id).tmp"
    if ([string]$Request.mode -cne $ExpectedMode -or
        [int]$Request.process_run -ne $ExpectedProcessRun -or
        [string]$Request.attempt_id -cne [string]$Manifest.attempt_id -or
        [int]$Request.warmup_samples -ne [int]$Profile.warmup_samples -or
        [int]$Request.seed -ne [int]$Profile.seed -or
        [double]$Request.minimum_sample_milliseconds -ne [double]$Profile.minimum_sample_milliseconds -or
        [int64]$Request.minimum_iterations -ne [int64]$Profile.minimum_iterations -or
        [int64]$Request.maximum_iterations -ne [int64]$Profile.maximum_iterations -or
        [int]$Request.result_schema.version -ne $ExpectedResultSchemaVersion -or
        [string]$Request.result_schema.sha256 -cne $ExpectedResultSchemaSha256 -or
        -not [string]::Equals(
            [string]$Request.result_path,
            $ExpectedResultPath,
            [System.StringComparison]::OrdinalIgnoreCase) -or
        [string]$Request.result_write.strategy -cne 'same_directory_temporary_then_atomic_rename' -or
        [bool]$Request.result_write.overwrite -or
        -not [string]::Equals(
            [string]$Request.result_write.temporary_path,
            $ExpectedTemporaryPath,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "ASP53S2046 request 未固定 profile/attempt/Schema：$Label"
    }
    $ExpectedTimedSamples = if ($ExpectedMode -ceq 'timed') { [int]$Profile.timed_samples } else { 0 }
    if ([int]$Request.timed_samples -ne $ExpectedTimedSamples) {
        throw "ASP53S2046 request timed_samples 不匹配：$Label"
    }
    Test-SidecarExactArray -Actual $Request.lanes -Expected @($Profile.lanes) `
        -Code 'ASP53S2046' -Label "$Label lanes"
    Test-SidecarExactArray -Actual $Request.workloads -Expected @($Profile.workloads) `
        -Code 'ASP53S2046' -Label "$Label workloads"
    Test-SidecarExactArray -Actual $Request.lane_order -Expected $ExpectedLaneOrder `
        -Code 'ASP53S2007' -Label "$Label lane_order"
    Test-SidecarProvenance `
        -Actual $Request.provenance `
        -Expected $Manifest.provenance `
        -Label $Label
}

$ResolvedAttemptPath = [System.IO.Path]::GetFullPath($AttemptPath)
if (-not (Test-Path -LiteralPath $ResolvedAttemptPath -PathType Container)) {
    throw "ASP53S2022 attempt 目录不存在：$ResolvedAttemptPath"
}
$Manifest = Read-SidecarJson -Path (Join-Path $ResolvedAttemptPath 'attempt.json') -Code 'ASP53S2023'
if ([int]$Manifest.schema_version -ne 1) {
    throw 'ASP53S2023 attempt schema_version 必须为 1'
}

$AggregatePath = Join-Path $ResolvedAttemptPath 'aggregate.json'
if ($Mode -ceq 'Aggregate' -and (Test-Path -LiteralPath $AggregatePath)) {
    throw "ASP53S2026 拒绝覆盖已有聚合结果：$AggregatePath"
}

$ProfilePath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$Manifest.profile.snapshot_path)
$RequestSchemaPath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$Manifest.request_schema.snapshot_path)
$CalibrationSchemaPath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$Manifest.calibration_schema.snapshot_path)
$ResultSchemaPath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$Manifest.result_schema.snapshot_path)
$AggregateSchemaPath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$Manifest.aggregate_schema.snapshot_path)
$ProfileSha256 = Get-SidecarFileSha256 -Path $ProfilePath
$RequestSchemaSha256 = Get-SidecarFileSha256 -Path $RequestSchemaPath
$CalibrationSchemaSha256 = Get-SidecarFileSha256 -Path $CalibrationSchemaPath
$ResultSchemaSha256 = Get-SidecarFileSha256 -Path $ResultSchemaPath
$AggregateSchemaSha256 = Get-SidecarFileSha256 -Path $AggregateSchemaPath
if ($ProfileSha256 -cne [string]$Manifest.profile.sha256) {
    throw 'ASP53S2024 profile 快照 SHA-256 不匹配'
}
if ($RequestSchemaSha256 -cne [string]$Manifest.request_schema.sha256 -or
    $CalibrationSchemaSha256 -cne [string]$Manifest.calibration_schema.sha256 -or
    $ResultSchemaSha256 -cne [string]$Manifest.result_schema.sha256 -or
    $AggregateSchemaSha256 -cne [string]$Manifest.aggregate_schema.sha256) {
    throw 'ASP53S2025 request/calibration/result/aggregate Schema 快照 SHA-256 不匹配'
}

$Profile = Read-SidecarJson -Path $ProfilePath -Code 'ASP53S2027'
Test-SidecarProfile -Profile $Profile
$AggregateSchema = Read-SidecarJson -Path $AggregateSchemaPath -Code 'ASP53S2027'
$IsFormalProfile =
    [int]$Profile.process_runs -eq 5 -and
    [int]$Profile.warmup_samples -eq 5 -and
    [int]$Profile.timed_samples -eq 30 -and
    [Math]::Abs([double]$Profile.minimum_sample_milliseconds - 5.0) -lt 0.000000001
if ([string]$Profile.profile_id -cne [string]$Manifest.profile.id -or
    [int]$Profile.process_runs -ne [int]$Manifest.profile.process_runs -or
    [int]$Profile.warmup_samples -ne [int]$Manifest.profile.warmup_samples -or
    [int]$Profile.timed_samples -ne [int]$Manifest.profile.timed_samples -or
    [string]$Profile.target -cne [string]$Manifest.provenance.target -or
    [string]$Profile.configuration -cne [string]$Manifest.provenance.configuration -or
    [bool]$Profile.null_rhi -ne [bool]$Manifest.provenance.null_rhi -or
    $ProfileSha256 -cne [string]$Manifest.provenance.profile_sha256 -or
    $RequestSchemaSha256 -cne [string]$Manifest.provenance.request_schema_sha256 -or
    $CalibrationSchemaSha256 -cne [string]$Manifest.provenance.calibration_schema_sha256 -or
    $ResultSchemaSha256 -cne [string]$Manifest.provenance.result_schema_sha256 -or
    $AggregateSchemaSha256 -cne [string]$Manifest.provenance.aggregate_schema_sha256 -or
    [int]$AggregateSchema.properties.schema_version.const -ne [int]$Manifest.aggregate_schema.version -or
    (-not $IsFormalProfile -and -not [bool]$Manifest.provenance.allow_non_formal_profile)) {
    throw 'ASP53S2014 attempt provenance 与 profile/Schema 固定值不一致'
}
if ([bool]$Manifest.provenance.avidscript_dirty) {
    throw 'ASP53S2014 attempt provenance 标记为 dirty，拒绝聚合'
}

$CalibrationRequestPath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$Manifest.calibration.request_path)
$CalibrationResultPath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$Manifest.calibration.raw_path)
if ((Get-SidecarFileSha256 -Path $CalibrationRequestPath) -cne [string]$Manifest.calibration.request_sha256) {
    throw 'ASP53S2051 calibration request SHA-256 与 attempt 清单不一致'
}
$CalibrationRequestRaw = Get-Content -LiteralPath $CalibrationRequestPath -Raw
if (-not ($CalibrationRequestRaw | Test-Json -SchemaFile $RequestSchemaPath -ErrorAction SilentlyContinue)) {
    throw 'ASP53S2046 calibration request 不符合固定 Schema'
}
$CalibrationRequest = $CalibrationRequestRaw | ConvertFrom-Json
Test-RequestProfileContract `
    -Request $CalibrationRequest `
    -Profile $Profile `
    -Manifest $Manifest `
    -ExpectedMode calibration `
    -ExpectedProcessRun -1 `
    -ExpectedLaneOrder @($Profile.lanes) `
    -ExpectedResultSchemaVersion ([int]$Manifest.calibration_schema.version) `
    -ExpectedResultSchemaSha256 ([string]$Manifest.calibration_schema.sha256) `
    -ExpectedResultPath $CalibrationResultPath `
    -Label 'calibration request'

$CalibrationMetadataPath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$Manifest.calibration.process_metadata_path)
$ValidatedCalibration = Test-SidecarCalibrationResult `
    -ResultPath $CalibrationResultPath `
    -SchemaPath $CalibrationSchemaPath `
    -Profile $Profile `
    -ExpectedProvenance $Manifest.provenance
$Calibration = $ValidatedCalibration.result
$CalibrationMetadata = Read-SidecarJson -Path $CalibrationMetadataPath -Code 'ASP53S2029'
if ([int]$CalibrationMetadata.process_run -ne -1 -or
    [int]$CalibrationMetadata.exit_code -ne 0 -or
    [int]$CalibrationMetadata.pid -lt 1 -or
    [string]$ValidatedCalibration.sha256 -cne [string]$Manifest.calibration.sha256 -or
    [string]$ValidatedCalibration.sha256 -cne [string]$CalibrationMetadata.calibration_sha256 -or
    [string]$Calibration.calibration_id -cne [string]$Manifest.calibration.calibration_id -or
    [string]$Calibration.calibration_id -cne [string]$Manifest.attempt_id -or
    [int64]$Calibration.timer_frequency_hz -ne [int64]$Manifest.calibration.timer_frequency_hz) {
    throw 'ASP53S2045 calibration 证据 SHA/ID/进程元数据不一致'
}
Test-IterationMap `
    -Actual $Calibration.iteration_counts `
    -Expected $Manifest.calibration.iteration_counts `
    -Profile $Profile `
    -Label 'calibration manifest'

$ExpectedProcessRuns = [int]$Profile.process_runs
$ManifestRuns = @($Manifest.process_runs)
if ($ManifestRuns.Count -ne $ExpectedProcessRuns) {
    throw "ASP53S2028 attempt 进程清单不完整：actual=$($ManifestRuns.Count) expected=$ExpectedProcessRuns"
}

$AllSamples = [System.Collections.Generic.List[object]]::new()
$SourceRuns = [System.Collections.Generic.List[object]]::new()
$ProcessIds = [System.Collections.Generic.HashSet[int]]::new()
[void]$ProcessIds.Add([int]$CalibrationMetadata.pid)
$SeenProcessRuns = [System.Collections.Generic.HashSet[int]]::new()
$CrossProcessCorrectness = @{}
$TimerFrequencyHz = [int64]$Calibration.timer_frequency_hz

foreach ($RunEntry in @($ManifestRuns | Sort-Object { [int]$_.process_run })) {
    $ProcessRun = [int]$RunEntry.process_run
    if ($ProcessRun -lt 0 -or $ProcessRun -ge $ExpectedProcessRuns -or
        -not $SeenProcessRuns.Add($ProcessRun)) {
        throw "ASP53S2028 attempt 含重复或越界进程：$ProcessRun"
    }

    $ExpectedLaneOrder = Get-SidecarRotatedLaneOrder -Lanes @($Profile.lanes) -ProcessRun $ProcessRun
    Test-SidecarExactArray -Actual $RunEntry.lane_order -Expected $ExpectedLaneOrder `
        -Code 'ASP53S2007' -Label "manifest process $ProcessRun lane_order"

    $RequestPath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$RunEntry.request_path)
    $RawResultPath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$RunEntry.raw_result_path)
    if ((Get-SidecarFileSha256 -Path $RequestPath) -cne [string]$RunEntry.request_sha256) {
        throw "ASP53S2051 timed request SHA-256 与 attempt 清单不一致：process=$ProcessRun"
    }
    $RequestRaw = Get-Content -LiteralPath $RequestPath -Raw
    if (-not ($RequestRaw | Test-Json -SchemaFile $RequestSchemaPath -ErrorAction SilentlyContinue)) {
        throw "ASP53S2046 timed request 不符合固定 Schema：process=$ProcessRun"
    }
    $Request = $RequestRaw | ConvertFrom-Json
    Test-RequestProfileContract `
        -Request $Request `
        -Profile $Profile `
        -Manifest $Manifest `
        -ExpectedMode timed `
        -ExpectedProcessRun $ProcessRun `
        -ExpectedLaneOrder $ExpectedLaneOrder `
        -ExpectedResultSchemaVersion ([int]$Manifest.result_schema.version) `
        -ExpectedResultSchemaSha256 ([string]$Manifest.result_schema.sha256) `
        -ExpectedResultPath $RawResultPath `
        -Label "timed request process=$ProcessRun"
    Test-IterationMap `
        -Actual $Request.iteration_counts `
        -Expected $Manifest.calibration.iteration_counts `
        -Profile $Profile `
        -Label "timed request process=$ProcessRun"

    $ProcessMetadataPath = Resolve-SidecarChildPath -Root $ResolvedAttemptPath -RelativePath ([string]$RunEntry.process_metadata_path)
    $ProcessMetadata = Read-SidecarJson -Path $ProcessMetadataPath -Code 'ASP53S2029'
    if ([int]$ProcessMetadata.process_run -ne $ProcessRun -or
        [int]$ProcessMetadata.exit_code -ne 0 -or
        [int]$ProcessMetadata.pid -lt 1) {
        throw "ASP53S2029 timed 进程元数据无效：process=$ProcessRun"
    }
    if (-not $ProcessIds.Add([int]$ProcessMetadata.pid)) {
        throw "ASP53S2047 calibration/timed Editor PID 重复：pid=$($ProcessMetadata.pid)"
    }

    $Validated = Test-SidecarProcessResult `
        -ResultPath $RawResultPath `
        -SchemaPath $ResultSchemaPath `
        -Profile $Profile `
        -Manifest $Manifest `
        -ExpectedProcessRun $ProcessRun
    $Result = $Validated.result
    if ([string]$Validated.sha256 -cne [string]$ProcessMetadata.raw_result_sha256) {
        throw "ASP53S2045 raw result 在进程验收后被改写：process=$ProcessRun"
    }
    if ([int64]$Result.timer_frequency_hz -ne $TimerFrequencyHz) {
        throw "ASP53S2015 raw result 与 calibration 计时器频率不一致：process=$ProcessRun"
    }
    if ([string]$Result.run_id -cne [string]$Manifest.attempt_id) {
        throw "ASP53S2030 raw result 的 run_id 未固定到 attempt_id：process=$ProcessRun"
    }

    foreach ($Sample in @($Result.samples)) {
        $CorrectnessKey = "$($Sample.workload)|$($Sample.sample_index)"
        $CorrectnessValue = "$($Sample.checksum)|$($Sample.final_scalar)|$($Sample.operation_call_count)"
        if (-not $CrossProcessCorrectness.ContainsKey($CorrectnessKey)) {
            $CrossProcessCorrectness[$CorrectnessKey] = $CorrectnessValue
        }
        elseif ([string]$CrossProcessCorrectness[$CorrectnessKey] -cne $CorrectnessValue) {
            throw "ASP53S2048 跨进程 correctness 不一致：process=$ProcessRun key=$CorrectnessKey"
        }
        $AllSamples.Add($Sample)
    }
    $SourceRuns.Add([pscustomobject][ordered]@{
        process_run = $ProcessRun
        run_id = [string]$Result.run_id
        lane_order = @($Result.lane_order)
        relative_path = [string]$RunEntry.raw_result_path
        sha256 = [string]$Validated.sha256
        pid = [int]$ProcessMetadata.pid
    })
}

if ($SeenProcessRuns.Count -ne $ExpectedProcessRuns) {
    throw "ASP53S2028 attempt 缺少进程结果：actual=$($SeenProcessRuns.Count) expected=$ExpectedProcessRuns"
}

$ProcessStatistics = [System.Collections.Generic.List[object]]::new()
for ($ProcessRun = 0; $ProcessRun -lt $ExpectedProcessRuns; ++$ProcessRun) {
    foreach ($Lane in @($Profile.lanes)) {
        foreach ($Workload in @($Profile.workloads)) {
            [double[]]$Values = @(
                $AllSamples |
                    Where-Object {
                        [int]$_.process_run -eq $ProcessRun -and
                        [string]$_.lane -ceq [string]$Lane -and
                        [string]$_.workload -ceq [string]$Workload
                    } |
                    ForEach-Object {
                        ([double]$_.elapsed_cycles / [double]$_.iterations) *
                            (1000000000.0 / [double]$TimerFrequencyHz)
                    }
            )
            if ($Values.Count -ne [int]$Profile.timed_samples) {
                throw "ASP53S2031 process 统计样本不足：process=$ProcessRun lane=$Lane workload=$Workload"
            }
            $Summary = Get-DistributionSummary -Values $Values
            $ProcessStatistics.Add([pscustomobject][ordered]@{
                process_run = $ProcessRun
                lane = [string]$Lane
                workload = [string]$Workload
                sample_count = [int]$Summary.sample_count
                p50_ns_per_operation = [double]$Summary.p50
                p95_ns_per_operation = [double]$Summary.p95
                mad_ns_per_operation = [double]$Summary.mad
                geometric_mean_ns_per_operation = [double]$Summary.geometric_mean
            })
        }
    }
}

$CrossProcessStatistics = [System.Collections.Generic.List[object]]::new()
$DescriptivePooledStatistics = [System.Collections.Generic.List[object]]::new()
foreach ($Lane in @($Profile.lanes)) {
    foreach ($Workload in @($Profile.workloads)) {
        $ProcessEntries = @($ProcessStatistics | Where-Object {
            [string]$_.lane -ceq [string]$Lane -and
            [string]$_.workload -ceq [string]$Workload
        })
        [double[]]$ProcessP50Values = @($ProcessEntries | ForEach-Object { [double]$_.p50_ns_per_operation })
        [double[]]$ProcessP95Values = @($ProcessEntries | ForEach-Object { [double]$_.p95_ns_per_operation })
        [double[]]$ProcessMadValues = @($ProcessEntries | ForEach-Object { [double]$_.mad_ns_per_operation })
        $P50Summary = Get-DistributionSummary -Values $ProcessP50Values
        $CrossProcessStatistics.Add([pscustomobject][ordered]@{
            lane = [string]$Lane
            workload = [string]$Workload
            process_count = $ProcessEntries.Count
            p50_of_process_p50_ns_per_operation = [double]$P50Summary.p50
            p95_of_process_p50_ns_per_operation = [double]$P50Summary.p95
            mad_of_process_p50_ns_per_operation = [double]$P50Summary.mad
            geometric_mean_of_process_p50_ns_per_operation = [double]$P50Summary.geometric_mean
            p50_of_process_p95_ns_per_operation =
                Get-SidecarNearestRankPercentile -Values $ProcessP95Values -Percentile 0.50
            p50_of_process_mad_ns_per_operation =
                Get-SidecarNearestRankPercentile -Values $ProcessMadValues -Percentile 0.50
        })

        [double[]]$PooledValues = @(
            $AllSamples |
                Where-Object {
                    [string]$_.lane -ceq [string]$Lane -and
                    [string]$_.workload -ceq [string]$Workload
                } |
                ForEach-Object {
                    ([double]$_.elapsed_cycles / [double]$_.iterations) *
                        (1000000000.0 / [double]$TimerFrequencyHz)
                }
        )
        $PooledSummary = Get-DistributionSummary -Values $PooledValues
        $DescriptivePooledStatistics.Add([pscustomobject][ordered]@{
            lane = [string]$Lane
            workload = [string]$Workload
            descriptive_only = $true
            sample_count = [int]$PooledSummary.sample_count
            p50_ns_per_operation = [double]$PooledSummary.p50
            p95_ns_per_operation = [double]$PooledSummary.p95
            mad_ns_per_operation = [double]$PooledSummary.mad
            geometric_mean_ns_per_operation = [double]$PooledSummary.geometric_mean
        })
    }
}

$BaselineLane = 'native_cpp'
if (@($Profile.lanes) -cnotcontains $BaselineLane) {
    throw "ASP53S2049 配对统计缺少 baseline lane：$BaselineLane"
}
$PairedComparisons = [System.Collections.Generic.List[object]]::new()
foreach ($Lane in @($Profile.lanes | Where-Object { [string]$_ -cne $BaselineLane })) {
    foreach ($Workload in @($Profile.workloads)) {
        $PerProcessRatios = [System.Collections.Generic.List[object]]::new()
        for ($ProcessRun = 0; $ProcessRun -lt $ExpectedProcessRuns; ++$ProcessRun) {
            $Baseline = @($ProcessStatistics | Where-Object {
                [int]$_.process_run -eq $ProcessRun -and
                [string]$_.lane -ceq $BaselineLane -and
                [string]$_.workload -ceq [string]$Workload
            })
            $Candidate = @($ProcessStatistics | Where-Object {
                [int]$_.process_run -eq $ProcessRun -and
                [string]$_.lane -ceq [string]$Lane -and
                [string]$_.workload -ceq [string]$Workload
            })
            if ($Baseline.Count -ne 1 -or $Candidate.Count -ne 1) {
                throw "ASP53S2050 配对统计缺少 process summary：process=$ProcessRun lane=$Lane workload=$Workload"
            }
            $Ratio = [double]$Candidate[0].p50_ns_per_operation / [double]$Baseline[0].p50_ns_per_operation
            $PerProcessRatios.Add([pscustomobject][ordered]@{
                process_run = $ProcessRun
                baseline_p50_ns_per_operation = [double]$Baseline[0].p50_ns_per_operation
                lane_p50_ns_per_operation = [double]$Candidate[0].p50_ns_per_operation
                ratio = $Ratio
            })
        }
        [double[]]$RatioValues = @($PerProcessRatios | ForEach-Object { [double]$_.ratio })
        $RatioSummary = Get-DistributionSummary -Values $RatioValues
        $PairedComparisons.Add([pscustomobject][ordered]@{
            baseline_lane = $BaselineLane
            lane = [string]$Lane
            workload = [string]$Workload
            process_count = $PerProcessRatios.Count
            per_process_ratios = @($PerProcessRatios)
            p50_ratio = [double]$RatioSummary.p50
            p95_ratio = [double]$RatioSummary.p95
            mad_ratio = [double]$RatioSummary.mad
            geometric_mean_ratio = [double]$RatioSummary.geometric_mean
        })
    }
}

$Aggregate = [pscustomobject][ordered]@{
    schema_version = 2
    attempt_id = [string]$Manifest.attempt_id
    profile = [pscustomobject][ordered]@{
        id = [string]$Manifest.profile.id
        sha256 = [string]$Manifest.profile.sha256
        process_runs = [int]$Profile.process_runs
        warmup_samples = [int]$Profile.warmup_samples
        timed_samples = [int]$Profile.timed_samples
    }
    result_schema = [pscustomobject][ordered]@{
        version = [int]$Manifest.result_schema.version
        sha256 = [string]$Manifest.result_schema.sha256
    }
    aggregate_schema = [pscustomobject][ordered]@{
        version = [int]$Manifest.aggregate_schema.version
        sha256 = [string]$Manifest.aggregate_schema.sha256
    }
    provenance = $Manifest.provenance
    calibration = [pscustomobject][ordered]@{
        calibration_id = [string]$Calibration.calibration_id
        raw_path = [string]$Manifest.calibration.raw_path
        sha256 = [string]$ValidatedCalibration.sha256
        timer_frequency_hz = [int64]$Calibration.timer_frequency_hz
        iteration_counts = $Calibration.iteration_counts
    }
    timer_frequency_hz = $TimerFrequencyHz
    source_runs = @($SourceRuns)
    statistics_method = [pscustomobject][ordered]@{
        percentile = 'nearest_rank'
        mad_center = 'nearest_rank_p50'
        primary_replicate = 'process_summary'
        paired_baseline_lane = $BaselineLane
        pooled_samples = 'descriptive_only'
    }
    process_statistics = @($ProcessStatistics)
    cross_process_statistics = @($CrossProcessStatistics)
    paired_comparisons = @($PairedComparisons)
    descriptive_pooled_statistics = @($DescriptivePooledStatistics)
    samples = @($AllSamples)
}

$AggregateJson = (($Aggregate | ConvertTo-Json -Depth 64) -replace "`r`n", "`n") + "`n"
if (-not ($AggregateJson | Test-Json -SchemaFile $AggregateSchemaPath -ErrorAction SilentlyContinue)) {
    throw 'ASP53S2032 聚合结果未通过 attempt 固定的 BenchmarkAggregate Schema'
}
if ($Mode -ceq 'Aggregate') {
    Write-SidecarNewText -Path $AggregatePath -Value $AggregateJson -Code 'ASP53S2026'
}

[pscustomobject][ordered]@{
    succeeded = $true
    mode = $Mode.ToLowerInvariant()
    attempt_path = $ResolvedAttemptPath
    aggregate_path = if ($Mode -ceq 'Aggregate') { $AggregatePath } else { $null }
    calibration_process_count = 1
    process_run_count = $ExpectedProcessRuns
    raw_sample_count = $AllSamples.Count
    process_statistic_count = $ProcessStatistics.Count
    cross_process_statistic_count = $CrossProcessStatistics.Count
    paired_comparison_count = $PairedComparisons.Count
    descriptive_pooled_statistic_count = $DescriptivePooledStatistics.Count
} | ConvertTo-Json -Depth 8
