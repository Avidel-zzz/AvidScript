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
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptAsyncMemberAssignments/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$null = New-Item -ItemType Directory -Path $runRoot -Force
$oldCliHome = $env:DOTNET_CLI_HOME
$oldNuGetPackages = $env:NUGET_PACKAGES
$oldFixtures = $env:AVIDSCRIPT_ASYNC_MEMBER_FIXTURE_DIR
$oldSyncFixtures = $env:AVIDSCRIPT_ASYNC_SYNCHRONOUS_FIXTURE_DIR
Push-Location $pluginRoot
try {
    $env:NUGET_PACKAGES = if ($oldNuGetPackages) { $oldNuGetPackages } else { Join-Path $env:USERPROFILE '.nuget/packages' }
    $env:DOTNET_CLI_HOME = Join-Path $runRoot 'dotnet-home'
    $env:AVIDSCRIPT_ASYNC_MEMBER_FIXTURE_DIR = Join-Path $runRoot 'Fixtures'
    $env:AVIDSCRIPT_ASYNC_SYNCHRONOUS_FIXTURE_DIR = Join-Path $runRoot 'SynchronousFixtures'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }
    $managedResults = @()
    foreach ($suite in @(
        @{ flag = '--await-member-assignments'; name = 'AwaitMemberAssignments'; log = 'managed.log' }
        @{ flag = '--async-synchronous-exceptions'; name = 'AsyncSynchronousExceptions'; log = 'managed-synchronous.log' }
    )) {
        $managed = @(& $DotNetPath run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release -- $suite.flag 2>&1)
        $managedExit = $LASTEXITCODE
        $managed | Set-Content -LiteralPath (Join-Path $runRoot $suite.log) -Encoding utf8
        $pattern = 'AvidScript.CSharpGuest.Tests.' + [regex]::Escape($suite.name) + ': (\d+)/(\d+) passed'
        $match = [regex]::Match(($managed -join "`n"), $pattern)
        if ($managedExit -ne 0 -or -not $match.Success -or [int]$match.Groups[1].Value -le 0 -or
            $match.Groups[1].Value -cne $match.Groups[2].Value) { throw "Fixture generation failed for $($suite.name): $runRoot" }
        $managedResults += [ordered]@{ suite = $suite.name; passed = [int]$match.Groups[1].Value }
        Write-Output "$($suite.name): $($match.Groups[1].Value)/$($match.Groups[2].Value) passed"
    }
    $env:DOTNET_CLI_HOME = $oldCliHome
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
            "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=1 2>&1 |
            Tee-Object -FilePath (Join-Path $runRoot 'build.log')
        if ($LASTEXITCODE -ne 0) { throw 'No-clean Win64 Editor build failed.' }
    }
    $tests = @(
        'AvidScript.Runtime.Continuation.CompiledAsyncMemberAssignments'
        'AvidScript.Runtime.Continuation.CompiledAsyncSynchronousExceptions'
    )
    $filter = $tests -join '+'
    $logPath = Join-Path $runRoot 'automation.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $projectPath `
        -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $filter;Quit" `
        '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Member await Automation failed: $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    $passed = 0
    foreach ($test in $tests) {
        $name = ($test -split '\.')[-1]
        $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + [regex]::Escape($name) +
            '\} Path=\{' + [regex]::Escape($test) + '\}'
        if ([regex]::Matches($log, $pattern).Count -eq 1) { $passed++ }
    }
    $cases = [regex]::Matches($log, 'async-member backend=\d+ scenario=[a-z0-9-]+ mode=\d+ result=-?\d+ trace=-?\d+ resumes=\d+').Count
    $syncCases = [regex]::Matches($log, 'async-synchronous backend=\d+ scenario=[a-z-]+ mode=\d+ result=-?\d+ trace=-?\d+ resumes=\d+').Count
    if ($passed -ne $tests.Count -or $cases -ne 282 -or $syncCases -ne 186 -or
        [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count -ne 0 -or
        [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1 -or
        [regex]::Matches($log, "Found 2 automation tests based on '$([regex]::Escape($filter))'").Count -ne 1) {
        throw "Member await evidence incomplete: tests=$passed cases=$cases synchronous=$syncCases log=$logPath"
    }
    [ordered]@{
        schema_version = 1
        semantic_version = '50/1.59'
        guest_ir_version = '29/1.28'
        managed = $managedResults
        native_tests_passed = $passed
        member_vm_modes_passed = $cases
        synchronous_vm_modes_passed = $syncCases
        collect_while_suspended = $true
        original_c10_completed = $false
        fixtures = @(Get-ChildItem -LiteralPath $env:AVIDSCRIPT_ASYNC_MEMBER_FIXTURE_DIR -Filter '*.wasm' | ForEach-Object {
            [ordered]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
        automation_log = $logPath
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    Write-Output "Member await: native=$passed/2, Wasmtime/WAMR=$cases/282, synchronous=$syncCases/186; evidence=$runRoot"
}
finally {
    Pop-Location
    $env:DOTNET_CLI_HOME = $oldCliHome
    $env:NUGET_PACKAGES = $oldNuGetPackages
    $env:AVIDSCRIPT_ASYNC_MEMBER_FIXTURE_DIR = $oldFixtures
    $env:AVIDSCRIPT_ASYNC_SYNCHRONOUS_FIXTURE_DIR = $oldSyncFixtures
}
