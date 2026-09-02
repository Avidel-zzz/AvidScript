[CmdletBinding()]
param(
    [ValidateSet('ValidateLock', 'Install', 'Verify', 'Remove')]
    [string]$Mode,
    [ValidateSet('Win64', 'AndroidArm64')]
    [string]$Platform = 'Win64',
    [string]$RepositoryRoot = '',
    [string]$CacheRoot = '',
    [string]$LockPath = '',
    [string]$DownloadUriOverride = ''
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent $ScriptRoot
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
if ([string]::IsNullOrWhiteSpace($LockPath)) {
    $LockFile = if ($Platform -ceq 'AndroidArm64') {
        'WasmtimeAndroidDependency.lock.json'
    }
    else {
        'WasmtimeDependency.lock.json'
    }
    $LockPath = Join-Path $RepositoryRoot "Source/ThirdParty/Wasmtime/$LockFile"
}
$SchemaFile = if ($Platform -ceq 'AndroidArm64') {
    'WasmtimeAndroidDependency.schema.json'
}
else {
    'WasmtimeDependency.schema.json'
}
$SchemaPath = Join-Path $RepositoryRoot "Source/ThirdParty/Wasmtime/$SchemaFile"

function Get-Sha256Bytes {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    $Digest = [System.Security.Cryptography.SHA256]::HashData($Bytes)
    return [Convert]::ToHexString($Digest).ToLowerInvariant()
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-CanonicalFileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Text = [System.IO.File]::ReadAllText($Path)
    if ($Text.Length -gt 0 -and $Text[0] -eq [char]0xFEFF) {
        $Text = $Text.Substring(1)
    }
    $CanonicalText = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    return Get-Sha256Bytes ([System.Text.UTF8Encoding]::new($false).GetBytes($CanonicalText))
}

function Assert-PinnedLockIdentity {
    param([Parameter(Mandatory = $true)]$Lock)

    if ([string]$Lock.platform -ceq 'Android') {
        $ExpectedAndroid = [ordered]@{
            schema_version = 1
            dependency = 'Wasmtime Cranelift C API'
            version = 'v45.0.0'
            platform = 'Android'
            architecture = 'arm64'
            release_url = 'https://github.com/bytecodealliance/wasmtime/releases/tag/v45.0.0'
            archive_name = 'wasmtime-v45.0.0-aarch64-android-c-api.tar.xz'
            archive_url = 'https://github.com/bytecodealliance/wasmtime/releases/download/v45.0.0/wasmtime-v45.0.0-aarch64-android-c-api.tar.xz'
            archive_size = [int64]13413296
            archive_sha256 = 'af5160bb3975686aec2f2ae1494bcd7ed4adcd3d4922af23e16bea3b687bfc1b'
            archive_root = 'wasmtime-v45.0.0-aarch64-android-c-api'
            include_path = 'include'
            static_path = 'lib/libwasmtime.a'
            static_sha256 = 'd53dd5a687cc555b1a8b77c4639bfc355812000b7987c4a0f559982e61e75b28'
            license_path = 'LICENSE'
            install_path = 'Source/ThirdParty/Wasmtime/installed/Android/arm64/v45.0.0'
            marker_name = '.avidscript-wasmtime-managed.json'
            license_expression = 'Apache-2.0 WITH LLVM-exception'
            tracked_license = 'Source/ThirdParty/Wasmtime/LICENSE.txt'
        }
        $ActualAndroid = [ordered]@{
            schema_version = [int]$Lock.schema_version
            dependency = [string]$Lock.dependency
            version = [string]$Lock.version
            platform = [string]$Lock.platform
            architecture = [string]$Lock.architecture
            release_url = [string]$Lock.release_url
            archive_name = [string]$Lock.archive.name
            archive_url = [string]$Lock.archive.url
            archive_size = [int64]$Lock.archive.size_bytes
            archive_sha256 = [string]$Lock.archive.sha256
            archive_root = [string]$Lock.archive.root
            include_path = [string]$Lock.layout.include_relative_path
            static_path = [string]$Lock.layout.static_library_relative_path
            static_sha256 = [string]$Lock.layout.static_library_sha256
            license_path = [string]$Lock.layout.license_relative_path
            install_path = [string]$Lock.install.relative_path
            marker_name = [string]$Lock.install.managed_marker_name
            license_expression = [string]$Lock.license.expression
            tracked_license = [string]$Lock.license.tracked_file
        }
        foreach ($Name in $ExpectedAndroid.Keys) {
            if ($ActualAndroid[$Name] -cne $ExpectedAndroid[$Name]) {
                throw "ASP54W1004 pinned Android lock identity mismatch: $Name"
            }
        }
        return
    }

    $Expected = [ordered]@{
        schema_version = 1
        dependency = 'Wasmtime Cranelift C API'
        version = 'v45.0.0'
        platform = 'Win64'
        release_url = 'https://github.com/bytecodealliance/wasmtime/releases/tag/v45.0.0'
        archive_name = 'wasmtime-v45.0.0-x86_64-windows-c-api.zip'
        archive_url = 'https://github.com/bytecodealliance/wasmtime/releases/download/v45.0.0/wasmtime-v45.0.0-x86_64-windows-c-api.zip'
        archive_size = [int64]28820070
        archive_sha256 = 'd5ee516fc141576ccd6c43146aafee1074c3c26764cba73b3a97f599a3791f9c'
        archive_root = 'wasmtime-v45.0.0-x86_64-windows-c-api'
        include_path = 'include'
        dll_path = 'lib/wasmtime.dll'
        import_path = 'lib/wasmtime.dll.lib'
        license_path = 'LICENSE'
        install_path = 'Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0'
        marker_name = '.avidscript-wasmtime-managed.json'
        license_expression = 'Apache-2.0 WITH LLVM-exception'
        tracked_license = 'Source/ThirdParty/Wasmtime/LICENSE.txt'
    }
    $Actual = [ordered]@{
        schema_version = [int]$Lock.schema_version
        dependency = [string]$Lock.dependency
        version = [string]$Lock.version
        platform = [string]$Lock.platform
        release_url = [string]$Lock.release_url
        archive_name = [string]$Lock.archive.name
        archive_url = [string]$Lock.archive.url
        archive_size = [int64]$Lock.archive.size_bytes
        archive_sha256 = [string]$Lock.archive.sha256
        archive_root = [string]$Lock.archive.root
        include_path = [string]$Lock.layout.include_relative_path
        dll_path = [string]$Lock.layout.dll_relative_path
        import_path = [string]$Lock.layout.import_library_relative_path
        license_path = [string]$Lock.layout.license_relative_path
        install_path = [string]$Lock.install.relative_path
        marker_name = [string]$Lock.install.managed_marker_name
        license_expression = [string]$Lock.license.expression
        tracked_license = [string]$Lock.license.tracked_file
    }
    foreach ($Name in $Expected.Keys) {
        if ($Actual[$Name] -cne $Expected[$Name]) {
            throw "ASP54W1004 pinned lock identity mismatch: $Name"
        }
    }
}

function Read-WasmtimeDependencyLock {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Schema
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "ASP54W1001 dependency lock is missing: $Path"
    }
    if (-not (Test-Path -LiteralPath $Schema -PathType Leaf)) {
        throw "ASP54W1002 dependency schema is missing: $Schema"
    }
    $Raw = Get-Content -LiteralPath $Path -Raw
    if (-not ($Raw | Test-Json -SchemaFile $Schema -ErrorAction SilentlyContinue)) {
        throw 'ASP54W1003 dependency lock does not satisfy its schema'
    }
    $Lock = $Raw | ConvertFrom-Json
    Assert-PinnedLockIdentity -Lock $Lock
    return $Lock
}

