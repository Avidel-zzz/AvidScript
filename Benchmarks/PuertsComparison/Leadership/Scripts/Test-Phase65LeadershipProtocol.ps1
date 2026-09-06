[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$LeadershipRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ComparisonRoot = Split-Path -Parent $LeadershipRoot
$PluginRoot = Split-Path -Parent (Split-Path -Parent $ComparisonRoot)
$ProtocolPath = Join-Path $LeadershipRoot 'Config/Phase65LeadershipProtocol.json'
$SchemaPath = Join-Path $LeadershipRoot 'Schema/Phase65LeadershipProtocol.schema.json'
$CandidateSchemaPath = Join-Path $LeadershipRoot 'Schema/Phase65LeadershipCandidate.schema.json'
$CandidateScriptPath = Join-Path $LeadershipRoot 'Scripts/New-Phase65LeadershipCandidate.ps1'

function Assert-True {
    param([Parameter(Mandatory = $true)][bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)

    if (-not $Condition) {
        throw "ASP65L1000 $Message"
    }
}

function Get-NormalizedTextSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Text = [IO.File]::ReadAllText($Path).Replace("`r`n", "`n").Replace("`r", "`n")
    $Bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

$ProtocolRaw = Get-Content -LiteralPath $ProtocolPath -Raw
Assert-True ($ProtocolRaw | Test-Json -SchemaFile $SchemaPath) 'leadership protocol does not satisfy schema v1'
$Protocol = $ProtocolRaw | ConvertFrom-Json -Depth 64
$ParserErrors = $null
$Tokens = $null
[void][Management.Automation.Language.Parser]::ParseFile($CandidateScriptPath, [ref]$Tokens, [ref]$ParserErrors)
Assert-True (@($ParserErrors).Count -eq 0) 'leadership candidate freezer has parser errors'
Assert-True ((Get-Content -LiteralPath $CandidateSchemaPath -Raw) | Test-Json) 'leadership candidate schema is not valid JSON'
$CandidateScriptText = Get-Content -LiteralPath $CandidateScriptPath -Raw
Assert-True ($CandidateScriptText.Contains('source did not stabilize after the bounded two-pass preparation')) 'candidate freezer must fail closed after two source-stabilization passes'
Assert-True ($CandidateScriptText.Contains("'-Profile=`"{0}`"'")) 'candidate freezer must preserve hyphenated profile paths through UE command-line parsing'
Assert-True ($CandidateScriptText.Contains('Assert-SidecarBenchmarkProjectProvenance')) 'candidate freezer must revalidate project provenance after artifact preparation'

$InputIds = @($Protocol.tracked_inputs | ForEach-Object { [string]$_.id })
Assert-True ($InputIds.Count -eq (@($InputIds | Sort-Object -Unique)).Count) 'tracked input ids must be unique'
foreach ($Input in @($Protocol.tracked_inputs)) {
    $Path = [IO.Path]::GetFullPath((Join-Path $PluginRoot ([string]$Input.relative_path)))
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) "tracked input is missing: $($Input.id)"
    Assert-True ((Get-NormalizedTextSha256 -Path $Path) -ceq [string]$Input.sha256) "tracked input identity drifted: $($Input.id)"
}

$ProfilesRoot = Join-Path $ComparisonRoot 'Profiles'
$MicroProfile = Get-Content -LiteralPath (Join-Path $ProfilesRoot 'Phase56Micro.formal.json') -Raw | ConvertFrom-Json -Depth 64
$GameplayProfile = Get-Content -LiteralPath (Join-Path $ProfilesRoot 'Phase56Gameplay.formal.json') -Raw | ConvertFrom-Json -Depth 64
$ExpectedSixLanes = @('native_cpp', 'puerts_v8_reflection', 'puerts_v8_static', 'avidscript_wasmtime_adaptive_semantic', 'avidscript_wasmtime_generated_s1', 'avidscript_wasmtime_data_oriented')
$ExpectedMicroWorkloads = @('callback_empty', 'callback_tick', 'pure_integer', 'scalar_noop', 'scalar_add_int32', 'property_get_set', 'vector_value', 'vector_ref_out', 'object_roundtrip', 'batch_scalar')
$ExpectedGameplayWorkloads = @('gameplay_frame_small', 'gameplay_frame_dense')
foreach ($Profile in @($MicroProfile, $GameplayProfile)) {
    Assert-True ([string]::Join('|', @($Profile.lanes)) -ceq [string]::Join('|', $ExpectedSixLanes)) "six-lane order drifted: $($Profile.profile_id)"
    Assert-True ([int]$Profile.process_runs -eq 5 -and [int]$Profile.warmup_samples -eq 5 -and [int]$Profile.timed_samples -eq 30) "sampling contract drifted: $($Profile.profile_id)"
}
Assert-True ([string]::Join('|', @($MicroProfile.workloads)) -ceq [string]::Join('|', $ExpectedMicroWorkloads)) 'micro workload order drifted'
Assert-True ([string]::Join('|', @($GameplayProfile.workloads)) -ceq [string]::Join('|', $ExpectedGameplayWorkloads)) 'gameplay workload order drifted'
Assert-True ([double]$GameplayProfile.gates.semantic_vs_puerts_reflection.maximum -eq 0.8) 'semantic leadership gate drifted'
Assert-True ([double]$GameplayProfile.gates.small_vs_best_puerts.maximum -eq 0.7 -and [double]$GameplayProfile.gates.dense_vs_best_puerts.maximum -eq 0.7) 'gameplay leadership gates drifted'

$ControlledRoot = Join-Path $ComparisonRoot 'ControlledRuntime'
$SuiteProfile = Get-Content -LiteralPath (Join-Path $ControlledRoot 'Config/ControlledRuntimeSuiteProfile.json') -Raw | ConvertFrom-Json -Depth 64
Assert-True (@($SuiteProfile.kernel_ids).Count -eq 12) 'identical WASM suite must contain twelve kernels'
Assert-True ([int]$SuiteProfile.process_runs -eq 5 -and [int]$SuiteProfile.warmup_samples -eq 5 -and [int]$SuiteProfile.timed_samples -eq 30) 'identical WASM sampling contract drifted'
Assert-True ([double]$SuiteProfile.pc_leadership_gate.maximum_geometric_mean_ratio -eq 0.95) 'identical WASM geometric-mean gate drifted'
Assert-True ([double]$SuiteProfile.pc_leadership_gate.maximum_mad_geometric_mean_ratio -eq 1.25) 'identical WASM MAD gate drifted'
Assert-True ([double]$SuiteProfile.pc_leadership_gate.minimum_kernel_win_rate -eq 0.6) 'identical WASM win-rate gate drifted'

$PuertsLock = Get-Content -LiteralPath (Join-Path $ComparisonRoot 'Config/PuertsDependency.lock.json') -Raw | ConvertFrom-Json -Depth 32
Assert-True ([string]$PuertsLock.source.commit_sha -ceq [string]$Protocol.competitors.puerts.source_commit) 'Puerts source commit differs from protocol'
Assert-True ([string]$PuertsLock.backend.runtime_version -ceq [string]$Protocol.competitors.puerts.runtime_version) 'Puerts runtime version differs from protocol'
Assert-True ([string]$PuertsLock.backend.sha256 -ceq [string]$Protocol.competitors.puerts.backend_sha256) 'Puerts backend differs from protocol'

$WasmtimeLock = Get-Content -LiteralPath (Join-Path $PluginRoot 'Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.lock.json') -Raw | ConvertFrom-Json -Depth 32
Assert-True ([string]$WasmtimeLock.upstream.version -ceq 'v45.0.0') 'Wasmtime version differs from protocol'
Assert-True ([string]$WasmtimeLock.compiler_profile.id -ceq [string]$Protocol.competitors.avidscript.compiler_profile) 'Wasmtime compiler profile differs from protocol'

$MatrixIds = @($Protocol.matrices | ForEach-Object { [string]$_.id })
Assert-True ([string]::Join('|', $MatrixIds) -ceq 'ue_micro_six_lane|ue_gameplay_six_lane|identical_wasm_execution|angelscript_same_semantics') 'required matrix order drifted'
Assert-True ([string]$Protocol.competitors.angelscript.availability -ceq 'not_frozen') 'AngelScript must remain explicitly blocked until its dependency and adapter are frozen'

Write-Output 'Phase 65 leadership protocol contracts passed: schema=2 scripts=1 tracked_inputs=7 ue_lanes=6 micro_workloads=10 gameplay_workloads=2 identical_wasm_kernels=12 competitors=4 matrices=4 bounded_source_stabilization=1 claims_fail_closed=1'
