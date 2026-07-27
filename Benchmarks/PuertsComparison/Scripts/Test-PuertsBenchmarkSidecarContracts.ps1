[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
$RunnerPath = Join-Path $ScriptRoot 'Invoke-PuertsBenchmarkProcesses.ps1'
$AggregatorPath = Join-Path $ScriptRoot 'Merge-PuertsBenchmarkResults.ps1'
$CommonPath = Join-Path $ScriptRoot 'PuertsBenchmarkSidecar.Common.ps1'
$RequestSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkProcessRequest.schema.json'
$CalibrationSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkCalibration.schema.json'
$RawSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkProcessResult.schema.json'
$AggregateSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkAggregate.schema.json'
$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('AvidScriptP53SidecarTest-' + [Guid]::NewGuid().ToString('N'))

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "ASP53ST1000 $Message"
    }
}
function Assert-Near {
    param(
        [Parameter(Mandatory = $true)][double]$Actual,
        [Parameter(Mandatory = $true)][double]$Expected,
        [double]$Tolerance = 0.000001,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ([Math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "ASP53ST1001 $Message，实际值=$Actual，期望值=$Expected"
    }
}

function Write-NewJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $Json = (($Value | ConvertTo-Json -Depth 64) -replace "`r`n", "`n") + "`n"
    [System.IO.File]::WriteAllText($Path, $Json, [System.Text.UTF8Encoding]::new($false))
}

function Get-TestFileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TestCanonicalTextSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Text = [System.IO.File]::ReadAllText($Path)
    $CanonicalText = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($CanonicalText)
    $Hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [Convert]::ToHexString($Hasher.ComputeHash($Bytes)).ToLowerInvariant()
    }
    finally {
        $Hasher.Dispose()
    }
}

function Get-TestDirectoryDigest {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Root = [System.IO.Path]::GetFullPath($Path).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $Entries = @(
        Get-ChildItem -LiteralPath $Root -File -Force -Recurse |
            ForEach-Object {
                $Relative = [System.IO.Path]::GetRelativePath($Root, $_.FullName).Replace('\', '/')
                '{0}`t{1}`t{2}' -f $Relative, [int64]$_.Length, (Get-TestFileSha256 $_.FullName)
            } |
            Sort-Object -CaseSensitive
    )
    $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes(([string]::Join("`n", $Entries)) + "`n")
    $Hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [pscustomobject]@{
            content_sha256 = ([System.BitConverter]::ToString($Hasher.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
            file_count = $Entries.Count
        }
    }
    finally {
        $Hasher.Dispose()
    }
}

function Invoke-TestGit {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryPath,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )

    $Output = & git -C $RepositoryPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "ASP53ST1003 git failed: git -C $RepositoryPath $($Arguments -join ' ')`n$($Output -join [Environment]::NewLine)"
    }
    return @($Output)
}

function New-TestJunction {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Target
    )

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null
}

function Invoke-ExpectedFailure {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$ExpectedCode
    )

    try {
        & $Action
        throw "ASP53ST1002 预期失败但命令成功：$ExpectedCode"
    }
    catch {
        Assert-True ($_.Exception.Message.Contains($ExpectedCode)) "失败信息未包含 $ExpectedCode：$($_.Exception.Message)"
    }
}

