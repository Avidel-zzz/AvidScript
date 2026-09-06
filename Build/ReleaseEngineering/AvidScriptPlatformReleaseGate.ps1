Set-StrictMode -Version Latest

$script:AvidScriptPlatformReleaseGateRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:AvidScriptPlatformReleaseGateBuildRoot = Split-Path -Parent $script:AvidScriptPlatformReleaseGateRoot
$script:AvidScriptPlatformReleaseGatePluginRoot = Split-Path -Parent $script:AvidScriptPlatformReleaseGateBuildRoot
$script:AvidScriptPlatformReleaseGateUtf8 = [System.Text.UTF8Encoding]::new($false)

. (Join-Path $script:AvidScriptPlatformReleaseGateRoot 'AvidScriptPluginReleasePackage.ps1')
. (Join-Path $script:AvidScriptPlatformReleaseGateBuildRoot 'Android/AvidScriptAndroidProcess.ps1')
. (Join-Path $script:AvidScriptPlatformReleaseGateBuildRoot 'AvidScriptGeneratedTypeCookPackage.ps1')

function Throw-AvidScriptPlatformReleaseGateError {
    param(
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $Exception = [System.InvalidOperationException]::new($Message)
    $Exception.Data['category'] = $Category
    throw $Exception
}

function Get-AvidScriptPlatformReleaseOptionalProperty {
    param(
        [AllowNull()]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        $Default = ''
    )

    if ($null -ne $Value -and $Value.PSObject.Properties.Name -ccontains $Name) {
        return $Value.$Name
    }
    return $Default
}

function New-AvidScriptPlatformReleaseLayer {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [Parameter(Mandatory = $true)][string]$Platform,
        [Parameter(Mandatory = $true)]
        [ValidateSet('passed', 'failed', 'blocked', 'not_run')]
        [string]$Status,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message,
        [hashtable]$Details = @{}
    )

    return [pscustomobject][ordered]@{
        id = $Id
        platform = $Platform
        status = $Status
        code = $Code
        message = $Message
        details = [pscustomobject]$Details
    }
}

function Test-AvidScriptPlatformReleasePathUnderRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $FullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    return $FullPath.Equals($FullRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
        $FullPath.StartsWith(
            $FullRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)
}

function Resolve-AvidScriptPlatformReleaseProjectPath {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [AllowEmptyString()][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Label,
        [ValidateSet('Any', 'Leaf')][string]$PathType = 'Any',
        [switch]$Optional
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        if ($Optional) { return '' }
        Throw-AvidScriptPlatformReleaseGateError 'plan_invalid' "$Label is required."
    }
    if ([System.IO.Path]::IsPathRooted($RelativePath)) {
        Throw-AvidScriptPlatformReleaseGateError 'plan_invalid' "$Label must be relative to the project root."
    }
    $Normalized = Normalize-AvidScriptPluginReleaseRelativePath $RelativePath $Label
    $Resolved = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $Normalized))
    if (-not (Test-AvidScriptPlatformReleasePathUnderRoot $Resolved $ProjectRoot)) {
        Throw-AvidScriptPlatformReleaseGateError 'plan_invalid' "$Label escaped the project root."
    }
    if ($PathType -ceq 'Leaf' -and -not (Test-Path -LiteralPath $Resolved -PathType Leaf)) {
        Throw-AvidScriptPlatformReleaseGateError 'plan_invalid' "$Label is missing: $Normalized"
    }
    return $Resolved
}

function Assert-AvidScriptPlatformReleaseEnabledPlan {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][string]$Platform
    )

    if (-not [bool]$Plan.enabled) { return }
    $Required = if ($Platform -ceq 'Win64') {
        @('configuration', 'source_path', 'csharp_project_path', 'module_id',
            'artifact_stem', 'output_root', 'archive_root')
    }
    else {
        @('configuration', 'archive_root')
    }
    foreach ($Name in $Required) {
        if ($Plan.PSObject.Properties.Name -cnotcontains $Name -or
            [string]::IsNullOrWhiteSpace([string]$Plan.$Name)) {
            Throw-AvidScriptPlatformReleaseGateError `
                'plan_invalid' `
                "$Platform execution plan is missing '$Name'."
        }
    }
    if ($Platform -ceq 'Android' -and [bool]$Plan.device.enabled) {
        foreach ($Name in @(
                'scenario_id', 'module_id', 'map', 'event_ids', 'expected_package_id')) {
            if ($Plan.device.PSObject.Properties.Name -cnotcontains $Name -or
                [string]::IsNullOrWhiteSpace([string]$Plan.device.$Name)) {
                Throw-AvidScriptPlatformReleaseGateError `
                    'plan_invalid' `
                    "Android device plan is missing '$Name'."
            }
        }
    }
}

