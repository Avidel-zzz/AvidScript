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
$logPath = Join-Path $projectRoot "Saved/Logs/AvidScript_TaskCancellation_$runId.log"
$testFilter = @(
    'AvidScript.Runtime.Continuation.Task'
    'AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject'
    'AvidScript.Runtime.LanguageErrorCatalog.TaskFaultVmImport'
    'AvidScript.Architecture.VM.EventSubscriptionImportContract'
) -join '+'
$expected = @(
    foreach ($name in @(
        'TaskCancellationAbi', 'TaskCancellationAdmission', 'TaskCancellationImportVersion',
        'TaskLanguageErrorAdmission', 'TaskLanguageError', 'TaskResults', 'TaskResultAbi',
        'TaskEndpoint', 'TaskDispatch', 'TaskProducerBinding', 'TaskContinuationOwnership'
    )) {
        "AvidScript.Runtime.Continuation.$name"
    }
    'AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject'
    'AvidScript.Runtime.LanguageErrorCatalog.TaskFaultVmImport'
    'AvidScript.Architecture.VM.EventSubscriptionImportContract'
)
if (-not $SkipBuild) {
    $build = Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat'
    & $build AvidTPSTemplateEditor Win64 Development "-Project=$projectPath" `
        -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles
    if ($LASTEXITCODE -ne 0) { throw 'No-clean Win64 Editor build failed.' }
}
$editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
& $editor $projectPath -unattended -nop4 -NullRHI -nosplash `
    "-ExecCmds=Automation RunTests $testFilter;Quit" `
    '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
if ($LASTEXITCODE -ne 0) { throw "Task cancellation Automation failed: $logPath" }
$log = Get-Content -LiteralPath $logPath -Raw
$foundPattern = "Found $($expected.Count) automation tests based on '$([regex]::Escape($testFilter))'"
$found = [regex]::Matches($log, $foundPattern).Count
$success = 0
foreach ($test in $expected) {
    $name = ($test -split '\.')[-1]
    $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + [regex]::Escape($name) +
        '\} Path=\{' + [regex]::Escape($test) + '\}'
    if ([regex]::Matches($log, $pattern).Count -eq 1) { $success++ }
}
$failed = [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count
$complete = [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
if ($found -ne 1 -or $success -ne $expected.Count -or $failed -ne 0 -or $complete -ne 1) {
    throw "Task cancellation evidence incomplete: found=$found success=$success failed=$failed complete=$complete log=$logPath"
}
Write-Output "Native Task cancellation: $success/$($expected.Count) passed; Wasmtime/WAMR ABI, admission, ownership and compatibility; C# cancellation lowering still pending; log=$logPath"