function Assert-SchemaRejectsNoncanonicalCatalog {
    param(
        [Parameter(Mandatory = $true)][string]$DocumentPath,
        [Parameter(Mandatory = $true)][string]$SchemaPath,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Original = Get-Content -LiteralPath $DocumentPath -Raw | ConvertFrom-Json
    $Swapped = Get-Content -LiteralPath $DocumentPath -Raw | ConvertFrom-Json
    $Swapped.lane_catalog = @(
        $Original.lane_catalog[1],
        $Original.lane_catalog[0],
        $Original.lane_catalog[2],
        $Original.lane_catalog[3],
        $Original.lane_catalog[4])
    Assert-True (-not (($Swapped | ConvertTo-Json -Depth 64) |
            Test-Json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue)) `
        "$Label schema 接受了交换后的 lane_catalog"

    $Duplicated = Get-Content -LiteralPath $DocumentPath -Raw | ConvertFrom-Json
    $Duplicated.lane_catalog = @(
        $Original.lane_catalog[0],
        $Original.lane_catalog[0],
        $Original.lane_catalog[2],
        $Original.lane_catalog[3],
        $Original.lane_catalog[4])
    Assert-True (-not (($Duplicated | ConvertTo-Json -Depth 64) |
            Test-Json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue)) `
        "$Label schema 接受了重复的 lane_catalog"
}

function Copy-AttemptFixture {
    param(
        [Parameter(Mandatory = $true)][string]$SourceAttempt,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $Candidate = Join-Path $FixtureRoot $Name
    Copy-Item -LiteralPath $SourceAttempt -Destination $Candidate -Recurse
    $ManifestPath = Join-Path $Candidate 'attempt.json'
    $Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json

    $CalibrationRequestPath = Join-Path $Candidate $Manifest.calibration.request_path
    $CalibrationRequest = Get-Content -LiteralPath $CalibrationRequestPath -Raw | ConvertFrom-Json
    $CalibrationResultPath = Join-Path $Candidate $Manifest.calibration.raw_path
    $CalibrationRequest.result_path = $CalibrationResultPath
    $CalibrationRequest.result_write.temporary_path = "$CalibrationResultPath.$($Manifest.attempt_id).tmp"
    Write-NewJson $CalibrationRequestPath $CalibrationRequest
    $Manifest.calibration.request_sha256 = Get-TestFileSha256 $CalibrationRequestPath

    foreach ($RunEntry in @($Manifest.process_runs)) {
        $RequestPath = Join-Path $Candidate $RunEntry.request_path
        $Request = Get-Content -LiteralPath $RequestPath -Raw | ConvertFrom-Json
        $ResultPath = Join-Path $Candidate $RunEntry.raw_result_path
        $Request.result_path = $ResultPath
        $Request.result_write.temporary_path = "$ResultPath.$($Manifest.attempt_id).tmp"
        Write-NewJson $RequestPath $Request
        $RunEntry.request_sha256 = Get-TestFileSha256 $RequestPath
    }
    Write-NewJson $ManifestPath $Manifest
    return $Candidate
}

function Copy-AndMutateRawResult {
    param(
        [Parameter(Mandatory = $true)][string]$SourceAttempt,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Mutation
    )

    $Candidate = Copy-AttemptFixture -SourceAttempt $SourceAttempt -Name $Name
    $RawPath = Join-Path $Candidate 'runs/02/raw-result.json'
    $Raw = Get-Content -LiteralPath $RawPath -Raw | ConvertFrom-Json
    & $Mutation $Raw
    Write-NewJson $RawPath $Raw
    return $Candidate
}

foreach ($RequiredPath in @(
    $RunnerPath,
    $AggregatorPath,
    $CommonPath,
    $RequestSchemaPath,
    $CalibrationSchemaPath,
    $RawSchemaPath,
    $AggregateSchemaPath)) {
    Assert-True (Test-Path -LiteralPath $RequiredPath -PathType Leaf) "缺少 P53.3 文件：$RequiredPath"
}

$ParserErrors = $null
$Tokens = $null
foreach ($ScriptPath in @($RunnerPath, $AggregatorPath, $CommonPath)) {
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $ScriptPath,
        [ref]$Tokens,
        [ref]$ParserErrors)
    Assert-True (@($ParserErrors).Count -eq 0) "PowerShell 解析失败：$ScriptPath"
}
. $CommonPath

$TrackedProfile = Get-Content -LiteralPath (Join-Path $BenchmarkRoot 'Config/BenchmarkProfile.json') -Raw | ConvertFrom-Json
$TrackedLockPath = Join-Path $BenchmarkRoot 'Config/PuertsDependency.lock.json'
$TrackedLock = Get-Content -LiteralPath $TrackedLockPath -Raw | ConvertFrom-Json
$TrackedLockSha256 = Get-TestCanonicalTextSha256 $TrackedLockPath
Assert-True ([int]$TrackedProfile.process_runs -eq 5) '正式 profile 必须固定执行 5 个独立进程'
Assert-True ([int]$TrackedProfile.warmup_samples -eq 5) '正式 profile 必须固定 5 次预热'
Assert-True ([int]$TrackedProfile.timed_samples -eq 30) '正式 profile 必须固定 30 个计时样本'
Assert-Near ([double]$TrackedProfile.minimum_sample_milliseconds) 5.0 -Message '正式 profile 必须固定最短样本时间为 5.0 ms'
$RequestSchema = Get-Content -LiteralPath $RequestSchemaPath -Raw | ConvertFrom-Json
Assert-True ([int]$RequestSchema.properties.result_schema.properties.version.const -eq 2) 'request result_schema.version 必须固定为 const 2'

try {
    New-Item -ItemType Directory -Path $FixtureRoot | Out-Null
    $ProjectPath = Join-Path $FixtureRoot 'SidecarFixture.uproject'
    [System.IO.File]::WriteAllText($ProjectPath, "{}`n", [System.Text.UTF8Encoding]::new($false))
    $SourceTarget = Join-Path $FixtureRoot 'source-target'
    $ConfigTarget = Join-Path $FixtureRoot 'config-target'
    $CandidateTarget = Join-Path $FixtureRoot 'candidate-target'
    $PuertsTarget = Join-Path $FixtureRoot 'puerts-target'
    $HarnessTarget = Join-Path $FixtureRoot 'harness-target'
    [System.IO.Directory]::CreateDirectory($SourceTarget) | Out-Null
    [System.IO.Directory]::CreateDirectory($ConfigTarget) | Out-Null
    [System.IO.Directory]::CreateDirectory($CandidateTarget) | Out-Null
    [System.IO.Directory]::CreateDirectory($PuertsTarget) | Out-Null
    [System.IO.Directory]::CreateDirectory($HarnessTarget) | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $SourceTarget 'fixture.cpp'), "fixture`n", [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $ConfigTarget 'DefaultEngine.ini'), "[fixture]`n", [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $CandidateTarget 'AvidScript.uplugin'), "{}`n", [System.Text.UTF8Encoding]::new($false))
    $FixtureWamrLibrary = Join-Path $CandidateTarget 'Source/ThirdParty/WAMR/lib/Win64/Release/iwasm.lib'
    $FixtureWasmtimeDll = Join-Path $CandidateTarget 'Binaries/Win64/wasmtime.dll'
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $FixtureWamrLibrary)) | Out-Null
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $FixtureWasmtimeDll)) | Out-Null
    [System.IO.File]::WriteAllText($FixtureWamrLibrary, 'fixture-wamr-runtime', [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($FixtureWasmtimeDll, 'fixture-wasmtime-runtime', [System.Text.UTF8Encoding]::new($false))
    Invoke-TestGit -RepositoryPath $CandidateTarget init --quiet | Out-Null
    Invoke-TestGit -RepositoryPath $CandidateTarget config user.name 'Codex Fixture' | Out-Null
    Invoke-TestGit -RepositoryPath $CandidateTarget config user.email 'fixture@example.invalid' | Out-Null
    Invoke-TestGit -RepositoryPath $CandidateTarget add . | Out-Null
    Invoke-TestGit -RepositoryPath $CandidateTarget commit --quiet -m fixture | Out-Null
    $CandidateCommit = ([string](@(Invoke-TestGit -RepositoryPath $CandidateTarget rev-parse HEAD)[0])).Trim().ToLowerInvariant()
    $CandidateTree = ([string](@(Invoke-TestGit -RepositoryPath $CandidateTarget rev-parse 'HEAD^{tree}')[0])).Trim().ToLowerInvariant()
    [System.IO.File]::WriteAllText((Join-Path $PuertsTarget 'Puerts.uplugin'), "{}`n", [System.Text.UTF8Encoding]::new($false))
    $BackendPath = Join-Path $PuertsTarget 'Backend.bin'
    [System.IO.File]::WriteAllText($BackendPath, 'backend', [System.Text.UTF8Encoding]::new($false))
    $PuertsCommit = [string]$TrackedLock.source.commit_sha
    $PuertsBackendSha = [string]$TrackedLock.backend.sha256
    $NestedBinariesPath = Join-Path $PuertsTarget 'ThirdParty/Test/Binaries/nested.bin'
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $NestedBinariesPath)) | Out-Null
    [System.IO.File]::WriteAllText($NestedBinariesPath, 'nested-binary', [System.Text.UTF8Encoding]::new($false))
    $TopLevelBinariesPath = Join-Path $PuertsTarget 'Binaries/ignored.bin'
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $TopLevelBinariesPath)) | Out-Null
    [System.IO.File]::WriteAllText($TopLevelBinariesPath, 'ignored-binary', [System.Text.UTF8Encoding]::new($false))
    $GeneratedCSharpObjPath = Join-Path $PuertsTarget 'Source/CSharpParamDefaultValueMetas/obj/fixture.assets.json'
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $GeneratedCSharpObjPath)) | Out-Null
    [System.IO.File]::WriteAllText($GeneratedCSharpObjPath, 'generated-msbuild-state', [System.Text.UTF8Encoding]::new($false))
    $ManagedMarkerPath = Join-Path $PuertsTarget '.avidscript-puerts-install.json'
    $ManagedMarker = [ordered]@{
        schema_version = 2
        source_commit_sha = $PuertsCommit
        source_plugin_tree_sha1 = [string]$TrackedLock.source.plugin_tree_sha1
        source_repository_url = [string]$TrackedLock.source.repository_url
        backend_sha256 = $PuertsBackendSha
        backend_asset_name = [string]$TrackedLock.backend.asset_name
        lock_sha256 = $TrackedLockSha256
        installed_content_sha256 = ('0' * 64)
        installed_file_count = 0
    }
    Write-NewJson $ManagedMarkerPath $ManagedMarker
    $InstalledPuertsDigest = Get-SidecarInstalledPuertsContentDigest `
        -Path $PuertsTarget `
        -ManagedMarkerName '.avidscript-puerts-install.json'
    $ManagedMarker.installed_content_sha256 = $InstalledPuertsDigest.content_sha256
    $ManagedMarker.installed_file_count = $InstalledPuertsDigest.file_count
    Write-NewJson $ManagedMarkerPath $ManagedMarker
    Assert-True ([int]$InstalledPuertsDigest.file_count -eq 3) 'Puerts digest must include nested ThirdParty/Test/Binaries content and exclude only known generated output'
    [System.IO.File]::WriteAllText((Join-Path $HarnessTarget 'AvidScriptPerfHarness.uplugin'), "{}`n", [System.Text.UTF8Encoding]::new($false))
    New-TestJunction -Path (Join-Path $FixtureRoot 'Source') -Target $SourceTarget
    New-TestJunction -Path (Join-Path $FixtureRoot 'Config') -Target $ConfigTarget
    New-TestJunction -Path (Join-Path $FixtureRoot 'Plugins/AvidScript') -Target $CandidateTarget
    New-TestJunction -Path (Join-Path $FixtureRoot 'Plugins/Puerts') -Target $PuertsTarget
    New-TestJunction -Path (Join-Path $FixtureRoot 'Plugins/AvidScriptPerfHarness') -Target $HarnessTarget
    $ArtifactRoot = Join-Path $FixtureRoot 'Saved/AvidScriptCSharpGuest/Profiles/profile_phase53_perf'
    New-Item -ItemType Directory -Force -Path $ArtifactRoot | Out-Null
    $WasmPath = Join-Path $ArtifactRoot 'profile_phase53_perf.wasm'
    $ManifestPath = Join-Path $ArtifactRoot 'profile_phase53_perf.avidscript.json'
    [System.IO.File]::WriteAllBytes($WasmPath, [byte[]](0,97,115,109,1,0,0,0))
    [System.IO.File]::WriteAllText($ManifestPath, "{}`n", [System.Text.UTF8Encoding]::new($false))
    $SourceDigest = Get-TestDirectoryDigest $SourceTarget
    $ConfigDigest = Get-TestDirectoryDigest $ConfigTarget
    Write-NewJson (Join-Path $FixtureRoot 'benchmark-project.json') ([ordered]@{
            schema_version = 2
            project_filename = 'SidecarFixture.uproject'
            candidate_commit = $CandidateCommit
            candidate_tree = $CandidateTree
            source = [ordered]@{ canonical_path = $SourceTarget; content_sha256 = $SourceDigest.content_sha256; file_count = $SourceDigest.file_count }
            config = [ordered]@{ canonical_path = $ConfigTarget; content_sha256 = $ConfigDigest.content_sha256; file_count = $ConfigDigest.file_count }
            junctions = [ordered]@{
                Source = $SourceTarget
                Config = $ConfigTarget
                AvidScript = $CandidateTarget
                Puerts = $PuertsTarget
                AvidScriptPerfHarness = $HarnessTarget
            }
        })

    $ProfilePath = Join-Path $FixtureRoot 'profile.json'
    $Profile = Get-Content -LiteralPath (
        Join-Path $BenchmarkRoot 'Config/BenchmarkProfile.json') -Raw | ConvertFrom-Json
    $Profile.profile_id = 'sidecar-contract'
    $Profile.target = 'SidecarFixtureEditor'
    $Profile.process_runs = 5
    $Profile.warmup_samples = 1
    $Profile.timed_samples = 3
    $Profile.minimum_sample_milliseconds = 0.1
    $Profile.minimum_iterations = 100
    $Profile.maximum_iterations = 100000
    $Profile.workloads = @('scalar_noop', 'scalar_add_int32', 'property_get_set')
    Write-NewJson $ProfilePath $Profile

    $FakeEditorPath = Join-Path $FixtureRoot 'Fake-UnrealEditor-Cmd.ps1'
    $FakeEditor = @'
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$EditorArguments
)

