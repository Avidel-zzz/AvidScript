param()

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$CacheHelperPath = Join-Path $PluginRoot "Build\AvidScriptCSharpSemanticCache.ps1"
$PreparedHelperPath = Join-Path $PluginRoot "Build\AvidScriptCSharpPreparedSemantic.ps1"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\SemanticCacheEntryContracts"
$CacheRoot = Join-Path $ProjectRoot "Saved\AvidScript\SemanticCacheEntryContracts\v1"
$CorruptRoot = Join-Path $CacheRoot "Corrupt"
$TransactionRoot = Join-Path $ProjectRoot "Intermediate\AvidScript\SemanticCacheTransactions"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Write-Utf8File {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text
    )

    $Directory = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($Directory)) {
        New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Text, $Utf8)
}

function Write-JsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    Write-Utf8File -Path $Path -Text ($Value | ConvertTo-Json -Depth 32)
}

function Convert-ToProjectRelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $ProjectRootFullPath = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $PathFullPath = [System.IO.Path]::GetFullPath($Path)
    return $PathFullPath.Substring($ProjectRootFullPath.Length + 1).Replace("\", "/")
}

function Assert-RejectedImport {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [Parameter(Mandatory = $true)][string]$ExpectedCode,
        [Parameter(Mandatory = $true)][string]$CaseName,
        [bool]$ExpectIsolation = $true
    )

    $FrontendDestination = Join-Path $RunRoot "$CaseName\rejected.frontend.json"
    $SemanticDestination = Join-Path $RunRoot "$CaseName\rejected.semantic.json"
    $CorruptCountBefore = if (Test-Path -LiteralPath $CorruptRoot -PathType Container) {
        @(Get-ChildItem -LiteralPath $CorruptRoot -Force).Count
    }
    else { 0 }
    $Result = Import-AvidScriptCSharpSemanticCacheEntry `
        -Context $Context `
        -ProjectRoot $ProjectRoot `
        -ExpectedSourcePath $SourcePath `
        -ExpectedAuthorizationPackage $AuthorizationPackage `
        -FrontendDestinationPath $FrontendDestination `
        -SemanticDestinationPath $SemanticDestination
    Assert-Condition ($Result.Status -ceq "rejected") "$CaseName did not reject the invalid entry"
    Assert-Condition ($Result.DiagnosticCode -ceq $ExpectedCode) `
        "$CaseName diagnostic differs expected=$ExpectedCode actual=$($Result.DiagnosticCode)"
    Assert-Condition (-not (Test-Path -LiteralPath $FrontendDestination)) "$CaseName copied rejected frontend"
    Assert-Condition (-not (Test-Path -LiteralPath $SemanticDestination)) "$CaseName copied rejected semantic"
    if ($ExpectIsolation) {
        Assert-Condition (-not (Test-Path -LiteralPath $Context.EntryDirectory)) `
            "$CaseName did not remove the corrupt content-addressed entry"
        $CorruptCountAfter = @(Get-ChildItem -LiteralPath $CorruptRoot -Force).Count
        Assert-Condition ($CorruptCountAfter -eq ($CorruptCountBefore + 1)) `
            "$CaseName did not create exactly one quarantined entry"
    }
    else {
        Assert-Condition (Test-Path -LiteralPath $Context.EntryDirectory) `
            "$CaseName unexpectedly consumed the entry while isolation was disabled or failed"
    }
    return $Result
}

