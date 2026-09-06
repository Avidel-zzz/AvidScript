$script:AvidScriptPluginReleaseUtf8 = [System.Text.UTF8Encoding]::new($false)
$script:AvidScriptPluginReleaseStrictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
$script:AvidScriptPluginReleaseModuleRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Throw-AvidScriptPluginReleaseError {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $Exception = [System.InvalidOperationException]::new("$Code $Message")
    $Exception.Data['code'] = $Code
    $Exception.Data['category'] = $Category
    throw $Exception
}

function Get-AvidScriptPluginReleaseSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Get-AvidScriptPluginReleaseBytesSha256 {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    $Hash = [System.Security.Cryptography.SHA256]::HashData($Bytes)
    return [System.Convert]::ToHexString($Hash).ToLowerInvariant()
}

function Get-AvidScriptPluginReleasePropertyNames {
    param([Parameter(Mandatory = $true)]$Value)

    return @($Value.PSObject.Properties | ForEach-Object { $_.Name })
}

function Assert-AvidScriptPluginReleaseObjectShape {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string[]]$Required,
        [string[]]$Optional = @(),
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($null -eq $Value -or $Value -is [string] -or
        $Value -is [System.Collections.IEnumerable] -and
        $Value -isnot [System.Collections.IDictionary] -and
        $Value.PSObject.Properties.Count -eq 0) {
        Throw-AvidScriptPluginReleaseError 'ASRE1001' 'schema_invalid' "$Label must be an object."
    }
    $Names = @(Get-AvidScriptPluginReleasePropertyNames $Value)
    foreach ($Name in $Required) {
        if ($Names -cnotcontains $Name) {
            Throw-AvidScriptPluginReleaseError 'ASRE1002' 'schema_invalid' "$Label is missing '$Name'."
        }
    }
    foreach ($Name in $Names) {
        if ($Required -cnotcontains $Name -and $Optional -cnotcontains $Name) {
            Throw-AvidScriptPluginReleaseError 'ASRE1003' 'schema_invalid' "$Label contains unsupported property '$Name'."
        }
    }
}

