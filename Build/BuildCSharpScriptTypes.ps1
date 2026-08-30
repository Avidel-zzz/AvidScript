param(
    [Parameter(Mandatory = $true)][string]$DotNetPath,
    [Parameter(Mandatory = $true)][string]$SourcePath,
    [Parameter(Mandatory = $true)][string]$SourceId,
    [Parameter(Mandatory = $true)][string]$BindingPackageManifestPath,
    [string]$OutputRoot = "",
    [string]$ArtifactRoot = "",
    [string]$ModuleName = "AvidScriptGenerated",
    [string]$UnrealVersion = "5.8",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildDir
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$BindingPackageFunctions = Join-Path $BuildDir "AvidScriptCSharpBindingPackage.ps1"
$FrontendScript = Join-Path $BuildDir "InvokeCSharpFrontend.ps1"
$SemanticScript = Join-Path $BuildDir "InvokeCSharpSemantic.ps1"
$GeneratorProject = Join-Path $PluginRoot "Tools\AvidScript.UeTypeGenerator\AvidScript.UeTypeGenerator.csproj"
$GlobalJsonPath = Join-Path $PluginRoot "global.json"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

foreach ($RequiredFile in @(
        $DotNetPath,
        $SourcePath,
        $BindingPackageManifestPath,
        $BindingPackageFunctions,
        $FrontendScript,
        $SemanticScript,
        $GeneratorProject,
        $GlobalJsonPath)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "Required C# script type generation file is missing: $RequiredFile"
    }
}
if ([string]::IsNullOrWhiteSpace($SourceId) -or
    [System.IO.Path]::IsPathRooted($SourceId) -or
    $SourceId.Contains("\") -or
    $SourceId.Split('/') -contains '..') {
    throw "SourceId must be a stable forward-slash relative identity without parent traversal."
}
if ($ModuleName -cnotmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
    throw "ModuleName must be an ASCII C++ identifier."
}

$DotNetPath = (Resolve-Path -LiteralPath $DotNetPath).Path
$SourcePath = (Resolve-Path -LiteralPath $SourcePath).Path
$BindingPackageManifestPath = (Resolve-Path -LiteralPath $BindingPackageManifestPath).Path
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $PluginRoot "Source\AvidScriptGenerated"
}
if ([string]::IsNullOrWhiteSpace($ArtifactRoot)) {
    $ArtifactRoot = Join-Path $ProjectRoot "Saved\AvidScript\ScriptTypeGeneration"
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$ArtifactRoot = [System.IO.Path]::GetFullPath($ArtifactRoot)

. $BindingPackageFunctions
$BindingPackage = Resolve-AvidScriptCSharpBindingPackage `
    -ManifestPath $BindingPackageManifestPath

$ExpectedSdkVersion = [string](Get-Content -Raw -LiteralPath $GlobalJsonPath | ConvertFrom-Json).sdk.version
$DotNetVersionOutput = @(& $DotNetPath --version)
$DotNetVersionExitCode = $LASTEXITCODE
$ActualSdkVersion = if ($DotNetVersionOutput.Count -gt 0) { [string]$DotNetVersionOutput[0] } else { "" }
if ($DotNetVersionExitCode -ne 0 -or $ActualSdkVersion.Trim() -cne $ExpectedSdkVersion) {
    throw "C# script type generation requires .NET SDK $ExpectedSdkVersion; actual=$ActualSdkVersion"
}

$SourceSha256 = (Get-FileHash -LiteralPath $SourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
$RunRoot = Join-Path $ArtifactRoot $SourceSha256
$FrontendPath = Join-Path $RunRoot "script-types.frontend.json"
$SemanticPath = Join-Path $RunRoot "script-types.semantic.json"
New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null

& $FrontendScript `
    -DotNetPath $DotNetPath `
    -SourcePath $SourcePath `
    -SourceId $SourceId `
    -OutputPath $FrontendPath `
    -Configuration $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "C# frontend failed with exit code $LASTEXITCODE."
}

