[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('ValidateLock', 'Build', 'Verify', 'Remove')]
    [string]$Mode,
    [string]$RepositoryRoot = '',
    [string]$CacheRoot = '',
    [string]$LockPath = '',
    [string]$SourceArchiveOverride = ''
)

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent $ScriptRoot
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
if ([string]::IsNullOrWhiteSpace($LockPath)) {
    $LockPath = Join-Path $RepositoryRoot `
        'Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.lock.json'
}
if ([string]::IsNullOrWhiteSpace($CacheRoot)) {
    $CacheRoot = Join-Path ([System.Environment]::GetFolderPath('LocalApplicationData')) `
        'AvidScript/WasmtimePerformanceToolchain'
}
$CacheRoot = [System.IO.Path]::GetFullPath($CacheRoot)
$SchemaPath = Join-Path $RepositoryRoot `
    'Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.schema.json'

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-CanonicalTextSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Text = [System.IO.File]::ReadAllText($Path)
    if ($Text.Length -gt 0 -and $Text[0] -eq [char]0xFEFF) {
        $Text = $Text.Substring(1)
    }
    $Canonical = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Canonical)
    return [Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

function Write-Utf8Json {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $Json = $Value | ConvertTo-Json -Depth 12
    [System.IO.File]::WriteAllText(
        $Path,
        $Json + [System.Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
}

function Read-PerformanceToolchainLock {
    if (-not (Test-Path -LiteralPath $LockPath -PathType Leaf)) {
        throw "ASP57W1001 performance toolchain lock is missing: $LockPath"
    }
    if (-not (Test-Path -LiteralPath $SchemaPath -PathType Leaf)) {
        throw "ASP57W1002 performance toolchain schema is missing: $SchemaPath"
    }
    $Raw = Get-Content -LiteralPath $LockPath -Raw
    if (-not ($Raw | Test-Json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue)) {
        throw 'ASP57W1003 performance toolchain lock does not satisfy its schema'
    }
    $Lock = $Raw | ConvertFrom-Json
    $ExpectedCommit = '377cd917af258d932d55b201a646917ecf193639'
    if ([string]$Lock.upstream.commit -cne $ExpectedCommit -or
        -not ([string]$Lock.upstream.source_archive.url).Contains($ExpectedCommit)) {
        throw 'ASP57W1004 source archive identity is not bound to the pinned commit'
    }
    $PatchPath = [System.IO.Path]::GetFullPath(
        (Join-Path $RepositoryRoot ([string]$Lock.patch.relative_path)))
    if (-not (Test-Path -LiteralPath $PatchPath -PathType Leaf)) {
        throw "ASP57W1005 performance patch is missing: $PatchPath"
    }
    $PatchSha256 = Get-CanonicalTextSha256 -Path $PatchPath
    if ($PatchSha256 -cne [string]$Lock.patch.canonical_sha256) {
        throw "ASP57W1006 performance patch SHA-256 mismatch: $PatchSha256"
    }
    $TrackedLicense = [System.IO.Path]::GetFullPath(
        (Join-Path $RepositoryRoot ([string]$Lock.license.tracked_file)))
    if (-not (Test-Path -LiteralPath $TrackedLicense -PathType Leaf)) {
        throw "ASP57W1007 tracked Wasmtime license is missing: $TrackedLicense"
    }
    return [pscustomobject]@{
        Lock = $Lock
        PatchPath = $PatchPath
        PatchSha256 = $PatchSha256
    }
}

function Assert-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Code
    )

    $FullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $Prefix = $FullRoot + [System.IO.Path]::DirectorySeparatorChar
    if ($FullPath -ceq $FullRoot -or
        -not $FullPath.StartsWith($Prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Code path escaped its managed root: $FullPath"
    }
}

function Get-ManagedPaths {
    param([Parameter(Mandatory = $true)]$Lock)

    $InstalledRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $RepositoryRoot 'Source/ThirdParty/Wasmtime/installed/Win64'))
    $InstallPath = [System.IO.Path]::GetFullPath(
        (Join-Path $RepositoryRoot ([string]$Lock.install.relative_path)))
    Assert-PathWithin -Root $InstalledRoot -Path $InstallPath -Code 'ASP57W1101'
    return [pscustomobject]@{
        InstalledRoot = $InstalledRoot
        InstallPath = $InstallPath
        MarkerPath = Join-Path $InstallPath ([string]$Lock.install.managed_marker_name)
    }
}

function Assert-Archive {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Lock
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "ASP57W1201 source archive is missing: $Path"
    }
    $Length = [int64](Get-Item -LiteralPath $Path).Length
    if ($Length -ne [int64]$Lock.upstream.source_archive.size_bytes) {
        throw "ASP57W1202 source archive size mismatch: $Length"
    }
    $Sha256 = Get-FileSha256 -Path $Path
    if ($Sha256 -cne [string]$Lock.upstream.source_archive.sha256) {
        throw "ASP57W1203 source archive SHA-256 mismatch: $Sha256"
    }
}

function Get-SourceArchive {
    param([Parameter(Mandatory = $true)]$Lock)

    if (-not [string]::IsNullOrWhiteSpace($SourceArchiveOverride)) {
        $Override = [System.IO.Path]::GetFullPath($SourceArchiveOverride)
        Assert-Archive -Path $Override -Lock $Lock
        return $Override
    }
    $ArchiveRoot = Join-Path $CacheRoot 'archives'
    $ArchivePath = Join-Path $ArchiveRoot ([string]$Lock.upstream.source_archive.name)
    if (Test-Path -LiteralPath $ArchivePath -PathType Leaf) {
        Assert-Archive -Path $ArchivePath -Lock $Lock
        return $ArchivePath
    }
    New-Item -ItemType Directory -Force -Path $ArchiveRoot | Out-Null
    $PartialPath = "$ArchivePath.partial-$([Guid]::NewGuid().ToString('N'))"
    try {
        Invoke-WebRequest -Uri ([string]$Lock.upstream.source_archive.url) -OutFile $PartialPath
        Assert-Archive -Path $PartialPath -Lock $Lock
        [System.IO.File]::Move($PartialPath, $ArchivePath)
    }
    finally {
        if (Test-Path -LiteralPath $PartialPath) {
            Remove-Item -LiteralPath $PartialPath -Force
        }
    }
    return $ArchivePath
}

function Invoke-NativeTool {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$Code
    )

    Push-Location $WorkingDirectory
    try {
        & $Executable @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "$Code native tool failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
}

function Get-PreparedSource {
    param(
        [Parameter(Mandatory = $true)]$Lock,
        [Parameter(Mandatory = $true)][string]$PatchPath,
        [Parameter(Mandatory = $true)][string]$PatchSha256,
        [Parameter(Mandatory = $true)][string]$ArchivePath
    )

    $Key = "$(($Lock.upstream.source_archive.sha256).Substring(0, 16))-$($PatchSha256.Substring(0, 16))"
    $PreparedRoot = Join-Path $CacheRoot "sources/$Key"
    $SourceRoot = Join-Path $PreparedRoot ([string]$Lock.upstream.source_archive.root)
    $PreparedMarker = Join-Path $SourceRoot '.avidscript-prepared-source.json'
    if (Test-Path -LiteralPath $PreparedMarker -PathType Leaf) {
        $Marker = Get-Content -LiteralPath $PreparedMarker -Raw | ConvertFrom-Json
        if ([string]$Marker.source_sha256 -ceq [string]$Lock.upstream.source_archive.sha256 -and
            [string]$Marker.patch_sha256 -ceq $PatchSha256) {
            return $SourceRoot
        }
    }
    $SourcesRoot = Join-Path $CacheRoot 'sources'
    New-Item -ItemType Directory -Force -Path $SourcesRoot | Out-Null
    Assert-PathWithin -Root $SourcesRoot -Path $PreparedRoot -Code 'ASP57W1301'
    if (Test-Path -LiteralPath $PreparedRoot) {
        Remove-Item -LiteralPath $PreparedRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $PreparedRoot | Out-Null
    [System.IO.Compression.ZipFile]::ExtractToDirectory($ArchivePath, $PreparedRoot)
    if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
        throw 'ASP57W1302 source archive root is missing after extraction'
    }
    Invoke-NativeTool -Executable 'git' `
        -Arguments @('apply', '--check', '--unidiff-zero', '--whitespace=nowarn', $PatchPath) `
        -WorkingDirectory $SourceRoot -Code 'ASP57W1303'
    Invoke-NativeTool -Executable 'git' `
        -Arguments @('apply', '--unidiff-zero', '--whitespace=nowarn', $PatchPath) `
        -WorkingDirectory $SourceRoot -Code 'ASP57W1304'
    Write-Utf8Json -Value ([ordered]@{
        source_sha256 = [string]$Lock.upstream.source_archive.sha256
        patch_sha256 = $PatchSha256
    }) -Path $PreparedMarker
    return $SourceRoot
}

function Get-InstalledContentSha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$MarkerName
    )

    $Lines = [System.Collections.Generic.List[string]]::new()
    foreach ($File in Get-ChildItem -LiteralPath $Root -File -Recurse | Sort-Object FullName) {
        $Relative = [System.IO.Path]::GetRelativePath($Root, $File.FullName).Replace('\', '/')
        if ($Relative -ceq $MarkerName) {
            continue
        }
        $Lines.Add("$Relative|$($File.Length)|$(Get-FileSha256 -Path $File.FullName)")
    }
    $Canonical = [string]::Join("`n", $Lines) + "`n"
    $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Canonical)
    return [Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

function Assert-Export {
    param(
        [Parameter(Mandatory = $true)][string]$DllPath,
        [Parameter(Mandatory = $true)][string]$Symbol
    )

    $Handle = [System.Runtime.InteropServices.NativeLibrary]::Load($DllPath)
    try {
        $Address = [IntPtr]::Zero
        if (-not [System.Runtime.InteropServices.NativeLibrary]::TryGetExport(
            $Handle,
            $Symbol,
            [ref]$Address) -or $Address -eq [IntPtr]::Zero) {
            throw "ASP57W1401 required compiler extension export is missing: $Symbol"
        }
    }
    finally {
        [System.Runtime.InteropServices.NativeLibrary]::Free($Handle)
    }
}

function Test-ManagedInstall {
    param(
        [Parameter(Mandatory = $true)]$Lock,
        [Parameter(Mandatory = $true)]$Paths,
        [Parameter(Mandatory = $true)][string]$PatchSha256
    )

    if (-not (Test-Path -LiteralPath $Paths.MarkerPath -PathType Leaf)) {
        throw 'ASP57W1501 performance toolchain managed marker is missing'
    }
    $Marker = Get-Content -LiteralPath $Paths.MarkerPath -Raw | ConvertFrom-Json
    if ([string]$Marker.toolchain_id -cne [string]$Lock.toolchain_id -or
        [string]$Marker.source_sha256 -cne [string]$Lock.upstream.source_archive.sha256 -or
        [string]$Marker.patch_sha256 -cne $PatchSha256) {
        throw 'ASP57W1502 performance toolchain managed marker identity mismatch'
    }
    $DllPath = Join-Path $Paths.InstallPath ([string]$Lock.layout.dll_relative_path)
    $ImportPath = Join-Path $Paths.InstallPath ([string]$Lock.layout.import_library_relative_path)
    $HeaderPath = Join-Path $Paths.InstallPath 'include/wasmtime.h'
    $ConfigHeaderPath = Join-Path $Paths.InstallPath 'include/wasmtime/config.h'
    $LicensePath = Join-Path $Paths.InstallPath ([string]$Lock.layout.license_relative_path)
    foreach ($RequiredPath in @($DllPath, $ImportPath, $HeaderPath, $ConfigHeaderPath, $LicensePath)) {
        if (-not (Test-Path -LiteralPath $RequiredPath -PathType Leaf)) {
            throw "ASP57W1503 managed output is missing: $RequiredPath"
        }
    }
    if ((Get-FileSha256 -Path $DllPath) -cne [string]$Marker.dll_sha256) {
        throw 'ASP57W1504 managed runtime DLL SHA-256 mismatch'
    }
    $ContentSha256 = Get-InstalledContentSha256 `
        -Root $Paths.InstallPath -MarkerName ([string]$Lock.install.managed_marker_name)
    if ($ContentSha256 -cne [string]$Marker.installed_content_sha256) {
        throw 'ASP57W1505 managed runtime content SHA-256 mismatch'
    }
    Assert-Export -DllPath $DllPath -Symbol ([string]$Lock.patch.export_symbol)
    Assert-Export -DllPath $DllPath -Symbol ([string]$Lock.patch.precompile_export_symbol)
    return [pscustomobject]@{
        install_path = $Paths.InstallPath
        dll_sha256 = [string]$Marker.dll_sha256
        installed_content_sha256 = $ContentSha256
        compiler_profile = [string]$Lock.compiler_profile.id
    }
}

