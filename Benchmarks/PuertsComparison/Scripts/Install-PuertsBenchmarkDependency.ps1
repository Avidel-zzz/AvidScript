[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('ValidateLock', 'Install', 'Verify', 'Remove')]
    [string]$Mode,

    [string]$ProjectRoot = '',

    [string]$CacheRoot = (Join-Path ([System.IO.Path]::GetTempPath()) 'AvidScriptPhase53'),

    [string]$LockPath = '',

    [string]$DownloadUriOverride = ''
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
if ([string]::IsNullOrWhiteSpace($LockPath)) {
    $LockPath = Join-Path $BenchmarkRoot 'Config/PuertsDependency.lock.json'
}
$SchemaPath = Join-Path $BenchmarkRoot 'Schema/PuertsDependencyLock.schema.json'

function Resolve-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path)
}

function Read-DependencyLock {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Schema
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "ASP53D1001 dependency lock is missing: $Path"
    }
    if (-not (Test-Path -LiteralPath $Schema -PathType Leaf)) {
        throw "ASP53D1002 dependency lock schema is missing: $Schema"
    }

    $Raw = Get-Content -LiteralPath $Path -Raw
    if (-not ($Raw | Test-Json -SchemaFile $Schema -ErrorAction SilentlyContinue)) {
        throw 'ASP53D1003 dependency lock does not satisfy its schema'
    }

    $Lock = $Raw | ConvertFrom-Json
    if ($Lock.source.repository_url -cne 'https://github.com/Tencent/puerts.git') {
        throw 'ASP53D1004 only the official Tencent Puerts repository is allowed'
    }
    return $Lock
}

function Invoke-GitChecked {
    param(
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $Output = & git -C $WorkingDirectory @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "ASP53D1100 git failed: git $($Arguments -join ' ')`n$($Output -join [Environment]::NewLine)"
    }
    return @($Output)
}

function Assert-ProjectRoot {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'ASP53D1200 ProjectRoot is required for this mode'
    }
    $Resolved = Resolve-FullPath $Path
    if (-not (Test-Path -LiteralPath $Resolved -PathType Container)) {
        throw "ASP53D1201 project root does not exist: $Resolved"
    }
    Assert-ProjectRootAncestorsAreOrdinary $Resolved
    $Resolved = Get-CanonicalDirectoryPath $Resolved
    $Projects = @(Get-ChildItem -LiteralPath $Resolved -Filter '*.uproject' -File)
    if ($Projects.Count -ne 1) {
        throw "ASP53D1202 project root must contain exactly one .uproject: $Resolved"
    }
    return $Resolved
}

