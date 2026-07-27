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

function Get-SidecarDependencyLockSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "ASP53S2000 无法计算缺失文件的 SHA-256：$Path"
    }

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
    $ExcludedRelativePrefixes = @(
        'Source/CSharpParamDefaultValueMetas/bin/',
        'Source/CSharpParamDefaultValueMetas/obj/')
    $ExcludedRelativeFiles = @(
        'Source/CSharpParamDefaultValueMetas/CSharpParamDefaultValueMetas.ubtplugin.csproj.props')
    $Entries = [System.Collections.Generic.List[string]]::new()
    foreach ($File in @(Get-ChildItem -LiteralPath $Root -File -Force -Recurse)) {
        $RelativePath = [System.IO.Path]::GetRelativePath($Root, $File.FullName).Replace('\', '/')
        if ($RelativePath -ieq $ManagedMarkerName) {
            continue
        }
        if ($ExcludedRelativeFiles -icontains $RelativePath) {
            continue
        }
        $PathParts = @($RelativePath.Split('/', [System.StringSplitOptions]::RemoveEmptyEntries))
        $RootDirectory = if ($PathParts.Count -gt 1) { $PathParts[0] } else { '' }
        if ($ExcludedDirectoryNames -icontains $RootDirectory) {
            continue
        }
        $IsKnownGeneratedFile = $false
        foreach ($Prefix in $ExcludedRelativePrefixes) {
            if ($RelativePath.StartsWith($Prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $IsKnownGeneratedFile = $true
                break
            }
        }
        if ($IsKnownGeneratedFile) {
            continue
        }
        $Entries.Add(("{0}`t{1}" -f $RelativePath, (Get-SidecarFileSha256 -Path $File.FullName)))
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

function Get-SidecarEditorIdentity {
    param([Parameter(Mandatory = $true)][string]$EditorExecutable)

    if (-not (Test-Path -LiteralPath $EditorExecutable -PathType Leaf)) {
        throw "ASP53S2119 UnrealEditor-Cmd executable is missing: $EditorExecutable"
    }
    try {
        $VersionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($EditorExecutable)
    }
    catch {
        throw "ASP53S2119 could not read executable version metadata: $EditorExecutable`n$($_.Exception.Message)"
    }

    return [pscustomobject][ordered]@{
        sha256 = Get-SidecarFileSha256 -Path $EditorExecutable
        file_version = if ([string]::IsNullOrWhiteSpace([string]$VersionInfo.FileVersion)) { 'unknown' } else { [string]$VersionInfo.FileVersion }
        product_name = [string]$VersionInfo.ProductName
        file_description = [string]$VersionInfo.FileDescription
    }
}

function Assert-SidecarFormalEditorExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$EditorExecutable,
        [Parameter(Mandatory = $true)][string]$UeVersion
    )

    $ResolvedEditorExecutable = [System.IO.Path]::GetFullPath($EditorExecutable)
    $Win64Directory = Split-Path -Parent $ResolvedEditorExecutable
    $BinariesDirectory = Split-Path -Parent $Win64Directory
    $EngineDirectory = Split-Path -Parent $BinariesDirectory
    if ([System.IO.Path]::GetFileName($ResolvedEditorExecutable) -ine 'UnrealEditor-Cmd.exe' -or
        [System.IO.Path]::GetFileName($Win64Directory) -ine 'Win64' -or
        [System.IO.Path]::GetFileName($BinariesDirectory) -ine 'Binaries' -or
        [System.IO.Path]::GetFileName($EngineDirectory) -ine 'Engine') {
        throw 'ASP53S2119 formal benchmark requires Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
    }

    $Identity = Get-SidecarEditorIdentity -EditorExecutable $ResolvedEditorExecutable
    if ($Identity.product_name -cne 'UnrealEditor' -or
        $Identity.file_description -cne 'UnrealEditor' -or
        -not $Identity.file_version.StartsWith($UeVersion, [System.StringComparison]::Ordinal)) {
        throw 'ASP53S2119 formal benchmark UnrealEditor-Cmd version metadata does not match UeVersion'
    }
    return $Identity
}

function Assert-SidecarRunnerCandidate {
    param(
        [Parameter(Mandatory = $true)][string]$PluginRoot,
        [Parameter(Mandatory = $true)][string]$CandidateRoot
    )

    $ResolvedPluginRoot = Resolve-SidecarCanonicalDirectory -Path $PluginRoot -Code 'ASP53S2120' -Label 'runner plugin root'
    if ($ResolvedPluginRoot -ine $CandidateRoot) {
        throw "ASP53S2120 formal runner checkout is not the marker candidate: runner=$ResolvedPluginRoot candidate=$CandidateRoot"
    }
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
    $LockSource = Get-SidecarRequiredPropertyValue $Lock 'source' 'ASP53S2117' 'PuertsDependency.lock'
    $LockBackend = Get-SidecarRequiredPropertyValue $Lock 'backend' 'ASP53S2117' 'PuertsDependency.lock'
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
    $SourcePluginTree = [string](Get-SidecarRequiredPropertyValue $Marker 'source_plugin_tree_sha1' 'ASP53S2117' 'Puerts managed marker')
    $SourceRepositoryUrl = [string](Get-SidecarRequiredPropertyValue $Marker 'source_repository_url' 'ASP53S2117' 'Puerts managed marker')
    $BackendSha = [string](Get-SidecarRequiredPropertyValue $Marker 'backend_sha256' 'ASP53S2117' 'Puerts managed marker')
    $BackendAssetName = [string](Get-SidecarRequiredPropertyValue $Marker 'backend_asset_name' 'ASP53S2117' 'Puerts managed marker')
    $LockSha256 = [string](Get-SidecarRequiredPropertyValue $Marker 'lock_sha256' 'ASP53S2117' 'Puerts managed marker')
    $InstalledDigest = [string](Get-SidecarRequiredPropertyValue $Marker 'installed_content_sha256' 'ASP53S2117' 'Puerts managed marker')
    $InstalledFileCount = [int](Get-SidecarRequiredPropertyValue $Marker 'installed_file_count' 'ASP53S2117' 'Puerts managed marker')
    $ExpectedSourceCommit = [string](Get-SidecarRequiredPropertyValue $LockSource 'commit_sha' 'ASP53S2117' 'PuertsDependency.lock.source')
    $ExpectedSourcePluginTree = [string](Get-SidecarRequiredPropertyValue $LockSource 'plugin_tree_sha1' 'ASP53S2117' 'PuertsDependency.lock.source')
    $ExpectedSourceRepositoryUrl = [string](Get-SidecarRequiredPropertyValue $LockSource 'repository_url' 'ASP53S2117' 'PuertsDependency.lock.source')
    $ExpectedBackendSha = [string](Get-SidecarRequiredPropertyValue $LockBackend 'sha256' 'ASP53S2117' 'PuertsDependency.lock.backend')
    $ExpectedBackendAssetName = [string](Get-SidecarRequiredPropertyValue $LockBackend 'asset_name' 'ASP53S2117' 'PuertsDependency.lock.backend')
    $ExpectedLockSha256 = Get-SidecarDependencyLockSha256 -Path $LockPath
    if ($PuertsCommit -cne $ExpectedSourceCommit -or $PuertsBackendSha256 -cne $ExpectedBackendSha -or
        $SourceCommit -cne $ExpectedSourceCommit -or $SourcePluginTree -cne $ExpectedSourcePluginTree -or
        $SourceRepositoryUrl -cne $ExpectedSourceRepositoryUrl -or $BackendSha -cne $ExpectedBackendSha -or
        $BackendAssetName -cne $ExpectedBackendAssetName -or $LockSha256 -cne $ExpectedLockSha256 -or
        $InstalledDigest -cnotmatch '^[0-9a-f]{64}$' -or $InstalledFileCount -lt 1) {
        throw 'ASP53S2117 Puerts managed marker/command line identity does not match the tracked dependency lock'
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

    if ([int]$Profile.schema_version -ne 2) {
        throw 'ASP54S2004 benchmark profile schema_version 必须为 2'
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
    $ExpectedLanes = @(
        'native_cpp',
        'puerts_v8_reflection',
        'puerts_v8_static',
        'avidscript_wasmtime_semantic',
        'avidscript_wasmtime_native_direct'
    )
    if ($Lanes.Count -ne $ExpectedLanes.Count -or $Workloads.Count -lt 1) {
        throw 'ASP54S2004 benchmark profile 必须包含五个 canonical lane 和至少一个 workload'
    }
    if (@($Lanes | Sort-Object -Unique).Count -ne $Lanes.Count -or
        @($Workloads | Sort-Object -Unique).Count -ne $Workloads.Count) {
        throw 'ASP53S2004 benchmark profile 的 lane/workload 不允许重复'
    }
    if ([string]$Profile.lane_identity_algorithm -cne 'canonical_json_utf8_sha256_v1' -or
        @($Profile.lane_catalog).Count -ne $ExpectedLanes.Count) {
        throw 'ASP54S2004 benchmark profile 缺少固定 lane catalog 或 identity 算法'
    }
    for ($Index = 0; $Index -lt $ExpectedLanes.Count; ++$Index) {
        if ($Lanes[$Index] -cne $ExpectedLanes[$Index] -or
            [string]$Profile.lane_catalog[$Index].lane_id -cne $ExpectedLanes[$Index]) {
            throw "ASP54S2004 benchmark profile lane/catalog 顺序不匹配：index=$Index"
        }
    }
    $SemanticLane = @($Profile.lane_catalog | Where-Object {
        [string]$_.lane_id -ceq 'avidscript_wasmtime_semantic'
    })[0]
    $DirectLane = @($Profile.lane_catalog | Where-Object {
        [string]$_.lane_id -ceq 'avidscript_wasmtime_native_direct'
    })[0]
    if ([string]$SemanticLane.backend_id -cne 'wasmtime.cranelift.jit' -or
        [string]$DirectLane.backend_id -cne 'wasmtime.cranelift.jit' -or
        [string]$SemanticLane.binding_invocation_mode -cne 'semantic_process_event' -or
        [string]$DirectLane.binding_invocation_mode -cne 'qualified_native_direct') {
        throw 'ASP54S2004 Wasmtime lane backend 或 binding invocation mode 无效'
    }
    foreach ($Property in @(
        'runtime_id',
        'runtime_version',
        'source_wasm_sha256',
        'execution_artifact_sha256',
        'runtime_build_identity',
        'runtime_artifact_sha256',
        'backend_id')) {
        if ([string]$SemanticLane.$Property -cne [string]$DirectLane.$Property) {
            throw "ASP54S2004 Wasmtime lane 未共享 artifact provenance：field=$Property"
        }
    }
}

function Get-SidecarAvidScriptRuntimeIdentity {
    param(
        [Parameter(Mandatory = $true)][string]$PluginRoot
    )

    $ResolvedPluginRoot = Resolve-SidecarCanonicalDirectory `
        -Path $PluginRoot `
        -Code 'ASP54S2056' `
        -Label 'AvidScript runtime identity root'
    $WamrCandidates = @(
        (Join-Path $ResolvedPluginRoot 'Source/ThirdParty/WAMR/lib/Win64/Release/iwasm.lib'),
        (Join-Path $ResolvedPluginRoot 'Source/ThirdParty/WAMR/lib/Win64/Release/libiwasm.lib'),
        (Join-Path $ResolvedPluginRoot 'Source/ThirdParty/WAMR/lib/Win64/Release/vmlib.lib')
    )
    $WamrLibrary = @(
        $WamrCandidates | Where-Object {
            Test-Path -LiteralPath $_ -PathType Leaf
        }
    ) | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace([string]$WamrLibrary)) {
        throw 'ASP54S2056 WAMR linked static runtime artifact is missing'
    }
    $WasmtimeCandidates = @(
        (Join-Path $ResolvedPluginRoot 'Binaries/Win64/wasmtime.dll'),
        (Join-Path $ResolvedPluginRoot 'Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0/lib/wasmtime.dll')
    )
    $WasmtimeDll = @(
        $WasmtimeCandidates | Where-Object {
            Test-Path -LiteralPath $_ -PathType Leaf
        }
    ) | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace([string]$WasmtimeDll)) {
        throw 'ASP54S2056 Wasmtime staged or managed runtime DLL is missing'
    }

    $WamrSha256 = Get-SidecarFileSha256 -Path ([string]$WamrLibrary)
    $WasmtimeSha256 = Get-SidecarFileSha256 -Path ([string]$WasmtimeDll)
    return [pscustomobject][ordered]@{
        wamr_static_lib_sha256 = $WamrSha256
        wamr_runtime_build_identity = (
            'wamr-v2.4.4;config=interp=1,fast_interp=1,aot=0,jit=0,fast_jit=0,simd=1,simde=1;' +
            "static_lib_sha256=$WamrSha256")
        wasmtime_dll_sha256 = $WasmtimeSha256
        wasmtime_runtime_build_identity = (
            "wasmtime-v45.0.0;cranelift=1;dll_sha256=$WasmtimeSha256")
    }
}

function ConvertTo-SidecarCanonicalJsonString {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    $Builder = [System.Text.StringBuilder]::new()
    [void]$Builder.Append('"')
    foreach ($Character in $Value.ToCharArray()) {
        $Code = [int]$Character
        switch ($Code) {
            0x08 { [void]$Builder.Append('\b'); continue }
            0x09 { [void]$Builder.Append('\t'); continue }
            0x0a { [void]$Builder.Append('\n'); continue }
            0x0c { [void]$Builder.Append('\f'); continue }
            0x0d { [void]$Builder.Append('\r'); continue }
            0x22 { [void]$Builder.Append('\"'); continue }
            0x5c { [void]$Builder.Append('\\'); continue }
        }
        if ($Code -lt 0x20) {
            [void]$Builder.AppendFormat(
                [System.Globalization.CultureInfo]::InvariantCulture,
                '\u{0:x4}',
                $Code)
        }
        else {
            [void]$Builder.Append($Character)
        }
    }
    [void]$Builder.Append('"')
    return $Builder.ToString()
}

function ConvertTo-SidecarCanonicalJson {
    param([AllowNull()]$Value)

    if ($null -eq $Value) {
        return 'null'
    }
    if ($Value -is [string]) {
        return ConvertTo-SidecarCanonicalJsonString -Value ([string]$Value)
    }
    if ($Value -is [bool]) {
        return $(if ([bool]$Value) { 'true' } else { 'false' })
    }
    if ($Value -is [System.Collections.IDictionary] -or
        $Value -is [System.Management.Automation.PSCustomObject]) {
        $Properties = @{}
        if ($Value -is [System.Collections.IDictionary]) {
            foreach ($Key in $Value.Keys) {
                $Properties[[string]$Key] = $Value[$Key]
            }
        }
        else {
            foreach ($Property in $Value.PSObject.Properties) {
                $Properties[[string]$Property.Name] = $Property.Value
            }
        }
        $Names = [string[]]@($Properties.Keys)
        [Array]::Sort($Names, [System.StringComparer]::Ordinal)
        $Members = foreach ($Name in $Names) {
            (ConvertTo-SidecarCanonicalJsonString -Value $Name) + ':' +
                (ConvertTo-SidecarCanonicalJson -Value $Properties[$Name])
        }
        return '{' + ($Members -join ',') + '}'
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        $Items = foreach ($Item in $Value) {
            ConvertTo-SidecarCanonicalJson -Value $Item
        }
        return '[' + ($Items -join ',') + ']'
    }
    if ($Value -is [byte] -or
        $Value -is [sbyte] -or
        $Value -is [int16] -or
        $Value -is [uint16] -or
        $Value -is [int32] -or
        $Value -is [uint32] -or
        $Value -is [int64] -or
        $Value -is [uint64]) {
        return ([System.IFormattable]$Value).ToString(
            $null,
            [System.Globalization.CultureInfo]::InvariantCulture)
    }
    if ($Value -is [single] -or $Value -is [double] -or $Value -is [decimal]) {
        $Number = [double]$Value
        if ([double]::IsNaN($Number) -or [double]::IsInfinity($Number)) {
            throw 'ASP54S2055 canonical JSON 不允许 NaN 或 Infinity'
        }
        return $Number.ToString(
            'R',
            [System.Globalization.CultureInfo]::InvariantCulture)
    }
    throw "ASP54S2055 canonical JSON 不支持值类型：$($Value.GetType().FullName)"
}

function Get-SidecarCanonicalJsonSha256 {
    param([Parameter(Mandatory = $true)]$Value)

    $Json = ConvertTo-SidecarCanonicalJson -Value $Value
    $Bytes = [System.Text.UTF8Encoding]::new($false, $true).GetBytes($Json)
    $Hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [Convert]::ToHexString($Hasher.ComputeHash($Bytes)).ToLowerInvariant()
    }
    finally {
        $Hasher.Dispose()
    }
}

function Get-SidecarLaneIdentitySha256 {
    param([Parameter(Mandatory = $true)]$Entry)

    $IdentityInput = [ordered]@{}
    if ($Entry -is [System.Collections.IDictionary]) {
        foreach ($Key in $Entry.Keys) {
            if ([string]$Key -cne 'lane_identity_sha256') {
                $IdentityInput[[string]$Key] = $Entry[$Key]
            }
        }
    }
    else {
        foreach ($Property in $Entry.PSObject.Properties) {
            if ($Property.Name -cne 'lane_identity_sha256') {
                $IdentityInput[$Property.Name] = $Property.Value
            }
        }
    }
    return Get-SidecarCanonicalJsonSha256 -Value $IdentityInput
}

function Get-SidecarLaneCatalogSha256 {
    param([Parameter(Mandatory = $true)]$Catalog)

    return Get-SidecarCanonicalJsonSha256 -Value @($Catalog)
}

function Resolve-SidecarCatalogValue {
    param(
        [AllowNull()]$Value,
        [Parameter(Mandatory = $true)][hashtable]$Tokens
    )

    if ($null -eq $Value) {
        return $null
    }
    if ($Value -is [string]) {
        if ($Tokens.ContainsKey([string]$Value)) {
            return $Tokens[[string]$Value]
        }
        return [string]$Value
    }
    if ($Value -is [System.Collections.IEnumerable] -and
        $Value -isnot [System.Management.Automation.PSCustomObject]) {
        $ResolvedItems = [System.Collections.Generic.List[object]]::new()
        foreach ($Item in @($Value)) {
            $ResolvedItems.Add((Resolve-SidecarCatalogValue -Value $Item -Tokens $Tokens))
        }
        return ,$ResolvedItems.ToArray()
    }
    if ($Value -is [System.Management.Automation.PSCustomObject] -or
        $Value -is [System.Collections.IDictionary]) {
        $Resolved = [ordered]@{}
        foreach ($Property in $Value.PSObject.Properties) {
            $Resolved[$Property.Name] =
                Resolve-SidecarCatalogValue -Value $Property.Value -Tokens $Tokens
        }
        return [pscustomobject]$Resolved
    }
    return $Value
}

function New-SidecarResolvedLaneCatalog {
    param(
        [Parameter(Mandatory = $true)]$Profile,
        [Parameter(Mandatory = $true)][hashtable]$Tokens
    )

    $Catalog = [System.Collections.Generic.List[object]]::new()
    foreach ($TemplateEntry in @($Profile.lane_catalog)) {
        $ResolvedEntry = Resolve-SidecarCatalogValue -Value $TemplateEntry -Tokens $Tokens
        $Identity = Get-SidecarLaneIdentitySha256 -Entry $ResolvedEntry
        $ResolvedEntry | Add-Member -NotePropertyName lane_identity_sha256 -NotePropertyValue $Identity
        $Catalog.Add($ResolvedEntry)
    }
    $CatalogArray = $Catalog.ToArray()
    return [pscustomobject][ordered]@{
        entries = $CatalogArray
        sha256 = Get-SidecarLaneCatalogSha256 -Catalog $CatalogArray
    }
}

function Test-SidecarLaneCatalog {
    param(
        [Parameter(Mandatory = $true)]$Actual,
        [Parameter(Mandatory = $true)][string]$ActualSha256,
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $ActualEntries = @($Actual)
    $ExpectedEntries = @($Expected)
    $ActualLaneIdentitiesValid = @(
        $ActualEntries | Where-Object {
            [string]$_.lane_identity_sha256 -cne
                (Get-SidecarLaneIdentitySha256 -Entry $_)
        }
    ).Count -eq 0
    $ExpectedLaneIdentitiesValid = @(
        $ExpectedEntries | Where-Object {
            [string]$_.lane_identity_sha256 -cne
                (Get-SidecarLaneIdentitySha256 -Entry $_)
        }
    ).Count -eq 0
    $SemanticLane = @($ActualEntries | Where-Object {
        [string]$_.lane_id -ceq 'avidscript_wasmtime_semantic'
    })[0]
    $DirectLane = @($ActualEntries | Where-Object {
        [string]$_.lane_id -ceq 'avidscript_wasmtime_native_direct'
    })[0]
    $WasmtimeModesValid =
        $null -ne $SemanticLane -and
        $null -ne $DirectLane -and
        [string]$SemanticLane.backend_id -ceq 'wasmtime.cranelift.jit' -and
        [string]$DirectLane.backend_id -ceq 'wasmtime.cranelift.jit' -and
        [string]$SemanticLane.binding_invocation_mode -ceq 'semantic_process_event' -and
        [string]$DirectLane.binding_invocation_mode -ceq 'qualified_native_direct' -and
        [string]$SemanticLane.lane_identity_sha256 -cne [string]$DirectLane.lane_identity_sha256
    if ($ActualSha256 -cne $ExpectedSha256 -or
        -not $ActualLaneIdentitiesValid -or
        -not $ExpectedLaneIdentitiesValid -or
        -not $WasmtimeModesValid -or
        (Get-SidecarLaneCatalogSha256 -Catalog $ActualEntries) -cne $ActualSha256 -or
        (Get-SidecarLaneCatalogSha256 -Catalog $ExpectedEntries) -cne $ExpectedSha256 -or
        (ConvertTo-SidecarCanonicalJson -Value $ActualEntries) -cne
            (ConvertTo-SidecarCanonicalJson -Value $ExpectedEntries)) {
        throw "ASP54S2052 lane catalog/hash 不一致：$Label"
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

    if ($BaseLaneOrder.Count -ne 5) {
        throw 'ASP54S2004 Williams 顺序要求恰好五个 lane'
    }
    $BalancedRows = @(
        @(0, 1, 4, 2, 3),
        @(1, 2, 0, 3, 4),
        @(2, 3, 1, 4, 0),
        @(3, 4, 2, 0, 1),
        @(4, 0, 3, 1, 2)
    )
    $Row = (($ProcessRun + $WorkloadIndex + $SampleIndex) % 5 + 5) % 5
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
        'editor_executable_sha256',
        'editor_file_version',
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
        'aggregate_schema_sha256',
        'lane_catalog_sha256'
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
        [Parameter(Mandatory = $true)]$ExpectedProvenance,
        [Parameter(Mandatory = $true)]$ExpectedLaneCatalog,
        [Parameter(Mandatory = $true)][string]$ExpectedLaneCatalogSha256
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
    Test-SidecarLaneCatalog `
        -Actual $Result.lane_catalog `
        -ActualSha256 ([string]$Result.lane_catalog_sha256) `
        -Expected $ExpectedLaneCatalog `
        -ExpectedSha256 $ExpectedLaneCatalogSha256 `
        -Label 'calibration'

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
    Test-SidecarLaneCatalog `
        -Actual $Result.lane_catalog `
        -ActualSha256 ([string]$Result.lane_catalog_sha256) `
        -Expected $Manifest.lane_catalog `
        -ExpectedSha256 ([string]$Manifest.lane_catalog_sha256) `
        -Label "process=$ExpectedProcessRun"

    $ExpectedLanes = @($Profile.lanes | ForEach-Object { [string]$_ })
    $LaneCatalogById = @{}
    foreach ($Entry in @($Manifest.lane_catalog)) {
        $LaneCatalogById[[string]$Entry.lane_id] = $Entry
    }
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
        $LaneCatalogEntry = $LaneCatalogById[$Lane]
        if ($null -eq $LaneCatalogEntry -or
            [string]$Sample.lane_identity_sha256 -cne [string]$LaneCatalogEntry.lane_identity_sha256) {
            throw "ASP54S2053 raw result lane identity 不匹配：process=$ExpectedProcessRun lane=$Lane"
        }
        if ($Lane.StartsWith('avidscript_', [System.StringComparison]::Ordinal)) {
            if ($null -eq $Sample.backend_info -or
                [bool]$Sample.backend_info.fallback_used -or
                [string]$Sample.backend_info.backend_id -cne [string]$LaneCatalogEntry.backend_id -or
                [string]$Sample.backend_info.binding_invocation_mode -cne [string]$LaneCatalogEntry.binding_invocation_mode -or
                [string]$Sample.backend_info.runtime_version -cne [string]$LaneCatalogEntry.runtime_version -or
                [string]$Sample.backend_info.execution_mode -cne [string]$LaneCatalogEntry.execution_mode -or
                [string]$Sample.backend_info.artifact_format -cne [string]$LaneCatalogEntry.execution_artifact_format -or
                [string]$Sample.backend_info.artifact_sha256 -cne [string]$LaneCatalogEntry.execution_artifact_sha256 -or
                [string]$Sample.backend_info.source_wasm_sha256 -cne [string]$LaneCatalogEntry.source_wasm_sha256 -or
                [string]$Sample.backend_info.target_triple -cne [string]$LaneCatalogEntry.target_triple -or
                [string]$Sample.backend_info.runtime_build_identity -cne [string]$LaneCatalogEntry.runtime_build_identity -or
                [string]$Sample.backend_info.runtime_artifact_sha256 -cne [string]$LaneCatalogEntry.runtime_artifact_sha256) {
                throw "ASP54S2054 raw result AvidScript backend provenance 不匹配：process=$ExpectedProcessRun lane=$Lane"
            }
        }
        if ($Lane -ceq 'avidscript_wasmtime_native_direct' -and
            $Workload -cin @('scalar_add_int32', 'batch_scalar') -and
            ([int64]$Sample.direct_hit_count -ne [int64]$Sample.iterations -or
             [int64]$Sample.requested_direct_fallback_count -ne 0)) {
            throw "ASP54S2057 direct scalar workload 出现 fallback 或缺少 direct hit：process=$ExpectedProcessRun workload=$Workload"
        }
        if ($Lane -ceq 'avidscript_wasmtime_semantic' -and
            ([int64]$Sample.direct_hit_count -ne 0 -or
             [int64]$Sample.requested_direct_fallback_count -ne 0)) {
            throw "ASP54S2057 semantic lane 不得报告 direct 请求证据：process=$ExpectedProcessRun workload=$Workload"
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