$Resolved = Read-PerformanceToolchainLock
$Lock = $Resolved.Lock
$Paths = Get-ManagedPaths -Lock $Lock
if ($Mode -eq 'ValidateLock') {
    [pscustomobject]@{
        result = 'wasmtime_performance_toolchain_lock_valid'
        toolchain_id = [string]$Lock.toolchain_id
        source_sha256 = [string]$Lock.upstream.source_archive.sha256
        patch_sha256 = $Resolved.PatchSha256
    } | ConvertTo-Json -Depth 4
    exit 0
}
if ($Mode -eq 'Verify') {
    $Result = Test-ManagedInstall -Lock $Lock -Paths $Paths -PatchSha256 $Resolved.PatchSha256
    [pscustomobject]@{
        result = 'wasmtime_performance_toolchain_verified'
        evidence = $Result
    } | ConvertTo-Json -Depth 6
    exit 0
}
if ($Mode -eq 'Remove') {
    if (Test-Path -LiteralPath $Paths.InstallPath) {
        if (-not (Test-Path -LiteralPath $Paths.MarkerPath -PathType Leaf)) {
            throw 'ASP57W1601 refusing to remove an unmanaged performance toolchain directory'
        }
        Assert-PathWithin -Root $Paths.InstalledRoot -Path $Paths.InstallPath -Code 'ASP57W1602'
        Remove-Item -LiteralPath $Paths.InstallPath -Recurse -Force
    }
    [pscustomobject]@{ result = 'wasmtime_performance_toolchain_removed' } | ConvertTo-Json
    exit 0
}

