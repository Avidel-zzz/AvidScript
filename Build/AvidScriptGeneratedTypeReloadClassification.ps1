function Test-AvidScriptSha256 {
    param([AllowEmptyString()][string]$Value)

    return -not [string]::IsNullOrWhiteSpace($Value) -and
        $Value -cmatch '^[0-9a-f]{64}$'
}

function Get-AvidScriptGeneratedTypeNativeStructureSha256 {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)]$Manifest)

    $SchemaVersion = [int]$Manifest.schema_version
    $GeneratorVersion = [string]$Manifest.generator_version
    $ModuleName = [string]$Manifest.module_name
    $UnrealVersion = [string]$Manifest.unreal_version
    $Outputs = @($Manifest.outputs)
    if ($SchemaVersion -le 0 -or
        [string]::IsNullOrWhiteSpace($GeneratorVersion) -or
        [string]::IsNullOrWhiteSpace($ModuleName) -or
        [string]::IsNullOrWhiteSpace($UnrealVersion) -or
        $Outputs.Count -eq 0) {
        throw "Generated type manifest cannot produce a native structure identity."
    }

    $IdentityLines = [System.Collections.Generic.List[string]]::new()
    $IdentityLines.Add("avidscript-generated-native-structure-v1")
    $IdentityLines.Add("schema=$SchemaVersion")
    $IdentityLines.Add("generator=$GeneratorVersion")
    $IdentityLines.Add("module=$ModuleName")
    $IdentityLines.Add("unreal=$UnrealVersion")
    $SeenPaths = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Output in @($Outputs | Sort-Object -Property relative_path)) {
        $RelativePath = ([string]$Output.relative_path).Replace('\', '/')
        $Sha256 = [string]$Output.sha256
        $Length = [long]$Output.length
        if ([string]::IsNullOrWhiteSpace($RelativePath) -or
            [System.IO.Path]::IsPathRooted($RelativePath) -or
            $RelativePath.Split('/') -contains '..' -or
            -not (Test-AvidScriptSha256 $Sha256) -or
            $Length -lt 0 -or
            -not $SeenPaths.Add($RelativePath)) {
            throw "Generated type manifest contains an invalid native output identity: $RelativePath"
        }
        $IdentityLines.Add("output=$RelativePath|$Sha256|$Length")
    }

    $IdentityBytes = [System.Text.Encoding]::UTF8.GetBytes(
        [string]::Join("`n", $IdentityLines) + "`n")
    return [System.Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($IdentityBytes)).ToLowerInvariant()
}

function Get-AvidScriptGeneratedTypeReloadBaseline {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$DescriptorPath,
        [Parameter(Mandatory = $true)][string]$OutputRoot
    )

    if (-not (Test-Path -LiteralPath $DescriptorPath -PathType Leaf)) {
        return $null
    }

    try {
        $Descriptor = Get-Content -Raw -LiteralPath $DescriptorPath | ConvertFrom-Json
        $PackageId = [string]$Descriptor.package_id
        if ([int]$Descriptor.schema_version -ne 1 -or
            -not (Test-AvidScriptSha256 $PackageId)) {
            return $null
        }

        if ($null -ne $Descriptor.reload -and
            [int]$Descriptor.reload.schema_version -eq 1 -and
            (Test-AvidScriptSha256 ([string]$Descriptor.reload.native_structure_sha256))) {
            return [pscustomobject]@{
                PackageId = $PackageId
                NativeStructureSha256 = [string]$Descriptor.reload.native_structure_sha256
            }
        }

        $TypeManifestRelativePath = [string]$Descriptor.type_manifest.file
        $ExpectedTypeManifestSha256 = [string]$Descriptor.type_manifest.sha256
        if ([string]::IsNullOrWhiteSpace($TypeManifestRelativePath) -or
            [System.IO.Path]::IsPathRooted($TypeManifestRelativePath) -or
            $TypeManifestRelativePath.Replace('\', '/').Split('/') -contains '..' -or
            -not (Test-AvidScriptSha256 $ExpectedTypeManifestSha256)) {
            return $null
        }

        $NormalizedOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
        $TypeManifestPath = [System.IO.Path]::GetFullPath(
            (Join-Path $NormalizedOutputRoot $TypeManifestRelativePath))
        if (-not $TypeManifestPath.StartsWith(
                $NormalizedOutputRoot + [System.IO.Path]::DirectorySeparatorChar,
                [System.StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $TypeManifestPath -PathType Leaf) -or
            (Get-FileHash -LiteralPath $TypeManifestPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne
                $ExpectedTypeManifestSha256) {
            return $null
        }

        $Manifest = Get-Content -Raw -LiteralPath $TypeManifestPath | ConvertFrom-Json
        return [pscustomobject]@{
            PackageId = $PackageId
            NativeStructureSha256 = Get-AvidScriptGeneratedTypeNativeStructureSha256 $Manifest
        }
    }
    catch {
        return $null
    }
}

function New-AvidScriptGeneratedTypeReloadMetadata {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$NativeStructureSha256,
        [AllowEmptyString()][string]$PreviousPackageId = "",
        [AllowEmptyString()][string]$PreviousNativeStructureSha256 = ""
    )

    if (-not (Test-AvidScriptSha256 $NativeStructureSha256)) {
        throw "Current generated type native structure identity is invalid."
    }
    $HasPreviousPackage = -not [string]::IsNullOrWhiteSpace($PreviousPackageId)
    $HasPreviousStructure = -not [string]::IsNullOrWhiteSpace($PreviousNativeStructureSha256)
    if ($HasPreviousPackage -ne $HasPreviousStructure -or
        ($HasPreviousPackage -and
            (-not (Test-AvidScriptSha256 $PreviousPackageId) -or
             -not (Test-AvidScriptSha256 $PreviousNativeStructureSha256)))) {
        throw "Previous generated type package identity is incomplete or invalid."
    }

    $Classification = if (-not $HasPreviousPackage) {
        "initial_install"
    }
    elseif ($NativeStructureSha256 -ceq $PreviousNativeStructureSha256) {
        "body_only"
    }
    else {
        "native_rebuild_required"
    }

    return [ordered]@{
        schema_version = 1
        classification = $Classification
        native_structure_sha256 = $NativeStructureSha256
        previous_native_structure_sha256 = if ($HasPreviousStructure) {
            $PreviousNativeStructureSha256
        } else {
            ""
        }
        previous_package_id = if ($HasPreviousPackage) { $PreviousPackageId } else { "" }
    }
}
