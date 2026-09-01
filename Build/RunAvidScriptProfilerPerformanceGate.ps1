param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$ProjectPath = '',
    [string]$LogPath = ''
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
    $ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
}
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $LogPath = Join-Path $ProjectRoot "Saved\Logs\AvidScript_ProfilerPerformance_$RunId.log"
}

$ProjectPath = [System.IO.Path]::GetFullPath($ProjectPath)
$LogPath = [System.IO.Path]::GetFullPath($LogPath)
$EditorExe = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
foreach ($RequiredFile in @($ProjectPath, $EditorExe)) {
    if (-not [System.IO.File]::Exists($RequiredFile)) {
        throw "Required profiler performance input is missing: $RequiredFile"
    }
}
[void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $LogPath))

$TestName = 'AvidScript.Performance.ProfilerDisabledGate'
$EditorArguments = @(
    $ProjectPath,
    '-unattended',
    '-nop4',
    '-NullRHI',
    '-nosplash',
    "-ExecCmds=Automation RunTests $TestName;Quit",
    '-TestExit=Automation Test Queue Empty',
    "-abslog=$LogPath"
)
& $EditorExe @EditorArguments
if ($LASTEXITCODE -ne 0) {
    throw "Profiler performance Automation failed with exit code $LASTEXITCODE. Log: $LogPath"
}

$Log = Get-Content -Raw -LiteralPath $LogPath
$Found = [regex]::Matches($Log, "Found 1 automation tests based on '$([regex]::Escape($TestName))'").Count
$Succeeded = [regex]::Matches(
    $Log,
    'Test Completed\. Result=\{Success\} Name=\{ProfilerDisabledGate\} Path=\{AvidScript\.Performance\.ProfilerDisabledGate\}').Count
$Failed = [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count
$Completed = [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
$RequestedExit = [regex]::Matches($Log, 'RequestExitWithStatus\(1, 0,').Count
$SummaryMatch = [regex]::Match($Log, 'profiler_overhead_benchmark \|[^\r\n]+budget=pass')
if ($Found -ne 1 -or
    $Succeeded -ne 1 -or
    $Failed -ne 0 -or
    $Completed -ne 1 -or
    $RequestedExit -lt 1 -or
    -not $SummaryMatch.Success) {
    throw "Profiler performance evidence is incomplete. found=$Found success=$Succeeded fail=$Failed complete=$Completed request_exit=$RequestedExit summary=$($SummaryMatch.Success) log=$LogPath"
}

[pscustomobject][ordered]@{
    result = 'passed'
    test = $TestName
    found = $Found
    succeeded = $Succeeded
    failed = $Failed
    summary = $SummaryMatch.Value
    log_path = $LogPath
    log_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $LogPath).Hash.ToLowerInvariant()
}