foreach ($CommandName in @('cmake', 'git', 'rustc', 'cargo')) {
    if ($null -eq (Get-Command $CommandName -ErrorAction SilentlyContinue)) {
        throw "ASP57W1701 required build tool is unavailable: $CommandName"
    }
}
$env:RUSTUP_TOOLCHAIN = [string]$Lock.rust.toolchain
$RustVersion = & rustc --version
if ($LASTEXITCODE -ne 0 -or $RustVersion -notmatch '^rustc 1\.93\.0 ') {
    throw "ASP57W1702 pinned Rust toolchain is unavailable: $($Lock.rust.toolchain)"
}
$ArchivePath = Get-SourceArchive -Lock $Lock
$SourceRoot = Get-PreparedSource -Lock $Lock `
    -PatchPath $Resolved.PatchPath `
    -PatchSha256 $Resolved.PatchSha256 `
    -ArchivePath $ArchivePath
$BuildRoot = Join-Path $CacheRoot "build/$($Lock.toolchain_id)"
$CargoTargetRoot = Join-Path $CacheRoot 'cargo-target'
$StagingRoot = Join-Path $CacheRoot "staging/$($Lock.toolchain_id)"
$env:CARGO_TARGET_DIR = $CargoTargetRoot.Replace([System.IO.Path]::DirectorySeparatorChar, '/')
$env:SOURCE_DATE_EPOCH = [string]$Lock.rust.source_date_epoch
foreach ($OwnedPath in @($BuildRoot, $StagingRoot)) {
    $OwnedParent = Split-Path -Parent $OwnedPath
    New-Item -ItemType Directory -Force -Path $OwnedParent | Out-Null
    Assert-PathWithin -Root $CacheRoot -Path $OwnedPath -Code 'ASP57W1703'
}
if (Test-Path -LiteralPath $BuildRoot) {
    Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}
if (Test-Path -LiteralPath $StagingRoot) {
    Remove-Item -LiteralPath $StagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $StagingRoot | Out-Null
$CMakeArguments = @(
    '-S', (Join-Path $SourceRoot 'crates/c-api'),
    '-B', $BuildRoot
) + @($Lock.rust.cmake_arguments) + @("-DCMAKE_INSTALL_PREFIX=$StagingRoot")
Invoke-NativeTool -Executable 'cmake' -Arguments $CMakeArguments `
    -WorkingDirectory $SourceRoot -Code 'ASP57W1704'
