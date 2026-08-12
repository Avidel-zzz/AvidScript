[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProfilePath,

    [Parameter(Mandatory = $true)]
    [string[]]$ProcessResultPaths,

    [Parameter(Mandatory = $true)]
    [string]$ResultPath,

    [switch]$RequireInternalGate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$expectedSizes = @(1, 4, 16, 64, 256, 1024)
$expectedLanes = @(
    'puerts_v8_reflection_tarray',
    'avidscript_wasmtime_element',
    'avidscript_wasmtime_bulk',
    'avidscript_wasmtime_compiler_region'
)

function Read-JsonFile {
    param([string]$Path)
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    return Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json -Depth 100
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-ExactSequence {
    param([object[]]$Actual, [object[]]$Expected, [string]$Label)
    Assert-True ($Actual.Count -eq $Expected.Count) "$Label count mismatch."
    for ($index = 0; $index -lt $Expected.Count; ++$index) {
        Assert-True ([string]$Actual[$index] -ceq [string]$Expected[$index]) `
            "$Label mismatch at index $index."
    }
}

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)
    $sorted = @($Values | Sort-Object)
    Assert-True ($sorted.Count -gt 0) 'Cannot calculate a percentile from no values.'
    $index = [Math]::Max(0, [Math]::Ceiling($Percentile * $sorted.Count) - 1)
    return [double]$sorted[$index]
}

function Write-NewJsonFile {
    param([object]$Value, [string]$Path)
    if (Test-Path -LiteralPath $Path) {
        throw "Refusing to overwrite Phase57Array result: $Path"
    }
    $parent = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    $json = $Value | ConvertTo-Json -Depth 100
    [IO.File]::WriteAllText(
        [IO.Path]::GetFullPath($Path),
        $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

$resolvedProfilePath = (Resolve-Path -LiteralPath $ProfilePath).Path
$profile = Read-JsonFile -Path $resolvedProfilePath
$profileSha256 = (Get-FileHash -LiteralPath $resolvedProfilePath -Algorithm SHA256).Hash.ToLowerInvariant()

Assert-True ([string]$profile.contract -ceq 'phase57_array_profile.v2') `
    'Phase57Array profile contract mismatch.'
Assert-ExactSequence @($profile.sizes) $expectedSizes 'profile sizes'
Assert-ExactSequence @($profile.headline.required_sizes) $expectedSizes 'headline sizes'
Assert-True ([string]$profile.internal_gate.classification -ceq 'diagnostic_only') `
    'The internal array gate must remain diagnostic_only.'
Assert-True ([int]$profile.internal_gate.minimum_size -eq 64 -and
    [double]$profile.internal_gate.maximum -eq 0.80) `
    'The internal array gate must remain N>=64 and <=0.80.'
Assert-True ([string]$profile.alternate_js_representations.arraybuffer -ceq 'diagnostic_only' -and
    [string]$profile.alternate_js_representations.typedarray -ceq 'diagnostic_only') `
    'ArrayBuffer and TypedArray must remain diagnostic_only.'
Assert-True ($ProcessResultPaths.Count -eq [int]$profile.process_runs) `
    'Process result count differs from the selected profile.'

$processResults = @()
$allSamples = @()
$seenRuns = [Collections.Generic.HashSet[int]]::new()
foreach ($path in $ProcessResultPaths) {
    $result = Read-JsonFile -Path $path
    Assert-True ([string]$result.contract -ceq 'phase57_array_process_result.v2') `
        "Process result contract mismatch: $path"
    Assert-True ([string]$result.profile_id -ceq [string]$profile.profile_id -and
        [string]$result.profile_sha256 -ceq $profileSha256 -and
        [string]$result.measurement_level -ceq [string]$profile.measurement_level) `
        "Process result profile identity mismatch: $path"
    Assert-ExactSequence @($result.sizes) $expectedSizes "process sizes: $path"
    Assert-True ($seenRuns.Add([int]$result.process_run)) `
        "Duplicate process_run: $($result.process_run)"
    Assert-True ([string]$result.provenance.avidscript_backend -ceq 'wasmtime') `
        'Phase57Array AvidScript results must come from Wasmtime.'
    Assert-True ([string]$result.provenance.avidscript_execution_mode -in @('interpreter', 'jit')) `
        'Phase57Array execution mode is invalid.'
    foreach ($digestName in @(
        'puerts_script_sha256',
        'puerts_runtime_sha256',
        'avidscript_wasm_sha256',
        'avidscript_compiler_wasm_sha256',
        'avidscript_compiler_source_sha256',
        'avidscript_compiler_reference_sha256',
        'avidscript_compiler_guest_ir_sha256',
        'avidscript_compiler_inspection_sha256')) {
        Assert-True ([string]$result.provenance.$digestName -cmatch '^[0-9a-f]{64}$') `
            "Invalid provenance digest: $digestName"
    }

    foreach ($sample in @($result.samples)) {
        $lane = [string]$sample.lane
        $size = [int]$sample.size
        $logicalCalls = [int64]$sample.logical_calls
        $expectedCalls = [int64]$profile.logical_calls_by_size.PSObject.Properties[[string]$size].Value
        Assert-True ($lane -in $expectedLanes -and $size -in $expectedSizes) `
            'Process sample lane or size is outside the frozen matrix.'
        Assert-True ($logicalCalls -eq $expectedCalls) `
            "logical_calls mismatch lane=$lane N=$size"
        $expectedElements = $logicalCalls * $size * 2
        $expectedCrossings = if ($lane -ceq 'puerts_v8_reflection_tarray') {
            $logicalCalls
        } elseif ($lane -ceq 'avidscript_wasmtime_element') {
            $expectedElements
        } elseif ($lane -ceq 'avidscript_wasmtime_bulk') {
            $logicalCalls * 2
        } else {
            4
        }
        Assert-True ([int64]$sample.elements -eq $expectedElements -and
            [int64]$sample.bytes -eq $expectedElements * 4 -and
            [int64]$sample.host_crossings -eq $expectedCrossings) `
            "Counter contract mismatch lane=$lane N=$size"
        $expectedTransferBytes = if ($lane -ceq 'avidscript_wasmtime_compiler_region') {
            $size * 2 * 4
        } else {
            $expectedElements * 4
        }
        Assert-True ([int64]$sample.host_transfer_bytes -eq $expectedTransferBytes) `
            "Host transfer byte mismatch lane=$lane N=$size"
        Assert-True ([double]$sample.elapsed_ns -gt 0 -and
            [double]$sample.ns_per_logical_call -gt 0 -and
            [double]$sample.ns_per_element -gt 0) `
            "Non-positive timing lane=$lane N=$size"
        Assert-True ([string]$sample.full_hash -cmatch '^fnv1a32:[0-9a-f]{8}$') `
            "Invalid full hash lane=$lane N=$size"
        $allSamples += [pscustomobject]@{
            process_run = [int]$result.process_run
            lane = $lane
            size = $size
            sample_index = [int]$sample.sample_index
            logical_calls = $logicalCalls
            elements = [int64]$sample.elements
            bytes = [int64]$sample.bytes
            host_transfer_bytes = [int64]$sample.host_transfer_bytes
            host_crossings = [int64]$sample.host_crossings
            elapsed_ns = [double]$sample.elapsed_ns
            ns_per_logical_call = [double]$sample.ns_per_logical_call
            ns_per_element = [double]$sample.ns_per_element
            full_hash = [string]$sample.full_hash
        }
    }
    $processResults += $result
}

$expectedSampleCount = [int]$profile.process_runs * [int]$profile.timed_samples *
    $expectedLanes.Count * $expectedSizes.Count
Assert-True ($allSamples.Count -eq $expectedSampleCount) `
    "Sample count mismatch: expected=$expectedSampleCount actual=$($allSamples.Count)"

$statistics = @()
$mismatches = @()
foreach ($size in $expectedSizes) {
    $hashes = @($allSamples | Where-Object { $_.size -eq $size } |
        Select-Object -ExpandProperty full_hash -Unique)
    if ($hashes.Count -ne 1) {
        $mismatches += "N=$size hashes=$($hashes -join ',')"
    }
    foreach ($lane in $expectedLanes) {
        $rows = @($allSamples | Where-Object {
            $_.lane -ceq $lane -and $_.size -eq $size
        })
        $expectedRows = [int]$profile.process_runs * [int]$profile.timed_samples
        Assert-True ($rows.Count -eq $expectedRows) `
            "Incomplete sample group lane=$lane N=$size"
        $statistics += [pscustomobject]@{
            lane = $lane
            size = $size
            logical_calls = [int64]$rows[0].logical_calls
            elements = [int64]$rows[0].elements
            bytes = [int64]$rows[0].bytes
            host_transfer_bytes = [int64]$rows[0].host_transfer_bytes
            host_crossings = [int64]$rows[0].host_crossings
            p50_ns_per_logical_call = Get-Percentile @($rows.ns_per_logical_call) 0.50
            p95_ns_per_logical_call = Get-Percentile @($rows.ns_per_logical_call) 0.95
            p50_ns_per_element = Get-Percentile @($rows.ns_per_element) 0.50
            full_hash = [string]$rows[0].full_hash
        }
    }
}

$headline = @()
$gateRows = @()
foreach ($size in $expectedSizes) {
    $puerts = $statistics | Where-Object {
        $_.lane -ceq 'puerts_v8_reflection_tarray' -and $_.size -eq $size
    }
    $element = $statistics | Where-Object {
        $_.lane -ceq 'avidscript_wasmtime_element' -and $_.size -eq $size
    }
    $compilerRegion = $statistics | Where-Object {
        $_.lane -ceq 'avidscript_wasmtime_compiler_region' -and $_.size -eq $size
    }
    $headline += [pscustomobject]@{
        size = $size
        avidscript_compiler_region_over_puerts_reflection =
            [double]$compilerRegion.p50_ns_per_logical_call / [double]$puerts.p50_ns_per_logical_call
    }
    if ($size -ge 64) {
        $ratio = [double]$compilerRegion.p50_ns_per_logical_call /
            [double]$element.p50_ns_per_logical_call
        $gateRows += [pscustomobject]@{
            size = $size
            compiler_region_over_element = $ratio
            passed = $ratio -le [double]$profile.internal_gate.maximum
        }
    }
}
$gatePassed = @($gateRows | Where-Object { -not $_.passed }).Count -eq 0
$correctnessPassed = $mismatches.Count -eq 0

$aggregate = [ordered]@{
    contract = 'phase57_array_result.v2'
    profile_id = [string]$profile.profile_id
    profile_sha256 = $profileSha256
    measurement_level = [string]$profile.measurement_level
    sizes = $expectedSizes
    process_results = $processResults
    statistics = $statistics
    headline = $headline
    internal_gate = [ordered]@{
        classification = 'diagnostic_only'
        minimum_size = 64
        maximum = 0.80
        passed = $gatePassed
        rows = $gateRows
    }
    correctness = [ordered]@{
        passed = $correctnessPassed
        full_hash_algorithm = 'fnv1a32_all_final_elements_v1'
        mismatches = $mismatches
    }
}
Write-NewJsonFile -Value $aggregate -Path $ResultPath

if (-not $correctnessPassed) {
    throw "Phase57Array full-hash correctness failed: $($mismatches -join '; ')"
}
if ($RequireInternalGate -and -not $gatePassed) {
    throw 'Phase57Array internal compiler-region/element diagnostic gate failed.'
}

[pscustomobject]@{
    result_path = [IO.Path]::GetFullPath($ResultPath)
    profile_id = [string]$profile.profile_id
    correctness_passed = $correctnessPassed
    internal_gate_passed = $gatePassed
}