function Test-IsReparsePoint {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return $false
    }
    $Item = Get-Item -LiteralPath $Path -Force
    return (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Assert-NoReparseTree {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Code
    )

    if (Test-IsReparsePoint $Root) {
        throw "$Code refusing a reparse-point managed root"
    }
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return
    }
    foreach ($Item in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Code refusing a reparse point in managed content"
        }
    }
}

function Assert-NoAlternateDataStreams {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Code
    )

    $Items = @((Get-Item -LiteralPath $Root -Force))
    $Items += @(Get-ChildItem -LiteralPath $Root -Recurse -Force)
    foreach ($Item in $Items) {
        try {
            $Streams = @(Get-Item -LiteralPath $Item.FullName -Stream * -ErrorAction Stop)
        }
        catch {
            throw "$Code unable to inspect NTFS streams in managed content"
        }
        foreach ($Stream in $Streams) {
            if ([string]$Stream.Stream -cne ':$DATA') {
                throw "$Code managed content contains a non-default NTFS stream"
            }
        }
    }
}

function Get-TrustedRepositoryRoot {
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

    $Root = [System.IO.Path]::GetFullPath($RepositoryRoot)
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "ASP54W1101 repository root does not exist: $Root"
    }
    $RootItem = Get-Item -LiteralPath $Root -Force
    if (($RootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        $ResolvedTarget = $RootItem.ResolveLinkTarget($true)
        if ($null -eq $ResolvedTarget -or -not $ResolvedTarget.PSIsContainer) {
            throw 'ASP54W1101 trusted repository root could not be resolved'
        }
        return [System.IO.Path]::GetFullPath($ResolvedTarget.FullName)
    }
    return [System.IO.Path]::GetFullPath($RootItem.FullName)
}

