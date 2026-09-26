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
    $GuestDirectory = Join-Path $ProjectRoot 'Saved/AvidScriptManagedHeapTests/GuestFixtures'
    $Dotnet = Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
    $PreviousGuestDirectory = $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR
    $PreviousGenericDirectory = $env:AVIDSCRIPT_GENERIC_METHOD_WASM_DIR
    $PreviousStaticGuardDirectory = $env:AVIDSCRIPT_STATIC_GUARD_WASM_DIR
    $PreviousStaticSourceDirectory = $env:AVIDSCRIPT_STATIC_SOURCE_WASM_DIR
    $PreviousStaticFailureDirectory = $env:AVIDSCRIPT_STATIC_FAILURE_WASM_DIR
    $PreviousCliHome = $env:DOTNET_CLI_HOME
    Push-Location $PluginRoot
    try {
        $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR = $GuestDirectory
        $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'avidscript-managed-heap-dotnet'
        $Sdk = & $Dotnet --version
        if ($LASTEXITCODE -ne 0 -or $Sdk -ne '8.0.416') { throw "Managed heap fixtures require SDK 8.0.416, got $Sdk" }
        & $Dotnet run --project (Join-Path $PluginRoot 'Tools/AvidScript.WasmBackend.Tests') --configuration Release
        if ($LASTEXITCODE -ne 0) { throw 'Managed heap Guest fixture compiler tests failed.' }
        & $Dotnet run --project (Join-Path $PluginRoot 'Tools/AvidScript.CSharpGuest.Tests') --configuration Release -- --closures
        if ($LASTEXITCODE -ne 0) { throw 'CSharp closure fixture compiler tests failed.' }
        $env:AVIDSCRIPT_GENERIC_METHOD_WASM_DIR = $GuestDirectory
        & $Dotnet run --project (Join-Path $PluginRoot 'Tools/AvidScript.CSharpGuest.Tests') --configuration Release -- --generic-methods
        if ($LASTEXITCODE -ne 0) { throw 'CSharp generic member fixture compiler tests failed.' }
        $env:AVIDSCRIPT_STATIC_GUARD_WASM_DIR = $GuestDirectory
        & $Dotnet run --project (Join-Path $PluginRoot 'Tools/AvidScript.CSharpGuest.Tests') --configuration Release -- --static-guards
        if ($LASTEXITCODE -ne 0) { throw 'CSharp static initialization guard compiler tests failed.' }
        $env:AVIDSCRIPT_STATIC_SOURCE_WASM_DIR = $GuestDirectory
        & $Dotnet run --project (Join-Path $PluginRoot 'Tools/AvidScript.CSharpGuest.Tests') --configuration Release -- --static-source
        if ($LASTEXITCODE -ne 0) { throw 'CSharp static source execution compiler tests failed.' }
        $env:AVIDSCRIPT_STATIC_FAILURE_WASM_DIR = $GuestDirectory
        & $Dotnet run --project (Join-Path $PluginRoot 'Tools/AvidScript.CSharpGuest.Tests') --configuration Release -- --static-failures
        if ($LASTEXITCODE -ne 0) { throw 'CSharp static source failure compiler tests failed.' }
    } finally {
        $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR = $PreviousGuestDirectory
        $env:AVIDSCRIPT_GENERIC_METHOD_WASM_DIR = $PreviousGenericDirectory
        $env:AVIDSCRIPT_STATIC_GUARD_WASM_DIR = $PreviousStaticGuardDirectory
        $env:AVIDSCRIPT_STATIC_SOURCE_WASM_DIR = $PreviousStaticSourceDirectory
        $env:AVIDSCRIPT_STATIC_FAILURE_WASM_DIR = $PreviousStaticFailureDirectory
        $env:DOTNET_CLI_HOME = $PreviousCliHome
        Pop-Location
    }
    $EditorExe = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
    $RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $LogPath = Join-Path $ProjectRoot "Saved/Logs/AvidScript_ManagedHeap_$RunId.log"
    $TestName = 'AvidScript.Runtime.ManagedHeap'
    & $EditorExe $ProjectPath -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $TestName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$LogPath"
    if ($LASTEXITCODE -ne 0) { throw "Managed heap Automation exited with $LASTEXITCODE. Log: $LogPath" }
    $Log = Get-Content -Raw -LiteralPath $LogPath
    $Found = [regex]::Matches($Log, "Found 12 automation tests based on '$([regex]::Escape($TestName))'").Count
    $Success = [regex]::Matches($Log, 'Test Completed\. Result=\{Success\} Name=\{(Ownership|HostAbi|StaticStorage|StaticGeneratedGuest|StaticTaskCancellation|StaticInitialization|StaticSourceInitialization|StaticSourceFailures|GeneratedGuest|CSharpClosures|GenericMembers|BorrowedReferences)\} Path=\{AvidScript\.Runtime\.ManagedHeap\.\1\}').Count
    $Failed = [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count
    $Complete = [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
    $Exit = [regex]::Matches($Log, 'RequestExitWithStatus\(1, 0,').Count
    if ($Found -ne 1 -or $Success -ne 12 -or $Failed -ne 0 -or $Complete -ne 1 -or $Exit -lt 1) {
        throw "Managed heap Automation evidence incomplete: found=$Found passed=$Success failed=$Failed complete=$Complete exit=$Exit log=$LogPath"
    }
    Write-Output "AvidScript.ManagedHeap.RuntimeAutomation: 12/12 passed; log=$LogPath"
}
