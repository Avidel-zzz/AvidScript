param(
    [string]$DotNetPath = (Join-Path $env:USERPROFILE ".dotnet\dotnet.exe"),
    [string]$BindingPackagePath = ""
)

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$BuildScript = Join-Path $PluginRoot "Build\BuildCSharpActorLifecycle.ps1"
$ProjectPath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\AvidScript.ActorLifecycle.csproj"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\SemanticCacheBuildIntegration"
$CacheParent = Join-Path $ProjectRoot "Saved\AvidScript\SemanticCacheBuildIntegration"
$CacheRoot = Join-Path $CacheParent "v1"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Resolve-ProjectPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $Path))
}

function Write-LifecycleSource {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Scale
    )

    $Text = @"
using System.Runtime.InteropServices;

namespace AvidScript;

public static class SemanticCacheBuildScript
{
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        UE.Self.SetActorScale3D(new FVector($Scale, 1.0f, 1.0f));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds) {}

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
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Text, $Utf8)
}

function New-BindingPackageSubset {
    param(
        [Parameter(Mandatory = $true)][string]$AuthorizationManifestPath,
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [Parameter(Mandatory = $true)][string]$UeFunction
    )

    $Manifest = Get-Content -Raw -LiteralPath $AuthorizationManifestPath | ConvertFrom-Json
    $PackageDirectory = Split-Path -Parent $AuthorizationManifestPath
    $DescriptorSource = Join-Path $PackageDirectory ([string]$Manifest.files.descriptor)
    $ReferenceSource = Join-Path $PackageDirectory ([string]$Manifest.files.reference_source)
    $Descriptor = Get-Content -Raw -LiteralPath $DescriptorSource | ConvertFrom-Json
    $Binding = @($Descriptor.bindings | Where-Object ue_function -eq $UeFunction | Select-Object -First 1)
    Assert-Condition ($Binding.Count -eq 1) "authorization subset could not find $UeFunction"
    $SelectedImport = @($Manifest.required_imports | Where-Object stable_id -eq ([string]$Binding[0].stable_id))
    Assert-Condition ($SelectedImport.Count -eq 1) "authorization subset could not find the import for $UeFunction"

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    Copy-Item -LiteralPath $DescriptorSource -Destination (Join-Path $OutputDirectory ([string]$Manifest.files.descriptor)) -Force
    Copy-Item -LiteralPath $ReferenceSource -Destination (Join-Path $OutputDirectory ([string]$Manifest.files.reference_source)) -Force
    $Manifest.required_imports = @($SelectedImport)
    $SubsetManifestPath = Join-Path $OutputDirectory "package.json"
    [System.IO.File]::WriteAllText(
        $SubsetManifestPath,
        ($Manifest | ConvertTo-Json -Depth 32),
        $Utf8)
    return $SubsetManifestPath
}

function Invoke-CacheBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$AuthorizationPath,
        [string]$PreparedReportPath = "",
        [switch]$DisableSemanticCache
    )

    $OutputRoot = Join-Path $RunRoot $Name
    $ReportPath = Join-Path $OutputRoot "$Name.csharp.report.json"
    $ManifestPath = Join-Path $OutputRoot "$Name.avidscript.json"
    $Arguments = @{
        DotNetPath = $DotNetPath
        OutputRoot = $OutputRoot
        SourcePath = $SourcePath
        ProjectPath = $ProjectPath
        BindingPackagePath = $AuthorizationPath
        ModuleId = "p43_5_$($Name.ToLowerInvariant())"
        ArtifactStem = $Name
        ReportPath = $ReportPath
        ManifestPath = $ManifestPath
        SemanticCacheRoot = $CacheRoot
    }
    if (-not [string]::IsNullOrWhiteSpace($PreparedReportPath)) {
        $Arguments.PreparedBuildReportPath = $PreparedReportPath
    }
    if ($DisableSemanticCache) {
        $Arguments.DisableSemanticCache = $true
    }
    & $BuildScript @Arguments | Out-Null
    $ExitCode = $LASTEXITCODE
    $Report = if (Test-Path -LiteralPath $ReportPath -PathType Leaf) {
        Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json
    }
    else { $null }
    return [pscustomobject]@{
        ExitCode = $ExitCode
        OutputRoot = $OutputRoot
        ReportPath = $ReportPath
        ManifestPath = $ManifestPath
        Report = $Report
    }
}

