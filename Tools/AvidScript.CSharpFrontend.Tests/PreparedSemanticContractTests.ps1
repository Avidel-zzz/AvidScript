param(
    [string]$DotNetPath = "C:\Users\user0\.dotnet\dotnet.exe"
)

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$BuildScript = Join-Path $PluginRoot "Build\BuildCSharpActorLifecycle.ps1"
$HelperPath = Join-Path $PluginRoot "Build\AvidScriptCSharpPreparedSemantic.ps1"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\PreparedSemanticContracts"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)

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

function Resolve-ProjectArtifactPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $Path))
}

function Write-LifecycleSource {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$BeginPlayBody = "",
        [string]$TickBody = ""
    )

    $Text = @"
using System.Runtime.InteropServices;

namespace AvidScript;

public static class PreparedSemanticContractScript
{
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        $BeginPlayBody
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        $TickBody
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay() {}

    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
    public static void OnTimer(int callbackId, int timerHandle) {}

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int eventId, float value) {}

    [UnmanagedCallersOnly(EntryPoint = "avid_on_gameplay_event")]
    public static void OnGameplayEvent(
        int eventType, int primaryId, int secondaryId, int objectSlot,
        int objectGeneration, float x, float y, float z) {}
}
"@
    [System.IO.File]::WriteAllText($Path, $Text, $Utf8)
}

function New-MutatedReportPath {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)]$Mutation
    )

    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $ReportPath = Join-Path $Directory $FileName
    $Clone = $PreparedReportJson | ConvertTo-Json -Depth 64 | ConvertFrom-Json
    & $Mutation $Clone
    [System.IO.File]::WriteAllText(
        $ReportPath,
        ($Clone | ConvertTo-Json -Depth 64) + [System.Environment]::NewLine,
        $Utf8)
    return $ReportPath
}

