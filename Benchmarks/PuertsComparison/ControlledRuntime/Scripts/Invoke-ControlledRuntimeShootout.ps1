[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectPath,

    [Parameter(Mandatory = $true)]
    [string]$EditorExecutable,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [Parameter(Mandatory = $true)]
    [string]$AvidScriptCommit,

    [Parameter(Mandatory = $true)]
    [string]$AvidScriptTreeSha,

    [Parameter(Mandatory = $true)]
    [bool]$AvidScriptDirty,

    [Parameter(Mandatory = $true)]
    [string]$PuertsCommit,

    [Parameter(Mandatory = $true)]
    [string]$PuertsBackendSha256,

    [string]$ProfilePath = '',

    [string]$KernelId = '',

    [switch]$AllowNonFormalProfile,

    [string[]]$AdditionalEditorArguments = @()
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ControlledRoot = Split-Path -Parent $ScriptRoot
$PuertsComparisonRoot = Split-Path -Parent $ControlledRoot
$SidecarScriptRoot = Join-Path $PuertsComparisonRoot 'Scripts'
$RunnerPluginRoot = Split-Path -Parent (Split-Path -Parent $PuertsComparisonRoot)
. (Join-Path $SidecarScriptRoot 'PuertsBenchmarkSidecar.Common.ps1')

$isSuiteRun = -not [string]::IsNullOrWhiteSpace($KernelId)
$canonicalProfileName = $isSuiteRun ?
    'ControlledRuntimeSuiteProfile.json' :
    'ControlledRuntimeProfile.json'
if ([string]::IsNullOrWhiteSpace($ProfilePath)) {
    $ProfilePath = Join-Path $ControlledRoot "Config/$canonicalProfileName"
}
$CanonicalProfilePath = Join-Path $ControlledRoot "Config/$canonicalProfileName"
$RequestSchemaPath = Join-Path $ControlledRoot 'Schema/ControlledRuntimeRequest.schema.json'
$ResultSchemaPath = Join-Path $ControlledRoot 'Schema/ControlledRuntimeResult.schema.json'
$ValidatorPath = Join-Path $ScriptRoot 'Test-ControlledRuntimeResult.ps1'
$MergerPath = Join-Path $ScriptRoot 'Merge-ControlledRuntimeResults.ps1'

$ResolvedProjectPath = [System.IO.Path]::GetFullPath($ProjectPath)
$ResolvedEditorExecutable = [System.IO.Path]::GetFullPath($EditorExecutable)
$ResolvedOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$ResolvedProfilePath = [System.IO.Path]::GetFullPath($ProfilePath)
if (-not (Test-Path -LiteralPath $ResolvedProjectPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $ResolvedEditorExecutable -PathType Leaf) -or
    -not (Test-Path -LiteralPath $ResolvedProfilePath -PathType Leaf)) {
    throw 'ASP54S4301 project, editor executable, or controlled profile is missing'
}
if ($AvidScriptCommit -cnotmatch '^[0-9a-f]{40}$' -or
    $AvidScriptTreeSha -cnotmatch '^[0-9a-f]{40}$' -or
    $PuertsCommit -cnotmatch '^[0-9a-f]{40}$' -or
    $PuertsBackendSha256 -cnotmatch '^[0-9a-f]{64}$') {
    throw 'ASP54S4302 commit, tree, and Puerts artifact identities must be lowercase fixed digests'
}
if ($AvidScriptDirty) {
    throw 'ASP54S4303 formal controlled runtime shootout rejects a dirty AvidScript candidate'
}
$CanonicalProfileBytes = [System.IO.File]::ReadAllBytes($CanonicalProfilePath)
$SelectedProfileBytes = [System.IO.File]::ReadAllBytes($ResolvedProfilePath)
if (-not $AllowNonFormalProfile -and
    -not [System.Linq.Enumerable]::SequenceEqual(
        [byte[]]$CanonicalProfileBytes,
        [byte[]]$SelectedProfileBytes)) {
    throw 'ASP54S4304 formal shootout requires the tracked controlled runtime profile bytes'
}

$ProjectJunctions = Assert-SidecarBenchmarkProjectProvenance `
    -ProjectPath $ResolvedProjectPath `
    -AvidScriptCommit $AvidScriptCommit `
    -AvidScriptTreeSha $AvidScriptTreeSha
if (-not $AllowNonFormalProfile) {
    Assert-SidecarRunnerCandidate `
        -PluginRoot $RunnerPluginRoot `
        -CandidateRoot ([string]$ProjectJunctions.AvidScript)
}
Assert-SidecarPuertsProvenance `
    -ProjectPath $ResolvedProjectPath `
    -PuertsCommit $PuertsCommit `
    -PuertsBackendSha256 $PuertsBackendSha256
$AvidScriptRuntimeIdentity = Get-SidecarAvidScriptRuntimeIdentity `
    -PluginRoot ([string]$ProjectJunctions.AvidScript)
$ProfileSha256 = Get-SidecarFileSha256 -Path $ResolvedProfilePath
$SuiteProfileSha256 = $isSuiteRun ? $ProfileSha256 : 'not_applicable'
$EngineExecutableSha256 = Get-SidecarFileSha256 -Path $ResolvedEditorExecutable
$EngineVersionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo(
    $ResolvedEditorExecutable)
$EngineVersion = [string]$EngineVersionInfo.ProductVersion
if ([string]::IsNullOrWhiteSpace($EngineVersion)) {
    $EngineVersion = [string]$EngineVersionInfo.FileVersion
}
if ([string]::IsNullOrWhiteSpace($EngineVersion)) {
    throw 'ASP54S4309 Unreal Editor version identity is unavailable'
}
$EngineBuildId = '{0};sha256={1}' -f $EngineVersion, $EngineExecutableSha256

$Profile = Get-Content -LiteralPath $ResolvedProfilePath -Raw | ConvertFrom-Json
$kernelRoot = Join-Path ([string]$ProjectJunctions.AvidScript) (
    'Benchmarks/PuertsComparison/ControlledRuntime/Kernel')
if ($isSuiteRun) {
    $suiteContractPath = Join-Path (
        Join-Path ([string]$ProjectJunctions.AvidScript) 'Benchmarks/PuertsComparison/ControlledRuntime') (
        [string]$Profile.suite_contract)
    $suiteContractSha256 = Get-SidecarFileSha256 -Path $suiteContractPath
    if ($suiteContractSha256 -cne [string]$Profile.suite_contract_sha256) {
        throw 'ASP54S4305 suite contract differs from profile identity'
    }
    $suiteContract = Get-Content -LiteralPath $suiteContractPath -Raw |
        ConvertFrom-Json -Depth 100
    $selectedKernels = @($suiteContract.kernels | Where-Object {
        [string]$_.kernel_id -ceq $KernelId
    })
    if ($selectedKernels.Count -ne 1 -or
        @($Profile.kernel_ids) -cnotcontains $KernelId) {
        throw "ASP54S4305 suite kernel id is not authorized: $KernelId"
    }
    $selectedKernel = $selectedKernels[0]
    $KernelPath = Join-Path $kernelRoot ([string]$selectedKernel.wasm_path)
    $KernelDigest = Get-SidecarFileSha256 -Path $KernelPath
    if ($KernelDigest -cne [string]$selectedKernel.wasm_sha256) {
        throw "ASP54S4305 suite kernel bytes differ: $KernelId"
    }
}
else {
    $KernelPath = Join-Path $kernelRoot 'controlled_runtime_kernel.wasm'
    $KernelContractPath = Join-Path $kernelRoot 'controlled_runtime_kernel.contract.json'
    $KernelContract = Get-Content -LiteralPath $KernelContractPath -Raw | ConvertFrom-Json
    $KernelDigest = Get-SidecarFileSha256 -Path $KernelPath
    if ($KernelDigest -cne [string]$KernelContract.wasm_sha256 -or
        $KernelDigest -cne [string]$Profile.kernel_wasm_sha256) {
        throw 'ASP54S4305 project kernel bytes differ from contract/profile identity'
    }
}

if (-not (Test-Path -LiteralPath $ResolvedOutputRoot)) {
    New-Item -ItemType Directory -Path $ResolvedOutputRoot | Out-Null
}
$AttemptId = [Guid]::NewGuid().ToString()
$AttemptPath = Join-Path $ResolvedOutputRoot (
    'attempt-{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ'), $AttemptId.Replace('-', ''))
New-Item -ItemType Directory -Path $AttemptPath | Out-Null
$RequestsPath = Join-Path $AttemptPath 'requests'
$ResultsPath = Join-Path $AttemptPath 'results'
$LogsPath = Join-Path $AttemptPath 'logs'
New-Item -ItemType Directory -Path $RequestsPath | Out-Null
New-Item -ItemType Directory -Path $ResultsPath | Out-Null
New-Item -ItemType Directory -Path $LogsPath | Out-Null

if ($isSuiteRun) {
    $Profile.benchmark_kind = 'identical_wasm_kernel'
    $Profile | Add-Member -Force -NotePropertyName kernel_contract -NotePropertyValue (
        "Kernel/phase54_suite.contract.json#$KernelId")
    $Profile | Add-Member -Force -NotePropertyName kernel_wasm_sha256 -NotePropertyValue $KernelDigest
    $Profile | Add-Member -Force -NotePropertyName pc_stop_gate -NotePropertyValue ([pscustomobject]@{
        baseline_lane = [string]$Profile.pc_leadership_gate.baseline_lane
        candidate_lane = [string]$Profile.pc_leadership_gate.candidate_lane
        maximum_slowdown_ratio = 1.0
        statistics = @('p50', 'p95')
    })
    $effectiveProfilePath = Join-Path $AttemptPath "$KernelId.effective-profile.json"
    [IO.File]::WriteAllText(
        $effectiveProfilePath,
        ($Profile | ConvertTo-Json -Depth 100) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
    $ResolvedProfilePath = $effectiveProfilePath
    $ProfileSha256 = Get-SidecarFileSha256 -Path $ResolvedProfilePath
}

function Write-Request {
    param(
        [System.Collections.IDictionary]$Request,
        [string]$Path
    )
    $Json = $Request | ConvertTo-Json -Depth 16
    if (-not ($Json | Test-Json -SchemaFile $RequestSchemaPath)) {
        throw 'ASP54S4306 generated controlled request does not match schema v1'
    }
    [System.IO.File]::WriteAllText(
        $Path,
        $Json,
        [System.Text.UTF8Encoding]::new($false))
}

function Invoke-ControlledProcess {
    param(
        [string]$RequestPath,
        [string]$ResultPath,
        [string]$LogStem
    )
    $Arguments = @(
        $ResolvedProjectPath
        '-run=AvidScriptControlledRuntime'
        '-Multiprocess'
        '-NoCompile'
        '-unattended'
        '-NoP4'
        '-NullRHI'
        '-NoSplash'
        '-NoSound'
        '-EnablePlugins=AvidScriptPerfHarness'
        "-AvidScriptControlledRuntimeRequest=$RequestPath"
        "-AvidScriptControlledRuntimeResult=$ResultPath"
    ) + @($AdditionalEditorArguments)
    $StdoutPath = Join-Path $LogsPath "$LogStem.stdout.log"
    $StderrPath = Join-Path $LogsPath "$LogStem.stderr.log"
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $ResolvedEditorExecutable
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Argument in $Arguments) {
        [void]$StartInfo.ArgumentList.Add([string]$Argument)
    }
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    if (-not $Process.Start()) {
        throw 'ASP54S4307 controlled Editor process could not start'
    }
    $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
    $StderrTask = $Process.StandardError.ReadToEndAsync()
    $Process.WaitForExit()
    [System.IO.File]::WriteAllText(
        $StdoutPath,
        $StdoutTask.GetAwaiter().GetResult(),
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText(
        $StderrPath,
        $StderrTask.GetAwaiter().GetResult(),
        [System.Text.UTF8Encoding]::new($false))
    $ExitCode = $Process.ExitCode
    $Process.Dispose()
    if ($ExitCode -ne 0) {
        throw "ASP54S4307 controlled Editor process failed with exit code $ExitCode; logs=$LogStem"
    }
    if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
        throw 'ASP54S4308 controlled Editor process did not publish a result'
    }
}

function New-BaseRequest {
    return [ordered]@{
        schema_version = 1
        benchmark_kind = 'identical_wasm_kernel'
        warmup_samples = [int]$Profile.warmup_samples
        minimum_sample_milliseconds = [double]$Profile.minimum_sample_milliseconds
        minimum_iterations = [int]$Profile.minimum_iterations
        maximum_iterations = [int]$Profile.maximum_iterations
        seed = [int]$Profile.seed
        kernel_wasm_path = $KernelPath
        kernel_wasm_sha256 = $KernelDigest
        puerts_commit = $PuertsCommit
        puerts_backend_sha256 = $PuertsBackendSha256
        target_triple = [string]$Profile.target_triple
        lane_schedule_id = [string]$Profile.lane_schedule_id
        lanes = @($Profile.lanes)
        attempt_id = $AttemptId
        profile_sha256 = $ProfileSha256
        calibration_sha256 = 'not_applicable'
        candidate_commit = $AvidScriptCommit
        candidate_tree_sha = $AvidScriptTreeSha
        candidate_clean = $true
        engine_version = $EngineVersion
        engine_build_id = $EngineBuildId
        engine_executable_sha256 = $EngineExecutableSha256
        wasmtime_runtime_build_identity = [string]$AvidScriptRuntimeIdentity.wasmtime_runtime_build_identity
        wasmtime_runtime_artifact_sha256 = [string]$AvidScriptRuntimeIdentity.wasmtime_dll_sha256
        wamr_runtime_build_identity = [string]$AvidScriptRuntimeIdentity.wamr_runtime_build_identity
        wamr_runtime_artifact_sha256 = [string]$AvidScriptRuntimeIdentity.wamr_static_lib_sha256
        calibration_confirmation_samples = [int]$Profile.calibration_confirmation_samples
    }
}
$CalibrationRequest = New-BaseRequest
$CalibrationRequest['mode'] = 'calibration'
$CalibrationRequest['process_run'] = -1
$CalibrationRequest['timed_samples'] = 0
$CalibrationRequestPath = Join-Path $RequestsPath 'calibration.request.json'
$CalibrationResultPath = Join-Path $ResultsPath 'calibration.result.json'
Write-Request -Request $CalibrationRequest -Path $CalibrationRequestPath
Invoke-ControlledProcess `
    -RequestPath $CalibrationRequestPath `
    -ResultPath $CalibrationResultPath `
    -LogStem 'calibration'
& $ValidatorPath `
    -ResultPath $CalibrationResultPath `
    -RequestPath $CalibrationRequestPath `
    -ProfilePath $ResolvedProfilePath | Out-Null
$Calibration = Get-Content -LiteralPath $CalibrationResultPath -Raw | ConvertFrom-Json
$CalibrationSha256 = Get-SidecarFileSha256 -Path $CalibrationResultPath

$Iterations = [ordered]@{}
foreach ($LaneId in @($Profile.lanes)) {
    $Iterations[[string]$LaneId] = [int]$Calibration.calibration.$LaneId.iterations
}
$TimedResultPaths = @()
for ($ProcessRun = 0; $ProcessRun -lt [int]$Profile.process_runs; ++$ProcessRun) {
    $TimedRequest = New-BaseRequest
    $TimedRequest['mode'] = 'timed'
    $TimedRequest['process_run'] = $ProcessRun
    $TimedRequest['timed_samples'] = [int]$Profile.timed_samples
    $TimedRequest['iterations'] = $Iterations
    $TimedRequest['calibration_sha256'] = $CalibrationSha256
    $TimedRequestPath = Join-Path $RequestsPath ("timed-$ProcessRun.request.json")
    $TimedResultPath = Join-Path $ResultsPath ("timed-$ProcessRun.result.json")
    Write-Request -Request $TimedRequest -Path $TimedRequestPath
    Invoke-ControlledProcess `
        -RequestPath $TimedRequestPath `
        -ResultPath $TimedResultPath `
        -LogStem "timed-$ProcessRun"
    & $ValidatorPath `
        -ResultPath $TimedResultPath `
        -RequestPath $TimedRequestPath `
        -ProfilePath $ResolvedProfilePath `
        -CalibrationResultPath $CalibrationResultPath | Out-Null
    $TimedResultPaths += $TimedResultPath
}
$AggregatePath = Join-Path $AttemptPath 'aggregate.json'
$MergeResult = & $MergerPath `
    -ResultPaths $TimedResultPaths `
    -RequestPaths @(
        0..([int]$Profile.process_runs - 1) |
            ForEach-Object { Join-Path $RequestsPath "timed-$_.request.json" }
    ) `
    -CalibrationResultPath $CalibrationResultPath `
    -CalibrationRequestPath $CalibrationRequestPath `
    -ProfilePath $ResolvedProfilePath `
    -OutputPath $AggregatePath