function Test-IsReparsePoint {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return $false
    }
    return (((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Assert-ProjectRootAncestorsAreOrdinary {
    param([Parameter(Mandatory = $true)][string]$CanonicalProjectRoot)

    $Ancestor = [IO.Directory]::GetParent((Resolve-FullPath $CanonicalProjectRoot))
    while ($null -ne $Ancestor) {
        if (Test-IsReparsePoint $Ancestor.FullName) {
            throw 'ASP53D1206 refusing to manage a ProjectRoot with a reparse-point ancestor'
        }
        $Ancestor = $Ancestor.Parent
    }
}

function Get-CanonicalDirectoryPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Item = Get-Item -LiteralPath $Path -Force
    if (-not $Item.PSIsContainer) {
        throw "ASP53D1901 expected a directory: $Path"
    }
    if (Test-IsReparsePoint $Path) {
        $Target = $Item.ResolveLinkTarget($true)
        if ($null -eq $Target) {
            throw "ASP53D1901 unable to resolve reparse-point directory: $Path"
        }
        return Resolve-FullPath $Target.FullName
    }
    return Resolve-FullPath $Item.FullName
}

function Get-InstallPaths {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)]$Lock
    )

    $PluginsRoot = Resolve-FullPath (Join-Path $ResolvedProjectRoot 'Plugins')
    $InstallPath = Resolve-FullPath (Join-Path $ResolvedProjectRoot $Lock.installation.project_plugin_path)
    $ExpectedInstallPath = Resolve-FullPath (Join-Path $PluginsRoot 'Puerts')
    if ($InstallPath -cne $ExpectedInstallPath) {
        throw 'ASP53D1203 dependency lock attempted to escape the expected Plugins/Puerts location'
    }
    if ((Test-Path -LiteralPath $PluginsRoot) -and (Test-IsReparsePoint $PluginsRoot)) {
        throw 'ASP53D1204 refusing to manage a reparse-point Plugins directory'
    }
    if ((Test-Path -LiteralPath $InstallPath) -and (Test-IsReparsePoint $InstallPath)) {
        throw 'ASP53D1205 refusing to manage a reparse-point Puerts directory'
    }
    return [pscustomobject]@{
        PluginsRoot = $PluginsRoot
        InstallPath = $InstallPath
        MarkerPath = Join-Path $InstallPath $Lock.installation.managed_marker_name
    }
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-DependencyLockSha256 {
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

function Get-InstalledContentSummary {
    param(
        [Parameter(Mandatory = $true)][string]$InstallPath,
        [Parameter(Mandatory = $true)][string]$MarkerName
    )

    $ExcludedDirectoryNames = @('Binaries', 'Intermediate', 'Saved', 'DerivedDataCache', '.git')
    $ExcludedRelativePrefixes = @(
        'Source/CSharpParamDefaultValueMetas/bin/',
        'Source/CSharpParamDefaultValueMetas/obj/')
    $ManifestLines = [System.Collections.Generic.List[string]]::new()
    foreach ($File in Get-ChildItem -LiteralPath $InstallPath -File -Recurse -Force) {
        $RelativePath = [System.IO.Path]::GetRelativePath($InstallPath, $File.FullName).Replace('\', '/')
        if ($RelativePath.StartsWith('./', [StringComparison]::Ordinal)) {
            $RelativePath = $RelativePath.Substring(2)
        }
        if ($RelativePath -ceq $MarkerName) {
            continue
        }
        $RootDirectoryName = $RelativePath.Split('/')[0]
        if ($RootDirectoryName -in $ExcludedDirectoryNames) {
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
        $ManifestLines.Add(("{0}`t{1}" -f $RelativePath, (Get-FileSha256 $File.FullName)))
    }

    $SortedManifestLines = @($ManifestLines)
    [Array]::Sort($SortedManifestLines, [StringComparer]::Ordinal)
    $Manifest = [string]::Join("`n", $SortedManifestLines) + "`n"
    $ManifestBytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Manifest)
    $Digest = [System.Security.Cryptography.SHA256]::HashData($ManifestBytes)
    return [pscustomobject]@{
        sha256 = ([System.Convert]::ToHexString($Digest)).ToLowerInvariant()
        file_count = [int64]$SortedManifestLines.Count
    }
}

function Assert-BackendArchive {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Lock
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "ASP53D1300 Puerts backend archive is missing: $Path"
    }
    $Item = Get-Item -LiteralPath $Path
    if ([int64]$Item.Length -ne [int64]$Lock.backend.size_bytes) {
        throw "ASP53D1301 Puerts backend archive size mismatch: actual=$($Item.Length) expected=$($Lock.backend.size_bytes)"
    }
    $ActualSha = Get-FileSha256 $Path
    if ($ActualSha -cne [string]$Lock.backend.sha256) {
        throw "ASP53D1302 Puerts backend archive SHA-256 mismatch: actual=$ActualSha"
    }
}

function Get-BackendArchive {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)]$Lock,
        [string]$DownloadUriOverride = ''
    )

    $ArchivePath = Join-Path $Root $Lock.backend.asset_name
    if (Test-Path -LiteralPath $ArchivePath -PathType Leaf) {
        Assert-BackendArchive $ArchivePath $Lock
        return $ArchivePath
    }

    New-Item -ItemType Directory -Force -Path $Root | Out-Null
    $TemporaryPath = Join-Path $Root ("$($Lock.backend.asset_name).partial-$([Guid]::NewGuid().ToString('N')).tmp")
    $DownloadUri = if ([string]::IsNullOrWhiteSpace($DownloadUriOverride)) { [string]$Lock.backend.asset_url } else { $DownloadUriOverride }
    try {
        try {
            Invoke-WebRequest -Uri $DownloadUri -OutFile $TemporaryPath -UseBasicParsing -ErrorAction Stop
        }
        catch {
            throw "ASP53D1303 unable to download Puerts backend archive: $($_.Exception.Message)"
        }
        Assert-BackendArchive $TemporaryPath $Lock
        try {
            [System.IO.File]::Move($TemporaryPath, $ArchivePath)
        }
        catch [System.IO.IOException] {
            if (Test-Path -LiteralPath $ArchivePath -PathType Leaf) {
                Assert-BackendArchive $ArchivePath $Lock
                return $ArchivePath
            }
            throw "ASP53D1304 unable to atomically publish Puerts backend archive: $($_.Exception.Message)"
        }
        return $ArchivePath
    }
    finally {
        if (Test-Path -LiteralPath $TemporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
    }
}