function Assert-ContainedOrdinaryDirectoryPath {
    param(
        [Parameter(Mandatory = $true)][string]$TrustedRoot,
        [Parameter(Mandatory = $true)][string]$TargetPath
    )

    $Root = [System.IO.Path]::GetFullPath($TrustedRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $Target = [System.IO.Path]::GetFullPath($TargetPath)
    $RootPrefix = $Root + [System.IO.Path]::DirectorySeparatorChar
    if ($Target -cne $Root -and
        -not $Target.StartsWith(
            $RootPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'ASP54W1102 managed path escaped the trusted repository root'
    }

    $RelativePath = [System.IO.Path]::GetRelativePath($Root, $Target)
    $SeparatorCharacters = [char[]]@(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $Segments = @($RelativePath.Split(
        $SeparatorCharacters,
        [System.StringSplitOptions]::RemoveEmptyEntries))
    $CurrentPath = $Root
    foreach ($Segment in $Segments) {
        if ($Segment -ceq '..' -or $Segment -ceq '.') {
            throw 'ASP54W1102 managed path escaped the trusted repository root'
        }
        $CurrentPath = Join-Path $CurrentPath $Segment
        if (-not (Test-Path -LiteralPath $CurrentPath)) {
            continue
        }
        $Item = Get-Item -LiteralPath $CurrentPath -Force
        if (-not $Item.PSIsContainer) {
            throw 'ASP54W1104 managed path ancestor is not a directory'
        }
        if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'ASP54W1104 refusing a reparse-point managed path ancestor'
        }
        $PhysicalPath = [System.IO.Path]::GetFullPath($Item.FullName)
        if ($PhysicalPath -cne $Root -and
            -not $PhysicalPath.StartsWith(
                $RootPrefix,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'ASP54W1104 managed path ancestor resolved outside the trusted repository root'
        }
    }
}

function Get-WasmtimeInstallPaths {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]$Lock
    )

    $Root = Get-TrustedRepositoryRoot -RepositoryRoot $RepositoryRoot
    $InstalledRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $Root 'Source/ThirdParty/Wasmtime/installed'))
    $InstallPath = [System.IO.Path]::GetFullPath((Join-Path $Root $Lock.install.relative_path))
    $ExpectedRelativePath = if ([string]$Lock.platform -ceq 'Android') {
        'Android/arm64/v45.0.0'
    }
    elseif ([string]$Lock.platform -ceq 'Win64') {
        'Win64/v45.0.0'
    }
    else {
        throw 'ASP54W1102 dependency lock has an unsupported platform boundary'
    }
    $ExpectedPath = [System.IO.Path]::GetFullPath(
        (Join-Path $InstalledRoot $ExpectedRelativePath))
    if ($InstallPath -cne $ExpectedPath) {
        throw 'ASP54W1102 dependency lock attempted to escape the managed install path'
    }
    $InstalledPrefix = $InstalledRoot.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $InstallPath.StartsWith(
        $InstalledPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'ASP54W1102 dependency lock attempted to escape the installed boundary'
    }
    Assert-ContainedOrdinaryDirectoryPath -TrustedRoot $Root -TargetPath $InstallPath
    $PlatformRoot = if ([string]$Lock.platform -ceq 'Android') {
        Join-Path $InstalledRoot 'Android/arm64'
    }
    else {
        Join-Path $InstalledRoot 'Win64'
    }
    $PlatformFamilyRoot = if ([string]$Lock.platform -ceq 'Android') {
        Join-Path $InstalledRoot 'Android'
    }
    else {
        $PlatformRoot
    }
    return [pscustomobject]@{
        RepositoryRoot = $Root
        InstalledRoot = $InstalledRoot
        PlatformRoot = $PlatformRoot
        PlatformFamilyRoot = $PlatformFamilyRoot
        InstallPath = $InstallPath
        MarkerPath = Join-Path $InstallPath $Lock.install.managed_marker_name
    }
}

function Get-WasmtimeArchiveCachePath {
    param(
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)]$Lock
    )

    $VersionRoot = Join-Path ([System.IO.Path]::GetFullPath($CacheRoot)) $Lock.version
    return Join-Path $VersionRoot $Lock.archive.name
}

function Assert-WasmtimeArchive {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Lock
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "ASP54W1300 dependency archive is missing: $Path"
    }
    $Length = [int64](Get-Item -LiteralPath $Path).Length
    if ($Length -ne [int64]$Lock.archive.size_bytes) {
        throw "ASP54W1301 archive size mismatch: actual=$Length expected=$($Lock.archive.size_bytes)"
    }
    $ActualSha256 = Get-FileSha256 -Path $Path
    if ($ActualSha256 -cne [string]$Lock.archive.sha256) {
        throw "ASP54W1302 archive SHA-256 mismatch: actual=$ActualSha256"
    }
}

function Get-WasmtimeDependencyArchive {
    param(
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)]$Lock,
        [string]$DownloadUriOverride = ''
    )

    $ArchivePath = Get-WasmtimeArchiveCachePath -CacheRoot $CacheRoot -Lock $Lock
    if (Test-Path -LiteralPath $ArchivePath -PathType Leaf) {
        Assert-WasmtimeArchive -Path $ArchivePath -Lock $Lock
        return $ArchivePath
    }

    $VersionCacheRoot = Split-Path -Parent $ArchivePath
    New-Item -ItemType Directory -Force -Path $VersionCacheRoot | Out-Null
    $PartialPath = "$ArchivePath.partial-$([Guid]::NewGuid().ToString('N')).tmp"
    $Uri = if ([string]::IsNullOrWhiteSpace($DownloadUriOverride)) {
        [string]$Lock.archive.url
    }
    else {
        $DownloadUriOverride
    }
    try {
        try {
            Invoke-WebRequest -Uri $Uri -OutFile $PartialPath
        }
        catch {
            throw "ASP54W1303 archive download failed: $($_.Exception.Message)"
        }
        Assert-WasmtimeArchive -Path $PartialPath -Lock $Lock
        if (Test-Path -LiteralPath $ArchivePath) {
            Assert-WasmtimeArchive -Path $ArchivePath -Lock $Lock
            Remove-Item -LiteralPath $PartialPath -Force
        }
        else {
            [System.IO.File]::Move($PartialPath, $ArchivePath)
        }
    }
    finally {
        if (Test-Path -LiteralPath $PartialPath) {
            Remove-Item -LiteralPath $PartialPath -Force
        }
    }
    return $ArchivePath
}