foreach ($Directory in @($RunRoot, (Split-Path -Parent $CacheRoot))) {
    if (Test-Path -LiteralPath $Directory) {
        Remove-Item -LiteralPath $Directory -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null
. $CacheHelperPath
. $PreparedHelperPath
Assert-Condition ($null -ne (Get-Command "Publish-AvidScriptCSharpSemanticCacheEntry" -ErrorAction SilentlyContinue)) `
    "semantic cache entry publisher is missing"
Assert-Condition ($null -ne (Get-Command "Import-AvidScriptCSharpSemanticCacheEntry" -ErrorAction SilentlyContinue)) `
    "semantic cache entry importer is missing"

$SourcePath = Join-Path $RunRoot "Source\SemanticCacheEntryScript.cs"
$ProjectPath = Join-Path $RunRoot "Source\SemanticCacheEntry.csproj"
$ReferenceSourcePath = Join-Path $RunRoot "Package\AvidScript.Bindings.generated.cs"
$PackageManifestPath = Join-Path $RunRoot "Package\package.json"
$PackageDescriptorPath = Join-Path $RunRoot "Package\bindings.v2.json"
$SeedOutputRoot = Join-Path $RunRoot "SeedOutput"
$SeedFrontendPath = Join-Path $SeedOutputRoot "seed.csharp.frontend.json"
$SeedSemanticPath = Join-Path $SeedOutputRoot "seed.csharp.semantic.json"
$SeedReportPath = Join-Path $SeedOutputRoot "seed.csharp.report.json"

Write-Utf8File -Path $SourcePath -Text "public static class SemanticCacheEntryScript {}"
Write-Utf8File -Path $ProjectPath -Text '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>'
Write-Utf8File -Path $ReferenceSourcePath -Text "namespace AvidScript.Bindings; public static class UE {}"
Write-Utf8File -Path $PackageManifestPath -Text '{"schema_version":1,"package_name":"avidscript.engine.gameplay"}'
Write-Utf8File -Path $PackageDescriptorPath -Text '{"schema_version":2,"package_name":"avidscript.engine.gameplay","bindings":[]}'

$SourceId = Convert-ToProjectRelativePath $SourcePath
$SourceSha256 = Get-AvidScriptBindingSha256Hex $SourcePath
$AuthorizationPackage = [pscustomobject]@{
    PackageName = "avidscript.engine.gameplay"
    PackageHash = ("1" * 64)
    ManifestPath = [System.IO.Path]::GetFullPath($PackageManifestPath)
    ManifestSha256 = Get-AvidScriptBindingSha256Hex $PackageManifestPath
    DescriptorPath = [System.IO.Path]::GetFullPath($PackageDescriptorPath)
    DescriptorSha256 = Get-AvidScriptBindingSha256Hex $PackageDescriptorPath
    ReferenceSourcePath = [System.IO.Path]::GetFullPath($ReferenceSourcePath)
    ReferenceSourceSha256 = Get-AvidScriptBindingSha256Hex $ReferenceSourcePath
    RequiredImports = @()
}

$FrontendModel = [ordered]@{
    schema_version = 1
    frontend_version = "1.0"
    succeeded = $true
    source = [ordered]@{ source_id = $SourceId; sha256 = $SourceSha256 }
    diagnostics = @()
}
Write-JsonFile -Path $SeedFrontendPath -Value $FrontendModel
$SemanticModel = [ordered]@{
    schema_version = 8
    semantic_version = "1.8"
    succeeded = $true
    source = [ordered]@{
        source_id = $SourceId
        sha256 = $SourceSha256
        frontend_sha256 = $SourceSha256
    }
    diagnostics = @()
}
Write-JsonFile -Path $SeedSemanticPath -Value $SemanticModel

$SeedReport = [ordered]@{
    schema_version = 1
    language = "csharp"
    module_id = "semantic_cache_entry_contract"
    result = "direct_abi_built"
    succeeded = $true
    source = [ordered]@{
        project = Convert-ToProjectRelativePath $ProjectPath
        file = $SourceId
        sha256 = $SourceSha256
        script_type = "SemanticCacheEntryScript"
    }
    output_root = Convert-ToProjectRelativePath $SeedOutputRoot
    binding_authorization = [ordered]@{
        required = $true
        package_name = $AuthorizationPackage.PackageName
        package_hash = $AuthorizationPackage.PackageHash
        manifest_file = Convert-ToProjectRelativePath $AuthorizationPackage.ManifestPath
        manifest_sha256 = $AuthorizationPackage.ManifestSha256
        descriptor_file = Convert-ToProjectRelativePath $AuthorizationPackage.DescriptorPath
        descriptor_sha256 = $AuthorizationPackage.DescriptorSha256
        reference_source_file = Convert-ToProjectRelativePath $AuthorizationPackage.ReferenceSourcePath
        reference_source_sha256 = $AuthorizationPackage.ReferenceSourceSha256
        profile_import_count = 0
        used_import_count = 0
        used_imports = @()
    }
    artifacts = [ordered]@{
        frontend_file = Convert-ToProjectRelativePath $SeedFrontendPath
        semantic_file = Convert-ToProjectRelativePath $SeedSemanticPath
    }
    frontend = [ordered]@{
        schema_version = 1
        version = "1.0"
        artifact_sha256 = Get-AvidScriptBindingSha256Hex $SeedFrontendPath
    }
    semantic = [ordered]@{
        schema_version = 8
        version = "1.8"
        succeeded = $true
        source_sha256 = $SourceSha256
        frontend_sha256 = $SourceSha256
        artifact_sha256 = Get-AvidScriptBindingSha256Hex $SeedSemanticPath
    }
}
Write-JsonFile -Path $SeedReportPath -Value $SeedReport

$Context = Get-AvidScriptCSharpSemanticCacheContext `
    -PluginRoot $PluginRoot `
    -ProjectRoot $ProjectRoot `
    -CacheRoot $CacheRoot `
    -Configuration "Release" `
    -SourcePath $SourcePath `
    -ProjectPath $ProjectPath `
    -AuthorizationPackage $AuthorizationPackage

$SeedReport.semantic_cache = [ordered]@{
    schema_version = 1
    key = [string]$Context.CacheKey
    toolchain_fingerprint = [string]$Context.ToolchainFingerprint
}
Write-JsonFile -Path $SeedReportPath -Value $SeedReport

Assert-Condition ($null -ne (Get-Command "Enter-AvidScriptSemanticCacheKeyLock" -ErrorAction SilentlyContinue)) `
    "semantic cache key lock helper is missing"
$FirstKeyLock = Enter-AvidScriptSemanticCacheKeyLock -Context $Context -TimeoutMilliseconds 1000
$SecondLockCode = ""
try {
    try {
        Enter-AvidScriptSemanticCacheKeyLock -Context $Context -TimeoutMilliseconds 50 | Out-Null
    }
    catch {
        $SecondLockCode = [string]$_.Exception.Data["AvidScriptCode"]
    }
}
finally {
    $FirstKeyLock.Dispose()
}
Assert-Condition ($SecondLockCode -ceq "ASBI4504") "semantic cache key lock did not enforce exclusive ownership"

$LockRoot = Join-Path $CacheRoot "Locks"
$LockPath = Join-Path $LockRoot "$($Context.CacheKey).lock"
if (Test-Path -LiteralPath $LockPath) {
    [System.IO.File]::Delete($LockPath)
}
$ExternalLockTarget = Join-Path $RunRoot "LockEscape\external.lock"
Write-Utf8File -Path $ExternalLockTarget -Text "external-lock-target"
try {
    New-Item -ItemType SymbolicLink -Path $LockPath -Target $ExternalLockTarget | Out-Null
}
catch {
    if ([string]$_.FullyQualifiedErrorId -notmatch 'NewItemSymbolicLinkElevationRequired') {
        throw
    }
    New-Item `
        -ItemType Junction `
        -Path $LockPath `
        -Target (Split-Path -Parent $ExternalLockTarget) | Out-Null
}
$LockEscapeCode = ""
try {
    Enter-AvidScriptSemanticCacheKeyLock -Context $Context -TimeoutMilliseconds 50 | Out-Null
}
catch {
    $LockEscapeCode = [string]$_.Exception.Data["AvidScriptCode"]
}
finally {
    if (Test-Path -LiteralPath $LockPath) {
        $LockAttributes = [System.IO.File]::GetAttributes($LockPath)
        if (($LockAttributes -band [System.IO.FileAttributes]::Directory) -ne 0) {
            [System.IO.Directory]::Delete($LockPath)
        }
        else {
            [System.IO.File]::Delete($LockPath)
        }
    }
}
Assert-Condition ($LockEscapeCode -ceq "ASBI4503") `
    "semantic cache key lock accepted a file symlink escape"
Assert-Condition ((Get-Content -Raw -LiteralPath $ExternalLockTarget) -ceq "external-lock-target") `
    "semantic cache key lock modified its symlink target"

$MismatchedContext = $Context | ConvertTo-Json -Depth 64 | ConvertFrom-Json
$MismatchedContext.CacheKey = ("d" * 64)
$MismatchedContext.EntryDirectory = Join-Path (Join-Path $CacheRoot "dd") $MismatchedContext.CacheKey
$MismatchedContext.EntryReportPath = Join-Path $MismatchedContext.EntryDirectory "entry.csharp.report.json"
$MismatchedContextCode = ""
try {
    Publish-AvidScriptCSharpSemanticCacheEntry `
        -Context $MismatchedContext `
        -ProjectRoot $ProjectRoot `
        -ExpectedSourcePath $SourcePath `
        -ExpectedAuthorizationPackage $AuthorizationPackage `
        -SourceReportPath $SeedReportPath | Out-Null
}
catch {
    $MismatchedContextCode = [string]$_.Exception.Data["AvidScriptCode"]
}
finally {
    if (Test-Path -LiteralPath $MismatchedContext.EntryDirectory) {
        Remove-Item -LiteralPath $MismatchedContext.EntryDirectory -Recurse -Force
    }
}
Assert-Condition ($MismatchedContextCode -ceq "ASBI4502") `
    "source report was published under a mismatched cache context"

$MissFrontendPath = Join-Path $RunRoot "Miss\miss.frontend.json"
$MissSemanticPath = Join-Path $RunRoot "Miss\miss.semantic.json"
$Miss = Import-AvidScriptCSharpSemanticCacheEntry `
    -Context $Context `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -FrontendDestinationPath $MissFrontendPath `
    -SemanticDestinationPath $MissSemanticPath
Assert-Condition ($Miss.Status -ceq "miss") "missing entry did not return miss"
Assert-Condition (-not (Test-Path -LiteralPath $MissFrontendPath)) "cache miss copied frontend"
Assert-Condition (-not (Test-Path -LiteralPath $MissSemanticPath)) "cache miss copied semantic"

function Write-AvidScriptSemanticCacheJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $Directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    if ((Split-Path -Leaf $Path) -ceq "entry.csharp.report.json") {
        [System.IO.File]::WriteAllText($Path, '{"corrupt":', $Utf8)
        return
    }
    Write-JsonFile -Path $Path -Value $Value
}
$StagedReportFaultCode = ""
try {
    Publish-AvidScriptCSharpSemanticCacheEntry `
        -Context $Context `
        -ProjectRoot $ProjectRoot `
        -ExpectedSourcePath $SourcePath `
        -ExpectedAuthorizationPackage $AuthorizationPackage `
        -SourceReportPath $SeedReportPath | Out-Null
}
catch {
    $StagedReportFaultCode = [string]$_.Exception.Data["AvidScriptCode"]
}
finally {
    Remove-Item -LiteralPath "Function:\Write-AvidScriptSemanticCacheJson" -Force
    . $CacheHelperPath
}
Assert-Condition ($StagedReportFaultCode -ceq "ASBI4502") `
    "corrupt staged entry report bytes reached atomic publication"
Assert-Condition (-not (Test-Path -LiteralPath $Context.EntryDirectory)) `
    "corrupt staged entry report created a content-addressed entry"

$script:TamperedReportMoveCount = 0
function Write-AvidScriptSemanticCacheJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $Directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    if ((Split-Path -Leaf $Path) -ceq "entry.csharp.report.json") {
        $Tampered = $Value | ConvertTo-Json -Depth 64 | ConvertFrom-Json
        $Tampered.module_id = "poisoned"
        $Tampered.artifacts | Add-Member -NotePropertyName "wasm_file" -NotePropertyValue "poisoned.wasm"
        Write-JsonFile -Path $Path -Value $Tampered
        return
    }
    Write-JsonFile -Path $Path -Value $Value
}
function Move-AvidScriptSemanticCacheDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    $script:TamperedReportMoveCount++
    throw "Tampered staged report reached atomic publication."
}
$TamperedReportCode = ""
try {
    Publish-AvidScriptCSharpSemanticCacheEntry `
        -Context $Context `
        -ProjectRoot $ProjectRoot `
        -ExpectedSourcePath $SourcePath `
        -ExpectedAuthorizationPackage $AuthorizationPackage `
        -SourceReportPath $SeedReportPath | Out-Null
}
catch {
    $TamperedReportCode = [string]$_.Exception.Data["AvidScriptCode"]
}
finally {
    Remove-Item -LiteralPath "Function:\Write-AvidScriptSemanticCacheJson" -Force
    Remove-Item -LiteralPath "Function:\Move-AvidScriptSemanticCacheDirectory" -Force
    . $CacheHelperPath
}
Assert-Condition ($TamperedReportCode -ceq "ASBI4502") `
    "well-formed staged entry report tampering was not rejected"
Assert-Condition ($script:TamperedReportMoveCount -eq 0) `
    "well-formed staged entry report tampering reached atomic publication"
Assert-Condition (-not (Test-Path -LiteralPath $Context.EntryDirectory)) `
    "well-formed staged entry report tampering created a content-addressed entry"

$Published = Publish-AvidScriptCSharpSemanticCacheEntry `
    -Context $Context `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -SourceReportPath $SeedReportPath
Assert-Condition ($Published.Published -and -not $Published.Reused) "cold cache entry was not published"
Assert-Condition (Test-Path -LiteralPath $Context.EntryReportPath -PathType Leaf) "entry report is missing"
Assert-Condition (Test-Path -LiteralPath (Join-Path $Context.EntryDirectory "semantic.frontend.json") -PathType Leaf) `
    "entry frontend is missing"
Assert-Condition (Test-Path -LiteralPath (Join-Path $Context.EntryDirectory "semantic.model.json") -PathType Leaf) `
    "entry semantic is missing"
$PublishedEntryReport = Get-Content -Raw -LiteralPath $Context.EntryReportPath | ConvertFrom-Json
foreach ($LoadableField in @("manifest_file", "wasm_file", "guest_ir_file")) {
    Assert-Condition ($PublishedEntryReport.artifacts.PSObject.Properties.Name -cnotcontains $LoadableField) `
        "semantic cache entry report exposes loadable artifact field $LoadableField"
}