function Initialize-SourceCache {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)]$Lock
    )

    $SourceRoot = Join-Path $Root 'puerts-upstream.git'
    if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot 'HEAD') -PathType Leaf)) {
        New-Item -ItemType Directory -Force -Path $Root | Out-Null
        $CloneOutput = & git clone --filter=blob:none --bare $Lock.source.repository_url $SourceRoot 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "ASP53D1400 unable to clone official Puerts source`n$($CloneOutput -join [Environment]::NewLine)"
        }
    }

    $RemoteOutput = @(Invoke-GitChecked $SourceRoot @('remote', 'get-url', 'origin'))
    $Remote = ([string]$RemoteOutput[-1]).Trim()
    if ($Remote -cne [string]$Lock.source.repository_url) {
        throw "ASP53D1401 Puerts source remote mismatch: $Remote"
    }

    $CommitExpression = "$($Lock.source.commit_sha)^{commit}"
    & git -C $SourceRoot cat-file -e $CommitExpression 2>$null
    if ($LASTEXITCODE -ne 0) {
        Invoke-GitChecked $SourceRoot @(
            'fetch',
            '--depth=1',
            'origin',
            [string]$Lock.source.commit_sha) | Out-Null
    }
    $CommitOutput = @(Invoke-GitChecked $SourceRoot @('rev-parse', [string]$Lock.source.commit_sha))
    $Commit = ([string]$CommitOutput[-1]).Trim()
    if ($Commit -cne [string]$Lock.source.commit_sha) {
        throw "ASP53D1402 Puerts source commit mismatch: $Commit"
    }
    $TreeExpression = "$($Lock.source.commit_sha):$($Lock.source.plugin_subdirectory)"
    $TreeOutput = @(Invoke-GitChecked $SourceRoot @('rev-parse', $TreeExpression))
    $Tree = ([string]$TreeOutput[-1]).Trim()
    if ($Tree -cne [string]$Lock.source.plugin_tree_sha1) {
        throw "ASP53D1403 Puerts plugin tree mismatch: $Tree"
    }
    return $SourceRoot
}

function Get-BackendPluginRoot {
    param([Parameter(Mandatory = $true)][string]$ExtractionRoot)

    $Candidates = @(
        (Join-Path $ExtractionRoot 'unreal/Puerts'),
        (Join-Path $ExtractionRoot 'Puerts'),
        $ExtractionRoot
    )
    foreach ($Candidate in $Candidates) {
        if (Test-Path -LiteralPath (Join-Path $Candidate 'ThirdParty') -PathType Container) {
            return $Candidate
        }
    }
    throw 'ASP53D1500 backend archive does not contain a recognized Puerts/ThirdParty layout'
}

function Write-ManagedMarker {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$LockFile,
        [Parameter(Mandatory = $true)]$Lock
    )

    $InstallPath = Split-Path -Parent $Path
    $InstalledContent = Get-InstalledContentSummary $InstallPath (Split-Path -Leaf $Path)
    $Marker = [ordered]@{
        schema_version = 2
        managed_by = 'AvidScriptPhase53'
        lock_sha256 = Get-DependencyLockSha256 $LockFile
        source_repository_url = [string]$Lock.source.repository_url
        source_commit_sha = [string]$Lock.source.commit_sha
        source_plugin_tree_sha1 = [string]$Lock.source.plugin_tree_sha1
        backend_asset_name = [string]$Lock.backend.asset_name
        backend_sha256 = [string]$Lock.backend.sha256
        installed_content_sha256 = $InstalledContent.sha256
        installed_file_count = $InstalledContent.file_count
        installed_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
    }
    $Json = (($Marker | ConvertTo-Json -Depth 8) -replace "`r`n", "`n") + "`n"
    [System.IO.File]::WriteAllText($Path, $Json, [System.Text.UTF8Encoding]::new($false))
}

