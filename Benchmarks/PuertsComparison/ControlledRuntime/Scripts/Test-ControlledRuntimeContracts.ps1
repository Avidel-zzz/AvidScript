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
. (Join-Path $PuertsComparisonRoot 'Scripts/PuertsBenchmarkSidecar.Common.ps1')
$ProfilePath = Join-Path $ControlledRoot 'Config/ControlledRuntimeProfile.json'
$RequestSchemaPath = Join-Path $ControlledRoot (
    'Schema/ControlledRuntimeRequest.schema.json')
$ResultSchemaPath = Join-Path $ControlledRoot (
    'Schema/ControlledRuntimeResult.schema.json')
$AggregateSchemaPath = Join-Path $ControlledRoot (
    'Schema/ControlledRuntimeAggregate.schema.json')
$KernelContractPath = Join-Path $ControlledRoot (
    'Kernel/controlled_runtime_kernel.contract.json')
$KernelWasmPath = Join-Path $ControlledRoot (
    'Kernel/controlled_runtime_kernel.wasm')
$KernelWatPath = Join-Path $ControlledRoot (
    'Kernel/controlled_runtime_kernel.wat')
$RunnerPath = Join-Path $HarnessRoot (
    'Source/AvidScriptPerfHarness/Private/AvidScriptControlledRuntimeRunner.cpp')
$JavaScriptPath = Join-Path $HarnessRoot (
    'Content/JavaScript/controlled_wasm.js')
$VmHeaderPath = Join-Path $PluginRoot (
    'Source/AvidScriptVM/Public/AvidScriptVmBackend.h')
$WasmtimeBackendPath = Join-Path $PluginRoot (
    'Source/AvidScriptVM/Private/AvidScriptWasmtimeBackend.cpp')
$WasmtimeApiPath = Join-Path $PluginRoot (
    'Source/AvidScriptVM/Private/AvidScriptWasmtimeApi.c')
$WamrBackendPath = Join-Path $PluginRoot (
    'Source/AvidScriptVM/Private/AvidScriptWamrBackend.cpp')
$WasmtimeTestsPath = Join-Path $PluginRoot (
    'Source/AvidScriptVM/Private/Tests/AvidScriptVmWasmtimeTests.cpp')
$WamrTestsPath = Join-Path $PluginRoot (
    'Source/AvidScriptVM/Private/Tests/AvidScriptVmContractTests.cpp')
$ResultFixtureBuilderPath = Join-Path $PluginRoot (
    'Source/AvidScriptVM/Private/Tests/AvidScriptVmResultFixtureBuilder.h')
$AttributesPath = Join-Path $PluginRoot '.gitattributes'
$ValidatorPath = Join-Path $ScriptRoot 'Test-ControlledRuntimeResult.ps1'
$MergerPath = Join-Path $ScriptRoot 'Merge-ControlledRuntimeResults.ps1'
$OrchestratorPath = Join-Path $ScriptRoot (
    'Invoke-ControlledRuntimeShootout.ps1')

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) {
        throw "ASP54T4401 $Message"
    }
}

function Write-JsonFile {
    param(
        $Value,
        [string]$Path
    )
    [System.IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 32),
        [System.Text.UTF8Encoding]::new($false))
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
    $WasmtimeApiPath,
    $WamrBackendPath,
    $WasmtimeTestsPath,
    $WamrTestsPath,
    $ResultFixtureBuilderPath,
    $AttributesPath,
    $ValidatorPath,
    $MergerPath,
    $OrchestratorPath
)) {
    Assert-True (Test-Path -LiteralPath $RequiredFile -PathType Leaf) (
        "required controlled runtime file is missing: $RequiredFile")
}

foreach ($PowerShellPath in @(
    $ValidatorPath,
    $MergerPath,
    $OrchestratorPath
)) {
    $ParseTokens = $null
    $ParseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $PowerShellPath,
        [ref]$ParseTokens,
        [ref]$ParseErrors)
    Assert-True (@($ParseErrors).Count -eq 0) (
        "PowerShell contract has parse errors: $PowerShellPath")
}

