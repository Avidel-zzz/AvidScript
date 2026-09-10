Set-StrictMode -Version Latest
$script:BinaryPrivacySourcePath = Join-Path $PSScriptRoot 'AvidScriptBinaryPrivacyScanner.cs'
$script:BinaryPrivacySchemaPath = Join-Path $PSScriptRoot 'AvidScriptBinaryPrivacyReport.schema.json'
$script:BinaryPrivacyPolicySha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()

function Initialize-AvidScriptBinaryPrivacy {
    if (-not $IsWindows) { throw 'ASBP1001 Windows is required for file sharing and handle identity checks' }
    if ($null -eq ('AvidScript.Release.BinaryPrivacyV1.Scanner' -as [type])) {
        Add-Type -Path $script:BinaryPrivacySourcePath -ErrorAction Stop
    }
    [AvidScript.Release.BinaryPrivacyV1.Scanner]::BindSourceIdentity(
        (Get-FileHash -LiteralPath $script:BinaryPrivacySourcePath -Algorithm SHA256).Hash.ToLowerInvariant())
}

function Assert-AvidScriptBinaryPrivacyAncestors {
    param([string]$Path)
    $cursor = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($cursor)) {
        if (Test-Path -LiteralPath $cursor) {
            if (((Get-Item -LiteralPath $cursor -Force -ErrorAction Stop).Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'ASBP1002 reparse point in input or report ancestry'
            }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
}

function Get-AvidScriptBinaryPrivacySnapshot {
    param([string]$Root)
    Assert-AvidScriptBinaryPrivacyAncestors -Path $Root
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { throw 'ASBP1002 input directory is missing' }
    $items = @((Get-Item -LiteralPath $Root -Force)) + @(Get-ChildItem -LiteralPath $Root -Force -Recurse -ErrorAction Stop |
        Select-Object -First 16385)
    if ($items.Count -gt 16384) { throw 'ASBP1002 directory exceeds 16384 entry scan limit' }
    $byPath = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    foreach ($item in $items) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'ASBP1002 reparse point in payload' }
        $streamPath = $item.FullName
        if (-not $streamPath.StartsWith('\\?\', [StringComparison]::Ordinal)) {
            $streamPath = if ($streamPath.StartsWith('\\', [StringComparison]::Ordinal)) {
                '\\?\UNC\' + $streamPath.Substring(2)
            } else { '\\?\' + $streamPath }
        }
        $streams = @(Get-Item -LiteralPath $streamPath -Stream * -ErrorAction Stop)
        if (@($streams | Where-Object Stream -CNE ':$DATA').Count -gt 0) { throw 'ASBP1002 alternate data stream in payload' }
        $relative = [IO.Path]::GetRelativePath($Root, $item.FullName).Replace('\', '/')
        if ($relative -ne '.' -and ($relative.StartsWith('../') -or [IO.Path]::IsPathRooted($relative))) {
            throw 'ASBP1002 inventory path escaped root'
        }
        $byPath.Add($relative, [pscustomobject]@{
            relative = $relative; full_path = $item.FullName; directory = [bool]$item.PSIsContainer
            length = if ($item.PSIsContainer) { 0L } else { [long]$item.Length }
            modified_ticks = $item.LastWriteTimeUtc.Ticks
        })
    }
    [string[]]$names = @($byPath.Keys)
    [Array]::Sort($names, [StringComparer]::Ordinal)
    return @($names | ForEach-Object { $byPath[$_] })
}

function New-AvidScriptBinaryPrivacyRules {
    param([string]$Root, [string[]]$PrivatePath)
    $rules = [Collections.Generic.List[AvidScript.Release.BinaryPrivacyV1.Rule]]::new()
    $rules.Add([AvidScript.Release.BinaryPrivacyV1.Rule]::new('windows-home', '[a-z]:[\\/]{1,2}(?:Users|Documents and Settings)[\\/]{1,2}', $true, 64))
    $rules.Add([AvidScript.Release.BinaryPrivacyV1.Rule]::new('unix-home', '/(?:home|Users|root)/', $true, 16))
    $rules.Add([AvidScript.Release.BinaryPrivacyV1.Rule]::new('mac-temp', '/private/var/folders/', $false, 32))
    $userName = [Environment]::UserName
    if ([string]::IsNullOrWhiteSpace($userName)) { throw 'ASBP1001 user identity is unavailable' }
    $rules.Add([AvidScript.Release.BinaryPrivacyV1.Rule]::new('current-user',
        '(?<![\p{L}\p{N}_])' + [regex]::Escape($userName) + '(?![\p{L}\p{N}_])', $true, $userName.Length + 2))
    $profilePath = [Environment]::GetFolderPath('UserProfile')
    if ([string]::IsNullOrWhiteSpace($profilePath)) { throw 'ASBP1001 user profile is unavailable' }
    $roots = @($Root, $profilePath, [IO.Path]::GetTempPath(), (Join-Path $profilePath '.cargo')) + @($PrivatePath)
    if (-not [string]::IsNullOrWhiteSpace($env:CARGO_HOME)) { $roots += $env:CARGO_HOME }
    if ($roots.Count -gt 100) { throw 'ASBP1001 too many private paths' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($privateRoot in $roots) {
        if ([string]::IsNullOrWhiteSpace($privateRoot) -or -not [IO.Path]::IsPathFullyQualified($privateRoot)) {
            throw 'ASBP1001 private paths must be absolute'
        }
        $full = [IO.Path]::GetFullPath($privateRoot).TrimEnd('\', '/')
        foreach ($token in @($full.Replace('\', '/'), $full.Replace('/', '\'))) {
            if ($seen.Add($token)) {
                $rules.Add([AvidScript.Release.BinaryPrivacyV1.Rule]::new(
                    ('private-path-{0:d3}' -f $seen.Count), $token, $false, $token.Length))
            }
        }
    }
    return $rules.ToArray()
}

function Invoke-AvidScriptBinaryPrivacyScan {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$ReportPath,
        [string[]]$PrivatePath = @()
    )
    $ErrorActionPreference = 'Stop'
    Initialize-AvidScriptBinaryPrivacy
    $Root = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($Root))
    $ReportPath = [IO.Path]::GetFullPath($ReportPath)
    if ($ReportPath.Equals($Root, [StringComparison]::OrdinalIgnoreCase) -or
        $ReportPath.StartsWith($Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ASBP1001 report must remain outside the scanned directory'
    }
    Assert-AvidScriptBinaryPrivacyAncestors -Path $ReportPath
    if (Test-Path -LiteralPath $ReportPath) { throw 'ASBP1001 report already exists' }
    if (-not (Test-Path -LiteralPath (Split-Path -Parent $ReportPath) -PathType Container)) {
        throw 'ASBP1001 report parent must already exist'
    }
    $handles = [Collections.Generic.List[IO.FileStream]]::new()
    $entries = [Collections.Generic.List[object]]::new()
    $report = [ordered]@{
        schema_version = 1; policy_id = 'win64-private-paths-v1'; passed = $false
        inventory_verified = $false; error_code = $null
        scanner_sha256 = (Get-FileHash -LiteralPath $script:BinaryPrivacySourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
        policy_sha256 = $script:BinaryPrivacyPolicySha256
        rule_ids = @(); file_count = 0; byte_count = 0L; files = @()
    }
    try {
        $rules = @(New-AvidScriptBinaryPrivacyRules -Root $Root -PrivatePath $PrivatePath)
        $report.rule_ids = @($rules | ForEach-Object Id)
        $before = @(Get-AvidScriptBinaryPrivacySnapshot -Root $Root)
        $files = @($before | Where-Object { -not $_.directory })
        if ($files.Count -eq 0) { throw 'ASBP1002 payload contains no files' }
        # Keep every handle until final inventory validation. Earlier files cannot change while later files are scanned.
        foreach ($file in $files) {
            $handle = [IO.FileStream]::new($file.full_path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            $handles.Add($handle)
            [AvidScript.Release.BinaryPrivacyV1.Scanner]::AssertHandlePath($handle, $file.full_path)
        }
        for ($index = 0; $index -lt $files.Count; ++$index) {
            $file = $files[$index]
            $nameHits = @([AvidScript.Release.BinaryPrivacyV1.Scanner]::MatchName($file.relative, $rules))
            $scan = [AvidScript.Release.BinaryPrivacyV1.Scanner]::ScanStream($handles[$index], $rules, 65536)
            if ($scan.Length -ne $file.length) { throw 'ASBP1004 file length changed during scan' }
            $hits = @($scan.MatchedViews.Keys | Sort-Object -CaseSensitive | ForEach-Object {
                [ordered]@{rule_id = $_; matched_views = $scan.MatchedViews[$_]}
            })
            $entries.Add([ordered]@{
                index = $index; path = if ($nameHits.Count -eq 0) { $file.relative } else { $null }
                length = $scan.Length; sha256 = $scan.Sha256; name_rule_ids = $nameHits; content_hits = $hits
            })
            $report.byte_count += $scan.Length
        }
        $after = @(Get-AvidScriptBinaryPrivacySnapshot -Root $Root)
        if (($before | ConvertTo-Json -Depth 5 -Compress) -cne ($after | ConvertTo-Json -Depth 5 -Compress)) {
            throw 'ASBP1004 directory changed during scan'
        }
        $report.inventory_verified = $true
        $report.passed = @($entries | Where-Object { $_.name_rule_ids.Count -gt 0 -or $_.content_hits.Count -gt 0 }).Count -eq 0
        if (-not $report.passed) { $report.error_code = 'ASBP1006' }
    }
    catch {
        $message = $_.Exception.GetBaseException().Message
        $report.error_code = if ($message -match '^ASBP100[1246]\b') { $Matches[0] } else { 'ASBP1004' }
    }
    finally {
        foreach ($handle in $handles) { $handle.Dispose() }
    }
    $report.files = @($entries.ToArray())
    $report.file_count = $entries.Count
    $json = $report | ConvertTo-Json -Depth 20
    if (-not ($json | Test-Json -SchemaFile $script:BinaryPrivacySchemaPath -ErrorAction SilentlyContinue)) {
        throw 'ASBP1005 generated privacy report violates schema'
    }
    $output = [IO.FileStream]::new($ReportPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        [AvidScript.Release.BinaryPrivacyV1.Scanner]::AssertHandlePath($output, $ReportPath)
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json + [Environment]::NewLine)
        $output.Write($bytes, 0, $bytes.Length)
        $output.Flush($true)
    }
    finally { $output.Dispose() }
    if (-not $report.passed) { throw "$($report.error_code) binary privacy scan failed; see sanitized report" }
    return [pscustomobject]$report
}
