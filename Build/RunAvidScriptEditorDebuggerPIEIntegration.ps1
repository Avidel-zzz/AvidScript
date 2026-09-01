param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$ProjectPath = '',
    [string]$DotNetPath = '',
    [string]$OutputRoot = '',
    [string]$LogPath = ''
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
    $ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $ProjectRoot 'Saved\AvidScriptCSharpGuest\DebuggerPIE'
}
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $LogPath = Join-Path $ProjectRoot "Saved\Logs\AvidScript_EditorDebuggerPIE_$RunId.log"
}

$ProjectPath = [System.IO.Path]::GetFullPath($ProjectPath)
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$LogPath = [System.IO.Path]::GetFullPath($LogPath)
$BuildScript = Join-Path $PSScriptRoot 'BuildCSharpActorLifecycle.ps1'
$EditorExe = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
foreach ($RequiredFile in @($ProjectPath, $BuildScript, $EditorExe)) {
    if (-not [System.IO.File]::Exists($RequiredFile)) {
        throw "Required debugger integration input is missing: $RequiredFile"
    }
}
[void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $LogPath))

$BuildArguments = @{
    Configuration = 'Debug'
    DebugInstrumentation = 'enabled'
    OutputRoot = $OutputRoot
    CompilerWorkerMode = 'auto'
}
if (-not [string]::IsNullOrWhiteSpace($DotNetPath)) {
    $BuildArguments.DotNetPath = $DotNetPath
}
& $BuildScript @BuildArguments
if ($LASTEXITCODE -ne 0) {
    throw "Debug ActorLifecycle build failed with exit code $LASTEXITCODE"
}

$EditorArguments = @(
    $ProjectPath,
    '-unattended',
    '-nop4',
    '-NullRHI',
    '-nosplash',
    '-ExecCmds=Automation RunTests AvidScript.Editor.Debugging.PIEIntegration;Quit',
    '-TestExit=Automation Test Queue Empty',
    "-abslog=$LogPath"
)
& $EditorExe @EditorArguments
if ($LASTEXITCODE -ne 0) {
    throw "Debugger PIE Automation process failed with exit code $LASTEXITCODE. Log: $LogPath"
}

$Log = Get-Content -Raw -LiteralPath $LogPath
$Found = [regex]::Matches(
    $Log,
    "Found 1 automation tests based on 'AvidScript\.Editor\.Debugging\.PIEIntegration'").Count
$Succeeded = [regex]::Matches(
    $Log,
    'Test Completed\. Result=\{Success\} Name=\{PIEIntegration\} Path=\{AvidScript\.Editor\.Debugging\.PIEIntegration\}').Count
$Failed = [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count
$Completed = [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
$RequestedExit = [regex]::Matches($Log, 'RequestExitWithStatus\(1, 0,').Count
if ($Found -ne 1 -or
    $Succeeded -ne 1 -or
    $Failed -ne 0 -or
    $Completed -ne 1 -or
    $RequestedExit -lt 1) {
    throw "Debugger PIE Automation evidence is incomplete. found=$Found success=$Succeeded fail=$Failed complete=$Completed request_exit=$RequestedExit log=$LogPath"
}

[pscustomobject][ordered]@{
    result = 'passed'
    test = 'AvidScript.Editor.Debugging.PIEIntegration'
    found = $Found
    succeeded = $Succeeded
    failed = $Failed
    output_root = $OutputRoot
    log_path = $LogPath
    log_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $LogPath).Hash.ToLowerInvariant()
}
