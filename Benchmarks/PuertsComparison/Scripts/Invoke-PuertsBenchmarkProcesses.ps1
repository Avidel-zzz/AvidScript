[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectPath,

    [Parameter(Mandatory = $true)]
    [string]$EditorExecutable,

    [string[]]$EditorPrefixArguments = @(),

    [string[]]$AdditionalEditorArguments = @(),

    [string]$ProfilePath = '',

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [Parameter(Mandatory = $true)]
    [string]$UeVersion,

    [Parameter(Mandatory = $true)]
    [string]$UeBuildId,

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

    [Parameter(Mandatory = $true)]
    [string]$CpuModel,

    [Parameter(Mandatory = $true)]
    [string]$OperatingSystem,

    [Parameter(Mandatory = $true)]
    [string]$WamrMode,

    [Parameter(Mandatory = $true)]
    [string]$V8Mode,

    [Parameter(Mandatory = $true)]
    [string]$WasmSha256,

    [Parameter(Mandatory = $true)]
    [string]$ManifestSha256,

    [switch]$AllowNonFormalProfile,

    [string]$CommandletName = 'AvidScriptPerfRun'
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
$RunnerPluginRoot = Split-Path -Parent (Split-Path -Parent $BenchmarkRoot)
. (Join-Path $ScriptRoot 'PuertsBenchmarkSidecar.Common.ps1')

if ([string]::IsNullOrWhiteSpace($ProfilePath)) {
    $ProfilePath = Join-Path $BenchmarkRoot 'Config/BenchmarkProfile.json'
}
$RequestSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkProcessRequest.schema.json'
$CalibrationSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkCalibration.schema.json'
$ResultSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkProcessResult.schema.json'
$AggregateSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkAggregate.schema.json'
$AggregatorPath = Join-Path $ScriptRoot 'Merge-PuertsBenchmarkResults.ps1'

$ResolvedProjectPath = [System.IO.Path]::GetFullPath($ProjectPath)
$ResolvedEditorExecutable = [System.IO.Path]::GetFullPath($EditorExecutable)
$ResolvedProfilePath = [System.IO.Path]::GetFullPath($ProfilePath)
$CanonicalProfilePath = [System.IO.Path]::GetFullPath((Join-Path $BenchmarkRoot 'Config/BenchmarkProfile.json'))
$ResolvedRequestSchemaPath = [System.IO.Path]::GetFullPath($RequestSchemaPath)
$ResolvedCalibrationSchemaPath = [System.IO.Path]::GetFullPath($CalibrationSchemaPath)
$ResolvedResultSchemaPath = [System.IO.Path]::GetFullPath($ResultSchemaPath)
$ResolvedAggregateSchemaPath = [System.IO.Path]::GetFullPath($AggregateSchemaPath)
$ResolvedOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)

if (-not (Test-Path -LiteralPath $ResolvedProjectPath -PathType Leaf) -or
    [System.IO.Path]::GetExtension($ResolvedProjectPath) -cne '.uproject') {
    throw "ASP53S2100 Unreal 项目文件不存在或扩展名不是 .uproject：$ResolvedProjectPath"
}
if (-not (Test-Path -LiteralPath $ResolvedEditorExecutable -PathType Leaf)) {
    throw "ASP53S2101 UnrealEditor-Cmd 可执行文件不存在：$ResolvedEditorExecutable"
}
foreach ($RequiredFile in @(
    $ResolvedProfilePath,
    $ResolvedRequestSchemaPath,
    $ResolvedCalibrationSchemaPath,
    $ResolvedResultSchemaPath,
    $ResolvedAggregateSchemaPath,
    $AggregatorPath)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "ASP53S2103 benchmark sidecar 输入文件不存在：$RequiredFile"
    }
}
if (@(
        $UeVersion,
        $UeBuildId,
        $CpuModel,
        $OperatingSystem,
        $WamrMode,
        $V8Mode,
        $CommandletName
    ) | Where-Object { [string]::IsNullOrWhiteSpace($_) }) {
    throw 'ASP53S2104 Engine/CPU/OS/runtime mode/Commandlet 固定值不能为空'
}
if ($AvidScriptCommit -cnotmatch '^[0-9a-f]{40}$' -or
    $AvidScriptTreeSha -cnotmatch '^[0-9a-f]{40}$' -or
    $PuertsCommit -cnotmatch '^[0-9a-f]{40}$' -or
    $PuertsBackendSha256 -cnotmatch '^[0-9a-f]{64}$' -or
    $WasmSha256 -cnotmatch '^[0-9a-f]{64}$' -or
    $ManifestSha256 -cnotmatch '^[0-9a-f]{64}$') {
    throw 'ASP53S2105 commit/tree/backend/WASM/manifest 必须是固定的小写摘要'
}
if ($AvidScriptDirty) {
    throw 'ASP53S2113 公平 benchmark 拒绝 dirty AvidScript 工作树'
}

