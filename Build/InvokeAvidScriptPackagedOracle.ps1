[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ArchiveRoot,
    [Parameter(Mandatory = $true)][string]$TargetName,
    [Parameter(Mandatory = $true)][string]$ModuleId,
    [ValidateSet('Development', 'Shipping')]
    [string]$Configuration = 'Development',
    [ValidateRange(10, 600)]
    [int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Throw-AvidScriptPackagedOracleError {
    param(
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $Exception = [System.InvalidOperationException]::new($Message)
    $Exception.Data['category'] = $Category
    throw $Exception
}

function Assert-AvidScriptPackagedOracleJsonProperties {
    param(
        [Parameter(Mandatory = $true)][System.Text.Json.JsonElement]$Element,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $Names = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($Property in $Element.EnumerateObject()) {
            if (-not $Names.Add($Property.Name)) {
                Throw-AvidScriptPackagedOracleError `
                    -Category 'oracle_json_duplicate_property' `
                    -Message "$Label contains duplicate property '$($Property.Name)'."
            }
            Assert-AvidScriptPackagedOracleJsonProperties `
                -Element $Property.Value `
                -Label "$Label.$($Property.Name)"
        }
    }
    elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        $Index = 0
        foreach ($Item in $Element.EnumerateArray()) {
            Assert-AvidScriptPackagedOracleJsonProperties `
                -Element $Item `
                -Label "$Label[$Index]"
            ++$Index
        }
    }
}

function ConvertFrom-AvidScriptPackagedOracleJson {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ([string]::IsNullOrWhiteSpace($Text)) {
        Throw-AvidScriptPackagedOracleError `
            -Category 'oracle_json_invalid' `
            -Message "$Label is empty."
    }
    try {
        $Document = [System.Text.Json.JsonDocument]::Parse($Text)
        try {
            if ($Document.RootElement.ValueKind -ne
                [System.Text.Json.JsonValueKind]::Object) {
                Throw-AvidScriptPackagedOracleError `
                    -Category 'oracle_json_invalid' `
                    -Message "$Label must be a JSON object."
            }
            Assert-AvidScriptPackagedOracleJsonProperties `
                -Element $Document.RootElement `
                -Label $Label
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
        Throw-AvidScriptPackagedOracleError `
            -Category 'oracle_json_invalid' `
            -Message "$Label is invalid: $($_.Exception.Message)"
    }
}

function Get-AvidScriptPackagedOracleProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Property = $Value.PSObject.Properties[$Name]
    if ($null -eq $Property) {
        Throw-AvidScriptPackagedOracleError `
            -Category 'oracle_schema_invalid' `
            -Message "$Label is missing '$Name'."
    }
    return $Property.Value
}

function Resolve-AvidScriptPackagedOracleExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedArchiveRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedTargetName
    )

    $Candidates = @(
        (Join-Path $ResolvedArchiveRoot "Windows/$ResolvedTargetName.exe"),
        (Join-Path $ResolvedArchiveRoot "$ResolvedTargetName.exe")
    )
    foreach ($Candidate in $Candidates) {
        if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($Candidate)
        }
    }
    Throw-AvidScriptPackagedOracleError `
        -Category 'packaged_executable_missing' `
        -Message "Packaged executable is missing below ArchiveRoot: $ResolvedTargetName.exe"
}

