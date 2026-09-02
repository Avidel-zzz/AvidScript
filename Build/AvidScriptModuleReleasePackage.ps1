function Get-AvidScriptModuleReleaseSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
}

function Get-AvidScriptModuleReleaseBytesSha256 {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    return [System.Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

function Test-AvidScriptModuleReleasePathUnderRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $FullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    return $FullPath.Equals($FullRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
        $FullPath.StartsWith(
            $FullRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-AvidScriptModuleReleasePropertyNames {
    param([Parameter(Mandatory = $true)][object]$Value)

    return @($Value.PSObject.Properties | ForEach-Object Name)
}

function Assert-AvidScriptModuleReleaseObjectShape {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Label,
        [string[]]$Required = @(),
        [string[]]$Optional = @()
    )

    if ($Value -isnot [System.Management.Automation.PSCustomObject] -and
        $Value -isnot [System.Collections.IDictionary]) {
        throw "$Label must be a JSON object."
    }
    $Names = @(Get-AvidScriptModuleReleasePropertyNames $Value)
    foreach ($Name in $Required) {
        if ($Names -cnotcontains $Name) {
            throw "$Label is missing required property '$Name'."
        }
    }
    $Allowed = @($Required) + @($Optional)
    $Unknown = @($Names | Where-Object { $Allowed -cnotcontains $_ })
    if ($Unknown.Count -ne 0) {
        throw "$Label contains unknown property '$($Unknown[0])'."
    }
}

function Assert-AvidScriptModuleReleaseNoDuplicateJsonProperties {
    param(
        [Parameter(Mandatory = $true)][System.Text.Json.JsonElement]$Element,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $Names = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($Property in $Element.EnumerateObject()) {
            if (-not $Names.Add($Property.Name)) {
                throw "$Label contains duplicate JSON property '$($Property.Name)'."
            }
            Assert-AvidScriptModuleReleaseNoDuplicateJsonProperties `
                -Element $Property.Value `
                -Label "$Label.$($Property.Name)"
        }
    }
    elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        $Index = 0
        foreach ($Item in $Element.EnumerateArray()) {
            Assert-AvidScriptModuleReleaseNoDuplicateJsonProperties `
                -Element $Item `
                -Label "$Label[$Index]"
            ++$Index
        }
    }
}

function Read-AvidScriptModuleReleaseJsonObject {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label does not exist: $Path"
    }
    $Utf8Strict = [System.Text.UTF8Encoding]::new($false, $true)
    try {
        $Text = $Utf8Strict.GetString([System.IO.File]::ReadAllBytes($Path))
        $Options = [System.Text.Json.JsonDocumentOptions]::new()
        $Options.AllowTrailingCommas = $false
        $Options.CommentHandling = [System.Text.Json.JsonCommentHandling]::Disallow
        $Options.MaxDepth = 64
        $Document = [System.Text.Json.JsonDocument]::Parse($Text, $Options)
        try {
            if ($Document.RootElement.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) {
                throw "$Label must have a JSON object root."
            }
            Assert-AvidScriptModuleReleaseNoDuplicateJsonProperties `
                -Element $Document.RootElement `
                -Label $Label
        }
        finally {
            $Document.Dispose()
        }
        return $Text | ConvertFrom-Json -Depth 64 -DateKind String -NoEnumerate
    }
    catch {
        if ($_.Exception.Message.StartsWith($Label, [System.StringComparison]::Ordinal)) {
            throw
        }
        throw "$Label is not strict UTF-8 JSON: $($_.Exception.Message)"
    }
}

function ConvertTo-AvidScriptModuleReleaseCanonicalValue {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return $null
    }
    if ($Value -is [System.Management.Automation.PSCustomObject] -or
        $Value -is [System.Collections.IDictionary]) {
        $Names = [System.Collections.Generic.List[string]]::new()
        foreach ($Name in @(Get-AvidScriptModuleReleasePropertyNames $Value)) {
            $Names.Add($Name)
        }
        $Names.Sort([System.StringComparer]::Ordinal)
        $Result = [ordered]@{}
        foreach ($Name in $Names) {
            $Result[$Name] = ConvertTo-AvidScriptModuleReleaseCanonicalValue $Value.$Name
        }
        return $Result
    }
    if ($Value -is [System.Array]) {
        $Items = [System.Collections.Generic.List[object]]::new()
        foreach ($Item in $Value) {
            $Items.Add((ConvertTo-AvidScriptModuleReleaseCanonicalValue $Item))
        }
        Write-Output -NoEnumerate $Items.ToArray()
        return
    }
    return $Value
}

function ConvertTo-AvidScriptModuleReleaseJsonBytes {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [switch]$Canonical
    )

    $JsonValue = if ($Canonical) {
        ConvertTo-AvidScriptModuleReleaseCanonicalValue $Value
    }
    else {
        $Value
    }
    $Json = ($JsonValue | ConvertTo-Json -Depth 64).Replace("`r`n", "`n") + "`n"
    return [System.Text.UTF8Encoding]::new($false).GetBytes($Json)
}

function Test-AvidScriptModuleReleaseJsonInteger {
    param([AllowNull()][object]$Value)

    return $Value -is [byte] -or
        $Value -is [sbyte] -or
        $Value -is [int16] -or
        $Value -is [uint16] -or
        $Value -is [int32] -or
        $Value -is [uint32] -or
        $Value -is [int64] -or
        $Value -is [uint64]
}

function Assert-AvidScriptModuleReleaseSha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Value -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Label must be a lowercase SHA-256 value."
    }
}

function Normalize-AvidScriptModuleReleaseModuleId {
    param([Parameter(Mandatory = $true)][string]$ModuleId)

    if ($ModuleId -cnotmatch '^[a-z][a-z0-9_.-]{0,63}$') {
        throw 'module_id must match ^[a-z][a-z0-9_.-]{0,63}$.'
    }
    return $ModuleId
}

function Assert-AvidScriptModuleReleaseRelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ([string]::IsNullOrWhiteSpace($Value) -or
        [System.IO.Path]::IsPathRooted($Value) -or
        [System.Uri]::IsWellFormedUriString($Value, [System.UriKind]::Absolute)) {
        throw "$Label must be a non-empty relative path."
    }
    $Normalized = $Value.Replace('\', '/')
    $Segments = @($Normalized.Split('/'))
    if ($Segments.Count -eq 0 -or
        @($Segments | Where-Object { $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' }).Count -ne 0 -or
        $Normalized.IndexOfAny([System.IO.Path]::GetInvalidPathChars()) -ge 0) {
        throw "$Label must not contain empty, current, or parent path segments."
    }
}

function Assert-AvidScriptModuleReleaseManifestPaths {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($null -eq $Value) {
        return
    }
    if ($Value -is [System.Management.Automation.PSCustomObject] -or
        $Value -is [System.Collections.IDictionary]) {
        foreach ($Property in $Value.PSObject.Properties) {
            $Name = [string]$Property.Name
            $Child = $Property.Value
            if (($Name -ceq 'file' -or $Name -clike '*_file' -or
                    $Name -ceq 'bindings' -or $Name -ceq 'generated_d') -and
                $Child -is [string] -and
                -not [string]::IsNullOrWhiteSpace($Child)) {
                Assert-AvidScriptModuleReleaseRelativePath `
                    -Value $Child `
                    -Label "$Label.$Name"
            }
            Assert-AvidScriptModuleReleaseManifestPaths -Value $Child -Label "$Label.$Name"
        }
    }
    elseif ($Value -is [System.Array]) {
        for ($Index = 0; $Index -lt $Value.Count; ++$Index) {
            Assert-AvidScriptModuleReleaseManifestPaths `
                -Value $Value[$Index] `
                -Label "$Label[$Index]"
        }
    }
}

function Resolve-AvidScriptModuleReleaseDependency {
    param(
        [Parameter(Mandatory = $true)][string]$RuntimeManifestPath,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$Label
    )

    Assert-AvidScriptModuleReleaseRelativePath -Value $RelativePath -Label $Label
    $Normalized = $RelativePath.Replace('\', '/')
    $ProjectRelative = $Normalized.StartsWith('Content/', [System.StringComparison]::OrdinalIgnoreCase) -or
        $Normalized.StartsWith('Plugins/', [System.StringComparison]::OrdinalIgnoreCase) -or
        $Normalized.StartsWith('Saved/', [System.StringComparison]::OrdinalIgnoreCase) -or
        $Normalized.StartsWith('Source/', [System.StringComparison]::OrdinalIgnoreCase)
    $BasePath = if ($ProjectRelative) {
        $ProjectRoot
    }
    else {
        Split-Path -Parent $RuntimeManifestPath
    }
    $Candidate = [System.IO.Path]::GetFullPath((Join-Path $BasePath $RelativePath))
    if (-not (Test-AvidScriptModuleReleasePathUnderRoot -Path $Candidate -Root $ProjectRoot)) {
        throw "$Label escapes the project root."
    }
    if (-not (Test-Path -LiteralPath $Candidate -PathType Leaf)) {
        throw "$Label dependency is missing: $RelativePath"
    }
    return (Resolve-Path -LiteralPath $Candidate -ErrorAction Stop).Path
}

function Assert-AvidScriptModuleReleaseDependencyHash {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][string]$Label
    )

    Assert-AvidScriptModuleReleaseSha256 -Value $ExpectedSha256 -Label "$Label.sha256"
    $ActualSha256 = Get-AvidScriptModuleReleaseSha256 $Path
    if ($ActualSha256 -cne $ExpectedSha256) {
        throw "$Label SHA-256 mismatch: expected=$ExpectedSha256 actual=$ActualSha256"
    }
}

function Get-AvidScriptModuleReleasePackageId {
    param([Parameter(Mandatory = $true)][object]$Descriptor)

    $Values = [System.Collections.Generic.List[string]]::new()
    foreach ($Value in @(
            [string]$Descriptor.schema_version,
            [string]$Descriptor.module_id,
            [string]$Descriptor.abi_version,
            [string]$Descriptor.platform,
            [string]$Descriptor.configuration,
            [string]$Descriptor.minimum_runtime_version,
            [string]$Descriptor.execution.backend,
            [string]$Descriptor.execution.format,
            [string]$Descriptor.execution.policy,
            [string]$Descriptor.execution.compiler_build_identity,
            [string]$Descriptor.execution.target_triple,
            [string]$Descriptor.execution.cpu_features)) {
        if ($Value.Contains("`r") -or $Value.Contains("`n")) {
            throw 'Package identity scalars must not contain newline characters.'
        }
        $Values.Add($Value)
    }
    foreach ($ArtifactName in @(
            'runtime_manifest',
            'canonical_wasm',
            'precompiled',
            'binding_manifest',
            'binding_descriptor',
            'debug_map')) {
        if ($Descriptor.artifacts.PSObject.Properties.Name -ccontains $ArtifactName) {
            $Artifact = $Descriptor.artifacts.$ArtifactName
            $Values.Add([string]$Artifact.sha256)
        }
    }
    $Preimage = [System.Text.Encoding]::UTF8.GetBytes([string]::Join("`n", $Values))
    return Get-AvidScriptModuleReleaseBytesSha256 $Preimage
}

function Copy-AvidScriptModuleReleaseJsonObject {
    param([Parameter(Mandatory = $true)][object]$Value)

    $Json = $Value | ConvertTo-Json -Depth 64 -Compress
    return $Json | ConvertFrom-Json -Depth 64 -DateKind String -NoEnumerate
}

function Remove-AvidScriptModuleReleaseProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($Value.PSObject.Properties.Name -ccontains $Name) {
        $Value.PSObject.Properties.Remove($Name)
    }
}

function Get-AvidScriptModuleReleaseExpectedFiles {
    param([Parameter(Mandatory = $true)][object]$Descriptor)

    $Expected = [ordered]@{
        'package.json' = ''
        'runtime.avidscript.json' = [string]$Descriptor.artifacts.runtime_manifest.sha256
        'module.wasm' = [string]$Descriptor.artifacts.canonical_wasm.sha256
        'module.wasmtime.cwasm' = [string]$Descriptor.artifacts.precompiled.sha256
        'bindings/package.json' = [string]$Descriptor.artifacts.binding_manifest.sha256
        'bindings/bindings.json' = [string]$Descriptor.artifacts.binding_descriptor.sha256
    }
    if ($Descriptor.artifacts.PSObject.Properties.Name -ccontains 'debug_map') {
        $Expected['diagnostics/debug-map.json'] = [string]$Descriptor.artifacts.debug_map.sha256
    }
    return $Expected
}

function Assert-AvidScriptModuleReleaseArtifactDescriptor {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$ExpectedFile
    )

    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $Value `
        -Label $Label `
        -Required @('file', 'sha256')
    if ([string]$Value.file -cne $ExpectedFile) {
        throw "$Label.file must be '$ExpectedFile'."
    }
    Assert-AvidScriptModuleReleaseSha256 -Value ([string]$Value.sha256) -Label "$Label.sha256"
}

