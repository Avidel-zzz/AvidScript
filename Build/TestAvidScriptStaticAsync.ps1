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
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptStaticAsync/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$null = New-Item -ItemType Directory -Path $runRoot -Force
$oldEnvironment = @{}
foreach ($name in @('DOTNET_CLI_HOME', 'NUGET_PACKAGES', 'AVIDSCRIPT_STATIC_ASYNC_FIXTURE_DIR', 'AVIDSCRIPT_ASYNC_MEMBER_FIXTURE_DIR', 'AVIDSCRIPT_ORIGINAL_ASYNC_MEMBER_DIR')) {
    $oldEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location $pluginRoot
try {
    $env:NUGET_PACKAGES = if ($oldEnvironment.NUGET_PACKAGES) { $oldEnvironment.NUGET_PACKAGES } else { Join-Path $env:USERPROFILE '.nuget/packages' }
    $env:DOTNET_CLI_HOME = Join-Path $runRoot 'dotnet-home'
    $env:AVIDSCRIPT_STATIC_ASYNC_FIXTURE_DIR = Join-Path $runRoot 'Fixtures'
    $env:AVIDSCRIPT_ASYNC_MEMBER_FIXTURE_DIR = Join-Path $runRoot 'MemberFixtures'
    $env:AVIDSCRIPT_ORIGINAL_ASYNC_MEMBER_DIR = Join-Path $runRoot 'OriginalFixtures'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }
    $managedResults = @()
    foreach ($suite in @(
        @{ flag = '--static-async'; name = 'StaticAsync'; log = 'managed.log' }
        @{ flag = '--await-member-assignments'; name = 'AwaitMemberAssignments'; log = 'managed-member.log' }
        @{ flag = '--original-async-member'; name = 'OriginalAsyncMember'; log = 'managed-original.log' }
    )) {
        $managed = @(& $DotNetPath run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release -- $suite.flag 2>&1)
        $managedExit = $LASTEXITCODE
        $managed | Set-Content -LiteralPath (Join-Path $runRoot $suite.log) -Encoding utf8
        $match = [regex]::Match(($managed -join "`n"), 'AvidScript.CSharpGuest.Tests.' + [regex]::Escape($suite.name) + ': (\d+)/(\d+) passed')
        if ($managedExit -ne 0 -or -not $match.Success -or [int]$match.Groups[1].Value -le 0 -or
            $match.Groups[1].Value -cne $match.Groups[2].Value) { throw "Fixture generation failed for $($suite.name): $runRoot" }
        $managedResults += [ordered]@{ suite = $suite.name; passed = [int]$match.Groups[1].Value }
        Write-Output "$($suite.name): $($match.Groups[1].Value)/$($match.Groups[2].Value) passed"
    }
    $env:DOTNET_CLI_HOME = $oldEnvironment.DOTNET_CLI_HOME
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
            "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=1 2>&1 |
            Tee-Object -FilePath (Join-Path $runRoot 'build.log')
        if ($LASTEXITCODE -ne 0) { throw 'No-clean Win64 Editor build failed.' }
    }
    $tests = @(
        'AvidScript.Runtime.Continuation.CompiledStaticAsync'
        'AvidScript.Runtime.Continuation.CompiledAsyncMemberAssignments'
        'AvidScript.Runtime.Continuation.CompiledOriginalAsyncMember'
        'AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject'
    )
    $filter = $tests -join '+'
    $logPath = Join-Path $runRoot 'automation.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $projectPath `
        -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $filter;Quit" `
        '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Static async Automation failed: $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    $passed = 0
    foreach ($test in $tests) {
        $name = ($test -split '\.')[-1]
        $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + [regex]::Escape($name) + '\} Path=\{' + [regex]::Escape($test) + '\}'
        if ([regex]::Matches($log, $pattern).Count -eq 1) { $passed++ }
    }
    $cases = [regex]::Matches($log, 'static-async backend=\d+ scenario=[a-z-]+ mode=\d+ result=-?\d+ trace=-?\d+ resumes=\d+').Count
    $memberCases = [regex]::Matches($log, '(?<![a-z-])async-member backend=\d+ scenario=[a-z0-9-]+ mode=\d+ result=-?\d+ trace=-?\d+ resumes=\d+').Count
    $originalCases = [regex]::Matches($log, 'original-async-member backend=\d+ scenario=[a-z0-9-]+ mode=\d+ result=-?\d+ trace=-?\d+ resumes=\d+').Count
    if ($passed -ne $tests.Count -or $cases -ne 222 -or $memberCases -ne 282 -or $originalCases -ne 174 -or
        [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count -ne 0 -or
        [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1 -or
        [regex]::Matches($log, "Found 4 automation tests based on '$([regex]::Escape($filter))'").Count -ne 1) {
        throw "Static async evidence incomplete: tests=$passed cases=$cases member=$memberCases original=$originalCases log=$logPath"
    }
    $fixtureDirectories = [ordered]@{
        static_async = $env:AVIDSCRIPT_STATIC_ASYNC_FIXTURE_DIR
        member = $env:AVIDSCRIPT_ASYNC_MEMBER_FIXTURE_DIR
        original_member = $env:AVIDSCRIPT_ORIGINAL_ASYNC_MEMBER_DIR
    }
    $irVersions = @(foreach ($directory in $fixtureDirectories.Values) {
        foreach ($file in Get-ChildItem -LiteralPath $directory -Filter '*.guest-ir.json') {
            $module = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
            "$($module.schema_version)/$($module.ir_version)"
        }
    })
    [ordered]@{
        schema_version = 2
        semantic_version = '51/1.60'
        guest_ir_versions = @($irVersions | Sort-Object -Unique)
        managed = $managedResults
        native_tests_passed = $passed
        static_async_vm_modes_passed = $cases
        member_vm_modes_passed = $memberCases
        original_member_vm_modes_passed = $originalCases
        original_observations_per_case = 18
        collect_while_suspended = $true
        original_c10_completed = $false
        fixtures = @(foreach ($suite in $fixtureDirectories.Keys) {
            Get-ChildItem -LiteralPath $fixtureDirectories[$suite] -Filter '*.wasm' | ForEach-Object {
                [ordered]@{ suite = $suite; name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
            }
        })
        automation_log = $logPath
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    Write-Output "Static async: native=$passed/4, Wasmtime/WAMR=$cases/222, member=$memberCases/282, original=$originalCases/174; evidence=$runRoot"
}
finally {
    Pop-Location
    foreach ($name in $oldEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $oldEnvironment[$name], 'Process') }
}
