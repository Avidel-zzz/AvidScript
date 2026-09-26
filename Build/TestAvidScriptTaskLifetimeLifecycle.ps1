[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BindingPackagePath,
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    # Use only with native test binaries built from the current sources.
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$projectPath = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptTaskLifetimeLifecycle/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$null = New-Item -ItemType Directory -Path $runRoot -Force
$package = (Resolve-Path -LiteralPath $BindingPackagePath).Path
$oldCliHome = $env:DOTNET_CLI_HOME
$oldManifest = $env:AVIDSCRIPT_TASK_LIFETIME_MANIFEST
$oldNextManifest = $env:AVIDSCRIPT_TASK_LIFETIME_NEXT_MANIFEST
$artifacts = @()
Push-Location $pluginRoot
try {
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'AvidScriptTaskLifetimeCliHome'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }
    Write-Output "Task lifetime lifecycle evidence: $runRoot"
    $source = [IO.File]::ReadAllText((Join-Path $pluginRoot 'Fixtures/Phase66/TaskLocalLifetimeLifecycle.cs'))
    $adapter = [IO.File]::ReadAllText((Join-Path $pluginRoot 'Fixtures/Phase66/TaskLocalLifetimeLifecycle.Guest.cs'))
    if ([regex]::Matches($source, [regex]::Escape('const int Seed = 16;')).Count -ne 1) { throw 'Expected one source seed.' }
    foreach ($seed in @(16, 32)) {
        $directory = Join-Path $runRoot "Seed$seed"
        $null = New-Item -ItemType Directory -Path $directory -Force
        $sharedPath = Join-Path $directory 'Shared.cs'
        $sourcePath = Join-Path $directory 'Lifecycle.cs'
        $shared = $source.Replace('const int Seed = 16;', "const int Seed = $seed;")
        [IO.File]::WriteAllText($sharedPath, $shared, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($sourcePath, $shared + "`n" + $adapter, [Text.UTF8Encoding]::new($false))
        [xml]$referenceProject = Get-Content 'Fixtures/Phase66/TaskLocalLifetimeLifecycle.Reference.csproj' -Raw
        $referenceProject.SelectSingleNode('//Compile[@Include="TaskLocalLifetimeLifecycle.cs"]').SetAttribute('Include', $sharedPath)
        $referenceProject.SelectSingleNode('//Compile[@Include="TaskLocalLifetimeLifecycle.Reference.cs"]').SetAttribute(
            'Include', (Join-Path $pluginRoot 'Fixtures/Phase66/TaskLocalLifetimeLifecycle.Reference.cs'))
        $referencePath = Join-Path $directory 'Reference.csproj'
        $referenceProject.Save($referencePath)
        $reference = @(& $DotNetPath run --project $referencePath -c Release -- $seed)
        $referenceExit = $LASTEXITCODE
        $reference | Set-Content (Join-Path $directory 'reference.log') -Encoding utf8NoBOM
        if ($referenceExit -ne 0 -or @($reference | Where-Object { $_ -ceq 'TaskLocalLifetimeLifecycle.Reference: 2/2 passed' }).Count -ne 1) {
            throw "Same-source .NET reference failed: $directory"
        }
        $outputRoot = Join-Path $directory 'FormalBuild'
        & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File Build/BuildCSharpActorLifecycle.ps1 `
            -DotNetPath $DotNetPath -SourcePath $sourcePath -ProjectPath Fixtures/Phase66/TaskLocalLifetimeLifecycle.csproj `
            -ProjectRoot $projectRoot -OutputRoot $outputRoot -BindingPackagePath $package `
            -ArtifactStem task_lifetime_lifecycle -ModuleId avidscript.fixture.task_lifetime_lifecycle `
            -LanguageErrors bounded -AsyncExceptionFlow -DirectAwaitCleanup -AsyncCancellationFlow `
            *> (Join-Path $directory 'build.log')
        if ($LASTEXITCODE -ne 0) { throw "Formal lifecycle build failed: $directory" }
        $prefix = Join-Path $outputRoot 'task_lifetime_lifecycle'
        $report = Get-Content "$prefix.csharp.report.json" -Raw | ConvertFrom-Json
        $manifest = Get-Content "$prefix.avidscript.json" -Raw | ConvertFrom-Json
        $ir = Get-Content "$prefix.guestir.json" -Raw | ConvertFrom-Json
        $wasmHash = (Get-FileHash "$prefix.wasm" -Algorithm SHA256).Hash.ToLowerInvariant()
        $irHash = (Get-FileHash "$prefix.guestir.json" -Algorithm SHA256).Hash.ToLowerInvariant()
        $semanticHash = (Get-FileHash "$prefix.csharp.semantic.json" -Algorithm SHA256).Hash.ToLowerInvariant()
        if (-not $report.succeeded -or $report.result -cne 'direct_abi_built' -or
            $report.semantic.schema_version -ne 45 -or $report.semantic.version -cne '1.54' -or
            $ir.schema_version -ne 25 -or $ir.ir_version -cne '1.24' -or -not $ir.succeeded -or
            $ir.task_local_lifetimes.exception_model -cne 'cancellation' -or
            @($ir.task_local_lifetimes.functions | ForEach-Object { $_.scope_exits }).Count -eq 0 -or
            $report.semantic_cache.enabled -or $report.compilation_cache.enabled -or $report.compiler_worker.used -or
            @($ir.async_exception_transfers).Count -eq 0 -or @($ir.direct_await_routes).Count -eq 0 -or
            $manifest.module_id -cne 'avidscript.fixture.task_lifetime_lifecycle' -or
            $ir.module_id -cne $manifest.module_id -or $report.module_id -cne $manifest.module_id -or
            $manifest.wasm.sha256 -cne $wasmHash -or $manifest.guest_ir.sha256 -cne $irHash -or
            $manifest.source.semantic_sha256 -cne $semanticHash -or $ir.provenance.semantic_sha256 -cne $semanticHash) {
            throw "Task lifetime formal contract failed: $directory"
        }
        $artifacts += [pscustomobject]@{ seed = $seed; manifest = "$prefix.avidscript.json"; wasm_sha256 = $wasmHash }
        Write-Output "PASS formal lifecycle seed=$seed; .NET=2/2; IR=25/1.24; wasm=$wasmHash"
    }
    if ($artifacts[0].wasm_sha256 -ceq $artifacts[1].wasm_sha256) { throw 'Reload must use different executable code.' }
    $env:AVIDSCRIPT_TASK_LIFETIME_MANIFEST = $artifacts[0].manifest
    $env:AVIDSCRIPT_TASK_LIFETIME_NEXT_MANIFEST = $artifacts[1].manifest
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
            "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles -gather
        if ($LASTEXITCODE -ne 0) { throw 'No-clean Win64 Editor build failed.' }
    }
    $testName = 'AvidScript.Runtime.Continuation.TaskLocalLifetimeLifecycle'
    $logPath = Join-Path $runRoot 'automation.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $projectPath -unattended -nop4 -NullRHI -nosplash `
        "-ExecCmds=Automation RunTests $testName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Task lifetime lifecycle Automation failed: $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    $found = [regex]::Matches($log, "Found 1 automation tests based on '$([regex]::Escape($testName))'").Count
    $success = [regex]::Matches($log, 'Test Completed\. Result=\{Success\} Name=\{TaskLocalLifetimeLifecycle\} Path=\{' + [regex]::Escape($testName) + '\}').Count
    $failed = [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count
    $complete = [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
    $cases = @([regex]::Matches($log, 'task lifetime lifecycle backend=[01] stage=(?:-1|[0-2]) mode=(?:-[1-3]|[0-2]) result=\d+ resources=0 runtimes=0').Value | Sort-Object -Unique).Count
    if ($found -ne 1 -or $success -ne 1 -or $failed -ne 0 -or $complete -ne 1 -or $cases -ne 48 -or
        -not $log.Contains('TaskLocalLifetimeLifecycle: 48/48 passed')) {
        throw "Lifecycle evidence incomplete: found=$found success=$success failed=$failed complete=$complete cases=$cases log=$logPath"
    }
    [ordered]@{ passed = 48; total = 48; reference_passed = 4; automation_passed = 1; artifacts = $artifacts; log = $logPath } |
        ConvertTo-Json -Depth 6 | Set-Content (Join-Path $runRoot 'results.json') -Encoding utf8NoBOM
    Write-Output "Task lifetime lifecycle: 48/48 passed; .NET=4/4 Automation=1/1; evidence=$runRoot"
}
finally {
    Pop-Location
    $env:DOTNET_CLI_HOME = $oldCliHome
    $env:AVIDSCRIPT_TASK_LIFETIME_MANIFEST = $oldManifest
    $env:AVIDSCRIPT_TASK_LIFETIME_NEXT_MANIFEST = $oldNextManifest
}