function Get-AvidScriptPlatformReleasePlan {
    param([AllowEmptyString()][string]$PlanPath)

    if ([string]::IsNullOrWhiteSpace($PlanPath)) {
        return [pscustomobject][ordered]@{
            schema_version = 1
            win64 = [pscustomobject]@{ enabled = $false }
            android = [pscustomobject]@{
                enabled = $false
                device = [pscustomobject]@{ enabled = $false }
            }
        }
    }
    $Resolved = (Resolve-Path -LiteralPath $PlanPath).Path
    $Plan = Read-AvidScriptPluginReleaseJsonObject $Resolved 'platform release gate plan'
    Assert-AvidScriptPluginReleaseJsonSchema `
        -JsonPath $Resolved `
        -SchemaPath (Join-Path $script:AvidScriptPlatformReleaseGateRoot 'AvidScriptPlatformReleaseGatePlan.schema.json') `
        -Label 'platform release gate plan'
    Assert-AvidScriptPlatformReleaseEnabledPlan $Plan.win64 'Win64'
    Assert-AvidScriptPlatformReleaseEnabledPlan $Plan.android 'Android'
    return $Plan
}

function ConvertFrom-AvidScriptPlatformReleaseChildJson {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Lines = @($Text -split '\r?\n' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($Lines.Count -ne 1) {
        Throw-AvidScriptPlatformReleaseGateError 'child_output_invalid' "$Label did not emit exactly one JSON line."
    }
    try {
        return $Lines[0] | ConvertFrom-Json -Depth 100 -DateKind String
    }
    catch {
        Throw-AvidScriptPlatformReleaseGateError 'child_output_invalid' "$Label emitted invalid JSON."
    }
}

function Invoke-AvidScriptPlatformReleaseScriptJson {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [ValidateRange(1, 7200)][int]$TimeoutSeconds = 60
    )

    $PowerShellPath = Join-Path $PSHOME 'pwsh.exe'
    $Process = Invoke-AvidScriptAndroidProcess `
        -Executable $PowerShellPath `
        -Arguments (@('-NoLogo', '-NoProfile', '-NonInteractive', '-File', $ScriptPath) + $Arguments) `
        -WorkingDirectory $WorkingDirectory `
        -TimeoutSeconds $TimeoutSeconds
    $Payload = ConvertFrom-AvidScriptPlatformReleaseChildJson $Process.stdout 'platform release child'
    return [pscustomobject]@{
        ExitCode = [int]$Process.exit_code
        Payload = $Payload
        Stderr = [string]$Process.stderr
        ElapsedMs = [double]$Process.elapsed_ms
    }
}

function Invoke-AvidScriptPlatformReleaseStep {
    param(
        [Parameter(Mandatory = $true)][string]$Step,
        [Parameter(Mandatory = $true)]$Input,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [ValidateRange(1, 7200)][int]$TimeoutSeconds
    )

    $InputRoot = [System.IO.Path]::GetFullPath('C:\tmp\AvidScript\P65PlatformGateSteps')
    [System.IO.Directory]::CreateDirectory($InputRoot) | Out-Null
    $InputPath = Join-Path $InputRoot ("$PID-$([guid]::NewGuid().ToString('N')).json")
    try {
        Write-AvidScriptPluginReleaseJson $InputPath $Input
        return Invoke-AvidScriptPlatformReleaseScriptJson `
            -ScriptPath (Join-Path $script:AvidScriptPlatformReleaseGateRoot 'AvidScriptPlatformReleaseGateStep.ps1') `
            -Arguments @('-Step', $Step, '-InputPath', $InputPath) `
            -WorkingDirectory $WorkingDirectory `
            -TimeoutSeconds $TimeoutSeconds
    }
    finally {
        if ((Test-AvidScriptPlatformReleasePathUnderRoot $InputPath $InputRoot) -and
            (Test-Path -LiteralPath $InputPath -PathType Leaf)) {
            Remove-Item -LiteralPath $InputPath -Force
        }
    }
}

function Get-AvidScriptPlatformReleaseCanonicalTextSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Text = [System.IO.File]::ReadAllText($Path)
    $Canonical = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    return Get-AvidScriptPluginReleaseBytesSha256 `
        ([System.Text.UTF8Encoding]::new($false).GetBytes($Canonical))
}

