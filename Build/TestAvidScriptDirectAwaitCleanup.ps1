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
$env:DOTNET_CLI_HOME = Join-Path $env:LOCALAPPDATA 'Temp/AvidScriptPhase66ReferenceCliHome'
$referenceOutput = & $DotNetPath run `
    --project (Join-Path $pluginRoot 'Fixtures/Phase66/DirectAwaitCleanup.Reference.csproj') `
    -c Release
if ($LASTEXITCODE -ne 0 -or
    @($referenceOutput | Where-Object { $_ -ceq 'DirectAwaitCleanup.Reference: 4/4 passed' }).Count -ne 1) {
    throw 'The same-source .NET direct await comparison failed.'
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

# Compile a second code generation through the same formal entrypoint. Only the
# fixture's captured value changes; neither WASM nor manifests are edited.
$nextSource = Join-Path $runRoot 'DirectAwaitCleanup.Next.cs'
$sourceText = [IO.File]::ReadAllText((Join-Path $pluginRoot 'Fixtures/Phase66/DirectAwaitCleanup.cs'))
if ([regex]::Matches($sourceText, [regex]::Escape('new AwaitValue(16)')).Count -ne 1) {
    throw 'Reload fixture must contain exactly one versioned captured value.'
}
[IO.File]::WriteAllText($nextSource, $sourceText.Replace('new AwaitValue(16)', 'new AwaitValue(32)'), [Text.UTF8Encoding]::new($false))
$nextReferenceProject = Join-Path $runRoot 'DirectAwaitCleanup.Next.Reference.csproj'
[xml]$referenceProject = Get-Content -LiteralPath (Join-Path $pluginRoot 'Fixtures/Phase66/DirectAwaitCleanup.Reference.csproj') -Raw
$referenceProject.SelectSingleNode('//Compile[@Include="DirectAwaitCleanup.cs"]').SetAttribute('Include', $nextSource)
$referenceProject.SelectSingleNode('//Compile[@Include="DirectAwaitCleanup.Reference.cs"]').SetAttribute(
    'Include', (Join-Path $pluginRoot 'Fixtures/Phase66/DirectAwaitCleanup.Reference.cs'))
$referenceProject.Save($nextReferenceProject)
$nextReferenceOutput = & $DotNetPath run --project $nextReferenceProject -c Release -- 32
if ($LASTEXITCODE -ne 0 -or
    @($nextReferenceOutput | Where-Object { $_ -ceq 'DirectAwaitCleanup.Reference: 4/4 passed' }).Count -ne 1) {
    throw 'The next-generation same-source .NET comparison failed.'
}
$nextRoot = Join-Path $runRoot 'ReloadBuild'
& (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File (Join-Path $PSScriptRoot 'BuildCSharpActorLifecycle.ps1') `
    -DotNetPath $DotNetPath -SourcePath $nextSource `
    -ProjectPath (Join-Path $pluginRoot 'Fixtures/Phase66/DirectAwaitCleanup.csproj') `
    -ProjectRoot $projectRoot -OutputRoot $nextRoot -BindingPackagePath $package `
    -ModuleId ([string]$manifest.module_id) -ArtifactStem $stem `
    -LanguageErrors bounded -AsyncExceptionFlow -DirectAwaitCleanup -CompilerWorkerMode disabled
if ($LASTEXITCODE -ne 0) { throw 'The next-generation formal build failed.' }
$nextManifestPath = Join-Path $nextRoot "$stem.avidscript.json"
$nextManifest = Get-Content -LiteralPath $nextManifestPath -Raw | ConvertFrom-Json
$nextReport = Get-Content -LiteralPath (Join-Path $nextRoot "$stem.csharp.report.json") -Raw | ConvertFrom-Json
if ($nextReport.result -cne 'direct_abi_built' -or -not $nextReport.succeeded -or
    $nextManifest.module_id -cne $manifest.module_id -or
    $nextManifest.source.sha256 -ceq $manifest.source.sha256 -or
    (Get-FileHash -LiteralPath (Join-Path $nextRoot "$stem.wasm")).Hash -ceq (Get-FileHash -LiteralPath $wasmPath).Hash) {
    throw 'Reload must use different compiled code under the same module identity.'
}
$env:AVIDSCRIPT_DIRECT_AWAIT_NEXT_MANIFEST_PATH = $nextManifestPath
$importOutput = & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File `
    (Join-Path $PSScriptRoot 'Contracts/TestCompilerManagedImportContracts.ps1') -GuestIrPath $irPath
if ($LASTEXITCODE -ne 0 -or
    @($importOutput | Where-Object { $_ -ceq 'CompilerManagedImportContracts: 54/54 passed' }).Count -ne 1) {
    throw 'Compiler-managed import authorization contracts failed.'
}
$offsets = @{}
foreach ($field in @('Result', 'CleanupCount', 'CatchCount', 'CleanupMode')) {
    $slot = @($state.slots | Where-Object stable_id -ceq "state:type:global::Script:$field")
    if ($slot.Count -ne 1 -or [int]$slot[0].size -ne 4 -or
        [int]$slot[0].offset -lt 0 -or [int]$slot[0].offset -ge 65536) {
        throw "Direct await state slot is invalid: $field"
    }
    $offsets[$field] = [string]$slot[0].offset
}
$env:AVIDSCRIPT_DIRECT_AWAIT_WASM_PATH = $wasmPath
$env:AVIDSCRIPT_DIRECT_AWAIT_MODULE_ID = [string]$manifest.module_id
$env:AVIDSCRIPT_DIRECT_AWAIT_MANIFEST_PATH = $manifestPath
$env:AVIDSCRIPT_DIRECT_AWAIT_RESULT_OFFSET = $offsets['Result']
$env:AVIDSCRIPT_DIRECT_AWAIT_CLEANUP_OFFSET = $offsets['CleanupCount']
$env:AVIDSCRIPT_DIRECT_AWAIT_CATCH_OFFSET = $offsets['CatchCount']
$env:AVIDSCRIPT_DIRECT_AWAIT_MODE_OFFSET = $offsets['CleanupMode']
$build = Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat'
# Refresh the source inventory so a cached UBT makefile cannot omit a newly
# added Automation cpp. Existing object files and build outputs remain intact.
& $build AvidTPSTemplateEditor Win64 Development "-Project=$projectPath" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles
if ($LASTEXITCODE -ne 0) { throw 'No-clean Win64 Editor build failed.' }
$editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
& $editor $projectPath -unattended -nop4 -NullRHI -nosplash `
    "-ExecCmds=Automation RunTests $testName;Quit" `
    '-TestExit=Automation Test Queue Empty' "-abslog=$logPath"
if ($LASTEXITCODE -ne 0) { throw "Direct await Automation failed: $logPath" }
$log = Get-Content -LiteralPath $logPath -Raw
$found = [regex]::Matches($log, "Found 3 automation tests based on '$([regex]::Escape($testName))'").Count
$success = @(
    foreach ($name in @('CompiledDirectAwaitCleanup', 'CompiledDirectAwaitCleanupLifecycle', 'CompiledDirectAwaitCleanupReload')) {
        $pattern = 'Test Completed\. Result=\{Success\} Name=\{' + $name +
            '\} Path=\{AvidScript\.Runtime\.Continuation\.' + $name + '\}'
        if ([regex]::Matches($log, $pattern).Count -eq 1) { $name }
    }
).Count
$failed = [regex]::Matches($log, 'Test Completed\. Result=\{Fail\}').Count
$complete = [regex]::Matches($log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count
$markers = @(
    foreach ($backend in @(0, 1)) {
        "compiled direct await backend=$backend cancel=0 cleanup_throw=0 result=16 cleanup=1 catch=0 resumes=2 cancelled=0"
        "compiled direct await backend=$backend cancel=1 cleanup_throw=0 result=0 cleanup=1 catch=0 resumes=2 cancelled=2"
        "compiled direct await backend=$backend cancel=0 cleanup_throw=1 result=0 cleanup=1 catch=0 resumes=2 cancelled=0"
        "compiled direct await backend=$backend cancel=1 cleanup_throw=1 result=0 cleanup=1 catch=0 resumes=2 cancelled=1"
    }
)
$scenarios = @($markers | Where-Object {
    [regex]::Matches($log, [regex]::Escape($_)).Count -eq 1
}).Count
$lifecycleMarkers = @(
    foreach ($backend in @(0, 1)) {
        foreach ($scenario in @('session', 'world', 'object', 'discard', 'commit', 'commit_cancelled')) {
            $activeCleanup = if ($scenario -ceq 'discard') { 1 } else { 0 }
            $candidateCleanup = if ($scenario -in @('commit', 'commit_cancelled')) { 1 } elseif ($scenario -ceq 'discard') { 0 } else { -1 }
            $resumes = if ($scenario -in @('discard', 'commit', 'commit_cancelled')) { 2 } else { 0 }
            $cancelled = if ($scenario -ceq 'commit_cancelled') { 2 } else { 0 }
            "compiled direct await lifecycle backend=$backend scenario=$scenario active_cleanup=$activeCleanup candidate_cleanup=$candidateCleanup resumes=$resumes cancelled=$cancelled roots=0 tasks=0 frames=0 sources=0 bindings=0"
        }
    }
)
$lifecycleScenarios = @($lifecycleMarkers | Where-Object {
    [regex]::Matches($log, [regex]::Escape($_)).Count -eq 1
}).Count
$reloadMarkers = @(
    foreach ($backend in @(0, 1)) {
        foreach ($oldCancel in @(0, 1)) {
            foreach ($mode in @(0, 1, 2)) {
                $committed = if ($mode -eq 2) { 0 } else { 1 }
                $survivorCancel = if ($mode -eq 2) { $oldCancel } elseif ($mode -eq 1) { 1 } else { 0 }
                $result = if ($survivorCancel -eq 1) { 0 } elseif ($committed -eq 1) { 32 } else { 16 }
                "compiled direct await reload backend=$backend old_cancel=$oldCancel mode=$mode committed=$committed survivor_cancel=$survivorCancel result=$result cleanup=1 tasks=0 frames=0 sources=0 bindings=0 runtimes=0"
            }
        }
    }
)
$reloadScenarios = @($reloadMarkers | Where-Object {
    [regex]::Matches($log, [regex]::Escape($_)).Count -eq 1
}).Count
if ($found -ne 1 -or $success -ne 3 -or $failed -ne 0 -or
    $complete -ne 1 -or $scenarios -ne 8 -or $lifecycleScenarios -ne 12 -or $reloadScenarios -ne 12) {
    throw "Direct await Automation evidence incomplete: found=$found success=$success scenarios=$scenarios lifecycle=$lifecycleScenarios reload=$reloadScenarios failed=$failed complete=$complete log=$logPath"
}
Write-Output "CompiledDirectAwaitCleanup: 3/3 passed; .NET=8/8 imports=54/54 default=ASCS3002 Win64 Wasmtime/WAMR=8/8 lifecycle=12/12 reload=12/12; log=$logPath"
