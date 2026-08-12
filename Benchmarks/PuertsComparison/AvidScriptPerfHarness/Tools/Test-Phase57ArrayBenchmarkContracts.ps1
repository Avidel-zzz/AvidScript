[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$harnessRoot = Split-Path -Parent $PSScriptRoot
$comparisonRoot = Split-Path -Parent $harnessRoot
$profilesRoot = Join-Path $comparisonRoot 'Profiles'
$expectedSizes = @(1, 4, 16, 64, 256, 1024)
$expectedLanes = @(
    'puerts_v8_reflection_tarray',
    'avidscript_wasmtime_element',
    'avidscript_wasmtime_bulk',
    'avidscript_wasmtime_compiler_region'
)

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Read-JsonFile {
    param([string]$Path)
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -Depth 100
}

$profilePaths = @(
    (Join-Path $profilesRoot 'Phase57Array.diagnostic.json'),
    (Join-Path $profilesRoot 'Phase57Array.formal.json')
)
$schemaPaths = @(
    (Join-Path $profilesRoot 'Phase57ArrayProfile.schema.json'),
    (Join-Path $profilesRoot 'Phase57ArrayRequest.schema.json'),
    (Join-Path $profilesRoot 'Phase57ArrayResult.schema.json')
)
foreach ($schemaPath in $schemaPaths) {
    $schema = Read-JsonFile -Path $schemaPath
    Assert-True ([string]$schema.'$schema' -ceq 'https://json-schema.org/draft/2020-12/schema') `
        "Invalid JSON schema declaration: $schemaPath"
}
foreach ($profilePath in $profilePaths) {
    $profile = Read-JsonFile -Path $profilePath
    Assert-True ([string]$profile.contract -ceq 'phase57_array_profile.v2') `
        "Invalid profile contract: $profilePath"
    Assert-True (@($profile.sizes).Count -eq $expectedSizes.Count) `
        "Invalid size count: $profilePath"
    for ($index = 0; $index -lt $expectedSizes.Count; ++$index) {
        Assert-True ([int]$profile.sizes[$index] -eq $expectedSizes[$index]) `
            "Size order mismatch: $profilePath"
    }
    Assert-True (@($profile.lanes).Count -eq 4) "Lane count mismatch: $profilePath"
    $actualLaneIds = @($profile.lanes.lane_id | Sort-Object) -join ','
    $expectedLaneIds = @($expectedLanes | Sort-Object) -join ','
    Assert-True ($actualLaneIds -ceq $expectedLaneIds) "Lane identity mismatch: $profilePath"
    Assert-True ([string]$profile.internal_gate.classification -ceq 'diagnostic_only' -and
        [int]$profile.internal_gate.minimum_size -eq 64 -and
        [double]$profile.internal_gate.maximum -eq 0.80) `
        "Internal gate drift: $profilePath"
    Assert-True ([string]$profile.alternate_js_representations.arraybuffer -ceq 'diagnostic_only' -and
        [string]$profile.alternate_js_representations.typedarray -ceq 'diagnostic_only') `
        "Alternate JS representation classification drift: $profilePath"
}

$wat = Get-Content -LiteralPath (Join-Path $harnessRoot 'Content\Wasm\phase57_array_kernel.wat') -Raw
$javascript = Get-Content -LiteralPath (Join-Path $harnessRoot 'Content\JavaScript\phase57_array_reflection.js') -Raw
$csharp = Get-Content -LiteralPath (Join-Path $harnessRoot 'Content\CSharp\phase57_array_compiler_region.cs') -Raw
$compilerScript = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Invoke-Phase57ArrayBenchmark.ps1') -Raw
$runnerCpp = Get-Content -LiteralPath (Join-Path $harnessRoot 'Source\AvidScriptPerfHarness\Private\AvidScriptPerfArrayRunner.cpp') -Raw
$commandletCpp = Get-Content -LiteralPath (Join-Path $harnessRoot 'Source\AvidScriptPerfHarness\Private\AvidScriptPerfRunCommandlet.cpp') -Raw
foreach ($importName in @(
    'avid_value_array_load',
    'avid_value_array_store',
    'avid_value_array_read_range',
    'avid_value_array_write_range')) {
    Assert-True ($wat.Contains($importName)) "WAT kernel is missing import: $importName"
}
Assert-True ($javascript.Contains('ReflectInt32ArrayRoundtrip') -and
    $javascript.Contains('UE.NewArray(UE.BuiltinInt)') -and
    $javascript.Contains('values.Add(') -and
    $javascript.Contains('values.Num()') -and
    $javascript.Contains('values.Get(index)') -and
    -not $javascript.Contains('ArrayBuffer') -and
    -not $javascript.Contains('new Int32Array')) `
    'Puerts headline must use reflected UE TArray, not ArrayBuffer or TypedArray.'
Assert-True ($csharp.Contains('[AvidExport("phase57_array_run")]') -and
    $csharp.Contains('int[] values') -and
    $csharp.Contains('values[index]') -and
    -not $csharp.Contains('AvidScriptArray.Snapshot') -and
    -not $csharp.Contains('AvidScriptArray.Flush')) `
    'Compiler-managed headline must use ordinary C# array indexing.'