function Get-AvidScriptPlatformReleasePackageLayer {
    param([Parameter(Mandatory = $true)][string]$PackageRoot)

    $Package = Resolve-AvidScriptPluginReleasePackage $PackageRoot
    $ArtifactContracts = [System.Collections.Generic.List[object]]::new()
    foreach ($Contract in @($Package.Manifest.artifact_contracts)) {
        $PayloadPath = Normalize-AvidScriptPluginReleaseRelativePath `
            ([string]$Contract.producer_path) `
            'artifact contract producer path'
        if (-not $PayloadPath.StartsWith('AvidScript/Build/', [System.StringComparison]::Ordinal)) {
            Throw-AvidScriptPlatformReleaseGateError `
                'validator_identity_mismatch' `
                "Artifact producer is outside AvidScript/Build: $($Contract.id)"
        }
        $LocalRelative = $PayloadPath.Substring('AvidScript/'.Length)
        $LocalPath = Join-Path $script:AvidScriptPlatformReleaseGatePluginRoot $LocalRelative
        $PackageProducerPath = Join-Path $Package.PayloadRoot $PayloadPath
        if (-not (Test-Path -LiteralPath $LocalPath -PathType Leaf) -or
            (Get-AvidScriptPlatformReleaseCanonicalTextSha256 $LocalPath) -cne
            (Get-AvidScriptPlatformReleaseCanonicalTextSha256 $PackageProducerPath)) {
            Throw-AvidScriptPlatformReleaseGateError `
                'validator_identity_mismatch' `
                "The running Gate does not match release artifact producer '$($Contract.id)'."
        }
        $ArtifactContracts.Add([pscustomobject][ordered]@{
                id = [string]$Contract.id
                sha256 = [string]$Contract.producer_sha256
            })
    }
    $Dependencies = @($Package.Manifest.dependencies | ForEach-Object {
            [pscustomobject][ordered]@{
                id = [string]$_.id
                version = [string]$_.version
                sha256 = [string]$_.identity_sha256
            }
        })
    return [pscustomobject]@{
        Package = $Package
        Layer = New-AvidScriptPlatformReleaseLayer `
            'release-package' 'shared' 'passed' 'release_verified' `
            'Release manifest, inventory, dependencies and artifact producers are verified.' `
            @{
                release_id = [string]$Package.Manifest.release_id
                dependencies = $Dependencies
                artifact_contracts = @($ArtifactContracts)
            }
    }
}

function Get-AvidScriptPlatformReleaseProjectArtifacts {
    param([Parameter(Mandatory = $true)][string]$ProjectRoot)

    $TargetPluginRoot = Join-Path $ProjectRoot 'Plugins/AvidScript'
    $GeneratedPackagePath = Join-Path `
        $TargetPluginRoot `
        'Source/AvidScriptGenerated/AvidScriptGeneratedPackage.json'
    $ModulesRoot = Join-Path $ProjectRoot 'Content/AvidScript/Modules'
    $CatalogPath = Join-Path $ModulesRoot 'catalog.json'
    $GeneratedExists = Test-Path -LiteralPath $GeneratedPackagePath -PathType Leaf
    $CatalogExists = Test-Path -LiteralPath $CatalogPath -PathType Leaf
    if (-not $GeneratedExists -and -not $CatalogExists) {
        return [pscustomobject]@{
            Layer = New-AvidScriptPlatformReleaseLayer `
                'project-artifacts' 'shared' 'not_run' 'project_artifacts_absent' `
                'The target project has not generated AvidScript artifacts.'
            GeneratedTypePackageId = ''
            RuntimePackageId = ''
            CatalogSha256 = ''
            Variants = @()
        }
    }
    if (-not $GeneratedExists -or -not $CatalogExists) {
        Throw-AvidScriptPlatformReleaseGateError `
            'project_artifact_incomplete' `
            'Generated Type metadata and the module catalog must exist together.'
    }

    $Descriptor = Read-AvidScriptPluginReleaseJsonObject `
        $GeneratedPackagePath `
        'Generated Type package descriptor'
    Assert-AvidScriptPluginReleaseObjectShape `
        -Value $Descriptor `
        -Required @(
            'schema_version', 'package_id', 'module_name', 'runtime_module_id',
            'generation_key_sha256', 'type_manifest', 'runtime_manifest',
            'runtime_package_id') `
        -Optional @('execution_backend', 'reload') `
        -Label 'Generated Type package descriptor'
    if ([int]$Descriptor.schema_version -ne 1) {
        Throw-AvidScriptPlatformReleaseGateError `
            'project_artifact_invalid' `
            'Generated Type package schema is unsupported.'
    }
    foreach ($Identity in @(
            [string]$Descriptor.package_id,
            [string]$Descriptor.generation_key_sha256,
            [string]$Descriptor.runtime_package_id,
            [string]$Descriptor.type_manifest.sha256,
            [string]$Descriptor.runtime_manifest.sha256)) {
        Assert-AvidScriptPluginReleaseSha256 $Identity 'Generated Type identity'
    }
    $TypeManifestPath = Resolve-AvidScriptCookPackageArtifactPath `
        -ManifestPath $GeneratedPackagePath `
        -ArtifactPath ([string]$Descriptor.type_manifest.file) `
        -ProjectRoot $ProjectRoot
    $RuntimeManifestPath = Resolve-AvidScriptCookPackageArtifactPath `
        -ManifestPath $GeneratedPackagePath `
        -ArtifactPath ([string]$Descriptor.runtime_manifest.file) `
        -ProjectRoot $ProjectRoot
    Assert-AvidScriptCookPackageFileHash `
        $TypeManifestPath ([string]$Descriptor.type_manifest.sha256) 'generated type manifest'
    Assert-AvidScriptCookPackageFileHash `
        $RuntimeManifestPath ([string]$Descriptor.runtime_manifest.sha256) 'generated Runtime manifest'
    $Catalog = Read-AvidScriptModuleReleaseCatalog `
        -CatalogPath $CatalogPath `
        -OutputRoot $ModulesRoot
    $Variants = @(
        foreach ($Module in @($Catalog.modules)) {
            foreach ($Variant in @($Module.variants)) {
                [pscustomobject][ordered]@{
                    module_id = [string]$Module.module_id
                    platform = [string]$Variant.platform
                    architecture = [string]$Variant.architecture
                    configuration = [string]$Variant.configuration
                    backend = [string]$Variant.backend
                    package_id = [string]$Variant.package_id
                    descriptor_sha256 = [string]$Variant.descriptor_sha256
                }
            }
        })
    $RuntimeMatches = @($Variants | Where-Object {
            [string]$_.module_id -ceq [string]$Descriptor.runtime_module_id -and
            [string]$_.package_id -ceq [string]$Descriptor.runtime_package_id
        })
    if ($RuntimeMatches.Count -ne 1) {
        Throw-AvidScriptPlatformReleaseGateError `
            'project_artifact_invalid' `
            'Generated Type runtime package identity is not uniquely present in the module catalog.'
    }
    $CatalogSha256 = Get-AvidScriptPluginReleaseSha256 $CatalogPath
    return [pscustomobject]@{
        Layer = New-AvidScriptPlatformReleaseLayer `
            'project-artifacts' 'shared' 'passed' 'project_artifacts_verified' `
            'Generated Type and module package identities are verified.' `
            @{
                generated_type_package_id = [string]$Descriptor.package_id
                runtime_package_id = [string]$Descriptor.runtime_package_id
                module_catalog_sha256 = $CatalogSha256
                module_variant_count = $Variants.Count
            }
        GeneratedTypePackageId = [string]$Descriptor.package_id
        RuntimePackageId = [string]$Descriptor.runtime_package_id
        CatalogSha256 = $CatalogSha256
        Variants = $Variants
    }
}

