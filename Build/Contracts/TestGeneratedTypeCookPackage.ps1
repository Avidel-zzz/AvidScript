$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. (Join-Path $BuildRoot 'AvidScriptGeneratedTypeCookPackage.ps1')

$Root = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ("AvidScriptCookPackageContract_$PID`_$([Guid]::NewGuid().ToString('N'))")
$Utf8 = [System.Text.UTF8Encoding]::new($false)
$Passed = 0
$Total = 0
$Failure = $null

function Write-TestJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Value
    )

    [System.IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 64) + "`n",
        $Utf8)
}

function Get-TestTreeIdentity {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [string]::Join(
        "`n",
        @(Get-ChildItem -LiteralPath $Path -File -Recurse |
            Sort-Object { [System.IO.Path]::GetRelativePath($Path, $_.FullName) } |
            ForEach-Object {
                $RelativePath = [System.IO.Path]::GetRelativePath(
                    $Path,
                    $_.FullName).Replace('\', '/')
                "$RelativePath=$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())"
            }))
}

function Resolve-TestGeneratedTypeRuntimeDescriptor {
    param(
        [Parameter(Mandatory = $true)][object]$Pointer,
        [Parameter(Mandatory = $true)][object]$Catalog,
        [Parameter(Mandatory = $true)][string]$CatalogPath
    )

    if ([int]$Pointer.schema_version -ne 2) {
        throw 'Generated Type pointer is not schema v2.'
    }
    $Matches = @($Catalog.modules | Where-Object {
            [string]$_.module_id -ceq [string]$Pointer.module_id
        })
    if ($Matches.Count -ne 1) {
        throw "Generated Type Runtime module is missing: $($Pointer.module_id)"
    }
    $Variants = @(if ([int]$Catalog.schema_version -eq 1) {
            $Matches[0]
        }
        elseif ([int]$Catalog.schema_version -eq 2) {
            @($Matches[0].variants)
        }
        else {
            throw "Generated Type Runtime catalog schema is unsupported: $($Catalog.schema_version)"
        })
    $PackageMatches = @($Variants | Where-Object {
            [string]$_.package_id -ceq [string]$Pointer.package_id
        })
    if ($PackageMatches.Count -ne 1) {
        throw "Generated Type Runtime package id drift: expected=$($Pointer.package_id) resolved=$($Matches[0].package_id)"
    }
    return Join-Path `
        (Split-Path -Parent $CatalogPath) `
        ([string]$PackageMatches[0].descriptor_file)
}

function Invoke-ContractTest {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    $script:Total++
    try {
        & $Body
        $script:Passed++
        Write-Output "PASS $Name"
    }
    catch {
        throw "$Name failed: $($_.Exception.Message)"
    }
}

function Get-TestBuilderIdentityGuard {
    param([Parameter(Mandatory = $true)][string]$Token)

    $Tokens = $null
    $ParseErrors = $null
    $Ast = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $BuildRoot 'BuildCSharpScriptTypes.ps1'),
        [ref]$Tokens,
        [ref]$ParseErrors)
    if ($ParseErrors.Count -ne 0) {
        throw 'Generated Type builder has PowerShell parse errors.'
    }
    $Guards = @($Ast.FindAll({
                param($Node)
                $Node -is [System.Management.Automation.Language.IfStatementAst] -and
                    $Node.Clauses[0].Item1.Extent.Text.Contains($Token)
            }, $true))
    if ($Guards.Count -ne 1) {
        throw "Expected one Generated Type identity guard for $Token."
    }
    return [scriptblock]::Create($Guards[0].Extent.Text)
}

