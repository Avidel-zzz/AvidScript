[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceProjectPath,

    [Parameter(Mandatory = $true)]
    [string]$AvidScriptPluginPath,

    [Parameter(Mandatory = $true)]
    [string]$PuertsPluginPath,

    [Parameter(Mandatory = $true)]
    [string]$HarnessPluginPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedAvidScriptCommit,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedAvidScriptTree
)

$ErrorActionPreference = 'Stop'

function Resolve-CanonicalExistingPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ValidateSet('Leaf', 'Container')][string]$PathType,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType $PathType)) {
        throw "$Code $Label does not exist or has the wrong type: $Path"
    }

    return [System.IO.Path]::GetFullPath((Get-Item -LiteralPath $Path -Force -ErrorAction Stop).FullName)
}

function Resolve-CanonicalOutputRoot {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Resolved = [System.IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $Resolved -PathType Leaf) {
        throw "ASP53B1006 OutputRoot must be a directory path: $Resolved"
    }

    [System.IO.Directory]::CreateDirectory($Resolved) | Out-Null
    return $Resolved
}

function Test-IsLowerHex40 {
    param([Parameter(Mandatory = $true)][string]$Value)

    return $Value -cmatch '^[0-9a-f]{40}$'
}

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryPath,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )

    $Output = & git -C $RepositoryPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "$Code git failed: git -C $RepositoryPath $($Arguments -join ' ')`n$($Output -join [Environment]::NewLine)"
    }

    return @($Output)
}

function Get-NormalizedPathPrefix {
    param([Parameter(Mandatory = $true)][string]$Path)

    return $Path.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
}

function Test-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $ResolvedPath = [System.IO.Path]::GetFullPath($Path)
    $ResolvedRoot = [System.IO.Path]::GetFullPath($Root)
    if ($ResolvedPath -ieq $ResolvedRoot) {
        return $true
    }

    return $ResolvedPath.StartsWith((Get-NormalizedPathPrefix -Path $ResolvedRoot), [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoOverlapWithDestination {
    param(
        [Parameter(Mandatory = $true)][string]$AttemptPath,
        [Parameter(Mandatory = $true)][string[]]$PluginPaths
    )

    foreach ($PluginPath in $PluginPaths) {
        if ((Test-PathWithin -Path $AttemptPath -Root $PluginPath) -or
            (Test-PathWithin -Path $PluginPath -Root $AttemptPath)) {
            throw "ASP53B1200 source/destination overlap is not allowed: plugin=$PluginPath destination=$AttemptPath"
        }
    }
}

function New-UniqueAttemptDirectory {
    param([Parameter(Mandatory = $true)][string]$Root)

    $Stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
    for ($Sequence = 1; $Sequence -le 9999; ++$Sequence) {
        $Candidate = Join-Path $Root ('benchmark-project-{0}-{1:D4}' -f $Stamp, $Sequence)
        if (Test-Path -LiteralPath $Candidate) {
            continue
        }

        New-Item -ItemType Directory -Path $Candidate -ErrorAction Stop | Out-Null
        return [System.IO.Path]::GetFullPath($Candidate)
    }

    throw "ASP53B1201 could not allocate a unique attempt directory under $Root"
}

function Write-NewTextFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value,
        [Parameter(Mandatory = $true)][string]$Code
    )

    $Parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $Parent -PathType Container)) {
        throw "$Code parent directory does not exist: $Parent"
    }

    $TemporaryPath = Join-Path $Parent (
        '.{0}.{1}.tmp' -f [System.IO.Path]::GetFileName($Path), [Guid]::NewGuid().ToString('N'))
    try {
        $Encoding = [System.Text.UTF8Encoding]::new($false)
        $Stream = [System.IO.FileStream]::new(
            $TemporaryPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read)
        try {
            $Writer = [System.IO.StreamWriter]::new($Stream, $Encoding, 4096, $true)
            try {
                $Writer.Write($Value)
                $Writer.Flush()
                $Stream.Flush($true)
            }
            finally {
                $Writer.Dispose()
            }
        }
        finally {
            $Stream.Dispose()
        }

        [System.IO.File]::Move($TemporaryPath, $Path)
    }
    catch [System.IO.IOException] {
        if (Test-Path -LiteralPath $TemporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
        throw "$Code refused to overwrite existing file: $Path"
    }
}

function Write-NewJsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Code
    )

    $Json = (($Value | ConvertTo-Json -Depth 32) -replace "`r`n", "`n") + "`n"
    Write-NewTextFile -Path $Path -Value $Json -Code $Code
}

