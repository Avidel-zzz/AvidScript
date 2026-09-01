$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. (Join-Path $BuildRoot 'AvidScriptModuleReleasePackage.ps1')

$Root = Join-Path ([System.IO.Path]::GetTempPath()) (
    "AvidScriptModuleReleaseContract_$PID`_$([Guid]::NewGuid().ToString('N'))")
$Utf8 = [System.Text.UTF8Encoding]::new($false)
$Passed = 0
$Total = 0
$Failures = [System.Collections.Generic.List[string]]::new()

function Write-TestJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Value
    )

    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    $Json = (($Value | ConvertTo-Json -Depth 64).Replace("`r`n", "`n")) + "`n"
    [System.IO.File]::WriteAllText($Path, $Json, $Utf8)
}

function New-ReleaseFixture {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$ModuleId = 'fixture.module',
        [string]$Policy = 'prefer_precompiled'
    )

    $ProjectRoot = Join-Path $Root $Name
    $ArtifactRoot = Join-Path $ProjectRoot 'Saved/ReleaseInput'
    $BindingRoot = Join-Path $ArtifactRoot 'Bindings'
    [void][System.IO.Directory]::CreateDirectory($BindingRoot)

    $WasmPath = Join-Path $ArtifactRoot 'module.wasm'
    $PrecompiledPath = Join-Path $ArtifactRoot 'module.wasmtime.cwasm'
    $DebugMapPath = Join-Path $ArtifactRoot 'debug-map.json'
    $BindingDescriptorPath = Join-Path $BindingRoot 'bindings.json'
    [System.IO.File]::WriteAllBytes($WasmPath, [byte[]](0x00, 0x61, 0x73, 0x6d, 0x01))
    [System.IO.File]::WriteAllBytes($PrecompiledPath, [byte[]](0x63, 0x77, 0x61, 0x73, 0x6d, 0x01))
    Write-TestJson -Path $DebugMapPath -Value ([ordered]@{
        schema_version = 1
        module_id = $ModuleId
        functions = @()
    })
    Write-TestJson -Path $BindingDescriptorPath -Value ([ordered]@{
        schema_version = 23
        module_id = $ModuleId
        imports = @()
    })

    $BindingManifestPath = Join-Path $BindingRoot 'package.json'
    $BindingManifest = [ordered]@{
        schema_version = 1
        descriptor_schema_version = 23
        package_name = 'FixtureBindings'
        package_hash = ('a' * 64)
        descriptor_sha256 = Get-AvidScriptModuleReleaseSha256 $BindingDescriptorPath
        required_imports = @()
        files = [ordered]@{
            descriptor = 'bindings.json'
        }
    }
    Write-TestJson -Path $BindingManifestPath -Value $BindingManifest

    $RuntimeManifestPath = Join-Path $ArtifactRoot 'fixture.avidscript.json'
    $RuntimeManifest = [ordered]@{
        schema_version = 1
        module_id = $ModuleId
        abi_version = 1
        language = 'csharp'
        source = [ordered]@{
            file = 'Source/Fixture.cs'
            semantic_file = 'Saved/ReleaseInput/fixture.semantic.json'
        }
        guest_ir = [ordered]@{
            file = 'Saved/ReleaseInput/fixture.guestir.json'
            sha256 = ('b' * 64)
        }
        semantic = [ordered]@{
            file = 'Saved/ReleaseInput/fixture.semantic.json'
            sha256 = ('c' * 64)
        }
        wasm = [ordered]@{
            file = 'module.wasm'
            sha256 = Get-AvidScriptModuleReleaseSha256 $WasmPath
        }
        execution = [ordered]@{
            format = 'wasmtime_serialized_v1'
            file = 'module.wasmtime.cwasm'
            sha256 = Get-AvidScriptModuleReleaseSha256 $PrecompiledPath
            canonical_sha256 = Get-AvidScriptModuleReleaseSha256 $WasmPath
            compiler_build_identity = 'wasmtime-fixture-build'
            target_triple = 'x86_64-pc-windows-msvc'
            attestation_id = 'editor-process-only'
            policy = $Policy
            fallback = 'wasmtime_jit'
        }
        binding_package = [ordered]@{
            package_name = 'FixtureBindings'
            package_hash = ('a' * 64)
            manifest_file = 'Bindings/package.json'
            manifest_sha256 = Get-AvidScriptModuleReleaseSha256 $BindingManifestPath
            descriptor_file = 'Bindings/bindings.json'
            descriptor_sha256 = Get-AvidScriptModuleReleaseSha256 $BindingDescriptorPath
        }
        debug_map = [ordered]@{
            file = 'debug-map.json'
            sha256 = Get-AvidScriptModuleReleaseSha256 $DebugMapPath
            schema_version = 1
            module_id = $ModuleId
        }
        required_exports = @('avid_on_begin_play')
        required_imports = @()
    }
    Write-TestJson -Path $RuntimeManifestPath -Value $RuntimeManifest
    return [pscustomobject]@{
        ProjectRoot = $ProjectRoot
        ArtifactRoot = $ArtifactRoot
        RuntimeManifestPath = $RuntimeManifestPath
        RuntimeManifest = $RuntimeManifest
        WasmPath = $WasmPath
        PrecompiledPath = $PrecompiledPath
    }
}