$Profile = Get-Content -LiteralPath $ProfilePath -Raw | ConvertFrom-Json
$ExpectedLanes = @(
    'puerts_v8_wasm_jit',
    'avidscript_wasmtime_cranelift_jit',
    'avidscript_wamr_interpreter',
    'native_cpp_reference'
)
Assert-True ([int]$Profile.schema_version -eq 1) (
    'profile schema_version must be 1')
Assert-True ([string]$Profile.benchmark_kind -ceq
    'identical_wasm_kernel') 'benchmark kind must be identical_wasm_kernel'
Assert-True ([int]$Profile.process_runs -eq 5) (
    'formal profile must use five fresh timed processes')
Assert-True ([int]$Profile.warmup_samples -eq 5) (
    'formal profile must use five warmups')
Assert-True ([int]$Profile.timed_samples -eq 30) (
    'formal profile must use 30 timed samples')
Assert-True ([int]$Profile.calibration_confirmation_samples -ge 3) (
    'calibration must freeze from at least three confirmations')
Assert-True ([double]$Profile.minimum_sample_milliseconds -ge 5.0) (
    'formal profile must calibrate each sample to at least 5 ms')
Assert-True ([int]$Profile.maximum_iterations -ge 100000000) (
    'formal profile must leave stable calibration headroom for modern JIT lanes')
Assert-True ([string]$Profile.lane_schedule_id -ceq
    'round_robin_process_sample_v1') (
    'formal profile must freeze the balanced lane schedule')
Assert-True (
    [string]::Join('|', @($Profile.lanes)) -ceq
        [string]::Join('|', $ExpectedLanes)) (
    'formal profile lane order differs from the controlled contract')

$KernelContract = Get-Content -LiteralPath $KernelContractPath -Raw |
    ConvertFrom-Json
$KernelDigest = Get-SidecarFileSha256 -Path $KernelWasmPath
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
    'webassembly.module_instance.cached_export.v1',
    'fixture.GetControlledWasmSha256()'
)) {
    Assert-True ($JavaScript.Contains($Token)) (
        "V8 adapter is missing runtime proof token: $Token")
}
Assert-True (-not $JavaScript.Contains(
    'for (let index = 0; index < iterations')) (
    'V8 adapter must not replace the WASM kernel with a JavaScript loop')

$Runner = Get-Content -LiteralPath $RunnerPath -Raw
foreach ($Token in @(
    'TEXT("single_cached_export_call")',
    'TEXT("v8.webassembly.tiered_jit")',
    'TEXT("wasmtime.cranelift.jit")',
    'Selection.bAllowFallback = false',
    'Backend->ResolveExport(TEXT("run")',
    'Backend->Call(RunExport, Frame, VmError, &Result)',
    'RunControlledRuntimeOracle(Iterations, Seed)',
    'GetScheduledLane(',
    'TEXT("lane_position")',
    'TEXT("lane_rotation")',
    'TEXT("request_sha256")',
    'ControlledRuntimeCalibrationConfirmationSamples'
)) {
    Assert-True ($Runner.Contains($Token)) (
        "controlled runner is missing token: $Token")
}
Assert-True (-not $Runner.Contains('v8_turbofan')) (
    'controlled runtime identity must not claim TurboFan-only')

$VmHeader = Get-Content -LiteralPath $VmHeaderPath -Raw
$WasmtimeBackend = Get-Content -LiteralPath $WasmtimeBackendPath -Raw
$WasmtimeApi = Get-Content -LiteralPath $WasmtimeApiPath -Raw
$WamrBackend = Get-Content -LiteralPath $WamrBackendPath -Raw
$WasmtimeTests = Get-Content -LiteralPath $WasmtimeTestsPath -Raw
$WamrTests = Get-Content -LiteralPath $WamrTestsPath -Raw
$ResultFixtureBuilder = Get-Content `
    -LiteralPath $ResultFixtureBuilderPath `
    -Raw
