param(
    [string]$DotNetPath = (Join-Path $env:USERPROFILE ".dotnet\dotnet.exe"),
    [string]$BindingPackagePath = "",
    [string]$GeneratedBindingRoot = ""
)

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$BuildScript = Join-Path $PluginRoot "Build\BuildCSharpActorLifecycle.ps1"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\BuildIntegration"
$Utf8 = [System.Text.UTF8Encoding]::new($false)
if ([string]::IsNullOrWhiteSpace($GeneratedBindingRoot)) {
    $GeneratedBindingRoot = Join-Path $ProjectRoot "Saved\AvidScriptGeneratedBindings"
}
$GeneratedBindingRoot = [System.IO.Path]::GetFullPath($GeneratedBindingRoot)

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

function Find-CanonicalBindingPackage {
    param(
        [Parameter(Mandatory = $true)][string]$PackageName,
        [Parameter(Mandatory = $true)]
        [ValidateSet("Absent", "Empty", "NonEmpty")]
        [string]$ActiveObjectTypeMode,
        [string[]]$RequiredUeMembers = @(),
        [string[]]$RequiredImportNames = @()
    )

    $Candidates = foreach ($Candidate in Get-ChildItem `
            -LiteralPath $GeneratedBindingRoot `
            -Filter "package.json" `
            -File `
            -Recurse `
            -ErrorAction SilentlyContinue) {
        try {
            $Manifest = Get-Content -Raw -LiteralPath $Candidate.FullName |
                ConvertFrom-Json
            if ([string]$Manifest.package_name -cne $PackageName) {
                continue
            }
            $DescriptorPath = Join-Path `
                $Candidate.DirectoryName `
                ([string]$Manifest.files.descriptor)
            $ReferenceSourcePath = Join-Path `
                $Candidate.DirectoryName `
                ([string]$Manifest.files.reference_source)
            $Descriptor = Get-Content -Raw -LiteralPath $DescriptorPath |
                ConvertFrom-Json
            if ([int]$Descriptor.schema_version -lt 6 -or
                [string]$Descriptor.package_name -cne $PackageName -or
                (Get-Sha256Hex $DescriptorPath) -cne
                    [string]$Manifest.descriptor_sha256 -or
                (Get-Sha256Hex $ReferenceSourcePath) -cne
                    [string]$Manifest.reference_source_sha256) {
                continue
            }

            $ActiveProperty =
                $Descriptor.PSObject.Properties['active_object_type_ordinals']
            $ActiveModeMatches = switch ($ActiveObjectTypeMode) {
                "Absent" { $null -eq $ActiveProperty }
                "Empty" {
                    $null -ne $ActiveProperty -and
                    $ActiveProperty.Value -is [System.Array] -and
                    @($ActiveProperty.Value).Count -eq 0
                }
                "NonEmpty" {
                    $null -ne $ActiveProperty -and
                    $ActiveProperty.Value -is [System.Array] -and
                    @($ActiveProperty.Value).Count -gt 0
                }
            }
            if (-not $ActiveModeMatches) {
                continue
            }

            $ImportNames = @($Manifest.required_imports |
                ForEach-Object { [string]$_.name })
            $HasRequiredImports = $true
            foreach ($RequiredImportName in $RequiredImportNames) {
                if ($ImportNames -cnotcontains $RequiredImportName) {
                    $HasRequiredImports = $false
                    break
                }
            }
            if (-not $HasRequiredImports) {
                continue
            }

            $ImportStableIds = @($Manifest.required_imports |
                ForEach-Object { [string]$_.stable_id })
            $HasRequiredMembers = $true
            foreach ($RequiredUeMember in $RequiredUeMembers) {
                $MatchingBindings = @($Descriptor.bindings | Where-Object {
                    [string]$_.ue_member -ceq $RequiredUeMember -and
                    $ImportStableIds -ccontains [string]$_.stable_id
                })
                if ($MatchingBindings.Count -eq 0) {
                    $HasRequiredMembers = $false
                    break
                }
            }
            if (-not $HasRequiredMembers) {
                continue
            }

            [pscustomobject]@{
                Path = $Candidate.FullName
                ImportCount = @($Manifest.required_imports).Count
                LastWriteTime = $Candidate.LastWriteTime
            }
        }
        catch {
            continue
        }
    }
    $Selected = $Candidates |
        Sort-Object `
            -Property @{ Expression = { $_.ImportCount }; Descending = $true },
                @{ Expression = { $_.LastWriteTime }; Descending = $true } |
        Select-Object -First 1
    Assert-Condition ($null -ne $Selected) `
        "canonical binding package fixture is missing: package=$PackageName active=$ActiveObjectTypeMode"
    return [string]$Selected.Path
}

function New-BindingPackageWithScalarActiveObjectType {
    param(
        [Parameter(Mandatory = $true)][string]$SourceManifestPath,
        [Parameter(Mandatory = $true)][string]$OutputDirectory
    )

    $Manifest = Get-Content -Raw -LiteralPath $SourceManifestPath |
        ConvertFrom-Json
    $SourceDirectory = Split-Path -Parent $SourceManifestPath
    $DescriptorSource = Join-Path `
        $SourceDirectory `
        ([string]$Manifest.files.descriptor)
    $ReferenceSource = Join-Path `
        $SourceDirectory `
        ([string]$Manifest.files.reference_source)
    $Descriptor = Get-Content -Raw -LiteralPath $DescriptorSource |
        ConvertFrom-Json
    $ActiveProperty =
        $Descriptor.PSObject.Properties['active_object_type_ordinals']
    Assert-Condition (
        $null -ne $ActiveProperty -and
        @($ActiveProperty.Value).Count -gt 0) `
        "scalar active object-type fixture requires a non-empty canonical source package"
    $ActiveProperty.Value = [int]@($ActiveProperty.Value)[0]

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $DescriptorDestination =
        Join-Path $OutputDirectory ([string]$Manifest.files.descriptor)
    $ReferenceDestination =
        Join-Path $OutputDirectory ([string]$Manifest.files.reference_source)
    [System.IO.File]::WriteAllText(
        $DescriptorDestination,
        ($Descriptor | ConvertTo-Json -Depth 64),
        $Utf8)
    Copy-Item `
        -LiteralPath $ReferenceSource `
        -Destination $ReferenceDestination `
        -Force
    $Manifest.descriptor_sha256 = Get-Sha256Hex $DescriptorDestination
    $OutputManifestPath = Join-Path $OutputDirectory "package.json"
    [System.IO.File]::WriteAllText(
        $OutputManifestPath,
        ($Manifest | ConvertTo-Json -Depth 32),
        $Utf8)
    return $OutputManifestPath
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

function Write-GuestProvenanceMutationCompiler {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Text = @'
param(
    [Parameter(Mandatory = $true)][string]$DotNetPath,
    [Parameter(Mandatory = $true)][string]$SemanticPath,
    [Parameter(Mandatory = $true)][string]$FrontendArtifactSha256,
    [Parameter(Mandatory = $true)][string]$GuestIrPath,
    [Parameter(Mandatory = $true)][string]$DebugMapPath,
    [Parameter(Mandatory = $true)][string]$StateSchemaPath,
    [Parameter(Mandatory = $true)][string]$WasmPath,
    [Parameter(Mandatory = $true)][string]$InspectionPath,
    [string]$Configuration = "Release",
    [ValidateSet("enabled", "disabled")]
    [string]$DataLaneFusion = "enabled"
)

$ErrorActionPreference = "Stop"
$RealCompilerPath = $env:AVIDSCRIPT_REAL_GUEST_COMPILER
if ([string]::IsNullOrWhiteSpace($RealCompilerPath) -or
    -not (Test-Path -LiteralPath $RealCompilerPath -PathType Leaf)) {
    throw "Real Guest compiler path is missing for the provenance mutation fixture."
}

& $RealCompilerPath @PSBoundParameters
$RealCompilerExit = $LASTEXITCODE
if ($RealCompilerExit -ne 0) {
    exit $RealCompilerExit
}

$Model = Get-Content -Raw -LiteralPath $GuestIrPath | ConvertFrom-Json
$ObjectTypeImportIds = @($Model.imports | Where-Object {
    [string]$_.module -ceq "avidscript" -and
    [string]$_.name -ceq "avid_object_type_is_a"
} | ForEach-Object { [string]$_.id })
$ConstantsByResultId = @{}
foreach ($Function in @($Model.functions)) {
    foreach ($Block in @($Function.blocks)) {
        foreach ($Instruction in @($Block.instructions)) {
            if ([string]$Instruction.op -ceq "constant") {
                $ConstantsByResultId[[string]$Instruction.result_id] = $Instruction
            }
        }
    }
}

$Mutated = $false
foreach ($Function in @($Model.functions)) {
    foreach ($Block in @($Function.blocks)) {
        foreach ($Instruction in @($Block.instructions)) {
            $Operands = @($Instruction.operand_ids)
            if ([string]$Instruction.op -ceq "call" -and
                $ObjectTypeImportIds -ccontains [string]$Instruction.target_id -and
                $Operands.Count -eq 3 -and
                $ConstantsByResultId.ContainsKey([string]$Operands[2])) {
                $ConstantsByResultId[[string]$Operands[2]].constant.value = "-1"
                $Mutated = $true
                break
            }
        }
        if ($Mutated) {
            break
        }
    }
    if ($Mutated) {
        break
    }
}
if (-not $Mutated) {
    throw "Guest provenance mutation fixture found no object-type intrinsic call."
}

$Utf8 = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText(
    $GuestIrPath,
    ($Model | ConvertTo-Json -Depth 100),
    $Utf8)
exit 0
'@
    [System.IO.File]::WriteAllText($Path, $Text, $Utf8)
}

New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null

if ([string]::IsNullOrWhiteSpace($BindingPackagePath)) {
    $RequiredAuthorizationFunctions = @("GetActorScale3D", "SetActorScale3D")
    $BindingPackageCandidates = foreach ($Candidate in Get-ChildItem `
            -LiteralPath $GeneratedBindingRoot `
            -Filter "package.json" `
            -File `
            -Recurse `
            -ErrorAction SilentlyContinue) {
        try {
            $CandidateManifest = Get-Content -Raw -LiteralPath $Candidate.FullName | ConvertFrom-Json
            $CandidateDescriptorPath = Join-Path $Candidate.DirectoryName ([string]$CandidateManifest.files.descriptor)
            $CandidateDescriptor = Get-Content -Raw -LiteralPath $CandidateDescriptorPath | ConvertFrom-Json
            $CandidateImportStableIds = @($CandidateManifest.required_imports | ForEach-Object { [string]$_.stable_id })
            $ContainsRequiredFunctions = $true
            foreach ($RequiredFunction in $RequiredAuthorizationFunctions) {
                $AuthorizedBindings = @($CandidateDescriptor.bindings | Where-Object {
                    [string]$_.ue_function -ceq $RequiredFunction -and
                    $CandidateImportStableIds -ccontains [string]$_.stable_id
                })
                if ($AuthorizedBindings.Count -eq 0) {
                    $ContainsRequiredFunctions = $false
                    break
                }
            }
            if ($ContainsRequiredFunctions) {
                [pscustomobject]@{
                    Path = $Candidate.FullName
                    ImportCount = @($CandidateManifest.required_imports).Count
                    LastWriteTime = $Candidate.LastWriteTime
                }
            }
        }
        catch {
            continue
        }
    }
    $BindingPackagePath = $BindingPackageCandidates |
        Sort-Object -Property @{ Expression = { $_.ImportCount }; Descending = $true },
            @{ Expression = { $_.LastWriteTime }; Descending = $true } |
        Select-Object -First 1 -ExpandProperty Path
}
Assert-Condition (
    -not [string]::IsNullOrWhiteSpace($BindingPackagePath) -and
    (Test-Path -LiteralPath $BindingPackagePath -PathType Leaf)) `
    "generated binding package is missing; publish the default Phase 42 package before this integration test"

$RuntimeSetPackagePath = New-BindingPackageSubset `
    -AuthorizationManifestPath $BindingPackagePath `
    -OutputDirectory (Join-Path $RunRoot "RuntimeSetPackage") `
    -UeFunctions @("SetActorScale3D")
