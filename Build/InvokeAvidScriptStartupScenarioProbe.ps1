[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ArchiveRoot,
    [Parameter(Mandatory = $true)][string]$TargetName,
    [Parameter(Mandatory = $true)][string]$ScenarioId,
    [Parameter(Mandatory = $true)][string]$ModuleId,
    [string]$EventIds = '64001',
    [string]$Map = '/Game/TopDown/Lvl_TopDown',
    [ValidateSet('Development', 'Shipping')]
    [string]$Configuration = 'Development',
    [ValidateRange(10, 600)]
    [int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Throw-AvidScriptScenarioProbeError {
    param(
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $Exception = [System.InvalidOperationException]::new($Message)
    $Exception.Data['category'] = $Category
    throw $Exception
}

function Assert-AvidScriptScenarioProbeJsonProperties {
    param(
        [Parameter(Mandatory = $true)][System.Text.Json.JsonElement]$Element,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $Names = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
        foreach ($Property in $Element.EnumerateObject()) {
            if (-not $Names.Add($Property.Name)) {
                Throw-AvidScriptScenarioProbeError -Category 'scenario_probe_json_invalid' -Message "$Label contains duplicate property '$($Property.Name)'."
            }
            Assert-AvidScriptScenarioProbeJsonProperties -Element $Property.Value -Label "$Label.$($Property.Name)"
        }
    }
    elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        $Index = 0
        foreach ($Item in $Element.EnumerateArray()) {
            Assert-AvidScriptScenarioProbeJsonProperties -Element $Item -Label "$Label[$Index]"
            ++$Index
        }
    }
}

function ConvertFrom-AvidScriptScenarioProbeJson {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Label
    )

    try {
        $Document = [System.Text.Json.JsonDocument]::Parse($Text)
        try {
            if ($Document.RootElement.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) {
                Throw-AvidScriptScenarioProbeError -Category 'scenario_probe_json_invalid' -Message "$Label must be a JSON object."
            }
            Assert-AvidScriptScenarioProbeJsonProperties -Element $Document.RootElement -Label $Label
        }
        finally {
            $Document.Dispose()
        }
        return ($Text | ConvertFrom-Json -Depth 64 -NoEnumerate)
    }
    catch {
        if ($_.Exception.Data.Contains('category')) {
            throw
        }
        Throw-AvidScriptScenarioProbeError -Category 'scenario_probe_json_invalid' -Message "$Label is invalid: $($_.Exception.Message)"
    }
}

function Resolve-AvidScriptScenarioProbeExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedArchiveRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedTargetName,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$ResolvedConfiguration
    )

    $BinaryName = if ($ResolvedConfiguration -ceq 'Shipping') {
        "$ResolvedTargetName-Win64-Shipping.exe"
    }
    else {
        "$ResolvedTargetName.exe"
    }
    foreach ($PackageRoot in @((Join-Path $ResolvedArchiveRoot 'Windows'), $ResolvedArchiveRoot)) {
        foreach ($Candidate in @(
                (Join-Path $PackageRoot "$ResolvedTargetName/Binaries/Win64/$BinaryName"),
                (Join-Path $PackageRoot "$ResolvedTargetName.exe"))) {
            if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
                return [System.IO.Path]::GetFullPath($Candidate)
            }
        }
    }
    Throw-AvidScriptScenarioProbeError -Category 'packaged_executable_missing' -Message "Packaged executable is missing below ArchiveRoot: $ResolvedTargetName.exe"
}

function Get-AvidScriptScenarioProbeProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Property = $Value.PSObject.Properties[$Name]
    if ($null -eq $Property) {
        Throw-AvidScriptScenarioProbeError `
            -Category 'scenario_probe_schema_invalid' `
            -Message "$Label is missing '$Name'."
    }
    return $Property.Value
}

