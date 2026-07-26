$ErrorActionPreference = 'Stop'

function Read-SidecarJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Code
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Code 缺少 JSON 文件：$Path"
    }

    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    }
    catch {
        throw "$Code JSON 文件无法解析：$Path"
    }
}

function Get-SidecarFileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "ASP53S2000 无法计算缺失文件的 SHA-256：$Path"
    }

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Resolve-SidecarCanonicalDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Label,
        [switch]$RequireJunction
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Code $Label directory is missing: $Path"
    }

    $Item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($RequireJunction -and $Item.LinkType -cne 'Junction') {
        throw "$Code $Label must be a junction: $Path"
    }

    try {
        $Resolved = if ($Item.LinkType -eq 'Junction') {
            $Item.ResolveLinkTarget($true)
        }
        else {
            $Item
        }
        if ($null -eq $Resolved) {
            throw 'ResolveLinkTarget returned no target.'
        }
        return [System.IO.Path]::GetFullPath($Resolved.FullName).TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar)
    }
    catch {
        throw "$Code $Label could not be resolved to a canonical directory: $Path`n$($_.Exception.Message)"
    }
}

function Get-SidecarDirectoryContentDigest {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Root = Resolve-SidecarCanonicalDirectory -Path $Path -Code 'ASP53S2000' -Label 'digest root'
    $Entries = [System.Collections.Generic.List[string]]::new()
    foreach ($File in @(Get-ChildItem -LiteralPath $Root -File -Force -Recurse)) {
        $RelativePath = [System.IO.Path]::GetRelativePath($Root, $File.FullName).Replace('\', '/')
        $Entries.Add(('{0}`t{1}`t{2}' -f $RelativePath, [int64]$File.Length, (Get-SidecarFileSha256 -Path $File.FullName)))
    }
    $Values = @($Entries)
    [System.Array]::Sort($Values, [System.StringComparer]::Ordinal)
    $Payload = [string]::Join("`n", $Values) + "`n"
    $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Payload)
    $Hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        $Digest = ([System.BitConverter]::ToString($Hasher.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $Hasher.Dispose()
    }

    return [pscustomobject][ordered]@{
        content_sha256 = $Digest
        file_count = [int]$Values.Count
    }
}

function Get-SidecarInstalledPuertsContentDigest {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ManagedMarkerName
    )

    $Root = Resolve-SidecarCanonicalDirectory -Path $Path -Code 'ASP53S2117' -Label 'Puerts install root'
    $ExcludedDirectoryNames = @('Binaries', 'Intermediate', 'Saved', 'DerivedDataCache', '.git')
    $Entries = [System.Collections.Generic.List[string]]::new()
    foreach ($File in @(Get-ChildItem -LiteralPath $Root -File -Force -Recurse)) {
        $RelativePath = [System.IO.Path]::GetRelativePath($Root, $File.FullName).Replace('\', '/')
        if ($RelativePath -ieq $ManagedMarkerName) {
            continue
        }
        $PathParts = @($RelativePath.Split('/', [System.StringSplitOptions]::RemoveEmptyEntries))
        $RootDirectory = if ($PathParts.Count -gt 1) { $PathParts[0] } else { '' }
        if ($ExcludedDirectoryNames -icontains $RootDirectory) {
            continue
        }
        $Entries.Add(('{0}`t{1}' -f $RelativePath, (Get-SidecarFileSha256 -Path $File.FullName)))
    }
    $Values = @($Entries)
    [System.Array]::Sort($Values, [System.StringComparer]::Ordinal)
    $Payload = [string]::Join("`n", $Values) + "`n"
    $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Payload)
    $Hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        $Digest = ([System.BitConverter]::ToString($Hasher.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $Hasher.Dispose()
    }

    return [pscustomobject][ordered]@{
        content_sha256 = $Digest
        file_count = [int]$Values.Count
    }
}

function Get-SidecarRequiredPropertyValue {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Property = $Object.PSObject.Properties[$Name]
    if ($null -eq $Property -or $null -eq $Property.Value) {
        throw "$Code missing required field: $Label.$Name"
    }
    return $Property.Value
}

function Invoke-SidecarGit {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryPath,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )

    $Output = & git -C $RepositoryPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "$Code git failed: git -C $RepositoryPath $($Arguments -join ' ')`n$($Output -join [Environment]::NewLine)"
    }
    return @($Output)
}

