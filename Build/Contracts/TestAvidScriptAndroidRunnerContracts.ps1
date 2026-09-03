Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent $PSScriptRoot
$BuildRunner = Join-Path $BuildRoot 'InvokeAvidScriptAndroidBuildCookRun.ps1'
$DeviceRunner = Join-Path $BuildRoot 'InvokeAvidScriptAndroidScenario.ps1'
$StartupProbeRunner = Join-Path $BuildRoot 'InvokeAvidScriptStartupScenarioProbe.ps1'
$PowerShellPath = Join-Path $PSHOME 'pwsh.exe'
$Passed = 0
$Total = 0
$Failures = [Collections.Generic.List[string]]::new()

function Assert-AndroidRunnerContract {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-AndroidRunnerCase {
    param([string]$Name, [scriptblock]$Body)
    ++$script:Total
    try { & $Body; ++$script:Passed }
    catch { $script:Failures.Add("$Name`: $($_.Exception.Message)") }
}

. $BuildRunner
. $DeviceRunner

function New-AndroidIdentityComponent {
    return [pscustomobject]@{
        module_id = 'fixture.module'; package_id = ('a' * 64); resolved_from_package = $true
        runtime_loaded = $true; begin_play = $true; ticks = 5; events = 1
        dropped_gameplay_events = 0; last_error = ''
    }
}

function Invoke-AndroidIdentityFixture {
    param([object[]]$Components, [string]$ExpectedPackageId = ('a' * 64))

    $Root = Join-Path ([IO.Path]::GetTempPath()) "AvidScriptAndroidIdentity_$([Guid]::NewGuid().ToString('N'))"
    [void][IO.Directory]::CreateDirectory($Root)
    try {
        $AdbPath = $PowerShellPath
        $AaptPath = $PowerShellPath
        $Mode = 'Run'
        $Configuration = 'Development'
        $DeviceId = 'one'
        $PackageName = 'com.fixture.game'
        $Activity = 'com.epicgames.unreal.GameActivity'
        $ScenarioId = 'fixture'
        $ModuleId = 'fixture.module'
        $Map = '/Game/Fixture'
        $EventIds = '64001'
        $TimeoutSeconds = 10
        $ApkPath = Join-Path $Root 'fixture.apk'
        $ObbPaths = @(Join-Path $Root 'main.17.com.fixture.game.obb')
        $OutputRoot = Join-Path $Root 'Reports'
        [IO.File]::WriteAllText($ApkPath, 'APK fixture; native ABI is covered separately.')
        [IO.File]::WriteAllText($ObbPaths[0], 'Previous package payload with the same application version.')
        $Context = @{ run_id = ''; commands = [Collections.Generic.List[string]]::new() }
        function Get-AvidScriptAndroidApkIdentity {
            param([string]$Path)
            return [pscustomobject]@{ sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
        }
        function Invoke-AvidScriptAndroidProcess {
            param($Executable, $Arguments, $WorkingDirectory, $TimeoutSeconds)
            $Context.commands.Add(($Arguments -join ' '))
            if ($Arguments[0] -ceq 'devices') {
                $Stdout = 'one device'
            }
            elseif ($Arguments[0] -ceq 'dump') {
                $Stdout = "package: name='com.fixture.game' versionCode='17'"
            }
            elseif ($Arguments[2] -ceq 'install') {
                $Stdout = 'Success'
            }
            elseif ($Arguments[2] -ceq 'push' -or
                ($Arguments[2] -ceq 'shell' -and $Arguments[3] -ceq 'mkdir')) {
                $Stdout = ''
            }
            elseif ($Arguments[2] -ceq 'shell' -and $Arguments[3] -ceq 'am') {
                $RunMatch = [regex]::Match($Arguments[-1], 'AvidScriptScenarioProbeRunId=([0-9a-f]{32})')
                Assert-AndroidRunnerContract $RunMatch.Success 'Launch did not provide a fresh run ID.'
                $Context.run_id = $RunMatch.Groups[1].Value
                $Stdout = 'Status: ok'
            }
            elseif ($Arguments[2] -ceq 'logcat') {
                $Report = [ordered]@{
                    schema_version = 1; run_id = $Context.run_id
                    result = 'avidscript_startup_scenario_probe_passed'; scenario_id = $ScenarioId
                    events_requested = 1; events_dispatched = 1; components = @($Components)
                }
                $Stdout = 'AVIDSCRIPT_STARTUP_SCENARIO_PROBE ' + ($Report | ConvertTo-Json -Depth 16 -Compress)
            }
            else {
                throw "Unexpected fixture process arguments: $($Arguments -join ' ')"
            }
            return [pscustomobject]@{ exit_code = 0; stdout = $Stdout; stderr = '' }
        }
        $Outcome = $null
        $Failure = ''
        try { $Outcome = Invoke-AvidScriptAndroidScenario }
        catch { $Failure = $_.Exception.Message }
        return [pscustomobject]@{
            outcome = $Outcome; failure = $Failure; run_id = $Context.run_id
            commands = @($Context.commands); report_written = (Test-Path -LiteralPath $OutputRoot)
        }
    }
    finally {
        $FullRoot = [IO.Path]::GetFullPath($Root)
        $TempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd(
            [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
        if (-not $FullRoot.StartsWith($TempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Android identity fixture cleanup escaped the temporary root.'
        }
        if (Test-Path -LiteralPath $FullRoot) { Remove-Item -LiteralPath $FullRoot -Recurse -Force }
    }
}

Invoke-AndroidRunnerCase 'PowerShell syntax' {
    foreach ($Path in @($BuildRunner, $DeviceRunner, $StartupProbeRunner, (Join-Path $BuildRoot 'Android/AvidScriptAndroidProcess.ps1'))) {
        $Errors = $null
        [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$null, [ref]$Errors) | Out-Null
        Assert-AndroidRunnerContract ($Errors.Count -eq 0) "Parse failed: $Path"
    }
}

Invoke-AndroidRunnerCase 'Android arm64 no-clean UAT arguments' {
    $Arguments = New-AvidScriptAndroidUatArguments -ProjectFile 'C:\Fixture Project\Game.uproject' `
        -TargetName Game -Configuration Shipping -ArchiveRoot 'C:\Fixture Project\Saved\Archive'
    foreach ($Expected in @('-targetplatform=Android', '-architectures=arm64', '-cookflavor=ASTC', '-clientconfig=Shipping', '-skipbuildeditor', '-package')) {
        Assert-AndroidRunnerContract ($Arguments -contains $Expected) "Missing UAT argument $Expected"
    }
    Assert-AndroidRunnerContract (-not ($Arguments -contains '-clean')) 'UAT must not clean the Editor target.'
}

Invoke-AndroidRunnerCase 'ADB device state parsing' {
    $Devices = @(ConvertFrom-AvidScriptAdbDevices "List of devices attached`nserial1 device product:test`nserial2 unauthorized`nserial3 offline`n* daemon started successfully *")
    Assert-AndroidRunnerContract ($Devices.Count -eq 3) 'ADB device count is invalid.'
    Assert-AndroidRunnerContract (@($Devices | Where-Object state -ceq 'device').Count -eq 1) 'Unauthorized/offline devices were accepted.'
}

Invoke-AndroidRunnerCase 'APK native ABI is validated before installation' {
    $Path = Join-Path ([IO.Path]::GetTempPath()) "$([Guid]::NewGuid().ToString('N')).apk"
    try {
        $Archive = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Create)
        try {
            $Stream = $Archive.CreateEntry('lib/arm64-v8a/libUnreal.so').Open()
            $Header = [byte[]]::new(20)
            ([byte[]](0x7f, 0x45, 0x4c, 0x46, 2, 1)).CopyTo($Header, 0)
            $Header[18] = 183
            try { $Stream.Write($Header, 0, $Header.Length) } finally { $Stream.Dispose() }
        } finally { $Archive.Dispose() }
        $Identity = Get-AvidScriptAndroidApkIdentity -Path $Path
        Assert-AndroidRunnerContract ($Identity.architecture -ceq 'arm64-v8a') 'Valid arm64 APK was rejected.'
        $Archive = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Update)
        try { $Archive.CreateEntry('lib/x86_64/libUnreal.so') | Out-Null } finally { $Archive.Dispose() }
        $Rejected = $false
        try { Get-AvidScriptAndroidApkIdentity -Path $Path | Out-Null } catch { $Rejected = $true }
        Assert-AndroidRunnerContract $Rejected 'APK containing another native ABI was accepted.'
        $Archive = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Update)
        try {
            $Archive.GetEntry('lib/x86_64/libUnreal.so').Delete()
            $Archive.GetEntry('lib/arm64-v8a/libUnreal.so').Delete()
            $Header[18] = 62
            $Stream = $Archive.CreateEntry('lib/arm64-v8a/libUnreal.so').Open()
            try { $Stream.Write($Header, 0, $Header.Length) } finally { $Stream.Dispose() }
        } finally { $Archive.Dispose() }
        $Rejected = $false
        try { Get-AvidScriptAndroidApkIdentity -Path $Path | Out-Null } catch { $Rejected = $true }
        Assert-AndroidRunnerContract $Rejected 'Renamed x86_64 library was accepted as arm64.'
    }
    finally { if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path } }
}

Invoke-AndroidRunnerCase 'Activity arguments preserve one scoped command line' {
    $RunId = 'a' * 32
    $Arguments = @(New-AvidScriptAndroidScenarioArguments -Serial serial1 -Package com.fixture.game -ActivityName com.epicgames.unreal.GameActivity -RunId $RunId)
    Assert-AndroidRunnerContract ($Arguments[0] -ceq '-s' -and $Arguments[1] -ceq 'serial1') 'Device serial was not scoped.'
    Assert-AndroidRunnerContract ($Arguments[-1].StartsWith("'") -and $Arguments[-1].EndsWith("'")) 'Remote cmdline is not one quoted argument.'
    Assert-AndroidRunnerContract ($Arguments[-1].Contains("-AvidScriptScenarioProbeRunId=$RunId")) 'Probe run ID is missing.'
}

Invoke-AndroidRunnerCase 'Old and malformed report isolation' {
    $RunId = 'b' * 32
    $Old = @{ run_id = ('a' * 32); result = 'old' } | ConvertTo-Json -Compress
    $Current = @{ run_id = $RunId; result = 'current' } | ConvertTo-Json -Compress
    $Text = "AVIDSCRIPT_STARTUP_SCENARIO_PROBE invalid`nAVIDSCRIPT_STARTUP_SCENARIO_PROBE $Old`nAVIDSCRIPT_STARTUP_SCENARIO_PROBE $Current"
    $Report = Find-AvidScriptAndroidScenarioReport -Text $Text -RunId $RunId
    Assert-AndroidRunnerContract ($Report.result -ceq 'current') 'An unrelated or malformed report was accepted.'
}

Invoke-AndroidRunnerCase 'Missing ADB is not_run without process execution' {
    $AdbPath = Join-Path ([IO.Path]::GetTempPath()) "$([Guid]::NewGuid().ToString('N')).missing"
    $Mode = 'Run'
    $ExpectedPackageId = 'a' * 64
    function Invoke-AvidScriptAndroidProcess { throw 'No process should run.' }
    $Result = Invoke-AvidScriptAndroidScenario
    Assert-AndroidRunnerContract ($Result.status -ceq 'not_run' -and $Result.reason -ceq 'adb_missing') 'Missing ADB was mislabeled.'
}

Invoke-AndroidRunnerCase 'Multiple devices require explicit selection' {
    $AdbPath = $PowerShellPath
    $DeviceId = ''
    $Mode = 'Run'
    $ExpectedPackageId = 'a' * 64
    function Invoke-AvidScriptAndroidProcess {
        param($Executable, $Arguments, $WorkingDirectory, $TimeoutSeconds)
        Assert-AndroidRunnerContract ($Arguments[0] -ceq 'devices') 'Ambiguous device selection started another process.'
        return [pscustomobject]@{ exit_code = 0; stdout = "one device`ntwo device"; stderr = '' }
    }
    $Result = Invoke-AvidScriptAndroidScenario
    Assert-AndroidRunnerContract ($Result.status -ceq 'not_run' -and $Result.reason -ceq 'device_selection_required') 'Multiple devices were not rejected.'
}

Invoke-AndroidRunnerCase 'Shipping does not pretend to accept development cmdline injection' {
    $AdbPath = $PowerShellPath
    $DeviceId = ''
    $Mode = 'Run'
    $ExpectedPackageId = 'a' * 64
    $Configuration = 'Shipping'
    function Invoke-AvidScriptAndroidProcess {
        param($Executable, $Arguments, $WorkingDirectory, $TimeoutSeconds)
        Assert-AndroidRunnerContract ($Arguments[0] -ceq 'devices') 'Shipping unsupported transport installed or launched an app.'
        return [pscustomobject]@{ exit_code = 0; stdout = 'one device'; stderr = '' }
    }
    $Result = Invoke-AvidScriptAndroidScenario
    Assert-AndroidRunnerContract ($Result.status -ceq 'not_run' -and $Result.reason -ceq 'shipping_requires_embedded_commandline_and_report_transport') 'Shipping transport limitation was hidden.'
}

Invoke-AndroidRunnerCase 'Missing SDK blocks UAT before any build' {
    $Mode = 'BuildCookRun'
    function Invoke-AvidScriptAndroidProcess {
        param($Executable, $Arguments, $WorkingDirectory, $TimeoutSeconds)
        Assert-AndroidRunnerContract ($Arguments -contains (Join-Path $BuildRoot 'TestAvidScriptAndroidToolchain.ps1')) 'UAT ran without a ready SDK.'
        return [pscustomobject]@{ exit_code = 2; stdout = '{"ready":false,"status":"blocked","toolchain":{}}'; stderr = '' }
    }
    $Result = Invoke-AvidScriptAndroidBuildCookRun
    Assert-AndroidRunnerContract ($Result.status -ceq 'not_run' -and $Result.build_cook_run -ceq 'not_run') 'Missing SDK was mislabeled as a build pass.'
}

Invoke-AndroidRunnerCase 'Run requires a valid expected package before any device process' {
    $AdbPath = $PowerShellPath
    $Mode = 'Run'
    function Invoke-AvidScriptAndroidProcess { throw 'Device process ran before input validation.' }
    foreach ($Invalid in @('', ('a' * 63), ('a' * 65), ('g' * 64), (('a' * 64) + "`n"))) {
        $ExpectedPackageId = $Invalid
        $Rejected = $false
        try { Invoke-AvidScriptAndroidScenario | Out-Null }
        catch { $Rejected = $_.Exception.Message.Contains('Run mode requires ExpectedPackageId') }
        Assert-AndroidRunnerContract $Rejected 'Missing or malformed expected package reached a device operation.'
    }
}

Invoke-AndroidRunnerCase 'Inspect remains compatible without an expected package' {
    $AdbPath = $PowerShellPath
    $DeviceId = ''
    $Mode = 'inspect'
    $ExpectedPackageId = ''
    function Invoke-AvidScriptAndroidProcess {
        param($Executable, $Arguments, $WorkingDirectory, $TimeoutSeconds)
        Assert-AndroidRunnerContract ($Arguments[0] -ceq 'devices') 'Inspect attempted installation or launch.'
        return [pscustomobject]@{ exit_code = 0; stdout = 'one device'; stderr = '' }
    }
    $Result = Invoke-AvidScriptAndroidScenario
    Assert-AndroidRunnerContract ($Result.status -ceq 'ok' -and $Result.reason -ceq 'inspect_only') 'Inspect now requires a package ID.'
}

Invoke-AndroidRunnerCase 'Current packaged module identity passes and normalizes expected hex' {
    $Fixture = Invoke-AndroidIdentityFixture -Components @(New-AndroidIdentityComponent) -ExpectedPackageId ('A' * 64)
    Assert-AndroidRunnerContract ($Fixture.failure -ceq '' -and $null -ne $Fixture.outcome) "Matching package failed: $($Fixture.failure)"
    Assert-AndroidRunnerContract ($Fixture.outcome.status -ceq 'ok' -and $Fixture.report_written) 'Matching package report was not published.'
    Assert-AndroidRunnerContract ($Fixture.outcome.package_id -ceq ('a' * 64) -and
        $Fixture.outcome.expected_package_id -ceq ('a' * 64)) 'The accepted package identity was not preserved.'
}

Invoke-AndroidRunnerCase 'Old OBB with a new run ID cannot pass expected package identity' {
    $Component = New-AndroidIdentityComponent
    $Component.package_id = 'b' * 64
    $Fixture = Invoke-AndroidIdentityFixture -Components @($Component)
    Assert-AndroidRunnerContract ($Fixture.run_id -cmatch '^[0-9a-f]{32}$') 'The stale package fixture did not use a fresh run ID.'
    Assert-AndroidRunnerContract (@($Fixture.commands | Where-Object { $_ -match ' push ' }).Count -eq 1) 'The OBB fixture did not reach publication.'
    Assert-AndroidRunnerContract ($Fixture.failure.Contains('runtime identity') -and $null -eq $Fixture.outcome -and
        -not $Fixture.report_written) 'A previous OBB package was accepted merely because its run ID was current.'
}

foreach ($InvalidReport in @('unpackaged', 'string provenance', 'missing package', 'missing provenance', 'duplicate module', 'wrong module')) {
    Invoke-AndroidRunnerCase "Reject $InvalidReport component identity" {
        $Component = New-AndroidIdentityComponent
        $Components = @($Component)
        switch ($InvalidReport) {
            'unpackaged' { $Component.resolved_from_package = $false }
            'string provenance' { $Component.resolved_from_package = 'true' }
            'missing package' { $Component.PSObject.Properties.Remove('package_id') }
            'missing provenance' { $Component.PSObject.Properties.Remove('resolved_from_package') }
            'duplicate module' { $Components += New-AndroidIdentityComponent }
            'wrong module' { $Component.module_id = 'another.module' }
        }
        $Fixture = Invoke-AndroidIdentityFixture -Components $Components
        Assert-AndroidRunnerContract ($Fixture.failure -cne '' -and $null -eq $Fixture.outcome -and
            -not $Fixture.report_written) "Invalid component identity passed: $InvalidReport"
    }
}

Invoke-AndroidRunnerCase 'Native Startup probe reports observed package identity' {
    $ProbeSourcePath = Join-Path (Split-Path -Parent $BuildRoot) `
        'Source/AvidScriptRuntime/Private/Shipping/AvidScriptStartupScenarioProbe.cpp'
    $ProbeSource = Get-Content -Raw -LiteralPath $ProbeSourcePath
    Assert-AndroidRunnerContract ($ProbeSource.Contains('SetStringField(TEXT("package_id"), Stats.PackageId)')) 'Probe does not report the actual Runtime package ID.'
    Assert-AndroidRunnerContract ($ProbeSource.Contains('SetBoolField(TEXT("resolved_from_package"), Stats.bResolvedFromPackage)')) 'Probe does not report package resolution provenance.'
}

Invoke-AndroidRunnerCase 'Native Startup probe timing accommodates respawn and maximum events' {
    $ProbeSourcePath = Join-Path (Split-Path -Parent $BuildRoot) `
        'Source/AvidScriptRuntime/Private/Shipping/AvidScriptStartupScenarioProbe.cpp'
    $ProbeSource = Get-Content -Raw -LiteralPath $ProbeSourcePath
    $Timing = @{}
    foreach ($Name in @('ProbeTimeoutSeconds', 'FirstEventSeconds', 'EventIntervalSeconds', 'CompletionDelaySeconds', 'MaximumProbeEvents')) {
        $Match = [regex]::Match($ProbeSource, "constexpr (?:float|int32) $Name = ([0-9.]+)f?;")
        Assert-AndroidRunnerContract $Match.Success "Probe timing constant is missing: $Name"
        $Timing[$Name] = [double]::Parse($Match.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
    }
    Assert-AndroidRunnerContract ($Timing.EventIntervalSeconds -eq 0.4) 'Probe events must allow the 0.35 second pickup respawn to complete.'
    Assert-AndroidRunnerContract ($Timing.ProbeTimeoutSeconds -ge 30 -and $Timing.MaximumProbeEvents -eq 64) 'Probe timeout does not accommodate the supported 64 event limit.'
    $MaximumDuration = $Timing.FirstEventSeconds + $Timing.EventIntervalSeconds * $Timing.MaximumProbeEvents + $Timing.CompletionDelaySeconds
    $DefaultDuration = $Timing.FirstEventSeconds + $Timing.EventIntervalSeconds * 5 + $Timing.CompletionDelaySeconds
    Assert-AndroidRunnerContract ($MaximumDuration -lt $Timing.ProbeTimeoutSeconds) 'The maximum event sequence cannot finish before the probe timeout.'
    Assert-AndroidRunnerContract ($DefaultDuration -lt 20) 'The default five-event sequence exceeds the 20 second round budget.'
}

Invoke-AndroidRunnerCase 'Standalone Startup probe forwards every default parameter' {
    $MissingArchive = Join-Path ([IO.Path]::GetTempPath()) "AvidScriptProbeMissing_$([Guid]::NewGuid().ToString('N'))"
    $Process = Invoke-AvidScriptAndroidProcess -Executable $PowerShellPath -WorkingDirectory $BuildRoot -TimeoutSeconds 30 `
        -Arguments @('-NoProfile', '-NonInteractive', '-File', $StartupProbeRunner,
            '-ArchiveRoot', $MissingArchive, '-TargetName', 'Fixture', '-ScenarioId', 'fixture', '-ModuleId', 'fixture.module')
    Assert-AndroidRunnerContract ($Process.exit_code -eq 1) 'Missing archive did not fail in the standalone probe.'
    $Failure = $Process.stdout | ConvertFrom-Json -Depth 16
    Assert-AndroidRunnerContract ($Failure.category -ceq 'archive_root_missing' -and
        $Failure.configuration -ceq 'Development' -and $Failure.scenario_id -ceq 'fixture' -and
        $Failure.module_id -ceq 'fixture.module') 'Standalone defaults were lost before archive validation.'
}

Invoke-AndroidRunnerCase 'Standalone Startup probe preserves explicit parameter overrides' {
    $MissingArchive = Join-Path ([IO.Path]::GetTempPath()) "AvidScriptProbeMissing_$([Guid]::NewGuid().ToString('N'))"
    $Process = Invoke-AvidScriptAndroidProcess -Executable $PowerShellPath -WorkingDirectory $BuildRoot -TimeoutSeconds 30 `
        -Arguments @('-NoProfile', '-NonInteractive', '-File', $StartupProbeRunner,
            '-ArchiveRoot', $MissingArchive, '-TargetName', 'Fixture', '-ScenarioId', 'fixture', '-ModuleId', 'fixture.module',
            '-EventIds', '7,8', '-Map', 'invalid-map', '-Configuration', 'Shipping', '-TimeoutSeconds', '10')
    Assert-AndroidRunnerContract ($Process.exit_code -eq 1) 'Invalid explicit map did not fail.'
    $Failure = $Process.stdout | ConvertFrom-Json -Depth 16
    Assert-AndroidRunnerContract ($Failure.category -ceq 'map_invalid' -and $Failure.configuration -ceq 'Shipping') 'Standalone overrides were replaced by defaults.'
}

$Result = [ordered]@{
    result = if ($Failures.Count -eq 0) { 'avidscript_android_runner_contracts_passed' } else { 'avidscript_android_runner_contracts_failed' }
    passed = $Passed; total = $Total; failures = @($Failures)
}
[Console]::Out.WriteLine(($Result | ConvertTo-Json -Depth 8 -Compress))
if ($Failures.Count -ne 0) { exit 1 }
exit 0
