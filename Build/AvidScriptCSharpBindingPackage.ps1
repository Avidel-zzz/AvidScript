function Get-AvidScriptBindingSha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }

    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        $Sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $HashBytes = $Sha256.ComputeHash($Stream)
        }
        finally {
            $Sha256.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
    }

    return [System.BitConverter]::ToString($HashBytes).Replace("-", "").ToLowerInvariant()
}

function Test-AvidScriptBindingSha256 {
    param([string]$Value)

    return -not [string]::IsNullOrWhiteSpace($Value) -and
        $Value -cmatch "^[0-9a-f]{64}$"
}

function Resolve-AvidScriptBindingPackageFile {
    param(
        [Parameter(Mandatory = $true)][string]$PackageDirectory,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$FieldName
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [System.IO.Path]::IsPathRooted($RelativePath)) {
        throw "$FieldName must be a non-empty package-relative path."
    }

    $Directory = [System.IO.Path]::GetFullPath($PackageDirectory).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $Candidate = [System.IO.Path]::GetFullPath((Join-Path $Directory $RelativePath))
    $ContainedPrefix = $Directory + [System.IO.Path]::DirectorySeparatorChar
    if (-not $Candidate.StartsWith(
        $ContainedPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$FieldName escapes the binding package directory."
    }
    if (-not (Test-Path -LiteralPath $Candidate -PathType Leaf)) {
        throw "$FieldName file is missing: $Candidate"
    }

    return $Candidate
}

function Resolve-AvidScriptCSharpBindingPackage {
    param([Parameter(Mandatory = $true)][string]$ManifestPath)

    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
        throw "Binding package manifest is missing: $ManifestPath"
    }

    $ManifestFullPath = (Resolve-Path -LiteralPath $ManifestPath).Path
    try {
        $Manifest = Get-Content -Raw -LiteralPath $ManifestFullPath | ConvertFrom-Json
    }
    catch {
        throw "Binding package manifest JSON is invalid: $($_.Exception.Message)"
    }

    if ([int]$Manifest.schema_version -ne 1) {
        throw "Binding package schema_version must be 1."
    }
    $PackageName = [string]$Manifest.package_name
    $PackageHash = [string]$Manifest.package_hash
    $DescriptorHash = [string]$Manifest.descriptor_sha256
    $ReferenceSourceHash = [string]$Manifest.reference_source_sha256
    if ([string]::IsNullOrWhiteSpace($PackageName) -or
        -not (Test-AvidScriptBindingSha256 $PackageHash) -or
        -not (Test-AvidScriptBindingSha256 $DescriptorHash) -or
        -not (Test-AvidScriptBindingSha256 $ReferenceSourceHash)) {
        throw "Binding package identity or SHA-256 fields are invalid."
    }
    if ($null -eq $Manifest.files) {
        throw "Binding package files object is missing."
    }

    $PackageDirectory = Split-Path -Parent $ManifestFullPath
    $DescriptorPath = Resolve-AvidScriptBindingPackageFile `
        -PackageDirectory $PackageDirectory `
        -RelativePath ([string]$Manifest.files.descriptor) `
        -FieldName "files.descriptor"
    $ReferenceSourcePath = Resolve-AvidScriptBindingPackageFile `
        -PackageDirectory $PackageDirectory `
        -RelativePath ([string]$Manifest.files.reference_source) `
        -FieldName "files.reference_source"

    $ActualDescriptorHash = Get-AvidScriptBindingSha256Hex $DescriptorPath
    $ActualReferenceSourceHash = Get-AvidScriptBindingSha256Hex $ReferenceSourcePath
    if ($ActualDescriptorHash -cne $DescriptorHash) {
        throw "Binding descriptor SHA-256 does not match package.json."
    }
    if ($ActualReferenceSourceHash -cne $ReferenceSourceHash) {
        throw "Generated C# reference source SHA-256 does not match package.json."
    }

    try {
        $Descriptor = Get-Content -Raw -LiteralPath $DescriptorPath | ConvertFrom-Json
    }
    catch {
        throw "Binding descriptor JSON is invalid: $($_.Exception.Message)"
    }
    if ([int]$Descriptor.schema_version -ne 2 -or
        [string]$Descriptor.package_name -cne $PackageName -or
        [string]$Descriptor.package_hash -cne $PackageHash) {
        throw "Binding descriptor identity does not match package.json."
    }

    $RequiredImports = @()
    $ImportKeys = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Import in @($Manifest.required_imports)) {
        $StableId = [string]$Import.stable_id
        $Ordinal = [int]$Import.ordinal
        $Module = [string]$Import.module
        $Name = [string]$Import.name
        $Signature = [string]$Import.signature
        if (-not (Test-AvidScriptBindingSha256 $StableId) -or
            $null -eq $Import.ordinal -or
            $Ordinal -lt 0 -or
            [string]::IsNullOrWhiteSpace($Module) -or
            [string]::IsNullOrWhiteSpace($Name) -or
            [string]::IsNullOrWhiteSpace($Signature)) {
            throw "Binding package required_imports contains invalid identity, ordinal, module, name, or signature data."
        }
        $Key = "$Module`n$Name"
        if (-not $ImportKeys.Add($Key)) {
            throw "Binding package required_imports contains duplicate import $Module.$Name."
        }
        $RequiredImports += [pscustomobject]@{
            StableId = $StableId
            Ordinal = $Ordinal
            Module = $Module
            Name = $Name
            Signature = $Signature
        }
    }
    if ($RequiredImports.Count -eq 0) {
        throw "Binding package required_imports must not be empty."
    }

    return [pscustomobject]@{
        ManifestPath = $ManifestFullPath
        ManifestSha256 = Get-AvidScriptBindingSha256Hex $ManifestFullPath
        PackageName = $PackageName
        PackageHash = $PackageHash
        DescriptorPath = $DescriptorPath
        DescriptorSha256 = $DescriptorHash
        ReferenceSourcePath = $ReferenceSourcePath
        ReferenceSourceSha256 = $ReferenceSourceHash
        RequiredImports = @($RequiredImports)
    }
}