function Resolve-AvidScriptPlatformReleaseDotNet {
    param([Parameter(Mandatory = $true)][string]$DotNetPath)

    if (Test-Path -LiteralPath $DotNetPath -PathType Leaf) {
        return (Resolve-Path -LiteralPath $DotNetPath).Path
    }
    $Command = Get-Command $DotNetPath -ErrorAction SilentlyContinue
    if ($null -eq $Command) {
        Throw-AvidScriptPlatformReleaseGateError 'dotnet_missing' 'The requested dotnet executable is unavailable.'
    }
    return $Command.Source
}

function Invoke-AvidScriptPlatformReleaseWin64 {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][string]$Mode,
        [Parameter(Mandatory = $true)][string]$RunId,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$EngineRoot,
        [Parameter(Mandatory = $true)][string]$DotNetPath,
        [ValidateRange(60, 7200)][int]$TimeoutSeconds
    )

    if ($Mode -cne 'Execute') {
        return New-AvidScriptPlatformReleaseLayer `
            'win64-shipping' 'Win64' 'not_run' 'inspect_only' `
            'Win64 BuildCookRun is disabled in Inspect mode.'
    }
    if (-not [bool]$Plan.enabled) {
        return New-AvidScriptPlatformReleaseLayer `
            'win64-shipping' 'Win64' 'not_run' 'plan_disabled' `
            'Win64 BuildCookRun is disabled by the execution plan.'
    }
    $Configuration = [string]$Plan.configuration
    $OutputBase = Resolve-AvidScriptPlatformReleaseProjectPath `
        $ProjectRoot ([string]$Plan.output_root) 'Win64 output root'
    $ArchiveBase = Resolve-AvidScriptPlatformReleaseProjectPath `
        $ProjectRoot ([string]$Plan.archive_root) 'Win64 archive root'
    foreach ($OwnedRoot in @($OutputBase, $ArchiveBase)) {
        $RequiredRoot = Join-Path $ProjectRoot 'Saved/AvidScript/ReleaseGate'
        if (-not (Test-AvidScriptPlatformReleasePathUnderRoot $OwnedRoot $RequiredRoot)) {
            Throw-AvidScriptPlatformReleaseGateError `
                'plan_invalid' `
                'Win64 output and archive roots must remain under Saved/AvidScript/ReleaseGate.'
        }
    }
    $Input = [pscustomobject][ordered]@{
        source_path = Resolve-AvidScriptPlatformReleaseProjectPath `
            $ProjectRoot ([string]$Plan.source_path) 'Win64 source path' Leaf
        csharp_project_path = Resolve-AvidScriptPlatformReleaseProjectPath `
            $ProjectRoot ([string]$Plan.csharp_project_path) 'Win64 C# project path' Leaf
        module_id = [string]$Plan.module_id
        artifact_stem = [string]$Plan.artifact_stem
        output_root = Join-Path $OutputBase $RunId
        dotnet_path = Resolve-AvidScriptPlatformReleaseDotNet $DotNetPath
        binding_package_path = Resolve-AvidScriptPlatformReleaseProjectPath `
            $ProjectRoot ([string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'binding_package_path')) `
            'Win64 binding package path' Leaf -Optional
        runtime_binding_package_path = Resolve-AvidScriptPlatformReleaseProjectPath `
            $ProjectRoot ([string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'runtime_binding_package_path')) `
            'Win64 Runtime binding package path' Leaf -Optional
        generated_type_manifest_path = Resolve-AvidScriptPlatformReleaseProjectPath `
            $ProjectRoot ([string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'generated_type_manifest_path')) `
            'Win64 Generated Type manifest path' Leaf -Optional
        configuration = $Configuration
        archive_root = Join-Path $ArchiveBase $RunId
        packaged_oracle_mode = [string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'packaged_oracle_mode' 'None')
        cook_maps = @((Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'cook_maps' @()))
        enable_plugins = @((Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'enable_plugins' @()))
        engine_root = $EngineRoot
    }
    $Child = Invoke-AvidScriptPlatformReleaseStep `
        'Win64Build' $Input $ProjectRoot $TimeoutSeconds
    if ($Child.ExitCode -ne 0 -or [string]$Child.Payload.status -cne 'ok' -or
        [string]$Child.Payload.result -cne 'avidscript_build_cook_run_succeeded') {
        return New-AvidScriptPlatformReleaseLayer `
            'win64-shipping' 'Win64' 'failed' 'win64_build_failed' `
            ([string](Get-AvidScriptPlatformReleaseOptionalProperty $Child.Payload 'message' 'Win64 BuildCookRun failed.')) `
            @{ configuration = $Configuration; elapsed_ms = [long]$Child.ElapsedMs }
    }
    return New-AvidScriptPlatformReleaseLayer `
        'win64-shipping' 'Win64' 'passed' 'win64_build_passed' `
        'Win64 BuildCookRun and package receipt verification passed.' `
        @{
            configuration = $Configuration
            module_id = [string]$Child.Payload.release.module_id
            package_id = [string]$Child.Payload.release.package_id
            receipt_freshness = [string]$Child.Payload.receipt_freshness
            packaged_oracle_mode = [string]$Child.Payload.packaged_oracle_mode
            elapsed_ms = [long]$Child.ElapsedMs
        }
}

function Get-AvidScriptPlatformReleaseAndroidToolchain {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$EngineRoot,
        [Parameter(Mandatory = $true)][string]$ToolchainScriptPath
    )

    $Arguments = @('-EngineRoot', $EngineRoot)
    foreach ($Pair in @(
            @('-AndroidSdkRoot', [string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'sdk_root')),
            @('-NdkRoot', [string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'ndk_root')),
            @('-JavaHome', [string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'java_home')))) {
        if (-not [string]::IsNullOrWhiteSpace($Pair[1])) { $Arguments += $Pair }
    }
    $Child = Invoke-AvidScriptPlatformReleaseScriptJson `
        $ToolchainScriptPath $Arguments $ProjectRoot 60
    $BlockedChecks = @($Child.Payload.checks | Where-Object { [string]$_.status -cne 'ok' })
    if ($Child.ExitCode -eq 0 -and [bool]$Child.Payload.ready) {
        return [pscustomobject]@{
            Ready = $true
            Payload = $Child.Payload
            Layer = New-AvidScriptPlatformReleaseLayer `
                'android-toolchain' 'Android' 'passed' 'android_toolchain_ready' `
                'The controlled Android SDK, NDK, JDK and ADB toolchain is ready.' `
                @{
                    passed_checks = @($Child.Payload.checks).Count
                    total_checks = @($Child.Payload.checks).Count
                    requirements = $Child.Payload.requirements
                }
        }
    }
    if ($Child.ExitCode -eq 2 -and -not [bool]$Child.Payload.ready) {
        return [pscustomobject]@{
            Ready = $false
            Payload = $Child.Payload
            Layer = New-AvidScriptPlatformReleaseLayer `
                'android-toolchain' 'Android' 'blocked' 'android_toolchain_unavailable' `
                'The controlled Android toolchain is incomplete; upper Android layers were not executed.' `
                @{
                    passed_checks = @($Child.Payload.checks).Count - $BlockedChecks.Count
                    total_checks = @($Child.Payload.checks).Count
                    blocked_check_ids = @($BlockedChecks | ForEach-Object { [string]$_.id })
                }
        }
    }
    return [pscustomobject]@{
        Ready = $false
        Payload = $Child.Payload
        Layer = New-AvidScriptPlatformReleaseLayer `
            'android-toolchain' 'Android' 'failed' 'android_toolchain_probe_failed' `
            'The Android toolchain probe failed.' `
            @{ exit_code = $Child.ExitCode; elapsed_ms = [long]$Child.ElapsedMs }
    }
}

function Invoke-AvidScriptPlatformReleaseAndroidBuild {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)]$Toolchain,
        [Parameter(Mandatory = $true)][string]$Mode,
        [Parameter(Mandatory = $true)][string]$RunId,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$EngineRoot,
        [ValidateRange(60, 7200)][int]$TimeoutSeconds
    )

    if (-not $Toolchain.Ready) {
        return [pscustomobject]@{
            Build = New-AvidScriptPlatformReleaseLayer `
                'android-arm64' 'Android' 'not_run' 'toolchain_unavailable' `
                'Android arm64 UBT was not run because the toolchain is unavailable.'
            Package = New-AvidScriptPlatformReleaseLayer `
                'android-package' 'Android' 'not_run' 'toolchain_unavailable' `
                'Android package creation was not run because the toolchain is unavailable.'
            Payload = $null
        }
    }
    if ($Mode -cne 'Execute') {
        return [pscustomobject]@{
            Build = New-AvidScriptPlatformReleaseLayer `
                'android-arm64' 'Android' 'not_run' 'inspect_only' `
                'Android arm64 UBT is disabled in Inspect mode.'
            Package = New-AvidScriptPlatformReleaseLayer `
                'android-package' 'Android' 'not_run' 'inspect_only' `
                'Android package creation is disabled in Inspect mode.'
            Payload = $null
        }
    }
    if (-not [bool]$Plan.enabled) {
        return [pscustomobject]@{
            Build = New-AvidScriptPlatformReleaseLayer `
                'android-arm64' 'Android' 'not_run' 'plan_disabled' `
                'Android arm64 UBT is disabled by the execution plan.'
            Package = New-AvidScriptPlatformReleaseLayer `
                'android-package' 'Android' 'not_run' 'plan_disabled' `
                'Android package creation is disabled by the execution plan.'
            Payload = $null
        }
    }
    $ArchiveBase = Resolve-AvidScriptPlatformReleaseProjectPath `
        $ProjectRoot ([string]$Plan.archive_root) 'Android archive root'
    $RequiredRoot = Join-Path $ProjectRoot 'Saved/AvidScript/ReleaseGate'
    if (-not (Test-AvidScriptPlatformReleasePathUnderRoot $ArchiveBase $RequiredRoot)) {
        Throw-AvidScriptPlatformReleaseGateError `
            'plan_invalid' `
            'Android archive root must remain under Saved/AvidScript/ReleaseGate.'
    }
    $Input = [pscustomobject][ordered]@{
        configuration = [string]$Plan.configuration
        archive_root = Join-Path $ArchiveBase $RunId
        engine_root = $EngineRoot
        sdk_root = [string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'sdk_root')
        ndk_root = [string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'ndk_root')
        java_home = [string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan 'java_home')
        timeout_seconds = $TimeoutSeconds
    }
    $Child = Invoke-AvidScriptPlatformReleaseStep `
        'AndroidBuild' $Input $ProjectRoot ($TimeoutSeconds + 60)
    if ($Child.ExitCode -ne 0 -or [string]$Child.Payload.status -cne 'ok' -or
        [string]$Child.Payload.result -cne 'avidscript_android_build_cook_run_passed') {
        return [pscustomobject]@{
            Build = New-AvidScriptPlatformReleaseLayer `
                'android-arm64' 'Android' 'failed' 'android_build_failed' `
                ([string](Get-AvidScriptPlatformReleaseOptionalProperty $Child.Payload 'message' 'Android BuildCookRun failed.')) `
                @{ configuration = [string]$Plan.configuration; elapsed_ms = [long]$Child.ElapsedMs }
            Package = New-AvidScriptPlatformReleaseLayer `
                'android-package' 'Android' 'not_run' 'android_build_failed' `
                'Android package verification was not reached.'
            Payload = $Child.Payload
        }
    }
    $Apks = @($Child.Payload.apks | ForEach-Object {
            [pscustomobject][ordered]@{
                sha256 = [string]$_.sha256
                architecture = [string]$_.architecture
                native_library_count = [int]$_.native_library_count
            }
        })
    return [pscustomobject]@{
        Build = New-AvidScriptPlatformReleaseLayer `
            'android-arm64' 'Android' 'passed' 'android_arm64_passed' `
            'Android arm64 BuildCookRun and target receipt validation passed.' `
            @{
                configuration = [string]$Plan.configuration
                receipt_freshness = [string]$Child.Payload.receipt_freshness
                elapsed_ms = [long]$Child.ElapsedMs
            }
        Package = New-AvidScriptPlatformReleaseLayer `
            'android-package' 'Android' 'passed' 'android_package_passed' `
            'The Android archive contains verified arm64 APK content.' `
            @{ apks = $Apks; obb_count = @($Child.Payload.obb_files).Count }
        Payload = $Child.Payload
    }
}

