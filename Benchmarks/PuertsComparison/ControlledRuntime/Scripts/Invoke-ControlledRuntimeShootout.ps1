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

if ([string]::IsNullOrWhiteSpace($ProfilePath)) {
    $ProfilePath = Join-Path $ControlledRoot 'Config/ControlledRuntimeProfile.json'
}
$CanonicalProfilePath = Join-Path $ControlledRoot 'Config/ControlledRuntimeProfile.json'
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

$KernelPath = Join-Path ([string]$ProjectJunctions.AvidScript) (
    'Benchmarks/PuertsComparison/ControlledRuntime/Kernel/controlled_runtime_kernel.wasm')
$KernelContractPath = Join-Path ([string]$ProjectJunctions.AvidScript) (
    'Benchmarks/PuertsComparison/ControlledRuntime/Kernel/controlled_runtime_kernel.contract.json')
$KernelContract = Get-Content -LiteralPath $KernelContractPath -Raw | ConvertFrom-Json
$KernelDigest = (Get-FileHash -Algorithm SHA256 -LiteralPath $KernelPath).Hash.ToLowerInvariant()
$Profile = Get-Content -LiteralPath $ResolvedProfilePath -Raw | ConvertFrom-Json
if ($KernelDigest -cne [string]$KernelContract.wasm_sha256 -or
    $KernelDigest -cne [string]$Profile.kernel_wasm_sha256) {
    throw 'ASP54S4305 project kernel bytes differ from contract/profile identity'
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
New-Item -ItemType Directory -Path $RequestsPath | Out-Null
New-Item -ItemType Directory -Path $ResultsPath | Out-Null

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
        [string]$ResultPath
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
    & $ResolvedEditorExecutable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "ASP54S4307 controlled Editor process failed with exit code $LASTEXITCODE"
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
    }
}
$CalibrationRequest = New-BaseRequest
$CalibrationRequest.mode = 'calibration'
$CalibrationRequest.process_run = -1
$CalibrationRequest.timed_samples = 0
$CalibrationRequestPath = Join-Path $RequestsPath 'calibration.request.json'
$CalibrationResultPath = Join-Path $ResultsPath 'calibration.result.json'
Write-Request -Request $CalibrationRequest -Path $CalibrationRequestPath
Invoke-ControlledProcess -RequestPath $CalibrationRequestPath -ResultPath $CalibrationResultPath
& $ValidatorPath -ResultPath $CalibrationResultPath | Out-Null
$Calibration = Get-Content -LiteralPath $CalibrationResultPath -Raw | ConvertFrom-Json

$Iterations = [ordered]@{}
foreach ($LaneId in @($Profile.lanes)) {
    $Iterations[[string]$LaneId] = [int]$Calibration.calibration.$LaneId.iterations
}
$TimedResultPaths = @()
for ($ProcessRun = 0; $ProcessRun -lt [int]$Profile.process_runs; ++$ProcessRun) {
    $TimedRequest = New-BaseRequest
    $TimedRequest.mode = 'timed'
    $TimedRequest.process_run = $ProcessRun
    $TimedRequest.timed_samples = [int]$Profile.timed_samples
    $TimedRequest.iterations = $Iterations
    $TimedRequestPath = Join-Path $RequestsPath ("timed-$ProcessRun.request.json")
    $TimedResultPath = Join-Path $ResultsPath ("timed-$ProcessRun.result.json")
    Write-Request -Request $TimedRequest -Path $TimedRequestPath
    Invoke-ControlledProcess -RequestPath $TimedRequestPath -ResultPath $TimedResultPath
    & $ValidatorPath `
        -ResultPath $TimedResultPath `
        -ExpectedTimedSamples ([int]$Profile.timed_samples) | Out-Null
    $TimedResultPaths += $TimedResultPath
}
$AggregatePath = Join-Path $AttemptPath 'aggregate.json'
$MergeResult = & $MergerPath `
    -ResultPaths $TimedResultPaths `
    -ProfilePath $ResolvedProfilePath `
    -OutputPath $AggregatePath

[pscustomobject]@{
    result = 'controlled_runtime_shootout_complete'
    attempt_id = $AttemptId
    attempt_path = $AttemptPath
    aggregate_path = $AggregatePath
    aggregate_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $AggregatePath).Hash.ToLowerInvariant()
    pc_default_gate = [string]$MergeResult.pc_default_gate
    wasmtime_runtime_build_identity = [string]$AvidScriptRuntimeIdentity.WasmtimeRuntimeBuildIdentity
}
