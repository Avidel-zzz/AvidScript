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
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptRoot 'PuertsBenchmarkSidecar.Common.ps1')

if (-not ('AvidScript.P53Benchmark.NativePath' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace AvidScript.P53Benchmark
{
    public static class NativePath
    {
        private const uint FileShareRead = 0x00000001;
        private const uint FileShareWrite = 0x00000002;
        private const uint FileShareDelete = 0x00000004;
        private const uint OpenExisting = 3;
        private const uint FileFlagBackupSemantics = 0x02000000;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            IntPtr securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFinalPathNameByHandleW(
            SafeFileHandle file,
            StringBuilder filePath,
            uint filePathLength,
            uint flags);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreateDirectoryW(
            string path,
            IntPtr securityAttributes);

        public static string ResolveExistingPath(string path)
        {
            using (SafeFileHandle handle = CreateFileW(
                path,
                0,
                FileShareRead | FileShareWrite | FileShareDelete,
                IntPtr.Zero,
                OpenExisting,
                FileFlagBackupSemantics,
                IntPtr.Zero))
            {
                if (handle.IsInvalid)
                {
                    throw new Win32Exception(
                        Marshal.GetLastWin32Error(),
                        "CreateFileW failed while resolving path: " + path);
                }

                int capacity = 512;
                while (true)
                {
                    StringBuilder buffer = new StringBuilder(capacity);
                    uint length = GetFinalPathNameByHandleW(
                        handle,
                        buffer,
                        checked((uint)buffer.Capacity),
                        0);
                    if (length == 0)
                    {
                        throw new Win32Exception(
                            Marshal.GetLastWin32Error(),
                            "GetFinalPathNameByHandleW failed for path: " + path);
                    }

                    if (length < buffer.Capacity)
                    {
                        return NormalizeDosPath(buffer.ToString());
                    }

                    capacity = checked((int)length + 1);
                }
            }
        }

        public static void CreateNewDirectory(string path)
        {
            if (!CreateDirectoryW(path, IntPtr.Zero))
            {
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(),
                    "CreateDirectoryW failed for path: " + path);
            }
        }

        private static string NormalizeDosPath(string path)
        {
            const string uncPrefix = @"\\?\UNC\";
            const string dosPrefix = @"\\?\";
            if (path.StartsWith(uncPrefix, StringComparison.OrdinalIgnoreCase))
            {
                return @"\\" + path.Substring(uncPrefix.Length);
            }

            if (path.StartsWith(dosPrefix, StringComparison.OrdinalIgnoreCase))
            {
                return path.Substring(dosPrefix.Length);
            }

            return path;
        }
    }
}
'@
}

function ConvertTo-NormalizedAbsolutePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $Root = [System.IO.Path]::GetPathRoot($FullPath)
    if ($FullPath.Length -gt $Root.Length) {
        $FullPath = $FullPath.TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar)
    }

    return $FullPath
}

function Resolve-CanonicalExistingPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ValidateSet('Leaf', 'Container')][string]$PathType,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $AbsolutePath = ConvertTo-NormalizedAbsolutePath -Path $Path
    if (-not (Test-Path -LiteralPath $AbsolutePath -PathType $PathType)) {
        throw "$Code $Label does not exist or has the wrong type: $Path"
    }

    try {
        return ConvertTo-NormalizedAbsolutePath -Path (
            [AvidScript.P53Benchmark.NativePath]::ResolveExistingPath($AbsolutePath))
    }
    catch {
        throw "$Code $Label could not be resolved to its final Windows target: $AbsolutePath`n$($_.Exception.Message)"
    }
}

