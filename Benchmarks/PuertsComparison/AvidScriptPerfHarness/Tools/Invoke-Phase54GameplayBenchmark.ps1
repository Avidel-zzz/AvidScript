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
$comparisonRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $comparisonRoot 'Scripts\PuertsBenchmarkSidecar.Common.ps1')

$expectedLanes = @(
    'native_cpp',
    'puerts_v8_reflection',
    'puerts_v8_static',
    'avidscript_wasmtime_adaptive_semantic',
    'avidscript_wasmtime_generated_s1',
    'avidscript_wasmtime_data_oriented'
)
$gameplayWorkloads = @(
    'gameplay_frame_small',
    'gameplay_frame_dense'
)
$microWorkloads = @(
    'callback_empty',
    'callback_tick',
    'pure_integer',
    'scalar_noop',
    'scalar_add_int32',
    'property_get_set',
    'vector_value',
    'vector_ref_out',
    'object_roundtrip',
    'batch_scalar'
)

$schemaRoot = Join-Path $comparisonRoot 'Profiles'

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
        [string]::IsNullOrWhiteSpace([string]$package.package_hash) -or
        $null -eq $manifest.wasm -or
        [string]::IsNullOrWhiteSpace([string]$manifest.wasm.file) -or
        [string]::IsNullOrWhiteSpace([string]$manifest.wasm.sha256)) {
        throw "Manifest has no auditable binding package identity: $manifestPath"
    }
    $wasmPath = Join-Path $ProjectRoot ([string]$manifest.wasm.file)
    $actualWasmSha256 = Get-SidecarFileSha256 -Path $wasmPath
    if ($actualWasmSha256 -cne [string]$manifest.wasm.sha256) {
        throw "Manifest WASM identity mismatch: $manifestPath"
    }
    return [pscustomobject]@{
        name = [string]$package.package_name
        hash = [string]$package.package_hash
        manifest_path = $manifestPath
        manifest_sha256 = Get-SidecarFileSha256 -Path $manifestPath
        wasm_path = $wasmPath
        wasm_sha256 = $actualWasmSha256
    }
}

function Get-GitText {
    param(
        [string]$Repository,
        [string[]]$Arguments
    )

    $output = @(& git -C $Repository @Arguments)
    if ($LASTEXITCODE -ne 0 -or $output.Count -eq 0) {
        throw "Git identity query failed: $($Arguments -join ' ')"
    }
    return ([string]$output[-1]).Trim()
}

