function Get-AvidScriptPluginInstallReceiptPath {
    param([Parameter(Mandatory = $true)][string]$PluginRoot)

    return Join-Path $PluginRoot '.avidscript-install.json'
}

function Assert-AvidScriptPluginInstallReceipt {
    param([Parameter(Mandatory = $true)]$Receipt)

    Assert-AvidScriptPluginReleaseObjectShape `
        -Value $Receipt `
        -Required @(
            'schema_version', 'format', 'release_id', 'version', 'manifest_sha256',
            'inventory_sha256', 'source_commit', 'source_tree', 'installed_at_utc', 'files') `
        -Label 'install receipt'
    if ([int]$Receipt.schema_version -ne 1 -or
        [string]$Receipt.format -cne 'avidscript.plugin.install' -or
        [string]$Receipt.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
        Throw-AvidScriptPluginReleaseError 'ASRI1001' 'receipt_invalid' 'install receipt header is invalid.'
    }
    foreach ($Name in @('release_id', 'manifest_sha256', 'inventory_sha256')) {
        Assert-AvidScriptPluginReleaseSha256 ([string]$Receipt.$Name) "install receipt $Name"
    }
    foreach ($Name in @('source_commit', 'source_tree')) {
        if ([string]$Receipt.$Name -notmatch '^[0-9a-f]{40}$') {
            Throw-AvidScriptPluginReleaseError 'ASRI1002' 'receipt_invalid' "install receipt $Name is invalid."
        }
    }
    try {
        [void][DateTimeOffset]::ParseExact(
            [string]$Receipt.installed_at_utc,
            'o',
            [System.Globalization.CultureInfo]::InvariantCulture)
    }
    catch {
        Throw-AvidScriptPluginReleaseError 'ASRI1003' 'receipt_invalid' 'install receipt timestamp is invalid.'
    }
    if (@($Receipt.files).Count -lt 1 -or
        (Get-AvidScriptPluginReleaseInventorySha256 @($Receipt.files)) -cne
        [string]$Receipt.inventory_sha256) {
        Throw-AvidScriptPluginReleaseError 'ASRI1004' 'receipt_invalid' 'install receipt inventory is invalid.'
    }
}

function Read-AvidScriptPluginInstallReceipt {
    param([Parameter(Mandatory = $true)][string]$PluginRoot)

    $Path = Get-AvidScriptPluginInstallReceiptPath $PluginRoot
    $Receipt = Read-AvidScriptPluginReleaseJsonObject $Path 'install receipt'
    Assert-AvidScriptPluginReleaseJsonSchema `
        -JsonPath $Path `
        -SchemaPath (Join-Path $script:AvidScriptPluginReleaseModuleRoot 'AvidScriptPluginInstallReceipt.schema.json') `
        -Label 'install receipt'
    Assert-AvidScriptPluginInstallReceipt $Receipt
    return $Receipt
}

