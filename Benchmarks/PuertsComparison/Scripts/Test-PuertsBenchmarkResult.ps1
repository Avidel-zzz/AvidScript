[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ResultPath,

    [string]$ProfilePath = ''
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
. (Join-Path $ScriptRoot 'PuertsBenchmarkSidecar.Common.ps1')
if ([string]::IsNullOrWhiteSpace($ProfilePath)) {
    $ProfilePath = Join-Path $BenchmarkRoot 'Config/BenchmarkProfile.json'
}
$SchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkResult.schema.json'

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Code
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Code file is missing: $Path"
    }
    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    }
    catch {
        throw "$Code file is not valid JSON: $Path"
    }
}

$ResolvedResultPath = [System.IO.Path]::GetFullPath($ResultPath)
$ResolvedProfilePath = [System.IO.Path]::GetFullPath($ProfilePath)
$ResultRaw = Get-Content -LiteralPath $ResolvedResultPath -Raw -ErrorAction Stop
if (-not ($ResultRaw | Test-Json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue)) {
    throw 'ASP53R1000 benchmark result does not satisfy the tracked JSON schema'
}
$Result = $ResultRaw | ConvertFrom-Json
$Profile = Read-JsonFile $ResolvedProfilePath 'ASP53R1001'

$ExpectedLanes = @($Profile.lanes | ForEach-Object { [string]$_ })
$ExpectedWorkloads = @($Profile.workloads | ForEach-Object { [string]$_ })
$ExpectedProcessRuns = [int]$Profile.process_runs
$ExpectedSamples = [int]$Profile.timed_samples
if ([int]$Profile.schema_version -ne 2 -or
    $ExpectedLanes.Count -ne 5 -or
    $ExpectedWorkloads.Count -lt 1 -or
    $ExpectedProcessRuns -lt 1 -or
    $ExpectedSamples -lt 1) {
    throw 'ASP53R1002 benchmark profile does not define a valid canonical five-lane result matrix'
}
if (@($Result.lane_catalog).Count -ne $ExpectedLanes.Count -or
    [string]$Result.lane_catalog_sha256 -cne [string]$Result.provenance.lane_catalog_sha256 -or
    (Get-SidecarLaneCatalogSha256 -Catalog @($Result.lane_catalog)) -cne
        [string]$Result.lane_catalog_sha256) {
    throw 'ASP54R1012 result lane catalog/hash is incomplete or inconsistent'
}
$LaneIdentityById = @{}
for ($LaneIndex = 0; $LaneIndex -lt $ExpectedLanes.Count; ++$LaneIndex) {
    $ExpectedLane = $ExpectedLanes[$LaneIndex]
    $CatalogEntry = $Result.lane_catalog[$LaneIndex]
    if ([string]$CatalogEntry.lane_id -cne $ExpectedLane) {
        throw "ASP54R1012 result lane catalog order mismatch: lane=$ExpectedLane"
    }
    if ([string]$CatalogEntry.lane_identity_sha256 -cne
        (Get-SidecarLaneIdentitySha256 -Entry $CatalogEntry)) {
        throw "ASP54R1012 result lane identity hash mismatch: lane=$ExpectedLane"
    }
    $LaneIdentityById[$ExpectedLane] = [string]$CatalogEntry.lane_identity_sha256
}
$SemanticCatalog = @($Result.lane_catalog | Where-Object {
    [string]$_.lane_id -ceq 'avidscript_wasmtime_semantic'
})[0]
$DirectCatalog = @($Result.lane_catalog | Where-Object {
    [string]$_.lane_id -ceq 'avidscript_wasmtime_native_direct'
})[0]
if ($null -eq $SemanticCatalog -or
    $null -eq $DirectCatalog -or
    [string]$SemanticCatalog.backend_id -cne 'wasmtime.cranelift.jit' -or
    [string]$DirectCatalog.backend_id -cne 'wasmtime.cranelift.jit' -or
    [string]$SemanticCatalog.binding_invocation_mode -cne 'semantic_process_event' -or
    [string]$DirectCatalog.binding_invocation_mode -cne 'qualified_native_direct' -or
    [string]$SemanticCatalog.lane_identity_sha256 -ceq [string]$DirectCatalog.lane_identity_sha256) {
    throw 'ASP54R1012 Wasmtime lane mode identity is missing or aliased'
}

$ExpectedCount = $ExpectedLanes.Count *
    $ExpectedWorkloads.Count *
    $ExpectedProcessRuns *
    $ExpectedSamples
$Samples = @($Result.samples)
if ($Samples.Count -ne $ExpectedCount) {
    throw "ASP53R1003 result matrix size mismatch: actual=$($Samples.Count) expected=$ExpectedCount"
}

