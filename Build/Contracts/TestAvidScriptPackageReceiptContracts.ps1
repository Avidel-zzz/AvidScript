[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$PluginRoot = [System.IO.Path]::GetFullPath(
    (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$ValidatorPath = Join-Path $PluginRoot 'Build/TestAvidScriptPackageReceipt.ps1'
$PowerShellPath = (Get-Process -Id $PID).Path
$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    "AvidScriptPackageReceiptContracts_$PID`_$([Guid]::NewGuid().ToString('N'))")
$Utf8 = [System.Text.UTF8Encoding]::new($false)
$Passed = 0
$Total = 0
$Failures = [System.Collections.Generic.List[string]]::new()

function Assert-ContractCondition {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Write-FixtureJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Value
    )

    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    $Json = (($Value | ConvertTo-Json -Depth 64).Replace("`r`n", "`n")) + "`n"
    [System.IO.File]::WriteAllText($Path, $Json, $Utf8)
}

function Write-FixtureBytes {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][byte[]]$Bytes
    )

    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    [System.IO.File]::WriteAllBytes($Path, $Bytes)
}

function Get-FixtureSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        $Hasher = [System.Security.Cryptography.SHA256]::Create()
        try {
            return [System.BitConverter]::ToString($Hasher.ComputeHash($Stream)).Replace('-', '').ToLowerInvariant()
        }
        finally {
            $Hasher.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
    }
}