function Assert-AvidScriptModuleReleasePackageDescriptor {
    param([Parameter(Mandatory = $true)][object]$Descriptor)

    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $Descriptor `
        -Label 'package.json' `
        -Required @(
            'schema_version',
            'package_id',
            'module_id',
            'abi_version',
            'platform',
            'configuration',
            'minimum_runtime_version',
            'execution',
            'artifacts')
    if (-not (Test-AvidScriptModuleReleaseJsonInteger $Descriptor.schema_version) -or
        [int64]$Descriptor.schema_version -ne 1) {
        throw 'package.json schema_version must be the JSON integer 1.'
    }
    $NormalizedModuleId = Normalize-AvidScriptModuleReleaseModuleId ([string]$Descriptor.module_id)
    if ($NormalizedModuleId -cne [string]$Descriptor.module_id) {
        throw 'package.json module_id must already be normalized.'
    }
    if (-not (Test-AvidScriptModuleReleaseJsonInteger $Descriptor.abi_version) -or
        [int64]$Descriptor.abi_version -lt 1) {
        throw 'package.json abi_version must be a positive JSON integer.'
    }
    if (@('win64', 'android') -cnotcontains [string]$Descriptor.platform -or
        @('development', 'shipping') -cnotcontains [string]$Descriptor.configuration) {
        throw 'package.json platform or configuration is invalid.'
    }
    if ([string]$Descriptor.minimum_runtime_version -cnotmatch
        '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$') {
        throw 'package.json minimum_runtime_version must be a non-empty SemVer value.'
    }
    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $Descriptor.execution `
        -Label 'package.json.execution' `
        -Required @(
            'backend',
            'format',
            'policy',
            'compiler_build_identity',
            'target_triple',
            'cpu_features')
    if ([string]$Descriptor.execution.backend -cne 'wasmtime' -or
        [string]$Descriptor.execution.format -cne 'wasmtime_serialized_v1' -or
        @('require_precompiled', 'prefer_precompiled') -cnotcontains [string]$Descriptor.execution.policy -or
        [string]::IsNullOrWhiteSpace([string]$Descriptor.execution.compiler_build_identity) -or
        [string]::IsNullOrWhiteSpace([string]$Descriptor.execution.target_triple) -or
        [string]::IsNullOrWhiteSpace([string]$Descriptor.execution.cpu_features)) {
        throw 'package.json execution contract is invalid.'
    }
    $ExpectedTargetTriple = if ([string]$Descriptor.platform -ceq 'android') {
        'aarch64-linux-android'
    }
    else {
        'x86_64-pc-windows-msvc'
    }
    $ExpectedCpuFeatures = if ([string]$Descriptor.platform -ceq 'android') {
        'arm64-v8a'
    }
    else {
        'x86-64-v3'
    }
    if ([string]$Descriptor.execution.target_triple -cne $ExpectedTargetTriple -or
        [string]$Descriptor.execution.cpu_features -cne $ExpectedCpuFeatures) {
        throw 'package.json execution target does not match its platform identity.'
    }
    if ([string]$Descriptor.configuration -ceq 'shipping' -and
        [string]$Descriptor.execution.policy -cne 'require_precompiled') {
        throw 'Shipping package.json must require precompiled execution.'
    }
    if ([string]$Descriptor.platform -ceq 'android' -and
        [string]$Descriptor.execution.policy -cne 'require_precompiled') {
        throw 'Android package.json must require precompiled execution.'
    }
    $RequiredArtifacts = @(
        'runtime_manifest',
        'canonical_wasm',
        'precompiled',
        'binding_manifest',
        'binding_descriptor')
    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $Descriptor.artifacts `
        -Label 'package.json.artifacts' `
        -Required $RequiredArtifacts `
        -Optional @('debug_map')
    $Files = [ordered]@{
        runtime_manifest = 'runtime.avidscript.json'
        canonical_wasm = 'module.wasm'
        precompiled = 'module.wasmtime.cwasm'
        binding_manifest = 'bindings/package.json'
        binding_descriptor = 'bindings/bindings.json'
    }
    if ($Descriptor.artifacts.PSObject.Properties.Name -ccontains 'debug_map') {
        $Files['debug_map'] = 'diagnostics/debug-map.json'
    }
    foreach ($ArtifactName in $Files.Keys) {
        Assert-AvidScriptModuleReleaseArtifactDescriptor `
            -Value $Descriptor.artifacts.$ArtifactName `
            -Label "package.json.artifacts.$ArtifactName" `
            -ExpectedFile $Files[$ArtifactName]
    }
    Assert-AvidScriptModuleReleaseSha256 `
        -Value ([string]$Descriptor.package_id) `
        -Label 'package.json.package_id'
    $ExpectedPackageId = Get-AvidScriptModuleReleasePackageId $Descriptor
    if ([string]$Descriptor.package_id -cne $ExpectedPackageId) {
        throw 'package.json package_id does not match its deterministic identity preimage.'
    }
}

function Assert-AvidScriptModuleReleasePackageDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$ExpectedDescriptorSha256,
        [Parameter(Mandatory = $true)][string]$ExpectedModuleId,
        [Parameter(Mandatory = $true)][string]$ExpectedPackageId
    )

    if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
        throw "Module release package directory is missing: $PackageRoot"
    }
    $DescriptorPath = Join-Path $PackageRoot 'package.json'
    Assert-AvidScriptModuleReleaseDependencyHash `
        -Path $DescriptorPath `
        -ExpectedSha256 $ExpectedDescriptorSha256 `
        -Label 'package.json'
    $Descriptor = Read-AvidScriptModuleReleaseJsonObject `
        -Path $DescriptorPath `
        -Label 'package.json'
    Assert-AvidScriptModuleReleasePackageDescriptor $Descriptor
    if ([string]$Descriptor.module_id -cne $ExpectedModuleId -or
        [string]$Descriptor.package_id -cne $ExpectedPackageId) {
        throw 'Module release package identity does not match its catalog entry.'
    }

    $ExpectedFiles = Get-AvidScriptModuleReleaseExpectedFiles $Descriptor
    $ExpectedFiles['package.json'] = $ExpectedDescriptorSha256
    $ActualFiles = @(
        Get-ChildItem -LiteralPath $PackageRoot -File -Recurse -ErrorAction Stop |
            ForEach-Object {
                [System.IO.Path]::GetRelativePath($PackageRoot, $_.FullName).Replace('\', '/')
            }
    )
    if ($ActualFiles.Count -ne $ExpectedFiles.Count) {
        throw 'Module release package contains missing or extra files.'
    }
    foreach ($RelativePath in $ExpectedFiles.Keys) {
        if ($ActualFiles -cnotcontains $RelativePath) {
            throw "Module release package is missing '$RelativePath'."
        }
        Assert-AvidScriptModuleReleaseDependencyHash `
            -Path (Join-Path $PackageRoot $RelativePath) `
            -ExpectedSha256 $ExpectedFiles[$RelativePath] `
            -Label "package file '$RelativePath'"
    }
    $ExpectedDirectories = @('bindings')
    if ($ExpectedFiles.Keys -ccontains 'diagnostics/debug-map.json') {
        $ExpectedDirectories += 'diagnostics'
    }
    $ActualDirectories = @(
        Get-ChildItem -LiteralPath $PackageRoot -Directory -Recurse -ErrorAction Stop |
            ForEach-Object {
                [System.IO.Path]::GetRelativePath($PackageRoot, $_.FullName).Replace('\', '/')
            }
    )
    if ($ActualDirectories.Count -ne $ExpectedDirectories.Count -or
        @($ActualDirectories | Where-Object { $ExpectedDirectories -cnotcontains $_ }).Count -ne 0) {
        throw 'Module release package contains an extra directory.'
    }
    return $Descriptor
}

function Sort-AvidScriptModuleReleaseCatalogModules {
    param([object[]]$Modules)

    $Sorted = [System.Collections.Generic.List[object]]::new()
    foreach ($Module in @($Modules)) {
        $InsertAt = $Sorted.Count
        for ($Index = 0; $Index -lt $Sorted.Count; ++$Index) {
            if ([System.StringComparer]::Ordinal.Compare(
                    [string]$Module.module_id,
                    [string]$Sorted[$Index].module_id) -lt 0) {
                $InsertAt = $Index
                break
            }
        }
        $Sorted.Insert($InsertAt, $Module)
    }
    return @($Sorted)
}

function Get-AvidScriptModuleReleaseVariantKey {
    param([Parameter(Mandatory = $true)][object]$Variant)

    return [string]::Join("`n", @(
            [string]$Variant.platform,
            [string]$Variant.architecture,
            [string]$Variant.configuration,
            [string]$Variant.backend,
            [string]$Variant.format))
}