function Invoke-AvidScriptPlatformReleaseAndroidDevice {
    param(
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)]$Toolchain,
        [Parameter(Mandatory = $true)]$AndroidBuild,
        [Parameter(Mandatory = $true)][string]$Mode,
        [Parameter(Mandatory = $true)][string]$RunId,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [ValidateRange(10, 600)][int]$TimeoutSeconds = 90
    )

    if (-not $Toolchain.Ready) {
        return New-AvidScriptPlatformReleaseLayer `
            'android-device' 'Android' 'not_run' 'toolchain_unavailable' `
            'Android device validation was not run because ADB is unavailable.'
    }
    if ($Mode -cne 'Execute' -or -not [bool]$Plan.device.enabled) {
        return New-AvidScriptPlatformReleaseLayer `
            'android-device' 'Android' 'not_run' `
            $(if ($Mode -cne 'Execute') { 'inspect_only' } else { 'plan_disabled' }) `
            'Android device lifecycle validation was not requested.'
    }
    if ([string]$AndroidBuild.Package.status -cne 'passed') {
        return New-AvidScriptPlatformReleaseLayer `
            'android-device' 'Android' 'not_run' 'android_package_unavailable' `
            'Android device validation requires a package from this Gate run.'
    }
    $Apks = @($AndroidBuild.Payload.apks)
    if ($Apks.Count -ne 1) {
        return New-AvidScriptPlatformReleaseLayer `
            'android-device' 'Android' 'not_run' 'apk_selection_required' `
            'Android device validation requires exactly one APK.'
    }
    $Input = [pscustomobject][ordered]@{
        mode = 'Run'
        configuration = [string]$Plan.configuration
        adb_path = [string]$Toolchain.Payload.toolchain.adb_path
        device_id = [string](Get-AvidScriptPlatformReleaseOptionalProperty $Plan.device 'device_id')
        apk_path = [string]$Apks[0].path
        obb_paths = @($AndroidBuild.Payload.obb_files | ForEach-Object { [string]$_.path })
        scenario_id = [string]$Plan.device.scenario_id
        module_id = [string]$Plan.device.module_id
        map = [string]$Plan.device.map
        event_ids = [string]$Plan.device.event_ids
        output_root = Join-Path $ProjectRoot "Saved/AvidScript/ReleaseGate/AndroidDevice/$RunId"
        timeout_seconds = $TimeoutSeconds
        expected_package_id = [string]$Plan.device.expected_package_id
    }
    $Child = Invoke-AvidScriptPlatformReleaseStep `
        'AndroidScenario' $Input $ProjectRoot ($TimeoutSeconds + 300)
    if ($Child.ExitCode -eq 0 -and [string]$Child.Payload.status -ceq 'ok' -and
        [string]$Child.Payload.result -ceq 'avidscript_android_scenario_passed') {
        return New-AvidScriptPlatformReleaseLayer `
            'android-device' 'Android' 'passed' 'android_device_passed' `
            'Android installation, launch and gameplay lifecycle validation passed.' `
            @{
                run_id = [string]$Child.Payload.run_id
                package_id = [string]$Child.Payload.package_id
                apk_sha256 = [string]$Child.Payload.apk_sha256
            }
    }
    if ($Child.ExitCode -eq 2 -and [string]$Child.Payload.status -ceq 'not_run') {
        return New-AvidScriptPlatformReleaseLayer `
            'android-device' 'Android' 'not_run' `
            ([string](Get-AvidScriptPlatformReleaseOptionalProperty $Child.Payload 'reason' 'device_unavailable')) `
            'Android device lifecycle validation could not run.'
    }
    return New-AvidScriptPlatformReleaseLayer `
        'android-device' 'Android' 'failed' 'android_device_failed' `
        ([string](Get-AvidScriptPlatformReleaseOptionalProperty $Child.Payload 'message' 'Android device lifecycle validation failed.')) `
        @{ exit_code = $Child.ExitCode; elapsed_ms = [long]$Child.ElapsedMs }
}

function Write-AvidScriptPlatformReleaseGateReport {
    param(
        [Parameter(Mandatory = $true)]$Report,
        [Parameter(Mandatory = $true)][string]$ReportPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot
    )

    $Resolved = [System.IO.Path]::GetFullPath($ReportPath)
    $RequiredRoot = Join-Path $ProjectRoot 'Saved/AvidScript/ReleaseGate'
    if (-not (Test-AvidScriptPlatformReleasePathUnderRoot $Resolved $RequiredRoot) -or
        [System.IO.Path]::GetExtension($Resolved) -cne '.json') {
        Throw-AvidScriptPlatformReleaseGateError `
            'report_path_invalid' `
            'ReportPath must be a JSON file under Saved/AvidScript/ReleaseGate.'
    }
    if (Test-Path -LiteralPath $Resolved) {
        Throw-AvidScriptPlatformReleaseGateError 'report_exists' 'ReportPath must be a new file.'
    }
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $Resolved)) | Out-Null
    $Temporary = "$Resolved.tmp.$PID.$([guid]::NewGuid().ToString('N'))"
    try {
        Write-AvidScriptPluginReleaseJson $Temporary $Report
        Assert-AvidScriptPluginReleaseJsonSchema `
            -JsonPath $Temporary `
            -SchemaPath (Join-Path $script:AvidScriptPlatformReleaseGateRoot 'AvidScriptPlatformReleaseGateReport.schema.json') `
            -Label 'platform release gate report'
        [System.IO.File]::Move($Temporary, $Resolved)
    }
    finally {
        if (Test-Path -LiteralPath $Temporary -PathType Leaf) {
            Remove-Item -LiteralPath $Temporary -Force
        }
    }
}