function Test-ReservedEditorArgument {
    param([AllowEmptyString()][string]$Argument)

    $Candidate = $Argument.Trim().Trim('"')
    if ([string]::IsNullOrWhiteSpace($Candidate)) {
        return $false
    }
    if ($Candidate -imatch (
            '^[-/](' +
            '(run|ExecCmds|AvidScriptPerfRequest|AvidScriptPerfResult|AbsLog)(=|:)|' +
            'Multiprocess$|NoCompile$|NullRHI$|' +
            '(D3D11|D3D12|DX11|DX12|Vulkan|OpenGL|Metal|SM5|SM6)$|' +
            '((Dynamic)?RHI)(=|:)|' +
            '(RHIThread|NoRHIThread)$' +
            ')')) {
        return $true
    }
    if ($Candidate -imatch '\.uproject"?$') {
        return $true
    }
    try {
        return [string]::Equals(
            [System.IO.Path]::GetFullPath($Candidate),
            $ResolvedProjectPath,
            [System.StringComparison]::OrdinalIgnoreCase)
    }
    catch {
        return $false
    }
}

foreach ($Argument in @($EditorPrefixArguments) + @($AdditionalEditorArguments)) {
    if (Test-ReservedEditorArgument -Argument ([string]$Argument)) {
        throw "ASP53S2114 Editor 参数不得覆盖 sidecar 保留参数：$Argument"
    }
}
if (@($EditorPrefixArguments).Count -gt 0) {
    $PrefixScriptPath = if (@($EditorPrefixArguments).Count -eq 3) {
        [System.IO.Path]::GetFullPath([string]$EditorPrefixArguments[2])
    }
    else {
        ''
    }
    if (-not $AllowNonFormalProfile -or
        @($EditorPrefixArguments).Count -ne 3 -or
        [string]$EditorPrefixArguments[0] -ine '-NoProfile' -or
        [string]$EditorPrefixArguments[1] -ine '-File' -or
        [System.IO.Path]::GetExtension($PrefixScriptPath) -ine '.ps1' -or
        -not (Test-Path -LiteralPath $PrefixScriptPath -PathType Leaf)) {
        throw 'ASP53S2114 EditorPrefixArguments 仅允许开发合同以 -NoProfile -File <现有.ps1> 启动假 Editor'
    }
}

$Profile = Read-SidecarJson -Path $ResolvedProfilePath -Code 'ASP53S2106'
Test-SidecarProfile -Profile $Profile
if (-not $AllowNonFormalProfile) {
    $CanonicalProfileSha = Get-SidecarFileSha256 -Path $CanonicalProfilePath
    $SelectedProfileSha = Get-SidecarFileSha256 -Path $ResolvedProfilePath
    $ProfilesMatch = $CanonicalProfileSha -ceq $SelectedProfileSha -and
        [System.Linq.Enumerable]::SequenceEqual(
            [byte[]][System.IO.File]::ReadAllBytes($CanonicalProfilePath),
            [byte[]][System.IO.File]::ReadAllBytes($ResolvedProfilePath))
    if (-not $ProfilesMatch) {
        throw 'ASP53S2115 formal benchmark requires the tracked Config/BenchmarkProfile.json bytes and SHA-256; use -AllowNonFormalProfile only for development contracts'
    }
}

$ProjectJunctions = Assert-SidecarBenchmarkProjectProvenance `
    -ProjectPath $ResolvedProjectPath `
    -AvidScriptCommit $AvidScriptCommit `
    -AvidScriptTreeSha $AvidScriptTreeSha
if (-not $AllowNonFormalProfile) {
    Assert-SidecarRunnerCandidate `
        -PluginRoot $RunnerPluginRoot `
        -CandidateRoot ([string]$ProjectJunctions.AvidScript)
    $EditorIdentity = Assert-SidecarFormalEditorExecutable `
        -EditorExecutable $ResolvedEditorExecutable `
        -UeVersion $UeVersion
}
else {
    $EditorIdentity = Get-SidecarEditorIdentity -EditorExecutable $ResolvedEditorExecutable
}
Assert-SidecarPuertsProvenance `
    -ProjectPath $ResolvedProjectPath `
    -PuertsCommit $PuertsCommit `
    -PuertsBackendSha256 $PuertsBackendSha256
