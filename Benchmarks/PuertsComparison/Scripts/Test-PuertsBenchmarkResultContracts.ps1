[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
$ValidatorPath = Join-Path $ScriptRoot 'Test-PuertsBenchmarkResult.ps1'
. (Join-Path $ScriptRoot 'PuertsBenchmarkSidecar.Common.ps1')
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
        'avidscript_wasmtime_semantic',
        'avidscript_wasmtime_native_direct'
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
        lane_identity_algorithm = 'canonical_json_utf8_sha256_v1'
        lane_catalog = @()
    }
    for ($LaneIndex = 0; $LaneIndex -lt $ExpectedLanes.Count; ++$LaneIndex) {
        $Lane = $ExpectedLanes[$LaneIndex]
        $IsAvidScript = $Lane.StartsWith('avidscript_', [System.StringComparison]::Ordinal)
        $CatalogEntry = [ordered]@{
            lane_id = $Lane
            runtime_id = if ($IsAvidScript) {
                'wasmtime'
            } else { $Lane }
            runtime_version = 'fixture-runtime'
            execution_tier = if ($IsAvidScript) { 'jit' } else { 'native_or_jit' }
            adapter_id = if ($IsAvidScript) { 'avidscript_runtime_session' } else { $Lane }
            wasm_workload_kind = if ($IsAvidScript) { 'csharp_frontend_output' } else { 'not_applicable' }
            source_wasm_sha256 = if ($IsAvidScript) { 'a' * 64 } else { $null }
            execution_artifact_format = if ($IsAvidScript) { 'wasm_bytecode' } else { 'native_or_javascript' }
            execution_artifact_sha256 = if ($IsAvidScript) { 'a' * 64 } else { 'b' * 64 }
            compiler_identity = 'fixture-compiler'
            compiler_flags = @('fixture')
            runtime_build_config = 'fixture-build'
            runtime_build_identity = 'fixture-runtime-build'
            runtime_artifact_sha256 = 'd' * 64
            target_triple = 'x86_64-pc-windows-msvc'
            cpu_feature_policy = 'host_default'
            backend_id = if ($IsAvidScript) {
                'wasmtime.cranelift.jit'
            } else { $null }
            binding_invocation_mode = if ($Lane -ceq 'avidscript_wasmtime_semantic') {
                'semantic_process_event'
            } elseif ($Lane -ceq 'avidscript_wasmtime_native_direct') {
                'qualified_native_direct'
            } else { $null }
            execution_mode = if ($IsAvidScript) { 'jit' } else { 'native_or_jit' }
            fallback_used = $false
        }
        $CatalogEntry['lane_identity_sha256'] =
            Get-SidecarLaneIdentitySha256 -Entry $CatalogEntry
        $Profile.lane_catalog += $CatalogEntry
    }
    Write-Json $ProfilePath $Profile

    $Samples = @()
    foreach ($Workload in $Profile.workloads) {
        $LaneIndex = 0
        foreach ($Lane in $Profile.lanes) {
            $Iterations = 1024 + $LaneIndex
            $ExpectedChecksum = if ($Workload -ceq 'scalar_noop') {
                17000 + $Iterations
            } else {
                23000 + $Iterations
            }
            $Samples += [ordered]@{
                process_run = 0
                lane = $Lane
                lane_identity_sha256 = [string]$Profile.lane_catalog[$LaneIndex].lane_identity_sha256
                workload = $Workload
                sample_index = 0
                seed = 4242
                iterations = $Iterations
                elapsed_cycles = 5000
                checksum = $ExpectedChecksum
                expected_checksum = $ExpectedChecksum
                direct_hit_count = 0
                requested_direct_fallback_count = if ($Lane -ceq 'avidscript_wasmtime_native_direct') {
                    $Iterations
                } else { 0 }
                correct = $true
            }
            if ($Lane.StartsWith('avidscript_', [System.StringComparison]::Ordinal)) {
                $Samples[-1]['backend_info'] = [ordered]@{
                    backend_id = [string]$Profile.lane_catalog[$LaneIndex].backend_id
                    binding_invocation_mode = [string]$Profile.lane_catalog[$LaneIndex].binding_invocation_mode
                    runtime_version = 'fixture-runtime'
                    execution_mode = [string]$Profile.lane_catalog[$LaneIndex].execution_mode
                    artifact_format = 'wasm_bytecode'
                    artifact_sha256 = 'a' * 64
                    source_wasm_sha256 = 'a' * 64
                    target_triple = 'x86_64-pc-windows-msvc'
                    runtime_build_identity = 'fixture-runtime-build'
                    runtime_artifact_sha256 = 'd' * 64
                    fallback_used = $false
                }
            }
            ++$LaneIndex
        }
    }
    $CatalogSha256 = Get-SidecarLaneCatalogSha256 -Catalog $Profile.lane_catalog
    $Result = [ordered]@{
        schema_version = 2
        run_id = '00000000-0000-0000-0000-000000000001'
        lane_catalog = $Profile.lane_catalog
        lane_catalog_sha256 = $CatalogSha256
        provenance = [ordered]@{
            ue_version = '5.8.0'
            ue_build_id = 'fixture-build'
            target = 'Editor'
            configuration = 'Development'
            avidscript_commit = ('a' * 40)
            puerts_commit = ('b' * 40)
            puerts_backend_sha256 = ('c' * 64)
            profile_id = 'contract-fixture'
            lane_catalog_sha256 = $CatalogSha256
        }
        samples = $Samples
    }
    Write-Json $ResultPath $Result

    $ValidOutput = & pwsh -NoProfile -File $ValidatorPath -ResultPath $ResultPath -ProfilePath $ProfilePath
    Assert-True ($LASTEXITCODE -eq 0) 'complete five-lane result did not pass'
    $Valid = $ValidOutput | ConvertFrom-Json
    Assert-True ([int]$Valid.raw_sample_count -eq 10) 'valid mixed-iteration fixture sample count mismatch'

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

    $WrongExpectedChecksum = $Result | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $WrongExpectedChecksum.samples[1].expected_checksum =
        [int64]$WrongExpectedChecksum.samples[1].checksum + 1
    $WrongExpectedChecksumPath = Join-Path $FixtureRoot 'wrong-expected-checksum.json'
    Write-Json $WrongExpectedChecksumPath $WrongExpectedChecksum
    Invoke-ValidatorFailure $WrongExpectedChecksumPath $ProfilePath 'ASP54R1014'

    $EqualIterationMismatch = $Result | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $EqualIterationMismatch.samples[1].iterations = $EqualIterationMismatch.samples[0].iterations
    $EqualIterationMismatch.samples[1].checksum = [int64]$EqualIterationMismatch.samples[0].checksum + 7
    $EqualIterationMismatch.samples[1].expected_checksum = $EqualIterationMismatch.samples[1].checksum
    $EqualIterationMismatchPath = Join-Path $FixtureRoot 'equal-iteration-mismatch.json'
    Write-Json $EqualIterationMismatchPath $EqualIterationMismatch
    Invoke-ValidatorFailure $EqualIterationMismatchPath $ProfilePath 'ASP53R1011'

    $ForgedHash = $Result | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $ForgedHash.lane_catalog_sha256 = 'f' * 64
    $ForgedHash.provenance.lane_catalog_sha256 = 'f' * 64
    $ForgedHashPath = Join-Path $FixtureRoot 'forged-catalog-hash.json'
    Write-Json $ForgedHashPath $ForgedHash
    Invoke-ValidatorFailure $ForgedHashPath $ProfilePath 'ASP54R1012'

    $OriginalEntry = $Profile.lane_catalog[0]
    $ReorderedEntry = [ordered]@{}
    $EntryNames = @($OriginalEntry.Keys)
    [array]::Reverse($EntryNames)
    foreach ($Name in $EntryNames) {
        if ($Name -cne 'lane_identity_sha256') {
            $ReorderedEntry[$Name] = $OriginalEntry[$Name]
        }
    }
    Assert-True (
        (Get-SidecarLaneIdentitySha256 -Entry $ReorderedEntry) -ceq
        [string]$OriginalEntry.lane_identity_sha256) `
        'lane identity changed when object properties were reordered'

    $OriginalCulture = [System.Globalization.CultureInfo]::CurrentCulture
    try {
        [System.Globalization.CultureInfo]::CurrentCulture =
            [System.Globalization.CultureInfo]::GetCultureInfo('fr-FR')
        Assert-True (
            (Get-SidecarLaneCatalogSha256 -Catalog $Profile.lane_catalog) -ceq
            $CatalogSha256) 'lane catalog hash changed under fr-FR culture'
    }
    finally {
        [System.Globalization.CultureInfo]::CurrentCulture = $OriginalCulture
    }

    $LfCatalogPath = Join-Path $FixtureRoot 'catalog-lf.json'
    $BomCatalogPath = Join-Path $FixtureRoot 'catalog-bom-crlf.json'
    $CatalogJson = $Profile.lane_catalog | ConvertTo-Json -Depth 32
    [System.IO.File]::WriteAllText(
        $LfCatalogPath,
        ($CatalogJson -replace "`r`n", "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText(
        $BomCatalogPath,
        ($CatalogJson -replace "(?<!`r)`n", "`r`n") + "`r`n",
        [System.Text.UTF8Encoding]::new($true))
    $LfCatalog = Get-Content -LiteralPath $LfCatalogPath -Raw | ConvertFrom-Json
    $BomCatalog = Get-Content -LiteralPath $BomCatalogPath -Raw | ConvertFrom-Json
    Assert-True (
        (Get-SidecarLaneCatalogSha256 -Catalog $LfCatalog) -ceq
        (Get-SidecarLaneCatalogSha256 -Catalog $BomCatalog)) `
        'lane catalog hash changed with BOM or newline serialization'

    $ResultSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkResult.schema.json'
    foreach ($Mutation in @('swapped', 'duplicate')) {
        $SchemaMutation = $Result | ConvertTo-Json -Depth 32 | ConvertFrom-Json
        if ($Mutation -ceq 'swapped') {
            $First = $SchemaMutation.lane_catalog[0]
            $SchemaMutation.lane_catalog[0] = $SchemaMutation.lane_catalog[1]
            $SchemaMutation.lane_catalog[1] = $First
        }
        else {
            $SchemaMutation.lane_catalog[1] = $SchemaMutation.lane_catalog[0]
        }
        $MutationJson = $SchemaMutation | ConvertTo-Json -Depth 32
        Assert-True (
            -not ($MutationJson | Test-Json -SchemaFile $ResultSchemaPath -ErrorAction SilentlyContinue)) `
            "BenchmarkResult schema accepted $Mutation lane_catalog"
    }

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

Write-Output 'Puerts benchmark result contracts passed: schema_v2=5 lanes=5 mixed_iterations=1 missing=1 duplicate=1 oracle=1 equal_iteration=1 canonical_hash=4 schema_order=2 identity=1 fallback=1 legacy=1'