foreach ($compilerStage in @(
    'InvokeCSharpFrontend.ps1',
    'InvokeCSharpSemantic.ps1',
    'InvokeCSharpGuestCompiler.ps1')) {
    Assert-True ($compilerScript.Contains($compilerStage)) `
        "Benchmark compiler pipeline is missing stage: $compilerStage"
}
Assert-True ($runnerCpp.Contains('EAvidScriptVmBackendKind::Wasmtime') -and
    $runnerCpp.Contains('ReadRange') -and
    $runnerCpp.Contains('phase57_array_process_result.v2') -and
    $runnerCpp.Contains('avidscript_wasmtime_compiler_region')) `
    'Phase57Array C++ runner is missing the Wasmtime/correctness/result path.'
Assert-True ($commandletCpp.Contains('AvidScriptPerfArrayRequest=') -and
    $commandletCpp.Contains('FAvidScriptPerfArrayRunner::RunFromFiles')) `
    'Existing commandlet does not dispatch the Phase57Array request.'

$diagnosticProfilePath = $profilePaths[0]
$diagnosticProfile = Read-JsonFile -Path $diagnosticProfilePath
$profileSha256 = (Get-FileHash -LiteralPath $diagnosticProfilePath -Algorithm SHA256).Hash.ToLowerInvariant()
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("AvidScript-Phase57Array-{0}" -f [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($tempRoot) | Out-Null
try {
    $samples = @()
    foreach ($lane in $expectedLanes) {
        foreach ($size in $expectedSizes) {
            $logicalCalls = [int64]$diagnosticProfile.logical_calls_by_size.PSObject.Properties[[string]$size].Value
            $elements = $logicalCalls * $size * 2
            $crossings = if ($lane -ceq 'puerts_v8_reflection_tarray') {
                $logicalCalls
            } elseif ($lane -ceq 'avidscript_wasmtime_element') {
                $elements
            } elseif ($lane -ceq 'avidscript_wasmtime_bulk') {
                $logicalCalls * 2
            } else {
                4
            }
            $nsPerCall = if ($lane -ceq 'puerts_v8_reflection_tarray') {
                1200.0
            } elseif ($lane -ceq 'avidscript_wasmtime_element') {
                1000.0
            } elseif ($lane -ceq 'avidscript_wasmtime_compiler_region' -and $size -ge 64) {
                700.0
            } else {
                900.0
            }
            for ($sampleIndex = 0; $sampleIndex -lt [int]$diagnosticProfile.timed_samples; ++$sampleIndex) {
                $elapsed = $nsPerCall * $logicalCalls
                $samples += [ordered]@{
                    lane = $lane
                    size = $size
                    sample_index = $sampleIndex
                    logical_calls = $logicalCalls
                    elements = $elements
                    bytes = $elements * 4
                    host_transfer_bytes = if ($lane -ceq 'avidscript_wasmtime_compiler_region') {
                        $size * 2 * 4
                    } else {
                        $elements * 4
                    }
                    host_crossings = $crossings
                    elapsed_ns = $elapsed
                    ns_per_logical_call = $nsPerCall
                    ns_per_element = $elapsed / $elements
                    full_hash = 'fnv1a32:1234abcd'
                }
            }
        }
    }
    $processResult = [ordered]@{
        contract = 'phase57_array_process_result.v2'
        profile_id = [string]$diagnosticProfile.profile_id
        profile_sha256 = $profileSha256
        measurement_level = [string]$diagnosticProfile.measurement_level
        process_run = 1
        sizes = $expectedSizes
        samples = $samples
        provenance = [ordered]@{
            engine_version = 'contract-test'
            platform = 'contract-test'
            puerts_script_sha256 = '1' * 64
            puerts_runtime_sha256 = '2' * 64
            avidscript_wasm_sha256 = '3' * 64
            avidscript_compiler_wasm_sha256 = '4' * 64
            avidscript_compiler_source_sha256 = '5' * 64
            avidscript_compiler_reference_sha256 = '6' * 64
            avidscript_compiler_guest_ir_sha256 = '7' * 64
            avidscript_compiler_inspection_sha256 = '8' * 64
            avidscript_backend = 'wasmtime'
            avidscript_execution_mode = 'jit'
        }
    }
    $processPath = Join-Path $tempRoot 'process.json'
    $aggregatePath = Join-Path $tempRoot 'aggregate.json'
    [IO.File]::WriteAllText(
        $processPath,
        ($processResult | ConvertTo-Json -Depth 100),
        [Text.UTF8Encoding]::new($false))
    & (Join-Path $PSScriptRoot 'Evaluate-Phase57ArrayPerformance.ps1') `
        -ProfilePath $diagnosticProfilePath `
        -ProcessResultPaths $processPath `
        -ResultPath $aggregatePath `
        -RequireInternalGate | Out-Null
    $aggregate = Read-JsonFile -Path $aggregatePath
    Assert-True ([bool]$aggregate.correctness.passed) 'Synthetic correctness contract did not pass.'
    Assert-True ([bool]$aggregate.internal_gate.passed) 'Synthetic internal gate did not pass.'
    Assert-True (@($aggregate.statistics).Count -eq 24 -and @($aggregate.headline).Count -eq 6) `
        'Synthetic aggregate matrix is incomplete.'
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output 'Phase57Array benchmark contracts passed.'
