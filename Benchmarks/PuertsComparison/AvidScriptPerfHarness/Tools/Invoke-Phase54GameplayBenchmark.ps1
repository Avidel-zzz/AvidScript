[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EditorExecutable,

    [Parameter(Mandatory = $true)]
    [string]$ProjectPath,

    [Parameter(Mandatory = $true)]
    [string]$ProfilePath,

    [Parameter(Mandatory = $true)]
    [string]$RequestTemplatePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$expectedLanes = @(
    'native_cpp',
    'puerts_v8_reflection',
    'puerts_v8_static',
    'avidscript_wasmtime_semantic',
    'avidscript_wasmtime_generated_s1',
    'avidscript_wasmtime_data_oriented'
)
$expectedWorkloads = @(
    'gameplay_frame_small',
    'gameplay_frame_dense'
)

function Read-JsonFile {
    param([string]$Path)

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    return Get-Content -LiteralPath $resolved -Raw |
        ConvertFrom-Json -Depth 100
}

function Write-NewJsonFile {
    param(
        [object]$Value,
        [string]$Path
    )

    if (Test-Path -LiteralPath $Path) {
        throw "Refusing to overwrite benchmark sidecar: $Path"
    }
    $json = $Value | ConvertTo-Json -Depth 100
    [IO.File]::WriteAllText(
        $Path,
        $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Assert-ExactSequence {
    param(
        [object[]]$Actual,
        [string[]]$Expected,
        [string]$Label
    )

    if ($Actual.Count -ne $Expected.Count) {
        throw "$Label must contain exactly $($Expected.Count) entries."
    }
    for ($index = 0; $index -lt $Expected.Count; ++$index) {
        if ([string]$Actual[$index] -cne $Expected[$index]) {
            throw "$Label mismatch at index $index."
        }
    }
}

function Get-ManifestBindingPackage {
    param(
        [string]$ProjectRoot,
        [string]$ManifestRelativePath
    )

    $manifestPath = Join-Path `
        (Join-Path $ProjectRoot 'Saved') `
        $ManifestRelativePath
    $manifest = Read-JsonFile -Path $manifestPath
    $package = $manifest.binding_package
    if ($null -eq $package -or
        [string]::IsNullOrWhiteSpace([string]$package.package_name) -or
        [string]::IsNullOrWhiteSpace([string]$package.package_hash)) {
        throw "Manifest has no auditable binding package identity: $manifestPath"
    }
    return [pscustomobject]@{
        name = [string]$package.package_name
        hash = [string]$package.package_hash
    }
}

function New-Request {
    param(
        [pscustomobject]$Template,
        [pscustomobject]$Profile,
        [string]$Mode,
        [int]$ProcessRun,
        [object]$IterationCounts,
        [string]$ResultPath
    )

    $request = $Template | ConvertTo-Json -Depth 100 |
        ConvertFrom-Json -Depth 100
    $request.mode = $Mode
    $request.attempt_id = [guid]::NewGuid().ToString('D')
    $request.process_run = $ProcessRun
    $request.lanes = @($Profile.lanes)
    $request.lane_order = @($Profile.lanes)
    $request.workloads = @($Profile.workloads)
    $request.seed = [int]$Profile.seed
    $request.warmup_samples = $Mode -ceq 'calibration' ?
        0 :
        [int]$Profile.warmup_samples
    $request.timed_samples = $Mode -ceq 'calibration' ?
        0 :
        [int]$Profile.timed_samples
    $request.minimum_sample_milliseconds =
        [double]$Profile.calibration.minimum_sample_milliseconds
    $request.minimum_iterations =
        [int]$Profile.calibration.minimum_iterations
    $request.maximum_iterations =
        [int]$Profile.calibration.maximum_iterations
    $request.calibration_confirmation_samples =
        [int]$Profile.calibration.confirmation_samples
    $request.data_lane_max_crossing_ratio =
        [double]$Profile.validity.data_lane_max_crossing_ratio
    $request.iteration_counts = $IterationCounts
    $request.result_path = $ResultPath
    $request.result_write.temporary_path = "$ResultPath.tmp"
    $request.provenance.profile_id = [string]$Profile.profile_id
    $request.provenance.profile_sha256 =
        (Get-FileHash -LiteralPath $ProfilePath -Algorithm SHA256).
            Hash.ToLowerInvariant()
    $request.provenance.allow_non_formal_profile =
        $Profile.evidence_class -cne 'formal'
    return $request
}

function Invoke-ProcessRequest {
    param(
        [pscustomobject]$Request,
        [string]$RequestPath,
        [string]$ResultPath
    )

    Write-NewJsonFile -Value $Request -Path $RequestPath
    & $EditorExecutable `
        $ProjectPath `
        '-run=AvidScriptPerfRun' `
        "-AvidScriptPerfRequest=$RequestPath" `
        "-AvidScriptPerfResult=$ResultPath" `
        '-unattended' `
        '-nop4' `
        '-nullrhi' `
        '-nosplash'
    if ($LASTEXITCODE -ne 0) {
        throw "AvidScriptPerfRun failed with exit code $LASTEXITCODE."
    }
    if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
        throw "AvidScriptPerfRun did not publish: $ResultPath"
    }
}

$resolvedEditor = (Resolve-Path -LiteralPath $EditorExecutable).Path
$resolvedProject = (Resolve-Path -LiteralPath $ProjectPath).Path
$projectRoot = Split-Path -Parent $resolvedProject
$resolvedOutput = [IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $resolvedOutput -PathType Container)) {
    throw "OutputDirectory must already exist: $resolvedOutput"
}
if (@(Get-ChildItem -LiteralPath $resolvedOutput -Force).Count -ne 0) {
    throw 'OutputDirectory must be empty to preserve one immutable attempt.'
}

$profile = Read-JsonFile -Path $ProfilePath
$template = Read-JsonFile -Path $RequestTemplatePath
Assert-ExactSequence -Actual @($profile.lanes) -Expected $expectedLanes -Label 'profile lanes'
Assert-ExactSequence -Actual @($profile.workloads) -Expected $expectedWorkloads -Label 'profile workloads'
Assert-ExactSequence `
    -Actual @($template.lane_catalog | ForEach-Object { $_.lane_id }) `
    -Expected $expectedLanes `
    -Label 'request template lane catalog'
foreach ($lane in @($profile.avidscript_artifacts.PSObject.Properties.Name)) {
    $catalogEntry = @($template.lane_catalog | Where-Object {
        $_.lane_id -ceq $lane
    })
    $artifact = $profile.avidscript_artifacts.$lane
    if ($catalogEntry.Count -ne 1 -or
        $catalogEntry[0].binding_invocation_mode -cne
            $artifact.binding_invocation_mode -or
        $catalogEntry[0].manifest_relative_path -cne
            $artifact.manifest_relative_path) {
        throw "Request template does not match the profile artifact contract: $lane"
    }
}
$generatedPackage = Get-ManifestBindingPackage `
    -ProjectRoot $projectRoot `
    -ManifestRelativePath ([string]$profile.avidscript_artifacts.avidscript_wasmtime_generated_s1.manifest_relative_path)
$dataPackage = Get-ManifestBindingPackage `
    -ProjectRoot $projectRoot `
    -ManifestRelativePath ([string]$profile.avidscript_artifacts.avidscript_wasmtime_data_oriented.manifest_relative_path)
if ($generatedPackage.name -cne $dataPackage.name -or
    $generatedPackage.hash -cne $dataPackage.hash) {
    throw 'Generated S1 and data-oriented manifests must share one binding package name and hash.'
}
if ($profile.evidence_class -ceq 'formal' -and
    ($profile.process_runs -ne 5 -or
     $profile.warmup_samples -ne 5 -or
     $profile.timed_samples -ne 30)) {
    throw 'Formal profile dimensions must be 5 processes, 5 warmups, and 30 samples.'
}
if ($profile.evidence_class -ceq 'diagnostic' -and $profile.process_runs -ne 1) {
    throw 'Diagnostic profile must use one process.'
}

$calibrationResultPath = Join-Path $resolvedOutput 'calibration.result.json'
$calibrationRequestPath = Join-Path $resolvedOutput 'calibration.request.json'
$calibrationRequest = New-Request `
    -Template $template `
    -Profile $profile `
    -Mode 'calibration' `
    -ProcessRun -1 `
    -IterationCounts ([pscustomobject]@{}) `
    -ResultPath $calibrationResultPath
Invoke-ProcessRequest `
    -Request $calibrationRequest `
    -RequestPath $calibrationRequestPath `
    -ResultPath $calibrationResultPath

$calibration = Read-JsonFile -Path $calibrationResultPath
foreach ($workload in $expectedWorkloads) {
    if ($calibration.iteration_counts.PSObject.Properties.Name -notcontains $workload) {
        throw "Calibration result is missing workload: $workload"
    }
    Assert-ExactSequence `
        -Actual @($calibration.iteration_counts.$workload.PSObject.Properties.Name) `
        -Expected $expectedLanes `
        -Label "calibration lanes for $workload"
}

for ($processRun = 0; $processRun -lt [int]$profile.process_runs; ++$processRun) {
    $resultPath = Join-Path $resolvedOutput (
        'process-{0:D2}.result.json' -f $processRun)
    $requestPath = Join-Path $resolvedOutput (
        'process-{0:D2}.request.json' -f $processRun)
    $request = New-Request `
        -Template $template `
        -Profile $profile `
        -Mode 'timed' `
        -ProcessRun $processRun `
        -IterationCounts $calibration.iteration_counts `
        -ResultPath $resultPath
    Invoke-ProcessRequest `
        -Request $request `
        -RequestPath $requestPath `
        -ResultPath $resultPath
}

[pscustomobject]@{
    profile_id = [string]$profile.profile_id
    evidence_class = [string]$profile.evidence_class
    calibration_processes = 1
    timed_processes = [int]$profile.process_runs
    warmup_samples = [int]$profile.warmup_samples
    timed_samples = [int]$profile.timed_samples
    output_policy = 'external_raw_evidence'
}