function Assert-SidecarBenchmarkProjectProvenance {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$AvidScriptCommit,
        [Parameter(Mandatory = $true)][string]$AvidScriptTreeSha
    )

    $ProjectRoot = Split-Path -Parent ([System.IO.Path]::GetFullPath($ProjectPath))
    $Marker = Read-SidecarJson -Path (Join-Path $ProjectRoot 'benchmark-project.json') -Code 'ASP53S2116'
    if ([int](Get-SidecarRequiredPropertyValue $Marker 'schema_version' 'ASP53S2116' 'benchmark-project') -ne 2) {
        throw 'ASP53S2116 benchmark-project marker schema_version must be 2'
    }

    $ExpectedJunctions = [ordered]@{
        Source = 'Source'
        Config = 'Config'
        AvidScript = 'Plugins/AvidScript'
        Puerts = 'Plugins/Puerts'
        AvidScriptPerfHarness = 'Plugins/AvidScriptPerfHarness'
    }
    $MarkerJunctions = Get-SidecarRequiredPropertyValue $Marker 'junctions' 'ASP53S2116' 'benchmark-project'
    $ActualJunctions = [ordered]@{}
    foreach ($Entry in $ExpectedJunctions.GetEnumerator()) {
        $ExpectedTarget = [string](Get-SidecarRequiredPropertyValue $MarkerJunctions $Entry.Key 'ASP53S2116' 'benchmark-project.junctions')
        $ActualTarget = Resolve-SidecarCanonicalDirectory -Path (Join-Path $ProjectRoot $Entry.Value) `
            -Code 'ASP53S2116' -Label "project junction $($Entry.Key)" -RequireJunction
        if ($ActualTarget -ine $ExpectedTarget) {
            throw "ASP53S2116 project junction target changed: name=$($Entry.Key) actual=$ActualTarget expected=$ExpectedTarget"
        }
        $ActualJunctions[$Entry.Key] = $ActualTarget
    }

    foreach ($Entry in @(@{ Name = 'source'; Path = $ActualJunctions.Source }, @{ Name = 'config'; Path = $ActualJunctions.Config })) {
        $Recorded = Get-SidecarRequiredPropertyValue $Marker $Entry.Name 'ASP53S2116' 'benchmark-project'
        $ExpectedCanonicalPath = [string](Get-SidecarRequiredPropertyValue $Recorded 'canonical_path' 'ASP53S2116' "benchmark-project.$($Entry.Name)")
        $ExpectedDigest = [string](Get-SidecarRequiredPropertyValue $Recorded 'content_sha256' 'ASP53S2116' "benchmark-project.$($Entry.Name)")
        $ExpectedCount = [int](Get-SidecarRequiredPropertyValue $Recorded 'file_count' 'ASP53S2116' "benchmark-project.$($Entry.Name)")
        if ($ExpectedCanonicalPath -ine $Entry.Path -or $ExpectedDigest -cnotmatch '^[0-9a-f]{64}$' -or $ExpectedCount -lt 0) {
            throw "ASP53S2116 benchmark-project $($Entry.Name) digest metadata is invalid"
        }
        $ActualDigest = Get-SidecarDirectoryContentDigest -Path $Entry.Path
        if ($ActualDigest.content_sha256 -cne $ExpectedDigest -or $ActualDigest.file_count -ne $ExpectedCount) {
            throw "ASP53S2116 project $($Entry.Name) content changed after marker creation"
        }
    }

    $MarkerCommit = [string](Get-SidecarRequiredPropertyValue $Marker 'candidate_commit' 'ASP53S2116' 'benchmark-project')
    $MarkerTree = [string](Get-SidecarRequiredPropertyValue $Marker 'candidate_tree' 'ASP53S2116' 'benchmark-project')
    if ($MarkerCommit -cne $AvidScriptCommit -or $MarkerTree -cne $AvidScriptTreeSha) {
        throw "ASP53S2116 marker candidate identity does not match command line: marker_commit=$MarkerCommit marker_tree=$MarkerTree"
    }
    $ActualCommit = ([string](@(Invoke-SidecarGit -RepositoryPath $ActualJunctions.AvidScript -Code 'ASP53S2116' rev-parse HEAD)[0])).Trim().ToLowerInvariant()
    $ActualTree = ([string](@(Invoke-SidecarGit -RepositoryPath $ActualJunctions.AvidScript -Code 'ASP53S2116' rev-parse 'HEAD^{tree}')[0])).Trim().ToLowerInvariant()
    $Status = @(Invoke-SidecarGit -RepositoryPath $ActualJunctions.AvidScript -Code 'ASP53S2116' status --porcelain=v1 --untracked-files=all)
    if ($ActualCommit -cne $AvidScriptCommit -or $ActualTree -cne $AvidScriptTreeSha -or ($Status -join "`n").Trim().Length -ne 0) {
        throw 'ASP53S2116 candidate Git HEAD/tree/clean status no longer matches the formal run identity'
    }

    return $ActualJunctions
}

