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

function Try-GetAvidScriptBindingJsonInt32 {
    param(
        [object]$Value,
        [Parameter(Mandatory = $true)][ref]$ParsedValue
    )

    if ($Value -is [int]) {
        $ParsedValue.Value = $Value
        return $true
    }
    if ($Value -is [long] -and
        $Value -ge [int]::MinValue -and
        $Value -le [int]::MaxValue) {
        $ParsedValue.Value = [int]$Value
        return $true
    }
    return $false
}

function Get-AvidScriptBindingFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path)
}

function Test-AvidScriptBindingPathContained {
    param(
        [Parameter(Mandatory = $true)][string]$RootPath,
        [Parameter(Mandatory = $true)][string]$CandidatePath
    )

    $NormalizedRoot = (Get-AvidScriptBindingFullPath $RootPath).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $NormalizedCandidate = Get-AvidScriptBindingFullPath $CandidatePath
    $ContainedPrefix = $NormalizedRoot + [System.IO.Path]::DirectorySeparatorChar
    if (-not $NormalizedCandidate.StartsWith(
            $ContainedPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        return $false
    }

    $RelativeCandidate = $NormalizedCandidate.Substring($ContainedPrefix.Length)
    $CurrentPath = $NormalizedRoot
    foreach ($Segment in @($RelativeCandidate -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($Segment)) {
            continue
        }
        $CurrentPath = Join-Path $CurrentPath $Segment
        if (Test-Path -LiteralPath $CurrentPath) {
            try {
                $Attributes = [System.IO.File]::GetAttributes($CurrentPath)
            }
            catch {
                return $false
            }
            if (($Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                return $false
            }
        }
    }
    return $true
}

function Resolve-AvidScriptBindingPath {
    param(
        [Parameter(Mandatory = $true)][string]$RootPath,
        [Parameter(Mandatory = $true)][string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return Get-AvidScriptBindingFullPath $Path
    }

    return Get-AvidScriptBindingFullPath (Join-Path $RootPath $Path)
}

function Publish-AvidScriptBindingFilePairAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$FirstSourcePath,
        [Parameter(Mandatory = $true)][string]$FirstDestinationPath,
        [Parameter(Mandatory = $true)][string]$SecondSourcePath,
        [Parameter(Mandatory = $true)][string]$SecondDestinationPath
    )

    $TransactionId = "$PID.$([System.Guid]::NewGuid().ToString('N'))"
    $Files = @(
        [pscustomobject]@{
            Source = Get-AvidScriptBindingFullPath $FirstSourcePath
            Destination = Get-AvidScriptBindingFullPath $FirstDestinationPath
            Temporary = ""
            Backup = ""
            HadExisting = $false
            Published = $false
        },
        [pscustomobject]@{
            Source = Get-AvidScriptBindingFullPath $SecondSourcePath
            Destination = Get-AvidScriptBindingFullPath $SecondDestinationPath
            Temporary = ""
            Backup = ""
            HadExisting = $false
            Published = $false
        })

    foreach ($File in $Files) {
        if (-not (Test-Path -LiteralPath $File.Source -PathType Leaf)) {
            throw "Atomic pair source file is missing: $($File.Source)"
        }
        if (Test-Path -LiteralPath $File.Destination -PathType Container) {
            throw "Atomic pair destination path is a directory: $($File.Destination)"
        }

        $DestinationDirectory = Split-Path -Parent $File.Destination
        if ([string]::IsNullOrWhiteSpace($DestinationDirectory)) {
            throw "Atomic pair destination directory is missing: $($File.Destination)"
        }
        New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null
        $DestinationFileName = Split-Path -Leaf $File.Destination
        $File.Temporary = Join-Path $DestinationDirectory ".$DestinationFileName.$TransactionId.tmp"
        $File.Backup = Join-Path $DestinationDirectory ".$DestinationFileName.$TransactionId.bak"
    }

    $Committed = $false
    $PreserveRecoveryMaterial = $false
    try {
        foreach ($File in $Files) {
            Copy-Item -LiteralPath $File.Source -Destination $File.Temporary -Force
            if ((Get-AvidScriptBindingSha256Hex $File.Temporary) -cne
                (Get-AvidScriptBindingSha256Hex $File.Source)) {
                throw "Atomic pair staging SHA-256 mismatch: $($File.Source)"
            }
        }

        foreach ($File in $Files) {
            if (Test-Path -LiteralPath $File.Destination -PathType Leaf) {
                Move-Item -LiteralPath $File.Destination -Destination $File.Backup
                $File.HadExisting = $true
            }
        }

        foreach ($File in $Files) {
            Move-Item -LiteralPath $File.Temporary -Destination $File.Destination
            $File.Published = $true
        }
        $Committed = $true
    }
    catch {
        $PublishFailure = $_.Exception
        $RollbackFailures = @()
        foreach ($File in $Files) {
            if ($File.Published -and (Test-Path -LiteralPath $File.Destination -PathType Leaf)) {
                try {
                    Remove-Item -LiteralPath $File.Destination -Force -ErrorAction Stop
                }
                catch {
                    $RollbackFailures += "Failed to remove published destination $($File.Destination): $($_.Exception.Message)"
                }
            }
        }

        foreach ($File in @($Files[1], $Files[0])) {
            if ($File.HadExisting -and (Test-Path -LiteralPath $File.Backup -PathType Leaf)) {
                try {
                    Move-Item -LiteralPath $File.Backup -Destination $File.Destination -Force
                }
                catch {
                    $RollbackFailures += $_.Exception.Message
                }
            }
        }
        if ($RollbackFailures.Count -gt 0) {
            $PreserveRecoveryMaterial = $true
            throw [System.InvalidOperationException]::new(
                "Atomic pair publication failed and rollback was incomplete: $($RollbackFailures -join '; ')",
                $PublishFailure)
        }
        throw $PublishFailure
    }
    finally {
        foreach ($File in $Files) {
            if (-not [string]::IsNullOrWhiteSpace($File.Temporary) -and
                (Test-Path -LiteralPath $File.Temporary -PathType Leaf) -and
                -not $PreserveRecoveryMaterial) {
                Remove-Item -LiteralPath $File.Temporary -Force -ErrorAction SilentlyContinue
            }
        }
    }

    if ($Committed) {
        foreach ($File in $Files) {
            if ($File.HadExisting -and (Test-Path -LiteralPath $File.Backup -PathType Leaf)) {
                try {
                    Remove-Item -LiteralPath $File.Backup -Force -ErrorAction Stop
                }
                catch {
                    Write-Warning (
                        "Atomic pair publication committed, but backup cleanup failed: " +
                        $File.Backup)
                }
            }
        }
    }
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

    $Directory = (Get-AvidScriptBindingFullPath $PackageDirectory).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $Candidate = Resolve-AvidScriptBindingPath -RootPath $Directory -Path $RelativePath
    if (-not (Test-AvidScriptBindingPathContained -RootPath $Directory -CandidatePath $Candidate)) {
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

    $ManifestSchemaVersion = 0
    if (-not (Try-GetAvidScriptBindingJsonInt32 `
            -Value $Manifest.schema_version `
            -ParsedValue ([ref]$ManifestSchemaVersion)) -or
        $ManifestSchemaVersion -ne 1) {
        throw "Binding package schema_version must be 1."
    }
    $ManifestDescriptorSchemaVersion = 0
    if (-not (Try-GetAvidScriptBindingJsonInt32 `
            -Value $Manifest.descriptor_schema_version `
            -ParsedValue ([ref]$ManifestDescriptorSchemaVersion))) {
        throw "Binding package descriptor_schema_version must be a JSON int32."
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
    $DescriptorSchemaVersion = 0
    if (-not (Try-GetAvidScriptBindingJsonInt32 `
            -Value $Descriptor.schema_version `
            -ParsedValue ([ref]$DescriptorSchemaVersion)) -or
        ($DescriptorSchemaVersion -ne 2 -and
        $DescriptorSchemaVersion -ne 3 -and
        $DescriptorSchemaVersion -ne 4 -and
        $DescriptorSchemaVersion -ne 5 -and
        $DescriptorSchemaVersion -ne 6) -or
        $ManifestDescriptorSchemaVersion -ne $DescriptorSchemaVersion -or
        [string]$Descriptor.package_name -cne $PackageName -or
        [string]$Descriptor.package_hash -cne $PackageHash) {
        throw "Binding descriptor identity does not match package.json."
    }
    $SelfTypeId = ""
    if ($DescriptorSchemaVersion -eq 6) {
        if ($Descriptor.self_type_id -isnot [string]) {
            throw "Binding descriptor self_type_id must be a JSON string."
        }
        $SelfTypeId = [string]$Descriptor.self_type_id
    }

    $RequiredImports = @()
    $ImportKeys = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    $SeenPackedOwner = $false
    foreach ($Import in @($Manifest.required_imports)) {
        $Ordinal = 0
        $HasOrdinal = Try-GetAvidScriptBindingJsonInt32 `
            -Value $Import.ordinal `
            -ParsedValue ([ref]$Ordinal)
        $StableId = [string]$Import.stable_id
        $Module = [string]$Import.module
        $Name = [string]$Import.name
        $Signature = [string]$Import.signature
        $IsReflectedImport = (Test-AvidScriptBindingSha256 $StableId) -and $Ordinal -ge 0
        $IsPackedOwnerImport =
            $StableId -ceq 'avidscript.owner_get_handle.v1' -and
            $Ordinal -eq -1 -and
            $Module -ceq 'avidscript' -and
            $Name -ceq 'avid_owner_get_handle' -and
            $Signature -ceq '()I'
        if (-not $HasOrdinal -or
            $Import.stable_id -isnot [string] -or
            $Import.module -isnot [string] -or
            $Import.name -isnot [string] -or
            $Import.signature -isnot [string] -or
            [string]::IsNullOrWhiteSpace($Module) -or
            [string]::IsNullOrWhiteSpace($Name) -or
            [string]::IsNullOrWhiteSpace($Signature) -or
            (-not $IsReflectedImport -and -not $IsPackedOwnerImport)) {
            throw "Binding package required_imports contains invalid identity, ordinal, module, name, or signature data."
        }
        if ($IsPackedOwnerImport) {
            if ($SeenPackedOwner) {
                throw "Binding package required_imports contains duplicate packed owner capability."
            }
            $SeenPackedOwner = $true
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
    if ($SeenPackedOwner -and
        ($DescriptorSchemaVersion -ne 6 -or
            [string]::IsNullOrWhiteSpace($SelfTypeId))) {
        throw "Binding package packed owner capability requires descriptor schema v6 with a non-empty self_type_id."
    }
    if ($RequiredImports.Count -eq 0) {
        throw "Binding package required_imports must not be empty."
    }

    return [pscustomobject]@{
        ManifestPath = $ManifestFullPath
        ManifestSha256 = Get-AvidScriptBindingSha256Hex $ManifestFullPath
        PackageName = $PackageName
        PackageHash = $PackageHash
        DescriptorSchemaVersion = $DescriptorSchemaVersion
        SelfTypeId = $SelfTypeId
        DescriptorPath = $DescriptorPath
        DescriptorSha256 = $DescriptorHash
        ReferenceSourcePath = $ReferenceSourcePath
        ReferenceSourceSha256 = $ReferenceSourceHash
        RequiredImports = @($RequiredImports)
    }
}

function Find-AvidScriptCSharpBindingPackageManifest {
    param(
        [Parameter(Mandatory = $true)][string]$RootPath,
        [Parameter(Mandatory = $true)][string[]]$RequiredUeFunctions
    )

    if ($RequiredUeFunctions.Count -eq 0 -or
        -not (Test-Path -LiteralPath $RootPath -PathType Container)) {
        return ""
    }

    $Candidates = foreach ($ManifestFile in Get-ChildItem `
            -LiteralPath $RootPath `
            -Filter "package.json" `
            -File `
            -Recurse `
            -ErrorAction SilentlyContinue) {
        try {
            $Package = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $ManifestFile.FullName
            $Descriptor = Get-Content -Raw -LiteralPath $Package.DescriptorPath | ConvertFrom-Json
            $AuthorizedStableIds = [System.Collections.Generic.HashSet[string]]::new(
                [System.StringComparer]::Ordinal)
            foreach ($Import in @($Package.RequiredImports)) {
                [void]$AuthorizedStableIds.Add([string]$Import.StableId)
            }

            $ContainsRequiredFunctions = $true
            foreach ($RequiredFunction in $RequiredUeFunctions) {
                $Matches = @($Descriptor.bindings | Where-Object {
                    [string]$_.ue_function -ceq $RequiredFunction -and
                    $AuthorizedStableIds.Contains([string]$_.stable_id)
                })
                if ($Matches.Count -eq 0) {
                    $ContainsRequiredFunctions = $false
                    break
                }
            }
            if ($ContainsRequiredFunctions) {
                [pscustomobject]@{
                    ManifestPath = $Package.ManifestPath
                    ImportCount = @($Package.RequiredImports).Count
                    LastWriteTime = $ManifestFile.LastWriteTimeUtc
                }
            }
        }
        catch {
            continue
        }
    }

    return [string]($Candidates |
        Sort-Object -Property @{ Expression = { $_.ImportCount }; Descending = $true },
            @{ Expression = { $_.LastWriteTime }; Descending = $true },
            @{ Expression = { $_.ManifestPath }; Descending = $false } |
        Select-Object -First 1 -ExpandProperty ManifestPath)
}