function Assert-AvidScriptPluginReleaseNoDuplicateProperties {
    param(
        [Parameter(Mandatory = $true)][System.Text.Json.JsonElement]$Element,
        [string]$Path = '$'
    )

    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $Names = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($Property in $Element.EnumerateObject()) {
            if (-not $Names.Add($Property.Name)) {
                Throw-AvidScriptPluginReleaseError `
                    'ASRE1004' `
                    'schema_invalid' `
                    "duplicate JSON property at ${Path}: $($Property.Name)"
            }
            Assert-AvidScriptPluginReleaseNoDuplicateProperties `
                -Element $Property.Value `
                -Path "${Path}.$($Property.Name)"
        }
    }
    elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        $Index = 0
        foreach ($Item in $Element.EnumerateArray()) {
            Assert-AvidScriptPluginReleaseNoDuplicateProperties `
                -Element $Item `
                -Path "${Path}[$Index]"
            $Index++
        }
    }
}

function Read-AvidScriptPluginReleaseJsonObject {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Throw-AvidScriptPluginReleaseError 'ASRE1005' 'input_missing' "$Label does not exist: $Path"
    }
    try {
        $Reader = [System.IO.StreamReader]::new(
            $Path,
            $script:AvidScriptPluginReleaseStrictUtf8,
            $true)
        try {
            $Text = $Reader.ReadToEnd()
        }
        finally {
            $Reader.Dispose()
        }
    }
    catch {
        Throw-AvidScriptPluginReleaseError 'ASRE1006' 'encoding_invalid' "$Label is not strict UTF-8."
    }
    try {
        $Document = [System.Text.Json.JsonDocument]::Parse($Text)
        try {
            Assert-AvidScriptPluginReleaseNoDuplicateProperties -Element $Document.RootElement
        }
        finally {
            $Document.Dispose()
        }
        return $Text | ConvertFrom-Json -Depth 64 -DateKind String
    }
    catch {
        if ($_.Exception.Data.Contains('code')) {
            throw
        }
        Throw-AvidScriptPluginReleaseError 'ASRE1007' 'schema_invalid' "$Label is not valid JSON."
    }
}

function Assert-AvidScriptPluginReleaseJsonSchema {
    param(
        [Parameter(Mandatory = $true)][string]$JsonPath,
        [Parameter(Mandatory = $true)][string]$SchemaPath,
        [Parameter(Mandatory = $true)][string]$Label
    )

    try {
        $Raw = [System.IO.File]::ReadAllText(
            $JsonPath,
            $script:AvidScriptPluginReleaseStrictUtf8)
        $Valid = [bool]($Raw | Test-Json -SchemaFile $SchemaPath -ErrorAction Stop)
    }
    catch {
        Throw-AvidScriptPluginReleaseError 'ASRE1008' 'schema_invalid' "$Label does not satisfy its JSON schema."
    }
    if (-not $Valid) {
        Throw-AvidScriptPluginReleaseError 'ASRE1008' 'schema_invalid' "$Label does not satisfy its JSON schema."
    }
}

function ConvertTo-AvidScriptPluginReleaseCanonicalValue {
    param([AllowNull()]$Value)

    if ($null -eq $Value -or $Value -is [string] -or
        $Value -is [bool] -or $Value -is [ValueType]) {
        return $Value
    }
    if ($Value -is [System.Collections.IDictionary]) {
        $Result = [ordered]@{}
        foreach ($Key in @($Value.Keys | ForEach-Object { [string]$_ } | Sort-Object -CaseSensitive)) {
            $Result[$Key] = ConvertTo-AvidScriptPluginReleaseCanonicalValue $Value[$Key]
        }
        return $Result
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        $Items = @($Value | ForEach-Object {
                ConvertTo-AvidScriptPluginReleaseCanonicalValue $_
            })
        return , $Items
    }

    $Object = [ordered]@{}
    foreach ($Property in @($Value.PSObject.Properties | Sort-Object Name -CaseSensitive)) {
        $Object[$Property.Name] = ConvertTo-AvidScriptPluginReleaseCanonicalValue $Property.Value
    }
    return $Object
}

function ConvertTo-AvidScriptPluginReleaseCanonicalJsonBytes {
    param([Parameter(Mandatory = $true)]$Value)

    $Canonical = ConvertTo-AvidScriptPluginReleaseCanonicalValue $Value
    $Json = $Canonical | ConvertTo-Json -Depth 64 -Compress
    return $script:AvidScriptPluginReleaseUtf8.GetBytes($Json)
}

function Write-AvidScriptPluginReleaseJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $Json = (ConvertTo-AvidScriptPluginReleaseCanonicalValue $Value) |
        ConvertTo-Json -Depth 64
    $Json = $Json -replace "`r`n?", "`n"
    [System.IO.File]::WriteAllText($Path, $Json + "`n", $script:AvidScriptPluginReleaseUtf8)
}

function Normalize-AvidScriptPluginReleaseRelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$Label = 'path'
    )

    $Value = $Path.Normalize([System.Text.NormalizationForm]::FormC).Replace('\', '/')
    if ([string]::IsNullOrWhiteSpace($Value) -or
        [System.IO.Path]::IsPathRooted($Value) -or
        $Value.Contains(':') -or
        $Value.Contains('//') -or
        $Value.StartsWith('/') -or
        $Value.EndsWith('/') -or $Value.Length -gt 240) {
        Throw-AvidScriptPluginReleaseError 'ASRE1101' 'path_invalid' "$Label is not a portable relative path: $Path"
    }
    foreach ($Segment in @($Value -split '/')) {
        if ($Segment -in @('', '.', '..') -or
            $Segment.EndsWith('.') -or
            $Segment.EndsWith(' ') -or
            $Segment -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?$' -or
            $Segment.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0) {
            Throw-AvidScriptPluginReleaseError 'ASRE1102' 'path_invalid' "$Label contains an invalid segment: $Path"
        }
    }
    return $Value
}

function Test-AvidScriptPluginReleasePathUnderRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $FullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    return $FullPath.Equals($FullRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
        $FullPath.StartsWith(
            $FullRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-AvidScriptPluginReleaseOrdinaryTree {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        Throw-AvidScriptPluginReleaseError 'ASRE1103' 'input_missing' "$Label does not exist: $Root"
    }
    if (((Get-Item -LiteralPath $Root -Force).Attributes -band
            [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        Throw-AvidScriptPluginReleaseError 'ASRE1104' 'reparse_point' "$Label must not be a reparse point: $Root"
    }
    foreach ($Item in @(Get-ChildItem -LiteralPath $Root -Force -Recurse)) {
        if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            Throw-AvidScriptPluginReleaseError 'ASRE1104' 'reparse_point' "$Label contains a reparse point: $($Item.FullName)"
        }
        if (-not $Item.PSIsContainer) {
            $AlternateStreams = @(Get-Item -LiteralPath $Item.FullName -Stream * |
                Where-Object { $_.Stream -cne ':$DATA' })
            if ($AlternateStreams.Count -gt 0) {
                Throw-AvidScriptPluginReleaseError 'ASRE1106' 'alternate_data_stream' "$Label contains alternate data streams: $($Item.FullName)"
            }
        }
    }
}

function Get-AvidScriptPluginReleaseRole {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    $PluginPath = if ($RelativePath.StartsWith('AvidScript/', [System.StringComparison]::Ordinal)) {
        $RelativePath.Substring('AvidScript/'.Length)
    }
    else {
        $RelativePath
    }
    if ($PluginPath.StartsWith('Source/ThirdParty/', [System.StringComparison]::Ordinal)) {
        return 'third_party'
    }
    if ($PluginPath.StartsWith('Source/', [System.StringComparison]::Ordinal)) {
        return 'native_source'
    }
    if ($PluginPath.StartsWith('Tools/', [System.StringComparison]::Ordinal)) {
        return 'managed_source'
    }
    if ($PluginPath.StartsWith('Build/', [System.StringComparison]::Ordinal)) {
        return 'tooling'
    }
    if ($PluginPath.StartsWith('Content/', [System.StringComparison]::Ordinal)) {
        return 'content'
    }
    if ($PluginPath.StartsWith('Samples/', [System.StringComparison]::Ordinal) -or
        $PluginPath.StartsWith('Templates/', [System.StringComparison]::Ordinal) -or
        $PluginPath.StartsWith('Docs/', [System.StringComparison]::Ordinal) -or
        $PluginPath -in @('README.md', 'LICENSE')) {
        return 'documentation'
    }
    return 'metadata'
}

function Get-AvidScriptPluginReleaseInventory {
    param([Parameter(Mandatory = $true)][string]$PayloadRoot)

    $PayloadRoot = [System.IO.Path]::GetFullPath($PayloadRoot)
    Assert-AvidScriptPluginReleaseOrdinaryTree -Root $PayloadRoot -Label 'payload root'
    $CaseNames = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $Entries = [System.Collections.Generic.List[object]]::new()
    foreach ($File in @(Get-ChildItem -LiteralPath $PayloadRoot -File -Force -Recurse)) {
        $Relative = Normalize-AvidScriptPluginReleaseRelativePath `
            ([System.IO.Path]::GetRelativePath($PayloadRoot, $File.FullName)) `
            'payload file'
        if (-not $CaseNames.Add($Relative)) {
            Throw-AvidScriptPluginReleaseError 'ASRE1105' 'path_collision' "payload contains a case-insensitive path collision: $Relative"
        }
        if ($File.Length -gt 536870912) {
            Throw-AvidScriptPluginReleaseError 'ASRE1107' 'size_limit' "payload file exceeds 512 MiB: $Relative"
        }
        $Entries.Add([pscustomobject][ordered]@{
                path = $Relative
                length = [int64]$File.Length
                sha256 = Get-AvidScriptPluginReleaseSha256 $File.FullName
                role = Get-AvidScriptPluginReleaseRole $Relative
            })
    }
    if ($Entries.Count -gt 10000 -or
        [int64]($Entries | Measure-Object length -Sum).Sum -gt 2147483648) {
        Throw-AvidScriptPluginReleaseError 'ASRE1108' 'size_limit' 'payload exceeds the file-count or 2 GiB aggregate limit.'
    }
    return @($Entries | Sort-Object path -CaseSensitive)
}

function Get-AvidScriptPluginReleaseInventorySha256 {
    param([Parameter(Mandatory = $true)][object[]]$Files)

    return Get-AvidScriptPluginReleaseBytesSha256 `
        (ConvertTo-AvidScriptPluginReleaseCanonicalJsonBytes @($Files))
}

function Get-AvidScriptPluginReleaseId {
    param([Parameter(Mandatory = $true)]$Manifest)

    $Identity = [ordered]@{
        schema_version = [int]$Manifest.schema_version
        format = [string]$Manifest.format
        version = [string]$Manifest.version
        channel = [string]$Manifest.channel
        profile = [string]$Manifest.profile
        publisher = $Manifest.publisher
        source = $Manifest.source
        compatibility = $Manifest.compatibility
        targets = @($Manifest.targets)
        contracts = $Manifest.contracts
        artifact_contracts = @($Manifest.artifact_contracts)
        dependencies = @($Manifest.dependencies)
        payload = [ordered]@{
            root = [string]$Manifest.payload.root
            inventory_sha256 = [string]$Manifest.payload.inventory_sha256
            file_count = [int64]$Manifest.payload.file_count
            total_bytes = [int64]$Manifest.payload.total_bytes
        }
    }
    return Get-AvidScriptPluginReleaseBytesSha256 `
        (ConvertTo-AvidScriptPluginReleaseCanonicalJsonBytes $Identity)
}

function Copy-AvidScriptPluginReleasePayload {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$DestinationRoot
    )

    [System.IO.Directory]::CreateDirectory($DestinationRoot) | Out-Null
    foreach ($Directory in @(Get-ChildItem -LiteralPath $SourceRoot -Directory -Force -Recurse)) {
        $Relative = Normalize-AvidScriptPluginReleaseRelativePath `
            ([System.IO.Path]::GetRelativePath($SourceRoot, $Directory.FullName)) `
            'payload directory'
        [System.IO.Directory]::CreateDirectory((Join-Path $DestinationRoot $Relative)) | Out-Null
    }
    foreach ($File in @(Get-ChildItem -LiteralPath $SourceRoot -File -Force -Recurse)) {
        $Relative = Normalize-AvidScriptPluginReleaseRelativePath `
            ([System.IO.Path]::GetRelativePath($SourceRoot, $File.FullName)) `
            'payload file'
        $Destination = Join-Path $DestinationRoot $Relative
        [System.IO.Directory]::CreateDirectory((Split-Path -Parent $Destination)) | Out-Null
        [System.IO.File]::Copy($File.FullName, $Destination, $false)
    }
}

function Assert-AvidScriptPluginReleaseSha256 {
    param(
        [AllowEmptyString()][string]$Value,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Value -notmatch '^[0-9a-f]{64}$') {
        Throw-AvidScriptPluginReleaseError 'ASRE1201' 'identity_invalid' "$Label is not a lowercase SHA-256."
    }
}

function Assert-AvidScriptPluginReleaseManifest {
    param([Parameter(Mandatory = $true)]$Manifest)

    Assert-AvidScriptPluginReleaseObjectShape `
        -Value $Manifest `
        -Required @(
            'schema_version', 'format', 'release_id', 'version', 'channel', 'profile',
            'publisher', 'source', 'compatibility', 'targets', 'contracts',
            'artifact_contracts', 'dependencies', 'payload', 'signature') `
        -Label 'release manifest'
    if ([int]$Manifest.schema_version -ne 1 -or
        [string]$Manifest.format -cne 'avidscript.plugin.release' -or
        [string]$Manifest.profile -cne 'source-developer') {
        Throw-AvidScriptPluginReleaseError 'ASRE1202' 'schema_invalid' 'release manifest header is unsupported.'
    }
    if ([string]$Manifest.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -or
        [string]$Manifest.channel -cne 'preview') {
        Throw-AvidScriptPluginReleaseError 'ASRE1203' 'version_invalid' 'release version or channel is invalid.'
    }
    if ($null -ne $Manifest.signature) {
        Throw-AvidScriptPluginReleaseError 'ASRE1204' 'signature_unsupported' 'signed release envelopes are not enabled in schema v1.'
    }
    Assert-AvidScriptPluginReleaseObjectShape `
        -Value $Manifest.publisher `
        -Required @('name', 'version') `
        -Label 'release publisher'
    if ([string]$Manifest.publisher.name -cne 'AvidScript.PluginRelease' -or
        [string]$Manifest.publisher.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
        Throw-AvidScriptPluginReleaseError 'ASRE1205' 'identity_invalid' 'release publisher identity is invalid.'
    }
    Assert-AvidScriptPluginReleaseObjectShape `
        -Value $Manifest.source `
        -Required @('commit', 'tree', 'committed_at_utc') `
        -Label 'release source'
    foreach ($Name in @('commit', 'tree')) {
        if ([string]$Manifest.source.$Name -notmatch '^[0-9a-f]{40}$') {
            Throw-AvidScriptPluginReleaseError 'ASRE1206' 'identity_invalid' "source $Name is invalid."
        }
    }
    try {
        [void][DateTimeOffset]::ParseExact(
            [string]$Manifest.source.committed_at_utc,
            'o',
            [System.Globalization.CultureInfo]::InvariantCulture)
    }
    catch {
        Throw-AvidScriptPluginReleaseError 'ASRE1207' 'identity_invalid' 'source commit time is invalid.'
    }
    Assert-AvidScriptPluginReleaseObjectShape `
        -Value $Manifest.compatibility `
        -Required @('unreal_min', 'unreal_max_exclusive', 'dotnet_sdk', 'powershell_min') `
        -Label 'release compatibility'
    if ([string]$Manifest.compatibility.unreal_min -cne '5.8.0' -or
        [string]$Manifest.compatibility.unreal_max_exclusive -cne '5.9.0' -or
        [string]$Manifest.compatibility.dotnet_sdk -cne '8.0.416') {
        Throw-AvidScriptPluginReleaseError 'ASRE1208' 'compatibility_invalid' 'release compatibility identity drifted.'
    }
    $Targets = @($Manifest.targets)
    if ($Targets.Count -ne 2 -or $Targets[0] -cne 'Android-arm64' -or
        $Targets[1] -cne 'Win64') {
        Throw-AvidScriptPluginReleaseError 'ASRE1209' 'compatibility_invalid' 'release target set is invalid.'
    }
    Assert-AvidScriptPluginReleaseObjectShape `
        -Value $Manifest.contracts `
        -Required @(
            'module_abi', 'binding_descriptor_max', 'semantic_schema',
            'guest_ir_schema') `
        -Label 'release contracts'
    if ([int]$Manifest.contracts.module_abi -ne 1 -or
        [int]$Manifest.contracts.binding_descriptor_max -ne 23 -or
        [int]$Manifest.contracts.semantic_schema -ne 20 -or
        [int]$Manifest.contracts.guest_ir_schema -ne 2) {
        Throw-AvidScriptPluginReleaseError 'ASRE1210' 'compatibility_invalid' 'release contract identity drifted.'
    }
    $ArtifactContractIds = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($ArtifactContract in @($Manifest.artifact_contracts)) {
        Assert-AvidScriptPluginReleaseObjectShape `
            -Value $ArtifactContract `
            -Required @('id', 'producer_path', 'producer_sha256') `
            -Label 'release artifact contract'
        if ([string]$ArtifactContract.id -notmatch '^[a-z0-9][a-z0-9._-]{0,63}$' -or
            -not $ArtifactContractIds.Add([string]$ArtifactContract.id)) {
            Throw-AvidScriptPluginReleaseError 'ASRE1222' 'artifact_contract_invalid' 'release artifact contract identity is invalid or duplicated.'
        }
        Normalize-AvidScriptPluginReleaseRelativePath `
            ([string]$ArtifactContract.producer_path) `
            'artifact contract producer path' | Out-Null
        Assert-AvidScriptPluginReleaseSha256 `
            ([string]$ArtifactContract.producer_sha256) `
            'artifact contract producer hash'
    }
    foreach ($RequiredId in @('generated-type-package', 'module-release-package')) {
        if (-not $ArtifactContractIds.Contains($RequiredId)) {
            Throw-AvidScriptPluginReleaseError 'ASRE1223' 'artifact_contract_invalid' "release artifact contract is missing: $RequiredId"
        }
    }
    $DependencyIds = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Dependency in @($Manifest.dependencies)) {
        Assert-AvidScriptPluginReleaseObjectShape `
            -Value $Dependency `
            -Required @('id', 'version', 'mode', 'identity_path', 'identity_sha256') `
            -Label 'release dependency'
        if ([string]$Dependency.id -notmatch '^[a-z0-9][a-z0-9._-]{0,63}$' -or
            -not $DependencyIds.Add([string]$Dependency.id)) {
            Throw-AvidScriptPluginReleaseError 'ASRE1211' 'dependency_invalid' 'release dependency identity is invalid or duplicated.'
        }
        if ([string]$Dependency.mode -cne 'external') {
            Throw-AvidScriptPluginReleaseError 'ASRE1212' 'dependency_invalid' 'schema v1 only permits external thin-package dependencies.'
        }
        Normalize-AvidScriptPluginReleaseRelativePath ([string]$Dependency.identity_path) 'dependency identity path' | Out-Null
        Assert-AvidScriptPluginReleaseSha256 ([string]$Dependency.identity_sha256) 'dependency identity hash'
    }
    Assert-AvidScriptPluginReleaseObjectShape `
        -Value $Manifest.payload `
        -Required @('root', 'inventory_sha256', 'file_count', 'total_bytes', 'files') `
        -Label 'release payload'
    if ([string]$Manifest.payload.root -cne 'payload' -or
        [int64]$Manifest.payload.file_count -lt 1 -or
        [int64]$Manifest.payload.total_bytes -lt 1 -or
        @($Manifest.payload.files).Count -ne [int64]$Manifest.payload.file_count) {
        Throw-AvidScriptPluginReleaseError 'ASRE1213' 'inventory_invalid' 'release payload summary is invalid.'
    }
    Assert-AvidScriptPluginReleaseSha256 ([string]$Manifest.payload.inventory_sha256) 'inventory hash'
    Assert-AvidScriptPluginReleaseSha256 ([string]$Manifest.release_id) 'release id'
    if ((Get-AvidScriptPluginReleaseId $Manifest) -cne [string]$Manifest.release_id) {
        Throw-AvidScriptPluginReleaseError 'ASRE1214' 'identity_invalid' 'release id does not match manifest identity.'
    }
}

function Resolve-AvidScriptPluginReleasePackage {
    param([Parameter(Mandatory = $true)][string]$PackageRoot)

    $PackageRoot = [System.IO.Path]::GetFullPath($PackageRoot)
    Assert-AvidScriptPluginReleaseOrdinaryTree -Root $PackageRoot -Label 'release package'
    $ManifestPath = Join-Path $PackageRoot 'release.json'
    $PayloadRoot = Join-Path $PackageRoot 'payload'
    $Manifest = Read-AvidScriptPluginReleaseJsonObject $ManifestPath 'release manifest'
    Assert-AvidScriptPluginReleaseJsonSchema `
        -JsonPath $ManifestPath `
        -SchemaPath (Join-Path $script:AvidScriptPluginReleaseModuleRoot 'AvidScriptPluginRelease.schema.json') `
        -Label 'release manifest'
    Assert-AvidScriptPluginReleaseManifest $Manifest
    if (-not (Test-Path -LiteralPath $PayloadRoot -PathType Container)) {
        Throw-AvidScriptPluginReleaseError 'ASRE1215' 'input_missing' 'release payload directory is missing.'
    }
    $RootEntries = @(Get-ChildItem -LiteralPath $PackageRoot -Force)
    if ($RootEntries.Count -ne 2 -or
        @($RootEntries | Where-Object { $_.Name -notin @('payload', 'release.json') }).Count -gt 0) {
        Throw-AvidScriptPluginReleaseError 'ASRE1216' 'inventory_invalid' 'release package root contains undeclared entries.'
    }

    $ActualFiles = @(Get-AvidScriptPluginReleaseInventory $PayloadRoot)
    $ExpectedFiles = @($Manifest.payload.files)
    if ($ActualFiles.Count -ne $ExpectedFiles.Count) {
        Throw-AvidScriptPluginReleaseError 'ASRE1217' 'inventory_invalid' 'release payload file count differs.'
    }
    for ($Index = 0; $Index -lt $ActualFiles.Count; $Index++) {
        $Expected = $ExpectedFiles[$Index]
        Assert-AvidScriptPluginReleaseObjectShape `
            -Value $Expected `
            -Required @('path', 'length', 'sha256', 'role') `
            -Label 'release inventory file'
        $Actual = $ActualFiles[$Index]
        if ([string]$Expected.path -cne [string]$Actual.path -or
            [int64]$Expected.length -ne [int64]$Actual.length -or
            [string]$Expected.sha256 -cne [string]$Actual.sha256 -or
            [string]$Expected.role -cne [string]$Actual.role) {
            Throw-AvidScriptPluginReleaseError 'ASRE1218' 'inventory_invalid' "release payload file differs: $($Actual.path)"
        }
    }
    $InventoryHash = Get-AvidScriptPluginReleaseInventorySha256 $ActualFiles
    if ($InventoryHash -cne [string]$Manifest.payload.inventory_sha256 -or
        [int64]($ActualFiles | Measure-Object length -Sum).Sum -ne [int64]$Manifest.payload.total_bytes) {
        Throw-AvidScriptPluginReleaseError 'ASRE1219' 'inventory_invalid' 'release payload aggregate differs.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $PayloadRoot 'AvidScript\AvidScript.uplugin') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $PayloadRoot 'AvidScript\LICENSE') -PathType Leaf)) {
        Throw-AvidScriptPluginReleaseError 'ASRE1220' 'layout_invalid' 'release payload lacks plugin descriptor or license.'
    }
    foreach ($Dependency in @($Manifest.dependencies)) {
        $IdentityPath = Join-Path $PayloadRoot ([string]$Dependency.identity_path)
        if (-not (Test-Path -LiteralPath $IdentityPath -PathType Leaf) -or
            (Get-AvidScriptPluginReleaseSha256 $IdentityPath) -cne [string]$Dependency.identity_sha256) {
            Throw-AvidScriptPluginReleaseError 'ASRE1221' 'dependency_invalid' "release dependency identity differs: $($Dependency.id)"
        }
    }
    foreach ($ArtifactContract in @($Manifest.artifact_contracts)) {
        $ProducerPath = Join-Path $PayloadRoot ([string]$ArtifactContract.producer_path)
        if (-not (Test-Path -LiteralPath $ProducerPath -PathType Leaf) -or
            (Get-AvidScriptPluginReleaseSha256 $ProducerPath) -cne
            [string]$ArtifactContract.producer_sha256) {
            Throw-AvidScriptPluginReleaseError 'ASRE1224' 'artifact_contract_invalid' "release artifact producer identity differs: $($ArtifactContract.id)"
        }
    }
    return [pscustomobject][ordered]@{
        Root = $PackageRoot
        ManifestPath = $ManifestPath
        ManifestSha256 = Get-AvidScriptPluginReleaseSha256 $ManifestPath
        PayloadRoot = $PayloadRoot
        PluginRoot = Join-Path $PayloadRoot 'AvidScript'
        Manifest = $Manifest
    }
}