Assert-SidecarFormalArtifacts `
    -ProjectPath $ResolvedProjectPath `
    -WasmSha256 $WasmSha256 `
    -ManifestSha256 $ManifestSha256
$RequestSchema = Read-SidecarJson -Path $ResolvedRequestSchemaPath -Code 'ASP53S2107'
$CalibrationSchema = Read-SidecarJson -Path $ResolvedCalibrationSchemaPath -Code 'ASP53S2107'
$ResultSchema = Read-SidecarJson -Path $ResolvedResultSchemaPath -Code 'ASP53S2107'
$AggregateSchema = Read-SidecarJson -Path $ResolvedAggregateSchemaPath -Code 'ASP53S2107'
$RequestSchemaVersion = [int]$RequestSchema.properties.schema_version.const
$CalibrationSchemaVersion = [int]$CalibrationSchema.properties.schema_version.const
$ResultSchemaVersion = [int]$ResultSchema.properties.schema_version.const
$AggregateSchemaVersion = [int]$AggregateSchema.properties.schema_version.const
if ($RequestSchemaVersion -ne 2 -or
    $CalibrationSchemaVersion -ne 2 -or
    $ResultSchemaVersion -ne 2 -or
    $AggregateSchemaVersion -ne 2) {
    throw 'ASP54S2107 request/calibration/result/aggregate Schema 版本不符合 Phase 54 固定合同'
}

if (-not (Test-Path -LiteralPath $ResolvedOutputRoot)) {
    New-Item -ItemType Directory -Path $ResolvedOutputRoot | Out-Null
}
elseif (-not (Test-Path -LiteralPath $ResolvedOutputRoot -PathType Container)) {
    throw "ASP53S2108 attempt 输出根路径不是目录：$ResolvedOutputRoot"
}

$AttemptId = [Guid]::NewGuid().ToString()
$AttemptName = 'attempt-{0}-{1}' -f (
    [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ')),
    $AttemptId.Replace('-', '')
$AttemptPath = Join-Path $ResolvedOutputRoot $AttemptName
New-Item -ItemType Directory -Path $AttemptPath | Out-Null
New-Item -ItemType Directory -Path (Join-Path $AttemptPath 'schemas') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $AttemptPath 'calibration') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $AttemptPath 'runs') | Out-Null

$ProfileSnapshotRelativePath = 'profile.snapshot.json'
$RequestSchemaSnapshotRelativePath = 'schemas/BenchmarkProcessRequest.schema.json'
$CalibrationSchemaSnapshotRelativePath = 'schemas/BenchmarkCalibration.schema.json'
$ResultSchemaSnapshotRelativePath = 'schemas/BenchmarkProcessResult.schema.json'
$AggregateSchemaSnapshotRelativePath = 'schemas/BenchmarkAggregate.schema.json'
$ProfileSnapshotPath = Join-Path $AttemptPath $ProfileSnapshotRelativePath
$RequestSchemaSnapshotPath = Join-Path $AttemptPath $RequestSchemaSnapshotRelativePath
$CalibrationSchemaSnapshotPath = Join-Path $AttemptPath $CalibrationSchemaSnapshotRelativePath
$ResultSchemaSnapshotPath = Join-Path $AttemptPath $ResultSchemaSnapshotRelativePath
$AggregateSchemaSnapshotPath = Join-Path $AttemptPath $AggregateSchemaSnapshotRelativePath
Copy-SidecarNewFile -Source $ResolvedProfilePath -Destination $ProfileSnapshotPath
Copy-SidecarNewFile -Source $ResolvedRequestSchemaPath -Destination $RequestSchemaSnapshotPath
Copy-SidecarNewFile -Source $ResolvedCalibrationSchemaPath -Destination $CalibrationSchemaSnapshotPath
Copy-SidecarNewFile -Source $ResolvedResultSchemaPath -Destination $ResultSchemaSnapshotPath
Copy-SidecarNewFile -Source $ResolvedAggregateSchemaPath -Destination $AggregateSchemaSnapshotPath
$ProfileSha256 = Get-SidecarFileSha256 -Path $ProfileSnapshotPath
$RequestSchemaSha256 = Get-SidecarFileSha256 -Path $RequestSchemaSnapshotPath
$CalibrationSchemaSha256 = Get-SidecarFileSha256 -Path $CalibrationSchemaSnapshotPath
$ResultSchemaSha256 = Get-SidecarFileSha256 -Path $ResultSchemaSnapshotPath
$AggregateSchemaSha256 = Get-SidecarFileSha256 -Path $AggregateSchemaSnapshotPath
$LaneCatalogResolution = New-SidecarResolvedLaneCatalog `
    -Profile $Profile `
    -Tokens @{
        '${ue_version}' = $UeVersion
        '${ue_build_id}' = $UeBuildId
        '${editor_executable_sha256}' = [string]$EditorIdentity.sha256
        '${avidscript_tree_sha}' = $AvidScriptTreeSha
        '${puerts_commit}' = $PuertsCommit
        '${puerts_backend_sha256}' = $PuertsBackendSha256
        '${wasm_sha256}' = $WasmSha256
    }
$LaneCatalog = @($LaneCatalogResolution.entries)
$LaneCatalogSha256 = [string]$LaneCatalogResolution.sha256

$Provenance = [pscustomobject][ordered]@{
    ue_version = $UeVersion
    ue_build_id = $UeBuildId
    editor_executable_sha256 = [string]$EditorIdentity.sha256
    editor_file_version = [string]$EditorIdentity.file_version
    target = [string]$Profile.target
    configuration = [string]$Profile.configuration
    avidscript_commit = $AvidScriptCommit
    puerts_commit = $PuertsCommit
    puerts_backend_sha256 = $PuertsBackendSha256
    null_rhi = [bool]$Profile.null_rhi
    cpu = $CpuModel
    os = $OperatingSystem
    wamr_mode = $WamrMode
    v8_mode = $V8Mode
    wasm_sha256 = $WasmSha256
    manifest_sha256 = $ManifestSha256
    avidscript_tree_sha = $AvidScriptTreeSha
    avidscript_dirty = $false
    profile_id = [string]$Profile.profile_id
    profile_sha256 = $ProfileSha256
    request_schema_sha256 = $RequestSchemaSha256
    calibration_schema_sha256 = $CalibrationSchemaSha256
    result_schema_sha256 = $ResultSchemaSha256
    aggregate_schema_sha256 = $AggregateSchemaSha256
    lane_catalog_sha256 = $LaneCatalogSha256
    allow_non_formal_profile = [bool]$AllowNonFormalProfile
}

function New-BenchmarkRequest {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('calibration', 'timed')][string]$Mode,
        [Parameter(Mandatory = $true)][int]$ProcessRun,
        [Parameter(Mandatory = $true)]$LaneOrder,
        [Parameter(Mandatory = $true)]$IterationCounts,
        [Parameter(Mandatory = $true)][int]$OutputSchemaVersion,
        [Parameter(Mandatory = $true)][string]$OutputSchemaSha256,
        [Parameter(Mandatory = $true)][string]$ResultPath
    )

    return [pscustomobject][ordered]@{
        schema_version = $RequestSchemaVersion
        mode = $Mode
        attempt_id = $AttemptId
        process_run = $ProcessRun
        lane_order = @($LaneOrder)
        lanes = @($Profile.lanes)
        lane_catalog = $LaneCatalog
        lane_catalog_sha256 = $LaneCatalogSha256
        workloads = @($Profile.workloads)
        warmup_samples = [int]$Profile.warmup_samples
        timed_samples = if ($Mode -ceq 'timed') { [int]$Profile.timed_samples } else { 0 }
        minimum_sample_milliseconds = [double]$Profile.minimum_sample_milliseconds
        minimum_iterations = [int64]$Profile.minimum_iterations
        maximum_iterations = [int64]$Profile.maximum_iterations
        seed = [int]$Profile.seed
        iteration_counts = $IterationCounts
        provenance = $Provenance
        result_schema = [pscustomobject][ordered]@{
            version = $OutputSchemaVersion
            sha256 = $OutputSchemaSha256
        }
        result_path = $ResultPath
        result_write = [pscustomobject][ordered]@{
            strategy = 'same_directory_temporary_then_atomic_rename'
            temporary_path = "$ResultPath.$AttemptId.tmp"
            overwrite = $false
        }
    }
}

function Write-ValidatedBenchmarkRequest {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Request,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $RequestJson = (($Request | ConvertTo-Json -Depth 64) -replace "`r`n", "`n") + "`n"
    if (-not ($RequestJson | Test-Json -SchemaFile $RequestSchemaSnapshotPath -ErrorAction SilentlyContinue)) {
        throw "ASP53S2112 生成的 process request 未通过固定 Schema：$Label"
    }
    Write-SidecarNewText -Path $Path -Value $RequestJson
}

function Invoke-BenchmarkEditorProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][int]$ProcessRun,
        [Parameter(Mandatory = $true)][string]$RunPath,
        [Parameter(Mandatory = $true)][string]$RequestPath,
        [Parameter(Mandatory = $true)][string]$ResultPath,
        [Parameter(Mandatory = $true)][string]$ProcessMetadataPath
    )

    $EngineLogPath = Join-Path $RunPath 'UnrealEditor-Cmd.log'
    $StdoutPath = Join-Path $RunPath 'stdout.log'
    $StderrPath = Join-Path $RunPath 'stderr.log'
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $ResolvedEditorExecutable
    $StartInfo.WorkingDirectory = $RunPath
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Argument in @($EditorPrefixArguments)) {
        [void]$StartInfo.ArgumentList.Add([string]$Argument)
    }
    [void]$StartInfo.ArgumentList.Add($ResolvedProjectPath)
    foreach ($Argument in @(
        "-run=$CommandletName",
        '-Multiprocess',
        '-NoCompile',
        '-Unattended',
        '-NoP4',
        '-NoSplash',
        '-NoSound')) {
        [void]$StartInfo.ArgumentList.Add($Argument)
    }
    if ([bool]$Profile.null_rhi) {
        [void]$StartInfo.ArgumentList.Add('-NullRHI')
    }
    [void]$StartInfo.ArgumentList.Add("-AvidScriptPerfRequest=$RequestPath")
    [void]$StartInfo.ArgumentList.Add("-AvidScriptPerfResult=$ResultPath")
    [void]$StartInfo.ArgumentList.Add("-AbsLog=$EngineLogPath")
    foreach ($Argument in @($AdditionalEditorArguments)) {
        [void]$StartInfo.ArgumentList.Add([string]$Argument)
    }

    $StartedUtc = [DateTime]::UtcNow
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    try {
        if (-not $Process.Start()) {
            throw "ASP53S2109 无法启动 Editor Commandlet：$Label"
        }
        $ProcessId = $Process.Id
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        $Process.WaitForExit()
        $Stdout = $StdoutTask.GetAwaiter().GetResult()
        $Stderr = $StderrTask.GetAwaiter().GetResult()
        $ExitCode = $Process.ExitCode
    }
    catch {
        throw "ASP53S2109 启动或等待 Editor Commandlet 失败：$Label detail=$($_.Exception.Message)"
    }
    finally {
        $Process.Dispose()
    }
    $CompletedUtc = [DateTime]::UtcNow
    Write-SidecarNewText -Path $StdoutPath -Value $Stdout
    Write-SidecarNewText -Path $StderrPath -Value $Stderr

    $Metadata = [pscustomobject][ordered]@{
        label = $Label
        process_run = $ProcessRun
        pid = $ProcessId
        exit_code = $ExitCode
        succeeded = ($ExitCode -eq 0)
        started_utc = $StartedUtc.ToString('o')
        completed_utc = $CompletedUtc.ToString('o')
        duration_milliseconds = ($CompletedUtc - $StartedUtc).TotalMilliseconds
    }
    if ($ExitCode -ne 0) {
        Write-SidecarNewJson -Path $ProcessMetadataPath -Value $Metadata
        $StderrSummary = ($Stderr -replace '\s+', ' ').Trim()
        if ($StderrSummary.Length -gt 2048) {
            $StderrSummary = $StderrSummary.Substring(0, 2048)
        }
        throw "ASP53S2110 Editor Commandlet 失败：$Label exit_code=$ExitCode stderr=$StderrSummary，日志位于 $RunPath"
    }
    return $Metadata
}

$CalibrationPath = Join-Path $AttemptPath 'calibration'
$CalibrationRequestPath = Join-Path $CalibrationPath 'request.json'
$CalibrationResultPath = Join-Path $CalibrationPath 'calibration.json'
$CalibrationProcessMetadataPath = Join-Path $CalibrationPath 'process.json'
$CalibrationRequest = New-BenchmarkRequest `
    -Mode calibration `
    -ProcessRun -1 `
    -LaneOrder @($Profile.lanes) `
    -IterationCounts ([pscustomobject]@{}) `
    -OutputSchemaVersion $CalibrationSchemaVersion `
    -OutputSchemaSha256 $CalibrationSchemaSha256 `
    -ResultPath $CalibrationResultPath