function Sort-AvidScriptModuleReleaseCatalogVariants {
    param([object[]]$Variants)

    $Sorted = [System.Collections.Generic.List[object]]::new()
    foreach ($Variant in @($Variants)) {
        $VariantKey = Get-AvidScriptModuleReleaseVariantKey $Variant
        $InsertAt = $Sorted.Count
        for ($Index = 0; $Index -lt $Sorted.Count; ++$Index) {
            $ExistingKey = Get-AvidScriptModuleReleaseVariantKey $Sorted[$Index]
            if ([System.StringComparer]::Ordinal.Compare($VariantKey, $ExistingKey) -lt 0) {
                $InsertAt = $Index
                break
            }
        }
        $Sorted.Insert($InsertAt, $Variant)
    }
    return @($Sorted)
}

function ConvertTo-AvidScriptModuleReleaseCatalogVariant {
    param(
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [Parameter(Mandatory = $true)][object]$Variant,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [switch]$Legacy
    )

    $Required = if ($Legacy) {
        @(
            'module_id',
            'package_id',
            'descriptor_file',
            'descriptor_sha256',
            'platform',
            'configuration')
    }
    else {
        @(
            'platform',
            'architecture',
            'configuration',
            'backend',
            'format',
            'package_id',
            'descriptor_file',
            'descriptor_sha256')
    }
    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $Variant `
        -Label 'catalog.json variant entry' `
        -Required $Required

    if ($Legacy -and [string]$Variant.module_id -cne $ModuleId) {
        throw 'catalog.json legacy module id is inconsistent.'
    }
    $Architecture = if ($Legacy) { 'x86_64' } else { [string]$Variant.architecture }
    $Backend = if ($Legacy) { 'wasmtime' } else { [string]$Variant.backend }
    $Format = if ($Legacy) { 'wasmtime_serialized_v1' } else { [string]$Variant.format }
    $PlatformIdentitySupported =
        ([string]$Variant.platform -ceq 'win64' -and $Architecture -ceq 'x86_64') -or
        ([string]$Variant.platform -ceq 'android' -and $Architecture -ceq 'arm64')
    if (-not $PlatformIdentitySupported -or
        @('development', 'shipping') -cnotcontains [string]$Variant.configuration -or
        $Backend -cne 'wasmtime' -or
        $Format -cne 'wasmtime_serialized_v1') {
        throw 'catalog.json variant contains an unsupported platform identity.'
    }

    Assert-AvidScriptModuleReleaseSha256 `
        -Value ([string]$Variant.package_id) `
        -Label 'catalog.json package_id'
    Assert-AvidScriptModuleReleaseSha256 `
        -Value ([string]$Variant.descriptor_sha256) `
        -Label 'catalog.json descriptor_sha256'
    $ExpectedDescriptorFile = "$ModuleId/$($Variant.package_id)/package.json"
    if ([string]$Variant.descriptor_file -cne $ExpectedDescriptorFile) {
        throw 'catalog.json variant descriptor path is not canonical.'
    }
    Assert-AvidScriptModuleReleaseRelativePath `
        -Value ([string]$Variant.descriptor_file) `
        -Label 'catalog.json descriptor_file'
    $DescriptorPath = [System.IO.Path]::GetFullPath(
        (Join-Path $OutputRoot ([string]$Variant.descriptor_file)))
    if (-not (Test-AvidScriptModuleReleasePathUnderRoot -Path $DescriptorPath -Root $OutputRoot)) {
        throw 'catalog.json descriptor_file escapes the module release root.'
    }
    Assert-AvidScriptModuleReleasePackageDirectory `
        -PackageRoot (Split-Path -Parent $DescriptorPath) `
        -ExpectedDescriptorSha256 ([string]$Variant.descriptor_sha256) `
        -ExpectedModuleId $ModuleId `
        -ExpectedPackageId ([string]$Variant.package_id) | Out-Null

    return [pscustomobject][ordered]@{
        platform = [string]$Variant.platform
        architecture = $Architecture
        configuration = [string]$Variant.configuration
        backend = $Backend
        format = $Format
        package_id = [string]$Variant.package_id
        descriptor_file = [string]$Variant.descriptor_file
        descriptor_sha256 = [string]$Variant.descriptor_sha256
    }
}

function Read-AvidScriptModuleReleaseCatalog {
    param(
        [Parameter(Mandatory = $true)][string]$CatalogPath,
        [Parameter(Mandatory = $true)][string]$OutputRoot
    )

    if (-not (Test-Path -LiteralPath $CatalogPath -PathType Leaf)) {
        return [pscustomobject][ordered]@{
            schema_version = 2
            modules = @()
        }
    }
    $Catalog = Read-AvidScriptModuleReleaseJsonObject -Path $CatalogPath -Label 'catalog.json'
    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $Catalog `
        -Label 'catalog.json' `
        -Required @('schema_version', 'modules')
    if (-not (Test-AvidScriptModuleReleaseJsonInteger $Catalog.schema_version) -or
        @([int64]1, [int64]2) -cnotcontains [int64]$Catalog.schema_version -or
        $Catalog.modules -isnot [System.Array]) {
        throw 'catalog.json must use schema_version 1 or 2 and a modules array.'
    }

    $NormalizedModules = [System.Collections.Generic.List[object]]::new()
    $PreviousModuleId = $null
    foreach ($Module in @($Catalog.modules)) {
        if ([int64]$Catalog.schema_version -eq 1) {
            $ModuleId = Normalize-AvidScriptModuleReleaseModuleId ([string]$Module.module_id)
            $Variants = @(
                ConvertTo-AvidScriptModuleReleaseCatalogVariant `
                    -ModuleId $ModuleId `
                    -Variant $Module `
                    -OutputRoot $OutputRoot `
                    -Legacy)
        }
        else {
            Assert-AvidScriptModuleReleaseObjectShape `
                -Value $Module `
                -Label 'catalog.json module entry' `
                -Required @('module_id', 'variants')
            $ModuleId = Normalize-AvidScriptModuleReleaseModuleId ([string]$Module.module_id)
            if ($Module.variants -isnot [System.Array] -or @($Module.variants).Count -eq 0) {
                throw 'catalog.json module variants must be a non-empty array.'
            }
            $Variants = @(
                foreach ($Variant in @($Module.variants)) {
                    ConvertTo-AvidScriptModuleReleaseCatalogVariant `
                        -ModuleId $ModuleId `
                        -Variant $Variant `
                        -OutputRoot $OutputRoot
                })
        }
        $ModuleId = Normalize-AvidScriptModuleReleaseModuleId ([string]$Module.module_id)
        if ($ModuleId -cne [string]$Module.module_id -or
            ($null -ne $PreviousModuleId -and
                [System.StringComparer]::Ordinal.Compare($PreviousModuleId, $ModuleId) -ge 0)) {
            throw 'catalog.json modules must be unique and strictly increasing by module_id ordinal.'
        }

        $PreviousVariantKey = $null
        foreach ($Variant in $Variants) {
            $VariantKey = Get-AvidScriptModuleReleaseVariantKey $Variant
            if ($null -ne $PreviousVariantKey -and
                [System.StringComparer]::Ordinal.Compare($PreviousVariantKey, $VariantKey) -ge 0) {
                throw 'catalog.json variants must be unique and strictly increasing by identity ordinal.'
            }
            $PreviousVariantKey = $VariantKey
        }
        $NormalizedModules.Add([pscustomobject][ordered]@{
                module_id = $ModuleId
                variants = @($Variants)
            })
        $PreviousModuleId = $ModuleId
    }
    return [pscustomobject][ordered]@{
        schema_version = 2
        modules = @($NormalizedModules)
    }
}

