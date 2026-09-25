[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [Parameter(Mandatory = $true)][string]$BindingPackagePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$projectPath = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$outputRoot = Join-Path $projectRoot "Saved/AvidScriptAsyncExceptionFlowTests/$runId/FormalBuild"
$logPath = Join-Path $projectRoot "Saved/Logs/AvidScript_CompiledAsyncExceptionFlow_$runId.log"
$testName = 'AvidScript.Runtime.Continuation.CompiledAsyncExceptionFlow'
$variables = @(
    'AVIDSCRIPT_ASYNC_EXCEPTION_WASM_PATH',
    'AVIDSCRIPT_ASYNC_EXCEPTION_MODULE_ID',
    'AVIDSCRIPT_ASYNC_EXCEPTION_MODE_OFFSET',
    'AVIDSCRIPT_ASYNC_EXCEPTION_RESULT_OFFSET',
    'AVIDSCRIPT_ASYNC_EXCEPTION_CLEANUP_OFFSET'
)
$previous = @{}
foreach ($name in $variables) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

try {
    $package = (Resolve-Path -LiteralPath $BindingPackagePath -ErrorAction Stop).Path
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') {
        throw "Async exception-flow test requires .NET SDK 8.0.416, got '$sdk'"
    }
    Push-Location $pluginRoot
    try {
        & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File 'Build/BuildCSharpActorLifecycle.ps1' `
            -DotNetPath $DotNetPath `
            -SourcePath 'Fixtures/Phase66/AsyncExceptionFlow.cs' `
            -ProjectPath 'Fixtures/Phase66/AsyncExceptionFlow.csproj' `
            -ProjectRoot $projectRoot `
            -OutputRoot $outputRoot `
            -BindingPackagePath $package `
            -ArtifactStem async_exception_flow `
            -LanguageErrors bounded `
            -AsyncExceptionFlow `
            -CompilerWorkerMode disabled
        if ($LASTEXITCODE -ne 0) {
            throw "Formal IR 22 build failed: $outputRoot"
        }
    }
    finally {
        Pop-Location
    }
    $wasmPath = Join-Path $outputRoot 'async_exception_flow.wasm'
    $irPath = Join-Path $outputRoot 'async_exception_flow.guestir.json'
    $manifestPath = Join-Path $outputRoot 'async_exception_flow.avidscript.json'
    $reportPath = Join-Path $outputRoot 'async_exception_flow.csharp.report.json'
    $statePath = Join-Path $outputRoot 'async_exception_flow.state.json'
    foreach ($artifact in @($wasmPath, $irPath, $manifestPath, $reportPath, $statePath)) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Formal IR 22 artifact is missing: $artifact"
        }
    }
    $ir = Get-Content -LiteralPath $irPath -Raw | ConvertFrom-Json
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if ($report.result -cne 'direct_abi_built' -or -not $report.succeeded -or
        @($report.diagnostics | Where-Object { [string]$_.severity -ceq 'error' }).Count -ne 0 -or
        [int]$report.semantic.schema_version -ne 42 -or
        [string]$report.semantic.version -cne '1.51' -or
        [int]$ir.schema_version -ne 22 -or [string]$ir.ir_version -cne '1.21' -or
        @($ir.async_exception_routes).Count -ne 2 -or
        [string]$manifest.module_id -cne [string]$ir.module_id -or
        ([IO.FileInfo]$wasmPath).Length -le 8) {
        throw "Formal async exception-flow contract is invalid: $outputRoot"
    }
    $offsets = @{}
    foreach ($field in @('TestMode', 'Result', 'CleanupCount')) {
        $slot = @($state.slots | Where-Object {
            [string]$_.stable_id -ceq "state:type:global::Script:$field"
        })
        if ($slot.Count -ne 1 -or [int]$slot[0].size -ne 4 -or
            [int]$slot[0].offset -lt 0 -or [int]$slot[0].offset -ge 65536) {
            throw "Formal async exception-flow state slot is invalid: $field"
        }
        $offsets[$field] = [string]$slot[0].offset
    }
    $env:AVIDSCRIPT_ASYNC_EXCEPTION_WASM_PATH = $wasmPath
    $env:AVIDSCRIPT_ASYNC_EXCEPTION_MODULE_ID = [string]$manifest.module_id
    $env:AVIDSCRIPT_ASYNC_EXCEPTION_MODE_OFFSET = $offsets['TestMode']
    $env:AVIDSCRIPT_ASYNC_EXCEPTION_RESULT_OFFSET = $offsets['Result']
    $env:AVIDSCRIPT_ASYNC_EXCEPTION_CLEANUP_OFFSET = $offsets['CleanupCount']

    $build = Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat'
    & $build AvidTPSTemplateEditor Win64 Development "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE
    if ($LASTEXITCODE -ne 0) {
        throw "No-clean Win64 Editor build failed: $LASTEXITCODE"
    }
    $editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
    & $editor $projectPath -unattended -nop4 -NullRHI -nosplash `
        "-ExecCmds=Automation RunTests $testName;Quit" `
        '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) {
        throw "IR 22 Automation exited with $LASTEXITCODE. Log: $logPath"
    }
    $log = Get-Content -LiteralPath $logPath -Raw
    $found = [regex]::Matches($log,
        "Found 1 automation tests based on '$([regex]::Escape($testName))'").Count
    $success = [regex]::Matches($log,
        'Test Completed\. Result=\{Success\} Name=\{CompiledAsyncExceptionFlow\} Path=\{AvidScript\.Runtime\.Continuation\.CompiledAsyncExceptionFlow\}').Count
    $failed = [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count
    $complete = [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
    $exit = [regex]::Matches($log, 'RequestExitWithStatus\(1, 0,').Count
    $scenarios = [regex]::Matches($log,
        'compiled async exception flow backend=[01] mode=[0-5] result=(12|7|0) cleanup=(1|11)').Count
    $isolationMarkers = @(
        foreach ($backend in @(0, 1)) {
            "compiled async exception flow isolation backend=$backend case=cancel result=0 cleanup=1 resumes=2 failures=1"
            "compiled async exception flow isolation backend=$backend case=host_fault result=0 cleanup=0 resumes=2 failures=2"
        }
    )
    $isolations = @($isolationMarkers | Where-Object {
        [regex]::Matches($log, [regex]::Escape($_)).Count -eq 1
    }).Count
    $teardowns = [regex]::Matches($log,
        'compiled async exception flow teardown backend=[01] ready=0 tasks=0').Count
    if ($found -ne 1 -or $success -ne 1 -or $failed -ne 0 -or
        $complete -ne 1 -or $exit -lt 1 -or $scenarios -ne 12 -or
        $isolations -ne 4 -or $teardowns -ne 2) {
        throw "IR 22 Automation evidence incomplete: found=$found success=$success scenarios=$scenarios isolations=$isolations teardowns=$teardowns failed=$failed complete=$complete exit=$exit log=$logPath"
    }
    Write-Output "AvidScript.Runtime.Continuation.CompiledAsyncExceptionFlow: 1/1 passed; Wasmtime/WAMR scenarios=12/12 isolation=4/4 teardown=2/2; log=$logPath"
}
finally {
    foreach ($name in $variables) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
}