$HitFrontendPath = Join-Path $RunRoot "Hit\hit.frontend.json"
$HitSemanticPath = Join-Path $RunRoot "Hit\hit.semantic.json"
$Hit = Import-AvidScriptCSharpSemanticCacheEntry `
    -Context $Context `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -FrontendDestinationPath $HitFrontendPath `
    -SemanticDestinationPath $HitSemanticPath
Assert-Condition ($Hit.Status -ceq "hit") "published entry did not hit"
Assert-Condition ((Get-AvidScriptBindingSha256Hex $HitFrontendPath) -ceq (Get-AvidScriptBindingSha256Hex $SeedFrontendPath)) `
    "cache hit frontend bytes differ"
Assert-Condition ((Get-AvidScriptBindingSha256Hex $HitSemanticPath) -ceq (Get-AvidScriptBindingSha256Hex $SeedSemanticPath)) `
    "cache hit semantic bytes differ"

$ExternalOutputParent = Join-Path ([System.IO.Path]::GetPathRoot($ProjectRoot)) "tmp"
$ExternalOutputRoot = Join-Path $ExternalOutputParent "AvidScriptSemanticCacheEntryContracts.$PID"
$ExternalOutputFullPath = [System.IO.Path]::GetFullPath($ExternalOutputRoot)
Assert-Condition (-not $ExternalOutputFullPath.StartsWith(
        ([System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd("\") + "\"),
        [System.StringComparison]::OrdinalIgnoreCase)) `
    "external output fixture unexpectedly belongs to the Unreal project"