function Assert-SidecarPuertsProvenance {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$PuertsCommit,
        [Parameter(Mandatory = $true)][string]$PuertsBackendSha256
    )

    $ProjectRoot = Split-Path -Parent ([System.IO.Path]::GetFullPath($ProjectPath))
    $LockPath = Join-Path $PSScriptRoot '../Config/PuertsDependency.lock.json'
    $Lock = Read-SidecarJson -Path $LockPath -Code 'ASP53S2117'
    $Installation = Get-SidecarRequiredPropertyValue $Lock 'installation' 'ASP53S2117' 'PuertsDependency.lock'
    $ManagedMarkerName = [string](Get-SidecarRequiredPropertyValue $Installation 'managed_marker_name' 'ASP53S2117' 'PuertsDependency.lock.installation')
    $PluginRelativePath = [string](Get-SidecarRequiredPropertyValue $Installation 'project_plugin_path' 'ASP53S2117' 'PuertsDependency.lock.installation')
    if ($ManagedMarkerName -cnotmatch '^(?:\.)?[A-Za-z0-9][A-Za-z0-9._-]*\.json$' -or $PluginRelativePath -cne 'Plugins/Puerts') {
        throw 'ASP53S2117 tracked Puerts dependency lock has an unsafe managed marker contract'
    }

    $PuertsPath = Join-Path $ProjectRoot $PluginRelativePath
    $Marker = Read-SidecarJson -Path (Join-Path $PuertsPath $ManagedMarkerName) -Code 'ASP53S2117'
    if ([int](Get-SidecarRequiredPropertyValue $Marker 'schema_version' 'ASP53S2117' 'Puerts managed marker') -ne 2) {
        throw 'ASP53S2117 Puerts managed marker schema_version must be 2'
    }
    $SourceCommit = [string](Get-SidecarRequiredPropertyValue $Marker 'source_commit_sha' 'ASP53S2117' 'Puerts managed marker')
    $BackendSha = [string](Get-SidecarRequiredPropertyValue $Marker 'backend_sha256' 'ASP53S2117' 'Puerts managed marker')
    $InstalledDigest = [string](Get-SidecarRequiredPropertyValue $Marker 'installed_content_sha256' 'ASP53S2117' 'Puerts managed marker')
    $InstalledFileCount = [int](Get-SidecarRequiredPropertyValue $Marker 'installed_file_count' 'ASP53S2117' 'Puerts managed marker')
    if ($SourceCommit -cne $PuertsCommit -or $BackendSha -cne $PuertsBackendSha256 -or
        $InstalledDigest -cnotmatch '^[0-9a-f]{64}$' -or $InstalledFileCount -lt 1) {
        throw 'ASP53S2117 Puerts managed marker does not bind the requested dependency identity'
    }
    $ActualDigest = Get-SidecarInstalledPuertsContentDigest -Path $PuertsPath -ManagedMarkerName $ManagedMarkerName
    if ($ActualDigest.content_sha256 -cne $InstalledDigest -or $ActualDigest.file_count -ne $InstalledFileCount) {
        throw 'ASP53S2117 Puerts installed content does not match the managed marker digest'
    }
}

