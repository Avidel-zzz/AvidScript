param(
    [Parameter(Mandatory = $true)][string]$DotNetPath,
    [Parameter(Mandatory = $true)][string]$SourcePath,
    [Parameter(Mandatory = $true)][string]$SourceId,
    [Parameter(Mandatory = $true)][string]$FrontendPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$ReferenceSourcePath = "",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildDir
$SemanticProject = Join-Path $PluginRoot "Tools\AvidScript.CSharpSemantic\AvidScript.CSharpSemantic.csproj"
$ToolHome = Join-Path $PluginRoot "Saved\AvidScriptCSharpSemanticTool"
$AppData = Join-Path $ToolHome "AppData"
$LocalAppData = Join-Path $ToolHome "LocalAppData"
$NuGetPackages = Join-Path $ToolHome "Packages"
$NuGetDirectory = Join-Path $AppData "NuGet"
$NuGetConfig = Join-Path $NuGetDirectory "NuGet.Config"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

foreach ($RequiredFile in @($DotNetPath, $SourcePath, $FrontendPath, $SemanticProject)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "Required C# semantic file is missing: $RequiredFile"
    }
}
if (-not [string]::IsNullOrWhiteSpace($ReferenceSourcePath) -and
    -not (Test-Path -LiteralPath $ReferenceSourcePath -PathType Leaf)) {
    throw "C# semantic reference source is missing: $ReferenceSourcePath"
}

New-Item -ItemType Directory -Force -Path $NuGetDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $LocalAppData | Out-Null
New-Item -ItemType Directory -Force -Path $NuGetPackages | Out-Null
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

$ExitCode = 2
Push-Location $PluginRoot
try {
    & $DotNetPath build $SemanticProject -c $Configuration --nologo --verbosity quiet "-p:RestoreConfigFile=$NuGetConfig"
    $BuildExitCode = $LASTEXITCODE
    if ($BuildExitCode -ne 0) {
        $ExitCode = $BuildExitCode
    }
    else {
        $SemanticDll = Join-Path $PluginRoot "Tools\AvidScript.CSharpSemantic\bin\$Configuration\net8.0\AvidScript.CSharpSemantic.dll"
        if (-not (Test-Path -LiteralPath $SemanticDll -PathType Leaf)) {
            throw "C# semantic assembly is missing after build: $SemanticDll"
        }

        $SemanticArguments = @(
            "--source", $SourcePath,
            "--source-id", $SourceId,
            "--frontend", $FrontendPath,
            "--output", $OutputPath
        )
        if (-not [string]::IsNullOrWhiteSpace($ReferenceSourcePath)) {
            $SemanticArguments += @("--reference-source", $ReferenceSourcePath)
        }
        & $DotNetPath $SemanticDll @SemanticArguments
        $ExitCode = $LASTEXITCODE
    }
}
finally {
    Pop-Location
}

exit $ExitCode
