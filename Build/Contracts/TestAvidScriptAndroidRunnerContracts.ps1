Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent $PSScriptRoot
$BuildRunner = Join-Path $BuildRoot 'InvokeAvidScriptAndroidBuildCookRun.ps1'
$DeviceRunner = Join-Path $BuildRoot 'InvokeAvidScriptAndroidScenario.ps1'
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

Invoke-AndroidRunnerCase 'PowerShell syntax' {
    foreach ($Path in @($BuildRunner, $DeviceRunner, (Join-Path $BuildRoot 'Android/AvidScriptAndroidProcess.ps1'))) {
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
    function Invoke-AvidScriptAndroidProcess { throw 'No process should run.' }
    $Result = Invoke-AvidScriptAndroidScenario
    Assert-AndroidRunnerContract ($Result.status -ceq 'not_run' -and $Result.reason -ceq 'adb_missing') 'Missing ADB was mislabeled.'
}

Invoke-AndroidRunnerCase 'Multiple devices require explicit selection' {
    $AdbPath = $PowerShellPath
    $DeviceId = ''
    $Mode = 'Run'
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

$Result = [ordered]@{
    result = if ($Failures.Count -eq 0) { 'avidscript_android_runner_contracts_passed' } else { 'avidscript_android_runner_contracts_failed' }
    passed = $Passed; total = $Total; failures = @($Failures)
}
[Console]::Out.WriteLine(($Result | ConvertTo-Json -Depth 8 -Compress))
if ($Failures.Count -ne 0) { exit 1 }
exit 0
