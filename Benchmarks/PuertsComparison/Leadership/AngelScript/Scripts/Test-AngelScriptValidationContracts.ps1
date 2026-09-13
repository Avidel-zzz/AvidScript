[CmdletBinding()]
param([Parameter(Mandatory)][string]$CorrectnessReport)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'AngelScriptValidation.psm1') -Force
$Json = Get-Content -LiteralPath $CorrectnessReport -Raw
Assert-As36CorrectnessReport ($Json | ConvertFrom-Json)
$Passed = 1
function Test-RejectedReport {
    param([string]$Name, [scriptblock]$Mutate)
    $Report = $Json | ConvertFrom-Json
    & $Mutate $Report
    $Rejected = $false
    try { Assert-As36CorrectnessReport $Report } catch { $Rejected = $true }
    if (-not $Rejected) { throw "Accepted invalid report: $Name" }
    $script:Passed++
}
Test-RejectedReport 'missing case' { param($r) $r.cases = @($r.cases | Select-Object -Skip 1) }
Test-RejectedReport 'duplicate tuple' { param($r) $r.cases[1] = $r.cases[0] }
Test-RejectedReport 'wrong checksum' { param($r) $r.cases[0].actual_checksum_u32++ }
Test-RejectedReport 'fractional checksum even if oracle agrees' { param($r) $r.cases[0].actual_checksum_u32 = 0.5; $r.cases[0].expected_checksum_u32 = 0.5 }
Test-RejectedReport 'string seed' { param($r) $r.cases[0].seed_u32 = '0' }
Test-RejectedReport 'fractional workload' { param($r) $r.cases[0].workload_id = 0.1 }
Test-RejectedReport 'wrong scalar' { param($r) $r.cases[0].actual_scalar++ }
Test-RejectedReport 'both call counts wrong' { param($r) $r.cases[0].actual_native_calls[0] = 1; $r.cases[0].expected_native_calls[0] = 1 }
Test-RejectedReport 'short counters' { param($r) $r.cases[0].actual_native_calls = @(0) }
Test-RejectedReport 'string match flag' { param($r) $r.cases[0].matched = 'true' }
Test-RejectedReport 'failed report' { param($r) $r.passed = $false }
Test-RejectedReport 'wrong runtime mode' { param($r) $r.mode = 'UE packaged generated native code' }
Test-RejectedReport 'duplicate control' { param($r) $r.rejection_controls[1] = $r.rejection_controls[0] }
Test-RejectedReport 'accepted invalid call' { param($r) $r.rejection_controls[0].rejected = $false }
Test-RejectedReport 'string rejection flag' { param($r) $r.rejection_controls[0].rejected = 'true' }
Test-RejectedReport 'callback state failure' { param($r) $r.per_actor_callback_state_passed = $false }
Test-RejectedReport 'type precision failure' { param($r) $r.float64_ref_out_object_identity_passed = $false }
Test-RejectedReport 'error with passed flag' { param($r) $r.error = 'failed' }
$Good = "AS36_VALIDATED cases=264 controls=17 mode=editor_vm`r`nCommandlet UAs36LeadershipValidateCommandlet finished execution (result 0)`r`n"
foreach ($Text in @($Good, $Good.Replace("`r`n", "`n"))) { Assert-As36CompletionLog $Text 0; $Passed++ }
foreach ($Case in @(
    @{ text = ''; exit = 0 }, @{ text = $Good; exit = 3 }, @{ text = $Good + $Good; exit = 0 },
    @{ text = $Good + 'LogTemp: Error: bad'; exit = 0 },
    @{ text = $Good.Replace('cases=264', 'cases=263'); exit = 0 },
    @{ text = $Good.Replace('result 0', 'result 1'); exit = 0 }
)) {
    $Rejected = $false
    try { Assert-As36CompletionLog $Case.text $Case.exit } catch { $Rejected = $true }
    if (-not $Rejected) { throw 'Accepted an incomplete, failed or duplicated log.' }
    $Passed++
}
Write-Output "AngelScript validation contracts: $Passed/27 passed"
if ($Passed -ne 27) { throw 'Unexpected contract count.' }
