function Get-AvidScriptCookPackageSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-AvidScriptCookPackagePathUnderRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $ResolvedPath = [System.IO.Path]::GetFullPath($Path)
    $ResolvedRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    return $ResolvedPath.Equals($ResolvedRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
        $ResolvedPath.StartsWith(
            $ResolvedRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)
}

function Resolve-AvidScriptCookPackageArtifactPath {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$ArtifactPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot
    )

    if ([string]::IsNullOrWhiteSpace($ArtifactPath)) {
        throw "Cook package artifact path must not be empty."
    }
    if ([System.IO.Path]::IsPathRooted($ArtifactPath)) {
        $Candidates = @([System.IO.Path]::GetFullPath($ArtifactPath))
    }
    else {
        $Normalized = $ArtifactPath.Replace('\', '/')
        $ManifestCandidate = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $ManifestPath) $ArtifactPath))
        $ProjectCandidate = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $ArtifactPath))
        $LooksProjectRelative = $Normalized.StartsWith('Saved/', [System.StringComparison]::OrdinalIgnoreCase) -or
            $Normalized.StartsWith('Content/', [System.StringComparison]::OrdinalIgnoreCase) -or
            $Normalized.StartsWith('Plugins/', [System.StringComparison]::OrdinalIgnoreCase)
        $Candidates = if ($LooksProjectRelative) {
            @($ProjectCandidate, $ManifestCandidate)
        }
        else {
            @($ManifestCandidate, $ProjectCandidate)
        }
    }
    foreach ($Candidate in $Candidates) {
        if ((Test-AvidScriptCookPackagePathUnderRoot -Path $Candidate -Root $ProjectRoot) -and
            (Test-Path -LiteralPath $Candidate -PathType Leaf)) {
            return $Candidate
        }
    }
    throw "Cook package artifact is missing or outside the project root: $ArtifactPath"
}

function Assert-AvidScriptCookPackageFileHash {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($ExpectedSha256 -cnotmatch '^[0-9a-f]{64}$' -or
        (Get-AvidScriptCookPackageSha256 $Path) -cne $ExpectedSha256) {
        throw "$Label SHA-256 does not match its manifest."
    }
}

$AvidScriptModuleReleasePublisherPath = Join-Path `
    $PSScriptRoot `
    'AvidScriptModuleReleasePackage.ps1'
if (-not (Test-Path -LiteralPath $AvidScriptModuleReleasePublisherPath -PathType Leaf)) {
    throw "AvidScript module release publisher is missing: $AvidScriptModuleReleasePublisherPath"
}
. $AvidScriptModuleReleasePublisherPath

