[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ResultPath,

    [int]$ExpectedTimedSamples = -1
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ControlledRoot = Split-Path -Parent $ScriptRoot
$SchemaPath = Join-Path $ControlledRoot 'Schema/ControlledRuntimeResult.schema.json'
$ResolvedResultPath = [System.IO.Path]::GetFullPath($ResultPath)

if (-not (Test-Path -LiteralPath $ResolvedResultPath -PathType Leaf)) {
    throw "ASP54R4101 controlled runtime result is missing: $ResolvedResultPath"
}
$ResultText = [System.IO.File]::ReadAllText($ResolvedResultPath)
if (-not ($ResultText | Test-Json -SchemaFile $SchemaPath)) {
    throw 'ASP54R4102 controlled runtime result does not match schema v1'
}
$Result = $ResultText | ConvertFrom-Json
$ExpectedLaneIds = @(
    'puerts_v8_wasm_jit',
    'avidscript_wasmtime_cranelift_jit',
    'avidscript_wamr_interpreter',
    'native_cpp_reference'
)
$ObservedLaneIds = @($Result.lane_identities | ForEach-Object { [string]$_.lane_id })
if ([string]::Join('|', $ObservedLaneIds) -cne [string]::Join('|', $ExpectedLaneIds)) {
    throw 'ASP54R4103 controlled runtime lane order or catalog differs from the frozen contract'
}
if ([string]$Result.lane_identities[0].runtime_id -cne 'v8.webassembly.tiered_jit' -or
    [string]$Result.lane_identities[0].compiler_identity -cne 'v8.webassembly.tiered_jit' -or
    ([string]$Result.lane_identities[0].compiler_identity).Contains('turbofan')) {
    throw 'ASP54R4104 V8 WebAssembly identity must be tiered JIT and must not claim TurboFan-only'
}
if ([string]$Result.lane_identities[1].runtime_id -cne 'wasmtime.cranelift.jit' -or
    [string]$Result.lane_identities[1].compiler_identity -cne 'cranelift') {
    throw 'ASP54R4105 Wasmtime identity must be wasmtime.cranelift.jit'
}
foreach ($Lane in @($Result.lane_identities)[0..2]) {
    if ([string]$Lane.source_wasm_sha256 -cne [string]$Result.kernel_wasm_sha256 -or
        [string]$Lane.execution_artifact_sha256 -cne [string]$Result.kernel_wasm_sha256) {
        throw "ASP54R4106 lane $($Lane.lane_id) did not execute the identical tracked WASM digest"
    }
    if ([bool]$Lane.fallback_used) {
        throw "ASP54R4107 lane $($Lane.lane_id) reported fallback"
    }
}
if ([bool]$Result.compile_in_timed_region -or
    [bool]$Result.instantiate_in_timed_region -or
    [bool]$Result.export_lookup_in_timed_region -or
    [bool]$Result.fallback_used) {
    throw 'ASP54R4108 timing boundary or no-fallback contract failed'
}
if ([int]$Result.correctness_failures -ne 0) {
    throw 'ASP54R4109 controlled runtime result contains correctness failures'
}

if ([string]$Result.mode -ceq 'calibration') {
    if (@($Result.samples).Count -ne 0) {
        throw 'ASP54R4110 calibration result must not contain timed samples'
    }
    foreach ($LaneId in $ExpectedLaneIds) {
        $Entry = $Result.calibration.PSObject.Properties[$LaneId]
        if ($null -eq $Entry -or
            [int]$Entry.Value.iterations -lt 1 -or
            [double]$Entry.Value.duration_ns -le 0) {
            throw "ASP54R4111 calibration is incomplete for lane $LaneId"
        }
    }
}
else {
    if ($ExpectedTimedSamples -lt 1) {
        throw 'ASP54R4112 timed result validation requires ExpectedTimedSamples'
    }
    $Samples = @($Result.samples)
    if ($Samples.Count -ne $ExpectedLaneIds.Count * $ExpectedTimedSamples) {
        throw "ASP54R4113 timed sample count mismatch: $($Samples.Count)"
    }
    foreach ($LaneId in $ExpectedLaneIds) {
        $LaneSamples = @($Samples | Where-Object { [string]$_.lane_id -ceq $LaneId })
        if ($LaneSamples.Count -ne $ExpectedTimedSamples) {
            throw "ASP54R4114 timed lane sample count mismatch: $LaneId"
        }
        for ($Index = 0; $Index -lt $ExpectedTimedSamples; ++$Index) {
            $Sample = $LaneSamples[$Index]
            if ([int]$Sample.sample_index -ne $Index -or
                -not [bool]$Sample.correct -or
                [int]$Sample.host_crossing_count -ne 1 -or
                [int64]$Sample.result -ne [int64]$Sample.expected) {
                throw "ASP54R4115 invalid timed sample lane=$LaneId index=$Index"
            }
        }
    }
    for ($Index = 0; $Index -lt $ExpectedTimedSamples; ++$Index) {
        $Seeds = @(
            $Samples |
                Where-Object { [int]$_.sample_index -eq $Index } |
                ForEach-Object { [int]$_.seed } |
                Select-Object -Unique
        )
        if ($Seeds.Count -ne 1) {
            throw "ASP54R4116 lanes did not receive the same runtime seed at sample $Index"
        }
    }
}

[pscustomobject]@{
    result = 'controlled_runtime_result_valid'
    mode = [string]$Result.mode
    pid = [int]$Result.pid
    sample_count = @($Result.samples).Count
    kernel_wasm_sha256 = [string]$Result.kernel_wasm_sha256
}
