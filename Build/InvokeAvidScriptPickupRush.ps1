[CmdletBinding()]
param(
    [ValidateSet('Editor', 'BuildCookRun', 'Run', 'Play')]
    [string]$Mode = 'Editor',
    [ValidateSet('Development', 'Shipping')]
    [string]$Configuration = 'Development',
    [string]$ArchiveRoot = '',
    [string]$BindingPackagePath = '',
    [string]$DotNetPath = 'C:\Users\12159\.dotnet\dotnet.exe',
    [string]$EngineRoot = 'C:\UnrealEngine',
    [ValidateRange(30, 1800)]
    [int]$TimeoutSeconds = 300
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$TargetName = Split-Path -Leaf $ProjectRoot
$ProjectFile = Join-Path $ProjectRoot "$TargetName.uproject"
$EvidenceRoot = Join-Path $ProjectRoot 'Saved/AvidScript/PickupRush'
$ScenarioId = 'pickup_rush'
$ModuleId = 'avidscript.pickup_rush'
$EventIds = '64001,64001,64001,64001,64001'
$Map = '/Game/TopDown/Lvl_TopDown'
. (Join-Path $BuildRoot 'AvidScriptCSharpBindingPackage.ps1')

function Throw-AvidScriptPickupRushError {
    param(
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $Exception = [System.InvalidOperationException]::new($Message)
    $Exception.Data['category'] = $Category
    throw $Exception
}

function Invoke-AvidScriptPickupRushProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $Executable
    $StartInfo.WorkingDirectory = $WorkingDirectory
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Argument in $Arguments) {
        [void]$StartInfo.ArgumentList.Add($Argument)
    }
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    try {
        if (-not $Process.Start()) {
            Throw-AvidScriptPickupRushError -Category 'process_launch_failed' -Message "Process did not start: $Executable"
        }
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
            try {
                $Process.Kill($true)
                $Process.WaitForExit()
            }
            catch {
            }
            Throw-AvidScriptPickupRushError -Category 'process_timeout' -Message "Process exceeded $TimeoutSeconds seconds: $Executable"
        }
        return [pscustomobject]@{
            ExitCode = $Process.ExitCode
            Stdout = $StdoutTask.GetAwaiter().GetResult()
            Stderr = $StderrTask.GetAwaiter().GetResult()
        }
    }
    finally {
        $Process.Dispose()
    }
}

function Invoke-AvidScriptPickupRushJsonScript {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    $PowerShellPath = Join-Path $PSHOME 'pwsh.exe'
    $ProcessArguments = @('-NoProfile', '-NonInteractive', '-File', $ScriptPath) + $Arguments
    $ProcessResult = Invoke-AvidScriptPickupRushProcess `
        -Executable $PowerShellPath `
        -Arguments $ProcessArguments `
        -WorkingDirectory $ProjectRoot `
        -TimeoutSeconds $TimeoutSeconds
    $Text = $ProcessResult.Stdout.Trim()
    try {
        $Payload = $Text | ConvertFrom-Json -Depth 100 -NoEnumerate
    }
    catch {
        Throw-AvidScriptPickupRushError -Category 'child_output_invalid' -Message "Child script did not return one JSON object: $ScriptPath"
    }
    if ($ProcessResult.ExitCode -ne 0) {
        $Message = if ($null -ne $Payload.PSObject.Properties['message']) { [string]$Payload.message } else { "Child script failed with exit code $($ProcessResult.ExitCode)." }
        Throw-AvidScriptPickupRushError -Category 'child_process_failed' -Message $Message
    }
    return $Payload
}