Write-ValidatedBenchmarkRequest `
    -Path $CalibrationRequestPath `
    -Request $CalibrationRequest `
    -Label 'calibration'
$CalibrationRequestSha256 = Get-SidecarFileSha256 -Path $CalibrationRequestPath
$CalibrationProcessMetadata = Invoke-BenchmarkEditorProcess `
    -Label 'calibration' `
    -ProcessRun -1 `
    -RunPath $CalibrationPath `
    -RequestPath $CalibrationRequestPath `
    -ResultPath $CalibrationResultPath `
    -ProcessMetadataPath $CalibrationProcessMetadataPath
$ValidatedCalibration = Test-SidecarCalibrationResult `
    -ResultPath $CalibrationResultPath `
    -SchemaPath $CalibrationSchemaSnapshotPath `
    -Profile $Profile `
    -ExpectedProvenance $Provenance `
    -ExpectedLaneCatalog $LaneCatalog `
    -ExpectedLaneCatalogSha256 $LaneCatalogSha256
$Calibration = $ValidatedCalibration.result
$CalibrationProcessMetadata | Add-Member -NotePropertyName calibration_sha256 -NotePropertyValue ([string]$ValidatedCalibration.sha256)
$CalibrationProcessMetadata | Add-Member -NotePropertyName iteration_counts -NotePropertyValue $Calibration.iteration_counts
Write-SidecarNewJson -Path $CalibrationProcessMetadataPath -Value $CalibrationProcessMetadata

$RunEntries = [System.Collections.Generic.List[object]]::new()
for ($ProcessRun = 0; $ProcessRun -lt [int]$Profile.process_runs; ++$ProcessRun) {
    $RunRelativePath = 'runs/{0:D2}' -f $ProcessRun
    $RunPath = Join-Path $AttemptPath $RunRelativePath
    New-Item -ItemType Directory -Path $RunPath | Out-Null
    $LaneOrder = Get-SidecarRotatedLaneOrder -Lanes @($Profile.lanes) -ProcessRun $ProcessRun
    $RequestRelativePath = "$RunRelativePath/request.json"
    $RawResultRelativePath = "$RunRelativePath/raw-result.json"
    $ProcessMetadataRelativePath = "$RunRelativePath/process.json"
    $RequestPath = Join-Path $AttemptPath $RequestRelativePath
    $RawResultPath = Join-Path $AttemptPath $RawResultRelativePath
    $Request = New-BenchmarkRequest `
        -Mode timed `
        -ProcessRun $ProcessRun `
        -LaneOrder $LaneOrder `
        -IterationCounts $Calibration.iteration_counts `
        -OutputSchemaVersion $ResultSchemaVersion `
        -OutputSchemaSha256 $ResultSchemaSha256 `
        -ResultPath $RawResultPath
    Write-ValidatedBenchmarkRequest -Path $RequestPath -Request $Request -Label "process=$ProcessRun"
    $RunEntries.Add([pscustomobject][ordered]@{
        process_run = $ProcessRun
        lane_order = $LaneOrder
        request_path = $RequestRelativePath
        request_sha256 = Get-SidecarFileSha256 -Path $RequestPath
        raw_result_path = $RawResultRelativePath
        process_metadata_path = $ProcessMetadataRelativePath
    })
}

