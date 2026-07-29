Set-StrictMode -Version Latest

function Get-Phase56NearestRank {
    param(
        [Parameter(Mandatory = $true)]
        [double[]]$Values,

        [Parameter(Mandatory = $true)]
        [ValidateRange(0.0, 1.0)]
        [double]$Percentile
    )

    if ($Values.Count -lt 1) {
        throw 'ASP56E5601 percentile requires at least one value'
    }
    $sorted = @($Values | Sort-Object)
    $rank = [Math]::Ceiling($Percentile * $sorted.Count)
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, $rank - 1))
    return [double]$sorted[$index]
}

function Assert-PhysicalCostAggregateIdentity {
    param(
        [Parameter(Mandatory = $true)]
        $Aggregate,

        [Parameter(Mandatory = $true)]
        $ExpectedProfile,

        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[0-9a-f]{64}$')]
        [string]$ExpectedProfileSha256
    )

    $requiredFields = @(
        'profile_id',
        'evidence_class',
        'profile_sha256',
        'process_runs',
        'warmup_samples_per_stage_per_process',
        'timed_samples_per_stage_per_process'
    )
    foreach ($field in $requiredFields) {
        if ($Aggregate.PSObject.Properties.Name -cnotcontains $field) {
            throw "ASP56E5602 physical aggregate is missing identity field: $field"
        }
    }

    if ([string]$Aggregate.profile_id -cne [string]$ExpectedProfile.profile_id -or
        [string]$Aggregate.evidence_class -cne
            [string]$ExpectedProfile.evidence_class -or
        [string]$Aggregate.profile_sha256 -cne $ExpectedProfileSha256 -or
        [int]$Aggregate.process_runs -ne [int]$ExpectedProfile.process_runs -or
        [int]$Aggregate.warmup_samples_per_stage_per_process -ne
            [int]$ExpectedProfile.warmup_samples -or
        [int]$Aggregate.timed_samples_per_stage_per_process -ne
            [int]$ExpectedProfile.timed_samples) {
        throw 'ASP56E5603 physical aggregate profile identity differs'
    }
}

function Get-PairedRatioAggregate {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$ProcessCosts,

        [Parameter(Mandatory = $true)]
        [ValidateRange(1, 2147483647)]
        [int]$ExpectedProcessCount
    )

    if ($ProcessCosts.Count -ne $ExpectedProcessCount) {
        throw 'ASP56E5604 paired ratio process count differs'
    }
    $processRuns = @($ProcessCosts | ForEach-Object {
        [int]$_.process_run
    } | Sort-Object -Unique)
    if ($processRuns.Count -ne $ExpectedProcessCount) {
        throw 'ASP56E5605 paired ratio process identities are not unique'
    }
    [double[]]$ratios = @($ProcessCosts | ForEach-Object {
        [double]$ratio = $_.paired_ratio
        if ([double]::IsNaN($ratio) -or
            [double]::IsInfinity($ratio) -or
            $ratio -lt 0.0) {
            throw 'ASP56E5606 paired ratio must be finite and non-negative'
        }
        $ratio
    })

    return [ordered]@{
        process_count = $ExpectedProcessCount
        p50 = Get-Phase56NearestRank -Values $ratios -Percentile 0.50
        p95 = Get-Phase56NearestRank -Values $ratios -Percentile 0.95
    }
}

function New-FullCrossingReconstruction {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessRun,

        [Parameter(Mandatory = $true)]
        [string]$TargetStage,

        [Parameter(Mandatory = $true)]
        [double]$ObservedP50,

        [Parameter(Mandatory = $true)]
        [string]$BaselineStage,

        [Parameter(Mandatory = $true)]
        [double]$BaselineP50,

        [Parameter(Mandatory = $true)]
        [string[]]$IncrementPairIds,

        [Parameter(Mandatory = $true)]
        [double[]]$IncrementP50Values
    )

    if ($IncrementPairIds.Count -lt 1 -or
        $IncrementPairIds.Count -ne $IncrementP50Values.Count) {
        throw 'ASP56E5607 full reconstruction increment chain differs'
    }
    if ($ObservedP50 -lt 0.0 -or $BaselineP50 -lt 0.0) {
        throw 'ASP56E5608 full reconstruction timings must be non-negative'
    }

    [double]$reconstructed = $BaselineP50
    $increments = [Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $IncrementPairIds.Count; ++$index) {
        [double]$increment = $IncrementP50Values[$index]
        if ([double]::IsNaN($increment) -or
            [double]::IsInfinity($increment)) {
            throw 'ASP56E5609 full reconstruction increment must be finite'
        }
        $reconstructed += $increment
        $increments.Add([ordered]@{
            pair = $IncrementPairIds[$index]
            paired_delta_p50_ns_per_iteration = $increment
        })
    }

    [double]$absoluteError = [Math]::Abs($reconstructed - $ObservedP50)
    [double]$allowedError = [Math]::Max(
        5.0,
        [Math]::Abs($ObservedP50) * 0.10)
    [double]$normalizedBudgetRatio = $absoluteError / $allowedError

    return [pscustomobject][ordered]@{
        process_run = $ProcessRun
        target_stage = $TargetStage
        baseline_stage = $BaselineStage
        observed_p50_ns_per_iteration = $ObservedP50
        baseline_p50_ns_per_iteration = $BaselineP50
        increments = @($increments)
        reconstructed_p50_ns_per_iteration = $reconstructed
        absolute_error_ns = $absoluteError
        allowed_error_ns = $allowedError
        normalized_budget_ratio = $normalizedBudgetRatio
        within_budget = $normalizedBudgetRatio -le 1.0
    }
}

function Get-ExpectedFusedGeneratedHits {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Workload,

        [Parameter(Mandatory = $true)]
        [uint64]$Iterations,

        [Parameter(Mandatory = $true)]
        [pscustomobject]$WorkloadContract,

        [Parameter(Mandatory = $true)]
        [bool]$DataLane
    )

    if ($Workload -ceq 'scalar_add_int32') {
        return $Iterations
    }
    if ($Workload -ceq 'property_get_set') {
        return $Iterations * 2u
    }
    if ($Workload -in @('gameplay_frame_small', 'gameplay_frame_dense')) {
        [uint64]$scalarPropertyCount =
            $Iterations *
            [uint64]$WorkloadContract.logical_entities_per_frame *
            [uint64]$WorkloadContract.scalar_property_operations_per_entity
        if ($DataLane) {
            return $scalarPropertyCount -
                ($Iterations *
                    [uint64]$WorkloadContract.property_write_operations_per_frame)
        }
        return $scalarPropertyCount
    }
    return 0u
}

function Test-PerformanceGateOverallPass {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$ValidityValid,

        [Parameter(Mandatory = $true)]
        $Gates
    )

    if (-not $ValidityValid) {
        return $false
    }

    $gateValues = if ($Gates -is [Collections.IDictionary]) {
        @($Gates.Values)
    }
    else {
        @($Gates.PSObject.Properties | ForEach-Object { $_.Value })
    }
    if ($gateValues.Count -lt 1) {
        return $false
    }
    foreach ($gate in $gateValues) {
        if ([string]$gate.status -cne 'pass' -or -not [bool]$gate.pass) {
            return $false
        }
    }
    return $true
}
