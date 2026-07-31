[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProfilePath,

    [Parameter(Mandatory = $true)]
    [string[]]$ProcessResultPath,

    [string]$ControlledSuiteAggregatePath,

    [string[]]$MicroProcessResultPath,

    [string]$MicroProfilePath,

    [string]$PhysicalCostAggregatePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$phase56ComparisonRoot = Split-Path -Parent (
    Split-Path -Parent $PSScriptRoot)
. (Join-Path $phase56ComparisonRoot (
    'ControlledRuntime/Scripts/Phase56Evidence.Common.ps1'))

function Read-JsonFile {
    param([string]$Path)

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    return Get-Content -LiteralPath $resolved -Raw |
        ConvertFrom-Json -Depth 100
}

function Get-NearestRank {
    param(
        [double[]]$Values,
        [double]$Percentile
    )

    if ($Values.Count -eq 0) {
        throw 'Cannot calculate a percentile for an empty set.'
    }
    $sorted = @($Values | Sort-Object)
    $rank = [Math]::Ceiling($Percentile * $sorted.Count)
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, $rank - 1))
    return [double]$sorted[$index]
}

function Get-Mad {
    param([double[]]$Values)

    $center = Get-NearestRank -Values $Values -Percentile 0.50
    $deviations = @($Values | ForEach-Object { [Math]::Abs($_ - $center) })
    return Get-NearestRank -Values $deviations -Percentile 0.50
}

function Get-ExpectedGeneratedHits {
    param(
        [string]$Workload,
        [uint64]$Iterations,
        [uint64]$LogicalOperationCount
    )

    if ($Workload -in @('gameplay_frame_small', 'gameplay_frame_dense')) {
        return $LogicalOperationCount
    }
    if ($Workload -in @(
        'scalar_add_int32',
        'vector_value',
        'object_roundtrip',
        'batch_scalar')) {
        return $Iterations
    }
    if ($Workload -ceq 'property_get_set') {
        return $Iterations * 2u
    }
    return 0u
}

function Get-ExpectedSemanticHits {
    param(
        [string]$Workload,
        [uint64]$Iterations,
        [uint64]$LogicalOperationCount
    )

    if ($Workload -in @('gameplay_frame_small', 'gameplay_frame_dense')) {
        return $LogicalOperationCount
    }
    if ($Workload -ceq 'property_get_set') {
        return $Iterations * 2u
    }
    if ($Workload -in @(
        'scalar_noop',
        'scalar_add_int32',
        'vector_value',
        'vector_ref_out',
        'object_roundtrip',
        'batch_scalar')) {
        return $Iterations
    }
    return 0u
}

function Get-ExpectedAdaptiveNativeHits {
    param(
        [string]$Workload,
        [uint64]$Iterations,
        [pscustomobject]$WorkloadContract
    )

    if ($Workload -in @('gameplay_frame_small', 'gameplay_frame_dense')) {
        [uint64]$scalarPropertyOperationsPerFrame =
            [uint64]$WorkloadContract.logical_entities_per_frame *
            [uint64]$WorkloadContract.scalar_property_operations_per_entity
        [uint64]$propertyWritesPerFrame =
            [uint64]$WorkloadContract.property_write_operations_per_frame
        [uint64]$eventOperationsPerFrame =
            [uint64]$WorkloadContract.event_operations_per_frame
        return $Iterations * (
            $scalarPropertyOperationsPerFrame -
            $propertyWritesPerFrame +
            $eventOperationsPerFrame)
    }
    if ($Workload -in @('scalar_add_int32', 'batch_scalar')) {
        return $Iterations
    }
    return 0u
}

function New-GateResult {
    param(
        [string]$Name,
        [Nullable[double]]$Value,
        [pscustomobject]$Threshold,
        [string]$Reason
    )

    if ($null -eq $Value) {
        return [ordered]@{
            name = $Name
            status = 'not_measured'
            pass = $false
            value = $null
            threshold = $Threshold
            reason = $Reason
        }
    }

    [double]$metricValue = $Value
    $passed = $true
    if ($Threshold.PSObject.Properties.Name -contains 'maximum') {
        $passed = $passed -and ($metricValue -le [double]$Threshold.maximum)
    }
    if ($Threshold.PSObject.Properties.Name -contains 'minimum') {
        $passed = $passed -and ($metricValue -ge [double]$Threshold.minimum)
    }
    return [ordered]@{
        name = $Name
        status = $passed ? 'pass' : 'fail'
        pass = $passed
        value = $metricValue
        threshold = $Threshold
        reason = $Reason
    }
}

function Find-CrossProcessStatistic {
    param(
        [object[]]$Statistics,
        [string]$Lane,
        [string]$Workload
    )

    $match = @($Statistics | Where-Object {
        $_.lane -ceq $Lane -and $_.workload -ceq $Workload
    })
    if ($match.Count -ne 1) {
        return $null
    }
    return $match[0]
}

