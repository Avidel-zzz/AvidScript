$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. (Join-Path $BuildRoot 'AvidScriptGeneratedTypeCookPackage.ps1')

$Root = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ("AvidScriptCookPackageContract_$PID`_$([Guid]::NewGuid().ToString('N'))")
$Utf8 = [System.Text.UTF8Encoding]::new($false)
$Passed = 0
$Total = 10
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
    if ([string]$Matches[0].package_id -cne [string]$Pointer.package_id) {
        throw "Generated Type Runtime package id drift: expected=$($Pointer.package_id) resolved=$($Matches[0].package_id)"
    }
    return Join-Path (Split-Path -Parent $CatalogPath) ([string]$Matches[0].descriptor_file)
}

function Invoke-ContractTest {
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
            -OutputRoot $OutputRoot
        if ($First.ModuleId -cne $Second.ModuleId -or
            $First.PackageId -cne $Second.PackageId -or
            $First.GeneratedTypePackageId -cne $Second.GeneratedTypePackageId -or
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
                '[switch]$HeadlessRelease',
                'InvokeAvidScriptRelease.ps1',
                '-GeneratedTypeManifestPath $GeneratedManifestPath',
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
    }

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
