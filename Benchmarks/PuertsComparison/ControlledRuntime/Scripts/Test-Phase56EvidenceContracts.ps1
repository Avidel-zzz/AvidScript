[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$controlledRoot = Split-Path -Parent $PSScriptRoot
$comparisonRoot = Split-Path -Parent $controlledRoot
$commonPath = Join-Path $PSScriptRoot 'Phase56Evidence.Common.ps1'
$physicalOrchestratorPath = Join-Path $PSScriptRoot (
    'Invoke-PhysicalCostLadder.ps1')
$evaluatorPath = Join-Path $comparisonRoot (
    'AvidScriptPerfHarness/Tools/Evaluate-Phase54PerformanceGates.ps1')

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw "ASP56T5601 $Message"
    }
}

Assert-True (Test-Path -LiteralPath $commonPath -PathType Leaf) (
    'Phase56 evidence common contract is missing')
. $commonPath

$formalProfile = [pscustomobject]@{
    profile_id = 'phase56.physical-formal'
    evidence_class = 'formal'
    process_runs = 5
    warmup_samples = 5
    timed_samples = 30
}
$formalProfileSha256 = 'a' * 64
$formalAggregate = [pscustomobject]@{
    profile_id = $formalProfile.profile_id
    evidence_class = $formalProfile.evidence_class
    profile_sha256 = $formalProfileSha256
    process_runs = $formalProfile.process_runs
    warmup_samples_per_stage_per_process = $formalProfile.warmup_samples
    timed_samples_per_stage_per_process = $formalProfile.timed_samples
}
Assert-PhysicalCostAggregateIdentity `
    -Aggregate $formalAggregate `
    -ExpectedProfile $formalProfile `
    -ExpectedProfileSha256 $formalProfileSha256

$diagnosticAggregate = $formalAggregate.PSObject.Copy()
$diagnosticAggregate.profile_id = 'phase56.physical-diagnostic'
$diagnosticAggregate.evidence_class = 'diagnostic'
$diagnosticAggregate.process_runs = 1
$diagnosticAggregate.warmup_samples_per_stage_per_process = 1
$diagnosticAggregate.timed_samples_per_stage_per_process = 5
$diagnosticRejected = $false
try {
    Assert-PhysicalCostAggregateIdentity `
        -Aggregate $diagnosticAggregate `
        -ExpectedProfile $formalProfile `
        -ExpectedProfileSha256 $formalProfileSha256
}
catch {
    $diagnosticRejected = $true
}
Assert-True $diagnosticRejected (
    'formal evaluation must reject diagnostic physical evidence')

$pairedRatios = @(
    [pscustomobject]@{
        process_run = 0
        paired_ratio = 0.50
    },
    [pscustomobject]@{
        process_run = 1
        paired_ratio = 0.75
    },
    [pscustomobject]@{
        process_run = 2
        paired_ratio = 1.25
    }
)
$ratioMetric = Get-PairedRatioAggregate `
    -ProcessCosts $pairedRatios `
    -ExpectedProcessCount 3
Assert-True ([double]$ratioMetric.p50 -eq 0.75) (
    'prepared export ratio must aggregate same-process ratios by P50')
Assert-True ([double]$ratioMetric.p95 -eq 1.25) (
    'prepared export ratio must publish cross-process P95')

$genericReconstruction = New-FullCrossingReconstruction `
    -ProcessRun 0 `
    -TargetStage 'generic_empty_import' `
    -ObservedP50 131.0 `
    -BaselineStage 'guest_loop_baseline' `
    -BaselineP50 100.0 `
    -IncrementPairIds @(
        'typed_empty_minus_guest_loop',
        'generic_empty_minus_typed_empty') `
    -IncrementP50Values @(11.0, 20.0)
Assert-True ([double]$genericReconstruction.reconstructed_p50_ns_per_iteration -eq
    131.0) 'generic empty must be rebuilt from the full paired increment chain'
Assert-True ([Math]::Abs(
        [double]$genericReconstruction.allowed_error_ns - 13.1) -lt 0.000001) (
    'full reconstruction budget must be max(5ns, 10 percent)')
Assert-True ([double]$genericReconstruction.normalized_budget_ratio -eq 0.0 -and
    [bool]$genericReconstruction.within_budget) (
    'exact full reconstruction must pass its normalized budget')

$typedPairReconstruction = New-FullCrossingReconstruction `
    -ProcessRun 1 `
    -TargetStage 'typed_i32_pair_import' `
    -ObservedP50 100.0 `
    -BaselineStage 'guest_loop_baseline' `
    -BaselineP50 80.0 `
    -IncrementPairIds @(
        'typed_empty_minus_guest_loop',
        'typed_i32_pair_minus_typed_empty') `
    -IncrementP50Values @(10.0, 30.0)
Assert-True ([double]$typedPairReconstruction.normalized_budget_ratio -eq 2.0 -and
    -not [bool]$typedPairReconstruction.within_budget) (
    'full reconstruction exceeding max(5ns, 10 percent) must fail')

$passingGates = [ordered]@{
    first = [pscustomobject]@{ status = 'pass'; pass = $true }
    second = [pscustomobject]@{ status = 'pass'; pass = $true }
}
$failingGates = [ordered]@{
    first = [pscustomobject]@{ status = 'pass'; pass = $true }
    second = [pscustomobject]@{ status = 'fail'; pass = $false }
}
$unmeasuredGates = [ordered]@{
    first = [pscustomobject]@{ status = 'not_measured'; pass = $false }
}
Assert-True (Test-PerformanceGateOverallPass `
    -ValidityValid $true `
    -Gates $passingGates) 'valid evidence with all passing gates must pass overall'
Assert-True (-not (Test-PerformanceGateOverallPass `
    -ValidityValid $false `
    -Gates $passingGates)) 'invalid evidence must fail overall'
Assert-True (-not (Test-PerformanceGateOverallPass `
    -ValidityValid $true `
    -Gates $failingGates)) 'a failed gate must fail overall'
Assert-True (-not (Test-PerformanceGateOverallPass `
    -ValidityValid $true `
    -Gates $unmeasuredGates)) 'an unmeasured gate must fail overall'

$physicalOrchestratorText =
    Get-Content -LiteralPath $physicalOrchestratorPath -Raw
$evaluatorText = Get-Content -LiteralPath $evaluatorPath -Raw
Assert-True ($physicalOrchestratorText.Contains(
        'Get-PairedRatioAggregate') -and
    $physicalOrchestratorText.Contains(
        'New-FullCrossingReconstruction')) (
    'physical aggregate must use the tested paired ratio and full reconstruction helpers')
Assert-True ($evaluatorText.Contains(
        'Assert-PhysicalCostAggregateIdentity') -and
    $evaluatorText.Contains(
        'Test-PerformanceGateOverallPass')) (
    'evaluator must use the tested physical identity and overall helpers')
$writeIndex = $evaluatorText.LastIndexOf(
    '[IO.File]::WriteAllText(',
    [StringComparison]::Ordinal)
$failIndex = $evaluatorText.LastIndexOf(
    'Performance gates failed closed',
    [StringComparison]::Ordinal)
Assert-True ($writeIndex -ge 0 -and $failIndex -gt $writeIndex) (
    'evaluator must write the report before returning a fail-closed error')

[pscustomobject]@{
    result = 'phase56_evidence_contracts_passed'
    identity = 'formal_rejects_diagnostic'
    paired_ratio = 'same_process_then_cross_process'
    reconstruction = 'full_increment_chain_max_5ns_or_10pct'
    overall = 'invalid_fail_not_measured_fail_closed'
}
