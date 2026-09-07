[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$EngineRoot,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [switch]$FinalizeExistingArchive
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$leadershipRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$comparisonRoot = Split-Path -Parent $leadershipRoot
. (Join-Path $comparisonRoot 'Scripts/PuertsBenchmarkSidecar.Common.ps1')

function Resolve-RequiredPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ValidateSet('Leaf', 'Container')][string]$PathType,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $resolved = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolved -PathType $PathType)) {
        throw "ASP65H2000 $Label is missing or has the wrong type: $resolved"
    }
    return $resolved
}

function Assert-GeneratedTypeIdentity {
    param(
        [Parameter(Mandatory = $true)]$Candidate,
        [Parameter(Mandatory = $true)][string]$ProjectRoot
    )

    $ExpectedCurrentPath = [IO.Path]::GetFullPath(
        (Join-Path $ProjectRoot 'Plugins/AvidScript/Content/AvidScriptGenerated/current.json'))
    $CurrentPath = Resolve-RequiredPath `
        -Path ([string]$Candidate.benchmark_project.generated_type.current_path) `
        -PathType Leaf `
        -Label 'Generated Type current pointer'
    if ($CurrentPath -ine $ExpectedCurrentPath -or
        (Get-SidecarFileSha256 -Path $CurrentPath) -cne
            [string]$Candidate.benchmark_project.generated_type.current_sha256) {
        throw 'ASP65H2002 Generated Type current pointer identity drifted'
    }
    $DescriptorPath = Resolve-RequiredPath `
        -Path ([string]$Candidate.benchmark_project.generated_type.descriptor_path) `
        -PathType Leaf `
        -Label 'Generated Type runtime package descriptor'
    if ((Get-SidecarFileSha256 -Path $DescriptorPath) -cne
        [string]$Candidate.benchmark_project.generated_type.descriptor_sha256) {
        throw 'ASP65H2002 Generated Type runtime package descriptor identity drifted'
    }
}

$candidatePath = Resolve-RequiredPath -Path $CandidateManifestPath -PathType Leaf -Label 'candidate manifest'
$enginePath = Resolve-RequiredPath -Path $EngineRoot -PathType Container -Label 'UE root'
$runUatPath = Resolve-RequiredPath `
    -Path (Join-Path $enginePath 'Engine/Build/BatchFiles/RunUAT.bat') `
    -PathType Leaf `
    -Label 'RunUAT.bat'
$candidateSchemaPath = Join-Path $leadershipRoot 'Schema/Phase65LeadershipCandidate.schema.json'
$hostSchemaPath = Join-Path $leadershipRoot 'Schema/Phase65PackagedBenchmarkHost.schema.json'
$candidateRaw = Get-Content -LiteralPath $candidatePath -Raw
if (-not ($candidateRaw | Test-Json -SchemaFile $candidateSchemaPath)) {
    throw 'ASP65H2001 candidate manifest does not satisfy the frozen schema'
}
$candidate = $candidateRaw | ConvertFrom-Json -Depth 64
$projectPath = Resolve-RequiredPath `
    -Path ([string]$candidate.benchmark_project.project_path) `
    -PathType Leaf `
    -Label 'candidate project'
$projectRoot = Split-Path -Parent $projectPath
$expectedCatalogPath = [IO.Path]::GetFullPath(
    (Join-Path $projectRoot 'Content/AvidScript/Modules/catalog.json'))
$catalogPath = Resolve-RequiredPath `
    -Path ([string]$candidate.benchmark_project.package_catalog_path) `
    -PathType Leaf `
    -Label 'candidate package catalog'
if ($catalogPath -ine $expectedCatalogPath) {
    throw "ASP65H2002 candidate package catalog is outside the production default path: $catalogPath"
}
if ((Get-SidecarFileSha256 -Path $catalogPath) -cne
    [string]$candidate.benchmark_project.package_catalog_sha256) {
    throw 'ASP65H2002 candidate package catalog identity drifted'
}
$markerPath = Resolve-RequiredPath `
    -Path (Join-Path $projectRoot 'benchmark-project.json') `
    -PathType Leaf `
    -Label 'benchmark project marker'