function Assert-ArchiveEntryIsSafe {
    param(
        [Parameter(Mandatory = $true)]$Entry,
        [Parameter(Mandatory = $true)][string]$ExpectedRoot,
        [Parameter(Mandatory = $true)]$SeenPaths
    )

    $RawPath = [string]$Entry.FullName
    $NormalizedPath = $RawPath.Replace('\', '/')
    if ([string]::IsNullOrWhiteSpace($NormalizedPath) -or
        $NormalizedPath.StartsWith('/', [System.StringComparison]::Ordinal) -or
        $NormalizedPath -match '^[A-Za-z]:') {
        throw "ASP54W1401 unsafe archive entry path: $RawPath"
    }
    $Segments = @($NormalizedPath.Split('/') | Where-Object { $_ -cne '' })
    if ($Segments.Count -eq 0 -or
        $Segments[0] -cne $ExpectedRoot -or
        @($Segments | Where-Object {
            $_ -ceq '..' -or
            $_ -ceq '.' -or
            $_.Contains(':')
        }).Count -gt 0) {
        throw "ASP54W1401 archive entry escaped or added an unexpected root: $RawPath"
    }
    $CanonicalEntryPath = $Segments -join '/'
    if (-not $SeenPaths.Add($CanonicalEntryPath)) {
        throw "ASP54W1401 duplicate archive entry path: $RawPath"
    }
    $UnixType = (([int64]$Entry.ExternalAttributes -shr 16) -band 0xF000)
    $WindowsAttributes = ([int64]$Entry.ExternalAttributes -band 0xFFFF)
    if ($UnixType -eq 0xA000 -or
        ($WindowsAttributes -band [int][System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "ASP54W1402 archive contains a link or reparse-point entry: $RawPath"
    }
}

function Expand-ValidatedWasmtimeZipArchive {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)]$Lock
    )

    New-Item -ItemType Directory -Path $Destination | Out-Null
    $DestinationRoot = [System.IO.Path]::GetFullPath($Destination)
    $DestinationPrefix = $DestinationRoot.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    $FileStream = [System.IO.File]::OpenRead($ArchivePath)
    $Archive = [System.IO.Compression.ZipArchive]::new(
        $FileStream,
        [System.IO.Compression.ZipArchiveMode]::Read,
        $false)
    try {
        $SeenPaths = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
        foreach ($Entry in $Archive.Entries) {
            Assert-ArchiveEntryIsSafe `
                -Entry $Entry `
                -ExpectedRoot $Lock.archive.root `
                -SeenPaths $SeenPaths
        }
        foreach ($Entry in $Archive.Entries) {
            $NormalizedPath = $Entry.FullName.Replace('\', '/')
            $Segments = @($NormalizedPath.Split('/') | Where-Object { $_ -cne '' })
            $RelativePath = $Segments -join [System.IO.Path]::DirectorySeparatorChar
            $TargetPath = [System.IO.Path]::GetFullPath((Join-Path $DestinationRoot $RelativePath))
            if ($TargetPath -cne $DestinationRoot -and
                -not $TargetPath.StartsWith(
                    $DestinationPrefix,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "ASP54W1401 archive entry escaped staging: $NormalizedPath"
            }
            $IsDirectory = $Entry.FullName.EndsWith(
                '/',
                [System.StringComparison]::Ordinal) -or [string]::IsNullOrEmpty($Entry.Name)
            if ($IsDirectory) {
                New-Item -ItemType Directory -Force -Path $TargetPath | Out-Null
                continue
            }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $TargetPath) | Out-Null
            $InputStream = $Entry.Open()
            $OutputStream = [System.IO.File]::Open(
                $TargetPath,
                [System.IO.FileMode]::CreateNew,
                [System.IO.FileAccess]::Write,
                [System.IO.FileShare]::None)
            try {
                $InputStream.CopyTo($OutputStream)
            }
            finally {
                $OutputStream.Dispose()
                $InputStream.Dispose()
            }
        }
    }
    finally {
        $Archive.Dispose()
        $FileStream.Dispose()
    }
    Assert-NoReparseTree -Root $DestinationRoot -Code 'ASP54W1402'
    $ArchiveRoot = Join-Path $DestinationRoot $Lock.archive.root
    if (-not (Test-Path -LiteralPath $ArchiveRoot -PathType Container)) {
        throw 'ASP54W1403 archive root is missing'
    }
    return $ArchiveRoot
}

function Expand-ValidatedWasmtimeTarArchive {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)]$Lock
    )

    $TarCommand = Get-Command tar.exe -CommandType Application -ErrorAction SilentlyContinue
    if ($null -eq $TarCommand) {
        throw 'ASP54W1406 tar.exe is required for the Android Wasmtime archive'
    }
    $Entries = @(& $TarCommand.Source -tf $ArchivePath)
    if ($LASTEXITCODE -ne 0 -or $Entries.Count -eq 0) {
        throw 'ASP54W1406 Android Wasmtime archive listing failed'
    }
    $VerboseEntries = @(& $TarCommand.Source -tvf $ArchivePath)
    if ($LASTEXITCODE -ne 0 -or $VerboseEntries.Count -ne $Entries.Count) {
        throw 'ASP54W1406 Android Wasmtime archive type listing failed'
    }
    foreach ($VerboseEntry in $VerboseEntries) {
        if ([string]::IsNullOrWhiteSpace([string]$VerboseEntry) -or
            @('-', 'd') -cnotcontains ([string]$VerboseEntry)[0].ToString()) {
            throw 'ASP54W1402 Android archive contains a link or special entry'
        }
    }

    $SeenPaths = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($Entry in $Entries) {
        $RawPath = [string]$Entry
        $NormalizedPath = $RawPath.Replace('\', '/').TrimEnd('/')
        if ([string]::IsNullOrWhiteSpace($NormalizedPath) -or
            $NormalizedPath.StartsWith('/', [System.StringComparison]::Ordinal) -or
            $NormalizedPath -match '^[A-Za-z]:') {
            throw "ASP54W1401 unsafe archive entry path: $RawPath"
        }
        $Segments = @($NormalizedPath.Split('/') | Where-Object { $_ -cne '' })
        if ($Segments.Count -eq 0 -or
            $Segments[0] -cne [string]$Lock.archive.root -or
            @($Segments | Where-Object {
                    $_ -ceq '..' -or
                    $_ -ceq '.' -or
                    $_.Contains(':')
                }).Count -gt 0 -or
            -not $SeenPaths.Add(($Segments -join '/'))) {
            throw "ASP54W1401 archive entry escaped, duplicated, or added an unexpected root: $RawPath"
        }
    }

    New-Item -ItemType Directory -Path $Destination | Out-Null
    & $TarCommand.Source -xf $ArchivePath -C $Destination
    if ($LASTEXITCODE -ne 0) {
        throw 'ASP54W1406 Android Wasmtime archive extraction failed'
    }
    Assert-NoReparseTree -Root $Destination -Code 'ASP54W1402'
    $ArchiveRoot = Join-Path $Destination $Lock.archive.root
    if (-not (Test-Path -LiteralPath $ArchiveRoot -PathType Container)) {
        throw 'ASP54W1403 archive root is missing'
    }
    return $ArchiveRoot
}

function Expand-ValidatedWasmtimeArchive {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)]$Lock
    )

    if ([string]$Lock.platform -ceq 'Android') {
        return Expand-ValidatedWasmtimeTarArchive `
            -ArchivePath $ArchivePath `
            -Destination $Destination `
            -Lock $Lock
    }
    return Expand-ValidatedWasmtimeZipArchive `
        -ArchivePath $ArchivePath `
        -Destination $Destination `
        -Lock $Lock
}

function Assert-AvidScriptAarch64StaticArchive {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        $Signature = [byte[]]::new(8)
        if ($Stream.Read($Signature, 0, $Signature.Length) -ne $Signature.Length -or
            [System.Text.Encoding]::ASCII.GetString($Signature) -cne "!<arch>`n") {
            throw 'ASP54W1407 Android static library is not a standard ar archive'
        }
        $FoundAarch64Elf = $false
        while ($Stream.Position -lt $Stream.Length) {
            $Header = [byte[]]::new(60)
            if ($Stream.Read($Header, 0, $Header.Length) -ne $Header.Length -or
                $Header[58] -ne [byte][char]'`' -or
                $Header[59] -ne [byte]10) {
                throw 'ASP54W1407 Android static library has an invalid member header'
            }
            $SizeText = [System.Text.Encoding]::ASCII.GetString($Header, 48, 10).Trim()
            $MemberSize = [int64]0
            if (-not [int64]::TryParse($SizeText, [ref]$MemberSize) -or $MemberSize -lt 0) {
                throw 'ASP54W1407 Android static library has an invalid member size'
            }
            $MemberStart = $Stream.Position
            if ($MemberSize -ge 20) {
                $Prefix = [byte[]]::new(20)
                if ($Stream.Read($Prefix, 0, $Prefix.Length) -ne $Prefix.Length) {
                    throw 'ASP54W1407 Android static library member is truncated'
                }
                if ($Prefix[0] -eq 0x7f -and
                    $Prefix[1] -eq [byte][char]'E' -and
                    $Prefix[2] -eq [byte][char]'L' -and
                    $Prefix[3] -eq [byte][char]'F') {
                    $Machine = [System.BitConverter]::ToUInt16($Prefix, 18)
                    if ($Prefix[4] -ne 2 -or $Prefix[5] -ne 1 -or $Machine -ne 183) {
                        throw 'ASP54W1407 Android static library contains a non-AArch64 ELF object'
                    }
                    $FoundAarch64Elf = $true
                }
            }
            $NextMember = $MemberStart + $MemberSize + ($MemberSize % 2)
            if ($NextMember -gt $Stream.Length) {
                throw 'ASP54W1407 Android static library member exceeds the archive boundary'
            }
            $Stream.Position = $NextMember
        }
        if (-not $FoundAarch64Elf) {
            throw 'ASP54W1407 Android static library contains no AArch64 ELF object'
        }
    }
    finally {
        $Stream.Dispose()
    }
}

