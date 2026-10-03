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
$engineDotNet = Join-Path $EngineRoot 'Engine/Binaries/ThirdParty/DotNet/10.0/win-x64/dotnet.exe'
$ubt = Join-Path $EngineRoot 'Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll'
$editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
foreach ($required in @($DotNetPath, $engineDotNet, $ubt, $editor, $projectPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Prepared toolchain prerequisite missing: $required" }
}
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptAsyncVoidReload/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N'))
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $runRoot 'Fixtures'))
$environmentNames = @('DOTNET_CLI_HOME', 'AVIDSCRIPT_ASYNC_VOID_RELOAD_FIXTURE_DIR', 'DOTNET_GENERATE_ASPNET_CERTIFICATE',
    'DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK', 'DOTNET_ADD_GLOBAL_TOOLS_TO_PATH', 'DOTNET_NOLOGO',
    'DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE', 'DOTNET_CLI_TELEMETRY_OPTOUT',
    'MSBUILDDISABLENODEREUSE', 'DOTNET_CLI_USE_MSBUILD_SERVER', 'UseSharedCompilation')
$previousEnvironment = @{}
foreach ($name in $environmentNames) { $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$userPathBefore = [Environment]::GetEnvironmentVariable('Path', 'User')
$machinePathBefore = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$process = [Diagnostics.Process]::GetCurrentProcess()
$oldPriority = $process.PriorityClass
$ownedFixtures = @()
$succeeded = $false
Push-Location $pluginRoot
try {
    $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
    $env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
    $env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
    $env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
    $env:DOTNET_NOLOGO = '1'
    $env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $env:MSBUILDDISABLENODEREUSE = '1'
    $env:DOTNET_CLI_USE_MSBUILD_SERVER = '0'
    $env:UseSharedCompilation = 'false'
    $env:DOTNET_CLI_HOME = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Toolchain/AsyncVoidReload'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected installed SDK 8.0.416, got $sdk" }
    $null = New-Item -ItemType Directory -Path $fixtureRoot
    $env:AVIDSCRIPT_ASYNC_VOID_RELOAD_FIXTURE_DIR = $fixtureRoot
    $testProject = 'Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj'
    & $DotNetPath build $testProject -c Release --no-restore --disable-build-servers -m:1 -nodeReuse:false `
        -p:UseSharedCompilation=false --nologo -v quiet
    if ($LASTEXITCODE -ne 0) { throw 'Single-node managed build failed. No SDK/workload installation attempted.' }
    $managed = @(& $DotNetPath run --project $testProject -c Release --no-build --no-restore -- --async-void-reload)
    $managedExit = $LASTEXITCODE
    $managed | Set-Content -LiteralPath (Join-Path $runRoot 'managed.log') -Encoding utf8
    $match = [regex]::Match(($managed -join "`n"), 'AvidScript.CSharpGuest.Tests.AsyncVoidReload: (\d+)/(\d+) passed')
    if ($managedExit -ne 0 -or -not $match.Success -or $match.Groups[1].Value -cne $match.Groups[2].Value) {
        throw "Same-source reload fixture generation failed: $runRoot"
    }
    $ownedFixtures = @(Get-ChildItem -LiteralPath $fixtureRoot -File | ForEach-Object {
        [ordered]@{ name = $_.Name; bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $ownedFixtures | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'fixture-hashes.json') -Encoding utf8
    if ($ownedFixtures.Count -ne 13) { throw "Expected two source/semantic/IR/WASM/state/manifest generations plus .NET references, got $($ownedFixtures.Count)." }
    if (-not $SkipBuild) {
        # Already-built UBT only; no Build.bat SDK/UBT bootstrap.
        Push-Location (Join-Path $EngineRoot 'Engine/Source')
        try {
            & $engineDotNet $ubt AvidTPSTemplateEditor Win64 Development `
                "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=1 -NoUBA
        }
        finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw 'No-clean single-action Win64 Editor build failed.' }
    }
    $tests = @(
        'AvidScript.Runtime.Continuation.AsyncVoidNaturalReload'
        'AvidScript.Runtime.LanguageErrorCatalog.AsyncVoidAdmission'
        'AvidScript.Runtime.LanguageErrorCatalog.AsyncVoidCheckedReport'
        'AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject'
    )
    $filter = $tests -join '+'
    $logPath = Join-Path $runRoot 'automation.log'
    & $editor $projectPath -unattended -nop4 -NullRHI -nosplash -Multiprocess `
        "-ExecCmds=Automation RunTests $filter;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Natural reload Automation failed: $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    $passed = 0
    foreach ($test in $tests) {
        $name = ($test -split '\.')[-1]
        $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + [regex]::Escape($name) + '\} Path=\{' + [regex]::Escape($test) + '\}'
        if ([regex]::Matches($log, $pattern).Count -eq 1) { $passed++ }
    }
    $cases = [regex]::Matches($log, 'async-void-reload backend=\d+ stage=[01] mode=[0-3] committed=[01] fault=[01] resources=0 runtimes=0').Count
    if ($passed -ne $tests.Count -or $cases -ne 16 -or
        [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count -ne 0 -or
        [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1 -or
        [regex]::Matches($log, "Found $($tests.Count) automation tests based on '$([regex]::Escape($filter))'").Count -ne 1) {
        throw "Natural reload evidence incomplete: tests=$passed/$($tests.Count) scenarios=$cases/16 log=$logPath"
    }
    if ($userPathBefore -cne [Environment]::GetEnvironmentVariable('Path', 'User') -or
        $machinePathBefore -cne [Environment]::GetEnvironmentVariable('Path', 'Machine')) {
        throw 'Persistent PATH changed; no repair or overwrite attempted.'
    }
    [ordered]@{
        schema_version = 1; semantic_version = '55/1.64'; guest_ir_version = '36/1.35'; sdk = $sdk
        managed_passed = [int]$match.Groups[1].Value; native_tests_passed = $passed; vm_scenarios_passed = $cases
        process_priority = 'BelowNormal'; max_parallel_build_actions = 1; persistent_path_unchanged = $true
        fixture_hashes = 'fixture-hashes.json'; automation_log = 'automation.log'
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    $succeeded = $true
    Write-Output "Async void natural reload: managed=$($match.Groups[1].Value)/$($match.Groups[2].Value), native=$passed/$($tests.Count), scenarios=$cases/16; evidence=$runRoot"
}
finally {
    try {
        # Never touch earlier failed probes, shared caches or engine artifacts.
        # Only this successful run's exact, still-matching files are removed.
        if ($succeeded) {
            $removedBytes = 0L
            foreach ($fixture in $ownedFixtures) {
                $target = [IO.Path]::GetFullPath((Join-Path $fixtureRoot $fixture.name))
                if (-not $target.StartsWith($fixtureRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
                    [IO.Path]::GetFileName($target) -cne $fixture.name) { throw 'Fixture cleanup escaped this run.' }
                if (Test-Path -LiteralPath $target -PathType Leaf) {
                    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -cne $fixture.sha256) {
                        throw "Fixture changed; preserved: $target"
                    }
                    Remove-Item -LiteralPath $target
                    $removedBytes += $fixture.bytes
                }
            }
            if (@(Get-ChildItem -LiteralPath $fixtureRoot -Force).Count -eq 0) { Remove-Item -LiteralPath $fixtureRoot }
            Write-Output "Owned fixture cleanup: $($ownedFixtures.Count) files, $removedBytes bytes; logs and hashes retained."
        }
    }
    finally {
        foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process') }
        $process.PriorityClass = $oldPriority
        Pop-Location
    }
}
