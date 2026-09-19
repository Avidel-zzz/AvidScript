param([string]$EngineRoot = 'C:\UnrealEngine')
$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
$RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$LogPath = Join-Path $ProjectRoot "Saved/Logs/AvidScript_VmReentry_$RunId.log"
$TestName = 'AvidScript.VM.ReentrantExecution'
& (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $ProjectPath -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $TestName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$LogPath"
if ($LASTEXITCODE -ne 0) { throw "VM reentry Automation exited with $LASTEXITCODE. Log: $LogPath" }
$Log = Get-Content -Raw -LiteralPath $LogPath
$Found = [regex]::Matches($Log, "Found 2 automation tests based on '$([regex]::Escape($TestName))'").Count
$Passed = [regex]::Matches($Log, 'Test Completed\. Result=\{Success\} Name=\{(Wamr|Wasmtime)\} Path=\{AvidScript\.VM\.ReentrantExecution\.\1\}')
$Failed = [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count
$Complete = [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
$Exit = [regex]::Matches($Log, 'RequestExitWithStatus\(1, 0,').Count
if ($Found -ne 1 -or $Passed.Count -ne 2 -or @($Passed | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique).Count -ne 2 -or $Failed -ne 0 -or $Complete -ne 1 -or $Exit -lt 1) {
    throw "VM reentry Automation incomplete: found=$Found passed=$($Passed.Count) failed=$Failed complete=$Complete exit=$Exit log=$LogPath"
}
Write-Output "AvidScript.VmReentry.RuntimeAutomation: 2/2 passed; log=$LogPath"