$EngineObjectAuthorizationPackagePath = Find-CanonicalBindingPackage `
    -PackageName "avidscript.engine.gameplay" `
    -ActiveObjectTypeMode "Absent" `
    -RequiredImportNames @("avid_object_type_is_a", "avid_owner_get_handle")
$EngineNoActiveRuntimePackagePath = Find-CanonicalBindingPackage `
    -PackageName "avidscript.engine.gameplay" `
    -ActiveObjectTypeMode "Empty" `
    -RequiredImportNames @("avid_object_type_is_a", "avid_owner_get_handle")
$ComponentAuthorizationPackagePath = Find-CanonicalBindingPackage `
    -PackageName "avidscript.sample.component_gameplay" `
    -ActiveObjectTypeMode "Absent" `
    -RequiredUeMembers @("ApplyGameplayValue") `
    -RequiredImportNames @("avid_owner_get_handle")
$ComponentActiveRuntimePackagePath = Find-CanonicalBindingPackage `
    -PackageName "avidscript.sample.component_gameplay" `
    -ActiveObjectTypeMode "NonEmpty" `
    -RequiredUeMembers @("ApplyGameplayValue") `
    -RequiredImportNames @("avid_owner_get_handle")
$ScalarActiveRuntimePackagePath =
    New-BindingPackageWithScalarActiveObjectType `
        -SourceManifestPath $ComponentActiveRuntimePackagePath `
        -OutputDirectory (Join-Path $RunRoot "ScalarActiveRuntimePackage")
