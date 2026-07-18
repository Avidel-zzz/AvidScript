param(
    [string]$DotNetPath = "C:\Users\user0\.dotnet\dotnet.exe",
    [string]$BindingPackagePath = ""
)

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$BuildScript = Join-Path $PluginRoot "Build\BuildCSharpActorLifecycle.ps1"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\BuildIntegration"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Resolve-ArtifactPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $Path))
}

function Get-Sha256Hex {
    param([string]$Path)
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

function New-BindingPackageSubset {
    param(
        [Parameter(Mandatory = $true)][string]$AuthorizationManifestPath,
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [Parameter(Mandatory = $true)][string[]]$UeFunctions
    )

    $AuthorizationManifest = Get-Content -Raw -LiteralPath $AuthorizationManifestPath | ConvertFrom-Json
    $AuthorizationDirectory = Split-Path -Parent $AuthorizationManifestPath
    $DescriptorSource = Join-Path $AuthorizationDirectory ([string]$AuthorizationManifest.files.descriptor)
    $ReferenceSource = Join-Path $AuthorizationDirectory ([string]$AuthorizationManifest.files.reference_source)
    $Descriptor = Get-Content -Raw -LiteralPath $DescriptorSource | ConvertFrom-Json
    $SelectedBindings = @($Descriptor.bindings | Where-Object { $UeFunctions -ccontains [string]$_.ue_function })
    Assert-Condition ($SelectedBindings.Count -eq $UeFunctions.Count) "binding subset fixture could not resolve every UE function"

    $SelectedStableIds = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Binding in $SelectedBindings) {
        [void]$SelectedStableIds.Add([string]$Binding.stable_id)
    }
    $SelectedImports = @($AuthorizationManifest.required_imports | Where-Object {
        $SelectedStableIds.Contains([string]$_.stable_id)
    })
    Assert-Condition ($SelectedImports.Count -eq $UeFunctions.Count) "binding subset fixture could not resolve every package import"

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    Copy-Item -LiteralPath $DescriptorSource -Destination (Join-Path $OutputDirectory ([string]$AuthorizationManifest.files.descriptor)) -Force
    Copy-Item -LiteralPath $ReferenceSource -Destination (Join-Path $OutputDirectory ([string]$AuthorizationManifest.files.reference_source)) -Force
    $AuthorizationManifest.required_imports = @($SelectedImports)
    $SubsetManifestPath = Join-Path $OutputDirectory "package.json"
    [System.IO.File]::WriteAllText(
        $SubsetManifestPath,
        ($AuthorizationManifest | ConvertTo-Json -Depth 32),
        $Utf8)
    return $SubsetManifestPath
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

public static class RuntimePackageContractScript
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

New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null

if ([string]::IsNullOrWhiteSpace($BindingPackagePath)) {
    $BindingPackagePath = Get-ChildItem `
        -LiteralPath (Join-Path $ProjectRoot "Saved\AvidScriptGeneratedBindings") `
        -Filter "package.json" `
        -File `
        -Recurse `
        -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
Assert-Condition (
    -not [string]::IsNullOrWhiteSpace($BindingPackagePath) -and
    (Test-Path -LiteralPath $BindingPackagePath -PathType Leaf)) `
    "generated binding package is missing; publish the default Phase 42 package before this integration test"

$RuntimeSetPackagePath = New-BindingPackageSubset `
    -AuthorizationManifestPath $BindingPackagePath `
    -OutputDirectory (Join-Path $RunRoot "RuntimeSetPackage") `
    -UeFunctions @("SetActorScale3D")

$RuntimeAllowedRoot = Join-Path $RunRoot "RuntimeAllowed"
New-Item -ItemType Directory -Force -Path $RuntimeAllowedRoot | Out-Null
$RuntimeAllowedSource = Join-Path $RuntimeAllowedRoot "RuntimeAllowedScript.cs"
$RuntimeAllowedReport = Join-Path $RuntimeAllowedRoot "runtime_allowed.csharp.report.json"
$RuntimeAllowedManifest = Join-Path $RuntimeAllowedRoot "runtime_allowed.avidscript.json"
Write-LifecycleSource `
    -Path $RuntimeAllowedSource `
    -BeginPlayBody "UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $RuntimeAllowedRoot `
    -SourcePath $RuntimeAllowedSource `
    -BindingPackagePath $BindingPackagePath `
    -RuntimeBindingPackagePath $RuntimeSetPackagePath `
    -ModuleId "p43_runtime_allowed" `
    -ArtifactStem "runtime_allowed" `
    -ReportPath $RuntimeAllowedReport `
    -ManifestPath $RuntimeAllowedManifest | Out-Null
$RuntimeAllowedExit = $LASTEXITCODE
Assert-Condition ($RuntimeAllowedExit -eq 0) "runtime subset build failed; actual=$RuntimeAllowedExit"
$RuntimeAllowedJson = Get-Content -Raw -LiteralPath $RuntimeAllowedReport | ConvertFrom-Json
Assert-Condition ($RuntimeAllowedJson.binding_authorization.profile_import_count -gt 1) "authorization package was not preserved"
Assert-Condition ($RuntimeAllowedJson.binding_authorization.used_import_count -eq 1) "authorization usage provenance is not one import"
Assert-Condition ($RuntimeAllowedJson.binding_package.profile_import_count -eq 1) "runtime package is not the one-import subset"
Assert-Condition ($RuntimeAllowedJson.binding_package.used_import_count -eq 1) "runtime package usage provenance is not one import"
Assert-Condition ($RuntimeAllowedJson.binding_authorization.manifest_file -ne $RuntimeAllowedJson.binding_package.manifest_file) "authorization and runtime package paths were collapsed"
$RuntimeAllowedManifestJson = Get-Content -Raw -LiteralPath $RuntimeAllowedManifest | ConvertFrom-Json
Assert-Condition ($RuntimeAllowedManifestJson.binding_package.profile_import_count -eq 1) "final manifest did not publish the runtime package subset"

$PreparedBootstrapRoot = Join-Path $RunRoot "PreparedBootstrap"
$PreparedBootstrapReport = Join-Path $PreparedBootstrapRoot "prepared_bootstrap.csharp.report.json"
$PreparedBootstrapManifest = Join-Path $PreparedBootstrapRoot "prepared_bootstrap.avidscript.json"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $PreparedBootstrapRoot `
    -SourcePath $RuntimeAllowedSource `
    -BindingPackagePath $BindingPackagePath `
    -ModuleId "p44_prepared_bootstrap" `
    -ArtifactStem "prepared_bootstrap" `
    -ReportPath $PreparedBootstrapReport `
    -ManifestPath $PreparedBootstrapManifest | Out-Null
$PreparedBootstrapExit = $LASTEXITCODE
Assert-Condition ($PreparedBootstrapExit -eq 0) "prepared bootstrap build failed; actual=$PreparedBootstrapExit"

$PreparedFinalRoot = Join-Path $RunRoot "PreparedFinal"
$PreparedFinalReport = Join-Path $PreparedFinalRoot "prepared_final.csharp.report.json"
$PreparedFinalManifest = Join-Path $PreparedFinalRoot "prepared_final.avidscript.json"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $PreparedFinalRoot `
    -SourcePath $RuntimeAllowedSource `
    -BindingPackagePath $BindingPackagePath `
    -RuntimeBindingPackagePath $RuntimeSetPackagePath `
    -PreparedBuildReportPath $PreparedBootstrapReport `
    -ModuleId "p44_prepared_final" `
    -ArtifactStem "prepared_final" `
    -ReportPath $PreparedFinalReport `
    -ManifestPath $PreparedFinalManifest | Out-Null
$PreparedFinalExit = $LASTEXITCODE
Assert-Condition ($PreparedFinalExit -eq 0) "prepared final build failed; actual=$PreparedFinalExit"
$PreparedFinalJson = Get-Content -Raw -LiteralPath $PreparedFinalReport | ConvertFrom-Json
Assert-Condition ($PreparedFinalJson.build_reuse.frontend_reused) "prepared final report did not mark frontend reuse"
Assert-Condition ($PreparedFinalJson.build_reuse.semantic_reused) "prepared final report did not mark semantic reuse"
Assert-Condition ((Resolve-ArtifactPath $PreparedFinalJson.build_reuse.prepared_report_file) -eq [System.IO.Path]::GetFullPath($PreparedBootstrapReport)) "prepared final report path provenance differs"
Assert-Condition ($PreparedFinalJson.build_reuse.prepared_report_sha256 -eq (Get-Sha256Hex $PreparedBootstrapReport)) "prepared final report hash provenance differs"
$PreparedBootstrapJson = Get-Content -Raw -LiteralPath $PreparedBootstrapReport | ConvertFrom-Json
$PreparedBootstrapFrontend = Resolve-ArtifactPath $PreparedBootstrapJson.artifacts.frontend_file
$PreparedBootstrapSemantic = Resolve-ArtifactPath $PreparedBootstrapJson.artifacts.semantic_file
$PreparedFinalFrontend = Resolve-ArtifactPath $PreparedFinalJson.artifacts.frontend_file
$PreparedFinalSemantic = Resolve-ArtifactPath $PreparedFinalJson.artifacts.semantic_file
Assert-Condition ($PreparedFinalFrontend.StartsWith([System.IO.Path]::GetFullPath($PreparedFinalRoot), [System.StringComparison]::OrdinalIgnoreCase)) "prepared frontend was not published under final output"
Assert-Condition ($PreparedFinalSemantic.StartsWith([System.IO.Path]::GetFullPath($PreparedFinalRoot), [System.StringComparison]::OrdinalIgnoreCase)) "prepared semantic was not published under final output"
Assert-Condition ((Get-Sha256Hex $PreparedFinalFrontend) -eq (Get-Sha256Hex $PreparedBootstrapFrontend)) "prepared frontend bytes differ in final output"
Assert-Condition ((Get-Sha256Hex $PreparedFinalSemantic) -eq (Get-Sha256Hex $PreparedBootstrapSemantic)) "prepared semantic bytes differ in final output"
Assert-Condition (Test-Path -LiteralPath (Resolve-ArtifactPath $PreparedFinalJson.artifacts.guest_ir_file) -PathType Leaf) "prepared final did not regenerate Guest IR"
Assert-Condition (Test-Path -LiteralPath (Resolve-ArtifactPath $PreparedFinalJson.artifacts.wasm_file) -PathType Leaf) "prepared final did not regenerate WASM"
Assert-Condition ($PreparedFinalJson.binding_authorization.profile_import_count -gt 1) "prepared final lost complete authorization"
Assert-Condition ($PreparedFinalJson.binding_package.profile_import_count -eq 1) "prepared final did not retain minimal runtime package"
$PreparedFinalManifestRaw = Get-Content -Raw -LiteralPath $PreparedFinalManifest
Assert-Condition ($PreparedFinalManifestRaw.IndexOf("PreparedBootstrap", [System.StringComparison]::OrdinalIgnoreCase) -lt 0) "final manifest retained a bootstrap path"

$LegacyPackageRoot = Join-Path $RunRoot "LegacyPackage"
New-Item -ItemType Directory -Force -Path $LegacyPackageRoot | Out-Null
$LegacyPackageReport = Join-Path $LegacyPackageRoot "legacy_package.csharp.report.json"
$LegacyPackageManifest = Join-Path $LegacyPackageRoot "legacy_package.avidscript.json"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $LegacyPackageRoot `
    -SourcePath $RuntimeAllowedSource `
    -BindingPackagePath $BindingPackagePath `
    -ModuleId "p43_legacy_package" `
    -ArtifactStem "legacy_package" `
    -ReportPath $LegacyPackageReport `
    -ManifestPath $LegacyPackageManifest | Out-Null
$LegacyPackageExit = $LASTEXITCODE
Assert-Condition ($LegacyPackageExit -eq 0) "legacy single-package build failed; actual=$LegacyPackageExit"
$LegacyPackageJson = Get-Content -Raw -LiteralPath $LegacyPackageReport | ConvertFrom-Json
Assert-Condition (
    $LegacyPackageJson.binding_authorization.manifest_file -eq $LegacyPackageJson.binding_package.manifest_file) `
    "legacy single-package build did not default runtime to authorization"
Assert-Condition (
    $LegacyPackageJson.binding_authorization.package_hash -eq $LegacyPackageJson.binding_package.package_hash) `
    "legacy single-package build changed runtime package identity"
Assert-Condition ($LegacyPackageJson.binding_package.used_import_count -eq 1) "legacy package did not retain used-import provenance"
Assert-Condition (Test-Path -LiteralPath $LegacyPackageManifest -PathType Leaf) "legacy package build did not publish a manifest"

$RuntimeMismatchRoot = Join-Path $RunRoot "RuntimeMismatch"
New-Item -ItemType Directory -Force -Path $RuntimeMismatchRoot | Out-Null
$RuntimeMismatchSource = Join-Path $RuntimeMismatchRoot "RuntimeMismatchScript.cs"
$RuntimeMismatchReport = Join-Path $RuntimeMismatchRoot "runtime_mismatch.csharp.report.json"
$RuntimeMismatchManifest = Join-Path $RuntimeMismatchRoot "runtime_mismatch.avidscript.json"
Write-LifecycleSource `
    -Path $RuntimeMismatchSource `
    -TickBody "FVector scale = UE.Self.GetActorScale3D();`n        UE.Self.SetActorScale3D(scale);"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $RuntimeMismatchRoot `
    -SourcePath $RuntimeMismatchSource `
    -BindingPackagePath $BindingPackagePath `
    -RuntimeBindingPackagePath $RuntimeSetPackagePath `
    -ModuleId "p43_runtime_mismatch" `
    -ArtifactStem "runtime_mismatch" `
    -ReportPath $RuntimeMismatchReport `
    -ManifestPath $RuntimeMismatchManifest | Out-Null
$RuntimeMismatchExit = $LASTEXITCODE
Assert-Condition ($RuntimeMismatchExit -eq 1) "runtime package mismatch must return exit 1; actual=$RuntimeMismatchExit"
$RuntimeMismatchJson = Get-Content -Raw -LiteralPath $RuntimeMismatchReport | ConvertFrom-Json
Assert-Condition ($RuntimeMismatchJson.result -eq "binding_runtime_import_mismatch") "runtime package mismatch has the wrong result"
Assert-Condition (@($RuntimeMismatchJson.diagnostics | Where-Object code -eq "ASBI4303").Count -eq 1) "runtime package mismatch diagnostic is missing"
Assert-Condition (-not (Test-Path -LiteralPath $RuntimeMismatchManifest -PathType Leaf)) "runtime package mismatch left a manifest"
Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $RuntimeMismatchRoot "runtime_mismatch.wasm") -PathType Leaf)) "runtime package mismatch left loadable WASM"

$OmitRuntimeRoot = Join-Path $RunRoot "OmitRuntime"
New-Item -ItemType Directory -Force -Path $OmitRuntimeRoot | Out-Null
$OmitRuntimeSource = Join-Path $OmitRuntimeRoot "OmitRuntimeScript.cs"
$OmitRuntimeReport = Join-Path $OmitRuntimeRoot "omit_runtime.csharp.report.json"
$OmitRuntimeManifest = Join-Path $OmitRuntimeRoot "omit_runtime.avidscript.json"
Write-LifecycleSource -Path $OmitRuntimeSource
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $OmitRuntimeRoot `
    -SourcePath $OmitRuntimeSource `
    -BindingPackagePath $BindingPackagePath `
    -OmitRuntimeBindingPackage `
    -ModuleId "p43_omit_runtime" `
    -ArtifactStem "omit_runtime" `
    -ReportPath $OmitRuntimeReport `
    -ManifestPath $OmitRuntimeManifest | Out-Null
$OmitRuntimeExit = $LASTEXITCODE
Assert-Condition ($OmitRuntimeExit -eq 0) "zero-binding build could not omit runtime package; actual=$OmitRuntimeExit"
$OmitRuntimeJson = Get-Content -Raw -LiteralPath $OmitRuntimeReport | ConvertFrom-Json
Assert-Condition ($null -ne $OmitRuntimeJson.binding_authorization) "zero-binding build lost authorization provenance"
Assert-Condition ($null -eq $OmitRuntimeJson.binding_package) "zero-binding report retained a runtime package"
$OmitRuntimeManifestJson = Get-Content -Raw -LiteralPath $OmitRuntimeManifest | ConvertFrom-Json
Assert-Condition ($OmitRuntimeManifestJson.PSObject.Properties.Name -notcontains "binding_package") "zero-binding manifest retained binding_package"

$OmitDynamicRoot = Join-Path $RunRoot "OmitDynamic"
New-Item -ItemType Directory -Force -Path $OmitDynamicRoot | Out-Null
$OmitDynamicReport = Join-Path $OmitDynamicRoot "omit_dynamic.csharp.report.json"
$OmitDynamicManifest = Join-Path $OmitDynamicRoot "omit_dynamic.avidscript.json"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $OmitDynamicRoot `
    -SourcePath $RuntimeAllowedSource `
    -BindingPackagePath $BindingPackagePath `
    -OmitRuntimeBindingPackage `
    -ModuleId "p43_omit_dynamic" `
    -ArtifactStem "omit_dynamic" `
    -ReportPath $OmitDynamicReport `
    -ManifestPath $OmitDynamicManifest | Out-Null
$OmitDynamicExit = $LASTEXITCODE
Assert-Condition ($OmitDynamicExit -eq 1) "dynamic binding build must reject omitted runtime package; actual=$OmitDynamicExit"
$OmitDynamicJson = Get-Content -Raw -LiteralPath $OmitDynamicReport | ConvertFrom-Json
Assert-Condition ($OmitDynamicJson.result -eq "binding_runtime_import_mismatch") "omitted dynamic package has the wrong result"
Assert-Condition (-not (Test-Path -LiteralPath $OmitDynamicManifest -PathType Leaf)) "omitted dynamic package left a manifest"

$BrokenRoot = Join-Path $RunRoot "Broken"
New-Item -ItemType Directory -Force -Path $BrokenRoot | Out-Null
$BrokenSource = Join-Path $BrokenRoot "BrokenScript.cs"
$BrokenReport = Join-Path $BrokenRoot "broken.csharp.report.json"
$BrokenManifest = Join-Path $BrokenRoot "broken.avidscript.json"
$BrokenText = @'
public static class BrokenScript
{
    public static void BeginPlay()
    {
        Actor.SetLocation(1.0f, 2.0f, 3.0f);
    }

    public static void Tick(float deltaSeconds)
    {
        Actor.SetLocation(deltaSeconds, 0.0f, 0.0f);
    }
'@
[System.IO.File]::WriteAllText($BrokenSource, $BrokenText, $Utf8)

& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $BrokenRoot `
    -SourcePath $BrokenSource `
    -BindingPackagePath $BindingPackagePath `
    -ModuleId "p39_broken" `
    -ArtifactStem "broken" `
    -ReportPath $BrokenReport `
    -ManifestPath $BrokenManifest | Out-Null
$BrokenExit = $LASTEXITCODE
Assert-Condition ($BrokenExit -eq 1) "syntax errors must return exit code 1; actual=$BrokenExit"
Assert-Condition (Test-Path -LiteralPath $BrokenReport -PathType Leaf) "syntax failure report is missing"
$BrokenJson = Get-Content -Raw -LiteralPath $BrokenReport | ConvertFrom-Json
Assert-Condition ($BrokenJson.result -eq "frontend_failed") "syntax failure report result is not frontend_failed"
Assert-Condition (@($BrokenJson.diagnostics | Where-Object severity -eq "error").Count -gt 0) "syntax failure report has no error diagnostic"
Assert-Condition (-not (Test-Path -LiteralPath $BrokenManifest -PathType Leaf)) "syntax failure must not produce a manifest"
Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $BrokenRoot "broken.csharp_adapter.wasm") -PathType Leaf)) "syntax failure must remove stale adapter WASM"
$BrokenFrontendPath = Resolve-ArtifactPath $BrokenJson.artifacts.frontend_file
Assert-Condition (Test-Path -LiteralPath $BrokenFrontendPath -PathType Leaf) "syntax failure frontend artifact is missing"

$SemanticBrokenRoot = Join-Path $RunRoot "SemanticBroken"
New-Item -ItemType Directory -Force -Path $SemanticBrokenRoot | Out-Null
$SemanticBrokenSource = Join-Path $SemanticBrokenRoot "SemanticBrokenScript.cs"
$SemanticBrokenReport = Join-Path $SemanticBrokenRoot "semantic_broken.csharp.report.json"
$SemanticBrokenManifest = Join-Path $SemanticBrokenRoot "semantic_broken.avidscript.json"
$SemanticBrokenWasm = Join-Path $SemanticBrokenRoot "semantic_broken.csharp_adapter.wasm"
$SemanticBrokenDotNetWasm = Join-Path $SemanticBrokenRoot "semantic_broken.dotnet.wasm"
$SemanticBrokenGuestIr = Join-Path $SemanticBrokenRoot "semantic_broken.guestir.json"
$SemanticBrokenFinalWasm = Join-Path $SemanticBrokenRoot "semantic_broken.wasm"
$SemanticBrokenText = @"
public static class SemanticBrokenScript
{
    public static void BeginPlay()
    {
        int value = "bad";
    }

    public static void Tick(float deltaSeconds)
    {
    }
}
"@
[System.IO.File]::WriteAllText($SemanticBrokenSource, $SemanticBrokenText, $Utf8)
[System.IO.File]::WriteAllText($SemanticBrokenManifest, "stale", $Utf8)
[System.IO.File]::WriteAllBytes($SemanticBrokenWasm, [byte[]]@(0, 97, 115, 109))
[System.IO.File]::WriteAllBytes($SemanticBrokenDotNetWasm, [byte[]]@(0, 97, 115, 109))
[System.IO.File]::WriteAllText($SemanticBrokenGuestIr, "stale", $Utf8)
[System.IO.File]::WriteAllBytes($SemanticBrokenFinalWasm, [byte[]]@(0, 97, 115, 109))

& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $SemanticBrokenRoot `
    -SourcePath $SemanticBrokenSource `
    -BindingPackagePath $BindingPackagePath `
    -ModuleId "p40_semantic_broken" `
    -ArtifactStem "semantic_broken" `
    -ReportPath $SemanticBrokenReport `
    -ManifestPath $SemanticBrokenManifest | Out-Null
$SemanticBrokenExit = $LASTEXITCODE
Assert-Condition ($SemanticBrokenExit -eq 1) "semantic errors must return exit code 1; actual=$SemanticBrokenExit"
Assert-Condition (Test-Path -LiteralPath $SemanticBrokenReport -PathType Leaf) "semantic failure report is missing"
$SemanticBrokenJson = Get-Content -Raw -LiteralPath $SemanticBrokenReport | ConvertFrom-Json
Assert-Condition ($SemanticBrokenJson.result -eq "semantic_failed") "semantic failure report result is not semantic_failed"
Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$SemanticBrokenJson.artifacts.semantic_file)) "semantic failure report does not reference a semantic artifact"
$SemanticBrokenArtifact = Resolve-ArtifactPath $SemanticBrokenJson.artifacts.semantic_file
Assert-Condition (Test-Path -LiteralPath $SemanticBrokenArtifact -PathType Leaf) "semantic failure artifact is missing"
$SemanticBrokenArtifactJson = Get-Content -Raw -LiteralPath $SemanticBrokenArtifact | ConvertFrom-Json
Assert-Condition (-not $SemanticBrokenArtifactJson.succeeded) "semantic failure artifact reports success"
Assert-Condition (@($SemanticBrokenJson.diagnostics | Where-Object code -eq "CS0029").Count -eq 1) "semantic failure report did not retain CS0029"
Assert-Condition (-not (Test-Path -LiteralPath $SemanticBrokenManifest -PathType Leaf)) "semantic failure must remove stale manifest"
Assert-Condition (-not (Test-Path -LiteralPath $SemanticBrokenWasm -PathType Leaf)) "semantic failure must remove stale adapter WASM"
Assert-Condition (-not (Test-Path -LiteralPath $SemanticBrokenDotNetWasm -PathType Leaf)) "semantic failure must remove stale dotnet WASM"
Assert-Condition (-not (Test-Path -LiteralPath $SemanticBrokenGuestIr -PathType Leaf)) "semantic failure must remove stale Guest IR"
Assert-Condition (-not (Test-Path -LiteralPath $SemanticBrokenFinalWasm -PathType Leaf)) "semantic failure must remove stale final WASM"

$NormalRoot = Join-Path $RunRoot "Normal"
$NormalReport = Join-Path $NormalRoot "normal.csharp.report.json"
$NormalManifest = Join-Path $NormalRoot "normal.avidscript.json"
$NormalSource = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\ActorLifecycleScript.cs"
New-Item -ItemType Directory -Force -Path $NormalRoot | Out-Null

& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $NormalRoot `
    -SourcePath $NormalSource `
    -ModuleId "p39_normal" `
    -ArtifactStem "normal" `
    -ReportPath $NormalReport `
    -ManifestPath $NormalManifest | Out-Null
$NormalExit = $LASTEXITCODE
Assert-Condition ($NormalExit -eq 0) "valid source build failed; exit=$NormalExit"
$NormalJson = Get-Content -Raw -LiteralPath $NormalReport | ConvertFrom-Json
Assert-Condition ($NormalJson.result -eq "direct_abi_built") "valid source did not build direct ABI"
$NormalFrontendPath = Resolve-ArtifactPath $NormalJson.artifacts.frontend_file
Assert-Condition (Test-Path -LiteralPath $NormalFrontendPath -PathType Leaf) "valid source frontend artifact is missing"
$FrontendJson = Get-Content -Raw -LiteralPath $NormalFrontendPath | ConvertFrom-Json
Assert-Condition ($FrontendJson.source.sha256 -eq $NormalJson.source.sha256) "report/frontend source hashes differ"
$NormalSemanticPath = Resolve-ArtifactPath $NormalJson.artifacts.semantic_file
Assert-Condition (Test-Path -LiteralPath $NormalSemanticPath -PathType Leaf) "valid source semantic artifact is missing"
$SemanticJson = Get-Content -Raw -LiteralPath $NormalSemanticPath | ConvertFrom-Json
Assert-Condition ($SemanticJson.schema_version -eq 5) "semantic artifact schema version is not 5"
Assert-Condition ($SemanticJson.semantic_version -eq "1.5") "semantic artifact version is not 1.5"
Assert-Condition ($SemanticJson.succeeded) "valid source semantic artifact reports failure"
Assert-Condition ($SemanticJson.source.sha256 -eq $FrontendJson.source.sha256) "semantic/frontend source hashes differ"
Assert-Condition ($SemanticJson.source.frontend_sha256 -eq $FrontendJson.source.sha256) "semantic artifact did not preserve the frontend source hash"
Assert-Condition (@($SemanticJson.callables).Count -eq 53) "ActorLifecycle semantic callable count is not 53"
Assert-Condition (@($SemanticJson.callables | Where-Object { $null -ne $_.import }).Count -eq 14) "ActorLifecycle semantic import count is not 14"
Assert-Condition (@($SemanticJson.callables | Where-Object { $null -ne $_.export }).Count -eq 6) "ActorLifecycle semantic export count is not 6"
Assert-Condition ($SemanticJson.reachability.mode -eq "export_roots") "ActorLifecycle semantic reachability is not export-rooted"
Assert-Condition (@($SemanticJson.reachability.root_callable_ids).Count -eq 6) "ActorLifecycle reachability root count is not 6"
Assert-Condition (@($SemanticJson.reachability.reachable_imports).Count -lt 14) "ActorLifecycle reachability did not remove unused imports"
Assert-Condition ($NormalJson.semantic.source_sha256 -eq $FrontendJson.source.sha256) "report semantic source hash differs"
Assert-Condition ($NormalJson.semantic.frontend_sha256 -eq $FrontendJson.source.sha256) "report semantic frontend hash differs"
Assert-Condition ($NormalJson.source.script_type -eq "ActorLifecycleScript") "report does not identify the AST-selected script type"
Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$NormalJson.artifacts.guest_ir_file)) "report does not reference Guest IR"
$NormalGuestIrPath = Resolve-ArtifactPath $NormalJson.artifacts.guest_ir_file
$NormalWasmPath = Resolve-ArtifactPath $NormalJson.artifacts.wasm_file
Assert-Condition (Test-Path -LiteralPath $NormalGuestIrPath -PathType Leaf) "valid source Guest IR artifact is missing"
Assert-Condition (Test-Path -LiteralPath $NormalWasmPath -PathType Leaf) "valid source WASM artifact is missing"
$GuestIrJson = Get-Content -Raw -LiteralPath $NormalGuestIrPath | ConvertFrom-Json
$SemanticSha256 = Get-Sha256Hex $NormalSemanticPath
$GuestIrSha256 = Get-Sha256Hex $NormalGuestIrPath
$WasmSha256 = Get-Sha256Hex $NormalWasmPath
Assert-Condition ($GuestIrJson.schema_version -eq 1 -and $GuestIrJson.ir_version -eq "1.0" -and $GuestIrJson.succeeded) "Guest IR contract is invalid"
Assert-Condition ($GuestIrJson.provenance.semantic_sha256 -eq $SemanticSha256) "Guest IR semantic provenance hash differs"
Assert-Condition ($NormalJson.guest_ir.schema_version -eq 1 -and $NormalJson.guest_ir.version -eq "1.0") "report Guest IR contract is invalid"
Assert-Condition ($NormalJson.guest_ir.semantic_sha256 -eq $SemanticSha256) "report Guest IR semantic hash differs"
Assert-Condition ($NormalJson.guest_ir.sha256 -eq $GuestIrSha256) "report Guest IR artifact hash differs"
Assert-Condition ($NormalJson.wasm.sha256 -eq $WasmSha256) "report WASM artifact hash differs"
Assert-Condition (@($NormalJson.observed_exports).Count -eq 6 -and $NormalJson.observed_exports -contains "avid_on_gameplay_event") "report does not expose all six direct ABI exports"
$ManifestJson = Get-Content -Raw -LiteralPath $NormalManifest | ConvertFrom-Json
Assert-Condition ($ManifestJson.source.sha256 -eq $FrontendJson.source.sha256) "manifest/frontend source hashes differ"
Assert-Condition ($ManifestJson.source.script_type -eq "ActorLifecycleScript") "manifest does not identify the AST-selected script type"
Assert-Condition (-not [string]::IsNullOrWhiteSpace($ManifestJson.source.frontend_file)) "manifest does not reference the frontend artifact"
Assert-Condition (-not [string]::IsNullOrWhiteSpace($ManifestJson.source.semantic_file)) "manifest does not reference the semantic artifact"
Assert-Condition ($ManifestJson.source.semantic_sha256 -eq $SemanticSha256) "manifest semantic hash differs"
Assert-Condition ($ManifestJson.guest_ir.file -eq $NormalJson.artifacts.guest_ir_file) "manifest Guest IR path differs"
Assert-Condition ($ManifestJson.guest_ir.schema_version -eq 1 -and $ManifestJson.guest_ir.version -eq "1.0") "manifest Guest IR contract is invalid"
Assert-Condition ($ManifestJson.guest_ir.sha256 -eq $GuestIrSha256) "manifest Guest IR hash differs"
Assert-Condition ($ManifestJson.wasm.sha256 -eq $WasmSha256) "manifest WASM hash differs"
Assert-Condition ($ManifestJson.toolchain.compiler -eq "avidscript-csharp-guest-wasm") "manifest does not identify the formal compiler chain"

Write-Output "AvidScript.CSharpFrontend.BuildIntegration: 9/9 passed"
