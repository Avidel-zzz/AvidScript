[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ResultPath,

    [string]$ProfilePath = ''
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
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
if ($ExpectedLanes.Count -ne 4 -or
    $ExpectedWorkloads.Count -lt 1 -or
    $ExpectedProcessRuns -lt 1 -or
    $ExpectedSamples -lt 1) {
    throw 'ASP53R1002 benchmark profile does not define a valid four-lane result matrix'
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
            $ReferenceIterations = $null
            $ReferenceChecksum = $null
            foreach ($Lane in $ExpectedLanes) {
                $Key = "$ProcessRun|$Workload|$SampleIndex|$Lane"
                if (-not $SamplesByKey.ContainsKey($Key)) {
                    throw "ASP53R1009 missing sample: $Key"
                }
                $Sample = $SamplesByKey[$Key]
                if ($null -eq $ReferenceIterations) {
                    $ReferenceIterations = [int64]$Sample.iterations
                    $ReferenceChecksum = [int64]$Sample.checksum
                }
                elseif ([int64]$Sample.iterations -ne $ReferenceIterations) {
                    throw "ASP53R1010 iteration mismatch across lanes: process=$ProcessRun workload=$Workload sample=$SampleIndex"
                }
                elseif ([int64]$Sample.checksum -ne $ReferenceChecksum) {
                    throw "ASP53R1011 checksum mismatch across lanes: process=$ProcessRun workload=$Workload sample=$SampleIndex"
                }
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