$SamplesByKey = @{}
foreach ($Sample in $Samples) {
    $Lane = [string]$Sample.lane
    $Workload = [string]$Sample.workload
    $ProcessRun = [int]$Sample.process_run
    $SampleIndex = [int]$Sample.sample_index
    if ($Lane -cnotin $ExpectedLanes) {
        throw "ASP53R1004 unknown lane: $Lane"
    }
    if ([string]$Sample.lane_identity_sha256 -cne [string]$LaneIdentityById[$Lane]) {
        throw "ASP54R1012 sample lane identity mismatch: lane=$Lane"
    }
    if ($Lane.StartsWith('avidscript_', [System.StringComparison]::Ordinal)) {
        $CatalogEntry = @($Result.lane_catalog | Where-Object { [string]$_.lane_id -ceq $Lane })[0]
        if ($null -eq $Sample.backend_info -or
            [bool]$Sample.backend_info.fallback_used -or
            [string]$Sample.backend_info.backend_id -cne [string]$CatalogEntry.backend_id -or
            [string]$Sample.backend_info.binding_invocation_mode -cne [string]$CatalogEntry.binding_invocation_mode -or
            [string]$Sample.backend_info.runtime_version -cne [string]$CatalogEntry.runtime_version -or
            [string]$Sample.backend_info.execution_mode -cne [string]$CatalogEntry.execution_mode -or
            [string]$Sample.backend_info.artifact_format -cne [string]$CatalogEntry.execution_artifact_format -or
            [string]$Sample.backend_info.artifact_sha256 -cne [string]$CatalogEntry.execution_artifact_sha256 -or
            [string]$Sample.backend_info.source_wasm_sha256 -cne [string]$CatalogEntry.source_wasm_sha256 -or
            [string]$Sample.backend_info.target_triple -cne [string]$CatalogEntry.target_triple -or
            [string]$Sample.backend_info.runtime_build_identity -cne [string]$CatalogEntry.runtime_build_identity -or
            [string]$Sample.backend_info.runtime_artifact_sha256 -cne [string]$CatalogEntry.runtime_artifact_sha256) {
            throw "ASP54R1013 AvidScript backend provenance mismatch: lane=$Lane"
        }
    }
    if ($Lane -ceq 'avidscript_wasmtime_native_direct' -and
        $Workload -cin @('scalar_add_int32', 'batch_scalar') -and
        ([int64]$Sample.direct_hit_count -ne [int64]$Sample.iterations -or
         [int64]$Sample.requested_direct_fallback_count -ne 0)) {
        throw "ASP54R1015 direct scalar evidence is invalid: workload=$Workload"
    }
    if ([int64]$Sample.checksum -ne [int64]$Sample.expected_checksum) {
        throw "ASP54R1014 lane-specific checksum oracle mismatch: lane=$Lane workload=$Workload"
    }
    if ($Workload -cnotin $ExpectedWorkloads) {
        throw "ASP53R1005 unknown workload: $Workload"
    }
    if ($ProcessRun -lt 0 -or $ProcessRun -ge $ExpectedProcessRuns) {
        throw "ASP53R1006 process run is outside the profile: $ProcessRun"
    }
    if ($SampleIndex -lt 0 -or $SampleIndex -ge $ExpectedSamples) {
        throw "ASP53R1007 sample index is outside the profile: $SampleIndex"
    }

    $Key = "$ProcessRun|$Workload|$SampleIndex|$Lane"
    if ($SamplesByKey.ContainsKey($Key)) {
        throw "ASP53R1008 duplicate sample: $Key"
    }
    $SamplesByKey[$Key] = $Sample
}

foreach ($ProcessRun in 0..($ExpectedProcessRuns - 1)) {
    foreach ($Workload in $ExpectedWorkloads) {
        foreach ($SampleIndex in 0..($ExpectedSamples - 1)) {
            $ComparableChecksums = @{}
            foreach ($Lane in $ExpectedLanes) {
                $Key = "$ProcessRun|$Workload|$SampleIndex|$Lane"
                if (-not $SamplesByKey.ContainsKey($Key)) {
                    throw "ASP53R1009 missing sample: $Key"
                }
                $Sample = $SamplesByKey[$Key]
                $ComparableKey = "$([int64]$Sample.seed)|$([int64]$Sample.iterations)"
                if ($ComparableChecksums.ContainsKey($ComparableKey) -and
                    [int64]$Sample.checksum -ne [int64]$ComparableChecksums[$ComparableKey]) {
                    throw "ASP53R1011 checksum mismatch across lanes: process=$ProcessRun workload=$Workload sample=$SampleIndex"
                }
                $ComparableChecksums[$ComparableKey] = [int64]$Sample.checksum
            }
        }
    }
}

[pscustomobject][ordered]@{
    succeeded = $true
    result_path = $ResolvedResultPath
    profile_id = [string]$Profile.profile_id
    lane_count = $ExpectedLanes.Count
    workload_count = $ExpectedWorkloads.Count
    process_run_count = $ExpectedProcessRuns
    timed_sample_count = $ExpectedSamples
    raw_sample_count = $Samples.Count
} | ConvertTo-Json -Depth 6
