param(
    [Parameter(Mandatory = $true)][string]$DotNetPath,
    [Parameter(Mandatory = $true)][string]$SourcePath,
    [Parameter(Mandatory = $true)][string]$SourceId,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildDir
$FrontendProject = Join-Path $PluginRoot "Tools\AvidScript.CSharpFrontend\AvidScript.CSharpFrontend.csproj"
$ToolHome = Join-Path $PluginRoot "Saved\AvidScriptCSharpFrontendTool"
$AppData = Join-Path $ToolHome "AppData"
$LocalAppData = Join-Path $ToolHome "LocalAppData"
$NuGetPackages = Join-Path $ToolHome "Packages"
$NuGetDirectory = Join-Path $AppData "NuGet"
$NuGetConfig = Join-Path $NuGetDirectory "NuGet.Config"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

foreach ($RequiredFile in @($DotNetPath, $SourcePath, $FrontendProject)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "Required C# frontend file is missing: $RequiredFile"
    }
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

& $DotNetPath build $FrontendProject -c $Configuration --nologo --verbosity quiet "-p:RestoreConfigFile=$NuGetConfig"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$FrontendDll = Join-Path $PluginRoot "Tools\AvidScript.CSharpFrontend\bin\$Configuration\net8.0\AvidScript.CSharpFrontend.dll"
if (-not (Test-Path -LiteralPath $FrontendDll -PathType Leaf)) {
    throw "C# frontend assembly is missing after build: $FrontendDll"
}

& $DotNetPath $FrontendDll --source $SourcePath --source-id $SourceId --output $OutputPath
exit $LASTEXITCODE
