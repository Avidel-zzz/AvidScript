[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [switch]$SkipBuild,
    [switch]$Composition,
    [switch]$PublicCli
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$projectPath = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptAsyncVoidOwners/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N'))
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $runRoot 'Fixtures'))
$null = New-Item -ItemType Directory -Path $fixtureRoot
$environmentNames = @('DOTNET_CLI_HOME', 'AVIDSCRIPT_ASYNC_VOID_OWNER_FIXTURE_DIR', 'DOTNET_GENERATE_ASPNET_CERTIFICATE',
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
    # This script and its children are task-owned. Never change another process.
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
    $env:DOTNET_CLI_HOME = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Toolchain/AsyncVoidOwners'
    $env:AVIDSCRIPT_ASYNC_VOID_OWNER_FIXTURE_DIR = $fixtureRoot
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected installed SDK 8.0.416, got $sdk" }
    $testProject = 'Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj'
    & $DotNetPath build $testProject -c Release --no-restore --disable-build-servers -m:1 -nodeReuse:false `
        -p:UseSharedCompilation=false --nologo -v quiet
    if ($LASTEXITCODE -ne 0) { throw 'Single-node managed build failed. No SDK or workload installation was attempted.' }
    $managedArgument = if ($PublicCli) { '--capability-cli' } elseif ($Composition) { '--async-void-composition' } else { '--async-void-error-owners' }
    $managedLabel = if ($PublicCli) { 'CapabilityCli' } elseif ($Composition) { 'AsyncVoidComposition' } else { 'AsyncVoidErrorOwners' }
    $managed = @(& $DotNetPath run --project $testProject -c Release --no-build --no-restore -- $managedArgument)
    $managedExit = $LASTEXITCODE
    $managed | Set-Content -LiteralPath (Join-Path $runRoot 'managed.log') -Encoding utf8
    $match = [regex]::Match(($managed -join "`n"), 'AvidScript\.CSharpGuest\.Tests\.' + $managedLabel + ': (\d+)/(\d+) passed')
    if ($managedExit -ne 0 -or -not $match.Success -or $match.Groups[1].Value -cne $match.Groups[2].Value) {
        throw "Same-source async void fixture generation failed: $runRoot"
    }
    $ownedFixtures = @(Get-ChildItem -LiteralPath $fixtureRoot -File | ForEach-Object {
        [ordered]@{ name = $_.Name; bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $ownedFixtures | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'fixture-hashes.json') -Encoding utf8
    if ($ownedFixtures.Count -ne 97) { throw "Expected 24 source/semantic/IR/WASM fixtures and one manifest, got $($ownedFixtures.Count)." }
    if (-not $SkipBuild) {
        # Use the already-built UBT. Build.bat can rebuild UBT with unbounded
        # MSBuild; this probe neither bootstraps an engine nor installs its SDK.
        $engineDotNet = Join-Path $EngineRoot 'Engine/Binaries/ThirdParty/DotNet/10.0/win-x64/dotnet.exe'
        $ubt = Join-Path $EngineRoot 'Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll'
        if (-not (Test-Path -LiteralPath $engineDotNet -PathType Leaf) -or -not (Test-Path -LiteralPath $ubt -PathType Leaf)) {
            throw 'A prepared UE5.8 Win64 engine with bundled .NET and prebuilt UBT is required.'
        }
        Push-Location (Join-Path $EngineRoot 'Engine/Source')
        try {
            & $engineDotNet $ubt AvidTPSTemplateEditor Win64 Development `
                "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=1 -NoUBA
        }
        finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw 'No-clean single-action Win64 Editor build failed.' }
    }
    $tests = @(
        'AvidScript.Runtime.Continuation.CompiledAsyncVoidErrorOwners'
        'AvidScript.Runtime.LanguageErrorCatalog.AsyncVoidAdmission'
        'AvidScript.Runtime.LanguageErrorCatalog.AsyncVoidCompositionAdmission'
        'AvidScript.Runtime.LanguageErrorCatalog.AsyncVoidCheckedReport'
        'AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject'
        'AvidScript.Runtime.LanguageErrorCatalog.TaskFaultVmImport'
        'AvidScript.Runtime.GeneratedTypes.SharedRuntimeContext'
        'AvidScript.Runtime.GeneratedTypes.SharedRuntimeCallbacks'
        'AvidScript.Runtime.GeneratedTypes.SharedInstanceLifecycle'
    )
    $filter = $tests -join '+'
    $logPath = Join-Path $runRoot 'automation.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $projectPath `
        -unattended -nop4 -NullRHI -nosplash -Multiprocess "-ExecCmds=Automation RunTests $filter;Quit" `
        '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    $editorExit = $LASTEXITCODE
    if ($editorExit -ne 0) { throw "Async void Automation failed (exit $editorExit): $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    $passed = 0
    foreach ($test in $tests) {
        $name = ($test -split '\.')[-1]
        $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + [regex]::Escape($name) +
            '\} Path=\{' + [regex]::Escape($test) + '\}'
        if ([regex]::Matches($log, $pattern).Count -eq 1) { $passed++ }
    }
    $cases = [regex]::Matches($log, 'async-void-owner backend=\d+ scenario=[a-z-]+ mode=\d+ fault=[01] trace=-?\d+ resumes=\d+').Count
    if ($passed -ne $tests.Count -or $cases -ne 192 -or
        [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count -ne 0 -or
        [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1 -or
        [regex]::Matches($log, "Found $($tests.Count) automation tests based on '$([regex]::Escape($filter))'").Count -ne 1) {
        throw "Async void evidence incomplete: tests=$passed/$($tests.Count) cases=$cases/192 log=$logPath"
    }
    if ($userPathBefore -cne [Environment]::GetEnvironmentVariable('Path', 'User') -or
        $machinePathBefore -cne [Environment]::GetEnvironmentVariable('Path', 'Machine')) {
        throw 'Persistent PATH changed during verification; no repair or overwrite was attempted.'
    }
    [ordered]@{
        schema_version = 1
        semantic_version = $(if ($Composition -or $PublicCli) { '56/1.65' } else { '55/1.64' })
        guest_ir_version = $(if ($Composition -or $PublicCli) { '37/1.36' } else { '36/1.35' })
        source_entry = $(if ($PublicCli) { 'public-cli' } else { 'compiler-api' })
        sdk = $sdk
        managed_passed = [int]$match.Groups[1].Value
        native_tests_passed = $passed
        vm_scenarios_passed = $cases
        process_priority = 'BelowNormal'
        max_parallel_build_actions = 1
        persistent_path_unchanged = $true
        fixture_hashes = 'fixture-hashes.json'
        automation_log = 'automation.log'
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    $succeeded = $true
    Write-Output "Async void owners: managed=$($match.Groups[1].Value)/$($match.Groups[2].Value), native=$passed/$($tests.Count), Wasmtime/WAMR=$cases/192; evidence=$runRoot"
}
finally {
    try {
    # A failed probe remains available for diagnosis. Success removes only the
    # files generated and hashed by this run, preserving compact logs/evidence.
    if ($succeeded) {
        $removedBytes = 0L
        foreach ($fixture in $ownedFixtures) {
            $target = [IO.Path]::GetFullPath((Join-Path $fixtureRoot $fixture.name))
            if (-not $target.StartsWith($fixtureRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
                [IO.Path]::GetFileName($target) -cne $fixture.name) { throw 'Fixture cleanup escaped this run.' }
            if (Test-Path -LiteralPath $target -PathType Leaf) {
                if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -cne $fixture.sha256) {
                    throw "Fixture changed after verification; preserved: $target"
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
