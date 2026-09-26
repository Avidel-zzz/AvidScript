[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    # Use only when native test binaries already match the current sources.
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$project = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptSharedTaskLifetime/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$null = New-Item -ItemType Directory -Path $runRoot -Force
$previousDirectory = $env:AVIDSCRIPT_SHARED_TASK_FIXTURE_DIR
$previousCliHome = $env:DOTNET_CLI_HOME
Push-Location $pluginRoot
try {
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'AvidScriptSharedTaskCliHome'
    $env:AVIDSCRIPT_SHARED_TASK_FIXTURE_DIR = Join-Path $runRoot 'GuestFixtures'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }
    Write-Output "Shared Task evidence: $runRoot"
    $managedLog = Join-Path $runRoot 'managed.log'
    & $DotNetPath run --project Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj -c Release -- --shared-task-lifetime *> $managedLog
    if ($LASTEXITCODE -ne 0 -or (Get-Content $managedLog -Raw) -notmatch 'SharedTaskLifetime: 26/26 passed') {
        Get-Content $managedLog -Tail 30
        throw "Shared Task compiler/reference checks failed: $managedLog"
    }
    if (-not $SkipBuild) {
        $buildLog = Join-Path $runRoot 'build.log'
        & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
            "-Project=$project" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles -gather *> $buildLog
        if ($LASTEXITCODE -ne 0) {
            Get-Content $buildLog -Tail 40
            throw "No-clean Editor build failed: $buildLog"
        }
    }
    $testName = 'AvidScript.Runtime.GeneratedTypes.SharedTaskLifetime'
    $logPath = Join-Path $runRoot 'automation.log'
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $project -unattended -nop4 -NullRHI -nosplash `
        "-ExecCmds=Automation RunTests $testName;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
    if ($LASTEXITCODE -ne 0) { throw "Shared Task Automation failed: $logPath" }
    $log = Get-Content $logPath -Raw
    $found = [regex]::Matches($log, "Found 1 automation tests based on '$([regex]::Escape($testName))'").Count
    $success = [regex]::Matches($log, 'Test Completed\. Result=\{Success\} Name=\{SharedTaskLifetime\} Path=\{' + [regex]::Escape($testName) + '\}').Count
    $failed = [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count
    $footer = [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
    $cases = @([regex]::Matches($log, 'shared Task passed backend=[01] mode=[0-7] cancel=[01]').Value | Sort-Object -Unique).Count
    if ($found -ne 1 -or $success -ne 1 -or $failed -ne 0 -or $footer -ne 1 -or $cases -ne 30 -or
        -not $log.Contains('SharedTaskLifetime: 30/30 passed')) {
        throw "Incomplete shared Task evidence: found=$found success=$success failed=$failed footer=$footer cases=$cases"
    }
    [ordered]@{ passed = 30; total = 30; managed_passed = 26; reference_cases = 8; automation_passed = 1; log = $logPath } |
        ConvertTo-Json | Set-Content (Join-Path $runRoot 'results.json') -Encoding utf8NoBOM
    Write-Output "Shared Task lifetime: 30/30 passed; managed=26/26; .NET reference=8/8; Automation=1/1; evidence=$runRoot"
}
finally {
    Pop-Location
    $env:AVIDSCRIPT_SHARED_TASK_FIXTURE_DIR = $previousDirectory
    $env:DOTNET_CLI_HOME = $previousCliHome
}
