[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$projectPath = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptAsyncSynchronousExceptions/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$null = New-Item -ItemType Directory -Path $runRoot -Force
$oldCliHome = $env:DOTNET_CLI_HOME
$oldNuGetPackages = $env:NUGET_PACKAGES
$oldFixtures = $env:AVIDSCRIPT_ASYNC_SYNCHRONOUS_FIXTURE_DIR
$oldThrowFixtures = $env:AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR
Push-Location $pluginRoot
try {
    # Changing CLI_HOME must not relocate restored package references in UBT.
    $env:NUGET_PACKAGES = if ($oldNuGetPackages) { $oldNuGetPackages } else { Join-Path $env:USERPROFILE '.nuget/packages' }
    $env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
    $env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
    $env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
    $env:DOTNET_NOLOGO = '1'
    $env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
    $env:DOTNET_CLI_HOME = Join-Path $runRoot 'dotnet-home'
    $env:AVIDSCRIPT_ASYNC_SYNCHRONOUS_FIXTURE_DIR = Join-Path $runRoot 'Fixtures'
    $env:AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR = Join-Path $runRoot 'LegacyFixtures'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }
    $managedResults = @()
    foreach ($suite in @(
        @{ flag = '--async-synchronous-exceptions'; name = 'AsyncSynchronousExceptions'; log = 'managed.log' }
        @{ flag = '--async-throw-routing'; name = 'AsyncThrowRouting'; log = 'managed-legacy.log' }
    )) {
        $managed = @(& $DotNetPath run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release -- $suite.flag 2>&1)
        $managedExit = $LASTEXITCODE
        $managed | Set-Content -LiteralPath (Join-Path $runRoot $suite.log) -Encoding utf8
        $pattern = 'AvidScript.CSharpGuest.Tests.' + [regex]::Escape($suite.name) + ': (\d+)/(\d+) passed'
        $match = [regex]::Match(($managed -join "`n"), $pattern)
        if ($managedExit -ne 0 -or -not $match.Success -or [int]$match.Groups[1].Value -le 0 -or
            $match.Groups[1].Value -cne $match.Groups[2].Value) {
            throw "Fixture generation failed for $($suite.name): $runRoot"
        }
        $managedResults += [ordered]@{ suite = $suite.name; passed = [int]$match.Groups[1].Value }
        Write-Output "$($suite.name): $($match.Groups[1].Value)/$($match.Groups[2].Value) passed"
    }
    # UE's bundled SDK and UBT use their existing restore/cache identity.
    # The isolated CLI state above belongs only to the Guest test runner.
    $env:DOTNET_CLI_HOME = $oldCliHome
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
            "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=1 2>&1 |
            Tee-Object -FilePath (Join-Path $runRoot 'build.log')
        if ($LASTEXITCODE -ne 0) { throw 'No-clean Win64 Editor build failed.' }
    }
    $tests = @(
        'AvidScript.Runtime.Continuation.CompiledAsyncSynchronousExceptions'
        'AvidScript.Runtime.Continuation.CompiledAsyncThrowRouting'
        'AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject'
        'AvidScript.Runtime.LanguageErrorCatalog.TaskFaultVmImport'
    )
    $filter = $tests -join '+'
    $logPath = Join-Path $runRoot 'automation.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $projectPath `
        -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $filter;Quit" `
        '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Synchronous async Automation failed: $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    $passed = 0
    foreach ($test in $tests) {
        $name = ($test -split '\.')[-1]
        $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + [regex]::Escape($name) +
            '\} Path=\{' + [regex]::Escape($test) + '\}'
        if ([regex]::Matches($log, $pattern).Count -eq 1) { $passed++ }
    }
    $cases = [regex]::Matches($log, 'async-synchronous backend=\d+ scenario=[a-z-]+ mode=\d+ result=-?\d+ trace=-?\d+ resumes=\d+').Count
    $legacyCases = [regex]::Matches($log, 'async-throw backend=\d+ scenario=[a-z-]+ mode=\d+ result=-?\d+ trace=-?\d+ resumes=\d+').Count
    if ($passed -ne $tests.Count -or $cases -ne 186 -or $legacyCases -ne 162 -or
        [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count -ne 0 -or
        [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1 -or
        [regex]::Matches($log, "Found 4 automation tests based on '$([regex]::Escape($filter))'").Count -ne 1) {
        throw "Synchronous async evidence incomplete: tests=$passed cases=$cases legacy=$legacyCases log=$logPath"
    }
    [ordered]@{
        schema_version = 1
        semantic_version = '50/1.59'
        guest_ir_version = '29/1.28'
        managed = $managedResults
        native_tests_passed = $passed
        vm_scenarios_passed = $cases
        legacy_vm_scenarios_passed = $legacyCases
        fixtures = @(Get-ChildItem -LiteralPath $env:AVIDSCRIPT_ASYNC_SYNCHRONOUS_FIXTURE_DIR -Filter '*.wasm' | ForEach-Object {
            [ordered]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
        automation_log = $logPath
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    Write-Output "Synchronous async exceptions: native=$passed/4, Wasmtime/WAMR=$cases/186, legacy=$legacyCases/162; evidence=$runRoot"
}
finally {
    Pop-Location
    $env:DOTNET_CLI_HOME = $oldCliHome
    $env:NUGET_PACKAGES = $oldNuGetPackages
    $env:AVIDSCRIPT_ASYNC_SYNCHRONOUS_FIXTURE_DIR = $oldFixtures
    $env:AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR = $oldThrowFixtures
}
