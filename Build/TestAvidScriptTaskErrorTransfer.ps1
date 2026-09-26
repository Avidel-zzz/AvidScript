[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    # Use only after a successful no-clean Editor build of the current sources.
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$projectPath = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$evidenceDirectory = Join-Path $projectRoot "Saved/AvidScriptTaskErrorTransferTests/$runId"
[void](New-Item -ItemType Directory -Path $evidenceDirectory -Force)
$dotnet = Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
$previousEnvironment = @{}
foreach ($name in @('DOTNET_CLI_HOME', 'DOTNET_NOLOGO', 'AVIDSCRIPT_TASK_ERROR_TRANSFER_WASM_DIR', 'AVIDSCRIPT_MANAGED_HEAP_WASM_DIR')) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}
Push-Location $pluginRoot
try {
    $env:DOTNET_CLI_HOME = Join-Path $projectRoot 'Saved/AvidScriptTaskErrorTransferTests/dotnet-home'
    $env:DOTNET_NOLOGO = '1'
    $env:AVIDSCRIPT_TASK_ERROR_TRANSFER_WASM_DIR = Join-Path $projectRoot 'Saved/AvidScriptTaskErrorTransferTests/GuestFixtures'
    $env:AVIDSCRIPT_MANAGED_HEAP_WASM_DIR = Join-Path $projectRoot 'Saved/AvidScriptManagedHeapTests/GuestFixtures'
    $sdk = & $dotnet --version
    if ($LASTEXITCODE -ne 0 -or $sdk -ne '8.0.416') { throw "Transfer tests require SDK 8.0.416, got $sdk" }
    foreach ($runner in @('GuestIr', 'WasmBackend')) {
        $logPath = Join-Path $evidenceDirectory "$runner.log"
        & $dotnet run --project "Tools/AvidScript.$runner.Tests/AvidScript.$runner.Tests.csproj" -c Release *> $logPath
        if ($LASTEXITCODE -ne 0) { throw "Transfer $runner tests failed: $logPath" }
        $text = Get-Content -LiteralPath $logPath -Raw
        $result = [regex]::Match($text, "AvidScript\.$runner\.Tests: ([1-9][0-9]*)/\1 passed")
        if (!$result.Success) { throw "Transfer $runner runner result missing: $logPath" }
        Write-Output $result.Value
    }
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
            "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=1 *> (Join-Path $evidenceDirectory 'ubt.log')
        if ($LASTEXITCODE -ne 0) { throw "No-clean Editor build failed: $evidenceDirectory" }
    }
    $expected = @(
        'AvidScript.Runtime.ManagedHeap.TaskErrorTransfer'
        'AvidScript.Runtime.ManagedHeap.StaticTaskCancellation'
        'AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject'
    )
    $filter = $expected -join '+'
    $nativeLog = Join-Path $evidenceDirectory 'runtime.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $projectPath `
        -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $filter;Quit" `
        '-TestExit=Automation Test Queue Empty' "-abslog=$nativeLog" *> (Join-Path $evidenceDirectory 'process.log')
    if ($LASTEXITCODE -ne 0) { throw "Transfer runtime exited with $LASTEXITCODE`: $nativeLog" }
    $nativeText = Get-Content -LiteralPath $nativeLog -Raw
    if ([regex]::Matches($nativeText, "Found 3 automation tests based on '$([regex]::Escape($filter))'").Count -ne 1 `
        -or [regex]::Matches($nativeText, 'Test Completed\. Result=\{Fail\}').Count -ne 0 `
        -or [regex]::Matches($nativeText, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1) {
        throw "Transfer runtime evidence incomplete: $nativeLog"
    }
    foreach ($test in $expected) {
        $name = ($test -split '\.')[-1]
        $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + [regex]::Escape($name) + '\} Path=\{' + [regex]::Escape($test) + '\}'
        if ([regex]::Matches($nativeText, $pattern).Count -ne 1) { throw "Missing transfer result $test`: $nativeLog" }
    }
    Write-Output "Task error transfer: 3/3 native tests passed; IR/WASM ownership evidence, C# source integration still pending; log=$nativeLog"
} finally {
    foreach ($name in $previousEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name]) }
    Pop-Location
}