function Copy-NewFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$Code
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "$Code source file is missing: $Source"
    }

    $Bytes = [System.IO.File]::ReadAllBytes($Source)
    $Parent = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $Parent -PathType Container)) {
        throw "$Code destination directory does not exist: $Parent"
    }

    $TemporaryPath = Join-Path $Parent (
        '.{0}.{1}.tmp' -f [System.IO.Path]::GetFileName($Destination), [Guid]::NewGuid().ToString('N'))
    try {
        $Stream = [System.IO.FileStream]::new(
            $TemporaryPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::Read)
        try {
            $Stream.Write($Bytes, 0, $Bytes.Length)
            $Stream.Flush($true)
        }
        finally {
            $Stream.Dispose()
        }

        [System.IO.File]::Move($TemporaryPath, $Destination)
    }
    catch [System.IO.IOException] {
        if (Test-Path -LiteralPath $TemporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
        throw "$Code refused to overwrite existing file: $Destination"
    }
}

function New-ProjectJunction {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Target,
        [Parameter(Mandatory = $true)][string]$Code
    )

    if (Test-Path -LiteralPath $Path) {
        throw "$Code junction destination already exists: $Path"
    }

    $Parent = Split-Path -Parent $Path
    [System.IO.Directory]::CreateDirectory($Parent) | Out-Null
    New-Item -ItemType Junction -Path $Path -Target $Target -ErrorAction Stop | Out-Null

    $Item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($Item.LinkType -cne 'Junction') {
        throw "$Code created item is not a junction: $Path"
    }

    $ResolvedTarget = [System.IO.Path]::GetFullPath([string]$Item.Target)
    if ($ResolvedTarget -cne $Target) {
        throw "$Code junction target mismatch: path=$Path target=$ResolvedTarget expected=$Target"
    }
}

$ResolvedSourceProjectPath = Resolve-CanonicalExistingPath -Path $SourceProjectPath -PathType Leaf `
    -Code 'ASP53B1000' -Label 'SourceProjectPath'
if ([System.IO.Path]::GetExtension($ResolvedSourceProjectPath) -cne '.uproject') {
    throw "ASP53B1000 SourceProjectPath must point to a .uproject file: $ResolvedSourceProjectPath"
}
$ResolvedSourceProjectRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $ResolvedSourceProjectPath))

$ResolvedAvidScriptPluginPath = Resolve-CanonicalExistingPath -Path $AvidScriptPluginPath -PathType Container `
    -Code 'ASP53B1001' -Label 'AvidScriptPluginPath'
if (-not (Test-Path -LiteralPath (Join-Path $ResolvedAvidScriptPluginPath 'AvidScript.uplugin') -PathType Leaf)) {
    throw "ASP53B1001 AvidScriptPluginPath must contain AvidScript.uplugin: $ResolvedAvidScriptPluginPath"
}

$ResolvedPuertsPluginPath = Resolve-CanonicalExistingPath -Path $PuertsPluginPath -PathType Container `
    -Code 'ASP53B1002' -Label 'PuertsPluginPath'
if (-not (Test-Path -LiteralPath (Join-Path $ResolvedPuertsPluginPath 'Puerts.uplugin') -PathType Leaf)) {
    throw "ASP53B1002 PuertsPluginPath must contain Puerts.uplugin: $ResolvedPuertsPluginPath"
}

$ResolvedHarnessPluginPath = Resolve-CanonicalExistingPath -Path $HarnessPluginPath -PathType Container `
    -Code 'ASP53B1003' -Label 'HarnessPluginPath'
if (-not (Test-Path -LiteralPath (Join-Path $ResolvedHarnessPluginPath 'AvidScriptPerfHarness.uplugin') -PathType Leaf)) {
    throw "ASP53B1003 HarnessPluginPath must contain AvidScriptPerfHarness.uplugin: $ResolvedHarnessPluginPath"
}
if (-not (Test-PathWithin -Path $ResolvedHarnessPluginPath -Root $ResolvedAvidScriptPluginPath)) {
    throw "ASP53B1003 HarnessPluginPath must stay inside the candidate worktree: $ResolvedHarnessPluginPath"
}

$ResolvedOutputRoot = Resolve-CanonicalOutputRoot -Path $OutputRoot

if (-not (Test-IsLowerHex40 -Value $ExpectedAvidScriptCommit)) {
    throw "ASP53B1004 ExpectedAvidScriptCommit must be a 40-character lower-case SHA-1: $ExpectedAvidScriptCommit"
}
if (-not (Test-IsLowerHex40 -Value $ExpectedAvidScriptTree)) {
    throw "ASP53B1005 ExpectedAvidScriptTree must be a 40-character lower-case SHA-1: $ExpectedAvidScriptTree"
}

$GitTopLevel = (@(Invoke-Git -RepositoryPath $ResolvedAvidScriptPluginPath -Code 'ASP53B1100' rev-parse --show-toplevel))[0].Trim()
if ([System.IO.Path]::GetFullPath($GitTopLevel) -cne $ResolvedAvidScriptPluginPath) {
    throw "ASP53B1100 AvidScriptPluginPath must be the Git worktree root: $ResolvedAvidScriptPluginPath"
}