Assert-True ($VmHeader.Contains('struct FAvidScriptVmCallResult')) (
    'VM contract must expose result cells')
Assert-True ($VmHeader.Contains(
    'FAvidScriptVmCallResult* OutResult = nullptr')) (
    'legacy VM Call must keep default null result compatibility')
Assert-True ($WasmtimeBackend.Contains(
    'ResultCellCount > FAvidScriptVmCallResult::MaxCells')) (
    'Wasmtime resolve must reject result cells above fixed capacity')
Assert-True ($WasmtimeApi.Contains(
    'AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE')) (
    'Wasmtime shim must distinguish local precondition failure')
foreach ($Token in @(
    'wasm_func_get_result_count',
    'wasm_func_get_result_types',
    'ExportResultCellCounts'
)) {
    Assert-True ($WamrBackend.Contains($Token)) (
        "WAMR result ABI is missing token: $Token")
}
$WamrCallStart = $WamrBackend.IndexOf(
    'bool Call(',
    [System.StringComparison]::Ordinal)
$WamrCallEnd = $WamrBackend.IndexOf(
    'void Unload() override',
    $WamrCallStart,
    [System.StringComparison]::Ordinal)
Assert-True ($WamrCallStart -ge 0 -and
    $WamrCallEnd -gt $WamrCallStart) (
    'WAMR Call owner slice is unavailable')
$WamrCallSlice = $WamrBackend.Substring(
    $WamrCallStart,
    $WamrCallEnd - $WamrCallStart)
Assert-True ($WamrCallSlice.Contains(
    'ResultCellCount * sizeof(uint32)')) (
    'WAMR result copy must remain inside Call')
Assert-True (
    $WamrBackend.IndexOf(
        'ResultCellCount * sizeof(uint32)',
        $WamrCallEnd,
        [System.StringComparison]::Ordinal) -lt 0) (
    'WAMR result copy must not escape the Call owner')
foreach ($Token in @(
    'void result has zero cells',
    'i32 result has one cell',
    'i64 result has two cells',
    'f64 result has two cells',
    'f32 result bits are preserved',
    'oversize result ABI rejects at resolve',
    'unsupported result ABI rejects at resolve'
)) {
    Assert-True ($WasmtimeTests.Contains($Token)) (
        "Wasmtime focused result test is missing: $Token")
}
Assert-True ($WamrTests.Contains(
    'void WAMR export has zero result cells')) (
    'WAMR focused void result test is missing')
Assert-True ($WamrTests.Contains(
    'generated i32 export preserves value')) (
    'WAMR focused i32 result test is missing')
foreach ($Token in @(
    'WAMR i64 result has two cells',
    'WAMR i64 low cell is first',
    'WAMR i64 high cell is second',
    'WAMR f64 result has two cells',
    'WAMR f64 low bits cell is first',
    'WAMR f64 high bits cell is second',
    'WAMR f32 result bits are preserved'
)) {
    Assert-True ($WamrTests.Contains($Token)) (
        "WAMR wide result test is missing: $Token")
}
foreach ($Token in @(
    'enum class EValueKind',
    'AppendSignedLeb',
    'AppendLittleEndian',
    'BuildSingle'
)) {
    Assert-True ($ResultFixtureBuilder.Contains($Token)) (
        "shared result fixture builder is missing: $Token")
}

if (-not [string]::IsNullOrWhiteSpace($WatCompilerModuleRoot)) {
    & (Join-Path $ScriptRoot 'Build-ControlledRuntimeKernel.ps1') `
        -Mode Verify `
        -WatCompilerModuleRoot $WatCompilerModuleRoot | Out-Null
}