function Assert-SidecarFormalArtifacts {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$WasmSha256,
        [Parameter(Mandatory = $true)][string]$ManifestSha256
    )

    $ProjectRoot = Split-Path -Parent ([System.IO.Path]::GetFullPath($ProjectPath))
    $ArtifactRoot = Join-Path $ProjectRoot 'Saved/AvidScriptCSharpGuest/Profiles/profile_phase53_perf'
    $ManifestPath = Join-Path $ArtifactRoot 'profile_phase53_perf.avidscript.json'
    $WasmPath = Join-Path $ArtifactRoot 'profile_phase53_perf.wasm'
    $ActualManifestSha = Get-SidecarFileSha256 -Path $ManifestPath
    $ActualWasmSha = Get-SidecarFileSha256 -Path $WasmPath
    if ($ActualManifestSha -cne $ManifestSha256 -or $ActualWasmSha -cne $WasmSha256) {
        throw 'ASP53S2118 formal Saved artifact digest does not match command line provenance'
    }
}

function Write-SidecarNewText {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value,
        [string]$Code = 'ASP53S2001'
    )

    $Parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $Parent -PathType Container)) {
        throw "$Code 写入目标目录不存在：$Parent"
    }

    $TemporaryPath = Join-Path $Parent (
        '.{0}.{1}.tmp' -f [System.IO.Path]::GetFileName($Path), [Guid]::NewGuid().ToString('N'))
    try {
        $Encoding = [System.Text.UTF8Encoding]::new($false)
        $Stream = [System.IO.FileStream]::new(
            $TemporaryPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read)
        try {
            $Writer = [System.IO.StreamWriter]::new($Stream, $Encoding, 4096, $true)
            try {
                $Writer.Write($Value)
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
        [System.IO.File]::Move($TemporaryPath, $Path)
    }
    catch [System.IO.IOException] {
        if (Test-Path -LiteralPath $TemporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
        throw "$Code 拒绝覆盖已有文件：$Path"
    }
}

function Write-SidecarNewJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value,
        [string]$Code = 'ASP53S2001'
    )

    $Json = (($Value | ConvertTo-Json -Depth 64) -replace "`r`n", "`n") + "`n"
    Write-SidecarNewText -Path $Path -Value $Json -Code $Code
}

function Copy-SidecarNewFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "ASP53S2002 快照源文件不存在：$Source"
    }
    $Bytes = [System.IO.File]::ReadAllBytes($Source)
    $Parent = Split-Path -Parent $Destination
    $TemporaryPath = Join-Path $Parent (
        '.{0}.{1}.tmp' -f [System.IO.Path]::GetFileName($Destination), [Guid]::NewGuid().ToString('N'))
    try {
        $Stream = [System.IO.FileStream]::new(
            $TemporaryPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read)
        try {
            $Stream.Write($Bytes, 0, $Bytes.Length)
            $Stream.Flush($true)
        }
        finally {
            $Stream.Dispose()
        }
        [System.IO.File]::Move($TemporaryPath, $Destination)
    }
    catch [System.IO.IOException] {
        if (Test-Path -LiteralPath $TemporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
        throw "ASP53S2002 拒绝覆盖已有快照：$Destination"
    }
}

function Resolve-SidecarChildPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    if ([System.IO.Path]::IsPathRooted($RelativePath)) {
        throw "ASP53S2003 attempt 清单只能使用相对路径：$RelativePath"
    }

    $ResolvedRoot = [System.IO.Path]::GetFullPath($Root)
    $ResolvedPath = [System.IO.Path]::GetFullPath((Join-Path $ResolvedRoot $RelativePath))
    $RootPrefix = $ResolvedRoot.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $ResolvedPath.StartsWith($RootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "ASP53S2003 attempt 相对路径越界：$RelativePath"
    }

    return $ResolvedPath
}