function Read-AndAssertManagedMarker {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$LockFile,
        [Parameter(Mandatory = $true)]$Lock,
        [switch]$AllowLegacySchema1
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw 'ASP53D1600 refusing to manage Puerts directory without the AvidScript marker'
    }
    try {
        $Marker = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    }
    catch {
        throw 'ASP53D1601 managed Puerts marker does not match the tracked dependency lock'
    }
    $ExpectedLockSha = Get-DependencyLockSha256 $LockFile
    $MarkerSchemaVersion = 0
    $HasValidSchemaVersion = [int]::TryParse(
        [string]$Marker.schema_version,
        [Globalization.NumberStyles]::None,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$MarkerSchemaVersion)
    $MatchesLock =
        $Marker.managed_by -ceq 'AvidScriptPhase53' -and
        $Marker.lock_sha256 -ceq $ExpectedLockSha -and
        $Marker.source_repository_url -ceq [string]$Lock.source.repository_url -and
        $Marker.source_commit_sha -ceq [string]$Lock.source.commit_sha -and
        $Marker.source_plugin_tree_sha1 -ceq [string]$Lock.source.plugin_tree_sha1 -and
        $Marker.backend_asset_name -ceq [string]$Lock.backend.asset_name -and
        $Marker.backend_sha256 -ceq [string]$Lock.backend.sha256
    if (-not $HasValidSchemaVersion -or -not $MatchesLock) {
        throw 'ASP53D1601 managed Puerts marker does not match the tracked dependency lock'
    }
    if ($MarkerSchemaVersion -eq 1) {
        if ($AllowLegacySchema1) {
            return $Marker
        }
        throw 'ASP53D1601 managed Puerts marker does not match the tracked dependency lock'
    }
    $MarkerFileCount = [int64]0
    $HasValidFileCount = [int64]::TryParse(
        [string]$Marker.installed_file_count,
        [Globalization.NumberStyles]::None,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$MarkerFileCount)
    if ($MarkerSchemaVersion -ne 2 -or
        $Marker.managed_by -cne 'AvidScriptPhase53' -or
        $Marker.lock_sha256 -cne $ExpectedLockSha -or
        $Marker.source_repository_url -cne [string]$Lock.source.repository_url -or
        $Marker.source_commit_sha -cne [string]$Lock.source.commit_sha -or
        $Marker.source_plugin_tree_sha1 -cne [string]$Lock.source.plugin_tree_sha1 -or
        $Marker.backend_asset_name -cne [string]$Lock.backend.asset_name -or
        $Marker.backend_sha256 -cne [string]$Lock.backend.sha256 -or
        ([string]$Marker.installed_content_sha256) -cnotmatch '^[0-9a-f]{64}$' -or
        ([string]$Marker.installed_file_count) -cnotmatch '^(0|[1-9][0-9]*)$' -or
        -not $HasValidFileCount) {
        throw 'ASP53D1601 managed Puerts marker does not match the tracked dependency lock'
    }
    return $Marker
}

function Assert-InstallPathIsContained {
    param(
        [Parameter(Mandatory = $true)]$Paths,
        [Parameter(Mandatory = $true)][string]$FailureCode
    )

    $CanonicalPluginsRoot = Get-CanonicalDirectoryPath $Paths.PluginsRoot
    $ExpectedInstallPath = Resolve-FullPath (Join-Path $CanonicalPluginsRoot 'Puerts')
    if ((Resolve-FullPath $Paths.InstallPath) -cne $ExpectedInstallPath) {
        throw "$FailureCode Puerts path is outside the canonical project Plugins root"
    }
}

function Assert-InstallDestinationBeforeWrite {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)]$Lock
    )

    # Directory-swap TOCTOU requires handles or ACLs and is outside the local trusted-build model.
    $RecheckedProjectRoot = Assert-ProjectRoot $ResolvedProjectRoot
    $RecheckedPaths = Get-InstallPaths $RecheckedProjectRoot $Lock
    if (-not (Test-Path -LiteralPath $RecheckedPaths.PluginsRoot -PathType Container)) {
        throw 'ASP53D1705 project Plugins directory disappeared before Puerts install'
    }
    Assert-InstallPathIsContained $RecheckedPaths 'ASP53D1705'
    if (Test-Path -LiteralPath $RecheckedPaths.InstallPath) {
        throw "ASP53D1700 install target already exists; verify or remove it explicitly: $($RecheckedPaths.InstallPath)"
    }
    return $RecheckedPaths
}