function Assert-WasmtimeArchiveLayout {
    param(
        [Parameter(Mandatory = $true)][string]$ArchiveRoot,
        [Parameter(Mandatory = $true)]$Lock
    )

    $RequiredPaths = [System.Collections.Generic.List[string]]::new()
    $RequiredPaths.Add((Join-Path $ArchiveRoot "$($Lock.layout.include_relative_path)/wasmtime.h"))
    $RequiredPaths.Add((Join-Path $ArchiveRoot "$($Lock.layout.include_relative_path)/wasmtime/conf.h"))
    $RequiredPaths.Add((Join-Path $ArchiveRoot $Lock.layout.license_relative_path))
    if ([string]$Lock.platform -ceq 'Android') {
        $RequiredPaths.Add((Join-Path $ArchiveRoot $Lock.layout.static_library_relative_path))
    }
    else {
        $RequiredPaths.Add((Join-Path $ArchiveRoot $Lock.layout.dll_relative_path))
        $RequiredPaths.Add((Join-Path $ArchiveRoot $Lock.layout.import_library_relative_path))
    }
    foreach ($RequiredPath in $RequiredPaths) {
        if (-not (Test-Path -LiteralPath $RequiredPath -PathType Leaf)) {
            throw "ASP54W1404 archive Cranelift layout is incomplete: $RequiredPath"
        }
    }
    $ConfigPath = Join-Path $ArchiveRoot "$($Lock.layout.include_relative_path)/wasmtime/conf.h"
    $ConfigText = [System.IO.File]::ReadAllText($ConfigPath)
    if ($ConfigText -notmatch '(?m)^\s*#define\s+WASMTIME_FEATURE_CRANELIFT\b') {
        throw 'ASP54W1405 archive C API does not enable the Cranelift compiler feature'
    }
    if ([string]$Lock.platform -ceq 'Android') {
        $StaticLibraryPath = Join-Path $ArchiveRoot $Lock.layout.static_library_relative_path
        if ((Get-FileSha256 -Path $StaticLibraryPath) -cne
            [string]$Lock.layout.static_library_sha256) {
            throw 'ASP54W1407 Android static library SHA-256 mismatch'
        }
        Assert-AvidScriptAarch64StaticArchive -Path $StaticLibraryPath
    }
}

