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

    [Parameter(Mandatory = $true)]
    [string]$PuertsCommit,

    [Parameter(Mandatory = $true)]
    [string]$PuertsBackendSha256,

    [string]$ProfilePath = '',

    [switch]$AllowNonFormalProfile,

    [string[]]$AdditionalEditorArguments = @()
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$controlledRoot = Split-Path -Parent $scriptRoot
$comparisonRoot = Split-Path -Parent $controlledRoot
. (Join-Path $comparisonRoot 'Scripts/PuertsBenchmarkSidecar.Common.ps1')

$canonicalProfilePath = Join-Path $controlledRoot (
    'Config/ControlledRuntimeSuiteProfile.json')
if ([string]::IsNullOrWhiteSpace($ProfilePath)) {
    $ProfilePath = $canonicalProfilePath
}
$resolvedProfilePath = [IO.Path]::GetFullPath($ProfilePath)
if (-not (Test-Path -LiteralPath $resolvedProfilePath -PathType Leaf)) {
    throw 'ASP54U4501 controlled runtime suite profile is missing'
}
if (-not $AllowNonFormalProfile -and
    -not [Linq.Enumerable]::SequenceEqual(
        [byte[]][IO.File]::ReadAllBytes($canonicalProfilePath),
        [byte[]][IO.File]::ReadAllBytes($resolvedProfilePath))) {
    throw 'ASP54U4502 formal suite requires the tracked profile bytes'
}

$profile = Get-Content -LiteralPath $resolvedProfilePath -Raw |
    ConvertFrom-Json -Depth 100
if ([string]$profile.benchmark_kind -cne 'identical_wasm_kernel_suite' -or
    @($profile.kernel_ids).Count -ne 12) {
    throw 'ASP54U4503 suite profile must authorize exactly twelve kernels'
}
$contractPath = Join-Path $controlledRoot ([string]$profile.suite_contract)
$contractSha256 = Get-SidecarFileSha256 -Path $contractPath
if ($contractSha256 -cne [string]$profile.suite_contract_sha256) {
    throw 'ASP54U4504 suite contract identity differs from profile'
}
$contract = Get-Content -LiteralPath $contractPath -Raw |
    ConvertFrom-Json -Depth 100
$contractIds = @($contract.kernels | ForEach-Object { [string]$_.kernel_id })
if ([string]::Join('|', @($profile.kernel_ids)) -cne
    [string]::Join('|', $contractIds)) {
    throw 'ASP54U4505 profile and contract kernel order differ'
}

$resolvedOutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (-not (Test-Path -LiteralPath $resolvedOutputRoot)) {
    New-Item -ItemType Directory -Path $resolvedOutputRoot | Out-Null
}
$suiteAttemptPath = Join-Path $resolvedOutputRoot (
    'suite-{0}-{1}' -f
        [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ'),
        [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suiteAttemptPath | Out-Null

function Get-GeometricMean {
    param([double[]]$Values)
    if ($Values.Count -lt 1 -or @($Values | Where-Object { $_ -le 0 }).Count -gt 0) {
        throw 'ASP54U4506 geometric mean requires positive observations'
    }
    [double]$logSum = 0.0
    foreach ($value in $Values) {
        $logSum += [Math]::Log($value)
    }
    return [Math]::Exp($logSum / $Values.Count)
}

$kernelResults = @()
foreach ($kernel in @($contract.kernels)) {
    $kernelId = [string]$kernel.kernel_id
    $kernelOutputRoot = Join-Path $suiteAttemptPath $kernelId
    $shootoutArguments = @{
        ProjectPath = $ProjectPath
        EditorExecutable = $EditorExecutable
        OutputRoot = $kernelOutputRoot
        AvidScriptCommit = $AvidScriptCommit
        AvidScriptTreeSha = $AvidScriptTreeSha
        AvidScriptDirty = $AvidScriptDirty
        PuertsCommit = $PuertsCommit
        PuertsBackendSha256 = $PuertsBackendSha256
        ProfilePath = $resolvedProfilePath
        KernelId = $kernelId
        AdditionalEditorArguments = $AdditionalEditorArguments
    }
    if ($AllowNonFormalProfile) {
        $shootoutArguments.AllowNonFormalProfile = $true
    }
    $run = & (Join-Path $scriptRoot 'Invoke-ControlledRuntimeShootout.ps1') `
        @shootoutArguments
    $aggregate = Get-Content -LiteralPath ([string]$run.aggregate_path) -Raw |
        ConvertFrom-Json -Depth 100
    if ([string]$aggregate.candidate.commit -cne $AvidScriptCommit -or
        [string]$aggregate.candidate.tree_sha -cne $AvidScriptTreeSha -or
        -not [bool]$aggregate.candidate.clean -or
        [int]$aggregate.correctness_failures -ne 0 -or
        [bool]$aggregate.fallback_used) {
        throw "ASP54U4507 invalid kernel provenance or correctness: $kernelId"
    }
    $v8Metric = @($aggregate.cross_process_metrics | Where-Object {
        [string]$_.lane_id -ceq 'puerts_v8_wasm_jit'
    })
    $wasmtimeMetric = @($aggregate.cross_process_metrics | Where-Object {
        [string]$_.lane_id -ceq 'avidscript_wasmtime_cranelift_jit'
    })
    if ($v8Metric.Count -ne 1 -or $wasmtimeMetric.Count -ne 1 -or
        [double]$v8Metric[0].process_mad_median_ns_per_iteration -lt 0) {
        throw "ASP54U4508 kernel lane statistics are incomplete: $kernelId"
    }
    [double]$p50Ratio =
        [double]$aggregate.paired_ratios.wasmtime_over_v8_cross_process_p50
    [double]$p95Ratio =
        [double]$aggregate.paired_ratios.wasmtime_over_v8_cross_process_p95
    [double]$v8Mad = [double]$v8Metric[0].process_mad_median_ns_per_iteration
    [double]$wasmtimeMad =
        [double]$wasmtimeMetric[0].process_mad_median_ns_per_iteration
    [double]$madRatio = if ($v8Mad -eq 0.0) {
        $wasmtimeMad -eq 0.0 ? 0.0 : [double]::PositiveInfinity
    }
    else {
        $wasmtimeMad / $v8Mad
    }
    if (-not [double]::IsFinite($p50Ratio) -or
        -not [double]::IsFinite($p95Ratio) -or
        -not [double]::IsFinite($madRatio)) {
        throw "ASP54U4509 kernel ratios must be finite: $kernelId"
    }
    $kernelResults += [ordered]@{
        kernel_id = $kernelId
        kernel_wasm_sha256 = [string]$kernel.wasm_sha256
        aggregate_sha256 = [string]$run.aggregate_sha256
        wasmtime_over_v8_p50 = $p50Ratio
        wasmtime_over_v8_p95 = $p95Ratio
        wasmtime_over_v8_mad = $madRatio
        p50_win = $p50Ratio -lt 1.0
        p95_win = $p95Ratio -lt 1.0
        correctness_failures = 0
        fallback_used = $false
    }
}

[double[]]$p50Ratios = @($kernelResults | ForEach-Object {
    [double]$_.wasmtime_over_v8_p50
})
[double[]]$p95Ratios = @($kernelResults | ForEach-Object {
    [double]$_.wasmtime_over_v8_p95
})
[double[]]$positiveMadRatios = @($kernelResults | ForEach-Object {
    [double]$_.wasmtime_over_v8_mad
} | Where-Object { $_ -gt 0 })
[double]$p50Geo = Get-GeometricMean -Values $p50Ratios
[double]$p95Geo = Get-GeometricMean -Values $p95Ratios
[double]$madGeo = $positiveMadRatios.Count -eq 0 ? 0.0 :
    (Get-GeometricMean -Values $positiveMadRatios)
[double]$p50WinRate = @($kernelResults | Where-Object { $_.p50_win }).Count /
    [double]$kernelResults.Count
[double]$p95WinRate = @($kernelResults | Where-Object { $_.p95_win }).Count /
    [double]$kernelResults.Count
[double]$maximumGeo = [double]$profile.pc_leadership_gate.maximum_geometric_mean_ratio
[double]$maximumMadGeo =
    [double]$profile.pc_leadership_gate.maximum_mad_geometric_mean_ratio
[double]$minimumWinRate = [double]$profile.pc_leadership_gate.minimum_kernel_win_rate
$passed = $p50Geo -le $maximumGeo -and
    $p95Geo -le $maximumGeo -and
    $madGeo -le $maximumMadGeo -and
    $p50WinRate -ge $minimumWinRate -and
    $p95WinRate -ge $minimumWinRate

$firstAggregatePath = Join-Path (
    Get-ChildItem -LiteralPath (Join-Path $suiteAttemptPath $contractIds[0]) `
        -Directory | Select-Object -First 1 -ExpandProperty FullName) 'aggregate.json'
$firstAggregate = Get-Content -LiteralPath $firstAggregatePath -Raw |
    ConvertFrom-Json -Depth 100
$suiteAggregate = [ordered]@{
    schema_version = 1
    benchmark_kind = 'identical_wasm_kernel_suite'
    profile_sha256 = Get-SidecarFileSha256 -Path $resolvedProfilePath
    suite_contract_sha256 = $contractSha256
    candidate = [ordered]@{
        commit = $AvidScriptCommit
        tree_sha = $AvidScriptTreeSha
        clean = $true
    }
    engine = [ordered]@{
        version = [string]$firstAggregate.engine.version
        build_id = [string]$firstAggregate.engine.build_id
        executable_sha256 = [string]$firstAggregate.engine.executable_sha256
    }
    kernel_count = $kernelResults.Count
    kernel_results = $kernelResults
    leadership = [ordered]@{
        maximum_geometric_mean_ratio = $maximumGeo
        maximum_mad_geometric_mean_ratio = $maximumMadGeo
        minimum_kernel_win_rate = $minimumWinRate
        p50_geometric_mean_ratio = $p50Geo
        p95_geometric_mean_ratio = $p95Geo
        mad_geometric_mean_ratio = $madGeo
        p50_kernel_win_rate = $p50WinRate
        p95_kernel_win_rate = $p95WinRate
        passed = $passed
    }
    correctness_failures = 0
    fallback_used = $false
}
$suiteJson = $suiteAggregate | ConvertTo-Json -Depth 100
$suiteSchemaPath = Join-Path $controlledRoot (
    'Schema/ControlledRuntimeSuiteAggregate.schema.json')
if (-not ($suiteJson | Test-Json -SchemaFile $suiteSchemaPath)) {
    throw 'ASP54U4510 generated suite aggregate does not match schema v1'
}
$aggregatePath = Join-Path $suiteAttemptPath 'suite.aggregate.json'
[IO.File]::WriteAllText(
    $aggregatePath,
    $suiteJson + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    result = 'controlled_runtime_suite_complete'
    aggregate_path = $aggregatePath
    aggregate_sha256 = Get-SidecarFileSha256 -Path $aggregatePath
    p50_geometric_mean_ratio = $p50Geo
    p95_geometric_mean_ratio = $p95Geo
    p50_kernel_win_rate = $p50WinRate
    p95_kernel_win_rate = $p95WinRate
    leadership_gate_passed = $passed
}
