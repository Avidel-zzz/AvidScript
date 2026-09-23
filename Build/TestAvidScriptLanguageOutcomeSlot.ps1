param(
    [string]$EngineRoot = 'C:\UnrealEngine'
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
$GuestDirectory = Join-Path $ProjectRoot 'Saved/AvidScriptOutcomeTests/GuestFixtures'
$DotNet = Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
$PreviousCliHome = $env:DOTNET_CLI_HOME
$PreviousOutputDirectory = $env:AVIDSCRIPT_OUTCOME_SLOT_WASM_DIR
Push-Location $PluginRoot
try {
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'avidscript-outcome-dotnet'
    $env:AVIDSCRIPT_OUTCOME_SLOT_WASM_DIR = $GuestDirectory
    $Sdk = & $DotNet --version
    if ($LASTEXITCODE -ne 0 -or $Sdk -ne '8.0.416') {
        throw "Language outcome fixtures require SDK 8.0.416, got $Sdk"
    }
    & $DotNet run --project 'Tools/AvidScript.WasmBackend.Tests/AvidScript.WasmBackend.Tests.csproj' -c Release
    if ($LASTEXITCODE -ne 0) { throw 'Language outcome Guest IR/WASM fixture failed.' }
    & node 'Tools/AvidScript.WasmBackend.Tests/RunOutcomeSlotWasm.cjs' $GuestDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Language outcome Node WASM execution failed.' }
}
finally {
    $env:DOTNET_CLI_HOME = $PreviousCliHome
    $env:AVIDSCRIPT_OUTCOME_SLOT_WASM_DIR = $PreviousOutputDirectory
    Pop-Location
}

$EditorExe = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$LogPath = Join-Path $ProjectRoot "Saved/Logs/AvidScript_LanguageOutcomeSlot_$RunId.log"
$TestName = 'AvidScript.Runtime.LanguageOutcomeSlot'
& $EditorExe $ProjectPath -unattended -nop4 -NullRHI -nosplash `
    "-ExecCmds=Automation RunTests $TestName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$LogPath"
if ($LASTEXITCODE -ne 0) { throw "Language outcome Automation exited with $LASTEXITCODE. Log: $LogPath" }
$Log = Get-Content -Raw -LiteralPath $LogPath
$Found = [regex]::Matches($Log, "Found 1 automation tests based on '$([regex]::Escape($TestName))'").Count
$Success = [regex]::Matches($Log, 'Test Completed\. Result=\{Success\} Name=\{LanguageOutcomeSlot\} Path=\{AvidScript\.Runtime\.LanguageOutcomeSlot\}').Count
$Failed = [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count
$Complete = [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
$Exit = [regex]::Matches($Log, 'RequestExitWithStatus\(1, 0,').Count
if ($Found -ne 1 -or $Success -ne 1 -or $Failed -ne 0 -or $Complete -ne 1 -or $Exit -lt 1) {
    throw "Language outcome Automation evidence incomplete: found=$Found passed=$Success failed=$Failed complete=$Complete exit=$Exit log=$LogPath"
}
Write-Output "AvidScript.Runtime.LanguageOutcomeSlot: 1/1 passed; log=$LogPath"
