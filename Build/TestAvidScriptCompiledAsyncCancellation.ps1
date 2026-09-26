[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [Parameter(Mandatory = $true)][string]$BindingPackagePath,
    # Only after a successful no-clean build of the current native sources.
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$projectPath = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$runRoot = Join-Path $projectRoot "Saved/AvidScriptCompiledCancellation/$runId"
$outputRoot = Join-Path $runRoot 'FormalBuild'
$sourcePath = Join-Path $runRoot 'AsyncCancellationFlow.cs'
$package = (Resolve-Path -LiteralPath $BindingPackagePath -ErrorAction Stop).Path
$logPath = Join-Path $projectRoot "Saved/Logs/AvidScript_CompiledAsyncCancellation_$runId.log"
$testName = 'AvidScript.Runtime.Continuation.CompiledAsyncCancellation'
$priorCliHome = $env:DOTNET_CLI_HOME
$priorOutput = $env:AVIDSCRIPT_CSHARP_CANCELLATION_WASM_DIR

function Invoke-FormalBuild {
    param([ValidateSet('legacy', 'typed', 'default')][string]$Mode, [string]$Name)
    $source = $sourcePath
    $project = Join-Path $pluginRoot 'Fixtures/Phase66/AsyncCancellationFlow.csproj'
    if ($Mode -ceq 'legacy') {
        $source = Join-Path $pluginRoot 'Fixtures/Phase66/DirectAwaitCleanup.cs'
        $project = Join-Path $pluginRoot 'Fixtures/Phase66/DirectAwaitCleanup.csproj'
    }
    $arguments = @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'BuildCSharpActorLifecycle.ps1'),
        '-DotNetPath', $DotNetPath, '-SourcePath', $source, '-ProjectPath', $project,
        '-ProjectRoot', $projectRoot, '-OutputRoot', $outputRoot,
        '-BindingPackagePath', $package, '-ArtifactStem', 'cancellation')
    if ($Mode -cne 'default') {
        # Leave the worker at its default: bounded mode must disable it itself.
        $arguments += @('-LanguageErrors', 'bounded', '-AsyncExceptionFlow', '-DirectAwaitCleanup')
    } else {
        $arguments += @('-CompilerWorkerMode', 'disabled')
    }
    if ($Mode -ceq 'typed') { $arguments += '-AsyncCancellationFlow' }
    & (Join-Path $PSHOME 'pwsh.exe') @arguments *> (Join-Path $runRoot "$Name.log")
    $exitCode = $LASTEXITCODE
    $reportPath = Join-Path $outputRoot 'cancellation.csharp.report.json'
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) { throw "Build report missing: $Name in $runRoot" }
    Copy-Item -LiteralPath $reportPath -Destination (Join-Path $runRoot "$Name.report.json")
    return [pscustomobject]@{
        ExitCode = $exitCode
        Report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    }
}

