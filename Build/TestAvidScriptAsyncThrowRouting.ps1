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
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptAsyncThrowRouting/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$null = New-Item -ItemType Directory -Path $runRoot -Force
$oldCliHome = $env:DOTNET_CLI_HOME
$oldFixtures = $env:AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR
Push-Location $pluginRoot
try {
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'AvidScriptAsyncThrowCliHome'
    $env:AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR = Join-Path $runRoot 'Fixtures'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }
    $managed = @(& $DotNetPath run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release -- --async-throw-routing)
    $managedExit = $LASTEXITCODE
    $managed | Set-Content -LiteralPath (Join-Path $runRoot 'managed.log') -Encoding utf8
    $match = [regex]::Match(($managed -join "`n"), 'AvidScript.CSharpGuest.Tests.AsyncThrowRouting: (\d+)/(\d+) passed')
    if ($managedExit -ne 0 -or -not $match.Success -or $match.Groups[1].Value -cne $match.Groups[2].Value) {
        throw "Async throw fixture generation failed: $runRoot"
    }
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
            "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles
        if ($LASTEXITCODE -ne 0) { throw 'No-clean Win64 Editor build failed.' }
    }
    $tests = @(
        'AvidScript.Runtime.Continuation.CompiledAsyncThrowRouting'
        'AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject'
        'AvidScript.Runtime.LanguageErrorCatalog.TaskFaultVmImport'
    )
    $filter = $tests -join '+'
    $logPath = Join-Path $runRoot 'automation.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $projectPath `
        -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $filter;Quit" `
        '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Async throw Automation failed: $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    $passed = 0
    foreach ($test in $tests) {
        $name = ($test -split '\.')[-1]
        $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + [regex]::Escape($name) +
            '\} Path=\{' + [regex]::Escape($test) + '\}'
        if ([regex]::Matches($log, $pattern).Count -eq 1) { $passed++ }
    }
    $cases = [regex]::Matches($log, 'async-throw backend=\d+ scenario=[a-z-]+ mode=\d+ result=-?\d+ trace=-?\d+ resumes=\d+').Count
    if ($passed -ne $tests.Count -or $cases -ne 162 -or
        [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count -ne 0 -or
        [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1 -or
        [regex]::Matches($log, "Found 3 automation tests based on '$([regex]::Escape($filter))'").Count -ne 1) {
        throw "Async throw evidence incomplete: tests=$passed cases=$cases log=$logPath"
    }
    [ordered]@{
        schema_version = 1
        semantic_version = '46/1.55'
        guest_ir_version = '26/1.25'
        managed_passed = [int]$match.Groups[1].Value
        native_tests_passed = $passed
        vm_scenarios_passed = $cases
        fixtures = @(Get-ChildItem -LiteralPath $env:AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR -Filter '*.wasm' | ForEach-Object {
            [ordered]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
        automation_log = $logPath
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    Write-Output "Async throw routing: managed=$($match.Groups[1].Value)/$($match.Groups[2].Value), native=$passed/3, Wasmtime/WAMR=$cases/162; evidence=$runRoot"
}
finally {
    Pop-Location
    $env:DOTNET_CLI_HOME = $oldCliHome
    $env:AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR = $oldFixtures
}
