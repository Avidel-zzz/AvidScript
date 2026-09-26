param(
    [Parameter(Mandatory = $true)][string]$BindingPackageManifestPath,
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [string]$EditorCmdPath = 'C:\UnrealEngine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    # Only when the installed binaries already include script_defined_types tests.
    [switch]$SkipNativeBuild
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
$CanonicalSource = Join-Path $PluginRoot 'Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs'
$CandidateSource = Join-Path $PluginRoot 'Tests/Fixtures/CSharp/P66B_ScriptDefinedTypesBodyCandidate.cs'
$CurrentPointerPath = Join-Path $PluginRoot 'Content/AvidScriptGenerated/current.json'
$BuildScript = Join-Path $PluginRoot 'Build/BuildCSharpScriptTypes.ps1'
$PowerShellPath = Join-Path $PSHOME 'pwsh.exe'
$SourceId = 'Plugins/AvidScript/Samples/CSharp/ScriptDefinedTypes/ScriptDefinedTypes.cs'
$RunRoot = Join-Path $ProjectRoot ('Saved/AvidScript/GeneratedEventVersionReload/' + [guid]::NewGuid().ToString('N'))
$NativeRoot = Join-Path $RunRoot 'Native'
$ArtifactRoot = Join-Path $RunRoot 'Artifacts'
$DescriptorPath = Join-Path $NativeRoot 'AvidScriptGeneratedPackage.json'
$GameLogPath = Join-Path $RunRoot 'EditorAutomation-Game.log'
$PieLogPath = Join-Path $RunRoot 'EditorAutomation-PIE.log'

foreach ($RequiredFile in @(
        $BindingPackageManifestPath, $DotNetPath, $EditorCmdPath, $ProjectPath,
        $CanonicalSource, $CandidateSource, $CurrentPointerPath, $BuildScript,
        $PowerShellPath)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "Required version-reload input is missing: $RequiredFile"
    }
}
$BindingPackageManifestPath = (Resolve-Path -LiteralPath $BindingPackageManifestPath).Path
$CurrentPointer = Get-Content -Raw -LiteralPath $CurrentPointerPath | ConvertFrom-Json
if ([int]$CurrentPointer.schema_version -ne 2) {
    throw 'The installed generated type package pointer must use schema 2.'
}

function Invoke-GeneratedTypeBuild {
    param([string]$SourcePath)

    & $PowerShellPath -NoProfile -File $BuildScript `
        -DotNetPath $DotNetPath `
        -SourcePath $SourcePath `
        -SourceId $SourceId `
        -BindingPackageManifestPath $BindingPackageManifestPath `
        -OutputRoot $NativeRoot `
        -ArtifactRoot $ArtifactRoot | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Generated type build failed for $SourcePath with exit code $LASTEXITCODE."
    }
    return Get-Content -Raw -LiteralPath $DescriptorPath | ConvertFrom-Json
}

$Baseline = Invoke-GeneratedTypeBuild -SourcePath $CanonicalSource
if ([string]$Baseline.generation_key_sha256 -cne [string]$CurrentPointer.generation_key_sha256 -or
    [string]$Baseline.reload.native_structure_sha256 -cne [string]$CurrentPointer.reload.native_structure_sha256) {
    throw 'The installed generated type package does not match the canonical C# source. Reinstall the canonical package before this test.'
}
$Candidate = Invoke-GeneratedTypeBuild -SourcePath $CandidateSource
if ([string]$Candidate.package_id -ceq [string]$Baseline.package_id -or
    [string]$Candidate.reload.classification -cne 'body_only' -or
    [string]$Candidate.reload.previous_package_id -cne [string]$Baseline.package_id -or
    [string]$Candidate.reload.native_structure_sha256 -cne [string]$Baseline.reload.native_structure_sha256 -or
    [string]$Candidate.execution_backend -cne 'wasmtime_jit') {
    throw 'The separately built candidate is not a distinct body-only JIT package.'
}

$PreviousDescriptor = [Environment]::GetEnvironmentVariable(
    'AVIDSCRIPT_GENERATED_EVENT_CANDIDATE_DESCRIPTOR', 'Process')
$PreviousPieWorld = [Environment]::GetEnvironmentVariable(
    'AVIDSCRIPT_GENERATED_EVENT_TEST_PIE_WORLD', 'Process')
try {
    if (-not $SkipNativeBuild) {
        $NativeBuildScript = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $EditorCmdPath) '../../Build/BatchFiles/Build.bat'))
        $NativeLog = Join-Path $RunRoot 'native-build.log'
        & $NativeBuildScript AvidTPSTemplateEditor Win64 Development "-Project=$ProjectPath" `
            -AvidScriptGeneratedTestSuite=script_defined_types -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles -gather *> $NativeLog
        if ($LASTEXITCODE -ne 0) { throw "Generated sample test build failed: $NativeLog" }
    }
    [Environment]::SetEnvironmentVariable(
        'AVIDSCRIPT_GENERATED_EVENT_CANDIDATE_DESCRIPTOR', $DescriptorPath, 'Process')
    foreach ($WorldCase in @(
            @{ Name = 'Game'; PieWorld = '0'; LogPath = $GameLogPath },
            @{ Name = 'PIE'; PieWorld = '1'; LogPath = $PieLogPath })) {
        [Environment]::SetEnvironmentVariable(
            'AVIDSCRIPT_GENERATED_EVENT_TEST_PIE_WORLD', $WorldCase.PieWorld, 'Process')
        & $EditorCmdPath $ProjectPath `
            -unattended -nop4 -nosplash -nullrhi -nosound `
            '-ExecCmds=Automation RunTests AvidScript.GeneratedTypes.CSharpEventAwaitReload;Quit' `
            "-abslog=$($WorldCase.LogPath)"
        $EditorExitCode = $LASTEXITCODE
        if (-not (Test-Path -LiteralPath $WorldCase.LogPath -PathType Leaf)) {
            throw "Editor Automation did not write its $($WorldCase.Name) log: $($WorldCase.LogPath)"
        }
        $Log = Get-Content -Raw -LiteralPath $WorldCase.LogPath
        if ($EditorExitCode -ne 0 -or
            $Log -notmatch "Found 1 automation tests based on 'AvidScript.GeneratedTypes.CSharpEventAwaitReload'" -or
            $Log -notmatch 'Test Completed\. Result=\{Success\} Name=\{CSharpEventAwaitReload\}' -or
            $Log -notmatch '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*') {
            throw "Generated C# event reload failed in $($WorldCase.Name) World. Editor exit=$EditorExitCode; log=$($WorldCase.LogPath)"
        }
    }
}
finally {
    [Environment]::SetEnvironmentVariable(
        'AVIDSCRIPT_GENERATED_EVENT_CANDIDATE_DESCRIPTOR', $PreviousDescriptor, 'Process')
    [Environment]::SetEnvironmentVariable(
        'AVIDSCRIPT_GENERATED_EVENT_TEST_PIE_WORLD', $PreviousPieWorld, 'Process')
}

[pscustomobject]@{
    result = 'generated_csharp_event_version_reload_passed'
    baseline_package_id = $Baseline.package_id
    candidate_package_id = $Candidate.package_id
    native_structure_sha256 = $Candidate.reload.native_structure_sha256
    editor_test_count = 2
    editor_failed_count = 0
    game_log = $GameLogPath
    pie_log = $PieLogPath
} | ConvertTo-Json -Depth 4