function Resolve-RequestTemplateIdentity {
    param(
        [pscustomobject]$Template,
        [pscustomobject]$Profile,
        [string]$ProjectRoot,
        [string]$EditorPath,
        [pscustomobject]$SemanticPackage,
        [pscustomobject]$GeneratedPackage,
        [pscustomobject]$DataPackage
    )

    $avidscriptRoot = Join-Path $ProjectRoot 'Plugins\AvidScript'
    $harnessRoot = Join-Path $ProjectRoot 'Plugins\AvidScriptPerfHarness'
    $puertsRoot = Join-Path $ProjectRoot 'Plugins\Puerts'
    $puertsMarkerPath = Join-Path $puertsRoot '.avidscript-puerts-install.json'
    $puertsRuntimePath = Join-Path $puertsRoot 'Binaries\Win64\UnrealEditor-JsEnv.dll'
    $reflectionScriptPath = Join-Path $harnessRoot 'Content\JavaScript\reflection.js'
    $staticScriptPath = Join-Path $harnessRoot 'Content\JavaScript\static.js'
    $wasmtimeRuntimePath = Join-Path $avidscriptRoot 'Binaries\Win64\wasmtime.dll'
    $harnessModulePath = Join-Path $harnessRoot (
        'Binaries\Win64\UnrealEditor-AvidScriptPerfHarness.dll')

    $puertsMarker = Read-JsonFile -Path $puertsMarkerPath
    $avidscriptCommit = Get-GitText -Repository $avidscriptRoot -Arguments @('rev-parse', 'HEAD')
    $avidscriptTree = Get-GitText -Repository $avidscriptRoot -Arguments @('rev-parse', 'HEAD^{tree}')
    $dirtyLines = @(& git -C $avidscriptRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) {
        throw 'Git dirty-state query failed.'
    }
    $isDirty = $dirtyLines.Count -ne 0
    if ($Profile.evidence_class -ceq 'formal' -and $isDirty) {
        throw 'Formal Phase54 evidence requires a clean AvidScript candidate.'
    }

    $editorSha256 = Get-SidecarFileSha256 -Path $EditorPath
    $puertsRuntimeSha256 = Get-SidecarFileSha256 -Path $puertsRuntimePath
    $reflectionScriptSha256 = Get-SidecarFileSha256 -Path $reflectionScriptPath
    $staticScriptSha256 = Get-SidecarFileSha256 -Path $staticScriptPath
    $wasmtimeRuntimeSha256 = Get-SidecarFileSha256 -Path $wasmtimeRuntimePath
    $harnessModuleSha256 = Get-SidecarFileSha256 -Path $harnessModulePath

    if ($Profile.evidence_class -ceq 'formal') {
        Assert-SidecarFormalEditorExecutable `
            -EditorExecutable $EditorPath `
            -UeVersion '5.8.0' | Out-Null
        Assert-SidecarBenchmarkProjectProvenance `
            -ProjectPath (Join-Path $projectRoot 'AvidTPSTemplate.uproject') `
            -AvidScriptCommit $avidscriptCommit `
            -AvidScriptTreeSha $avidscriptTree | Out-Null
        Assert-SidecarPuertsProvenance `
            -ProjectPath (Join-Path $projectRoot 'AvidTPSTemplate.uproject') `
            -PuertsCommit ([string]$puertsMarker.source_commit_sha) `
            -PuertsBackendSha256 ([string]$puertsMarker.backend_sha256)
    }

    $catalog = @($Template.lane_catalog)
    $catalog[0].execution_artifact_sha256 = $harnessModuleSha256
    $catalog[0].runtime_build_identity =
        "ue58-editor=$editorSha256;harness=$harnessModuleSha256"
    $catalog[0].runtime_artifact_sha256 = $editorSha256

    foreach ($index in 1, 2) {
        $catalog[$index].runtime_version = [string]$puertsMarker.source_commit_sha
        $catalog[$index].runtime_build_identity = [string]$puertsMarker.installed_content_sha256
        $catalog[$index].runtime_artifact_sha256 = $puertsRuntimeSha256
    }
    $catalog[1].execution_artifact_sha256 = $reflectionScriptSha256
    $catalog[2].execution_artifact_sha256 = $staticScriptSha256

    $packageByLane = [ordered]@{
        avidscript_wasmtime_adaptive_semantic = $SemanticPackage
        avidscript_wasmtime_generated_s1 = $GeneratedPackage
        avidscript_wasmtime_data_oriented = $DataPackage
    }
    foreach ($index in 3, 4, 5) {
        $entry = $catalog[$index]
        $package = $packageByLane[[string]$entry.lane_id]
        $artifact = $Profile.avidscript_artifacts.PSObject.Properties[
            [string]$entry.lane_id
        ].Value
        $entry.source_wasm_sha256 = [string]$package.wasm_sha256
        $entry.execution_artifact_sha256 = [string]$package.wasm_sha256
        $entry.runtime_build_identity =
            "wasmtime-v45.0.0;cranelift=1;opt=speed_and_size;isa=sse42;wasm32_memory_stable=1;dll_sha256=$wasmtimeRuntimeSha256"
        $entry.runtime_artifact_sha256 = $wasmtimeRuntimeSha256
        $entry.manifest_relative_path = [string]$artifact.manifest_relative_path
    }

    foreach ($entry in $catalog) {
        $entry.lane_identity_sha256 = Get-SidecarLaneIdentitySha256 -Entry $entry
    }
    $catalogSha256 = Get-SidecarLaneCatalogSha256 -Catalog $catalog
    $Template.lane_catalog_sha256 = $catalogSha256
    $Template.provenance.ue_version = '5.8.0'
    $Template.provenance.editor_executable_sha256 = $editorSha256
    $Template.provenance.harness_module_sha256 = $harnessModuleSha256
    $Template.provenance.avidscript_commit = $avidscriptCommit
    $Template.provenance.avidscript_tree_sha = $avidscriptTree
    $Template.provenance.avidscript_dirty = $isDirty
    $Template.provenance.puerts_commit = [string]$puertsMarker.source_commit_sha
    $Template.provenance.puerts_runtime_sha256 = $puertsRuntimeSha256
    $Template.provenance.puerts_reflection_script_sha256 = $reflectionScriptSha256
    $Template.provenance.puerts_static_script_sha256 = $staticScriptSha256
    $Template.provenance.wasmtime_runtime_sha256 = $wasmtimeRuntimeSha256
    $Template.provenance.wasm_sha256 = [string]$SemanticPackage.wasm_sha256
    $Template.provenance.manifest_sha256 = [string]$SemanticPackage.manifest_sha256
    $Template.provenance.lane_catalog_sha256 = $catalogSha256
    $Template.provenance.request_schema_sha256 = Get-SidecarFileSha256 -Path (
        Join-Path $schemaRoot 'Phase54SixLaneRequest.schema.json')
    $Template.provenance.calibration_schema_sha256 = Get-SidecarFileSha256 -Path (
        Join-Path $schemaRoot 'Phase54SixLaneCalibration.schema.json')
    $Template.provenance.result_schema_sha256 = Get-SidecarFileSha256 -Path (
        Join-Path $schemaRoot 'Phase54SixLaneProcessResult.schema.json')
    return $Template
}

