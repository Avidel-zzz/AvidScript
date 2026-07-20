param(
    [Parameter(Mandatory = $true)][string]$DotNetPath,
    [Parameter(Mandatory = $true)][string]$SemanticPath,
    [Parameter(Mandatory = $true)][string]$FrontendArtifactSha256,
    [Parameter(Mandatory = $true)][string]$GuestIrPath,
    [Parameter(Mandatory = $true)][string]$DebugMapPath,
    [Parameter(Mandatory = $true)][string]$StateSchemaPath,
    [Parameter(Mandatory = $true)][string]$WasmPath,
    [Parameter(Mandatory = $true)][string]$InspectionPath,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildDir
$GuestProject = Join-Path $PluginRoot "Tools\AvidScript.CSharpGuest\AvidScript.CSharpGuest.csproj"
$BackendProject = Join-Path $PluginRoot "Tools\AvidScript.WasmBackend\AvidScript.WasmBackend.csproj"
$ToolRoot = Join-Path $PluginRoot "Saved\AvidScriptCSharpGuestCompilerTool"
$CliHome = Join-Path $ToolRoot "Home"
$AppData = Join-Path $ToolRoot "AppData\Roaming"
$LocalAppData = Join-Path $ToolRoot "AppData\Local"
$NuGetPackages = Join-Path $ToolRoot "NuGet\Packages"
$NuGetDirectory = Join-Path $AppData "NuGet"
$NuGetConfig = Join-Path $NuGetDirectory "NuGet.Config"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

foreach ($RequiredFile in @($DotNetPath, $SemanticPath, $GuestProject, $BackendProject)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "Required formal C# guest compiler file is missing: $RequiredFile"
    }
}

foreach ($Directory in @(
    $CliHome,
    $AppData,
    $LocalAppData,
    $NuGetPackages,
    $NuGetDirectory,
    (Split-Path -Parent $GuestIrPath),
    (Split-Path -Parent $DebugMapPath),
    (Split-Path -Parent $StateSchemaPath),
    (Split-Path -Parent $WasmPath))) {
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
}

foreach ($StaleArtifact in @($GuestIrPath, $DebugMapPath, $StateSchemaPath, $WasmPath, $InspectionPath)) {
    if (Test-Path -LiteralPath $StaleArtifact -PathType Leaf) {
        Remove-Item -LiteralPath $StaleArtifact -Force
    }
}

[System.IO.File]::WriteAllText(
    $NuGetConfig,
    "<?xml version=`"1.0`" encoding=`"utf-8`"?><configuration><packageSources><clear /></packageSources></configuration>",
    $Utf8)

$env:DOTNET_CLI_HOME = $CliHome
$env:APPDATA = $AppData
$env:LOCALAPPDATA = $LocalAppData
$env:NUGET_PACKAGES = $NuGetPackages
$env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"

$ExitCode = 2
Push-Location $PluginRoot
try {
    foreach ($Project in @($GuestProject, $BackendProject)) {
        & $DotNetPath build $Project -c $Configuration --nologo --verbosity quiet "-p:RestoreConfigFile=$NuGetConfig"
        if ($LASTEXITCODE -ne 0) {
            $ExitCode = $LASTEXITCODE
            throw "Formal C# guest compiler project build failed: $Project"
        }
    }

    $GuestDll = Join-Path $PluginRoot "Tools\AvidScript.CSharpGuest\bin\$Configuration\net8.0\AvidScript.CSharpGuest.dll"
    $BackendDll = Join-Path $PluginRoot "Tools\AvidScript.WasmBackend\bin\$Configuration\net8.0\AvidScript.WasmBackend.dll"
    foreach ($Assembly in @($GuestDll, $BackendDll)) {
        if (-not (Test-Path -LiteralPath $Assembly -PathType Leaf)) {
            throw "Formal C# guest compiler assembly is missing after build: $Assembly"
        }
    }

    & $DotNetPath $GuestDll `
        --semantic $SemanticPath `
        --output $GuestIrPath `
        --state-schema $StateSchemaPath `
        --debug-map $DebugMapPath `
        --frontend-artifact-sha256 $FrontendArtifactSha256
    if ($LASTEXITCODE -ne 0) {
        $ExitCode = $LASTEXITCODE
        throw "C# semantic to Guest IR lowering failed with exit code $ExitCode."
    }

    & $DotNetPath $BackendDll $GuestIrPath $WasmPath
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0 -and (Test-Path -LiteralPath $WasmPath -PathType Leaf)) {
        Remove-Item -LiteralPath $WasmPath -Force
    }
    if ($ExitCode -eq 0) {
        & $DotNetPath $BackendDll --inspect $WasmPath $InspectionPath
        $ExitCode = $LASTEXITCODE
    }
}
finally {
    Pop-Location
}

if ($ExitCode -eq 0) {
    foreach ($Artifact in @($GuestIrPath, $DebugMapPath, $StateSchemaPath, $WasmPath, $InspectionPath)) {
        if (-not (Test-Path -LiteralPath $Artifact -PathType Leaf)) {
            throw "Formal C# guest compiler did not publish expected artifact: $Artifact"
        }
    }
}

exit $ExitCode
