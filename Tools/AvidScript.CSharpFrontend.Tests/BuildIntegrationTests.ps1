param(
    [string]$DotNetPath = "C:\Users\user0\.dotnet\dotnet.exe"
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

New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null

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
Assert-Condition ($NormalJson.source.script_type -eq "ActorLifecycleScript") "report does not identify the AST-selected script type"
$ManifestJson = Get-Content -Raw -LiteralPath $NormalManifest | ConvertFrom-Json
Assert-Condition ($ManifestJson.source.sha256 -eq $FrontendJson.source.sha256) "manifest/frontend source hashes differ"
Assert-Condition ($ManifestJson.source.script_type -eq "ActorLifecycleScript") "manifest does not identify the AST-selected script type"
Assert-Condition (-not [string]::IsNullOrWhiteSpace($ManifestJson.source.frontend_file)) "manifest does not reference the frontend artifact"

Write-Output "AvidScript.CSharpFrontend.BuildIntegration: 2/2 passed"