$Manifest = [pscustomobject][ordered]@{
    schema_version = 2
    attempt_id = $AttemptId
    created_utc = [DateTime]::UtcNow.ToString('o')
    project_name = [System.IO.Path]::GetFileNameWithoutExtension($ResolvedProjectPath)
    profile = [pscustomobject][ordered]@{
        id = [string]$Profile.profile_id
        sha256 = $ProfileSha256
        snapshot_path = $ProfileSnapshotRelativePath
        process_runs = [int]$Profile.process_runs
        warmup_samples = [int]$Profile.warmup_samples
        timed_samples = [int]$Profile.timed_samples
    }
    request_schema = [pscustomobject][ordered]@{
        version = $RequestSchemaVersion
        sha256 = $RequestSchemaSha256
        snapshot_path = $RequestSchemaSnapshotRelativePath
    }
    calibration_schema = [pscustomobject][ordered]@{
        version = $CalibrationSchemaVersion
        sha256 = $CalibrationSchemaSha256
        snapshot_path = $CalibrationSchemaSnapshotRelativePath
    }
    result_schema = [pscustomobject][ordered]@{
        version = $ResultSchemaVersion
        sha256 = $ResultSchemaSha256
        snapshot_path = $ResultSchemaSnapshotRelativePath
    }
    aggregate_schema = [pscustomobject][ordered]@{
        version = $AggregateSchemaVersion
        sha256 = $AggregateSchemaSha256
        snapshot_path = $AggregateSchemaSnapshotRelativePath
    }
    lane_catalog = $LaneCatalog
    lane_catalog_sha256 = $LaneCatalogSha256
    provenance = $Provenance
    calibration = [pscustomobject][ordered]@{
        request_path = 'calibration/request.json'
        request_sha256 = $CalibrationRequestSha256
        raw_path = 'calibration/calibration.json'
        process_metadata_path = 'calibration/process.json'
        sha256 = [string]$ValidatedCalibration.sha256
        calibration_id = [string]$Calibration.calibration_id
        timer_frequency_hz = [int64]$Calibration.timer_frequency_hz
        iteration_counts = $Calibration.iteration_counts
    }
    process_runs = @($RunEntries)
}
Write-SidecarNewJson -Path (Join-Path $AttemptPath 'attempt.json') -Value $Manifest

