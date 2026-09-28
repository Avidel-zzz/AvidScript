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
$project = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$artifactRoot = Join-Path $projectRoot 'Saved/AvidScriptComposableIr35'
$fixtureRoot = Join-Path $artifactRoot 'GuestFixtures'
$runRoot = Join-Path $artifactRoot ([DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$null = New-Item -ItemType Directory -Path $runRoot -Force
$oldEnvironment = @{}
foreach ($name in @('DOTNET_CLI_HOME', 'NUGET_PACKAGES', 'AVIDSCRIPT_COMPOSABLE_IR35_FIXTURE_DIR',
        'AVIDSCRIPT_COMPOSABLE_ORIGINAL_DIR')) {
    $oldEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

Push-Location $pluginRoot
try {
    $env:DOTNET_CLI_HOME = Join-Path $runRoot 'dotnet-home'
    $env:NUGET_PACKAGES = if ($oldEnvironment.NUGET_PACKAGES) { $oldEnvironment.NUGET_PACKAGES } else { Join-Path $env:USERPROFILE '.nuget/packages' }
    $env:AVIDSCRIPT_COMPOSABLE_IR35_FIXTURE_DIR = $fixtureRoot
    $env:AVIDSCRIPT_COMPOSABLE_ORIGINAL_DIR = Join-Path $fixtureRoot 'original-ir35'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }

    $managed = @(& $DotNetPath run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj `
        -c Release -- --composable-ir35 2>&1)
    $managedExit = $LASTEXITCODE
    $managed | Set-Content -LiteralPath (Join-Path $runRoot 'managed.log') -Encoding utf8
    $match = [regex]::Match(($managed -join "`n"), 'AvidScript\.CSharpGuest\.Tests\.ComposableIr35: (\d+)/(\d+) passed')
    if ($managedExit -ne 0 -or -not $match.Success -or [int]$match.Groups[1].Value -lt 20 -or
        $match.Groups[1].Value -cne $match.Groups[2].Value) {
        throw "IR 35 C# fixture generation failed: $runRoot"
    }
    foreach ($name in @('composable-static-token.cs', 'composable-static-token.semantic.json',
        'composable-static-token.guestir.json', 'composable-static-token.wasm',
        'composable-async-static-token.cs', 'composable-async-static-token.semantic.json',
        'composable-async-static-token.guestir.json', 'composable-async-static-token.wasm',
        'original-ir35/cases.json', 'original-ir35/field-mode-0.cs',
        'original-ir35/field-mode-0.semantic.json', 'original-ir35/field-mode-0.guestir.json',
        'original-ir35/field-mode-0.wasm', 'original-ir35/field-mode-0.avidscript.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $fixtureRoot $name))) {
            throw "Missing IR 35 fixture: $name"
        }
    }
    $originalCases = @(Get-Content -LiteralPath (Join-Path $fixtureRoot 'original-ir35/cases.json') -Raw | ConvertFrom-Json)
    if ($originalCases.Count -ne 29) { throw "Expected all 29 original IR 35 cases, found $($originalCases.Count)" }
    $originalNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($case in $originalCases) {
        $name = [string]$case.name
        if ($name -cnotmatch '^[a-z0-9-]+$' -or -not $originalNames.Add($name)) {
            throw "Invalid or duplicate original IR 35 case name: $name"
        }
        foreach ($extension in @('cs', 'semantic.json', 'guestir.json', 'wasm', 'avidscript.json')) {
            if (-not (Test-Path -LiteralPath (Join-Path $fixtureRoot "original-ir35/$name.$extension"))) {
                throw "Missing original IR 35 fixture: $name.$extension"
            }
        }
    }
    $wasmPath = Join-Path $fixtureRoot 'composable-static-token.wasm'
    $wasmHash = (Get-FileHash -LiteralPath $wasmPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $asyncWasmHash = (Get-FileHash -LiteralPath (Join-Path $fixtureRoot 'composable-async-static-token.wasm') -Algorithm SHA256).Hash.ToLowerInvariant()
    $originalWasmHash = (Get-FileHash -LiteralPath (Join-Path $fixtureRoot 'original-ir35/field-mode-0.wasm') -Algorithm SHA256).Hash.ToLowerInvariant()
    $semanticHash = (Get-FileHash -LiteralPath (Join-Path $fixtureRoot 'composable-static-token.semantic.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    $guestIrHash = (Get-FileHash -LiteralPath (Join-Path $fixtureRoot 'composable-static-token.guestir.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Output "Composable IR 35 managed: $($match.Groups[1].Value)/$($match.Groups[2].Value); wasm_sha256=$wasmHash; async_wasm_sha256=$asyncWasmHash; original_wasm_sha256=$originalWasmHash"

    $env:DOTNET_CLI_HOME = $oldEnvironment.DOTNET_CLI_HOME
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
            "-Project=$project" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=1 `
            *> (Join-Path $runRoot 'build.log')
        if ($LASTEXITCODE -ne 0) { throw "No-clean Win64 Editor build failed: $runRoot" }
    }

    $tests = @(
        'AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject',
        'AvidScript.Runtime.ManagedHeap.ComposableIr35StaticToken',
        'AvidScript.Runtime.ManagedHeap.ComposableIr35AsyncExecution',
        'AvidScript.Runtime.Continuation.ComposableOriginalAsyncMember',
        'AvidScript.Component.ComposableOriginalTeardown'
    )
    $filter = $tests -join '+'
    $logPath = Join-Path $runRoot 'automation.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $project `
        -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $filter;Quit" `
        '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "IR 35 Automation failed: $logPath" }

    $log = Get-Content -LiteralPath $logPath -Raw
    $passed = 0
    foreach ($test in $tests) {
        $name = ($test -split '\.')[-1]
        $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + [regex]::Escape($name) + '\} Path=\{' + [regex]::Escape($test) + '\}'
        if ([regex]::Matches($log, $pattern).Count -eq 1) { $passed++ }
    }
    $observations = [regex]::Matches($log, 'composable-ir35 backend=\d+ domain=\d+ first=2 second=3 roots=\d+').Count
    $asyncObservations = 0
    foreach ($backend in @(0, 1)) {
        foreach ($case in @(@{ Mode = 'complete'; Result = 1 }, @{ Mode = 'cancel'; Result = 7 },
                @{ Mode = 'teardown'; Result = 0 })) {
            $pattern = "composable-ir35-async backend=$backend mode=$($case.Mode) result=$($case.Result) continuations=0 tasks=0 sources=0"
            if ([regex]::Matches($log, [regex]::Escape($pattern)).Count -eq 1) { $asyncObservations++ }
        }
    }
    $originalObservations = 0
    foreach ($case in $originalCases) {
        foreach ($backend in @(0, 1)) {
            foreach ($mode in @(0, 1, 2)) {
                $pattern = "original-ir35 backend=$backend scenario=$($case.name) mode=$mode result=-?\d+ trace=-?\d+ resumes=\d+"
                if ([regex]::Matches($log, $pattern).Count -eq 1) { $originalObservations++ }
            }
        }
    }
    $componentObservations = 0
    foreach ($backend in @(0, 1)) {
        foreach ($mode in @('actor', 'world')) {
            $pattern = "original-ir35-component backend=$backend teardown=$mode tasks_before=\d+ waiters_before=\d+ released=1"
            if ([regex]::Matches($log, $pattern).Count -eq 1) { $componentObservations++ }
        }
    }
    if ($passed -ne $tests.Count -or $observations -ne 4 -or $asyncObservations -ne 6 -or
        $originalObservations -ne 174 -or $componentObservations -ne 4 -or
        [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count -ne 0 -or
        [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1 -or
        [regex]::Matches($log, "Found $($tests.Count) automation tests based on '$([regex]::Escape($filter))'").Count -ne 1) {
        throw "IR 35 Automation evidence incomplete: tests=$passed observations=$observations async_observations=$asyncObservations original_observations=$originalObservations component_observations=$componentObservations log=$logPath"
    }
    [ordered]@{
        schema_version = 1
        sdk = $sdk
        managed_passed = [int]$match.Groups[1].Value
        semantic_sha256 = $semanticHash
        guest_ir_sha256 = $guestIrHash
        wasm_sha256 = $wasmHash
        async_wasm_sha256 = $asyncWasmHash
        original_wasm_sha256 = $originalWasmHash
        vm_observations = $observations
        async_vm_observations = $asyncObservations
        original_vm_observations = $originalObservations
        component_teardown_observations = $componentObservations
        automation_passed = $passed
        automation_log = $logPath
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    Write-Output "Composable IR 35: sync observations $observations/4; async observations $asyncObservations/6; original observations $originalObservations/174; component teardown $componentObservations/4; Automation $passed/$($tests.Count); evidence=$runRoot"
}
finally {
    Pop-Location
    foreach ($name in $oldEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $oldEnvironment[$name], 'Process')
    }
}