function Invoke-AvidScriptPlatformReleaseGate {
    param(
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [ValidateSet('Inspect', 'Execute')][string]$Mode = 'Inspect',
        [string]$PlanPath = '',
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$EngineRoot,
        [Parameter(Mandatory = $true)][string]$DotNetPath,
        [string]$ReportPath = '',
        [ValidateRange(60, 7200)][int]$TimeoutSeconds = 3600,
        [string]$ToolchainScriptPath = ''
    )

    $StartedAt = [DateTimeOffset]::UtcNow.ToString('o')
    $RunId = [guid]::NewGuid().ToString('N')
    $ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
    $EngineRoot = [System.IO.Path]::GetFullPath($EngineRoot)
    if (-not (Test-Path -LiteralPath $ProjectRoot -PathType Container) -or
        @((Get-ChildItem -LiteralPath $ProjectRoot -Filter '*.uproject' -File)).Count -ne 1) {
        Throw-AvidScriptPlatformReleaseGateError `
            'project_invalid' `
            'ProjectRoot must contain exactly one Unreal project descriptor.'
    }
    if ([string]::IsNullOrWhiteSpace($ToolchainScriptPath)) {
        $ToolchainScriptPath = Join-Path $script:AvidScriptPlatformReleaseGateBuildRoot 'TestAvidScriptAndroidToolchain.ps1'
    }
    $Plan = Get-AvidScriptPlatformReleasePlan $PlanPath
    $ResolvedReportPath = if ([string]::IsNullOrWhiteSpace($ReportPath)) {
        Join-Path $ProjectRoot "Saved/AvidScript/ReleaseGate/Reports/$RunId.json"
    }
    else {
        [System.IO.Path]::GetFullPath($ReportPath)
    }
    $ReportRelative = [System.IO.Path]::GetRelativePath(
        $ProjectRoot,
        $ResolvedReportPath).Replace('\', '/')

    $Release = $null
    try {
        $Release = Get-AvidScriptPlatformReleasePackageLayer $PackageRoot
        $ReleaseLayer = $Release.Layer
    }
    catch {
        $ReleaseLayer = New-AvidScriptPlatformReleaseLayer `
            'release-package' 'shared' 'failed' 'release_verification_failed' `
            $_.Exception.Message `
            @{ category = [string]$_.Exception.Data['category'] }
    }

    if ($null -eq $Release) {
        $ProjectArtifacts = [pscustomobject]@{
            Layer = New-AvidScriptPlatformReleaseLayer `
                'project-artifacts' 'shared' 'not_run' 'release_verification_failed' `
                'Project artifact binding requires a verified release package.'
            GeneratedTypePackageId = ''
            RuntimePackageId = ''
            CatalogSha256 = ''
            Variants = @()
        }
        $Win64Layer = New-AvidScriptPlatformReleaseLayer `
            'win64-shipping' 'Win64' 'not_run' 'release_verification_failed' `
            'Win64 validation requires a verified release package.'
        $Toolchain = [pscustomobject]@{
            Ready = $false
            Layer = New-AvidScriptPlatformReleaseLayer `
                'android-toolchain' 'Android' 'not_run' 'release_verification_failed' `
                'Android validation requires a verified release package.'
        }
        $AndroidBuild = [pscustomobject]@{
            Build = New-AvidScriptPlatformReleaseLayer `
                'android-arm64' 'Android' 'not_run' 'release_verification_failed' `
                'Android arm64 validation requires a verified release package.'
            Package = New-AvidScriptPlatformReleaseLayer `
                'android-package' 'Android' 'not_run' 'release_verification_failed' `
                'Android package validation requires a verified release package.'
        }
        $DeviceLayer = New-AvidScriptPlatformReleaseLayer `
            'android-device' 'Android' 'not_run' 'release_verification_failed' `
            'Android device validation requires a verified release package.'
    }
    else {
        try {
            $ProjectArtifacts = Get-AvidScriptPlatformReleaseProjectArtifacts $ProjectRoot
        }
        catch {
            $ProjectArtifacts = [pscustomobject]@{
                Layer = New-AvidScriptPlatformReleaseLayer `
                    'project-artifacts' 'shared' 'failed' 'project_artifact_verification_failed' `
                    $_.Exception.Message `
                    @{ category = [string]$_.Exception.Data['category'] }
                GeneratedTypePackageId = ''
                RuntimePackageId = ''
                CatalogSha256 = ''
                Variants = @()
            }
        }
        $Win64Layer = Invoke-AvidScriptPlatformReleaseWin64 `
            $Plan.win64 $Mode $RunId $ProjectRoot $EngineRoot $DotNetPath $TimeoutSeconds
        $Toolchain = Get-AvidScriptPlatformReleaseAndroidToolchain `
            $Plan.android $ProjectRoot $EngineRoot $ToolchainScriptPath
        $AndroidBuild = Invoke-AvidScriptPlatformReleaseAndroidBuild `
            $Plan.android $Toolchain $Mode $RunId $ProjectRoot $EngineRoot $TimeoutSeconds
        $DeviceLayer = Invoke-AvidScriptPlatformReleaseAndroidDevice `
            $Plan.android $Toolchain $AndroidBuild $Mode $RunId $ProjectRoot 90
        if ($Mode -ceq 'Execute' -and
            ($Win64Layer.status -ceq 'passed' -or $AndroidBuild.Build.status -ceq 'passed')) {
            try {
                $ProjectArtifacts = Get-AvidScriptPlatformReleaseProjectArtifacts $ProjectRoot
            }
            catch {
                $ProjectArtifacts = [pscustomobject]@{
                    Layer = New-AvidScriptPlatformReleaseLayer `
                        'project-artifacts' 'shared' 'failed' 'project_artifact_verification_failed' `
                        $_.Exception.Message `
                        @{ category = [string]$_.Exception.Data['category'] }
                    GeneratedTypePackageId = ''
                    RuntimePackageId = ''
                    CatalogSha256 = ''
                    Variants = @()
                }
            }
        }
    }

    $ManualLayer = New-AvidScriptPlatformReleaseLayer `
        'shipping-manual' 'manual' 'not_run' 'manual_acceptance_required' `
        'Shipping UI, physical input and device experience require separate human acceptance.'
    $Layers = @(
        $ReleaseLayer,
        $ProjectArtifacts.Layer,
        $Win64Layer,
        $Toolchain.Layer,
        $AndroidBuild.Build,
        $AndroidBuild.Package,
        $DeviceLayer,
        $ManualLayer)
    $Summary = [ordered]@{
        passed = @($Layers | Where-Object { $_.status -ceq 'passed' }).Count
        failed = @($Layers | Where-Object { $_.status -ceq 'failed' }).Count
        blocked = @($Layers | Where-Object { $_.status -ceq 'blocked' }).Count
        not_run = @($Layers | Where-Object { $_.status -ceq 'not_run' }).Count
        total = 8
    }
    $Result = if ($Summary.failed -gt 0) {
        'failed'
    }
    elseif ($Summary.blocked -gt 0 -or $Summary.not_run -gt 0) {
        'partial'
    }
    else {
        'passed'
    }
    $Identities = [ordered]@{
        release_id = $(if ($null -eq $Release) { '' } else { [string]$Release.Package.Manifest.release_id })
        release_manifest_sha256 = $(if ($null -eq $Release) { '' } else { [string]$Release.Package.ManifestSha256 })
        source_commit = $(if ($null -eq $Release) { '' } else { [string]$Release.Package.Manifest.source.commit })
        source_tree = $(if ($null -eq $Release) { '' } else { [string]$Release.Package.Manifest.source.tree })
        generated_type_package_id = [string]$ProjectArtifacts.GeneratedTypePackageId
        runtime_package_id = [string]$ProjectArtifacts.RuntimePackageId
        module_catalog_sha256 = [string]$ProjectArtifacts.CatalogSha256
        module_variants = @($ProjectArtifacts.Variants)
    }
    $Report = [pscustomobject][ordered]@{
        schema_version = 1
        format = 'avidscript.platform.release-gate'
        run_id = $RunId
        mode = $Mode
        result = $Result
        report_file = $ReportRelative
        started_at_utc = $StartedAt
        completed_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
        identities = [pscustomobject]$Identities
        summary = [pscustomobject]$Summary
        layers = $Layers
    }
    Write-AvidScriptPlatformReleaseGateReport $Report $ResolvedReportPath $ProjectRoot
    return $Report
}