function New-Request {
    param(
        [pscustomobject]$Template,
        [pscustomobject]$Profile,
        [string]$AttemptId,
        [string]$Mode,
        [int]$ProcessRun,
        [object]$IterationCounts,
        [string]$ResultPath
    )

    $request = $Template | ConvertTo-Json -Depth 100 |
        ConvertFrom-Json -Depth 100
    $request.mode = $Mode
    $request.attempt_id = $AttemptId
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
    $request.callback_result_mode =
        [string]$Profile.callback_result_mode
    $request.iteration_counts = $IterationCounts
    $request.result_path = $ResultPath
    $request.result_write.temporary_path = "$ResultPath.tmp"
    $request.result_schema.sha256 = $Mode -ceq 'calibration' ?
        [string]$request.provenance.calibration_schema_sha256 :
        [string]$request.provenance.result_schema_sha256
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

    $requestSchemaPath = Join-Path $schemaRoot 'Phase54SixLaneRequest.schema.json'
    if (-not ($Request | ConvertTo-Json -Depth 100 |
        Test-Json -SchemaFile $requestSchemaPath)) {
        throw 'Phase54 benchmark request failed schema validation.'
    }
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
    $resultSchemaPath = $Request.mode -ceq 'calibration' ?
        (Join-Path $schemaRoot 'Phase54SixLaneCalibration.schema.json') :
        (Join-Path $schemaRoot 'Phase54SixLaneProcessResult.schema.json')
    if (-not (Get-Content -LiteralPath $ResultPath -Raw |
        Test-Json -SchemaFile $resultSchemaPath)) {
        throw "AvidScriptPerfRun result failed schema validation: $ResultPath"
    }
    $result = Read-JsonFile -Path $ResultPath
    $requestSha256 = Get-SidecarFileSha256 -Path $RequestPath
    $resultAttemptId = $Request.mode -ceq 'calibration' ?
        [string]$result.calibration_id :
        [string]$result.run_id
    if ($resultAttemptId -cne [string]$Request.attempt_id -or
        [string]$result.request_sha256 -cne $requestSha256 -or
        [string]$result.lane_catalog_sha256 -cne
            [string]$Request.lane_catalog_sha256 -or
        [string]$result.provenance.avidscript_commit -cne
            [string]$Request.provenance.avidscript_commit -or
        [string]$result.provenance.avidscript_tree_sha -cne
            [string]$Request.provenance.avidscript_tree_sha -or
        [string]$result.provenance.profile_sha256 -cne
            [string]$Request.provenance.profile_sha256) {
        throw "AvidScriptPerfRun result provenance differs from request: $ResultPath"
    }
    if ($Request.mode -ceq 'timed' -and
        [int]$result.process_run -ne [int]$Request.process_run) {
        throw "AvidScriptPerfRun process identity differs from request: $ResultPath"
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
$expectedWorkloads = @($profile.workloads).Count -eq $gameplayWorkloads.Count ?
    $gameplayWorkloads :
    $microWorkloads
Assert-ExactSequence -Actual @($profile.lanes) -Expected $expectedLanes -Label 'profile lanes'
Assert-ExactSequence -Actual @($profile.workloads) -Expected $expectedWorkloads -Label 'profile workloads'
Assert-ExactSequence `
    -Actual @($template.lane_catalog | ForEach-Object { $_.lane_id }) `
    -Expected $expectedLanes `
    -Label 'request template lane catalog'
if ($profile.evidence_class -ceq 'formal') {
    $profilePrefix = [string]$profile.profile_id -clike 'phase56.*' ?
        'Phase56' :
        'Phase54'
    $canonicalProfileName = @($profile.workloads).Count -eq
        $gameplayWorkloads.Count ?
        "$profilePrefix`Gameplay.formal.json" :
        "$profilePrefix`Micro.formal.json"
    $canonicalProfilePath = Join-Path $schemaRoot $canonicalProfileName
    $canonicalTemplatePath = Join-Path $schemaRoot (
        'Phase54SixLaneRequest.template.json')
    if (-not [Linq.Enumerable]::SequenceEqual(
            [byte[]][IO.File]::ReadAllBytes($canonicalProfilePath),
            [byte[]][IO.File]::ReadAllBytes((Resolve-Path $ProfilePath).Path)) -or
        -not [Linq.Enumerable]::SequenceEqual(
            [byte[]][IO.File]::ReadAllBytes($canonicalTemplatePath),
            [byte[]][IO.File]::ReadAllBytes((Resolve-Path $RequestTemplatePath).Path))) {
        throw 'Formal Phase54 run requires the tracked profile and request template bytes.'
    }
}
$semanticPackage = Get-ManifestBindingPackage `
    -ProjectRoot $projectRoot `
    -ManifestRelativePath ([string]$profile.avidscript_artifacts.avidscript_wasmtime_adaptive_semantic.manifest_relative_path)
$generatedPackage = Get-ManifestBindingPackage `
    -ProjectRoot $projectRoot `
    -ManifestRelativePath ([string]$profile.avidscript_artifacts.avidscript_wasmtime_generated_s1.manifest_relative_path)
$dataPackage = Get-ManifestBindingPackage `
    -ProjectRoot $projectRoot `
    -ManifestRelativePath ([string]$profile.avidscript_artifacts.avidscript_wasmtime_data_oriented.manifest_relative_path)
$template = Resolve-RequestTemplateIdentity `
    -Template $template `
    -Profile $profile `
    -ProjectRoot $projectRoot `
    -EditorPath $resolvedEditor `
    -SemanticPackage $semanticPackage `
    -GeneratedPackage $generatedPackage `
    -DataPackage $dataPackage
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

$attemptId = [guid]::NewGuid().ToString('D')
$calibrationResultPath = Join-Path $resolvedOutput 'calibration.result.json'
$calibrationRequestPath = Join-Path $resolvedOutput 'calibration.request.json'
$calibrationRequest = New-Request `
    -Template $template `
    -Profile $profile `
    -AttemptId $attemptId `
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
        -AttemptId $attemptId `
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