function Publish-AvidScriptGeneratedTypeCookPackage {
    param(
        [Parameter(Mandatory = $true)][string]$PackageDescriptorPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [ValidateSet('Development', 'Shipping')][string]$Configuration = 'Development'
    )

    $PackageDescriptorPath = (Resolve-Path -LiteralPath $PackageDescriptorPath).Path
    $ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
    $OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
    if (-not (Test-AvidScriptCookPackagePathUnderRoot -Path $PackageDescriptorPath -Root $ProjectRoot) -or
        -not (Test-AvidScriptCookPackagePathUnderRoot -Path $OutputRoot -Root $ProjectRoot)) {
        throw "Cook package descriptor and output root must remain inside the project root."
    }
    $Descriptor = Get-Content -Raw -LiteralPath $PackageDescriptorPath | ConvertFrom-Json -Depth 32
    if ([int]$Descriptor.schema_version -ne 1 -or
        [string]$Descriptor.generation_key_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        [string]::IsNullOrWhiteSpace([string]$Descriptor.module_name) -or
        [string]::IsNullOrWhiteSpace([string]$Descriptor.runtime_module_id) -or
        [string]$Descriptor.execution_backend -cnotin @('wasmtime_jit', 'wasmtime_precompiled') -or
        $null -eq $Descriptor.type_manifest -or
        $null -eq $Descriptor.runtime_manifest -or
        $null -eq $Descriptor.reload -or
        [string]$Descriptor.reload.native_structure_sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw "Generated type package descriptor is invalid for Cook publication."
    }
    if ($Configuration -ceq 'Shipping' -and
        [string]$Descriptor.execution_backend -cne 'wasmtime_precompiled') {
        throw "Shipping Generated Type packages require a precompiled Runtime module."
    }

    $TypeManifestPath = Resolve-AvidScriptCookPackageArtifactPath `
        -ManifestPath $PackageDescriptorPath `
        -ArtifactPath ([string]$Descriptor.type_manifest.file) `
        -ProjectRoot $ProjectRoot
    $RuntimeManifestPath = Resolve-AvidScriptCookPackageArtifactPath `
        -ManifestPath $PackageDescriptorPath `
        -ArtifactPath ([string]$Descriptor.runtime_manifest.file) `
        -ProjectRoot $ProjectRoot
    Assert-AvidScriptCookPackageFileHash `
        -Path $TypeManifestPath `
        -ExpectedSha256 ([string]$Descriptor.type_manifest.sha256) `
        -Label 'generated type manifest'
    Assert-AvidScriptCookPackageFileHash `
        -Path $RuntimeManifestPath `
        -ExpectedSha256 ([string]$Descriptor.runtime_manifest.sha256) `
        -Label 'generated Runtime manifest'
    $TypeSha256 = Get-AvidScriptCookPackageSha256 $TypeManifestPath
    $RuntimePackage = Publish-AvidScriptModuleReleasePackage `
        -RuntimeManifestPath $RuntimeManifestPath `
        -ProjectRoot $ProjectRoot `
        -ModuleId ([string]$Descriptor.runtime_module_id) `
        -Configuration $Configuration
    $GeneratedPackageIdentityBytes = [System.Text.Encoding]::UTF8.GetBytes(
        "$($Descriptor.generation_key_sha256)`n$TypeSha256`n$($RuntimePackage.ModuleId)`n$($RuntimePackage.PackageId)")
    $GeneratedPackageId = [System.Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData(
            $GeneratedPackageIdentityBytes)).ToLowerInvariant()

    [void][System.IO.Directory]::CreateDirectory($OutputRoot)
    $BundleRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $OutputRoot $GeneratedPackageId))
    if (-not $BundleRoot.StartsWith(
            $OutputRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Cook package bundle escaped its output root."
    }
    if (-not (Test-Path -LiteralPath $BundleRoot -PathType Container)) {
        $TempBundleRoot = "$BundleRoot.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
        try {
            [void][System.IO.Directory]::CreateDirectory($TempBundleRoot)
            Copy-Item -LiteralPath $TypeManifestPath -Destination (Join-Path $TempBundleRoot 'type-manifest.json')
            Move-Item -LiteralPath $TempBundleRoot -Destination $BundleRoot
        }
        finally {
            if (Test-Path -LiteralPath $TempBundleRoot -PathType Container) {
                Remove-Item -LiteralPath $TempBundleRoot -Recurse -Force
            }
        }
    }

    $ExpectedBundleFiles = [ordered]@{ 'type-manifest.json' = $TypeSha256 }
    $ActualBundleFiles = @(
        Get-ChildItem -LiteralPath $BundleRoot -File -Recurse |
            ForEach-Object {
                [System.IO.Path]::GetRelativePath(
                    $BundleRoot,
                    $_.FullName).Replace('\', '/')
            })
    if ($ActualBundleFiles.Count -ne $ExpectedBundleFiles.Count) {
        throw 'Content-addressed Generated Type bundle contains extra files.'
    }
    foreach ($Entry in $ExpectedBundleFiles.GetEnumerator()) {
        $BundleFile = Join-Path $BundleRoot $Entry.Key
        if ($ActualBundleFiles -cnotcontains $Entry.Key -or
            (Get-AvidScriptCookPackageSha256 $BundleFile) -cne $Entry.Value) {
            throw "Content-addressed Cook bundle is incomplete or collided: $($Entry.Key)"
        }
    }

    $ReloadMetadata = [ordered]@{
        schema_version = 1
        classification = 'initial_install'
        native_structure_sha256 = [string]$Descriptor.reload.native_structure_sha256
        previous_native_structure_sha256 = ''
        previous_package_id = ''
    }
    $CookDescriptor = [ordered]@{
        schema_version = 2
        module_name = [string]$Descriptor.module_name
        module_id = [string]$RuntimePackage.ModuleId
        package_id = [string]$RuntimePackage.PackageId
        generation_key_sha256 = [string]$Descriptor.generation_key_sha256
        type_manifest = [ordered]@{
            file = "$GeneratedPackageId/type-manifest.json"
            sha256 = $TypeSha256
        }
        reload = $ReloadMetadata
    }
    $Utf8 = [System.Text.UTF8Encoding]::new($false)
    $CurrentPath = Join-Path $OutputRoot 'current.json'
    $CurrentTempPath = "$CurrentPath.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
    [System.IO.File]::WriteAllText(
        $CurrentTempPath,
        ($CookDescriptor | ConvertTo-Json -Depth 16) + [System.Environment]::NewLine,
        $Utf8)
    [System.IO.File]::Move($CurrentTempPath, $CurrentPath, $true)

    return [pscustomobject][ordered]@{
        ModuleId = [string]$RuntimePackage.ModuleId
        PackageId = [string]$RuntimePackage.PackageId
        GeneratedTypePackageId = $GeneratedPackageId
        DescriptorPath = $CurrentPath
        BundleRoot = $BundleRoot
        RuntimePackageRoot = [string]$RuntimePackage.PackageRoot
        RuntimeDescriptorPath = [string]$RuntimePackage.DescriptorPath
        RuntimeManifestSha256 = Get-AvidScriptCookPackageSha256 (
            Join-Path $RuntimePackage.PackageRoot 'runtime.avidscript.json')
        FileCount = $ExpectedBundleFiles.Count + 1
        RuntimeFileCount = [int]$RuntimePackage.FileCount
    }
}
