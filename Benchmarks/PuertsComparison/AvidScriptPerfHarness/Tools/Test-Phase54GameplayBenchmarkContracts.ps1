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

Assert-True $evaluatorText.Contains('p95_ratio') `
    'Gameplay Gate 必须输出跨进程 P95 ratio。'
Assert-True $evaluatorText.Contains('two_mad_separated') `
    'Gameplay Gate 必须输出 2xMAD 明确分离判定。'
Assert-True $evaluatorText.Contains('supplemental_candidate_match') `
    'Supplemental evidence 必须绑定 process candidate identity。'

Write-Output 'Phase54 gameplay benchmark contracts passed.'