function Invoke-AvidScriptStartupScenarioProbe {
    param(
        [Parameter(Mandatory = $true)][string]$ArchiveRoot,
        [Parameter(Mandatory = $true)][string]$TargetName,
        [Parameter(Mandatory = $true)][string]$ScenarioId,
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [Parameter(Mandatory = $true)][string]$EventIds,
        [Parameter(Mandatory = $true)][string]$Map,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration,
        [ValidateRange(10, 600)]
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    if ($TargetName -cnotmatch '^[A-Za-z][A-Za-z0-9_]{0,63}$') {
        Throw-AvidScriptScenarioProbeError -Category 'target_name_invalid' -Message 'TargetName is not canonical.'
    }
    if ($ScenarioId -cnotmatch '^[a-z][a-z0-9_.-]{0,63}$') {
        Throw-AvidScriptScenarioProbeError -Category 'scenario_id_invalid' -Message 'ScenarioId is not canonical.'
    }
    if ($ModuleId -cnotmatch '^[a-z][a-z0-9_.-]{0,63}$') {
        Throw-AvidScriptScenarioProbeError -Category 'module_id_invalid' -Message 'ModuleId is not canonical.'
    }
    if ($Map -cnotmatch '^/Game/[A-Za-z0-9_./]+$') {
        Throw-AvidScriptScenarioProbeError -Category 'map_invalid' -Message 'Map must be a canonical /Game path.'
    }
    $ParsedEventIds = @($EventIds -split ',' | ForEach-Object {
        $Parsed = 0
        if (-not [int]::TryParse($_, [ref]$Parsed) -or $Parsed -le 0) {
            Throw-AvidScriptScenarioProbeError -Category 'event_ids_invalid' -Message 'EventIds must contain positive int32 values.'
        }
        $Parsed
    })
    if ($ParsedEventIds.Count -eq 0 -or $ParsedEventIds.Count -gt 64) {
        Throw-AvidScriptScenarioProbeError -Category 'event_ids_invalid' -Message 'EventIds must contain between 1 and 64 values.'
    }
    $CanonicalEventIds = $ParsedEventIds -join ','

    $ResolvedArchiveRoot = [System.IO.Path]::GetFullPath($ArchiveRoot)
    if (-not (Test-Path -LiteralPath $ResolvedArchiveRoot -PathType Container)) {
        Throw-AvidScriptScenarioProbeError -Category 'archive_root_missing' -Message "ArchiveRoot is missing: $ResolvedArchiveRoot"
    }
    $ExecutablePath = Resolve-AvidScriptScenarioProbeExecutable `
        -ResolvedArchiveRoot $ResolvedArchiveRoot `
        -ResolvedTargetName $TargetName `
        -ResolvedConfiguration $Configuration
    $PackageRoot = Join-Path $ResolvedArchiveRoot 'Windows'
    if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
        $PackageRoot = $ResolvedArchiveRoot
    }
    $ProjectSavedRoot = if ($Configuration -ceq 'Shipping') {
        $LocalAppData = [System.Environment]::GetFolderPath(
            [System.Environment+SpecialFolder]::LocalApplicationData)
        Join-Path $LocalAppData "$TargetName/Saved"
    }
    else {
        Join-Path $PackageRoot "$TargetName/Saved"
    }
    $EvidenceRoot = Join-Path $ProjectSavedRoot 'AvidScript/ScenarioProbe'
    [void][System.IO.Directory]::CreateDirectory($EvidenceRoot)
    $RunId = "$Configuration-$PID-$([Guid]::NewGuid().ToString('N'))"
    $ReportPath = Join-Path $EvidenceRoot "$RunId.json"
    $LogPath = Join-Path $EvidenceRoot "$RunId.log"

    $Arguments = [System.Collections.Generic.List[string]]::new()
    $ExpectedBinaryRoot = [System.IO.Path]::GetFullPath((Join-Path $PackageRoot "$TargetName/Binaries/Win64"))
    $ActualBinaryRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $ExecutablePath))
    if ($ActualBinaryRoot.Equals($ExpectedBinaryRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        $Arguments.Add($TargetName)
    }
    foreach ($Argument in @(
            $Map,
            '-game',
            '-unattended',
            '-nullrhi',
            '-nosplash',
            '-nosound',
            '-stdout',
            '-FullStdOutLogOutput',
            "-abslog=$LogPath",
            "-AvidScriptScenario=$ScenarioId",
            "-AvidScriptScenarioProbeReport=$ReportPath",
            "-AvidScriptScenarioProbeEvents=$CanonicalEventIds")) {
        $Arguments.Add($Argument)
    }

    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $ExecutablePath
    $StartInfo.WorkingDirectory = $PackageRoot
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Argument in $Arguments) {
        [void]$StartInfo.ArgumentList.Add($Argument)
    }

    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    $StartedUtc = [System.DateTime]::UtcNow
    try {
        if (-not $Process.Start()) {
            Throw-AvidScriptScenarioProbeError -Category 'process_launch_failed' -Message "Packaged executable did not start: $ExecutablePath"
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
            Throw-AvidScriptScenarioProbeError -Category 'process_timeout' -Message "Scenario probe exceeded $TimeoutSeconds seconds."
        }
        $Stdout = $StdoutTask.GetAwaiter().GetResult()
        $Stderr = $StderrTask.GetAwaiter().GetResult()
        if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf)) {
            Throw-AvidScriptScenarioProbeError -Category 'report_missing' -Message "Packaged process exited $($Process.ExitCode) without a scenario report. See $LogPath"
        }
        $Report = ConvertFrom-AvidScriptScenarioProbeJson `
            -Text ([System.IO.File]::ReadAllText($ReportPath)) `
            -Label 'startup scenario probe report'
        $Components = @(Get-AvidScriptScenarioProbeProperty $Report 'components' 'startup scenario probe report')
        if ($Components.Count -eq 0) {
            Throw-AvidScriptScenarioProbeError -Category 'scenario_probe_assertion_failed' -Message 'Scenario report contains no runtime components.'
        }
        $First = $Components[0]
        $Valid = $Process.ExitCode -eq 0 `
            -and [long](Get-AvidScriptScenarioProbeProperty $Report 'schema_version' 'startup scenario probe report') -eq 1 `
            -and [string](Get-AvidScriptScenarioProbeProperty $Report 'result' 'startup scenario probe report') -ceq 'avidscript_startup_scenario_probe_passed' `
            -and [string](Get-AvidScriptScenarioProbeProperty $Report 'scenario_id' 'startup scenario probe report') -ceq $ScenarioId `
            -and [long](Get-AvidScriptScenarioProbeProperty $Report 'events_requested' 'startup scenario probe report') -eq $ParsedEventIds.Count `
            -and [long](Get-AvidScriptScenarioProbeProperty $Report 'events_dispatched' 'startup scenario probe report') -eq $ParsedEventIds.Count `
            -and [string](Get-AvidScriptScenarioProbeProperty $First 'module_id' 'first component') -ceq $ModuleId `
            -and [bool](Get-AvidScriptScenarioProbeProperty $First 'runtime_loaded' 'first component') `
            -and [bool](Get-AvidScriptScenarioProbeProperty $First 'begin_play' 'first component') `
            -and [long](Get-AvidScriptScenarioProbeProperty $First 'ticks' 'first component') -gt 0 `
            -and [long](Get-AvidScriptScenarioProbeProperty $First 'events' 'first component') -ge $ParsedEventIds.Count `
            -and [long](Get-AvidScriptScenarioProbeProperty $First 'dropped_gameplay_events' 'first component') -eq 0 `
            -and [string](Get-AvidScriptScenarioProbeProperty $First 'last_error' 'first component') -ceq ''
        if (-not $Valid) {
            Throw-AvidScriptScenarioProbeError -Category 'scenario_probe_assertion_failed' -Message "Scenario probe assertions failed. See $ReportPath"
        }

        return [pscustomobject][ordered]@{
            schema_version = 1
            result = 'avidscript_startup_scenario_probe_process_passed'
            status = 'ok'
            target_name = $TargetName
            configuration = $Configuration
            scenario_id = $ScenarioId
            module_id = $ModuleId
            executable_path = $ExecutablePath
            report_path = $ReportPath
            log_path = $LogPath
            exit_code = $Process.ExitCode
            elapsed_ms = [Math]::Round(([System.DateTime]::UtcNow - $StartedUtc).TotalMilliseconds, 3)
            report = $Report
        }
    }
    finally {
        $Process.Dispose()
    }
}

if ($MyInvocation.InvocationName -eq '.') {
    return
}

try {
    $Result = Invoke-AvidScriptStartupScenarioProbe @PSBoundParameters
    [Console]::Out.WriteLine(($Result | ConvertTo-Json -Depth 64 -Compress))
    exit 0
}
catch {
    $Category = if ($_.Exception.Data.Contains('category')) { [string]$_.Exception.Data['category'] } else { 'unexpected_failure' }
    $Failure = [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_startup_scenario_probe_process_failed'
        status = 'error'
        category = $Category
        configuration = $Configuration
        scenario_id = $ScenarioId
        module_id = $ModuleId
        message = $_.Exception.Message
    }
    [Console]::Out.WriteLine(($Failure | ConvertTo-Json -Depth 8 -Compress))
    exit 1
}