function Get-Phase54MicroStatistics {
    param(
        [string[]]$ResultPath,
        [string]$CanonicalProfilePath,
        [string]$CandidateCommit,
        [string]$CandidateTree
    )

    if ($ResultPath.Count -ne 5) {
        throw 'Formal supplemental micro evidence requires five process results.'
    }
    $microProfile = Read-JsonFile -Path $CanonicalProfilePath
    $microProfileSha256 =
        (Get-FileHash -LiteralPath $CanonicalProfilePath -Algorithm SHA256).
            Hash.ToLowerInvariant()
    $microResults = @($ResultPath | ForEach-Object { Read-JsonFile -Path $_ })
    $processRuns = @($microResults.process_run | ForEach-Object { [int]$_ } |
        Sort-Object -Unique)
    $runIds = @($microResults.run_id | ForEach-Object { [string]$_ } |
        Sort-Object -Unique)
    $requestHashes = @($microResults.request_sha256 | ForEach-Object {
        [string]$_
    })
    if (@(Compare-Object -ReferenceObject @(0..4) -DifferenceObject $processRuns).Count -ne 0 -or
        $runIds.Count -ne 1 -or [string]::IsNullOrWhiteSpace($runIds[0]) -or
        @($requestHashes | Sort-Object -Unique).Count -ne 5 -or
        @($requestHashes | Where-Object { $_ -cnotmatch '^[0-9a-f]{64}$' }).Count -ne 0) {
        throw 'Formal supplemental micro process/request identities are invalid.'
    }

    $rows = [Collections.Generic.List[object]]::new()
    foreach ($result in $microResults) {
        if ([string]$result.provenance.avidscript_commit -cne $CandidateCommit -or
            [string]$result.provenance.avidscript_tree_sha -cne $CandidateTree -or
            [bool]$result.provenance.avidscript_dirty -or
            [string]$result.provenance.profile_id -cne
                [string]$microProfile.profile_id -or
            [string]$result.provenance.profile_sha256 -cne $microProfileSha256) {
            throw 'Formal supplemental micro candidate/profile identity differs.'
        }
        [double]$timerFrequency = $result.timer_frequency_hz
        foreach ($sample in @($result.samples)) {
            [uint64]$logical = $sample.logical_operation_count
            if (-not [bool]$sample.correct -or $logical -eq 0 -or
                [uint64]$sample.checksum -ne [uint64]$sample.expected_checksum) {
                throw 'Formal supplemental micro result contains an invalid sample.'
            }
            $rows.Add([pscustomobject]@{
                process_run = [int]$result.process_run
                lane = [string]$sample.lane
                workload = [string]$sample.workload
                ns_per_operation =
                    ([double]$sample.elapsed_cycles * 1000000000.0) /
                    ($timerFrequency * [double]$logical)
            })
        }
    }
    $expectedRows = 5 * [int]$microProfile.timed_samples *
        @($microProfile.lanes).Count * @($microProfile.workloads).Count
    if ($rows.Count -ne $expectedRows) {
        throw "Formal supplemental micro matrix differs: $($rows.Count)/$expectedRows."
    }

    $processStats = [Collections.Generic.List[object]]::new()
    foreach ($group in @($rows | Group-Object process_run, lane, workload)) {
        [double[]]$values = @($group.Group | ForEach-Object {
            [double]$_.ns_per_operation
        })
        if ($values.Count -ne [int]$microProfile.timed_samples) {
            throw 'Formal supplemental micro sample count differs.'
        }
        $processStats.Add([pscustomobject]@{
            lane = [string]$group.Group[0].lane
            workload = [string]$group.Group[0].workload
            p50 = Get-NearestRank -Values $values -Percentile 0.50
        })
    }

    $statistics = [Collections.Generic.List[object]]::new()
    foreach ($group in @($processStats | Group-Object lane, workload)) {
        [double[]]$values = @($group.Group | ForEach-Object { [double]$_.p50 })
        if ($values.Count -ne 5) {
            throw 'Formal supplemental micro process count differs.'
        }
        $statistics.Add([pscustomobject]@{
            lane = [string]$group.Group[0].lane
            workload = [string]$group.Group[0].workload
            p50_ns_per_operation =
                Get-NearestRank -Values $values -Percentile 0.50
        })
    }
    return @($statistics)
}

function Get-RequiredMicroP50 {
    param(
        [object[]]$Statistics,
        [string]$Lane,
        [string]$Workload
    )

    $match = @($Statistics | Where-Object {
        [string]$_.lane -ceq $Lane -and
        [string]$_.workload -ceq $Workload
    })
    if ($match.Count -ne 1 -or [double]$match[0].p50_ns_per_operation -le 0) {
        throw "Formal supplemental micro statistic is missing: $Lane/$Workload."
    }
    return [double]$match[0].p50_ns_per_operation
}

function New-GameplayGateResult {
    param(
        [string]$Name,
        [pscustomobject]$Candidate,
        [pscustomobject]$Reflection,
        [pscustomobject]$Static,
        [pscustomobject]$Threshold,
        [bool]$RawEvidenceValid
    )

    if (-not $RawEvidenceValid -or
        $null -eq $Candidate -or
        $null -eq $Reflection -or
        $null -eq $Static) {
        return [ordered]@{
            name = $Name
            status = 'not_measured'
            pass = $false
            value = $null
            threshold = $Threshold
            reason = $RawEvidenceValid ?
                'required gameplay statistics are missing' :
                'raw gameplay evidence is invalid'
            audit = $null
        }
    }

    $p50Comparator = if (
        [double]$Reflection.p50_of_process_p50_ns_per_operation -le
        [double]$Static.p50_of_process_p50_ns_per_operation) {
        $Reflection
    }
    else {
        $Static
    }
    $p95Comparator = if (
        [double]$Reflection.p50_of_process_p95_ns_per_operation -le
        [double]$Static.p50_of_process_p95_ns_per_operation) {
        $Reflection
    }
    else {
        $Static
    }
    $candidateP50 =
        [double]$Candidate.p50_of_process_p50_ns_per_operation
    $candidateP95 =
        [double]$Candidate.p50_of_process_p95_ns_per_operation
    $candidateMad =
        [double]$Candidate.mad_of_process_p50_ns_per_operation
    $comparatorP50 =
        [double]$p50Comparator.p50_of_process_p50_ns_per_operation
    $comparatorP95 =
        [double]$p95Comparator.p50_of_process_p95_ns_per_operation
    $comparatorMad =
        [double]$p50Comparator.mad_of_process_p50_ns_per_operation

    if ($comparatorP50 -le 0.0 -or $comparatorP95 -le 0.0) {
        return [ordered]@{
            name = $Name
            status = 'not_measured'
            pass = $false
            value = $null
            threshold = $Threshold
            reason = 'Puerts comparator statistics must be positive'
            audit = $null
        }
    }

    $maximum = [double]$Threshold.maximum
    $p50Ratio = $candidateP50 / $comparatorP50
    $p95Ratio = $candidateP95 / $comparatorP95
    $candidateUpper2Mad = $candidateP50 + (2.0 * $candidateMad)
    $comparatorLower2Mad = $comparatorP50 - (2.0 * $comparatorMad)
    $p50Pass = $p50Ratio -le $maximum
    $p95Pass = $p95Ratio -le $maximum
    $twoMadSeparated = $candidateUpper2Mad -lt $comparatorLower2Mad
    $passed = $p50Pass -and $p95Pass -and $twoMadSeparated

    return [ordered]@{
        name = $Name
        status = $passed ? 'pass' : 'fail'
        pass = $passed
        value = $p50Ratio
        threshold = $Threshold
        reason = 'P50 ratio, cross-process P95 ratio, and 2xMAD separation'
        audit = [ordered]@{
            candidate_lane = [string]$Candidate.lane
            p50_comparator_lane = [string]$p50Comparator.lane
            p95_comparator_lane = [string]$p95Comparator.lane
            p50_ratio = $p50Ratio
            p95_ratio = $p95Ratio
            maximum_ratio = $maximum
            p50_pass = $p50Pass
            p95_pass = $p95Pass
            candidate_p50_ns_per_operation = $candidateP50
            candidate_median_process_p95_ns_per_operation = $candidateP95
            candidate_mad_ns_per_operation = $candidateMad
            comparator_p50_ns_per_operation = $comparatorP50
            comparator_median_process_p95_ns_per_operation = $comparatorP95
            comparator_mad_ns_per_operation = $comparatorMad
            candidate_upper_2mad_ns_per_operation = $candidateUpper2Mad
            comparator_lower_2mad_ns_per_operation = $comparatorLower2Mad
            two_mad_separated = $twoMadSeparated
        }
    }
}

