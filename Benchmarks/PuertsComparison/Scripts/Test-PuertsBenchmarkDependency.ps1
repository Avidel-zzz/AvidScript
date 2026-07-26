[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
$InstallerPath = Join-Path $ScriptRoot 'Install-PuertsBenchmarkDependency.ps1'
$LockPath = Join-Path $BenchmarkRoot 'Config/PuertsDependency.lock.json'
$SchemaPath = Join-Path $BenchmarkRoot 'Schema/PuertsDependencyLock.schema.json'

function Assert-True {
    param([Parameter(Mandatory = $true)][bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)

    if (-not $Condition) {
        throw "ASP53T1000 $Message"
    }
}

function Invoke-Installer {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('ValidateLock', 'Install', 'Verify')][string]$Mode,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)][string]$CandidateLockPath,
        [string]$DownloadUriOverride = ''
    )

    $Arguments = @(
        '-NoProfile', '-File', $InstallerPath,
        '-Mode', $Mode,
        '-ProjectRoot', $ProjectRoot,
        '-CacheRoot', $CacheRoot,
        '-LockPath', $CandidateLockPath)
    if (-not [string]::IsNullOrWhiteSpace($DownloadUriOverride)) {
        $Arguments += @('-DownloadUriOverride', $DownloadUriOverride)
    }
    $Output = & pwsh @Arguments 2>&1
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Text = $Output -join "`n"
    }
}

function Assert-InstallerFailure {
    param([Parameter(Mandatory = $true)]$Invocation, [Parameter(Mandatory = $true)][string]$ExpectedCode)

    Assert-True ($Invocation.ExitCode -ne 0) "expected installer failure containing $ExpectedCode"
    Assert-True ($Invocation.Text.Contains($ExpectedCode)) "installer failure did not contain ${ExpectedCode}: $($Invocation.Text)"
}

function New-TestProject {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][string]$Name)

    $ProjectRoot = Join-Path $Root $Name
    New-Item -ItemType Directory -Force -Path $ProjectRoot | Out-Null
    '{ "FileVersion": 3 }' | Set-Content -LiteralPath (Join-Path $ProjectRoot "$Name.uproject") -Encoding utf8NoBOM
    return $ProjectRoot
}

function New-TestSourceCache {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$SeedBareRepository
    )

    New-Item -ItemType Directory -Force -Path $Root | Out-Null
    Copy-Item -LiteralPath $SeedBareRepository -Destination (Join-Path $Root 'puerts-upstream.git') -Recurse -Force
}

function New-TestBackendArchive {
    param(
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)][string]$AssetName,
        [Parameter(Mandatory = $true)][string]$FixtureRoot
    )

    $ArchiveRoot = Join-Path $FixtureRoot ('archive-' + [Guid]::NewGuid().ToString('N'))
    $V8Root = Join-Path $ArchiveRoot 'Puerts/ThirdParty/v8_9.4.146.24'
    New-Item -ItemType Directory -Force -Path (Join-Path $V8Root 'Inc') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $V8Root 'Bin/Win64') | Out-Null
    'v8 test header' | Set-Content -LiteralPath (Join-Path $V8Root 'Inc/v8.h') -Encoding utf8NoBOM
    'v8 test dll' | Set-Content -LiteralPath (Join-Path $V8Root 'Bin/Win64/v8.dll') -Encoding utf8NoBOM
    $ArchivePath = Join-Path $CacheRoot $AssetName
    & tar -czf $ArchivePath -C $ArchiveRoot Puerts
    Assert-True ($LASTEXITCODE -eq 0) 'unable to create local V8 fixture archive'
    return $ArchivePath
}

