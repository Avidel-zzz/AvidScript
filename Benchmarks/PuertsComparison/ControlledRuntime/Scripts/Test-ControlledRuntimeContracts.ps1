[CmdletBinding()]
param(
    [string]$WatCompilerModuleRoot = ''
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ControlledRoot = Split-Path -Parent $ScriptRoot
$PuertsComparisonRoot = Split-Path -Parent $ControlledRoot
$PluginRoot = Split-Path -Parent (Split-Path -Parent $PuertsComparisonRoot)
$HarnessRoot = Join-Path $PuertsComparisonRoot 'AvidScriptPerfHarness'
$ProfilePath = Join-Path $ControlledRoot 'Config/ControlledRuntimeProfile.json'
$RequestSchemaPath = Join-Path $ControlledRoot 'Schema/ControlledRuntimeRequest.schema.json'
$ResultSchemaPath = Join-Path $ControlledRoot 'Schema/ControlledRuntimeResult.schema.json'
$AggregateSchemaPath = Join-Path $ControlledRoot 'Schema/ControlledRuntimeAggregate.schema.json'
$KernelContractPath = Join-Path $ControlledRoot 'Kernel/controlled_runtime_kernel.contract.json'
$KernelWasmPath = Join-Path $ControlledRoot 'Kernel/controlled_runtime_kernel.wasm'
$KernelWatPath = Join-Path $ControlledRoot 'Kernel/controlled_runtime_kernel.wat'
$RunnerPath = Join-Path $HarnessRoot (
    'Source/AvidScriptPerfHarness/Private/AvidScriptControlledRuntimeRunner.cpp')
$JavaScriptPath = Join-Path $HarnessRoot 'Content/JavaScript/controlled_wasm.js'
$VmHeaderPath = Join-Path $PluginRoot 'Source/AvidScriptVM/Public/AvidScriptVmBackend.h'
$WasmtimeBackendPath = Join-Path $PluginRoot (
    'Source/AvidScriptVM/Private/AvidScriptWasmtimeBackend.cpp')
$WamrBackendPath = Join-Path $PluginRoot (
    'Source/AvidScriptVM/Private/AvidScriptWamrBackend.cpp')
$AttributesPath = Join-Path $PluginRoot '.gitattributes'

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) {
        throw "ASP54T4401 $Message"
    }
}

foreach ($RequiredFile in @(
    $ProfilePath,
    $RequestSchemaPath,
    $ResultSchemaPath,
    $AggregateSchemaPath,
    $KernelContractPath,
    $KernelWasmPath,
    $KernelWatPath,
    $RunnerPath,
    $JavaScriptPath,
    $VmHeaderPath,
    $WasmtimeBackendPath,
    $WamrBackendPath,
    $AttributesPath)) {
    Assert-True (Test-Path -LiteralPath $RequiredFile -PathType Leaf) (
        "required controlled runtime file is missing: $RequiredFile")
}

$Profile = Get-Content -LiteralPath $ProfilePath -Raw | ConvertFrom-Json
$ExpectedLanes = @(
    'puerts_v8_wasm_jit',
    'avidscript_wasmtime_cranelift_jit',
    'avidscript_wamr_interpreter',
    'native_cpp_reference'
)
Assert-True ([int]$Profile.schema_version -eq 1) 'profile schema_version must be 1'
Assert-True ([string]$Profile.benchmark_kind -ceq 'identical_wasm_kernel') (
    'profile benchmark kind must be identical_wasm_kernel')
Assert-True ([int]$Profile.process_runs -eq 5) 'formal profile must use five fresh processes'
Assert-True ([int]$Profile.warmup_samples -eq 5) 'formal profile must use five warmups'
Assert-True ([int]$Profile.timed_samples -eq 30) 'formal profile must use 30 timed samples'
Assert-True ([double]$Profile.minimum_sample_milliseconds -ge 5.0) (
    'formal profile must calibrate each sample to at least 5 ms')
Assert-True (
    [string]::Join('|', @($Profile.lanes)) -ceq
        [string]::Join('|', $ExpectedLanes)) (
    'formal profile lane order differs from the controlled contract')

$KernelContract = Get-Content -LiteralPath $KernelContractPath -Raw | ConvertFrom-Json
$KernelDigest = (Get-FileHash -Algorithm SHA256 -LiteralPath $KernelWasmPath).Hash.ToLowerInvariant()
Assert-True ($KernelDigest -ceq [string]$KernelContract.wasm_sha256) (
    'tracked WASM digest differs from the kernel contract')
Assert-True ($KernelDigest -ceq [string]$Profile.kernel_wasm_sha256) (
    'tracked WASM digest differs from the profile')
$Attributes = Get-Content -LiteralPath $AttributesPath -Raw
Assert-True ($Attributes.Contains(
    'Benchmarks/PuertsComparison/ControlledRuntime/Kernel/*.wasm binary')) (
    'tracked WASM must be declared binary in .gitattributes')

