[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CandidateRoot,

    [Parameter(Mandatory = $true)]
    [string]$SourceProjectPath,

    [Parameter(Mandatory = $true)]
    [string]$PuertsPluginPath,

    [Parameter(Mandatory = $true)]
    [string]$EngineRoot,

    [Parameter(Mandatory = $true)]
    [string]$WasmtimeInstallSource,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$LeadershipRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ComparisonRoot = Split-Path -Parent $LeadershipRoot
$RunnerPluginRoot = Split-Path -Parent (Split-Path -Parent $ComparisonRoot)
$CommonPath = Join-Path $ComparisonRoot 'Scripts/PuertsBenchmarkSidecar.Common.ps1'
. $CommonPath
. (Join-Path $RunnerPluginRoot 'Build/AvidScriptModuleReleasePackage.ps1')

function Resolve-RequiredPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ValidateSet('Leaf', 'Container')][string]$PathType,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Resolved = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $Resolved -PathType $PathType)) {
        throw "ASP65L2000 $Label is missing or has the wrong type: $Resolved"
    }
    return $Resolved
}

function Get-NormalizedTextSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Text = [IO.File]::ReadAllText($Path).Replace("`r`n", "`n").Replace("`r", "`n")
    $Bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

function Invoke-GitValue {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $Output = & git -C $Root @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "ASP65L2001 git failed: git $($Arguments -join ' ')`n$($Output -join [Environment]::NewLine)"
    }
    return ([string]@($Output)[0]).Trim().ToLowerInvariant()
}

function New-LeadershipBenchmarkProject {
    param(
        [Parameter(Mandatory = $true)][string]$SourceProjectPath,
        [Parameter(Mandatory = $true)][string]$CandidateRoot,
        [Parameter(Mandatory = $true)][string]$PuertsPluginPath,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$Commit,
        [Parameter(Mandatory = $true)][string]$Tree
    )

    $ProjectResultText = & (Join-Path $ComparisonRoot 'Scripts/New-PuertsBenchmarkProject.ps1') `
        -SourceProjectPath $SourceProjectPath `
        -AvidScriptPluginPath $CandidateRoot `
        -PuertsPluginPath $PuertsPluginPath `
        -HarnessPluginPath (Join-Path $CandidateRoot 'Benchmarks/PuertsComparison/AvidScriptPerfHarness') `
        -OutputRoot $OutputRoot `
        -ExpectedAvidScriptCommit $Commit `
        -ExpectedAvidScriptTree $Tree
    if ($LASTEXITCODE -ne 0) {
        throw 'ASP65L2008 benchmark project creation failed'
    }

    $ProjectResult = ($ProjectResultText -join "`n") | ConvertFrom-Json
    $ProjectPath = Resolve-RequiredPath -Path ([string]$ProjectResult.project_path) -PathType Leaf -Label 'generated benchmark project'
    return [pscustomobject][ordered]@{
        project_path = $ProjectPath
        project_root = Split-Path -Parent $ProjectPath
    }
}

function Invoke-LeadershipEditorBuild {
    param(
        [Parameter(Mandatory = $true)][string]$EngineRoot,
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$PassId
    )

    $BuildScript = Resolve-RequiredPath -Path (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') -PathType Leaf -Label 'UE Build.bat'
    $Target = '{0}Editor' -f [IO.Path]::GetFileNameWithoutExtension($ProjectPath)
    $LogRoot = Join-Path $ProjectRoot 'Saved/Logs/Phase65Leadership'
    New-Item -ItemType Directory -Force -Path $LogRoot | Out-Null
    $LogPath = Join-Path $LogRoot ("build-$PassId-$([Guid]::NewGuid().ToString('N')).log")
    $Output = @(& $BuildScript $Target Win64 Development "-Project=$ProjectPath" -WaitMutex -NoHotReloadFromIDE "-log=$LogPath" 2>&1)
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0) {
        $Tail = [string]::Join([Environment]::NewLine, @($Output | Select-Object -Last 80))
        throw "ASP65L2014 UE Editor build failed: pass=$PassId exit=$ExitCode log=$LogPath`n$Tail"
    }
    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
        throw "ASP65L2014 UE Editor build did not produce its evidence log: $LogPath"
    }

    return [pscustomobject][ordered]@{
        target = $Target
        log_path = $LogPath
        log_sha256 = Get-SidecarFileSha256 -Path $LogPath
    }
}

