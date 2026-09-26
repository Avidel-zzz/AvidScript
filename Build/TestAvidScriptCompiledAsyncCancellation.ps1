[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    # Only after a successful no-clean build of the current native sources.
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$projectPath = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$logPath = Join-Path $projectRoot "Saved/Logs/AvidScript_CompiledAsyncCancellation_$runId.log"
$testName = 'AvidScript.Runtime.Continuation.CompiledAsyncCancellation'
$priorCliHome = $env:DOTNET_CLI_HOME
$priorOutput = $env:AVIDSCRIPT_CSHARP_CANCELLATION_WASM_DIR
Push-Location $pluginRoot
try {
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got '$sdk'." }
    $env:DOTNET_CLI_HOME = Join-Path $env:LOCALAPPDATA 'Temp/AvidScriptPhase66ReferenceCliHome'
    $env:AVIDSCRIPT_CSHARP_CANCELLATION_WASM_DIR = Join-Path $projectRoot "Saved/AvidScriptCompiledCancellation/$runId"
    $reference = & $DotNetPath run --project Fixtures/Phase66/AsyncCancellationFlow.Reference.csproj -c Release
    $reference | Write-Output
    if ($LASTEXITCODE -ne 0 -or @($reference | Where-Object { $_ -ceq 'AsyncCancellationFlow.Reference: 12/12 passed' }).Count -ne 1) {
        throw 'The same-source .NET reference failed.'
    }
    $compiler = & $DotNetPath run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release -- --async-cancellation
    $compiler | Write-Output
    if ($LASTEXITCODE -ne 0 -or @($compiler | Where-Object { $_ -match '^AvidScript.CSharpGuest.Tests.AsyncCancellation: (\d+)/\1 passed$' }).Count -ne 1) {
        throw 'The C# cancellation compiler checks failed.'
    }
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development "-Project=$projectPath" `
            -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles
        if ($LASTEXITCODE -ne 0) { throw 'No-clean Win64 Editor build failed.' }
    }
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $projectPath -unattended -nop4 -NullRHI -nosplash `
        "-ExecCmds=Automation RunTests $testName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Compiled cancellation Automation failed: $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    $found = [regex]::Matches($log, "Found 1 automation tests based on '$([regex]::Escape($testName))'").Count
    $success = [regex]::Matches($log, 'Test Completed\. Result=\{Success\} Name=\{CompiledAsyncCancellation\} Path=\{AvidScript\.Runtime\.Continuation\.CompiledAsyncCancellation\}').Count
    $failed = [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count
    $complete = [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
    $cases = [regex]::Matches($log, 'compiled cancellation backend=[01] cancel=[01] case=[0-5] result=\d+ trace=\d+ resumes=\d+ cancelled=\d+').Value | Sort-Object -Unique
    if ($found -ne 1 -or $success -ne 1 -or $failed -ne 0 -or $complete -ne 1 -or @($cases).Count -ne 24 -or
        -not $log.Contains('CompiledAsyncCancellation: 24/24 passed')) {
        throw "Compiled cancellation evidence incomplete: found=$found success=$success failed=$failed complete=$complete cases=$(@($cases).Count) log=$logPath"
    }
    Write-Output "Compiled C# cancellation: 24/24 passed; .NET reference: 12/12; log=$logPath"
}
finally {
    $env:DOTNET_CLI_HOME = $priorCliHome
    $env:AVIDSCRIPT_CSHARP_CANCELLATION_WASM_DIR = $priorOutput
    Pop-Location
}