$GitDir = (@(Invoke-Git -RepositoryPath $ResolvedAvidScriptPluginPath -Code 'ASP53B1101' rev-parse --git-dir))[0].Trim()
$GitCommonDir = (@(Invoke-Git -RepositoryPath $ResolvedAvidScriptPluginPath -Code 'ASP53B1101' rev-parse --git-common-dir))[0].Trim()
$ResolvedGitDir = [System.IO.Path]::GetFullPath((Join-Path $ResolvedAvidScriptPluginPath $GitDir))
$ResolvedGitCommonDir = [System.IO.Path]::GetFullPath((Join-Path $ResolvedAvidScriptPluginPath $GitCommonDir))
if ($ResolvedGitDir -ceq $ResolvedGitCommonDir) {
    throw "ASP53B1101 AvidScriptPluginPath is not a linked Git worktree: $ResolvedAvidScriptPluginPath"
}

$ActualCommit = (@(Invoke-Git -RepositoryPath $ResolvedAvidScriptPluginPath -Code 'ASP53B1103' rev-parse HEAD))[0].Trim().ToLowerInvariant()
if ($ActualCommit -cne $ExpectedAvidScriptCommit) {
    throw "ASP53B1103 worktree HEAD commit mismatch: actual=$ActualCommit expected=$ExpectedAvidScriptCommit"
}

$ActualTree = (@(Invoke-Git -RepositoryPath $ResolvedAvidScriptPluginPath -Code 'ASP53B1104' rev-parse 'HEAD^{tree}'))[0].Trim().ToLowerInvariant()
if ($ActualTree -cne $ExpectedAvidScriptTree) {
    throw "ASP53B1104 worktree HEAD tree mismatch: actual=$ActualTree expected=$ExpectedAvidScriptTree"
}

$StatusOutput = @(Invoke-Git -RepositoryPath $ResolvedAvidScriptPluginPath -Code 'ASP53B1105' status --porcelain=v1 --untracked-files=all)
if (($StatusOutput -join "`n").Trim().Length -ne 0) {
    throw "ASP53B1105 candidate worktree must be clean: $($StatusOutput -join '; ')"
}

$AttemptPath = New-UniqueAttemptDirectory -Root $ResolvedOutputRoot
Assert-NoOverlapWithDestination -AttemptPath $AttemptPath -PluginPaths @(
    $ResolvedAvidScriptPluginPath,
    $ResolvedPuertsPluginPath,
    $ResolvedHarnessPluginPath
)

$AttemptProjectPath = Join-Path $AttemptPath ([System.IO.Path]::GetFileName($ResolvedSourceProjectPath))
Copy-NewFile -Source $ResolvedSourceProjectPath -Destination $AttemptProjectPath -Code 'ASP53B1202'

New-ProjectJunction -Path (Join-Path $AttemptPath 'Source') `
    -Target ([System.IO.Path]::GetFullPath((Join-Path $ResolvedSourceProjectRoot 'Source'))) `
    -Code 'ASP53B1203'
New-ProjectJunction -Path (Join-Path $AttemptPath 'Config') `
    -Target ([System.IO.Path]::GetFullPath((Join-Path $ResolvedSourceProjectRoot 'Config'))) `
    -Code 'ASP53B1203'
New-ProjectJunction -Path (Join-Path $AttemptPath 'Plugins\AvidScript') `
    -Target $ResolvedAvidScriptPluginPath `
    -Code 'ASP53B1203'
New-ProjectJunction -Path (Join-Path $AttemptPath 'Plugins\Puerts') `
    -Target $ResolvedPuertsPluginPath `
    -Code 'ASP53B1203'
New-ProjectJunction -Path (Join-Path $AttemptPath 'Plugins\AvidScriptPerfHarness') `
    -Target $ResolvedHarnessPluginPath `
    -Code 'ASP53B1203'

$MarkerPath = Join-Path $AttemptPath 'benchmark-project.json'
$Marker = [ordered]@{
    schema_version = 1
    created_utc = [System.DateTimeOffset]::UtcNow.ToString('o')
    project_filename = [System.IO.Path]::GetFileName($ResolvedSourceProjectPath)
    candidate_commit = $ActualCommit
    candidate_tree = $ActualTree
    junctions = [ordered]@{
        Source = [System.IO.Path]::GetFullPath((Join-Path $ResolvedSourceProjectRoot 'Source'))
        Config = [System.IO.Path]::GetFullPath((Join-Path $ResolvedSourceProjectRoot 'Config'))
        AvidScript = $ResolvedAvidScriptPluginPath
        Puerts = $ResolvedPuertsPluginPath
        AvidScriptPerfHarness = $ResolvedHarnessPluginPath
    }
}
Write-NewJsonFile -Path $MarkerPath -Value $Marker -Code 'ASP53B1204'

[pscustomobject][ordered]@{
    attempt_path = $AttemptPath
    project_path = $AttemptProjectPath
    commit = $ActualCommit
    tree = $ActualTree
    dirty = $false
} | ConvertTo-Json -Depth 8