function Assert-DebugArtifact {
    param(
        [Parameter(Mandatory = $true)]$Build,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $DebugMapPath = Resolve-ProjectPath ([string]$Build.Report.artifacts.debug_map_file)
    Assert-Condition (Test-Path -LiteralPath $DebugMapPath -PathType Leaf) "$Label C# debug map is missing"
    $DebugMap = Get-Content -Raw -LiteralPath $DebugMapPath | ConvertFrom-Json
    $Manifest = Get-Content -Raw -LiteralPath $Build.ManifestPath | ConvertFrom-Json
    Assert-Condition (
        [int]$DebugMap.schema_version -eq 1 -and
        [string]$DebugMap.debug_version -ceq "1.0" -and
        [string]$DebugMap.module_id -ceq [string]$Manifest.guest_ir.module_id) `
        "$Label C# debug map contract differs from Guest IR"
    Assert-Condition (
        [string]$DebugMap.source.id -ceq [string]$Build.Report.source.file -and
        [string]$DebugMap.provenance.semantic_sha256 -ceq [string]$Build.Report.guest_ir.semantic_sha256 -and
        [string]$DebugMap.provenance.guest_ir_sha256 -ceq [string]$Build.Report.guest_ir.sha256) `
        "$Label C# debug map provenance differs from report"
    Assert-Condition (
        [string]$Manifest.debug_map.file -ceq [string]$Build.Report.artifacts.debug_map_file -and
        [string]$Manifest.debug_map.sha256 -ceq [string]$Build.Report.debug_map.sha256) `
        "$Label manifest C# debug map binding differs from report"
    return $DebugMap
}

function Assert-SemanticContract {
    param(
        [Parameter(Mandatory = $true)]$Build,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $SemanticPath = Resolve-ProjectPath ([string]$Build.Report.artifacts.semantic_file)
    Assert-Condition (Test-Path -LiteralPath $SemanticPath -PathType Leaf) "$Label semantic artifact is missing"
    $Semantic = Get-Content -Raw -LiteralPath $SemanticPath | ConvertFrom-Json
    Assert-Condition (
        [int]$Build.Report.semantic.schema_version -eq 15 -and
        [string]$Build.Report.semantic.version -ceq "1.16" -and
        [int]$Semantic.schema_version -eq 15 -and
        [string]$Semantic.semantic_version -ceq "1.16") `
        "$Label semantic contract is not 15/1.16"
}

foreach ($Directory in @($RunRoot, $CacheParent)) {
    if (Test-Path -LiteralPath $Directory) {
        Remove-Item -LiteralPath $Directory -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null

if ([string]::IsNullOrWhiteSpace($BindingPackagePath)) {
    . (Join-Path $PluginRoot "Build\AvidScriptCSharpBindingPackage.ps1")
    $BindingPackagePath = Find-AvidScriptCSharpBindingPackageManifest `
        -RootPath (Join-Path $ProjectRoot "Saved\AvidScriptGeneratedBindings") `
        -RequiredUeFunctions @("SetActorScale3D")
}
Assert-Condition (
    -not [string]::IsNullOrWhiteSpace($BindingPackagePath) -and
    (Test-Path -LiteralPath $BindingPackagePath -PathType Leaf)) `
    "generated binding package is missing for semantic cache build integration"

$SourcePath = Join-Path $RunRoot "Source\SemanticCacheBuildScript.cs"
Write-LifecycleSource -Path $SourcePath -Scale "1.0f"
$SubsetPackagePath = New-BindingPackageSubset `
    -AuthorizationManifestPath $BindingPackagePath `
    -OutputDirectory (Join-Path $RunRoot "SubsetPackage") `
    -UeFunction "SetActorScale3D"

$Cold = Invoke-CacheBuild -Name "Cold" -SourcePath $SourcePath -AuthorizationPath $BindingPackagePath
Assert-Condition ($Cold.ExitCode -eq 0 -and $Cold.Report.succeeded) "cold semantic cache build failed"
Assert-Condition ($Cold.Report.semantic_cache.lookup -ceq "miss") "cold semantic cache build did not miss"
Assert-Condition (
    [int]$Cold.Report.semantic_cache.schema_version -eq 1 -and
    [string]$Cold.Report.semantic_cache.key -cmatch '^[0-9a-f]{64}$' -and
    [string]$Cold.Report.semantic_cache.toolchain_fingerprint -cmatch '^[0-9a-f]{64}$') `
    "cold semantic cache report is missing its producing identity"
Assert-Condition ($Cold.Report.semantic_cache.published) "cold semantic cache build did not publish"
Assert-Condition (
    [int]$Cold.Report.tool_invocations.frontend -eq 1 -and
    [int]$Cold.Report.tool_invocations.semantic -eq 1 -and
    [int]$Cold.Report.tool_invocations.guest_ir -eq 1 -and
    [int]$Cold.Report.tool_invocations.wasm_backend -eq 1) `
    "cold semantic cache invocation counts differ"
$ColdDebugMap = Assert-DebugArtifact -Build $Cold -Label "cold semantic cache build"
Assert-SemanticContract -Build $Cold -Label "cold semantic cache build"

$Warm = Invoke-CacheBuild -Name "Warm" -SourcePath $SourcePath -AuthorizationPath $BindingPackagePath
Assert-Condition ($Warm.ExitCode -eq 0 -and $Warm.Report.succeeded) "warm semantic cache build failed"
Assert-Condition ($Warm.Report.semantic_cache.lookup -ceq "hit") "warm semantic cache build did not hit"
Assert-Condition (-not $Warm.Report.semantic_cache.published) "warm semantic cache hit republished its entry"
Assert-Condition (
    [int]$Warm.Report.tool_invocations.frontend -eq 0 -and
    [int]$Warm.Report.tool_invocations.semantic -eq 0 -and
    [int]$Warm.Report.tool_invocations.guest_ir -eq 1 -and
    [int]$Warm.Report.tool_invocations.wasm_backend -eq 1) `
    "warm semantic cache invocation counts differ"
Assert-Condition ($Warm.Report.semantic_cache.key -ceq $Cold.Report.semantic_cache.key) "warm cache key differs from cold key"
Assert-Condition ($Warm.Report.semantic_cache.toolchain_fingerprint -ceq $Cold.Report.semantic_cache.toolchain_fingerprint) `
    "warm cache toolchain fingerprint differs from cold fingerprint"
$WarmDebugMap = Assert-DebugArtifact -Build $Warm -Label "warm semantic cache build"
Assert-SemanticContract -Build $Warm -Label "warm semantic cache build"
Assert-Condition (
    [string]$Warm.Report.debug_map.sha256 -ceq [string]$Cold.Report.debug_map.sha256 -and
    @($WarmDebugMap.functions).Count -eq @($ColdDebugMap.functions).Count) `
    "warm semantic cache build did not deterministically regenerate the C# debug map"
$WarmManifestText = Get-Content -Raw -LiteralPath $Warm.ManifestPath
Assert-Condition ($WarmManifestText.IndexOf("semantic_cache", [System.StringComparison]::Ordinal) -lt 0) `
    "runtime manifest leaked semantic cache metadata"
Assert-Condition ($WarmManifestText.IndexOf("tool_invocations", [System.StringComparison]::Ordinal) -lt 0) `
    "runtime manifest leaked tool invocation metadata"

$EntryReportPath = Resolve-ProjectPath ([string]$Warm.Report.semantic_cache.entry_report_file)
$EntryReport = Get-Content -Raw -LiteralPath $EntryReportPath | ConvertFrom-Json
$EntryFrontendPath = Resolve-ProjectPath ([string]$EntryReport.artifacts.frontend_file)
[System.IO.File]::WriteAllText($EntryFrontendPath, '{"tampered":true}', $Utf8)
$Recovered = Invoke-CacheBuild -Name "Recovered" -SourcePath $SourcePath -AuthorizationPath $BindingPackagePath
Assert-Condition ($Recovered.ExitCode -eq 0 -and $Recovered.Report.succeeded) "corrupt cache recovery build failed"
Assert-Condition ($Recovered.Report.semantic_cache.lookup -ceq "rejected") "corrupt cache entry was not rejected"
Assert-Condition ($Recovered.Report.semantic_cache.published) "corrupt cache entry was not rebuilt and published"
Assert-Condition ($Recovered.Report.semantic_cache.diagnostic_code -ceq "ASBI4502") `
    "corrupt cache recovery lost the stable rejection diagnostic"
Assert-Condition (@($Recovered.Report.diagnostics | Where-Object code -eq "ASBI4502").Count -eq 1) `
    "corrupt cache recovery did not retain a warning diagnostic"
Assert-Condition (
    [int]$Recovered.Report.tool_invocations.frontend -eq 1 -and
    [int]$Recovered.Report.tool_invocations.semantic -eq 1) `
    "corrupt cache recovery did not run the full Roslyn path"

Write-LifecycleSource -Path $SourcePath -Scale "2.0f"
$ChangedSource = Invoke-CacheBuild -Name "ChangedSource" -SourcePath $SourcePath -AuthorizationPath $BindingPackagePath
Assert-Condition ($ChangedSource.ExitCode -eq 0 -and $ChangedSource.Report.semantic_cache.lookup -ceq "miss") `
    "source change did not produce a semantic cache miss"
Assert-Condition ($ChangedSource.Report.semantic_cache.key -cne $Recovered.Report.semantic_cache.key) `
    "source change did not change the semantic cache key"

Write-LifecycleSource -Path $SourcePath -Scale "1.0f"
$ChangedAuthorization = Invoke-CacheBuild -Name "ChangedAuthorization" -SourcePath $SourcePath -AuthorizationPath $SubsetPackagePath
Assert-Condition ($ChangedAuthorization.ExitCode -eq 0 -and $ChangedAuthorization.Report.semantic_cache.lookup -ceq "miss") `
    "authorization change did not produce a semantic cache miss"
Assert-Condition ($ChangedAuthorization.Report.semantic_cache.key -cne $Recovered.Report.semantic_cache.key) `
    "authorization change did not change the semantic cache key"

$Prepared = Invoke-CacheBuild `
    -Name "Prepared" `
    -SourcePath $SourcePath `
    -AuthorizationPath $BindingPackagePath `
    -PreparedReportPath $Recovered.ReportPath
Assert-Condition ($Prepared.ExitCode -eq 0 -and $Prepared.Report.succeeded) "prepared final cache build failed"
Assert-Condition ($Prepared.Report.semantic_cache.lookup -ceq "disabled") "prepared final unexpectedly performed cache lookup"
Assert-Condition ([string]::IsNullOrWhiteSpace([string]$Prepared.Report.semantic_cache.key)) `
    "prepared final retained a semantic cache key"
Assert-Condition (
    [int]$Prepared.Report.tool_invocations.frontend -eq 0 -and
    [int]$Prepared.Report.tool_invocations.semantic -eq 0 -and
    [int]$Prepared.Report.tool_invocations.guest_ir -eq 1 -and
    [int]$Prepared.Report.tool_invocations.wasm_backend -eq 1) `
    "prepared final invocation counts differ"

$Disabled = Invoke-CacheBuild `
    -Name "Disabled" `
    -SourcePath $SourcePath `
    -AuthorizationPath $BindingPackagePath `
    -DisableSemanticCache
Assert-Condition ($Disabled.ExitCode -eq 0 -and $Disabled.Report.succeeded) `
    "explicitly disabled semantic cache build failed"
Assert-Condition (
    -not [bool]$Disabled.Report.semantic_cache.enabled -and
    [string]$Disabled.Report.semantic_cache.lookup -ceq "disabled" -and
    [string]::IsNullOrWhiteSpace([string]$Disabled.Report.semantic_cache.key) -and
    [string]::IsNullOrWhiteSpace([string]$Disabled.Report.semantic_cache.toolchain_fingerprint) -and
    -not [bool]$Disabled.Report.semantic_cache.published) `
    "explicitly disabled semantic cache report differs"
Assert-Condition (
    [int]$Disabled.Report.tool_invocations.frontend -eq 1 -and
    [int]$Disabled.Report.tool_invocations.semantic -eq 1 -and
    [int]$Disabled.Report.tool_invocations.guest_ir -eq 1 -and
    [int]$Disabled.Report.tool_invocations.wasm_backend -eq 1) `
    "explicitly disabled semantic cache invocation counts differ"

$BrokenSourcePath = Join-Path $RunRoot "Broken\BrokenSemanticCacheScript.cs"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $BrokenSourcePath) | Out-Null
[System.IO.File]::WriteAllText($BrokenSourcePath, "public static class Broken {", $Utf8)
$Broken = Invoke-CacheBuild -Name "Broken" -SourcePath $BrokenSourcePath -AuthorizationPath $BindingPackagePath
Assert-Condition ($Broken.ExitCode -eq 1 -and $Broken.Report.result -ceq "frontend_failed") `
    "frontend failure cache build did not fail at Frontend"
Assert-Condition (
    [int]$Broken.Report.tool_invocations.frontend -eq 1 -and
    [int]$Broken.Report.tool_invocations.semantic -eq 0 -and
    [int]$Broken.Report.tool_invocations.guest_ir -eq 0 -and
    [int]$Broken.Report.tool_invocations.wasm_backend -eq 0) `
    "frontend failure invocation counts differ"

Write-Output "AvidScript.CSharpFrontend.SemanticCacheBuildIntegration: 8/8 passed"
exit 0