$JavaScript = Get-Content -LiteralPath $JavaScriptPath -Raw
foreach ($Token in @(
    'new WebAssembly.Module(kernelBytes)',
    'new WebAssembly.Instance(kernelModule, {})',
    'const run = kernelInstance.exports.run',
    '(iterations, seed) => run(iterations | 0, seed | 0) | 0')) {
    Assert-True ($JavaScript.Contains($Token)) "V8 adapter is missing token: $Token"
}
Assert-True (-not $JavaScript.Contains('for (let index = 0; index < iterations')) (
    'V8 adapter must not replace the WASM kernel with a JavaScript loop')

$Runner = Get-Content -LiteralPath $RunnerPath -Raw
foreach ($Token in @(
    'TEXT("single_cached_export_call")',
    'TEXT("v8.webassembly.tiered_jit")',
    'TEXT("wasmtime.cranelift.jit")',
    'Selection.bAllowFallback = false',
    'Backend->ResolveExport(TEXT("run")',
    'Backend->Call(RunExport, Frame, VmError, &Result)',
    'RunControlledRuntimeOracle(Iterations, Seed)')) {
    Assert-True ($Runner.Contains($Token)) "controlled runner is missing token: $Token"
}
Assert-True (-not $Runner.Contains('v8_turbofan')) (
    'controlled runtime identity must not claim TurboFan-only')
$InitializationOffset = $Runner.IndexOf(
    'if (!Environment.Initialize(WasmBytes, ActualWasmSha256, OutError))',
    [System.StringComparison]::Ordinal)
$TimingOffset = $Runner.IndexOf(
    'const uint64 StartCycles = FPlatformTime::Cycles64()',
    [System.StringComparison]::Ordinal)
Assert-True ($InitializationOffset -ge 0 -and $TimingOffset -ge 0) (
    'runner must expose initialization and timing boundaries')

$VmHeader = Get-Content -LiteralPath $VmHeaderPath -Raw
$WasmtimeBackend = Get-Content -LiteralPath $WasmtimeBackendPath -Raw
$WamrBackend = Get-Content -LiteralPath $WamrBackendPath -Raw
Assert-True ($VmHeader.Contains('struct FAvidScriptVmCallResult')) (
    'VM contract must expose result cells')
Assert-True ($WasmtimeBackend.Contains('Entry.ResultCellCount')) (
    'Wasmtime backend must retain export result arity')
$WamrCallStart = $WamrBackend.IndexOf(
    'bool Call(',
    [System.StringComparison]::Ordinal)
$WamrCallEnd = $WamrBackend.IndexOf(
    'void Unload() override',
    $WamrCallStart,
    [System.StringComparison]::Ordinal)
Assert-True ($WamrCallStart -ge 0 -and $WamrCallEnd -gt $WamrCallStart) (
    'WAMR Call owner slice is unavailable')
$WamrCallSlice = $WamrBackend.Substring(
    $WamrCallStart,
    $WamrCallEnd - $WamrCallStart)
Assert-True ($WamrCallSlice.Contains('OutResult->Cells[0] = Cells[0]')) (
    'WAMR backend must publish the i32 return cell')
Assert-True (
    $WamrBackend.IndexOf(
        'OutResult->Cells[0] = Cells[0]',
        $WamrCallEnd,
        [System.StringComparison]::Ordinal) -lt 0) (
    'WAMR return-cell write must not escape the Call owner')

if (-not [string]::IsNullOrWhiteSpace($WatCompilerModuleRoot)) {
    & (Join-Path $ScriptRoot 'Build-ControlledRuntimeKernel.ps1') `
        -Mode Verify `
        -WatCompilerModuleRoot $WatCompilerModuleRoot | Out-Null
}