$ExternalHitFrontendPath = Join-Path $ExternalOutputFullPath "external.frontend.json"
$ExternalHitSemanticPath = Join-Path $ExternalOutputFullPath "external.semantic.json"
Assert-AvidScriptSemanticCacheDestinations `
    -CacheRoot $CacheRoot `
    -FrontendDestinationPath $ExternalHitFrontendPath `
    -SemanticDestinationPath $ExternalHitSemanticPath
$ExternalOutputWritable = $true
try {
    New-Item -ItemType Directory -Force -Path $ExternalOutputFullPath | Out-Null
    $ExternalProbePath = Join-Path $ExternalOutputFullPath ".write-probe"
    [System.IO.File]::WriteAllText($ExternalProbePath, "probe", $Utf8)
    Remove-Item -LiteralPath $ExternalProbePath -Force
}
catch {
    $ExternalOutputWritable = $false
}
if ($ExternalOutputWritable) {
    try {
        $ExternalHit = Import-AvidScriptCSharpSemanticCacheEntry `
            -Context $Context `
            -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $SourcePath `
            -ExpectedAuthorizationPackage $AuthorizationPackage `
            -FrontendDestinationPath $ExternalHitFrontendPath `
            -SemanticDestinationPath $ExternalHitSemanticPath
        Assert-Condition ($ExternalHit.Status -ceq "hit") "external output root did not receive a cache hit"
        Assert-Condition ((Get-AvidScriptBindingSha256Hex $ExternalHitFrontendPath) -ceq (Get-AvidScriptBindingSha256Hex $SeedFrontendPath)) `
            "external cache hit frontend bytes differ"
        Assert-Condition ((Get-AvidScriptBindingSha256Hex $ExternalHitSemanticPath) -ceq (Get-AvidScriptBindingSha256Hex $SeedSemanticPath)) `
            "external cache hit semantic bytes differ"
    }
    finally {
        if (Test-Path -LiteralPath $ExternalOutputFullPath) {
            Remove-Item -LiteralPath $ExternalOutputFullPath -Recurse -Force
        }
    }
}

