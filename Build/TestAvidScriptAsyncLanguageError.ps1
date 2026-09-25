[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [string]$BindingPackagePath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$projectPath = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$fixtureDirectory = Join-Path $projectRoot "Saved/AvidScriptAsyncLanguageErrorTests/$runId/GuestFixtures"
$wasmPath = Join-Path $fixtureDirectory 'csharp-task-language-error.wasm'
$irPath = Join-Path $fixtureDirectory 'csharp-task-language-error.guest-ir.json'
$logPath = Join-Path $projectRoot "Saved/Logs/AvidScript_CompiledTaskLanguageError_$runId.log"
$testName = 'AvidScript.Runtime.Continuation.CompiledTaskLanguageError'
$previousCliHome = $env:DOTNET_CLI_HOME
$previousFixtures = $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_WASM_DIR
$previousWasm = $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_WASM_PATH
$previousModuleId = $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_MODULE_ID

try {
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'avidscript-p66c5-dotnet'
    $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_WASM_DIR = $fixtureDirectory
    $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_WASM_PATH = $null
    $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_MODULE_ID = $null
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') {
        throw "Async language-error fixtures require .NET SDK 8.0.416, got '$sdk'"
    }
    Push-Location $pluginRoot
    try {
        & $DotNetPath run --project 'Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj' --configuration Release -- --async-language-errors
        if ($LASTEXITCODE -ne 0) {
            throw 'C# async language-error fixture generation failed'
        }
    }
    finally {
        Pop-Location
    }
    if (-not (Test-Path -LiteralPath $wasmPath -PathType Leaf) -or
        ([IO.FileInfo]$wasmPath).Length -le 8 -or
        -not (Test-Path -LiteralPath $irPath -PathType Leaf)) {
        throw "Async language-error fixture is incomplete: $fixtureDirectory"
    }
    $ir = Get-Content -LiteralPath $irPath -Raw | ConvertFrom-Json
    if ($ir.schema_version -ne 21 -or $ir.ir_version -cne '1.20' -or
        $ir.provenance.semantic_schema_version -ne 41 -or
        $ir.provenance.semantic_version -cne '1.50' -or
        $null -eq $ir.language_error_catalog) {
        throw "Async language-error fixture has the wrong versioned contract: $irPath"
    }
    $formal = -not [string]::IsNullOrWhiteSpace($BindingPackagePath)
    if ($formal) {
        $package = (Resolve-Path -LiteralPath $BindingPackagePath -ErrorAction Stop).Path
        $formalOutput = Join-Path $projectRoot "Saved/AvidScriptAsyncLanguageErrorTests/$runId/FormalBuild"
        Push-Location $pluginRoot
        try {
            & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File 'Build/BuildCSharpActorLifecycle.ps1' -DotNetPath $DotNetPath -SourcePath 'Fixtures/Phase66/TaskLanguageError.cs' -ProjectPath 'Fixtures/Phase66/TaskLanguageError.csproj' -ProjectRoot $projectRoot -OutputRoot $formalOutput -BindingPackagePath $package -ArtifactStem task_language_error -LanguageErrors bounded -CompilerWorkerMode disabled
            if ($LASTEXITCODE -ne 0) {
                throw "Formal async language-error build failed: $formalOutput"
            }
        }
        finally {
            Pop-Location
        }
        $formalWasm = Join-Path $formalOutput 'task_language_error.wasm'
        $formalIrPath = Join-Path $formalOutput 'task_language_error.guestir.json'
        $manifestPath = Join-Path $formalOutput 'task_language_error.avidscript.json'
        $reportPath = Join-Path $formalOutput 'task_language_error.csharp.report.json'
        foreach ($artifact in @($formalWasm, $formalIrPath, $manifestPath, $reportPath)) {
            if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
                throw "Formal async language-error artifact is missing: $artifact"
            }
        }
        $formalIr = Get-Content -LiteralPath $formalIrPath -Raw | ConvertFrom-Json
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        if ($report.result -cne 'direct_abi_built' -or -not $report.succeeded -or
            $report.semantic.succeeded -or $report.semantic.version -cne '1.50' -or
            $formalIr.schema_version -ne 21 -or $formalIr.ir_version -cne '1.20' -or
            $formalIr.provenance.semantic_schema_version -ne 41 -or
            $manifest.module_id -cne $formalIr.module_id) {
            throw "Formal async language-error build has the wrong contract: $formalOutput"
        }
        $defaultOutput = Join-Path $projectRoot "Saved/AvidScriptAsyncLanguageErrorTests/$runId/DefaultRejects"
        Push-Location $pluginRoot
        try {
            & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File 'Build/BuildCSharpActorLifecycle.ps1' -DotNetPath $DotNetPath -SourcePath 'Fixtures/Phase66/TaskLanguageError.cs' -ProjectPath 'Fixtures/Phase66/TaskLanguageError.csproj' -ProjectRoot $projectRoot -OutputRoot $defaultOutput -BindingPackagePath $package -ArtifactStem task_language_error -CompilerWorkerMode disabled
            $defaultExit = $LASTEXITCODE
        }
        finally {
            Pop-Location
        }
        $defaultReportPath = Join-Path $defaultOutput 'task_language_error.csharp.report.json'
        if (-not (Test-Path -LiteralPath $defaultReportPath -PathType Leaf)) {
            throw "Default async language-error build wrote no report: $defaultOutput"
        }
        $defaultReport = Get-Content -LiteralPath $defaultReportPath -Raw | ConvertFrom-Json
        if ($defaultExit -eq 0 -or $defaultReport.succeeded -or
            $defaultReport.result -cne 'semantic_failed' -or
            (Test-Path -LiteralPath (Join-Path $defaultOutput 'task_language_error.wasm')) -or
            (Test-Path -LiteralPath (Join-Path $defaultOutput 'task_language_error.avidscript.json'))) {
            throw "Default build accepted or published an async language error: $defaultOutput"
        }
        $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_WASM_PATH = $formalWasm
        $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_MODULE_ID = [string]$manifest.module_id
    }

    $build = Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat'
    & $build AvidTPSTemplateEditor Win64 Development "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE
    if ($LASTEXITCODE -ne 0) {
        throw "No-clean Win64 Editor build failed: $LASTEXITCODE"
    }
    $editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
    & $editor $projectPath -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $testName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) {
        throw "Async language-error Automation exited with $LASTEXITCODE. Log: $logPath"
    }
    $log = Get-Content -LiteralPath $logPath -Raw
    $found = [regex]::Matches($log, "Found 1 automation tests based on '$([regex]::Escape($testName))'").Count
    $success = [regex]::Matches($log,
        'Test Completed\. Result=\{Success\} Name=\{CompiledTaskLanguageError\} Path=\{AvidScript\.Runtime\.Continuation\.CompiledTaskLanguageError\}').Count
    $wasmtime = [regex]::Matches($log,
        'compiled Task language error backend=0 producer=1 waiter=1').Count
    $wamr = [regex]::Matches($log,
        'compiled Task language error backend=1 producer=1 waiter=1').Count
    $failed = [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count
    $complete = [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
    $exit = [regex]::Matches($log, 'RequestExitWithStatus\(1, 0,').Count
    if ($found -ne 1 -or $success -ne 1 -or $wasmtime -ne 1 -or $wamr -ne 1 -or
        $failed -ne 0 -or $complete -ne 1 -or $exit -lt 1) {
        throw "Async language-error Automation evidence incomplete: found=$found success=$success wasmtime=$wasmtime wamr=$wamr failed=$failed complete=$complete exit=$exit log=$logPath"
    }
    Write-Output "AvidScript.Runtime.Continuation.CompiledTaskLanguageError: 1/1 passed; Wasmtime/WAMR=2/2; formal=$formal; log=$logPath"
}
finally {
    $env:DOTNET_CLI_HOME = $previousCliHome
    $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_WASM_DIR = $previousFixtures
    $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_WASM_PATH = $previousWasm
    $env:AVIDSCRIPT_ASYNC_LANGUAGE_ERROR_MODULE_ID = $previousModuleId
}
