$ErrorActionPreference = "Stop"
$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$AvidScriptSemanticCachePluginRoot = Split-Path -Parent $BuildDir
. (Join-Path $BuildDir "AvidScriptCSharpPreparedSemantic.ps1")

function Fail-AvidScriptCSharpSemanticCache {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $Exception = [System.InvalidOperationException]::new("$Code $Message")
    $Exception.Data["AvidScriptCode"] = $Code
    throw $Exception
}

function Assert-AvidScriptCSharpSemanticCache {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        Fail-AvidScriptCSharpSemanticCache -Code $Code -Message $Message
    }
}

function Get-AvidScriptUtf8Sha256 {
    param([Parameter(Mandatory = $true)][string]$Text)

    $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
    $Sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $HashBytes = $Sha256.ComputeHash($Bytes)
    }
    finally {
        $Sha256.Dispose()
    }
    return [System.BitConverter]::ToString($HashBytes).Replace("-", "").ToLowerInvariant()
}

function Get-AvidScriptUtf8JsonSha256 {
    param([Parameter(Mandatory = $true)]$Value)

    $Json = $Value | ConvertTo-Json -Compress -Depth 32
    return Get-AvidScriptUtf8Sha256 $Json
}

function Convert-ToAvidScriptSemanticCacheProjectPath {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$FieldName
    )

    $ProjectRootFullPath = Get-AvidScriptBindingFullPath $ProjectRoot
    $CandidateFullPath = Get-AvidScriptBindingFullPath $Path
    $ProjectPrefix = $ProjectRootFullPath.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ($CandidateFullPath.StartsWith($ProjectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) `
        -Code "ASBI4501" `
        -Message "$FieldName must be located under the project root."
    return $CandidateFullPath.Substring($ProjectPrefix.Length).Replace("\", "/").ToLowerInvariant()
}

function Get-AvidScriptSemanticCacheToolchainFiles {
    param([Parameter(Mandatory = $true)][string]$PluginRoot)

    $PluginRootFullPath = Get-AvidScriptBindingFullPath $PluginRoot
    $RequiredFiles = @(
        (Join-Path $PluginRootFullPath "global.json"),
        (Join-Path $PluginRootFullPath "Build\InvokeCSharpFrontend.ps1"),
        (Join-Path $PluginRootFullPath "Build\InvokeCSharpSemantic.ps1"))
    foreach ($RequiredFile in $RequiredFiles) {
        Assert-AvidScriptCSharpSemanticCache `
            -Condition (Test-Path -LiteralPath $RequiredFile -PathType Leaf) `
            -Code "ASBI4501" `
            -Message "Required semantic cache toolchain file is missing: $RequiredFile"
    }

    $SourceFiles = @()
    foreach ($SourceRootName in @("AvidScript.CSharpFrontend", "AvidScript.CSharpSemantic")) {
        $SourceRoot = Join-Path $PluginRootFullPath ("Tools\" + $SourceRootName)
        Assert-AvidScriptCSharpSemanticCache `
            -Condition (Test-Path -LiteralPath $SourceRoot -PathType Container) `
            -Code "ASBI4501" `
            -Message "Semantic cache toolchain source root is missing: $SourceRoot"
        $SourceFiles += @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File | Where-Object {
            ($_.Extension -eq ".cs" -or $_.Extension -eq ".csproj") -and
            $_.FullName -notmatch '[\\/](?:bin|obj)[\\/]'
        })
    }

    $AllFiles = @($RequiredFiles | ForEach-Object { Get-Item -LiteralPath $_ }) + @($SourceFiles)
    $PluginPrefix = $PluginRootFullPath.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    return @($AllFiles | ForEach-Object {
        $FullPath = Get-AvidScriptBindingFullPath $_.FullName
        Assert-AvidScriptCSharpSemanticCache `
            -Condition (Test-AvidScriptBindingPathContained -RootPath $PluginRootFullPath -CandidatePath $FullPath) `
            -Code "ASBI4501" `
            -Message "Toolchain source file escapes the plugin root: $FullPath"
        [pscustomobject]@{
            FullPath = $FullPath
            RelativePath = $FullPath.Substring($PluginPrefix.Length).Replace("\", "/")
        }
    } | Sort-Object -Property RelativePath)
}

function Get-AvidScriptCSharpToolchainFingerprint {
    param([Parameter(Mandatory = $true)][string]$PluginRoot)

    $PluginRootFullPath = Get-AvidScriptBindingFullPath $PluginRoot
    $GlobalJsonPath = Join-Path $PluginRootFullPath "global.json"
    try {
        $GlobalJson = Get-Content -Raw -LiteralPath $GlobalJsonPath | ConvertFrom-Json
    }
    catch {
        Fail-AvidScriptCSharpSemanticCache `
            -Code "ASBI4501" `
            -Message "global.json is invalid: $($_.Exception.Message)"
    }
    $SdkVersion = [string]$GlobalJson.sdk.version
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (-not [string]::IsNullOrWhiteSpace($SdkVersion) -and
            $SdkVersion -cmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$') `
        -Code "ASBI4501" `
        -Message "global.json must define a pinned three-part sdk.version."

    $FileRecords = @(Get-AvidScriptSemanticCacheToolchainFiles -PluginRoot $PluginRootFullPath | ForEach-Object {
        $FileInfo = Get-Item -LiteralPath $_.FullPath
        [ordered]@{
            path = [string]$_.RelativePath
            length = [long]$FileInfo.Length
            sha256 = Get-AvidScriptBindingSha256Hex $_.FullPath
        }
    })
    $FingerprintValue = [ordered]@{
        schema_version = 1
        files = @($FileRecords)
    }
    return [pscustomobject]@{
        DotNetSdkVersion = $SdkVersion
        Sha256 = Get-AvidScriptUtf8JsonSha256 $FingerprintValue
        Files = @($FileRecords)
    }
}

function New-AvidScriptSemanticCacheReferenceSources {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [AllowNull()][object]$AuthorizationPackage
    )

    if ($null -eq $AuthorizationPackage) {
        return @()
    }
    $ReferenceSourcePath = [string]$AuthorizationPackage.ReferenceSourcePath
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (Test-Path -LiteralPath $ReferenceSourcePath -PathType Leaf) `
        -Code "ASBI4501" `
        -Message "Authorization reference source is missing: $ReferenceSourcePath"
    $ReferenceSourceSha256 = Get-AvidScriptBindingSha256Hex $ReferenceSourcePath
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ([string]$AuthorizationPackage.ReferenceSourceSha256 -ceq $ReferenceSourceSha256) `
        -Code "ASBI4501" `
        -Message "Authorization reference source SHA-256 does not match the current file."
    $ReferenceSourceId = Convert-ToAvidScriptSemanticCacheProjectPath `
        -ProjectRoot $ProjectRoot `
        -Path $ReferenceSourcePath `
        -FieldName "authorization reference source"
    return @([ordered]@{
        ordinal = 0
        kind = "executable"
        source_id = "reference:0:$ReferenceSourceId"
        sha256 = $ReferenceSourceSha256
    })
}

function New-AvidScriptSemanticCacheAuthorizationIdentity {
    param([AllowNull()][object]$AuthorizationPackage)

    if ($null -eq $AuthorizationPackage) {
        return [ordered]@{
            required = $false
            package_name = ""
            package_hash = ""
            manifest_sha256 = ""
            descriptor_sha256 = ""
            reference_source_sha256 = ""
        }
    }
    foreach ($Field in @("PackageHash", "ManifestSha256", "DescriptorSha256", "ReferenceSourceSha256")) {
        Assert-AvidScriptCSharpSemanticCache `
            -Condition (Test-AvidScriptBindingSha256 ([string]$AuthorizationPackage.$Field)) `
            -Code "ASBI4501" `
            -Message "Authorization $Field must be a lowercase SHA-256 value."
    }
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (-not [string]::IsNullOrWhiteSpace([string]$AuthorizationPackage.PackageName)) `
        -Code "ASBI4501" `
        -Message "Authorization package name is missing."
    return [ordered]@{
        required = $true
        package_name = [string]$AuthorizationPackage.PackageName
        package_hash = [string]$AuthorizationPackage.PackageHash
        manifest_sha256 = [string]$AuthorizationPackage.ManifestSha256
        descriptor_sha256 = [string]$AuthorizationPackage.DescriptorSha256
        reference_source_sha256 = [string]$AuthorizationPackage.ReferenceSourceSha256
    }
}

function Get-AvidScriptCSharpSemanticCacheContext {
    param(
        [Parameter(Mandatory = $true)][string]$PluginRoot,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [AllowNull()][object]$AuthorizationPackage
    )

    $PluginRootFullPath = Get-AvidScriptBindingFullPath $PluginRoot
    $ProjectRootFullPath = Get-AvidScriptBindingFullPath $ProjectRoot
    $CacheRootFullPath = Get-AvidScriptBindingFullPath $CacheRoot
    $SourceFullPath = Get-AvidScriptBindingFullPath $SourcePath
    $ProjectFullPath = Get-AvidScriptBindingFullPath $ProjectPath
    foreach ($RequiredInput in @($SourceFullPath, $ProjectFullPath)) {
        Assert-AvidScriptCSharpSemanticCache `
            -Condition (Test-Path -LiteralPath $RequiredInput -PathType Leaf) `
            -Code "ASBI4501" `
            -Message "Semantic cache input file is missing: $RequiredInput"
    }
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (-not [string]::IsNullOrWhiteSpace($Configuration)) `
        -Code "ASBI4501" `
        -Message "Semantic cache configuration is missing."

    $CacheRootExists = Test-Path -LiteralPath $CacheRootFullPath
    $CacheRootIsDirectory = Test-Path -LiteralPath $CacheRootFullPath -PathType Container
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (-not $CacheRootExists -or $CacheRootIsDirectory) `
        -Code "ASBI4503" `
        -Message "Semantic cache root must be a directory when it already exists."

    $ProjectCacheNamespace = Join-Path $ProjectRootFullPath "Saved\AvidScript"
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (Test-AvidScriptBindingPathContained -RootPath $ProjectCacheNamespace -CandidatePath $CacheRootFullPath) `
        -Code "ASBI4503" `
        -Message "Semantic cache root must be physically contained by the project Saved/AvidScript namespace."

    $Toolchain = Get-AvidScriptCSharpToolchainFingerprint -PluginRoot $PluginRootFullPath
    $ReferenceSources = @(New-AvidScriptSemanticCacheReferenceSources `
        -ProjectRoot $ProjectRootFullPath `
        -AuthorizationPackage $AuthorizationPackage)
    $AuthorizationIdentity = New-AvidScriptSemanticCacheAuthorizationIdentity $AuthorizationPackage
    $CanonicalInput = [ordered]@{
        schema_version = 1
        configuration = $Configuration
        target_framework = "net8.0"
        dotnet_sdk_version = [string]$Toolchain.DotNetSdkVersion
        source = [ordered]@{
            id = Convert-ToAvidScriptSemanticCacheProjectPath `
                -ProjectRoot $ProjectRootFullPath `
                -Path $SourceFullPath `
                -FieldName "source"
            sha256 = Get-AvidScriptBindingSha256Hex $SourceFullPath
        }
        project_sha256 = Get-AvidScriptBindingSha256Hex $ProjectFullPath
        reference_sources = @($ReferenceSources)
        authorization = $AuthorizationIdentity
        toolchain_fingerprint = [string]$Toolchain.Sha256
    }
    $CacheKey = Get-AvidScriptUtf8JsonSha256 $CanonicalInput
    $EntryDirectory = Join-Path (Join-Path $CacheRootFullPath $CacheKey.Substring(0, 2)) $CacheKey
    return [pscustomobject]@{
        Enabled = $true
        CacheKey = $CacheKey
        ToolchainFingerprint = [string]$Toolchain.Sha256
        PluginRoot = $PluginRootFullPath
        ProjectRoot = $ProjectRootFullPath
        Configuration = $Configuration
        SourcePath = $SourceFullPath
        ProjectPath = $ProjectFullPath
        DotNetSdkVersion = [string]$Toolchain.DotNetSdkVersion
        CacheRoot = $CacheRootFullPath
        EntryDirectory = $EntryDirectory
        EntryReportPath = Join-Path $EntryDirectory "entry.csharp.report.json"
        DiagnosticCode = ""
        DiagnosticMessage = ""
        CanonicalInput = $CanonicalInput
    }
}

function Convert-ToAvidScriptSemanticCacheReportPath {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$FieldName
    )

    $ProjectRootFullPath = Get-AvidScriptBindingFullPath $ProjectRoot
    $PathFullPath = Get-AvidScriptBindingFullPath $Path
    $ProjectPrefix = $ProjectRootFullPath.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ($PathFullPath.StartsWith($ProjectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) `
        -Code "ASBI4503" `
        -Message "$FieldName must be located under the project root."
    return $PathFullPath.Substring($ProjectPrefix.Length).Replace("\", "/")
}

function Read-AvidScriptSemanticCacheJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    try {
        return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    }
    catch {
        Fail-AvidScriptCSharpSemanticCache `
            -Code "ASBI4502" `
            -Message "$Label JSON is invalid: $($_.Exception.Message)"
    }
}

function Write-AvidScriptSemanticCacheJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $Directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $Json = $Value | ConvertTo-Json -Depth 32
    [System.IO.File]::WriteAllText($Path, $Json, [System.Text.UTF8Encoding]::new($false))
}

function Assert-AvidScriptSemanticCacheContext {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [Parameter(Mandatory = $true)][string]$ProjectRoot
    )

    Assert-AvidScriptCSharpSemanticCache `
        -Condition ([bool]$Context.Enabled -and
            (Test-AvidScriptBindingSha256 ([string]$Context.CacheKey)) -and
            (Test-AvidScriptBindingSha256 ([string]$Context.ToolchainFingerprint))) `
        -Code "ASBI4502" `
        -Message "Semantic cache context identity is invalid."
    $CacheRootFullPath = Get-AvidScriptBindingFullPath ([string]$Context.CacheRoot)
    $ExpectedEntryDirectory = Join-Path `
        (Join-Path $CacheRootFullPath ([string]$Context.CacheKey).Substring(0, 2)) `
        ([string]$Context.CacheKey)
    $ExpectedReportPath = Join-Path $ExpectedEntryDirectory "entry.csharp.report.json"
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ($ExpectedEntryDirectory.Equals(
                (Get-AvidScriptBindingFullPath ([string]$Context.EntryDirectory)),
                [System.StringComparison]::OrdinalIgnoreCase) -and
            $ExpectedReportPath.Equals(
                (Get-AvidScriptBindingFullPath ([string]$Context.EntryReportPath)),
                [System.StringComparison]::OrdinalIgnoreCase)) `
        -Code "ASBI4503" `
        -Message "Semantic cache context entry paths do not match the cache key."
    $ProjectCacheNamespace = Join-Path (Get-AvidScriptBindingFullPath $ProjectRoot) "Saved\AvidScript"
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ((Test-AvidScriptBindingPathContained -RootPath $ProjectCacheNamespace -CandidatePath $CacheRootFullPath) -and
            (Test-AvidScriptBindingPathContained -RootPath $CacheRootFullPath -CandidatePath $ExpectedEntryDirectory) -and
            (Test-AvidScriptBindingPathContained -RootPath $ExpectedEntryDirectory -CandidatePath $ExpectedReportPath)) `
        -Code "ASBI4503" `
        -Message "Semantic cache context paths escape their physical ownership boundaries."
}

function Move-AvidScriptSemanticCacheDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    [System.IO.Directory]::Move($SourcePath, $DestinationPath)
}

function Remove-AvidScriptSemanticCacheDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    Microsoft.PowerShell.Management\Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
}

function Test-AvidScriptSemanticCachePathEqualOrContained {
    param(
        [Parameter(Mandatory = $true)][string]$RootPath,
        [Parameter(Mandatory = $true)][string]$CandidatePath
    )

    $RootFullPath = Get-AvidScriptBindingFullPath $RootPath
    $CandidateFullPath = Get-AvidScriptBindingFullPath $CandidatePath
    $ContainedPrefix = $RootFullPath.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    return $RootFullPath.Equals($CandidateFullPath, [System.StringComparison]::OrdinalIgnoreCase) -or
        $CandidateFullPath.StartsWith($ContainedPrefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-AvidScriptSemanticCachePathWithoutReparsePoint {
    param([Parameter(Mandatory = $true)][string]$Path)

    $FullPath = Get-AvidScriptBindingFullPath $Path
    $PathRoot = [System.IO.Path]::GetPathRoot($FullPath)
    $CurrentPath = $PathRoot
    foreach ($Segment in @(($FullPath.Substring($PathRoot.Length)) -split '[\\/]')) {
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

function Assert-AvidScriptSemanticCacheDestinations {
    param(
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)][string]$FrontendDestinationPath,
        [Parameter(Mandatory = $true)][string]$SemanticDestinationPath
    )

    $FrontendDestination = Get-AvidScriptBindingFullPath $FrontendDestinationPath
    $SemanticDestination = Get-AvidScriptBindingFullPath $SemanticDestinationPath
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (-not $FrontendDestination.Equals(
                $SemanticDestination,
                [System.StringComparison]::OrdinalIgnoreCase)) `
        -Code "ASBI4503" `
        -Message "Semantic cache Frontend and Semantic destinations must be different files."
    foreach ($Destination in @($FrontendDestination, $SemanticDestination)) {
        Assert-AvidScriptCSharpSemanticCache `
            -Condition (Test-AvidScriptSemanticCachePathWithoutReparsePoint -Path $Destination) `
            -Code "ASBI4503" `
            -Message "Semantic cache import destination must not traverse a reparse point."
        Assert-AvidScriptCSharpSemanticCache `
            -Condition (-not (Test-AvidScriptSemanticCachePathEqualOrContained `
                -RootPath $CacheRoot `
                -CandidatePath $Destination)) `
            -Code "ASBI4503" `
            -Message "Semantic cache import destination must not overlap cache ownership."
    }
}

function Enter-AvidScriptSemanticCacheKeyLock {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [int]$TimeoutMilliseconds = 15000
    )

    Assert-AvidScriptCSharpSemanticCache `
        -Condition ($TimeoutMilliseconds -ge 0) `
        -Code "ASBI4504" `
        -Message "Semantic cache key lock timeout must be non-negative."
    $CacheRoot = Get-AvidScriptBindingFullPath ([string]$Context.CacheRoot)
    $LockRoot = Join-Path $CacheRoot "Locks"
    try {
        New-Item -ItemType Directory -Force -Path $LockRoot | Out-Null
    }
    catch {
        Fail-AvidScriptCSharpSemanticCache `
            -Code "ASBI4504" `
            -Message "Semantic cache key lock directory could not be created: $($_.Exception.Message)"
    }
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (Test-AvidScriptBindingPathContained -RootPath $CacheRoot -CandidatePath $LockRoot) `
        -Code "ASBI4503" `
        -Message "Semantic cache key lock directory escapes cache ownership."
    $LockPath = Join-Path $LockRoot ("$($Context.CacheKey).lock")
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ((Test-AvidScriptBindingPathContained -RootPath $CacheRoot -CandidatePath $LockPath) -and
            (Test-AvidScriptSemanticCachePathWithoutReparsePoint -Path $LockPath)) `
        -Code "ASBI4503" `
        -Message "Semantic cache key lock path escapes cache ownership or traverses a reparse point."
    $Deadline = [System.DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ($true) {
        try {
            return [System.IO.File]::Open(
                $LockPath,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None)
        }
        catch [System.IO.IOException] {
            if ([System.DateTime]::UtcNow -ge $Deadline) {
                Fail-AvidScriptCSharpSemanticCache `
                    -Code "ASBI4504" `
                    -Message "Timed out acquiring semantic cache key lock."
            }
            [System.Threading.Thread]::Sleep(25)
        }
        catch {
            Fail-AvidScriptCSharpSemanticCache `
                -Code "ASBI4504" `
                -Message "Semantic cache key lock acquisition failed: $($_.Exception.Message)"
        }
    }
}

function Move-AvidScriptCSharpSemanticCacheCorruptEntry {
    param([Parameter(Mandatory = $true)]$Context)

    $CacheRoot = Get-AvidScriptBindingFullPath ([string]$Context.CacheRoot)
    $EntryDirectory = Get-AvidScriptBindingFullPath ([string]$Context.EntryDirectory)
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ((Test-Path -LiteralPath $EntryDirectory -PathType Container) -or
            (Test-Path -LiteralPath $EntryDirectory -PathType Leaf)) `
        -Code "ASBI4505" `
        -Message "Corrupt semantic cache entry path is unavailable for isolation."
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (Test-AvidScriptBindingPathContained -RootPath $CacheRoot -CandidatePath $EntryDirectory) `
        -Code "ASBI4505" `
        -Message "Corrupt semantic cache entry is not physically contained by the cache root."

    $CorruptRoot = Join-Path $CacheRoot "Corrupt"
    New-Item -ItemType Directory -Force -Path $CorruptRoot | Out-Null
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (Test-AvidScriptBindingPathContained -RootPath $CacheRoot -CandidatePath $CorruptRoot) `
        -Code "ASBI4505" `
        -Message "Semantic cache corrupt isolation root escapes the cache root."
    $CorruptDestination = Join-Path `
        $CorruptRoot `
        ("$($Context.CacheKey)." + [System.Guid]::NewGuid().ToString("N"))
    try {
        if (Test-Path -LiteralPath $EntryDirectory -PathType Container) {
            Move-AvidScriptSemanticCacheDirectory `
                -SourcePath $EntryDirectory `
                -DestinationPath $CorruptDestination
        }
        else {
            [System.IO.File]::Move($EntryDirectory, $CorruptDestination)
        }
    }
    catch {
        Fail-AvidScriptCSharpSemanticCache `
            -Code "ASBI4505" `
            -Message "Failed to isolate corrupt semantic cache entry: $($_.Exception.Message)"
    }
    return $CorruptDestination
}

function Get-AvidScriptSemanticCacheMappedCode {
    param([Parameter(Mandatory = $true)]$Exception)

    $Code = [string]$Exception.Data["AvidScriptCode"]
    if ($Code.StartsWith("ASBI45", [System.StringComparison]::Ordinal)) {
        return $Code
    }
    if ($Code -ceq "ASBI4404") {
        return "ASBI4503"
    }
    return "ASBI4502"
}

function Import-AvidScriptCSharpSemanticCacheEntry {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ExpectedSourcePath,
        [AllowNull()][object]$ExpectedAuthorizationPackage,
        [Parameter(Mandatory = $true)][string]$FrontendDestinationPath,
        [Parameter(Mandatory = $true)][string]$SemanticDestinationPath,
        [AllowNull()][object]$CacheLock
    )

    $CanIsolate = $false
    $OwnedCacheLock = $null
    try {
        Assert-AvidScriptSemanticCacheContext -Context $Context -ProjectRoot $ProjectRoot
        Assert-AvidScriptSemanticCacheDestinations `
            -CacheRoot ([string]$Context.CacheRoot) `
            -FrontendDestinationPath $FrontendDestinationPath `
            -SemanticDestinationPath $SemanticDestinationPath
        if ($null -eq $CacheLock) {
            $OwnedCacheLock = Enter-AvidScriptSemanticCacheKeyLock -Context $Context
            $CacheLock = $OwnedCacheLock
        }
        $EntryDirectory = Get-AvidScriptBindingFullPath ([string]$Context.EntryDirectory)
        $EntryReportPath = Get-AvidScriptBindingFullPath ([string]$Context.EntryReportPath)
        if (-not (Test-Path -LiteralPath $EntryDirectory)) {
            return [pscustomobject]@{
                Status = "miss"
                DiagnosticCode = ""
                DiagnosticMessage = ""
                EntryReportPath = $EntryReportPath
                EntryReportSha256 = ""
            }
        }
        $CanIsolate = $true
        Assert-AvidScriptCSharpSemanticCache `
            -Condition (Test-Path -LiteralPath $EntryDirectory -PathType Container) `
            -Code "ASBI4502" `
            -Message "Semantic cache entry path is not a directory."
        Assert-AvidScriptCSharpSemanticCache `
            -Condition (Test-Path -LiteralPath $EntryReportPath -PathType Leaf) `
            -Code "ASBI4502" `
            -Message "Semantic cache entry report is missing."

        $EntryReport = Read-AvidScriptSemanticCacheJson -Path $EntryReportPath -Label "Semantic cache entry report"
        Assert-AvidScriptCSharpSemanticCache `
            -Condition ([int]$EntryReport.semantic_cache.schema_version -eq 1 -and
                [string]$EntryReport.semantic_cache.key -ceq [string]$Context.CacheKey -and
                [string]$EntryReport.semantic_cache.toolchain_fingerprint -ceq [string]$Context.ToolchainFingerprint) `
            -Code "ASBI4502" `
            -Message "Semantic cache entry identity does not match the current key and toolchain."

        $ResolvedOutputRoot = Resolve-AvidScriptBindingPath `
            -RootPath $ProjectRoot `
            -Path ([string]$EntryReport.output_root)
        $ResolvedFrontendPath = Resolve-AvidScriptBindingPath `
            -RootPath $ProjectRoot `
            -Path ([string]$EntryReport.artifacts.frontend_file)
        $ResolvedSemanticPath = Resolve-AvidScriptBindingPath `
            -RootPath $ProjectRoot `
            -Path ([string]$EntryReport.artifacts.semantic_file)
        $ExpectedFrontendPath = Join-Path $EntryDirectory "semantic.frontend.json"
        $ExpectedSemanticPath = Join-Path $EntryDirectory "semantic.model.json"
        Assert-AvidScriptCSharpSemanticCache `
            -Condition ($ResolvedOutputRoot.Equals($EntryDirectory, [System.StringComparison]::OrdinalIgnoreCase) -and
                $ResolvedFrontendPath.Equals($ExpectedFrontendPath, [System.StringComparison]::OrdinalIgnoreCase) -and
                $ResolvedSemanticPath.Equals($ExpectedSemanticPath, [System.StringComparison]::OrdinalIgnoreCase) -and
                (Test-AvidScriptBindingPathContained -RootPath $EntryDirectory -CandidatePath $ResolvedFrontendPath) -and
                (Test-AvidScriptBindingPathContained -RootPath $EntryDirectory -CandidatePath $ResolvedSemanticPath)) `
            -Code "ASBI4503" `
            -Message "Semantic cache entry report paths do not match the immutable entry layout."
        foreach ($LoadableField in @("manifest_file", "wasm_file", "guest_ir_file")) {
            Assert-AvidScriptCSharpSemanticCache `
                -Condition ([string]::IsNullOrWhiteSpace([string]$EntryReport.artifacts.$LoadableField)) `
                -Code "ASBI4502" `
                -Message "Semantic cache entry must not reference $LoadableField."
        }

        $Prepared = Import-AvidScriptCSharpPreparedSemantic `
            -PreparedReportPath $EntryReportPath `
            -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $ExpectedSourcePath `
            -ExpectedAuthorizationPackage $ExpectedAuthorizationPackage `
            -FrontendDestinationPath $FrontendDestinationPath `
            -SemanticDestinationPath $SemanticDestinationPath
        return [pscustomobject]@{
            Status = "hit"
            DiagnosticCode = ""
            DiagnosticMessage = ""
            EntryReportPath = $EntryReportPath
            EntryReportSha256 = [string]$Prepared.PreparedReportSha256
            FrontendModel = $Prepared.FrontendModel
            SemanticModel = $Prepared.SemanticModel
        }
    }
    catch {
        $MappedCode = Get-AvidScriptSemanticCacheMappedCode $_.Exception
        $DiagnosticMessage = $_.Exception.Message
        $StructuredCode = [string]$_.Exception.Data["AvidScriptCode"]
        $ShouldIsolate = $CanIsolate -and (
            $StructuredCode.StartsWith("ASBI44", [System.StringComparison]::Ordinal) -or
            $StructuredCode.StartsWith("ASBI45", [System.StringComparison]::Ordinal))
        if ($ShouldIsolate -and (Test-Path -LiteralPath ([string]$Context.EntryDirectory))) {
            try {
                $CorruptEntryPath = Move-AvidScriptCSharpSemanticCacheCorruptEntry -Context $Context
            }
            catch {
                return [pscustomobject]@{
                    Status = "rejected"
                    DiagnosticCode = "ASBI4505"
                    DiagnosticMessage = $_.Exception.Message
                    EntryReportPath = [string]$Context.EntryReportPath
                    EntryReportSha256 = ""
                    CorruptEntryPath = ""
                }
            }
        }
        return [pscustomobject]@{
            Status = "rejected"
            DiagnosticCode = $MappedCode
            DiagnosticMessage = $DiagnosticMessage
            EntryReportPath = [string]$Context.EntryReportPath
            EntryReportSha256 = ""
            CorruptEntryPath = [string]$CorruptEntryPath
        }
    }
    finally {
        if ($null -ne $OwnedCacheLock) {
            $OwnedCacheLock.Dispose()
        }
    }
}

function Assert-AvidScriptSemanticCachePublicationContext {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ExpectedSourcePath,
        [AllowNull()][object]$ExpectedAuthorizationPackage,
        [Parameter(Mandatory = $true)][string]$SourceReportPath
    )

    Assert-AvidScriptSemanticCacheContext -Context $Context -ProjectRoot $ProjectRoot
    foreach ($RequiredProperty in @(
        "PluginRoot",
        "ProjectRoot",
        "Configuration",
        "SourcePath",
        "ProjectPath")) {
        Assert-AvidScriptCSharpSemanticCache `
            -Condition (-not [string]::IsNullOrWhiteSpace([string]$Context.$RequiredProperty)) `
            -Code "ASBI4502" `
            -Message "Semantic cache publication context is missing $RequiredProperty."
    }

    $ProjectRootFullPath = Get-AvidScriptBindingFullPath $ProjectRoot
    $ExpectedSourceFullPath = Get-AvidScriptBindingFullPath $ExpectedSourcePath
    $ContextPluginRoot = Get-AvidScriptBindingFullPath ([string]$Context.PluginRoot)
    $ContextProjectRoot = Get-AvidScriptBindingFullPath ([string]$Context.ProjectRoot)
    $ContextSourcePath = Get-AvidScriptBindingFullPath ([string]$Context.SourcePath)
    $ContextProjectPath = Get-AvidScriptBindingFullPath ([string]$Context.ProjectPath)
    $ExpectedPluginRoot = Get-AvidScriptBindingFullPath $AvidScriptSemanticCachePluginRoot
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ($ContextPluginRoot.Equals($ExpectedPluginRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            $ContextProjectRoot.Equals($ProjectRootFullPath, [System.StringComparison]::OrdinalIgnoreCase) -and
            $ContextSourcePath.Equals($ExpectedSourceFullPath, [System.StringComparison]::OrdinalIgnoreCase)) `
        -Code "ASBI4502" `
        -Message "Semantic cache publication context ownership differs from the active build."
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (Test-Path -LiteralPath $ContextProjectPath -PathType Leaf) `
        -Code "ASBI4502" `
        -Message "Semantic cache publication project file is missing."

    $SourceReport = Read-AvidScriptSemanticCacheJson `
        -Path $SourceReportPath `
        -Label "Semantic cache source report"
    $ReportProjectPath = Resolve-AvidScriptBindingPath `
        -RootPath $ProjectRootFullPath `
        -Path ([string]$SourceReport.source.project)
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ($ReportProjectPath.Equals($ContextProjectPath, [System.StringComparison]::OrdinalIgnoreCase)) `
        -Code "ASBI4502" `
        -Message "Semantic cache source report project differs from the publication context."

    $RecomputedContext = Get-AvidScriptCSharpSemanticCacheContext `
        -PluginRoot $ExpectedPluginRoot `
        -ProjectRoot $ProjectRootFullPath `
        -CacheRoot ([string]$Context.CacheRoot) `
        -Configuration ([string]$Context.Configuration) `
        -SourcePath $ExpectedSourceFullPath `
        -ProjectPath $ContextProjectPath `
        -AuthorizationPackage $ExpectedAuthorizationPackage
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ([string]$RecomputedContext.CacheKey -ceq [string]$Context.CacheKey -and
            [string]$RecomputedContext.ToolchainFingerprint -ceq [string]$Context.ToolchainFingerprint) `
        -Code "ASBI4502" `
        -Message "Semantic cache publication context is stale for the current inputs or toolchain."

    $SourceCacheIdentity = $SourceReport.semantic_cache
    Assert-AvidScriptCSharpSemanticCache `
        -Condition ($null -ne $SourceCacheIdentity -and
            [int]$SourceCacheIdentity.schema_version -eq 1 -and
            (Test-AvidScriptBindingSha256 ([string]$SourceCacheIdentity.key)) -and
            (Test-AvidScriptBindingSha256 ([string]$SourceCacheIdentity.toolchain_fingerprint)) -and
            [string]$SourceCacheIdentity.key -ceq [string]$RecomputedContext.CacheKey -and
            [string]$SourceCacheIdentity.toolchain_fingerprint -ceq [string]$RecomputedContext.ToolchainFingerprint) `
        -Code "ASBI4502" `
        -Message "Semantic cache source report identity differs from the recomputed publication context."
    return $SourceReport
}

function Publish-AvidScriptCSharpSemanticCacheEntry {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ExpectedSourcePath,
        [AllowNull()][object]$ExpectedAuthorizationPackage,
        [Parameter(Mandatory = $true)][string]$SourceReportPath
    )

    $PublicationResult = $null
    $CacheLock = $null
    $ValidationDirectory = $null
    $StagingDirectory = $null
    $CacheRoot = Get-AvidScriptBindingFullPath ([string]$Context.CacheRoot)
    $EntryDirectory = Get-AvidScriptBindingFullPath ([string]$Context.EntryDirectory)
    $ShardDirectory = Split-Path -Parent $EntryDirectory
    $TransactionId = "$PID.$([System.Guid]::NewGuid().ToString('N'))"
    $TransactionRoot = Join-Path `
        (Get-AvidScriptBindingFullPath $ProjectRoot) `
        "Intermediate\AvidScript\SemanticCacheTransactions"
    $ValidationDirectory = Join-Path $TransactionRoot ".validation.$TransactionId"
    $ValidationFrontendPath = Join-Path $ValidationDirectory "source.frontend.json"
    $ValidationSemanticPath = Join-Path $ValidationDirectory "source.semantic.json"
    $StagingDirectory = Join-Path $ShardDirectory ".staging.$TransactionId"

    try {
        $SourceReport = Assert-AvidScriptSemanticCachePublicationContext `
            -Context $Context `
            -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $ExpectedSourcePath `
            -ExpectedAuthorizationPackage $ExpectedAuthorizationPackage `
            -SourceReportPath $SourceReportPath
        $SourceReportSha256 = Get-AvidScriptBindingSha256Hex $SourceReportPath
        New-Item -ItemType Directory -Force -Path $ShardDirectory | Out-Null
        Assert-AvidScriptSemanticCacheDestinations `
            -CacheRoot $CacheRoot `
            -FrontendDestinationPath $ValidationFrontendPath `
            -SemanticDestinationPath $ValidationSemanticPath
        try {
            $Prepared = Import-AvidScriptCSharpPreparedSemantic `
                -PreparedReportPath $SourceReportPath `
                -ProjectRoot $ProjectRoot `
                -ExpectedSourcePath $ExpectedSourcePath `
                -ExpectedAuthorizationPackage $ExpectedAuthorizationPackage `
                -FrontendDestinationPath $ValidationFrontendPath `
                -SemanticDestinationPath $ValidationSemanticPath
        }
        catch {
            Fail-AvidScriptCSharpSemanticCache `
                -Code (Get-AvidScriptSemanticCacheMappedCode $_.Exception) `
                -Message "Source report is not eligible for semantic cache publication: $($_.Exception.Message)"
        }

        $SourceReport = Assert-AvidScriptSemanticCachePublicationContext `
            -Context $Context `
            -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $ExpectedSourcePath `
            -ExpectedAuthorizationPackage $ExpectedAuthorizationPackage `
            -SourceReportPath $SourceReportPath
        Assert-AvidScriptCSharpSemanticCache `
            -Condition ((Get-AvidScriptBindingSha256Hex $SourceReportPath) -ceq $SourceReportSha256) `
            -Code "ASBI4502" `
            -Message "Semantic cache source report changed during publication validation."
        $CacheLock = Enter-AvidScriptSemanticCacheKeyLock -Context $Context

        if (Test-Path -LiteralPath $EntryDirectory) {
            $Winner = Import-AvidScriptCSharpSemanticCacheEntry `
                -Context $Context `
                -ProjectRoot $ProjectRoot `
                -ExpectedSourcePath $ExpectedSourcePath `
                -ExpectedAuthorizationPackage $ExpectedAuthorizationPackage `
                -FrontendDestinationPath (Join-Path $ValidationDirectory "winner.frontend.json") `
                -SemanticDestinationPath (Join-Path $ValidationDirectory "winner.semantic.json") `
                -CacheLock $CacheLock
            if ($Winner.Status -ceq "hit") {
                $PublicationResult = [pscustomobject]@{
                    Published = $false
                    Reused = $true
                    EntryReportPath = $Winner.EntryReportPath
                    EntryReportSha256 = $Winner.EntryReportSha256
                    DiagnosticCode = ""
                    DiagnosticMessage = ""
                }
                return $PublicationResult
            }
            if (Test-Path -LiteralPath $EntryDirectory) {
                $WinnerFailureCode = if ($Winner.DiagnosticCode -ceq "ASBI4505") { "ASBI4505" } else { "ASBI4504" }
                Fail-AvidScriptCSharpSemanticCache `
                    -Code $WinnerFailureCode `
                    -Message "Existing semantic cache winner could not be reused or isolated: $($Winner.DiagnosticMessage)"
            }
        }

        New-Item -ItemType Directory -Path $StagingDirectory | Out-Null
        $StagingFrontendPath = Join-Path $StagingDirectory "semantic.frontend.json"
        $StagingSemanticPath = Join-Path $StagingDirectory "semantic.model.json"
        Copy-Item -LiteralPath $ValidationFrontendPath -Destination $StagingFrontendPath
        Copy-Item -LiteralPath $ValidationSemanticPath -Destination $StagingSemanticPath
        $EntryReport = [ordered]@{
            schema_version = 1
            language = "csharp"
            module_id = [string]$SourceReport.module_id
            result = "direct_abi_built"
            succeeded = $true
            source = $SourceReport.source
            output_root = Convert-ToAvidScriptSemanticCacheReportPath `
                -ProjectRoot $ProjectRoot `
                -Path $EntryDirectory `
                -FieldName "semantic cache entry output root"
            binding_authorization = $SourceReport.binding_authorization
            artifacts = [ordered]@{
                frontend_file = Convert-ToAvidScriptSemanticCacheReportPath `
                    -ProjectRoot $ProjectRoot `
                    -Path (Join-Path $EntryDirectory "semantic.frontend.json") `
                    -FieldName "semantic cache frontend artifact"
                semantic_file = Convert-ToAvidScriptSemanticCacheReportPath `
                    -ProjectRoot $ProjectRoot `
                    -Path (Join-Path $EntryDirectory "semantic.model.json") `
                    -FieldName "semantic cache semantic artifact"
            }
            frontend = [ordered]@{
                schema_version = 1
                version = "1.0"
                artifact_sha256 = Get-AvidScriptBindingSha256Hex $StagingFrontendPath
            }
            semantic = [ordered]@{
                schema_version = 12
                version = "1.12"
                succeeded = $true
                source_sha256 = [string]$Prepared.SemanticModel.source.sha256
                frontend_sha256 = [string]$Prepared.SemanticModel.source.frontend_sha256
                artifact_sha256 = Get-AvidScriptBindingSha256Hex $StagingSemanticPath
            }
            semantic_cache = [ordered]@{
                schema_version = 1
                key = [string]$Context.CacheKey
                toolchain_fingerprint = [string]$Context.ToolchainFingerprint
            }
        }
        $ExpectedEntryReportSha256 = Get-AvidScriptUtf8JsonSha256 $EntryReport
        $StagingEntryReportPath = Join-Path $StagingDirectory "entry.csharp.report.json"
        Write-AvidScriptSemanticCacheJson `
            -Path $StagingEntryReportPath `
            -Value $EntryReport

        $StagingValidationReport = Read-AvidScriptSemanticCacheJson `
            -Path $StagingEntryReportPath `
            -Label "Staged semantic cache entry report"
        $StagedOutputRoot = Resolve-AvidScriptBindingPath `
            -RootPath $ProjectRoot `
            -Path ([string]$StagingValidationReport.output_root)
        $StagedFrontendReportPath = Resolve-AvidScriptBindingPath `
            -RootPath $ProjectRoot `
            -Path ([string]$StagingValidationReport.artifacts.frontend_file)
        $StagedSemanticReportPath = Resolve-AvidScriptBindingPath `
            -RootPath $ProjectRoot `
            -Path ([string]$StagingValidationReport.artifacts.semantic_file)
        Assert-AvidScriptCSharpSemanticCache `
            -Condition ((Get-AvidScriptUtf8JsonSha256 $StagingValidationReport) -ceq $ExpectedEntryReportSha256 -and
                [int]$StagingValidationReport.schema_version -eq 1 -and
                [string]$StagingValidationReport.language -ceq "csharp" -and
                [string]$StagingValidationReport.result -ceq "direct_abi_built" -and
                [bool]$StagingValidationReport.succeeded -and
                $StagedOutputRoot.Equals($EntryDirectory, [System.StringComparison]::OrdinalIgnoreCase) -and
                $StagedFrontendReportPath.Equals(
                    (Join-Path $EntryDirectory "semantic.frontend.json"),
                    [System.StringComparison]::OrdinalIgnoreCase) -and
                $StagedSemanticReportPath.Equals(
                    (Join-Path $EntryDirectory "semantic.model.json"),
                    [System.StringComparison]::OrdinalIgnoreCase) -and
                (Get-AvidScriptUtf8JsonSha256 $StagingValidationReport.source) -ceq
                    (Get-AvidScriptUtf8JsonSha256 $SourceReport.source) -and
                (Get-AvidScriptUtf8JsonSha256 $StagingValidationReport.binding_authorization) -ceq
                    (Get-AvidScriptUtf8JsonSha256 $SourceReport.binding_authorization) -and
                [string]$StagingValidationReport.frontend.artifact_sha256 -ceq
                    (Get-AvidScriptBindingSha256Hex $StagingFrontendPath) -and
                [string]$StagingValidationReport.semantic.artifact_sha256 -ceq
                    (Get-AvidScriptBindingSha256Hex $StagingSemanticPath) -and
                [int]$StagingValidationReport.semantic_cache.schema_version -eq 1 -and
                [string]$StagingValidationReport.semantic_cache.key -ceq [string]$Context.CacheKey -and
                [string]$StagingValidationReport.semantic_cache.toolchain_fingerprint -ceq
                    [string]$Context.ToolchainFingerprint) `
            -Code "ASBI4502" `
            -Message "Staged semantic cache entry report bytes failed publication validation."
        $StagingValidationReport.output_root = Convert-ToAvidScriptSemanticCacheReportPath `
            -ProjectRoot $ProjectRoot `
            -Path $StagingDirectory `
            -FieldName "semantic cache staging output root"
        $StagingValidationReport.artifacts.frontend_file = Convert-ToAvidScriptSemanticCacheReportPath `
            -ProjectRoot $ProjectRoot `
            -Path $StagingFrontendPath `
            -FieldName "semantic cache staging frontend artifact"
        $StagingValidationReport.artifacts.semantic_file = Convert-ToAvidScriptSemanticCacheReportPath `
            -ProjectRoot $ProjectRoot `
            -Path $StagingSemanticPath `
            -FieldName "semantic cache staging semantic artifact"
        $StagingValidationReportPath = Join-Path $StagingDirectory ".staging-validation.csharp.report.json"
        Write-AvidScriptSemanticCacheJson -Path $StagingValidationReportPath -Value $StagingValidationReport
        Import-AvidScriptCSharpPreparedSemantic `
            -PreparedReportPath $StagingValidationReportPath `
            -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $ExpectedSourcePath `
            -ExpectedAuthorizationPackage $ExpectedAuthorizationPackage `
            -FrontendDestinationPath (Join-Path $ValidationDirectory "staging.frontend.json") `
            -SemanticDestinationPath (Join-Path $ValidationDirectory "staging.semantic.json") | Out-Null
        Remove-Item -LiteralPath $StagingValidationReportPath -Force -ErrorAction Stop

        $WonPublication = $true
        try {
            Move-AvidScriptSemanticCacheDirectory `
                -SourcePath $StagingDirectory `
                -DestinationPath $EntryDirectory
        }
        catch {
            if (-not (Test-Path -LiteralPath $EntryDirectory -PathType Container)) {
                Fail-AvidScriptCSharpSemanticCache `
                    -Code "ASBI4504" `
                    -Message "Semantic cache entry directory publication failed: $($_.Exception.Message)"
            }
            $WonPublication = $false
        }

        $PublishedEntry = Import-AvidScriptCSharpSemanticCacheEntry `
            -Context $Context `
            -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $ExpectedSourcePath `
            -ExpectedAuthorizationPackage $ExpectedAuthorizationPackage `
            -FrontendDestinationPath (Join-Path $ValidationDirectory "published.frontend.json") `
            -SemanticDestinationPath (Join-Path $ValidationDirectory "published.semantic.json") `
            -CacheLock $CacheLock
        if ($PublishedEntry.Status -cne "hit") {
            Fail-AvidScriptCSharpSemanticCache `
                -Code "ASBI4504" `
                -Message "Published semantic cache entry failed winner validation: $($PublishedEntry.DiagnosticMessage)"
        }
        $PublicationResult = [pscustomobject]@{
            Published = $WonPublication
            Reused = -not $WonPublication
            EntryReportPath = $PublishedEntry.EntryReportPath
            EntryReportSha256 = $PublishedEntry.EntryReportSha256
            DiagnosticCode = ""
            DiagnosticMessage = ""
        }
        return $PublicationResult
    }
    catch {
        $StructuredCode = [string]$_.Exception.Data["AvidScriptCode"]
        if ($StructuredCode.StartsWith("ASBI45", [System.StringComparison]::Ordinal)) {
            throw
        }
        Fail-AvidScriptCSharpSemanticCache `
            -Code "ASBI4504" `
            -Message "Semantic cache entry publication failed: $($_.Exception.Message)"
    }
    finally {
        $CleanupFailures = @()
        foreach ($CleanupDirectory in @($ValidationDirectory, $StagingDirectory)) {
            if (-not [string]::IsNullOrWhiteSpace([string]$CleanupDirectory) -and
                (Test-Path -LiteralPath $CleanupDirectory -PathType Container)) {
                try {
                    Remove-AvidScriptSemanticCacheDirectory -Path $CleanupDirectory
                }
                catch {
                    $CleanupFailures += $_.Exception.Message
                }
            }
        }
        if ($null -ne $CacheLock) {
            try {
                $CacheLock.Dispose()
            }
            catch {
                $CleanupFailures += $_.Exception.Message
            }
        }
        if ($CleanupFailures.Count -gt 0) {
            $CleanupMessage = "Semantic cache transaction cleanup failed: $($CleanupFailures -join '; ')"
            if ($null -eq $PublicationResult) {
                Fail-AvidScriptCSharpSemanticCache -Code "ASBI4505" -Message $CleanupMessage
            }
            $PublicationResult.DiagnosticCode = "ASBI4505"
            $PublicationResult.DiagnosticMessage = $CleanupMessage
        }
    }
}
