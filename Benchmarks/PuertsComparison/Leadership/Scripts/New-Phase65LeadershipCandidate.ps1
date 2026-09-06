[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CandidateRoot,

    [Parameter(Mandatory = $true)]
    [string]$SourceProjectPath,

    [Parameter(Mandatory = $true)]
    [string]$PuertsPluginPath,

    [Parameter(Mandatory = $true)]
    [string]$EngineRoot,

    [Parameter(Mandatory = $true)]
    [string]$WasmtimeInstallSource,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$LeadershipRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ComparisonRoot = Split-Path -Parent $LeadershipRoot
$RunnerPluginRoot = Split-Path -Parent (Split-Path -Parent $ComparisonRoot)
$CommonPath = Join-Path $ComparisonRoot 'Scripts/PuertsBenchmarkSidecar.Common.ps1'
. $CommonPath

function Resolve-RequiredPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ValidateSet('Leaf', 'Container')][string]$PathType,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Resolved = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $Resolved -PathType $PathType)) {
        throw "ASP65L2000 $Label is missing or has the wrong type: $Resolved"
    }
    return $Resolved
}

function Get-NormalizedTextSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Text = [IO.File]::ReadAllText($Path).Replace("`r`n", "`n").Replace("`r", "`n")
    $Bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

function Invoke-GitValue {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $Output = & git -C $Root @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "ASP65L2001 git failed: git $($Arguments -join ' ')`n$($Output -join [Environment]::NewLine)"
    }
    return ([string]@($Output)[0]).Trim().ToLowerInvariant()
}

$ResolvedCandidateRoot = Resolve-RequiredPath -Path $CandidateRoot -PathType Container -Label 'CandidateRoot'
$ResolvedRunnerPluginRoot = Resolve-RequiredPath -Path $RunnerPluginRoot -PathType Container -Label 'runner plugin root'
if ($ResolvedCandidateRoot -ine $ResolvedRunnerPluginRoot) {
    throw "ASP65L2002 candidate freezer must run from the candidate worktree: candidate=$ResolvedCandidateRoot runner=$ResolvedRunnerPluginRoot"
}
$ResolvedSourceProjectPath = Resolve-RequiredPath -Path $SourceProjectPath -PathType Leaf -Label 'SourceProjectPath'
$ResolvedPuertsPluginPath = Resolve-RequiredPath -Path $PuertsPluginPath -PathType Container -Label 'PuertsPluginPath'
$ResolvedEngineRoot = Resolve-RequiredPath -Path $EngineRoot -PathType Container -Label 'EngineRoot'
$ResolvedWasmtimeInstallSource = Resolve-RequiredPath -Path $WasmtimeInstallSource -PathType Container -Label 'WasmtimeInstallSource'
$ResolvedOutputRoot = [IO.Path]::GetFullPath($OutputRoot)

$GitTopLevel = Invoke-GitValue -Root $ResolvedCandidateRoot -Arguments @('rev-parse', '--show-toplevel')
if ([IO.Path]::GetFullPath($GitTopLevel) -ine $ResolvedCandidateRoot) {
    throw 'ASP65L2003 CandidateRoot must be the AvidScript Git root'
}
$Commit = Invoke-GitValue -Root $ResolvedCandidateRoot -Arguments @('rev-parse', 'HEAD')
$Tree = Invoke-GitValue -Root $ResolvedCandidateRoot -Arguments @('rev-parse', 'HEAD^{tree}')
$Status = & git -C $ResolvedCandidateRoot status --porcelain=v1 --untracked-files=all
if ($LASTEXITCODE -ne 0 -or ($Status -join "`n").Trim().Length -ne 0) {
    throw "ASP65L2004 leadership candidate must be clean: $($Status -join '; ')"
}

$ProtocolPath = Join-Path $LeadershipRoot 'Config/Phase65LeadershipProtocol.json'
$ProtocolSchemaPath = Join-Path $LeadershipRoot 'Schema/Phase65LeadershipProtocol.schema.json'
$CandidateSchemaPath = Join-Path $LeadershipRoot 'Schema/Phase65LeadershipCandidate.schema.json'
$ProtocolRaw = Get-Content -LiteralPath $ProtocolPath -Raw
if (-not ($ProtocolRaw | Test-Json -SchemaFile $ProtocolSchemaPath)) {
    throw 'ASP65L2005 leadership protocol does not satisfy schema v1'
}
$Protocol = $ProtocolRaw | ConvertFrom-Json -Depth 64
foreach ($Input in @($Protocol.tracked_inputs)) {
    $InputPath = Resolve-RequiredPath -Path (Join-Path $ResolvedCandidateRoot ([string]$Input.relative_path)) -PathType Leaf -Label "tracked input $($Input.id)"
    if ((Get-NormalizedTextSha256 -Path $InputPath) -cne [string]$Input.sha256) {
        throw "ASP65L2006 tracked leadership input drifted: $($Input.id)"
    }
}

