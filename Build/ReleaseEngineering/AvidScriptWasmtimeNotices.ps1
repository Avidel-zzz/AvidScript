. (Join-Path $PSScriptRoot 'AvidScriptBinaryPrivacy.ps1')
$script:WasmtimeNoticesSchema = Join-Path $PSScriptRoot 'AvidScriptWasmtimeNotices.schema.json'

function Get-AvidNoticeRelativePath([string]$Root, [string]$Path) {
    $relative = [IO.Path]::GetRelativePath([IO.Path]::GetFullPath($Root), [IO.Path]::GetFullPath($Path)).Replace('\', '/')
    if ($relative -eq '..' -or $relative.StartsWith('../') -or [IO.Path]::IsPathRooted($relative)) {
        throw 'ASWN1001 notice source escaped its declared root'
    }
    Assert-AvidScriptBinaryPrivacyAncestors -Path $Path
    return $relative
}

function Read-AvidNoticeBytes([string]$Path) {
    Assert-AvidScriptBinaryPrivacyAncestors -Path $Path
    if (@(Get-Item -LiteralPath $Path -Stream * | Where-Object Stream -CNE ':$DATA').Count -ne 0) {
        throw 'ASWN1001 alternate stream in notice source'
    }
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        [AvidScript.Release.BinaryPrivacyV1.Scanner]::AssertHandlePath($stream, $Path)
        if ($stream.Length -eq 0 -or $stream.Length -gt 16MB) { throw 'ASWN1001 empty or oversized notice source' }
        $bytes = [byte[]]::new($stream.Length)
        $stream.ReadExactly($bytes, 0, $bytes.Length)
        return ,$bytes
    } finally { $stream.Dispose() }
}

function Add-AvidNoticeText {
    param([string]$Path, [string]$OriginRoot, [string]$BundleRoot, $Checksums = $null)
    $relative = Get-AvidNoticeRelativePath $OriginRoot $Path
    $bytes = Read-AvidNoticeBytes $Path
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    if ($null -ne $Checksums -and (-not $Checksums.Contains($relative) -or $Checksums[$relative] -cne $hash)) {
        throw 'ASWN1002 registry notice bytes differ from Cargo checksum'
    }
    $extension = if ([IO.Path]::GetExtension($Path) -ieq '.html') { '.html' } else { '.txt' }
    $destination = "texts/$hash$extension"
    $fullDestination = Join-Path $BundleRoot $destination
    if (-not (Test-Path -LiteralPath $fullDestination)) { [IO.File]::WriteAllBytes($fullDestination, $bytes) }
    return [ordered]@{origin_path=$relative;path=$destination;sha256=$hash;length=$bytes.Length}
}

function Get-AvidNoticeArchiveChecksums([string]$ArchivePath, [string]$Prefix) {
    Assert-AvidScriptBinaryPrivacyAncestors -Path $ArchivePath
    $stream = [IO.FileStream]::new($ArchivePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        [AvidScript.Release.BinaryPrivacyV1.Scanner]::AssertHandlePath($stream, $ArchivePath)
        if ($stream.Length -gt 128MB) { throw 'ASWN1002 oversized crate archive' }
        $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant()
        $stream.Position = 0
        $gzip = [IO.Compression.GZipStream]::new($stream, [IO.Compression.CompressionMode]::Decompress, $true)
        try {
            $tar = [System.Formats.Tar.TarReader]::new($gzip, $true)
            try {
                $files = @{}
                $total = 0L
                while ($null -ne ($entry = $tar.GetNextEntry())) {
                    if ($entry.EntryType -eq [System.Formats.Tar.TarEntryType]::Directory) { continue }
                    if ($entry.EntryType -notin @([System.Formats.Tar.TarEntryType]::RegularFile, [System.Formats.Tar.TarEntryType]::V7RegularFile) -or
                        -not $entry.Name.StartsWith($Prefix + '/', [StringComparison]::Ordinal)) { throw 'ASWN1002 unsupported crate archive entry' }
                    $relative = $entry.Name.Substring($Prefix.Length + 1)
                    if ($relative -match '(^|/)\.\.(/|$)|[\\:]' -or $relative.StartsWith('/') -or $files.ContainsKey($relative)) {
                        throw 'ASWN1002 ambiguous crate archive path'
                    }
                    $total += $entry.Length
                    if ($total -gt 256MB -or $files.Count -ge 50000) { throw 'ASWN1002 crate archive exceeds scan limits' }
                    $digest = if ($entry.Length -eq 0) { [Security.Cryptography.SHA256]::HashData([byte[]]@()) }
                        else { [Security.Cryptography.SHA256]::HashData([IO.Stream]$entry.DataStream) }
                    $files[$relative] = [Convert]::ToHexString($digest).ToLowerInvariant()
                }
                return @{package=$hash;files=$files}
            } finally { $tar.Dispose() }
        } finally { $gzip.Dispose() }
    } finally { $stream.Dispose() }
}