function Assert-RemoveTargetIsOrdinaryAndContained {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)]$Lock
    )

    # Directory-swap TOCTOU requires handles or ACLs and is outside the local trusted-build model.
    $RecheckedProjectRoot = Assert-ProjectRoot $ResolvedProjectRoot
    $RecheckedPaths = Get-InstallPaths $RecheckedProjectRoot $Lock
    if (-not (Test-Path -LiteralPath $RecheckedPaths.PluginsRoot -PathType Container) -or
        (Test-IsReparsePoint $RecheckedPaths.PluginsRoot)) {
        throw 'ASP53D1900 refusing to remove through a reparse-point Plugins directory'
    }
    if (-not (Test-Path -LiteralPath $RecheckedPaths.InstallPath -PathType Container) -or
        (Test-IsReparsePoint $RecheckedPaths.InstallPath)) {
        throw 'ASP53D1900 refusing to remove a reparse-point Puerts directory'
    }
    Assert-InstallPathIsContained $RecheckedPaths 'ASP53D1900'
    return $RecheckedPaths
}

function Install-Dependency {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedCacheRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedLockPath,
        [Parameter(Mandatory = $true)]$Lock,
        [string]$DownloadUriOverride = ''
    )

    $Paths = Get-InstallPaths $ResolvedProjectRoot $Lock
    if (Test-Path -LiteralPath $Paths.InstallPath) {
        throw "ASP53D1700 install target already exists; verify or remove it explicitly: $($Paths.InstallPath)"
    }

    $SourceRoot = Initialize-SourceCache $ResolvedCacheRoot $Lock
    $ArchivePath = Get-BackendArchive $ResolvedCacheRoot $Lock $DownloadUriOverride

    New-Item -ItemType Directory -Force -Path $Paths.PluginsRoot | Out-Null
    $Paths = Get-InstallPaths $ResolvedProjectRoot $Lock
    $StageRoot = Join-Path $Paths.PluginsRoot ('.AvidScriptPuertsStage-' + [Guid]::NewGuid().ToString('N'))
    $SourceExtraction = Join-Path $StageRoot 'source'
    $BackendExtraction = Join-Path $StageRoot 'backend'
    $StagedPlugin = Join-Path $StageRoot 'Puerts'

    try {
        New-Item -ItemType Directory -Force -Path $SourceExtraction | Out-Null
        New-Item -ItemType Directory -Force -Path $BackendExtraction | Out-Null
        $SourceArchive = Join-Path $StageRoot 'puerts-source.tar'
        Invoke-GitChecked $SourceRoot @(
            'archive',
            '--format=tar',
            "--output=$SourceArchive",
            [string]$Lock.source.commit_sha,
            [string]$Lock.source.plugin_subdirectory) | Out-Null
        & tar -xf $SourceArchive -C $SourceExtraction
        if ($LASTEXITCODE -ne 0) {
            throw 'ASP53D1701 unable to extract Puerts source archive'
        }
        $ExtractedPlugin = Join-Path $SourceExtraction $Lock.source.plugin_subdirectory
        if (-not (Test-Path -LiteralPath (Join-Path $ExtractedPlugin 'Puerts.uplugin') -PathType Leaf)) {
            throw 'ASP53D1702 extracted Puerts source is missing Puerts.uplugin'
        }
        Move-Item -LiteralPath $ExtractedPlugin -Destination $StagedPlugin

        & tar -xzf $ArchivePath -C $BackendExtraction
        if ($LASTEXITCODE -ne 0) {
            throw 'ASP53D1703 unable to extract Puerts backend archive'
        }
        $BackendPluginRoot = Get-BackendPluginRoot $BackendExtraction
        Copy-Item -LiteralPath (Join-Path $BackendPluginRoot 'ThirdParty') -Destination $StagedPlugin -Recurse -Force

        $ExpectedV8Root = Join-Path $StagedPlugin 'ThirdParty/v8_9.4.146.24'
        if (-not (Test-Path -LiteralPath $ExpectedV8Root -PathType Container)) {
            throw 'ASP53D1704 extracted backend does not provide V8 9.4.146.24'
        }
        Write-ManagedMarker (Join-Path $StagedPlugin $Lock.installation.managed_marker_name) $ResolvedLockPath $Lock
        $Paths = Assert-InstallDestinationBeforeWrite $ResolvedProjectRoot $Lock
        Move-Item -LiteralPath $StagedPlugin -Destination $Paths.InstallPath
    }
    finally {
        if (Test-Path -LiteralPath $StageRoot) {
            Remove-Item -LiteralPath $StageRoot -Recurse -Force
        }
    }

    return Verify-Dependency $ResolvedProjectRoot $ResolvedCacheRoot $ResolvedLockPath $Lock
}

