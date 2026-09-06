$script:AvidScriptSupportBundleRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $script:AvidScriptSupportBundleRoot 'AvidScriptPluginReleasePackage.ps1')
. (Join-Path $script:AvidScriptSupportBundleRoot 'AvidScriptCompatibilityDoctor.ps1')

$script:AvidScriptSupportMaximumLogs = 8
$script:AvidScriptSupportMaximumLogBytes = 65536
$script:AvidScriptSupportMaximumLogLines = 200

function ConvertTo-AvidScriptSupportSafeText {
    param(
        [AllowEmptyString()][string]$Text,
        [Parameter(Mandatory = $true)]$SensitivePaths
    )

    $Result = $Text
    $PathMappings = @(
        @{ Path = [string]$SensitivePaths.PluginRoot; Token = '<PLUGIN>' },
        @{ Path = [string]$SensitivePaths.ProjectRoot; Token = '<PROJECT>' },
        @{ Path = [string]$SensitivePaths.EngineRoot; Token = '<ENGINE>' }) |
        Sort-Object { $_.Path.Length } -Descending
    foreach ($Mapping in $PathMappings) {
        if ([string]::IsNullOrWhiteSpace($Mapping.Path)) {
            continue
        }
        foreach ($Variant in @(
                $Mapping.Path,
                $Mapping.Path.Replace('\', '/'))) {
            $Result = [regex]::Replace(
                $Result,
                [regex]::Escape($Variant),
                [string]$Mapping.Token,
                [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        }
    }

    $UserName = [Environment]::UserName
    if (-not [string]::IsNullOrWhiteSpace($UserName)) {
        $Result = [regex]::Replace(
            $Result,
            [regex]::Escape($UserName),
            '<USER>',
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    }
    $Replacements = @(
        @{ Pattern = '(?i)[A-Z]:[\\/]+Users[\\/]+[^\\/\s"'']+'; Value = '<USER_HOME>' },
        @{ Pattern = '(?i)/(?:home|Users)/[^/\s"'']+'; Value = '<USER_HOME>' },
        @{ Pattern = '(?i)\\\\(?:wsl(?:\.localhost)?\\[^\\\s]+|[^\\\s]+\\[^\\\s]+)'; Value = '<REMOTE_ROOT>' },
        @{ Pattern = '(?i)(https?://)[^/@\s:]+:[^/@\s]+@'; Value = '$1<REDACTED>@' },
        @{ Pattern = '(?i)\bBearer\s+(?!<REDACTED>)[A-Za-z0-9._~+/=-]{12,}'; Value = 'Bearer <REDACTED>' },
        @{ Pattern = '\bgh[pousr]_[A-Za-z0-9_]{20,}\b'; Value = '<REDACTED_TOKEN>' },
        @{ Pattern = '\bAKIA[0-9A-Z]{16}\b'; Value = '<REDACTED_KEY>' },
        @{ Pattern = '\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b'; Value = '<REDACTED_JWT>' },
        @{ Pattern = '(?i)\b(password|passwd|pwd|token|secret|api[_-]?key)\s*[:=]\s*[^\s;,]+'; Value = '$1=<REDACTED>' },
        @{ Pattern = '(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b'; Value = '<REDACTED_EMAIL>' })
    foreach ($Replacement in $Replacements) {
        $Result = [regex]::Replace($Result, $Replacement.Pattern, $Replacement.Value)
    }
    $Result = [regex]::Replace(
        $Result,
        '(?is)-----BEGIN [A-Z ]*PRIVATE KEY-----.*?-----END [A-Z ]*PRIVATE KEY-----',
        '<REDACTED_PRIVATE_KEY>')
    return $Result
}

function ConvertTo-AvidScriptSupportCompatibilityReport {
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)]$SensitivePaths
    )

    Assert-AvidScriptCompatibilityDoctorReport $Report
    $Checks = foreach ($Check in @($Report.checks)) {
        [pscustomobject][ordered]@{
            id = [string]$Check.id
            area = [string]$Check.area
            status = [string]$Check.status
            severity = [string]$Check.severity
            code = [string]$Check.code
            category = [string]$Check.category
            expected = ConvertTo-AvidScriptSupportSafeText ([string]$Check.expected) $SensitivePaths
            actual = ConvertTo-AvidScriptSupportSafeText ([string]$Check.actual) $SensitivePaths
            message = ConvertTo-AvidScriptSupportSafeText ([string]$Check.message) $SensitivePaths
            remediation = ConvertTo-AvidScriptSupportSafeText ([string]$Check.remediation) $SensitivePaths
        }
    }
    return [pscustomobject][ordered]@{
        schema_version = 1
        format = 'avidscript.compatibility.report'
        result = [string]$Report.result
        generated_at_utc = [string]$Report.generated_at_utc
        context = [pscustomobject][ordered]@{
            plugin_root = '<PLUGIN>'
            project_root = '<PROJECT>'
            engine_root = '<ENGINE>'
        }
        identity = [pscustomobject][ordered]@{
            plugin_version = [string]$Report.identity.plugin_version
            engine_version = [string]$Report.identity.engine_version
            dotnet_sdk = [string]$Report.identity.dotnet_sdk
            powershell = [string]$Report.identity.powershell
            install_release_id = [string]$Report.identity.install_release_id
            binding_package_sha256 = [string]$Report.identity.binding_package_sha256
            generated_type_package_id = [string]$Report.identity.generated_type_package_id
            wasmtime_toolchain_id = [string]$Report.identity.wasmtime_toolchain_id
        }
        summary = [pscustomobject][ordered]@{
            passed = [int]$Report.summary.passed
            warning = [int]$Report.summary.warning
            blocked = [int]$Report.summary.blocked
            not_run = [int]$Report.summary.not_run
            total = [int]$Report.summary.total
        }
        checks = @($Checks)
    }
}

function Get-AvidScriptSupportLogTail {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$SensitivePaths
    )

    $Path = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
        [System.IO.Path]::GetExtension($Path).ToLowerInvariant() -notin @('.log', '.txt')) {
        Throw-AvidScriptPluginReleaseError 'ASSB1001' 'log_invalid' 'support logs must be existing .log or .txt files.'
    }
    $File = Get-Item -LiteralPath $Path -Force
    if (($File.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        Throw-AvidScriptPluginReleaseError 'ASSB1002' 'log_invalid' 'support logs must not be reparse points.'
    }
    if ($IsWindows) {
        $Streams = @(Get-Item -LiteralPath $Path -Stream * -ErrorAction Stop)
        if (@($Streams | Where-Object Stream -ne ':$DATA').Count -gt 0) {
            Throw-AvidScriptPluginReleaseError 'ASSB1003' 'log_invalid' 'support logs must not contain alternate data streams.'
        }
    }

    $Length = [int64]$File.Length
    $Start = [Math]::Max(
        [int64]0,
        $Length - $script:AvidScriptSupportMaximumLogBytes - 4)
    $Count = [int]($Length - $Start)
    $Bytes = [byte[]]::new($Count)
    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        [void]$Stream.Seek($Start, [System.IO.SeekOrigin]::Begin)
        $Read = 0
        while ($Read -lt $Count) {
            $Chunk = $Stream.Read($Bytes, $Read, $Count - $Read)
            if ($Chunk -le 0) { break }
            $Read += $Chunk
        }
        if ($Read -ne $Count) {
            Throw-AvidScriptPluginReleaseError 'ASSB1004' 'log_invalid' 'support log changed while its tail was read.'
        }
    }
    finally {
        $Stream.Dispose()
    }
    $StrictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
    $Text = $null
    $MaximumPrefix = if ($Start -gt 0) { [Math]::Min(3, $Bytes.Length) } else { 0 }
    for ($Prefix = 0; $Prefix -le $MaximumPrefix; $Prefix++) {
        try {
            $Text = $StrictUtf8.GetString($Bytes, $Prefix, $Bytes.Length - $Prefix)
            break
        }
        catch {
            continue
        }
    }
    if ($null -eq $Text) {
        Throw-AvidScriptPluginReleaseError 'ASSB1005' 'log_invalid' 'support logs must be strict UTF-8.'
    }
    $Text = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    if ($Start -gt 0) {
        $FirstNewline = $Text.IndexOf("`n", [System.StringComparison]::Ordinal)
        $Text = if ($FirstNewline -ge 0) { $Text.Substring($FirstNewline + 1) } else { '' }
    }
    $Lines = @($Text.Split("`n") | Select-Object -Last $script:AvidScriptSupportMaximumLogLines)
    $SafeText = ConvertTo-AvidScriptSupportSafeText ([string]::Join("`n", $Lines)) $SensitivePaths
    $SafeBytes = [System.Text.UTF8Encoding]::new($false).GetBytes($SafeText)
    if ($SafeBytes.Length -le $script:AvidScriptSupportMaximumLogBytes) {
        return $SafeText
    }
    $SafeStart = $SafeBytes.Length - $script:AvidScriptSupportMaximumLogBytes
    while ($SafeStart -lt $SafeBytes.Length -and
        (($SafeBytes[$SafeStart] -band 0xC0) -eq 0x80)) {
        $SafeStart++
    }
    return [System.Text.UTF8Encoding]::new($false, $true).GetString(
        $SafeBytes,
        $SafeStart,
        $SafeBytes.Length - $SafeStart)
}

