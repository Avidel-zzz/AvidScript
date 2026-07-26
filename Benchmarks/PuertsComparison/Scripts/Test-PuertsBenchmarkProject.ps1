[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BootstrapPath = Join-Path $ScriptRoot 'New-PuertsBenchmarkProject.ps1'
$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('AvidScriptP53CleanProjectTest-' + [Guid]::NewGuid().ToString('N'))

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "ASP53BT1000 $Message"
    }
}

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)]$Actual,
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ([string]$Actual -cne [string]$Expected) {
        throw "ASP53BT1001 $Message actual=[$Actual] expected=[$Expected]"
    }
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value
    )

    $Parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrEmpty($Parent)) {
        [System.IO.Directory]::CreateDirectory($Parent) | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Value, [System.Text.UTF8Encoding]::new($false))
}

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryPath,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )

    $Output = & git -C $RepositoryPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "ASP53BT1002 git failed: git -C $RepositoryPath $($Arguments -join ' ')`n$($Output -join [Environment]::NewLine)"
    }

    return @($Output)
}

function Invoke-ExpectedFailure {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$ExpectedCode
    )

    try {
        & $Action
        throw "ASP53BT1003 expected failure was not raised: $ExpectedCode"
    }
    catch {
        Assert-True ($_.Exception.Message.Contains($ExpectedCode)) "failure did not contain ${ExpectedCode}: $($_.Exception.Message)"
    }
}

function Assert-JunctionTarget {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedTarget
    )

    $Item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    Assert-Equal $Item.LinkType 'Junction' "expected junction at $Path"
    Assert-Equal ([System.IO.Path]::GetFullPath(([string]$Item.Target))) $ExpectedTarget "junction target mismatch for $Path"
}

Assert-True (Test-Path -LiteralPath $BootstrapPath -PathType Leaf) "missing source file $BootstrapPath"

$ParserErrors = $null
$Tokens = $null
foreach ($ScriptPath in @($BootstrapPath, $MyInvocation.MyCommand.Path)) {
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $ScriptPath,
        [ref]$Tokens,
        [ref]$ParserErrors)
    Assert-True (@($ParserErrors).Count -eq 0) "PowerShell parser errors: $ScriptPath"
}

$BootstrapText = Get-Content -LiteralPath $BootstrapPath -Raw
$Separator = [System.IO.Path]::DirectorySeparatorChar
$PersonalPathToken = 'C:' + $Separator + 'Users' + $Separator
Assert-True (-not $BootstrapText.Contains($PersonalPathToken)) 'tracked bootstrap script contains a literal personal path'

