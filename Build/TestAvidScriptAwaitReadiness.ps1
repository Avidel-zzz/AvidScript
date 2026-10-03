[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [switch]$CancellationIdentity,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$project = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptAwaitReadiness/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$null = New-Item -ItemType Directory -Path $runRoot -Force
$oldEnvironment = @{}
foreach ($name in @('DOTNET_CLI_HOME', 'NUGET_PACKAGES', 'AVIDSCRIPT_AWAIT_READINESS_DIR')) {
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
    $env:AVIDSCRIPT_AWAIT_READINESS_DIR = Join-Path $runRoot 'Fixtures'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }
    $managedMode = if ($CancellationIdentity) { '--await-readiness-identity' } else { '--await-readiness-evaluation' }
    $managed = @(& $DotNetPath run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release -- $managedMode 2>&1)
    $managedExit = $LASTEXITCODE
    $managed | Set-Content -LiteralPath (Join-Path $runRoot 'managed.log') -Encoding utf8
    $match = [regex]::Match(($managed -join "`n"), 'AvidScript.CSharpGuest.Tests.AwaitReadinessEvaluation: (\d+)/(\d+) passed')
    if ($managedExit -ne 0 -or -not $match.Success -or [int]$match.Groups[1].Value -le 0 -or
        $match.Groups[1].Value -cne $match.Groups[2].Value) { throw "Readiness fixture generation failed: $runRoot" }
    Write-Output "Readiness managed: $($match.Groups[1].Value)/$($match.Groups[2].Value)"
    $env:DOTNET_CLI_HOME = $oldEnvironment.DOTNET_CLI_HOME
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
            "-Project=$project" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=1 *> (Join-Path $runRoot 'build.log')
        if ($LASTEXITCODE -ne 0) { throw "No-clean Win64 build failed: $runRoot" }
    }
    $test = 'AvidScript.Runtime.Continuation.CompiledAwaitReadinessEvaluation'
    $logPath = Join-Path $runRoot 'automation.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $project `
        -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $test;Quit" `
        '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Readiness Automation failed: $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    $cases = [regex]::Matches($log, 'await-readiness backend=\d+ scenario=[a-z-]+ mode=\d+ result=-?\d+ trace=-?\d+ resumes=\d+').Count
    if ($cases -ne 108 -or
        [regex]::Matches($log, 'Test Completed\. Result=\{Success\} Name=\{CompiledAwaitReadinessEvaluation\}').Count -ne 1 -or
        [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count -ne 0 -or
        [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1 -or
        [regex]::Matches($log, "Found 1 automation tests based on '$([regex]::Escape($test))'").Count -ne 1) {
        throw "Readiness evidence incomplete: cases=$cases log=$logPath"
    }
    [ordered]@{
        schema_version = 1
        cancellation_identity = [bool]$CancellationIdentity
        managed_passed = [int]$match.Groups[1].Value
        scenarios = 18
        vm_modes_passed = $cases
        automation_passed = 1
        final_observations = 9
        first_resume_observations = 4
        fixtures = @(Get-ChildItem -LiteralPath $env:AVIDSCRIPT_AWAIT_READINESS_DIR -Filter '*.wasm' | ForEach-Object {
            [ordered]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
        automation_log = $logPath
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    Write-Output "Await readiness: $cases/108; Automation=1/1; evidence=$runRoot"
}
finally {
    Pop-Location
    foreach ($name in $oldEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $oldEnvironment[$name], 'Process') }
}
