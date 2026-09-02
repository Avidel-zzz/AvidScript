[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ReceiptPath,
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][string]$PluginRoot,
    [Parameter(Mandatory = $true)]
    [ValidateSet('Development', 'Shipping')]
    [string]$Configuration
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Throw-ReceiptValidationFailure {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message
    )

    throw [System.InvalidOperationException]::new("$Code|$Message")
}

function Get-RequiredPropertyValue {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Property = $Value.PSObject.Properties[$Name]
    if ($null -eq $Property) {
        Throw-ReceiptValidationFailure 'JSON_SCHEMA_INVALID' "$Label is missing '$Name'."
    }
    return $Property.Value
}

function Assert-NoDuplicateJsonProperties {
    param(
        [Parameter(Mandatory = $true)][System.Text.Json.JsonElement]$Element,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $Names = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($Property in $Element.EnumerateObject()) {
            if (-not $Names.Add($Property.Name)) {
                Throw-ReceiptValidationFailure `
                    'JSON_DUPLICATE_PROPERTY' `
                    "$Label contains duplicate property '$($Property.Name)'."
            }
            Assert-NoDuplicateJsonProperties `
                -Element $Property.Value `
                -Label "$Label.$($Property.Name)"
        }
    }
    elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        $Index = 0
        foreach ($Item in $Element.EnumerateArray()) {
            Assert-NoDuplicateJsonProperties -Element $Item -Label "$Label[$Index]"
            ++$Index
        }
    }
}

function Read-StrictJsonObject {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Throw-ReceiptValidationFailure 'SOURCE_FILE_MISSING' "$Label is missing: $Path"
    }
    try {
        $Text = [System.IO.File]::ReadAllText($Path)
        $Document = [System.Text.Json.JsonDocument]::Parse($Text)
        try {
            if ($Document.RootElement.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) {
                Throw-ReceiptValidationFailure 'JSON_SCHEMA_INVALID' "$Label must be a JSON object."
            }
            Assert-NoDuplicateJsonProperties -Element $Document.RootElement -Label $Label
        }
        finally {
            $Document.Dispose()
        }
        return ($Text | ConvertFrom-Json -Depth 100)
    }
    catch {
        if ($_.Exception.Message -match '^[A-Z0-9_]+\|') {
            throw
        }
        Throw-ReceiptValidationFailure 'JSON_PARSE_FAILED' "$Label could not be parsed: $($_.Exception.Message)"
    }
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Throw-ReceiptValidationFailure 'SOURCE_FILE_MISSING' "Source file is missing: $Path"
    }
    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        $Hasher = [System.Security.Cryptography.SHA256]::Create()
        try {
            return [System.BitConverter]::ToString($Hasher.ComputeHash($Stream)).Replace('-', '').ToLowerInvariant()
        }
        finally {
            $Hasher.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
    }
}

function Assert-LowercaseSha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Value -cnotmatch '^[0-9a-f]{64}$') {
        Throw-ReceiptValidationFailure 'JSON_SCHEMA_INVALID' "$Label must be a lowercase SHA-256."
    }
}

