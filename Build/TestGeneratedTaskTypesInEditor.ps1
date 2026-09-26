[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BindingPackageManifestPath,
    [ValidateSet('SharedTask', 'AsyncThrow')][string]$Fixture = 'SharedTask',
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent $PSScriptRoot
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$project = Join-Path $projectRoot 'AvidTPSTemplate.uproject'
$nativeRoot = Join-Path $pluginRoot 'Source/AvidScriptGenerated'
$runRoot = Join-Path $projectRoot ('Saved/AvidScript/GeneratedTaskEditor/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$backupRoot = Join-Path $runRoot 'Original'
$candidateRoot = Join-Path $runRoot 'Candidate'
$isThrow = $Fixture -ceq 'AsyncThrow'
$fixtureName = if ($isThrow) { 'GeneratedAsyncThrow' } else { 'SharedTaskLifetime' }
$sourceId = "Fixtures/Phase66/$fixtureName.cs"
$source = Join-Path $pluginRoot $sourceId
$fixtureProject = Join-Path $pluginRoot "Fixtures/Phase66/$fixtureName.csproj"
$engineName = if ($isThrow) { 'AsyncThrowActor' } else { 'SharedTaskActor' }
$suite = if ($isThrow) { 'async_throw' } else { 'shared_task' }
$test = if ($isThrow) { 'AvidScript.GeneratedTypes.AsyncThrowLifecycle' } else { 'AvidScript.GeneratedTypes.TaskUhtLifecycle' }
$caseCount = if ($isThrow) { 108 } else { 12 }
$automationCount = if ($isThrow) { 5 } else { 3 }
$casePattern = if ($isThrow) { 'generated async throw passed scenario=[0-5] mode=[0-5] point=[0-2]' } else { 'generated Task UHT passed mode=[0-5] cancel=[01]' }
$summaryName = if ($isThrow) { 'GeneratedAsyncThrowLifecycle' } else { 'GeneratedTaskUhtLifecycle' }
$runtimeModuleId = if ($isThrow) { 'avidscript.fixture.generated_async_throw' } else { 'avidscript.fixture.shared_task_types' }
$build = Join-Path $pluginRoot 'Build/BuildCSharpScriptTypes.ps1'
$package = (Resolve-Path -LiteralPath $BindingPackageManifestPath).Path
$editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$files = @('Public/AvidScriptGeneratedTypes.h', 'Private/AvidScriptGeneratedTypes.cpp',
    'AvidScriptGeneratedManifest.json', 'AvidScriptGeneratedPackage.json')
$hashes = @{}
$previousCandidate = $env:AVIDSCRIPT_GENERATED_TASK_CANDIDATE
$previousCliHome = $env:DOTNET_CLI_HOME
$installed = $false
$restored = $false
$failure = $null

function Invoke-NativeBuild([string]$Name, [string]$Suite) {
    $log = Join-Path $runRoot "$Name.log"
    # The suite is a command-line input to UBT's makefile identity.
    & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') AvidTPSTemplateEditor Win64 Development `
        "-Project=$project" "-AvidScriptGeneratedTestSuite=$Suite" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles -gather *> $log
    if ($LASTEXITCODE -ne 0) {
        Get-Content $log -Tail 45
        throw "No-clean native build failed: $log"
    }
    Write-Output "PASS $Name ($Suite)"
}

function Invoke-EditorTest([string]$Name, [string]$Test, [int]$Cases = 0) {
    $log = Join-Path $runRoot "$Name.log"
    & $editor $project -unattended -nop4 -NullRHI -nosplash -nosound `
        "-ExecCmds=Automation RunTests $Test;Quit" '-TestExit=Automation Test Queue Empty' "-abslog=$log" *> (Join-Path $runRoot "$Name.stdout.log")
    $exitCode = $LASTEXITCODE
    $text = Get-Content -LiteralPath $log -Raw
    $testName = [regex]::Escape($Test)
    if ($exitCode -ne 0 -or
        [regex]::Matches($text, "Found 1 automation tests based on '$testName'").Count -ne 1 -or
        [regex]::Matches($text, 'Test Completed\. Result=\{Success\} Name=\{[^}]+\} Path=\{' + $testName + '\}').Count -ne 1 -or
        $text -match 'Test Completed\. Result=\{Fail\}' -or
        [regex]::Matches($text, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -ne 1) {
        Get-Content $log -Tail 35
        throw "Editor test failed or incomplete: $log (exit=$exitCode)"
    }
    if ($Cases -gt 0 -and (@([regex]::Matches($text, $casePattern).Value | Sort-Object -Unique).Count -ne $Cases -or
        -not $text.Contains("${summaryName}: $Cases/$caseCount passed"))) {
        throw "Editor Task scenario count mismatch: $log"
    }
    Write-Output "PASS $Test"
}

function Invoke-GuestBuild([string]$Name, [string]$Source, [string]$Output) {
    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File $build -DotNetPath $DotNetPath `
        -SourcePath $Source -SourceId $sourceId -ProjectPath $fixtureProject `
        -BindingPackageManifestPath $package -OutputRoot $Output `
        -ArtifactRoot (Join-Path $runRoot "$Name/Artifacts") -CookOutputRoot (Join-Path $runRoot "$Name/Cook") `
        -RuntimeModuleId $runtimeModuleId `
        -LanguageErrors bounded -AsyncExceptionFlow -DirectAwaitCleanup -AsyncCancellationFlow *> (Join-Path $runRoot "$Name.log")
    if ($LASTEXITCODE -ne 0) { throw "Formal generated Task build failed: $runRoot/$Name.log" }
    $descriptorPath = Join-Path $Output 'AvidScriptGeneratedPackage.json'
    $descriptor = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json
    foreach ($entry in @($descriptor.type_manifest, $descriptor.runtime_manifest)) {
        if ((Get-FileHash -LiteralPath (Join-Path $Output $entry.file) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) {
            throw 'Published artifact hash mismatch'
        }
    }
    $types = Get-Content -LiteralPath (Join-Path $Output $descriptor.type_manifest.file) -Raw | ConvertFrom-Json
    if (@($types.types).Count -ne 1 -or $types.types[0].engine_name -cne $engineName) { throw 'Unexpected generated fixture' }
    if ($isThrow -and ($types.semantic_schema_version -ne 46 -or $types.semantic_version -cne '1.55')) {
        throw 'Generated throw fixture must use Semantic 46/1.55'
    }
    if ($isThrow) {
        $runtimePath = Join-Path $Output $descriptor.runtime_manifest.file
        $runtime = Get-Content -LiteralPath $runtimePath -Raw | ConvertFrom-Json
        $runtimeRoot = Split-Path -Parent $runtimePath
        $ir = Get-Content -LiteralPath (Join-Path $runtimeRoot 'generated_types.guestir.json') -Raw | ConvertFrom-Json
        $wasmHash = (Get-FileHash -LiteralPath (Join-Path $runtimeRoot 'generated_types.wasm') -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($ir.schema_version -ne 26 -or $ir.ir_version -cne '1.25' -or
            $runtime.wasm.sha256 -cne $wasmHash -or $runtime.module_id -cne $runtimeModuleId -or
            $ir.task_local_lifetimes.exception_model -cne 'cancellation') {
            throw 'Generated throw Runtime must publish exact IR 26/1.25 and matching WASM'
        }
    }
    foreach ($entry in $types.outputs) {
        if ((Get-FileHash -LiteralPath (Join-Path $Output $entry.relative_path) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) {
            throw 'Generated native source hash mismatch'
        }
    }
    return $descriptor
}

Push-Location $pluginRoot
try {
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }
    $activeEditors = @(Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor.exe' OR Name = 'UnrealEditor-Cmd.exe'" |
        Where-Object { -not $_.CommandLine -or $_.CommandLine.Contains($project) -or $_.CommandLine.Contains('AvidTPSTemplate') })
    if ($activeEditors.Count) { throw 'Close the project Editor before replacing its generated native types; no process was stopped.' }
    $originalTypes = Get-Content -LiteralPath (Join-Path $nativeRoot 'AvidScriptGeneratedManifest.json') -Raw | ConvertFrom-Json
    if ((@($originalTypes.types.cpp_name | Sort-Object) -join '|') -cne 'AExplosiveProjectile|AProjectile|UEncounterSubsystem|UHealthComponent|UProfileSubsystem') {
        throw 'This repository test requires the installed ScriptDefinedTypes sample so its canonical restoration can be validated.'
    }
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'AvidScriptGeneratedTaskCliHome'
    $null = New-Item -ItemType Directory -Path $backupRoot -Force
    Write-Output "Generated Task Editor evidence: $runRoot"
    if ($isThrow) {
        & $DotNetPath run --project (Join-Path $pluginRoot 'Tools/AvidScript.CSharpGuest.Tests/AvidScript.CSharpGuest.Tests.csproj') `
            -c Release -- --generated-async-throw *> (Join-Path $runRoot 'managed-reference.log')
        if ($LASTEXITCODE -ne 0 -or
            (Get-Content -LiteralPath (Join-Path $runRoot 'managed-reference.log') -Raw) -notmatch 'GeneratedAsyncThrow: 48/48 passed') {
            throw 'Same-source .NET/compiler reference failed'
        }
    }
    # Require a complete original installation. Never delete an unknown or
    # partially installed project to make this fixture fit.
    foreach ($file in $files) {
        $original = Join-Path $nativeRoot $file
        $destination = Join-Path $backupRoot $file
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
        $hashes[$file] = (Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash
        Copy-Item -LiteralPath $original -Destination $destination
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -cne $hashes[$file]) { throw 'Backup hash mismatch' }
    }
    $hashes | ConvertTo-Json | Set-Content (Join-Path $runRoot 'original-hashes.json') -Encoding utf8NoBOM
    $installed = $true
    $initial = Invoke-GuestBuild 'initial' $source $nativeRoot
    # Seed the owner builder's previous-package metadata. The next invocation
    # replaces the descriptor with paths relative to the candidate directory.
    foreach ($file in $files) {
        $destination = Join-Path $candidateRoot $file
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
        Copy-Item -LiteralPath (Join-Path $nativeRoot $file) -Destination $destination
        $archive = Join-Path $runRoot "InstalledInitial/$file"
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $archive) -Force
        Copy-Item -LiteralPath (Join-Path $nativeRoot $file) -Destination $archive
    }
    $nextSource = Join-Path $runRoot "${fixtureName}Next.cs"
    $sourceText = [IO.File]::ReadAllText($source)
    if ([regex]::Matches($sourceText, [regex]::Escape('const int CodeOffset = 0;')).Count -ne 1) { throw 'Expected one code generation constant' }
    [IO.File]::WriteAllText($nextSource, $sourceText.Replace('const int CodeOffset = 0;', 'const int CodeOffset = 16;'), [Text.UTF8Encoding]::new($false))
    $next = Invoke-GuestBuild 'next' $nextSource $candidateRoot
    if ($next.reload.classification -cne 'body_only' -or $next.reload.previous_package_id -cne $initial.package_id -or
        $next.package_id -ceq $initial.package_id -or $next.reload.native_structure_sha256 -cne $initial.reload.native_structure_sha256) {
        throw 'Candidate must preserve UHT structure and change executable generation'
    }
    $env:AVIDSCRIPT_GENERATED_TASK_CANDIDATE = Join-Path $candidateRoot 'AvidScriptGeneratedPackage.json'
    Invoke-NativeBuild 'custom-default-build' 'none'
    Invoke-EditorTest 'custom-reflection' 'AvidScript.GeneratedTypes.Reflection'
    Invoke-NativeBuild 'task-suite-build' $suite
    if ($isThrow) {
        Invoke-EditorTest 'task-ownership' 'AvidScript.Runtime.Continuation.TaskContinuationOwnership'
        Invoke-EditorTest 'task-abi' 'AvidScript.Runtime.Continuation.TaskResultAbi'
    }
    Invoke-EditorTest 'task-lifecycle' $test $caseCount
}
catch { $failure = $_ }
finally {
    try {
        if ($installed) {
            foreach ($file in $files) {
                Copy-Item -LiteralPath (Join-Path $backupRoot $file) -Destination (Join-Path $nativeRoot $file) -Force
                if ((Get-FileHash -LiteralPath (Join-Path $nativeRoot $file) -Algorithm SHA256).Hash -cne $hashes[$file]) { throw "Restore mismatch: $file" }
                # Copy-Item preserves the old timestamp. UHT must see the restored
                # declarations as newer than the temporary fixture's generated header.
                [IO.File]::SetLastWriteTimeUtc((Join-Path $nativeRoot $file), [DateTime]::UtcNow)
            }
            Invoke-NativeBuild 'canonical-restore-build' 'script_defined_types'
            Invoke-EditorTest 'canonical-restore' 'AvidScript.GeneratedTypes.CSharpPropertyInteraction'
            $restored = $true
        }
    }
    finally {
        $env:AVIDSCRIPT_GENERATED_TASK_CANDIDATE = $previousCandidate
        $env:DOTNET_CLI_HOME = $previousCliHome
        Pop-Location
    }
}
if ($failure) { throw $failure }
[ordered]@{ fixture = $Fixture; passed = $caseCount; total = $caseCount; automation_passed = $automationCount; canonical_restored = $restored;
    initial_package_id = $initial.package_id; candidate_package_id = $next.package_id; evidence = $runRoot } |
    ConvertTo-Json | Set-Content (Join-Path $runRoot 'results.json') -Encoding utf8NoBOM
Write-Output "Generated Task UHT ($Fixture): $caseCount/$caseCount; Automation=$automationCount/$automationCount; canonical restored=$restored; evidence=$runRoot"