function Enter-AvidScriptModuleReleaseCatalogLock {
    param([Parameter(Mandatory = $true)][string]$OutputRoot)

    $Identity = [System.Text.Encoding]::UTF8.GetBytes(
        [System.IO.Path]::GetFullPath($OutputRoot).ToLowerInvariant())
    $Name = 'AvidScriptModuleRelease_' + (Get-AvidScriptModuleReleaseBytesSha256 $Identity)
    $Mutex = [System.Threading.Mutex]::new($false, $Name)
    try {
        if (-not $Mutex.WaitOne([TimeSpan]::FromSeconds(30))) {
            throw 'Timed out waiting for the module release catalog lock.'
        }
        return $Mutex
    }
    catch {
        $Mutex.Dispose()
        throw
    }
}

function Publish-AvidScriptModuleReleasePackage {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$RuntimeManifestPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [string]$ModuleId = '',
        [ValidateSet('Development', 'Shipping')][string]$Configuration = 'Development',
        [ValidateSet('Win64', 'Android')][string]$TargetPlatform = 'Win64',
        [string]$OutputRoot = ''
    )

    $PlatformValue = if ($TargetPlatform -ieq 'Android') { 'android' } else { 'win64' }
    $Architecture = if ($TargetPlatform -ieq 'Android') { 'arm64' } else { 'x86_64' }
    $TargetTriple = if ($TargetPlatform -ieq 'Android') {
        'aarch64-linux-android'
    }
    else {
        'x86_64-pc-windows-msvc'
    }
    $CpuFeatures = if ($TargetPlatform -ieq 'Android') { 'arm64-v8a' } else { 'x86-64-v3' }

    $ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot -ErrorAction Stop).Path
    $RuntimeManifestPath = (Resolve-Path -LiteralPath $RuntimeManifestPath -ErrorAction Stop).Path
    if (-not (Test-AvidScriptModuleReleasePathUnderRoot -Path $RuntimeManifestPath -Root $ProjectRoot)) {
        throw 'Runtime manifest must remain inside the project root.'
    }
    $ExpectedOutputRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $ProjectRoot 'Content/AvidScript/Modules'))
    if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
        $OutputRoot = $ExpectedOutputRoot
    }
    else {
        $OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
    }
    if (-not $OutputRoot.Equals($ExpectedOutputRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'Module release output root must be ProjectRoot/Content/AvidScript/Modules.'
    }

    $RuntimeManifest = Read-AvidScriptModuleReleaseJsonObject `
        -Path $RuntimeManifestPath `
        -Label 'Runtime manifest'
    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $RuntimeManifest `
        -Label 'Runtime manifest' `
        -Required @('schema_version', 'module_id', 'abi_version', 'wasm', 'execution', 'binding_package') `
        -Optional @(
            'language',
            'source',
            'compilation',
            'guest_ir',
            'semantic',
            'debug',
            'debug_map',
            'state_migration',
            'required_exports',
            'required_imports',
            'toolchain')
    if (-not (Test-AvidScriptModuleReleaseJsonInteger $RuntimeManifest.schema_version) -or
        [int64]$RuntimeManifest.schema_version -ne 1 -or
        -not (Test-AvidScriptModuleReleaseJsonInteger $RuntimeManifest.abi_version) -or
        [int64]$RuntimeManifest.abi_version -lt 1) {
        throw 'Runtime manifest schema_version and abi_version are invalid.'
    }
    if ($RuntimeManifest.PSObject.Properties.Name -ccontains 'language' -and
        [string]$RuntimeManifest.language -cne 'csharp') {
        throw 'Module release publication only accepts C# Runtime manifests.'
    }
    Assert-AvidScriptModuleReleaseManifestPaths -Value $RuntimeManifest -Label 'Runtime manifest'

    $ManifestModuleId = Normalize-AvidScriptModuleReleaseModuleId ([string]$RuntimeManifest.module_id)
    if (-not [string]::IsNullOrWhiteSpace($ModuleId)) {
        $RequestedModuleId = Normalize-AvidScriptModuleReleaseModuleId $ModuleId
        if ($RequestedModuleId -cne $ManifestModuleId) {
            throw 'Requested module id does not match Runtime manifest module_id.'
        }
    }
    $ModuleId = $ManifestModuleId

    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $RuntimeManifest.wasm `
        -Label 'Runtime manifest.wasm' `
        -Required @('file', 'sha256')
    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $RuntimeManifest.execution `
        -Label 'Runtime manifest.execution' `
        -Required @(
            'format',
            'file',
            'sha256',
            'canonical_sha256',
            'compiler_build_identity',
            'target_triple',
            'policy') `
        -Optional @('backend', 'cpu_features', 'attestation_id', 'fallback')
    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $RuntimeManifest.binding_package `
        -Label 'Runtime manifest.binding_package' `
        -Required @('manifest_file', 'manifest_sha256', 'descriptor_file', 'descriptor_sha256') `
        -Optional @(
            'package_name',
            'package_hash',
            'reference_source_file',
            'reference_source_sha256',
            'profile_import_count',
            'used_import_count',
            'used_imports',
            'used_object_type_count',
            'used_object_type_ordinals')
    if ([string]$RuntimeManifest.execution.format -cne 'wasmtime_serialized_v1' -or
        ($RuntimeManifest.execution.PSObject.Properties.Name -ccontains 'backend' -and
            [string]$RuntimeManifest.execution.backend -cne 'wasmtime') -or
        ($RuntimeManifest.execution.PSObject.Properties.Name -ccontains 'cpu_features' -and
            [string]$RuntimeManifest.execution.cpu_features -cne $CpuFeatures) -or
        @('require_precompiled', 'prefer_precompiled') -cnotcontains [string]$RuntimeManifest.execution.policy -or
        [string]::IsNullOrWhiteSpace([string]$RuntimeManifest.execution.compiler_build_identity) -or
        [string]$RuntimeManifest.execution.target_triple -cne $TargetTriple -or
        ($TargetPlatform -ieq 'Android' -and
            [string]$RuntimeManifest.execution.policy -cne 'require_precompiled')) {
        throw 'Runtime manifest execution contract is invalid.'
    }
    if ([string]$RuntimeManifest.execution.canonical_sha256 -cne [string]$RuntimeManifest.wasm.sha256) {
        throw 'Runtime manifest execution canonical_sha256 does not match wasm.sha256.'
    }
    $ConfigurationValue = $Configuration.ToLowerInvariant()
    $Policy = if ($TargetPlatform -ieq 'Android' -or
        $ConfigurationValue -ceq 'shipping') {
        'require_precompiled'
    }
    else {
        'prefer_precompiled'
    }

    if ($Policy -ceq 'prefer_precompiled' -and
        ([string]$RuntimeManifest.execution.attestation_id -cnotmatch '^[0-9a-f]{32}$' -or
            [string]$RuntimeManifest.execution.fallback -cne 'wasmtime_jit')) {
        throw 'Development precompiled execution requires a lowercase 32-hex attestation_id and wasmtime_jit fallback.'
    }

    $WasmPath = Resolve-AvidScriptModuleReleaseDependency `
        -RuntimeManifestPath $RuntimeManifestPath `
        -RelativePath ([string]$RuntimeManifest.wasm.file) `
        -ProjectRoot $ProjectRoot `
        -Label 'Runtime manifest.wasm.file'
    $PrecompiledPath = Resolve-AvidScriptModuleReleaseDependency `
        -RuntimeManifestPath $RuntimeManifestPath `
        -RelativePath ([string]$RuntimeManifest.execution.file) `
        -ProjectRoot $ProjectRoot `
        -Label 'Runtime manifest.execution.file'
    $BindingManifestPath = Resolve-AvidScriptModuleReleaseDependency `
        -RuntimeManifestPath $RuntimeManifestPath `
        -RelativePath ([string]$RuntimeManifest.binding_package.manifest_file) `
        -ProjectRoot $ProjectRoot `
        -Label 'Runtime manifest.binding_package.manifest_file'
    $BindingDescriptorPath = Resolve-AvidScriptModuleReleaseDependency `
        -RuntimeManifestPath $RuntimeManifestPath `
        -RelativePath ([string]$RuntimeManifest.binding_package.descriptor_file) `
        -ProjectRoot $ProjectRoot `
        -Label 'Runtime manifest.binding_package.descriptor_file'
    Assert-AvidScriptModuleReleaseDependencyHash `
        -Path $WasmPath `
        -ExpectedSha256 ([string]$RuntimeManifest.wasm.sha256) `
        -Label 'canonical WASM'
    Assert-AvidScriptModuleReleaseDependencyHash `
        -Path $PrecompiledPath `
        -ExpectedSha256 ([string]$RuntimeManifest.execution.sha256) `
        -Label 'precompiled artifact'
    Assert-AvidScriptModuleReleaseDependencyHash `
        -Path $BindingManifestPath `
        -ExpectedSha256 ([string]$RuntimeManifest.binding_package.manifest_sha256) `
        -Label 'binding manifest'
    Assert-AvidScriptModuleReleaseDependencyHash `
        -Path $BindingDescriptorPath `
        -ExpectedSha256 ([string]$RuntimeManifest.binding_package.descriptor_sha256) `
        -Label 'binding descriptor'

    $BindingManifest = Read-AvidScriptModuleReleaseJsonObject `
        -Path $BindingManifestPath `
        -Label 'Binding manifest'
    $BindingDescriptor = Read-AvidScriptModuleReleaseJsonObject `
        -Path $BindingDescriptorPath `
        -Label 'Binding descriptor'
    Assert-AvidScriptModuleReleaseManifestPaths `
        -Value $BindingDescriptor `
        -Label 'Binding descriptor'
    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $BindingManifest `
        -Label 'Binding manifest' `
        -Required @('schema_version', 'descriptor_sha256', 'files') `
        -Optional @(
            'emitter_version',
            'package_name',
            'package_hash',
            'descriptor_schema_version',
            'delegate_event_count',
            'inbound_handler_count',
            'class_reference_count',
            'object_factory_count',
            'reference_source_sha256',
            'required_imports')
    Assert-AvidScriptModuleReleaseManifestPaths `
        -Value $BindingManifest `
        -Label 'Binding manifest'
    if (-not (Test-AvidScriptModuleReleaseJsonInteger $BindingManifest.schema_version) -or
        [int64]$BindingManifest.schema_version -ne 1 -or
        [string]$BindingManifest.descriptor_sha256 -cne
            [string]$RuntimeManifest.binding_package.descriptor_sha256) {
        throw 'Binding manifest schema or descriptor identity is invalid.'
    }
    Assert-AvidScriptModuleReleaseObjectShape `
        -Value $BindingManifest.files `
        -Label 'Binding manifest.files' `
        -Required @('descriptor') `
        -Optional @('reference_source')
    $DescriptorFromBinding = Resolve-AvidScriptModuleReleaseDependency `
        -RuntimeManifestPath $BindingManifestPath `
        -RelativePath ([string]$BindingManifest.files.descriptor) `
        -ProjectRoot $ProjectRoot `
        -Label 'Binding manifest.files.descriptor'
    if (-not $DescriptorFromBinding.Equals(
            $BindingDescriptorPath,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'Binding manifest files.descriptor does not match the Runtime manifest dependency.'
    }
    $HasReferenceFile = $BindingManifest.files.PSObject.Properties.Name -ccontains 'reference_source'
    $HasReferenceHash = $BindingManifest.PSObject.Properties.Name -ccontains 'reference_source_sha256'
    $RuntimeHasReferenceFile = $RuntimeManifest.binding_package.PSObject.Properties.Name -ccontains 'reference_source_file'
    $RuntimeHasReferenceHash = $RuntimeManifest.binding_package.PSObject.Properties.Name -ccontains 'reference_source_sha256'
    if ($HasReferenceFile -or $HasReferenceHash -or $RuntimeHasReferenceFile -or $RuntimeHasReferenceHash) {
        if (-not ($HasReferenceFile -and $HasReferenceHash -and
                $RuntimeHasReferenceFile -and $RuntimeHasReferenceHash)) {
            throw 'Binding reference source dependency is incomplete.'
        }
        if ([string]$BindingManifest.reference_source_sha256 -cne
            [string]$RuntimeManifest.binding_package.reference_source_sha256) {
            throw 'Binding reference source identities do not match.'
        }
        $ReferenceSourcePath = Resolve-AvidScriptModuleReleaseDependency `
            -RuntimeManifestPath $BindingManifestPath `
            -RelativePath ([string]$BindingManifest.files.reference_source) `
            -ProjectRoot $ProjectRoot `
            -Label 'Binding manifest.files.reference_source'
        $RuntimeReferenceSourcePath = Resolve-AvidScriptModuleReleaseDependency `
            -RuntimeManifestPath $RuntimeManifestPath `
            -RelativePath ([string]$RuntimeManifest.binding_package.reference_source_file) `
            -ProjectRoot $ProjectRoot `
            -Label 'Runtime manifest.binding_package.reference_source_file'
        if (-not $ReferenceSourcePath.Equals(
                $RuntimeReferenceSourcePath,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'Binding reference source paths do not match.'
        }
        Assert-AvidScriptModuleReleaseDependencyHash `
            -Path $ReferenceSourcePath `
            -ExpectedSha256 ([string]$BindingManifest.reference_source_sha256) `
            -Label 'binding reference source'
    }

    $DebugMapPath = ''
    $IncludeDebugMap = $ConfigurationValue -ceq 'development' -and
        $RuntimeManifest.PSObject.Properties.Name -ccontains 'debug_map'
    if ($RuntimeManifest.PSObject.Properties.Name -ccontains 'debug_map') {
        Assert-AvidScriptModuleReleaseObjectShape `
            -Value $RuntimeManifest.debug_map `
            -Label 'Runtime manifest.debug_map' `
            -Required @('file', 'sha256') `
            -Optional @(
                'schema_version',
                'version',
                'module_id',
                'imported_function_count',
                'defined_function_count',
                'function_count')
        $DebugMapPath = Resolve-AvidScriptModuleReleaseDependency `
            -RuntimeManifestPath $RuntimeManifestPath `
            -RelativePath ([string]$RuntimeManifest.debug_map.file) `
            -ProjectRoot $ProjectRoot `
            -Label 'Runtime manifest.debug_map.file'
        Assert-AvidScriptModuleReleaseDependencyHash `
            -Path $DebugMapPath `
            -ExpectedSha256 ([string]$RuntimeManifest.debug_map.sha256) `
            -Label 'debug map'
        $DebugMap = Read-AvidScriptModuleReleaseJsonObject -Path $DebugMapPath -Label 'Debug map'
        Assert-AvidScriptModuleReleaseManifestPaths -Value $DebugMap -Label 'Debug map'
    }

    $ReleaseBindingManifest = Copy-AvidScriptModuleReleaseJsonObject $BindingManifest
    $ReleaseBindingManifest.files.descriptor = 'bindings.json'
    Remove-AvidScriptModuleReleaseProperty $ReleaseBindingManifest.files 'reference_source'
    Remove-AvidScriptModuleReleaseProperty $ReleaseBindingManifest 'reference_source_sha256'
    $ReleaseBindingManifestBytes = ConvertTo-AvidScriptModuleReleaseJsonBytes `
        -Value $ReleaseBindingManifest `
        -Canonical
    $ReleaseBindingManifestSha256 = Get-AvidScriptModuleReleaseBytesSha256 $ReleaseBindingManifestBytes

    $ReleaseRuntimeManifest = Copy-AvidScriptModuleReleaseJsonObject $RuntimeManifest
    $ReleaseRuntimeManifest.module_id = $ModuleId
    foreach ($PropertyName in @('source', 'guest_ir', 'semantic', 'debug')) {
        Remove-AvidScriptModuleReleaseProperty $ReleaseRuntimeManifest $PropertyName
    }
    $ReleaseRuntimeManifest.wasm = [pscustomobject][ordered]@{
        file = 'module.wasm'
        sha256 = [string]$RuntimeManifest.wasm.sha256
    }
    $ReleaseRuntimeManifest.execution = [pscustomobject][ordered]@{
        format = 'wasmtime_serialized_v1'
        file = 'module.wasmtime.cwasm'
        sha256 = [string]$RuntimeManifest.execution.sha256
        canonical_sha256 = [string]$RuntimeManifest.wasm.sha256
        compiler_build_identity = [string]$RuntimeManifest.execution.compiler_build_identity
        target_triple = [string]$RuntimeManifest.execution.target_triple
        cpu_features = $CpuFeatures
        policy = $Policy
    }
    if ($Policy -ceq 'prefer_precompiled') {
        $ReleaseRuntimeManifest.execution | Add-Member `
            -NotePropertyName attestation_id `
            -NotePropertyValue ([string]$RuntimeManifest.execution.attestation_id)
        $ReleaseRuntimeManifest.execution | Add-Member `
            -NotePropertyName fallback `
            -NotePropertyValue 'wasmtime_jit'
    }
    $ReleaseBindingPackage = Copy-AvidScriptModuleReleaseJsonObject $RuntimeManifest.binding_package
    $ReleaseBindingPackage.manifest_file = 'bindings/package.json'
    $ReleaseBindingPackage.manifest_sha256 = $ReleaseBindingManifestSha256
    $ReleaseBindingPackage.descriptor_file = 'bindings/bindings.json'
    Remove-AvidScriptModuleReleaseProperty $ReleaseBindingPackage 'reference_source_file'
    Remove-AvidScriptModuleReleaseProperty $ReleaseBindingPackage 'reference_source_sha256'
    $ReleaseRuntimeManifest.binding_package = $ReleaseBindingPackage
    if ($IncludeDebugMap) {
        Assert-AvidScriptModuleReleaseObjectShape `
            -Value $RuntimeManifest.source `
            -Label 'Runtime manifest.source debug provenance' `
            -Required @('file', 'sha256', 'frontend_sha256', 'semantic_sha256') `
            -Optional @(
                'script_type',
                'frontend_file',
                'frontend_schema_version',
                'frontend_version',
                'semantic_file',
                'semantic_schema_version',
                'semantic_version')
        Assert-AvidScriptModuleReleaseObjectShape `
            -Value $RuntimeManifest.guest_ir `
            -Label 'Runtime manifest.guest_ir debug provenance' `
            -Required @('module_id', 'sha256') `
            -Optional @('file', 'schema_version', 'version')
        foreach ($HashProperty in @('sha256', 'frontend_sha256', 'semantic_sha256')) {
            Assert-AvidScriptModuleReleaseSha256 `
                -Value ([string]$RuntimeManifest.source.$HashProperty) `
                -Label "Runtime manifest.source.$HashProperty"
        }
        Assert-AvidScriptModuleReleaseSha256 `
            -Value ([string]$RuntimeManifest.guest_ir.sha256) `
            -Label 'Runtime manifest.guest_ir.sha256'
        $ReleaseRuntimeManifest | Add-Member `
            -NotePropertyName source `
            -NotePropertyValue ([pscustomobject][ordered]@{
            file = [string]$RuntimeManifest.source.file
            sha256 = [string]$RuntimeManifest.source.sha256
            frontend_sha256 = [string]$RuntimeManifest.source.frontend_sha256
            semantic_sha256 = [string]$RuntimeManifest.source.semantic_sha256
        })
        $ReleaseRuntimeManifest | Add-Member `
            -NotePropertyName guest_ir `
            -NotePropertyValue ([pscustomobject][ordered]@{
            module_id = [string]$RuntimeManifest.guest_ir.module_id
            sha256 = [string]$RuntimeManifest.guest_ir.sha256
        })
        $ReleaseDebugMap = Copy-AvidScriptModuleReleaseJsonObject $RuntimeManifest.debug_map
        $ReleaseDebugMap.file = 'diagnostics/debug-map.json'
        $ReleaseRuntimeManifest.debug_map = $ReleaseDebugMap
    }
    else {
        Remove-AvidScriptModuleReleaseProperty $ReleaseRuntimeManifest 'debug_map'
    }
    $ReleaseRuntimeManifestBytes = ConvertTo-AvidScriptModuleReleaseJsonBytes `
        -Value $ReleaseRuntimeManifest `
        -Canonical
    $ReleaseRuntimeManifestSha256 = Get-AvidScriptModuleReleaseBytesSha256 $ReleaseRuntimeManifestBytes

    $Descriptor = [pscustomobject][ordered]@{
        schema_version = 1
        package_id = ''
        module_id = $ModuleId
        abi_version = [int64]$RuntimeManifest.abi_version
        platform = $PlatformValue
        configuration = $ConfigurationValue
        minimum_runtime_version = '0.1.0'
        execution = [pscustomobject][ordered]@{
            backend = 'wasmtime'
            format = 'wasmtime_serialized_v1'
            policy = $Policy
            compiler_build_identity = [string]$RuntimeManifest.execution.compiler_build_identity
            target_triple = [string]$RuntimeManifest.execution.target_triple
            cpu_features = $CpuFeatures
        }
        artifacts = [pscustomobject][ordered]@{
            runtime_manifest = [pscustomobject][ordered]@{
                file = 'runtime.avidscript.json'
                sha256 = $ReleaseRuntimeManifestSha256
            }
            canonical_wasm = [pscustomobject][ordered]@{
                file = 'module.wasm'
                sha256 = [string]$RuntimeManifest.wasm.sha256
            }
            precompiled = [pscustomobject][ordered]@{
                file = 'module.wasmtime.cwasm'
                sha256 = [string]$RuntimeManifest.execution.sha256
            }
            binding_manifest = [pscustomobject][ordered]@{
                file = 'bindings/package.json'
                sha256 = $ReleaseBindingManifestSha256
            }
            binding_descriptor = [pscustomobject][ordered]@{
                file = 'bindings/bindings.json'
                sha256 = [string]$RuntimeManifest.binding_package.descriptor_sha256
            }
        }
    }
    if ($IncludeDebugMap) {
        $Descriptor.artifacts | Add-Member `
            -NotePropertyName debug_map `
            -NotePropertyValue ([pscustomobject][ordered]@{
                file = 'diagnostics/debug-map.json'
                sha256 = [string]$RuntimeManifest.debug_map.sha256
            })
    }
    $Descriptor.package_id = Get-AvidScriptModuleReleasePackageId $Descriptor
    Assert-AvidScriptModuleReleasePackageDescriptor $Descriptor
    $DescriptorBytes = ConvertTo-AvidScriptModuleReleaseJsonBytes -Value $Descriptor
    $DescriptorSha256 = Get-AvidScriptModuleReleaseBytesSha256 $DescriptorBytes

    [void][System.IO.Directory]::CreateDirectory($OutputRoot)
    $CatalogLock = Enter-AvidScriptModuleReleaseCatalogLock $OutputRoot
    try {
        $CatalogPath = Join-Path $OutputRoot 'catalog.json'
        $Catalog = Read-AvidScriptModuleReleaseCatalog `
            -CatalogPath $CatalogPath `
            -OutputRoot $OutputRoot

        $ModuleRoot = Join-Path $OutputRoot $ModuleId
        [void][System.IO.Directory]::CreateDirectory($ModuleRoot)
        $PackageRoot = Join-Path $ModuleRoot ([string]$Descriptor.package_id)
        if (Test-Path -LiteralPath $PackageRoot -PathType Container) {
            Assert-AvidScriptModuleReleasePackageDirectory `
                -PackageRoot $PackageRoot `
                -ExpectedDescriptorSha256 $DescriptorSha256 `
                -ExpectedModuleId $ModuleId `
                -ExpectedPackageId ([string]$Descriptor.package_id) | Out-Null
        }
        else {
            $TempPackageRoot = "$PackageRoot.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
            try {
                [void][System.IO.Directory]::CreateDirectory((Join-Path $TempPackageRoot 'bindings'))
                if ($IncludeDebugMap) {
                    [void][System.IO.Directory]::CreateDirectory((Join-Path $TempPackageRoot 'diagnostics'))
                }
                [System.IO.File]::WriteAllBytes(
                    (Join-Path $TempPackageRoot 'package.json'),
                    $DescriptorBytes)
                [System.IO.File]::WriteAllBytes(
                    (Join-Path $TempPackageRoot 'runtime.avidscript.json'),
                    $ReleaseRuntimeManifestBytes)
                [System.IO.File]::WriteAllBytes(
                    (Join-Path $TempPackageRoot 'bindings/package.json'),
                    $ReleaseBindingManifestBytes)
                Copy-Item -LiteralPath $WasmPath -Destination (Join-Path $TempPackageRoot 'module.wasm')
                Copy-Item -LiteralPath $PrecompiledPath -Destination (Join-Path $TempPackageRoot 'module.wasmtime.cwasm')
                Copy-Item -LiteralPath $BindingDescriptorPath -Destination (Join-Path $TempPackageRoot 'bindings/bindings.json')
                if ($IncludeDebugMap) {
                    Copy-Item -LiteralPath $DebugMapPath -Destination (Join-Path $TempPackageRoot 'diagnostics/debug-map.json')
                }
                Move-Item -LiteralPath $TempPackageRoot -Destination $PackageRoot -ErrorAction Stop
            }
            finally {
                if (Test-Path -LiteralPath $TempPackageRoot -PathType Container) {
                    Remove-Item -LiteralPath $TempPackageRoot -Recurse -Force
                }
            }
            Assert-AvidScriptModuleReleasePackageDirectory `
                -PackageRoot $PackageRoot `
                -ExpectedDescriptorSha256 $DescriptorSha256 `
                -ExpectedModuleId $ModuleId `
                -ExpectedPackageId ([string]$Descriptor.package_id) | Out-Null
        }

        $NewVariant = [pscustomobject][ordered]@{
            platform = $PlatformValue
            architecture = $Architecture
            configuration = $ConfigurationValue
            backend = 'wasmtime'
            format = 'wasmtime_serialized_v1'
            package_id = [string]$Descriptor.package_id
            descriptor_file = "$ModuleId/$($Descriptor.package_id)/package.json"
            descriptor_sha256 = $DescriptorSha256
        }
        $NewVariantKey = Get-AvidScriptModuleReleaseVariantKey $NewVariant
        $Modules = @($Catalog.modules | Where-Object {
                [string]$_.module_id -cne $ModuleId
            })
        $ExistingModule = @($Catalog.modules | Where-Object {
                [string]$_.module_id -ceq $ModuleId
            })
        $Variants = @(if ($ExistingModule.Count -eq 1) {
                $ExistingModule[0].variants | Where-Object {
                    (Get-AvidScriptModuleReleaseVariantKey $_) -cne $NewVariantKey
                }
            })
        $Variants += $NewVariant
        $Modules += [pscustomobject][ordered]@{
            module_id = $ModuleId
            variants = @(Sort-AvidScriptModuleReleaseCatalogVariants $Variants)
        }
        $SortedModules = Sort-AvidScriptModuleReleaseCatalogModules $Modules
        $NewCatalog = [pscustomobject][ordered]@{
            schema_version = 2
            modules = @($SortedModules)
        }
        $CatalogBytes = ConvertTo-AvidScriptModuleReleaseJsonBytes -Value $NewCatalog
        $CatalogTempPath = "$CatalogPath.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
        try {
            [System.IO.File]::WriteAllBytes($CatalogTempPath, $CatalogBytes)
            [System.IO.File]::Move($CatalogTempPath, $CatalogPath, $true)
        }
        finally {
            if (Test-Path -LiteralPath $CatalogTempPath -PathType Leaf) {
                Remove-Item -LiteralPath $CatalogTempPath -Force
            }
        }
    }
    finally {
        try {
            $CatalogLock.ReleaseMutex()
        }
        finally {
            $CatalogLock.Dispose()
        }
    }

    $ExpectedFiles = Get-AvidScriptModuleReleaseExpectedFiles $Descriptor
    return [pscustomobject][ordered]@{
        ModuleId = $ModuleId
        PackageId = [string]$Descriptor.package_id
        PackageRoot = $PackageRoot
        DescriptorPath = Join-Path $PackageRoot 'package.json'
        DescriptorSha256 = $DescriptorSha256
        CatalogPath = $CatalogPath
        Configuration = $ConfigurationValue
        Platform = $PlatformValue
        Architecture = $Architecture
        TargetTriple = $TargetTriple
        FileCount = $ExpectedFiles.Count
    }
}
