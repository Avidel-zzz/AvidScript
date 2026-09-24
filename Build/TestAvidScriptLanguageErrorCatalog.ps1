param(
    [string]$EngineRoot = 'C:\UnrealEngine'
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
$FixtureDirectory = Join-Path $ProjectRoot 'Saved/AvidScriptLanguageErrorCatalogTests/GuestFixtures'
$DotNet = Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
$PreviousCliHome = $env:DOTNET_CLI_HOME
$PreviousTelemetry = $env:DOTNET_CLI_TELEMETRY_OPTOUT
$PreviousOutputDirectory = $env:AVIDSCRIPT_THROW_PRODUCER_WASM_DIR
Push-Location $PluginRoot
try {
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'avidscript-language-error-catalog-dotnet'
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $env:AVIDSCRIPT_THROW_PRODUCER_WASM_DIR = $FixtureDirectory
    $Sdk = & $DotNet --version
    if ($LASTEXITCODE -ne 0 -or $Sdk -ne '8.0.416') {
        throw "Language-error catalog fixtures require SDK 8.0.416, got $Sdk"
    }
    & $DotNet run --project 'Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj' -c Release -- --throw-producer
    if ($LASTEXITCODE -ne 0) { throw 'C# language-error fixture generation failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'throw-caller.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# language-error Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'catch-caller.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# handled language-error Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'multi-catch.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# multi-producer catch Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'catch-mismatch.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# unmatched language-error Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'local-catch.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# local-catch Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'multi-local-catch.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# multi-local-catch Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'local-mismatch.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# unmatched local-catch Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'finally-catch.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# finally-catch Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'nested-finally-catch.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# nested-finally-catch Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'throw-finally-catch.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# throw-finally-catch Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'nested-local-throw-finally.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# nested local-throw cleanup Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'multi-local-throw-finally.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# shared local-throw cleanup Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'mixed-local-throw-finally.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# mixed normal/error cleanup Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'mixed-branching-finally.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# mixed branching cleanup Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'called-return-finally.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# called-return cleanup Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'called-branching-finally.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# called-branching cleanup Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'catch-finally.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# catch-finally Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'catch-branching-finally.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# catch-branching-finally Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'cleanup-replaces-error.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# cleanup-replaces-error Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'nested-cleanup-replaces-error.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# nested-cleanup-replaces-error Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'branching-cleanup.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# branching-cleanup Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'catch-rethrow.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# catch-rethrow Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'nested-rethrow.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# nested-rethrow Node WASM probe failed.' }
    & node 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs' (Join-Path $FixtureDirectory 'catch-variable.wasm')
    if ($LASTEXITCODE -ne 0) { throw 'C# catch-variable Node WASM probe failed.' }
}
finally {
    $env:DOTNET_CLI_HOME = $PreviousCliHome
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = $PreviousTelemetry
    $env:AVIDSCRIPT_THROW_PRODUCER_WASM_DIR = $PreviousOutputDirectory
    Pop-Location
}

$EditorExe = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$LogPath = Join-Path $ProjectRoot "Saved/Logs/AvidScript_LanguageErrorCatalog_$RunId.log"
$TestPrefix = 'AvidScript.Runtime.LanguageErrorCatalog'
& $EditorExe $ProjectPath -unattended -nop4 -NullRHI -nosplash `
    "-ExecCmds=Automation RunTests $TestPrefix;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$LogPath"
if ($LASTEXITCODE -ne 0) { throw "Language-error catalog Automation exited with $LASTEXITCODE. Log: $LogPath" }
$Log = Get-Content -Raw -LiteralPath $LogPath
$Found = [regex]::Matches($Log, "Found 3 automation tests based on '$([regex]::Escape($TestPrefix))'").Count
$LoadSuccess = [regex]::Matches($Log,
    'Test Completed\. Result=\{Success\} Name=\{LoadAndReject\} Path=\{AvidScript\.Runtime\.LanguageErrorCatalog\.LoadAndReject\}').Count
$RealSuccess = [regex]::Matches($Log,
    'Test Completed\. Result=\{Success\} Name=\{RealCompilerArtifact\} Path=\{AvidScript\.Runtime\.LanguageErrorCatalog\.RealCompilerArtifact\}').Count
$HandledSuccess = [regex]::Matches($Log,
    'Test Completed\. Result=\{Success\} Name=\{HandledCompilerArtifact\} Path=\{AvidScript\.Runtime\.LanguageErrorCatalog\.HandledCompilerArtifact\}').Count
$Failed = [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count
$Complete = [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
$Exit = [regex]::Matches($Log, 'RequestExitWithStatus\(1, 0,').Count
if ($Found -ne 1 -or $LoadSuccess -ne 1 -or $RealSuccess -ne 1 -or $HandledSuccess -ne 1 -or $Failed -ne 0 -or $Complete -ne 1 -or $Exit -lt 1) {
    throw "Language-error catalog Automation evidence incomplete: found=$Found load=$LoadSuccess real=$RealSuccess handled=$HandledSuccess failed=$Failed complete=$Complete exit=$Exit log=$LogPath"
}
Write-Output "AvidScript.Runtime.LanguageErrorCatalog: 3/3 passed; log=$LogPath"
