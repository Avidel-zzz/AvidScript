[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'AngelScriptTiming.psm1') -Force
$Run = Get-Content -LiteralPath (Join-Path $RunDirectory 'run.json') -Raw | ConvertFrom-Json
if ($Run.status -cne 'passed' -or -not $Run.passed) { throw 'Use a completed real timing run as the mutation-test baseline.' }
$Inputs = @{}
foreach ($Mode in @('calibration', 'measure')) {
    $Job = @($Run.jobs | Where-Object { $_.suite -ceq 'micro' -and $_.mode -ceq $Mode })[0]
    $Inputs[$Mode] = @{
        request = Get-Content -LiteralPath (Join-Path $RunDirectory $Job.request) -Raw
        report = Get-Content -LiteralPath (Join-Path $RunDirectory $Job.result) -Raw
    }
    Assert-As38TimingReport -Report ($Inputs[$Mode].report | ConvertFrom-Json) -Request ($Inputs[$Mode].request | ConvertFrom-Json)
}
$Passed = 2
function Test-Rejection {
    param([string]$Name, [string]$Mode, [scriptblock]$Mutate)
    $Pair = [pscustomobject]@{ request = $Inputs[$Mode].request | ConvertFrom-Json; report = $Inputs[$Mode].report | ConvertFrom-Json }
    & $Mutate $Pair
    $Rejected = $false
    try { Assert-As38TimingReport -Report $Pair.report -Request $Pair.request } catch { $Rejected = $true }
    if (-not $Rejected) { throw "Accepted invalid timing data: $Name" }
    $script:Passed++
}
Test-Rejection 'missing sample' 'measure' { param($p) $p.report.samples = @($p.report.samples | Select-Object -Skip 1) }
Test-Rejection 'duplicate sample' 'measure' { param($p) $p.report.samples[1] = $p.report.samples[0] }
Test-Rejection 'changed paired execution order' 'measure' { param($p) $r = $p.report.samples[0]; $p.report.samples[0] = $p.report.samples[1]; $p.report.samples[1] = $r }
Test-Rejection 'changed seed' 'measure' { param($p) $p.report.samples[0].seed_u32++ }
Test-Rejection 'changed request identity' 'measure' { param($p) $p.report.request_id = [guid]::NewGuid().ToString('N') }
Test-Rejection 'wrong process index' 'measure' { param($p) $p.report.process_run = 5; $p.request.process_run = 5 }
Test-Rejection 'wrong frozen iteration count' 'measure' { param($p) $p.report.iteration_counts.callback_empty.native_cpp++; $p.request.iteration_counts.callback_empty.native_cpp++ }
Test-Rejection 'cycle count string' 'measure' { param($p) $p.report.samples[0].elapsed_cycles = '50000' }
Test-Rejection 'cycle time inconsistent' 'measure' { param($p) $p.report.samples[0].milliseconds *= 2 }
Test-Rejection 'checksum mismatch' 'measure' { param($p) $p.report.samples[0].actual_checksum_u32++ }
Test-Rejection 'fractional matching checksums' 'measure' { param($p) $p.report.samples[0].actual_checksum_u32 = 0.5; $p.report.samples[0].expected_checksum_u32 = 0.5 }
Test-Rejection 'scalar mismatch' 'measure' { param($p) $p.report.samples[0].actual_scalar++ }
Test-Rejection 'both native counter arrays wrong' 'measure' { param($p) $p.report.samples[0].actual_native_calls[0]++; $p.report.samples[0].expected_native_calls[0]++ }
Test-Rejection 'static native code mislabeled as VM' 'measure' { param($p) $p.report.static_jit_transpiled_code_loaded = $true }
Test-Rejection 'unknown lane' 'measure' { param($p) $p.report.samples[0].lane = 'different_lane' }
Test-Rejection 'pass flag string' 'measure' { param($p) $p.report.passed = 'true' }
Test-Rejection 'matched flag string' 'measure' { param($p) $p.report.samples[0].matched = 'true' }
Test-Rejection 'shortened warmup' 'measure' { param($p) $p.request.warmup_samples = 4; $p.report.warmup_samples = 4 }
Test-Rejection 'duplicate calibration sample' 'calibration' { param($p) $p.report.samples[1] = $p.report.samples[0] }
Test-Rejection 'missing confirmation' 'calibration' { param($p) $p.report.samples = @($p.report.samples | Where-Object { -not ($_.phase -ceq 'calibration_confirm' -and $_.lane -ceq 'native_cpp' -and $_.workload -ceq 'callback_empty') }) }
Test-Rejection 'wrong final calibrated count' 'calibration' { param($p) $p.report.iteration_counts.callback_empty.native_cpp++ }
Test-Rejection 'wrong calibration phase' 'calibration' { param($p) $p.report.samples[0].phase = 'warmup' }
Test-Rejection 'calibration seed string' 'calibration' { param($p) $p.report.samples[0].seed_u32 = [string]$p.report.samples[0].seed_u32 }
Test-Rejection 'calibration request with measured process id' 'calibration' { param($p) $p.request.process_run = 0; $p.report.process_run = 0 }
$Report = $Inputs.measure.report | ConvertFrom-Json
$Good = "AS38_TIMING_VALIDATED mode=$($Report.mode) suite=$($Report.suite) samples=$(@($Report.samples).Count)`r`nCommandlet UAs38LeadershipTimingCommandlet finished execution (result 0)`r`n"
foreach ($Text in @($Good, $Good.Replace("`r`n", "`n"))) { Assert-As38TimingLog $Text 0 $Report; $Passed++ }
foreach ($Case in @(@{ text = ''; exit = 0 }, @{ text = $Good; exit = 3 }, @{ text = $Good + $Good; exit = 0 },
    @{ text = $Good + 'LogTemp: Error: invalid'; exit = 0 }, @{ text = $Good.Replace('samples=700', 'samples=699'); exit = 0 },
    @{ text = $Good.Replace('result 0', 'result 1'); exit = 0 })) {
    $Rejected = $false
    try { Assert-As38TimingLog $Case.text $Case.exit $Report } catch { $Rejected = $true }
    if (-not $Rejected) { throw 'Accepted failed or incomplete timing log.' }
    $Passed++
}
Write-Output "AngelScript timing contracts: $Passed/34 passed"
if ($Passed -ne 34) { throw 'Unexpected timing contract count.' }