function Test-SidecarExactArray {
    param(
        [Parameter(Mandatory = $true)]$Actual,
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $ActualValues = @($Actual)
    $ExpectedValues = @($Expected)
    if ($ActualValues.Count -ne $ExpectedValues.Count) {
        throw "$Code $Label 数量不一致：actual=$($ActualValues.Count) expected=$($ExpectedValues.Count)"
    }
    for ($Index = 0; $Index -lt $ExpectedValues.Count; ++$Index) {
        if ([string]$ActualValues[$Index] -cne [string]$ExpectedValues[$Index]) {
            throw "$Code $Label 在索引 $Index 不一致：actual=$($ActualValues[$Index]) expected=$($ExpectedValues[$Index])"
        }
    }
}

function Test-SidecarProfile {
    param([Parameter(Mandatory = $true)]$Profile)

    if ([int]$Profile.schema_version -ne 1) {
        throw 'ASP53S2004 benchmark profile schema_version 必须为 1'
    }
    if ([string]::IsNullOrWhiteSpace([string]$Profile.profile_id) -or
        [string]::IsNullOrWhiteSpace([string]$Profile.target) -or
        [string]::IsNullOrWhiteSpace([string]$Profile.configuration)) {
        throw 'ASP53S2004 benchmark profile 缺少 profile_id、target 或 configuration'
    }
    if ([int]$Profile.process_runs -lt 1 -or
        [int]$Profile.warmup_samples -lt 0 -or
        [int]$Profile.timed_samples -lt 1) {
        throw 'ASP53S2004 benchmark profile 的进程、预热或样本数量无效'
    }
    if ([double]$Profile.minimum_sample_milliseconds -le 0.0 -or
        [int64]$Profile.minimum_iterations -lt 1 -or
        [int64]$Profile.maximum_iterations -lt [int64]$Profile.minimum_iterations) {
        throw 'ASP53S2004 benchmark profile 的校准时间或 iteration 范围无效'
    }

    $Lanes = @($Profile.lanes | ForEach-Object { [string]$_ })
    $Workloads = @($Profile.workloads | ForEach-Object { [string]$_ })
    if ($Lanes.Count -ne 4 -or $Workloads.Count -lt 1) {
        throw 'ASP53S2004 benchmark profile 必须包含四个 lane 和至少一个 workload'
    }
    if (@($Lanes | Sort-Object -Unique).Count -ne $Lanes.Count -or
        @($Workloads | Sort-Object -Unique).Count -ne $Workloads.Count) {
        throw 'ASP53S2004 benchmark profile 的 lane/workload 不允许重复'
    }
}

function Get-SidecarRotatedLaneOrder {
    param(
        [Parameter(Mandatory = $true)][string[]]$Lanes,
        [Parameter(Mandatory = $true)][int]$ProcessRun
    )

    if ($Lanes.Count -eq 0) {
        throw 'ASP53S2004 无法轮转空 lane 列表'
    }

    $Offset = $ProcessRun % $Lanes.Count
    $Result = [System.Collections.Generic.List[string]]::new()
    for ($Index = 0; $Index -lt $Lanes.Count; ++$Index) {
        $Result.Add($Lanes[($Index + $Offset) % $Lanes.Count])
    }
    return $Result.ToArray()
}

function Get-SidecarWilliamsLaneOrder {
    param(
        [Parameter(Mandatory = $true)][string[]]$BaseLaneOrder,
        [Parameter(Mandatory = $true)][int]$ProcessRun,
        [Parameter(Mandatory = $true)][int]$WorkloadIndex,
        [Parameter(Mandatory = $true)][int]$SampleIndex
    )

    if ($BaseLaneOrder.Count -ne 4) {
        throw 'ASP53S2004 Williams 顺序要求恰好四个 lane'
    }
    $BalancedRows = @(
        @(0, 1, 3, 2),
        @(1, 2, 0, 3),
        @(2, 3, 1, 0),
        @(3, 0, 2, 1)
    )
    $Row = (($ProcessRun + $WorkloadIndex + $SampleIndex) % 4 + 4) % 4
    return @($BalancedRows[$Row] | ForEach-Object { $BaseLaneOrder[$_] })
}

function Get-SidecarExpectedSampleSeed {
    param(
        [Parameter(Mandatory = $true)][int]$Seed,
        [Parameter(Mandatory = $true)][int]$WorkloadIndex,
        [Parameter(Mandatory = $true)][int]$SampleIndex
    )

    [uint64]$SeedBits = [int64]$Seed -band 0xffffffffL
    [uint64]$WorkloadBits = (
        [uint64]($WorkloadIndex + 1) * [uint64]2654435769) -band 0xffffffffL
    [uint64]$Value = ($SeedBits -bxor $WorkloadBits -bxor [uint64]($SampleIndex + 1)) -band 0xffffffffL
    [uint64]$Mixed = (
        ($Value * [uint64]1664525) + [uint64]1013904223) -band 0xffffffffL
    return [int]($Mixed -band 0x007fffffL)
}

function Get-SidecarProvenanceFieldNames {
    return @(
        'ue_version',
        'ue_build_id',
        'target',
        'configuration',
        'avidscript_commit',
        'puerts_commit',
        'puerts_backend_sha256',
        'null_rhi',
        'cpu',
        'os',
        'wamr_mode',
        'v8_mode',
        'wasm_sha256',
        'manifest_sha256',
        'avidscript_tree_sha',
        'avidscript_dirty',
        'allow_non_formal_profile',
        'profile_id',
        'profile_sha256',
        'request_schema_sha256',
        'calibration_schema_sha256',
        'result_schema_sha256',
        'aggregate_schema_sha256'
    )
}

function Test-SidecarProvenance {
    param(
        [Parameter(Mandatory = $true)]$Actual,
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)][string]$Label
    )

    foreach ($Property in @(Get-SidecarProvenanceFieldNames)) {
        $ActualProperty = $Actual.PSObject.Properties[$Property]
        $ExpectedProperty = $Expected.PSObject.Properties[$Property]
        if ($null -eq $ActualProperty -or $null -eq $ExpectedProperty) {
            throw "ASP53S2014 provenance 缺少固定字段：$Label field=$Property"
        }
        $ActualValue = $ActualProperty.Value
        $ExpectedValue = $ExpectedProperty.Value
        if ($ActualValue -is [bool] -or $ExpectedValue -is [bool]) {
            if ([bool]$ActualValue -ne [bool]$ExpectedValue) {
                throw "ASP53S2014 provenance 混用：$Label field=$Property actual=$ActualValue expected=$ExpectedValue"
            }
        }
        elseif ([string]$ActualValue -cne [string]$ExpectedValue) {
            throw "ASP53S2014 provenance 混用：$Label field=$Property actual=$ActualValue expected=$ExpectedValue"
        }
    }
}