$GuestProvenanceMutationCompilerPath = Join-Path `
    $RunRoot `
    "MutateGuestObjectTypeProvenance.ps1"
Write-GuestProvenanceMutationCompiler `
    -Path $GuestProvenanceMutationCompilerPath

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

$GuestObjectTypeMismatchRoot = Join-Path $RunRoot "GuestObjectTypeMismatch"
New-Item -ItemType Directory -Force -Path $GuestObjectTypeMismatchRoot |
    Out-Null
$GuestObjectTypeMismatchSource = Join-Path `
    $GuestObjectTypeMismatchRoot `
    "GuestObjectTypeMismatchScript.cs"
$GuestObjectTypeMismatchReport = Join-Path `
    $GuestObjectTypeMismatchRoot `
    "guest_object_type_mismatch.csharp.report.json"
$GuestObjectTypeMismatchManifest = Join-Path `
    $GuestObjectTypeMismatchRoot `
    "guest_object_type_mismatch.avidscript.json"
Write-LifecycleSource `
    -Path $GuestObjectTypeMismatchSource `
    -BeginPlayBody 'UObject value = UE.Self; AActor casted = AActor.TryCast(value);'
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $GuestObjectTypeMismatchRoot `
    -SourcePath $GuestObjectTypeMismatchSource `
    -BindingPackagePath $EngineObjectAuthorizationPackagePath `
    -RuntimeBindingPackagePath $EngineNoActiveRuntimePackagePath `
    -ModuleId "p51_guest_object_type_mismatch" `
    -ArtifactStem "guest_object_type_mismatch" `
    -ReportPath $GuestObjectTypeMismatchReport `
    -ManifestPath $GuestObjectTypeMismatchManifest | Out-Null
$GuestObjectTypeMismatchExit = $LASTEXITCODE
Assert-Condition ($GuestObjectTypeMismatchExit -eq 1) `
    "guest object-type mismatch must return exit 1; actual=$GuestObjectTypeMismatchExit"
$GuestObjectTypeMismatchJson =
    Get-Content -Raw -LiteralPath $GuestObjectTypeMismatchReport |
    ConvertFrom-Json
Assert-Condition (
    $GuestObjectTypeMismatchJson.result -eq
        "binding_runtime_object_type_mismatch") `
    "guest object-type mismatch has the wrong result"
Assert-Condition (
    @($GuestObjectTypeMismatchJson.diagnostics |
        Where-Object code -eq "ASBI4304").Count -eq 1) `
    "guest object-type mismatch diagnostic is missing"
$GuestObjectTypeMismatchDiagnostic = @(
    $GuestObjectTypeMismatchJson.diagnostics |
        Where-Object code -eq "ASBI4304")[0]
Assert-Condition (
    @($GuestObjectTypeMismatchDiagnostic.runtime_active_object_type_ordinals).Count -eq 0 -and
    @($GuestObjectTypeMismatchDiagnostic.guest_used_object_type_ordinals).Count -gt 0) `
    "guest object-type mismatch did not exercise an unactivated Guest ordinal"
Assert-Condition (
    -not (Test-Path -LiteralPath $GuestObjectTypeMismatchManifest -PathType Leaf)) `
    "guest object-type mismatch left a manifest"
Assert-Condition (
    -not (Test-Path -LiteralPath (
        Join-Path $GuestObjectTypeMismatchRoot "guest_object_type_mismatch.wasm") -PathType Leaf)) `
    "guest object-type mismatch left loadable WASM"