function New-PackageReceiptFixture {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [ValidateSet('Development', 'Shipping')][string]$Configuration = 'Development'
    )

    $ProjectRoot = Join-Path $FixtureRoot $Name
    $PluginRoot = Join-Path $ProjectRoot 'Plugins/AvidScript'
    $ModulesRoot = Join-Path $ProjectRoot 'Content/AvidScript/Modules'
    $ModuleId = 'fixture.module'
    $PackageId = 'a' * 64
    $PackageRelative = "$ModuleId/$PackageId"
    $PackageRoot = Join-Path $ModulesRoot $PackageRelative
    $ConfigurationValue = $Configuration.ToLowerInvariant()
    [void][System.IO.Directory]::CreateDirectory($PackageRoot)

    Write-FixtureJson -Path (Join-Path $PluginRoot 'AvidScript.uplugin') -Value ([ordered]@{
        FileVersion = 3
        FriendlyName = 'AvidScript Fixture'
    })

    $ArtifactDefinitions = [ordered]@{
        runtime_manifest = [pscustomobject]@{ File = 'runtime.avidscript.json'; Bytes = [byte[]](0x7b, 0x7d, 0x0a) }
        canonical_wasm = [pscustomobject]@{ File = 'module.wasm'; Bytes = [byte[]](0x00, 0x61, 0x73, 0x6d, 0x01) }
        precompiled = [pscustomobject]@{ File = 'module.wasmtime.cwasm'; Bytes = [byte[]](0x63, 0x77, 0x61, 0x73, 0x6d) }
        binding_manifest = [pscustomobject]@{ File = 'bindings/package.json'; Bytes = [byte[]](0x7b, 0x7d, 0x0a) }
        binding_descriptor = [pscustomobject]@{ File = 'bindings/bindings.json'; Bytes = [byte[]](0x7b, 0x7d, 0x0a) }
        debug_map = [pscustomobject]@{ File = 'diagnostics/debug-map.json'; Bytes = [byte[]](0x7b, 0x7d, 0x0a) }
    }
    $Artifacts = [ordered]@{}
    foreach ($Entry in $ArtifactDefinitions.GetEnumerator()) {
        $ArtifactPath = Join-Path $PackageRoot $Entry.Value.File
        Write-FixtureBytes -Path $ArtifactPath -Bytes $Entry.Value.Bytes
        $Artifacts[$Entry.Key] = [ordered]@{
            file = $Entry.Value.File
            sha256 = Get-FixtureSha256 -Path $ArtifactPath
        }
    }

    $DescriptorPath = Join-Path $PackageRoot 'package.json'
    Write-FixtureJson -Path $DescriptorPath -Value ([ordered]@{
        schema_version = 1
        package_id = $PackageId
        module_id = $ModuleId
        abi_version = 1
        platform = 'win64'
        configuration = $ConfigurationValue
        minimum_runtime_version = '0.1.0'
        execution = [ordered]@{
            backend = 'wasmtime'
            format = 'wasmtime_serialized_v1'
            policy = if ($Configuration -ceq 'Shipping') { 'require_precompiled' } else { 'prefer_precompiled' }
        }
        artifacts = $Artifacts
    })
    $DescriptorSha256 = Get-FixtureSha256 -Path $DescriptorPath
    $CatalogPath = Join-Path $ModulesRoot 'catalog.json'
    Write-FixtureJson -Path $CatalogPath -Value ([ordered]@{
        schema_version = 2
        modules = @([ordered]@{
            module_id = $ModuleId
            variants = @([ordered]@{
                platform = 'win64'
                architecture = 'x86_64'
                configuration = $ConfigurationValue
                backend = 'wasmtime'
                format = 'wasmtime_serialized_v1'
                package_id = $PackageId
                descriptor_file = "$PackageRelative/package.json"
                descriptor_sha256 = $DescriptorSha256
            })
        })
    })

    $GeneratedPackageId = 'b' * 64
    $GeneratedRoot = Join-Path $PluginRoot 'Content/AvidScriptGenerated'
    $TypeManifestRelative = "$GeneratedPackageId/type-manifest.json"
    $TypeManifestPath = Join-Path $GeneratedRoot $TypeManifestRelative
    Write-FixtureJson -Path $TypeManifestPath -Value ([ordered]@{
        schema_version = 1
        module_id = $ModuleId
        types = @()
    })
    Write-FixtureJson -Path (Join-Path $GeneratedRoot 'current.json') -Value ([ordered]@{
        schema_version = 2
        module_name = 'FixtureModule'
        module_id = $ModuleId
        package_id = $PackageId
        generation_key_sha256 = ('c' * 64)
        type_manifest = [ordered]@{
            file = $TypeManifestRelative
            sha256 = Get-FixtureSha256 -Path $TypeManifestPath
        }
        reload = [ordered]@{
            schema_version = 1
            classification = 'initial_install'
        }
    })

    $WasmtimeRoot = Join-Path $PluginRoot 'Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0-avidscript.1'
    $WasmtimeDllPath = Join-Path $WasmtimeRoot 'lib/wasmtime.dll'
    Write-FixtureBytes -Path $WasmtimeDllPath -Bytes ([byte[]](0x4d, 0x5a, 0x46, 0x49, 0x58, 0x54, 0x55, 0x52, 0x45))
    Write-FixtureBytes -Path (Join-Path $WasmtimeRoot 'LICENSE') -Bytes ([System.Text.Encoding]::UTF8.GetBytes("fixture license`n"))
    Write-FixtureJson `
        -Path (Join-Path $WasmtimeRoot '.avidscript-wasmtime-performance-managed.json') `
        -Value ([ordered]@{
            schema_version = 1
            toolchain_id = 'fixture-toolchain'
            dll_sha256 = Get-FixtureSha256 -Path $WasmtimeDllPath
        })

    $RuntimeDependencies = [System.Collections.Generic.List[object]]::new()
    $RuntimeDependencies.Add([ordered]@{ Path = '$(ProjectDir)/Plugins/AvidScript/AvidScript.uplugin'; Type = 'UFS' })
    $RuntimeDependencies.Add([ordered]@{ Path = '$(ProjectDir)/Content/AvidScript/Modules/catalog.json'; Type = 'UFS' })
    $RuntimeDependencies.Add([ordered]@{ Path = "`$(ProjectDir)/Content/AvidScript/Modules/$PackageRelative/package.json"; Type = 'UFS' })
    foreach ($Entry in $ArtifactDefinitions.GetEnumerator()) {
        $RuntimeDependencies.Add([ordered]@{
            Path = "`$(ProjectDir)/Content/AvidScript/Modules/$PackageRelative/$($Entry.Value.File)"
            Type = 'UFS'
        })
    }
    $RuntimeDependencies.Add([ordered]@{ Path = '$(PluginDir)/Content/AvidScriptGenerated/current.json'; Type = 'UFS' })
    $RuntimeDependencies.Add([ordered]@{ Path = "`$(PluginDir)/Content/AvidScriptGenerated/$TypeManifestRelative"; Type = 'UFS' })
    $RuntimeDependencies.Add([ordered]@{ Path = '$(ProjectDir)/Plugins/AvidScript/Binaries/Win64/wasmtime.dll'; Type = 'NonUFS' })
    $RuntimeDependencies.Add([ordered]@{ Path = '$(PluginDir)/Binaries/Win64/wasmtime.LICENSE.txt'; Type = 'NonUFS' })
    $RuntimeDependencies.Add([ordered]@{ Path = '$(EngineDir)/Content/Slate/Fonts/Roboto-Regular.ttf'; Type = 'UFS' })

    $ReceiptPath = Join-Path $ProjectRoot "Binaries/Win64/AvidFixture-$Configuration.target"
    Write-FixtureJson -Path $ReceiptPath -Value ([ordered]@{
        TargetName = 'AvidFixture'
        Platform = 'Win64'
        Configuration = $Configuration
        TargetType = 'Game'
        RuntimeDependencies = @($RuntimeDependencies)
    })
    return [pscustomobject]@{
        ProjectRoot = $ProjectRoot
        PluginRoot = $PluginRoot
        ReceiptPath = $ReceiptPath
        CatalogPath = $CatalogPath
        Configuration = $Configuration
        PackageRoot = $PackageRoot
        WasmtimeDllPath = $WasmtimeDllPath
    }
}