function Test-SidecarCalibrationResult {
    param(
        [Parameter(Mandatory = $true)][string]$ResultPath,
        [Parameter(Mandatory = $true)][string]$SchemaPath,
        [Parameter(Mandatory = $true)]$Profile,
        [Parameter(Mandatory = $true)]$ExpectedProvenance
    )

    if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
        throw "ASP53S2041 calibration Editor 未生成 calibration.json：$ResultPath"
    }
    $Raw = Get-Content -LiteralPath $ResultPath -Raw
    if (-not ($Raw | Test-Json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue)) {
        throw "ASP53S2041 calibration.json 不符合固定 Schema：$ResultPath"
    }
    $Result = $Raw | ConvertFrom-Json
    Test-SidecarProvenance -Actual $Result.provenance -Expected $ExpectedProvenance -Label 'calibration'

    $ExpectedWorkloads = @($Profile.workloads | ForEach-Object { [string]$_ })
    $ExpectedLanes = @($Profile.lanes | ForEach-Object { [string]$_ })
    $ActualWorkloads = @($Result.iteration_counts.PSObject.Properties.Name | ForEach-Object { [string]$_ })
    if ($ActualWorkloads.Count -ne $ExpectedWorkloads.Count) {
        throw "ASP53S2042 calibration iteration map 数量不完整：actual=$($ActualWorkloads.Count) expected=$($ExpectedWorkloads.Count)"
    }
    foreach ($Workload in $ExpectedWorkloads) {
        if ($ActualWorkloads -cnotcontains $Workload) {
            throw "ASP53S2042 calibration iteration map 缺少 workload：$Workload"
        }
        $LaneCounts = $Result.iteration_counts.$Workload
        $ActualLanes = @($LaneCounts.PSObject.Properties.Name | ForEach-Object { [string]$_ })
        if ($ActualLanes.Count -ne $ExpectedLanes.Count) {
            throw "ASP53S2042 calibration lane iteration map 数量不完整：workload=$Workload"
        }
        foreach ($Lane in $ExpectedLanes) {
            if ($ActualLanes -cnotcontains $Lane) {
                throw "ASP53S2042 calibration iteration map 缺少 lane：workload=$Workload lane=$Lane"
            }
            $Iterations = [int64]$LaneCounts.$Lane
            if ($Iterations -lt [int64]$Profile.minimum_iterations -or
                $Iterations -gt [int64]$Profile.maximum_iterations) {
                throw "ASP53S2043 calibration iterations 越界：workload=$Workload lane=$Lane iterations=$Iterations"
            }
        }
    }

    return [pscustomobject][ordered]@{
        result = $Result
        sha256 = Get-SidecarFileSha256 -Path $ResultPath
    }
}