function Publish-AvidScriptPickupRushBindings {
    if (-not [string]::IsNullOrWhiteSpace($BindingPackagePath)) {
        $ResolvedPath = if ([System.IO.Path]::IsPathRooted($BindingPackagePath)) {
            [System.IO.Path]::GetFullPath($BindingPackagePath)
        }
        else {
            [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $BindingPackagePath))
        }
        return (Resolve-AvidScriptCSharpBindingPackage -ManifestPath $ResolvedPath).ManifestPath
    }

    [void][System.IO.Directory]::CreateDirectory($EvidenceRoot)
    $LogPath = Join-Path $EvidenceRoot "PublishBindings-$PID-$([Guid]::NewGuid().ToString('N')).log"
    $EditorCmd = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
    $Arguments = @(
        $ProjectFile,
        '-unattended',
        '-nop4',
        '-nosplash',
        '-nullrhi',
        '-nosound',
        '-ExecCmds=AvidScript.PublishCSharpBindings exit',
        "-abslog=$LogPath")
    $ProcessResult = Invoke-AvidScriptPickupRushProcess `
        -Executable $EditorCmd `
        -Arguments $Arguments `
        -WorkingDirectory $ProjectRoot `
        -TimeoutSeconds $TimeoutSeconds
    if ($ProcessResult.ExitCode -ne 0 -or
        -not (Test-Path -LiteralPath $LogPath -PathType Leaf) -or
        -not ([System.IO.File]::ReadAllText($LogPath).Contains('AvidScript.PublishCSharpBindings succeeded.'))) {
        Throw-AvidScriptPickupRushError -Category 'binding_publication_failed' -Message "C# binding publication failed. See $LogPath"
    }

    $BindingRoot = Join-Path $ProjectRoot 'Saved/AvidScriptGeneratedBindings/avidscript.engine.gameplay'
    $Candidates = foreach ($ManifestFile in Get-ChildItem -LiteralPath $BindingRoot -Filter 'package.json' -File -Recurse) {
        try {
            $Package = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $ManifestFile.FullName
            if ($Package.PackageName -ceq 'avidscript.engine.gameplay') {
                [pscustomobject]@{
                    ManifestPath = $Package.ManifestPath
                    ImportCount = @($Package.RequiredImports).Count
                    LastWriteTime = $ManifestFile.LastWriteTimeUtc
                }
            }
        }
        catch {
        }
    }
    $Selected = $Candidates | Sort-Object `
        -Property @{ Expression = { $_.ImportCount }; Descending = $true },
        @{ Expression = { $_.LastWriteTime }; Descending = $true } | Select-Object -First 1
    if ($null -eq $Selected) {
        Throw-AvidScriptPickupRushError -Category 'binding_package_missing' -Message "No valid EngineGameplay binding package was found below $BindingRoot"
    }
    return [string]$Selected.ManifestPath
}

function Invoke-AvidScriptPickupRushRelease {
    param([Parameter(Mandatory = $true)][string]$ResolvedBindingPackagePath)

    $Arguments = @(
        '-SourcePath', 'Plugins/AvidScript/Samples/CSharp/PickupRush/PickupRushScript.cs',
        '-CSharpProjectPath', 'Plugins/AvidScript/Samples/CSharp/PickupRush/AvidScript.PickupRush.csproj',
        '-ModuleId', $ModuleId,
        '-ArtifactStem', 'pickup_rush',
        '-OutputRoot', 'Content/AvidScript/Modules',
        '-DotNetPath', $DotNetPath,
        '-BindingPackagePath', $ResolvedBindingPackagePath,
        '-RuntimeBindingPackagePath', $ResolvedBindingPackagePath,
        '-Configuration', $Configuration,
        '-TargetPlatform', 'Win64',
        '-EngineRoot', $EngineRoot)
    return Invoke-AvidScriptPickupRushJsonScript `
        -ScriptPath (Join-Path $BuildRoot 'InvokeAvidScriptRelease.ps1') `
        -Arguments $Arguments `
        -TimeoutSeconds $TimeoutSeconds
}