function Get-InstalledContentSummary {
    param(
        [Parameter(Mandatory = $true)][string]$InstallPath,
        [Parameter(Mandatory = $true)][string]$MarkerName
    )

    $ManifestLines = [System.Collections.Generic.List[string]]::new()
    foreach ($File in Get-ChildItem -LiteralPath $InstallPath -File -Recurse -Force) {
        $RelativePath = [System.IO.Path]::GetRelativePath(
            $InstallPath,
            $File.FullName).Replace('\', '/')
        if ($RelativePath -ceq $MarkerName) {
            continue
        }
        $ManifestLines.Add(("{0}`t{1}" -f $RelativePath, (Get-FileSha256 $File.FullName)))
    }
    $SortedLines = @($ManifestLines)
    [Array]::Sort($SortedLines, [System.StringComparer]::Ordinal)
    $CanonicalManifest = [string]::Join("`n", $SortedLines) + "`n"
    return [pscustomobject]@{
        sha256 = Get-Sha256Bytes (
            [System.Text.UTF8Encoding]::new($false).GetBytes($CanonicalManifest))
        file_count = [int64]$SortedLines.Count
    }
}

function Write-ManagedMarker {
    param(
        [Parameter(Mandatory = $true)][string]$InstallPath,
        [Parameter(Mandatory = $true)]$Lock,
        [Parameter(Mandatory = $true)][string]$LockSha256
    )

    $Summary = Get-InstalledContentSummary `
        -InstallPath $InstallPath `
        -MarkerName $Lock.install.managed_marker_name
    $Marker = [ordered]@{
        schema_version = 1
        lock_canonical_sha256 = $LockSha256
        version = [string]$Lock.version
        platform = [string]$Lock.platform
        archive_sha256 = [string]$Lock.archive.sha256
        installed_content_sha256 = $Summary.sha256
        installed_file_count = $Summary.file_count
    }
    if ($Lock.PSObject.Properties.Name -ccontains 'architecture') {
        $Marker['architecture'] = [string]$Lock.architecture
    }
    $MarkerPath = Join-Path $InstallPath $Lock.install.managed_marker_name
    $MarkerJson = $Marker | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText(
        $MarkerPath,
        $MarkerJson + "`n",
        [System.Text.UTF8Encoding]::new($false))
    return $Summary
}

function Test-WasmtimeDependency {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]$Lock,
        [Parameter(Mandatory = $true)][string]$LockSha256
    )

    $Paths = Get-WasmtimeInstallPaths -RepositoryRoot $RepositoryRoot -Lock $Lock
    if (-not (Test-Path -LiteralPath $Paths.InstallPath -PathType Container)) {
        throw 'ASP54W1601 managed Wasmtime installation is missing'
    }
    Assert-NoReparseTree -Root $Paths.InstallPath -Code 'ASP54W1603'
    Assert-NoAlternateDataStreams -Root $Paths.InstallPath -Code 'ASP54W1606'
    if (-not (Test-Path -LiteralPath $Paths.MarkerPath -PathType Leaf)) {
        throw 'ASP54W1602 managed marker is missing'
    }
    try {
        $Marker = Get-Content -LiteralPath $Paths.MarkerPath -Raw | ConvertFrom-Json
    }
    catch {
        throw "ASP54W1602 managed marker is invalid JSON: $($_.Exception.Message)"
    }
    $ExpectedProperties = @(
        'schema_version',
        'lock_canonical_sha256',
        'version',
        'platform',
        'archive_sha256',
        'installed_content_sha256',
        'installed_file_count')
    if ($Lock.PSObject.Properties.Name -ccontains 'architecture') {
        $ExpectedProperties += 'architecture'
    }
    $ActualProperties = @($Marker.PSObject.Properties.Name)
    if ($ActualProperties.Count -ne $ExpectedProperties.Count -or
        @($ExpectedProperties | Where-Object { $_ -notin $ActualProperties }).Count -gt 0) {
        throw 'ASP54W1602 managed marker shape is invalid'
    }
    if ([int]$Marker.schema_version -ne 1 -or
        [string]$Marker.lock_canonical_sha256 -cne $LockSha256 -or
        [string]$Marker.version -cne [string]$Lock.version -or
        [string]$Marker.platform -cne [string]$Lock.platform -or
        [string]$Marker.archive_sha256 -cne [string]$Lock.archive.sha256) {
        throw 'ASP54W1604 managed marker or lock identity drifted'
    }
    if ($Lock.PSObject.Properties.Name -ccontains 'architecture' -and
        [string]$Marker.architecture -cne [string]$Lock.architecture) {
        throw 'ASP54W1604 managed marker architecture drifted'
    }
    $RequiredInstalledPaths = if ([string]$Lock.platform -ceq 'Android') {
        @('include/wasmtime.h', 'lib/libwasmtime.a', 'LICENSE')
    }
    else {
        @('include/wasmtime.h', 'lib/wasmtime.dll', 'lib/wasmtime.dll.lib', 'LICENSE')
    }
    foreach ($RelativePath in $RequiredInstalledPaths) {
        if (-not (Test-Path -LiteralPath (Join-Path $Paths.InstallPath $RelativePath) -PathType Leaf)) {
            throw "ASP54W1605 managed content is incomplete: $RelativePath"
        }
    }
    if ([string]$Lock.platform -ceq 'Android') {
        $StaticLibraryPath = Join-Path $Paths.InstallPath 'lib/libwasmtime.a'
        if ((Get-FileSha256 -Path $StaticLibraryPath) -cne
            [string]$Lock.layout.static_library_sha256) {
            throw 'ASP54W1605 Android static library identity drifted'
        }
        Assert-AvidScriptAarch64StaticArchive -Path $StaticLibraryPath
    }
    $Summary = Get-InstalledContentSummary `
        -InstallPath $Paths.InstallPath `
        -MarkerName $Lock.install.managed_marker_name
    if ([string]$Marker.installed_content_sha256 -cne $Summary.sha256 -or
        [int64]$Marker.installed_file_count -ne $Summary.file_count) {
        throw 'ASP54W1605 managed content summary drifted'
    }
    return [pscustomobject]@{
        succeeded = $true
        mode = 'Verify'
        status = 'verified'
        version = [string]$Lock.version
        installed_content_sha256 = $Summary.sha256
        installed_file_count = $Summary.file_count
    }
}

