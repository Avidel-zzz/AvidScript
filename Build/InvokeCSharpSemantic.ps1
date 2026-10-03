param(
    [Parameter(Mandatory = $true)][string]$DotNetPath,
    [Parameter(Mandatory = $true)][string]$SourcePath,
    [Parameter(Mandatory = $true)][string]$SourceId,
    [Parameter(Mandatory = $true)][string]$FrontendPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$ReferenceSourcePath = "",
    [string]$ExecutableReferenceSourcePath = "",
    [ValidateSet("enabled", "disabled")]
    [string]$AsyncExceptionFlow = "disabled",
    [ValidateSet("enabled", "disabled")]
    [string]$DirectAwaitCleanup = "disabled",
    [ValidateSet("enabled", "disabled")]
    [string]$AsyncCancellationFlow = "disabled",
    [ValidateSet("enabled", "disabled")]
    [string]$AsyncSynchronousExceptions = "disabled",
    [ValidateSet("enabled", "disabled")]
    [string]$StaticInitialization = "disabled",
    [ValidateSet("enabled", "disabled")]
    [string]$AsyncCatchVariables = "disabled",
    [ValidateSet("enabled", "disabled")]
    [string]$CancellationTokens = "disabled",
    [ValidateSet("enabled", "disabled")]
    [string]$AsyncVoidErrorOwner = "disabled",
    [string]$LanguageProfile = "",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
if (-not [string]::IsNullOrWhiteSpace($LanguageProfile)) {
    foreach ($name in @('AsyncExceptionFlow', 'DirectAwaitCleanup', 'AsyncCancellationFlow',
        'AsyncSynchronousExceptions', 'StaticInitialization', 'AsyncCatchVariables', 'CancellationTokens', 'AsyncVoidErrorOwner')) {
        if ($PSBoundParameters.ContainsKey($name)) { throw 'A language profile cannot be combined with explicit semantic analysis options.' }
    }
}
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
if (-not [string]::IsNullOrWhiteSpace($ExecutableReferenceSourcePath) -and
    -not (Test-Path -LiteralPath $ExecutableReferenceSourcePath -PathType Leaf)) {
    throw "C# semantic executable reference source is missing: $ExecutableReferenceSourcePath"
}

New-Item -ItemType Directory -Force -Path $NuGetDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $LocalAppData | Out-Null
New-Item -ItemType Directory -Force -Path $NuGetPackages | Out-Null
[System.IO.File]::WriteAllText(
    $NuGetConfig,
    "<?xml version=`"1.0`" encoding=`"utf-8`"?><configuration><packageSources><clear /></packageSources></configuration>",
    $Utf8)

$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
$env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
$env:DOTNET_NOLOGO = '1'
$env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
$env:DOTNET_CLI_HOME = $ToolHome
$env:APPDATA = $AppData
$env:LOCALAPPDATA = $LocalAppData
$env:NUGET_PACKAGES = $NuGetPackages
$env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"

$ExitCode = 2
Push-Location $PluginRoot
try {
    & $DotNetPath build $SemanticProject -c $Configuration --nologo --verbosity quiet --disable-build-servers -m:1 -nodeReuse:false -p:UseSharedCompilation=false "-p:RestoreConfigFile=$NuGetConfig"
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
        if (-not [string]::IsNullOrWhiteSpace($LanguageProfile)) {
            $SemanticArguments += @('--language-profile', $LanguageProfile)
        }
        if (-not [string]::IsNullOrWhiteSpace($ReferenceSourcePath)) {
            $SemanticArguments += @("--reference-source", $ReferenceSourcePath)
        }
        if (-not [string]::IsNullOrWhiteSpace($ExecutableReferenceSourcePath)) {
            $SemanticArguments += @("--executable-reference-source", $ExecutableReferenceSourcePath)
        }
        if ($AsyncExceptionFlow -ceq "enabled") {
            $SemanticArguments += @("--async-exception-flow", "enabled")
        }
        if ($DirectAwaitCleanup -ceq "enabled") {
            $SemanticArguments += @("--direct-await-cleanup", "enabled")
        }
        if ($AsyncCancellationFlow -ceq "enabled") {
            $SemanticArguments += @("--async-cancellation-flow", "enabled")
        }
        $capabilityOptions = [ordered]@{
            '--async-synchronous-exceptions' = $AsyncSynchronousExceptions
            '--static-initialization' = $StaticInitialization
            '--async-catch-variables' = $AsyncCatchVariables
            '--cancellation-tokens' = $CancellationTokens
            '--async-void-error-owner' = $AsyncVoidErrorOwner
        }
        foreach ($option in $capabilityOptions.GetEnumerator()) {
            if ($option.Value -ceq 'enabled') { $SemanticArguments += @($option.Key, 'enabled') }
        }
        & $DotNetPath $SemanticDll @SemanticArguments
        $ExitCode = $LASTEXITCODE
    }
}
finally {
    Pop-Location
}

exit $ExitCode