function Resolve-ContainedSourceFile {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Normalized = $RelativePath.Replace('\', '/')
    $Segments = @($Normalized.Split('/'))
    if ([string]::IsNullOrWhiteSpace($Normalized) -or
        [System.IO.Path]::IsPathRooted($RelativePath) -or
        $Segments.Count -eq 0 -or
        $Segments -contains '' -or
        $Segments -contains '.' -or
        $Segments -contains '..') {
        Throw-ReceiptValidationFailure 'SOURCE_PATH_INVALID' "$Label path is not a canonical relative path: $RelativePath"
    }

    $FullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $FullPath = [System.IO.Path]::GetFullPath((Join-Path $FullRoot $RelativePath))
    $RootPrefix = $FullRoot + [System.IO.Path]::DirectorySeparatorChar
    if (-not $FullPath.StartsWith($RootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-ReceiptValidationFailure 'SOURCE_PATH_INVALID' "$Label escapes its source root: $RelativePath"
    }
    if (-not (Test-Path -LiteralPath $FullPath -PathType Leaf)) {
        Throw-ReceiptValidationFailure 'SOURCE_FILE_MISSING' "$Label is missing: $FullPath"
    }
    return $FullPath
}

function Add-ExpectedDependency {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.Dictionary[string, object]]$Dependencies,
        [Parameter(Mandatory = $true)][string]$TargetPath,
        [Parameter(Mandatory = $true)][ValidateSet('UFS', 'NonUFS')][string]$Type,
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [AllowEmptyString()][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$Owner
    )

    if ($Dependencies.ContainsKey($TargetPath)) {
        Throw-ReceiptValidationFailure 'EXPECTED_DEPENDENCY_COLLISION' "Expected dependency is duplicated: $TargetPath"
    }
    if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
        Throw-ReceiptValidationFailure 'SOURCE_FILE_MISSING' "$Owner source file is missing: $SourcePath"
    }

    $ActualSha256 = Get-FileSha256 -Path $SourcePath
    if (-not [string]::IsNullOrEmpty($ExpectedSha256)) {
        Assert-LowercaseSha256 -Value $ExpectedSha256 -Label "$Owner.sha256"
        if ($ActualSha256 -cne $ExpectedSha256) {
            Throw-ReceiptValidationFailure `
                'SOURCE_HASH_MISMATCH' `
                "$Owner hash drifted for '$SourcePath': expected $ExpectedSha256, actual $ActualSha256."
        }
    }

    $Dependencies.Add($TargetPath, [pscustomobject][ordered]@{
        target_path = $TargetPath
        type = $Type
        source_path = $SourcePath
        source_sha256 = $ActualSha256
        owner = $Owner
    })
}

function ConvertTo-CanonicalReceiptPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ResolvedProjectRoot,
        [Parameter(Mandatory = $true)][string]$PluginTargetRoot
    )

    $Normalized = $Path.Replace('\', '/')
    if ($Normalized -imatch '^\$\(ProjectDir\)(?:/(?<relative>.*))?$') {
        $Relative = $Matches.relative
        if ([string]::IsNullOrEmpty($Relative)) {
            return '$(ProjectDir)'
        }
        return "`$(ProjectDir)/$Relative"
    }
    if ($Normalized -imatch '^\$\(PluginDir\)(?:/(?<relative>.*))?$') {
        $Relative = $Matches.relative
        if ([string]::IsNullOrEmpty($Relative)) {
            return $PluginTargetRoot
        }
        return "$PluginTargetRoot/$Relative"
    }
    if ([System.IO.Path]::IsPathFullyQualified($Path)) {
        $FullPath = [System.IO.Path]::GetFullPath($Path)
        $ProjectPrefix = $ResolvedProjectRoot.TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
        if ($FullPath.StartsWith($ProjectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $Relative = [System.IO.Path]::GetRelativePath($ResolvedProjectRoot, $FullPath).Replace('\', '/')
            return "`$(ProjectDir)/$Relative"
        }
    }
    return $Normalized
}

function Test-IsAvidScriptDependencyPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$PluginTargetRoot
    )

    return $Path.StartsWith(
            '$(ProjectDir)/Content/AvidScript/',
            [System.StringComparison]::OrdinalIgnoreCase) -or
        $Path.Equals(
            '$(ProjectDir)/Content/AvidScript',
            [System.StringComparison]::OrdinalIgnoreCase) -or
        $Path.StartsWith(
            "$PluginTargetRoot/",
            [System.StringComparison]::OrdinalIgnoreCase) -or
        $Path.Equals($PluginTargetRoot, [System.StringComparison]::OrdinalIgnoreCase)
}