function Invoke-ReleaseContract {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    ++$script:Total
    try {
        & $Body
        ++$script:Passed
        Write-Output "PASS $Name"
    }
    catch {
        $Message = "$Name`: $($_.Exception.Message)"
        $script:Failures.Add($Message)
        Write-Output "FAIL $Message"
    }
}

function Assert-ReleaseRejected {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Body,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    $Rejected = $false
    try {
        & $Body
    }
    catch {
        $Rejected = $_.Exception.Message.Contains($Pattern)
    }
    if (-not $Rejected) {
        throw "Expected rejection containing '$Pattern'."
    }
}

try {
    [void][System.IO.Directory]::CreateDirectory($Root)

    Invoke-ReleaseContract 'deterministic repeat publication' {
        $Fixture = New-ReleaseFixture -Name 'Deterministic'
        $First = Publish-AvidScriptModuleReleasePackage `
            -RuntimeManifestPath $Fixture.RuntimeManifestPath `
            -ProjectRoot $Fixture.ProjectRoot `
            -Configuration Development
        $FirstHashes = @(
            Get-ChildItem -LiteralPath $First.PackageRoot -File -Recurse |
                Sort-Object FullName |
                ForEach-Object {
                    $Relative = [System.IO.Path]::GetRelativePath(
                        $First.PackageRoot,
                        $_.FullName).Replace('\', '/')
                    "$Relative=$(Get-AvidScriptModuleReleaseSha256 $_.FullName)"
                }
        )
        $Second = Publish-AvidScriptModuleReleasePackage `
            -RuntimeManifestPath $Fixture.RuntimeManifestPath `
            -ProjectRoot $Fixture.ProjectRoot `
            -Configuration Development
        $SecondHashes = @(
            Get-ChildItem -LiteralPath $Second.PackageRoot -File -Recurse |
                Sort-Object FullName |
                ForEach-Object {
                    $Relative = [System.IO.Path]::GetRelativePath(
                        $Second.PackageRoot,
                        $_.FullName).Replace('\', '/')
                    "$Relative=$(Get-AvidScriptModuleReleaseSha256 $_.FullName)"
                }
        )
        if ($First.PackageId -cne $Second.PackageId -or
            $First.DescriptorSha256 -cne $Second.DescriptorSha256 -or
            [string]::Join("`n", $FirstHashes) -cne [string]::Join("`n", $SecondHashes) -or
            $First.FileCount -ne 7) {
            throw 'Repeated publication changed identity, file set, or hashes.'
        }
        $Descriptor = Get-Content -Raw -LiteralPath $First.DescriptorPath |
            ConvertFrom-Json -Depth 32
        $IdentityValues = @(
            [string]$Descriptor.schema_version,
            [string]$Descriptor.module_id,
            [string]$Descriptor.abi_version,
            [string]$Descriptor.platform,
            [string]$Descriptor.configuration,
            [string]$Descriptor.minimum_runtime_version,
            [string]$Descriptor.execution.backend,
            [string]$Descriptor.execution.format,
            [string]$Descriptor.execution.policy,
            [string]$Descriptor.execution.compiler_build_identity,
            [string]$Descriptor.execution.target_triple,
            [string]$Descriptor.execution.cpu_features,
            [string]$Descriptor.artifacts.runtime_manifest.sha256,
            [string]$Descriptor.artifacts.canonical_wasm.sha256,
            [string]$Descriptor.artifacts.precompiled.sha256,
            [string]$Descriptor.artifacts.binding_manifest.sha256,
            [string]$Descriptor.artifacts.binding_descriptor.sha256,
            [string]$Descriptor.artifacts.debug_map.sha256)
        $ExpectedPackageId = Get-AvidScriptModuleReleaseBytesSha256 (
            [System.Text.Encoding]::UTF8.GetBytes([string]::Join("`n", $IdentityValues)))
        if ([string]$Descriptor.package_id -cne $ExpectedPackageId -or
            [string]$Descriptor.minimum_runtime_version -cne '0.1.0' -or
            [string]$Descriptor.execution.cpu_features -cne 'x86-64-v3') {
            throw 'Package identity payload or frozen compatibility fields are wrong.'
        }
    }

    Invoke-ReleaseContract 'catalog schema and ordinal ordering' {
        $Fixture = New-ReleaseFixture -Name 'CatalogZ' -ModuleId 'zeta.module'
        $First = Publish-AvidScriptModuleReleasePackage `
            -RuntimeManifestPath $Fixture.RuntimeManifestPath `
            -ProjectRoot $Fixture.ProjectRoot `
            -Configuration Development
        $SecondFixture = New-ReleaseFixture -Name 'CatalogA' -ModuleId 'alpha.module'
        $SecondArtifactRoot = Join-Path $Fixture.ProjectRoot 'Saved/AlphaInput'
        Copy-Item -LiteralPath $SecondFixture.ArtifactRoot -Destination $SecondArtifactRoot -Recurse
        $SecondManifestPath = Join-Path $SecondArtifactRoot 'fixture.avidscript.json'
        $Second = Publish-AvidScriptModuleReleasePackage `
            -RuntimeManifestPath $SecondManifestPath `
            -ProjectRoot $Fixture.ProjectRoot `
            -Configuration Development
        $Catalog = Get-Content -Raw -LiteralPath $Second.CatalogPath | ConvertFrom-Json -Depth 32
        $Names = @($Catalog.modules.module_id)
        if ([int]$Catalog.schema_version -ne 1 -or
            $Names.Count -ne 2 -or
            $Names[0] -cne 'alpha.module' -or
            $Names[1] -cne 'zeta.module') {
            throw 'Catalog schema or ordinal module ordering is wrong.'
        }
        foreach ($Entry in @($Catalog.modules)) {
            if ([string]$Entry.descriptor_file -cne
                "$($Entry.module_id)/$($Entry.package_id)/package.json" -or
                [string]$Entry.platform -cne 'win64' -or
                [string]$Entry.configuration -cne 'development') {
                throw 'Catalog entry does not match schema v1.'
            }
        }
        $null = $First
    }

    Invoke-ReleaseContract 'Shipping strips diagnostics and build-only inputs' {
        $Fixture = New-ReleaseFixture -Name 'Shipping'
        $Published = Publish-AvidScriptModuleReleasePackage `
            -RuntimeManifestPath $Fixture.RuntimeManifestPath `
            -ProjectRoot $Fixture.ProjectRoot `
            -Configuration Shipping
        $Descriptor = Get-Content -Raw -LiteralPath $Published.DescriptorPath | ConvertFrom-Json -Depth 32
        $Runtime = Get-Content -Raw -LiteralPath (
            Join-Path $Published.PackageRoot 'runtime.avidscript.json') | ConvertFrom-Json -Depth 32
        if ([string]$Descriptor.configuration -cne 'shipping' -or
            [string]$Descriptor.minimum_runtime_version -cne '0.1.0' -or
            [string]$Descriptor.execution.policy -cne 'require_precompiled' -or
            [string]$Descriptor.execution.cpu_features -cne 'x86-64-v3' -or
            $Descriptor.artifacts.PSObject.Properties.Name -ccontains 'debug_map' -or
            $Runtime.PSObject.Properties.Name -ccontains 'debug_map' -or
            $Runtime.PSObject.Properties.Name -ccontains 'source' -or
            $Runtime.PSObject.Properties.Name -ccontains 'guest_ir' -or
            $Runtime.PSObject.Properties.Name -ccontains 'semantic' -or
            $Runtime.execution.PSObject.Properties.Name -ccontains 'attestation_id' -or
            $Runtime.execution.PSObject.Properties.Name -ccontains 'fallback' -or
            (Test-Path -LiteralPath (Join-Path $Published.PackageRoot 'diagnostics') -PathType Container) -or
            $Published.FileCount -ne 6) {
            throw 'Shipping package retained stripped data or did not force require_precompiled.'
        }
    }

    Invoke-ReleaseContract 'parent path escape is rejected' {
        $Fixture = New-ReleaseFixture -Name 'PathEscape'
        $Fixture.RuntimeManifest.wasm.file = '../module.wasm'
        Write-TestJson -Path $Fixture.RuntimeManifestPath -Value $Fixture.RuntimeManifest
        Assert-ReleaseRejected -Pattern 'parent path segments' -Body {
            Publish-AvidScriptModuleReleasePackage `
                -RuntimeManifestPath $Fixture.RuntimeManifestPath `
                -ProjectRoot $Fixture.ProjectRoot | Out-Null
        }
    }

    Invoke-ReleaseContract 'absolute dependency path is rejected' {
        $Fixture = New-ReleaseFixture -Name 'AbsolutePath'
        $Fixture.RuntimeManifest.execution.file = $Fixture.PrecompiledPath
        Write-TestJson -Path $Fixture.RuntimeManifestPath -Value $Fixture.RuntimeManifest
        Assert-ReleaseRejected -Pattern 'relative path' -Body {
            Publish-AvidScriptModuleReleasePackage `
                -RuntimeManifestPath $Fixture.RuntimeManifestPath `
                -ProjectRoot $Fixture.ProjectRoot | Out-Null
        }
    }

    Invoke-ReleaseContract 'unknown Runtime dependency is rejected' {
        $Fixture = New-ReleaseFixture -Name 'UnknownDependency'
        $Fixture.RuntimeManifest['unknown_dependency'] = [ordered]@{
            file = 'unknown.bin'
            sha256 = ('d' * 64)
        }
        Write-TestJson -Path $Fixture.RuntimeManifestPath -Value $Fixture.RuntimeManifest
        Assert-ReleaseRejected -Pattern 'unknown property' -Body {
            Publish-AvidScriptModuleReleasePackage `
                -RuntimeManifestPath $Fixture.RuntimeManifestPath `
                -ProjectRoot $Fixture.ProjectRoot | Out-Null
        }
    }

    Invoke-ReleaseContract 'non-normalized module id is rejected' {
        $Fixture = New-ReleaseFixture -Name 'ModuleId' -ModuleId 'Fixture.Module'
        Assert-ReleaseRejected -Pattern 'module_id must match' -Body {
            Publish-AvidScriptModuleReleasePackage `
                -RuntimeManifestPath $Fixture.RuntimeManifestPath `
                -ProjectRoot $Fixture.ProjectRoot | Out-Null
        }
    }

    Invoke-ReleaseContract 'incompatible CPU feature profile is rejected' {
        $Fixture = New-ReleaseFixture -Name 'CpuFeatures'
        $Fixture.RuntimeManifest.execution['cpu_features'] = 'x86-64-v2'
        Write-TestJson -Path $Fixture.RuntimeManifestPath -Value $Fixture.RuntimeManifest
        Assert-ReleaseRejected -Pattern 'execution contract is invalid' -Body {
            Publish-AvidScriptModuleReleasePackage `
                -RuntimeManifestPath $Fixture.RuntimeManifestPath `
                -ProjectRoot $Fixture.ProjectRoot | Out-Null
        }
    }

    Invoke-ReleaseContract 'artifact hash mismatch is rejected' {
        $Fixture = New-ReleaseFixture -Name 'HashMismatch'
        $Fixture.RuntimeManifest.wasm.sha256 = ('0' * 64)
        $Fixture.RuntimeManifest.execution.canonical_sha256 = ('0' * 64)
        Write-TestJson -Path $Fixture.RuntimeManifestPath -Value $Fixture.RuntimeManifest
        Assert-ReleaseRejected -Pattern 'SHA-256 mismatch' -Body {
            Publish-AvidScriptModuleReleasePackage `
                -RuntimeManifestPath $Fixture.RuntimeManifestPath `
                -ProjectRoot $Fixture.ProjectRoot | Out-Null
        }
    }

    Invoke-ReleaseContract 'missing precompiled artifact is rejected' {
        $Fixture = New-ReleaseFixture -Name 'MissingPrecompiled'
        Remove-Item -LiteralPath $Fixture.PrecompiledPath
        Assert-ReleaseRejected -Pattern 'dependency is missing' -Body {
            Publish-AvidScriptModuleReleasePackage `
                -RuntimeManifestPath $Fixture.RuntimeManifestPath `
                -ProjectRoot $Fixture.ProjectRoot | Out-Null
        }
    }

    Invoke-ReleaseContract 'extra package file is rejected on reuse' {
        $Fixture = New-ReleaseFixture -Name 'ExtraFile'
        $Published = Publish-AvidScriptModuleReleasePackage `
            -RuntimeManifestPath $Fixture.RuntimeManifestPath `
            -ProjectRoot $Fixture.ProjectRoot
        [System.IO.File]::WriteAllText(
            (Join-Path $Published.PackageRoot 'unexpected.txt'),
            'unexpected',
            $Utf8)
        Assert-ReleaseRejected -Pattern 'missing or extra files' -Body {
            Publish-AvidScriptModuleReleasePackage `
                -RuntimeManifestPath $Fixture.RuntimeManifestPath `
                -ProjectRoot $Fixture.ProjectRoot | Out-Null
        }
    }

    Invoke-ReleaseContract 'duplicate catalog module is rejected' {
        $Fixture = New-ReleaseFixture -Name 'DuplicateCatalog'
        $Published = Publish-AvidScriptModuleReleasePackage `
            -RuntimeManifestPath $Fixture.RuntimeManifestPath `
            -ProjectRoot $Fixture.ProjectRoot
        $Catalog = Get-Content -Raw -LiteralPath $Published.CatalogPath | ConvertFrom-Json -Depth 32
        $Catalog.modules = @($Catalog.modules[0], $Catalog.modules[0])
        Write-TestJson -Path $Published.CatalogPath -Value $Catalog
        Assert-ReleaseRejected -Pattern 'unique and strictly increasing' -Body {
            Publish-AvidScriptModuleReleasePackage `
                -RuntimeManifestPath $Fixture.RuntimeManifestPath `
                -ProjectRoot $Fixture.ProjectRoot | Out-Null
        }
    }
}
catch {
    $Failures.Add("runner setup: $($_.Exception.Message)")
}
finally {
    if (Test-Path -LiteralPath $Root -PathType Container) {
        Remove-Item -LiteralPath $Root -Recurse -Force
    }
}

Write-Output "AvidScript.ModuleReleasePackage: $Passed/$Total passed"
if ($Failures.Count -ne 0 -or $Passed -ne $Total) {
    foreach ($Failure in $Failures) {
        Write-Error $Failure -ErrorAction Continue
    }
    exit 1
}
exit 0
