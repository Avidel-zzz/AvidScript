[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

$toolsRoot = $PSScriptRoot
$evaluatorText = Get-Content -LiteralPath (
    Join-Path $toolsRoot 'Evaluate-Phase54PerformanceGates.ps1') -Raw
$invokeText = Get-Content -LiteralPath (
    Join-Path $toolsRoot 'Invoke-Phase54GameplayBenchmark.ps1') -Raw
$harnessRoot = Split-Path -Parent $toolsRoot
$comparisonRoot = Split-Path -Parent $harnessRoot
$runnerText = Get-Content -LiteralPath (
    Join-Path $harnessRoot 'Source\AvidScriptPerfHarness\Private\AvidScriptPerfRunner.cpp') -Raw
$profileRoot = Join-Path $comparisonRoot 'Profiles'
$requestTemplate = Get-Content -LiteralPath (
    Join-Path $profileRoot 'Phase54SixLaneRequest.template.json') -Raw |
    ConvertFrom-Json -Depth 100
$microFormal = Get-Content -LiteralPath (
    Join-Path $profileRoot 'Phase54Micro.formal.json') -Raw |
    ConvertFrom-Json -Depth 100
$microDiagnostic = Get-Content -LiteralPath (
    Join-Path $profileRoot 'Phase54Micro.diagnostic.json') -Raw |
    ConvertFrom-Json -Depth 100
$gameplayFormal = Get-Content -LiteralPath (
    Join-Path $profileRoot 'Phase54Gameplay.formal.json') -Raw |
    ConvertFrom-Json -Depth 100
$gameplayDiagnostic = Get-Content -LiteralPath (
    Join-Path $profileRoot 'Phase54Gameplay.diagnostic.json') -Raw |
    ConvertFrom-Json -Depth 100
$calibrationSchema = Get-Content -LiteralPath (
    Join-Path $profileRoot 'Phase54SixLaneCalibration.schema.json') -Raw |
    ConvertFrom-Json -Depth 100
$processSchema = Get-Content -LiteralPath (
    Join-Path $profileRoot 'Phase54SixLaneProcessResult.schema.json') -Raw |
    ConvertFrom-Json -Depth 100
$csharpProfileRoot = Join-Path $harnessRoot 'Content\CSharp'
$semanticCSharpProfile = Get-Content -LiteralPath (
    Join-Path $csharpProfileRoot 'AvidScriptPerfWorkload.semantic.csharp-profile.json') -Raw |
    ConvertFrom-Json -Depth 100
$generatedCSharpProfile = Get-Content -LiteralPath (
    Join-Path $csharpProfileRoot 'AvidScriptPerfWorkload.csharp-profile.json') -Raw |
    ConvertFrom-Json -Depth 100
$dataCSharpProfile = Get-Content -LiteralPath (
    Join-Path $csharpProfileRoot 'AvidScriptPerfWorkload.data-oriented.csharp-profile.json') -Raw |
    ConvertFrom-Json -Depth 100

Assert-True $evaluatorText.Contains('p95_ratio') `
    'Gameplay Gate 必须输出跨进程 P95 ratio。'
Assert-True $evaluatorText.Contains('two_mad_separated') `
    'Gameplay Gate 必须输出 2xMAD 明确分离判定。'
Assert-True $evaluatorText.Contains('supplemental_candidate_match') `
    'Supplemental evidence 必须绑定 process candidate identity。'
Assert-True ($evaluatorText.Contains('[string]$ControlledSuiteAggregatePath') -and
    $evaluatorText.Contains('[string[]]$MicroProcessResultPath') -and
    $evaluatorText.Contains('[string]$PhysicalCostAggregatePath') -and
    -not $evaluatorText.Contains('[string]$SupplementalEvidencePath') -and
    $evaluatorText.Contains('Get-Phase54MicroStatistics')) `
    '十项 supplemental Gate 必须直接从 suite、micro raw 与 physical raw 证据推导。'
Assert-True ($evaluatorText.Contains(
        '[double]$Candidate.p50_of_process_p95_ns_per_operation') -and
    $evaluatorText.Contains('$p95Comparator')) `
    'Gameplay P95 Gate 必须比较进程内 P95，并独立选择最快 Puerts P95 comparator。'
Assert-True (@($requestTemplate.lane_catalog).Count -eq 6) `
    'Phase54 必须提交确定性的六 lane request template。'
Assert-True (@($microFormal.workloads).Count -eq 10) `
    'Phase54 micro profile 必须保留十项正式 workload。'
Assert-True (@($microFormal.workload_contracts.PSObject.Properties).Count -eq 10) `
    'Phase54 micro profile 必须为十项 workload 提交显式计数合同。'
Assert-True ($semanticCSharpProfile.data_lane_fusion -ceq 'disabled' -and
    $generatedCSharpProfile.data_lane_fusion -ceq 'disabled' -and
    $dataCSharpProfile.data_lane_fusion -ceq 'enabled') `
    'Semantic、generated S1 与 data-oriented C# profile 必须显式锁定各自 data lane fusion 模式。'
Assert-True ($generatedCSharpProfile.binding_profile.package_name -ceq
    $dataCSharpProfile.binding_profile.package_name -and
    $generatedCSharpProfile.binding_profile.package_name.Length -le 40) `
    'Generated S1 与 data-oriented profile 必须共享适合 Windows 短路径工程的稳定 binding package identity。'
Assert-True ($invokeText.Contains('puerts_reflection_script_sha256') -and
    $invokeText.Contains('puerts_runtime_sha256') -and
    $invokeText.Contains('editor_executable_sha256') -and
    $invokeText.Contains('wasmtime_runtime_sha256')) `
    'Runner 编排必须从实物计算 JS、Puerts、Editor 与 Wasmtime 身份。'
Assert-True ($invokeText.Contains('Assert-SidecarBenchmarkProjectProvenance') -and
    $invokeText.Contains('Assert-SidecarPuertsProvenance') -and
    $invokeText.Contains('$harnessModulePath = Join-Path $harnessRoot') -and
    $invokeText.Contains('UnrealEditor-AvidScriptPerfHarness.dll') -and
    $invokeText.Contains('requires the tracked profile and request template bytes') -and
    $invokeText.Contains('wasmtime-v45.0.0;cranelift=1;dll_sha256=')) `
    'Formal 六通道必须锁定项目、Puerts、Harness DLL、profile/template 与真实 Wasmtime build identity。'
Assert-True ($invokeText.Contains('Get-SidecarLaneIdentitySha256') -and
    $invokeText.Contains('Get-SidecarLaneCatalogSha256')) `
    '实物身份变化后必须重新计算 canonical lane catalog。'
Assert-True ($runnerText.Contains('Puerts workload script identity mismatch') -and
    $runnerText.Contains('Puerts runtime artifact identity mismatch') -and
    $runnerText.Contains('Editor executable identity mismatch')) `
    'C++ runner 必须在加载或计时前复验实物身份。'
Assert-True $runnerText.Contains('bDataMicroInvalid') `
    'Data-oriented lane 必须为非 gameplay micro workload 提供明确零-buffer合同。'
Assert-True (
    [int]$microDiagnostic.calibration.maximum_iterations -ge 1000000 -and
    [int]$microFormal.calibration.maximum_iterations -ge
        [int]$microDiagnostic.calibration.maximum_iterations) `
    'Micro profile 必须允许 native empty callback 达到计时下限，且 formal 上限不得低于 diagnostic。'
Assert-True (
    [int]$gameplayFormal.calibration.maximum_iterations -ge 1048576 -and
    [int]$gameplayFormal.calibration.maximum_iterations -gt
        [int]$gameplayDiagnostic.calibration.maximum_iterations) `
    'Gameplay formal profile 必须允许 native small frame 达到 5 ms 计时下限。'
Assert-True (
    $runnerText.Contains('GetExpectedDataOrientedHostCallCount') -and
    $runnerText.Contains('Observation.HostImportCallCount != ExpectedHostImportCallCount')) `
    'Runner 必须对 data-oriented lane 的实际 Host crossing 做精确计数校验。'
Assert-True ($evaluatorText.Contains('function Get-ExpectedGeneratedHits') -and
    $evaluatorText.Contains('function Get-ExpectedSemanticHits') -and
    $evaluatorText.Contains('$expectedDataGeneratedHits') -and
    $evaluatorText.Contains('$expectedDataHostCalls') -and
    $evaluatorText.Contains('host import count mismatch') -and
    $evaluatorText.Contains('$isGameplay')) `
    '统一 Gate 必须复现 semantic、generated S1、data micro/gameplay 与 Host crossing 的不同计数合同。'
Assert-True ($evaluatorText.Contains('exact zero-based process_run sequence') -and
    $evaluatorText.Contains('share one non-empty run_id') -and
    $evaluatorText.Contains('distinct lowercase SHA-256 request identities') -and
    $evaluatorText.Contains('evaluated profile bytes')) `
    '统一 Gate 必须拒绝跨 run、重复 request、跳号进程与 profile bytes 不匹配。'
Assert-True (-not [regex]::IsMatch(
    $runnerText,
    'return\s+Workload\s*==\s*EAvidScriptPerfWorkload::PureInteger\s*\?\s*0')) `
    'pure_integer 必须按真实迭代数发布 logical operation count。'
Assert-True (@($calibrationSchema.required) -ccontains 'request_sha256' -and
    @($processSchema.required) -ccontains 'request_sha256' -and
    $runnerText.Contains('TEXT("request_sha256")') -and
    $invokeText.Contains('$attemptId = [guid]::NewGuid()') -and
    $invokeText.Contains('[string]$result.request_sha256 -cne $requestSha256')) `
    '六通道 calibration/timed result 必须共享 attempt 并绑定各自 request SHA-256。'

Write-Output 'Phase54 gameplay benchmark contracts passed: gates=3 identity=5 lanes=6 csharp_profiles=3 micro=10.'
