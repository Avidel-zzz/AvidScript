$ErrorActionPreference = 'Stop'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "ASP53C1001 $Message"
    }
}

function Get-SourceText {
    param([Parameter(Mandatory = $true)][string]$Path)

    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) "missing source file: $Path"
    return Get-Content -LiteralPath $Path -Raw
}

$HarnessRoot = Split-Path -Parent $PSScriptRoot
$SourceRoot = Join-Path $HarnessRoot 'Source/AvidScriptPerfHarness'
$RunnerHeader = Get-SourceText (Join-Path $SourceRoot 'Public/AvidScriptPerfRunner.h')
$RunnerSource = Get-SourceText (Join-Path $SourceRoot 'Private/AvidScriptPerfRunner.cpp')
$CommandletHeader = Get-SourceText (Join-Path $SourceRoot 'Public/AvidScriptPerfRunCommandlet.h')
$CommandletSource = Get-SourceText (Join-Path $SourceRoot 'Private/AvidScriptPerfRunCommandlet.cpp')
$ModuleSource = Get-SourceText (Join-Path $SourceRoot 'Private/AvidScriptPerfHarnessModule.cpp')

Assert-True ($RunnerHeader.Contains('RunWarmBenchmarkFromFiles')) `
    'runner must expose the warm benchmark file entrypoint'
Assert-True ($RunnerHeader.Contains('RunFourLaneCorrectnessSmoke')) `
    'existing four-lane correctness smoke API must remain available'