function Test-AvidScriptWasmtimeNotices {
    param([Parameter(Mandatory)][string]$BundleRoot)
    Initialize-AvidScriptBinaryPrivacy
    $manifestPath = Join-Path $BundleRoot 'manifest.json'
    $json = Get-Content -LiteralPath $manifestPath -Raw
    if (-not ($json | Test-Json -SchemaFile $script:WasmtimeNoticesSchema -ErrorAction SilentlyContinue)) {
        throw 'ASWN1003 notice manifest violates schema'
    }
    $manifest = $json | ConvertFrom-Json -Depth 30
    if ($manifest.package_count -ne $manifest.packages.Count -or
        @($manifest.packages.id | Sort-Object -Unique).Count -ne $manifest.package_count) {
        throw 'ASWN1003 notice package inventory is inconsistent'
    }
    $references = @($manifest.packages | ForEach-Object notices) + @($manifest.rust.notices)
    $expected = @{}
    foreach ($reference in $references) {
        $full = Join-Path $BundleRoot $reference.path
        [void](Get-AvidNoticeRelativePath $BundleRoot $full)
        $bytes = Read-AvidNoticeBytes $full
        $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
        if ($hash -cne $reference.sha256 -or $bytes.Length -ne $reference.length) { throw 'ASWN1003 notice content identity mismatch' }
        $expected[$reference.path] = $true
    }
    $snapshot = @(Get-AvidScriptBinaryPrivacySnapshot -Root $BundleRoot)
    $actual = @($snapshot | Where-Object { -not $_.directory -and $_.relative -cne 'manifest.json' })
    if ($actual.Count -ne $expected.Count -or @($actual | Where-Object {-not $expected.ContainsKey($_.relative)}).Count -gt 0) {
        throw 'ASWN1003 notice bundle contains missing or unreferenced files'
    }
    return $manifest
}