$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'AvidScriptControlledRuntimeContracts-' +
    [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $FixtureRoot | Out-Null
try {
    $FixtureProfilePath = Join-Path $FixtureRoot 'profile.json'
    Write-JsonFile -Value $Profile -Path $FixtureProfilePath
    $ProfileSha256 = Get-SidecarFileSha256 -Path $FixtureProfilePath
    $AttemptId = '11111111-2222-3333-4444-555555555555'
    $CandidateCommit = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    $CandidateTree = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
    $EngineSha256 = 'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc'
    $PuertsSha256 = 'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd'
    $WasmtimeSha256 = 'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee'
    $WamrSha256 = 'ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff'

    function New-LaneIdentity {
        param(
            [string]$LaneId,
            [string]$RuntimeId,
            [string]$ExecutionTier,
            [string]$CompilerIdentity,
            [string]$BuildIdentity,
            [string]$RuntimeSha256
        )
        $AdapterId = switch ($LaneId) {
            'puerts_v8_wasm_jit' { 'puerts_webassembly_module_instance' }
            'native_cpp_reference' { 'native_cpp_oracle' }
            default { 'avidscript_vm_backend' }
        }
        $Identity = [ordered]@{
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
            runtime_build_identity = $BuildIdentity
            runtime_artifact_sha256 = $RuntimeSha256
            target_triple = 'x86_64-pc-windows-msvc'
            fallback_used = $false
        }
        if ($LaneId -ceq 'puerts_v8_wasm_jit') {
            $Identity['adapter_proof_id'] =
                'webassembly.module_instance.cached_export.v1'
            $Identity['adapter_source_wasm_sha256'] = $KernelDigest
            $Identity['adapter_artifact_wasm_sha256'] = $KernelDigest
        }
        return $Identity
    }

    $LaneIdentities = @(
        (New-LaneIdentity `
            -LaneId 'puerts_v8_wasm_jit' `
            -RuntimeId 'v8.webassembly.tiered_jit' `
            -ExecutionTier 'tiered_jit' `
            -CompilerIdentity 'v8.webassembly.tiered_jit' `
            -BuildIdentity $CandidateCommit `
            -RuntimeSha256 $PuertsSha256),
        (New-LaneIdentity `
            -LaneId 'avidscript_wasmtime_cranelift_jit' `
            -RuntimeId 'wasmtime.cranelift.jit' `
            -ExecutionTier 'jit' `
            -CompilerIdentity 'cranelift' `
            -BuildIdentity 'fixture_wasmtime' `
            -RuntimeSha256 $WasmtimeSha256),
        (New-LaneIdentity `
            -LaneId 'avidscript_wamr_interpreter' `
            -RuntimeId 'wamr.interpreter' `
            -ExecutionTier 'interpreter' `
            -CompilerIdentity 'not_applicable' `
            -BuildIdentity 'fixture_wamr' `
            -RuntimeSha256 $WamrSha256),
        (New-LaneIdentity `
            -LaneId 'native_cpp_reference' `
            -RuntimeId 'unreal_engine_native' `
            -ExecutionTier 'native' `
            -CompilerIdentity 'ue58_msvc' `
            -BuildIdentity 'fixture_engine' `
            -RuntimeSha256 $KernelDigest)
    )

    function New-BaseRequest {
        return [ordered]@{
            schema_version = 1
            benchmark_kind = 'identical_wasm_kernel'
            warmup_samples = [int]$Profile.warmup_samples
            minimum_sample_milliseconds =
                [double]$Profile.minimum_sample_milliseconds
            minimum_iterations = [int]$Profile.minimum_iterations
            maximum_iterations = [int]$Profile.maximum_iterations
            seed = [int]$Profile.seed
            kernel_wasm_path = $KernelWasmPath
            kernel_wasm_sha256 = $KernelDigest
            puerts_commit = $CandidateCommit
            puerts_backend_sha256 = $PuertsSha256
            target_triple = [string]$Profile.target_triple
            lane_schedule_id = [string]$Profile.lane_schedule_id
            lanes = @($Profile.lanes)
            attempt_id = $AttemptId
            profile_sha256 = $ProfileSha256
            calibration_sha256 = 'not_applicable'
            candidate_commit = $CandidateCommit
            candidate_tree_sha = $CandidateTree
            candidate_clean = $true
            engine_version = '5.8.0-fixture'
            engine_build_id = 'fixture_engine'
            engine_executable_sha256 = $EngineSha256
            wasmtime_runtime_build_identity = 'fixture_wasmtime'
            wasmtime_runtime_artifact_sha256 = $WasmtimeSha256
            wamr_runtime_build_identity = 'fixture_wamr'
            wamr_runtime_artifact_sha256 = $WamrSha256
            calibration_confirmation_samples = 3
        }
    }

    $CalibrationRequest = New-BaseRequest
    $CalibrationRequest['mode'] = 'calibration'
    $CalibrationRequest['process_run'] = -1
    $CalibrationRequest['timed_samples'] = 0
    $CalibrationRequestPath = Join-Path $FixtureRoot (
        'calibration.request.json')
    Write-JsonFile `
        -Value $CalibrationRequest `
        -Path $CalibrationRequestPath
    Assert-True (
        ([System.IO.File]::ReadAllText($CalibrationRequestPath) |
            Test-Json -SchemaFile $RequestSchemaPath)) (
        'calibration fixture request must match schema')

    $CalibrationEntries = [ordered]@{}
    foreach ($LaneId in $ExpectedLanes) {
        $CalibrationEntries[$LaneId] = [ordered]@{
            iterations = 1000
            median_duration_ns = 6000000.0
            confirmation_duration_ns = @(
                5900000.0,
                6000000.0,
                6100000.0
            )
        }
    }
    $CalibrationResult = [ordered]@{
        schema_version = 1
        benchmark_kind = 'identical_wasm_kernel'
        mode = 'calibration'
        request_seed = [int]$Profile.seed
        attempt_id = $AttemptId
        request_sha256 = Get-SidecarFileSha256 `
            -Path $CalibrationRequestPath
        profile_sha256 = $ProfileSha256
        calibration_sha256 = 'not_applicable'
        candidate_commit = $CandidateCommit
        candidate_tree_sha = $CandidateTree
        candidate_clean = $true
        engine_version = '5.8.0-fixture'
        engine_build_id = 'fixture_engine'
        engine_executable_sha256 = $EngineSha256
        process_run = -1
        pid = 49000
        kernel_wasm_sha256 = $KernelDigest
        timing_boundary = 'single_cached_export_call'
        compile_in_timed_region = $false
        instantiate_in_timed_region = $false
        export_lookup_in_timed_region = $false
        fallback_used = $false
        lane_schedule_id = 'round_robin_process_sample_v1'
        lane_identities = $LaneIdentities
        calibration = $CalibrationEntries
        samples = @()
        correctness_failures = 0
    }
    $CalibrationResultPath = Join-Path $FixtureRoot (
        'calibration.result.json')
    Write-JsonFile `
        -Value $CalibrationResult `
        -Path $CalibrationResultPath
    & $ValidatorPath `
        -ResultPath $CalibrationResultPath `
        -RequestPath $CalibrationRequestPath `
        -ProfilePath $FixtureProfilePath | Out-Null
    $CalibrationSha256 = Get-SidecarFileSha256 `
        -Path $CalibrationResultPath

    $Iterations = [ordered]@{}
    foreach ($LaneId in $ExpectedLanes) {
        $Iterations[$LaneId] = 1000
    }
    $TimedPaths = @()
    $TimedRequestPaths = @()
    for ($ProcessRun = 0; $ProcessRun -lt 5; ++$ProcessRun) {
        $TimedRequest = New-BaseRequest
        $TimedRequest['mode'] = 'timed'
        $TimedRequest['process_run'] = $ProcessRun
        $TimedRequest['timed_samples'] = 30
        $TimedRequest['calibration_sha256'] = $CalibrationSha256
        $TimedRequest['iterations'] = $Iterations
        $TimedRequestPath = Join-Path $FixtureRoot (
            "timed-$ProcessRun.request.json")
        Write-JsonFile -Value $TimedRequest -Path $TimedRequestPath

        $Samples = @()
        $TimedOrders = @()
        for ($SampleIndex = 0; $SampleIndex -lt 30; ++$SampleIndex) {
            $Rotation = ($ProcessRun + $SampleIndex) % 4
            $Order = @(
                0..3 | ForEach-Object {
                    $ExpectedLanes[($Rotation + $_) % 4]
                }
            )
            $TimedOrders += ,$Order
            for ($LanePosition = 0; $LanePosition -lt 4; ++$LanePosition) {
                $LaneId = $Order[$LanePosition]
                $Seed = [int]$Profile.seed +
                    $ProcessRun * 1009 + $SampleIndex * 17
                $NsPerIteration = switch ($LaneId) {
                    'puerts_v8_wasm_jit' { 100.0 }
                    'avidscript_wasmtime_cranelift_jit' {
                        if ($ProcessRun -eq 0) { 130.0 } else { 100.0 }
                    }
                    'avidscript_wamr_interpreter' { 500.0 }
                    default { 50.0 }
                }
                $Oracle = [AvidScriptControlledOracle]::Run(1000, $Seed)
                $Samples += [ordered]@{
                    lane_id = $LaneId
                    sample_index = $SampleIndex
                    lane_position = $LanePosition
                    lane_rotation = $Rotation
                    iterations = 1000
                    seed = $Seed
                    duration_ns = $NsPerIteration * 1000.0
                    ns_per_iteration = $NsPerIteration
                    result = $Oracle
                    expected = $Oracle
                    correct = $true
                    host_crossing_count = 1
                }
            }
        }
        $WarmupOrders = @()
        for ($WarmupIndex = 0; $WarmupIndex -lt 5; ++$WarmupIndex) {
            $Rotation = ($ProcessRun + $WarmupIndex) % 4
            $WarmupOrders += ,@(
                0..3 | ForEach-Object {
                    $ExpectedLanes[($Rotation + $_) % 4]
                }
            )
        }
        $FixtureResult = [ordered]@{
            schema_version = 1
            benchmark_kind = 'identical_wasm_kernel'
            mode = 'timed'
            request_seed = [int]$Profile.seed
            attempt_id = $AttemptId
            request_sha256 = Get-SidecarFileSha256 `
                -Path $TimedRequestPath
            profile_sha256 = $ProfileSha256
            calibration_sha256 = $CalibrationSha256
            candidate_commit = $CandidateCommit
            candidate_tree_sha = $CandidateTree
            candidate_clean = $true
            engine_version = '5.8.0-fixture'
            engine_build_id = 'fixture_engine'
            engine_executable_sha256 = $EngineSha256
            process_run = $ProcessRun
            pid = 50000 + $ProcessRun
            kernel_wasm_sha256 = $KernelDigest
            timing_boundary = 'single_cached_export_call'
            compile_in_timed_region = $false
            instantiate_in_timed_region = $false
            export_lookup_in_timed_region = $false
            fallback_used = $false
            lane_schedule_id = 'round_robin_process_sample_v1'
            warmup_lane_orders = $WarmupOrders
            timed_lane_orders = $TimedOrders
            iterations = $Iterations
            lane_identities = $LaneIdentities
            calibration = [ordered]@{}
            samples = $Samples
            correctness_failures = 0
        }
        $FixturePath = Join-Path $FixtureRoot (
            "timed-$ProcessRun.result.json")
        Write-JsonFile -Value $FixtureResult -Path $FixturePath
        & $ValidatorPath `
            -ResultPath $FixturePath `
            -RequestPath $TimedRequestPath `
            -ProfilePath $FixtureProfilePath `
            -CalibrationResultPath $CalibrationResultPath | Out-Null
        $TimedPaths += $FixturePath
        $TimedRequestPaths += $TimedRequestPath
    }

    $AggregatePath = Join-Path $FixtureRoot 'aggregate.json'
    $MergeResult = & $MergerPath `
        -ResultPaths $TimedPaths `
        -RequestPaths $TimedRequestPaths `
        -CalibrationResultPath $CalibrationResultPath `
        -CalibrationRequestPath $CalibrationRequestPath `
        -ProfilePath $FixtureProfilePath `
        -OutputPath $AggregatePath
    Assert-True (
        ([System.IO.File]::ReadAllText($AggregatePath) |
            Test-Json -SchemaFile $AggregateSchemaPath)) (
        'fixture aggregate must match schema v1')
    $Aggregate = Get-Content -LiteralPath $AggregatePath -Raw |
        ConvertFrom-Json
    Assert-True (@($Aggregate.process_metrics).Count -eq 20) (
        'aggregate must publish one metric per process/lane')
    Assert-True (@($Aggregate.cross_process_metrics).Count -eq 4) (
        'aggregate must publish cross-process lane metrics')
    $CrossProcessP50 = [double](
        $Aggregate.paired_ratios.wasmtime_over_v8_cross_process_p50)
    $CrossProcessP95 = [double](
        $Aggregate.paired_ratios.wasmtime_over_v8_cross_process_p95)
    Assert-True ($CrossProcessP50 -eq 1.0) (
        'one slow/four fast mutation must keep paired cross-process P50 at 1')
    Assert-True ($CrossProcessP95 -eq 1.3) (
        'one slow/four fast mutation must expose paired cross-process P95')
    Assert-True ([string]$MergeResult.pc_default_gate -ceq
        'wasmtime_pc_default_rejected') (
        'paired P95 must reject one slow/four fast process evidence')

    $SeedMutationRequest = Get-Content `
        -LiteralPath $TimedRequestPaths[0] `
        -Raw | ConvertFrom-Json
    $SeedMutationRequest.seed = [int]$Profile.seed + 41
    $SeedMutationRequestPath = Join-Path $FixtureRoot (
        'rejected-seed.request.json')
    Write-JsonFile `
        -Value $SeedMutationRequest `
        -Path $SeedMutationRequestPath
    $SeedMutationResult = Get-Content `
        -LiteralPath $TimedPaths[0] `
        -Raw | ConvertFrom-Json
    $SeedMutationResult.request_seed = [int]$SeedMutationRequest.seed
    $SeedMutationResult.request_sha256 = Get-SidecarFileSha256 `
        -Path $SeedMutationRequestPath
    foreach ($Sample in @($SeedMutationResult.samples)) {
        $MutatedSeed = [int]$SeedMutationRequest.seed +
            [int]$SeedMutationRequest.process_run * 1009 +
            [int]$Sample.sample_index * 17
        $MutatedOracle = [AvidScriptControlledOracle]::Run(
            [int]$Sample.iterations,
            $MutatedSeed)
        $Sample.seed = $MutatedSeed
        $Sample.result = $MutatedOracle
        $Sample.expected = $MutatedOracle
    }
    $SeedMutationResultPath = Join-Path $FixtureRoot (
        'rejected-seed.result.json')
    Write-JsonFile `
        -Value $SeedMutationResult `
        -Path $SeedMutationResultPath
    $SeedValidatorRejected = $false
    try {
        & $ValidatorPath `
            -ResultPath $SeedMutationResultPath `
            -RequestPath $SeedMutationRequestPath `
            -ProfilePath $FixtureProfilePath `
            -CalibrationResultPath $CalibrationResultPath | Out-Null
    }
    catch {
        $SeedValidatorRejected = $true
    }
    Assert-True $SeedValidatorRejected (
        'profile seed drift must fail even after coherent oracle/hash rewrite')

    $SeedResultPaths = @($SeedMutationResultPath) + @($TimedPaths[1..4])
    $SeedRequestPaths = @($SeedMutationRequestPath) + @(
        $TimedRequestPaths[1..4])
    $SeedMergerRejected = $false
    try {
        & $MergerPath `
            -ResultPaths $SeedResultPaths `
            -RequestPaths $SeedRequestPaths `
            -CalibrationResultPath $CalibrationResultPath `
            -CalibrationRequestPath $CalibrationRequestPath `
            -ProfilePath $FixtureProfilePath `
            -OutputPath (Join-Path $FixtureRoot (
                'rejected-seed.aggregate.json')) | Out-Null
    }
    catch {
        $SeedMergerRejected = $true
    }
    Assert-True $SeedMergerRejected (
        'merger must independently reject one timed request seed drift')

    $UnequalResult = Get-Content -LiteralPath $TimedPaths[0] -Raw |
        ConvertFrom-Json
    $UnequalResult.samples = @($UnequalResult.samples)[0..118]
    $UnequalPath = Join-Path $FixtureRoot 'rejected-unequal.result.json'
    Write-JsonFile -Value $UnequalResult -Path $UnequalPath
    $UnequalRejected = $false
    try {
        & $ValidatorPath `
            -ResultPath $UnequalPath `
            -RequestPath $TimedRequestPaths[0] `
            -ProfilePath $FixtureProfilePath `
            -CalibrationResultPath $CalibrationResultPath | Out-Null
    }
    catch {
        $UnequalRejected = $true
    }
    Assert-True $UnequalRejected (
        'unequal process/lane sample mutation must be rejected')

    $DuplicateResult = Get-Content -LiteralPath $TimedPaths[0] -Raw |
        ConvertFrom-Json
    $DuplicateResult.samples[4].lane_id =
        [string]$DuplicateResult.samples[0].lane_id
    $DuplicateResult.samples[4].sample_index =
        [int]$DuplicateResult.samples[0].sample_index
    $DuplicatePath = Join-Path $FixtureRoot (
        'rejected-duplicate.result.json')
    Write-JsonFile -Value $DuplicateResult -Path $DuplicatePath
    $DuplicateRejected = $false
    try {
        & $ValidatorPath `
            -ResultPath $DuplicatePath `
            -RequestPath $TimedRequestPaths[0] `
            -ProfilePath $FixtureProfilePath `
            -CalibrationResultPath $CalibrationResultPath | Out-Null
    }
    catch {
        $DuplicateRejected = $true
    }
    Assert-True $DuplicateRejected (
        'duplicate lane/sample index mutation must be rejected')

    $InvalidResult = Get-Content -LiteralPath $TimedPaths[0] -Raw |
        ConvertFrom-Json
    $InvalidResult.samples[0].duration_ns = 0
    $InvalidResult.samples[1].seed = [int]$InvalidResult.samples[1].seed + 1
    $InvalidResult.samples[2].result = [int]$InvalidResult.samples[2].result + 1
    $InvalidPath = Join-Path $FixtureRoot 'rejected-raw.result.json'
    Write-JsonFile -Value $InvalidResult -Path $InvalidPath
    $InvalidRejected = $false
    try {
        & $ValidatorPath `
            -ResultPath $InvalidPath `
            -RequestPath $TimedRequestPaths[0] `
            -ProfilePath $FixtureProfilePath `
            -CalibrationResultPath $CalibrationResultPath | Out-Null
    }
    catch {
        $InvalidRejected = $true
    }
    Assert-True $InvalidRejected (
        'duration/seed/oracle mutation must be rejected independently')
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
    statistics = 'per_process_then_cross_process_paired'
}