$WasmtimeLockPath = Join-Path $ResolvedCandidateRoot 'Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.lock.json'
$WasmtimeLock = Get-Content -LiteralPath $WasmtimeLockPath -Raw | ConvertFrom-Json -Depth 32
$WasmtimeDestination = [IO.Path]::GetFullPath((Join-Path $ResolvedCandidateRoot ([string]$WasmtimeLock.install.relative_path)))
$SourceMarkerPath = Join-Path $ResolvedWasmtimeInstallSource ([string]$WasmtimeLock.install.managed_marker_name)
$SourceMarker = Get-Content -LiteralPath (Resolve-RequiredPath -Path $SourceMarkerPath -PathType Leaf -Label 'Wasmtime source marker') -Raw | ConvertFrom-Json -Depth 32
if ([string]$SourceMarker.toolchain_id -cne [string]$WasmtimeLock.toolchain_id -or
    [string]$SourceMarker.compiler_profile -cne [string]$WasmtimeLock.compiler_profile.id -or
    [string]$SourceMarker.patch_sha256 -cne [string]$WasmtimeLock.patch.canonical_sha256) {
    throw 'ASP65L2012 Wasmtime install source does not match the frozen toolchain lock'
}
if (-not (Test-Path -LiteralPath $WasmtimeDestination -PathType Container)) {
    $WasmtimeParent = Split-Path -Parent $WasmtimeDestination
    New-Item -ItemType Directory -Force -Path $WasmtimeParent | Out-Null
    $PublishPath = "$WasmtimeDestination.publish-$([Guid]::NewGuid().ToString('N'))"
    try {
        Copy-Item -LiteralPath $ResolvedWasmtimeInstallSource -Destination $PublishPath -Recurse
        [IO.Directory]::Move($PublishPath, $WasmtimeDestination)
    }
    finally {
        if (Test-Path -LiteralPath $PublishPath -PathType Container) {
            Remove-Item -LiteralPath $PublishPath -Recurse -Force
        }
    }
}
$WasmtimeVerifyText = & (Join-Path $ResolvedCandidateRoot 'Build/BuildAvidScriptWasmtimePerformanceToolchain.ps1') `
    -Mode Verify `
    -RepositoryRoot $ResolvedCandidateRoot
if ($LASTEXITCODE -ne 0) {
    throw 'ASP65L2013 copied Wasmtime performance toolchain failed candidate verification'
}
$WasmtimeEvidence = (($WasmtimeVerifyText -join "`n") | ConvertFrom-Json).evidence

$EditorExecutable = Resolve-RequiredPath `
    -Path (Join-Path $ResolvedEngineRoot ([string]$Protocol.host.editor_relative_path)) `
    -PathType Leaf `
    -Label 'UnrealEditor-Cmd'
$EditorIdentity = Assert-SidecarFormalEditorExecutable `
    -EditorExecutable $EditorExecutable `
    -UeVersion ([string]$Protocol.host.ue_version)
$EngineBuildId = '{0};sha256={1}' -f [string]$Protocol.host.ue_version, [string]$EditorIdentity.sha256

$PuertsLock = Get-Content -LiteralPath (Join-Path $ComparisonRoot 'Config/PuertsDependency.lock.json') -Raw | ConvertFrom-Json -Depth 32
$MarkerPath = Join-Path $ResolvedPuertsPluginPath ([string]$PuertsLock.installation.managed_marker_name)
$PuertsMarker = Get-Content -LiteralPath (Resolve-RequiredPath -Path $MarkerPath -PathType Leaf -Label 'Puerts managed marker') -Raw | ConvertFrom-Json -Depth 32
$PuertsContent = Get-SidecarInstalledPuertsContentDigest `
    -Path $ResolvedPuertsPluginPath `
    -ManagedMarkerName ([string]$PuertsLock.installation.managed_marker_name)
if ([string]$PuertsMarker.source_commit_sha -cne [string]$Protocol.competitors.puerts.source_commit -or
    [string]$PuertsMarker.backend_sha256 -cne [string]$Protocol.competitors.puerts.backend_sha256 -or
    [string]$PuertsMarker.installed_content_sha256 -cne [string]$PuertsContent.content_sha256 -or
    [int]$PuertsMarker.installed_file_count -ne [int]$PuertsContent.file_count) {
    throw 'ASP65L2007 installed Puerts identity differs from the frozen protocol or managed marker'
}

$ProjectResultText = & (Join-Path $ComparisonRoot 'Scripts/New-PuertsBenchmarkProject.ps1') `
    -SourceProjectPath $ResolvedSourceProjectPath `
    -AvidScriptPluginPath $ResolvedCandidateRoot `
    -PuertsPluginPath $ResolvedPuertsPluginPath `
    -HarnessPluginPath (Join-Path $ResolvedCandidateRoot 'Benchmarks/PuertsComparison/AvidScriptPerfHarness') `
    -OutputRoot $ResolvedOutputRoot `
    -ExpectedAvidScriptCommit $Commit `
    -ExpectedAvidScriptTree $Tree