function Resolve-CanonicalOutputRoot {
    param([Parameter(Mandatory = $true)][string]$Path)

    $AbsolutePath = ConvertTo-NormalizedAbsolutePath -Path $Path
    if (Test-Path -LiteralPath $AbsolutePath -PathType Leaf) {
        throw "ASP53B1006 OutputRoot must be a directory path: $AbsolutePath"
    }
    if (Test-Path -LiteralPath $AbsolutePath -PathType Container) {
        return Resolve-CanonicalExistingPath -Path $AbsolutePath -PathType Container `
            -Code 'ASP53B1006' -Label 'OutputRoot'
    }

    $MissingComponents = [System.Collections.Generic.List[string]]::new()
    $ExistingAncestor = $AbsolutePath
    while (-not (Test-Path -LiteralPath $ExistingAncestor)) {
        $Component = [System.IO.Path]::GetFileName($ExistingAncestor)
        if ([string]::IsNullOrEmpty($Component)) {
            throw "ASP53B1006 OutputRoot has no resolvable existing ancestor: $AbsolutePath"
        }

        $MissingComponents.Insert(0, $Component)
        $Parent = [System.IO.Path]::GetDirectoryName($ExistingAncestor)
        if ([string]::IsNullOrEmpty($Parent) -or $Parent -ceq $ExistingAncestor) {
            throw "ASP53B1006 OutputRoot has no resolvable existing ancestor: $AbsolutePath"
        }
        $ExistingAncestor = $Parent
    }

    if (-not (Test-Path -LiteralPath $ExistingAncestor -PathType Container)) {
        throw "ASP53B1006 OutputRoot ancestor must be a directory: $ExistingAncestor"
    }

    $Resolved = Resolve-CanonicalExistingPath -Path $ExistingAncestor -PathType Container `
        -Code 'ASP53B1006' -Label 'OutputRoot ancestor'
    foreach ($Component in $MissingComponents) {
        $Resolved = Join-Path $Resolved $Component
    }

    return ConvertTo-NormalizedAbsolutePath -Path $Resolved
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

function Test-ResolvedPathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $ResolvedPath = ConvertTo-NormalizedAbsolutePath -Path $Path
    $ResolvedRoot = ConvertTo-NormalizedAbsolutePath -Path $Root
    if ($ResolvedPath -ieq $ResolvedRoot) {
        return $true
    }

    return $ResolvedPath.StartsWith((Get-NormalizedPathPrefix -Path $ResolvedRoot), [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoOverlapWithDestination {
    param(
        [Parameter(Mandatory = $true)][string]$AttemptPath,
        [Parameter(Mandatory = $true)][string[]]$SourcePaths
    )

    foreach ($SourcePath in $SourcePaths) {
        if ((Test-ResolvedPathWithin -Path $AttemptPath -Root $SourcePath) -or
            (Test-ResolvedPathWithin -Path $SourcePath -Root $AttemptPath)) {
            throw "ASP53B1200 source/destination overlap is not allowed: source=$SourcePath destination=$AttemptPath"
        }
    }
}

function Get-UniqueAttemptPath {
    param([Parameter(Mandatory = $true)][string]$Root)

    $Stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
    for ($Sequence = 1; $Sequence -le 9999; ++$Sequence) {
        $Candidate = Join-Path $Root ('benchmark-project-{0}-{1:D4}' -f $Stamp, $Sequence)
        if (Test-Path -LiteralPath $Candidate) {
            continue
        }

        return ConvertTo-NormalizedAbsolutePath -Path $Candidate
    }

    throw "ASP53B1201 could not select a unique attempt directory under $Root"
}

function New-ValidatedAttemptDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$AttemptPath
    )

    [System.IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
    $CreatedOutputRoot = Resolve-CanonicalExistingPath -Path $OutputRoot -PathType Container `
        -Code 'ASP53B1201' -Label 'created OutputRoot'
    if ($CreatedOutputRoot -ine $OutputRoot) {
        throw "ASP53B1201 OutputRoot target changed before creation: actual=$CreatedOutputRoot expected=$OutputRoot"
    }

    try {
        [AvidScript.P53Benchmark.NativePath]::CreateNewDirectory($AttemptPath)
    }
    catch {
        throw "ASP53B1201 refused to reuse or overwrite attempt directory: $AttemptPath`n$($_.Exception.Message)"
    }

    $CreatedAttemptPath = Resolve-CanonicalExistingPath -Path $AttemptPath -PathType Container `
        -Code 'ASP53B1201' -Label 'created attempt directory'
    if ($CreatedAttemptPath -ine $AttemptPath) {
        throw "ASP53B1201 attempt target changed during creation: actual=$CreatedAttemptPath expected=$AttemptPath"
    }

    return $CreatedAttemptPath
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

    $ResolvedTarget = Resolve-CanonicalExistingPath -Path $Path -PathType Container `
        -Code $Code -Label 'created junction'
    if ($ResolvedTarget -ine $Target) {
        throw "$Code junction target mismatch: path=$Path target=$ResolvedTarget expected=$Target"
    }
}

$ResolvedSourceProjectPath = Resolve-CanonicalExistingPath -Path $SourceProjectPath -PathType Leaf `
    -Code 'ASP53B1000' -Label 'SourceProjectPath'
if ([System.IO.Path]::GetExtension($ResolvedSourceProjectPath) -cne '.uproject') {
    throw "ASP53B1000 SourceProjectPath must point to a .uproject file: $ResolvedSourceProjectPath"
}
$ResolvedSourceProjectRoot = Resolve-CanonicalExistingPath `
    -Path (Split-Path -Parent $ResolvedSourceProjectPath) `
    -PathType Container `
    -Code 'ASP53B1000' `
    -Label 'source project root'
$ResolvedSourceDirectory = Resolve-CanonicalExistingPath `
    -Path (Join-Path $ResolvedSourceProjectRoot 'Source') `
    -PathType Container `
    -Code 'ASP53B1000' `
    -Label 'source project Source directory'
$ResolvedConfigDirectory = Resolve-CanonicalExistingPath `
    -Path (Join-Path $ResolvedSourceProjectRoot 'Config') `
    -PathType Container `
    -Code 'ASP53B1000' `
    -Label 'source project Config directory'

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
if (-not (Test-ResolvedPathWithin -Path $ResolvedHarnessPluginPath -Root $ResolvedAvidScriptPluginPath)) {
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
$ResolvedGitTopLevel = Resolve-CanonicalExistingPath -Path $GitTopLevel -PathType Container `
    -Code 'ASP53B1100' -Label 'Git worktree root'
if ($ResolvedGitTopLevel -ine $ResolvedAvidScriptPluginPath) {
    throw "ASP53B1100 AvidScriptPluginPath must be the Git worktree root: $ResolvedAvidScriptPluginPath"
}

$GitDir = (@(Invoke-Git -RepositoryPath $ResolvedAvidScriptPluginPath -Code 'ASP53B1101' rev-parse --git-dir))[0].Trim()
$GitCommonDir = (@(Invoke-Git -RepositoryPath $ResolvedAvidScriptPluginPath -Code 'ASP53B1101' rev-parse --git-common-dir))[0].Trim()
$GitDirPath = if ([System.IO.Path]::IsPathFullyQualified($GitDir)) {
    $GitDir
}
else {
    Join-Path $ResolvedAvidScriptPluginPath $GitDir
}
$GitCommonDirPath = if ([System.IO.Path]::IsPathFullyQualified($GitCommonDir)) {
    $GitCommonDir
}
else {
    Join-Path $ResolvedAvidScriptPluginPath $GitCommonDir
}
$ResolvedGitDir = Resolve-CanonicalExistingPath -Path $GitDirPath -PathType Container `
    -Code 'ASP53B1101' -Label 'Git directory'
$ResolvedGitCommonDir = Resolve-CanonicalExistingPath -Path $GitCommonDirPath -PathType Container `
    -Code 'ASP53B1101' -Label 'Git common directory'
if ($ResolvedGitDir -ieq $ResolvedGitCommonDir) {
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

$AttemptPath = Get-UniqueAttemptPath -Root $ResolvedOutputRoot
Assert-NoOverlapWithDestination -AttemptPath $AttemptPath -SourcePaths @(
    $ResolvedSourceDirectory,
    $ResolvedConfigDirectory,
    $ResolvedAvidScriptPluginPath,
    $ResolvedPuertsPluginPath,
    $ResolvedHarnessPluginPath
)
$AttemptPath = New-ValidatedAttemptDirectory -OutputRoot $ResolvedOutputRoot -AttemptPath $AttemptPath

$AttemptProjectPath = Join-Path $AttemptPath ([System.IO.Path]::GetFileName($ResolvedSourceProjectPath))
Copy-NewFile -Source $ResolvedSourceProjectPath -Destination $AttemptProjectPath -Code 'ASP53B1202'

New-ProjectJunction -Path (Join-Path $AttemptPath 'Source') `
    -Target $ResolvedSourceDirectory `
    -Code 'ASP53B1203'
New-ProjectJunction -Path (Join-Path $AttemptPath 'Config') `
    -Target $ResolvedConfigDirectory `
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
$SourceDigest = Get-SidecarDirectoryContentDigest -Path $ResolvedSourceDirectory
$ConfigDigest = Get-SidecarDirectoryContentDigest -Path $ResolvedConfigDirectory
$Marker = [ordered]@{
    schema_version = 2
    created_utc = [System.DateTimeOffset]::UtcNow.ToString('o')
    project_filename = [System.IO.Path]::GetFileName($ResolvedSourceProjectPath)
    candidate_commit = $ActualCommit
    candidate_tree = $ActualTree
    source = [ordered]@{
        canonical_path = $ResolvedSourceDirectory
        content_sha256 = [string]$SourceDigest.content_sha256
        file_count = [int]$SourceDigest.file_count
    }
    config = [ordered]@{
        canonical_path = $ResolvedConfigDirectory
        content_sha256 = [string]$ConfigDigest.content_sha256
        file_count = [int]$ConfigDigest.file_count
    }
    junctions = [ordered]@{
        Source = $ResolvedSourceDirectory
        Config = $ResolvedConfigDirectory
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