if ((Get-SidecarFileSha256 -Path $markerPath) -cne [string]$candidate.benchmark_project.marker_sha256) {
    throw 'ASP65H2002 benchmark project marker identity drifted'
}
[void](Assert-SidecarBenchmarkProjectProvenance `
    -ProjectPath $projectPath `
    -AvidScriptCommit ([string]$candidate.candidate.commit) `
    -AvidScriptTreeSha ([string]$candidate.candidate.tree))
[void](Assert-GeneratedTypeIdentity -Candidate $candidate -ProjectRoot $projectRoot)
$resolvedOutput = [IO.Path]::GetFullPath($OutputRoot)
if ((Test-Path -LiteralPath $resolvedOutput) -and -not $FinalizeExistingArchive) {
    throw "ASP65H2003 refusing to overwrite packaged-host output: $resolvedOutput"
}
$archiveRoot = Join-Path $resolvedOutput 'archive'
$uatLogPath = Join-Path $resolvedOutput 'build-cook-run.log'
$target = [IO.Path]::GetFileNameWithoutExtension($projectPath)
if ($FinalizeExistingArchive) {
    $null = Resolve-RequiredPath -Path $resolvedOutput -PathType Container -Label 'packaged-host output'
    $null = Resolve-RequiredPath -Path $archiveRoot -PathType Container -Label 'completed archive'
    $null = Resolve-RequiredPath -Path $uatLogPath -PathType Leaf -Label 'completed UAT log'
    $uatOutput = Get-Content -LiteralPath $uatLogPath -Raw
    if (-not $uatOutput.Contains('BUILD SUCCESSFUL') -or
        -not $uatOutput.Contains('AutomationTool exiting with ExitCode=0')) {
        throw 'ASP65H2005 existing archive has no successful BuildCookRun evidence'
    }
}
else {
    [void][IO.Directory]::CreateDirectory($resolvedOutput)
    [void][IO.Directory]::CreateDirectory($archiveRoot)
    $uatArguments = @(
        'BuildCookRun',
        '-nop4',
        '-unattended',
        '-utf8output',
        "-project=$projectPath",
        "-target=$target",
        '-targetplatform=Win64',
        '-clientconfig=Development',
        '-build',
        '-skipbuildeditor',
        '-cook',
        '-stage',
        '-pak',
        '-archive',
        "-archivedirectory=$archiveRoot",
        '-AdditionalCookerOptions=-SkipZenStore -AvidScriptSuppressGeneratedTypeExecution -EnablePlugins=AvidScriptPerfHarness,Puerts',
        '-ubtargs=-EnablePlugin=AvidScriptPerfHarness+Puerts -WaitMutex -NoHotReloadFromIDE'
    )
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = Join-Path $env:SystemRoot 'System32/cmd.exe'
    $startInfo.WorkingDirectory = $projectRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in @('/d', '/s', '/c', $runUatPath) + $uatArguments) {
        [void]$startInfo.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'ASP65H2004 RunUAT process did not start'
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $uatOutput = $stdoutTask.GetAwaiter().GetResult()
    $uatError = $stderrTask.GetAwaiter().GetResult()
    if (-not [string]::IsNullOrWhiteSpace($uatError)) {
        $uatOutput += [Environment]::NewLine + $uatError
    }
    [IO.File]::WriteAllText($uatLogPath, $uatOutput, [Text.UTF8Encoding]::new($false))
    if ($process.ExitCode -ne 0) {
        $tail = [string]::Join(
            [Environment]::NewLine,
            @($uatOutput -split '\r?\n' | Select-Object -Last 80))
        throw "ASP65H2005 BuildCookRun failed with exit code $($process.ExitCode): $uatLogPath`n$tail"
    }
}
$executablePath = Resolve-RequiredPath `
    -Path (Join-Path $archiveRoot "Windows/$target.exe") `
    -PathType Leaf `
    -Label 'packaged host launcher'
$runtimeExecutablePath = Resolve-RequiredPath `
    -Path (Join-Path $archiveRoot "Windows/$target/Binaries/Win64/$target.exe") `
    -PathType Leaf `
    -Label 'packaged host runtime executable'