foreach ($RunEntry in @($RunEntries)) {
    $ProcessRun = [int]$RunEntry.process_run
    $RunPath = Resolve-SidecarChildPath -Root $AttemptPath -RelativePath ('runs/{0:D2}' -f $ProcessRun)
    $RequestPath = Resolve-SidecarChildPath -Root $AttemptPath -RelativePath ([string]$RunEntry.request_path)
    $RawResultPath = Resolve-SidecarChildPath -Root $AttemptPath -RelativePath ([string]$RunEntry.raw_result_path)
    $ProcessMetadataPath = Resolve-SidecarChildPath -Root $AttemptPath -RelativePath ([string]$RunEntry.process_metadata_path)
    $ProcessMetadata = Invoke-BenchmarkEditorProcess `
        -Label "timed process=$ProcessRun" `
        -ProcessRun $ProcessRun `
        -RunPath $RunPath `
        -RequestPath $RequestPath `
        -ResultPath $RawResultPath `
        -ProcessMetadataPath $ProcessMetadataPath
    $Validated = Test-SidecarProcessResult `
        -ResultPath $RawResultPath `
        -SchemaPath $ResultSchemaSnapshotPath `
        -Profile $Profile `
        -Manifest $Manifest `
        -ExpectedProcessRun $ProcessRun
    $ProcessMetadata | Add-Member -NotePropertyName raw_result_sha256 -NotePropertyValue ([string]$Validated.sha256)
    $ProcessMetadata | Add-Member -NotePropertyName raw_sample_count -NotePropertyValue ([int]$Validated.sample_count)
    Write-SidecarNewJson -Path $ProcessMetadataPath -Value $ProcessMetadata
}

$AggregationOutput = & $AggregatorPath -AttemptPath $AttemptPath -Mode Aggregate
$Aggregation = $AggregationOutput | ConvertFrom-Json
if ($Aggregation.succeeded -ne $true) {
    throw "ASP53S2111 聚合器未返回 succeeded=true：$AttemptPath"
}

[pscustomobject][ordered]@{
    succeeded = $true
    attempt_id = $AttemptId
    attempt_path = $AttemptPath
    aggregate_path = [string]$Aggregation.aggregate_path
    calibration_process_count = 1
    process_run_count = [int]$Aggregation.process_run_count
    raw_sample_count = [int]$Aggregation.raw_sample_count
    process_statistic_count = [int]$Aggregation.process_statistic_count
    cross_process_statistic_count = [int]$Aggregation.cross_process_statistic_count
    paired_comparison_count = [int]$Aggregation.paired_comparison_count
} | ConvertTo-Json -Depth 8
