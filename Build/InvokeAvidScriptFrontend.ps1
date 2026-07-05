param(
    [string]$SourcePath = "",
    [string]$BindingsPath = "",
    [string]$OutputRoot = "",
    [string]$ReportPath = "",
    [string]$Ldc2Path = "",
    [string]$ToolchainRoot = "",
    [switch]$SkipCompile
)

$ErrorActionPreference = "Stop"

function Convert-KeyValueRestToMap {
    param([string]$Rest)

    $Fields = [ordered]@{}
    if ([string]::IsNullOrWhiteSpace($Rest)) {
        return $Fields
    }

    $Matches = [regex]::Matches($Rest.Trim(), '(?<key>[A-Za-z_][A-Za-z0-9_]*)=(?<value>.*?)(?=\s+[A-Za-z_][A-Za-z0-9_]*=|$)')
    foreach ($Match in $Matches) {
        $Fields[$Match.Groups['key'].Value] = $Match.Groups['value'].Value.Trim()
    }

    return $Fields
}

function Read-FrontendOutput {
    param([string[]]$Lines)

    $Diagnostics = @()
    $BuildEvents = @()

    foreach ($Line in $Lines) {
        $DiagnosticMatch = [regex]::Match($Line, '^\[AvidScript\]\[Frontend\]\[Diagnostic\]\s+code=(?<code>\S+)\s+severity=(?<severity>\S+)\s+line=(?<line>\d+)\s+column=(?<column>\d+)\s+message="(?<message>.*)"$')
        if ($DiagnosticMatch.Success) {
            $Diagnostics += [ordered]@{
                code = $DiagnosticMatch.Groups['code'].Value
                severity = $DiagnosticMatch.Groups['severity'].Value
                line = [int]$DiagnosticMatch.Groups['line'].Value
                column = [int]$DiagnosticMatch.Groups['column'].Value
                message = $DiagnosticMatch.Groups['message'].Value.Replace('\"', '"')
            }
            continue
        }

        $BuildMatch = [regex]::Match($Line, '^\[AvidScript\]\[Frontend\]\[Build\]\s+result=(?<result>\S+)(?<rest>.*)$')
        if ($BuildMatch.Success) {
            $BuildEvents += [ordered]@{
                result = $BuildMatch.Groups['result'].Value
                fields = Convert-KeyValueRestToMap -Rest $BuildMatch.Groups['rest'].Value
            }
        }
    }

    return [PSCustomObject]@{
        Diagnostics = $Diagnostics
        BuildEvents = $BuildEvents
    }
}

function Test-FrontendSucceeded {
    param(
        [int]$ExitCode,
        [object[]]$Diagnostics,
        [object[]]$BuildEvents
    )

    if ($ExitCode -ne 0) {
        return $false
    }

    foreach ($Diagnostic in $Diagnostics) {
        if ([string]$Diagnostic.severity -eq 'error') {
            return $false
        }
    }

    if ($BuildEvents.Count -eq 0) {
        return $false
    }

    $LastResult = [string]$BuildEvents[$BuildEvents.Count - 1].result
    return @('generated', 'built').Contains($LastResult)
}

$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildDir
$ProjectPluginsDir = Split-Path -Parent $PluginRoot
$ProjectRoot = Split-Path -Parent $ProjectPluginsDir
$FrontendScript = Join-Path $BuildDir 'BuildAvidScriptActor.ps1'

if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportRoot = Join-Path $ProjectRoot 'Saved\AvidScriptReports'
    $BaseName = 'frontend'
    if (-not [string]::IsNullOrWhiteSpace($SourcePath)) {
        $BaseName = [System.IO.Path]::GetFileNameWithoutExtension($SourcePath)
        if ([string]::IsNullOrWhiteSpace($BaseName)) {
            $BaseName = 'frontend'
        }
    }
    $ReportPath = Join-Path $ReportRoot "$BaseName.frontend.report.json"
}

$ReportDirectory = Split-Path -Parent $ReportPath
if (-not [string]::IsNullOrWhiteSpace($ReportDirectory)) {
    New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
}

$ChildArguments = @(
    '-NoProfile',
    '-ExecutionPolicy',
    'Bypass',
    '-File',
    $FrontendScript
)

if (-not [string]::IsNullOrWhiteSpace($SourcePath)) {
    $ChildArguments += @('-SourcePath', $SourcePath)
}
if (-not [string]::IsNullOrWhiteSpace($BindingsPath)) {
    $ChildArguments += @('-BindingsPath', $BindingsPath)
}
if (-not [string]::IsNullOrWhiteSpace($OutputRoot)) {
    $ChildArguments += @('-OutputRoot', $OutputRoot)
}
if (-not [string]::IsNullOrWhiteSpace($Ldc2Path)) {
    $ChildArguments += @('-Ldc2Path', $Ldc2Path)
}
if (-not [string]::IsNullOrWhiteSpace($ToolchainRoot)) {
    $ChildArguments += @('-ToolchainRoot', $ToolchainRoot)
}
if ($SkipCompile) {
    $ChildArguments += '-SkipCompile'
}

$Output = & powershell @ChildArguments 2>&1
$ExitCode = $LASTEXITCODE
$OutputLines = @($Output | ForEach-Object { [string]$_ })

foreach ($Line in $OutputLines) {
    Write-Output $Line
}

$Parsed = Read-FrontendOutput -Lines $OutputLines
$Succeeded = Test-FrontendSucceeded -ExitCode $ExitCode -Diagnostics $Parsed.Diagnostics -BuildEvents $Parsed.BuildEvents

$Report = [ordered]@{
    schema_version = 1
    source = $SourcePath
    bindings = $BindingsPath
    output_root = $OutputRoot
    exit_code = $ExitCode
    succeeded = $Succeeded
    diagnostics = @($Parsed.Diagnostics)
    build_events = @($Parsed.BuildEvents)
    raw_output = @($OutputLines)
}

$ReportJson = $Report | ConvertTo-Json -Depth 10
[System.IO.File]::WriteAllText($ReportPath, $ReportJson + [System.Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

Write-Output "[AvidScript][Frontend][Report] path=$ReportPath succeeded=$Succeeded diagnostics=$($Parsed.Diagnostics.Count) build_events=$($Parsed.BuildEvents.Count)"
exit $ExitCode