$ErrorActionPreference = 'Stop'
function Get-NativeSwitchValue {
    param([Parameter(Mandatory = $true)][string]$Prefix)

    for ($Index = 0; $Index -lt $EditorArguments.Count; ++$Index) {
        if (-not $EditorArguments[$Index].StartsWith($Prefix)) {
            continue
        }
        $Value = $EditorArguments[$Index].Substring($Prefix.Length)
        # pwsh 的脚本参数绑定器会把 -Key=C:\Path 拆成 C 与 \Path；真实 native Editor 不会。
        if ($Value -cmatch '^[A-Za-z]$' -and
            $Index + 1 -lt $EditorArguments.Count -and
            $EditorArguments[$Index + 1].StartsWith('\')) {
            return $Value + ':' + $EditorArguments[$Index + 1]
        }
        return $Value
    }
    throw "假 Editor 未收到参数：$Prefix"
}

foreach ($RequiredArgument in @('-run=AvidScriptPerfRun', '-Multiprocess', '-NoCompile')) {
    if ($EditorArguments -cnotcontains $RequiredArgument) {
        throw "假 Editor 缺少 Commandlet 参数：$RequiredArgument"
    }
}
if (@($EditorArguments | Where-Object { $_.StartsWith('-ExecCmds=') }).Count -ne 0) {
    throw '假 Editor 不允许收到 -ExecCmds'
}

$RequestPath = Get-NativeSwitchValue '-AvidScriptPerfRequest='
$ResultPath = Get-NativeSwitchValue '-AvidScriptPerfResult='
if (-not (Test-Path -LiteralPath $RequestPath -PathType Leaf)) {
    throw "假 Editor 找不到 request：path=[$RequestPath] args=[$($EditorArguments -join '][')]"
}
$Request = Get-Content -LiteralPath $RequestPath -Raw | ConvertFrom-Json
$Result = $null
if ([string]$Request.mode -ceq 'calibration') {
    $IterationCounts = [ordered]@{}
    for ($WorkloadIndex = 0; $WorkloadIndex -lt $Request.workloads.Count; ++$WorkloadIndex) {
        $LaneCounts = [ordered]@{}
        for ($LaneIndex = 0; $LaneIndex -lt $Request.lanes.Count; ++$LaneIndex) {
            $LaneCounts[[string]$Request.lanes[$LaneIndex]] = [int64](
                100 * ($WorkloadIndex + 1) * ($LaneIndex + 1))
        }
        $IterationCounts[[string]$Request.workloads[$WorkloadIndex]] = $LaneCounts
    }
    $Result = [ordered]@{
        schema_version = [int]$Request.result_schema.version
        calibration_id = [string]$Request.attempt_id
        timer_frequency_hz = 1000000000
        lane_catalog = @($Request.lane_catalog)
        lane_catalog_sha256 = [string]$Request.lane_catalog_sha256
        provenance = $Request.provenance
        iteration_counts = $IterationCounts
    }
}
elseif ([string]$Request.mode -ceq 'timed') {
    function Get-FakeSampleSeed {
        param([int]$Seed, [int]$WorkloadIndex, [int]$SampleIndex)

        [uint64]$SeedBits = [int64]$Seed -band 0xffffffffL
        [uint64]$WorkloadBits = (
            [uint64]($WorkloadIndex + 1) * [uint64]2654435769) -band 0xffffffffL
        [uint64]$Value = ($SeedBits -bxor $WorkloadBits -bxor [uint64]($SampleIndex + 1)) -band 0xffffffffL
        [uint64]$Mixed = (
            ($Value * [uint64]1664525) + [uint64]1013904223) -band 0xffffffffL
        return [int]($Mixed -band 0x007fffffL)
    }

    function Get-FakeWilliamsOrder {
        param(
            [string[]]$BaseOrder,
            [int]$ProcessRun,
            [int]$WorkloadIndex,
            [int]$SampleIndex
        )

        $Rows = @(
            @(0, 1, 4, 2, 3),
            @(1, 2, 0, 3, 4),
            @(2, 3, 1, 4, 0),
            @(3, 4, 2, 0, 1),
            @(4, 0, 3, 1, 2)
        )
        $Row = (($ProcessRun + $WorkloadIndex + $SampleIndex) % 5 + 5) % 5
        return @($Rows[$Row] | ForEach-Object { $BaseOrder[$_] })
    }

    $Samples = [System.Collections.Generic.List[object]]::new()
    for ($WorkloadIndex = 0; $WorkloadIndex -lt $Request.workloads.Count; ++$WorkloadIndex) {
        $Workload = [string]$Request.workloads[$WorkloadIndex]
        for ($SampleIndex = 0; $SampleIndex -lt [int]$Request.timed_samples; ++$SampleIndex) {
            $SampleSeed = Get-FakeSampleSeed `
                -Seed ([int]$Request.seed) `
                -WorkloadIndex $WorkloadIndex `
                -SampleIndex $SampleIndex
            $Checksum = 1000 + ($WorkloadIndex * 100) + $SampleIndex
            $WilliamsOrder = Get-FakeWilliamsOrder `
                -BaseOrder @($Request.lane_order) `
                -ProcessRun ([int]$Request.process_run) `
                -WorkloadIndex $WorkloadIndex `
                -SampleIndex $SampleIndex
            for ($LanePosition = 0; $LanePosition -lt $WilliamsOrder.Count; ++$LanePosition) {
                $Lane = [string]$WilliamsOrder[$LanePosition]
                $LaneIndex = [Array]::IndexOf([object[]]@($Request.lanes), $Lane)
                $Iterations = [int64]$Request.iteration_counts.$Workload.$Lane
                $CyclesPerOperation = 1 + $SampleIndex + ($LaneIndex * 100) + ($WorkloadIndex * 1000)
                $LaneChecksum = $Checksum + $LaneIndex
                $CatalogEntry = $Request.lane_catalog[$LaneIndex]
                $DirectHitCount = 0
                $RequestedDirectFallbackCount = 0
                if ($Lane -ceq 'avidscript_wasmtime_native_direct') {
                    if ($Workload -cin @('scalar_add_int32', 'batch_scalar')) {
                        $DirectHitCount = $Iterations
                    }
                    elseif ($Workload -ceq 'property_get_set') {
                        $RequestedDirectFallbackCount = $Iterations * 2
                    }
                    elseif ($Workload -cnotin @('callback_empty', 'callback_tick', 'pure_integer')) {
                        $RequestedDirectFallbackCount = $Iterations
                    }
                }
                $Sample = [ordered]@{
                    process_run = [int]$Request.process_run
                    lane = $Lane
                    lane_identity_sha256 = [string]$CatalogEntry.lane_identity_sha256
                    lane_position = $LanePosition
                    workload = $Workload
                    sample_index = $SampleIndex
                    seed = $SampleSeed
                    iterations = $Iterations
                    elapsed_cycles = [int64]($Iterations * $CyclesPerOperation)
                    checksum = [int64]$LaneChecksum
                    expected_checksum = [int64]$LaneChecksum
                    final_scalar = [double](10 + $WorkloadIndex + $SampleIndex)
                    expected_final_scalar = [double](10 + $WorkloadIndex + $SampleIndex)
                    operation_call_count = $Iterations
                    expected_operation_call_count = $Iterations
                    host_import_call_count = [int64]($LaneIndex * $Iterations)
                    expected_host_import_call_count = [int64]($LaneIndex * $Iterations)
                    direct_hit_count = [int64]$DirectHitCount
                    requested_direct_fallback_count = [int64]$RequestedDirectFallbackCount
                    correct = $true
                }
                if ($Lane.StartsWith('avidscript_', [System.StringComparison]::Ordinal)) {
                    $Sample['backend_info'] = [ordered]@{
                        backend_id = [string]$CatalogEntry.backend_id
                        binding_invocation_mode = [string]$CatalogEntry.binding_invocation_mode
                        runtime_version = [string]$CatalogEntry.runtime_version
                        execution_mode = [string]$CatalogEntry.execution_mode
                        artifact_format = [string]$CatalogEntry.execution_artifact_format
                        artifact_sha256 = [string]$CatalogEntry.execution_artifact_sha256
                        source_wasm_sha256 = [string]$CatalogEntry.source_wasm_sha256
                        target_triple = [string]$CatalogEntry.target_triple
                        runtime_build_identity = [string]$CatalogEntry.runtime_build_identity
                        runtime_artifact_sha256 = [string]$CatalogEntry.runtime_artifact_sha256
                        fallback_used = $false
                    }
                }
                $Samples.Add($Sample)
            }
        }
    }
    $Result = [ordered]@{
        schema_version = [int]$Request.result_schema.version
        run_id = [string]$Request.attempt_id
        process_run = [int]$Request.process_run
        lane_order = @($Request.lane_order)
        lane_catalog = @($Request.lane_catalog)
        lane_catalog_sha256 = [string]$Request.lane_catalog_sha256
        timer_frequency_hz = 1000000000
        provenance = $Request.provenance
        samples = $Samples
    }
}
else {
    throw "假 Editor 收到未知模式：$($Request.mode)"
}

$Json = (($Result | ConvertTo-Json -Depth 64) -replace "`r`n", "`n") + "`n"
$Encoding = [System.Text.UTF8Encoding]::new($false)
$Stream = [System.IO.FileStream]::new(
    [string]$Request.result_write.temporary_path,
    [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::Write,
    [System.IO.FileShare]::Read)
try {
    $Writer = [System.IO.StreamWriter]::new($Stream, $Encoding, 4096, $true)
    try {
        $Writer.Write($Json)
        $Writer.Flush()
        $Stream.Flush($true)
    }
    finally {
        $Writer.Dispose()
    }
}
finally {
    $Stream.Dispose()
}
[System.IO.File]::Move([string]$Request.result_write.temporary_path, $ResultPath)

Write-Output "假 Editor PID=$PID"
'@
    [System.IO.File]::WriteAllText($FakeEditorPath, $FakeEditor, [System.Text.UTF8Encoding]::new($false))

    $OutputRoot = Join-Path $FixtureRoot 'attempts'
    $PowerShellExecutable = (Get-Command pwsh -ErrorAction Stop).Source
    $RunnerArguments = @{
        ProjectPath = $ProjectPath
        EditorExecutable = $PowerShellExecutable
        EditorPrefixArguments = @('-NoProfile', '-File', $FakeEditorPath)
        ProfilePath = $ProfilePath
        OutputRoot = $OutputRoot
        UeVersion = '5.8.0'
        UeBuildId = 'sidecar-engine-build'
        AvidScriptCommit = $CandidateCommit
        AvidScriptTreeSha = $CandidateTree
        AvidScriptDirty = $false
        PuertsCommit = $PuertsCommit
        PuertsBackendSha256 = $PuertsBackendSha
        CpuModel = 'Contract CPU'
        OperatingSystem = 'Contract OS'
        WamrMode = 'interpreter'
        V8Mode = 'jit'
        WasmSha256 = Get-TestFileSha256 $WasmPath
        ManifestSha256 = Get-TestFileSha256 $ManifestPath
        AllowNonFormalProfile = $true
    }

    $DefaultGateArguments = @{}
    foreach ($Entry in $RunnerArguments.GetEnumerator()) {
        if ($Entry.Key -cne 'AllowNonFormalProfile') {
            $DefaultGateArguments[$Entry.Key] = $Entry.Value
        }
    }
    $DefaultGateArguments.EditorPrefixArguments = @()
    Invoke-ExpectedFailure {
        & $RunnerPath @DefaultGateArguments | Out-Null
    } 'ASP53S2115'

    Invoke-ExpectedFailure {
        Assert-SidecarFormalEditorExecutable -EditorExecutable $PowerShellExecutable -UeVersion '5.8.0' | Out-Null
    } 'ASP53S2119'

    $NonCandidateFormalArguments = $DefaultGateArguments.Clone()
    $NonCandidateFormalArguments.ProfilePath = Join-Path $BenchmarkRoot 'Config/BenchmarkProfile.json'
    Invoke-ExpectedFailure {
        & $RunnerPath @NonCandidateFormalArguments | Out-Null
    } 'ASP53S2120'

    $OneWorkloadProfilePath = Join-Path $FixtureRoot 'one-workload-formal-looking-profile.json'
    $OneWorkloadProfile = $Profile | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $OneWorkloadProfile.process_runs = [int]$TrackedProfile.process_runs
    $OneWorkloadProfile.warmup_samples = [int]$TrackedProfile.warmup_samples
    $OneWorkloadProfile.timed_samples = [int]$TrackedProfile.timed_samples
    $OneWorkloadProfile.minimum_sample_milliseconds = [double]$TrackedProfile.minimum_sample_milliseconds
    $OneWorkloadProfile.workloads = @('scalar_noop')
    Write-NewJson $OneWorkloadProfilePath $OneWorkloadProfile
    $OneWorkloadArguments = $DefaultGateArguments.Clone()
    $OneWorkloadArguments.ProfilePath = $OneWorkloadProfilePath
    Invoke-ExpectedFailure {
        & $RunnerPath @OneWorkloadArguments | Out-Null
    } 'ASP53S2115'

    $WrongCandidateIdentityArguments = $RunnerArguments.Clone()
    $WrongCandidateIdentityArguments.AvidScriptTreeSha = ('0' * 40)
    Invoke-ExpectedFailure {
        & $RunnerPath @WrongCandidateIdentityArguments | Out-Null
    } 'ASP53S2116'

    $DirtyCandidatePath = Join-Path $CandidateTarget 'dirty.txt'
    [System.IO.File]::WriteAllText($DirtyCandidatePath, "dirty`n", [System.Text.UTF8Encoding]::new($false))
    Invoke-ExpectedFailure {
        & $RunnerPath @RunnerArguments | Out-Null
    } 'ASP53S2116'
    Remove-Item -LiteralPath $DirtyCandidatePath -Force

    $SourceFixturePath = Join-Path $SourceTarget 'fixture.cpp'
    $SourceFixtureBytes = [System.IO.File]::ReadAllBytes($SourceFixturePath)
    [System.IO.File]::WriteAllText($SourceFixturePath, "tampered`n", [System.Text.UTF8Encoding]::new($false))
    Invoke-ExpectedFailure {
        & $RunnerPath @RunnerArguments | Out-Null
    } 'ASP53S2116'
    [System.IO.File]::WriteAllBytes($SourceFixturePath, $SourceFixtureBytes)

    $ManagedMarkerPath = Join-Path $PuertsTarget '.avidscript-puerts-install.json'
    $ManagedMarkerRaw = [System.IO.File]::ReadAllText($ManagedMarkerPath)
    $ManagedMarker = $ManagedMarkerRaw | ConvertFrom-Json
    $ManagedMarker.schema_version = 1
    Write-NewJson $ManagedMarkerPath $ManagedMarker
    Invoke-ExpectedFailure {
        & $RunnerPath @RunnerArguments | Out-Null
    } 'ASP53S2117'
    [System.IO.File]::WriteAllText($ManagedMarkerPath, $ManagedMarkerRaw, [System.Text.UTF8Encoding]::new($false))

    $MutualCheatMarker = $ManagedMarkerRaw | ConvertFrom-Json
    $MutualCheatMarker.source_commit_sha = ('a' * 40)
    $MutualCheatMarker.backend_sha256 = ('b' * 64)
    Write-NewJson $ManagedMarkerPath $MutualCheatMarker
    $MutualCheatArguments = $RunnerArguments.Clone()
    $MutualCheatArguments.PuertsCommit = ('a' * 40)
    $MutualCheatArguments.PuertsBackendSha256 = ('b' * 64)
    Invoke-ExpectedFailure {
        & $RunnerPath @MutualCheatArguments | Out-Null
    } 'ASP53S2117'
    [System.IO.File]::WriteAllText($ManagedMarkerPath, $ManagedMarkerRaw, [System.Text.UTF8Encoding]::new($false))

    $PuertsUpluginPath = Join-Path $PuertsTarget 'Puerts.uplugin'
    $PuertsUpluginBytes = [System.IO.File]::ReadAllBytes($PuertsUpluginPath)
    [System.IO.File]::WriteAllText($PuertsUpluginPath, "tampered`n", [System.Text.UTF8Encoding]::new($false))
    Invoke-ExpectedFailure {
        & $RunnerPath @RunnerArguments | Out-Null
    } 'ASP53S2117'
    [System.IO.File]::WriteAllBytes($PuertsUpluginPath, $PuertsUpluginBytes)

    $BackendBytes = [System.IO.File]::ReadAllBytes($BackendPath)
    [System.IO.File]::WriteAllText($BackendPath, "tampered-backend`n", [System.Text.UTF8Encoding]::new($false))
    Invoke-ExpectedFailure {
        & $RunnerPath @RunnerArguments | Out-Null
    } 'ASP53S2117'
    [System.IO.File]::WriteAllBytes($BackendPath, $BackendBytes)

    $NestedBinariesBytes = [System.IO.File]::ReadAllBytes($NestedBinariesPath)
    [System.IO.File]::WriteAllText($NestedBinariesPath, "tampered-nested`n", [System.Text.UTF8Encoding]::new($false))
    Invoke-ExpectedFailure {
        & $RunnerPath @RunnerArguments | Out-Null
    } 'ASP53S2117'
    [System.IO.File]::WriteAllBytes($NestedBinariesPath, $NestedBinariesBytes)

    $TopLevelBinariesBytes = [System.IO.File]::ReadAllBytes($TopLevelBinariesPath)
    [System.IO.File]::WriteAllText($TopLevelBinariesPath, "tampered-top-level`n", [System.Text.UTF8Encoding]::new($false))
    $IgnoredTopLevelBinariesRun = (& $RunnerPath @RunnerArguments) | ConvertFrom-Json
    Assert-True ($IgnoredTopLevelBinariesRun.succeeded -eq $true) 'top-level Puerts Binaries content must not affect the installed digest'
    [System.IO.File]::WriteAllBytes($TopLevelBinariesPath, $TopLevelBinariesBytes)

    $WasmBytes = [System.IO.File]::ReadAllBytes($WasmPath)
    [System.IO.File]::WriteAllBytes($WasmPath, [byte[]](0,97,115,109,2,0,0,0))
    Invoke-ExpectedFailure {
        & $RunnerPath @RunnerArguments | Out-Null
    } 'ASP53S2118'
    [System.IO.File]::WriteAllBytes($WasmPath, $WasmBytes)

    foreach ($ReservedArgument in @(
        '-eXeCcMdS:Quit',
        '-RuN=Other',
        '-aViDsCrIpTpErFrEqUeSt=x',
        '-AVIDSCRIPTPERFRESULT=x',
        '-mUlTiPrOcEsS',
        '-nOcOmPiLe',
        '-nUlLrHi',
        '-D3D12',
        '-rHi=Vulkan',
        '-aBsLoG=x',
        '/NullRHI',
        '/D3D12',
        '/Multiprocess',
        '/run=Other',
        '/AbsLog=x',
        $ProjectPath)) {
        $RejectedArguments = $RunnerArguments.Clone()
        $RejectedArguments.AdditionalEditorArguments = @($ReservedArgument)
        Invoke-ExpectedFailure {
            & $RunnerPath @RejectedArguments | Out-Null
        } 'ASP53S2114'
    }
    $RejectedPrefixArguments = $RunnerArguments.Clone()
    $RejectedPrefixArguments.EditorPrefixArguments = @(
        '-RuN=Other',
        '-File',
        $FakeEditorPath
    )
    Invoke-ExpectedFailure {
        & $RunnerPath @RejectedPrefixArguments | Out-Null
    } 'ASP53S2114'

    $FirstRunOutput = & $RunnerPath @RunnerArguments
    $FirstRun = $FirstRunOutput | ConvertFrom-Json
    Assert-True ($FirstRun.succeeded -eq $true) '5 进程 sidecar 未成功完成'
    Assert-True ([int]$FirstRun.process_run_count -eq 5) 'sidecar 未执行 5 个进程'
    $FirstAttempt = [string]$FirstRun.attempt_path
    Assert-True (Test-Path -LiteralPath $FirstAttempt -PathType Container) 'attempt 目录不存在'

    $Manifest = Get-Content -LiteralPath (Join-Path $FirstAttempt 'attempt.json') -Raw | ConvertFrom-Json
    Assert-True ($Manifest.profile.sha256 -cmatch '^[0-9a-f]{64}$') 'attempt 未固定 profile 哈希'
    Assert-True ($Manifest.result_schema.sha256 -cmatch '^[0-9a-f]{64}$') 'attempt 未固定 raw schema 哈希'
    Assert-True ($Manifest.aggregate_schema.sha256 -cmatch '^[0-9a-f]{64}$') 'attempt 未固定 aggregate schema 哈希'
    Assert-True ((Get-TestFileSha256 (Join-Path $FirstAttempt $Manifest.aggregate_schema.snapshot_path)) -ceq $Manifest.aggregate_schema.sha256) 'aggregate schema 快照哈希不匹配'
    Assert-True ([int]$Manifest.process_runs.Count -eq 5) 'attempt 清单没有 5 个进程'
    Assert-True ($Manifest.calibration.sha256 -cmatch '^[0-9a-f]{64}$') 'attempt 未固定 calibration raw 哈希'
    Assert-True ($Manifest.calibration.request_sha256 -cmatch '^[0-9a-f]{64}$') 'attempt 未固定 calibration request 哈希'
    Assert-True (Test-Path -LiteralPath (Join-Path $FirstAttempt $Manifest.calibration.raw_path) -PathType Leaf) 'attempt 未保留 calibration.json'
    Assert-True ($Manifest.provenance.allow_non_formal_profile -eq $true) 'fixture attempt 未记录非正式 profile 开关'
    Assert-True ($Manifest.provenance.editor_executable_sha256 -cmatch '^[0-9a-f]{64}$') 'fixture attempt 未记录 editor 可执行文件哈希'
    Assert-True (-not [string]::IsNullOrWhiteSpace([string]$Manifest.provenance.editor_file_version)) 'fixture attempt 未记录 editor 文件版本'

    $ExpectedLaneOrders = @(
        'native_cpp,puerts_v8_reflection,puerts_v8_static,avidscript_wasmtime_semantic,avidscript_wasmtime_native_direct',
        'puerts_v8_reflection,puerts_v8_static,avidscript_wasmtime_semantic,avidscript_wasmtime_native_direct,native_cpp',
        'puerts_v8_static,avidscript_wasmtime_semantic,avidscript_wasmtime_native_direct,native_cpp,puerts_v8_reflection',
        'avidscript_wasmtime_semantic,avidscript_wasmtime_native_direct,native_cpp,puerts_v8_reflection,puerts_v8_static',
        'avidscript_wasmtime_native_direct,native_cpp,puerts_v8_reflection,puerts_v8_static,avidscript_wasmtime_semantic'
    )
    $ProcessIds = [System.Collections.Generic.HashSet[int]]::new()
    $CalibrationProcess = Get-Content -LiteralPath (Join-Path $FirstAttempt $Manifest.calibration.process_metadata_path) -Raw | ConvertFrom-Json
    Assert-True ($ProcessIds.Add([int]$CalibrationProcess.pid)) 'calibration 未使用独立 Editor 进程'
    $ExpectedIterationCounts = ($Manifest.calibration.iteration_counts | ConvertTo-Json -Compress)
    for ($ProcessRun = 0; $ProcessRun -lt 5; ++$ProcessRun) {
        $RunDirectory = Join-Path $FirstAttempt ('runs/{0:D2}' -f $ProcessRun)
        $Request = Get-Content -LiteralPath (Join-Path $RunDirectory 'request.json') -Raw | ConvertFrom-Json
        $Process = Get-Content -LiteralPath (Join-Path $RunDirectory 'process.json') -Raw | ConvertFrom-Json
        Assert-True ([int]$Request.schema_version -eq 2) "进程 $ProcessRun request 缺少 schema_version"
        Assert-True ([string]$Request.mode -ceq 'timed') "进程 $ProcessRun request 不是 timed 模式"
        Assert-True ((@($Request.lane_order) -join ',') -ceq $ExpectedLaneOrders[$ProcessRun]) "lane 顺序未按进程轮转：$ProcessRun"
        Assert-True ([int]$Request.warmup_samples -eq 1) "进程 $ProcessRun 未使用 fixture profile 预热次数"
        Assert-True ([int]$Request.timed_samples -eq 3) "进程 $ProcessRun 未使用 fixture profile 样本数"
        Assert-True ([string]$Request.result_write.strategy -ceq 'same_directory_temporary_then_atomic_rename') "进程 $ProcessRun 未声明原子发布策略"
        Assert-True ((Get-TestFileSha256 (Join-Path $RunDirectory 'request.json')) -ceq $Manifest.process_runs[$ProcessRun].request_sha256) "进程 $ProcessRun request 哈希未固定"
        Assert-True ([string]$Request.result_path -ceq (Join-Path $RunDirectory 'raw-result.json')) "进程 $ProcessRun result_path 未绑定 run 目录"
        Assert-True ([string]$Request.result_write.temporary_path -ceq "$($Request.result_path).$($Manifest.attempt_id).tmp") "进程 $ProcessRun temporary_path 未绑定 run 目录"
        Assert-True ($Request.provenance.null_rhi -eq $true) "进程 $ProcessRun 未固定 NullRHI"
        Assert-True ($Request.provenance.avidscript_dirty -eq $false) "进程 $ProcessRun 允许 dirty AvidScript"
        Assert-True (($Request.iteration_counts | ConvertTo-Json -Compress) -ceq $ExpectedIterationCounts) "进程 $ProcessRun 未复用 calibration iteration map"
        Assert-True ($ProcessIds.Add([int]$Process.pid)) "进程 $ProcessRun 未使用新的 Editor 进程"
        Assert-True (Test-Path -LiteralPath (Join-Path $RunDirectory 'raw-result.json') -PathType Leaf) "进程 $ProcessRun 未保留 raw JSON"
    }
    Assert-True ($ProcessIds.Count -eq 6) '一次 attempt 必须使用 1 calibration + 5 timed 独立 PID'
    Assert-True (@(Get-ChildItem -LiteralPath $FirstAttempt -Recurse -File -Filter '*.tmp').Count -eq 0) '成功 attempt 遗留未发布临时文件'

    $AggregatePath = Join-Path $FirstAttempt 'aggregate.json'
    $AggregateRaw = Get-Content -LiteralPath $AggregatePath -Raw
    $AggregateSchemaSnapshotPath = Join-Path $FirstAttempt $Manifest.aggregate_schema.snapshot_path
    Assert-True ($AggregateRaw | Test-Json -SchemaFile $AggregateSchemaSnapshotPath) '聚合结果不符合 attempt 固定 Schema'
    Assert-SchemaRejectsNoncanonicalCatalog `
        -DocumentPath (Join-Path $FirstAttempt $Manifest.calibration.request_path) `
        -SchemaPath $RequestSchemaPath `
        -Label 'BenchmarkProcessRequest'
    Assert-SchemaRejectsNoncanonicalCatalog `
        -DocumentPath (Join-Path $FirstAttempt $Manifest.calibration.raw_path) `
        -SchemaPath $CalibrationSchemaPath `
        -Label 'BenchmarkCalibration'
    Assert-SchemaRejectsNoncanonicalCatalog `
        -DocumentPath (Join-Path $FirstAttempt $Manifest.process_runs[0].raw_result_path) `
        -SchemaPath $RawSchemaPath `
        -Label 'BenchmarkProcessResult'
    Assert-SchemaRejectsNoncanonicalCatalog `
        -DocumentPath $AggregatePath `
        -SchemaPath $AggregateSchemaPath `
        -Label 'BenchmarkAggregate'
    $Aggregate = $AggregateRaw | ConvertFrom-Json
    Assert-True ($Aggregate.aggregate_schema.sha256 -ceq $Manifest.aggregate_schema.sha256) '聚合结果未记录 aggregate schema 哈希'
    Assert-True ([int]$Aggregate.samples.Count -eq 225) '聚合结果未保留全部 raw samples'
    Assert-True ([int]$Aggregate.process_statistics.Count -eq 75) '每进程统计矩阵不完整'
    Assert-True ([int]$Aggregate.cross_process_statistics.Count -eq 15) '跨进程统计矩阵不完整'
    Assert-True ([int]$Aggregate.paired_comparisons.Count -eq 12) '同进程配对比较矩阵不完整'
    Assert-True ([int]$Aggregate.descriptive_pooled_statistics.Count -eq 15) '池化描述统计矩阵不完整'
    $ScalarNativeProcess = @($Aggregate.process_statistics | Where-Object {
        [int]$_.process_run -eq 0 -and
        $_.lane -ceq 'native_cpp' -and $_.workload -ceq 'scalar_noop'
    })
    Assert-True ($ScalarNativeProcess.Count -eq 1) '缺少 process/native_cpp/scalar_noop 统计'
    Assert-True ([int]$ScalarNativeProcess[0].sample_count -eq 3) '每个 fixture process/lane/workload 应有 3 个样本'
    Assert-Near ([double]$ScalarNativeProcess[0].p50_ns_per_operation) 2.0 -Message '每进程 P50 计算错误'
    Assert-Near ([double]$ScalarNativeProcess[0].p95_ns_per_operation) 3.0 -Message '每进程 P95 计算错误'
    Assert-Near ([double]$ScalarNativeProcess[0].mad_ns_per_operation) 1.0 -Message '每进程 MAD 计算错误'
    $ScalarNativeCrossProcess = @($Aggregate.cross_process_statistics | Where-Object {
        $_.lane -ceq 'native_cpp' -and $_.workload -ceq 'scalar_noop'
    })
    Assert-True ($ScalarNativeCrossProcess.Count -eq 1) '缺少跨进程 native_cpp/scalar_noop 统计'
    Assert-True ([int]$ScalarNativeCrossProcess[0].process_count -eq 5) '跨进程统计必须基于 5 个 process summary'
    Assert-Near ([double]$ScalarNativeCrossProcess[0].p50_of_process_p50_ns_per_operation) 2.0 -Message '跨进程 P50 计算错误'
    Assert-Near ([double]$ScalarNativeCrossProcess[0].p95_of_process_p50_ns_per_operation) 2.0 -Message '跨进程 P95 计算错误'
    Assert-Near ([double]$ScalarNativeCrossProcess[0].mad_of_process_p50_ns_per_operation) 0.0 -Message '跨进程 MAD 计算错误'
    $ReflectionPair = @($Aggregate.paired_comparisons | Where-Object {
        $_.lane -ceq 'puerts_v8_reflection' -and $_.workload -ceq 'scalar_noop'
    })
    Assert-True ($ReflectionPair.Count -eq 1 -and [int]$ReflectionPair[0].per_process_ratios.Count -eq 5) '配对比较未保留 5 个 process ratio'
    Assert-Near ([double]$ReflectionPair[0].p50_ratio) (102.0 / 2.0) -Message '同进程配对 ratio 计算错误'
    $ScalarNativePooled = @($Aggregate.descriptive_pooled_statistics | Where-Object {
        $_.lane -ceq 'native_cpp' -and $_.workload -ceq 'scalar_noop'
    })
    Assert-True ($ScalarNativePooled.Count -eq 1 -and $ScalarNativePooled[0].descriptive_only -eq $true) '池化统计必须明确标记 descriptive_only'
    Assert-True ([int]$ScalarNativePooled[0].sample_count -eq 15) 'fixture 池化描述统计应保留 15 个样本'
    $ExpectedGeometricMean = [Math]::Exp((1..3 | ForEach-Object { [Math]::Log([double]$_) } | Measure-Object -Average).Average)
    Assert-Near ([double]$ScalarNativePooled[0].geometric_mean_ns_per_operation) $ExpectedGeometricMean 0.000001 -Message '池化几何平均数计算错误'

    $RawAuditResult = Get-Content -LiteralPath (Join-Path $FirstAttempt 'runs/00/raw-result.json') -Raw | ConvertFrom-Json
    $RawAuditSample = $RawAuditResult.samples[0]
    foreach ($Field in @(
        'lane_position',
        'seed',
        'expected_checksum',
        'final_scalar',
        'expected_final_scalar',
        'operation_call_count',
        'expected_operation_call_count',
        'host_import_call_count',
        'expected_host_import_call_count')) {
        Assert-True ($null -ne $RawAuditSample.PSObject.Properties[$Field]) "raw sample 缺少公平性字段：$Field"
    }
    $WilliamsNativeSample = @($RawAuditResult.samples | Where-Object {
        $_.workload -ceq 'scalar_noop' -and
        [int]$_.sample_index -eq 1 -and
        $_.lane -ceq 'native_cpp'
    })
    Assert-True ($WilliamsNativeSample.Count -eq 1 -and [int]$WilliamsNativeSample[0].lane_position -eq 2) 'Williams row 1 应把 native_cpp 放在 position 2'

    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $FirstAttempt -Mode Aggregate | Out-Null
    } 'ASP53S2026'

    $SecondRunOutput = & $RunnerPath @RunnerArguments
    $SecondRun = $SecondRunOutput | ConvertFrom-Json
    Assert-True ([string]$SecondRun.attempt_path -cne $FirstAttempt) '重复执行覆盖了已有 attempt'
    Assert-True (Test-Path -LiteralPath $AggregatePath -PathType Leaf) '第二次执行破坏了第一次 raw/aggregate 证据'

    $MixedCommit = Copy-AndMutateRawResult $FirstAttempt 'mixed-commit' {
        param($Raw)
        $Raw.provenance.avidscript_commit = ('d' * 40)
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $MixedCommit -Mode Validate | Out-Null
    } 'ASP53S2014'

    $MixedConfig = Copy-AndMutateRawResult $FirstAttempt 'mixed-config' {
        param($Raw)
        $Raw.provenance.configuration = 'Shipping'
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $MixedConfig -Mode Validate | Out-Null
    } 'ASP53S2014'

    $MixedEngine = Copy-AndMutateRawResult $FirstAttempt 'mixed-engine' {
        param($Raw)
        $Raw.provenance.ue_build_id = 'other-engine-build'
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $MixedEngine -Mode Validate | Out-Null
    } 'ASP53S2014'

    $MixedSchema = Copy-AndMutateRawResult $FirstAttempt 'mixed-schema' {
        param($Raw)
        $Raw.provenance.result_schema_sha256 = ('e' * 64)
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $MixedSchema -Mode Validate | Out-Null
    } 'ASP53S2014'

    $TamperedRequestAttempt = Copy-AttemptFixture $FirstAttempt 'tampered-request-sha'
    $TamperedRequestPath = Join-Path $TamperedRequestAttempt 'runs/02/request.json'
    $TamperedRequest = Get-Content -LiteralPath $TamperedRequestPath -Raw | ConvertFrom-Json
    $TamperedRequest.seed = [int]$TamperedRequest.seed + 1
    Write-NewJson $TamperedRequestPath $TamperedRequest
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $TamperedRequestAttempt -Mode Validate | Out-Null
    } 'ASP53S2051'

    $TamperedCalibrationRequestAttempt = Copy-AttemptFixture $FirstAttempt 'tampered-calibration-request-sha'
    $TamperedCalibrationRequestPath = Join-Path $TamperedCalibrationRequestAttempt 'calibration/request.json'
    $TamperedCalibrationRequest = Get-Content -LiteralPath $TamperedCalibrationRequestPath -Raw | ConvertFrom-Json
    $TamperedCalibrationRequest.seed = [int]$TamperedCalibrationRequest.seed + 1
    Write-NewJson $TamperedCalibrationRequestPath $TamperedCalibrationRequest
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $TamperedCalibrationRequestAttempt -Mode Validate | Out-Null
    } 'ASP53S2051'

    $TamperedAggregateSchemaAttempt = Copy-AttemptFixture $FirstAttempt 'tampered-aggregate-schema'
    $TamperedAggregateSchemaPath = Join-Path $TamperedAggregateSchemaAttempt $Manifest.aggregate_schema.snapshot_path
    $TamperedAggregateSchema = Get-Content -LiteralPath $TamperedAggregateSchemaPath -Raw | ConvertFrom-Json
    $TamperedAggregateSchema.title = '被篡改的聚合 Schema'
    Write-NewJson $TamperedAggregateSchemaPath $TamperedAggregateSchema
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $TamperedAggregateSchemaAttempt -Mode Validate | Out-Null
    } 'ASP53S2025'

    $MixedIterationRequest = Copy-AttemptFixture $FirstAttempt 'mixed-iteration-request'
    $TimedRequestPath = Join-Path $MixedIterationRequest 'runs/02/request.json'
    $TimedRequest = Get-Content -LiteralPath $TimedRequestPath -Raw | ConvertFrom-Json
    $TimedRequest.iteration_counts.scalar_noop.avidscript_wasmtime_semantic =
        [int64]$TimedRequest.iteration_counts.scalar_noop.avidscript_wasmtime_semantic + 1
    Write-NewJson $TimedRequestPath $TimedRequest
    $MixedIterationManifestPath = Join-Path $MixedIterationRequest 'attempt.json'
    $MixedIterationManifest = Get-Content -LiteralPath $MixedIterationManifestPath -Raw | ConvertFrom-Json
    $MixedIterationManifest.process_runs[2].request_sha256 = Get-TestFileSha256 $TimedRequestPath
    Write-NewJson $MixedIterationManifestPath $MixedIterationManifest
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $MixedIterationRequest -Mode Validate | Out-Null
    } 'ASP53S2046'

    $V2RequestAttempt = Copy-AttemptFixture $FirstAttempt 'v2-result-schema-request'
    $V2RequestPath = Join-Path $V2RequestAttempt 'runs/02/request.json'
    $V2Request = Get-Content -LiteralPath $V2RequestPath -Raw | ConvertFrom-Json
    $V2Request.result_schema.version = 2
    Write-NewJson $V2RequestPath $V2Request
    $V2ManifestPath = Join-Path $V2RequestAttempt 'attempt.json'
    $V2Manifest = Get-Content -LiteralPath $V2ManifestPath -Raw | ConvertFrom-Json
    $V2Manifest.process_runs[2].request_sha256 = Get-TestFileSha256 $V2RequestPath
    Write-NewJson $V2ManifestPath $V2Manifest
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $V2RequestAttempt -Mode Validate | Out-Null
    } 'ASP53S2046'

    $WrongResultPathAttempt = Copy-AttemptFixture $FirstAttempt 'wrong-result-path-request'
    $WrongResultRequestPath = Join-Path $WrongResultPathAttempt 'runs/02/request.json'
    $WrongResultRequest = Get-Content -LiteralPath $WrongResultRequestPath -Raw | ConvertFrom-Json
    $WrongResultRequest.result_path = Join-Path $WrongResultPathAttempt 'runs/03/raw-result.json'
    Write-NewJson $WrongResultRequestPath $WrongResultRequest
    $WrongResultManifestPath = Join-Path $WrongResultPathAttempt 'attempt.json'
    $WrongResultManifest = Get-Content -LiteralPath $WrongResultManifestPath -Raw | ConvertFrom-Json
    $WrongResultManifest.process_runs[2].request_sha256 = Get-TestFileSha256 $WrongResultRequestPath
    Write-NewJson $WrongResultManifestPath $WrongResultManifest
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $WrongResultPathAttempt -Mode Validate | Out-Null
    } 'ASP53S2046'

    $WrongTemporaryPathAttempt = Copy-AttemptFixture $FirstAttempt 'wrong-temporary-path-request'
    $WrongTemporaryRequestPath = Join-Path $WrongTemporaryPathAttempt 'runs/02/request.json'
    $WrongTemporaryRequest = Get-Content -LiteralPath $WrongTemporaryRequestPath -Raw | ConvertFrom-Json
    $WrongTemporaryRequest.result_write.temporary_path = "$($WrongTemporaryRequest.result_path).wrong.tmp"
    Write-NewJson $WrongTemporaryRequestPath $WrongTemporaryRequest
    $WrongTemporaryManifestPath = Join-Path $WrongTemporaryPathAttempt 'attempt.json'
    $WrongTemporaryManifest = Get-Content -LiteralPath $WrongTemporaryManifestPath -Raw | ConvertFrom-Json
    $WrongTemporaryManifest.process_runs[2].request_sha256 = Get-TestFileSha256 $WrongTemporaryRequestPath
    Write-NewJson $WrongTemporaryManifestPath $WrongTemporaryManifest
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $WrongTemporaryPathAttempt -Mode Validate | Out-Null
    } 'ASP53S2046'

    $BadSeed = Copy-AndMutateRawResult $FirstAttempt 'bad-seed' {
        param($Raw)
        $Raw.samples[0].seed = [int]$Raw.samples[0].seed + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadSeed -Mode Validate | Out-Null
    } 'ASP53S2035'

    $BadExpectedChecksum = Copy-AndMutateRawResult $FirstAttempt 'bad-expected-checksum' {
        param($Raw)
        $Raw.samples[0].expected_checksum = [int64]$Raw.samples[0].checksum + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadExpectedChecksum -Mode Validate | Out-Null
    } 'ASP53S2036'

    $BadFinalScalar = Copy-AndMutateRawResult $FirstAttempt 'bad-final-scalar' {
        param($Raw)
        $Raw.samples[0].expected_final_scalar = [double]$Raw.samples[0].final_scalar + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadFinalScalar -Mode Validate | Out-Null
    } 'ASP53S2037'

    $BadOperationCount = Copy-AndMutateRawResult $FirstAttempt 'bad-operation-count' {
        param($Raw)
        $Raw.samples[0].expected_operation_call_count = [int64]$Raw.samples[0].operation_call_count + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadOperationCount -Mode Validate | Out-Null
    } 'ASP53S2038'

    $BadHostImportCount = Copy-AndMutateRawResult $FirstAttempt 'bad-host-import-count' {
        param($Raw)
        $Raw.samples[0].expected_host_import_call_count = [int64]$Raw.samples[0].host_import_call_count + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadHostImportCount -Mode Validate | Out-Null
    } 'ASP53S2039'

    $BadLanePosition = Copy-AndMutateRawResult $FirstAttempt 'bad-lane-position' {
        param($Raw)
        $Target = @($Raw.samples | Where-Object {
            $_.workload -ceq 'scalar_noop' -and
            [int]$_.sample_index -eq 1 -and
            $_.lane -ceq 'native_cpp'
        })
        $Target[0].lane_position = 0
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadLanePosition -Mode Validate | Out-Null
    } 'ASP53S2040'

    $BadIterations = Copy-AndMutateRawResult $FirstAttempt 'bad-iterations' {
        param($Raw)
        $Raw.samples[0].iterations = [int64]$Raw.samples[0].iterations + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadIterations -Mode Validate | Out-Null
    } 'ASP53S2044'

    $BadLaneIdentity = Copy-AndMutateRawResult $FirstAttempt 'bad-lane-identity' {
        param($Raw)
        $Raw.samples[0].lane_identity_sha256 = '0' * 64
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadLaneIdentity -Mode Validate | Out-Null
    } 'ASP54S2053'

    $BadBackendIdentity = Copy-AndMutateRawResult $FirstAttempt 'bad-backend-identity' {
        param($Raw)
        $Sample = @($Raw.samples | Where-Object {
            $_.lane -ceq 'avidscript_wasmtime_native_direct'
        })[0]
        $Sample.backend_info.runtime_version = 'wrong-runtime'
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadBackendIdentity -Mode Validate | Out-Null
    } 'ASP54S2054'

    $FallbackUsed = Copy-AndMutateRawResult $FirstAttempt 'fallback-used' {
        param($Raw)
        $Sample = @($Raw.samples | Where-Object {
            $_.lane -ceq 'avidscript_wasmtime_native_direct'
        })[0]
        $Sample.backend_info.fallback_used = $true
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $FallbackUsed -Mode Validate | Out-Null
    } 'ASP53S2005'

    $DirectScalarFallback = Copy-AndMutateRawResult $FirstAttempt 'direct-scalar-fallback' {
        param($Raw)
        $Sample = @($Raw.samples | Where-Object {
            $_.lane -ceq 'avidscript_wasmtime_native_direct' -and
            $_.workload -ceq 'scalar_add_int32'
        })[0]
        Assert-True ($null -ne $Sample) 'direct scalar fallback 夹具缺少目标 sample'
        Assert-True ($null -ne $Sample.PSObject.Properties['direct_hit_count']) 'direct scalar fallback 夹具缺少 direct_hit_count'
        Assert-True ($null -ne $Sample.PSObject.Properties['requested_direct_fallback_count']) 'direct scalar fallback 夹具缺少 requested_direct_fallback_count'
        $Sample.direct_hit_count = [int64]$Sample.iterations - 1
        $Sample.requested_direct_fallback_count = 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $DirectScalarFallback -Mode Validate | Out-Null
    } 'ASP54S2057'
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

Write-Output 'Puerts benchmark sidecar 合同通过：parser=1 formal_gate=4 provenance_rejections=9 known_generated_ignored=2 reserved_args=17 calibration_processes=1 timed_processes=5 fresh_pids=6 williams=1 request_hash=2 aggregate_snapshot=1 schema_order_rejections=8 schema_v2=1 raw_samples=225 process_stats=75 cross_process_stats=15 paired=12 identity_rejections=3 mixed_rejections=4'