function Get-AvidScriptInstalledPluginInventory {
    param([Parameter(Mandatory = $true)][string]$PluginRoot)

    Assert-AvidScriptPluginReleaseOrdinaryTree -Root $PluginRoot -Label 'installed plugin'
    $Names = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $Entries = [System.Collections.Generic.List[object]]::new()
    foreach ($File in @(Get-ChildItem -LiteralPath $PluginRoot -File -Force -Recurse)) {
        if ($File.FullName.Equals(
                (Get-AvidScriptPluginInstallReceiptPath $PluginRoot),
                [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $Relative = Normalize-AvidScriptPluginReleaseRelativePath `
            ([System.IO.Path]::GetRelativePath($PluginRoot, $File.FullName)) `
            'installed plugin file'
        $PackageRelative = "AvidScript/$Relative"
        if (-not $Names.Add($PackageRelative)) {
            Throw-AvidScriptPluginReleaseError 'ASRI1101' 'path_collision' "installed plugin contains a path collision: $PackageRelative"
        }
        $Entries.Add([pscustomobject][ordered]@{
                path = $PackageRelative
                length = [int64]$File.Length
                sha256 = Get-AvidScriptPluginReleaseSha256 $File.FullName
                role = Get-AvidScriptPluginReleaseRole $PackageRelative
            })
    }
    return @($Entries | Sort-Object path -CaseSensitive)
}

function Assert-AvidScriptInstalledPluginInventory {
    param(
        [Parameter(Mandatory = $true)][string]$PluginRoot,
        [Parameter(Mandatory = $true)][object[]]$ExpectedFiles,
        [Parameter(Mandatory = $true)][string]$ExpectedInventorySha256
    )

    $Drift = Get-AvidScriptInstalledPluginDrift $PluginRoot $ExpectedFiles
    if (-not $Drift.clean -or
        (Get-AvidScriptPluginReleaseInventorySha256 @($Drift.actual_files)) -cne
        $ExpectedInventorySha256) {
        Throw-AvidScriptPluginReleaseError `
            'ASRI1102' `
            'installed_drift' `
            ("installed plugin differs: missing=$($Drift.missing.Count), " +
                "modified=$($Drift.modified.Count), unknown=$($Drift.unknown.Count)")
    }
}

function Get-AvidScriptInstalledPluginDrift {
    param(
        [Parameter(Mandatory = $true)][string]$PluginRoot,
        [Parameter(Mandatory = $true)][object[]]$ExpectedFiles
    )

    $ActualFiles = @(Get-AvidScriptInstalledPluginInventory $PluginRoot)
    $ActualByPath = @{}
    foreach ($Actual in $ActualFiles) {
        $ActualByPath[[string]$Actual.path] = $Actual
    }
    $ExpectedByPath = @{}
    foreach ($Expected in $ExpectedFiles) {
        $ExpectedByPath[[string]$Expected.path] = $Expected
    }
    $Missing = [System.Collections.Generic.List[string]]::new()
    $Modified = [System.Collections.Generic.List[string]]::new()
    $Unknown = [System.Collections.Generic.List[string]]::new()
    foreach ($Path in @($ExpectedByPath.Keys | Sort-Object -CaseSensitive)) {
        if (-not $ActualByPath.ContainsKey($Path)) {
            $Missing.Add($Path)
            continue
        }
        $Actual = $ActualByPath[$Path]
        $Expected = $ExpectedByPath[$Path]
        if ([int64]$Actual.length -ne [int64]$Expected.length -or
            [string]$Actual.sha256 -cne [string]$Expected.sha256 -or
            [string]$Actual.role -cne [string]$Expected.role) {
            $Modified.Add($Path)
        }
    }
    foreach ($Path in @($ActualByPath.Keys | Sort-Object -CaseSensitive)) {
        if (-not $ExpectedByPath.ContainsKey($Path)) {
            $Unknown.Add($Path)
        }
    }
    return [pscustomobject][ordered]@{
        clean = $Missing.Count -eq 0 -and $Modified.Count -eq 0 -and $Unknown.Count -eq 0
        missing = @($Missing)
        modified = @($Modified)
        unknown = @($Unknown)
        actual_files = $ActualFiles
    }
}

function Get-AvidScriptPluginInstallTarget {
    param([Parameter(Mandatory = $true)][string]$ProjectRoot)

    $ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
    if (-not (Test-Path -LiteralPath $ProjectRoot -PathType Container)) {
        Throw-AvidScriptPluginReleaseError 'ASRI1201' 'project_invalid' "project root does not exist: $ProjectRoot"
    }
    $ProjectFiles = @(Get-ChildItem -LiteralPath $ProjectRoot -File -Filter '*.uproject')
    if ($ProjectFiles.Count -ne 1) {
        Throw-AvidScriptPluginReleaseError 'ASRI1202' 'project_invalid' 'project root must contain exactly one .uproject file.'
    }
    if ((Get-Item -LiteralPath $ProjectRoot -Force).Attributes -band
        [System.IO.FileAttributes]::ReparsePoint) {
        Throw-AvidScriptPluginReleaseError 'ASRI1203' 'reparse_point' 'project root must not be a reparse point.'
    }
    $PluginsRoot = Join-Path $ProjectRoot 'Plugins'
    if ((Test-Path -LiteralPath $PluginsRoot) -and
        (((Get-Item -LiteralPath $PluginsRoot -Force).Attributes -band
                [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
        Throw-AvidScriptPluginReleaseError 'ASRI1204' 'reparse_point' 'project Plugins directory must not be a reparse point.'
    }
    return [pscustomobject][ordered]@{
        ProjectRoot = $ProjectRoot
        ProjectFile = $ProjectFiles[0].FullName
        PluginsRoot = $PluginsRoot
        PluginRoot = Join-Path $PluginsRoot 'AvidScript'
        LockPath = Join-Path $PluginsRoot '.avidscript-install.lock'
    }
}

function Compare-AvidScriptPluginReleaseVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Left,
        [Parameter(Mandatory = $true)][string]$Right
    )

    return ([version]$Left).CompareTo([version]$Right)
}

function Get-AvidScriptPluginInstallPlan {
    param(
        [Parameter(Mandatory = $true)]$Package,
        [Parameter(Mandatory = $true)]$Target,
        [switch]$AllowDowngrade
    )

    if (-not (Test-Path -LiteralPath $Target.PluginRoot)) {
        return [pscustomobject][ordered]@{
            action = 'Install'
            current_release_id = $null
            current_version = $null
            target_release_id = [string]$Package.Manifest.release_id
            target_version = [string]$Package.Manifest.version
        }
    }
    if (((Get-Item -LiteralPath $Target.PluginRoot -Force).Attributes -band
            [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        Throw-AvidScriptPluginReleaseError 'ASRI1205' 'reparse_point' 'existing AvidScript target must not be a reparse point.'
    }
    if (-not (Test-Path -LiteralPath (Get-AvidScriptPluginInstallReceiptPath $Target.PluginRoot) -PathType Leaf)) {
        Throw-AvidScriptPluginReleaseError 'ASRI1206' 'unmanaged_target' 'existing AvidScript directory has no valid install receipt.'
    }
    $Current = Read-AvidScriptPluginInstallReceipt $Target.PluginRoot
    $Drift = Get-AvidScriptInstalledPluginDrift $Target.PluginRoot @($Current.files)
    if ($Drift.unknown.Count -gt 0) {
        Throw-AvidScriptPluginReleaseError 'ASRI1207' 'installed_drift' 'installed plugin contains files that are not owned by its receipt.'
    }
    if ([string]$Current.release_id -ceq [string]$Package.Manifest.release_id) {
        return [pscustomobject][ordered]@{
            action = if ($Drift.clean) { 'NoOp' } else { 'Repair' }
            current_release_id = [string]$Current.release_id
            current_version = [string]$Current.version
            target_release_id = [string]$Package.Manifest.release_id
            target_version = [string]$Package.Manifest.version
        }
    }
    if (-not $Drift.clean) {
        Throw-AvidScriptPluginReleaseError 'ASRI1208' 'installed_drift' 'installed plugin drift must be repaired before changing release identity.'
    }
    $Comparison = Compare-AvidScriptPluginReleaseVersion `
        ([string]$Current.version) `
        ([string]$Package.Manifest.version)
    if ($Comparison -gt 0 -and -not $AllowDowngrade) {
        Throw-AvidScriptPluginReleaseError 'ASRI1209' 'downgrade_rejected' 'target release is older than the installed release.'
    }
    return [pscustomobject][ordered]@{
        action = if ($Comparison -eq 0) { 'Repair' } elseif ($Comparison -gt 0) { 'Downgrade' } else { 'Upgrade' }
        current_release_id = [string]$Current.release_id
        current_version = [string]$Current.version
        target_release_id = [string]$Package.Manifest.release_id
        target_version = [string]$Package.Manifest.version
    }
}

function Enter-AvidScriptPluginInstallLock {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [ValidateRange(1, 300)][int]$TimeoutSeconds = 30
    )

    $Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        try {
            return [System.IO.File]::Open(
                $Path,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None)
        }
        catch [System.IO.IOException] {
            if ([DateTime]::UtcNow -ge $Deadline) {
                Throw-AvidScriptPluginReleaseError 'ASRI1301' 'lock_timeout' 'timed out waiting for the project install lock.'
            }
            Start-Sleep -Milliseconds 50
        }
    } while ($true)
}

function New-AvidScriptPluginInstallReceipt {
    param([Parameter(Mandatory = $true)]$Package)

    return [pscustomobject][ordered]@{
        schema_version = 1
        format = 'avidscript.plugin.install'
        release_id = [string]$Package.Manifest.release_id
        version = [string]$Package.Manifest.version
        manifest_sha256 = [string]$Package.ManifestSha256
        inventory_sha256 = [string]$Package.Manifest.payload.inventory_sha256
        source_commit = [string]$Package.Manifest.source.commit
        source_tree = [string]$Package.Manifest.source.tree
        installed_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
        files = @($Package.Manifest.payload.files)
    }
}

function Assert-AvidScriptPluginInstallWorkingPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$PluginsRoot,
        [Parameter(Mandatory = $true)][string]$Prefix
    )

    if (-not (Test-AvidScriptPluginReleasePathUnderRoot $Path $PluginsRoot) -or
        -not [System.IO.Path]::GetFileName($Path).StartsWith(
            $Prefix,
            [System.StringComparison]::Ordinal)) {
        Throw-AvidScriptPluginReleaseError 'ASRI1302' 'path_invalid' 'installer working path escaped its project boundary.'
    }
}

function Invoke-AvidScriptPluginInstallTransaction {
    param(
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [ValidateSet('Plan', 'Apply', 'Verify')][string]$Mode = 'Plan',
        [switch]$AllowDowngrade,
        [ValidateRange(1, 300)][int]$LockTimeoutSeconds = 30,
        [ValidateSet('', 'AfterStage', 'AfterBackup', 'BeforeReadback', 'BackupCleanup')]
        [string]$FaultPoint = ''
    )

    $Package = Resolve-AvidScriptPluginReleasePackage $PackageRoot
    $Target = Get-AvidScriptPluginInstallTarget $ProjectRoot
    $Plan = Get-AvidScriptPluginInstallPlan $Package $Target -AllowDowngrade:$AllowDowngrade
    if ($Mode -ceq 'Plan') {
        return [pscustomobject][ordered]@{
            schema_version = 1
            result = 'planned'
            mode = $Mode
            action = $Plan.action
            project_root = $Target.ProjectRoot
            plugin_root = $Target.PluginRoot
            release_id = [string]$Package.Manifest.release_id
            version = [string]$Package.Manifest.version
            changed = $false
            warnings = @()
        }
    }
    if ($Mode -ceq 'Verify') {
        if (-not (Test-Path -LiteralPath $Target.PluginRoot -PathType Container)) {
            Throw-AvidScriptPluginReleaseError 'ASRI1401' 'input_missing' 'AvidScript is not installed in the target project.'
        }
        $Receipt = Read-AvidScriptPluginInstallReceipt $Target.PluginRoot
        if ([string]$Receipt.release_id -cne [string]$Package.Manifest.release_id -or
            [string]$Receipt.manifest_sha256 -cne [string]$Package.ManifestSha256) {
            Throw-AvidScriptPluginReleaseError 'ASRI1402' 'release_mismatch' 'installed release differs from the requested package.'
        }
        Assert-AvidScriptInstalledPluginInventory `
            -PluginRoot $Target.PluginRoot `
            -ExpectedFiles @($Package.Manifest.payload.files) `
            -ExpectedInventorySha256 ([string]$Package.Manifest.payload.inventory_sha256)
        return [pscustomobject][ordered]@{
            schema_version = 1
            result = 'passed'
            mode = $Mode
            action = 'Verify'
            project_root = $Target.ProjectRoot
            plugin_root = $Target.PluginRoot
            release_id = [string]$Package.Manifest.release_id
            version = [string]$Package.Manifest.version
            changed = $false
            warnings = @()
        }
    }

    [System.IO.Directory]::CreateDirectory($Target.PluginsRoot) | Out-Null
    $Lock = Enter-AvidScriptPluginInstallLock $Target.LockPath $LockTimeoutSeconds
    $Staging = Join-Path $Target.PluginsRoot (
        '.AvidScript.stage.' + [guid]::NewGuid().ToString('N'))
    $Backup = Join-Path $Target.PluginsRoot (
        '.AvidScript.backup.' + [guid]::NewGuid().ToString('N'))
    Assert-AvidScriptPluginInstallWorkingPath $Staging $Target.PluginsRoot '.AvidScript.stage.'
    Assert-AvidScriptPluginInstallWorkingPath $Backup $Target.PluginsRoot '.AvidScript.backup.'
    $Committed = $false
    $PreserveRecoveryMaterials = $false
    $Warnings = [System.Collections.Generic.List[string]]::new()
    try {
        $Plan = Get-AvidScriptPluginInstallPlan $Package $Target -AllowDowngrade:$AllowDowngrade
        if ($Plan.action -ceq 'NoOp') {
            return [pscustomobject][ordered]@{
                schema_version = 1
                result = 'passed'
                mode = $Mode
                action = 'NoOp'
                project_root = $Target.ProjectRoot
                plugin_root = $Target.PluginRoot
                release_id = [string]$Package.Manifest.release_id
                version = [string]$Package.Manifest.version
                changed = $false
                warnings = @()
            }
        }
        Copy-AvidScriptPluginReleasePayload $Package.PluginRoot $Staging
        Write-AvidScriptPluginReleaseJson `
            (Get-AvidScriptPluginInstallReceiptPath $Staging) `
            (New-AvidScriptPluginInstallReceipt $Package)
        Assert-AvidScriptInstalledPluginInventory `
            -PluginRoot $Staging `
            -ExpectedFiles @($Package.Manifest.payload.files) `
            -ExpectedInventorySha256 ([string]$Package.Manifest.payload.inventory_sha256)
        if ($FaultPoint -ceq 'AfterStage') {
            throw 'ASRI_TEST injected failure after staging.'
        }
        if (Test-Path -LiteralPath $Target.PluginRoot) {
            [System.IO.Directory]::Move($Target.PluginRoot, $Backup)
        }
        if ($FaultPoint -ceq 'AfterBackup') {
            throw 'ASRI_TEST injected failure after backup.'
        }
        [System.IO.Directory]::Move($Staging, $Target.PluginRoot)
        if ($FaultPoint -ceq 'BeforeReadback') {
            throw 'ASRI_TEST injected failure before readback.'
        }
        $InstalledReceipt = Read-AvidScriptPluginInstallReceipt $Target.PluginRoot
        if ([string]$InstalledReceipt.release_id -cne [string]$Package.Manifest.release_id -or
            [string]$InstalledReceipt.manifest_sha256 -cne [string]$Package.ManifestSha256) {
            Throw-AvidScriptPluginReleaseError 'ASRI1403' 'readback_failed' 'installed receipt differs after commit.'
        }
        Assert-AvidScriptInstalledPluginInventory `
            -PluginRoot $Target.PluginRoot `
            -ExpectedFiles @($Package.Manifest.payload.files) `
            -ExpectedInventorySha256 ([string]$Package.Manifest.payload.inventory_sha256)
        $Committed = $true
        if (Test-Path -LiteralPath $Backup) {
            if ($FaultPoint -ceq 'BackupCleanup') {
                $Warnings.Add("backup cleanup was intentionally deferred: $Backup")
            }
            else {
                try {
                    Remove-Item -LiteralPath $Backup -Recurse -Force
                }
                catch {
                    $Warnings.Add("backup cleanup failed; recovery material was preserved: $Backup")
                }
            }
        }
        return [pscustomobject][ordered]@{
            schema_version = 1
            result = 'passed'
            mode = $Mode
            action = $Plan.action
            project_root = $Target.ProjectRoot
            plugin_root = $Target.PluginRoot
            release_id = [string]$Package.Manifest.release_id
            version = [string]$Package.Manifest.version
            changed = $true
            warnings = @($Warnings)
        }
    }
    catch {
        $OriginalFailure = $_
        if (-not $Committed) {
            $RollbackFailures = [System.Collections.Generic.List[string]]::new()
            try {
                if (Test-Path -LiteralPath $Backup) {
                    if (Test-Path -LiteralPath $Target.PluginRoot) {
                        Remove-Item -LiteralPath $Target.PluginRoot -Recurse -Force
                    }
                    [System.IO.Directory]::Move($Backup, $Target.PluginRoot)
                }
                elseif ((Test-Path -LiteralPath $Target.PluginRoot) -and
                    $Plan.action -ceq 'Install') {
                    Remove-Item -LiteralPath $Target.PluginRoot -Recurse -Force
                }
            }
            catch {
                $RollbackFailures.Add("target restore: $($_.Exception.Message)")
            }
            try {
                if (Test-Path -LiteralPath $Staging) {
                    Remove-Item -LiteralPath $Staging -Recurse -Force
                }
            }
            catch {
                $RollbackFailures.Add("staging cleanup: $($_.Exception.Message)")
            }
            if ($RollbackFailures.Count -gt 0) {
                $PreserveRecoveryMaterials = $true
                Throw-AvidScriptPluginReleaseError `
                    'ASRI1404' `
                    'rollback_incomplete' `
                    ("install failed and rollback was incomplete; recovery material was preserved. " +
                        "original=$($OriginalFailure.Exception.Message); " +
                        "rollback=$($RollbackFailures -join '; ')")
            }
        }
        throw $OriginalFailure
    }
    finally {
        $Lock.Dispose()
        if (-not $PreserveRecoveryMaterials -and (Test-Path -LiteralPath $Staging)) {
            try {
                Remove-Item -LiteralPath $Staging -Recurse -Force
            }
            catch {
                if ($Committed) {
                    $Warnings.Add("staging cleanup failed after commit: $Staging")
                }
            }
        }
    }
}
