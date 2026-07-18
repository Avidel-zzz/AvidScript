$ErrorActionPreference = "Stop"
$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $BuildDir "AvidScriptCSharpBindingPackage.ps1")

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
            ($_.Extension -ceq ".cs" -or $_.Extension -ceq ".csproj") -and
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
            -Condition ($FullPath.StartsWith($PluginPrefix, [System.StringComparison]::OrdinalIgnoreCase)) `
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

    $ProjectSavedRoot = Join-Path $ProjectRootFullPath "Saved"
    Assert-AvidScriptCSharpSemanticCache `
        -Condition (Test-AvidScriptBindingPathContained -RootPath $ProjectSavedRoot -CandidatePath $CacheRootFullPath) `
        -Code "ASBI4503" `
        -Message "Semantic cache root must be physically contained by the project Saved directory."

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
        DotNetSdkVersion = [string]$Toolchain.DotNetSdkVersion
        CacheRoot = $CacheRootFullPath
        EntryDirectory = $EntryDirectory
        EntryReportPath = Join-Path $EntryDirectory "entry.csharp.report.json"
        DiagnosticCode = ""
        DiagnosticMessage = ""
        CanonicalInput = $CanonicalInput
    }
}