Invoke-NativeTool -Executable 'cmake' `
    -Arguments @('--build', $BuildRoot, '--config', 'Release', '--parallel', '8') `
    -WorkingDirectory $SourceRoot -Code 'ASP57W1705'
Invoke-NativeTool -Executable 'cmake' `
    -Arguments @('--install', $BuildRoot, '--config', 'Release') `
    -WorkingDirectory $SourceRoot -Code 'ASP57W1706'
Copy-Item -LiteralPath (Join-Path $SourceRoot 'LICENSE') `
    -Destination (Join-Path $StagingRoot ([string]$Lock.layout.license_relative_path))
$StagedDll = Join-Path $StagingRoot ([string]$Lock.layout.dll_relative_path)
Assert-Export -DllPath $StagedDll -Symbol ([string]$Lock.patch.export_symbol)
Assert-Export -DllPath $StagedDll -Symbol ([string]$Lock.patch.precompile_export_symbol)
$MarkerName = [string]$Lock.install.managed_marker_name
$MarkerPath = Join-Path $StagingRoot $MarkerName
$InstalledContentSha256 = Get-InstalledContentSha256 -Root $StagingRoot -MarkerName $MarkerName
$Marker = [ordered]@{
    schema_version = 1
    toolchain_id = [string]$Lock.toolchain_id
    source_sha256 = [string]$Lock.upstream.source_archive.sha256
    patch_sha256 = $Resolved.PatchSha256
    rust_toolchain = [string]$Lock.rust.toolchain
    build_profile = [string]$Lock.rust.build_profile
    compiler_profile = [string]$Lock.compiler_profile.id
    dll_sha256 = Get-FileSha256 -Path $StagedDll
    installed_content_sha256 = $InstalledContentSha256
}
Write-Utf8Json -Value $Marker -Path $MarkerPath
New-Item -ItemType Directory -Force -Path $Paths.InstalledRoot | Out-Null
$PublishPath = "$($Paths.InstallPath).publish-$([Guid]::NewGuid().ToString('N'))"
Assert-PathWithin -Root $Paths.InstalledRoot -Path $PublishPath -Code 'ASP57W1707'
Copy-Item -LiteralPath $StagingRoot -Destination $PublishPath -Recurse
if (Test-Path -LiteralPath $Paths.InstallPath) {
    Assert-PathWithin -Root $Paths.InstalledRoot -Path $Paths.InstallPath -Code 'ASP57W1708'
    Remove-Item -LiteralPath $Paths.InstallPath -Recurse -Force
}
[System.IO.Directory]::Move($PublishPath, $Paths.InstallPath)
$Result = Test-ManagedInstall -Lock $Lock -Paths $Paths -PatchSha256 $Resolved.PatchSha256
[pscustomobject]@{
    result = 'wasmtime_performance_toolchain_built'
    rustc = $RustVersion
    evidence = $Result
} | ConvertTo-Json -Depth 6