$pakFiles = @(Get-ChildItem -LiteralPath $archiveRoot -Filter '*.pak' -File -Recurse |
    Sort-Object FullName)
if ($pakFiles.Count -eq 0) {
    throw 'ASP65H2007 packaged host contains no Pak files'
}
[void](Assert-SidecarBenchmarkProjectProvenance `
    -ProjectPath $projectPath `
    -AvidScriptCommit ([string]$candidate.candidate.commit) `
    -AvidScriptTreeSha ([string]$candidate.candidate.tree))
[void](Assert-GeneratedTypeIdentity -Candidate $candidate -ProjectRoot $projectRoot)
if ((Get-SidecarFileSha256 -Path $catalogPath) -cne
    [string]$candidate.benchmark_project.package_catalog_sha256) {
    throw 'ASP65H2007 BuildCookRun changed the frozen package catalog'
}
$archiveDigest = Get-SidecarDirectoryContentDigest -Path $archiveRoot
$HostManifest = [ordered]@{
    schema_version = 2
    created_utc = [DateTimeOffset]::UtcNow.ToString('o')
    candidate_id = [string]$candidate.candidate_id
    candidate_manifest_sha256 = Get-SidecarFileSha256 -Path $candidatePath
    candidate_commit = [string]$candidate.candidate.commit
    candidate_tree = [string]$candidate.candidate.tree
    package_catalog_sha256 = [string]$candidate.benchmark_project.package_catalog_sha256
    configuration = 'Development'
    target = $target
    uat_log_path = $uatLogPath
    uat_log_sha256 = Get-SidecarFileSha256 -Path $uatLogPath
    archive_root = $archiveRoot
    archive_content_sha256 = [string]$archiveDigest.content_sha256
    archive_file_count = [int]$archiveDigest.file_count
    executable_path = $executablePath
    executable_sha256 = Get-SidecarFileSha256 -Path $executablePath
    runtime_executable_path = $runtimeExecutablePath
    runtime_executable_sha256 = Get-SidecarFileSha256 -Path $runtimeExecutablePath
    pak_files = @($pakFiles | ForEach-Object {
        [ordered]@{
            path = $_.FullName
            sha256 = Get-SidecarFileSha256 -Path $_.FullName
            size_bytes = [int64]$_.Length
        }
    })
}
$hostJson = (($HostManifest | ConvertTo-Json -Depth 32) -replace "`r`n", "`n") + "`n"
if (-not ($hostJson | Test-Json -SchemaFile $hostSchemaPath)) {
    throw 'ASP65H2008 packaged host result does not satisfy schema v2'
}
$hostPath = Join-Path $resolvedOutput 'phase65-packaged-benchmark-host-v2.json'
if (Test-Path -LiteralPath $hostPath) {
    throw "ASP65H2008 refusing to overwrite packaged host manifest: $hostPath"
}
[IO.File]::WriteAllText($hostPath, $hostJson, [Text.UTF8Encoding]::new($false))
[pscustomobject][ordered]@{
    result = 'phase65_packaged_benchmark_host_ready'
    host_path = $hostPath
    host_sha256 = Get-SidecarFileSha256 -Path $hostPath
    executable_path = $executablePath
    executable_sha256 = [string]$HostManifest.executable_sha256
    runtime_executable_path = $runtimeExecutablePath
    runtime_executable_sha256 = [string]$HostManifest.runtime_executable_sha256
    pak_count = $pakFiles.Count
}
