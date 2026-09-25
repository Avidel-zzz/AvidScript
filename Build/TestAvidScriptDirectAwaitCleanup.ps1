[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [Parameter(Mandatory = $true)][string]$BindingPackagePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$projectPath = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$runRoot = Join-Path $projectRoot "Saved/AvidScriptDirectAwaitCleanupTests/$runId"
$negativeRoot = Join-Path $runRoot 'DefaultReject'
$outputRoot = Join-Path $runRoot 'FormalBuild'
$logPath = Join-Path $projectRoot "Saved/Logs/AvidScript_DirectAwaitCleanup_$runId.log"
$stem = 'direct_await_cleanup'
$testName = 'AvidScript.Runtime.Continuation.CompiledDirectAwaitCleanup'
$package = (Resolve-Path -LiteralPath $BindingPackagePath -ErrorAction Stop).Path
$packageModel = Get-Content -LiteralPath $package -Raw | ConvertFrom-Json
$reference = Join-Path (Split-Path -Parent $package) ([string]$packageModel.files.reference_source)
if (-not (Test-Path -LiteralPath $reference -PathType Leaf) -or
    -not (Select-String -LiteralPath $reference -SimpleMatch -Pattern 'avid_continuation_delay_cancel_resume_v1' -Quiet)) {
    throw 'The selected binding package predates the cancel-resume Host import.'
}
$sdk = & $DotNetPath --version
if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') {
    throw "Direct await cleanup requires .NET SDK 8.0.416, got '$sdk'."
}