$profile = Read-JsonFile -Path $ProfilePath
$isPhase56 =
    [string]$profile.profile_id -clike 'phase56.*'
if ($profile.evidence_class -ceq 'formal') {
    if ($profile.process_runs -ne 5 -or
        $profile.warmup_samples -ne 5 -or
        $profile.timed_samples -ne 30) {
        throw 'Formal profile must specify 5 processes, 5 warmups, and 30 samples.'
    }
}
elseif ($profile.process_runs -ne 1) {
    throw 'Diagnostic profile must specify exactly one process.'
}

$resolvedOutput = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $resolvedOutput) {
    throw "Refusing to overwrite gate output: $resolvedOutput"
}
$outputDirectory = Split-Path -Parent $resolvedOutput
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    throw "Gate output directory does not exist: $outputDirectory"
}

$results = @($ProcessResultPath | ForEach-Object { Read-JsonFile -Path $_ })
if ($results.Count -ne [int]$profile.process_runs) {
    throw "Expected $($profile.process_runs) process results, found $($results.Count)."
}

$sourceHashes = @($ProcessResultPath | ForEach-Object {
    (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant()
})
$processRuns = @($results | ForEach-Object { [int]$_.process_run } | Sort-Object -Unique)
if ($processRuns.Count -ne $results.Count) {
    throw 'Process results must have unique process_run values.'
}
$expectedProcessRuns = @(0..([int]$profile.process_runs - 1))
if (@(Compare-Object -ReferenceObject $expectedProcessRuns -DifferenceObject $processRuns).Count -ne 0) {
    throw 'Process results must contain the exact zero-based process_run sequence.'
}
$runIds = @($results | ForEach-Object { [string]$_.run_id } | Sort-Object -Unique)
if ($runIds.Count -ne 1 -or [string]::IsNullOrWhiteSpace($runIds[0])) {
    throw 'Timed process results must share one non-empty run_id.'
}
$requestHashes = @($results | ForEach-Object { [string]$_.request_sha256 })
if (@($requestHashes | Sort-Object -Unique).Count -ne $results.Count -or
    @($requestHashes | Where-Object { $_ -cnotmatch '^[0-9a-f]{64}$' }).Count -ne 0) {
    throw 'Timed process results must carry distinct lowercase SHA-256 request identities.'
}

$identityFields = @(
    'editor_executable_sha256',
    'harness_module_sha256',
    'avidscript_commit',
    'avidscript_tree_sha',
    'puerts_commit',
    'puerts_runtime_sha256',
    'puerts_reflection_script_sha256',
    'puerts_static_script_sha256',
    'wasmtime_runtime_sha256',
    'wasm_sha256',
    'manifest_sha256',
    'profile_id',
    'profile_sha256',
    'lane_catalog_sha256',
    'request_schema_sha256',
    'result_schema_sha256'
)
$provenance = [ordered]@{}
foreach ($field in $identityFields) {
    $values = @($results | ForEach-Object { [string]$_.provenance.$field } |
        Sort-Object -Unique)
    if ($values.Count -ne 1 -or [string]::IsNullOrWhiteSpace($values[0])) {
        throw "Process provenance disagrees or is empty: $field"
    }
    $provenance[$field] = $values[0]
}
$expectedProfileSha256 =
    (Get-FileHash -LiteralPath $ProfilePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ([string]$provenance.profile_id -cne [string]$profile.profile_id -or
    [string]$provenance.profile_sha256 -cne $expectedProfileSha256) {
    throw 'Process provenance does not identify the evaluated profile bytes.'
}
if ($profile.evidence_class -ceq 'formal' -and
    @($results | Where-Object { [bool]$_.provenance.avidscript_dirty }).Count -ne 0) {
    throw 'Formal evidence requires a clean candidate tree.'
}

$validityErrors = [Collections.Generic.List[string]]::new()
$sampleRows = [Collections.Generic.List[object]]::new()
$crossingRatioLimit = [double]$profile.validity.data_lane_max_crossing_ratio
foreach ($result in $results) {
    $timerFrequency = [double]$result.timer_frequency_hz
    foreach ($sample in @($result.samples)) {
        $requiredCounters = @(
            'host_import_call_count',
            'expected_host_import_call_count',
            'generated_s1_hit_count',
            'generated_s1_fallback_count',
            'generated_s1_reject_count',
            'generated_fused_fast_hit_count',
            'generated_fused_revalidate_count',
            'generated_fused_call_site_prepare_count',
            'generated_direct_read_prepare_count',
            'generated_direct_write_prepare_count',
            'generated_journal_slow_path_count',
            'data_lane_command_count',
            'data_lane_crossing_count',
            'data_lane_rejected_buffer_count',
            'semantic_hit_count',
            'logical_operation_count',
            'checksum'
        )
        if ([string]$sample.lane -ceq 'avidscript_wasmtime_adaptive_semantic') {
            $requiredCounters += @(
                'adaptive_native_hit_count',
                'adaptive_process_event_fallback_count',
                'adaptive_guard_reject_count'
            )
        }
        foreach ($counter in $requiredCounters) {
            if ($sample.PSObject.Properties.Name -notcontains $counter) {
                $validityErrors.Add(
                "missing $counter process=$($result.process_run) lane=$($sample.lane) workload=$($sample.workload)")
            }
        }
        $workloadProperty = $profile.workload_contracts.PSObject.Properties[
            [string]$sample.workload
        ]
        if ($null -eq $workloadProperty) {
            throw "Profile has no workload contract for $($sample.workload)."
        }
        $workloadContract = $workloadProperty.Value
        $expectedFixtureCalls =
            [uint64]$sample.iterations *
            [uint64]$workloadContract.fixture_function_calls_per_frame
        if ([uint64]$sample.checksum -ne [uint64]$sample.expected_checksum) {
            $validityErrors.Add(
                "checksum mismatch process=$($result.process_run) lane=$($sample.lane) workload=$($sample.workload)")
        }
        if ([uint64]$sample.operation_call_count -ne $expectedFixtureCalls -or
            [uint64]$sample.expected_operation_call_count -ne
                $expectedFixtureCalls) {
            $validityErrors.Add(
                "fixture operation count mismatch process=$($result.process_run) lane=$($sample.lane) workload=$($sample.workload)")
        }

        $logical = [uint64]$sample.logical_operation_count
        if ($logical -eq 0) {
            $validityErrors.Add(
                "zero logical operation count process=$($result.process_run) lane=$($sample.lane) workload=$($sample.workload)")
            continue
        }
        $expectedGeneratedHits = Get-ExpectedGeneratedHits `
            -Workload ([string]$sample.workload) `
            -Iterations ([uint64]$sample.iterations) `
            -LogicalOperationCount $logical
        $expectedSemanticHits = Get-ExpectedSemanticHits `
            -Workload ([string]$sample.workload) `
            -Iterations ([uint64]$sample.iterations) `
            -LogicalOperationCount $logical
        $expectedAdaptiveNativeHits = Get-ExpectedAdaptiveNativeHits `
            -Workload ([string]$sample.workload) `
            -Iterations ([uint64]$sample.iterations) `
            -WorkloadContract $workloadContract
        [uint64]$expectedAdaptiveFallbackHits =
            $expectedSemanticHits - $expectedAdaptiveNativeHits
        $isGameplayWorkload = [string]$sample.workload -in @(
            'gameplay_frame_small',
            'gameplay_frame_dense')
        $isDataLane =
            [string]$sample.lane -ceq
                'avidscript_wasmtime_data_oriented'
        $expectedFusedHits = Get-ExpectedFusedGeneratedHits `
            -Workload ([string]$sample.workload) `
            -Iterations ([uint64]$sample.iterations) `
            -WorkloadContract $workloadContract `
            -DataLane $isDataLane
        $isFusedLane = [string]$sample.lane -in @(
            'avidscript_wasmtime_generated_s1',
            'avidscript_wasmtime_data_oriented')
        $isAdaptiveLane =
            [string]$sample.lane -ceq
                'avidscript_wasmtime_adaptive_semantic'
        $directPrepareCount =
            [uint64]$sample.generated_direct_read_prepare_count +
            [uint64]$sample.generated_direct_write_prepare_count
        $fusedInvalid = if ($isFusedLane) {
            [uint64]$sample.generated_journal_slow_path_count -ne 0 -or
            ($expectedFusedHits -gt 0 -and (
                [uint64]$sample.generated_fused_revalidate_count -ne 1 -or
                [uint64]$sample.generated_fused_fast_hit_count + 1 -ne
                    $expectedFusedHits -or
                [uint64]$sample.generated_fused_call_site_prepare_count -eq 0 -or
                $directPrepareCount -ne
                    [uint64]$sample.generated_fused_call_site_prepare_count)) -or
            ($expectedFusedHits -eq 0 -and (
                [uint64]$sample.generated_fused_fast_hit_count -ne 0 -or
                [uint64]$sample.generated_fused_revalidate_count -ne 0 -or
                [uint64]$sample.generated_fused_call_site_prepare_count -ne 0 -or
                $directPrepareCount -ne 0))
        }
        elseif ($isAdaptiveLane) {
            if ($expectedAdaptiveNativeHits -gt 0) {
                [uint64]$sample.generated_fused_revalidate_count -ne 1 -or
                [uint64]$sample.generated_fused_fast_hit_count + 1 -ne
                    $expectedAdaptiveNativeHits -or
                [uint64]$sample.generated_fused_call_site_prepare_count -ne 0 -or
                $directPrepareCount -ne 0 -or
                [uint64]$sample.generated_journal_slow_path_count -ne 0
            }
            else {
                [uint64]$sample.generated_fused_fast_hit_count -ne 0 -or
                [uint64]$sample.generated_fused_revalidate_count -ne 0 -or
                [uint64]$sample.generated_fused_call_site_prepare_count -ne 0 -or
                $directPrepareCount -ne 0 -or
                [uint64]$sample.generated_journal_slow_path_count -ne 0
            }
        }
        else {
            [uint64]$sample.generated_fused_fast_hit_count -ne 0 -or
            [uint64]$sample.generated_fused_revalidate_count -ne 0 -or
            [uint64]$sample.generated_fused_call_site_prepare_count -ne 0 -or
            $directPrepareCount -ne 0 -or
            [uint64]$sample.generated_journal_slow_path_count -ne 0
        }
        if ($fusedInvalid) {
            $validityErrors.Add(
                "fused path mismatch process=$($result.process_run) lane=$($sample.lane) workload=$($sample.workload)")
        }
        if ($profile.evidence_class -ceq 'formal' -and (
            [uint64]$sample.generated_fused_fast_hit_cycles -ne 0 -or
            [uint64]$sample.generated_fused_revalidate_cycles -ne 0 -or
            [uint64]$sample.generated_fused_call_site_prepare_cycles -ne 0 -or
            [uint64]$sample.generated_journal_slow_path_cycles -ne 0)) {
            $validityErrors.Add(
                "formal fused timing must stay disabled process=$($result.process_run) lane=$($sample.lane) workload=$($sample.workload)")
        }
        $isAvidScriptLane =
            ([string]$sample.lane).StartsWith(
                'avidscript_',
                [StringComparison]::Ordinal)
        if ($isAvidScriptLane -and
            [uint64]$sample.host_import_call_count -ne
                [uint64]$sample.expected_host_import_call_count) {
            $validityErrors.Add(
                "host import count mismatch process=$($result.process_run) lane=$($sample.lane) workload=$($sample.workload)")
        }
        if ($sample.lane -ceq 'avidscript_wasmtime_generated_s1' -and
            ([uint64]$sample.generated_s1_hit_count -ne $expectedGeneratedHits -or
             [uint64]$sample.generated_s1_fallback_count -ne 0 -or
             [uint64]$sample.generated_s1_reject_count -ne 0 -or
             [uint64]$sample.semantic_hit_count -ne 0 -or
             [uint64]$sample.data_lane_command_count -ne 0 -or
             [uint64]$sample.data_lane_crossing_count -ne 0 -or
             [uint64]$sample.data_lane_rejected_buffer_count -ne 0)) {
            $validityErrors.Add(
                "generated S1 path mismatch process=$($result.process_run) workload=$($sample.workload)")
        }
        if ($isAdaptiveLane -and
            ([uint64]$sample.adaptive_native_hit_count -ne
                $expectedAdaptiveNativeHits -or
             [uint64]$sample.adaptive_process_event_fallback_count -ne
                $expectedAdaptiveFallbackHits -or
             [uint64]$sample.adaptive_guard_reject_count -ne 0 -or
             [uint64]$sample.semantic_hit_count -ne
                $expectedAdaptiveFallbackHits -or
             [uint64]$sample.generated_s1_hit_count -ne 0 -or
             [uint64]$sample.generated_s1_fallback_count -ne 0 -or
             [uint64]$sample.generated_s1_reject_count -ne 0 -or
             [uint64]$sample.data_lane_command_count -ne 0 -or
             [uint64]$sample.data_lane_crossing_count -ne 0 -or
             [uint64]$sample.data_lane_rejected_buffer_count -ne 0)) {
            $validityErrors.Add(
                "adaptive path mismatch process=$($result.process_run) workload=$($sample.workload)")
        }
        if ($sample.lane -ceq 'avidscript_wasmtime_semantic' -and
            ([uint64]$sample.semantic_hit_count -ne $expectedSemanticHits -or
             [uint64]$sample.generated_s1_hit_count -ne 0 -or
             [uint64]$sample.generated_s1_fallback_count -ne 0 -or
             [uint64]$sample.generated_s1_reject_count -ne 0 -or
             [uint64]$sample.data_lane_command_count -ne 0 -or
             [uint64]$sample.data_lane_crossing_count -ne 0 -or
             [uint64]$sample.data_lane_rejected_buffer_count -ne 0)) {
            $validityErrors.Add(
                "semantic path mismatch process=$($result.process_run) workload=$($sample.workload)")
        }
        if ($sample.lane -ceq 'avidscript_wasmtime_data_oriented') {
            $commands = [uint64]$sample.data_lane_command_count
            $crossings = [uint64]$sample.data_lane_crossing_count
            $isGameplay = [string]$sample.workload -in @(
                'gameplay_frame_small',
                'gameplay_frame_dense')
            $propertyWrites = $isGameplay ?
                ([uint64]$sample.iterations *
                    [uint64]$workloadContract.property_write_operations_per_frame) :
                0u
            $expectedDataGeneratedHits = $isGameplay ?
                ($logical - $propertyWrites) :
                $expectedGeneratedHits
            $expectedCrossings = $isGameplay ?
                [uint64]($propertyWrites / 2) :
                0u
            $expectedDataHostCalls = $isGameplay ?
                (1u + $expectedDataGeneratedHits + $expectedCrossings) :
                [uint64]$sample.expected_host_import_call_count
            $invalidCrossingRatio = $commands -gt 0 -and
                ([double]$crossings / [double]$commands) -gt $crossingRatioLimit
            if ([uint64]$sample.generated_s1_hit_count -ne $expectedDataGeneratedHits -or
                [uint64]$sample.generated_s1_fallback_count -ne 0 -or
                [uint64]$sample.generated_s1_reject_count -ne 0 -or
                [uint64]$sample.semantic_hit_count -ne 0 -or
                $commands -ne $propertyWrites -or
                $crossings -ne $expectedCrossings -or
                [uint64]$sample.expected_host_import_call_count -ne
                    $expectedDataHostCalls -or
                [uint64]$sample.data_lane_rejected_buffer_count -ne 0 -or
                ($isGameplay -and $commands -eq 0) -or
                $invalidCrossingRatio) {
                $validityErrors.Add(
                    "data path mismatch process=$($result.process_run) workload=$($sample.workload)")
            }
        }

        $sampleRows.Add([pscustomobject]@{
            process_run = [int]$result.process_run
            lane = [string]$sample.lane
            workload = [string]$sample.workload
            sample_index = [int]$sample.sample_index
            ns_per_operation =
                ([double]$sample.elapsed_cycles * 1000000000.0) /
                ($timerFrequency * [double]$logical)
            peak_memory_bytes = [uint64]$sample.peak_memory_bytes
            allocation_status = [string]$sample.allocations.status
            generated_code_size_status =
                [string]$sample.generated_code_size.status
        })
    }
}

$expectedSamples =
    [int]$profile.process_runs *
    [int]$profile.timed_samples *
    @($profile.lanes).Count *
    @($profile.workloads).Count
if ($sampleRows.Count -ne $expectedSamples) {
    $validityErrors.Add(
        "sample matrix mismatch expected=$expectedSamples actual=$($sampleRows.Count)")
}

$processStatistics = [Collections.Generic.List[object]]::new()
foreach ($group in @($sampleRows | Group-Object process_run, lane, workload)) {
    $first = $group.Group[0]
    $values = [double[]]@($group.Group | ForEach-Object { $_.ns_per_operation })
    $processStatistics.Add([pscustomobject][ordered]@{
        process_run = $first.process_run
        lane = $first.lane
        workload = $first.workload
        sample_count = $values.Count
        p50_ns_per_operation = Get-NearestRank -Values $values -Percentile 0.50
        p95_ns_per_operation = Get-NearestRank -Values $values -Percentile 0.95
        mad_ns_per_operation = Get-Mad -Values $values
        peak_memory_bytes = [uint64](($group.Group |
            Measure-Object -Property peak_memory_bytes -Maximum).Maximum)
    })
    if ($values.Count -ne [int]$profile.timed_samples) {
        throw 'Gameplay process statistic sample count differs from the profile.'
    }
}

$expectedProcessStatistics =
    [int]$profile.process_runs *
    @($profile.lanes).Count *
    @($profile.workloads).Count
if ($processStatistics.Count -ne $expectedProcessStatistics) {
    throw "Gameplay process statistic matrix differs: $($processStatistics.Count)/$expectedProcessStatistics."
}

$crossProcessStatistics = [Collections.Generic.List[object]]::new()
foreach ($group in @($processStatistics | Group-Object lane, workload)) {
    $first = $group.Group[0]
    $p50Values = [double[]]@($group.Group | ForEach-Object {
        $_.p50_ns_per_operation
    })
    $p95Values = [double[]]@($group.Group | ForEach-Object {
        $_.p95_ns_per_operation
    })
    $madValues = [double[]]@($group.Group | ForEach-Object {
        $_.mad_ns_per_operation
    })
    $crossProcessStatistics.Add([ordered]@{
        lane = $first.lane
        workload = $first.workload
        process_count = $group.Count
        p50_of_process_p50_ns_per_operation =
            Get-NearestRank -Values $p50Values -Percentile 0.50
        p95_of_process_p50_ns_per_operation =
            Get-NearestRank -Values $p50Values -Percentile 0.95
        mad_of_process_p50_ns_per_operation = Get-Mad -Values $p50Values
        p50_of_process_p95_ns_per_operation =
            Get-NearestRank -Values $p95Values -Percentile 0.50
        p50_of_process_mad_ns_per_operation =
            Get-NearestRank -Values $madValues -Percentile 0.50
    })
    if ($group.Count -ne [int]$profile.process_runs) {
        throw 'Gameplay cross-process statistic process count differs from the profile.'
    }
}

$expectedCrossProcessStatistics =
    @($profile.lanes).Count * @($profile.workloads).Count
if ($crossProcessStatistics.Count -ne $expectedCrossProcessStatistics) {
    throw "Gameplay cross-process statistic matrix differs: $($crossProcessStatistics.Count)/$expectedCrossProcessStatistics."
}

$supplementalGateInputs = $null
$supplementalCandidateMatch = $false
$supplementalReason = 'required raw supplemental evidence is missing'
$supplementalCommit = $null
$supplementalTree = $null
$supplementalSourceSha256 = $null
$supplementalProvided =
    -not [string]::IsNullOrWhiteSpace($ControlledSuiteAggregatePath) -and
    $null -ne $MicroProcessResultPath -and
    $MicroProcessResultPath.Count -gt 0 -and
    -not [string]::IsNullOrWhiteSpace($PhysicalCostAggregatePath)
if ($supplementalProvided) {
    $comparisonRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $controlledRoot = Join-Path $comparisonRoot 'ControlledRuntime'
    $suiteJson = Get-Content -LiteralPath $ControlledSuiteAggregatePath -Raw
    $physicalJson = Get-Content -LiteralPath $PhysicalCostAggregatePath -Raw
    if (-not ($suiteJson | Test-Json -SchemaFile (
            Join-Path $controlledRoot 'Schema/ControlledRuntimeSuiteAggregate.schema.json')) -or
        -not ($physicalJson | Test-Json -SchemaFile (
            Join-Path $controlledRoot 'Schema/PhysicalCostAggregate.schema.json'))) {
        throw 'Raw supplemental aggregate schema validation failed.'
    }
    $suite = $suiteJson | ConvertFrom-Json -Depth 100
    $physical = $physicalJson | ConvertFrom-Json -Depth 100
    if ($isPhase56) {
        $physicalProfilePath = Join-Path $controlledRoot (
            'Config/PhysicalCostProfile.json')
        $physicalProfile = Read-JsonFile -Path $physicalProfilePath
        $physicalProfileSha256 = (
            Get-FileHash -LiteralPath $physicalProfilePath -Algorithm SHA256
        ).Hash.ToLowerInvariant()
        Assert-PhysicalCostAggregateIdentity `
            -Aggregate $physical `
            -ExpectedProfile $physicalProfile `
            -ExpectedProfileSha256 $physicalProfileSha256
        $physicalStageCount = @($physicalProfile.stages).Count
        $expectedPhysicalObservations =
            [int]$physicalProfile.process_runs *
            [int]$physicalProfile.timed_samples *
            $physicalStageCount
        $expectedPhysicalProcessMetrics =
            [int]$physicalProfile.process_runs * $physicalStageCount
        $expectedPairedProcessCosts =
            [int]$physicalProfile.process_runs * 4
        $expectedFullReconstructions =
            [int]$physicalProfile.process_runs * 3
        if ([int]$physical.observation_count -ne
                $expectedPhysicalObservations -or
            @($physical.process_metrics).Count -ne
                $expectedPhysicalProcessMetrics -or
            @($physical.cross_process_metrics).Count -ne
                $physicalStageCount -or
            @($physical.paired_process_costs).Count -ne
                $expectedPairedProcessCosts -or
            @($physical.paired_cost_metrics).Count -ne 4 -or
            @($physical.full_crossing_reconstructions).Count -ne
                $expectedFullReconstructions) {
            throw 'Phase56 physical aggregate does not match the tracked formal profile and complete sample matrix.'
        }
    }
    $supplementalCommit = [string]$suite.candidate.commit
    $supplementalTree = [string]$suite.candidate.tree_sha
    if ($supplementalCommit -cne [string]$provenance.avidscript_commit -or
        $supplementalTree -cne [string]$provenance.avidscript_tree_sha -or
        [string]$physical.candidate.commit -cne $supplementalCommit -or
        [string]$physical.candidate.tree_sha -cne $supplementalTree -or
        -not [bool]$suite.candidate.clean -or
        -not [bool]$physical.candidate.clean -or
        [int]$suite.correctness_failures -ne 0 -or
        [int]$physical.correctness_failures -ne 0 -or
        [bool]$suite.fallback_used -or
        [bool]$physical.fallback_used) {
        throw 'Raw supplemental candidate, correctness, or fallback identity differs.'
    }

    $microProfilePath = if (
        [string]::IsNullOrWhiteSpace($MicroProfilePath)) {
        Join-Path $comparisonRoot (
            $isPhase56 ?
                'Profiles/Phase56Micro.formal.json' :
                'Profiles/Phase54Micro.formal.json')
    }
    else {
        (Resolve-Path -LiteralPath $MicroProfilePath).Path
    }
    $microStats = Get-Phase54MicroStatistics `
        -ResultPath $MicroProcessResultPath `
        -CanonicalProfilePath $microProfilePath `
        -CandidateCommit $supplementalCommit `
        -CandidateTree $supplementalTree
    $adaptiveSemanticScalar = Get-RequiredMicroP50 -Statistics $microStats `
        -Lane 'avidscript_wasmtime_adaptive_semantic' -Workload 'scalar_add_int32'
    $generatedScalar = Get-RequiredMicroP50 -Statistics $microStats `
        -Lane 'avidscript_wasmtime_generated_s1' -Workload 'scalar_add_int32'
    $reflectionScalar = Get-RequiredMicroP50 -Statistics $microStats `
        -Lane 'puerts_v8_reflection' -Workload 'scalar_add_int32'
    $staticScalar = Get-RequiredMicroP50 -Statistics $microStats `
        -Lane 'puerts_v8_static' -Workload 'scalar_add_int32'
    $generatedProperty = Get-RequiredMicroP50 -Statistics $microStats `
        -Lane 'avidscript_wasmtime_generated_s1' -Workload 'property_get_set'
    $generatedCallback = Get-RequiredMicroP50 -Statistics $microStats `
        -Lane 'avidscript_wasmtime_generated_s1' -Workload 'callback_empty'
    $reflectionCallback = Get-RequiredMicroP50 -Statistics $microStats `
        -Lane 'puerts_v8_reflection' -Workload 'callback_empty'
    $staticCallback = Get-RequiredMicroP50 -Statistics $microStats `
        -Lane 'puerts_v8_static' -Workload 'callback_empty'
    $generatedVector = Get-RequiredMicroP50 -Statistics $microStats `
        -Lane 'avidscript_wasmtime_generated_s1' -Workload 'vector_value'

    $historicalPath = Join-Path (
        Split-Path -Parent (Split-Path -Parent $comparisonRoot)) (
        'Docs/Phase54/P54_CSharp_Five_Lane_Benchmark_Evidence.json')
    $historical = Read-JsonFile -Path $historicalPath
    $historicalVector = @($historical.formal.results | Where-Object {
        [string]$_.workload -ceq 'vector_value'
    })
    if ($historicalVector.Count -ne 1 -or
        [double]$historicalVector[0].lanes.avidscript_wasmtime_jit.p50 -le 0) {
        throw 'Tracked Phase54 vector baseline is invalid.'
    }
    $typedMetric = @($physical.cross_process_metrics | Where-Object {
        [string]$_.stage -ceq 'typed_empty_import'
    })
    if ($typedMetric.Count -ne 1) {
        throw 'Physical cost aggregate has no typed empty import metric.'
    }

    $microRawResults = @($MicroProcessResultPath | ForEach-Object {
        Read-JsonFile -Path $_
    })
    $fusedSamples = @($microRawResults.samples | Where-Object {
        Test-Phase56MeasuredFusedSample -Sample $_
    })
    [double]$fusedFastHitCount = [double](
        $fusedSamples |
            Measure-Object -Property generated_fused_fast_hit_count -Sum
    ).Sum
    [double]$fusedRevalidateCount = [double](
        $fusedSamples |
            Measure-Object -Property generated_fused_revalidate_count -Sum
    ).Sum
    [double]$directPrepareCount = [double](
        $fusedSamples |
            Measure-Object -Property generated_direct_read_prepare_count -Sum
    ).Sum + [double](
        $fusedSamples |
            Measure-Object -Property generated_direct_write_prepare_count -Sum
    ).Sum
    [double]$journalSlowPathCount = [double](
        $microRawResults.samples |
            Measure-Object -Property generated_journal_slow_path_count -Sum
    ).Sum
    [double]$fusedHitCount =
        $fusedFastHitCount + $fusedRevalidateCount
    if ($isPhase56 -and (
        $fusedSamples.Count -eq 0 -or
        $fusedHitCount -le 0.0)) {
        throw 'Phase56 fused-path evidence contains no measured call-site hits.'
    }

    $supplementalGateInputs = [ordered]@{
        wasmtime_v8_geo_ratio = [Math]::Max(
            [double]$suite.leadership.p50_geometric_mean_ratio,
            [double]$suite.leadership.p95_geometric_mean_ratio)
        kernel_win_rate = [Math]::Min(
            [double]$suite.leadership.p50_kernel_win_rate,
            [double]$suite.leadership.p95_kernel_win_rate)
        semantic_vs_puerts_reflection =
            $adaptiveSemanticScalar / $reflectionScalar
        s1_scalar_ns = $generatedScalar
        s1_vs_puerts_static = $generatedScalar / $staticScalar
        s1_property_ns = $generatedProperty
        callback_ns = $generatedCallback
        callback_vs_puerts = $generatedCallback /
            [Math]::Min($reflectionCallback, $staticCallback)
        vector_vs_phase54_baseline = $generatedVector /
            [double]$historicalVector[0].lanes.avidscript_wasmtime_jit.p50
    }
    if ($isPhase56) {
        $supplementalGateInputs.prepared_export_ratio =
            [double]$physical.cost_deltas.prepared_export_over_generic_p50
        $supplementalGateInputs.physical_reconstruction_error_ratio =
            [double]$physical.cost_deltas.max_reconstruction_error_ratio
        $supplementalGateInputs.typed_empty_net_ns =
            [double]$physical.cost_deltas.typed_empty_net_p50_ns
        $supplementalGateInputs.fused_fast_hit_ratio =
            $fusedFastHitCount / $fusedHitCount
        $supplementalGateInputs.fused_revalidate_per_callback =
            $fusedRevalidateCount / [double]$fusedSamples.Count
        $supplementalGateInputs.direct_effect_prepare_count =
            $directPrepareCount
        $supplementalGateInputs.journal_slow_path_count =
            $journalSlowPathCount
    }
    else {
        $supplementalGateInputs.typed_empty_explained_ratio =
            [double]$typedMetric[0].process_p95_median_ns_per_iteration /
            $staticScalar
    }
    $supplementalSourceSha256 = [ordered]@{
        controlled_suite = (Get-FileHash -LiteralPath $ControlledSuiteAggregatePath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        micro_process_results = @($MicroProcessResultPath | ForEach-Object {
            (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant()
        })
        physical_cost = (Get-FileHash -LiteralPath $PhysicalCostAggregatePath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        historical_vector_baseline = (Get-FileHash -LiteralPath $historicalPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $supplementalCandidateMatch = $true
    $supplementalReason =
        'gate inputs were derived directly from candidate-bound raw evidence'
}
$supplementalAudit = [ordered]@{
    provided = $supplementalProvided
    supplemental_candidate_match = $supplementalCandidateMatch
    process_avidscript_commit = [string]$provenance.avidscript_commit
    process_avidscript_tree_sha = [string]$provenance.avidscript_tree_sha
    supplemental_avidscript_commit = $supplementalCommit
    supplemental_avidscript_tree_sha = $supplementalTree
    source_sha256 = $supplementalSourceSha256
    reason = $supplementalReason
}

$gates = [ordered]@{}
$supplementalGateNames = @(
    'wasmtime_v8_geo_ratio',
    'kernel_win_rate',
    'semantic_vs_puerts_reflection',
    's1_scalar_ns',
    's1_vs_puerts_static',
    's1_property_ns',
    'callback_ns',
    'callback_vs_puerts',
    'vector_vs_phase54_baseline'
)
if ($isPhase56) {
    $supplementalGateNames += @(
        'prepared_export_ratio',
        'physical_reconstruction_error_ratio',
        'typed_empty_net_ns',
        'fused_fast_hit_ratio',
        'fused_revalidate_per_callback',
        'direct_effect_prepare_count',
        'journal_slow_path_count'
    )
}
else {
    $supplementalGateNames += 'typed_empty_explained_ratio'
}
foreach ($name in $supplementalGateNames) {
    $value = $null
    if ($supplementalCandidateMatch -and
        $supplementalGateInputs.Contains($name)) {
        $value = [Nullable[double]]([double]$supplementalGateInputs[$name])
    }
    $gates[$name] = New-GateResult `
        -Name $name `
        -Value $value `
        -Threshold $profile.gates.$name `
        -Reason ($null -eq $value ? $supplementalReason : $supplementalReason)
}

foreach ($definition in @(
    [pscustomobject]@{
        name = 'small_vs_best_puerts'
        workload = 'gameplay_frame_small'
    },
    [pscustomobject]@{
        name = 'dense_vs_best_puerts'
        workload = 'gameplay_frame_dense'
    }
)) {
    $threshold = $profile.gates.($definition.name)
    $candidate = Find-CrossProcessStatistic `
        -Statistics $crossProcessStatistics `
        -Lane ([string]$threshold.lane) `
        -Workload $definition.workload
    $reflection = Find-CrossProcessStatistic `
        -Statistics $crossProcessStatistics `
        -Lane 'puerts_v8_reflection' `
        -Workload $definition.workload
    $static = Find-CrossProcessStatistic `
        -Statistics $crossProcessStatistics `
        -Lane 'puerts_v8_static' `
        -Workload $definition.workload
    $gates[$definition.name] = New-GameplayGateResult `
        -Name $definition.name `
        -Candidate $candidate `
        -Reflection $reflection `
        -Static $static `
        -Threshold $threshold `
        -RawEvidenceValid ($validityErrors.Count -eq 0)
}

$output = [ordered]@{
    schema_version = 1
    profile = [ordered]@{
        id = [string]$profile.profile_id
        evidence_class = [string]$profile.evidence_class
        process_runs = [int]$profile.process_runs
        warmup_samples = [int]$profile.warmup_samples
        timed_samples = [int]$profile.timed_samples
    }
    provenance = $provenance
    supplemental_evidence = $supplementalAudit
    source_process_result_sha256 = $sourceHashes
    validity = [ordered]@{
        valid = $validityErrors.Count -eq 0
        errors = @($validityErrors)
    }
    statistics_method = [ordered]@{
        percentile = 'nearest_rank'
        mad_center = 'nearest_rank_p50'
        primary_replicate = 'process_summary'
        unit = 'ns_per_logical_operation'
    }
    process_statistics = @($processStatistics)
    cross_process_statistics = @($crossProcessStatistics)
    gates = $gates
}
$failedGateNames = @($gates.GetEnumerator() | Where-Object {
    -not [bool]$_.Value.pass
} | ForEach-Object {
    [string]$_.Key
})
$output.overall_pass = Test-PerformanceGateOverallPass `
    -ValidityValid ($validityErrors.Count -eq 0) `
    -Gates $gates
$output.failed_gates = $failedGateNames

$json = $output | ConvertTo-Json -Depth 100
$gateSchemaPath = Join-Path (
    Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) (
    $isPhase56 ?
        'Profiles/Phase56GateResult.schema.json' :
        'Profiles/Phase54GateResult.schema.json')
if (-not ($json | Test-Json -SchemaFile $gateSchemaPath)) {
    throw 'Generated performance gate result does not match its phase schema.'
}
[IO.File]::WriteAllText(
    $resolvedOutput,
    $json + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))
if (-not [bool]$output.overall_pass) {
    throw "Performance gates failed closed: $(
        [string]::Join(', ', $failedGateNames))"
}
