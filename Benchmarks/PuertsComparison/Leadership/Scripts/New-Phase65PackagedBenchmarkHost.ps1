[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$EngineRoot,
    [Parameter(Mandatory = $true)][string]$OutputRoot
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
$resolvedOutput = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $resolvedOutput) {
    throw "ASP65H2003 refusing to overwrite packaged-host output: $resolvedOutput"
}
[void][IO.Directory]::CreateDirectory($resolvedOutput)
$archiveRoot = Join-Path $resolvedOutput 'archive'
[void][IO.Directory]::CreateDirectory($archiveRoot)
$uatLogPath = Join-Path $resolvedOutput 'build-cook-run.log'
$target = [IO.Path]::GetFileNameWithoutExtension($projectPath)
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
$executables = @(Get-ChildItem -LiteralPath $archiveRoot -Filter "$target.exe" -File -Recurse)
if ($executables.Count -ne 1) {
    throw "ASP65H2006 packaged host must contain exactly one $target.exe"
}
$pakFiles = @(Get-ChildItem -LiteralPath $archiveRoot -Filter '*.pak' -File -Recurse |
    Sort-Object FullName)
if ($pakFiles.Count -eq 0) {
    throw 'ASP65H2007 packaged host contains no Pak files'
}
[void](Assert-SidecarBenchmarkProjectProvenance `
    -ProjectPath $projectPath `
    -AvidScriptCommit ([string]$candidate.candidate.commit) `
    -AvidScriptTreeSha ([string]$candidate.candidate.tree))
if ((Get-SidecarFileSha256 -Path $catalogPath) -cne
    [string]$candidate.benchmark_project.package_catalog_sha256) {
    throw 'ASP65H2007 BuildCookRun changed the frozen package catalog'
}
$archiveDigest = Get-SidecarDirectoryContentDigest -Path $archiveRoot
$host = [ordered]@{
    schema_version = 1
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
    executable_path = $executables[0].FullName
    executable_sha256 = Get-SidecarFileSha256 -Path $executables[0].FullName
    pak_files = @($pakFiles | ForEach-Object {
        [ordered]@{
            path = $_.FullName
            sha256 = Get-SidecarFileSha256 -Path $_.FullName
            size_bytes = [int64]$_.Length
        }
    })
}
$hostJson = (($host | ConvertTo-Json -Depth 32) -replace "`r`n", "`n") + "`n"
if (-not ($hostJson | Test-Json -SchemaFile $hostSchemaPath)) {
    throw 'ASP65H2008 packaged host result does not satisfy schema v1'
}
$hostPath = Join-Path $resolvedOutput 'phase65-packaged-benchmark-host.json'
[IO.File]::WriteAllText($hostPath, $hostJson, [Text.UTF8Encoding]::new($false))
[pscustomobject][ordered]@{
    result = 'phase65_packaged_benchmark_host_ready'
    host_path = $hostPath
    host_sha256 = Get-SidecarFileSha256 -Path $hostPath
    executable_path = $executables[0].FullName
    executable_sha256 = [string]$host.executable_sha256
    pak_count = $pakFiles.Count
}