function Test-SidecarProcessResult {
    param(
        [Parameter(Mandatory = $true)][string]$ResultPath,
        [Parameter(Mandatory = $true)][string]$SchemaPath,
        [Parameter(Mandatory = $true)]$Profile,
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][int]$ExpectedProcessRun
    )

    if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
        throw "ASP53S2005 Editor 进程未生成 raw result：$ResultPath"
    }

    $Raw = Get-Content -LiteralPath $ResultPath -Raw
    if (-not ($Raw | Test-Json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue)) {
        throw "ASP53S2005 raw result 不符合固定 Schema：$ResultPath"
    }
    $Result = $Raw | ConvertFrom-Json

    if ([int]$Result.schema_version -ne [int]$Manifest.result_schema.version) {
        throw "ASP53S2013 raw result schema_version 混用：process=$ExpectedProcessRun"
    }
    if ([int]$Result.process_run -ne $ExpectedProcessRun) {
        throw "ASP53S2006 raw result 的 process_run 不匹配：actual=$($Result.process_run) expected=$ExpectedProcessRun"
    }

    $ExpectedLaneOrder = Get-SidecarRotatedLaneOrder -Lanes @($Profile.lanes) -ProcessRun $ExpectedProcessRun
    Test-SidecarExactArray -Actual $Result.lane_order -Expected $ExpectedLaneOrder `
        -Code 'ASP53S2007' -Label "process $ExpectedProcessRun lane_order"

    Test-SidecarProvenance `
        -Actual $Result.provenance `
        -Expected $Manifest.provenance `
        -Label "process=$ExpectedProcessRun"

    $ExpectedLanes = @($Profile.lanes | ForEach-Object { [string]$_ })
    $ExpectedWorkloads = @($Profile.workloads | ForEach-Object { [string]$_ })
    $TimedSamples = [int]$Profile.timed_samples
    $ExpectedCount = $ExpectedLanes.Count * $ExpectedWorkloads.Count * $TimedSamples
    $Samples = @($Result.samples)
    if ($Samples.Count -ne $ExpectedCount) {
        throw "ASP53S2008 raw result 样本矩阵不完整：process=$ExpectedProcessRun actual=$($Samples.Count) expected=$ExpectedCount"
    }

    $SamplesByKey = @{}
    $PositionsByKey = @{}
    foreach ($Sample in $Samples) {
        $Lane = [string]$Sample.lane
        $Workload = [string]$Sample.workload
        $SampleIndex = [int]$Sample.sample_index
        $LanePosition = [int]$Sample.lane_position
        if ([int]$Sample.process_run -ne $ExpectedProcessRun -or
            $Lane -cnotin $ExpectedLanes -or
            $Workload -cnotin $ExpectedWorkloads -or
            $SampleIndex -lt 0 -or
            $SampleIndex -ge $TimedSamples) {
            throw "ASP53S2009 raw result 含越界样本：process=$ExpectedProcessRun lane=$Lane workload=$Workload sample=$SampleIndex"
        }
        $WorkloadIndex = [Array]::IndexOf([object[]]$ExpectedWorkloads, $Workload)
        $WilliamsOrder = Get-SidecarWilliamsLaneOrder `
            -BaseLaneOrder @($Result.lane_order) `
            -ProcessRun $ExpectedProcessRun `
            -WorkloadIndex $WorkloadIndex `
            -SampleIndex $SampleIndex
        if ($LanePosition -lt 0 -or
            $LanePosition -ge $ExpectedLanes.Count -or
            [string]$WilliamsOrder[$LanePosition] -cne $Lane) {
            throw "ASP53S2040 lane_position 与 Williams 顺序不匹配：process=$ExpectedProcessRun lane=$Lane workload=$Workload sample=$SampleIndex position=$LanePosition"
        }
        $PositionKey = "$Workload|$SampleIndex|$LanePosition"
        if ($PositionsByKey.ContainsKey($PositionKey)) {
            throw "ASP53S2040 同一 workload/sample 的 lane_position 重复：process=$ExpectedProcessRun key=$PositionKey"
        }
        $PositionsByKey[$PositionKey] = $true

        $ExpectedSeed = Get-SidecarExpectedSampleSeed `
            -Seed ([int]$Profile.seed) `
            -WorkloadIndex $WorkloadIndex `
            -SampleIndex $SampleIndex
        if ([int]$Sample.seed -ne $ExpectedSeed) {
            throw "ASP53S2035 sample 派生 seed 不匹配：process=$ExpectedProcessRun lane=$Lane workload=$Workload sample=$SampleIndex actual=$($Sample.seed) expected=$ExpectedSeed"
        }
        if ([int64]$Sample.checksum -ne [int64]$Sample.expected_checksum) {
            throw "ASP53S2036 checksum 未达到 expected_checksum：process=$ExpectedProcessRun lane=$Lane workload=$Workload sample=$SampleIndex"
        }
        if ([double]$Sample.final_scalar -ne [double]$Sample.expected_final_scalar) {
            throw "ASP53S2037 final_scalar 未达到 expected_final_scalar：process=$ExpectedProcessRun lane=$Lane workload=$Workload sample=$SampleIndex"
        }
        if ([int64]$Sample.operation_call_count -ne [int64]$Sample.expected_operation_call_count) {
            throw "ASP53S2038 operation_call_count 未达到 expected 值：process=$ExpectedProcessRun lane=$Lane workload=$Workload sample=$SampleIndex"
        }
        if ([int64]$Sample.host_import_call_count -ne [int64]$Sample.expected_host_import_call_count) {
            throw "ASP53S2039 host_import_call_count 未达到 expected 值：process=$ExpectedProcessRun lane=$Lane workload=$Workload sample=$SampleIndex"
        }
        $CalibratedIterations = [int64]$Manifest.calibration.iteration_counts.$Workload.$Lane
        if ([int64]$Sample.iterations -ne $CalibratedIterations) {
            throw "ASP53S2044 timed sample 未使用固定 calibration iterations：process=$ExpectedProcessRun lane=$Lane workload=$Workload actual=$($Sample.iterations) expected=$CalibratedIterations"
        }

        $Key = "$Workload|$SampleIndex|$Lane"
        if ($SamplesByKey.ContainsKey($Key)) {
            throw "ASP53S2010 raw result 含重复样本：process=$ExpectedProcessRun key=$Key"
        }
        $SamplesByKey[$Key] = $Sample
    }

    foreach ($Workload in $ExpectedWorkloads) {
        for ($SampleIndex = 0; $SampleIndex -lt $TimedSamples; ++$SampleIndex) {
            foreach ($Lane in $ExpectedLanes) {
                $Key = "$Workload|$SampleIndex|$Lane"
                if (-not $SamplesByKey.ContainsKey($Key)) {
                    throw "ASP53S2011 raw result 缺少样本：process=$ExpectedProcessRun key=$Key"
                }
            }
        }
    }

    return [pscustomobject][ordered]@{
        result = $Result
        sha256 = Get-SidecarFileSha256 -Path $ResultPath
        sample_count = $Samples.Count
    }
}

function Get-SidecarNearestRankPercentile {
    param(
        [Parameter(Mandatory = $true)][double[]]$Values,
        [Parameter(Mandatory = $true)][ValidateRange(0.0, 1.0)][double]$Percentile
    )

    if ($Values.Count -eq 0) {
        throw 'ASP53S2020 无法计算空样本集合的百分位数'
    }

    $Sorted = @($Values | Sort-Object)
    $Rank = [Math]::Max(1, [Math]::Ceiling($Percentile * $Sorted.Count))
    return [double]$Sorted[[int]$Rank - 1]
}

function Get-SidecarGeometricMean {
    param([Parameter(Mandatory = $true)][double[]]$Values)

    if ($Values.Count -eq 0 -or @($Values | Where-Object { $_ -le 0 }).Count -gt 0) {
        throw 'ASP53S2021 几何平均数要求全部样本大于零'
    }

    $LogSum = 0.0
    foreach ($Value in $Values) {
        $LogSum += [Math]::Log($Value)
    }
    return [Math]::Exp($LogSum / $Values.Count)
}
