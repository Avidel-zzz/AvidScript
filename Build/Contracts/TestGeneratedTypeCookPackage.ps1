$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. (Join-Path $BuildRoot 'AvidScriptGeneratedTypeCookPackage.ps1')

$Root = Join-Path ([System.IO.Path]::GetTempPath()) ("AvidScriptCookPackageContract_$PID`_$([Guid]::NewGuid().ToString('N'))")
$Utf8 = [System.Text.UTF8Encoding]::new($false)
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
    $DebugPath = Join-Path $RuntimeRoot 'debug.json'
    $BindingManifestPath = Join-Path $BindingRoot 'package.json'
    $BindingDescriptorPath = Join-Path $BindingRoot 'bindings.json'
    [System.IO.File]::WriteAllText($TypePath, '{"schema_version":5}', $Utf8)
    [System.IO.File]::WriteAllBytes($WasmPath, [byte[]](0x00, 0x61, 0x73, 0x6d))
    [System.IO.File]::WriteAllText($DebugPath, '{"schema_version":1}', $Utf8)
    [System.IO.File]::WriteAllText($BindingDescriptorPath, '{"schema_version":19}', $Utf8)
    $BindingManifest = [ordered]@{
        schema_version = 1
        descriptor_sha256 = Get-AvidScriptCookPackageSha256 $BindingDescriptorPath
        files = [ordered]@{
            descriptor = 'bindings.v5.json'
            reference_source = 'Saved/Bindings/AvidScript.Bindings.generated.cs'
        }
    }
    [System.IO.File]::WriteAllText(
        $BindingManifestPath,
        ($BindingManifest | ConvertTo-Json -Depth 8),
        $Utf8)

    $RuntimePath = Join-Path $RuntimeRoot 'runtime.json'
    $Runtime = [ordered]@{
        source = [ordered]@{
            file = 'Plugins/AvidScript/Samples/CSharp/Test.cs'
            frontend_file = 'Saved/Runtime/frontend.json'
            semantic_file = 'Saved/Runtime/semantic.json'
        }
        wasm = [ordered]@{ file = 'module.wasm'; sha256 = Get-AvidScriptCookPackageSha256 $WasmPath }
        debug_map = [ordered]@{ file = 'debug.json'; sha256 = Get-AvidScriptCookPackageSha256 $DebugPath }
        binding_package = [ordered]@{
            manifest_file = 'Saved/Bindings/package.json'
            manifest_sha256 = Get-AvidScriptCookPackageSha256 $BindingManifestPath
            descriptor_file = 'Saved/Bindings/bindings.json'
            descriptor_sha256 = Get-AvidScriptCookPackageSha256 $BindingDescriptorPath
            reference_source_file = 'Saved/Bindings/AvidScript.Bindings.generated.cs'
        }
        guest_ir = [ordered]@{ file = 'Saved/Runtime/module.guestir.json' }
    }
    [System.IO.File]::WriteAllText($RuntimePath, ($Runtime | ConvertTo-Json -Depth 8), $Utf8)
    $DescriptorPath = Join-Path $SourceRoot 'package.json'
    $Descriptor = [ordered]@{
        schema_version = 1
        package_id = ('a' * 64)
        module_name = 'AvidScriptGenerated'
        runtime_module_id = 'avidscript_generated'
        execution_backend = 'wasmtime_jit'
        generation_key_sha256 = ('b' * 64)
        type_manifest = [ordered]@{ file = 'types.json'; sha256 = Get-AvidScriptCookPackageSha256 $TypePath }
        runtime_manifest = [ordered]@{ file = '../Saved/Runtime/runtime.json'; sha256 = Get-AvidScriptCookPackageSha256 $RuntimePath }
        reload = [ordered]@{ native_structure_sha256 = ('c' * 64) }
    }
    $Descriptor.runtime_manifest.file = [System.IO.Path]::GetRelativePath($SourceRoot, $RuntimePath).Replace('\', '/')
    [System.IO.File]::WriteAllText($DescriptorPath, ($Descriptor | ConvertTo-Json -Depth 8), $Utf8)

    $First = Publish-AvidScriptGeneratedTypeCookPackage -PackageDescriptorPath $DescriptorPath -ProjectRoot $ProjectRoot -OutputRoot $OutputRoot
    $Second = Publish-AvidScriptGeneratedTypeCookPackage -PackageDescriptorPath $DescriptorPath -ProjectRoot $ProjectRoot -OutputRoot $OutputRoot
    if ($First.PackageId -cne $Second.PackageId -or
        $First.FileCount -ne 7 -or
        -not (Test-Path -LiteralPath $First.DescriptorPath -PathType Leaf)) {
        throw 'Cook package publication is not deterministic.'
    }
    $Current = Get-Content -Raw -LiteralPath $First.DescriptorPath | ConvertFrom-Json
    $CookRuntimeJson = Get-Content -Raw -LiteralPath (Join-Path $First.BundleRoot 'runtime-manifest.json')
    $CookRuntime = $CookRuntimeJson | ConvertFrom-Json
    $CookBindingJson = Get-Content -Raw -LiteralPath (Join-Path $First.BundleRoot 'bindings\package.json')
    $CookBinding = $CookBindingJson | ConvertFrom-Json
    if ([string]$Current.package_id -cne $First.PackageId -or
        [string]$Current.type_manifest.file -cne "$($First.PackageId)/type-manifest.json" -or
        [string]$CookRuntime.wasm.file -cne 'generated_types.wasm' -or
        [string]$CookRuntime.debug_map.file -cne 'generated_types.debug.json' -or
        [string]$CookRuntime.binding_package.manifest_file -cne 'bindings/package.json' -or
        [string]$CookRuntime.binding_package.descriptor_file -cne 'bindings/bindings.json' -or
        [string]$CookBinding.files.descriptor -cne 'bindings.json' -or
        (Get-Content -Raw -LiteralPath $First.DescriptorPath).Contains('Saved/') -or
        $CookRuntimeJson.Contains('Saved/') -or
        $CookBindingJson.Contains('Saved/')) {
        throw 'Cook package contains a non-portable runtime path.'
    }

    $OutsideRoot = Join-Path $Root 'OutsideProject'
    [void][System.IO.Directory]::CreateDirectory($OutsideRoot)
    $OutsideArtifactPath = Join-Path $OutsideRoot 'outside.json'
    [System.IO.File]::WriteAllText($OutsideArtifactPath, '{}', $Utf8)
    $RejectedOutsideProject = $false
    try {
        Resolve-AvidScriptCookPackageArtifactPath `
            -ManifestPath $DescriptorPath `
            -ArtifactPath $OutsideArtifactPath `
            -ProjectRoot $ProjectRoot | Out-Null
    }
    catch {
        $RejectedOutsideProject = $_.Exception.Message.Contains('outside the project root')
    }
    if (-not $RejectedOutsideProject) {
        throw 'Cook package publication accepted an artifact outside the project root.'
    }

    [System.IO.File]::WriteAllBytes($WasmPath, [byte[]](0x00, 0x61, 0x73, 0x6d, 0x01))
    $RejectedTamper = $false
    try {
        Publish-AvidScriptGeneratedTypeCookPackage -PackageDescriptorPath $DescriptorPath -ProjectRoot $ProjectRoot -OutputRoot $OutputRoot | Out-Null
    }
    catch {
        $RejectedTamper = $_.Exception.Message.Contains('WASM SHA-256')
    }
    if (-not $RejectedTamper) {
        throw 'Cook package publication accepted a tampered WASM artifact.'
    }

    Write-Output 'Generated type Cook package contracts: PASS'
}
finally {
    if (Test-Path -LiteralPath $Root -PathType Container) {
        Remove-Item -LiteralPath $Root -Recurse -Force
    }
}
