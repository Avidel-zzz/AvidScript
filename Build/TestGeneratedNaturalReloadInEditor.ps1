[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BindingPackageManifestPath,
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.dotnet/dotnet.exe'),
    [string]$PreparedRunRoot = '',
    [string]$PreparedExecutionRoot = '',
    [switch]$PrepareOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$project = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$preparedExecution = ''
if (-not [string]::IsNullOrWhiteSpace($PreparedExecutionRoot)) {
    if ($PrepareOnly -or [string]::IsNullOrWhiteSpace($PreparedRunRoot)) {
        throw 'PreparedExecutionRoot requires a verified PreparedRunRoot and native execution.'
    }
    $preparedExecution = (Resolve-Path -LiteralPath $PreparedExecutionRoot).Path
    $executionNamespace = [IO.Path]::GetFullPath((Join-Path $projectRoot 'Saved/AvidScript/GeneratedNaturalReload'))
    if (-not $preparedExecution.StartsWith($executionNamespace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'PreparedExecutionRoot must be an existing owned project GeneratedNaturalReload directory.'
    }
}
$nativeRoot = Join-Path $pluginRoot 'Source/AvidScriptGenerated'
$runRoot = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) (
    'AvidScript/Verification/GeneratedNaturalReload/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$preparedRoot = $runRoot
if (-not [string]::IsNullOrWhiteSpace($PreparedRunRoot)) {
    if ($PrepareOnly) { throw 'PreparedRunRoot is for consuming verified preparation during the native run.' }
    $preparedRoot = (Resolve-Path -LiteralPath $PreparedRunRoot).Path
    $allowedRoot = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Verification/GeneratedNaturalReload'))
    if (-not $preparedRoot.StartsWith($allowedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'PreparedRunRoot must be an existing owned GeneratedNaturalReload evidence directory.'
    }
}
$backupRoot = Join-Path $runRoot 'Original'
$candidateRoot = Join-Path $preparedRoot 'Candidates'
$sourceId = 'Fixtures/Phase66/GeneratedNaturalReload.cs'
$source = Join-Path $pluginRoot $sourceId
$fixtureProject = Join-Path $pluginRoot 'Fixtures/Phase66/GeneratedNaturalReload.csproj'
$moduleId = 'avidscript.fixture.generated_natural_reload'
$package = (Resolve-Path -LiteralPath $BindingPackageManifestPath).Path
$build = Join-Path $PSScriptRoot 'BuildCSharpScriptTypes.ps1'
$files = @('Public/AvidScriptGeneratedTypes.h', 'Private/AvidScriptGeneratedTypes.cpp',
    'AvidScriptGeneratedManifest.json', 'AvidScriptGeneratedPackage.json')
$hashes = @{}
$installed = $false
$restored = $false
$failure = $null
$process = [Diagnostics.Process]::GetCurrentProcess()
$oldPriority = $process.PriorityClass
$oldCandidateRoot = $env:AVIDSCRIPT_GENERATED_NATURAL_RELOAD_ROOT
$savedEnvironment = @{}
$safeEnvironment = @{
    DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'; DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
    DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'; DOTNET_NOLOGO = '1'
    DOTNET_CLI_TELEMETRY_OPTOUT = '1'; DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
    MSBUILDDISABLENODEREUSE = '1'; DOTNET_CLI_USE_MSBUILD_SERVER = '0'; UseSharedCompilation = 'false'
    DOTNET_CLI_HOME = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Toolchain/GeneratedTypes')
}
$persistentPaths = @{}
foreach ($scope in @('User', 'Machine')) { $persistentPaths[$scope] = [Environment]::GetEnvironmentVariable('Path', $scope) }

function Invoke-GeneratedBuild([string]$Name, [string]$SourcePath, [string]$OutputRoot, [string]$ArtifactGroup, [bool]$ReadOnly = $false,
    [string]$ArtifactBaseRoot = $preparedRoot) {
    $log = Join-Path $runRoot "$Name.log"
    if (-not $ReadOnly) {
    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File $build -DotNetPath $DotNetPath `
        -SourcePath $SourcePath -SourceId $sourceId -ProjectPath $fixtureProject `
        -BindingPackageManifestPath $package -OutputRoot $OutputRoot `
        -ArtifactRoot (Join-Path $ArtifactBaseRoot "Artifacts/$ArtifactGroup") -CookOutputRoot (Join-Path $runRoot 'UnusedCook') `
        -RuntimeModuleId $moduleId -LanguageProfile gameplay-v1 -CompilerWorkerMode disabled *> $log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log -Tail 30; throw "Formal generated build failed: $log" }
    }
    $descriptor = Get-Content -LiteralPath (Join-Path $OutputRoot 'AvidScriptGeneratedPackage.json') -Raw | ConvertFrom-Json
    foreach ($entry in @($descriptor.type_manifest, $descriptor.runtime_manifest)) {
        if ((Get-FileHash -LiteralPath (Join-Path $OutputRoot $entry.file) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) {
            throw 'Formal package references different artifact bytes.'
        }
    }
    $types = Get-Content -LiteralPath (Join-Path $OutputRoot $descriptor.type_manifest.file) -Raw | ConvertFrom-Json
    if ((@($types.types.cpp_name | Sort-Object) -join '|') -cne 'AReloadFlowActor|UReloadFlowComponent' -or
        $types.semantic_schema_version -ne 56 -or $types.semantic_version -cne '1.65') { throw 'Original composed UE declarations were changed.' }
    foreach ($entry in $types.outputs) {
        if ((Get-FileHash -LiteralPath (Join-Path $OutputRoot $entry.relative_path) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) {
            throw 'Native shell differs from the formal manifest.'
        }
    }
    $runtimePath = Join-Path $OutputRoot $descriptor.runtime_manifest.file
    $runtimeDirectory = Split-Path -Parent $runtimePath
    $runtime = Get-Content -LiteralPath $runtimePath -Raw | ConvertFrom-Json
    $wasmPath = if ([IO.Path]::IsPathRooted([string]$runtime.wasm.file)) { [string]$runtime.wasm.file } else {
        $relative = Join-Path $runtimeDirectory $runtime.wasm.file
        if (Test-Path -LiteralPath $relative -PathType Leaf) { $relative } else { Join-Path $projectRoot $runtime.wasm.file }
    }
    if ((Get-FileHash -LiteralPath $wasmPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $runtime.wasm.sha256) {
        throw 'Formal manifest references different canonical WASM bytes.'
    }
    $report = Get-Content -LiteralPath (Join-Path $runtimeDirectory 'generated_types.csharp.report.json') -Raw | ConvertFrom-Json
    $state = Get-Content -LiteralPath (Join-Path $runtimeDirectory 'generated_types.state.json') -Raw | ConvertFrom-Json
    $ir = Get-Content -LiteralPath (Join-Path $runtimeDirectory 'generated_types.guestir.json') -Raw | ConvertFrom-Json -Depth 100
    if (@($ir.static_storage.slots).Count -ne 1 -or
        $ir.static_storage.slots[0].id -cne 'static:$initialization:type:global::ReloadFlow' -or
        $ir.static_storage.slots[0].type_id -cne 'type:$static:state_ref' -or
        (Get-FileHash -LiteralPath (Join-Path $runtimeDirectory 'generated_types.guestir.json') -Algorithm SHA256).Hash.ToLowerInvariant() -cne $runtime.guest_ir.sha256) {
        throw 'Original IR must declare exactly the ReloadFlow initialization domain slot.'
    }
    $debugPath = Join-Path $runtimeDirectory 'generated_types.csharp.debug.json'
    $debug = Get-Content -LiteralPath $debugPath -Raw | ConvertFrom-Json
    if ($runtime.source.file -cne $sourceId -or $debug.source.id -cne $runtime.source.file -or
        $debug.source.sha256 -cne $runtime.source.sha256 -or
        (Get-FileHash -LiteralPath $debugPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $runtime.debug_map.sha256) {
        throw 'Formal manifest and original debug map must preserve the same source identity and bytes.'
    }
    if ($runtime.module_id -cne $moduleId -or $report.result -cne 'direct_abi_built' -or
        $report.language_profile.name -cne 'gameplay-v1' -or
        $report.language_profile.contract_sha256 -cne 'de80599da0475dcf3e54d49595f1e223c9a0c21567875affd74c2824c3464631' -or
        $report.source.sha256 -cne (Get-FileHash -LiteralPath $SourcePath -Algorithm SHA256).Hash.ToLowerInvariant() -or
        $state.owner_type_id -cne "execution_domain:$moduleId" -or @($state.slots).Count -ne 3) {
        throw 'Generated Runtime, gameplay admission and domain state contract disagree.'
    }
    Write-Host "PASS formal $Name ($($descriptor.package_id))"
    return $descriptor
}

function Invoke-NativeBuild([string]$Name, [string]$Suite) {
    $log = Join-Path $runRoot "$Name.log"
    & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
        "-Project=$project" "-AvidScriptGeneratedTestSuite=$Suite" -WaitMutex -NoHotReloadFromIDE `
        -NoUBTMakefiles -gather -MaxParallelActions=1 -NoUBA *> $log
    if ($LASTEXITCODE -ne 0) {
        Select-String -LiteralPath $log -Pattern 'error C[0-9]+|error LNK[0-9]+|fatal error' | ForEach-Object { $_.Line }
        Get-Content -LiteralPath $log -Tail 6
        throw "No-clean build failed: $log"
    }
    Write-Output "PASS $Name"
}

function Test-RejectedInputs([string]$InitialRoot) {
    $descriptor = Get-Content -LiteralPath (Join-Path $InitialRoot 'AvidScriptGeneratedPackage.json') -Raw | ConvertFrom-Json
    $runtimeDirectory = Split-Path -Parent (Join-Path $InitialRoot $descriptor.runtime_manifest.file)
    $protectedFiles = @(Get-ChildItem -LiteralPath $InitialRoot,$runtimeDirectory -File -Recurse)
    $before = @{}
    foreach ($file in $protectedFiles) { $before[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
    $cases = @(
        @{ Name='UnknownProfile'; Extra=@('-LanguageProfile','unknown-profile'); SourceId=$sourceId; Message='Unsupported C# language profile' }
        @{ Name='RawConflict'; Extra=@('-LanguageProfile','gameplay-v1','-AsyncExceptionFlow'); SourceId=$sourceId; Message='LanguageProfile cannot be combined' }
        @{ Name='DisabledErrors'; Extra=@('-LanguageProfile','gameplay-v1','-LanguageErrors','disabled'); SourceId=$sourceId; Message='Gameplay profile requires bounded' }
        @{ Name='Shipping'; Extra=@('-LanguageProfile','gameplay-v1','-PackageConfiguration','Shipping'); SourceId=$sourceId; Message='Bounded language errors in generated types require' }
        @{ Name='Headless'; Extra=@('-LanguageProfile','gameplay-v1','-HeadlessRelease'); SourceId=$sourceId; Message='Bounded language errors in generated types require' }
        @{ Name='ShellOnly'; Extra=@('-LanguageProfile','gameplay-v1','-SkipRuntimePackage'); SourceId=$sourceId; Message='Bounded language errors in generated types require' }
        @{ Name='SourceTraversal'; Extra=@('-LanguageProfile','gameplay-v1'); SourceId='../forged.cs'; Message='SourceId must be a stable' }
        @{ Name='SourceControl'; Extra=@('-LanguageProfile','gameplay-v1'); SourceId="forged`n.cs"; Message='SourceId must be a stable' }
    )
    foreach ($case in $cases) {
        $log = Join-Path $runRoot ("Rejected-$($case.Name).log")
        $arguments = @('-NoProfile','-File',$build,'-DotNetPath',$DotNetPath,'-SourcePath',$source,
            '-SourceId',$case.SourceId,'-ProjectPath',$fixtureProject,'-BindingPackageManifestPath',$package,
            '-OutputRoot',$InitialRoot,'-ArtifactRoot',(Join-Path $preparedRoot 'Artifacts/Initial'),'-RuntimeModuleId',$moduleId,
            '-CompilerWorkerMode','disabled') + $case.Extra
        & (Join-Path $PSHOME 'pwsh.exe') @arguments *> $log
        if ($LASTEXITCODE -eq 0 -or -not (Get-Content -LiteralPath $log -Raw).Contains($case.Message)) { throw "Expected specific input rejection: $log" }
        foreach ($path in $before.Keys) {
            if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -cne $before[$path]) { throw "Rejected input replaced a published artifact: $path" }
        }
    }
    # The shared runtime builder must also reject its new explicit identity
    # before touching a previously published runtime output.
    foreach ($identity in @('../forged.cs', "forged`n.cs")) {
        $log = Join-Path $runRoot ('Rejected-RuntimeIdentity-' + [Guid]::NewGuid().ToString('N') + '.log')
        & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File (Join-Path $PSScriptRoot 'BuildCSharpActorLifecycle.ps1') `
            -SourceId $identity -OutputRoot $runtimeDirectory *> $log
        if ($LASTEXITCODE -eq 0 -or -not (Get-Content -LiteralPath $log -Raw).Contains('SourceId must be a stable')) { throw "Runtime identity was not rejected: $log" }
        foreach ($path in $before.Keys) {
            if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -cne $before[$path]) { throw 'Invalid identity changed live Runtime outputs.' }
        }
    }
    Write-Output 'PASS rejected generated/profile/source inputs 10/10; published artifacts unchanged'
}

function Invoke-EditorTest([string]$Name, [string]$Test, [int]$Cases = 0) {
    $log = Join-Path $runRoot "$Name.log"
    & (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $project -unattended -nop4 -NullRHI -nosplash -nosound `
        "-ExecCmds=Automation RunTests $Test;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$log" *> (Join-Path $runRoot "$Name.stdout.log")
    $exitCode = $LASTEXITCODE
    $text = Get-Content -LiteralPath $log -Raw
    $escaped = [regex]::Escape($Test)
    if ($exitCode -ne 0 -or [regex]::Matches($text, "Found 1 automation tests based on '$escaped'").Count -ne 1 -or
        [regex]::Matches($text, 'Test Completed\. Result=\{Success\} Name=\{[^}]+\} Path=\{' + $escaped + '\}').Count -ne 1 -or
        $text -match 'Test Completed\. Result=\{Fail\}' -or
        [regex]::Matches($text, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1) {
        Get-Content -LiteralPath $log -Tail 40; throw "Editor test failed or incomplete: $log (exit=$exitCode)"
    }
    if ($Cases -gt 0 -and (@([regex]::Matches($text, 'natural generated reload passed backend=[0-9]+ mode=[0-8] point=[01]').Value |
            Sort-Object -Unique).Count -ne $Cases -or -not $text.Contains("GeneratedNaturalLanguageRollback: $Cases/36 passed"))) {
        throw "Real generated type scenario count mismatch: $log"
    }
    Write-Output "PASS $Test ($Cases cases)"
}

Push-Location $pluginRoot
try {
    foreach ($key in $safeEnvironment.Keys) {
        $savedEnvironment[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
        [Environment]::SetEnvironmentVariable($key, $safeEnvironment[$key], 'Process')
    }
    $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected installed SDK 8.0.416, got $sdk" }
    if (-not $PrepareOnly) {
        $activeEditors = @(Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor.exe' OR Name = 'UnrealEditor-Cmd.exe'" |
            Where-Object { -not $_.CommandLine -or $_.CommandLine.Contains($project) -or $_.CommandLine.Contains('AvidTPSTemplate') })
        if ($activeEditors.Count) { throw 'Project Editor is running; generated native types were not replaced and no process was stopped.' }
        $originalTypes = Get-Content -LiteralPath (Join-Path $nativeRoot 'AvidScriptGeneratedManifest.json') -Raw | ConvertFrom-Json
        if ((@($originalTypes.types.cpp_name | Sort-Object) -join '|') -cne 'AExplosiveProjectile|AProjectile|UEncounterSubsystem|UHealthComponent|UProfileSubsystem') {
            throw 'Canonical generated sample identity differs; no project file was replaced.'
        }
    }
    $null = New-Item -ItemType Directory -Path $runRoot -Force
    Write-Host "Generated natural reload evidence: $runRoot"
    $reusePrepared = -not [string]::IsNullOrWhiteSpace($PreparedRunRoot)
    $initialRoot = Join-Path $preparedRoot 'PreparedInitial'
    $initial = Invoke-GeneratedBuild 'Initial' $source $initialRoot 'Initial' $reusePrepared
    Test-RejectedInputs $initialRoot
    $sourceText = [IO.File]::ReadAllText($source)
    foreach ($name in @('Next', 'Reject1', 'Reject2', 'Reject3', 'AsyncFailure')) {
        $output = Join-Path $candidateRoot $name
        if (-not $reusePrepared) { foreach ($file in $files) {
            $destination = Join-Path $output $file
            $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
            Copy-Item -LiteralPath (Join-Path $initialRoot $file) -Destination $destination
        } }
        $candidateSource = Join-Path $preparedRoot "$name.cs"
        $text = $sourceText.Replace('const int CodeOffset = 0;', 'const int CodeOffset = 16;')
        if ($name.StartsWith('Reject')) { $text = $text.Replace('const int RejectBegin = 0;', "const int RejectBegin = $($name.Substring(6));") }
        if ($name -ceq 'AsyncFailure') { $text = $text.Replace('const int RejectAsyncSeed = 0;', 'const int RejectAsyncSeed = 26;') }
        if ($reusePrepared) {
            if ([IO.File]::ReadAllText($candidateSource) -cne $text) { throw "Prepared source differs from the current ordinary fixture: $name" }
        } else { [IO.File]::WriteAllText($candidateSource, $text, [Text.UTF8Encoding]::new($false)) }
        $next = Invoke-GeneratedBuild $name $candidateSource $output $name $reusePrepared
        if ($next.reload.classification -cne 'body_only' -or $next.reload.previous_package_id -cne $initial.package_id -or
            $next.package_id -ceq $initial.package_id -or $next.reload.native_structure_sha256 -cne $initial.reload.native_structure_sha256) {
            throw "Candidate changes reflection structure or fails to identify its predecessor: $name"
        }
    }
    if (-not $PrepareOnly) {
        # Runtime deliberately admits descriptor references only inside the
        # project. Publish through the same owner into a unique Saved staging
        # root; do not weaken that boundary or hand-rewrite the descriptors.
        $executionRoot = Join-Path $projectRoot ('Saved/AvidScript/GeneratedNaturalReload/' + (Split-Path -Leaf $runRoot))
        $reuseExecution = -not [string]::IsNullOrWhiteSpace($preparedExecution)
        if ($reuseExecution) { $executionRoot = $preparedExecution }
        $executionInitialRoot = Join-Path $executionRoot 'Initial'
        $executionInitial = Invoke-GeneratedBuild 'ExecutionInitial' $source $executionInitialRoot 'Initial' $reuseExecution $executionRoot
        $initialRuntime = Get-Content -LiteralPath (Join-Path $initialRoot $initial.runtime_manifest.file) -Raw | ConvertFrom-Json
        $executionInitialRuntime = Get-Content -LiteralPath (Join-Path $executionInitialRoot $executionInitial.runtime_manifest.file) -Raw | ConvertFrom-Json
        if ($executionInitial.type_manifest.sha256 -cne $initial.type_manifest.sha256 -or
            $executionInitialRuntime.wasm.sha256 -cne $initialRuntime.wasm.sha256 -or
            $executionInitial.reload.native_structure_sha256 -cne $initial.reload.native_structure_sha256) {
            throw 'Project publication changed the verified initial source contract.'
        }
        $executionCandidateRoot = Join-Path $executionRoot 'Candidates'
        foreach ($name in @('Next', 'Reject1', 'Reject2', 'Reject3', 'AsyncFailure')) {
            $output = Join-Path $executionCandidateRoot $name
            if (-not $reuseExecution) { foreach ($file in $files) {
                $destination = Join-Path $output $file
                $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
                Copy-Item -LiteralPath (Join-Path $executionInitialRoot $file) -Destination $destination
            } }
            $candidate = Invoke-GeneratedBuild ("Execution$name") (Join-Path $preparedRoot "$name.cs") $output $name $reuseExecution $executionRoot
            $prior = Get-Content -LiteralPath (Join-Path (Join-Path $candidateRoot $name) 'AvidScriptGeneratedPackage.json') -Raw | ConvertFrom-Json
            $priorRuntime = Get-Content -LiteralPath (Join-Path (Join-Path $candidateRoot $name) $prior.runtime_manifest.file) -Raw | ConvertFrom-Json
            $publishedRuntime = Get-Content -LiteralPath (Join-Path $output $candidate.runtime_manifest.file) -Raw | ConvertFrom-Json
            if ($candidate.type_manifest.sha256 -cne $prior.type_manifest.sha256 -or
                $publishedRuntime.wasm.sha256 -cne $priorRuntime.wasm.sha256 -or
                $candidate.reload.classification -cne 'body_only' -or
                $candidate.reload.previous_package_id -cne $executionInitial.package_id -or
                $candidate.reload.native_structure_sha256 -cne $initial.reload.native_structure_sha256) {
                throw "Project publication changed the verified source contract: $name"
            }
        }
        $candidateRoot = $executionCandidateRoot
        foreach ($file in $files) {
            $original = Join-Path $nativeRoot $file
            $destination = Join-Path $backupRoot $file
            $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
            $hashes[$file] = (Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash
            Copy-Item -LiteralPath $original -Destination $destination
            if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -cne $hashes[$file]) { throw 'Backup hash mismatch.' }
        }
        $hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'original-hashes.json') -Encoding utf8NoBOM
        $installed = $true
        $null = Invoke-GeneratedBuild 'InitialInstall' $source $nativeRoot 'Initial' $false $executionRoot
        $env:AVIDSCRIPT_GENERATED_NATURAL_RELOAD_ROOT = $candidateRoot
        Invoke-NativeBuild 'natural-reload-build' 'natural_reload'
        Invoke-EditorTest 'language-error-report' 'AvidScript.Runtime.LanguageErrorCatalog.AsyncVoidCheckedReport'
        Invoke-EditorTest 'natural-reload' 'AvidScript.GeneratedTypes.NaturalLanguageRollback' 36
    }
}
catch { $failure = $_ }
finally {
    try {
        if ($installed) {
            foreach ($file in $files) {
                Copy-Item -LiteralPath (Join-Path $backupRoot $file) -Destination (Join-Path $nativeRoot $file) -Force
                if ((Get-FileHash -LiteralPath (Join-Path $nativeRoot $file) -Algorithm SHA256).Hash -cne $hashes[$file]) { throw "Restore mismatch: $file" }
                [IO.File]::SetLastWriteTimeUtc((Join-Path $nativeRoot $file), [DateTime]::UtcNow)
            }
            Invoke-NativeBuild 'canonical-restore-build' 'script_defined_types'
            Invoke-EditorTest 'canonical-restore' 'AvidScript.GeneratedTypes.CSharpPropertyInteraction'
            $restored = $true
        }
        foreach ($scope in @('User', 'Machine')) {
            if ([Environment]::GetEnvironmentVariable('Path', $scope) -cne $persistentPaths[$scope]) { throw "Persistent $scope PATH changed." }
        }
    }
    finally {
        $env:AVIDSCRIPT_GENERATED_NATURAL_RELOAD_ROOT = $oldCandidateRoot
        foreach ($key in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($key, $savedEnvironment[$key], 'Process') }
        $process.PriorityClass = $oldPriority
        Pop-Location
    }
}
if ($failure) { throw $failure }
[ordered]@{ prepared = 6; runtime_cases = $(if ($PrepareOnly) { 0 } else { 36 }); runtime_executed = -not [bool]$PrepareOnly
    canonical_restored = $restored; process_priority = 'BelowNormal'; max_parallel_build_actions = 1
    persistent_path_unchanged = $true; initial_package_id = $initial.package_id; evidence = $runRoot } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8NoBOM
Write-Output "Generated natural reload: prepared=6; runtime_executed=$(-not [bool]$PrepareOnly); canonical_restored=$restored; evidence=$runRoot"