$Attempt = [ordered]@{
    schema_version = 1
    attempt_id = $AttemptId
    kernel_id = $isSuiteRun ? $KernelId : 'controlled_runtime_kernel'
    profile_path = $ResolvedProfilePath
    profile_sha256 = $ProfileSha256
    suite_profile_sha256 = $SuiteProfileSha256
    suite_contract_sha256 = $isSuiteRun ? $suiteContractSha256 : 'not_applicable'
    calibration_request_sha256 = Get-SidecarFileSha256 -Path $CalibrationRequestPath
    calibration_sha256 = $CalibrationSha256
    calibration_pid = [int]$Calibration.pid
    candidate = [ordered]@{
        commit = $AvidScriptCommit
        tree_sha = $AvidScriptTreeSha
        clean = $true
    }
    engine = [ordered]@{
        version = $EngineVersion
        build_id = $EngineBuildId
        executable_sha256 = $EngineExecutableSha256
    }
    runtimes = [ordered]@{
        puerts_commit = $PuertsCommit
        puerts_backend_sha256 = $PuertsBackendSha256
        wasmtime_build_identity = [string]$AvidScriptRuntimeIdentity.wasmtime_runtime_build_identity
        wasmtime_artifact_sha256 = [string]$AvidScriptRuntimeIdentity.wasmtime_dll_sha256
        wamr_build_identity = [string]$AvidScriptRuntimeIdentity.wamr_runtime_build_identity
        wamr_artifact_sha256 = [string]$AvidScriptRuntimeIdentity.wamr_static_lib_sha256
    }
    kernel_wasm_sha256 = $KernelDigest
    aggregate_sha256 = Get-SidecarFileSha256 -Path $AggregatePath
}
$AttemptJson = $Attempt | ConvertTo-Json -Depth 16
[System.IO.File]::WriteAllText(
    (Join-Path $AttemptPath 'attempt.json'),
    $AttemptJson,
    [System.Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    result = 'controlled_runtime_shootout_complete'
    attempt_id = $AttemptId
    kernel_id = $isSuiteRun ? $KernelId : 'controlled_runtime_kernel'
    attempt_path = $AttemptPath
    aggregate_path = $AggregatePath
    aggregate_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $AggregatePath).Hash.ToLowerInvariant()
    pc_default_gate = [string]$MergeResult.pc_default_gate
    suite_profile_sha256 = $SuiteProfileSha256
    wasmtime_runtime_build_identity = [string]$AvidScriptRuntimeIdentity.WasmtimeRuntimeBuildIdentity
}
