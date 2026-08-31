$ErrorActionPreference = "Stop"

$BuildRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $BuildRoot "AvidScriptGeneratedTypeReloadClassification.ps1")

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)]$Actual,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ($Expected -cne $Actual) {
        throw "$Message expected=$Expected actual=$Actual"
    }
}

$HashA = "a" * 64
$HashB = "b" * 64
$PackageA = "c" * 64
$BaseManifest = [pscustomobject]@{
    schema_version = 5
    generator_version = "1.6"
    semantic_artifact_sha256 = "d" * 64
    generation_key_sha256 = "e" * 64
    module_name = "AvidScriptGenerated"
    unreal_version = "5.8"
    outputs = @(
        [pscustomobject]@{
            relative_path = "Public/AvidScriptGeneratedTypes.h"
            sha256 = $HashA
            length = 128
        },
        [pscustomobject]@{
            relative_path = "Private/AvidScriptGeneratedTypes.cpp"
            sha256 = $HashB
            length = 256
        }
    )
}
$BodyOnlyManifest = $BaseManifest | ConvertTo-Json -Depth 8 | ConvertFrom-Json
$BodyOnlyManifest.semantic_artifact_sha256 = "f" * 64
$BodyOnlyManifest.generation_key_sha256 = "1" * 64
$StructuralManifest = $BaseManifest | ConvertTo-Json -Depth 8 | ConvertFrom-Json
$StructuralManifest.outputs[1].sha256 = "2" * 64

$BaseStructure = Get-AvidScriptGeneratedTypeNativeStructureSha256 $BaseManifest
$BodyOnlyStructure = Get-AvidScriptGeneratedTypeNativeStructureSha256 $BodyOnlyManifest
$StructuralStructure = Get-AvidScriptGeneratedTypeNativeStructureSha256 $StructuralManifest
Assert-Equal $BaseStructure $BodyOnlyStructure "Body-only semantic changes must preserve native structure"
if ($BaseStructure -ceq $StructuralStructure) {
    throw "Native output drift must change the generated type structure identity."
}

$Initial = New-AvidScriptGeneratedTypeReloadMetadata -NativeStructureSha256 $BaseStructure
Assert-Equal "initial_install" $Initial.classification "Missing baseline classification"
$BodyOnly = New-AvidScriptGeneratedTypeReloadMetadata `
    -NativeStructureSha256 $BodyOnlyStructure `
    -PreviousPackageId $PackageA `
    -PreviousNativeStructureSha256 $BaseStructure
Assert-Equal "body_only" $BodyOnly.classification "Equal structure classification"
$Structural = New-AvidScriptGeneratedTypeReloadMetadata `
    -NativeStructureSha256 $StructuralStructure `
    -PreviousPackageId $PackageA `
    -PreviousNativeStructureSha256 $BaseStructure
Assert-Equal "native_rebuild_required" $Structural.classification "Changed structure classification"

$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) "AvidScriptGeneratedReloadContract"
if (Test-Path -LiteralPath $FixtureRoot) {
    Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $FixtureRoot | Out-Null
try {
    $ManifestPath = Join-Path $FixtureRoot "AvidScriptGeneratedManifest.json"
    $DescriptorPath = Join-Path $FixtureRoot "AvidScriptGeneratedPackage.json"
    [System.IO.File]::WriteAllText(
        $ManifestPath,
        ($BaseManifest | ConvertTo-Json -Depth 8),
        [System.Text.UTF8Encoding]::new($false))
    $ManifestSha256 = (Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $LegacyDescriptor = [ordered]@{
        schema_version = 1
        package_id = $PackageA
        type_manifest = [ordered]@{
            file = "AvidScriptGeneratedManifest.json"
            sha256 = $ManifestSha256
        }
    }
    [System.IO.File]::WriteAllText(
        $DescriptorPath,
        ($LegacyDescriptor | ConvertTo-Json -Depth 8),
        [System.Text.UTF8Encoding]::new($false))

    $LegacyBaseline = Get-AvidScriptGeneratedTypeReloadBaseline `
        -DescriptorPath $DescriptorPath `
        -OutputRoot $FixtureRoot
    Assert-Equal $PackageA $LegacyBaseline.PackageId "Legacy package baseline id"
    Assert-Equal $BaseStructure $LegacyBaseline.NativeStructureSha256 "Legacy package structure fallback"

    $LegacyDescriptor.reload = [pscustomobject]$BodyOnly
    [System.IO.File]::WriteAllText(
        $DescriptorPath,
        ($LegacyDescriptor | ConvertTo-Json -Depth 8),
        [System.Text.UTF8Encoding]::new($false))
    Remove-Item -LiteralPath $ManifestPath -Force
    $EmbeddedBaseline = Get-AvidScriptGeneratedTypeReloadBaseline `
        -DescriptorPath $DescriptorPath `
        -OutputRoot $FixtureRoot
    Assert-Equal $BaseStructure $EmbeddedBaseline.NativeStructureSha256 "Embedded baseline survives missing outputs"
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

Write-Output "Generated type reload classification contracts: PASS"