function Invoke-AvidScriptPackagedOracle {
    param(
        [Parameter(Mandatory = $true)][string]$ArchiveRoot,
        [Parameter(Mandatory = $true)][string]$TargetName,
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [ValidateSet('Development', 'Shipping')]
        [Parameter(Mandatory = $true)][string]$Configuration,
        [ValidateRange(10, 600)]
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    if ($TargetName -cnotmatch '^[A-Za-z][A-Za-z0-9_]{0,63}$') {
        Throw-AvidScriptPackagedOracleError `
            -Category 'target_name_invalid' `
            -Message 'TargetName is not canonical.'
    }
    if ($ModuleId -cnotmatch '^[a-z][a-z0-9_.-]{0,63}$') {
        Throw-AvidScriptPackagedOracleError `
            -Category 'module_id_invalid' `
            -Message 'ModuleId is not canonical.'
    }
    $ResolvedArchiveRoot = [System.IO.Path]::GetFullPath($ArchiveRoot)
    if (-not (Test-Path -LiteralPath $ResolvedArchiveRoot -PathType Container)) {
        Throw-AvidScriptPackagedOracleError `
            -Category 'archive_root_missing' `
            -Message "ArchiveRoot is missing: $ResolvedArchiveRoot"
    }
    $ExecutablePath = Resolve-AvidScriptPackagedOracleExecutable `
        -ResolvedArchiveRoot $ResolvedArchiveRoot `
        -ResolvedTargetName $TargetName
    $ExecutableRoot = Split-Path -Parent $ExecutablePath
    $EvidenceRoot = Join-Path `
        $ExecutableRoot `
        "$TargetName/Saved/AvidScript/PackagedOracle"
    [void][System.IO.Directory]::CreateDirectory($EvidenceRoot)
    $RunId = "$Configuration-$PID-$([Guid]::NewGuid().ToString('N'))"
    $ReportPath = Join-Path $EvidenceRoot "$RunId.json"
    $LogPath = Join-Path $EvidenceRoot "$RunId.log"

    $Arguments = @(
        '/Game/TopDown/Lvl_TopDown',
        '-game',
        '-unattended',
        '-nullrhi',
        '-nosplash',
        '-nosound',
        '-stdout',
        '-FullStdOutLogOutput',
        "-abslog=$LogPath",
        "-AvidScriptPackagedOracle=$ModuleId",
        "-AvidScriptPackagedOracleReport=$ReportPath"
    )
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $ExecutablePath
    $StartInfo.WorkingDirectory = $ExecutableRoot
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Argument in $Arguments) {
        [void]$StartInfo.ArgumentList.Add($Argument)
    }
    $StartInfo.Environment['AVIDSCRIPT_PACKAGED_ORACLE_MODULE'] = $ModuleId
    $StartInfo.Environment['AVIDSCRIPT_PACKAGED_ORACLE_REPORT'] = $ReportPath

    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    $StartedUtc = [System.DateTime]::UtcNow
    $Stdout = ''
    $Stderr = ''
    try {
        if (-not $Process.Start()) {
            Throw-AvidScriptPackagedOracleError `
                -Category 'packaged_process_launch_failed' `
                -Message "Packaged executable did not start: $ExecutablePath"
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
            Throw-AvidScriptPackagedOracleError `
                -Category 'packaged_process_timeout' `
                -Message "Packaged oracle exceeded $TimeoutSeconds seconds."
        }
        $Stdout = $StdoutTask.GetAwaiter().GetResult()
        $Stderr = $StderrTask.GetAwaiter().GetResult()
        $ElapsedMs = ([System.DateTime]::UtcNow - $StartedUtc).TotalMilliseconds
        $LogText = "--- stdout ---`n$Stdout`n--- stderr ---`n$Stderr"
        [System.IO.File]::WriteAllText(
            $LogPath,
            $LogText,
            [System.Text.UTF8Encoding]::new($false))
        if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf)) {
            Throw-AvidScriptPackagedOracleError `
                -Category 'oracle_report_missing' `
                -Message "Packaged process exited $($Process.ExitCode) without an oracle report. See $LogPath"
        }
        $Oracle = ConvertFrom-AvidScriptPackagedOracleJson `
            -Text ([System.IO.File]::ReadAllText($ReportPath)) `
            -Label 'packaged oracle report'
        $Healthy = Get-AvidScriptPackagedOracleProperty $Oracle 'healthy' 'packaged oracle report'
        $Fault = Get-AvidScriptPackagedOracleProperty $Oracle 'fault' 'packaged oracle report'
        $Valid = $Process.ExitCode -eq 0 `
            -and [long](Get-AvidScriptPackagedOracleProperty $Oracle 'schema_version' 'packaged oracle report') -eq 1 `
            -and [string](Get-AvidScriptPackagedOracleProperty $Oracle 'result' 'packaged oracle report') -ceq 'avidscript_packaged_oracle_passed' `
            -and [string](Get-AvidScriptPackagedOracleProperty $Oracle 'configuration' 'packaged oracle report') -ceq $Configuration `
            -and [string](Get-AvidScriptPackagedOracleProperty $Oracle 'module_id' 'packaged oracle report') -ceq $ModuleId `
            -and [bool](Get-AvidScriptPackagedOracleProperty $Oracle 'fault_rejected' 'packaged oracle report') `
            -and [bool](Get-AvidScriptPackagedOracleProperty $Oracle 'world_continued' 'packaged oracle report') `
            -and [bool](Get-AvidScriptPackagedOracleProperty $Oracle 'continuation_observed' 'packaged oracle report') `
            -and [bool](Get-AvidScriptPackagedOracleProperty $Healthy 'resolved_from_package' 'healthy result') `
            -and [bool](Get-AvidScriptPackagedOracleProperty $Healthy 'begin_play' 'healthy result') `
            -and [bool](Get-AvidScriptPackagedOracleProperty $Healthy 'end_play' 'healthy result') `
            -and [long](Get-AvidScriptPackagedOracleProperty $Healthy 'ticks' 'healthy result') -ge 3 `
            -and [long](Get-AvidScriptPackagedOracleProperty $Healthy 'timer_callbacks' 'healthy result') -ge 1 `
            -and [long](Get-AvidScriptPackagedOracleProperty $Healthy 'event_callbacks' 'healthy result') -ge 1 `
            -and -not [bool](Get-AvidScriptPackagedOracleProperty $Fault 'runtime_loaded' 'fault result')
        if (-not $Valid) {
            Throw-AvidScriptPackagedOracleError `
                -Category 'oracle_assertion_failed' `
                -Message "Packaged oracle assertions failed with exit code $($Process.ExitCode). See $ReportPath"
        }

        return [pscustomobject][ordered]@{
            schema_version = 1
            result = 'avidscript_packaged_oracle_process_passed'
            status = 'ok'
            target_name = $TargetName
            configuration = $Configuration
            module_id = $ModuleId
            executable_path = $ExecutablePath
            report_path = $ReportPath
            log_path = $LogPath
            exit_code = $Process.ExitCode
            elapsed_ms = [Math]::Round($ElapsedMs, 3)
            oracle = $Oracle
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
    $Result = Invoke-AvidScriptPackagedOracle `
        -ArchiveRoot $ArchiveRoot `
        -TargetName $TargetName `
        -ModuleId $ModuleId `
        -Configuration $Configuration `
        -TimeoutSeconds $TimeoutSeconds
    [Console]::Out.WriteLine(($Result | ConvertTo-Json -Depth 64 -Compress))
    exit 0
}
catch {
    $Category = if ($_.Exception.Data.Contains('category')) {
        [string]$_.Exception.Data['category']
    }
    else {
        'unexpected_failure'
    }
    $Failure = [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_packaged_oracle_process_failed'
        status = 'error'
        category = $Category
        configuration = $Configuration
        module_id = $ModuleId
        message = $_.Exception.Message
    }
    [Console]::Out.WriteLine(($Failure | ConvertTo-Json -Depth 8 -Compress))
    exit 1
}