$InvalidGuestProvenanceRoot = Join-Path $RunRoot "InvalidGuestProvenance"
New-Item -ItemType Directory -Force -Path $InvalidGuestProvenanceRoot |
    Out-Null
$InvalidGuestProvenanceReport = Join-Path `
    $InvalidGuestProvenanceRoot `
    "invalid_guest_provenance.csharp.report.json"
$InvalidGuestProvenanceManifest = Join-Path `
    $InvalidGuestProvenanceRoot `
    "invalid_guest_provenance.avidscript.json"
$PreviousRealGuestCompiler = $env:AVIDSCRIPT_REAL_GUEST_COMPILER
$env:AVIDSCRIPT_REAL_GUEST_COMPILER = Join-Path `
    $PluginRoot `
    "Build\InvokeCSharpGuestCompiler.ps1"
try {
    & $BuildScript `
        -DotNetPath $DotNetPath `
        -OutputRoot $InvalidGuestProvenanceRoot `
        -SourcePath $GuestObjectTypeMismatchSource `
        -BindingPackagePath $EngineObjectAuthorizationPackagePath `
        -RuntimeBindingPackagePath $EngineNoActiveRuntimePackagePath `
        -GuestCompilerPath $GuestProvenanceMutationCompilerPath `
        -ModuleId "p51_invalid_guest_provenance" `
        -ArtifactStem "invalid_guest_provenance" `
        -ReportPath $InvalidGuestProvenanceReport `
        -ManifestPath $InvalidGuestProvenanceManifest | Out-Null
    $InvalidGuestProvenanceExit = $LASTEXITCODE
}
finally {
    $env:AVIDSCRIPT_REAL_GUEST_COMPILER = $PreviousRealGuestCompiler
}
Assert-Condition ($InvalidGuestProvenanceExit -eq 1) `
    "invalid Guest provenance must return exit 1; actual=$InvalidGuestProvenanceExit"
$InvalidGuestProvenanceJson =
    Get-Content -Raw -LiteralPath $InvalidGuestProvenanceReport |
    ConvertFrom-Json
Assert-Condition (
    $InvalidGuestProvenanceJson.result -eq
        "guest_object_type_provenance_invalid") `
    "invalid Guest provenance has the wrong result"
Assert-Condition (
    @($InvalidGuestProvenanceJson.diagnostics |
        Where-Object code -eq "ASBI4305").Count -eq 1) `
    "invalid Guest provenance diagnostic is missing"
Assert-Condition (
    -not (Test-Path -LiteralPath $InvalidGuestProvenanceManifest -PathType Leaf)) `
    "invalid Guest provenance left a manifest"
