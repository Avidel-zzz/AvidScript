[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
$InstallerPath = Join-Path $ScriptRoot 'Install-PuertsBenchmarkDependency.ps1'
$LockPath = Join-Path $BenchmarkRoot 'Config/PuertsDependency.lock.json'
$SchemaPath = Join-Path $BenchmarkRoot 'Schema/PuertsDependencyLock.schema.json'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "ASP53T1000 $Message"
    }
}

function Invoke-ExpectedFailure {
    param(
        [Parameter(Mandatory = $true)][string]$CandidateLockPath,
        [Parameter(Mandatory = $true)][string]$ExpectedCode
    )

    $Output = & pwsh -NoProfile -File $InstallerPath -Mode ValidateLock -LockPath $CandidateLockPath 2>&1
    Assert-True ($LASTEXITCODE -ne 0) "invalid lock unexpectedly passed: $CandidateLockPath"
    Assert-True (($Output -join "`n").Contains($ExpectedCode)) "failure did not contain $ExpectedCode"
}

$ParserErrors = $null
$Tokens = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(
    $InstallerPath,
    [ref]$Tokens,
    [ref]$ParserErrors)
Assert-True (@($ParserErrors).Count -eq 0) 'installer has PowerShell parser errors'

$SchemaRaw = Get-Content -LiteralPath $SchemaPath -Raw
$LockRaw = Get-Content -LiteralPath $LockPath -Raw
Assert-True ($LockRaw | Test-Json -SchemaFile $SchemaPath) 'tracked dependency lock does not satisfy its schema'

$ResultRaw = & pwsh -NoProfile -File $InstallerPath -Mode ValidateLock -LockPath $LockPath
Assert-True ($LASTEXITCODE -eq 0) 'tracked dependency lock validation failed'
$Result = $ResultRaw | ConvertFrom-Json
Assert-True ($Result.succeeded -eq $true) 'tracked dependency lock did not return succeeded=true'
Assert-True ($Result.source_commit_sha -cmatch '^[0-9a-f]{40}$') 'source commit is not immutable'
Assert-True ($Result.backend_sha256 -cmatch '^[0-9a-f]{64}$') 'backend digest is not immutable'

$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('AvidScriptP53DependencyTest-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force -Path $FixtureRoot | Out-Null

    $BadRemote = $LockRaw | ConvertFrom-Json
    $BadRemote.source.repository_url = 'https://example.invalid/puerts.git'
    $BadRemotePath = Join-Path $FixtureRoot 'bad-remote.json'
    $BadRemote | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $BadRemotePath -Encoding utf8NoBOM
    Invoke-ExpectedFailure $BadRemotePath 'ASP53D1003'

    $BadCommit = $LockRaw | ConvertFrom-Json
    $BadCommit.source.commit_sha = 'main'
    $BadCommitPath = Join-Path $FixtureRoot 'bad-commit.json'
    $BadCommit | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $BadCommitPath -Encoding utf8NoBOM
    Invoke-ExpectedFailure $BadCommitPath 'ASP53D1003'

    $BadAsset = $LockRaw | ConvertFrom-Json
    $BadAsset.backend.asset_url = 'https://example.invalid/puerts_v8_94.tgz'
    $BadAssetPath = Join-Path $FixtureRoot 'bad-asset.json'
    $BadAsset | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $BadAssetPath -Encoding utf8NoBOM
    Invoke-ExpectedFailure $BadAssetPath 'ASP53D1003'
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

Write-Output 'Puerts dependency contracts passed: parser=1 schema=1 valid_lock=1 rejected_remote=1 rejected_commit=1 rejected_asset=1'