try {
    $ProjectRoot = Join-Path $Root 'Project'
    $SourceRoot = Join-Path $ProjectRoot 'Source'
    $RuntimeRoot = Join-Path $ProjectRoot 'Saved\Runtime'
    $BindingRoot = Join-Path $ProjectRoot 'Saved\Bindings'
    $OutputRoot = Join-Path $ProjectRoot 'Plugins\AvidScript\Content\AvidScriptGenerated'
    [void][System.IO.Directory]::CreateDirectory($SourceRoot)
    [void][System.IO.Directory]::CreateDirectory($RuntimeRoot)
    [void][System.IO.Directory]::CreateDirectory($BindingRoot)

    $TypePath = Join-Path $SourceRoot 'types.json'
    $WasmPath = Join-Path $RuntimeRoot 'module.wasm'
    $PrecompiledPath = Join-Path $RuntimeRoot 'module.wasmtime.cwasm'
    $DebugPath = Join-Path $RuntimeRoot 'debug-map.json'
    $BindingManifestPath = Join-Path $BindingRoot 'package.json'
    $BindingDescriptorPath = Join-Path $BindingRoot 'bindings.json'
    Write-TestJson -Path $TypePath -Value ([ordered]@{
            schema_version = 5
            module_name = 'AvidScriptGenerated'
            generation_key_sha256 = ('b' * 64)
            types = @()
        })
    [System.IO.File]::WriteAllBytes(
        $WasmPath,
        [byte[]](0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00))
    [System.IO.File]::WriteAllBytes(
        $PrecompiledPath,
        [byte[]](0x61, 0x76, 0x69, 0x64, 0x2d, 0x63, 0x77, 0x61, 0x73, 0x6d))
    Write-TestJson -Path $DebugPath -Value ([ordered]@{
            schema_version = 1
            module_id = 'avidscript_generated'
        })
    Write-TestJson -Path $BindingDescriptorPath -Value ([ordered]@{
            schema_version = 19
            modules = @()
        })
    $BindingDescriptorSha256 = Get-AvidScriptCookPackageSha256 $BindingDescriptorPath
    Write-TestJson -Path $BindingManifestPath -Value ([ordered]@{
            schema_version = 1
            descriptor_sha256 = $BindingDescriptorSha256
            files = [ordered]@{ descriptor = 'bindings.json' }
        })

    $RuntimePath = Join-Path $RuntimeRoot 'runtime.avidscript.json'
    $WasmSha256 = Get-AvidScriptCookPackageSha256 $WasmPath
    $RuntimeManifest = [ordered]@{
        schema_version = 1
        module_id = 'avidscript_generated'
        abi_version = 1
        language = 'csharp'
        source = [ordered]@{
            file = 'GeneratedTypes.cs'
            sha256 = ('d' * 64)
            frontend_sha256 = ('e' * 64)
            semantic_sha256 = ('f' * 64)
        }
        guest_ir = [ordered]@{
            module_id = 'avidscript_generated'
            sha256 = ('1' * 64)
        }
        wasm = [ordered]@{
            file = 'module.wasm'
            sha256 = $WasmSha256
        }
        execution = [ordered]@{
            backend = 'wasmtime'
            format = 'wasmtime_serialized_v1'
            file = 'module.wasmtime.cwasm'
            sha256 = Get-AvidScriptCookPackageSha256 $PrecompiledPath
            canonical_sha256 = $WasmSha256
            compiler_build_identity = 'wasmtime-contract-build'
            target_triple = 'x86_64-pc-windows-msvc'
            cpu_features = 'x86-64-v3'
            attestation_id = ('1' * 32)
            policy = 'prefer_precompiled'
            fallback = 'wasmtime_jit'
        }
        binding_package = [ordered]@{
            manifest_file = 'Saved/Bindings/package.json'
            manifest_sha256 = Get-AvidScriptCookPackageSha256 $BindingManifestPath
            descriptor_file = 'Saved/Bindings/bindings.json'
            descriptor_sha256 = $BindingDescriptorSha256
        }
        debug_map = [ordered]@{
            file = 'debug-map.json'
            sha256 = Get-AvidScriptCookPackageSha256 $DebugPath
        }
        required_exports = @()
        required_imports = @()
    }
    Write-TestJson -Path $RuntimePath -Value $RuntimeManifest

    $DescriptorPath = Join-Path $SourceRoot 'package.json'
    $Descriptor = [ordered]@{
        schema_version = 1
        package_id = ('a' * 64)
        module_name = 'AvidScriptGenerated'
        runtime_module_id = 'avidscript_generated'
        execution_backend = 'wasmtime_jit'
        generation_key_sha256 = ('b' * 64)
        type_manifest = [ordered]@{
            file = 'types.json'
            sha256 = Get-AvidScriptCookPackageSha256 $TypePath
        }
        runtime_manifest = [ordered]@{
            file = [System.IO.Path]::GetRelativePath(
                $SourceRoot,
                $RuntimePath).Replace('\', '/')
            sha256 = Get-AvidScriptCookPackageSha256 $RuntimePath
        }
        reload = [ordered]@{ native_structure_sha256 = ('c' * 64) }
    }
    Write-TestJson -Path $DescriptorPath -Value $Descriptor

    $First = $null
    $Second = $null
    $Current = $null
    $Catalog = $null
    $CatalogPath = Join-Path $ProjectRoot 'Content\AvidScript\Modules\catalog.json'

    Invoke-ContractTest -Name 'schema v2 deterministic publication' -Body {
        $script:First = Publish-AvidScriptGeneratedTypeCookPackage `
            -PackageDescriptorPath $DescriptorPath `
            -ProjectRoot $ProjectRoot `
            -OutputRoot $OutputRoot
        $FirstPointerIdentity = Get-AvidScriptCookPackageSha256 $First.DescriptorPath
        $FirstGeneratedIdentity = Get-TestTreeIdentity $First.BundleRoot
        $FirstRuntimeIdentity = Get-TestTreeIdentity $First.RuntimePackageRoot
        $FirstCatalogIdentity = Get-AvidScriptCookPackageSha256 $CatalogPath
        $script:Second = Publish-AvidScriptGeneratedTypeCookPackage `
            -PackageDescriptorPath $DescriptorPath `
            -ProjectRoot $ProjectRoot `
            -OutputRoot $OutputRoot `
            -TargetPlatform Win64
        if ($First.ModuleId -cne $Second.ModuleId -or
            $First.PackageId -cne $Second.PackageId -or
            $First.GeneratedTypePackageId -cne $Second.GeneratedTypePackageId -or
            $First.Platform -cne 'win64' -or
            $First.Architecture -cne 'x86_64' -or
            $First.TargetTriple -cne 'x86_64-pc-windows-msvc' -or
            $First.Configuration -cne 'development' -or
            $First.FileCount -ne 2 -or
            $First.RuntimeFileCount -ne 7 -or
            $FirstPointerIdentity -cne (Get-AvidScriptCookPackageSha256 $Second.DescriptorPath) -or
            $FirstGeneratedIdentity -cne (Get-TestTreeIdentity $Second.BundleRoot) -or
            $FirstRuntimeIdentity -cne (Get-TestTreeIdentity $Second.RuntimePackageRoot) -or
            $FirstCatalogIdentity -cne (Get-AvidScriptCookPackageSha256 $CatalogPath)) {
            throw 'Repeated publication changed an identity, file set, or hash.'
        }
        $GeneratedFiles = @(Get-ChildItem -LiteralPath $First.BundleRoot -File -Recurse)
        if ($GeneratedFiles.Count -ne 1 -or $GeneratedFiles[0].Name -cne 'type-manifest.json') {
            throw 'Generated Type content-addressed bundle is not the exact v2 file set.'
        }
    }

    Invoke-ContractTest -Name 'schema v2 pointer and catalog' -Body {
        $script:Current = Get-Content -Raw -LiteralPath $First.DescriptorPath |
            ConvertFrom-Json -Depth 64
        $script:Catalog = Get-Content -Raw -LiteralPath $CatalogPath |
            ConvertFrom-Json -Depth 64
        if ([int]$Current.schema_version -ne 2 -or
            [string]$Current.module_id -cne $First.ModuleId -or
            [string]$Current.package_id -cne $First.PackageId -or
            [string]$Current.type_manifest.file -cne
                "$($First.GeneratedTypePackageId)/type-manifest.json" -or
            $Current.PSObject.Properties.Name -ccontains 'runtime_manifest' -or
            $Current.PSObject.Properties.Name -ccontains 'runtime_module_id') {
            throw 'Cook pointer does not have the frozen schema v2 shape.'
        }
        $ResolvedDescriptor = Resolve-TestGeneratedTypeRuntimeDescriptor `
            -Pointer $Current `
            -Catalog $Catalog `
            -CatalogPath $CatalogPath
        if (-not $ResolvedDescriptor.Equals(
                $First.RuntimeDescriptorPath,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'Generated Type pointer and Runtime catalog resolve different descriptors.'
        }
    }

    Invoke-ContractTest -Name 'RuntimeHost public resolver integration' -Body {
        $RuntimeHostPath = Join-Path `
            (Split-Path -Parent $BuildRoot) `
            'Source/AvidScriptRuntime/Private/ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.cpp'
        $RuntimeHostSource = Get-Content -Raw -LiteralPath $RuntimeHostPath
        foreach ($RequiredToken in @(
                'FAvidScriptRuntimeArtifactLoader::LoadPublishedModule(',
                'FName(*RuntimeModuleId)',
                'PackageId,',
                'TEXT("runtime_module_id")',
                'TEXT("execution_backend")')) {
            if (-not $RuntimeHostSource.Contains($RequiredToken)) {
                throw "RuntimeHost is missing compatibility token: $RequiredToken"
            }
        }
    }

    Invoke-ContractTest -Name 'package id drift rejection' -Body {
        $DriftedPointer = $Current | ConvertTo-Json -Depth 64 |
            ConvertFrom-Json -Depth 64
        $DriftedPointer.package_id = ('d' * 64)
        $Rejected = $false
        try {
            Resolve-TestGeneratedTypeRuntimeDescriptor `
                -Pointer $DriftedPointer `
                -Catalog $Catalog `
                -CatalogPath $CatalogPath | Out-Null
        }
        catch {
            $Rejected = $_.Exception.Message.Contains('package id drift')
        }
        if (-not $Rejected) {
            throw 'A Generated Type pointer with a drifted package id was accepted.'
        }
    }

    Invoke-ContractTest -Name 'missing module rejection' -Body {
        $MissingPointer = $Current | ConvertTo-Json -Depth 64 |
            ConvertFrom-Json -Depth 64
        $MissingPointer.module_id = 'missing_generated_module'
        $Rejected = $false
        try {
            Resolve-TestGeneratedTypeRuntimeDescriptor `
                -Pointer $MissingPointer `
                -Catalog $Catalog `
                -CatalogPath $CatalogPath | Out-Null
        }
        catch {
            $Rejected = $_.Exception.Message.Contains('module is missing')
        }
        if (-not $Rejected) {
            throw 'A Generated Type pointer for a missing module was accepted.'
        }
    }

    Invoke-ContractTest -Name 'project-root escape rejection' -Body {
        $OutsideRoot = Join-Path $Root 'OutsideProject'
        [void][System.IO.Directory]::CreateDirectory($OutsideRoot)
        $OutsideArtifactPath = Join-Path $OutsideRoot 'outside.json'
        [System.IO.File]::WriteAllText($OutsideArtifactPath, '{}', $Utf8)
        $Rejected = $false
        try {
            Resolve-AvidScriptCookPackageArtifactPath `
                -ManifestPath $DescriptorPath `
                -ArtifactPath $OutsideArtifactPath `
                -ProjectRoot $ProjectRoot | Out-Null
        }
        catch {
            $Rejected = $_.Exception.Message.Contains('outside the project root')
        }
        if (-not $Rejected) {
            throw 'An artifact outside the project root was accepted.'
        }
    }

    Invoke-ContractTest -Name 'Shipping rejects JIT Generated Type package' -Body {
        $Rejected = $false
        try {
            Publish-AvidScriptGeneratedTypeCookPackage `
                -PackageDescriptorPath $DescriptorPath `
                -ProjectRoot $ProjectRoot `
                -OutputRoot $OutputRoot `
                -Configuration Shipping | Out-Null
        }
        catch {
            $Rejected = $_.Exception.Message.Contains(
                'require a precompiled Runtime module')
        }
        if (-not $Rejected) {
            throw 'Shipping accepted a JIT Generated Type package.'
        }
    }

    Invoke-ContractTest -Name 'Shipping precompiled Generated Type package' -Body {
        $Descriptor.execution_backend = 'wasmtime_precompiled'
        Write-TestJson -Path $DescriptorPath -Value $Descriptor
        $ShippingPackage = Publish-AvidScriptGeneratedTypeCookPackage `
            -PackageDescriptorPath $DescriptorPath `
            -ProjectRoot $ProjectRoot `
            -OutputRoot $OutputRoot `
            -Configuration Shipping
        $ShippingRuntimeDescriptor = Get-Content `
            -Raw `
            -LiteralPath $ShippingPackage.RuntimeDescriptorPath |
            ConvertFrom-Json -Depth 64
        if ([string]$ShippingRuntimeDescriptor.configuration -cne 'shipping' -or
            [string]$ShippingRuntimeDescriptor.execution.policy -cne
                'require_precompiled') {
            throw 'Shipping Generated Type package did not preserve its precompiled policy.'
        }
    }

    Invoke-ContractTest -Name 'headless Generated Type release route' -Body {
        $ScriptTypeBuilderPath = Join-Path $BuildRoot 'BuildCSharpScriptTypes.ps1'
        $ScriptTypeBuilderSource = Get-Content `
            -Raw `
            -LiteralPath $ScriptTypeBuilderPath
        foreach ($RequiredToken in @(
                '[string]$PackageConfiguration = "Development"',
                '[string]$TargetPlatform = "Win64"',
                '[switch]$HeadlessRelease',
                'InvokeAvidScriptRelease.ps1',
                '-GeneratedTypeManifestPath $GeneratedManifestPath',
                '-TargetPlatform $TargetPlatform',
                '-Configuration $PackageConfiguration',
                '"wasmtime_precompiled"')) {
            if (-not $ScriptTypeBuilderSource.Contains($RequiredToken)) {
                throw "Generated Type headless release route is missing: $RequiredToken"
            }
        }
        if ($ScriptTypeBuilderSource -notmatch
            '(?s)if\s*\(\$HeadlessRelease\)\s*\{\s*\$CookPackage\s*=\s*Publish-AvidScriptGeneratedTypeCookPackage') {
            throw 'Editor JIT Generated Type builds are not isolated from the Cook publisher.'
        }
        if ([regex]::Matches($ScriptTypeBuilderSource, '-TargetPlatform \$TargetPlatform').Count -ne 2) {
            throw 'Generated Type builder must forward TargetPlatform to Release and Cook publication.'
        }
    }

    foreach ($GuardKind in @('Release', 'Cook')) {
        Invoke-ContractTest -Name "$GuardKind return identity validation" -Body {
            $Token = if ($GuardKind -ceq 'Release') { '$ReleaseSummary.result' } else { '$CookPackage.ModuleId' }
            $Guard = Get-TestBuilderIdentityGuard -Token $Token
            foreach ($TargetPlatform in @('Win64', 'Android')) {
                $RuntimeModuleId = 'avidscript_generated'
                $PackageConfiguration = 'Development'
                $ExpectedArchitecture = if ($TargetPlatform -ceq 'Android') { 'arm64' } else { 'x86_64' }
                $ExpectedTargetTriple = if ($TargetPlatform -ceq 'Android') {
                    'aarch64-linux-android'
                }
                else {
                    'x86_64-pc-windows-msvc'
                }
                $ReleaseSummary = [pscustomobject]@{
                    schema_version = 1
                    result = 'avidscript_module_release_succeeded'
                    module_id = $RuntimeModuleId
                    package_id = ('a' * 64)
                    configuration = 'development'
                    target_platform = $TargetPlatform.ToLowerInvariant()
                    architecture = $ExpectedArchitecture
                    target_triple = $ExpectedTargetTriple
                }
                $CookPackage = [pscustomobject]@{
                    ModuleId = $RuntimeModuleId
                    PackageId = $ReleaseSummary.package_id
                    Configuration = 'development'
                    Platform = $TargetPlatform.ToLowerInvariant()
                    Architecture = $ExpectedArchitecture
                    TargetTriple = $ExpectedTargetTriple
                }
                & $Guard
                $Identity = if ($GuardKind -ceq 'Release') { $ReleaseSummary } else { $CookPackage }
                foreach ($Property in @($Identity.PSObject.Properties)) {
                    $Original = $Property.Value
                    $Property.Value = if ($Property.Name -ceq 'schema_version') { 0 } else { 'drifted' }
                    $Rejected = $false
                    try { & $Guard }
                    catch { $Rejected = $_.Exception.Message.Contains('identity') }
                    finally { $Property.Value = $Original }
                    if (-not $Rejected) {
                        throw "$GuardKind accepted $TargetPlatform identity drift in $($Property.Name)."
                    }
                }
            }
        }
    }

    foreach ($AndroidConfiguration in @('Development', 'Shipping')) {
        Invoke-ContractTest -Name "Android $AndroidConfiguration requires HeadlessRelease" -Body {
            foreach ($SkipPackage in @($false, $true)) {
                $Rejected = $false
                try {
                    & (Join-Path $BuildRoot 'BuildCSharpScriptTypes.ps1') `
                        -DotNetPath 'unused' `
                        -SourcePath 'unused' `
                        -SourceId 'Fixture.cs' `
                        -BindingPackageManifestPath 'unused' `
                        -TargetPlatform android `
                        -PackageConfiguration $AndroidConfiguration `
                        -SkipRuntimePackage:$SkipPackage | Out-Null
                }
                catch {
                    $Rejected = $_.Exception.Message.Contains(
                        'Android Generated Type packages require -HeadlessRelease')
                }
                if (-not $Rejected) {
                    throw 'Android reached generation without HeadlessRelease.'
                }
            }
        }
    }

    $AndroidDescriptor = $Descriptor | ConvertTo-Json -Depth 64 | ConvertFrom-Json -Depth 64
    $AndroidDescriptor.execution_backend = 'wasmtime_jit'
    Write-TestJson -Path $DescriptorPath -Value $AndroidDescriptor
    foreach ($AndroidConfiguration in @('Development', 'Shipping')) {
        Invoke-ContractTest -Name "Android $AndroidConfiguration rejects JIT Generated Type package" -Body {
            $PointerIdentity = Get-AvidScriptCookPackageSha256 $First.DescriptorPath
            $CatalogIdentity = Get-AvidScriptCookPackageSha256 $CatalogPath
            $Rejected = $false
            try {
                Publish-AvidScriptGeneratedTypeCookPackage `
                    -PackageDescriptorPath $DescriptorPath `
                    -ProjectRoot $ProjectRoot `
                    -OutputRoot $OutputRoot `
                    -TargetPlatform android `
                    -Configuration $AndroidConfiguration | Out-Null
            }
            catch {
                $Rejected = $_.Exception.Message.Contains('require a precompiled Runtime module')
            }
            if (-not $Rejected -or
                $PointerIdentity -cne (Get-AvidScriptCookPackageSha256 $First.DescriptorPath) -or
                $CatalogIdentity -cne (Get-AvidScriptCookPackageSha256 $CatalogPath)) {
                throw 'Android JIT rejection failed or changed published identities.'
            }
        }
    }

    $AndroidRuntimeManifest = $RuntimeManifest | ConvertTo-Json -Depth 64 | ConvertFrom-Json -Depth 64
    $AndroidRuntimeManifest.execution.target_triple = 'aarch64-linux-android'
    $AndroidRuntimeManifest.execution.cpu_features = 'arm64-v8a'
    $AndroidRuntimeManifest.execution.policy = 'require_precompiled'
    Write-TestJson -Path $RuntimePath -Value $AndroidRuntimeManifest
    $AndroidDescriptor.execution_backend = 'wasmtime_precompiled'
    $AndroidDescriptor.runtime_manifest.sha256 = Get-AvidScriptCookPackageSha256 $RuntimePath
    Write-TestJson -Path $DescriptorPath -Value $AndroidDescriptor
    foreach ($AndroidConfiguration in @('Development', 'Shipping')) {
        Invoke-ContractTest -Name "Android $AndroidConfiguration precompiled arm64 publication" -Body {
            $AndroidPackage = Publish-AvidScriptGeneratedTypeCookPackage `
                -PackageDescriptorPath $DescriptorPath `
                -ProjectRoot $ProjectRoot `
                -OutputRoot $OutputRoot `
                -TargetPlatform Android `
                -Configuration $AndroidConfiguration
            $Published = Get-Content -Raw -LiteralPath $AndroidPackage.RuntimeDescriptorPath |
                ConvertFrom-Json -Depth 64
            $PublishedRuntime = Get-Content -Raw -LiteralPath (
                Join-Path $AndroidPackage.RuntimePackageRoot 'runtime.avidscript.json') |
                ConvertFrom-Json -Depth 64
            $AndroidCatalog = Get-Content -Raw -LiteralPath $CatalogPath | ConvertFrom-Json -Depth 64
            $Variant = @($AndroidCatalog.modules[0].variants | Where-Object {
                    $_.package_id -ceq $AndroidPackage.PackageId
                })
            $Pointer = Get-Content -Raw -LiteralPath $AndroidPackage.DescriptorPath |
                ConvertFrom-Json -Depth 64
            $Resolved = Resolve-TestGeneratedTypeRuntimeDescriptor `
                -Pointer $Pointer -Catalog $AndroidCatalog -CatalogPath $CatalogPath
            if ($AndroidPackage.Platform -cne 'android' -or
                $AndroidPackage.Architecture -cne 'arm64' -or
                $AndroidPackage.TargetTriple -cne 'aarch64-linux-android' -or
                $AndroidPackage.Configuration -cne $AndroidConfiguration.ToLowerInvariant() -or
                $Published.platform -cne 'android' -or
                $Published.configuration -cne $AndroidConfiguration.ToLowerInvariant() -or
                $Published.execution.policy -cne 'require_precompiled' -or
                $Published.execution.target_triple -cne 'aarch64-linux-android' -or
                $Published.execution.cpu_features -cne 'arm64-v8a' -or
                $PublishedRuntime.execution.format -cne 'wasmtime_serialized_v1' -or
                $PublishedRuntime.execution.policy -cne 'require_precompiled' -or
                $PublishedRuntime.execution.PSObject.Properties.Name -ccontains 'fallback' -or
                $Variant.Count -ne 1 -or $Variant[0].platform -cne 'android' -or
                $Variant[0].architecture -cne 'arm64' -or
                $Resolved -cne $AndroidPackage.RuntimeDescriptorPath -or
                @($AndroidCatalog.modules[0].variants | Where-Object {
                    $_.platform -ceq 'win64' -and $_.package_id -ceq $First.PackageId
                }).Count -ne 1) {
                throw 'Android publication lost its arm64/AOT identity, pointer resolution, or Win64 variant.'
            }
        }
    }

    foreach ($InvalidExecution in @(
            @{ Name = 'JIT fallback policy'; Field = 'policy'; Value = 'prefer_precompiled' },
            @{ Name = 'loose WASM'; Field = 'format'; Value = 'wasm' },
            @{ Name = 'Win64 target'; Field = 'target_triple'; Value = 'x86_64-pc-windows-msvc' })) {
        Invoke-ContractTest -Name "Android rejects $($InvalidExecution.Name)" -Body {
            $InvalidManifest = $AndroidRuntimeManifest | ConvertTo-Json -Depth 64 |
                ConvertFrom-Json -Depth 64
            $InvalidManifest.execution.($InvalidExecution.Field) = $InvalidExecution.Value
            Write-TestJson -Path $RuntimePath -Value $InvalidManifest
            $AndroidDescriptor.runtime_manifest.sha256 = Get-AvidScriptCookPackageSha256 $RuntimePath
            Write-TestJson -Path $DescriptorPath -Value $AndroidDescriptor
            $Rejected = $false
            try {
                Publish-AvidScriptGeneratedTypeCookPackage `
                    -PackageDescriptorPath $DescriptorPath `
                    -ProjectRoot $ProjectRoot `
                    -OutputRoot $OutputRoot `
                    -TargetPlatform Android | Out-Null
            }
            catch {
                $Rejected = $_.Exception.Message.Contains('Runtime manifest execution contract is invalid')
            }
            if (-not $Rejected) {
                throw "Android accepted $($InvalidExecution.Name)."
            }
        }
    }

    Invoke-ContractTest -Name 'Cook publisher rejects Runtime return identity drift' -Body {
        function Publish-AvidScriptModuleReleasePackage { return $InvalidPackage }
        foreach ($Field in @('ModuleId', 'PackageId', 'Platform', 'Architecture', 'TargetTriple', 'Configuration')) {
            $InvalidPackage = [pscustomobject]@{
                ModuleId = 'avidscript_generated'
                PackageId = ('a' * 64)
                Platform = 'android'
                Architecture = 'arm64'
                TargetTriple = 'aarch64-linux-android'
                Configuration = 'development'
            }
            $InvalidPackage.$Field = 'drifted'
            $Rejected = $false
            try {
                Publish-AvidScriptGeneratedTypeCookPackage `
                    -PackageDescriptorPath $DescriptorPath `
                    -ProjectRoot $ProjectRoot `
                    -OutputRoot $OutputRoot `
                    -TargetPlatform Android | Out-Null
            }
            catch {
                $Rejected = $_.Exception.Message.Contains('Runtime publication identity is invalid')
            }
            if (-not $Rejected) {
                throw "Cook publisher accepted Runtime identity drift in $Field."
            }
        }
    }

    Write-TestJson -Path $RuntimePath -Value $RuntimeManifest
    Write-TestJson -Path $DescriptorPath -Value $Descriptor
    Invoke-ContractTest -Name 'Runtime artifact hash rejection' -Body {
        [System.IO.File]::WriteAllBytes(
            $WasmPath,
            [byte[]](0x00, 0x61, 0x73, 0x6d, 0x02, 0x00, 0x00, 0x00))
        $Rejected = $false
        try {
            Publish-AvidScriptGeneratedTypeCookPackage `
                -PackageDescriptorPath $DescriptorPath `
                -ProjectRoot $ProjectRoot `
                -OutputRoot $OutputRoot | Out-Null
        }
        catch {
            $Rejected = $_.Exception.Message.Contains('WASM SHA-256 mismatch')
        }
        if (-not $Rejected) {
            throw 'A tampered canonical WASM artifact was accepted.'
        }
    }
}
catch {
    $Failure = $_.Exception.Message
}
finally {
    if (Test-Path -LiteralPath $Root -PathType Container) {
        Remove-Item -LiteralPath $Root -Recurse -Force
    }
}

if ($null -ne $Failure) {
    [Console]::Error.WriteLine(
        "Generated type Cook package contracts: $Passed/$Total passed; FAIL: $Failure")
    exit 1
}

Write-Output "Generated type Cook package contracts: PASS ($Passed/$Total)"
exit 0