function Get-AvidScriptSupportBundleId {
    param([Parameter(Mandatory = $true)]$Manifest)

    $Copy = [ordered]@{}
    foreach ($Property in $Manifest.PSObject.Properties) {
        $Copy[$Property.Name] = if ($Property.Name -ceq 'bundle_id') { '' } else { $Property.Value }
    }
    return Get-AvidScriptPluginReleaseBytesSha256 (
        ConvertTo-AvidScriptPluginReleaseCanonicalJsonBytes $Copy)
}

function Assert-AvidScriptSupportBundlePrivacy {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)]$SensitivePaths
    )

    $Patterns = @(
        '(?i)[A-Z]:[\\/]+Users[\\/]+',
        '(?i)/(?:home|Users)/[^/\s"'']+',
        '(?i)-----BEGIN [A-Z ]*PRIVATE KEY-----',
        '\bgh[pousr]_[A-Za-z0-9_]{20,}\b',
        '\bAKIA[0-9A-Z]{16}\b',
        '\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b',
        '(?i)\bBearer\s+(?!<REDACTED>)',
        '(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b')
    $SensitiveStrings = @(
        [string]$SensitivePaths.PluginRoot,
        [string]$SensitivePaths.ProjectRoot,
        [string]$SensitivePaths.EngineRoot,
        [Environment]::UserName) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    foreach ($File in @(Get-ChildItem -LiteralPath $Root -File -Force -Recurse)) {
        $Text = [System.IO.File]::ReadAllText(
            $File.FullName,
            [System.Text.UTF8Encoding]::new($false, $true))
        foreach ($Sensitive in $SensitiveStrings) {
            if ($Text.IndexOf($Sensitive, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                Throw-AvidScriptPluginReleaseError 'ASSB1101' 'privacy_failed' "support bundle contains sensitive identity: $($File.Name)"
            }
        }
        foreach ($Pattern in $Patterns) {
            if ($Text -match $Pattern) {
                Throw-AvidScriptPluginReleaseError 'ASSB1102' 'privacy_failed' "support bundle privacy scan failed: $($File.Name)"
            }
        }
    }
}

