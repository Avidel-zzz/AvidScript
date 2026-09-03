[CmdletBinding()]
param(
    [ValidateSet('Inspect', 'Run')][string]$Mode = 'Inspect',
    [ValidateSet('Development', 'Shipping')][string]$Configuration = 'Development',
    [string]$AdbPath = '',
    [string]$AaptPath = '',
    [string]$DeviceId = '',
    [string]$ApkPath = '',
    [string[]]$ObbPaths = @(),
    [string]$PackageName = '',
    [string]$Activity = 'com.epicgames.unreal.GameActivity',
    [string]$ScenarioId = 'pickup_rush',
    [string]$ModuleId = 'avidscript.pickup_rush',
    [string]$Map = '/Game/TopDown/Lvl_TopDown',
    [string]$EventIds = '64001,64001,64001,64001,64001',
    [string]$OutputRoot = '',
    [ValidateRange(10, 600)][int]$TimeoutSeconds = 90,
    [string]$ExpectedPackageId = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$BuildRoot = $PSScriptRoot
$PluginRoot = Split-Path -Parent $BuildRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
. (Join-Path $BuildRoot 'Android/AvidScriptAndroidProcess.ps1')

function ConvertFrom-AvidScriptAdbDevices {
    param([string]$Text)
    foreach ($Line in $Text -split '\r?\n') {
        if ($Line -match '^(?<serial>[A-Za-z0-9_.:-]+)\s+(?<state>device|offline|unauthorized)(?:\s|$)') {
            [pscustomobject]@{ serial = $Matches.serial; state = $Matches.state }
        }
    }
}

function New-AvidScriptAndroidScenarioArguments {
    param([string]$Serial, [string]$Package, [string]$ActivityName, [string]$RunId)
    $CommandLine = "$Map -AvidScriptScenario=$ScenarioId -AvidScriptScenarioProbeRunId=$RunId -AvidScriptScenarioProbeEvents=$EventIds"
    return @('-s', $Serial, 'shell', 'am', 'start', '-S', '-W', '-n', "$Package/$ActivityName", '--es', 'cmdline', "'$CommandLine'")
}

function Find-AvidScriptAndroidScenarioReport {
    param([string]$Text, [string]$RunId)
    foreach ($Line in $Text -split '\r?\n') {
        $Marker = 'AVIDSCRIPT_STARTUP_SCENARIO_PROBE '
        $Index = $Line.IndexOf($Marker, [StringComparison]::Ordinal)
        if ($Index -lt 0) { continue }
        try { $Report = $Line.Substring($Index + $Marker.Length) | ConvertFrom-Json -Depth 64 }
        catch { continue }
        if ($null -ne $Report.PSObject.Properties['run_id'] -and $Report.run_id -ceq $RunId) { return $Report }
    }
    return $null
}

function Invoke-AvidScriptAndroidScenario {
    $CanonicalExpectedPackageId = ''
    if ($Mode -ieq 'Run') {
        if ($ExpectedPackageId -notmatch '\A[0-9a-f]{64}\z') {
            throw 'Run mode requires ExpectedPackageId to be a 64-hex package identity.'
        }
        $CanonicalExpectedPackageId = $ExpectedPackageId.ToLowerInvariant()
    }
    $ResolvedAdb = $AdbPath
    if ([string]::IsNullOrWhiteSpace($ResolvedAdb)) {
        $Command = Get-Command adb.exe -ErrorAction SilentlyContinue
        if ($null -ne $Command) { $ResolvedAdb = $Command.Source }
        elseif (-not [string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) {
            $ResolvedAdb = Join-Path $env:ANDROID_HOME 'platform-tools/adb.exe'
        }
    }
    $Result = [ordered]@{
        schema_version = 1; result = 'avidscript_android_scenario_not_run'; status = 'not_run'
        reason = 'adb_missing'; configuration = $Configuration; devices = @(); device_id = ''; report = $null
    }
    if ([string]::IsNullOrWhiteSpace($ResolvedAdb) -or -not (Test-Path -LiteralPath $ResolvedAdb -PathType Leaf)) {
        return [pscustomobject]$Result
    }
    $ResolvedAdb = (Resolve-Path -LiteralPath $ResolvedAdb).Path
    $DevicesProcess = Invoke-AvidScriptAndroidProcess -Executable $ResolvedAdb `
        -Arguments @('devices', '-l') -WorkingDirectory $PluginRoot -TimeoutSeconds 30
    if ($DevicesProcess.exit_code -ne 0) { throw 'ADB device enumeration failed.' }
    $Devices = @(ConvertFrom-AvidScriptAdbDevices $DevicesProcess.stdout)
    $Result.devices = $Devices
    $ReadyDevices = @($Devices | Where-Object { $_.state -ceq 'device' -and ([string]::IsNullOrWhiteSpace($DeviceId) -or $_.serial -ceq $DeviceId) })
    if ($ReadyDevices.Count -ne 1) {
        $Result.reason = if ($ReadyDevices.Count -gt 1) { 'device_selection_required' } else { 'device_unavailable' }
        return [pscustomobject]$Result
    }
    $Serial = $ReadyDevices[0].serial
    $Result.device_id = $Serial
    if ($Mode -ieq 'Inspect') {
        $Result.result = 'avidscript_android_device_ready'; $Result.status = 'ok'; $Result.reason = 'inspect_only'
        return [pscustomobject]$Result
    }
    if ($Configuration -ieq 'Shipping') {
        $Result.reason = 'shipping_requires_embedded_commandline_and_report_transport'
        return [pscustomobject]$Result
    }
    if ($ScenarioId -cnotmatch '^[a-z][a-z0-9_.-]{0,63}$' -or $ModuleId -cnotmatch '^[a-z][a-z0-9_.-]{0,63}$' -or
        $Map -cnotmatch '^/Game/[A-Za-z0-9_/]+$' -or $Activity -cnotmatch '^[A-Za-z_][A-Za-z0-9_.]*$') {
        throw 'Scenario, module, map or Activity identity is invalid.'
    }
    $Events = @($EventIds.Split(','))
    if ($Events.Count -eq 0 -or $Events.Count -gt 64) { throw 'Expected 1-64 event IDs.' }
    foreach ($Event in $Events) {
        $Value = 0
        if (-not [int]::TryParse($Event, [ref]$Value) -or $Value -le 0 -or $Event -cnotmatch '^[0-9]+$') {
            throw 'Event IDs must be positive Int32 values.'
        }
    }
    if (-not (Test-Path -LiteralPath $ApkPath -PathType Leaf) -or [IO.Path]::GetExtension($ApkPath) -cne '.apk') {
        throw 'Run mode requires an existing APK.'
    }
    $ResolvedApk = (Resolve-Path -LiteralPath $ApkPath).Path
    $ApkIdentity = Get-AvidScriptAndroidApkIdentity -Path $ResolvedApk
    $ResolvedAapt = if ([string]::IsNullOrWhiteSpace($AaptPath)) {
        Join-Path (Split-Path -Parent (Split-Path -Parent $ResolvedAdb)) 'build-tools/35.0.1/aapt.exe'
    } else { $AaptPath }
    if (-not (Test-Path -LiteralPath $ResolvedAapt -PathType Leaf)) { throw 'aapt is required to verify APK package identity before installation.' }
    $Badging = Invoke-AvidScriptAndroidProcess -Executable $ResolvedAapt -Arguments @('dump', 'badging', $ResolvedApk) `
        -WorkingDirectory $PluginRoot -TimeoutSeconds 30
    $PackageMatch = [regex]::Match($Badging.stdout, "(?m)^package: name='(?<name>[A-Za-z_][A-Za-z0-9_.]+)' versionCode='(?<version>[0-9]+)'")
    if ($Badging.exit_code -ne 0 -or -not $PackageMatch.Success) { throw 'APK package identity could not be verified.' }
    $ResolvedPackage = $PackageMatch.Groups['name'].Value
    $VersionCode = $PackageMatch.Groups['version'].Value
    if (-not [string]::IsNullOrWhiteSpace($PackageName) -and $PackageName -cne $ResolvedPackage) {
        throw 'APK package name does not match the requested package.'
    }
    $ValidatedObbs = foreach ($ObbPath in $ObbPaths) {
        $Name = [IO.Path]::GetFileName($ObbPath)
        if (-not (Test-Path -LiteralPath $ObbPath -PathType Leaf) -or
            $Name -cnotmatch ('^(main|patch)\.' + $VersionCode + '\.' + [regex]::Escape($ResolvedPackage) + '\.obb$')) {
            throw 'OBB does not match the verified APK package and version.'
        }
        [pscustomobject]@{ path = (Resolve-Path -LiteralPath $ObbPath).Path; name = $Name }
    }
    if ((Get-FileHash -LiteralPath $ResolvedApk -Algorithm SHA256).Hash.ToLowerInvariant() -cne $ApkIdentity.sha256) {
        throw 'APK changed after package verification.'
    }
    $Install = Invoke-AvidScriptAndroidProcess -Executable $ResolvedAdb -Arguments @('-s', $Serial, 'install', '-r', $ResolvedApk) `
        -WorkingDirectory $PluginRoot -TimeoutSeconds 180
    if ($Install.exit_code -ne 0 -or $Install.stdout -notmatch '(?m)^Success\s*$') { throw 'ADB APK installation failed.' }
    foreach ($Obb in $ValidatedObbs) {
        $RemoteRoot = "/sdcard/Android/obb/$ResolvedPackage"
        $Mkdir = Invoke-AvidScriptAndroidProcess -Executable $ResolvedAdb -Arguments @('-s', $Serial, 'shell', 'mkdir', '-p', $RemoteRoot) `
            -WorkingDirectory $PluginRoot -TimeoutSeconds 30
        if ($Mkdir.exit_code -ne 0) { throw 'ADB OBB directory creation failed.' }
        $Push = Invoke-AvidScriptAndroidProcess -Executable $ResolvedAdb -Arguments @('-s', $Serial, 'push', $Obb.path, "$RemoteRoot/$($Obb.name)") `
            -WorkingDirectory $PluginRoot -TimeoutSeconds 300
        if ($Push.exit_code -ne 0) { throw 'ADB OBB publication failed.' }
    }

    $RunId = [Guid]::NewGuid().ToString('N')
    $Launch = Invoke-AvidScriptAndroidProcess -Executable $ResolvedAdb `
        -Arguments (New-AvidScriptAndroidScenarioArguments -Serial $Serial -Package $ResolvedPackage -ActivityName $Activity -RunId $RunId) `
        -WorkingDirectory $PluginRoot -TimeoutSeconds 30
    if ($Launch.exit_code -ne 0 -or $Launch.stdout -match '(?m)^Error:') { throw 'Android Activity launch failed.' }
    $Clock = [Diagnostics.Stopwatch]::StartNew()
    $Report = $null
    while ($Clock.Elapsed.TotalSeconds -lt $TimeoutSeconds -and $null -eq $Report) {
        $Log = Invoke-AvidScriptAndroidProcess -Executable $ResolvedAdb `
            -Arguments @('-s', $Serial, 'logcat', '-d', '-v', 'raw', '-t', '2000', '-s', 'UE:D', '*:S') `
            -WorkingDirectory $PluginRoot -TimeoutSeconds 15
        if ($Log.exit_code -ne 0) { throw 'ADB could not read the UE report stream.' }
        $Report = Find-AvidScriptAndroidScenarioReport -Text $Log.stdout -RunId $RunId
        if ($null -eq $Report) { Start-Sleep -Milliseconds 1000 }
    }
    if ($null -eq $Report) { throw 'Android scenario report timed out; no result is accepted from an earlier run.' }
    $Components = @($Report.components | Where-Object { $_.module_id -ceq $ModuleId })
    if ($Report.schema_version -ne 1 -or
        $Report.result -cne 'avidscript_startup_scenario_probe_passed' -or $Report.scenario_id -cne $ScenarioId -or
        $Report.events_requested -ne $Events.Count -or $Report.events_dispatched -ne $Events.Count -or $Components.Count -ne 1 -or
        $Components[0].package_id -cne $CanonicalExpectedPackageId -or
        $Components[0].resolved_from_package -isnot [bool] -or -not $Components[0].resolved_from_package -or
        -not $Components[0].runtime_loaded -or -not $Components[0].begin_play -or $Components[0].ticks -le 0 -or
        $Components[0].dropped_gameplay_events -ne 0 -or $Components[0].last_error -cne '') {
        throw 'Android scenario report did not satisfy the requested runtime identity and lifecycle.'
    }
    $Result.result = 'avidscript_android_scenario_passed'; $Result.status = 'ok'; $Result.reason = ''
    $Result.report = $Report
    $Result['run_id'] = $RunId
    $Result['package_name'] = $ResolvedPackage
    $Result['expected_package_id'] = $CanonicalExpectedPackageId
    $Result['package_id'] = [string]$Components[0].package_id
    $Result['apk_sha256'] = $ApkIdentity.sha256
    $Result['process_exit'] = 'not_observable_via_activity_manager'
    $ResolvedOutputRoot = if ([string]::IsNullOrWhiteSpace($OutputRoot)) { Join-Path $ProjectRoot 'Saved/AvidScript/Android/DeviceReports' } else { [IO.Path]::GetFullPath($OutputRoot) }
    [void][IO.Directory]::CreateDirectory($ResolvedOutputRoot)
    $Result['report_path'] = Join-Path $ResolvedOutputRoot "$RunId.json"
    [IO.File]::WriteAllText($Result.report_path, ($Result | ConvertTo-Json -Depth 100), [Text.UTF8Encoding]::new($false))
    return [pscustomobject]$Result
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        $Result = Invoke-AvidScriptAndroidScenario
        [Console]::Out.WriteLine(($Result | ConvertTo-Json -Depth 100 -Compress))
        if ($Result.status -ceq 'not_run') { exit 2 }
        exit 0
    }
    catch {
        [Console]::Out.WriteLine((@{
            schema_version = 1; result = 'avidscript_android_scenario_failed'; status = 'error'; message = $_.Exception.Message
        } | ConvertTo-Json -Compress))
        exit 1
    }
}
