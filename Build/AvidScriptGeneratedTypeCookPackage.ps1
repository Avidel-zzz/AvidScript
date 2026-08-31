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

function Publish-AvidScriptGeneratedTypeCookPackage {
    param(
        [Parameter(Mandatory = $true)][string]$PackageDescriptorPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$OutputRoot
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
        [string]$Descriptor.execution_backend -cne 'wasmtime_jit' -or
        $null -eq $Descriptor.type_manifest -or
        $null -eq $Descriptor.runtime_manifest -or
        $null -eq $Descriptor.reload -or
        [string]$Descriptor.reload.native_structure_sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw "Generated type package descriptor is invalid for Cook publication."
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

    $RuntimeManifest = Get-Content -Raw -LiteralPath $RuntimeManifestPath | ConvertFrom-Json -Depth 64
    if ($null -eq $RuntimeManifest.wasm -or
        $null -eq $RuntimeManifest.debug_map -or
        $null -eq $RuntimeManifest.binding_package) {
        throw "Generated Runtime manifest is missing Cook-required artifact entries."
    }
    $WasmPath = Resolve-AvidScriptCookPackageArtifactPath `
        -ManifestPath $RuntimeManifestPath `
        -ArtifactPath ([string]$RuntimeManifest.wasm.file) `
        -ProjectRoot $ProjectRoot
    $DebugMapPath = Resolve-AvidScriptCookPackageArtifactPath `
        -ManifestPath $RuntimeManifestPath `
        -ArtifactPath ([string]$RuntimeManifest.debug_map.file) `
        -ProjectRoot $ProjectRoot
    $BindingManifestPath = Resolve-AvidScriptCookPackageArtifactPath `
        -ManifestPath $RuntimeManifestPath `
        -ArtifactPath ([string]$RuntimeManifest.binding_package.manifest_file) `
        -ProjectRoot $ProjectRoot
    $BindingDescriptorPath = Resolve-AvidScriptCookPackageArtifactPath `
        -ManifestPath $RuntimeManifestPath `
        -ArtifactPath ([string]$RuntimeManifest.binding_package.descriptor_file) `
        -ProjectRoot $ProjectRoot
    Assert-AvidScriptCookPackageFileHash -Path $WasmPath -ExpectedSha256 ([string]$RuntimeManifest.wasm.sha256) -Label 'WASM'
    Assert-AvidScriptCookPackageFileHash -Path $DebugMapPath -ExpectedSha256 ([string]$RuntimeManifest.debug_map.sha256) -Label 'debug map'
    Assert-AvidScriptCookPackageFileHash -Path $BindingManifestPath -ExpectedSha256 ([string]$RuntimeManifest.binding_package.manifest_sha256) -Label 'binding package manifest'
    Assert-AvidScriptCookPackageFileHash -Path $BindingDescriptorPath -ExpectedSha256 ([string]$RuntimeManifest.binding_package.descriptor_sha256) -Label 'binding descriptor'

    $BindingManifest = Get-Content -Raw -LiteralPath $BindingManifestPath | ConvertFrom-Json -Depth 64
    if ($null -eq $BindingManifest.files -or
        [string]$BindingManifest.descriptor_sha256 -cne [string]$RuntimeManifest.binding_package.descriptor_sha256) {
        throw 'Binding package manifest is invalid for Cook publication.'
    }
    $BindingManifest.files.descriptor = 'bindings.json'
    if ($BindingManifest.files.PSObject.Properties.Name -contains 'reference_source') {
        $BindingManifest.files.reference_source = ''
    }
    $Utf8 = [System.Text.UTF8Encoding]::new($false)
    $BindingManifestJson = ($BindingManifest | ConvertTo-Json -Depth 64) + [System.Environment]::NewLine
    $BindingManifestBytes = $Utf8.GetBytes($BindingManifestJson)
    $BindingManifestSha256 = [System.Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($BindingManifestBytes)).ToLowerInvariant()

    $RuntimeManifest.wasm.file = 'generated_types.wasm'
    $RuntimeManifest.debug_map.file = 'generated_types.debug.json'
    $RuntimeManifest.binding_package.manifest_file = 'bindings/package.json'
    $RuntimeManifest.binding_package.manifest_sha256 = $BindingManifestSha256
    $RuntimeManifest.binding_package.descriptor_file = 'bindings/bindings.json'
    if ($null -ne $RuntimeManifest.source) {
        if ($RuntimeManifest.source.PSObject.Properties.Name -contains 'frontend_file') {
            $RuntimeManifest.source.frontend_file = ''
        }
        if ($RuntimeManifest.source.PSObject.Properties.Name -contains 'semantic_file') {
            $RuntimeManifest.source.semantic_file = ''
        }
    }
    if ($RuntimeManifest.binding_package.PSObject.Properties.Name -contains 'reference_source_file') {
        $RuntimeManifest.binding_package.reference_source_file = ''
    }
    if ($null -ne $RuntimeManifest.guest_ir -and
        $RuntimeManifest.guest_ir.PSObject.Properties.Name -contains 'file') {
        $RuntimeManifest.guest_ir.file = ''
    }
    $RuntimeJson = ($RuntimeManifest | ConvertTo-Json -Depth 64) + [System.Environment]::NewLine
    $RuntimeBytes = $Utf8.GetBytes($RuntimeJson)
    $RuntimeSha256 = [System.Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($RuntimeBytes)).ToLowerInvariant()
    $TypeSha256 = Get-AvidScriptCookPackageSha256 $TypeManifestPath
    $PackageIdentityBytes = [System.Text.Encoding]::UTF8.GetBytes(
        "$($Descriptor.generation_key_sha256)`n$TypeSha256`n$RuntimeSha256")
    $PackageId = [System.Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($PackageIdentityBytes)).ToLowerInvariant()

    [void][System.IO.Directory]::CreateDirectory($OutputRoot)
    $BundleRoot = [System.IO.Path]::GetFullPath((Join-Path $OutputRoot $PackageId))
    if (-not $BundleRoot.StartsWith(
            $OutputRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Cook package bundle escaped its output root."
    }
    if (-not (Test-Path -LiteralPath $BundleRoot -PathType Container)) {
        $TempBundleRoot = "$BundleRoot.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
        try {
            [void][System.IO.Directory]::CreateDirectory((Join-Path $TempBundleRoot 'bindings'))
            Copy-Item -LiteralPath $TypeManifestPath -Destination (Join-Path $TempBundleRoot 'type-manifest.json')
            [System.IO.File]::WriteAllBytes((Join-Path $TempBundleRoot 'runtime-manifest.json'), $RuntimeBytes)
            Copy-Item -LiteralPath $WasmPath -Destination (Join-Path $TempBundleRoot 'generated_types.wasm')
            Copy-Item -LiteralPath $DebugMapPath -Destination (Join-Path $TempBundleRoot 'generated_types.debug.json')
            [System.IO.File]::WriteAllBytes((Join-Path $TempBundleRoot 'bindings\package.json'), $BindingManifestBytes)
            Copy-Item -LiteralPath $BindingDescriptorPath -Destination (Join-Path $TempBundleRoot 'bindings\bindings.json')
            Move-Item -LiteralPath $TempBundleRoot -Destination $BundleRoot
        }
        finally {
            if (Test-Path -LiteralPath $TempBundleRoot -PathType Container) {
                Remove-Item -LiteralPath $TempBundleRoot -Recurse -Force
            }
        }
    }

    $ExpectedBundleFiles = [ordered]@{
        'type-manifest.json' = $TypeSha256
        'runtime-manifest.json' = $RuntimeSha256
        'generated_types.wasm' = [string]$RuntimeManifest.wasm.sha256
        'generated_types.debug.json' = [string]$RuntimeManifest.debug_map.sha256
        'bindings/package.json' = $BindingManifestSha256
        'bindings/bindings.json' = [string]$RuntimeManifest.binding_package.descriptor_sha256
    }
    foreach ($Entry in $ExpectedBundleFiles.GetEnumerator()) {
        $BundleFile = Join-Path $BundleRoot $Entry.Key
        if (-not (Test-Path -LiteralPath $BundleFile -PathType Leaf) -or
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
        schema_version = 1
        package_id = $PackageId
        module_name = [string]$Descriptor.module_name
        runtime_module_id = [string]$Descriptor.runtime_module_id
        execution_backend = [string]$Descriptor.execution_backend
        generation_key_sha256 = [string]$Descriptor.generation_key_sha256
        type_manifest = [ordered]@{
            file = "$PackageId/type-manifest.json"
            sha256 = $TypeSha256
        }
        runtime_manifest = [ordered]@{
            file = "$PackageId/runtime-manifest.json"
            sha256 = $RuntimeSha256
        }
        reload = $ReloadMetadata
    }
    $CurrentPath = Join-Path $OutputRoot 'current.json'
    $CurrentTempPath = "$CurrentPath.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
    [System.IO.File]::WriteAllText(
        $CurrentTempPath,
        ($CookDescriptor | ConvertTo-Json -Depth 16) + [System.Environment]::NewLine,
        $Utf8)
    [System.IO.File]::Move($CurrentTempPath, $CurrentPath, $true)

    return [pscustomobject][ordered]@{
        PackageId = $PackageId
        DescriptorPath = $CurrentPath
        BundleRoot = $BundleRoot
        RuntimeManifestSha256 = $RuntimeSha256
        FileCount = $ExpectedBundleFiles.Count + 1
    }
}