function Invoke-LeadershipProfilePreparation {
    param(
        [Parameter(Mandatory = $true)][string]$EditorExecutable,
        [Parameter(Mandatory = $true)][string]$EngineRoot,
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ProfileRelativePath,
        [Parameter(Mandatory = $true)][bool]$AllowGeneratedBuildHandshake
    )

    $ProfilePath = Resolve-RequiredPath -Path (Join-Path $ProjectRoot $ProfileRelativePath) -PathType Leaf -Label "C# artifact profile $ProfileRelativePath"
    $Profile = Get-Content -LiteralPath $ProfilePath -Raw | ConvertFrom-Json -Depth 64
    $LogRoot = Join-Path $ProjectRoot 'Saved/Logs/Phase65Leadership'
    New-Item -ItemType Directory -Force -Path $LogRoot | Out-Null
    $NormalizedProfilePath = $ProfileRelativePath.Replace('\', '/')
    $ProfileArgument = '-Profile="{0}"' -f $NormalizedProfilePath
    $GeneratedBuildCount = 0
    $Attempt = 0
    $LogPath = $null

    while ($true) {
        $Attempt++
        $LogPath = Join-Path $LogRoot ("prepare-$([string]$Profile.artifact_stem)-attempt$Attempt-$([Guid]::NewGuid().ToString('N')).log")
        $Output = @(& $EditorExecutable $ProjectPath -run=AvidScriptPerfPrepare $ProfileArgument -unattended -nop4 -nullrhi -nosplash "-abslog=$LogPath" 2>&1)
        $ExitCode = $LASTEXITCODE
        if ($ExitCode -eq 0) {
            break
        }
        if ($ExitCode -eq 4 -and $AllowGeneratedBuildHandshake -and $GeneratedBuildCount -eq 0) {
            $GeneratedBuildCount++
            $null = Invoke-LeadershipEditorBuild `
                -EngineRoot $EngineRoot `
                -ProjectPath $ProjectPath `
                -ProjectRoot $ProjectRoot `
                -PassId 'generated-binding-handshake'
            continue
        }

        $Tail = [string]::Join([Environment]::NewLine, @($Output | Select-Object -Last 80))
        throw "ASP65L2015 C# artifact preparation failed: profile=$NormalizedProfilePath attempt=$Attempt exit=$ExitCode log=$LogPath`n$Tail"
    }

    $ManifestPath = Resolve-RequiredPath -Path (Join-Path $ProjectRoot ([string]$Profile.manifest_path)) -PathType Leaf -Label "prepared manifest $($Profile.artifact_stem)"
    $ReportPath = Resolve-RequiredPath -Path (Join-Path $ProjectRoot ([string]$Profile.report_path)) -PathType Leaf -Label "prepared report $($Profile.artifact_stem)"
    return [pscustomobject][ordered]@{
        module_id = [string]$Profile.module_id
        artifact_stem = [string]$Profile.artifact_stem
        profile_path = $NormalizedProfilePath
        manifest_path = $ManifestPath
        manifest_sha256 = Get-SidecarFileSha256 -Path $ManifestPath
        report_path = $ReportPath
        report_sha256 = Get-SidecarFileSha256 -Path $ReportPath
        commandlet_log_path = $LogPath
        generated_build_count = $GeneratedBuildCount
    }
}

function Invoke-LeadershipGeneratedTypePublication {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$PackageCatalogPath,
        [Parameter(Mandatory = $true)][object[]]$Artifacts
    )

    $GeneratedLane = @($Artifacts | Where-Object {
            [string]$_.module_id -ceq 'csharp_profile_phase54_6_generated_s1'
        })
    if ($GeneratedLane.Count -ne 1) {
        throw 'ASP65L2018 generated S1 artifact is not unique'
    }
    $Report = Get-Content -LiteralPath ([string]$GeneratedLane[0].report_path) -Raw |
        ConvertFrom-Json -Depth 64
    $DotNetPath = Resolve-RequiredPath `
        -Path ([string]$Report.toolchain.dotnet) `
        -PathType Leaf `
        -Label 'Generated Type .NET host'
    $SourceId = [string]$Report.source.file
    $SourcePath = Resolve-RequiredPath `
        -Path (Join-Path $ProjectRoot $SourceId) `
        -PathType Leaf `
        -Label 'Generated Type source'
    $CSharpProjectPath = Resolve-RequiredPath `
        -Path (Join-Path $ProjectRoot ([string]$Report.source.project)) `
        -PathType Leaf `
        -Label 'Generated Type C# project'
    $BindingPackagePath = Resolve-RequiredPath `
        -Path (Join-Path $ProjectRoot ([string]$Report.binding_package.manifest_file)) `
        -PathType Leaf `
        -Label 'Generated Type binding package'
    $GeneratorPath = Resolve-RequiredPath `
        -Path (Join-Path $ProjectRoot 'Plugins/AvidScript/Build/BuildCSharpScriptTypes.ps1') `
        -PathType Leaf `
        -Label 'Generated Type builder'
    $RuntimeModuleId = 'avidscript_phase65_benchmark_generated_types'
    Push-Location -LiteralPath (Join-Path $ProjectRoot 'Plugins/AvidScript')
    try {
        $Output = @(& $GeneratorPath `
                -DotNetPath $DotNetPath `
                -SourcePath $SourcePath `
                -SourceId $SourceId `
                -BindingPackageManifestPath $BindingPackagePath `
                -ProjectPath $CSharpProjectPath `
                -RuntimeModuleId $RuntimeModuleId `
                -PackageConfiguration Development `
                -TargetPlatform Win64 `
                -HeadlessRelease 2>&1)
        $ExitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($ExitCode -ne 0) {
        $Tail = [string]::Join([Environment]::NewLine, @($Output | Select-Object -Last 80))
        throw "ASP65L2018 Generated Type publication failed: exit=$ExitCode`n$Tail"
    }

    $CurrentPath = Resolve-RequiredPath `
        -Path (Join-Path $ProjectRoot 'Plugins/AvidScript/Content/AvidScriptGenerated/current.json') `
        -PathType Leaf `
        -Label 'Generated Type current pointer'
    $Current = Get-Content -LiteralPath $CurrentPath -Raw | ConvertFrom-Json -Depth 32
    if ([string]$Current.module_id -cne $RuntimeModuleId -or
        [string]$Current.package_id -cnotmatch '^[0-9a-f]{64}$' -or
        [string]$Current.generation_key_sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw 'ASP65L2018 Generated Type current pointer has an unexpected identity'
    }
    $Catalog = Get-Content -LiteralPath $PackageCatalogPath -Raw | ConvertFrom-Json -Depth 64
    $MatchingVariants = @($Catalog.modules |
            Where-Object { [string]$_.module_id -ceq $RuntimeModuleId } |
            ForEach-Object { $_.variants } |
            Where-Object {
                [string]$_.platform -ceq 'win64' -and
                [string]$_.architecture -ceq 'x86_64' -and
                [string]$_.configuration -ceq 'development' -and
                [string]$_.package_id -ceq [string]$Current.package_id
            })
    if ($MatchingVariants.Count -ne 1) {
        throw 'ASP65L2018 Generated Type package is not a unique Win64 Development catalog variant'
    }
    $DescriptorPath = Resolve-RequiredPath `
        -Path (Join-Path (Split-Path -Parent $PackageCatalogPath) ([string]$MatchingVariants[0].descriptor_file)) `
        -PathType Leaf `
        -Label 'Generated Type runtime package descriptor'
    if ((Get-SidecarFileSha256 -Path $DescriptorPath) -cne
        [string]$MatchingVariants[0].descriptor_sha256) {
        throw 'ASP65L2018 Generated Type runtime package descriptor identity drifted'
    }
    return [pscustomobject][ordered]@{
        module_id = $RuntimeModuleId
        package_id = [string]$Current.package_id
        generation_key_sha256 = [string]$Current.generation_key_sha256
        current_path = $CurrentPath
        current_sha256 = Get-SidecarFileSha256 -Path $CurrentPath
        descriptor_path = $DescriptorPath
        descriptor_sha256 = [string]$MatchingVariants[0].descriptor_sha256
    }
}

