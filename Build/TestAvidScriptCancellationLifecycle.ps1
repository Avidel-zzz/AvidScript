[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [Parameter(Mandatory = $true)][string]$BindingPackagePath,
    # Only after a successful build of the current native test sources.
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$projectPath = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$runRoot = Join-Path $projectRoot "Saved/AvidScriptTypedCancellation/$runId"
$logPath = Join-Path $projectRoot "Saved/Logs/AvidScript_TypedCancellation_$runId.log"
$package = (Resolve-Path -LiteralPath $BindingPackagePath).Path
$testName = 'AvidScript.Runtime.Continuation.TypedCancellation'
$priorCliHome = $env:DOTNET_CLI_HOME
$priorManifest = $env:AVIDSCRIPT_TYPED_CANCELLATION_MANIFEST
$priorNextManifest = $env:AVIDSCRIPT_TYPED_CANCELLATION_NEXT_MANIFEST
$artifacts = @()

Push-Location $pluginRoot
try {
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got '$sdk'." }
    $env:DOTNET_CLI_HOME = Join-Path $env:LOCALAPPDATA 'Temp/AvidScriptPhase66ReferenceCliHome'
    [void][IO.Directory]::CreateDirectory($runRoot)
    $source = [IO.File]::ReadAllText((Join-Path $pluginRoot 'Fixtures/Phase66/AsyncCancellationFlow.cs'))
    $adapter = [IO.File]::ReadAllText((Join-Path $pluginRoot 'Fixtures/Phase66/AsyncCancellationLifecycle.Guest.cs'))
    if ([regex]::Matches($source, [regex]::Escape('return 16;')).Count -ne 3) {
        throw 'Expected exactly three normal completion values in the shared fixture.'
    }
    foreach ($value in @(16, 32)) {
        $directory = Join-Path $runRoot "Value$value"
        [void][IO.Directory]::CreateDirectory($directory)
        $sharedPath = Join-Path $directory 'Shared.cs'
        $sourcePath = Join-Path $directory 'Lifecycle.cs'
        $shared = $source.Replace('return 16;', "return $value;")
        [IO.File]::WriteAllText($sharedPath, $shared, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($sourcePath, $shared + "`n" + $adapter, [Text.UTF8Encoding]::new($false))
        # The same shared bodies execute in .NET for both code generations.
        [xml]$referenceProject = Get-Content -LiteralPath 'Fixtures/Phase66/AsyncCancellationFlow.Reference.csproj' -Raw
        $referenceProject.SelectSingleNode('//Compile[@Include="AsyncCancellationFlow.cs"]').SetAttribute('Include', $sharedPath)
        $referenceProject.SelectSingleNode('//Compile[@Include="AsyncCancellationFlow.Reference.cs"]').SetAttribute(
            'Include', (Join-Path $pluginRoot 'Fixtures/Phase66/AsyncCancellationFlow.Reference.cs'))
        $referencePath = Join-Path $directory 'Reference.csproj'
        $referenceProject.Save($referencePath)
        $reference = & $DotNetPath run --project $referencePath -c Release -- $value
        $reference | Set-Content -LiteralPath (Join-Path $directory 'reference.log') -Encoding utf8NoBOM
        if ($LASTEXITCODE -ne 0 -or @($reference | Where-Object { $_ -ceq 'AsyncCancellationFlow.Reference: 28/28 passed' }).Count -ne 1) {
            throw "The same-source .NET reference failed for value $value."
        }
        $outputRoot = Join-Path $directory 'FormalBuild'
        & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File Build/BuildCSharpActorLifecycle.ps1 `
            -DotNetPath $DotNetPath -SourcePath $sourcePath -ProjectPath Fixtures/Phase66/AsyncCancellationFlow.csproj `
            -ProjectRoot $projectRoot -OutputRoot $outputRoot -BindingPackagePath $package `
            -ArtifactStem cancellation_lifecycle -ModuleId avidscript.fixture.typed_cancellation_lifecycle `
            -LanguageErrors bounded -AsyncExceptionFlow -DirectAwaitCleanup -AsyncCancellationFlow `
            *> (Join-Path $directory 'build.log')
        if ($LASTEXITCODE -ne 0) { throw "Formal lifecycle build failed: $directory" }
        $prefix = Join-Path $outputRoot 'cancellation_lifecycle'
        $report = Get-Content -LiteralPath "$prefix.csharp.report.json" -Raw | ConvertFrom-Json
        $manifest = Get-Content -LiteralPath "$prefix.avidscript.json" -Raw | ConvertFrom-Json
        $ir = Get-Content -LiteralPath "$prefix.guestir.json" -Raw | ConvertFrom-Json
        $wasmHash = (Get-FileHash -LiteralPath "$prefix.wasm" -Algorithm SHA256).Hash.ToLowerInvariant()
        $irHash = (Get-FileHash -LiteralPath "$prefix.guestir.json" -Algorithm SHA256).Hash.ToLowerInvariant()
        $semanticHash = (Get-FileHash -LiteralPath "$prefix.csharp.semantic.json" -Algorithm SHA256).Hash.ToLowerInvariant()
        if (-not $report.succeeded -or $report.result -cne 'direct_abi_built' -or
            $report.semantic.schema_version -ne 44 -or $report.semantic.version -cne '1.53' -or
            $ir.schema_version -ne 24 -or $ir.ir_version -cne '1.23' -or -not $ir.succeeded -or
            $report.compilation.language_errors -cne 'bounded' -or -not $report.compilation.async_cancellation_flow -or
            $report.compiler_worker.mode -cne 'disabled' -or $report.compiler_worker.used -or
            $report.semantic_cache.enabled -or $report.compilation_cache.enabled -or
            @($ir.async_exception_transfers).Count -eq 0 -or @($ir.direct_await_routes).Count -eq 0 -or
            $manifest.module_id -cne 'avidscript.fixture.typed_cancellation_lifecycle' -or
            $ir.module_id -cne $manifest.module_id -or $report.module_id -cne $manifest.module_id -or
            $manifest.wasm.sha256 -cne $wasmHash -or $manifest.guest_ir.sha256 -cne $irHash -or
            $manifest.source.semantic_sha256 -cne $semanticHash -or $ir.provenance.semantic_sha256 -cne $semanticHash) {
            throw "Formal lifecycle identity/preview contract failed: $directory"
        }
        $imports = & (Join-Path $PSScriptRoot 'Contracts/TestAsyncCancellationImportContracts.ps1') -GuestIrPath "$prefix.guestir.json"
        if ($LASTEXITCODE -ne 0 -or @($imports | Where-Object { $_ -ceq 'AsyncCancellationImportContracts: 42/42 passed' }).Count -ne 1) {
            throw "Typed cancellation import contracts failed for value $value."
        }
        $artifacts += [pscustomobject]@{ value = $value; manifest = "$prefix.avidscript.json"; wasm_sha256 = $wasmHash }
        Write-Output "PASS formal lifecycle value=$value; .NET=28/28 imports=42/42 wasm=$wasmHash"
    }
    if ($artifacts[0].wasm_sha256 -ceq $artifacts[1].wasm_sha256) { throw 'Reload generations must contain different code.' }
    $env:AVIDSCRIPT_TYPED_CANCELLATION_MANIFEST = $artifacts[0].manifest
    $env:AVIDSCRIPT_TYPED_CANCELLATION_NEXT_MANIFEST = $artifacts[1].manifest
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development "-Project=$projectPath" `
            -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles
        if ($LASTEXITCODE -ne 0) { throw 'No-clean Win64 Editor build failed.' }
    }
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $projectPath -unattended -nop4 -NullRHI -nosplash `
        "-ExecCmds=Automation RunTests $testName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Typed cancellation Automation failed: $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    $found = [regex]::Matches($log, "Found 2 automation tests based on '$([regex]::Escape($testName))'").Count
    $success = @('TypedCancellationLifecycle', 'TypedCancellationReload' | Where-Object {
        [regex]::Matches($log, 'Test Completed\. Result=\{Success\} Name=\{' + $_ + '\} Path=\{AvidScript\.Runtime\.Continuation\.' + $_ + '\}').Count -eq 1
    }).Count
    $failed = [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count
    $complete = [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
    $lifecycle = @([regex]::Matches($log, 'typed cancellation lifecycle backend=[01] stage=[0-2] scenario=(?:session|world|object|discard|commit|commit_cancelled) resumes=\d+ cancelled=\d+ resources=0').Value | Sort-Object -Unique).Count
    $reload = @([regex]::Matches($log, 'typed cancellation reload backend=[01] stage=(?:-1|[0-2]) mode=[0-2] committed=[01] result=\d+ resources=0 runtimes=0').Value | Sort-Object -Unique).Count
    if ($found -ne 1 -or $success -ne 2 -or $failed -ne 0 -or $complete -ne 1 -or $lifecycle -ne 36 -or $reload -ne 24 -or
        -not $log.Contains('TypedCancellationLifecycle: 36/36 passed') -or -not $log.Contains('TypedCancellationReload: 24/24 passed')) {
        throw "Typed cancellation evidence incomplete: found=$found success=$success failed=$failed complete=$complete lifecycle=$lifecycle reload=$reload log=$logPath"
    }
    [ordered]@{ passed = 60; total = 60; lifecycle_passed = 36; reload_passed = 24; reference_passed = 56;
        automation_passed = 2; artifacts = $artifacts; log = $logPath } |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8NoBOM
    Write-Output "Typed cancellation lifecycle: 60/60 passed; .NET=56/56 Automation=2/2; evidence=$runRoot; log=$logPath"
}
finally {
    $env:DOTNET_CLI_HOME = $priorCliHome
    $env:AVIDSCRIPT_TYPED_CANCELLATION_MANIFEST = $priorManifest
    $env:AVIDSCRIPT_TYPED_CANCELLATION_NEXT_MANIFEST = $priorNextManifest
    Pop-Location
}
