[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
$ValidatorPath = Join-Path $ScriptRoot 'Test-PuertsBenchmarkResult.ps1'
$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('AvidScriptP53ResultTest-' + [Guid]::NewGuid().ToString('N'))
$ProfileContractPath = Join-Path $BenchmarkRoot 'Config/BenchmarkProfile.json'
$SchemaPaths = @(
    (Join-Path $BenchmarkRoot 'Schema/BenchmarkResult.schema.json'),
    (Join-Path $BenchmarkRoot 'Schema/BenchmarkAggregate.schema.json'),
    (Join-Path $BenchmarkRoot 'Schema/BenchmarkProcessRequest.schema.json'),
    (Join-Path $BenchmarkRoot 'Schema/BenchmarkProcessResult.schema.json'),
    (Join-Path $BenchmarkRoot 'Schema/BenchmarkCalibration.schema.json')
)

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
    $ExpectedLanes = @(
        'native_cpp',
        'puerts_v8_reflection',
        'puerts_v8_static',
        'avidscript_wamr_interpreter',
        'avidscript_wasmtime_jit'
    )
    $TrackedProfile = Get-Content -LiteralPath $ProfileContractPath -Raw | ConvertFrom-Json
    Assert-True ([int]$TrackedProfile.schema_version -eq 2) 'tracked profile is not schema v2'
    Assert-True (@($TrackedProfile.lanes).Count -eq $ExpectedLanes.Count) 'tracked profile is not five-lane'
    Assert-True (@($TrackedProfile.lane_catalog).Count -eq $ExpectedLanes.Count) 'tracked profile does not lock lane_catalog'
    for ($Index = 0; $Index -lt $ExpectedLanes.Count; ++$Index) {
        Assert-True ([string]$TrackedProfile.lanes[$Index] -ceq $ExpectedLanes[$Index]) `
            "tracked profile lane mismatch at index $Index"
        Assert-True ([string]$TrackedProfile.lane_catalog[$Index].lane_id -ceq $ExpectedLanes[$Index]) `
            "tracked lane_catalog mismatch at index $Index"
    }
    foreach ($SchemaPath in $SchemaPaths) {
        $Schema = Get-Content -LiteralPath $SchemaPath -Raw | ConvertFrom-Json
        Assert-True ([int]$Schema.properties.schema_version.const -eq 2) `
            "schema is not v2: $([System.IO.Path]::GetFileName($SchemaPath))"
        Assert-True (@($Schema.required) -ccontains 'lane_catalog') `
            "schema does not require lane_catalog: $([System.IO.Path]::GetFileName($SchemaPath))"
        Assert-True (@($Schema.required) -ccontains 'lane_catalog_sha256') `
            "schema does not require lane_catalog_sha256: $([System.IO.Path]::GetFileName($SchemaPath))"
    }

    New-Item -ItemType Directory -Force -Path $FixtureRoot | Out-Null
    $ProfilePath = Join-Path $FixtureRoot 'profile.json'
    $ResultPath = Join-Path $FixtureRoot 'result.json'
    $Profile = [ordered]@{
        schema_version = 2
        profile_id = 'contract-fixture'
        process_runs = 1
        timed_samples = 1
        lanes = $ExpectedLanes
        workloads = @('scalar_noop', 'property_get_set')
        lane_catalog = @()
    }
    for ($LaneIndex = 0; $LaneIndex -lt $ExpectedLanes.Count; ++$LaneIndex) {
        $Lane = $ExpectedLanes[$LaneIndex]
        $IsAvidScript = $Lane.StartsWith('avidscript_', [System.StringComparison]::Ordinal)
        $Profile.lane_catalog += [ordered]@{
            lane_id = $Lane
            runtime_id = if ($IsAvidScript) {
                if ($Lane -ceq 'avidscript_wamr_interpreter') { 'wamr.interpreter' } else { 'wasmtime.cranelift.jit' }
            } else { $Lane }
            runtime_version = 'fixture-runtime'
            execution_tier = if ($Lane -ceq 'avidscript_wasmtime_jit') { 'jit' } elseif ($IsAvidScript) { 'interpreter' } else { 'native_or_jit' }
            adapter_id = if ($IsAvidScript) { 'avidscript_runtime_session' } else { $Lane }
            wasm_workload_kind = if ($IsAvidScript) { 'csharp_frontend_output' } else { 'not_applicable' }
            source_wasm_sha256 = if ($IsAvidScript) { 'a' * 64 } else { $null }
            execution_artifact_format = if ($IsAvidScript) { 'wasm_bytecode' } else { 'native_or_javascript' }
            execution_artifact_sha256 = if ($IsAvidScript) { 'a' * 64 } else { 'b' * 64 }
            compiler_identity = 'fixture-compiler'
            compiler_flags = @('fixture')
            runtime_build_config = 'fixture-build'
            runtime_build_identity = 'fixture-runtime-build'
            target_triple = 'x86_64-pc-windows-msvc'
            cpu_feature_policy = 'host_default'
            backend_id = if ($IsAvidScript) {
                if ($Lane -ceq 'avidscript_wamr_interpreter') { 'wamr.interpreter' } else { 'wasmtime.cranelift.jit' }
            } else { $null }
            execution_mode = if ($Lane -ceq 'avidscript_wasmtime_jit') { 'jit' } elseif ($IsAvidScript) { 'interpreter' } else { 'native_or_jit' }
            fallback_used = $false
            lane_identity_sha256 = ([char]([int][char]'a' + $LaneIndex)).ToString() * 64
        }
    }
    Write-Json $ProfilePath $Profile

    $Samples = @()
    foreach ($Workload in $Profile.workloads) {
        $LaneIndex = 0
        foreach ($Lane in $Profile.lanes) {
            $Samples += [ordered]@{
                process_run = 0
                lane = $Lane
                lane_identity_sha256 = [string]$Profile.lane_catalog[$LaneIndex].lane_identity_sha256
                workload = $Workload
                sample_index = 0
                iterations = 1024
                elapsed_cycles = 5000
                checksum = if ($Workload -ceq 'scalar_noop') { 17 } else { 23 }
                correct = $true
            }
            if ($Lane.StartsWith('avidscript_', [System.StringComparison]::Ordinal)) {
                $Samples[-1]['backend_info'] = [ordered]@{
                    backend_id = [string]$Profile.lane_catalog[$LaneIndex].backend_id
                    runtime_version = 'fixture-runtime'
                    execution_mode = [string]$Profile.lane_catalog[$LaneIndex].execution_mode
                    artifact_format = 'wasm_bytecode'
                    artifact_sha256 = 'a' * 64
                    source_wasm_sha256 = 'a' * 64
                    target_triple = 'x86_64-pc-windows-msvc'
                    runtime_build_identity = 'fixture-runtime-build'
                    fallback_used = $false
                }
            }
            ++$LaneIndex
        }
    }
    $Result = [ordered]@{
        schema_version = 2
        run_id = '00000000-0000-0000-0000-000000000001'
        lane_catalog = $Profile.lane_catalog
        lane_catalog_sha256 = 'f' * 64
        provenance = [ordered]@{
            ue_version = '5.8.0'
            ue_build_id = 'fixture-build'
            target = 'Editor'
            configuration = 'Development'
            avidscript_commit = ('a' * 40)
            puerts_commit = ('b' * 40)
            puerts_backend_sha256 = ('c' * 64)
            profile_id = 'contract-fixture'
            lane_catalog_sha256 = 'f' * 64
        }
        samples = $Samples
    }
    Write-Json $ResultPath $Result

    $ValidOutput = & pwsh -NoProfile -File $ValidatorPath -ResultPath $ResultPath -ProfilePath $ProfilePath
    Assert-True ($LASTEXITCODE -eq 0) 'complete five-lane result did not pass'
    $Valid = $ValidOutput | ConvertFrom-Json
    Assert-True ([int]$Valid.raw_sample_count -eq 10) 'valid fixture sample count mismatch'

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

    $LegacyLane = $Result | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $LegacyLane.samples[0].lane = 'avidscript_wamr'
    $LegacyLanePath = Join-Path $FixtureRoot 'legacy-lane.json'
    Write-Json $LegacyLanePath $LegacyLane
    Invoke-ValidatorFailure $LegacyLanePath $ProfilePath 'ASP53R1000'

    $WrongIdentity = $Result | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $WrongIdentity.samples[3].lane_identity_sha256 = '0' * 64
    $WrongIdentityPath = Join-Path $FixtureRoot 'wrong-identity.json'
    Write-Json $WrongIdentityPath $WrongIdentity
    Invoke-ValidatorFailure $WrongIdentityPath $ProfilePath 'ASP54R1012'

    $Fallback = $Result | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $Fallback.samples[4].backend_info.fallback_used = $true
    $FallbackPath = Join-Path $FixtureRoot 'fallback.json'
    Write-Json $FallbackPath $Fallback
    Invoke-ValidatorFailure $FallbackPath $ProfilePath 'ASP53R1000'
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

Write-Output 'Puerts benchmark result contracts passed: schema_v2=5 lanes=5 complete=1 missing=1 duplicate=1 checksum=1 identity=1 fallback=1 legacy=1'