$EntryReportTextBeforeOverlap = Get-Content -Raw -LiteralPath $Context.EntryReportPath
$EntryReportHashBeforeOverlap = Get-AvidScriptBindingSha256Hex $Context.EntryReportPath
$OverlapSemanticDestination = Join-Path $RunRoot "DestinationOverlap\semantic.json"
try {
    $OverlapResult = Import-AvidScriptCSharpSemanticCacheEntry `
        -Context $Context `
        -ProjectRoot $ProjectRoot `
        -ExpectedSourcePath $SourcePath `
        -ExpectedAuthorizationPackage $AuthorizationPackage `
        -FrontendDestinationPath $Context.EntryReportPath `
        -SemanticDestinationPath $OverlapSemanticDestination
}
finally {
    Write-Utf8File -Path $Context.EntryReportPath -Text $EntryReportTextBeforeOverlap
    if (Test-Path -LiteralPath $OverlapSemanticDestination) {
        Remove-Item -LiteralPath $OverlapSemanticDestination -Force
    }
}
Assert-Condition (
    $OverlapResult.Status -ceq "rejected" -and
    $OverlapResult.DiagnosticCode -ceq "ASBI4503") `
    "cache-owned destination was not rejected"
Assert-Condition ((Get-AvidScriptBindingSha256Hex $Context.EntryReportPath) -ceq $EntryReportHashBeforeOverlap) `
    "cache-owned destination modified the immutable entry report"

