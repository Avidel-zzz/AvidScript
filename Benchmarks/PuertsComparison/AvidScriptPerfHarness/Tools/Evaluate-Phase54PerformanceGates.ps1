[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProfilePath,

    [Parameter(Mandatory = $true)]
    [string[]]$ProcessResultPath,

    [string]$SupplementalEvidencePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

    $passed = $true
    if ($Threshold.PSObject.Properties.Name -contains 'maximum') {
        $passed = $passed -and ($Value.Value -le [double]$Threshold.maximum)
    }
    if ($Threshold.PSObject.Properties.Name -contains 'minimum') {
        $passed = $passed -and ($Value.Value -ge [double]$Threshold.minimum)
    }
    return [ordered]@{
        name = $Name
        status = $passed ? 'pass' : 'fail'
        pass = $passed
        value = $Value.Value
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

    $comparator = if (
        [double]$Reflection.p50_of_process_p50_ns_per_operation -le
        [double]$Static.p50_of_process_p50_ns_per_operation) {
        $Reflection
    }
    else {
        $Static
    }
    $candidateP50 =
        [double]$Candidate.p50_of_process_p50_ns_per_operation
    $candidateP95 =
        [double]$Candidate.p95_of_process_p50_ns_per_operation
    $candidateMad =
        [double]$Candidate.mad_of_process_p50_ns_per_operation
    $comparatorP50 =
        [double]$comparator.p50_of_process_p50_ns_per_operation
    $comparatorP95 =
        [double]$comparator.p95_of_process_p50_ns_per_operation
    $comparatorMad =
        [double]$comparator.mad_of_process_p50_ns_per_operation

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
            comparator_lane = [string]$comparator.lane
            p50_ratio = $p50Ratio
            p95_ratio = $p95Ratio
            maximum_ratio = $maximum
            p50_pass = $p50Pass
            p95_pass = $p95Pass
            candidate_p50_ns_per_operation = $candidateP50
            candidate_cross_process_p95_ns_per_operation = $candidateP95
            candidate_mad_ns_per_operation = $candidateMad
            comparator_p50_ns_per_operation = $comparatorP50
            comparator_cross_process_p95_ns_per_operation = $comparatorP95
            comparator_mad_ns_per_operation = $comparatorMad
            candidate_upper_2mad_ns_per_operation = $candidateUpper2Mad
            comparator_lower_2mad_ns_per_operation = $comparatorLower2Mad
            two_mad_separated = $twoMadSeparated
        }
    }
}

$profile = Read-JsonFile -Path $ProfilePath
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

