param([string]$EngineRoot = 'C:\UnrealEngine', [switch]$IncludeInstanceLifecycle, [string]$SuitePrefix)
$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
# Regress both legacy dispatch paths affected by contextual callback integration.
# The new shared-runtime and generated-session cases run in TestAvidScriptUeReceivers.ps1.
$Suites = @(
    @{ Prefix = 'AvidScript.Runtime.Continuation'; Count = 10 },
    @{ Prefix = 'AvidScript.Runtime.DelegateSubscription'; Count = 5 }
)
if ($IncludeInstanceLifecycle) {
    $Suites += @(
        @{ Prefix = 'AvidScript.Runtime.Timer'; Count = 6 },
        @{ Prefix = 'AvidScript.Architecture.RuntimeLifecycle'; Count = 1 },
        @{ Prefix = 'AvidScript.Architecture.Session'; Count = 10 },
        @{ Prefix = 'AvidScript.Runtime.Session'; Filter = 'AvidScript.Runtime.Session.'; Count = 4 }
    )
}
if ($SuitePrefix) {
    $Suites = @($Suites | Where-Object { $_.Prefix -ceq $SuitePrefix })
    if ($Suites.Count -ne 1) { throw "Unknown or disabled callback suite: $SuitePrefix" }
}
foreach ($Suite in $Suites) {
    $RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $TestName = $Suite.Prefix
    $TestFilter = if ($Suite.Filter) { $Suite.Filter } else { $TestName }
    $LogPath = Join-Path $ProjectRoot "Saved/Logs/AvidScript_Callbacks_$($TestName.Split('.')[-1])_$RunId.log"
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $ProjectPath -unattended -nop4 -NullRHI -nosplash "-ExecCmds=Automation RunTests $TestFilter;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$LogPath"
    if ($LASTEXITCODE -ne 0) { throw "Callback Automation exited with $LASTEXITCODE. Log: $LogPath" }
    $Log = Get-Content -Raw -LiteralPath $LogPath
    $EscapedPrefix = [regex]::Escape($TestName)
    $Found = [regex]::Matches($Log, "Found $($Suite.Count) automation tests based on '$([regex]::Escape($TestFilter))'").Count
    $Passed = [regex]::Matches($Log, "Test Completed\. Result=\{Success\} Name=\{([^}]+)\} Path=\{$EscapedPrefix\.\1\}")
    $Failed = [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count
    $Complete = [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
    $Exit = [regex]::Matches($Log, 'RequestExitWithStatus\(1, 0,').Count
    $Unique = @($Passed | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique).Count
    if ($Found -ne 1 -or $Passed.Count -ne $Suite.Count -or $Unique -ne $Suite.Count -or $Failed -ne 0 -or $Complete -ne 1 -or $Exit -lt 1) {
        throw "Callback Automation incomplete: suite=$TestName found=$Found passed=$($Passed.Count) unique=$Unique failed=$Failed complete=$Complete exit=$Exit log=$LogPath"
    }
    Write-Output "${TestName}: $($Suite.Count)/$($Suite.Count) passed; log=$LogPath"
}