foreach ($InvalidGuestArtifact in @(
    "invalid_guest_provenance.guestir.json",
    "invalid_guest_provenance.csharp.debug.json",
    "invalid_guest_provenance.state.json",
    "invalid_guest_provenance.wasm")) {
    Assert-Condition (
        -not (Test-Path -LiteralPath (
            Join-Path $InvalidGuestProvenanceRoot $InvalidGuestArtifact) -PathType Leaf)) `
        "invalid Guest provenance left loadable artifact $InvalidGuestArtifact"
}

$RuntimeObjectTypeMismatchRoot = Join-Path $RunRoot "RuntimeObjectTypeMismatch"
New-Item -ItemType Directory -Force -Path $RuntimeObjectTypeMismatchRoot |
    Out-Null
$RuntimeObjectTypeMismatchSource = Join-Path `
    $RuntimeObjectTypeMismatchRoot `
    "RuntimeObjectTypeMismatchScript.cs"
$RuntimeObjectTypeMismatchReport = Join-Path `
    $RuntimeObjectTypeMismatchRoot `
    "runtime_object_type_mismatch.csharp.report.json"
$RuntimeObjectTypeMismatchManifest = Join-Path `
    $RuntimeObjectTypeMismatchRoot `
    "runtime_object_type_mismatch.avidscript.json"
Write-LifecycleSource `
    -Path $RuntimeObjectTypeMismatchSource `
    -BeginPlayBody 'UE.Self.ApplyGameplayValue(1.0f);'
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $RuntimeObjectTypeMismatchRoot `
    -SourcePath $RuntimeObjectTypeMismatchSource `
    -BindingPackagePath $ComponentAuthorizationPackagePath `
    -RuntimeBindingPackagePath $ComponentActiveRuntimePackagePath `
    -ModuleId "p51_runtime_object_type_mismatch" `
    -ArtifactStem "runtime_object_type_mismatch" `
    -ReportPath $RuntimeObjectTypeMismatchReport `
    -ManifestPath $RuntimeObjectTypeMismatchManifest | Out-Null
$RuntimeObjectTypeMismatchExit = $LASTEXITCODE
Assert-Condition ($RuntimeObjectTypeMismatchExit -eq 1) `
    "runtime object-type mismatch must return exit 1; actual=$RuntimeObjectTypeMismatchExit"
$RuntimeObjectTypeMismatchJson =
    Get-Content -Raw -LiteralPath $RuntimeObjectTypeMismatchReport |
    ConvertFrom-Json
Assert-Condition (
    $RuntimeObjectTypeMismatchJson.result -eq
        "binding_runtime_object_type_mismatch") `
    "runtime object-type mismatch has the wrong result"
Assert-Condition (
    @($RuntimeObjectTypeMismatchJson.diagnostics |
        Where-Object code -eq "ASBI4304").Count -eq 1) `
    "runtime object-type mismatch diagnostic is missing"
$RuntimeObjectTypeMismatchDiagnostic = @(
    $RuntimeObjectTypeMismatchJson.diagnostics |
        Where-Object code -eq "ASBI4304")[0]
Assert-Condition (
    @($RuntimeObjectTypeMismatchDiagnostic.runtime_active_object_type_ordinals).Count -gt 0 -and
    @($RuntimeObjectTypeMismatchDiagnostic.guest_used_object_type_ordinals).Count -eq 0) `
    "runtime object-type mismatch did not exercise an unused activation ordinal"
Assert-Condition (
    -not (Test-Path -LiteralPath $RuntimeObjectTypeMismatchManifest -PathType Leaf)) `
    "runtime object-type mismatch left a manifest"
Assert-Condition (
    -not (Test-Path -LiteralPath (
        Join-Path $RuntimeObjectTypeMismatchRoot "runtime_object_type_mismatch.wasm") -PathType Leaf)) `
    "runtime object-type mismatch left loadable WASM"

$ScalarActiveRoot = Join-Path $RunRoot "ScalarActiveObjectType"
New-Item -ItemType Directory -Force -Path $ScalarActiveRoot | Out-Null
$ScalarActiveReport = Join-Path `
    $ScalarActiveRoot `
    "scalar_active_object_type.csharp.report.json"
$ScalarActiveManifest = Join-Path `
    $ScalarActiveRoot `
    "scalar_active_object_type.avidscript.json"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $ScalarActiveRoot `
    -SourcePath $RuntimeObjectTypeMismatchSource `
    -BindingPackagePath $ComponentAuthorizationPackagePath `
    -RuntimeBindingPackagePath $ScalarActiveRuntimePackagePath `
    -ModuleId "p51_scalar_active_object_type" `
    -ArtifactStem "scalar_active_object_type" `
    -ReportPath $ScalarActiveReport `
    -ManifestPath $ScalarActiveManifest | Out-Null
$ScalarActiveExit = $LASTEXITCODE
Assert-Condition ($ScalarActiveExit -eq 1) `
    "scalar active object-type package must return exit 1; actual=$ScalarActiveExit"
$ScalarActiveJson = Get-Content -Raw -LiteralPath $ScalarActiveReport |
    ConvertFrom-Json
Assert-Condition ($ScalarActiveJson.result -eq "runtime_binding_package_invalid") `
    "scalar active object-type package has the wrong result"
Assert-Condition (
    @($ScalarActiveJson.diagnostics | Where-Object {
        $_.code -eq "ASBI4302" -and
        $_.message -like "*must be a JSON array*"
    }).Count -eq 1) `
    "scalar active object-type package was not rejected as a JSON shape error"
Assert-Condition (
    -not (Test-Path -LiteralPath $ScalarActiveManifest -PathType Leaf)) `
    "scalar active object-type package left a manifest"
Assert-Condition (
    -not (Test-Path -LiteralPath (
        Join-Path $ScalarActiveRoot "scalar_active_object_type.wasm") -PathType Leaf)) `
    "scalar active object-type package left loadable WASM"

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
Assert-Condition ($PreparedFinalJson.semantic_cache.lookup -ceq "disabled") `
    "prepared final unexpectedly performed semantic cache lookup"
Assert-Condition (
    [int]$PreparedFinalJson.tool_invocations.frontend -eq 0 -and
    [int]$PreparedFinalJson.tool_invocations.semantic -eq 0 -and
    [int]$PreparedFinalJson.tool_invocations.guest_ir -eq 1 -and
    [int]$PreparedFinalJson.tool_invocations.wasm_backend -eq 1) `
    "prepared final structured invocation counts differ"
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
Assert-Condition (
    [int]$BrokenJson.tool_invocations.frontend -eq 1 -and
    [int]$BrokenJson.tool_invocations.semantic -eq 0) `
    "syntax failure structured invocation counts differ"
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
Assert-Condition (@("miss", "hit", "rejected") -ccontains [string]$NormalJson.semantic_cache.lookup) `
    "valid source report has an invalid semantic cache lookup state"
$ExpectedRoslynCount = if ($NormalJson.semantic_cache.lookup -ceq "hit") { 0 } else { 1 }
Assert-Condition (
    [int]$NormalJson.tool_invocations.frontend -eq $ExpectedRoslynCount -and
    [int]$NormalJson.tool_invocations.semantic -eq $ExpectedRoslynCount) `
    "valid source Roslyn invocation counts disagree with cache lookup"
Assert-Condition (
    [int]$NormalJson.tool_invocations.guest_ir -eq 1 -and
    [int]$NormalJson.tool_invocations.wasm_backend -eq 1) `
    "valid source did not retain Guest IR and WASM invocation counts"
$NormalFrontendPath = Resolve-ArtifactPath $NormalJson.artifacts.frontend_file
Assert-Condition (Test-Path -LiteralPath $NormalFrontendPath -PathType Leaf) "valid source frontend artifact is missing"
$FrontendJson = Get-Content -Raw -LiteralPath $NormalFrontendPath | ConvertFrom-Json
$FrontendArtifactSha256 = Get-Sha256Hex $NormalFrontendPath
Assert-Condition ($FrontendJson.source.sha256 -eq $NormalJson.source.sha256) "report/frontend source hashes differ"
$NormalSemanticPath = Resolve-ArtifactPath $NormalJson.artifacts.semantic_file
Assert-Condition (Test-Path -LiteralPath $NormalSemanticPath -PathType Leaf) "valid source semantic artifact is missing"
$SemanticJson = Get-Content -Raw -LiteralPath $NormalSemanticPath | ConvertFrom-Json
Assert-Condition ($SemanticJson.schema_version -eq 17) "semantic artifact schema version is not 17"
Assert-Condition ($SemanticJson.semantic_version -eq "1.19") "semantic artifact version is not 1.19"
Assert-Condition ($SemanticJson.succeeded) "valid source semantic artifact reports failure"
Assert-Condition ($SemanticJson.source.sha256 -eq $FrontendJson.source.sha256) "semantic/frontend source hashes differ"
Assert-Condition ($SemanticJson.source.frontend_sha256 -eq $FrontendJson.source.sha256) "semantic artifact did not preserve the frontend source hash"
Assert-Condition (@($SemanticJson.callables).Count -eq 84) "ActorLifecycle semantic callable count is not 84"
$ActorMatchesCallables = @($SemanticJson.callables | Where-Object {
    [string]$_.method_symbol_id -ceq "symbol:method:global::AvidScript.AActor.Matches(global::AvidScript.AActor):bool"
})
Assert-Condition ($ActorMatchesCallables.Count -eq 1) "ActorLifecycle semantic artifact is missing the AActor.Matches helper"
Assert-Condition (@($SemanticJson.callables | Where-Object { $null -ne $_.import }).Count -eq 20) "ActorLifecycle semantic import count is not 20"
Assert-Condition (@($SemanticJson.callables | Where-Object { $null -ne $_.export }).Count -eq 5) "ActorLifecycle semantic export count is not 5"
$ContinuationCallbacks = @($SemanticJson.continuation_callbacks)
Assert-Condition ($ContinuationCallbacks.Count -eq 2) "ActorLifecycle does not expose two continuation callbacks"
Assert-Condition ((@($ContinuationCallbacks.callback_id) -join ',') -ceq '9,10') "ActorLifecycle continuation callback ids differ"
Assert-Condition ((@($ContinuationCallbacks.payload_kind) -join ',') -ceq 'none,object') "ActorLifecycle continuation callback payload kinds differ"
$AsyncMethods = @($SemanticJson.async_methods)
Assert-Condition ($AsyncMethods.Count -eq 1) "ActorLifecycle does not expose one controlled async method"
Assert-Condition ([string]$AsyncMethods[0].export_name -ceq 'avid_on_begin_play' -and
    [string]$AsyncMethods[0].lowering -ceq 'continuation_cfg' -and
    [int]$AsyncMethods[0].entry_segment_ordinal -eq 0 -and
    @($AsyncMethods[0].segments).Count -eq 29) `
    "ActorLifecycle BeginPlay controlled async graph is invalid"
$AsyncAwaitSites = @($AsyncMethods[0].segments | ForEach-Object { $_.await_site } | Where-Object { $null -ne $_ })
Assert-Condition ((@($AsyncAwaitSites.callback_id) -join ',') -ceq '1073741824,1073741825,1073741826' -and
    (@($AsyncAwaitSites.producer_kind) -join ',') -ceq 'object_load,next_tick,delay' -and
    (@($AsyncAwaitSites.state_frame | ForEach-Object { @($_.slots).Count }) -join ',') -ceq '2,4,1') `
    "ActorLifecycle compiler-owned await sites differ"
$AsyncTransfers = @($AsyncMethods[0].segments | ForEach-Object { $_.transfer })
Assert-Condition (@($AsyncTransfers | Where-Object { [string]$_.kind -ceq 'await' }).Count -eq 3 -and
    @($AsyncTransfers | Where-Object { [string]$_.kind -ceq 'branch' }).Count -eq 5 -and
    @($AsyncTransfers | Where-Object { [string]$_.kind -ceq 'goto' }).Count -eq 18 -and
    @($AsyncTransfers | Where-Object { [string]$_.kind -ceq 'return' }).Count -eq 3 -and
    @($AsyncTransfers | Where-Object { [string]$_.kind -ceq 'await' -and [int]$_.primary_target -lt 0 }).Count -eq 0) `
    "ActorLifecycle continuation CFG transfer contract differs"
$GameplayCallbacks = @($SemanticJson.gameplay_event_callbacks)
Assert-Condition ($GameplayCallbacks.Count -eq 4) "ActorLifecycle does not expose four natural gameplay callbacks"
Assert-Condition ((@($GameplayCallbacks.event_type) -join ',') -ceq '1,2,3,4') "ActorLifecycle gameplay callback event types differ"
Assert-Condition ((@($GameplayCallbacks.name) -join ',') -ceq 'OnBeginOverlap,OnEndOverlap,OnHit,OnInput') "ActorLifecycle gameplay callback names differ"
Assert-Condition ($SemanticJson.reachability.mode -eq "entrypoint_roots") "ActorLifecycle semantic reachability is not entrypoint-rooted"
Assert-Condition (@($SemanticJson.reachability.root_callable_ids).Count -eq 11) "ActorLifecycle reachability root count is not 11"
Assert-Condition (@($SemanticJson.reachability.reachable_imports).Count -lt 20) "ActorLifecycle reachability did not remove unused imports"
Assert-Condition (@($SemanticJson.reachability.reachable_imports | Where-Object {
    [string]$_.module -ceq 'env' -and [string]$_.name -ceq 'continuation_load_object'
}).Count -eq 1) "ActorLifecycle reachability omits continuation_load_object"
foreach ($StateImportName in @('continuation_state_store', 'continuation_state_read')) {
    Assert-Condition (@($SemanticJson.callables | Where-Object {
        [string]$_.import.module -ceq 'env' -and [string]$_.import.name -ceq $StateImportName
    }).Count -eq 1) "ActorLifecycle semantic ABI omits $StateImportName"
}
Assert-Condition ($NormalJson.semantic.source_sha256 -eq $FrontendJson.source.sha256) "report semantic source hash differs"
Assert-Condition ($NormalJson.semantic.frontend_sha256 -eq $FrontendJson.source.sha256) "report semantic frontend hash differs"
Assert-Condition ($NormalJson.source.script_type -eq "ActorLifecycleScript") "report does not identify the AST-selected script type"
Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$NormalJson.artifacts.guest_ir_file)) "report does not reference Guest IR"
$NormalGuestIrPath = Resolve-ArtifactPath $NormalJson.artifacts.guest_ir_file
$NormalDebugMapPath = Resolve-ArtifactPath $NormalJson.artifacts.debug_map_file
$NormalStateSchemaPath = Resolve-ArtifactPath $NormalJson.artifacts.state_schema_file
$NormalWasmPath = Resolve-ArtifactPath $NormalJson.artifacts.wasm_file
Assert-Condition (Test-Path -LiteralPath $NormalGuestIrPath -PathType Leaf) "valid source Guest IR artifact is missing"
Assert-Condition (Test-Path -LiteralPath $NormalDebugMapPath -PathType Leaf) "valid source C# debug map artifact is missing"
Assert-Condition (Test-Path -LiteralPath $NormalStateSchemaPath -PathType Leaf) "valid source state schema artifact is missing"
Assert-Condition (Test-Path -LiteralPath $NormalWasmPath -PathType Leaf) "valid source WASM artifact is missing"
$GuestIrJson = Get-Content -Raw -LiteralPath $NormalGuestIrPath | ConvertFrom-Json
$DebugMapJson = Get-Content -Raw -LiteralPath $NormalDebugMapPath | ConvertFrom-Json
$StateSchemaJson = Get-Content -Raw -LiteralPath $NormalStateSchemaPath | ConvertFrom-Json
$SemanticSha256 = Get-Sha256Hex $NormalSemanticPath
$GuestIrSha256 = Get-Sha256Hex $NormalGuestIrPath
$DebugMapSha256 = Get-Sha256Hex $NormalDebugMapPath
$WasmSha256 = Get-Sha256Hex $NormalWasmPath
Assert-Condition ($GuestIrJson.schema_version -eq 2 -and $GuestIrJson.ir_version -eq "1.1" -and $GuestIrJson.succeeded) "Guest IR contract is invalid"
Assert-Condition ($GuestIrJson.provenance.semantic_sha256 -eq $SemanticSha256) "Guest IR semantic provenance hash differs"
Assert-Condition (@($GuestIrJson.imports | Where-Object {
    [string]$_.module -ceq 'env' -and [string]$_.name -ceq 'continuation_load_object'
}).Count -eq 1) "Guest IR does not import continuation_load_object exactly once"
foreach ($StateImportName in @('continuation_state_store', 'continuation_state_read')) {
    Assert-Condition (@($GuestIrJson.imports | Where-Object {
        [string]$_.module -ceq 'env' -and [string]$_.name -ceq $StateImportName
    }).Count -eq 1) "Guest IR does not import $StateImportName exactly once"
}
Assert-Condition (@($GuestIrJson.exports | Where-Object {
    [string]$_.name -ceq 'avid_on_continuation_v2'
}).Count -eq 1) "Guest IR does not export avid_on_continuation_v2 exactly once"
Assert-Condition (@($GuestIrJson.exports | Where-Object {
    [string]$_.name -ceq 'avid_on_continuation'
}).Count -eq 0) "Semantic 13 Guest IR retained the legacy continuation export"
Assert-Condition (@($GuestIrJson.functions | Where-Object {
    [string]$_.id -clike 'function:synthetic:async_resume:*'
}).Count -eq 3) "Guest IR does not contain three controlled async resume functions"
$BeginPlayFunctionId = [string](@($GuestIrJson.exports | Where-Object {
    [string]$_.name -ceq 'avid_on_begin_play'
})[0].function_id)
$ControlledAsyncFunctions = @($GuestIrJson.functions | Where-Object {
    ([string]$_.id -ceq $BeginPlayFunctionId) -or
        ([string]$_.id -clike 'function:synthetic:async_resume:*')
})
Assert-Condition (@($ControlledAsyncFunctions | ForEach-Object {
        @($_.blocks | Where-Object { [string]$_.id -clike '*:schedule_rejected' })
    }).Count -eq 5 -and
    @($ControlledAsyncFunctions | ForEach-Object {
        @($_.blocks | Where-Object { [string]$_.id -clike '*:state_rejected' })
    }).Count -eq 3 -and
    @($ControlledAsyncFunctions | ForEach-Object {
        @($_.blocks | Where-Object { [string]$_.terminator.kind -ceq 'trap' })
    }).Count -eq 8) `
    "each ActorLifecycle await producer does not fail closed on state-store or scheduling rejection"
Assert-Condition ($DebugMapJson.schema_version -eq 1 -and $DebugMapJson.debug_version -eq "1.0") "C# debug map contract is invalid"
Assert-Condition ($DebugMapJson.module_id -eq $GuestIrJson.module_id) "C# debug map module identity differs from Guest IR"
Assert-Condition ($DebugMapJson.source.id -eq $NormalJson.source.file -and $DebugMapJson.source.sha256 -eq $FrontendJson.source.sha256) "C# debug map source identity differs"
Assert-Condition ($DebugMapJson.provenance.frontend_artifact_sha256 -eq $FrontendArtifactSha256) "C# debug map frontend artifact provenance hash differs"
Assert-Condition ($DebugMapJson.provenance.semantic_sha256 -eq $SemanticSha256) "C# debug map semantic provenance hash differs"
Assert-Condition ($DebugMapJson.provenance.guest_ir_sha256 -eq $GuestIrSha256) "C# debug map Guest IR provenance hash differs"
Assert-Condition (@($DebugMapJson.functions).Count -gt 0) "C# debug map does not contain generated function symbols"
Assert-Condition (@($GuestIrJson.functions | Where-Object { $_.id -ceq 'function:synthetic:gameplay_event' }).Count -eq 1) "Guest IR does not contain exactly one generated gameplay router"
Assert-Condition (@($DebugMapJson.functions | Where-Object { $_.guest_function_id -ceq 'function:synthetic:gameplay_event' }).Count -eq 0) "generated gameplay router published a fake C# source location"
Assert-Condition (@($DebugMapJson.functions | Where-Object {
    [string]$_.guest_function_id -clike 'function:synthetic:async_resume:*' -and
    [string]$_.method_symbol_id -clike '*#async_resume:*'
}).Count -eq 3) "controlled async resume functions do not map to their source segments"
Assert-Condition ([int]$DebugMapJson.defined_function_count -eq @($GuestIrJson.functions).Count) "generated gameplay router collapsed the debug-map function index space"
Assert-Condition ($NormalJson.guest_ir.schema_version -eq 2 -and $NormalJson.guest_ir.version -eq "1.1") "report Guest IR contract is invalid"
Assert-Condition ($NormalJson.guest_ir.semantic_sha256 -eq $SemanticSha256) "report Guest IR semantic hash differs"
Assert-Condition ($NormalJson.guest_ir.sha256 -eq $GuestIrSha256) "report Guest IR artifact hash differs"
Assert-Condition ($NormalJson.debug_map.schema_version -eq 1 -and $NormalJson.debug_map.version -eq "1.0") "report C# debug map contract is invalid"
Assert-Condition ($NormalJson.debug_map.module_id -eq $GuestIrJson.module_id) "report C# debug map module identity differs"
Assert-Condition ($NormalJson.debug_map.sha256 -eq $DebugMapSha256) "report C# debug map artifact hash differs"
Assert-Condition ($StateSchemaJson.schema_version -eq 2 -and
    $StateSchemaJson.strategy -eq "host_snapshot" -and
    $StateSchemaJson.policy -eq "compatible" -and
    $StateSchemaJson.contract_version -eq 1 -and
    @($StateSchemaJson.slots | Where-Object { $_.PSObject.Properties.Name -contains "aliases" }).Count -eq @($StateSchemaJson.slots).Count) `
    "state schema v2 contract is invalid"
Assert-Condition ($NormalJson.state_migration.schema_version -eq 2 -and
    $NormalJson.state_migration.policy -eq "compatible" -and
    $NormalJson.state_migration.contract_version -eq 1) `
    "report state migration contract is invalid"
Assert-Condition ($NormalJson.wasm.sha256 -eq $WasmSha256) "report WASM artifact hash differs"
Assert-Condition (
    @($NormalJson.observed_exports).Count -eq 7 -and
    $NormalJson.observed_exports -contains "avid_on_gameplay_event" -and
    $NormalJson.observed_exports -contains "avid_on_continuation_v2" -and
    $NormalJson.observed_exports -notcontains "avid_on_continuation") `
    "report does not expose all seven direct ABI exports"
$ManifestJson = Get-Content -Raw -LiteralPath $NormalManifest | ConvertFrom-Json
Assert-Condition ($ManifestJson.source.sha256 -eq $FrontendJson.source.sha256) "manifest/frontend source hashes differ"
Assert-Condition ($ManifestJson.source.script_type -eq "ActorLifecycleScript") "manifest does not identify the AST-selected script type"
Assert-Condition (-not [string]::IsNullOrWhiteSpace($ManifestJson.source.frontend_file)) "manifest does not reference the frontend artifact"
Assert-Condition (-not [string]::IsNullOrWhiteSpace($ManifestJson.source.semantic_file)) "manifest does not reference the semantic artifact"
Assert-Condition ($ManifestJson.source.semantic_sha256 -eq $SemanticSha256) "manifest semantic hash differs"
Assert-Condition ($ManifestJson.guest_ir.file -eq $NormalJson.artifacts.guest_ir_file) "manifest Guest IR path differs"
Assert-Condition ($ManifestJson.guest_ir.schema_version -eq 2 -and $ManifestJson.guest_ir.version -eq "1.1") "manifest Guest IR contract is invalid"
Assert-Condition ($ManifestJson.guest_ir.module_id -eq $GuestIrJson.module_id) "manifest Guest IR module identity differs"
Assert-Condition ($ManifestJson.guest_ir.sha256 -eq $GuestIrSha256) "manifest Guest IR hash differs"
Assert-Condition ($ManifestJson.debug_map.file -eq $NormalJson.artifacts.debug_map_file) "manifest C# debug map path differs"
Assert-Condition ($ManifestJson.debug_map.schema_version -eq 1 -and $ManifestJson.debug_map.version -eq "1.0") "manifest C# debug map contract is invalid"
Assert-Condition ($ManifestJson.debug_map.module_id -eq $GuestIrJson.module_id) "manifest C# debug map module identity differs"
Assert-Condition ($ManifestJson.debug_map.sha256 -eq $DebugMapSha256) "manifest C# debug map hash differs"
Assert-Condition ($ManifestJson.state_migration.schema_version -eq 2 -and
    $ManifestJson.state_migration.policy -eq "compatible" -and
    $ManifestJson.state_migration.contract_version -eq 1 -and
    @($ManifestJson.state_migration.slots | Where-Object { $_.PSObject.Properties.Name -contains "aliases" }).Count -eq @($ManifestJson.state_migration.slots).Count) `
    "manifest state migration contract is invalid"
Assert-Condition ($ManifestJson.wasm.sha256 -eq $WasmSha256) "manifest WASM hash differs"
Assert-Condition ($ManifestJson.toolchain.compiler -eq "avidscript-csharp-guest-wasm") "manifest does not identify the formal compiler chain"

Write-Output "AvidScript.CSharpFrontend.BuildIntegration: 15/15 passed"
