param(
    [switch]$RuntimeAutomation,
    [string]$EngineRoot = 'C:\UnrealEngine'
)
$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$TestSource = Join-Path $PluginRoot 'Tools/AvidScript.ManagedHeap.Tests'
$BuildRoot = Join-Path $PluginRoot 'Saved/ManagedHeapTests/Win64'
& cmake -S $TestSource -B $BuildRoot -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Managed heap CMake configuration failed.' }
& cmake --build $BuildRoot --config Release
if ($LASTEXITCODE -ne 0) { throw 'Managed heap native compilation failed.' }
& (Join-Path $BuildRoot 'Release/AvidScriptManagedHeapTests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Managed heap execution tests failed.' }

if ($RuntimeAutomation) {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
    $ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
    $EditorExe = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
    $RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $LogPath = Join-Path $ProjectRoot "Saved/Logs/AvidScript_ManagedHeap_$RunId.log"
    $TestName = 'AvidScript.Runtime.ManagedHeap'
    & $EditorExe $ProjectPath -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $TestName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$LogPath"
    if ($LASTEXITCODE -ne 0) { throw "Managed heap Automation exited with $LASTEXITCODE. Log: $LogPath" }
    $Log = Get-Content -Raw -LiteralPath $LogPath
    $Found = [regex]::Matches($Log, "Found 2 automation tests based on '$([regex]::Escape($TestName))'").Count
    $Success = [regex]::Matches($Log, 'Test Completed\. Result=\{Success\} Name=\{(Ownership|HostAbi)\} Path=\{AvidScript\.Runtime\.ManagedHeap\.\1\}').Count
    $Failed = [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count
    $Complete = [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
    $Exit = [regex]::Matches($Log, 'RequestExitWithStatus\(1, 0,').Count
    if ($Found -ne 1 -or $Success -ne 2 -or $Failed -ne 0 -or $Complete -ne 1 -or $Exit -lt 1) {
        throw "Managed heap Automation evidence incomplete: found=$Found passed=$Success failed=$Failed complete=$Complete exit=$Exit log=$LogPath"
    }
    Write-Output "AvidScript.ManagedHeap.RuntimeAutomation: 2/2 passed; log=$LogPath"
}