function Read-FixtureReceipt {
    param([Parameter(Mandatory = $true)][object]$Fixture)
    return Get-Content -LiteralPath $Fixture.ReceiptPath -Raw | ConvertFrom-Json -Depth 64
}

function Write-FixtureReceipt {
    param(
        [Parameter(Mandatory = $true)][object]$Fixture,
        [Parameter(Mandatory = $true)][object]$Receipt
    )
    Write-FixtureJson -Path $Fixture.ReceiptPath -Value $Receipt
}

function Invoke-ReceiptValidator {
    param(
        [Parameter(Mandatory = $true)][object]$Fixture,
        [string]$Configuration = $Fixture.Configuration
    )

    $Output = & $PowerShellPath `
        -NoProfile `
        -NonInteractive `
        -ExecutionPolicy Bypass `
        -File $ValidatorPath `
        -ReceiptPath $Fixture.ReceiptPath `
        -ProjectRoot $Fixture.ProjectRoot `
        -PluginRoot $Fixture.PluginRoot `
        -Configuration $Configuration 2>&1
    $ExitCode = $LASTEXITCODE
    $Text = @($Output | ForEach-Object { $_.ToString() }) -join [System.Environment]::NewLine
    try {
        $Summary = $Text | ConvertFrom-Json -Depth 64
    }
    catch {
        throw "Validator did not emit one JSON summary. Output: $Text"
    }
    return [pscustomobject]@{
        ExitCode = $ExitCode
        Summary = $Summary
        Raw = $Text
    }
}

function Invoke-ContractCase {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    ++$script:Total
    try {
        & $Body
        ++$script:Passed
    }
    catch {
        $script:Failures.Add("$Name`: $($_.Exception.Message)")
    }
}

