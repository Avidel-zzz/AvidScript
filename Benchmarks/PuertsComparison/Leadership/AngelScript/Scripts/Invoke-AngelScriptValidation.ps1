[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProjectFile,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$BuildEvidence,
    [Parameter(Mandatory)][string]$OutputRoot,
    [ValidateRange(30, 1800)][int]$TimeoutSeconds = 600,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'AngelScriptValidation.psm1') -Force
$Adapter = Split-Path -Parent $PSScriptRoot
$ProjectFile = [IO.Path]::GetFullPath($ProjectFile)
$ProjectRoot = Split-Path -Parent $ProjectFile
$Executable = [IO.Path]::GetFullPath($Executable)
$Bin = Join-Path $ProjectRoot 'Binaries/Win64'
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
foreach ($Path in @($ProjectFile, $Executable, $OutputRoot)) { Assert-As36PhysicalPath $Path }
foreach ($Path in @($ProjectRoot, $OutputRoot)) {
    foreach ($ProtectedRoot in (Get-As36ProtectedRoots $Adapter)) {
        if ($Path.Equals($ProtectedRoot, [StringComparison]::OrdinalIgnoreCase) -or
            $Path.StartsWith($ProtectedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Use an isolated project and evidence directory outside the main project.' }
    }
}
if (-not $Executable.StartsWith([IO.Path]::GetFullPath($Bin) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    -not $Executable.EndsWith('Editor-Cmd.exe', [StringComparison]::OrdinalIgnoreCase)) { throw 'Use the actual isolated project Editor commandlet executable.' }
$Build = Get-Content -LiteralPath $BuildEvidence -Raw | ConvertFrom-Json
if ($Build.status -cne 'succeeded' -or $Build.exit_code -ne 0) { throw 'Successful native build evidence is required.' }
$Target = [IO.Path]::GetFileName($Executable).Replace('-Cmd.exe', '')
$ManifestPath = Join-Path $Bin "$Target.modules"
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$Harness = Join-Path $ProjectRoot 'Plugins/AvidScriptPerfHarness'
$ContractPath = Join-Path $Adapter 'contract.json'
$Contract = Get-Content -LiteralPath $ContractPath -Raw | ConvertFrom-Json
$Files = @($PSCommandPath, (Join-Path $PSScriptRoot 'AngelScriptValidation.psm1'), $ContractPath,
    $BuildEvidence, $ProjectFile, $Executable, $ManifestPath, (Join-Path $Bin "$Target.target"))
foreach ($InputFile in $Contract.inputs) {
    $Path = Join-Path $Harness $InputFile.path
    Assert-As36PhysicalPath $Path
    if ((Get-As36Sha256 $Path) -cne $InputFile.sha256) { throw "Frozen fixture/workload changed: $($InputFile.path)" }
    $Files += $Path
}
$Installed = @($Contract.native_bridge.files | ForEach-Object {
    [pscustomobject]@{ path = Join-Path $Harness ('Source/AvidScriptPerfHarness/Private/' + [IO.Path]::GetFileName($_.path)); sha256 = $_.sha256 }
})
$Installed += [pscustomobject]@{ path = Join-Path $ProjectRoot 'Script/AvidScriptLeadership.as'; sha256 = $Contract.script_sha256 }
foreach ($Config in $Contract.integration_files) { $Installed += [pscustomobject]@{ path = Join-Path $Harness $Config.destination; sha256 = $Config.sha256 } }
foreach ($Entry in $Installed) {
    Assert-As36PhysicalPath $Entry.path
    if ((Get-As36Sha256 $Entry.path) -cne $Entry.sha256) { throw "Installed adapter/configuration changed: $($Entry.path)" }
    $Files += $Entry.path
}
foreach ($Module in @('AngelscriptCode', 'AngelscriptEditor', 'AngelscriptLoader', 'JsEnv', 'AvidScriptCore',
    'AvidScriptBindings', 'AvidScriptRuntime', 'AvidScriptVM', 'AvidScriptGenerated', 'AvidScriptEditor', 'AvidScriptPerfHarness')) {
    $Relative = $Manifest.Modules.$Module
    if ([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative)) { throw "Invalid module entry: $Module" }
    $Full = [IO.Path]::GetFullPath((Join-Path $Bin $Relative))
    if (-not $Full.StartsWith([IO.Path]::GetFullPath($Bin) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Module escaped isolated output.' }
    Assert-As36PhysicalPath $Full
    $Files += $Full
}
$Files += @(Get-ChildItem -LiteralPath (Join-Path $Harness 'Source/AvidScriptPerfHarness') -File -Recurse |
    Where-Object { $_.Extension -in @('.h', '.cpp', '.inl', '.cs') } | ForEach-Object FullName)
$Before = @($Files | Sort-Object -Unique | ForEach-Object { [pscustomobject]@{ path = $_; sha256 = Get-As36Sha256 $_ } })
if ($PrepareOnly) {
    [pscustomobject]@{ inputs = $Before; build_id = $Manifest.BuildId; project_root = $ProjectRoot; executable = $Executable }
    return
}
$RunRoot = Join-Path $OutputRoot ('validation-' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$null = New-Item -ItemType Directory -Path $RunRoot
$Log = Join-Path $RunRoot 'validation.log'
$ReportPath = Join-Path $RunRoot 'correctness.json'
$StatePath = Join-Path $RunRoot 'run.json'
$Arguments = @($ProjectFile, '-run=As36LeadershipValidate', "-As36Result=$ReportPath", '-unattended', '-nullrhi', '-nosplash', '-nosound', '-nop4', '-NoLiveCoding', "-abslog=$Log")
if (@($Arguments | Where-Object { $_ -match '["\r\n]' }).Count -ne 0) { throw 'Unsupported quote or newline in process arguments.' }
$ArgumentText = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
$State = [ordered]@{ status = 'prepared'; passed = $false; started_utc = [DateTimeOffset]::UtcNow.ToString('o');
    executable = $Executable; command = $Arguments; build_id = $Manifest.BuildId; inputs = $Before;
    scope = 'Editor VM correctness only; no formal timing or packaged native claim' }
$State | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $StatePath -Encoding utf8
$Process = $null
try {
    $Process = Start-Process -FilePath $Executable -ArgumentList $ArgumentText -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $RunRoot 'stdout.log') -RedirectStandardError (Join-Path $RunRoot 'stderr.log')
    $State.process_id = $Process.Id
    $State.status = 'running'
    $State | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $StatePath -Encoding utf8
    Write-Output "AngelScript correctness started: PID=$($Process.Id), evidence=$RunRoot"
    $Watch = [Diagnostics.Stopwatch]::StartNew()
    while (-not $Process.WaitForExit(1000)) {
        if ($Watch.Elapsed.TotalSeconds -gt $TimeoutSeconds) { throw 'Owned validation timed out.' }
    }
    $Process.Refresh()
    $State.exit_code = $Process.ExitCode
    Assert-As36CompletionLog -Text (Get-Content -LiteralPath $Log -Raw) -ExitCode $Process.ExitCode
    Assert-As36CorrectnessReport -Report (Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json)
    foreach ($Entry in $Before) {
        if ((Get-As36Sha256 $Entry.path) -cne $Entry.sha256) { throw "Input changed during validation: $($Entry.path)" }
    }
    $State.correctness_report_sha256 = Get-As36Sha256 $ReportPath
    $State.log_sha256 = Get-As36Sha256 $Log
    $State.completed_cases = 264
    $State.rejection_controls = 17
    $State.status = 'passed'
    $State.passed = $true
} catch {
    $State.status = 'failed'
    $State.error = $_.Exception.Message
    if ($null -ne $Process -and -not $Process.HasExited) {
        & taskkill.exe /PID $Process.Id /T /F
        $Process.WaitForExit()
        $Process.Refresh()
        $State.exit_code = $Process.ExitCode
    }
} finally {
    $State.finished_utc = [DateTimeOffset]::UtcNow.ToString('o')
    $State | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $StatePath -Encoding utf8
}
Write-Output "AngelScript validation $($State.status): $StatePath"
if (-not $State.passed) { Write-Output $State.error; exit 1 }