function Install-WasmtimeDependency {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)]$Lock,
        [Parameter(Mandatory = $true)][string]$LockSha256,
        [string]$DownloadUriOverride = ''
    )

    $Paths = Get-WasmtimeInstallPaths -RepositoryRoot $RepositoryRoot -Lock $Lock
    if (Test-Path -LiteralPath $Paths.InstallPath) {
        $Verified = Test-WasmtimeDependency $RepositoryRoot $Lock $LockSha256
        $Verified.mode = 'Install'
        $Verified.status = 'already_verified'
        return $Verified
    }
    $ArchivePath = Get-WasmtimeDependencyArchive `
        -CacheRoot $CacheRoot `
        -Lock $Lock `
        -DownloadUriOverride $DownloadUriOverride
    Assert-WasmtimeArchive -Path $ArchivePath -Lock $Lock

    $ExtractionRoot = Join-Path (
        [System.IO.Path]::GetTempPath()) (
        'AvidScriptWasmtimeExtract-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $Paths.InstalledRoot | Out-Null
    New-Item -ItemType Directory -Force -Path $Paths.PlatformRoot | Out-Null
    $CandidatePath = Join-Path $Paths.PlatformRoot (
        '.install-' + [Guid]::NewGuid().ToString('N'))
    try {
        $ArchiveRoot = Expand-ValidatedWasmtimeArchive `
            -ArchivePath $ArchivePath `
            -Destination $ExtractionRoot `
            -Lock $Lock
        Assert-WasmtimeArchiveLayout -ArchiveRoot $ArchiveRoot -Lock $Lock
        New-Item -ItemType Directory -Path $CandidatePath | Out-Null
        Copy-Item `
            -LiteralPath (Join-Path $ArchiveRoot $Lock.layout.include_relative_path) `
            -Destination (Join-Path $CandidatePath 'include') `
            -Recurse
        New-Item -ItemType Directory -Path (Join-Path $CandidatePath 'lib') | Out-Null
        if ([string]$Lock.platform -ceq 'Android') {
            Copy-Item `
                -LiteralPath (Join-Path $ArchiveRoot $Lock.layout.static_library_relative_path) `
                -Destination (Join-Path $CandidatePath 'lib/libwasmtime.a')
        }
        else {
            Copy-Item `
                -LiteralPath (Join-Path $ArchiveRoot $Lock.layout.dll_relative_path) `
                -Destination (Join-Path $CandidatePath 'lib/wasmtime.dll')
            Copy-Item `
                -LiteralPath (Join-Path $ArchiveRoot $Lock.layout.import_library_relative_path) `
                -Destination (Join-Path $CandidatePath 'lib/wasmtime.dll.lib')
        }
        Copy-Item `
            -LiteralPath (Join-Path $ArchiveRoot $Lock.layout.license_relative_path) `
            -Destination (Join-Path $CandidatePath 'LICENSE')
        $Summary = Write-ManagedMarker `
            -InstallPath $CandidatePath `
            -Lock $Lock `
            -LockSha256 $LockSha256
        if (Test-Path -LiteralPath $Paths.InstallPath) {
            throw 'ASP54W1501 install target appeared before atomic publication'
        }
        $PublishPaths = Get-WasmtimeInstallPaths -RepositoryRoot $RepositoryRoot -Lock $Lock
        if ($PublishPaths.InstallPath -cne $Paths.InstallPath -or
            $PublishPaths.PlatformRoot -cne $Paths.PlatformRoot) {
            throw 'ASP54W1503 managed publication path changed during installation'
        }
        Assert-NoReparseTree -Root $CandidatePath -Code 'ASP54W1503'
        if (Test-Path -LiteralPath $PublishPaths.InstallPath) {
            throw 'ASP54W1501 install target appeared before atomic publication'
        }
        [System.IO.Directory]::Move($CandidatePath, $PublishPaths.InstallPath)
        return [pscustomobject]@{
            succeeded = $true
            mode = 'Install'
            status = 'installed'
            version = [string]$Lock.version
            installed_content_sha256 = $Summary.sha256
            installed_file_count = $Summary.file_count
        }
    }
    finally {
        if (Test-Path -LiteralPath $ExtractionRoot) {
            Remove-Item -LiteralPath $ExtractionRoot -Recurse -Force
        }
        if (Test-Path -LiteralPath $CandidatePath) {
            $CleanupPaths = Get-WasmtimeInstallPaths -RepositoryRoot $RepositoryRoot -Lock $Lock
            if ($CleanupPaths.PlatformRoot -cne $Paths.PlatformRoot) {
                throw 'ASP54W1502 candidate cleanup path changed during installation'
            }
            $CandidateFullPath = [System.IO.Path]::GetFullPath($CandidatePath)
            $PlatformPrefix = $CleanupPaths.PlatformRoot.TrimEnd(
                [System.IO.Path]::DirectorySeparatorChar,
                [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
            if (-not $CandidateFullPath.StartsWith(
                $PlatformPrefix,
                [System.StringComparison]::OrdinalIgnoreCase)) {
                throw 'ASP54W1502 candidate cleanup escaped the platform root'
            }
            Remove-Item -LiteralPath $CandidateFullPath -Recurse -Force
        }
    }
}

function Remove-WasmtimeDependency {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]$Lock,
        [Parameter(Mandatory = $true)][string]$LockSha256
    )

    $Paths = Get-WasmtimeInstallPaths -RepositoryRoot $RepositoryRoot -Lock $Lock
    [void](Test-WasmtimeDependency $RepositoryRoot $Lock $LockSha256)
    $RemovalPaths = Get-WasmtimeInstallPaths -RepositoryRoot $RepositoryRoot -Lock $Lock
    if ($RemovalPaths.InstallPath -cne $Paths.InstallPath -or
        $RemovalPaths.InstalledRoot -cne $Paths.InstalledRoot) {
        throw 'ASP54W1701 managed removal path changed after verification'
    }
    $InstallFullPath = [System.IO.Path]::GetFullPath($RemovalPaths.InstallPath)
    $InstalledPrefix = $RemovalPaths.InstalledRoot.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $InstallFullPath.StartsWith(
        $InstalledPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'ASP54W1701 refusing to remove outside the installed boundary'
    }
    Assert-NoReparseTree -Root $InstallFullPath -Code 'ASP54W1702'
    Assert-NoAlternateDataStreams -Root $InstallFullPath -Code 'ASP54W1606'
    Remove-Item -LiteralPath $InstallFullPath -Recurse -Force
    foreach ($Parent in @(
            $RemovalPaths.PlatformRoot,
            $RemovalPaths.PlatformFamilyRoot,
            $RemovalPaths.InstalledRoot) | Select-Object -Unique) {
        if ((Test-Path -LiteralPath $Parent -PathType Container) -and
            @(Get-ChildItem -LiteralPath $Parent -Force).Count -eq 0) {
            Assert-ContainedOrdinaryDirectoryPath `
                -TrustedRoot $RemovalPaths.RepositoryRoot `
                -TargetPath $Parent
            if (Test-IsReparsePoint $Parent) {
                throw 'ASP54W1702 refusing to remove a reparse-point parent'
            }
            Remove-Item -LiteralPath $Parent -Force
        }
    }
    return [pscustomobject]@{
        succeeded = $true
        mode = 'Remove'
        status = 'removed'
        version = [string]$Lock.version
    }
}