function Invoke-Git {
    param([Parameter(Mandatory = $true)][string]$WorkingDirectory, [Parameter(Mandatory = $true)][string[]]$Arguments)

    $Output = & git -C $WorkingDirectory @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "ASP53T1001 git fixture setup failed: $($Output -join "`n")"
    }
    return @($Output)
}

$ParserErrors = $null
$Tokens = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($InstallerPath, [ref]$Tokens, [ref]$ParserErrors)
Assert-True (@($ParserErrors).Count -eq 0) 'installer has PowerShell parser errors'

$LockRaw = Get-Content -LiteralPath $LockPath -Raw
Assert-True ($LockRaw | Test-Json -SchemaFile $SchemaPath) 'tracked dependency lock does not satisfy its schema'

$ValidateResult = & pwsh -NoProfile -File $InstallerPath -Mode ValidateLock -LockPath $LockPath
Assert-True ($LASTEXITCODE -eq 0) 'tracked dependency lock validation failed'
Assert-True (($ValidateResult | ConvertFrom-Json).succeeded -eq $true) 'tracked dependency lock did not return succeeded=true'

$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('AvidScriptP53DependencyTest-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force -Path $FixtureRoot | Out-Null
    $SeedWorktree = Join-Path $FixtureRoot 'seed-worktree'
    Invoke-Git $FixtureRoot @('init', 'seed-worktree') | Out-Null
    Invoke-Git $SeedWorktree @('config', 'user.email', 'dependency-test@example.invalid') | Out-Null
    Invoke-Git $SeedWorktree @('config', 'user.name', 'Dependency Test') | Out-Null
    $PluginRoot = Join-Path $SeedWorktree 'unreal/Puerts'
    New-Item -ItemType Directory -Force -Path (Join-Path $PluginRoot 'Source/JsEnv') | Out-Null
    '{ "FileVersion": 3 }' | Set-Content -LiteralPath (Join-Path $PluginRoot 'Puerts.uplugin') -Encoding utf8NoBOM
    'public class JsEnvRules {}' | Set-Content -LiteralPath (Join-Path $PluginRoot 'Source/JsEnv/JsEnv.Build.cs') -Encoding utf8NoBOM
    'source header' | Set-Content -LiteralPath (Join-Path $PluginRoot 'Source/JsEnv/JsEnv.h') -Encoding utf8NoBOM
    Invoke-Git $SeedWorktree @('add', '.') | Out-Null
    Invoke-Git $SeedWorktree @('commit', '-m', 'fixture source') | Out-Null
    $CommitOutput = @(Invoke-Git $SeedWorktree @('rev-parse', 'HEAD'))
    $Commit = ([string]$CommitOutput[-1]).Trim()
    $TreeOutput = @(Invoke-Git $SeedWorktree @('rev-parse', "${Commit}:unreal/Puerts"))
    $Tree = ([string]$TreeOutput[-1]).Trim()
    $SeedBare = Join-Path $FixtureRoot 'seed-bare.git'
    & git clone --bare $SeedWorktree $SeedBare 2>&1 | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'unable to create local bare source fixture'
    Invoke-Git $SeedBare @('remote', 'set-url', 'origin', 'https://github.com/Tencent/puerts.git') | Out-Null

    $CacheRoot = Join-Path $FixtureRoot 'cache'
    New-TestSourceCache $CacheRoot $SeedBare
    $ArchivePath = New-TestBackendArchive $CacheRoot 'fixture-v8.tgz' $FixtureRoot
    $TestLock = $LockRaw | ConvertFrom-Json
    $TestLock.source.commit_sha = $Commit
    $TestLock.source.plugin_tree_sha1 = $Tree
    $TestLock.backend.asset_name = 'fixture-v8.tgz'
    $TestLock.backend.asset_url = 'https://github.com/Tencent/puerts/releases/download/test/fixture-v8.tgz'
    $TestLock.backend.size_bytes = [int64](Get-Item -LiteralPath $ArchivePath).Length
    $TestLock.backend.sha256 = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $TestLockPath = Join-Path $FixtureRoot 'fixture-lock.json'
    $TestLock | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $TestLockPath -Encoding utf8NoBOM
    Assert-True ((Get-Content -LiteralPath $TestLockPath -Raw) | Test-Json -SchemaFile $SchemaPath) 'fixture lock does not satisfy its schema'

    $HappyProject = New-TestProject $FixtureRoot 'happy'
    $Happy = Invoke-Installer Install $HappyProject $CacheRoot $TestLockPath
    Assert-True ($Happy.ExitCode -eq 0) "happy-path install failed: $($Happy.Text)"
    $MarkerPath = Join-Path $HappyProject 'Plugins/Puerts/.avidscript-puerts-install.json'
    $Marker = Get-Content -LiteralPath $MarkerPath -Raw | ConvertFrom-Json
    Assert-True ($Marker.schema_version -eq 2) 'installed marker did not use content-integrity schema version 2'
    Assert-True ($Marker.installed_content_sha256 -cmatch '^[0-9a-f]{64}$') 'installed marker is missing a deterministic content digest'
    Assert-True ([int64]$Marker.installed_file_count -gt 0) 'installed marker is missing a deterministic file count'
    New-Item -ItemType Directory -Force -Path (Join-Path $HappyProject 'Plugins/Puerts/Binaries/Win64') | Out-Null
    'generated binary' | Set-Content -LiteralPath (Join-Path $HappyProject 'Plugins/Puerts/Binaries/Win64/UnrealEditor-Puerts.dll') -Encoding utf8NoBOM
    $Verified = Invoke-Installer Verify $HappyProject $CacheRoot $TestLockPath
    Assert-True ($Verified.ExitCode -eq 0) "happy-path verify failed: $($Verified.Text)"
    $CachedArchiveProject = New-TestProject $FixtureRoot 'cached-archive'
    $CachedArchiveInstall = Invoke-Installer Install $CachedArchiveProject $CacheRoot $TestLockPath 'http://127.0.0.1:1/fixture-v8.tgz'
    Assert-True ($CachedArchiveInstall.ExitCode -eq 0) "existing valid cache was not used atomically: $($CachedArchiveInstall.Text)"
    Assert-True ((Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $TestLock.backend.sha256) 'existing valid cache archive was overwritten'

    $SourceProject = New-TestProject $FixtureRoot 'source-tamper'
    Assert-True ((Invoke-Installer Install $SourceProject $CacheRoot $TestLockPath).ExitCode -eq 0) 'source-tamper fixture install failed'
    Add-Content -LiteralPath (Join-Path $SourceProject 'Plugins/Puerts/Source/JsEnv/JsEnv.h') -Value 'tampered source'
    Assert-InstallerFailure (Invoke-Installer Verify $SourceProject $CacheRoot $TestLockPath) 'ASP53D1802'

    $BackendProject = New-TestProject $FixtureRoot 'backend-tamper'
    Assert-True ((Invoke-Installer Install $BackendProject $CacheRoot $TestLockPath).ExitCode -eq 0) 'backend-tamper fixture install failed'
    Add-Content -LiteralPath (Join-Path $BackendProject 'Plugins/Puerts/ThirdParty/v8_9.4.146.24/Bin/Win64/v8.dll') -Value 'tampered backend'
    Assert-InstallerFailure (Invoke-Installer Verify $BackendProject $CacheRoot $TestLockPath) 'ASP53D1802'

    $MarkerProject = New-TestProject $FixtureRoot 'marker-tamper'
    Assert-True ((Invoke-Installer Install $MarkerProject $CacheRoot $TestLockPath).ExitCode -eq 0) 'marker-tamper fixture install failed'
    $TamperedMarkerPath = Join-Path $MarkerProject 'Plugins/Puerts/.avidscript-puerts-install.json'
    $TamperedMarker = Get-Content -LiteralPath $TamperedMarkerPath -Raw | ConvertFrom-Json
    $TamperedMarker.installed_content_sha256 = ('0' * 64)
    $TamperedMarker | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $TamperedMarkerPath -Encoding utf8NoBOM
    Assert-InstallerFailure (Invoke-Installer Verify $MarkerProject $CacheRoot $TestLockPath) 'ASP53D1802'

    $FailureCacheRoot = Join-Path $FixtureRoot 'failure-cache'
    New-TestSourceCache $FailureCacheRoot $SeedBare
    $FailureProject = New-TestProject $FixtureRoot 'download-failure'
    $DownloadFailure = Invoke-Installer Install $FailureProject $FailureCacheRoot $TestLockPath 'http://127.0.0.1:1/fixture-v8.tgz'
    Assert-InstallerFailure $DownloadFailure 'ASP53D1303'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $FailureCacheRoot $TestLock.backend.asset_name))) 'failed download left a final cache file'
    $Residuals = @(Get-ChildItem -LiteralPath $FailureCacheRoot -File | Where-Object { $_.Name -like "$($TestLock.backend.asset_name).partial-*.tmp" })
    Assert-True ($Residuals.Count -eq 0) 'failed download left a temporary cache file'
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

Write-Output 'Puerts dependency contracts passed: parser=1 schema=1 happy=1 source_tamper=1 backend_tamper=1 marker_tamper=1 atomic_download=1'
