$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$PluginRoot = Split-Path -Parent $BuildRoot
$PackageModule = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptPluginReleasePackage.ps1'
$InstallerModule = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptPluginInstaller.ps1'
$PublisherPath = Join-Path $BuildRoot 'PublishAvidScriptPluginRelease.ps1'
$InstallerPath = Join-Path $BuildRoot 'InstallAvidScriptPluginRelease.ps1'
$ReleaseSchema = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptPluginRelease.schema.json'
$ReceiptSchema = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptPluginInstallReceipt.schema.json'
$SourceFilesSchema = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptSourceReleaseFiles.schema.json'
$SourceFilesProfile = Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptSourceRelease.files.json'
. $PackageModule
. $InstallerModule

$Root = Join-Path 'C:\tmp\AvidScript\P65Contracts' (
    "$PID-$([guid]::NewGuid().ToString('N'))")
$Passed = 0
$Total = 24

function Invoke-PluginReleaseContract {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    try {
        & $Body
        $script:Passed++
    }
    catch {
        throw "$Name failed: $($_.Exception.Message)"
    }
}

function New-PluginReleaseFixture {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Version,
        [string]$Marker = 'one'
    )

    $FixtureRoot = Join-Path $Root $Name
    $PayloadRoot = Join-Path $FixtureRoot 'input'
    $PayloadPlugin = Join-Path $PayloadRoot 'AvidScript'
    $DependencyReceipt = Join-Path `
        $PayloadPlugin `
        'Source/ThirdParty/TestRuntime/.managed.json'
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $DependencyReceipt)) | Out-Null
    [System.IO.Directory]::CreateDirectory((Join-Path $PayloadPlugin 'Build')) | Out-Null
    [System.IO.File]::WriteAllText(
        (Join-Path $PayloadPlugin 'AvidScript.uplugin'),
        "{`"FileVersion`":3,`"VersionName`":`"$Version`"}")
    [System.IO.File]::WriteAllText((Join-Path $PayloadPlugin 'LICENSE'), 'MIT')
    [System.IO.File]::WriteAllText((Join-Path $PayloadPlugin 'Build/marker.txt'), $Marker)
    $GeneratedTypeProducer = Join-Path $PayloadPlugin 'Build/AvidScriptGeneratedTypeCookPackage.ps1'
    $ModuleReleaseProducer = Join-Path $PayloadPlugin 'Build/AvidScriptModuleReleasePackage.ps1'
    [System.IO.File]::WriteAllText($GeneratedTypeProducer, "# generated type $Marker")
    [System.IO.File]::WriteAllText($ModuleReleaseProducer, "# module release $Marker")
    [System.IO.File]::WriteAllText($DependencyReceipt, '{"managed":true}')
    $Dependency = [pscustomobject][ordered]@{
        id = 'test-runtime'
        version = '1.0.0'
        mode = 'external'
        identity_path = 'AvidScript/Source/ThirdParty/TestRuntime/.managed.json'
        identity_sha256 = Get-AvidScriptPluginReleaseSha256 $DependencyReceipt
    }
    $ArtifactContracts = @(
        [pscustomobject][ordered]@{
            id = 'generated-type-package'
            producer_path = 'AvidScript/Build/AvidScriptGeneratedTypeCookPackage.ps1'
            producer_sha256 = Get-AvidScriptPluginReleaseSha256 $GeneratedTypeProducer
        },
        [pscustomobject][ordered]@{
            id = 'module-release-package'
            producer_path = 'AvidScript/Build/AvidScriptModuleReleasePackage.ps1'
            producer_sha256 = Get-AvidScriptPluginReleaseSha256 $ModuleReleaseProducer
        })
    return Publish-AvidScriptPluginReleasePackage `
        -PayloadSourceRoot $PayloadRoot `
        -OutputRoot (Join-Path $FixtureRoot 'output') `
        -Version $Version `
        -Channel preview `
        -Commit ('a' * 40) `
        -Tree ('b' * 40) `
        -CommittedAtUtc '2026-09-06T00:00:00.0000000+00:00' `
        -ArtifactContracts $ArtifactContracts `
        -Dependencies @($Dependency)
}

function New-PluginReleaseProject {
    param([Parameter(Mandatory = $true)][string]$Name)

    $ProjectRoot = Join-Path $Root $Name
    [System.IO.Directory]::CreateDirectory($ProjectRoot) | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $ProjectRoot "$Name.uproject"), '{}')
    return $ProjectRoot
}

function Get-ScriptParameterNames {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Tokens = $null
    $Errors = $null
    $Ast = [System.Management.Automation.Language.Parser]::ParseFile(
        $Path,
        [ref]$Tokens,
        [ref]$Errors)
    if ($Errors.Count -gt 0) {
        throw "$Path contains parse errors: $($Errors.Message -join '; ')"
    }
    return @($Ast.ParamBlock.Parameters |
        ForEach-Object { $_.Name.VariablePath.UserPath } |
        Sort-Object)
}

[System.IO.Directory]::CreateDirectory($Root) | Out-Null
try {
    foreach ($Path in @(
            $PackageModule,
            $InstallerModule,
            $PublisherPath,
            $InstallerPath,
            $ReleaseSchema,
            $ReceiptSchema,
            $SourceFilesSchema,
            $SourceFilesProfile)) {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "required P65.A file is missing: $Path"
        }
    }
    $null = Get-Content -LiteralPath $ReleaseSchema -Raw | ConvertFrom-Json
    $null = Get-Content -LiteralPath $ReceiptSchema -Raw | ConvertFrom-Json

    Invoke-PluginReleaseContract 'source release profile schema' {
        $Valid = (Get-Content -LiteralPath $SourceFilesProfile -Raw) |
            Test-Json -SchemaFile $SourceFilesSchema
        if (-not $Valid) {
            throw 'checked-in source release profile does not satisfy its schema.'
        }
    }

    Invoke-PluginReleaseContract 'source release profile schema rejects drift' {
        $InvalidPath = Join-Path $Root 'invalid-source-release-files.json'
        $Profile = Get-Content -LiteralPath $SourceFilesProfile -Raw |
            ConvertFrom-Json -Depth 32 -DateKind String
        $Profile.generated.readme_destination = '../README.md'
        Write-AvidScriptPluginReleaseJson $InvalidPath $Profile
        $Rejected = $false
        try {
            Assert-AvidScriptPluginReleaseJsonSchema `
                $InvalidPath $SourceFilesSchema 'invalid source release profile'
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'schema_invalid'
        }
        if (-not $Rejected) {
            throw 'source release profile schema accepted destination drift.'
        }
    }

    Invoke-PluginReleaseContract 'deterministic package identity' {
        $First = New-PluginReleaseFixture 'DeterministicA' '0.1.0'
        $Second = New-PluginReleaseFixture 'DeterministicB' '0.1.0'
        if ([string]$First.Manifest.release_id -cne [string]$Second.Manifest.release_id -or
            [string]$First.ManifestSha256 -cne [string]$Second.ManifestSha256) {
            throw 'identical payload and source identity produced different manifests.'
        }
    }

    Invoke-PluginReleaseContract 'package readback' {
        $Package = New-PluginReleaseFixture 'Readback' '0.1.0'
        $Readback = Resolve-AvidScriptPluginReleasePackage $Package.Root
        $SchemaValid = (Get-Content -LiteralPath $Readback.ManifestPath -Raw) |
            Test-Json -SchemaFile $ReleaseSchema
        if ([string]$Readback.Manifest.release_id -cne [string]$Package.Manifest.release_id -or
            [int]$Readback.Manifest.payload.file_count -ne 6 -or -not $SchemaValid) {
            throw 'package readback identity differs.'
        }
    }

    Invoke-PluginReleaseContract 'artifact contract identity' {
        $Package = New-PluginReleaseFixture 'ArtifactIdentity' '0.1.0'
        $Contracts = @($Package.Manifest.artifact_contracts)
        if ($Contracts.Count -ne 2 -or
            [string]$Contracts[0].id -cne 'generated-type-package' -or
            [string]$Contracts[1].id -cne 'module-release-package' -or
            @($Contracts | Where-Object { [string]$_.producer_sha256 -notmatch '^[0-9a-f]{64}$' }).Count -ne 0) {
            throw 'release manifest did not bind the expected artifact producer identities.'
        }
    }

    Invoke-PluginReleaseContract 'artifact producer mismatch rejection' {
        $Package = New-PluginReleaseFixture 'ArtifactMismatch' '0.1.0'
        $Manifest = Get-Content -LiteralPath $Package.ManifestPath -Raw |
            ConvertFrom-Json -Depth 64 -DateKind String
        $Manifest.artifact_contracts[0].producer_sha256 = 'f' * 64
        $Manifest.release_id = Get-AvidScriptPluginReleaseId $Manifest
        Write-AvidScriptPluginReleaseJson $Package.ManifestPath $Manifest
        $Rejected = $false
        try {
            Resolve-AvidScriptPluginReleasePackage $Package.Root | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'artifact_contract_invalid'
        }
        if (-not $Rejected) {
            throw 'artifact producer identity mismatch was accepted.'
        }
    }

    Invoke-PluginReleaseContract 'same-root publication winner' {
        $First = New-PluginReleaseFixture 'SameRoot' '0.1.0'
        $Winner = Publish-AvidScriptPluginReleasePackage `
            -PayloadSourceRoot $First.PayloadRoot `
            -OutputRoot (Split-Path -Parent $First.Root) `
            -Version '0.1.0' `
            -Channel preview `
            -Commit ('a' * 40) `
            -Tree ('b' * 40) `
            -CommittedAtUtc '2026-09-06T00:00:00.0000000+00:00' `
            -ArtifactContracts @($First.Manifest.artifact_contracts) `
            -Dependencies @($First.Manifest.dependencies)
        if ($Winner.Root -cne $First.Root -or
            $Winner.ManifestSha256 -cne $First.ManifestSha256) {
            throw 'same-root deterministic publication did not reuse its verified winner.'
        }
    }

    Invoke-PluginReleaseContract 'tamper rejection' {
        $Package = New-PluginReleaseFixture 'Tamper' '0.1.0'
        [System.IO.File]::AppendAllText((Join-Path $Package.PluginRoot 'Build/marker.txt'), 'tampered')
        $Rejected = $false
        try {
            Resolve-AvidScriptPluginReleasePackage $Package.Root | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'inventory_invalid'
        }
        if (-not $Rejected) {
            throw 'tampered payload was accepted.'
        }
    }

    Invoke-PluginReleaseContract 'unknown file rejection' {
        $Package = New-PluginReleaseFixture 'UnknownFile' '0.1.0'
        [System.IO.File]::WriteAllText((Join-Path $Package.PluginRoot 'unknown.txt'), 'unknown')
        $Rejected = $false
        try {
            Resolve-AvidScriptPluginReleasePackage $Package.Root | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'inventory_invalid'
        }
        if (-not $Rejected) {
            throw 'undeclared payload file was accepted.'
        }
    }

    Invoke-PluginReleaseContract 'duplicate JSON rejection' {
        $Path = Join-Path $Root 'duplicate.json'
        [System.IO.File]::WriteAllText($Path, '{"schema_version":1,"schema_version":1}')
        $Rejected = $false
        try {
            Read-AvidScriptPluginReleaseJsonObject $Path 'duplicate fixture' | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['code'] -ceq 'ASRE1004'
        }
        if (-not $Rejected) {
            throw 'duplicate JSON property was accepted.'
        }
    }

    Invoke-PluginReleaseContract 'portable path rejection' {
        foreach ($Invalid in @('../escape', 'C:/escape', 'a//b', 'a/./b', 'a/b.')) {
            $Rejected = $false
            try {
                Normalize-AvidScriptPluginReleaseRelativePath $Invalid | Out-Null
            }
            catch {
                $Rejected = [string]$_.Exception.Data['category'] -ceq 'path_invalid'
            }
            if (-not $Rejected) {
                throw "invalid path was accepted: $Invalid"
            }
        }
    }

    Invoke-PluginReleaseContract 'plan install without writes' {
        $Package = New-PluginReleaseFixture 'Plan' '0.1.0'
        $Project = New-PluginReleaseProject 'PlanProject'
        $Plan = Invoke-AvidScriptPluginInstallTransaction `
            -PackageRoot $Package.Root `
            -ProjectRoot $Project `
            -Mode Plan
        if ($Plan.action -cne 'Install' -or
            (Test-Path -LiteralPath (Join-Path $Project 'Plugins/AvidScript'))) {
            throw 'Plan mode wrote the project or selected the wrong action.'
        }
    }

    Invoke-PluginReleaseContract 'install verify and no-op' {
        $Package = New-PluginReleaseFixture 'Install' '0.1.0'
        $Project = New-PluginReleaseProject 'InstallProject'
        $Install = Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Apply
        $Verify = Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Verify
        $NoOp = Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Apply
        $ReceiptPath = Join-Path $Project 'Plugins/AvidScript/.avidscript-install.json'
        $ReceiptSchemaValid = (Get-Content -LiteralPath $ReceiptPath -Raw) |
            Test-Json -SchemaFile $ReceiptSchema
        if ($Install.action -cne 'Install' -or $Verify.result -cne 'passed' -or
            $NoOp.action -cne 'NoOp' -or $NoOp.changed -or -not $ReceiptSchemaValid) {
            throw 'install, verify, or idempotent apply contract failed.'
        }
    }

    Invoke-PluginReleaseContract 'upgrade transaction' {
        $Old = New-PluginReleaseFixture 'UpgradeOld' '0.1.0' 'old'
        $New = New-PluginReleaseFixture 'UpgradeNew' '0.2.0' 'new'
        $Project = New-PluginReleaseProject 'UpgradeProject'
        Invoke-AvidScriptPluginInstallTransaction $Old.Root $Project Apply | Out-Null
        $Upgrade = Invoke-AvidScriptPluginInstallTransaction $New.Root $Project Apply
        $Verify = Invoke-AvidScriptPluginInstallTransaction $New.Root $Project Verify
        if ($Upgrade.action -cne 'Upgrade' -or $Verify.result -cne 'passed') {
            throw 'upgrade did not commit the new release.'
        }
    }

    Invoke-PluginReleaseContract 'downgrade rejection' {
        $Old = New-PluginReleaseFixture 'DowngradeOld' '0.1.0' 'old'
        $New = New-PluginReleaseFixture 'DowngradeNew' '0.2.0' 'new'
        $Project = New-PluginReleaseProject 'DowngradeProject'
        Invoke-AvidScriptPluginInstallTransaction $New.Root $Project Apply | Out-Null
        $Rejected = $false
        try {
            Invoke-AvidScriptPluginInstallTransaction $Old.Root $Project Plan | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'downgrade_rejected'
        }
        if (-not $Rejected) {
            throw 'implicit downgrade was accepted.'
        }
    }

    Invoke-PluginReleaseContract 'upgrade rollback after backup' {
        $Old = New-PluginReleaseFixture 'RollbackOld' '0.1.0' 'old'
        $New = New-PluginReleaseFixture 'RollbackNew' '0.2.0' 'new'
        $Project = New-PluginReleaseProject 'RollbackProject'
        Invoke-AvidScriptPluginInstallTransaction $Old.Root $Project Apply | Out-Null
        try {
            Invoke-AvidScriptPluginInstallTransaction `
                $New.Root $Project Apply -FaultPoint AfterBackup | Out-Null
            throw 'fault injection did not fail.'
        }
        catch {
            if (-not $_.Exception.Message.Contains('ASRI_TEST')) {
                throw
            }
        }
        $Verify = Invoke-AvidScriptPluginInstallTransaction $Old.Root $Project Verify
        if ($Verify.result -cne 'passed') {
            throw 'old release was not restored after backup failure.'
        }
    }

    Invoke-PluginReleaseContract 'initial install rollback' {
        $Package = New-PluginReleaseFixture 'InitialRollback' '0.1.0'
        $Project = New-PluginReleaseProject 'InitialRollbackProject'
        try {
            Invoke-AvidScriptPluginInstallTransaction `
                $Package.Root $Project Apply -FaultPoint BeforeReadback | Out-Null
            throw 'fault injection did not fail.'
        }
        catch {
            if (-not $_.Exception.Message.Contains('ASRI_TEST')) {
                throw
            }
        }
        if (Test-Path -LiteralPath (Join-Path $Project 'Plugins/AvidScript')) {
            throw 'failed initial install left a committed plugin.'
        }
    }

    Invoke-PluginReleaseContract 'installed drift blocks upgrade' {
        $Old = New-PluginReleaseFixture 'DriftOld' '0.1.0' 'old'
        $New = New-PluginReleaseFixture 'DriftNew' '0.2.0' 'new'
        $Project = New-PluginReleaseProject 'DriftProject'
        Invoke-AvidScriptPluginInstallTransaction $Old.Root $Project Apply | Out-Null
        [System.IO.File]::AppendAllText(
            (Join-Path $Project 'Plugins/AvidScript/Build/marker.txt'),
            'user-change')
        $Rejected = $false
        try {
            Invoke-AvidScriptPluginInstallTransaction $New.Root $Project Plan | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'installed_drift'
        }
        if (-not $Rejected) {
            throw 'upgrade overwrote or accepted installed drift.'
        }
    }

    Invoke-PluginReleaseContract 'managed repair transaction' {
        $Package = New-PluginReleaseFixture 'Repair' '0.1.0'
        $Project = New-PluginReleaseProject 'RepairProject'
        Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Apply | Out-Null
        [System.IO.File]::AppendAllText(
            (Join-Path $Project 'Plugins/AvidScript/Build/marker.txt'),
            'damaged')
        $Plan = Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Plan
        $Repair = Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Apply
        $Verify = Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Verify
        if ($Plan.action -cne 'Repair' -or $Repair.action -cne 'Repair' -or
            $Verify.result -cne 'passed') {
            throw 'known managed file damage was not repaired transactionally.'
        }
    }

    Invoke-PluginReleaseContract 'unknown installed file blocks repair' {
        $Package = New-PluginReleaseFixture 'RepairUnknown' '0.1.0'
        $Project = New-PluginReleaseProject 'RepairUnknownProject'
        Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Apply | Out-Null
        [System.IO.File]::WriteAllText(
            (Join-Path $Project 'Plugins/AvidScript/user-owned.txt'),
            'preserve')
        $Rejected = $false
        try {
            Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Plan | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'installed_drift'
        }
        if (-not $Rejected) {
            throw 'repair accepted a file outside the managed receipt.'
        }
    }

    Invoke-PluginReleaseContract 'unmanaged target rejection' {
        $Package = New-PluginReleaseFixture 'Unmanaged' '0.1.0'
        $Project = New-PluginReleaseProject 'UnmanagedProject'
        [System.IO.Directory]::CreateDirectory((Join-Path $Project 'Plugins/AvidScript')) | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $Project 'Plugins/AvidScript/user.txt'), 'owned')
        $Rejected = $false
        try {
            Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Plan | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'unmanaged_target'
        }
        if (-not $Rejected) {
            throw 'unmanaged existing plugin was accepted.'
        }
    }

    Invoke-PluginReleaseContract 'project install lock timeout' {
        $Package = New-PluginReleaseFixture 'LockTimeout' '0.1.0'
        $Project = New-PluginReleaseProject 'LockTimeoutProject'
        Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Apply | Out-Null
        $LockPath = Join-Path $Project 'Plugins/.avidscript-install.lock'
        $HeldLock = [System.IO.File]::Open(
            $LockPath,
            [System.IO.FileMode]::OpenOrCreate,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
        try {
            $Rejected = $false
            try {
                Invoke-AvidScriptPluginInstallTransaction `
                    $Package.Root $Project Apply -LockTimeoutSeconds 1 | Out-Null
            }
            catch {
                $Rejected = [string]$_.Exception.Data['category'] -ceq 'lock_timeout'
            }
            if (-not $Rejected) {
                throw 'concurrent installer lock was ignored.'
            }
        }
        finally {
            $HeldLock.Dispose()
        }
    }

    Invoke-PluginReleaseContract 'post-commit cleanup warning' {
        $Old = New-PluginReleaseFixture 'CleanupOld' '0.1.0' 'old'
        $New = New-PluginReleaseFixture 'CleanupNew' '0.2.0' 'new'
        $Project = New-PluginReleaseProject 'CleanupProject'
        Invoke-AvidScriptPluginInstallTransaction $Old.Root $Project Apply | Out-Null
        $Upgrade = Invoke-AvidScriptPluginInstallTransaction `
            $New.Root $Project Apply -FaultPoint BackupCleanup
        $Backups = @(Get-ChildItem -LiteralPath (Join-Path $Project 'Plugins') -Directory -Force |
            Where-Object { $_.Name.StartsWith('.AvidScript.backup.', [System.StringComparison]::Ordinal) })
        $Verify = Invoke-AvidScriptPluginInstallTransaction $New.Root $Project Verify
        if ($Upgrade.warnings.Count -ne 1 -or $Backups.Count -ne 1 -or
            $Verify.result -cne 'passed') {
            throw 'post-commit cleanup failure rolled back or hid recovery material.'
        }
    }

    Invoke-PluginReleaseContract 'public CLI and privacy boundary' {
        $PublisherParameters = @(Get-ScriptParameterNames $PublisherPath)
        $InstallerParameters = @(Get-ScriptParameterNames $InstallerPath)
        $ExpectedPublisher = @('Channel', 'Commit', 'OutputRoot', 'PluginRoot', 'Version')
        $ExpectedInstaller = @(
            'AllowDowngrade', 'LockTimeoutSeconds', 'Mode', 'PackageRoot',
            'ProjectRoot', 'ReportPath')
        if ([string]::Join('|', $PublisherParameters) -cne
            [string]::Join('|', $ExpectedPublisher) -or
            [string]::Join('|', $InstallerParameters) -cne
            [string]::Join('|', $ExpectedInstaller)) {
            throw 'release CLI parameter schema drifted.'
        }
        $Publisher = Get-Content -LiteralPath $PublisherPath -Raw
        $ReleaseFiles = Get-Content -LiteralPath (
            Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptSourceRelease.files.json') -Raw
        $ReleaseReadme = Get-Content -LiteralPath (
            Join-Path $BuildRoot 'ReleaseEngineering/AvidScriptReleaseReadme.md') -Raw
        foreach ($Token in @(
                'git', 'archive', 'Assert-AvidScriptPluginReleasePrivacy',
                'Assert-AvidScriptPluginReleaseBinaryPrivacy',
                'AvidScriptSourceRelease.files.json')) {
            if (-not $Publisher.Contains($Token)) {
                throw "publisher privacy or provenance token is missing: $Token"
            }
        }
        foreach ($Token in @(
                'wasmtime-win64', 'wasmtime-android-arm64',
                'generated-type-package', 'module-release-package',
                'Source/ThirdParty/WAMR/upstream/tests', 'Tools/*.Tests')) {
            if (-not $ReleaseFiles.Contains($Token)) {
                throw "release allowlist token is missing: $Token"
            }
        }
        if (-not $ReleaseReadme.Contains(
                '.\payload\AvidScript\Build\InstallAvidScriptPluginRelease.ps1')) {
            throw 'release README does not invoke the installer from package root.'
        }
    }
}
finally {
    $ResolvedRoot = [System.IO.Path]::GetFullPath($Root)
    $AllowedRoot = [System.IO.Path]::GetFullPath('C:\tmp\AvidScript\P65Contracts').TrimEnd('\') + '\'
    if ($ResolvedRoot.StartsWith($AllowedRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $ResolvedRoot)) {
        Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force
    }
}

if ($Passed -ne $Total) {
    throw "AvidScript plugin release contracts: $Passed/$Total passed"
}
Write-Output "AvidScript plugin release contracts: $Passed/$Total passed"
