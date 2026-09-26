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
$outputRoot = Join-Path $projectRoot "Saved/AvidScriptIntegratedLanguageFlowTests/$runId/FormalBuild"
$negativeRoot = Join-Path $projectRoot "Saved/AvidScriptIntegratedLanguageFlowTests/$runId/DirectAwaitNegative"
$logPath = Join-Path $projectRoot "Saved/Logs/AvidScript_CompiledIntegratedLanguageFlow_$runId.log"
$testName = 'AvidScript.Runtime.Continuation.CompiledIntegratedLanguageFlow'
$variables = @(
    'AVIDSCRIPT_INTEGRATED_WASM_PATH',
    'AVIDSCRIPT_INTEGRATED_MODULE_ID',
    'AVIDSCRIPT_INTEGRATED_MODE_OFFSET',
    'AVIDSCRIPT_INTEGRATED_RESULT_OFFSET',
    'AVIDSCRIPT_INTEGRATED_CLEANUP_OFFSET',
    'DOTNET_CLI_HOME',
    'NUGET_PACKAGES'
)
$previous = @{}
foreach ($name in $variables) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

try {
    $package = (Resolve-Path -LiteralPath $BindingPackagePath -ErrorAction Stop).Path
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') {
        throw "Integrated language-flow test requires .NET SDK 8.0.416, got '$sdk'"
    }
    $env:DOTNET_CLI_HOME = Join-Path $projectRoot 'Saved/AvidScriptIntegratedLanguageFlowTests/DotnetHome'
    Push-Location $pluginRoot
    try {
        $reference = & $DotNetPath run --project `
            'Fixtures/Phase66/IntegratedLanguageFlow.Reference.csproj' -c Release
        if ($LASTEXITCODE -ne 0 -or
            @($reference | Where-Object { $_ -match 'IntegratedLanguageFlow.Reference: 4/4 passed' }).Count -ne 1) {
            throw 'Integrated .NET reference did not complete 4/4 cases.'
        }
        & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File 'Build/BuildCSharpActorLifecycle.ps1' `
            -DotNetPath $DotNetPath `
            -SourcePath 'Fixtures/Phase66/DirectAwaitInTry.cs' `
            -ProjectPath 'Fixtures/Phase66/DirectAwaitInTry.csproj' `
            -ProjectRoot $projectRoot `
            -OutputRoot $negativeRoot `
            -BindingPackagePath $package `
            -ArtifactStem direct_await_in_try `
            -LanguageErrors bounded `
            -AsyncExceptionFlow `
            -CompilerWorkerMode disabled
        $negativeExit = $LASTEXITCODE
        $negativeReportPath = Join-Path $negativeRoot 'direct_await_in_try.csharp.report.json'
        $negativeWasmPath = Join-Path $negativeRoot 'direct_await_in_try.wasm'
        if (-not (Test-Path -LiteralPath $negativeReportPath -PathType Leaf)) {
            throw "Direct-await negative report is missing: $negativeReportPath"
        }
        $negativeReport = Get-Content -LiteralPath $negativeReportPath -Raw | ConvertFrom-Json
        $unsupported = @($negativeReport.diagnostics | Where-Object {
            [string]$_.severity -ceq 'error' -and [string]$_.code -ceq 'ASCS3002'
        })
        if ($negativeExit -ne 1 -or $negativeReport.result -cne 'semantic_failed' -or
            $negativeReport.succeeded -or $unsupported.Count -lt 1 -or
            (Test-Path -LiteralPath $negativeWasmPath)) {
            throw "Direct-await negative contract changed: exit=$negativeExit report=$negativeReportPath"
        }
        & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File 'Build/BuildCSharpActorLifecycle.ps1' `
            -DotNetPath $DotNetPath `
            -SourcePath 'Fixtures/Phase66/IntegratedLanguageFlow.cs' `
            -ProjectPath 'Fixtures/Phase66/IntegratedLanguageFlow.csproj' `
            -ProjectRoot $projectRoot `
            -OutputRoot $outputRoot `
            -BindingPackagePath $package `
            -ArtifactStem integrated_language_flow `
            -LanguageErrors bounded `
            -AsyncExceptionFlow `
            -DirectAwaitCleanup `
            -CompilerWorkerMode disabled
        if ($LASTEXITCODE -ne 0) {
            throw "Formal integrated language-flow build failed: $outputRoot"
        }
    }
    finally {
        Pop-Location
    }
    $stem = 'integrated_language_flow'
    $wasmPath = Join-Path $outputRoot "$stem.wasm"
    $irPath = Join-Path $outputRoot "$stem.guestir.json"
    $manifestPath = Join-Path $outputRoot "$stem.avidscript.json"
    $reportPath = Join-Path $outputRoot "$stem.csharp.report.json"
    $statePath = Join-Path $outputRoot "$stem.state.json"
    foreach ($artifact in @($wasmPath, $irPath, $manifestPath, $reportPath, $statePath)) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Formal integrated language-flow artifact is missing: $artifact"
        }
    }
    $ir = Get-Content -LiteralPath $irPath -Raw | ConvertFrom-Json
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    $arrayImports = @($ir.imports | Where-Object {
        [string]$_.module -ceq 'avidscript' -and
        [string]$_.name -in @('avid_value_array_length', 'avid_value_array_read_range')
    })
    if ($report.result -cne 'direct_abi_built' -or -not $report.succeeded -or
        @($report.diagnostics | Where-Object { [string]$_.severity -ceq 'error' }).Count -ne 0 -or
        [int]$report.semantic.schema_version -ne 43 -or
        [string]$report.semantic.version -cne '1.52' -or
        [int]$ir.schema_version -ne 23 -or [string]$ir.ir_version -cne '1.22' -or
        @($ir.async_exception_routes).Count -ne 1 -or
        @($ir.direct_await_routes).Count -ne 1 -or
        $arrayImports.Count -ne 2 -or
        [string]$manifest.module_id -cne [string]$ir.module_id -or
        ([IO.FileInfo]$wasmPath).Length -le 8) {
        throw "Formal integrated language-flow contract is invalid: $outputRoot"
    }
    $offsets = @{}
    foreach ($field in @('TestMode', 'Result', 'CleanupCount')) {
        $slot = @($state.slots | Where-Object {
            [string]$_.stable_id -ceq "state:type:global::Script:$field"
        })
        if ($slot.Count -ne 1 -or [int]$slot[0].size -ne 4 -or
            [int]$slot[0].offset -lt 0 -or [int]$slot[0].offset -ge 65536) {
            throw "Formal integrated language-flow state slot is invalid: $field"
        }
        $offsets[$field] = [string]$slot[0].offset
    }
    $env:AVIDSCRIPT_INTEGRATED_WASM_PATH = $wasmPath
    $env:AVIDSCRIPT_INTEGRATED_MODULE_ID = [string]$manifest.module_id
    $env:AVIDSCRIPT_INTEGRATED_MODE_OFFSET = $offsets['TestMode']
    $env:AVIDSCRIPT_INTEGRATED_RESULT_OFFSET = $offsets['Result']
    $env:AVIDSCRIPT_INTEGRATED_CLEANUP_OFFSET = $offsets['CleanupCount']
    [Environment]::SetEnvironmentVariable('DOTNET_CLI_HOME', $previous['DOTNET_CLI_HOME'], 'Process')
    $env:NUGET_PACKAGES = Join-Path $env:USERPROFILE '.nuget/packages'

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
        throw "Integrated Automation exited with $LASTEXITCODE. Log: $logPath"
    }
    $log = Get-Content -LiteralPath $logPath -Raw
    $found = [regex]::Matches($log,
        "Found 1 automation tests based on '$([regex]::Escape($testName))'").Count
    $success = [regex]::Matches($log,
        'Test Completed\. Result=\{Success\} Name=\{CompiledIntegratedLanguageFlow\} Path=\{AvidScript\.Runtime\.Continuation\.CompiledIntegratedLanguageFlow\}').Count
    $failed = [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count
    $complete = [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
    $exit = [regex]::Matches($log, 'RequestExitWithStatus\(1, 0,').Count
    $markers = @(
        foreach ($backend in @(0, 1)) {
            "compiled integrated language flow backend=$backend mode=0 result=16 cleanup=1 resumes=4"
            "compiled integrated language flow backend=$backend mode=1 result=17 cleanup=1 resumes=4"
            "compiled integrated language flow backend=$backend mode=2 result=17 cleanup=1 resumes=4"
            "compiled integrated language flow backend=$backend mode=4 result=0 cleanup=1 resumes=4"
            $category = if ($backend -eq 1) { 'guest_trap' } else { 'trap' }
            "compiled continuation status guard backend=$backend category=$category result=0 cleanup=0"
        }
    )
    $scenarios = @($markers | Where-Object {
        [regex]::Matches($log, [regex]::Escape($_)).Count -eq 1
    }).Count
    if ($found -ne 1 -or $success -ne 1 -or $failed -ne 0 -or
        $complete -ne 1 -or $exit -lt 1 -or $scenarios -ne 10) {
        throw "Integrated Automation evidence incomplete: found=$found success=$success scenarios=$scenarios failed=$failed complete=$complete exit=$exit log=$logPath"
    }
    Write-Output "AvidScript.Runtime.Continuation.CompiledIntegratedLanguageFlow: 1/1 passed; .NET=4/4 negative=ASCS3002 direct-await=1 Wasmtime/WAMR=8/8 status-guard=2/2; log=$logPath"
}
finally {
    foreach ($name in $variables) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
}