function Assert-AvidScriptPickupRushReport {
    param([Parameter(Mandatory = $true)][object]$Report)

    $Components = @($Report.components)
    if ($Report.result -cne 'avidscript_startup_scenario_probe_passed' -or
        $Report.scenario_id -cne $ScenarioId -or
        [long]$Report.events_requested -ne 5 -or
        [long]$Report.events_dispatched -ne 5 -or
        $Components.Count -ne 1) {
        Throw-AvidScriptPickupRushError -Category 'gameplay_report_invalid' -Message 'PickupRush report identity or event counts are invalid.'
    }
    $Component = $Components[0]
    $Valid = $Component.module_id -ceq $ModuleId `
        -and [bool]$Component.runtime_loaded `
        -and [bool]$Component.begin_play `
        -and [long]$Component.ticks -gt 0 `
        -and [long]$Component.events -eq 5 `
        -and [long]$Component.dropped_gameplay_events -eq 0 `
        -and [string]$Component.last_error -ceq '' `
        -and [Math]::Abs([double]$Component.location.x - 600.0) -le 0.01 `
        -and [Math]::Abs([double]$Component.location.z - 300.0) -le 0.01 `
        -and [Math]::Abs([double]$Component.scale.x - 2.5) -le 0.01 `
        -and -not [bool]$Component.hidden `
        -and [bool]$Component.collision_enabled
    if (-not $Valid) {
        Throw-AvidScriptPickupRushError -Category 'gameplay_state_invalid' -Message 'PickupRush did not reach its expected five-pickup victory state.'
    }
}

function Invoke-AvidScriptPickupRushEditorProbe {
    [void][System.IO.Directory]::CreateDirectory($EvidenceRoot)
    $RunId = "Editor-$PID-$([Guid]::NewGuid().ToString('N'))"
    $ReportPath = Join-Path $EvidenceRoot "$RunId.json"
    $LogPath = Join-Path $EvidenceRoot "$RunId.log"
    $EditorCmd = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
    $Arguments = @(
        $ProjectFile,
        $Map,
        '-game',
        '-unattended',
        '-nop4',
        '-nosplash',
        '-nullrhi',
        '-nosound',
        "-AvidScriptScenario=$ScenarioId",
        "-AvidScriptScenarioProbeReport=$ReportPath",
        "-AvidScriptScenarioProbeEvents=$EventIds",
        "-abslog=$LogPath")
    $ProcessResult = Invoke-AvidScriptPickupRushProcess `
        -Executable $EditorCmd `
        -Arguments $Arguments `
        -WorkingDirectory $ProjectRoot `
        -TimeoutSeconds $TimeoutSeconds
    if ($ProcessResult.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $ReportPath -PathType Leaf)) {
        Throw-AvidScriptPickupRushError -Category 'editor_probe_failed' -Message "Editor scenario probe failed. See $LogPath"
    }
    $Report = [System.IO.File]::ReadAllText($ReportPath) | ConvertFrom-Json -Depth 64 -NoEnumerate
    Assert-AvidScriptPickupRushReport -Report $Report
    return [pscustomobject]@{ report_path = $ReportPath; log_path = $LogPath; report = $Report }
}

function Start-AvidScriptPickupRushPlaySession {
    [void][System.IO.Directory]::CreateDirectory($EvidenceRoot)
    $LogPath = Join-Path $EvidenceRoot "Play-$PID-$([Guid]::NewGuid().ToString('N')).log"
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor.exe'
    $StartInfo.WorkingDirectory = $ProjectRoot
    $StartInfo.UseShellExecute = $true
    foreach ($Argument in @($ProjectFile, $Map, '-game', '-log', '-AvidScriptScenario=pickup_rush', "-abslog=$LogPath")) {
        [void]$StartInfo.ArgumentList.Add($Argument)
    }
    $Process = [System.Diagnostics.Process]::Start($StartInfo)
    if ($null -eq $Process) {
        Throw-AvidScriptPickupRushError -Category 'play_launch_failed' -Message 'PickupRush play session did not start.'
    }
    return [pscustomobject]@{ process_id = $Process.Id; log_path = $LogPath }
}

try {
    if (-not (Test-Path -LiteralPath $ProjectFile -PathType Leaf)) {
        Throw-AvidScriptPickupRushError -Category 'project_missing' -Message "Project file is missing: $ProjectFile"
    }
    $BindingManifest = if ($Mode -eq 'Run') { '' } else { Publish-AvidScriptPickupRushBindings }
    $Release = if ($Mode -in @('Editor', 'Play')) {
        Invoke-AvidScriptPickupRushRelease -ResolvedBindingPackagePath $BindingManifest
    }
    else {
        $null
    }
    $EditorProbe = $null
    $BuildCookRun = $null
    $PackagedProbe = $null
    $Play = $null

    if ($Mode -eq 'Editor') {
        $EditorProbe = Invoke-AvidScriptPickupRushEditorProbe
    }
    elseif ($Mode -eq 'BuildCookRun') {
        $ResolvedArchiveRoot = if ([string]::IsNullOrWhiteSpace($ArchiveRoot)) {
            Join-Path $ProjectRoot "Saved/AvidScript/PickupRush/Packages/$Configuration-$PID-$([Guid]::NewGuid().ToString('N'))"
        }
        else {
            $ArchiveRoot
        }
        $Arguments = @(
            '-SourcePath', 'Plugins/AvidScript/Samples/CSharp/PickupRush/PickupRushScript.cs',
            '-CSharpProjectPath', 'Plugins/AvidScript/Samples/CSharp/PickupRush/AvidScript.PickupRush.csproj',
            '-ModuleId', $ModuleId,
            '-ArtifactStem', 'pickup_rush',
            '-OutputRoot', 'Content/AvidScript/Modules',
            '-DotNetPath', $DotNetPath,
            '-BindingPackagePath', $BindingManifest,
            '-RuntimeBindingPackagePath', $BindingManifest,
            '-Configuration', $Configuration,
            '-ArchiveRoot', $ResolvedArchiveRoot,
            '-PackagedOracleMode', 'None',
            '-PackagedOracleTimeoutSeconds', $TimeoutSeconds,
            '-EngineRoot', $EngineRoot)
        $BuildCookRun = Invoke-AvidScriptPickupRushJsonScript `
            -ScriptPath (Join-Path $BuildRoot 'InvokeAvidScriptBuildCookRun.ps1') `
            -Arguments $Arguments `
            -TimeoutSeconds 1800
        $ArchiveRoot = [string]$BuildCookRun.archive_root
    }
    if ($Mode -in @('BuildCookRun', 'Run')) {
        if ([string]::IsNullOrWhiteSpace($ArchiveRoot)) {
            Throw-AvidScriptPickupRushError -Category 'archive_root_required' -Message 'Run mode requires ArchiveRoot.'
        }
        $Arguments = @(
            '-ArchiveRoot', $ArchiveRoot,
            '-TargetName', $TargetName,
            '-ScenarioId', $ScenarioId,
            '-ModuleId', $ModuleId,
            '-EventIds', $EventIds,
            '-Map', $Map,
            '-Configuration', $Configuration,
            '-TimeoutSeconds', $TimeoutSeconds)
        $PackagedProbe = Invoke-AvidScriptPickupRushJsonScript `
            -ScriptPath (Join-Path $BuildRoot 'InvokeAvidScriptStartupScenarioProbe.ps1') `
            -Arguments $Arguments `
            -TimeoutSeconds $TimeoutSeconds
        Assert-AvidScriptPickupRushReport -Report $PackagedProbe.report
    }
    elseif ($Mode -eq 'Play') {
        $Play = Start-AvidScriptPickupRushPlaySession
    }

    $Result = [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_pickup_rush_succeeded'
        status = 'ok'
        mode = $Mode
        configuration = $Configuration
        binding_package = $BindingManifest
        release = $Release
        editor_probe = $EditorProbe
        build_cook_run = $BuildCookRun
        packaged_probe = $PackagedProbe
        play = $Play
    }
    [Console]::Out.WriteLine(($Result | ConvertTo-Json -Depth 100 -Compress))
    exit 0
}
catch {
    $Category = if ($_.Exception.Data.Contains('category')) { [string]$_.Exception.Data['category'] } else { 'unexpected_failure' }
    $Failure = [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_pickup_rush_failed'
        status = 'error'
        mode = $Mode
        configuration = $Configuration
        category = $Category
        message = $_.Exception.Message
    }
    [Console]::Out.WriteLine(($Failure | ConvertTo-Json -Depth 8 -Compress))
    exit 1
}
