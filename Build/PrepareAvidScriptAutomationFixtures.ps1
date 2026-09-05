param(
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet\dotnet.exe'),
    [string]$ProjectRoot = '',
    [string]$ReportPath = ''
)

$ErrorActionPreference = 'Stop'
$ExpectedDotNetSdk = '8.0.416'
$PluginRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
}
$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
$DotNetPath = [System.IO.Path]::GetFullPath($DotNetPath)
$BuildScript = Join-Path $PSScriptRoot 'BuildCSharpActorLifecycle.ps1'
$ReleaseRoot = Join-Path $ProjectRoot 'Saved\AvidScriptCSharpGuest\ActorLifecycle'
$DebugRoot = Join-Path $ProjectRoot 'Saved\AvidScriptCSharpGuest\DebuggerPIE'
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $ProjectRoot 'Saved\AvidScriptAutomationFixtures\automation-fixtures.json'
}
$ReportPath = [System.IO.Path]::GetFullPath($ReportPath)

foreach ($RequiredFile in @($DotNetPath, $BuildScript)) {
    if (-not [System.IO.File]::Exists($RequiredFile)) {
        throw "Required Automation fixture input is missing: $RequiredFile"
    }
}

$VersionOutput = @(& $DotNetPath --version 2>&1)
$VersionExitCode = $LASTEXITCODE
$DotNetVersion = ($VersionOutput | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
$DotNetVersion = $DotNetVersion.Trim()
if ($VersionExitCode -ne 0 -or $DotNetVersion -cne $ExpectedDotNetSdk) {
    throw "Automation fixtures require .NET SDK $ExpectedDotNetSdk; selected '$DotNetVersion' from $DotNetPath."
}

function Invoke-ActorLifecycleFixtureBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$DebugInstrumentation,
        [Parameter(Mandatory = $true)][string]$OutputRoot
    )

    $BuildOutput = @(& $BuildScript `
        -DotNetPath $DotNetPath `
        -OutputRoot $OutputRoot `
        -Configuration $Configuration `
        -DebugInstrumentation $DebugInstrumentation `
        -CompilerWorkerMode auto 2>&1)
    $BuildExitCode = $LASTEXITCODE
    if ($BuildExitCode -ne 0) {
        $Diagnostic = ($BuildOutput | Select-Object -Last 20 | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
        throw "$Configuration ActorLifecycle fixture build failed with exit code ${BuildExitCode}:`n$Diagnostic"
    }

    $BuildReportPath = Join-Path $OutputRoot 'actor_lifecycle.csharp.report.json'
    $ManifestPath = Join-Path $OutputRoot 'actor_lifecycle.avidscript.json'
    $WasmPath = Join-Path $OutputRoot 'actor_lifecycle.wasm'
    foreach ($ArtifactPath in @($BuildReportPath, $ManifestPath, $WasmPath)) {
        if (-not [System.IO.File]::Exists($ArtifactPath)) {
            throw "$Configuration ActorLifecycle fixture did not publish: $ArtifactPath"
        }
    }

    $BuildReport = Get-Content -Raw -LiteralPath $BuildReportPath | ConvertFrom-Json
    if (-not [bool]$BuildReport.succeeded -or
        [string]$BuildReport.result -cne 'direct_abi_built' -or
        [string]$BuildReport.compilation.debug_instrumentation -cne $DebugInstrumentation) {
        throw "$Configuration ActorLifecycle fixture report is inconsistent: $BuildReportPath"
    }

    return [ordered]@{
        configuration = $Configuration
        debug_instrumentation = $DebugInstrumentation
        output_root = $OutputRoot
        build_report = $BuildReportPath
        manifest = $ManifestPath
        wasm = $WasmPath
        wasm_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $WasmPath).Hash.ToLowerInvariant()
    }
}

$Release = Invoke-ActorLifecycleFixtureBuild `
    -Configuration 'Release' `
    -DebugInstrumentation 'disabled' `
    -OutputRoot $ReleaseRoot
$Debug = Invoke-ActorLifecycleFixtureBuild `
    -Configuration 'Debug' `
    -DebugInstrumentation 'enabled' `
    -OutputRoot $DebugRoot

$Report = [ordered]@{
    schema_version = 1
    result = 'passed'
    generated_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
    project_root = $ProjectRoot
    dotnet = [ordered]@{
        path = $DotNetPath
        sdk_version = $DotNetVersion
    }
    fixtures = @($Release, $Debug)
}

$ReportDirectory = Split-Path -Parent $ReportPath
[void][System.IO.Directory]::CreateDirectory($ReportDirectory)
$TemporaryReportPath = "$ReportPath.tmp.$PID"
$Utf8 = [System.Text.UTF8Encoding]::new($false)
try {
    $Json = $Report | ConvertTo-Json -Depth 12
    [System.IO.File]::WriteAllText($TemporaryReportPath, $Json + [Environment]::NewLine, $Utf8)
    Move-Item -LiteralPath $TemporaryReportPath -Destination $ReportPath -Force
}
finally {
    if ([System.IO.File]::Exists($TemporaryReportPath)) {
        Remove-Item -LiteralPath $TemporaryReportPath -Force
    }
}

Write-Output "[AvidScript][AutomationFixtures] result=passed release=1 debug=1 report=$ReportPath"