$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'AvidScriptControlledRuntimeContracts-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $FixtureRoot | Out-Null
try {
    function New-LaneIdentity {
        param(
            [string]$LaneId,
            [string]$RuntimeId,
            [string]$ExecutionTier,
            [string]$CompilerIdentity
        )
        $AdapterId = switch ($LaneId) {
            'puerts_v8_wasm_jit' { 'puerts_webassembly_module_instance' }
            'native_cpp_reference' { 'native_cpp_oracle' }
            default { 'avidscript_vm_backend' }
        }
        return [ordered]@{
            lane_id = $LaneId
            runtime_id = $RuntimeId
            runtime_version = 'fixture'
            adapter_id = $AdapterId
            execution_tier = $ExecutionTier
            wasm_workload_kind = 'identical_wasm_kernel'
            source_wasm_sha256 = $KernelDigest
            execution_artifact_format = 'wasm_bytecode'
            execution_artifact_sha256 = $KernelDigest
            compiler_identity = $CompilerIdentity
            compiler_flags = 'host_default'
            runtime_build_identity = 'fixture_build'
            runtime_artifact_sha256 = $KernelDigest
            target_triple = 'x86_64-pc-windows-msvc'
            fallback_used = $false
        }
    }

    $LaneIdentities = @(
        (New-LaneIdentity `
            -LaneId 'puerts_v8_wasm_jit' `
            -RuntimeId 'v8.webassembly.tiered_jit' `
            -ExecutionTier 'tiered_jit' `
            -CompilerIdentity 'v8.webassembly.tiered_jit'),
        (New-LaneIdentity `
            -LaneId 'avidscript_wasmtime_cranelift_jit' `
            -RuntimeId 'wasmtime.cranelift.jit' `
            -ExecutionTier 'jit' `
            -CompilerIdentity 'cranelift'),
        (New-LaneIdentity `
            -LaneId 'avidscript_wamr_interpreter' `
            -RuntimeId 'wamr.interpreter' `
            -ExecutionTier 'interpreter' `
            -CompilerIdentity 'not_applicable'),
        (New-LaneIdentity `
            -LaneId 'native_cpp_reference' `
            -RuntimeId 'unreal_engine_native' `
            -ExecutionTier 'native' `
            -CompilerIdentity 'ue58_msvc')
    )
    $TimedPaths = @()
    for ($ProcessRun = 0; $ProcessRun -lt 5; ++$ProcessRun) {
        $Samples = @()
        for ($LaneIndex = 0; $LaneIndex -lt $ExpectedLanes.Count; ++$LaneIndex) {
            $Samples += [ordered]@{
                lane_id = $ExpectedLanes[$LaneIndex]
                sample_index = 0
                iterations = 1000
                seed = 1397313 + $ProcessRun * 1009
                duration_ns = 5000000.0 + $LaneIndex * 100000.0
                ns_per_iteration = 5000.0 + $LaneIndex * 100.0
                result = 123
                expected = 123
                correct = $true
                host_crossing_count = 1
            }
        }
        $FixtureResult = [ordered]@{
            schema_version = 1
            benchmark_kind = 'identical_wasm_kernel'
            mode = 'timed'
            process_run = $ProcessRun
            pid = 50000 + $ProcessRun
            kernel_wasm_sha256 = $KernelDigest
            timing_boundary = 'single_cached_export_call'
            compile_in_timed_region = $false
            instantiate_in_timed_region = $false
            export_lookup_in_timed_region = $false
            fallback_used = $false
            lane_identities = $LaneIdentities
            calibration = [ordered]@{}
            samples = $Samples
            correctness_failures = 0
        }
        $FixturePath = Join-Path $FixtureRoot ("timed-$ProcessRun.json")
        [System.IO.File]::WriteAllText(
            $FixturePath,
            ($FixtureResult | ConvertTo-Json -Depth 16),
            [System.Text.UTF8Encoding]::new($false))
        & (Join-Path $ScriptRoot 'Test-ControlledRuntimeResult.ps1') `
            -ResultPath $FixturePath `
            -ExpectedTimedSamples 1 | Out-Null
        $TimedPaths += $FixturePath
    }
    $FixtureProfile = $Profile.PSObject.Copy()
    $FixtureProfile.process_runs = 5
    $FixtureProfile.timed_samples = 1
    $FixtureProfilePath = Join-Path $FixtureRoot 'profile.json'
    [System.IO.File]::WriteAllText(
        $FixtureProfilePath,
        ($FixtureProfile | ConvertTo-Json -Depth 16),
        [System.Text.UTF8Encoding]::new($false))
    $AggregatePath = Join-Path $FixtureRoot 'aggregate.json'
    & (Join-Path $ScriptRoot 'Merge-ControlledRuntimeResults.ps1') `
        -ResultPaths $TimedPaths `
        -ProfilePath $FixtureProfilePath `
        -OutputPath $AggregatePath | Out-Null
    $AggregateText = [System.IO.File]::ReadAllText($AggregatePath)
    Assert-True ($AggregateText | Test-Json -SchemaFile $AggregateSchemaPath) (
        'fixture aggregate must match schema v1')

    $RejectedFixture = Get-Content -LiteralPath $TimedPaths[0] -Raw | ConvertFrom-Json
    $RejectedFixture.lane_identities[0].compiler_identity = 'v8_turbofan'
    $RejectedPath = Join-Path $FixtureRoot 'rejected-turbofan-only.json'
    [System.IO.File]::WriteAllText(
        $RejectedPath,
        ($RejectedFixture | ConvertTo-Json -Depth 16),
        [System.Text.UTF8Encoding]::new($false))
    $Rejected = $false
    try {
        & (Join-Path $ScriptRoot 'Test-ControlledRuntimeResult.ps1') `
            -ResultPath $RejectedPath `
            -ExpectedTimedSamples 1 | Out-Null
    }
    catch {
        $Rejected = $true
    }
    Assert-True $Rejected 'TurboFan-only V8 identity mutation must be rejected'
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

[pscustomobject]@{
    result = 'controlled_runtime_contracts_passed'
    lane_count = $ExpectedLanes.Count
    kernel_wasm_sha256 = $KernelDigest
    formal_observation_count = (
        [int]$Profile.process_runs *
        [int]$Profile.timed_samples *
        $ExpectedLanes.Count)
}
