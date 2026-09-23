param(
    [string]$EngineRoot = 'C:\UnrealEngine'
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
$GuestDirectory = Join-Path $ProjectRoot 'Saved/AvidScriptEnumeratorTests/GuestFixtures'
$DotNet = Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
$PreviousCliHome = $env:DOTNET_CLI_HOME
Push-Location $PluginRoot
try {
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'avidscript-enumerator-dotnet'
    $Sdk = & $DotNet --version
    if ($LASTEXITCODE -ne 0 -or $Sdk -ne '8.0.416') {
        throw "Enumerator cleanup fixtures require SDK 8.0.416, got $Sdk"
    }
    & pwsh -NoProfile -File (Join-Path $PluginRoot 'Tools/AvidScript.CSharpGuest.Tests/RunEnumeratorComparison.ps1') `
        -DotNetPath $DotNet -OutputDirectory $GuestDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Enumerator cleanup .NET/WASM comparison failed.' }
}
finally {
    $env:DOTNET_CLI_HOME = $PreviousCliHome
    Pop-Location
}

$EditorExe = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$LogPath = Join-Path $ProjectRoot "Saved/Logs/AvidScript_EnumeratorCleanup_$RunId.log"
$TestName = 'AvidScript.Runtime.EnumeratorCleanup'
& $EditorExe $ProjectPath -unattended -nop4 -NullRHI -nosplash `
    "-ExecCmds=Automation RunTests $TestName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$LogPath"
if ($LASTEXITCODE -ne 0) { throw "Enumerator cleanup Automation exited with $LASTEXITCODE. Log: $LogPath" }
$Log = Get-Content -Raw -LiteralPath $LogPath
$Found = [regex]::Matches($Log, "Found 1 automation tests based on '$([regex]::Escape($TestName))'").Count
$Success = [regex]::Matches($Log, 'Test Completed\. Result=\{Success\} Name=\{EnumeratorCleanup\} Path=\{AvidScript\.Runtime\.EnumeratorCleanup\}').Count
$Failed = [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count
$Complete = [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
$Exit = [regex]::Matches($Log, 'RequestExitWithStatus\(1, 0,').Count
if ($Found -ne 1 -or $Success -ne 1 -or $Failed -ne 0 -or $Complete -ne 1 -or $Exit -lt 1) {
    throw "Enumerator cleanup Automation evidence incomplete: found=$Found passed=$Success failed=$Failed complete=$Complete exit=$Exit log=$LogPath"
}
Write-Output "AvidScript.Runtime.EnumeratorCleanup: 1/1 passed; log=$LogPath"
