[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ResultPath,

    [Parameter(Mandatory = $true)]
    [string]$RequestPath,

    [Parameter(Mandatory = $true)]
    [string]$ProfilePath,

    [string]$CalibrationResultPath = ''
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ControlledRoot = Split-Path -Parent $ScriptRoot
$PuertsComparisonRoot = Split-Path -Parent $ControlledRoot
. (Join-Path $PuertsComparisonRoot 'Scripts/PuertsBenchmarkSidecar.Common.ps1')
$SchemaPath = Join-Path $ControlledRoot 'Schema/ControlledRuntimeResult.schema.json'
$RequestSchemaPath = Join-Path $ControlledRoot 'Schema/ControlledRuntimeRequest.schema.json'
$ResolvedResultPath = [System.IO.Path]::GetFullPath($ResultPath)
$ResolvedRequestPath = [System.IO.Path]::GetFullPath($RequestPath)
$ResolvedProfilePath = [System.IO.Path]::GetFullPath($ProfilePath)

foreach ($RequiredPath in @(
    $ResolvedResultPath,
    $ResolvedRequestPath,
    $ResolvedProfilePath
)) {
    if (-not (Test-Path -LiteralPath $RequiredPath -PathType Leaf)) {
        throw "ASP54R4101 controlled runtime evidence is missing: $RequiredPath"
    }
}

$ResultText = [System.IO.File]::ReadAllText($ResolvedResultPath)
$RequestText = [System.IO.File]::ReadAllText($ResolvedRequestPath)
if (-not ($ResultText | Test-Json -SchemaFile $SchemaPath)) {
    throw 'ASP54R4102 controlled runtime result does not match schema v1'
}
if (-not ($RequestText | Test-Json -SchemaFile $RequestSchemaPath)) {
    throw 'ASP54R4103 controlled runtime request does not match schema v1'
}
$Result = $ResultText | ConvertFrom-Json
$Request = $RequestText | ConvertFrom-Json
$Profile = Get-Content -LiteralPath $ResolvedProfilePath -Raw | ConvertFrom-Json
$RequestSha256 = Get-SidecarFileSha256 -Path $ResolvedRequestPath
$ProfileSha256 = Get-SidecarFileSha256 -Path $ResolvedProfilePath

$ExpectedTimedSamples = if ([string]$Request.mode -ceq 'calibration') {
    0
}
else {
    [int]$Profile.timed_samples
}
$ExpectedProcessRun = [int]$Request.process_run
$ExpectedLaneCatalog = [string]::Join('|', @($Profile.lanes))
if ([string]$Request.benchmark_kind -cne
        [string]$Profile.benchmark_kind -or
    [int]$Request.seed -ne [int]$Profile.seed -or
    [int]$Request.warmup_samples -ne [int]$Profile.warmup_samples -or
    [int]$Request.timed_samples -ne $ExpectedTimedSamples -or
    [double]$Request.minimum_sample_milliseconds -ne
        [double]$Profile.minimum_sample_milliseconds -or
    [int]$Request.minimum_iterations -ne
        [int]$Profile.minimum_iterations -or
    [int]$Request.maximum_iterations -ne
        [int]$Profile.maximum_iterations -or
    [int]$Request.calibration_confirmation_samples -ne
        [int]$Profile.calibration_confirmation_samples -or
    [string]$Request.target_triple -cne
        [string]$Profile.target_triple -or
    [string]$Request.kernel_wasm_sha256 -cne
        [string]$Profile.kernel_wasm_sha256 -or
    [string]$Request.lane_schedule_id -cne
        [string]$Profile.lane_schedule_id -or
    [string]::Join('|', @($Request.lanes)) -cne $ExpectedLaneCatalog) {
    throw 'ASP54R4133 request workload contract differs from tracked profile'
}
if ([string]$Request.mode -ceq 'calibration') {
    if ($ExpectedProcessRun -ne -1) {
        throw 'ASP54R4134 calibration process_run must be -1'
    }
}
elseif ($ExpectedProcessRun -lt 0 -or
    $ExpectedProcessRun -ge [int]$Profile.process_runs) {
    throw 'ASP54R4135 timed process_run is outside the tracked profile range'
}

$BoundFields = @(
    'attempt_id',
    'profile_sha256',
    'calibration_sha256',
    'candidate_commit',
    'candidate_tree_sha',
    'candidate_clean',
    'engine_version',
    'engine_build_id',
    'engine_executable_sha256'
)
foreach ($Field in $BoundFields) {
    if ([string]$Result.$Field -cne [string]$Request.$Field) {
        throw "ASP54R4104 result/request provenance mismatch: $Field"
    }
}
if ([string]$Result.request_sha256 -cne $RequestSha256 -or
    [string]$Result.profile_sha256 -cne $ProfileSha256) {
    throw 'ASP54R4105 request or profile digest binding failed'
}
if ([string]$Result.kernel_wasm_sha256 -cne
        [string]$Request.kernel_wasm_sha256 -or
    [string]$Result.kernel_wasm_sha256 -cne
        [string]$Profile.kernel_wasm_sha256) {
    throw 'ASP54R4106 kernel digest binding failed'
}
if ([int]$Result.process_run -ne [int]$Request.process_run -or
    [int]$Result.request_seed -ne [int]$Request.seed -or
    [string]$Result.mode -cne [string]$Request.mode) {
    throw 'ASP54R4107 mode, process, or seed identity differs from request'
}

$ExpectedLaneIds = @($Profile.lanes)
$ObservedLaneIds = @(
    $Result.lane_identities |
        ForEach-Object { [string]$_.lane_id }
)
if ([string]::Join('|', $ObservedLaneIds) -cne
    [string]::Join('|', $ExpectedLaneIds)) {
    throw 'ASP54R4108 controlled runtime lane catalog differs from contract'
}
$V8Identity = $Result.lane_identities[0]
if ([string]$V8Identity.runtime_id -cne 'v8.webassembly.tiered_jit' -or
    [string]$V8Identity.compiler_identity -cne 'v8.webassembly.tiered_jit' -or
    [string]$V8Identity.adapter_proof_id -cne
        'webassembly.module_instance.cached_export.v1' -or
    ([string]$V8Identity.compiler_identity).Contains('turbofan')) {
    throw 'ASP54R4109 V8 identity/proof must describe tiered WebAssembly Module/Instance'
}
if ([string]$Result.lane_identities[1].runtime_id -cne
        'wasmtime.cranelift.jit' -or
    [string]$Result.lane_identities[1].compiler_identity -cne 'cranelift') {
    throw 'ASP54R4110 Wasmtime identity must be wasmtime.cranelift.jit'
}
foreach ($Lane in @($Result.lane_identities)[0..2]) {
    if ([string]$Lane.source_wasm_sha256 -cne
            [string]$Result.kernel_wasm_sha256 -or
        [string]$Lane.execution_artifact_sha256 -cne
            [string]$Result.kernel_wasm_sha256 -or
        [bool]$Lane.fallback_used) {
        throw "ASP54R4111 lane identity or no-fallback failed: $($Lane.lane_id)"
    }
}
if ([string]$V8Identity.adapter_source_wasm_sha256 -cne
        [string]$Result.kernel_wasm_sha256 -or
    [string]$V8Identity.adapter_artifact_wasm_sha256 -cne
        [string]$Result.kernel_wasm_sha256) {
    throw 'ASP54R4112 V8 runtime adapter proof digest failed'
}
if ([string]$Result.lane_identities[0].runtime_build_identity -cne
        [string]$Request.puerts_commit -or
    [string]$Result.lane_identities[0].runtime_artifact_sha256 -cne
        [string]$Request.puerts_backend_sha256 -or
    [string]$Result.lane_identities[1].runtime_build_identity -cne
        [string]$Request.wasmtime_runtime_build_identity -or
    [string]$Result.lane_identities[1].runtime_artifact_sha256 -cne
        [string]$Request.wasmtime_runtime_artifact_sha256 -or
    [string]$Result.lane_identities[2].runtime_build_identity -cne
        [string]$Request.wamr_runtime_build_identity -or
    [string]$Result.lane_identities[2].runtime_artifact_sha256 -cne
        [string]$Request.wamr_runtime_artifact_sha256) {
    throw 'ASP54R4113 runtime build/artifact provenance differs from request'
}
if ([bool]$Result.compile_in_timed_region -or
    [bool]$Result.instantiate_in_timed_region -or
    [bool]$Result.export_lookup_in_timed_region -or
    [bool]$Result.fallback_used -or
    [int]$Result.correctness_failures -ne 0) {
    throw 'ASP54R4114 timing boundary, no-fallback, or correctness contract failed'
}
if ([string]$Result.lane_schedule_id -cne
        [string]$Profile.lane_schedule_id -or
    [string]$Result.lane_schedule_id -cne
        [string]$Request.lane_schedule_id) {
    throw 'ASP54R4115 lane schedule identity differs from request/profile'
}

if (-not ('AvidScriptControlledOracle' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
public static class AvidScriptControlledOracle
{
    public static int Run(int iterations, int seed)
    {
        unchecked
        {
            uint value = (uint)seed ^ 0x6d2b79f5u;
            for (uint index = 0; index < (uint)iterations; ++index)
            {
                value = (value << 13) | (value >> 19);
                value = value * 1664525u + 1013904223u;
                value ^= (uint)seed + index;
            }
            return (int)value;
        }
    }
}
'@
}

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -lt 1) {
        throw 'ASP54R4116 median requires observations'
    }
    $Sorted = @($Values | Sort-Object)
    $Middle = [int][Math]::Floor($Sorted.Count / 2)
    if (($Sorted.Count % 2) -eq 1) {
        return [double]$Sorted[$Middle]
    }
    return ([double]$Sorted[$Middle - 1] +
        [double]$Sorted[$Middle]) / 2.0
}