$Winner = Publish-AvidScriptCSharpSemanticCacheEntry `
    -Context $Context `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -SourceReportPath $SeedReportPath
Assert-Condition (-not $Winner.Published -and $Winner.Reused) "existing winner was not reused"

Remove-Item -LiteralPath $Context.EntryReportPath -Force
Assert-RejectedImport -Context $Context -ExpectedCode "ASBI4502" -CaseName "MissingEntryReport" | Out-Null
$Rebuilt = Publish-AvidScriptCSharpSemanticCacheEntry `
    -Context $Context `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -SourceReportPath $SeedReportPath
Assert-Condition ($Rebuilt.Published -and -not $Rebuilt.Reused) "missing report entry was not republished"

Remove-Item -LiteralPath $Context.EntryDirectory -Recurse -Force
Write-Utf8File -Path $Context.EntryDirectory -Text "invalid entry file"
Assert-RejectedImport -Context $Context -ExpectedCode "ASBI4502" -CaseName "EntryPathFile" | Out-Null
$Rebuilt = Publish-AvidScriptCSharpSemanticCacheEntry `
    -Context $Context `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -SourceReportPath $SeedReportPath
Assert-Condition ($Rebuilt.Published -and -not $Rebuilt.Reused) "file-shaped entry was not republished"

$EntryFrontendPath = Join-Path $Context.EntryDirectory "semantic.frontend.json"
Write-Utf8File -Path $EntryFrontendPath -Text '{"tampered":true}'
Assert-RejectedImport -Context $Context -ExpectedCode "ASBI4502" -CaseName "ArtifactHash" | Out-Null
$Rebuilt = Publish-AvidScriptCSharpSemanticCacheEntry `
    -Context $Context `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -SourceReportPath $SeedReportPath
Assert-Condition ($Rebuilt.Published -and -not $Rebuilt.Reused) "artifact corruption was not republished"

$EntryReport = Get-Content -Raw -LiteralPath $Context.EntryReportPath | ConvertFrom-Json
$EntryReport.semantic_cache.key = ("f" * 64)
Write-JsonFile -Path $Context.EntryReportPath -Value $EntryReport
Assert-RejectedImport -Context $Context -ExpectedCode "ASBI4502" -CaseName "EntryKey" | Out-Null
$Rebuilt = Publish-AvidScriptCSharpSemanticCacheEntry `
    -Context $Context `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -SourceReportPath $SeedReportPath
Assert-Condition ($Rebuilt.Published -and -not $Rebuilt.Reused) "key corruption was not republished"

$EntryReport = Get-Content -Raw -LiteralPath $Context.EntryReportPath | ConvertFrom-Json
$EntryReport.semantic_cache.schema_version = 2
Write-JsonFile -Path $Context.EntryReportPath -Value $EntryReport
Assert-RejectedImport -Context $Context -ExpectedCode "ASBI4502" -CaseName "EntrySchema" | Out-Null
$Rebuilt = Publish-AvidScriptCSharpSemanticCacheEntry `
    -Context $Context `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -SourceReportPath $SeedReportPath
Assert-Condition ($Rebuilt.Published -and -not $Rebuilt.Reused) "schema corruption was not republished"

$OutsideFrontendPath = Join-Path $RunRoot "Outside\escaped.frontend.json"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutsideFrontendPath) | Out-Null
Copy-Item -LiteralPath $EntryFrontendPath -Destination $OutsideFrontendPath -Force
$EntryReport = Get-Content -Raw -LiteralPath $Context.EntryReportPath | ConvertFrom-Json
$EntryReport.artifacts.frontend_file = Convert-ToProjectRelativePath $OutsideFrontendPath
Write-JsonFile -Path $Context.EntryReportPath -Value $EntryReport
Assert-RejectedImport -Context $Context -ExpectedCode "ASBI4503" -CaseName "ArtifactEscape" | Out-Null
$Rebuilt = Publish-AvidScriptCSharpSemanticCacheEntry `
    -Context $Context `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -SourceReportPath $SeedReportPath
Assert-Condition ($Rebuilt.Published -and -not $Rebuilt.Reused) "path corruption was not republished"