try {
    New-Item -ItemType Directory -Path $FixtureRoot | Out-Null

    $SourceProjectRoot = Join-Path $FixtureRoot 'SourceProject'
    $SourceProjectPath = Join-Path $SourceProjectRoot 'AvidTPSTemplate.uproject'
    Write-Utf8NoBom -Path $SourceProjectPath -Value "{`n  `"FileVersion`": 3`n}`n"
    Write-Utf8NoBom -Path (Join-Path $SourceProjectRoot 'Source\Gameplay.txt') -Value "source`n"
    Write-Utf8NoBom -Path (Join-Path $SourceProjectRoot 'Config\DefaultGame.ini') -Value "[/Script/EngineSettings.GameMapsSettings]`n"
    Write-Utf8NoBom -Path (Join-Path $SourceProjectRoot 'Plugins\AvidScript\should-not-leak.txt') -Value "main-plugin`n"
    foreach ($GeneratedDirectory in @('Binaries', 'Intermediate', 'Saved', 'DerivedDataCache')) {
        Write-Utf8NoBom -Path (Join-Path $SourceProjectRoot "$GeneratedDirectory\sentinel.txt") -Value "$GeneratedDirectory`n"
    }

    $RepositoryRoot = Join-Path $FixtureRoot 'AvidScriptRepo'
    New-Item -ItemType Directory -Force -Path $RepositoryRoot | Out-Null
    Write-Utf8NoBom -Path (Join-Path $RepositoryRoot 'AvidScript.uplugin') -Value "{`n  `"FileVersion`": 3`n}`n"
    Write-Utf8NoBom -Path (Join-Path $RepositoryRoot 'PluginSource\candidate.txt') -Value "candidate`n"
    Write-Utf8NoBom -Path (Join-Path $RepositoryRoot 'Benchmarks\PuertsComparison\AvidScriptPerfHarness\AvidScriptPerfHarness.uplugin') -Value "{`n  `"FileVersion`": 3`n}`n"
    Write-Utf8NoBom -Path (Join-Path $RepositoryRoot 'Benchmarks\PuertsComparison\AvidScriptPerfHarness\Harness.txt') -Value "harness`n"

    Invoke-Git -RepositoryPath $RepositoryRoot init --quiet | Out-Null
    Invoke-Git -RepositoryPath $RepositoryRoot config user.name 'Codex Fixture' | Out-Null
    Invoke-Git -RepositoryPath $RepositoryRoot config user.email 'fixture@example.invalid' | Out-Null
    Invoke-Git -RepositoryPath $RepositoryRoot add . | Out-Null
    Invoke-Git -RepositoryPath $RepositoryRoot commit --quiet -m 'fixture' | Out-Null

    $CandidateWorktreePath = Join-Path $FixtureRoot 'AvidScriptCandidate'
    Invoke-Git -RepositoryPath $RepositoryRoot worktree add --quiet -b benchmark-candidate $CandidateWorktreePath HEAD | Out-Null

    $PuertsPluginPath = Join-Path $FixtureRoot 'PinnedPuerts'
    Write-Utf8NoBom -Path (Join-Path $PuertsPluginPath 'Puerts.uplugin') -Value "{`n  `"FileVersion`": 3`n}`n"
    Write-Utf8NoBom -Path (Join-Path $PuertsPluginPath 'Runtime\puerts.txt') -Value "puerts`n"

    $OutputRoot = Join-Path $FixtureRoot 'Outputs'
    New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

    $HarnessPluginPath = Join-Path $CandidateWorktreePath 'Benchmarks\PuertsComparison\AvidScriptPerfHarness'
    $ExpectedCommit = (@(Invoke-Git -RepositoryPath $CandidateWorktreePath rev-parse HEAD))[0].Trim().ToLowerInvariant()
    $ExpectedTree = (@(Invoke-Git -RepositoryPath $CandidateWorktreePath rev-parse 'HEAD^{tree}'))[0].Trim().ToLowerInvariant()

    $FirstRun = (& $BootstrapPath `
        -SourceProjectPath $SourceProjectPath `
        -AvidScriptPluginPath $CandidateWorktreePath `
        -PuertsPluginPath $PuertsPluginPath `
        -HarnessPluginPath $HarnessPluginPath `
        -OutputRoot $OutputRoot `
        -ExpectedAvidScriptCommit $ExpectedCommit `
        -ExpectedAvidScriptTree $ExpectedTree) | ConvertFrom-Json

    $AttemptPath = [System.IO.Path]::GetFullPath(([string]$FirstRun.attempt_path))
    $AttemptProjectPath = [System.IO.Path]::GetFullPath(([string]$FirstRun.project_path))
    Assert-True (Test-Path -LiteralPath $AttemptPath -PathType Container) 'attempt directory was not created'
    Assert-Equal $AttemptProjectPath (Join-Path $AttemptPath 'AvidTPSTemplate.uproject') 'project copy path mismatch'
    Assert-Equal ([string]($FirstRun.commit)) $ExpectedCommit 'result commit mismatch'
    Assert-Equal ([string]($FirstRun.tree)) $ExpectedTree 'result tree mismatch'
    Assert-True ($FirstRun.dirty -eq $false) 'result dirty flag must be false'

    Assert-True (Test-Path -LiteralPath $AttemptProjectPath -PathType Leaf) 'uproject copy is missing'
    Assert-Equal (Get-Content -LiteralPath $AttemptProjectPath -Raw) (Get-Content -LiteralPath $SourceProjectPath -Raw) 'uproject copy content mismatch'

    Assert-JunctionTarget -Path (Join-Path $AttemptPath 'Source') -ExpectedTarget ([System.IO.Path]::GetFullPath((Join-Path $SourceProjectRoot 'Source')))
    Assert-JunctionTarget -Path (Join-Path $AttemptPath 'Config') -ExpectedTarget ([System.IO.Path]::GetFullPath((Join-Path $SourceProjectRoot 'Config')))
    Assert-JunctionTarget -Path (Join-Path $AttemptPath 'Plugins\AvidScript') -ExpectedTarget ([System.IO.Path]::GetFullPath($CandidateWorktreePath))
    Assert-JunctionTarget -Path (Join-Path $AttemptPath 'Plugins\Puerts') -ExpectedTarget ([System.IO.Path]::GetFullPath($PuertsPluginPath))
    Assert-JunctionTarget -Path (Join-Path $AttemptPath 'Plugins\AvidScriptPerfHarness') -ExpectedTarget ([System.IO.Path]::GetFullPath($HarnessPluginPath))

    foreach ($GeneratedDirectory in @('Binaries', 'Intermediate', 'Saved', 'DerivedDataCache')) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $AttemptPath $GeneratedDirectory))) "generated/build directory leaked into attempt: $GeneratedDirectory"
    }
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $AttemptPath 'Plugins\AvidScript\should-not-leak.txt'))) 'attempt must not reuse the source project main AvidScript plugin'

    $MarkerPath = Join-Path $AttemptPath 'benchmark-project.json'
    Assert-True (Test-Path -LiteralPath $MarkerPath -PathType Leaf) 'marker file is missing'
    $MarkerBytes = [System.IO.File]::ReadAllBytes($MarkerPath)
    Assert-True ($MarkerBytes.Length -ge 3) 'marker file is unexpectedly short'
    Assert-True (-not ($MarkerBytes[0] -eq 239 -and $MarkerBytes[1] -eq 187 -and $MarkerBytes[2] -eq 191)) 'marker file must be UTF-8 without BOM'
    $MarkerRaw = Get-Content -LiteralPath $MarkerPath -Raw
    $Marker = $MarkerRaw | ConvertFrom-Json
    Assert-Equal ([int]($Marker.schema_version)) 1 'marker schema_version mismatch'
    Assert-Equal ([string]($Marker.project_filename)) 'AvidTPSTemplate.uproject' 'marker project filename mismatch'
    Assert-Equal ([string]($Marker.candidate_commit)) $ExpectedCommit 'marker commit mismatch'
    Assert-Equal ([string]($Marker.candidate_tree)) $ExpectedTree 'marker tree mismatch'
    $CreatedUtcMatch = [regex]::Match($MarkerRaw, '"created_utc"\s*:\s*"([^"]+)"')
    Assert-True $CreatedUtcMatch.Success 'marker created_utc field is missing'
    $CreatedUtc = $CreatedUtcMatch.Groups[1].Value
    $ParsedCreatedUtc = [System.DateTimeOffset]::Parse($CreatedUtc)
    Assert-True ($ParsedCreatedUtc.Offset -eq [TimeSpan]::Zero) 'marker created_utc must be UTC'
    Assert-Equal ([string]($Marker.junctions.Source)) ([System.IO.Path]::GetFullPath((Join-Path $SourceProjectRoot 'Source'))) 'marker source junction mismatch'
    Assert-Equal ([string]($Marker.junctions.Config)) ([System.IO.Path]::GetFullPath((Join-Path $SourceProjectRoot 'Config'))) 'marker config junction mismatch'
    Assert-Equal ([string]($Marker.junctions.AvidScript)) ([System.IO.Path]::GetFullPath($CandidateWorktreePath)) 'marker AvidScript junction mismatch'
    Assert-Equal ([string]($Marker.junctions.Puerts)) ([System.IO.Path]::GetFullPath($PuertsPluginPath)) 'marker Puerts junction mismatch'
    Assert-Equal ([string]($Marker.junctions.AvidScriptPerfHarness)) ([System.IO.Path]::GetFullPath($HarnessPluginPath)) 'marker harness junction mismatch'

    $FirstMarkerHash = (Get-FileHash -LiteralPath $MarkerPath -Algorithm SHA256).Hash
    $SecondRun = (& $BootstrapPath `
        -SourceProjectPath $SourceProjectPath `
        -AvidScriptPluginPath $CandidateWorktreePath `
        -PuertsPluginPath $PuertsPluginPath `
        -HarnessPluginPath $HarnessPluginPath `
        -OutputRoot $OutputRoot `
        -ExpectedAvidScriptCommit $ExpectedCommit `
        -ExpectedAvidScriptTree $ExpectedTree) | ConvertFrom-Json
    Assert-True (([string]($SecondRun.attempt_path)) -cne ([string]($FirstRun.attempt_path))) 'existing output/attempt collision reused an existing attempt directory'
    Assert-Equal ((Get-FileHash -LiteralPath $MarkerPath -Algorithm SHA256).Hash) $FirstMarkerHash 'second run overwrote the first marker'

    $DirtyMarkerPath = Join-Path $CandidateWorktreePath 'dirty.txt'
    Write-Utf8NoBom -Path $DirtyMarkerPath -Value "dirty`n"
    Invoke-ExpectedFailure {
        & $BootstrapPath `
            -SourceProjectPath $SourceProjectPath `
            -AvidScriptPluginPath $CandidateWorktreePath `
            -PuertsPluginPath $PuertsPluginPath `
            -HarnessPluginPath $HarnessPluginPath `
            -OutputRoot $OutputRoot `
            -ExpectedAvidScriptCommit $ExpectedCommit `
            -ExpectedAvidScriptTree $ExpectedTree | Out-Null
    } 'ASP53B1105'
    Remove-Item -LiteralPath $DirtyMarkerPath -Force

    Invoke-ExpectedFailure {
        & $BootstrapPath `
            -SourceProjectPath $SourceProjectPath `
            -AvidScriptPluginPath $CandidateWorktreePath `
            -PuertsPluginPath $PuertsPluginPath `
            -HarnessPluginPath $HarnessPluginPath `
            -OutputRoot $OutputRoot `
            -ExpectedAvidScriptCommit ('0' * 40) `
            -ExpectedAvidScriptTree $ExpectedTree | Out-Null
    } 'ASP53B1103'

    Invoke-ExpectedFailure {
        & $BootstrapPath `
            -SourceProjectPath $SourceProjectPath `
            -AvidScriptPluginPath $CandidateWorktreePath `
            -PuertsPluginPath $PuertsPluginPath `
            -HarnessPluginPath $HarnessPluginPath `
            -OutputRoot $OutputRoot `
            -ExpectedAvidScriptCommit $ExpectedCommit `
            -ExpectedAvidScriptTree ('1' * 40) | Out-Null
    } 'ASP53B1104'

    Invoke-ExpectedFailure {
        $NestedOutputRoot = Join-Path $CandidateWorktreePath 'OutputInsideCandidate'
        & $BootstrapPath `
            -SourceProjectPath $SourceProjectPath `
            -AvidScriptPluginPath $CandidateWorktreePath `
            -PuertsPluginPath $PuertsPluginPath `
            -HarnessPluginPath $HarnessPluginPath `
            -OutputRoot $NestedOutputRoot `
            -ExpectedAvidScriptCommit $ExpectedCommit `
            -ExpectedAvidScriptTree $ExpectedTree | Out-Null
    } 'ASP53B1200'
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

Write-Output 'Puerts benchmark clean project contracts passed: parser=2 happy_path=1 junctions=5 marker=1 dirty_rejection=1 commit_rejection=1 tree_rejection=1 attempt_reuse_rejection=1 overlap_rejection=1 generated_dirs=4 privacy=1'