function Assert-ValidatorRejected {
    param(
        [Parameter(Mandatory = $true)][object]$Fixture,
        [Parameter(Mandatory = $true)][string]$ErrorCode,
        [string]$Configuration = $Fixture.Configuration
    )

    $Result = Invoke-ReceiptValidator -Fixture $Fixture -Configuration $Configuration
    Assert-ContractCondition ($Result.ExitCode -ne 0) "Validator unexpectedly accepted fixture: $($Result.Raw)"
    Assert-ContractCondition ([string]$Result.Summary.status -ceq 'error') 'Rejected summary status is not error.'
    Assert-ContractCondition ([string]$Result.Summary.error_code -ceq $ErrorCode) `
        "Expected $ErrorCode, got '$($Result.Summary.error_code)': $($Result.Raw)"
}

try {
    [void][System.IO.Directory]::CreateDirectory($FixtureRoot)

    Invoke-ContractCase 'valid exact Development receipt' {
        $Fixture = New-PackageReceiptFixture -Name 'ValidDevelopment'
        $Result = Invoke-ReceiptValidator -Fixture $Fixture
        Assert-ContractCondition ($Result.ExitCode -eq 0) "Valid receipt was rejected: $($Result.Raw)"
        Assert-ContractCondition ([string]$Result.Summary.status -ceq 'ok') 'Success summary status is not ok.'
        Assert-ContractCondition ([int]$Result.Summary.expected_dependency_count -eq 13) 'Unexpected exact dependency count.'
        Assert-ContractCondition ([int]$Result.Summary.ufs_dependency_count -eq 11) 'Unexpected UFS dependency count.'
        Assert-ContractCondition ([int]$Result.Summary.non_ufs_dependency_count -eq 2) 'Unexpected NonUFS dependency count.'
    }

    Invoke-ContractCase 'foreign-platform catalog module is ignored' {
        $Fixture = New-PackageReceiptFixture -Name 'ForeignPlatformModule'
        $Catalog = Get-Content -LiteralPath $Fixture.CatalogPath -Raw | ConvertFrom-Json -Depth 64
        $ForeignModule = [pscustomobject][ordered]@{
            module_id = 'android.only'
            variants = @([pscustomobject][ordered]@{
                platform = 'android'
                architecture = 'arm64'
                configuration = 'development'
                backend = 'wasmtime'
                format = 'wasmtime_serialized_v1'
                package_id = 'd' * 64
                descriptor_file = "android.only/$('d' * 64)/package.json"
                descriptor_sha256 = 'e' * 64
            })
        }
        $Catalog.modules = @($ForeignModule) + @($Catalog.modules)
        Write-FixtureJson -Path $Fixture.CatalogPath -Value $Catalog
        $Result = Invoke-ReceiptValidator -Fixture $Fixture
        Assert-ContractCondition ($Result.ExitCode -eq 0) "Foreign-platform module was rejected: $($Result.Raw)"
        Assert-ContractCondition ([int]$Result.Summary.expected_dependency_count -eq 13) 'Foreign-platform module changed staged dependency count.'
    }

    Invoke-ContractCase 'plugin startup scenario is required as UFS' {
        $Fixture = New-PackageReceiptFixture -Name 'StartupScenario'
        $ScenarioPath = Join-Path $Fixture.PluginRoot 'Content/AvidScript/Startup/scenarios.json'
        Write-FixtureJson -Path $ScenarioPath -Value ([ordered]@{
            schema_version = 1
            scenarios = @()
        })
        $Receipt = Read-FixtureReceipt $Fixture
        $Receipt.RuntimeDependencies = @($Receipt.RuntimeDependencies) + @([pscustomobject]@{
            Path = '$(ProjectDir)/Plugins/AvidScript/Content/AvidScript/Startup/scenarios.json'
            Type = 'UFS'
        })
        Write-FixtureReceipt -Fixture $Fixture -Receipt $Receipt
        $Result = Invoke-ReceiptValidator -Fixture $Fixture
        Assert-ContractCondition ($Result.ExitCode -eq 0) "Startup scenario dependency was rejected: $($Result.Raw)"
        Assert-ContractCondition ([int]$Result.Summary.expected_dependency_count -eq 14) 'Startup scenario expected dependency count is invalid.'
    }

    Invoke-ContractCase 'missing staged dependency rejected' {
        $Fixture = New-PackageReceiptFixture -Name 'MissingDependency'
        $Receipt = Read-FixtureReceipt $Fixture
        $Receipt.RuntimeDependencies = @($Receipt.RuntimeDependencies | Where-Object {
            [string]$_.Path -cne '$(ProjectDir)/Content/AvidScript/Modules/fixture.module/' + ('a' * 64) + '/module.wasm'
        })
        Write-FixtureReceipt -Fixture $Fixture -Receipt $Receipt
        Assert-ValidatorRejected -Fixture $Fixture -ErrorCode 'MISSING_DEPENDENCY'
    }

    Invoke-ContractCase 'extra AvidScript dependency rejected' {
        $Fixture = New-PackageReceiptFixture -Name 'ExtraDependency'
        $Receipt = Read-FixtureReceipt $Fixture
        $Receipt.RuntimeDependencies = @($Receipt.RuntimeDependencies) + @([pscustomobject]@{
            Path = '$(ProjectDir)/Plugins/AvidScript/Content/unexpected.bin'
            Type = 'UFS'
        })
        Write-FixtureReceipt -Fixture $Fixture -Receipt $Receipt
        Assert-ValidatorRejected -Fixture $Fixture -ErrorCode 'EXTRA_DEPENDENCY'
    }

    Invoke-ContractCase 'module dependency must be UFS' {
        $Fixture = New-PackageReceiptFixture -Name 'WrongUfsType'
        $Receipt = Read-FixtureReceipt $Fixture
        $Dependency = @($Receipt.RuntimeDependencies | Where-Object {
            [string]$_.Path -like '*/module.wasm'
        })[0]
        $Dependency.Type = 'NonUFS'
        Write-FixtureReceipt -Fixture $Fixture -Receipt $Receipt
        Assert-ValidatorRejected -Fixture $Fixture -ErrorCode 'DEPENDENCY_TYPE_MISMATCH'
    }

    Invoke-ContractCase 'Wasmtime dependency must be NonUFS' {
        $Fixture = New-PackageReceiptFixture -Name 'WrongNonUfsType'
        $Receipt = Read-FixtureReceipt $Fixture
        $Dependency = @($Receipt.RuntimeDependencies | Where-Object {
            [string]$_.Path -like '*/wasmtime.dll'
        })[0]
        $Dependency.Type = 'UFS'
        Write-FixtureReceipt -Fixture $Fixture -Receipt $Receipt
        Assert-ValidatorRejected -Fixture $Fixture -ErrorCode 'DEPENDENCY_TYPE_MISMATCH'
    }

    Invoke-ContractCase 'source hash drift rejected' {
        $Fixture = New-PackageReceiptFixture -Name 'HashDrift'
        [System.IO.File]::AppendAllText(
            (Join-Path $Fixture.PackageRoot 'module.wasm'),
            'drift',
            $Utf8)
        Assert-ValidatorRejected -Fixture $Fixture -ErrorCode 'SOURCE_HASH_MISMATCH'
    }

    Invoke-ContractCase 'missing source file rejected' {
        $Fixture = New-PackageReceiptFixture -Name 'MissingSource'
        Remove-Item -LiteralPath (Join-Path $Fixture.PackageRoot 'bindings/bindings.json')
        Assert-ValidatorRejected -Fixture $Fixture -ErrorCode 'SOURCE_FILE_MISSING'
    }

    Invoke-ContractCase 'Wasmtime marker hash drift rejected' {
        $Fixture = New-PackageReceiptFixture -Name 'WasmtimeHashDrift'
        [System.IO.File]::AppendAllText($Fixture.WasmtimeDllPath, 'drift', $Utf8)
        Assert-ValidatorRejected -Fixture $Fixture -ErrorCode 'WASMTIME_DLL_HASH_MISMATCH'
    }

    Invoke-ContractCase 'receipt Shipping Development mismatch rejected' {
        $Fixture = New-PackageReceiptFixture -Name 'ReceiptConfigurationMismatch' -Configuration Development
        Assert-ValidatorRejected `
            -Fixture $Fixture `
            -Configuration Shipping `
            -ErrorCode 'PACKAGE_VARIANT_MISMATCH'
    }

    Invoke-ContractCase 'package Shipping Development mismatch rejected' {
        $Fixture = New-PackageReceiptFixture -Name 'PackageConfigurationMismatch' -Configuration Shipping
        $Receipt = Read-FixtureReceipt $Fixture
        $Receipt.Configuration = 'Development'
        Write-FixtureReceipt -Fixture $Fixture -Receipt $Receipt
        Assert-ValidatorRejected `
            -Fixture $Fixture `
            -Configuration Shipping `
            -ErrorCode 'RECEIPT_CONFIGURATION_MISMATCH'
    }

    if ($Failures.Count -gt 0) {
        throw "Package receipt contracts failed ($Passed/$Total): $($Failures -join ' | ')"
    }

    [pscustomobject][ordered]@{
        result = 'avidscript_package_receipt_contracts_passed'
        status = 'ok'
        passed = $Passed
        total = $Total
        coverage = @(
            'positive',
            'foreign_platform_variant',
            'startup_scenario_ufs',
            'missing_dependency',
            'extra_dependency',
            'ufs_type',
            'non_ufs_type',
            'source_hash_drift',
            'missing_source',
            'wasmtime_marker_hash_drift',
            'shipping_development_mismatch'
        )
    } | ConvertTo-Json -Depth 4 -Compress
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot -PathType Container) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}