if ($LASTEXITCODE -ne 0) {
    throw 'ASP65L2008 benchmark project creation failed'
}
$ProjectResult = ($ProjectResultText -join "`n") | ConvertFrom-Json
$BenchmarkProjectPath = Resolve-RequiredPath -Path ([string]$ProjectResult.project_path) -PathType Leaf -Label 'generated benchmark project'
$BenchmarkProjectRoot = Split-Path -Parent $BenchmarkProjectPath
Assert-SidecarPuertsProvenance `
    -ProjectPath $BenchmarkProjectPath `
    -PuertsCommit ([string]$Protocol.competitors.puerts.source_commit) `
    -PuertsBackendSha256 ([string]$Protocol.competitors.puerts.backend_sha256)

$CpuNames = @(Get-CimInstance Win32_Processor | ForEach-Object { ([string]$_.Name).Trim() } | Where-Object { $_ })
$CpuModel = [string]::Join(' + ', $CpuNames)
if ([string]::IsNullOrWhiteSpace($CpuModel)) {
    throw 'ASP65L2009 CPU identity is unavailable'
}
$CandidateIdPayload = [Text.UTF8Encoding]::new($false).GetBytes("$Commit`n$Tree`n$($EditorIdentity.sha256)`n$($WasmtimeEvidence.installed_content_sha256)`n$($PuertsContent.content_sha256)`n")
$CandidateId = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($CandidateIdPayload)).ToLowerInvariant().Substring(0, 20)
$Matrices = @(
    [ordered]@{ id = 'ue_micro_six_lane'; status = 'ready_after_build'; reason = '六 lane micro profile 与三种 AvidScript binding mode 已冻结，等待统一构建。' },
    [ordered]@{ id = 'ue_gameplay_six_lane'; status = 'ready_after_build'; reason = 'small/dense gameplay frame 与 data-oriented lane 已冻结，等待统一构建。' },
    [ordered]@{ id = 'identical_wasm_execution'; status = 'ready_after_build'; reason = '同 WASM suite 和 Cranelift/V8 身份已冻结，等待统一构建。' },
    [ordered]@{ id = 'angelscript_same_semantics'; status = 'blocked'; reason = [string]$Protocol.competitors.angelscript.reason }
)
$Candidate = [ordered]@{
    schema_version = 1
    candidate_id = $CandidateId
    created_utc = [DateTimeOffset]::UtcNow.ToString('o')
    protocol = [ordered]@{
        id = [string]$Protocol.protocol_id
        path = $ProtocolPath
        sha256 = Get-NormalizedTextSha256 -Path $ProtocolPath
    }
    candidate = [ordered]@{
        root = $ResolvedCandidateRoot
        commit = $Commit
        tree = $Tree
        clean = $true
    }
    engine = [ordered]@{
        root = $ResolvedEngineRoot
        version = [string]$Protocol.host.ue_version
        editor_executable = $EditorExecutable
        editor_sha256 = [string]$EditorIdentity.sha256
        build_id = $EngineBuildId
    }
    host = [ordered]@{
        cpu = $CpuModel
        logical_processors = [Environment]::ProcessorCount
        os = [Runtime.InteropServices.RuntimeInformation]::OSDescription
    }
    wasmtime = [ordered]@{
        root = [string]$WasmtimeEvidence.install_path
        dll_sha256 = [string]$WasmtimeEvidence.dll_sha256
        installed_content_sha256 = [string]$WasmtimeEvidence.installed_content_sha256
        compiler_profile = [string]$WasmtimeEvidence.compiler_profile
    }
    puerts = [ordered]@{
        root = $ResolvedPuertsPluginPath
        source_commit = [string]$PuertsMarker.source_commit_sha
        backend_sha256 = [string]$PuertsMarker.backend_sha256
        installed_content_sha256 = [string]$PuertsContent.content_sha256
        installed_file_count = [int]$PuertsContent.file_count
    }
    benchmark_project = [ordered]@{
        root = $BenchmarkProjectRoot
        project_path = $BenchmarkProjectPath
        marker_sha256 = Get-SidecarFileSha256 -Path (Join-Path $BenchmarkProjectRoot 'benchmark-project.json')
    }
    matrices = $Matrices
    complete_leadership_claim_ready = $false
}
$CandidateJson = (($Candidate | ConvertTo-Json -Depth 64) -replace "`r`n", "`n") + "`n"
if (-not ($CandidateJson | Test-Json -SchemaFile $CandidateSchemaPath)) {
    throw 'ASP65L2010 generated leadership candidate does not satisfy schema v1'
}
$CandidatePath = Join-Path $BenchmarkProjectRoot 'phase65-leadership-candidate.json'
if (Test-Path -LiteralPath $CandidatePath) {
    throw "ASP65L2011 refusing to overwrite leadership candidate manifest: $CandidatePath"
}
[IO.File]::WriteAllText($CandidatePath, $CandidateJson, [Text.UTF8Encoding]::new($false))

[pscustomobject][ordered]@{
    result = 'phase65_leadership_candidate_frozen'
    candidate_id = $CandidateId
    candidate_path = $CandidatePath
    candidate_sha256 = Get-SidecarFileSha256 -Path $CandidatePath
    project_path = $BenchmarkProjectPath
    commit = $Commit
    tree = $Tree
    complete_leadership_claim_ready = $false
    blocked_matrix = 'angelscript_same_semantics'
} | ConvertTo-Json -Depth 16