function Export-AvidScriptWasmtimeNotices {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ArtifactLog,
        [Parameter(Mandatory)][string]$SourceRoot,
        [Parameter(Mandatory)][string]$CargoHome,
        [Parameter(Mandatory)][string]$RustSysroot,
        [Parameter(Mandatory)][string]$StagingRoot,
        [Parameter(Mandatory)]$Lock
    )
    $ErrorActionPreference = 'Stop'
    Initialize-AvidScriptBinaryPrivacy
    $bundleRoot = Join-Path $StagingRoot 'notices'
    Assert-AvidScriptBinaryPrivacyAncestors -Path $bundleRoot
    foreach ($inputRoot in @($SourceRoot, $CargoHome, $RustSysroot)) {
        $inputPrefix = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($inputRoot)) + '\'
        if (([IO.Path]::GetFullPath($bundleRoot) + '\').StartsWith($inputPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'ASWN1001 notice destination must remain outside input roots'
        }
    }
    if (Test-Path -LiteralPath $bundleRoot) { throw 'ASWN1001 notice destination already exists' }
    $messages = [Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $ArtifactLog) {
        if ($line.TrimStart().StartsWith('{')) { $messages.Add(($line.Trim() | ConvertFrom-Json -Depth 30)) }
    }
    $finished = @($messages | Where-Object reason -eq 'build-finished')
    $artifacts = @($messages | Where-Object reason -eq 'compiler-artifact')
    if ($finished.Count -ne 1 -or -not $finished[0].success -or $messages[-1].reason -cne 'build-finished' -or
        $artifacts.Count -eq 0 -or $artifacts.Count -gt 4096) { throw 'ASWN1004 incomplete Cargo build messages' }
    $roots = @($artifacts | Where-Object { $_.target.name -ceq 'wasmtime' -and $_.target.crate_types -contains 'cdylib' })
    if ($roots.Count -ne 1 -or $roots[0].profile.opt_level -cne '3' -or $roots[0].profile.debuginfo -ne 0 -or
        $roots[0].profile.debug_assertions -or $roots[0].profile.test -or
        (@($roots[0].features | Sort-Object) -join ',') -cne (@($Lock.rust.features | Sort-Object) -join ',')) {
        throw 'ASWN1004 Cargo runtime profile or features differ from lock'
    }
    $runtime = [ordered]@{}
    foreach ($name in @('wasmtime.dll', 'wasmtime.lib', 'wasmtime.dll.lib')) {
        $paths = @($roots[0].filenames | Where-Object { [IO.Path]::GetFileName($_) -ceq $name })
        if ($paths.Count -ne 1) { throw 'ASWN1004 missing or duplicate runtime artifact' }
        $builtHash = (Get-FileHash -LiteralPath $paths[0]).Hash.ToLowerInvariant()
        $stagedHash = (Get-FileHash -LiteralPath (Join-Path $StagingRoot "lib/$name")).Hash.ToLowerInvariant()
        if ($builtHash -cne $stagedHash) { throw 'ASWN1004 staged runtime differs from Cargo build' }
        $runtime[$name] = $stagedHash
    }
    $groups = @($artifacts | Group-Object package_id)
    if ($groups.Count -gt 512) { throw 'ASWN1004 too many built packages' }
    $graph = & cargo tree --manifest-path (Join-Path $SourceRoot 'Cargo.toml') --package $Lock.rust.package `
        --target x86_64-pc-windows-msvc --no-default-features --features (@($Lock.rust.features) -join ',') `
        --edges normal,build --prefix none --format '{p}' --locked --offline
    if ($LASTEXITCODE -ne 0) { throw 'ASWN1005 locked Cargo dependency graph failed' }
    $graphKeys = @($graph | ForEach-Object {
        if ($_ -notmatch '^([A-Za-z0-9_-]+) v([^\s]+)(?: |$)') { throw 'ASWN1005 unrecognized Cargo graph entry' }
        "$($Matches[1])@$($Matches[2])"
    } | Sort-Object -Unique)
    $metadataByPath = @{}
    $packages = [Collections.Generic.List[object]]::new()
    [void][IO.Directory]::CreateDirectory((Join-Path $bundleRoot 'texts'))
    foreach ($group in $groups) {
        $manifests = @($group.Group.manifest_path | ForEach-Object {[IO.Path]::GetFullPath($_)} | Sort-Object -Unique)
        if ($manifests.Count -ne 1) { throw 'ASWN1004 package has inconsistent manifests' }
        $manifestPath = $manifests[0]
        $packageRoot = Split-Path -Parent $manifestPath
        $isWorkspace = $group.Name.StartsWith('path+file:', [StringComparison]::Ordinal)
        $originRoot = if ($isWorkspace) { $SourceRoot } else { $packageRoot }
        [void](Get-AvidNoticeRelativePath $(if ($isWorkspace) {$SourceRoot} else {$CargoHome}) $manifestPath)
        if (-not $metadataByPath.ContainsKey($manifestPath)) {
            $raw = & cargo metadata --no-deps --format-version 1 --offline --manifest-path $manifestPath
            if ($LASTEXITCODE -ne 0) { throw 'ASWN1005 Cargo package metadata failed' }
            foreach ($package in (($raw -join "`n") | ConvertFrom-Json -Depth 64).packages) {
                $metadataByPath[[IO.Path]::GetFullPath($package.manifest_path)] = $package
            }
        }
        if (-not $metadataByPath.ContainsKey($manifestPath)) { throw 'ASWN1005 metadata lacks exact manifest' }
        $package = $metadataByPath[$manifestPath]
        if ([string]::IsNullOrWhiteSpace($package.license)) { throw 'ASWN1005 built package has no declared license' }
        $checksum = $null
        $source = [ordered]@{kind='wasmtime';identity=$Lock.upstream.commit;checksum=$null}
        if ($isWorkspace) {
            $id = 'wasmtime:' + (Get-AvidNoticeRelativePath $SourceRoot $packageRoot) + '#' + $package.name + '@' + $package.version
        } else {
            $id = 'crates-io:' + $package.name + '@' + $package.version
            if ($group.Name -cne "registry+https://github.com/rust-lang/crates.io-index#$($package.name)@$($package.version)") {
                throw 'ASWN1005 unsupported or inconsistent registry package identity'
            }
            $registryRelative = Get-AvidNoticeRelativePath $CargoHome $packageRoot
            $prefix = "$($package.name)-$($package.version)"
            if ($registryRelative -notmatch ('^registry/src/([^/]+)/' + [regex]::Escape($prefix) + '$')) {
                throw 'ASWN1002 registry source does not match cache layout'
            }
            $archivePath = Join-Path $CargoHome "registry/cache/$($Matches[1])/$prefix.crate"
            $checksum = Get-AvidNoticeArchiveChecksums $archivePath $prefix
            if (-not $checksum.files.ContainsKey('Cargo.toml') -or $checksum.files['Cargo.toml'] -cne (Get-FileHash -LiteralPath $manifestPath).Hash.ToLowerInvariant()) {
                throw 'ASWN1002 registry manifest checksum mismatch'
            }
            $source = [ordered]@{kind='crates-io';identity='https://crates.io/crates/' + $package.name + '/' + $package.version;checksum=$checksum.package}
        }
        $items = @(Get-ChildItem -LiteralPath $packageRoot -Recurse -Force)
        if (@($items | Where-Object {($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0}).Count -gt 0) {
            throw 'ASWN1001 linked package source'
        }
        $noticePaths = @($items | Where-Object {-not $_.PSIsContainer -and $_.Name -match '^(?i:licen[sc]e|copying|copyright|notice)(?:$|[._-])'} | ForEach-Object FullName)
        if ($isWorkspace) { $noticePaths += Join-Path $SourceRoot 'LICENSE' }
        if (-not [string]::IsNullOrWhiteSpace($package.license_file)) {
            $noticePaths += [IO.Path]::GetFullPath((Join-Path $packageRoot $package.license_file))
        }
        $noticePaths = @($noticePaths | Sort-Object -Unique)
        if ($noticePaths.Count -eq 0 -or $noticePaths.Count -gt 128) { throw 'ASWN1005 missing or excessive package notices' }
        $notices = @($noticePaths | ForEach-Object {
            Add-AvidNoticeText -Path $_ -OriginRoot $originRoot -BundleRoot $bundleRoot -Checksums $(if ($isWorkspace) {$null} else {$checksum.files})
        })
        $packages.Add([pscustomobject][ordered]@{
            id=$id;name=$package.name;version=$package.version;license_expression=$package.license
            source=$source;features=@($group.Group | ForEach-Object features | Sort-Object -Unique);notices=$notices
        })
    }
    $builtKeys = @($packages | ForEach-Object { "$($_.name)@$($_.version)" } | Sort-Object -Unique)
    if ($builtKeys.Count -ne $packages.Count -or $graphKeys.Count -ne $packages.Count -or
        @((Compare-Object $graphKeys $builtKeys)).Count -gt 0) {
        throw 'ASWN1004 artifact package set differs from locked dependency graph'
    }
    $rustNotice = Join-Path $RustSysroot 'share/doc/rust/COPYRIGHT-library.html'
    $rust = [ordered]@{toolchain=$Lock.rust.toolchain;notices=@(Add-AvidNoticeText $rustNotice $RustSysroot $bundleRoot)}
    $report = [ordered]@{
        schema_version=1;scope='win64-wasmtime-build-notices';package_count=$packages.Count
        toolchain_id=$Lock.toolchain_id;source_archive_sha256=$Lock.upstream.source_archive.sha256;patch_sha256=$Lock.patch.canonical_sha256
        cargo_lock_sha256=(Get-FileHash -LiteralPath (Join-Path $SourceRoot 'Cargo.lock')).Hash.ToLowerInvariant()
        artifact_log_sha256=(Get-FileHash -LiteralPath $ArtifactLog).Hash.ToLowerInvariant();runtime=$runtime
        packages=@($packages | Sort-Object id);rust=$rust
    }
    [IO.File]::WriteAllText((Join-Path $bundleRoot 'manifest.json'), ($report | ConvertTo-Json -Depth 30) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    [void](Test-AvidScriptWasmtimeNotices -BundleRoot $bundleRoot)
    return [pscustomobject]@{package_count=$packages.Count;manifest_sha256=(Get-FileHash (Join-Path $bundleRoot 'manifest.json')).Hash.ToLowerInvariant()}
}
