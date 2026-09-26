[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BindingPackageManifestPath,
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$runRoot = Join-Path $projectRoot ('Saved/AvidScript/AsyncGeneratedTypeContracts/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$null = New-Item -ItemType Directory -Path $runRoot -Force
$package = (Resolve-Path -LiteralPath $BindingPackageManifestPath).Path
$source = Join-Path $pluginRoot 'Fixtures/Phase66/SharedTaskLifetime.cs'
$project = Join-Path $pluginRoot 'Fixtures/Phase66/SharedTaskLifetime.csproj'
$build = Join-Path $pluginRoot 'Build/BuildCSharpScriptTypes.ps1'
$preview = @('-LanguageErrors', 'bounded', '-AsyncExceptionFlow', '-DirectAwaitCleanup', '-AsyncCancellationFlow')
$checks = 0
Write-Output "Async generated type evidence: $runRoot"

function Invoke-Case {
    param([string]$Name, [string]$Source, [string[]]$Options, [string]$Native = '')
    $caseRoot = Join-Path $runRoot $Name
    if (-not $Native) { $Native = Join-Path $caseRoot 'Native' }
    $arguments = @('-NoProfile', '-File', $build, '-DotNetPath', $DotNetPath,
        '-SourcePath', $Source, '-SourceId', 'Fixtures/Phase66/SharedTaskLifetime.cs',
        '-ProjectPath', $project, '-BindingPackageManifestPath', $package,
        '-OutputRoot', $Native, '-ArtifactRoot', (Join-Path $caseRoot 'Artifacts'),
        '-CookOutputRoot', (Join-Path $caseRoot 'Cook'), '-RuntimeModuleId', 'avidscript.fixture.shared_task_types') + $Options
    & (Join-Path $PSHOME 'pwsh.exe') @arguments *> (Join-Path $runRoot "$Name.log")
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Native = $Native;
        Descriptor = Join-Path $Native 'AvidScriptGeneratedPackage.json' }
}

function Read-VerifiedPackage {
    param($Case)
    if ($Case.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $Case.Descriptor)) {
        throw "Async generated type build failed: $runRoot"
    }
    $descriptor = Get-Content $Case.Descriptor -Raw | ConvertFrom-Json
    $typePath = [IO.Path]::GetFullPath((Join-Path $Case.Native $descriptor.type_manifest.file))
    $runtimePath = [IO.Path]::GetFullPath((Join-Path $Case.Native $descriptor.runtime_manifest.file))
    $types = Get-Content $typePath -Raw | ConvertFrom-Json
    $runtime = Get-Content $runtimePath -Raw | ConvertFrom-Json
    $runtimeRoot = Split-Path -Parent $runtimePath
    $ir = Get-Content (Join-Path $runtimeRoot 'generated_types.guestir.json') -Raw | ConvertFrom-Json
    $report = Get-Content (Join-Path $runtimeRoot 'generated_types.csharp.report.json') -Raw | ConvertFrom-Json
    $semantic = Join-Path (Split-Path -Parent $runtimeRoot) 'script-types.semantic.json'
    $wasm = Join-Path $runtimeRoot 'generated_types.wasm'
    if ($types.semantic_schema_version -ne 45 -or $types.semantic_version -cne '1.54' -or
        $types.semantic_artifact_sha256 -cne (Get-FileHash $semantic -Algorithm SHA256).Hash.ToLowerInvariant() -or
        $descriptor.type_manifest.sha256 -cne (Get-FileHash $typePath -Algorithm SHA256).Hash.ToLowerInvariant() -or
        $descriptor.runtime_manifest.sha256 -cne (Get-FileHash $runtimePath -Algorithm SHA256).Hash.ToLowerInvariant() -or
        $runtime.wasm.sha256 -cne (Get-FileHash $wasm -Algorithm SHA256).Hash.ToLowerInvariant() -or
        $runtime.module_id -cne $descriptor.runtime_module_id -or
        $ir.schema_version -ne 25 -or $ir.ir_version -cne '1.24' -or
        $ir.task_local_lifetimes.exception_model -cne 'cancellation' -or
        -not $report.succeeded -or -not $report.compilation.async_cancellation_flow -or
        -not $report.compilation.async_exception_flow -or -not $report.compilation.direct_await_cleanup -or
        $report.semantic_cache.enabled -or $report.compilation_cache.enabled -or $report.compiler_worker.used -or
        @($types.types).Count -ne 1 -or $types.types[0].engine_name -cne 'SharedTaskActor') {
        throw "Generated shell/Runtime artifact identity or async options diverged: $runRoot"
    }
    foreach ($output in $types.outputs) {
        $outputPath = Join-Path $Case.Native $output.relative_path
        if ((Get-FileHash $outputPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $output.sha256) {
            throw "Generated source hash mismatch: $outputPath"
        }
    }
    return [pscustomobject]@{ Descriptor = $descriptor; WasmHash = $runtime.wasm.sha256; Native = $Case.Native }
}

$initialCase = Invoke-Case 'initial' $source $preview
$initial = Read-VerifiedPackage $initialCase
if ($initial.Descriptor.reload.classification -cne 'initial_install') { throw 'Initial package classification mismatch' }
# Preserve the first published manifest and descriptor before body-only output
# replaces them. The sibling directory retains their relative Runtime paths.
$initialArchive = Join-Path (Split-Path -Parent $initialCase.Native) 'InitialPublished'
Copy-Item -LiteralPath $initialCase.Native -Destination $initialArchive -Recurse
$initial = Read-VerifiedPackage ([pscustomobject]@{ ExitCode = 0; Native = $initialArchive;
    Descriptor = Join-Path $initialArchive 'AvidScriptGeneratedPackage.json' })
$checks++
Write-Output 'PASS Task cancellation source -> native shell + Semantic 45 -> IR 25 -> WASM package'

$nextSource = Join-Path $runRoot 'SharedTaskLifetimeNext.cs'
$text = [IO.File]::ReadAllText($source)
if ([regex]::Matches($text, [regex]::Escape('const int CodeOffset = 0;')).Count -ne 1) { throw 'Expected one code-generation constant' }
[IO.File]::WriteAllText($nextSource, $text.Replace('const int CodeOffset = 0;', 'const int CodeOffset = 16;'), [Text.UTF8Encoding]::new($false))
$nextCase = Invoke-Case 'next' $nextSource $preview $initialCase.Native
$next = Read-VerifiedPackage $nextCase
if ($next.Descriptor.reload.classification -cne 'body_only' -or
    $next.Descriptor.reload.previous_package_id -cne $initial.Descriptor.package_id -or
    $next.Descriptor.reload.native_structure_sha256 -cne $initial.Descriptor.reload.native_structure_sha256 -or
    $next.Descriptor.package_id -ceq $initial.Descriptor.package_id -or $next.WasmHash -ceq $initial.WasmHash) {
    throw 'Body update must change executable code, preserve native structure and link the previous package'
}
$checks++
Write-Output 'PASS body-only update retains native structure and changes WASM'

$rejectCases = @(
    @{ Name = 'default'; Options = @() },
    @{ Name = 'missing_cancel'; Options = @('-LanguageErrors', 'bounded', '-AsyncExceptionFlow', '-DirectAwaitCleanup') },
    @{ Name = 'missing_bounded'; Options = @('-AsyncExceptionFlow') },
    @{ Name = 'missing_exception'; Options = @('-AsyncCancellationFlow') },
    @{ Name = 'cleanup_without_exception'; Options = @('-DirectAwaitCleanup') },
    @{ Name = 'shipping'; Options = $preview + @('-PackageConfiguration', 'Shipping') },
    @{ Name = 'headless'; Options = $preview + @('-HeadlessRelease') },
    @{ Name = 'shell_only'; Options = $preview + @('-SkipRuntimePackage') }
)
foreach ($case in $rejectCases) {
    $rejected = Invoke-Case $case.Name $source $case.Options
    if ($rejected.ExitCode -eq 0 -or (Test-Path -LiteralPath $rejected.Descriptor)) {
        throw "Unexpected publication for $($case.Name): $runRoot"
    }
    $checks++
    Write-Output "PASS rejects $($case.Name) without publishing a package"
}
[ordered]@{ passed = $checks; total = 10; initial = $initial; next = $next } |
    ConvertTo-Json -Depth 10 | Set-Content (Join-Path $runRoot 'results.json') -Encoding utf8NoBOM
Write-Output "Async generated type build contracts: $checks/10 passed; evidence=$runRoot"