$OutsideEntryDirectory = Join-Path $RunRoot "EntryJunction\OutsideEntry"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutsideEntryDirectory) | Out-Null
[System.IO.Directory]::Move($Context.EntryDirectory, $OutsideEntryDirectory)
New-Item -ItemType Junction -Path $Context.EntryDirectory -Target $OutsideEntryDirectory | Out-Null
try {
    Assert-RejectedImport `
        -Context $Context `
        -ExpectedCode "ASBI4503" `
        -CaseName "EntryJunction" `
        -ExpectIsolation $false | Out-Null
}
finally {
    if (Test-Path -LiteralPath $Context.EntryDirectory) {
        [System.IO.Directory]::Delete($Context.EntryDirectory)
    }
    [System.IO.Directory]::Move($OutsideEntryDirectory, $Context.EntryDirectory)
}

$OriginalEntryReportText = Get-Content -Raw -LiteralPath $Context.EntryReportPath
$EntryReport = $OriginalEntryReportText | ConvertFrom-Json
$EntryReport.semantic_cache.key = ("e" * 64)
Write-JsonFile -Path $Context.EntryReportPath -Value $EntryReport
function Move-AvidScriptSemanticCacheDirectory {
    param([string]$SourcePath, [string]$DestinationPath)
    throw "Injected corrupt isolation failure."
}
try {
    Assert-RejectedImport `
        -Context $Context `
        -ExpectedCode "ASBI4505" `
        -CaseName "IsolationFailure" `
        -ExpectIsolation $false | Out-Null
}
finally {
    Remove-Item -LiteralPath "Function:\Move-AvidScriptSemanticCacheDirectory" -Force
    Write-Utf8File -Path $Context.EntryReportPath -Text $OriginalEntryReportText
}

Remove-Item -LiteralPath $Context.EntryDirectory -Recurse -Force
function Move-AvidScriptSemanticCacheDirectory {
    param([string]$SourcePath, [string]$DestinationPath)
    [System.IO.Directory]::Move($SourcePath, $DestinationPath)
    throw "Injected concurrent winner publication."
}
try {
    $ConcurrentWinner = Publish-AvidScriptCSharpSemanticCacheEntry `
        -Context $Context `
        -ProjectRoot $ProjectRoot `
        -ExpectedSourcePath $SourcePath `
        -ExpectedAuthorizationPackage $AuthorizationPackage `
        -SourceReportPath $SeedReportPath
}
finally {
    Remove-Item -LiteralPath "Function:\Move-AvidScriptSemanticCacheDirectory" -Force
}
Assert-Condition (-not $ConcurrentWinner.Published -and $ConcurrentWinner.Reused) `
    "concurrent semantic cache winner was not reported as reused"

$DefaultReport = $SeedReport | ConvertTo-Json -Depth 32 | ConvertFrom-Json
$DefaultReport.binding_authorization = [pscustomobject][ordered]@{
    required = $false
    package_name = ""
    package_hash = ""
    manifest_file = ""
    manifest_sha256 = ""
    descriptor_file = ""
    descriptor_sha256 = ""
    reference_source_file = ""
    reference_source_sha256 = ""
    profile_import_count = 0
    used_import_count = 0
    used_imports = @()
}
$DefaultReportPath = Join-Path $SeedOutputRoot "default.csharp.report.json"
Write-JsonFile -Path $DefaultReportPath -Value $DefaultReport
$DefaultPrepared = Import-AvidScriptCSharpPreparedSemantic `
    -PreparedReportPath $DefaultReportPath `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $null `
    -FrontendDestinationPath (Join-Path $RunRoot "DefaultAuthorization\default.frontend.json") `
    -SemanticDestinationPath (Join-Path $RunRoot "DefaultAuthorization\default.semantic.json")
Assert-Condition ($null -ne $DefaultPrepared.FrontendModel -and $null -ne $DefaultPrepared.SemanticModel) `
    "default script prepared semantic import did not accept explicit empty authorization"

function Get-DefaultAuthorizationFailureCode {
    param(
        [Parameter(Mandatory = $true)]$AuthorizationModel,
        [Parameter(Mandatory = $true)][string]$CaseName
    )

    $CaseReport = $DefaultReport | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $CaseReport.binding_authorization = $AuthorizationModel
    $CaseReportPath = Join-Path $SeedOutputRoot "$CaseName.csharp.report.json"
    $FrontendDestination = Join-Path $RunRoot "$CaseName\frontend.json"
    $SemanticDestination = Join-Path $RunRoot "$CaseName\semantic.json"
    Write-JsonFile -Path $CaseReportPath -Value $CaseReport
    try {
        Import-AvidScriptCSharpPreparedSemantic `
            -PreparedReportPath $CaseReportPath `
            -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $SourcePath `
            -ExpectedAuthorizationPackage $null `
            -FrontendDestinationPath $FrontendDestination `
            -SemanticDestinationPath $SemanticDestination | Out-Null
    }
    catch {
        return [string]$_.Exception.Data["AvidScriptCode"]
    }
    finally {
        foreach ($Destination in @($FrontendDestination, $SemanticDestination)) {
            if (Test-Path -LiteralPath $Destination) {
                Remove-Item -LiteralPath $Destination -Force
            }
        }
    }
    return ""
}