function Assert-PreparedSemanticFailure {
    param(
        [Parameter(Mandatory = $true)][string]$PreparedReportPath,
        [Parameter(Mandatory = $true)][string]$ExpectedSourcePath,
        [Parameter(Mandatory = $true)][object]$ExpectedAuthorizationPackage,
        [Parameter(Mandatory = $true)][string]$ExpectedCode,
        [Parameter(Mandatory = $true)][string]$FrontendDestinationPath,
        [Parameter(Mandatory = $true)][string]$SemanticDestinationPath
    )

    foreach ($Destination in @($FrontendDestinationPath, $SemanticDestinationPath)) {
        if (Test-Path -LiteralPath $Destination -PathType Leaf) {
            Remove-Item -LiteralPath $Destination -Force
        }
    }

    $Failed = $false
    try {
        Import-AvidScriptCSharpPreparedSemantic `
            -PreparedReportPath $PreparedReportPath `
            -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $ExpectedSourcePath `
            -ExpectedAuthorizationPackage $ExpectedAuthorizationPackage `
            -FrontendDestinationPath $FrontendDestinationPath `
            -SemanticDestinationPath $SemanticDestinationPath | Out-Null
    }
    catch {
        $Failed = $true
        Assert-Condition (
            [string]$_.Exception.Data["AvidScriptCode"] -ceq $ExpectedCode) `
            "expected structured AvidScriptCode=$ExpectedCode but saw: $($_.Exception.Data["AvidScriptCode"])"
        Assert-Condition (
            $_.Exception.Message.IndexOf($ExpectedCode, [System.StringComparison]::Ordinal) -ge 0) `
            "expected $ExpectedCode but saw: $($_.Exception.Message)"
    }

    Assert-Condition $Failed "expected prepared semantic import to fail with $ExpectedCode"
    Assert-Condition (-not (Test-Path -LiteralPath $FrontendDestinationPath -PathType Leaf)) "failed prepared import left frontend output"
    Assert-Condition (-not (Test-Path -LiteralPath $SemanticDestinationPath -PathType Leaf)) "failed prepared import left semantic output"
}

New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null
$env:TEMP = Join-Path $RunRoot "Temp"
$env:TMP = $env:TEMP
$env:DOTNET_CLI_HOME = Join-Path $RunRoot "DotNetHome"
$env:APPDATA = Join-Path $RunRoot "AppData"
$env:LOCALAPPDATA = Join-Path $RunRoot "LocalAppData"
foreach ($Directory in @($env:TEMP, $env:DOTNET_CLI_HOME, $env:APPDATA, $env:LOCALAPPDATA)) {
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
}

$BindingPackagePath = Get-ChildItem `
    -LiteralPath (Join-Path $ProjectRoot "Saved\AvidScriptGeneratedBindings") `
    -Filter "package.json" `
    -File `
    -Recurse `
    -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1 -ExpandProperty FullName
Assert-Condition (
    -not [string]::IsNullOrWhiteSpace($BindingPackagePath) -and
    (Test-Path -LiteralPath $BindingPackagePath -PathType Leaf)) `
    "generated binding package is missing; publish the default gameplay package before this contract suite"

$FixtureRoot = Join-Path $RunRoot "Fixture"
$SourcePath = Join-Path $FixtureRoot "PreparedSemanticContractScript.cs"
$ReportPath = Join-Path $FixtureRoot "prepared_semantic_contract.csharp.report.json"
$ManifestPath = Join-Path $FixtureRoot "prepared_semantic_contract.avidscript.json"
New-Item -ItemType Directory -Force -Path $FixtureRoot | Out-Null
Write-LifecycleSource `
    -Path $SourcePath `
    -BeginPlayBody "UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $FixtureRoot `
    -SourcePath $SourcePath `
    -BindingPackagePath $BindingPackagePath `
    -ModuleId "p43_prepared_semantic_contract" `
    -ArtifactStem "prepared_semantic_contract" `
    -ReportPath $ReportPath `
    -ManifestPath $ManifestPath | Out-Null
Assert-Condition ($LASTEXITCODE -eq 0) "prepared semantic fixture build failed"
Assert-Condition (Test-Path -LiteralPath $ReportPath -PathType Leaf) "prepared semantic fixture report is missing"

if (-not (Test-Path -LiteralPath $HelperPath -PathType Leaf)) {
    throw "Prepared semantic helper is missing: $HelperPath"
}

. $HelperPath
. (Join-Path $PluginRoot "Build\AvidScriptCSharpBindingPackage.ps1")

$PreparedReportJson = Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json
$AuthorizationPackage = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $BindingPackagePath
$PreparedFrontendPath = Resolve-ProjectArtifactPath ([string]$PreparedReportJson.artifacts.frontend_file)
$PreparedSemanticPath = Resolve-ProjectArtifactPath ([string]$PreparedReportJson.artifacts.semantic_file)
Assert-Condition (Test-Path -LiteralPath $PreparedFrontendPath -PathType Leaf) "prepared frontend artifact is missing"
Assert-Condition (Test-Path -LiteralPath $PreparedSemanticPath -PathType Leaf) "prepared semantic artifact is missing"

$ValidRoot = Join-Path $RunRoot "Valid"
$ValidFrontendPath = Join-Path $ValidRoot "prepared_semantic_contract.csharp.frontend.json"
$ValidSemanticPath = Join-Path $ValidRoot "prepared_semantic_contract.csharp.semantic.json"
$Imported = Import-AvidScriptCSharpPreparedSemantic `
    -PreparedReportPath $ReportPath `
    -ProjectRoot $ProjectRoot `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -FrontendDestinationPath $ValidFrontendPath `
    -SemanticDestinationPath $ValidSemanticPath
Assert-Condition ($null -ne $Imported.FrontendModel) "prepared semantic import did not return a frontend model"
Assert-Condition ($null -ne $Imported.SemanticModel) "prepared semantic import did not return a semantic model"
Assert-Condition ([System.IO.Path]::GetFullPath($Imported.PreparedReportPath) -eq [System.IO.Path]::GetFullPath($ReportPath)) "prepared report path was not normalized"
Assert-Condition ($Imported.PreparedReportSha256 -eq (Get-Sha256Hex $ReportPath)) "prepared report sha256 is incorrect"
Assert-Condition ((Get-Sha256Hex $ValidFrontendPath) -eq (Get-Sha256Hex $PreparedFrontendPath)) "frontend artifact bytes changed during prepared import"
Assert-Condition ((Get-Sha256Hex $ValidSemanticPath) -eq (Get-Sha256Hex $PreparedSemanticPath)) "semantic artifact bytes changed during prepared import"

$MismatchedSourcePath = Join-Path $RunRoot "SourceMismatch\PreparedSemanticContractScript.cs"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $MismatchedSourcePath) | Out-Null
Write-LifecycleSource `
    -Path $MismatchedSourcePath `
    -BeginPlayBody "UE.Self.SetActorScale3D(new FVector(2.0f, 1.0f, 1.0f));"
Assert-PreparedSemanticFailure `
    -PreparedReportPath $ReportPath `
    -ExpectedSourcePath $MismatchedSourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -ExpectedCode "ASBI4401" `
    -FrontendDestinationPath (Join-Path $RunRoot "SourceMismatch\prepared_semantic_contract.csharp.frontend.json") `
    -SemanticDestinationPath (Join-Path $RunRoot "SourceMismatch\prepared_semantic_contract.csharp.semantic.json")

$AuthorizationMismatch = $AuthorizationPackage | ConvertTo-Json -Depth 32 | ConvertFrom-Json
$AuthorizationMismatch.PackageHash = ("0" * 64)
Assert-PreparedSemanticFailure `
    -PreparedReportPath $ReportPath `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationMismatch `
    -ExpectedCode "ASBI4402" `
    -FrontendDestinationPath (Join-Path $RunRoot "AuthorizationMismatch\prepared_semantic_contract.csharp.frontend.json") `
    -SemanticDestinationPath (Join-Path $RunRoot "AuthorizationMismatch\prepared_semantic_contract.csharp.semantic.json")

$ArtifactMismatchReportPath = New-MutatedReportPath `
    -Directory (Join-Path $RunRoot "ArtifactMismatch") `
    -FileName "prepared_semantic_contract.csharp.report.json" `
    -Mutation {
        param($Report)
        $Report.frontend.artifact_sha256 = ("f" * 64)
    }
Assert-PreparedSemanticFailure `
    -PreparedReportPath $ArtifactMismatchReportPath `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -ExpectedCode "ASBI4403" `
    -FrontendDestinationPath (Join-Path $RunRoot "ArtifactMismatch\prepared_semantic_contract.csharp.frontend.json") `
    -SemanticDestinationPath (Join-Path $RunRoot "ArtifactMismatch\prepared_semantic_contract.csharp.semantic.json")

$EscapedArtifactPath = Join-Path $RunRoot "EscapedArtifacts\escaped.csharp.frontend.json"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $EscapedArtifactPath) | Out-Null
Copy-Item -LiteralPath $PreparedFrontendPath -Destination $EscapedArtifactPath -Force
$EscapedReportPath = New-MutatedReportPath `
    -Directory (Join-Path $RunRoot "PathEscape") `
    -FileName "prepared_semantic_contract.csharp.report.json" `
    -Mutation {
        param($Report)
        $Report.artifacts.frontend_file = Resolve-Path -LiteralPath $EscapedArtifactPath | Select-Object -ExpandProperty Path
        $Report.frontend.artifact_sha256 = Get-Sha256Hex $EscapedArtifactPath
    }
Assert-PreparedSemanticFailure `
    -PreparedReportPath $EscapedReportPath `
    -ExpectedSourcePath $SourcePath `
    -ExpectedAuthorizationPackage $AuthorizationPackage `
    -ExpectedCode "ASBI4404" `
    -FrontendDestinationPath (Join-Path $RunRoot "PathEscape\prepared_semantic_contract.csharp.frontend.json") `
    -SemanticDestinationPath (Join-Path $RunRoot "PathEscape\prepared_semantic_contract.csharp.semantic.json")

Assert-Condition (
    $null -ne (Get-Command "Publish-AvidScriptBindingFilePairAtomic" -ErrorAction SilentlyContinue)) `
    "transactional binding file pair publisher is missing"
$PairRollbackRoot = Join-Path $RunRoot "PairRollback"
$PairFrontendSource = Join-Path $PairRollbackRoot "new.frontend.json"
$PairSemanticSource = Join-Path $PairRollbackRoot "new.semantic.json"
$PairFrontendDestination = Join-Path $PairRollbackRoot "current.frontend.json"
$PairSemanticDestination = Join-Path $PairRollbackRoot "current.semantic.json"
New-Item -ItemType Directory -Force -Path $PairRollbackRoot | Out-Null
Copy-Item -LiteralPath $PreparedFrontendPath -Destination $PairFrontendSource -Force
Copy-Item -LiteralPath $PreparedSemanticPath -Destination $PairSemanticSource -Force
[System.IO.File]::WriteAllText($PairFrontendDestination, "old-frontend", $Utf8)
[System.IO.File]::WriteAllText($PairSemanticDestination, "old-semantic", $Utf8)
$SemanticLock = [System.IO.File]::Open(
    $PairSemanticDestination,
    [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Read,
    [System.IO.FileShare]::None)
$PairPublishFailed = $false
try {
    try {
        Publish-AvidScriptBindingFilePairAtomic `
            -FirstSourcePath $PairFrontendSource `
            -FirstDestinationPath $PairFrontendDestination `
            -SecondSourcePath $PairSemanticSource `
            -SecondDestinationPath $PairSemanticDestination
    }
    catch {
        $PairPublishFailed = $true
    }
}
finally {
    $SemanticLock.Dispose()
}
Assert-Condition $PairPublishFailed "locked second destination did not fail transactional pair publication"
Assert-Condition ((Get-Content -Raw -LiteralPath $PairFrontendDestination) -ceq "old-frontend") "pair rollback did not restore the old frontend"
Assert-Condition ((Get-Content -Raw -LiteralPath $PairSemanticDestination) -ceq "old-semantic") "pair rollback did not preserve the old semantic"
Assert-Condition (@(Get-ChildItem -LiteralPath $PairRollbackRoot -File | Where-Object Name -Match '\.(tmp|bak)\.').Count -eq 0) "pair rollback left temporary or backup files"

$PublishRollbackRoot = Join-Path $RunRoot "PublishedPairRollback"
$PublishFrontendSource = Join-Path $PublishRollbackRoot "new.frontend.json"
$PublishSemanticSource = Join-Path $PublishRollbackRoot "new.semantic.json"
$PublishFrontendDestination = Join-Path $PublishRollbackRoot "current.frontend.json"
$PublishSemanticDestination = Join-Path $PublishRollbackRoot "current.semantic.json"
New-Item -ItemType Directory -Force -Path $PublishRollbackRoot | Out-Null
Copy-Item -LiteralPath $PreparedFrontendPath -Destination $PublishFrontendSource -Force
Copy-Item -LiteralPath $PreparedSemanticPath -Destination $PublishSemanticSource -Force
[System.IO.File]::WriteAllText($PublishFrontendDestination, "old-published-frontend", $Utf8)
[System.IO.File]::WriteAllText($PublishSemanticDestination, "old-published-semantic", $Utf8)
$script:PairFaultSecondDestination = [System.IO.Path]::GetFullPath($PublishSemanticDestination)
$script:PairFaultFirstDestination = [System.IO.Path]::GetFullPath($PublishFrontendDestination)
$script:PairFaultObservedFirstPublish = $false
function Move-Item {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$LiteralPath,
        [Parameter(Mandatory = $true)][string]$Destination,
        [switch]$Force
    )

    $SourceFullPath = [System.IO.Path]::GetFullPath($LiteralPath)
    $DestinationFullPath = [System.IO.Path]::GetFullPath($Destination)
    if ($SourceFullPath.EndsWith(".tmp", [System.StringComparison]::OrdinalIgnoreCase)) {
        if ($DestinationFullPath.Equals($script:PairFaultFirstDestination, [System.StringComparison]::OrdinalIgnoreCase)) {
            $script:PairFaultObservedFirstPublish = $true
        }
        if ($DestinationFullPath.Equals($script:PairFaultSecondDestination, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Injected second pair publication failure."
        }
    }
    Microsoft.PowerShell.Management\Move-Item @PSBoundParameters
}
$PublishedPairFailed = $false
try {
    try {
        Publish-AvidScriptBindingFilePairAtomic `
            -FirstSourcePath $PublishFrontendSource `
            -FirstDestinationPath $PublishFrontendDestination `
            -SecondSourcePath $PublishSemanticSource `
            -SecondDestinationPath $PublishSemanticDestination
    }
    catch {
        $PublishedPairFailed = $true
    }
}
finally {
    Remove-Item -LiteralPath "Function:\Move-Item" -Force
}
Assert-Condition $PublishedPairFailed "injected second publication failure did not fail the pair transaction"
Assert-Condition $script:PairFaultObservedFirstPublish "fault injection fired before the first new file was published"
Assert-Condition ((Get-Content -Raw -LiteralPath $PublishFrontendDestination) -ceq "old-published-frontend") "published-pair rollback did not restore the old frontend"
Assert-Condition ((Get-Content -Raw -LiteralPath $PublishSemanticDestination) -ceq "old-published-semantic") "published-pair rollback did not restore the old semantic"
Assert-Condition (@(Get-ChildItem -LiteralPath $PublishRollbackRoot -File | Where-Object Name -Match '\.(tmp|bak)\.').Count -eq 0) "published-pair rollback left temporary or backup files"

Write-Output "AvidScript.CSharpFrontend.PreparedSemanticContracts: 7/7 passed"
exit 0