function Invoke-LeadershipProjectPreparation {
    param(
        [Parameter(Mandatory = $true)][string]$EditorExecutable,
        [Parameter(Mandatory = $true)][string]$EngineRoot,
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$PassId
    )

    $Build = Invoke-LeadershipEditorBuild `
        -EngineRoot $EngineRoot `
        -ProjectPath $ProjectPath `
        -ProjectRoot $ProjectRoot `
        -PassId $PassId
    $Profiles = @(
        'Plugins/AvidScriptPerfHarness/Content/CSharp/AvidScriptPerfWorkload.semantic.csharp-profile.json',
        'Plugins/AvidScriptPerfHarness/Content/CSharp/AvidScriptPerfWorkload.csharp-profile.json',
        'Plugins/AvidScriptPerfHarness/Content/CSharp/AvidScriptPerfWorkload.data-oriented.csharp-profile.json'
    )
    $PackageCatalogPath = ''
    $Artifacts = @(
        foreach ($ProfilePath in $Profiles) {
            $Prepared = Invoke-LeadershipProfilePreparation `
                -EditorExecutable $EditorExecutable `
                -EngineRoot $EngineRoot `
                -ProjectPath $ProjectPath `
                -ProjectRoot $ProjectRoot `
                -ProfileRelativePath $ProfilePath `
                -AllowGeneratedBuildHandshake $true
            $Published = Publish-AvidScriptModuleReleasePackage `
                -RuntimeManifestPath ([string]$Prepared.manifest_path) `
                -ProjectRoot $ProjectRoot `
                -ModuleId ([string]$Prepared.module_id) `
                -Configuration Development `
                -TargetPlatform Win64
            [pscustomobject][ordered]@{
                module_id = [string]$Prepared.module_id
                artifact_stem = [string]$Prepared.artifact_stem
                profile_path = [string]$Prepared.profile_path
                manifest_path = [string]$Prepared.manifest_path
                manifest_sha256 = [string]$Prepared.manifest_sha256
                report_path = [string]$Prepared.report_path
                report_sha256 = [string]$Prepared.report_sha256
                commandlet_log_path = [string]$Prepared.commandlet_log_path
                generated_build_count = [int]$Prepared.generated_build_count
                package_id = [string]$Published.PackageId
                package_descriptor_path = [string]$Published.DescriptorPath
                package_descriptor_sha256 = [string]$Published.DescriptorSha256
            }
            if ([string]::IsNullOrWhiteSpace($PackageCatalogPath)) {
                $PackageCatalogPath = [string]$Published.CatalogPath
            }
            elseif ($PackageCatalogPath -cne [string]$Published.CatalogPath) {
                throw 'ASP65L2017 prepared artifacts were published to different package catalogs'
            }
        }
    )
    $GeneratedType = Invoke-LeadershipGeneratedTypePublication `
        -ProjectRoot $ProjectRoot `
        -PackageCatalogPath $PackageCatalogPath `
        -Artifacts $Artifacts
    return [pscustomobject][ordered]@{
        build = $Build
        artifacts = $Artifacts
        generated_type = $GeneratedType
        package_catalog_path = $PackageCatalogPath
        package_catalog_sha256 = Get-SidecarFileSha256 -Path $PackageCatalogPath
    }
}

function Test-LeadershipProjectSourceStable {
    param([Parameter(Mandatory = $true)][string]$ProjectRoot)

    $Marker = Read-SidecarJson -Path (Join-Path $ProjectRoot 'benchmark-project.json') -Code 'ASP65L2016'
    $SourcePath = Resolve-SidecarCanonicalDirectory -Path (Join-Path $ProjectRoot 'Source') -Code 'ASP65L2016' -Label 'benchmark project source' -RequireJunction
    $Actual = Get-SidecarDirectoryContentDigest -Path $SourcePath
    return ([string]$Actual.content_sha256 -ceq [string]$Marker.source.content_sha256 -and [int]$Actual.file_count -eq [int]$Marker.source.file_count)
}

$ResolvedCandidateRoot = Resolve-RequiredPath -Path $CandidateRoot -PathType Container -Label 'CandidateRoot'
$ResolvedRunnerPluginRoot = Resolve-RequiredPath -Path $RunnerPluginRoot -PathType Container -Label 'runner plugin root'
if ($ResolvedCandidateRoot -ine $ResolvedRunnerPluginRoot) {
    throw "ASP65L2002 candidate freezer must run from the candidate worktree: candidate=$ResolvedCandidateRoot runner=$ResolvedRunnerPluginRoot"
}
$ResolvedSourceProjectPath = Resolve-RequiredPath -Path $SourceProjectPath -PathType Leaf -Label 'SourceProjectPath'
$ResolvedPuertsPluginPath = Resolve-RequiredPath -Path $PuertsPluginPath -PathType Container -Label 'PuertsPluginPath'
$ResolvedEngineRoot = Resolve-RequiredPath -Path $EngineRoot -PathType Container -Label 'EngineRoot'
$ResolvedWasmtimeInstallSource = Resolve-RequiredPath -Path $WasmtimeInstallSource -PathType Container -Label 'WasmtimeInstallSource'
$ResolvedOutputRoot = [IO.Path]::GetFullPath($OutputRoot)

$GitTopLevel = Invoke-GitValue -Root $ResolvedCandidateRoot -Arguments @('rev-parse', '--show-toplevel')
if ([IO.Path]::GetFullPath($GitTopLevel) -ine $ResolvedCandidateRoot) {
    throw 'ASP65L2003 CandidateRoot must be the AvidScript Git root'
}
$Commit = Invoke-GitValue -Root $ResolvedCandidateRoot -Arguments @('rev-parse', 'HEAD')
$Tree = Invoke-GitValue -Root $ResolvedCandidateRoot -Arguments @('rev-parse', 'HEAD^{tree}')
$Status = & git -C $ResolvedCandidateRoot status --porcelain=v1 --untracked-files=all
if ($LASTEXITCODE -ne 0 -or ($Status -join "`n").Trim().Length -ne 0) {
    throw "ASP65L2004 leadership candidate must be clean: $($Status -join '; ')"
}

$ProtocolPath = Join-Path $LeadershipRoot 'Config/Phase65LeadershipProtocol.json'
$ProtocolSchemaPath = Join-Path $LeadershipRoot 'Schema/Phase65LeadershipProtocol.schema.json'
$CandidateSchemaPath = Join-Path $LeadershipRoot 'Schema/Phase65LeadershipCandidate.schema.json'
$ProtocolRaw = Get-Content -LiteralPath $ProtocolPath -Raw
if (-not ($ProtocolRaw | Test-Json -SchemaFile $ProtocolSchemaPath)) {
    throw 'ASP65L2005 leadership protocol does not satisfy schema v1'
}
$Protocol = $ProtocolRaw | ConvertFrom-Json -Depth 64
foreach ($Input in @($Protocol.tracked_inputs)) {
    $InputPath = Resolve-RequiredPath -Path (Join-Path $ResolvedCandidateRoot ([string]$Input.relative_path)) -PathType Leaf -Label "tracked input $($Input.id)"
    if ((Get-NormalizedTextSha256 -Path $InputPath) -cne [string]$Input.sha256) {
        throw "ASP65L2006 tracked leadership input drifted: $($Input.id)"
    }
}

$WasmtimeLockPath = Join-Path $ResolvedCandidateRoot 'Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.lock.json'
$WasmtimeLock = Get-Content -LiteralPath $WasmtimeLockPath -Raw | ConvertFrom-Json -Depth 32
$WasmtimeDestination = [IO.Path]::GetFullPath((Join-Path $ResolvedCandidateRoot ([string]$WasmtimeLock.install.relative_path)))
$SourceMarkerPath = Join-Path $ResolvedWasmtimeInstallSource ([string]$WasmtimeLock.install.managed_marker_name)
$SourceMarker = Get-Content -LiteralPath (Resolve-RequiredPath -Path $SourceMarkerPath -PathType Leaf -Label 'Wasmtime source marker') -Raw | ConvertFrom-Json -Depth 32
if ([string]$SourceMarker.toolchain_id -cne [string]$WasmtimeLock.toolchain_id -or
    [string]$SourceMarker.compiler_profile -cne [string]$WasmtimeLock.compiler_profile.id -or
    [string]$SourceMarker.patch_sha256 -cne [string]$WasmtimeLock.patch.canonical_sha256) {
    throw 'ASP65L2012 Wasmtime install source does not match the frozen toolchain lock'
}
if (-not (Test-Path -LiteralPath $WasmtimeDestination -PathType Container)) {
    $WasmtimeParent = Split-Path -Parent $WasmtimeDestination
    New-Item -ItemType Directory -Force -Path $WasmtimeParent | Out-Null
    $PublishPath = "$WasmtimeDestination.publish-$([Guid]::NewGuid().ToString('N'))"
    try {
        Copy-Item -LiteralPath $ResolvedWasmtimeInstallSource -Destination $PublishPath -Recurse
        [IO.Directory]::Move($PublishPath, $WasmtimeDestination)
    }
    finally {
        if (Test-Path -LiteralPath $PublishPath -PathType Container) {
            Remove-Item -LiteralPath $PublishPath -Recurse -Force
        }
    }
}
$WasmtimeVerifyText = & (Join-Path $ResolvedCandidateRoot 'Build/BuildAvidScriptWasmtimePerformanceToolchain.ps1') `
    -Mode Verify `
    -RepositoryRoot $ResolvedCandidateRoot
if ($LASTEXITCODE -ne 0) {
    throw 'ASP65L2013 copied Wasmtime performance toolchain failed candidate verification'
}
$WasmtimeEvidence = (($WasmtimeVerifyText -join "`n") | ConvertFrom-Json).evidence

$EditorExecutable = Resolve-RequiredPath `
    -Path (Join-Path $ResolvedEngineRoot ([string]$Protocol.host.editor_relative_path)) `
    -PathType Leaf `
    -Label 'UnrealEditor-Cmd'
$EditorIdentity = Assert-SidecarFormalEditorExecutable `
    -EditorExecutable $EditorExecutable `
    -UeVersion ([string]$Protocol.host.ue_version)
$EngineBuildId = '{0};sha256={1}' -f [string]$Protocol.host.ue_version, [string]$EditorIdentity.sha256

$PuertsLock = Get-Content -LiteralPath (Join-Path $ComparisonRoot 'Config/PuertsDependency.lock.json') -Raw | ConvertFrom-Json -Depth 32
$MarkerPath = Join-Path $ResolvedPuertsPluginPath ([string]$PuertsLock.installation.managed_marker_name)
$PuertsMarker = Get-Content -LiteralPath (Resolve-RequiredPath -Path $MarkerPath -PathType Leaf -Label 'Puerts managed marker') -Raw | ConvertFrom-Json -Depth 32
$PuertsContent = Get-SidecarInstalledPuertsContentDigest `
    -Path $ResolvedPuertsPluginPath `
    -ManagedMarkerName ([string]$PuertsLock.installation.managed_marker_name)
if ([string]$PuertsMarker.source_commit_sha -cne [string]$Protocol.competitors.puerts.source_commit -or
    [string]$PuertsMarker.backend_sha256 -cne [string]$Protocol.competitors.puerts.backend_sha256 -or
    [string]$PuertsMarker.installed_content_sha256 -cne [string]$PuertsContent.content_sha256 -or
    [int]$PuertsMarker.installed_file_count -ne [int]$PuertsContent.file_count) {
    throw 'ASP65L2007 installed Puerts identity differs from the frozen protocol or managed marker'
}

$Project = New-LeadershipBenchmarkProject `
    -SourceProjectPath $ResolvedSourceProjectPath `
    -CandidateRoot $ResolvedCandidateRoot `
    -PuertsPluginPath $ResolvedPuertsPluginPath `
    -OutputRoot $ResolvedOutputRoot `
    -Commit $Commit `
    -Tree $Tree
$Preparation = Invoke-LeadershipProjectPreparation `
    -EditorExecutable $EditorExecutable `
    -EngineRoot $ResolvedEngineRoot `
    -ProjectPath $Project.project_path `
    -ProjectRoot $Project.project_root `
    -PassId 'initial'
$StabilizationPasses = 1
if (-not (Test-LeadershipProjectSourceStable -ProjectRoot $Project.project_root)) {
    $Project = New-LeadershipBenchmarkProject `
        -SourceProjectPath $ResolvedSourceProjectPath `
        -CandidateRoot $ResolvedCandidateRoot `
        -PuertsPluginPath $ResolvedPuertsPluginPath `
        -OutputRoot $ResolvedOutputRoot `
        -Commit $Commit `
        -Tree $Tree
    $Preparation = Invoke-LeadershipProjectPreparation `
        -EditorExecutable $EditorExecutable `
        -EngineRoot $ResolvedEngineRoot `
        -ProjectPath $Project.project_path `
        -ProjectRoot $Project.project_root `
        -PassId 'stabilized'
    $StabilizationPasses++
}
$BenchmarkProjectPath = $Project.project_path
$BenchmarkProjectRoot = $Project.project_root
if (-not (Test-LeadershipProjectSourceStable -ProjectRoot $BenchmarkProjectRoot)) {
    throw 'ASP65L2016 generated project source did not stabilize after the bounded two-pass preparation'
}
$null = Assert-SidecarBenchmarkProjectProvenance `
    -ProjectPath $BenchmarkProjectPath `
    -AvidScriptCommit $Commit `
    -AvidScriptTreeSha $Tree
Assert-SidecarPuertsProvenance `
    -ProjectPath $BenchmarkProjectPath `
    -PuertsCommit ([string]$Protocol.competitors.puerts.source_commit) `
    -PuertsBackendSha256 ([string]$Protocol.competitors.puerts.backend_sha256)

$CpuNames = @(Get-CimInstance Win32_Processor | ForEach-Object { ([string]$_.Name).Trim() } | Where-Object { $_ })
$CpuModel = [string]::Join(' + ', $CpuNames)
if ([string]::IsNullOrWhiteSpace($CpuModel)) {
    throw 'ASP65L2009 CPU identity is unavailable'
}
$CandidateIdPayload = [Text.UTF8Encoding]::new($false).GetBytes("$Commit`n$Tree`n$($EditorIdentity.sha256)`n$($WasmtimeEvidence.installed_content_sha256)`n$($PuertsContent.content_sha256)`n$($Preparation.package_catalog_sha256)`n")
$CandidateId = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($CandidateIdPayload)).ToLowerInvariant().Substring(0, 20)
$Matrices = @(
    [ordered]@{ id = 'ue_micro_six_lane'; status = 'ready'; reason = '六 lane micro profile 与三种 AvidScript binding artifact 已构建并冻结。' },
    [ordered]@{ id = 'ue_gameplay_six_lane'; status = 'ready'; reason = 'small/dense gameplay frame 与 data-oriented artifact 已构建并冻结。' },
    [ordered]@{ id = 'identical_wasm_execution'; status = 'ready'; reason = '同 WASM suite、Cranelift/V8 身份与 UE 候选环境已冻结。' },
    [ordered]@{ id = 'angelscript_same_semantics'; status = 'blocked'; reason = [string]$Protocol.competitors.angelscript.reason }
)
$Candidate = [ordered]@{
    schema_version = 3
    candidate_id = $CandidateId
    created_utc = [DateTimeOffset]::UtcNow.ToString('o')
    protocol = [ordered]@{
        id = [string]$Protocol.protocol_id
        path = $ProtocolPath
        sha256 = Get-NormalizedTextSha256 -Path $ProtocolPath
    }
    candidate = [ordered]@{
        root = $ResolvedCandidateRoot
        commit = $Commit
        tree = $Tree
        clean = $true
    }
    engine = [ordered]@{
        root = $ResolvedEngineRoot
        version = [string]$Protocol.host.ue_version
        editor_executable = $EditorExecutable
        editor_sha256 = [string]$EditorIdentity.sha256
        build_id = $EngineBuildId
    }
    host = [ordered]@{
        cpu = $CpuModel
        logical_processors = [Environment]::ProcessorCount
        os = [Runtime.InteropServices.RuntimeInformation]::OSDescription
    }
    wasmtime = [ordered]@{
        root = [string]$WasmtimeEvidence.install_path
        dll_sha256 = [string]$WasmtimeEvidence.dll_sha256
        installed_content_sha256 = [string]$WasmtimeEvidence.installed_content_sha256
        compiler_profile = [string]$WasmtimeEvidence.compiler_profile
        identical_wasm_compiler_profile =
            [string]$WasmtimeLock.fuel_free_compiler_profile.id
    }
    puerts = [ordered]@{
        root = $ResolvedPuertsPluginPath
        source_commit = [string]$PuertsMarker.source_commit_sha
        backend_sha256 = [string]$PuertsMarker.backend_sha256
        installed_content_sha256 = [string]$PuertsContent.content_sha256
        installed_file_count = [int]$PuertsContent.file_count
    }
    benchmark_project = [ordered]@{
        root = $BenchmarkProjectRoot
        project_path = $BenchmarkProjectPath
        marker_sha256 = Get-SidecarFileSha256 -Path (Join-Path $BenchmarkProjectRoot 'benchmark-project.json')
        build_target = [string]$Preparation.build.target
        build_log_path = [string]$Preparation.build.log_path
        build_log_sha256 = [string]$Preparation.build.log_sha256
        source_stabilization_passes = $StabilizationPasses
        package_catalog_path = [string]$Preparation.package_catalog_path
        package_catalog_sha256 = [string]$Preparation.package_catalog_sha256
        generated_type = $Preparation.generated_type
        artifacts = @($Preparation.artifacts)
    }
    matrices = $Matrices
    complete_leadership_claim_ready = $false
}
$CandidateJson = (($Candidate | ConvertTo-Json -Depth 64) -replace "`r`n", "`n") + "`n"
if (-not ($CandidateJson | Test-Json -SchemaFile $CandidateSchemaPath)) {
    throw 'ASP65L2010 generated leadership candidate does not satisfy schema v3'
}
$CandidatePath = Join-Path $BenchmarkProjectRoot 'phase65-leadership-candidate.json'
if (Test-Path -LiteralPath $CandidatePath) {
    throw "ASP65L2011 refusing to overwrite leadership candidate manifest: $CandidatePath"
}
[IO.File]::WriteAllText($CandidatePath, $CandidateJson, [Text.UTF8Encoding]::new($false))

[pscustomobject][ordered]@{
    result = 'phase65_leadership_candidate_frozen'
    candidate_id = $CandidateId
    candidate_path = $CandidatePath
    candidate_sha256 = Get-SidecarFileSha256 -Path $CandidatePath
    project_path = $BenchmarkProjectPath
    commit = $Commit
    tree = $Tree
    complete_leadership_claim_ready = $false
    blocked_matrix = 'angelscript_same_semantics'
} | ConvertTo-Json -Depth 16
