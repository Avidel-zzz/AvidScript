param([string]$EngineRoot = 'C:\UnrealEngine')
$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
$PreviousDirectory = $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR
$PreviousCliHome = $env:DOTNET_CLI_HOME
Push-Location $PluginRoot
try {
    $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR = Join-Path $ProjectRoot 'Saved/AvidScriptManagedHeapTests/GuestFixtures'
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'AvidScript-P66-DotNet'
    $Dotnet = Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
    $Sdk = & $Dotnet --version
    if ($LASTEXITCODE -ne 0 -or $Sdk -ne '8.0.416') { throw "Receiver fixtures require SDK 8.0.416, got $Sdk" }
    $CompilerOutput = & $Dotnet run --project Tools/AvidScript.CSharpGuest.Tests --configuration Release -- --ue-receivers
    $CompilerExit = $LASTEXITCODE
    $CompilerOutput | Write-Output
    if ($CompilerExit -ne 0 -or ($CompilerOutput -join "`n") -notmatch 'AvidScript.CSharpGuest.Tests.UeReceivers: 32/32 passed') {
        throw 'CSharp UE receiver compiler fixtures did not pass completely.'
    }
    $DispatchOutput = & $Dotnet run --project Tools/AvidScript.CSharpGuest.Tests --configuration Release -- --ue-dispatch
    $DispatchExit = $LASTEXITCODE
    $DispatchOutput | Write-Output
    if ($DispatchExit -ne 0 -or ($DispatchOutput -join "`n") -notmatch 'AvidScript.CSharpGuest.Tests.UeDispatch: 15/15 passed') {
        throw 'CSharp UE dynamic dispatch compiler fixtures did not pass completely.'
    }
    $DelegateOutput = & $Dotnet run --project Tools/AvidScript.CSharpGuest.Tests --configuration Release -- --ue-delegates
    $DelegateExit = $LASTEXITCODE
    $DelegateOutput | Write-Output
    if ($DelegateExit -ne 0 -or ($DelegateOutput -join "`n") -notmatch 'AvidScript.CSharpGuest.Tests.UeDelegates: 16/16 passed') {
        throw 'CSharp UE delegate binding compiler fixtures did not pass completely.'
    }
} finally {
    $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR = $PreviousDirectory
    $env:DOTNET_CLI_HOME = $PreviousCliHome
    Pop-Location
}
$RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$LogPath = Join-Path $ProjectRoot "Saved/Logs/AvidScript_CSharpUeReceiver_$RunId.log"
$TestName = 'AvidScript.Runtime.GeneratedTypes'
$ExpectedCount = 20
& (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $ProjectPath -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $TestName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$LogPath"
if ($LASTEXITCODE -ne 0) { throw "Generated type Automation exited with $LASTEXITCODE. Log: $LogPath" }
$Log = Get-Content -Raw -LiteralPath $LogPath
$Found = [regex]::Matches($Log, "Found $ExpectedCount automation tests based on '$([regex]::Escape($TestName))'").Count
$Passed = [regex]::Matches($Log, 'Test Completed\. Result=\{Success\} Name=\{([^}]+)\} Path=\{AvidScript\.Runtime\.GeneratedTypes\.\1\}')
$Failed = [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count
$Complete = [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
$Exit = [regex]::Matches($Log, 'RequestExitWithStatus\(1, 0,').Count
if ($Found -ne 1 -or $Passed.Count -ne $ExpectedCount -or @($Passed | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique).Count -ne $ExpectedCount -or $Failed -ne 0 -or $Complete -ne 1 -or $Exit -lt 1) {
    throw "Generated type Automation incomplete: found=$Found passed=$($Passed.Count) failed=$Failed complete=$Complete exit=$Exit log=$LogPath"
}
Write-Output "AvidScript.UeReceivers.RuntimeAutomation: $ExpectedCount/$ExpectedCount passed; log=$LogPath"
