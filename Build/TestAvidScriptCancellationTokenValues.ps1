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
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptCancellationTokenValues/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$fixtureRoot = Join-Path $runRoot 'Fixtures'
$null = New-Item -ItemType Directory -Path $runRoot -Force
$oldEnvironment = @{}
foreach ($name in @('DOTNET_CLI_HOME', 'NUGET_PACKAGES', 'AVIDSCRIPT_CSHARP_TOKEN_FIXTURE_DIR')) {
    $oldEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location $pluginRoot

try {
    $env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
    $env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
    $env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
    $env:DOTNET_NOLOGO = '1'
    $env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
    $env:DOTNET_CLI_HOME = Join-Path $runRoot 'dotnet-home'
    $env:NUGET_PACKAGES = if ($oldEnvironment.NUGET_PACKAGES) { $oldEnvironment.NUGET_PACKAGES } else { Join-Path $env:USERPROFILE '.nuget/packages' }
    $env:AVIDSCRIPT_CSHARP_TOKEN_FIXTURE_DIR = $fixtureRoot
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }

    $managed = @(& $DotNetPath run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release -- --cancellation-token-values 2>&1)
    $managedExit = $LASTEXITCODE
    $managed | Set-Content -LiteralPath (Join-Path $runRoot 'managed.log') -Encoding utf8
    $match = [regex]::Match(($managed -join "`n"), 'AvidScript.CSharpGuest.Tests.CancellationTokenValues: (\d+)/(\d+) passed')
    if ($managedExit -ne 0 -or -not $match.Success -or $match.Groups[1].Value -cne '312' -or $match.Groups[2].Value -cne '312') {
        throw "Token fixture generation failed: $runRoot"
    }

    $manifestPath = Join-Path $fixtureRoot 'cases.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Missing token fixture manifest: $manifestPath" }
    $cases = @(Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json)
    if ($cases.Count -ne 21 -or @($cases | Where-Object { $_.asynchronous }).Count -ne 10 -or
        @($cases | Where-Object { -not $_.asynchronous }).Count -ne 11) {
        throw "Unexpected token fixture matrix: $($cases.Count) cases"
    }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($case in $cases) {
        $name = [string]$case.name
        if ($name -cnotmatch '^[a-z][a-z0-9-]+$' -or -not $names.Add($name)) {
            throw "Invalid or duplicate token fixture name: $name"
        }
        foreach ($extension in @('.cs', '.semantic.json', '.guestir.json', '.exports.json', '.wasm')) {
            if (-not (Test-Path -LiteralPath (Join-Path $fixtureRoot ($name + $extension)))) {
                throw "Missing token fixture file: $name$extension"
            }
        }
        foreach ($item in @(
            @{ path = Join-Path $fixtureRoot ($name + '.semantic.json'); sha = [string]$case.semanticSha256 },
            @{ path = Join-Path $fixtureRoot ($name + '.wasm'); sha = [string]$case.wasmSha256 }
        )) {
            $actual = (Get-FileHash -LiteralPath $item.path -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -cne $item.sha) { throw "Token fixture hash mismatch: $($item.path)" }
        }
    }
    if (@(Get-ChildItem -LiteralPath $fixtureRoot -Filter '*.wasm').Count -ne 21) {
        throw "Token fixture directory contains an unexpected WASM count: $fixtureRoot"
    }
    Write-Output "Token fixtures: 312/312; 21 manifests and hashes; evidence=$runRoot"

    $env:DOTNET_CLI_HOME = $oldEnvironment.DOTNET_CLI_HOME
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
            "-Project=$project" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=1 *> (Join-Path $runRoot 'build.log')
        if ($LASTEXITCODE -ne 0) { throw "No-clean Win64 build failed: $runRoot" }
    }

    $tests = @(
        'AvidScript.Runtime.Continuation.CompiledCancellationTokenValues',
        'AvidScript.Runtime.Continuation.ExceptionCancellationObjectReader',
        'AvidScript.Runtime.Continuation.TaskCancellationIdentityAbi',
        'AvidScript.Runtime.Continuation.TaskCancellationIdentityAdmission',
        'AvidScript.Runtime.Continuation.TaskCancellationIdentityVersion',
        'AvidScript.Runtime.Continuation.TaskCancellationIdentityLifecycle',
        'AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject'
    )
    $filter = $tests -join '+'
    $logPath = Join-Path $runRoot 'automation.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $project `
        -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $filter;Quit" `
        '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Token Automation failed: $logPath" }

    $log = Get-Content -LiteralPath $logPath -Raw
    $observations = [regex]::Matches($log, 'cancellation-token backend=\d+ scenario=[a-z-]+ mode=\d+ result=-?\d+ trace=-?\d+ resumes=\d+').Count
    $abiModes = [regex]::Matches($log, 'cancellation identity mode=\d+ object_reader=\d+').Count
    $passed = 0
    foreach ($test in $tests) {
        $name = ($test -split '\.')[-1]
        $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + [regex]::Escape($name) + '\} Path=\{' + [regex]::Escape($test) + '\}'
        if ([regex]::Matches($log, $pattern).Count -eq 1) { $passed++ }
    }
    if ($observations -ne 82 -or $abiModes -ne 12 -or $passed -ne 7 -or
        [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count -ne 0 -or
        [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1 -or
        [regex]::Matches($log, "Found 7 automation tests based on '$([regex]::Escape($filter))'").Count -ne 1) {
        throw "Token Automation evidence incomplete: observations=$observations abi=$abiModes tests=$passed log=$logPath"
    }
    [ordered]@{
        schema_version = 1
        sdk = $sdk
        managed_passed = 312
        scenarios = 21
        vm_mode_observations = $observations
        abi_modes = $abiModes
        automation_passed = $passed
        fixtures = @($cases | ForEach-Object { [ordered]@{ name = $_.name; wasm_sha256 = $_.wasmSha256 } })
        automation_log = $logPath
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    Write-Output "Token WASM: $observations/82; native ABI: $abiModes/12; Automation: $passed/7; evidence=$runRoot"
}
finally {
    Pop-Location
    foreach ($name in $oldEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $oldEnvironment[$name], 'Process') }
}