try {
    $ResolvedReceiptPath = [System.IO.Path]::GetFullPath($ReceiptPath)
    $ResolvedProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $ResolvedPluginRoot = [System.IO.Path]::GetFullPath($PluginRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $ResolvedProjectRoot -PathType Container)) {
        Throw-ReceiptValidationFailure 'PROJECT_ROOT_INVALID' "Project root is missing: $ResolvedProjectRoot"
    }
    if (-not (Test-Path -LiteralPath $ResolvedPluginRoot -PathType Container)) {
        Throw-ReceiptValidationFailure 'PLUGIN_ROOT_INVALID' "Plugin root is missing: $ResolvedPluginRoot"
    }

    $PluginRelativePath = [System.IO.Path]::GetRelativePath(
        $ResolvedProjectRoot,
        $ResolvedPluginRoot).Replace('\', '/')
    if (-not $PluginRelativePath.Equals(
            'Plugins/AvidScript',
            [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-ReceiptValidationFailure `
            'PLUGIN_ROOT_INVALID' `
            "PluginRoot must resolve to ProjectRoot/Plugins/AvidScript, got '$PluginRelativePath'."
    }
    $PluginTargetRoot = '$(ProjectDir)/Plugins/AvidScript'
    $ExpectedConfiguration = $Configuration.ToLowerInvariant()
    $ExpectedDependencies = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)

    $PluginDescriptorPath = Join-Path $ResolvedPluginRoot 'AvidScript.uplugin'
    Add-ExpectedDependency `
        -Dependencies $ExpectedDependencies `
        -TargetPath "$PluginTargetRoot/AvidScript.uplugin" `
        -Type UFS `
        -SourcePath $PluginDescriptorPath `
        -ExpectedSha256 '' `
        -Owner 'plugin_descriptor'

    $ModulesRoot = Join-Path $ResolvedProjectRoot 'Content/AvidScript/Modules'
    $CatalogPath = Join-Path $ModulesRoot 'catalog.json'
    $Catalog = Read-StrictJsonObject -Path $CatalogPath -Label 'module catalog'
    if ([long](Get-RequiredPropertyValue $Catalog 'schema_version' 'module catalog') -ne 1) {
        Throw-ReceiptValidationFailure 'JSON_SCHEMA_INVALID' 'Module catalog schema_version must be 1.'
    }
    $CatalogModules = @(Get-RequiredPropertyValue $Catalog 'modules' 'module catalog')
    if ($CatalogModules.Count -eq 0) {
        Throw-ReceiptValidationFailure 'JSON_SCHEMA_INVALID' 'Module catalog must contain at least one module.'
    }
    Add-ExpectedDependency `
        -Dependencies $ExpectedDependencies `
        -TargetPath '$(ProjectDir)/Content/AvidScript/Modules/catalog.json' `
        -Type UFS `
        -SourcePath $CatalogPath `
        -ExpectedSha256 '' `
        -Owner 'module_catalog'

    $CatalogIdentities = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Module in $CatalogModules) {
        $ModuleId = [string](Get-RequiredPropertyValue $Module 'module_id' 'module catalog entry')
        $PackageId = [string](Get-RequiredPropertyValue $Module 'package_id' "module '$ModuleId'")
        $DescriptorFile = [string](Get-RequiredPropertyValue $Module 'descriptor_file' "module '$ModuleId'")
        $DescriptorSha256 = [string](Get-RequiredPropertyValue $Module 'descriptor_sha256' "module '$ModuleId'")
        $Platform = [string](Get-RequiredPropertyValue $Module 'platform' "module '$ModuleId'")
        $ModuleConfiguration = [string](Get-RequiredPropertyValue $Module 'configuration' "module '$ModuleId'")
        if ($ModuleId -cnotmatch '^[a-z][a-z0-9_.-]{0,63}$') {
            Throw-ReceiptValidationFailure 'JSON_SCHEMA_INVALID' "Module id is invalid: '$ModuleId'."
        }
        Assert-LowercaseSha256 -Value $PackageId -Label "module '$ModuleId'.package_id"
        Assert-LowercaseSha256 -Value $DescriptorSha256 -Label "module '$ModuleId'.descriptor_sha256"
        if ($Platform -cne 'win64') {
            Throw-ReceiptValidationFailure 'PACKAGE_PLATFORM_MISMATCH' "Module '$ModuleId' platform is '$Platform', expected 'win64'."
        }
        if ($ModuleConfiguration -cne $ExpectedConfiguration) {
            Throw-ReceiptValidationFailure `
                'PACKAGE_CONFIGURATION_MISMATCH' `
                "Module '$ModuleId' configuration is '$ModuleConfiguration', expected '$ExpectedConfiguration'."
        }
        $ExpectedDescriptorFile = "$ModuleId/$PackageId/package.json"
        if ($DescriptorFile -cne $ExpectedDescriptorFile) {
            Throw-ReceiptValidationFailure 'SOURCE_PATH_INVALID' "Module '$ModuleId' descriptor path is not canonical."
        }
        $Identity = "$ModuleId|$PackageId"
        if (-not $CatalogIdentities.Add($Identity)) {
            Throw-ReceiptValidationFailure 'JSON_SCHEMA_INVALID' "Duplicate module package identity: $Identity"
        }

        $DescriptorPath = Resolve-ContainedSourceFile `
            -Root $ModulesRoot `
            -RelativePath $DescriptorFile `
            -Label "module '$ModuleId' descriptor"
        Add-ExpectedDependency `
            -Dependencies $ExpectedDependencies `
            -TargetPath "`$(ProjectDir)/Content/AvidScript/Modules/$DescriptorFile" `
            -Type UFS `
            -SourcePath $DescriptorPath `
            -ExpectedSha256 $DescriptorSha256 `
            -Owner "module:${ModuleId}:descriptor"

        $Descriptor = Read-StrictJsonObject -Path $DescriptorPath -Label "module '$ModuleId' descriptor"
        if ([long](Get-RequiredPropertyValue $Descriptor 'schema_version' "module '$ModuleId' descriptor") -ne 1 -or
            [string](Get-RequiredPropertyValue $Descriptor 'module_id' "module '$ModuleId' descriptor") -cne $ModuleId -or
            [string](Get-RequiredPropertyValue $Descriptor 'package_id' "module '$ModuleId' descriptor") -cne $PackageId) {
            Throw-ReceiptValidationFailure 'JSON_SCHEMA_INVALID' "Module '$ModuleId' descriptor identity is invalid."
        }
        if ([string](Get-RequiredPropertyValue $Descriptor 'platform' "module '$ModuleId' descriptor") -cne 'win64') {
            Throw-ReceiptValidationFailure 'PACKAGE_PLATFORM_MISMATCH' "Module '$ModuleId' descriptor platform is invalid."
        }
        if ([string](Get-RequiredPropertyValue $Descriptor 'configuration' "module '$ModuleId' descriptor") -cne $ExpectedConfiguration) {
            Throw-ReceiptValidationFailure `
                'PACKAGE_CONFIGURATION_MISMATCH' `
                "Module '$ModuleId' descriptor configuration does not match '$ExpectedConfiguration'."
        }

        $Artifacts = Get-RequiredPropertyValue $Descriptor 'artifacts' "module '$ModuleId' descriptor"
        $PackageRoot = Split-Path -Parent $DescriptorPath
        foreach ($ArtifactName in @(
                'runtime_manifest',
                'canonical_wasm',
                'precompiled',
                'binding_manifest',
                'binding_descriptor')) {
            $Artifact = Get-RequiredPropertyValue $Artifacts $ArtifactName "module '$ModuleId' artifacts"
            $ArtifactFile = [string](Get-RequiredPropertyValue $Artifact 'file' "module '$ModuleId' artifact '$ArtifactName'")
            $ArtifactSha256 = [string](Get-RequiredPropertyValue $Artifact 'sha256' "module '$ModuleId' artifact '$ArtifactName'")
            Assert-LowercaseSha256 -Value $ArtifactSha256 -Label "module '$ModuleId' artifact '$ArtifactName'.sha256"
            $ArtifactPath = Resolve-ContainedSourceFile `
                -Root $PackageRoot `
                -RelativePath $ArtifactFile `
                -Label "module '$ModuleId' artifact '$ArtifactName'"
            Add-ExpectedDependency `
                -Dependencies $ExpectedDependencies `
                -TargetPath "`$(ProjectDir)/Content/AvidScript/Modules/$ModuleId/$PackageId/$($ArtifactFile.Replace('\', '/'))" `
                -Type UFS `
                -SourcePath $ArtifactPath `
                -ExpectedSha256 $ArtifactSha256 `
                -Owner "module:${ModuleId}:$ArtifactName"
        }
        $DebugMapProperty = $Artifacts.PSObject.Properties['debug_map']
        if ($null -ne $DebugMapProperty) {
            $DebugMap = $DebugMapProperty.Value
            $DebugMapFile = [string](Get-RequiredPropertyValue $DebugMap 'file' "module '$ModuleId' artifact 'debug_map'")
            $DebugMapSha256 = [string](Get-RequiredPropertyValue $DebugMap 'sha256' "module '$ModuleId' artifact 'debug_map'")
            Assert-LowercaseSha256 -Value $DebugMapSha256 -Label "module '$ModuleId' artifact 'debug_map'.sha256"
            $DebugMapPath = Resolve-ContainedSourceFile `
                -Root $PackageRoot `
                -RelativePath $DebugMapFile `
                -Label "module '$ModuleId' artifact 'debug_map'"
            Add-ExpectedDependency `
                -Dependencies $ExpectedDependencies `
                -TargetPath "`$(ProjectDir)/Content/AvidScript/Modules/$ModuleId/$PackageId/$($DebugMapFile.Replace('\', '/'))" `
                -Type UFS `
                -SourcePath $DebugMapPath `
                -ExpectedSha256 $DebugMapSha256 `
                -Owner "module:${ModuleId}:debug_map"
        }
    }

    $GeneratedRoot = Join-Path $ResolvedPluginRoot 'Content/AvidScriptGenerated'
    $GeneratedCurrentPath = Join-Path $GeneratedRoot 'current.json'
    $GeneratedCurrent = Read-StrictJsonObject -Path $GeneratedCurrentPath -Label 'Generated Type current.json'
    if ([long](Get-RequiredPropertyValue $GeneratedCurrent 'schema_version' 'Generated Type current.json') -ne 2) {
        Throw-ReceiptValidationFailure 'GENERATED_TYPE_SCHEMA_MISMATCH' 'Generated Type current.json schema_version must be 2.'
    }
    $GeneratedModuleId = [string](Get-RequiredPropertyValue $GeneratedCurrent 'module_id' 'Generated Type current.json')
    $GeneratedPackageId = [string](Get-RequiredPropertyValue $GeneratedCurrent 'package_id' 'Generated Type current.json')
    if (-not $CatalogIdentities.Contains("$GeneratedModuleId|$GeneratedPackageId")) {
        Throw-ReceiptValidationFailure `
            'GENERATED_TYPE_PACKAGE_MISMATCH' `
            "Generated Type package '$GeneratedModuleId|$GeneratedPackageId' is not in the selected module catalog."
    }
    Add-ExpectedDependency `
        -Dependencies $ExpectedDependencies `
        -TargetPath "$PluginTargetRoot/Content/AvidScriptGenerated/current.json" `
        -Type UFS `
        -SourcePath $GeneratedCurrentPath `
        -ExpectedSha256 '' `
        -Owner 'generated_type:current'

    $TypeManifest = Get-RequiredPropertyValue $GeneratedCurrent 'type_manifest' 'Generated Type current.json'
    $TypeManifestFile = [string](Get-RequiredPropertyValue $TypeManifest 'file' 'Generated Type type_manifest')
    $TypeManifestSha256 = [string](Get-RequiredPropertyValue $TypeManifest 'sha256' 'Generated Type type_manifest')
    Assert-LowercaseSha256 -Value $TypeManifestSha256 -Label 'Generated Type type_manifest.sha256'
    if ($TypeManifestFile -cnotmatch '^[0-9a-f]{64}/type-manifest\.json$') {
        Throw-ReceiptValidationFailure 'SOURCE_PATH_INVALID' 'Generated Type type_manifest.file is not canonical.'
    }
    $TypeManifestPath = Resolve-ContainedSourceFile `
        -Root $GeneratedRoot `
        -RelativePath $TypeManifestFile `
        -Label 'Generated Type type-manifest'
    Add-ExpectedDependency `
        -Dependencies $ExpectedDependencies `
        -TargetPath "$PluginTargetRoot/Content/AvidScriptGenerated/$TypeManifestFile" `
        -Type UFS `
        -SourcePath $TypeManifestPath `
        -ExpectedSha256 $TypeManifestSha256 `
        -Owner 'generated_type:type_manifest'

    $WasmtimeRoot = Join-Path $ResolvedPluginRoot 'Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0-avidscript.1'
    $WasmtimeMarkerPath = Join-Path $WasmtimeRoot '.avidscript-wasmtime-performance-managed.json'
    $WasmtimeDllPath = Join-Path $WasmtimeRoot 'lib/wasmtime.dll'
    $WasmtimeLicensePath = Join-Path $WasmtimeRoot 'LICENSE'
    $WasmtimeMarker = Read-StrictJsonObject -Path $WasmtimeMarkerPath -Label 'Wasmtime performance managed marker'
    if ([long](Get-RequiredPropertyValue $WasmtimeMarker 'schema_version' 'Wasmtime performance managed marker') -ne 1) {
        Throw-ReceiptValidationFailure 'WASMTIME_MARKER_SCHEMA_MISMATCH' 'Wasmtime performance marker schema_version must be 1.'
    }
    $ManagedDllSha256 = [string](Get-RequiredPropertyValue $WasmtimeMarker 'dll_sha256' 'Wasmtime performance managed marker')
    Assert-LowercaseSha256 -Value $ManagedDllSha256 -Label 'Wasmtime performance managed marker.dll_sha256'
    if ((Get-FileSha256 -Path $WasmtimeDllPath) -cne $ManagedDllSha256) {
        Throw-ReceiptValidationFailure `
            'WASMTIME_DLL_HASH_MISMATCH' `
            'Wasmtime performance marker dll_sha256 does not match lib/wasmtime.dll.'
    }
    Add-ExpectedDependency `
        -Dependencies $ExpectedDependencies `
        -TargetPath "$PluginTargetRoot/Binaries/Win64/wasmtime.dll" `
        -Type NonUFS `
        -SourcePath $WasmtimeDllPath `
        -ExpectedSha256 $ManagedDllSha256 `
        -Owner 'wasmtime:dll'
    Add-ExpectedDependency `
        -Dependencies $ExpectedDependencies `
        -TargetPath "$PluginTargetRoot/Binaries/Win64/wasmtime.LICENSE.txt" `
        -Type NonUFS `
        -SourcePath $WasmtimeLicensePath `
        -ExpectedSha256 '' `
        -Owner 'wasmtime:license'

    $Receipt = Read-StrictJsonObject -Path $ResolvedReceiptPath -Label 'UBT target receipt'
    $ReceiptConfiguration = [string](Get-RequiredPropertyValue $Receipt 'Configuration' 'UBT target receipt')
    if (-not $ReceiptConfiguration.Equals($Configuration, [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-ReceiptValidationFailure `
            'RECEIPT_CONFIGURATION_MISMATCH' `
            "Receipt configuration is '$ReceiptConfiguration', expected '$Configuration'."
    }
    $ReceiptPlatform = [string](Get-RequiredPropertyValue $Receipt 'Platform' 'UBT target receipt')
    if ($ReceiptPlatform -cne 'Win64') {
        Throw-ReceiptValidationFailure 'RECEIPT_PLATFORM_MISMATCH' "Receipt platform is '$ReceiptPlatform', expected 'Win64'."
    }
    $RuntimeDependencies = @(Get-RequiredPropertyValue $Receipt 'RuntimeDependencies' 'UBT target receipt')
    $ActualDependencies = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($Dependency in $RuntimeDependencies) {
        $RawPath = [string](Get-RequiredPropertyValue $Dependency 'Path' 'runtime dependency')
        $DependencyType = [string](Get-RequiredPropertyValue $Dependency 'Type' "runtime dependency '$RawPath'")
        $CanonicalPath = ConvertTo-CanonicalReceiptPath `
            -Path $RawPath `
            -ResolvedProjectRoot $ResolvedProjectRoot `
            -PluginTargetRoot $PluginTargetRoot
        if (-not (Test-IsAvidScriptDependencyPath -Path $CanonicalPath -PluginTargetRoot $PluginTargetRoot)) {
            continue
        }
        if ($ActualDependencies.ContainsKey($CanonicalPath)) {
            Throw-ReceiptValidationFailure 'DUPLICATE_DEPENDENCY' "Receipt dependency is duplicated: $CanonicalPath"
        }
        $ActualDependencies.Add($CanonicalPath, [pscustomobject]@{
            target_path = $CanonicalPath
            type = $DependencyType
            receipt_path = $RawPath
        })
    }

    $Missing = @(
        $ExpectedDependencies.Keys |
            Where-Object { -not $ActualDependencies.ContainsKey($_) } |
            Sort-Object)
    if ($Missing.Count -gt 0) {
        Throw-ReceiptValidationFailure 'MISSING_DEPENDENCY' "Receipt is missing: $($Missing -join ', ')"
    }
    $Extra = @(
        $ActualDependencies.Keys |
            Where-Object { -not $ExpectedDependencies.ContainsKey($_) } |
            Sort-Object)
    if ($Extra.Count -gt 0) {
        Throw-ReceiptValidationFailure 'EXTRA_DEPENDENCY' "Receipt contains unexpected AvidScript dependencies: $($Extra -join ', ')"
    }
    foreach ($TargetPath in @($ExpectedDependencies.Keys | Sort-Object)) {
        $Expected = $ExpectedDependencies[$TargetPath]
        $Actual = $ActualDependencies[$TargetPath]
        if ([string]$Actual.type -cne [string]$Expected.type) {
            Throw-ReceiptValidationFailure `
                'DEPENDENCY_TYPE_MISMATCH' `
                "Receipt dependency '$TargetPath' has type '$($Actual.type)', expected '$($Expected.type)'."
        }
    }

    $ExpectedList = @(
        $ExpectedDependencies.Values |
            Sort-Object target_path |
            ForEach-Object {
                [pscustomobject][ordered]@{
                    target_path = $_.target_path
                    type = $_.type
                    owner = $_.owner
                    source_sha256 = $_.source_sha256
                }
            })
    $Summary = [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_package_receipt_valid'
        status = 'ok'
        receipt_path = $ResolvedReceiptPath
        configuration = $Configuration
        platform = $ReceiptPlatform
        target_name = [string](Get-RequiredPropertyValue $Receipt 'TargetName' 'UBT target receipt')
        module_count = $CatalogModules.Count
        generated_type_package_id = $TypeManifestFile.Split('/')[0]
        wasmtime_dll_sha256 = $ManagedDllSha256
        expected_dependency_count = $ExpectedDependencies.Count
        actual_dependency_count = $ActualDependencies.Count
        ufs_dependency_count = @($ExpectedList | Where-Object type -ceq 'UFS').Count
        non_ufs_dependency_count = @($ExpectedList | Where-Object type -ceq 'NonUFS').Count
        dependencies = $ExpectedList
    }
    [Console]::Out.WriteLine(($Summary | ConvertTo-Json -Depth 8 -Compress))
    exit 0
}
catch {
    $Message = $_.Exception.Message
    $Code = 'UNEXPECTED_VALIDATION_ERROR'
    $Detail = $Message
    if ($Message -match '^(?<code>[A-Z0-9_]+)\|(?<detail>.*)$') {
        $Code = $Matches.code
        $Detail = $Matches.detail
    }
    $Failure = [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_package_receipt_invalid'
        status = 'error'
        error_code = $Code
        message = $Detail
        receipt_path = $ReceiptPath
        configuration = $Configuration
    }
    [Console]::Out.WriteLine(($Failure | ConvertTo-Json -Depth 4 -Compress))
    exit 1
}