foreach ($Switch in @('AvidScriptPerfRequest=', 'AvidScriptPerfResult=')) {
    Assert-True ($CommandletSource.Contains($Switch)) "commandlet must parse -$Switch"
}
Assert-True ($CommandletHeader.Contains('UCommandlet')) 'warm runner must be an Editor commandlet'
Assert-True ($ModuleSource.Contains('AvidScript.PerformanceComparison.Run')) `
    'module must expose the sidecar console command'

foreach ($Workload in @(
    'pure_integer',
    'scalar_noop',
    'scalar_add_int32',
    'property_get_set',
    'vector_value',
    'object_roundtrip',
    'batch_scalar')) {
    Assert-True ($RunnerSource.Contains("TEXT(`"$Workload`")")) "missing workload: $Workload"
}
foreach ($UnsupportedWorkload in @('callback_empty', 'callback_begin_play', 'callback_tick', 'vector_ref_out')) {
    Assert-True (-not $RunnerSource.Contains("TEXT(`"$UnsupportedWorkload`")")) `
        "warm core must not advertise unsupported workload: $UnsupportedWorkload"
}

Assert-True ($RunnerSource.Contains('BuildBalancedLaneOrder')) `
    'warm core must derive a balanced lane order for each sample'
foreach ($BalancedRow in @(
    '{ 0, 1, 3, 2 }',
    '{ 1, 2, 0, 3 }',
    '{ 2, 3, 1, 0 }',
    '{ 3, 0, 2, 1 }')) {
    Assert-True ($RunnerSource.Contains($BalancedRow)) "missing balanced lane row: $BalancedRow"
}
Assert-True ($RunnerSource.Contains('LanePosition')) 'samples must record lane_position'
Assert-True ($RunnerSource.Contains('CalibrateWorkloadIterations')) `
    'warm core must calibrate one shared iteration count per workload'
Assert-True ($RunnerSource.Contains('MaximumIterations / 2')) `
    'calibration doubling must guard overflow'
Assert-True ($RunnerSource.Contains('EAvidScriptPerfBenchmarkMode::Calibrate')) `
    'request mode must support calibration-only processes'
Assert-True ($RunnerSource.Contains('TEXT("calibration")')) `
    'warm core must accept the sidecar calibration mode spelling'
Assert-True ($RunnerSource.Contains('EAvidScriptPerfBenchmarkMode::Timed')) `
    'request mode must support timed processes'
Assert-True ($RunnerSource.Contains('iteration_counts')) `
    'calibration must publish and timed mode must consume frozen iteration counts'
Assert-True ($RunnerSource.Contains('TEXT("calibration_id")')) `
    'calibration result must use its dedicated result contract'
Assert-True ($RunnerSource.Contains('calibration mode requires process_run=-1 and timed_samples=0')) `
    'calibration mode must enforce its process and sample shape'
Assert-True ($RunnerSource.Contains('timed mode requires an exact iteration_counts mapping')) `
    'timed mode must reject missing or incomplete frozen iteration counts'
Assert-True ($RunnerSource.Contains('if (Request.Mode != EAvidScriptPerfBenchmarkMode::Timed)')) `
    'explicit timed mode must not enter calibration'
Assert-True ($RunnerSource.Contains('constexpr int32 PerfRunnerResultSchemaVersion = 1;')) `
    'warm core must pin the emitted result payload to schema v1'
$ExactVersionPattern = 'TEXT\("version"\),\s*PerfRunnerResultSchemaVersion,\s*PerfRunnerResultSchemaVersion,'
Assert-True ([regex]::IsMatch($RunnerSource, $ExactVersionPattern)) `
    'result_schema.version parser must accept exactly v1'
$SerializedVersionPattern = 'TEXT\("schema_version"\),\s*PerfRunnerResultSchemaVersion\)'
Assert-True ([regex]::IsMatch($RunnerSource, $SerializedVersionPattern)) `
    'result serialization must always label the payload as v1'
$SchemaHashFieldPattern = 'OutRequest\.Mode == EAvidScriptPerfBenchmarkMode::Calibrate\s*\?\s*TEXT\("calibration_schema_sha256"\)\s*:\s*TEXT\("result_schema_sha256"\)'
Assert-True ([regex]::IsMatch($RunnerSource, $SchemaHashFieldPattern)) `
    'calibration and timed modes must select their respective provenance schema hash fields'
Assert-True ($RunnerSource.Contains('request result_schema.sha256 must equal selected provenance schema hash')) `
    'warm core must fail closed on a selected schema hash mismatch'
$HashEqualityPattern = 'ResultSchemaSha256\.Equals\(\s*ExpectedProvenanceSchemaSha256,\s*ESearchCase::CaseSensitive\)'
Assert-True ([regex]::IsMatch($RunnerSource, $HashEqualityPattern)) `
    'selected result schema hash equality must be case-sensitive'
Assert-True ($RunnerSource.Contains('WarmupSamples')) `
    'warm core must execute the exact requested warmup count'
Assert-True ($RunnerSource.Contains('TimedSamples')) `
    'warm core must execute the exact requested timed sample count'

Assert-True ($RunnerSource.Contains('FPlatformTime::Cycles64()')) `
    'timed samples must use FPlatformTime::Cycles64'
Assert-True ($RunnerSource.Contains('DispatchWorkload')) `
    'AvidScript DispatchEvent must have an isolated timed entrypoint'
Assert-True ($RunnerSource.Contains('CollectWorkloadResult')) `
    'AvidScript state collection must be separate from DispatchEvent timing'
$RunLaneStart = $RunnerSource.IndexOf('bool RunPerfLane(')
$RunLaneEnd = $RunnerSource.IndexOf('bool ValidatePerfObservation(', $RunLaneStart)
Assert-True ($RunLaneStart -ge 0 -and $RunLaneEnd -gt $RunLaneStart) `
    'unable to isolate RunPerfLane source'
$RunLaneSource = $RunnerSource.Substring($RunLaneStart, $RunLaneEnd - $RunLaneStart)
$DispatchIndex = $RunLaneSource.IndexOf('AvidScript.DispatchWorkload(')
$EndCyclesIndex = $RunLaneSource.IndexOf('EndCycles = FPlatformTime::Cycles64();', $DispatchIndex)
$CollectIndex = $RunLaneSource.IndexOf('AvidScript.CollectWorkloadResult(')
Assert-True ($DispatchIndex -ge 0 -and $EndCyclesIndex -gt $DispatchIndex -and $CollectIndex -gt $EndCyclesIndex) `
    'AvidScript result/state collection must occur after the timed DispatchEvent region'
Assert-True ($RunnerSource.Contains('FILEWRITE_NoReplaceExisting')) `
    'successful result publication must reject overwrite'
Assert-True ($RunnerSource.Contains('same_directory_temporary_then_atomic_rename')) `
    'warm core must enforce the request atomic publication strategy'
Assert-True ($RunnerSource.Contains('IFileManager::Get().Move')) `
    'successful result publication must atomically rename the request temporary path'

foreach ($Field in @(
    'seed',
    'expected_checksum',
    'final_scalar',
    'expected_final_scalar',
    'operation_call_count',
    'expected_operation_call_count',
    'host_import_call_count',
    'expected_host_import_call_count',
    'correct')) {
    Assert-True ($RunnerSource.Contains("TEXT(`"$Field`")")) "sample JSON must include $Field"
}

Write-Output 'AvidScript warm benchmark core static contracts passed: commandlet=1 lanes=4 workloads=7 timing=cycles64 validation=1'