if ($MyInvocation.InvocationName -eq '.') {
    return
}

try {
    if ([string]::IsNullOrWhiteSpace($Mode)) {
        throw 'ASP54W1000 Mode is required'
    }
    $Lock = Read-WasmtimeDependencyLock -Path $LockPath -Schema $SchemaPath
    $LockSha256 = Get-CanonicalFileSha256 -Path $LockPath
    if ([string]::IsNullOrWhiteSpace($CacheRoot)) {
        $CacheRoot = Join-Path ([System.IO.Path]::GetTempPath()) 'AvidScriptDeps/Wasmtime'
    }
    $Result = switch ($Mode) {
        'ValidateLock' {
            [pscustomobject]@{
                succeeded = $true
                mode = 'ValidateLock'
                status = 'valid'
                version = [string]$Lock.version
                lock_canonical_sha256 = $LockSha256
            }
        }
        'Install' {
            Install-WasmtimeDependency `
                -RepositoryRoot $RepositoryRoot `
                -CacheRoot $CacheRoot `
                -Lock $Lock `
                -LockSha256 $LockSha256 `
                -DownloadUriOverride $DownloadUriOverride
        }
        'Verify' {
            Test-WasmtimeDependency `
                -RepositoryRoot $RepositoryRoot `
                -Lock $Lock `
                -LockSha256 $LockSha256
        }
        'Remove' {
            Remove-WasmtimeDependency `
                -RepositoryRoot $RepositoryRoot `
                -Lock $Lock `
                -LockSha256 $LockSha256
        }
    }
    $Result | ConvertTo-Json -Depth 8
}
catch {
    $Message = $_.Exception.Message
    if (-not $Message.StartsWith('ASP54W', [System.StringComparison]::Ordinal)) {
        $Message = "ASP54W1999 unexpected dependency failure: $Message"
    }
    Write-Error $Message
    exit 1
}