& $SemanticScript `
    -DotNetPath $DotNetPath `
    -SourcePath $SourcePath `
    -SourceId $SourceId `
    -FrontendPath $FrontendPath `
    -OutputPath $SemanticPath `
    -ExecutableReferenceSourcePath $BindingPackage.ReferenceSourcePath `
    -Configuration $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "C# semantic projection failed with exit code $LASTEXITCODE."
}

$Semantic = Get-Content -Raw -LiteralPath $SemanticPath | ConvertFrom-Json
if ([int]$Semantic.schema_version -ne 18 -or
    [string]$Semantic.semantic_version -cne "1.20" -or
    -not [bool]$Semantic.succeeded -or
    @($Semantic.ue_type_declarations).Count -eq 0) {
    throw "Semantic artifact must be a successful schema 18/1.20 document with UE type declarations."
}

$ToolHome = Join-Path $env:TEMP "AvidScriptUeTypeGenerator"
$AppData = Join-Path $ToolHome "AppData"
$LocalAppData = Join-Path $ToolHome "LocalAppData"
$NuGetPackages = Join-Path $ToolHome "Packages"
$NuGetDirectory = Join-Path $AppData "NuGet"
$NuGetConfig = Join-Path $NuGetDirectory "NuGet.Config"
foreach ($Directory in @($NuGetDirectory, $LocalAppData, $NuGetPackages)) {
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
}
[System.IO.File]::WriteAllText(
    $NuGetConfig,
    "<?xml version=`"1.0`" encoding=`"utf-8`"?><configuration><packageSources><clear /></packageSources></configuration>",
    $Utf8)
$env:DOTNET_CLI_HOME = $ToolHome
$env:APPDATA = $AppData
$env:LOCALAPPDATA = $LocalAppData
$env:NUGET_PACKAGES = $NuGetPackages
$env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"

& $DotNetPath build $GeneratorProject -c $Configuration --nologo --verbosity quiet "-p:RestoreConfigFile=$NuGetConfig"
if ($LASTEXITCODE -ne 0) {
    throw "UE type generator build failed with exit code $LASTEXITCODE."
}
$GeneratorDll = Join-Path $PluginRoot "Tools\AvidScript.UeTypeGenerator\bin\$Configuration\net8.0\AvidScript.UeTypeGenerator.dll"
if (-not (Test-Path -LiteralPath $GeneratorDll -PathType Leaf)) {
    throw "UE type generator assembly is missing after build: $GeneratorDll"
}

& $DotNetPath $GeneratorDll `
    --semantic $SemanticPath `
    --output $OutputRoot `
    --module $ModuleName `
    --ue-version $UnrealVersion
if ($LASTEXITCODE -ne 0) {
    throw "UE type generator failed with exit code $LASTEXITCODE."
}

$GeneratedManifestPath = Join-Path $OutputRoot "AvidScriptGeneratedManifest.json"
$GeneratedManifest = Get-Content -Raw -LiteralPath $GeneratedManifestPath | ConvertFrom-Json
$SemanticSha256 = (Get-FileHash -LiteralPath $SemanticPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ([int]$GeneratedManifest.schema_version -ne 1 -or
    [string]$GeneratedManifest.generator_version -cne "1.0" -or
    [string]$GeneratedManifest.semantic_artifact_sha256 -cne $SemanticSha256 -or
    [string]$GeneratedManifest.module_name -cne $ModuleName -or
    [string]$GeneratedManifest.unreal_version -cne $UnrealVersion -or
    @($GeneratedManifest.types).Count -ne @($Semantic.ue_type_declarations).Count) {
    throw "Generated UE type manifest identity does not match the validated semantic artifact."
}
foreach ($Output in @($GeneratedManifest.outputs)) {
    $OutputPath = [System.IO.Path]::GetFullPath((Join-Path $OutputRoot ([string]$Output.relative_path)))
    if (-not $OutputPath.StartsWith(
            $OutputRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $OutputPath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $OutputPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne [string]$Output.sha256) {
        throw "Generated UE type output hash mismatch: $($Output.relative_path)"
    }
}

Write-Host "AvidScript C# script type generation succeeded."
Write-Host "  source_id=$SourceId"
Write-Host "  binding_package=$($BindingPackage.PackageHash)"
Write-Host "  semantic=$SemanticPath"
Write-Host "  ue_types=$(@($GeneratedManifest.types).Count)"
Write-Host "  generation_key=$($GeneratedManifest.generation_key_sha256)"
Write-Host "  output=$OutputRoot"
