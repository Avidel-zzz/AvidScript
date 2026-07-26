[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ValidatorPath = Join-Path $ScriptRoot 'Test-PuertsBenchmarkResult.ps1'
$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('AvidScriptP53ResultTest-' + [Guid]::NewGuid().ToString('N'))

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "ASP53RT1000 $Message"
    }
}

function Write-Json {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $Json = (($Value | ConvertTo-Json -Depth 32) -replace "`r`n", "`n") + "`n"
    [System.IO.File]::WriteAllText($Path, $Json, [System.Text.UTF8Encoding]::new($false))
}

function Invoke-ValidatorFailure {
    param(
        [Parameter(Mandatory = $true)][string]$ResultPath,
        [Parameter(Mandatory = $true)][string]$ProfilePath,
        [Parameter(Mandatory = $true)][string]$ExpectedCode
    )

    $Output = & pwsh -NoProfile -File $ValidatorPath -ResultPath $ResultPath -ProfilePath $ProfilePath 2>&1
    Assert-True ($LASTEXITCODE -ne 0) "invalid result unexpectedly passed: $ExpectedCode"
    Assert-True (($Output -join "`n").Contains($ExpectedCode)) "invalid result did not report $ExpectedCode"
}

try {
    New-Item -ItemType Directory -Force -Path $FixtureRoot | Out-Null
    $ProfilePath = Join-Path $FixtureRoot 'profile.json'
    $ResultPath = Join-Path $FixtureRoot 'result.json'
    $Profile = [ordered]@{
        schema_version = 1
        profile_id = 'contract-fixture'
        process_runs = 1
        timed_samples = 1
        lanes = @('native_cpp', 'puerts_v8_reflection', 'puerts_v8_static', 'avidscript_wamr')
        workloads = @('scalar_noop', 'property_get_set')
    }
    Write-Json $ProfilePath $Profile

    $Samples = @()
    foreach ($Workload in $Profile.workloads) {
        foreach ($Lane in $Profile.lanes) {
            $Samples += [ordered]@{
                process_run = 0
                lane = $Lane
                workload = $Workload
                sample_index = 0
                iterations = 1024
                elapsed_cycles = 5000
                checksum = if ($Workload -ceq 'scalar_noop') { 17 } else { 23 }
                correct = $true
            }
        }
    }
    $Result = [ordered]@{
        schema_version = 1
        run_id = '00000000-0000-0000-0000-000000000001'
        provenance = [ordered]@{
            ue_version = '5.8.0'
            ue_build_id = 'fixture-build'
            target = 'Editor'
            configuration = 'Development'
            avidscript_commit = ('a' * 40)
            puerts_commit = ('b' * 40)
            puerts_backend_sha256 = ('c' * 64)
            profile_id = 'contract-fixture'
        }
        samples = $Samples
    }
    Write-Json $ResultPath $Result

    $ValidOutput = & pwsh -NoProfile -File $ValidatorPath -ResultPath $ResultPath -ProfilePath $ProfilePath
    Assert-True ($LASTEXITCODE -eq 0) 'complete four-lane result did not pass'
    $Valid = $ValidOutput | ConvertFrom-Json
    Assert-True ([int]$Valid.raw_sample_count -eq 8) 'valid fixture sample count mismatch'

    $Missing = $Result | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $Missing.samples = @($Missing.samples | Select-Object -Skip 1)
    $MissingPath = Join-Path $FixtureRoot 'missing.json'
    Write-Json $MissingPath $Missing
    Invoke-ValidatorFailure $MissingPath $ProfilePath 'ASP53R1003'

    $Duplicate = $Result | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $Duplicate.samples[1] = $Duplicate.samples[0]
    $DuplicatePath = Join-Path $FixtureRoot 'duplicate.json'
    Write-Json $DuplicatePath $Duplicate
    Invoke-ValidatorFailure $DuplicatePath $ProfilePath 'ASP53R1008'

    $Checksum = $Result | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $Checksum.samples[1].checksum = 99
    $ChecksumPath = Join-Path $FixtureRoot 'checksum.json'
    Write-Json $ChecksumPath $Checksum
    Invoke-ValidatorFailure $ChecksumPath $ProfilePath 'ASP53R1011'
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

Write-Output 'Puerts benchmark result contracts passed: complete=1 missing=1 duplicate=1 checksum=1'
