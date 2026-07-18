param()

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$HelperPath = Join-Path $PluginRoot "Build\AvidScriptCSharpSemanticCache.ps1"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\SemanticCacheContracts"
$ProjectCacheRunRoot = Join-Path $ProjectRoot "Saved\AvidScript\SemanticCacheContracts"
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

function Assert-CacheContextFailure {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Arguments,
        [Parameter(Mandatory = $true)][string]$ExpectedCode,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $ObservedCode = ""
    try {
        Get-AvidScriptCSharpSemanticCacheContext @Arguments | Out-Null
    }
    catch {
        $ObservedCode = [string]$_.Exception.Data["AvidScriptCode"]
    }
    Assert-Condition ($ObservedCode -ceq $ExpectedCode) `
        "$Message expected=$ExpectedCode actual=$ObservedCode"
}

if (Test-Path -LiteralPath $RunRoot) {
    Remove-Item -LiteralPath $RunRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null
if (Test-Path -LiteralPath $ProjectCacheRunRoot) {
    Remove-Item -LiteralPath $ProjectCacheRunRoot -Recurse -Force
}
Assert-Condition (Test-Path -LiteralPath $HelperPath -PathType Leaf) `
    "semantic cache helper is missing: $HelperPath"
. $HelperPath

$ToolchainRoot = Join-Path $RunRoot "Toolchain"
$SourcePath = Join-Path $RunRoot "Source\CacheContractScript.cs"
$AlternateSourcePath = Join-Path $RunRoot "Source\CacheContractScriptCopy.cs"
$ProjectPath = Join-Path $RunRoot "Source\CacheContract.csproj"
$ReferenceSourcePath = Join-Path $RunRoot "References\AvidScript.Bindings.generated.cs"
$CacheRoot = Join-Path $ProjectCacheRunRoot "CacheA\v1"
$AlternateCacheRoot = Join-Path $ProjectCacheRunRoot "CacheB\v1"
$ReservedArtifactCacheRoot = Join-Path $ProjectRoot "Saved\AvidScriptCSharpGuest\ActorLifecycle\SemanticCache\v1"
$FrontendToolPath = Join-Path $ToolchainRoot "Tools\AvidScript.CSharpFrontend\Frontend.cs"
$SemanticToolPath = Join-Path $ToolchainRoot "Tools\AvidScript.CSharpSemantic\Semantic.cs"
$GlobalJsonPath = Join-Path $ToolchainRoot "global.json"

Write-Utf8File -Path $SourcePath -Text "public static class CacheContractScript {}"
Copy-Item -LiteralPath $SourcePath -Destination $AlternateSourcePath -Force
Write-Utf8File -Path $ProjectPath -Text '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>'
Write-Utf8File -Path $ReferenceSourcePath -Text "namespace AvidScript.Bindings; public static class UE {}"
Write-Utf8File -Path $GlobalJsonPath -Text '{"sdk":{"version":"8.0.416","rollForward":"disable","allowPrerelease":false}}'
Write-Utf8File -Path (Join-Path $ToolchainRoot "Build\InvokeCSharpFrontend.ps1") -Text 'Write-Output "frontend"'
Write-Utf8File -Path (Join-Path $ToolchainRoot "Build\InvokeCSharpSemantic.ps1") -Text 'Write-Output "semantic"'
Write-Utf8File -Path $FrontendToolPath -Text "namespace Frontend; public static class Analyzer {}"
Write-Utf8File -Path (Join-Path $ToolchainRoot "Tools\AvidScript.CSharpFrontend\Frontend.csproj") -Text '<Project Sdk="Microsoft.NET.Sdk" />'
Write-Utf8File -Path $SemanticToolPath -Text "namespace Semantic; public static class Analyzer {}"
Write-Utf8File -Path (Join-Path $ToolchainRoot "Tools\AvidScript.CSharpSemantic\Semantic.csproj") -Text '<Project Sdk="Microsoft.NET.Sdk" />'
Write-Utf8File -Path (Join-Path $ToolchainRoot "Tools\AvidScript.CSharpFrontend\bin\Ignored.cs") -Text "ignored bin"
Write-Utf8File -Path (Join-Path $ToolchainRoot "Tools\AvidScript.CSharpSemantic\obj\Ignored.cs") -Text "ignored obj"

$AuthorizationPackage = [pscustomobject]@{
    PackageName = "avidscript.engine.gameplay"
    PackageHash = ("1" * 64)
    ManifestSha256 = ("2" * 64)
    DescriptorSha256 = ("3" * 64)
    ReferenceSourcePath = [System.IO.Path]::GetFullPath($ReferenceSourcePath)
    ReferenceSourceSha256 = Get-AvidScriptBindingSha256Hex $ReferenceSourcePath
}

$ContextArguments = @{
    PluginRoot = $ToolchainRoot
    ProjectRoot = $ProjectRoot
    CacheRoot = $CacheRoot
    Configuration = "Release"
    SourcePath = $SourcePath
    ProjectPath = $ProjectPath
    AuthorizationPackage = $AuthorizationPackage
}

$First = Get-AvidScriptCSharpSemanticCacheContext @ContextArguments
$Second = Get-AvidScriptCSharpSemanticCacheContext @ContextArguments
Assert-Condition ($First.Enabled -and $First.CacheKey -ceq $Second.CacheKey) `
    "identical semantic inputs did not produce the same cache key"
Assert-Condition ($First.CacheKey -match '^[0-9a-f]{64}$') "cache key is not lowercase SHA-256"
Assert-Condition ($First.ToolchainFingerprint -match '^[0-9a-f]{64}$') "toolchain fingerprint is not lowercase SHA-256"

$AlternateRootArguments = $ContextArguments.Clone()
$AlternateRootArguments.CacheRoot = $AlternateCacheRoot
$AlternateRoot = Get-AvidScriptCSharpSemanticCacheContext @AlternateRootArguments
Assert-Condition ($AlternateRoot.CacheKey -ceq $First.CacheKey) "cache root changed the semantic cache key"
Assert-Condition ($AlternateRoot.EntryDirectory -cne $First.EntryDirectory) "cache root did not change entry location"

$OutsideRootArguments = $ContextArguments.Clone()
$OutsideRootArguments.CacheRoot = Join-Path $RunRoot "OutsideProjectSaved"
Assert-CacheContextFailure `
    -Arguments $OutsideRootArguments `
    -ExpectedCode "ASBI4503" `
    -Message "cache root outside project Saved was accepted"

$ReservedRootArguments = $ContextArguments.Clone()
$ReservedRootArguments.CacheRoot = $ReservedArtifactCacheRoot
Assert-CacheContextFailure `
    -Arguments $ReservedRootArguments `
    -ExpectedCode "ASBI4503" `
    -Message "cache root overlapping final artifact ownership was accepted"

$CacheRootFile = Join-Path $ProjectCacheRunRoot "CacheRootFile"
Write-Utf8File -Path $CacheRootFile -Text "not a directory"
$CacheRootFileArguments = $ContextArguments.Clone()
$CacheRootFileArguments.CacheRoot = $CacheRootFile
Assert-CacheContextFailure `
    -Arguments $CacheRootFileArguments `
    -ExpectedCode "ASBI4503" `
    -Message "cache root file was accepted"

$JunctionTarget = Join-Path $RunRoot "JunctionTarget"
$JunctionRoot = Join-Path $ProjectCacheRunRoot "JunctionRoot"
New-Item -ItemType Directory -Force -Path $JunctionTarget | Out-Null
New-Item -ItemType Directory -Force -Path $ProjectCacheRunRoot | Out-Null
New-Item -ItemType Junction -Path $JunctionRoot -Target $JunctionTarget | Out-Null
try {
    $JunctionArguments = $ContextArguments.Clone()
    $JunctionArguments.CacheRoot = Join-Path $JunctionRoot "v1"
    Assert-CacheContextFailure `
        -Arguments $JunctionArguments `
        -ExpectedCode "ASBI4503" `
        -Message "cache root junction escape was accepted"
}
finally {
    if (Test-Path -LiteralPath $JunctionRoot) {
        [System.IO.Directory]::Delete($JunctionRoot)
    }
}

$AlternateSourceArguments = $ContextArguments.Clone()
$AlternateSourceArguments.SourcePath = $AlternateSourcePath
$AlternateSource = Get-AvidScriptCSharpSemanticCacheContext @AlternateSourceArguments
Assert-Condition ($AlternateSource.CacheKey -cne $First.CacheKey) "source identity did not invalidate cache key"
Write-Utf8File -Path $SourcePath -Text "public static class CacheContractScript { public static int Value = 1; }"
$ChangedSource = Get-AvidScriptCSharpSemanticCacheContext @ContextArguments
Assert-Condition ($ChangedSource.CacheKey -cne $First.CacheKey) "source content did not invalidate cache key"
Write-Utf8File -Path $SourcePath -Text "public static class CacheContractScript {}"

Write-Utf8File -Path $ProjectPath -Text '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework><LangVersion>latest</LangVersion></PropertyGroup></Project>'
$ChangedProject = Get-AvidScriptCSharpSemanticCacheContext @ContextArguments
Assert-Condition ($ChangedProject.CacheKey -cne $First.CacheKey) "project content did not invalidate cache key"
Write-Utf8File -Path $ProjectPath -Text '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>'

$DebugArguments = $ContextArguments.Clone()
$DebugArguments.Configuration = "Debug"
$DebugContext = Get-AvidScriptCSharpSemanticCacheContext @DebugArguments
Assert-Condition ($DebugContext.CacheKey -cne $First.CacheKey) "configuration did not invalidate cache key"

$ChangedAuthorization = [pscustomobject]@{
    PackageName = $AuthorizationPackage.PackageName
    PackageHash = ("4" * 64)
    ManifestSha256 = $AuthorizationPackage.ManifestSha256
    DescriptorSha256 = $AuthorizationPackage.DescriptorSha256
    ReferenceSourcePath = $AuthorizationPackage.ReferenceSourcePath
    ReferenceSourceSha256 = $AuthorizationPackage.ReferenceSourceSha256
}
$ChangedAuthorizationArguments = $ContextArguments.Clone()
$ChangedAuthorizationArguments.AuthorizationPackage = $ChangedAuthorization
$ChangedAuthorizationContext = Get-AvidScriptCSharpSemanticCacheContext @ChangedAuthorizationArguments
Assert-Condition ($ChangedAuthorizationContext.CacheKey -cne $First.CacheKey) "authorization identity did not invalidate cache key"

Write-Utf8File -Path $ReferenceSourcePath -Text "namespace AvidScript.Bindings; public static class UE { public static int Version => 2; }"
$AuthorizationPackage.ReferenceSourceSha256 = Get-AvidScriptBindingSha256Hex $ReferenceSourcePath
$ChangedReference = Get-AvidScriptCSharpSemanticCacheContext @ContextArguments
Assert-Condition ($ChangedReference.CacheKey -cne $First.CacheKey) "reference source did not invalidate cache key"
Write-Utf8File -Path $ReferenceSourcePath -Text "namespace AvidScript.Bindings; public static class UE {}"
$AuthorizationPackage.ReferenceSourceSha256 = Get-AvidScriptBindingSha256Hex $ReferenceSourcePath

Write-Utf8File -Path $GlobalJsonPath -Text '{"sdk":{"version":"8.0.417","rollForward":"disable","allowPrerelease":false}}'
$ChangedSdk = Get-AvidScriptCSharpSemanticCacheContext @ContextArguments
Assert-Condition ($ChangedSdk.CacheKey -cne $First.CacheKey) "SDK version did not invalidate cache key"
Write-Utf8File -Path $GlobalJsonPath -Text '{"sdk":{"version":"8.0.416","rollForward":"disable","allowPrerelease":false}}'

Write-Utf8File -Path $GlobalJsonPath -Text '{"sdk":{"version":"latest","rollForward":"disable","allowPrerelease":false}}'
Assert-CacheContextFailure `
    -Arguments $ContextArguments `
    -ExpectedCode "ASBI4501" `
    -Message "non-version SDK identity was accepted"
Write-Utf8File -Path $GlobalJsonPath -Text '{"sdk":{"version":"8.0.416","rollForward":"disable","allowPrerelease":false}}'

Write-Utf8File -Path (Join-Path $ToolchainRoot "Tools\AvidScript.CSharpFrontend\bin\Ignored.cs") -Text "changed ignored bin"
Write-Utf8File -Path (Join-Path $ToolchainRoot "Tools\AvidScript.CSharpSemantic\obj\Ignored.cs") -Text "changed ignored obj"
$IgnoredBuildOutput = Get-AvidScriptCSharpSemanticCacheContext @ContextArguments
Assert-Condition ($IgnoredBuildOutput.CacheKey -ceq $First.CacheKey) `
    "bin or obj content invalidated semantic cache key"

$MixedCaseToolPath = Join-Path $ToolchainRoot "Tools\AvidScript.CSharpFrontend\Injected.CS"
Write-Utf8File -Path $MixedCaseToolPath -Text "namespace Frontend; public static class Injected {}"
$MixedCaseToolchain = Get-AvidScriptCSharpSemanticCacheContext @ContextArguments
Assert-Condition ($MixedCaseToolchain.ToolchainFingerprint -cne $First.ToolchainFingerprint) `
    "mixed-case toolchain source extension did not invalidate toolchain fingerprint"
Assert-Condition ($MixedCaseToolchain.CacheKey -cne $First.CacheKey) `
    "mixed-case toolchain source extension did not invalidate cache key"
Remove-Item -LiteralPath $MixedCaseToolPath -Force

$EscapedToolchainRoot = Join-Path $RunRoot "EscapedToolchain"
$ExternalFrontendRoot = Join-Path $RunRoot "ExternalFrontend"
$LinkedFrontendRoot = Join-Path $EscapedToolchainRoot "Tools\AvidScript.CSharpFrontend"
Write-Utf8File -Path (Join-Path $EscapedToolchainRoot "global.json") -Text '{"sdk":{"version":"8.0.416","rollForward":"disable","allowPrerelease":false}}'
Write-Utf8File -Path (Join-Path $EscapedToolchainRoot "Build\InvokeCSharpFrontend.ps1") -Text 'Write-Output "frontend"'
Write-Utf8File -Path (Join-Path $EscapedToolchainRoot "Build\InvokeCSharpSemantic.ps1") -Text 'Write-Output "semantic"'
Write-Utf8File -Path (Join-Path $EscapedToolchainRoot "Tools\AvidScript.CSharpSemantic\Semantic.cs") -Text "namespace Semantic; public static class Analyzer {}"
Write-Utf8File -Path (Join-Path $EscapedToolchainRoot "Tools\AvidScript.CSharpSemantic\Semantic.csproj") -Text '<Project Sdk="Microsoft.NET.Sdk" />'
Write-Utf8File -Path (Join-Path $ExternalFrontendRoot "Frontend.cs") -Text "namespace External; public static class Escaped {}"
Write-Utf8File -Path (Join-Path $ExternalFrontendRoot "Frontend.csproj") -Text '<Project Sdk="Microsoft.NET.Sdk" />'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LinkedFrontendRoot) | Out-Null
New-Item -ItemType Junction -Path $LinkedFrontendRoot -Target $ExternalFrontendRoot | Out-Null
try {
    $EscapedToolchainArguments = $ContextArguments.Clone()
    $EscapedToolchainArguments.PluginRoot = $EscapedToolchainRoot
    Assert-CacheContextFailure `
        -Arguments $EscapedToolchainArguments `
        -ExpectedCode "ASBI4501" `
        -Message "toolchain source-root junction escape was accepted"
}
finally {
    if (Test-Path -LiteralPath $LinkedFrontendRoot) {
        [System.IO.Directory]::Delete($LinkedFrontendRoot)
    }
}

Write-Utf8File -Path $SemanticToolPath -Text "namespace Semantic; public static class Analyzer { public const int Version = 2; }"
$ChangedToolchain = Get-AvidScriptCSharpSemanticCacheContext @ContextArguments
Assert-Condition ($ChangedToolchain.ToolchainFingerprint -cne $First.ToolchainFingerprint) `
    "toolchain source did not invalidate toolchain fingerprint"
Assert-Condition ($ChangedToolchain.CacheKey -cne $First.CacheKey) "toolchain source did not invalidate cache key"

Write-Output "AvidScript.CSharpFrontend.SemanticCacheContracts: 16/16 passed"
exit 0