$identityFields = @(
    'avidscript_commit',
    'avidscript_tree_sha',
    'profile_id',
    'profile_sha256',
    'lane_catalog_sha256'
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
            'generated_s1_hit_count',
            'generated_s1_fallback_count',
            'generated_s1_reject_count',
            'data_lane_command_count',
            'data_lane_crossing_count',
            'data_lane_rejected_buffer_count',
            'semantic_hit_count',
            'logical_operation_count',
            'checksum'
        )
        foreach ($counter in $requiredCounters) {
            if ($sample.PSObject.Properties.Name -notcontains $counter) {
                $validityErrors.Add(
                "missing $counter process=$($result.process_run) lane=$($sample.lane) workload=$($sample.workload)")
            }
        }
        $workloadContract =
            $profile.workload_contracts.PSObject.Properties[
                [string]$sample.workload
            ].Value
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
        if ($sample.lane -ceq 'avidscript_wasmtime_generated_s1' -and
            ([uint64]$sample.generated_s1_hit_count -ne $logical -or
             [uint64]$sample.generated_s1_fallback_count -ne 0 -or
             [uint64]$sample.generated_s1_reject_count -ne 0 -or
             [uint64]$sample.data_lane_command_count -ne 0 -or
             [uint64]$sample.data_lane_crossing_count -ne 0 -or
             [uint64]$sample.data_lane_rejected_buffer_count -ne 0)) {
            $validityErrors.Add(
                "generated S1 path mismatch process=$($result.process_run) workload=$($sample.workload)")
        }
        if ($sample.lane -ceq 'avidscript_wasmtime_semantic' -and
            [uint64]$sample.semantic_hit_count -ne $logical) {
            $validityErrors.Add(
                "semantic path mismatch process=$($result.process_run) workload=$($sample.workload)")
        }
        if ($sample.lane -ceq 'avidscript_wasmtime_data_oriented') {
            $propertyWrites =
                [uint64]$sample.iterations *
                [uint64]$workloadContract.property_write_operations_per_frame
            $expectedGeneratedHits = $logical - $propertyWrites
            $expectedCrossings = [uint64]($propertyWrites / 2)
            $commands = [uint64]$sample.data_lane_command_count
            $crossings = [uint64]$sample.data_lane_crossing_count
            if ([uint64]$sample.generated_s1_hit_count -ne $expectedGeneratedHits -or
                [uint64]$sample.generated_s1_fallback_count -ne 0 -or
                [uint64]$sample.generated_s1_reject_count -ne 0 -or
                $commands -ne $propertyWrites -or
                $crossings -ne $expectedCrossings -or
                [uint64]$sample.data_lane_rejected_buffer_count -ne 0 -or
                $commands -eq 0 -or
                ([double]$crossings / [double]$commands) -gt $crossingRatioLimit) {
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
    $processStatistics.Add([ordered]@{
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
}

$supplemental = $null
if (-not [string]::IsNullOrWhiteSpace($SupplementalEvidencePath)) {
    $supplemental = Read-JsonFile -Path $SupplementalEvidencePath
}
$supplementalCandidateMatch = $false
$supplementalReason = 'required supplemental evidence is missing'
$supplementalCommit = $null
$supplementalTree = $null
if ($null -ne $supplemental) {
    if ($supplemental.PSObject.Properties.Name -contains 'provenance' -and
        $null -ne $supplemental.provenance -and
        $supplemental.provenance.PSObject.Properties.Name -contains
            'avidscript_commit' -and
        $supplemental.provenance.PSObject.Properties.Name -contains
            'avidscript_tree_sha') {
        $supplementalCommit =
            [string]$supplemental.provenance.avidscript_commit
        $supplementalTree =
            [string]$supplemental.provenance.avidscript_tree_sha
        $supplementalCandidateMatch =
            $supplementalCommit -ceq [string]$provenance.avidscript_commit -and
            $supplementalTree -ceq [string]$provenance.avidscript_tree_sha
        $supplementalReason = $supplementalCandidateMatch ?
            'supplemental evidence candidate identity matches process evidence' :
            'supplemental evidence candidate commit/tree does not match process evidence'
    }
    else {
        $supplementalReason =
            'supplemental evidence has no candidate commit/tree provenance'
    }
}
$supplementalAudit = [ordered]@{
    provided = $null -ne $supplemental
    supplemental_candidate_match = $supplementalCandidateMatch
    process_avidscript_commit = [string]$provenance.avidscript_commit
    process_avidscript_tree_sha = [string]$provenance.avidscript_tree_sha
    supplemental_avidscript_commit = $supplementalCommit
    supplemental_avidscript_tree_sha = $supplementalTree
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
    'vector_vs_p54_5',
    'typed_empty_explained_ratio'
)
foreach ($name in $supplementalGateNames) {
    $value = $null
    if ($supplementalCandidateMatch -and
        $supplemental.PSObject.Properties.Name -contains 'gate_inputs' -and
        $supplemental.gate_inputs.PSObject.Properties.Name -contains $name) {
        $value = [Nullable[double]]([double]$supplemental.gate_inputs.$name)
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

$json = $output | ConvertTo-Json -Depth 100
[IO.File]::WriteAllText(
    $resolvedOutput,
    $json + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))