Push-Location $pluginRoot
try {
    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File 'Build/BuildCSharpActorLifecycle.ps1' `
        -DotNetPath $DotNetPath `
        -SourcePath 'Fixtures/Phase66/DirectAwaitCleanup.cs' `
        -ProjectPath 'Fixtures/Phase66/DirectAwaitCleanup.csproj' `
        -ProjectRoot $projectRoot `
        -OutputRoot $negativeRoot `
        -BindingPackagePath $package `
        -ArtifactStem $stem `
        -LanguageErrors bounded `
        -AsyncExceptionFlow `
        -CompilerWorkerMode disabled
    $negativeExit = $LASTEXITCODE
    $negativeReportPath = Join-Path $negativeRoot "$stem.csharp.report.json"
    if (-not (Test-Path -LiteralPath $negativeReportPath -PathType Leaf)) {
        throw "Default-mode rejection report is missing: $negativeReportPath"
    }
    $negative = Get-Content -LiteralPath $negativeReportPath -Raw | ConvertFrom-Json
    if ($negativeExit -ne 1 -or $negative.result -cne 'semantic_failed' -or
        @($negative.diagnostics | Where-Object code -ceq 'ASCS3002').Count -lt 1 -or
        (Test-Path -LiteralPath (Join-Path $negativeRoot "$stem.wasm"))) {
        throw 'Protected direct await must remain rejected without the preview switch.'
    }

    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File 'Build/BuildCSharpActorLifecycle.ps1' `
        -DotNetPath $DotNetPath `
        -SourcePath 'Fixtures/Phase66/DirectAwaitCleanup.cs' `
        -ProjectPath 'Fixtures/Phase66/DirectAwaitCleanup.csproj' `
        -ProjectRoot $projectRoot `
        -OutputRoot $outputRoot `
        -BindingPackagePath $package `
        -ArtifactStem $stem `
        -LanguageErrors bounded `
        -AsyncExceptionFlow `
        -DirectAwaitCleanup `
        -CompilerWorkerMode disabled
    if ($LASTEXITCODE -ne 0) { throw "Formal direct await build failed: $outputRoot" }
}
finally {
    Pop-Location
}

$wasmPath = Join-Path $outputRoot "$stem.wasm"
$irPath = Join-Path $outputRoot "$stem.guestir.json"
$manifestPath = Join-Path $outputRoot "$stem.avidscript.json"
$reportPath = Join-Path $outputRoot "$stem.csharp.report.json"
$statePath = Join-Path $outputRoot "$stem.state.json"
foreach ($path in @($wasmPath, $irPath, $manifestPath, $reportPath, $statePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Formal direct await artifact is missing: $path"
    }
}
$ir = Get-Content -LiteralPath $irPath -Raw | ConvertFrom-Json
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
if ($report.result -cne 'direct_abi_built' -or -not $report.succeeded -or
    [int]$report.semantic.schema_version -ne 43 -or
    [string]$report.semantic.version -cne '1.52' -or
    [int]$ir.schema_version -ne 23 -or [string]$ir.ir_version -cne '1.22' -or
    @($ir.direct_await_routes).Count -ne 1 -or
    @($ir.imports | Where-Object name -ceq 'avid_continuation_delay_cancel_resume_v1').Count -ne 1 -or
    [string]$manifest.module_id -cne [string]$ir.module_id -or
    ([IO.FileInfo]$wasmPath).Length -le 8) {
    throw "Formal direct await contract is invalid: $outputRoot"
}
$offsets = @{}
foreach ($field in @('Result', 'CleanupCount')) {
    $slot = @($state.slots | Where-Object stable_id -ceq "state:type:global::Script:$field")
    if ($slot.Count -ne 1 -or [int]$slot[0].size -ne 4 -or
        [int]$slot[0].offset -lt 0 -or [int]$slot[0].offset -ge 65536) {
        throw "Direct await state slot is invalid: $field"
    }
    $offsets[$field] = [string]$slot[0].offset
}
$env:AVIDSCRIPT_DIRECT_AWAIT_WASM_PATH = $wasmPath
$env:AVIDSCRIPT_DIRECT_AWAIT_MODULE_ID = [string]$manifest.module_id
$env:AVIDSCRIPT_DIRECT_AWAIT_RESULT_OFFSET = $offsets['Result']
$env:AVIDSCRIPT_DIRECT_AWAIT_CLEANUP_OFFSET = $offsets['CleanupCount']
$build = Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat'
& $build AvidTPSTemplateEditor Win64 Development "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE
if ($LASTEXITCODE -ne 0) { throw 'No-clean Win64 Editor build failed.' }
$editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
& $editor $projectPath -unattended -nop4 -NullRHI -nosplash `
    "-ExecCmds=Automation RunTests $testName;Quit" `
    '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
if ($LASTEXITCODE -ne 0) { throw "Direct await Automation failed: $logPath" }
$log = Get-Content -LiteralPath $logPath -Raw
$found = [regex]::Matches($log, "Found 1 automation tests based on '$([regex]::Escape($testName))'").Count
$success = [regex]::Matches($log,
    'Test Completed\. Result=\{Success\} Name=\{CompiledDirectAwaitCleanup\} Path=\{AvidScript\.Runtime\.Continuation\.CompiledDirectAwaitCleanup\}').Count
$failed = [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count
$complete = [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
$markers = @(
    foreach ($backend in @(0, 1)) {
        "compiled direct await backend=$backend cancel=0 result=16 cleanup=1 resumes=2 cancelled=0"
        "compiled direct await backend=$backend cancel=1 result=0 cleanup=1 resumes=2 cancelled=2"
    }
)
$scenarios = @($markers | Where-Object {
    [regex]::Matches($log, [regex]::Escape($_)).Count -eq 1
}).Count
if ($found -ne 1 -or $success -ne 1 -or $failed -ne 0 -or
    $complete -ne 1 -or $scenarios -ne 4) {
    throw "Direct await Automation evidence incomplete: found=$found success=$success scenarios=$scenarios failed=$failed complete=$complete log=$logPath"
}
Write-Output "CompiledDirectAwaitCleanup: 1/1 passed; default=ASCS3002 Win64 Wasmtime/WAMR=4/4; log=$logPath"