$PartialDefaultCode = Get-DefaultAuthorizationFailureCode `
    -AuthorizationModel ([pscustomobject]@{ used_imports = @() }) `
    -CaseName "PartialDefaultAuthorization"
Assert-Condition ($PartialDefaultCode -ceq "ASBI4402") `
    "partial default authorization object was accepted"
$ScalarDefaultCode = Get-DefaultAuthorizationFailureCode `
    -AuthorizationModel "none" `
    -CaseName "ScalarDefaultAuthorization"
Assert-Condition ($ScalarDefaultCode -ceq "ASBI4402") `
    "scalar default authorization value was accepted"

$InvalidDefaultReport = $DefaultReport | ConvertTo-Json -Depth 32 | ConvertFrom-Json
$InvalidDefaultReport.binding_authorization.package_name = "unexpected.package"
$InvalidDefaultReportPath = Join-Path $SeedOutputRoot "invalid_default.csharp.report.json"
Write-JsonFile -Path $InvalidDefaultReportPath -Value $InvalidDefaultReport
$InvalidDefaultCode = ""
try {
    Import-AvidScriptCSharpPreparedSemantic `
        -PreparedReportPath $InvalidDefaultReportPath `
        -ProjectRoot $ProjectRoot `
        -ExpectedSourcePath $SourcePath `
        -ExpectedAuthorizationPackage $null `
        -FrontendDestinationPath (Join-Path $RunRoot "InvalidDefaultAuthorization\invalid.frontend.json") `
        -SemanticDestinationPath (Join-Path $RunRoot "InvalidDefaultAuthorization\invalid.semantic.json") | Out-Null
}
catch {
    $InvalidDefaultCode = [string]$_.Exception.Data["AvidScriptCode"]
}
Assert-Condition ($InvalidDefaultCode -ceq "ASBI4402") `
    "default script accepted non-empty authorization identity"

function Remove-AvidScriptSemanticCacheDirectory {
    param([string]$Path)
    throw "Injected semantic cache cleanup failure."
}
try {
    $CleanupWarningResult = Publish-AvidScriptCSharpSemanticCacheEntry `
        -Context $Context `
        -ProjectRoot $ProjectRoot `
        -ExpectedSourcePath $SourcePath `
        -ExpectedAuthorizationPackage $AuthorizationPackage `
        -SourceReportPath $SeedReportPath
}
finally {
    Remove-Item -LiteralPath "Function:\Remove-AvidScriptSemanticCacheDirectory" -Force
}
Assert-Condition ($CleanupWarningResult.Reused -and -not $CleanupWarningResult.Published) `
    "cleanup failure changed an already validated winner result"
Assert-Condition ($CleanupWarningResult.DiagnosticCode -ceq "ASBI4505") `
    "cleanup failure did not return ASBI4505"
$TransientRoots = @($CacheRoot, $TransactionRoot)
$TransientDirectories = @()
foreach ($TransientRoot in $TransientRoots) {
    if (Test-Path -LiteralPath $TransientRoot -PathType Container) {
        $TransientDirectories += @(Get-ChildItem -LiteralPath $TransientRoot -Directory -Recurse -Force | Where-Object {
            $_.Name -match '^\.(?:validation|staging)\.'
        })
    }
}
foreach ($TransientDirectory in $TransientDirectories) {
    Microsoft.PowerShell.Management\Remove-Item -LiteralPath $TransientDirectory.FullName -Recurse -Force
}

$TransientCachePaths = @()
foreach ($TransientRoot in $TransientRoots) {
    if (Test-Path -LiteralPath $TransientRoot -PathType Container) {
        $TransientCachePaths += @(Get-ChildItem -LiteralPath $TransientRoot -Recurse -Force | Where-Object {
            $_.Name -match '^\.(?:validation|staging)\.'
        })
    }
}
Assert-Condition ($TransientCachePaths.Count -eq 0) "semantic cache left validation or staging paths"

Write-Output "AvidScript.CSharpFrontend.SemanticCacheEntryContracts: 25/25 passed"
exit 0