function Verify-Dependency {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedCacheRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedLockPath,
        [Parameter(Mandatory = $true)]$Lock
    )

    $Paths = Get-InstallPaths $ResolvedProjectRoot $Lock
    if (-not (Test-Path -LiteralPath $Paths.InstallPath -PathType Container)) {
        throw "ASP53D1800 Puerts is not installed at $($Paths.InstallPath)"
    }
    $Marker = Read-AndAssertManagedMarker $Paths.MarkerPath $ResolvedLockPath $Lock
    Initialize-SourceCache $ResolvedCacheRoot $Lock | Out-Null

    $ArchivePath = Join-Path $ResolvedCacheRoot $Lock.backend.asset_name
    Assert-BackendArchive $ArchivePath $Lock
    $RequiredFiles = @(
        'Puerts.uplugin',
        'Source/JsEnv/JsEnv.Build.cs',
        'ThirdParty/v8_9.4.146.24/Inc/v8.h'
    )
    foreach ($RelativePath in $RequiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $Paths.InstallPath $RelativePath) -PathType Leaf)) {
            throw "ASP53D1801 installed Puerts dependency is incomplete: $RelativePath"
        }
    }
    $InstalledContent = Get-InstalledContentSummary $Paths.InstallPath (Split-Path -Leaf $Paths.MarkerPath)
    if ($Marker.installed_content_sha256 -cne $InstalledContent.sha256 -or
        [int64]$Marker.installed_file_count -ne $InstalledContent.file_count) {
        throw 'ASP53D1802 installed Puerts content does not match its managed marker'
    }

    return [pscustomobject][ordered]@{
        succeeded = $true
        mode = 'Verify'
        source_commit_sha = [string]$Lock.source.commit_sha
        source_plugin_tree_sha1 = [string]$Lock.source.plugin_tree_sha1
        backend_sha256 = [string]$Lock.backend.sha256
        runtime = [string]$Lock.backend.runtime
        runtime_version = [string]$Lock.backend.runtime_version
        install_path = $Paths.InstallPath
        installed_content_sha256 = $InstalledContent.sha256
        installed_file_count = $InstalledContent.file_count
    }
}

function Remove-Dependency {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedLockPath,
        [Parameter(Mandatory = $true)]$Lock
    )

    $Paths = Get-InstallPaths $ResolvedProjectRoot $Lock
    if (-not (Test-Path -LiteralPath $Paths.InstallPath)) {
        return [pscustomobject]@{ succeeded = $true; mode = 'Remove'; removed = $false }
    }
    Read-AndAssertManagedMarker $Paths.MarkerPath $ResolvedLockPath $Lock -AllowLegacySchema1 | Out-Null
    $Paths = Assert-RemoveTargetIsOrdinaryAndContained $ResolvedProjectRoot $Lock
    Remove-Item -LiteralPath $Paths.InstallPath -Recurse -Force
    return [pscustomobject]@{ succeeded = $true; mode = 'Remove'; removed = $true }
}

$ResolvedLockPath = Resolve-FullPath $LockPath
$ResolvedSchemaPath = Resolve-FullPath $SchemaPath
$DependencyLock = Read-DependencyLock $ResolvedLockPath $ResolvedSchemaPath

if ($Mode -ceq 'ValidateLock') {
    [pscustomobject][ordered]@{
        succeeded = $true
        mode = 'ValidateLock'
        source_commit_sha = [string]$DependencyLock.source.commit_sha
        source_plugin_tree_sha1 = [string]$DependencyLock.source.plugin_tree_sha1
        backend_sha256 = [string]$DependencyLock.backend.sha256
    } | ConvertTo-Json -Depth 8
    exit 0
}

$ResolvedProjectRoot = Assert-ProjectRoot $ProjectRoot
$ResolvedCacheRoot = Resolve-FullPath $CacheRoot
$Result = switch ($Mode) {
    'Install' { Install-Dependency $ResolvedProjectRoot $ResolvedCacheRoot $ResolvedLockPath $DependencyLock $DownloadUriOverride }
    'Verify' { Verify-Dependency $ResolvedProjectRoot $ResolvedCacheRoot $ResolvedLockPath $DependencyLock }
    'Remove' { Remove-Dependency $ResolvedProjectRoot $ResolvedLockPath $DependencyLock }
}
$Result | ConvertTo-Json -Depth 8