if ([string]$Result.mode -ceq 'calibration') {
    if (@($Result.samples).Count -ne 0 -or
        [string]$Result.calibration_sha256 -cne 'not_applicable') {
        throw 'ASP54R4117 calibration result must not contain timed evidence'
    }
    foreach ($LaneId in $ExpectedLaneIds) {
        $Entry = $Result.calibration.PSObject.Properties[$LaneId]
        if ($null -eq $Entry -or [int]$Entry.Value.iterations -lt
            [int]$Profile.minimum_iterations -or
            [int]$Entry.Value.iterations -gt [int]$Profile.maximum_iterations) {
            throw "ASP54R4118 calibration iterations invalid: $LaneId"
        }
        [double[]]$Durations = @($Entry.Value.confirmation_duration_ns)
        if ($Durations.Count -ne
                [int]$Profile.calibration_confirmation_samples -or
            @($Durations | Where-Object {
                -not [double]::IsFinite($_) -or $_ -le 0
            }).Count -ne 0) {
            throw "ASP54R4119 calibration confirmation evidence invalid: $LaneId"
        }
        $Median = Get-Median -Values $Durations
        if ([Math]::Abs($Median -
                [double]$Entry.Value.median_duration_ns) -gt 0.5 -or
            $Median -lt
                [double]$Profile.minimum_sample_milliseconds * 1000000.0) {
            throw "ASP54R4120 calibration median below frozen minimum: $LaneId"
        }
    }
}
else {
    if ([string]::IsNullOrWhiteSpace($CalibrationResultPath)) {
        throw 'ASP54R4121 timed validation requires frozen calibration evidence'
    }
    $ResolvedCalibrationPath = [System.IO.Path]::GetFullPath(
        $CalibrationResultPath)
    $CalibrationSha256 = Get-SidecarFileSha256 -Path $ResolvedCalibrationPath
    if ([string]$Result.calibration_sha256 -cne $CalibrationSha256 -or
        [string]$Request.calibration_sha256 -cne $CalibrationSha256) {
        throw 'ASP54R4122 timed result is not bound to frozen calibration bytes'
    }
    $Calibration = Get-Content -LiteralPath $ResolvedCalibrationPath -Raw |
        ConvertFrom-Json
    if ([string]$Calibration.attempt_id -cne [string]$Result.attempt_id -or
        [int]$Calibration.pid -eq [int]$Result.pid) {
        throw 'ASP54R4123 calibration attempt mismatch or PID reuse'
    }

    $TimedSamples = [int]$Profile.timed_samples
    $WarmupSamples = [int]$Profile.warmup_samples
    $Samples = @($Result.samples)
    if ($Samples.Count -ne $ExpectedLaneIds.Count * $TimedSamples -or
        @($Result.warmup_lane_orders).Count -ne $WarmupSamples -or
        @($Result.timed_lane_orders).Count -ne $TimedSamples) {
        throw 'ASP54R4124 timed observation or schedule count mismatch'
    }
    $Seen = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    for ($SampleIndex = 0; $SampleIndex -lt $TimedSamples; ++$SampleIndex) {
        $Rotation = ([int]$Result.process_run + $SampleIndex) %
            $ExpectedLaneIds.Count
        $ExpectedOrder = @(
            0..($ExpectedLaneIds.Count - 1) |
                ForEach-Object {
                    $ExpectedLaneIds[($Rotation + $_) % $ExpectedLaneIds.Count]
                }
        )
        $ObservedOrder = @($Result.timed_lane_orders[$SampleIndex])
        if ([string]::Join('|', $ObservedOrder) -cne
            [string]::Join('|', $ExpectedOrder)) {
            throw "ASP54R4125 timed lane order mismatch: sample=$SampleIndex"
        }
        for ($LanePosition = 0;
            $LanePosition -lt $ExpectedLaneIds.Count;
            ++$LanePosition) {
            $LaneId = $ExpectedOrder[$LanePosition]
            $Matches = @($Samples | Where-Object {
                [string]$_.lane_id -ceq $LaneId -and
                [int]$_.sample_index -eq $SampleIndex
            })
            if ($Matches.Count -ne 1) {
                throw "ASP54R4126 missing or duplicate sample: lane=$LaneId index=$SampleIndex"
            }
            $Sample = $Matches[0]
            $Key = "$LaneId/$SampleIndex"
            if (-not $Seen.Add($Key) -or
                [int]$Sample.lane_position -ne $LanePosition -or
                [int]$Sample.lane_rotation -ne $Rotation) {
                throw "ASP54R4127 lane position or duplicate failed: $Key"
            }
            $ExpectedIterations = [int]$Request.iterations.$LaneId
            $ResultIterations = [int]$Result.iterations.$LaneId
            $CalibrationIterations =
                [int]$Calibration.calibration.$LaneId.iterations
            $ExpectedSeed = [int]$Request.seed +
                [int]$Request.process_run * 1009 + $SampleIndex * 17
            if ([int]$Sample.iterations -ne $ExpectedIterations -or
                $ResultIterations -ne $ExpectedIterations -or
                $ExpectedIterations -ne $CalibrationIterations -or
                [int]$Sample.seed -ne $ExpectedSeed) {
                throw "ASP54R4128 frozen iterations or seed mismatch: $Key"
            }
            $Duration = [double]$Sample.duration_ns
            $NsPerIteration = [double]$Sample.ns_per_iteration
            if (-not [double]::IsFinite($Duration) -or $Duration -le 0 -or
                -not [double]::IsFinite($NsPerIteration) -or
                $NsPerIteration -le 0) {
                throw "ASP54R4129 non-positive or non-finite timing: $Key"
            }
            $RecomputedNs = $Duration / [double]$ExpectedIterations
            $Tolerance = [Math]::Max(1e-12, [Math]::Abs($RecomputedNs) * 1e-12)
            if ([Math]::Abs($RecomputedNs - $NsPerIteration) -gt $Tolerance) {
                throw "ASP54R4130 ns_per_iteration recomputation failed: $Key"
            }
            $Oracle = [AvidScriptControlledOracle]::Run(
                $ExpectedIterations,
                $ExpectedSeed)
            if ([int]$Sample.result -ne $Oracle -or
                [int]$Sample.expected -ne $Oracle -or
                -not [bool]$Sample.correct -or
                [int]$Sample.host_crossing_count -ne 1) {
                throw "ASP54R4131 independent oracle failed: $Key"
            }
        }
    }
    for ($WarmupIndex = 0; $WarmupIndex -lt $WarmupSamples; ++$WarmupIndex) {
        $Rotation = ([int]$Result.process_run + $WarmupIndex) %
            $ExpectedLaneIds.Count
        $ExpectedOrder = @(
            0..($ExpectedLaneIds.Count - 1) |
                ForEach-Object {
                    $ExpectedLaneIds[($Rotation + $_) % $ExpectedLaneIds.Count]
                }
        )
        if ([string]::Join('|', @($Result.warmup_lane_orders[$WarmupIndex])) -cne
            [string]::Join('|', $ExpectedOrder)) {
            throw "ASP54R4132 warmup lane order mismatch: index=$WarmupIndex"
        }
    }
}

[pscustomobject]@{
    result = 'controlled_runtime_result_valid'
    mode = [string]$Result.mode
    pid = [int]$Result.pid
    sample_count = @($Result.samples).Count
    request_sha256 = $RequestSha256
    profile_sha256 = $ProfileSha256
    kernel_wasm_sha256 = [string]$Result.kernel_wasm_sha256
}