function Assert-FormalArtifact {
    param([Parameter(Mandatory = $true)]$Build, [bool]$Typed)
    $report = $Build.Report
    $semanticSchema = if ($Typed) { 44 } else { 43 }
    $semanticVersion = if ($Typed) { '1.53' } else { '1.52' }
    $irSchema = if ($Typed) { 24 } else { 23 }
    $irVersion = if ($Typed) { '1.23' } else { '1.22' }
    foreach ($extension in @('wasm', 'avidscript.json', 'guestir.json', 'state.json', 'csharp.semantic.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $outputRoot "cancellation.$extension") -PathType Leaf)) {
            throw "Formal artifact missing: $extension in $runRoot"
        }
    }
    $ir = Get-Content -LiteralPath (Join-Path $outputRoot 'cancellation.guestir.json') -Raw | ConvertFrom-Json
    $manifest = Get-Content -LiteralPath (Join-Path $outputRoot 'cancellation.avidscript.json') -Raw | ConvertFrom-Json
    if ($Build.ExitCode -ne 0 -or $report.result -cne 'direct_abi_built' -or -not $report.succeeded -or
        ($Typed -and -not $report.semantic.succeeded) -or $report.semantic.schema_version -ne $semanticSchema -or
        $report.semantic.version -cne $semanticVersion -or $ir.schema_version -ne $irSchema -or
        $ir.ir_version -cne $irVersion -or -not $ir.succeeded -or
        $report.compilation.language_errors -cne 'bounded' -or
        -not $report.compilation.async_exception_flow -or -not $report.compilation.direct_await_cleanup -or
        $report.compilation.async_cancellation_flow -ne $Typed -or
        $null -eq $ir.language_error_catalog -or @($ir.direct_await_routes).Count -eq 0 -or
        $report.compiler_worker.mode -cne 'disabled' -or $report.compiler_worker.used -or
        $report.compiler_worker.request_count -ne 0) {
        throw "Incorrect formal preview contract: typed=$Typed in $runRoot"
    }
    foreach ($cache in @($report.semantic_cache, $report.compilation_cache)) {
        if ($cache.enabled -or $cache.lookup -cne 'disabled' -or $cache.published -or $cache.key -cne '') {
            throw "Preview reused or published cached artifacts: $runRoot"
        }
    }
    foreach ($stage in @('frontend', 'semantic', 'guest_ir', 'wasm_backend')) {
        if ($report.tool_invocations.$stage -ne 1) { throw "Expected one real $stage invocation: $runRoot" }
    }
    foreach ($reuse in @('frontend_reused', 'semantic_reused', 'guest_ir_reused', 'wasm_reused')) {
        if ($report.build_reuse.$reuse) { throw "Unexpected prepared artifact reuse: $reuse" }
    }
    $reserved = @($ir.imports | Where-Object name -CIn @('avid_task_cancel_language_error_v1',
        'avid_task_terminal_error_meta_v1', 'avid_task_terminal_error_root_v1'))
    if (($Typed -and ($reserved.Count -ne 3 -or @($ir.async_exception_transfers).Count -eq 0)) -or
        (-not $Typed -and $reserved.Count -ne 0)) { throw "Incorrect cancellation imports or transfers: $runRoot" }
    $wasmHash = (Get-FileHash -LiteralPath (Join-Path $outputRoot 'cancellation.wasm') -Algorithm SHA256).Hash.ToLowerInvariant()
    $irHash = (Get-FileHash -LiteralPath (Join-Path $outputRoot 'cancellation.guestir.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    $semanticHash = (Get-FileHash -LiteralPath (Join-Path $outputRoot 'cancellation.csharp.semantic.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($manifest.module_id -cne $ir.module_id -or $manifest.module_id -cne $report.module_id -or
        $manifest.wasm.sha256 -cne $wasmHash -or $manifest.guest_ir.sha256 -cne $irHash -or
        $manifest.source.semantic_sha256 -cne $semanticHash -or $ir.provenance.semantic_sha256 -cne $semanticHash) {
        throw "Formal manifest identity does not match executed bytes: $runRoot"
    }
    return $wasmHash
}

Push-Location $pluginRoot
try {
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got '$sdk'." }
    $env:DOTNET_CLI_HOME = Join-Path $env:LOCALAPPDATA 'Temp/AvidScriptPhase66ReferenceCliHome'
    # Focused compiler checks must not emit their internal-API fixture over the
    # formal CLI artifacts consumed by native Automation below.
    $env:AVIDSCRIPT_CSHARP_CANCELLATION_WASM_DIR = $null
    [void][IO.Directory]::CreateDirectory($runRoot)
    $sharedSource = [IO.File]::ReadAllText((Join-Path $pluginRoot 'Fixtures/Phase66/AsyncCancellationFlow.cs'))
    $adapter = [IO.File]::ReadAllText((Join-Path $pluginRoot 'Fixtures/Phase66/AsyncCancellationFlow.Guest.cs'))
    [IO.File]::WriteAllText($sourcePath, $sharedSource + "`n" + $adapter, [Text.UTF8Encoding]::new($false))
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
    $legacy = Invoke-FormalBuild -Mode legacy -Name 'legacy_cleanup'
    $legacyHash = Assert-FormalArtifact -Build $legacy -Typed $false
    & (Join-Path $PSScriptRoot 'Contracts/TestCompilerManagedImportContracts.ps1') `
        -GuestIrPath (Join-Path $outputRoot 'cancellation.guestir.json')
    Write-Output 'PASS formal Semantic 43 / IR 23 compatibility'
    $positive = Invoke-FormalBuild -Mode typed -Name 'typed_cancellation'
    $firstHash = Assert-FormalArtifact -Build $positive -Typed $true
    Write-Output 'PASS formal Semantic 44 / IR 24; caches and worker disabled'
    $negative = Invoke-FormalBuild -Mode default -Name 'default_rejects'
    if ($negative.ExitCode -ne 1 -or $negative.Report.succeeded -or
        $negative.Report.result -cne 'semantic_failed' -or
        @($negative.Report.diagnostics | Where-Object code -CIn @('ASCS3001', 'ASCS3002')).Count -eq 0 -or
        (Test-Path -LiteralPath (Join-Path $outputRoot 'cancellation.wasm')) -or
        (Test-Path -LiteralPath (Join-Path $outputRoot 'cancellation.avidscript.json'))) {
        throw "Default mode did not reject syntax and retire earlier loadable artifacts: $runRoot"
    }
    Write-Output 'PASS default mode rejects protected awaits and removes earlier WASM/manifest'
    $restored = Invoke-FormalBuild -Mode typed -Name 'typed_rebuild'
    $wasmHash = Assert-FormalArtifact -Build $restored -Typed $true
    if ($wasmHash -cne $firstHash) { throw "Repeated formal builds differ: $runRoot" }
    Write-Output 'PASS explicit preview rebuild restores identical WASM'
    & (Join-Path $PSScriptRoot 'Contracts/TestAsyncCancellationImportContracts.ps1') `
        -GuestIrPath (Join-Path $outputRoot 'cancellation.guestir.json')
    $env:AVIDSCRIPT_CSHARP_CANCELLATION_WASM_DIR = $outputRoot
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
    [ordered]@{
        passed = 24
        total = 24
        reference_passed = 12
        import_contracts_passed = 42
        managed_import_contracts_passed = 54
        formal_build_contracts_passed = 4
        semantic_version = '44/1.53'
        guest_ir_version = '24/1.23'
        wasm_sha256 = $wasmHash
        legacy_wasm_sha256 = $legacyHash
        manifest = Join-Path $outputRoot 'cancellation.avidscript.json'
        log = $logPath
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8NoBOM
    Write-Output "Formal C# cancellation: 24/24 passed; .NET=12/12 imports=42/42 build=4/4; wasm_sha256=$wasmHash; evidence=$runRoot; log=$logPath"
}
finally {
    $env:DOTNET_CLI_HOME = $priorCliHome
    $env:AVIDSCRIPT_CSHARP_CANCELLATION_WASM_DIR = $priorOutput
    Pop-Location
}