function Enter-AvidScriptPluginReleasePublishLock {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [ValidateRange(1, 300)][int]$TimeoutSeconds = 30
    )

    $Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        try {
            return [System.IO.File]::Open(
                $Path,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None)
        }
        catch [System.IO.IOException] {
            if ([DateTime]::UtcNow -ge $Deadline) {
                Throw-AvidScriptPluginReleaseError 'ASRE1304' 'lock_timeout' 'timed out waiting for the release publisher lock.'
            }
            Start-Sleep -Milliseconds 50
        }
    } while ($true)
}

function Publish-AvidScriptPluginReleasePackage {
    param(
        [Parameter(Mandatory = $true)][string]$PayloadSourceRoot,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$Version,
        [ValidateSet('preview')][string]$Channel = 'preview',
        [Parameter(Mandatory = $true)][string]$Commit,
        [Parameter(Mandatory = $true)][string]$Tree,
        [Parameter(Mandatory = $true)][string]$CommittedAtUtc,
        [Parameter(Mandatory = $true)][object[]]$ArtifactContracts,
        [Parameter(Mandatory = $true)][object[]]$Dependencies,
        [string]$PublisherVersion = '65.2.0',
        [string[]]$Targets = @('Android-arm64', 'Win64'),
        $Contracts = $null
    )

    $PayloadSourceRoot = [System.IO.Path]::GetFullPath($PayloadSourceRoot)
    $OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
    Assert-AvidScriptPluginReleaseOrdinaryTree -Root $PayloadSourceRoot -Label 'payload source'
    if (-not (Test-Path -LiteralPath (Join-Path $PayloadSourceRoot 'AvidScript\AvidScript.uplugin') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $PayloadSourceRoot 'AvidScript\LICENSE') -PathType Leaf)) {
        Throw-AvidScriptPluginReleaseError 'ASRE1301' 'layout_invalid' 'payload source lacks AvidScript descriptor or license.'
    }
    if (Test-AvidScriptPluginReleasePathUnderRoot $OutputRoot $PayloadSourceRoot) {
        Throw-AvidScriptPluginReleaseError 'ASRE1302' 'path_invalid' 'output root must not be inside the payload source.'
    }
    [System.IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
    $SortedArtifactContracts = @($ArtifactContracts | Sort-Object id -CaseSensitive)
    $SortedDependencies = @($Dependencies | Sort-Object id -CaseSensitive)
    $StagingRoot = Join-Path $OutputRoot ('.avidscript-release-stage-' + [guid]::NewGuid().ToString('N'))
    try {
        $StagingPayload = Join-Path $StagingRoot 'payload'
        [System.IO.Directory]::CreateDirectory($StagingRoot) | Out-Null
        Copy-AvidScriptPluginReleasePayload $PayloadSourceRoot $StagingPayload
        $Files = @(Get-AvidScriptPluginReleaseInventory $StagingPayload)
        if ($null -eq $Contracts) {
            $Contracts = [pscustomobject][ordered]@{
                module_abi = 1
                binding_descriptor_max = 23
                semantic_schema = 20
                guest_ir_schema = 2
            }
        }
        $Manifest = [pscustomobject][ordered]@{
            schema_version = 1
            format = 'avidscript.plugin.release'
            release_id = ''
            version = $Version
            channel = $Channel
            profile = 'source-developer'
            publisher = [pscustomobject][ordered]@{
                name = 'AvidScript.PluginRelease'
                version = $PublisherVersion
            }
            source = [pscustomobject][ordered]@{
                commit = $Commit
                tree = $Tree
                committed_at_utc = $CommittedAtUtc
            }
            compatibility = [pscustomobject][ordered]@{
                unreal_min = '5.8.0'
                unreal_max_exclusive = '5.9.0'
                dotnet_sdk = '8.0.416'
                powershell_min = '7.4.0'
            }
            targets = @($Targets | Sort-Object -CaseSensitive)
            contracts = $Contracts
            artifact_contracts = $SortedArtifactContracts
            dependencies = $SortedDependencies
            payload = [pscustomobject][ordered]@{
                root = 'payload'
                inventory_sha256 = Get-AvidScriptPluginReleaseInventorySha256 $Files
                file_count = [int64]$Files.Count
                total_bytes = [int64]($Files | Measure-Object length -Sum).Sum
                files = $Files
            }
            signature = $null
        }
        $Manifest.release_id = Get-AvidScriptPluginReleaseId $Manifest
        $FinalRoot = Join-Path $OutputRoot (
            "AvidScript-$Version-source-$($Manifest.release_id.Substring(0, 20))")
        Write-AvidScriptPluginReleaseJson (Join-Path $StagingRoot 'release.json') $Manifest
        Resolve-AvidScriptPluginReleasePackage $StagingRoot | Out-Null
        $PublishLock = Enter-AvidScriptPluginReleasePublishLock `
            (Join-Path $OutputRoot '.avidscript-release-publish.lock')
        try {
            if (Test-Path -LiteralPath $FinalRoot) {
                $Winner = Resolve-AvidScriptPluginReleasePackage $FinalRoot
                if ([string]$Winner.Manifest.release_id -cne [string]$Manifest.release_id -or
                    [string]$Winner.ManifestSha256 -cne
                    (Get-AvidScriptPluginReleaseSha256 (Join-Path $StagingRoot 'release.json'))) {
                    Throw-AvidScriptPluginReleaseError 'ASRE1303' 'output_collision' "release output identity collision: $FinalRoot"
                }
                return $Winner
            }
            [System.IO.Directory]::Move($StagingRoot, $FinalRoot)
            return Resolve-AvidScriptPluginReleasePackage $FinalRoot
        }
        finally {
            $PublishLock.Dispose()
        }
    }
    finally {
        if (Test-Path -LiteralPath $StagingRoot) {
            Remove-Item -LiteralPath $StagingRoot -Recurse -Force
        }
    }
}