function Assert-AvidScriptSupportBundleManifest {
    param([Parameter(Mandatory = $true)]$Manifest)

    if ([int]$Manifest.schema_version -ne 1 -or
        [string]$Manifest.format -cne 'avidscript.support.bundle' -or
        [string]$Manifest.payload.root -cne 'files' -or
        @($Manifest.payload.files).Count -ne [int]$Manifest.payload.file_count -or
        [int]$Manifest.payload.file_count -lt 1 -or
        [int]$Manifest.payload.file_count -gt 9) {
        Throw-AvidScriptPluginReleaseError 'ASSB1201' 'schema_invalid' 'support bundle manifest is invalid.'
    }
    Assert-AvidScriptPluginReleaseSha256 ([string]$Manifest.bundle_id) 'support bundle id'
    Assert-AvidScriptPluginReleaseSha256 ([string]$Manifest.payload.inventory_sha256) 'support inventory hash'
    if ((Get-AvidScriptSupportBundleId $Manifest) -cne [string]$Manifest.bundle_id) {
        Throw-AvidScriptPluginReleaseError 'ASSB1202' 'identity_invalid' 'support bundle id does not match its manifest.'
    }
}

function Resolve-AvidScriptSupportBundle {
    param(
        [Parameter(Mandatory = $true)][string]$BundleRoot,
        [Parameter(Mandatory = $true)]$SensitivePaths
    )

    $BundleRoot = [System.IO.Path]::GetFullPath($BundleRoot)
    Assert-AvidScriptPluginReleaseOrdinaryTree $BundleRoot 'support bundle'
    $ManifestPath = Join-Path $BundleRoot 'support.json'
    $FilesRoot = Join-Path $BundleRoot 'files'
    $Manifest = Read-AvidScriptPluginReleaseJsonObject $ManifestPath 'support bundle manifest'
    Assert-AvidScriptPluginReleaseJsonSchema `
        $ManifestPath `
        (Join-Path $script:AvidScriptSupportBundleRoot 'AvidScriptSupportBundle.schema.json') `
        'support bundle manifest'
    Assert-AvidScriptSupportBundleManifest $Manifest
    $RootNames = @(Get-ChildItem -LiteralPath $BundleRoot -Force | Sort-Object Name | ForEach-Object Name)
    if ([string]::Join('|', $RootNames) -cne 'files|support.json') {
        Throw-AvidScriptPluginReleaseError 'ASSB1203' 'inventory_invalid' 'support bundle root contains undeclared entries.'
    }
    $ActualFiles = @(Get-AvidScriptPluginReleaseInventory $FilesRoot)
    $ExpectedFiles = @($Manifest.payload.files)
    $ActualFilesSha256 = Get-AvidScriptPluginReleaseBytesSha256 (
        ConvertTo-AvidScriptPluginReleaseCanonicalJsonBytes $ActualFiles)
    $ExpectedFilesSha256 = Get-AvidScriptPluginReleaseBytesSha256 (
        ConvertTo-AvidScriptPluginReleaseCanonicalJsonBytes $ExpectedFiles)
    if ($ActualFilesSha256 -cne $ExpectedFilesSha256 -or
        (Get-AvidScriptPluginReleaseInventorySha256 $ActualFiles) -cne [string]$Manifest.payload.inventory_sha256 -or
        [int64]($ActualFiles | Measure-Object length -Sum).Sum -ne [int64]$Manifest.payload.total_bytes) {
        Throw-AvidScriptPluginReleaseError 'ASSB1204' 'inventory_invalid' 'support bundle payload differs from its inventory.'
    }
    Assert-AvidScriptSupportBundlePrivacy $BundleRoot $SensitivePaths
    return [pscustomobject][ordered]@{
        Root = $BundleRoot
        ManifestPath = $ManifestPath
        ManifestSha256 = Get-AvidScriptPluginReleaseSha256 $ManifestPath
        Manifest = $Manifest
    }
}

function Publish-AvidScriptSupportBundle {
    param(
        [Parameter(Mandatory = $true)]$CompatibilityReport,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [string[]]$LogPath = @()
    )

    if ($LogPath.Count -gt $script:AvidScriptSupportMaximumLogs) {
        Throw-AvidScriptPluginReleaseError 'ASSB1301' 'limit_exceeded' 'support bundle accepts at most 8 logs.'
    }
    $OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
    $SensitivePaths = [pscustomobject][ordered]@{
        PluginRoot = [string]$CompatibilityReport.context.plugin_root
        ProjectRoot = [string]$CompatibilityReport.context.project_root
        EngineRoot = [string]$CompatibilityReport.context.engine_root
    }
    foreach ($SensitiveRoot in @(
            $SensitivePaths.PluginRoot,
            $SensitivePaths.ProjectRoot,
            $SensitivePaths.EngineRoot)) {
        if ($OutputRoot.Equals($SensitiveRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
            (Test-AvidScriptPluginReleasePathUnderRoot $OutputRoot $SensitiveRoot)) {
            Throw-AvidScriptPluginReleaseError 'ASSB1302' 'path_invalid' 'support bundle output must remain outside source, project, and engine roots.'
        }
    }
    [System.IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
    if (-not (Test-AvidScriptCompatibilityOrdinaryRoot $OutputRoot)) {
        Throw-AvidScriptPluginReleaseError 'ASSB1303' 'path_invalid' 'support bundle output root must be an ordinary directory.'
    }
    $StagingRoot = Join-Path $OutputRoot ('.avidscript-support-stage-' + [guid]::NewGuid().ToString('N'))
    try {
        $FilesRoot = Join-Path $StagingRoot 'files'
        [System.IO.Directory]::CreateDirectory((Join-Path $FilesRoot 'logs')) | Out-Null
        $SafeReport = ConvertTo-AvidScriptSupportCompatibilityReport $CompatibilityReport $SensitivePaths
        Write-AvidScriptPluginReleaseJson (Join-Path $FilesRoot 'compatibility.json') $SafeReport
        for ($Index = 0; $Index -lt $LogPath.Count; $Index++) {
            $Tail = Get-AvidScriptSupportLogTail $LogPath[$Index] $SensitivePaths
            $LogDestination = Join-Path $FilesRoot ('logs/log-{0:d2}.txt' -f ($Index + 1))
            [System.IO.File]::WriteAllText(
                $LogDestination,
                $Tail.TrimEnd("`r", "`n") + "`n",
                [System.Text.UTF8Encoding]::new($false))
        }
        if ($LogPath.Count -eq 0) {
            Remove-Item -LiteralPath (Join-Path $FilesRoot 'logs') -Force
        }
        $Files = @(Get-AvidScriptPluginReleaseInventory $FilesRoot)
        $Manifest = [pscustomobject][ordered]@{
            schema_version = 1
            format = 'avidscript.support.bundle'
            bundle_id = ''
            generated_at_utc = [string]$CompatibilityReport.generated_at_utc
            compatibility = [pscustomobject][ordered]@{
                format = [string]$CompatibilityReport.format
                result = [string]$CompatibilityReport.result
                check_count = [int]$CompatibilityReport.summary.total
            }
            limits = [pscustomobject][ordered]@{
                maximum_logs = $script:AvidScriptSupportMaximumLogs
                maximum_log_bytes = $script:AvidScriptSupportMaximumLogBytes
                maximum_log_lines = $script:AvidScriptSupportMaximumLogLines
            }
            payload = [pscustomobject][ordered]@{
                root = 'files'
                inventory_sha256 = Get-AvidScriptPluginReleaseInventorySha256 $Files
                file_count = $Files.Count
                total_bytes = [int64]($Files | Measure-Object length -Sum).Sum
                files = $Files
            }
        }
        $Manifest.bundle_id = Get-AvidScriptSupportBundleId $Manifest
        Write-AvidScriptPluginReleaseJson (Join-Path $StagingRoot 'support.json') $Manifest
        Assert-AvidScriptSupportBundlePrivacy $StagingRoot $SensitivePaths
        $FinalRoot = Join-Path $OutputRoot (
            'AvidScriptSupport-' + $Manifest.bundle_id.Substring(0, 20))
        $Lock = Enter-AvidScriptPluginReleasePublishLock (
            Join-Path $OutputRoot '.avidscript-support-publish.lock')
        try {
            if (Test-Path -LiteralPath $FinalRoot) {
                $Winner = Resolve-AvidScriptSupportBundle $FinalRoot $SensitivePaths
                if ([string]$Winner.Manifest.bundle_id -cne [string]$Manifest.bundle_id) {
                    Throw-AvidScriptPluginReleaseError 'ASSB1304' 'output_collision' 'support bundle output identity collision.'
                }
                return $Winner
            }
            [System.IO.Directory]::Move($StagingRoot, $FinalRoot)
            return Resolve-AvidScriptSupportBundle $FinalRoot $SensitivePaths
        }
        finally {
            $Lock.Dispose()
        }
    }
    finally {
        if (Test-Path -LiteralPath $StagingRoot) {
            Remove-Item -LiteralPath $StagingRoot -Recurse -Force
        }
    }
}
