# Loaded by the trusted release resolver, never from the package being inspected.
. (Join-Path $PSScriptRoot 'AvidScriptWasmtimeNotices.ps1')

function Get-AvidScriptWin64RuntimeRelativePath {
    param([Parameter(Mandatory)][string]$ToolchainId)

    # Explicit compatibility list lets the current installer verify an older
    # offline package during upgrade without accepting package-selected paths.
    switch -CaseSensitive ($ToolchainId) {
        'avidscript-wasmtime-v45.0.0-patchset.2-win64-multiarch' {
            return 'Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0-avidscript.2'
        }
        'avidscript-wasmtime-v45.0.0-patchset.3-win64-multiarch' {
            return 'Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0-avidscript.3'
        }
        default { throw 'ASWB1001 unsupported Win64 runtime toolchain' }
    }
}

function Get-AvidScriptWin64BundleIdentity {
    param(
        [Parameter(Mandatory)][string]$PluginRoot,
        [Parameter(Mandatory)][string]$RuntimeRoot
    )

    Assert-AvidScriptPluginReleaseOrdinaryTree -Root $RuntimeRoot -Label 'bundled Win64 runtime'
    $lockRelative = 'Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.lock.json'
    $lockPath = Join-Path $PluginRoot $lockRelative
    $lock = Read-AvidScriptPluginReleaseJsonObject $lockPath 'bundled runtime source lock'
    Assert-AvidScriptPluginReleaseJsonSchema -JsonPath $lockPath `
        -SchemaPath (Join-Path $script:AvidScriptPluginReleaseModuleRoot '../../Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.schema.json') `
        -Label 'bundled runtime source lock'
    # Fixed layout is part of this profile, not an arbitrary path supplied by a package.
    $runtimeRelative = Get-AvidScriptWin64RuntimeRelativePath $lock.toolchain_id
    $markerName = '.avidscript-wasmtime-performance-managed.json'
    $patchRelative = 'Source/ThirdParty/Wasmtime/PerformanceToolchain/avidscript-wasmtime-v45-inlining.patch'
    if ($lock.platform -cne 'Win64' -or $lock.install.relative_path -cne $runtimeRelative -or
        $lock.install.managed_marker_name -cne $markerName -or $lock.patch.relative_path -cne $patchRelative) {
        throw 'ASWB1001 unsupported bundled runtime layout'
    }
    $patchText = [IO.File]::ReadAllText((Join-Path $PluginRoot $patchRelative)).Replace("`r`n", "`n")
    $patchHash = Get-AvidScriptPluginReleaseBytesSha256 ([Text.UTF8Encoding]::new($false).GetBytes($patchText))
    if ($patchHash -cne $lock.patch.canonical_sha256) { throw 'ASWB1002 bundled runtime patch differs from source lock' }
    $markerPath = Join-Path $RuntimeRoot $markerName
    $marker = Read-AvidScriptPluginReleaseJsonObject $markerPath 'bundled runtime managed marker'
    Assert-AvidScriptPluginReleaseObjectShape -Value $marker -Required @(
        'schema_version', 'toolchain_id', 'source_sha256', 'patch_sha256', 'rust_toolchain',
        'build_profile', 'compiler_profile', 'dll_sha256', 'installed_content_sha256') -Label 'bundled runtime managed marker'
    if ($marker.schema_version -ne 1 -or $marker.toolchain_id -cne $lock.toolchain_id -or
        $marker.source_sha256 -cne $lock.upstream.source_archive.sha256 -or $marker.patch_sha256 -cne $patchHash -or
        $marker.rust_toolchain -cne $lock.rust.toolchain -or $marker.build_profile -cne $lock.rust.build_profile -or
        $marker.compiler_profile -cne $lock.compiler_profile.id) { throw 'ASWB1002 bundled runtime producer identity differs' }
    foreach ($required in @('include/wasmtime.h', 'include/wasmtime/config.h', 'LICENSE',
            'lib/wasmtime.dll', 'lib/wasmtime.dll.lib', 'lib/wasmtime.lib', 'notices/manifest.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $RuntimeRoot $required) -PathType Leaf)) {
            throw "ASWB1003 bundled runtime file is missing: $required"
        }
    }
    # Match the producer's installed-content contract, including its ordering and newline format.
    $lines = [Collections.Generic.List[string]]::new()
    foreach ($file in Get-ChildItem -LiteralPath $RuntimeRoot -File -Recurse -Force | Sort-Object FullName) {
        $relative = [IO.Path]::GetRelativePath($RuntimeRoot, $file.FullName).Replace('\', '/')
        if ($relative -ceq $markerName) { continue }
        $lines.Add("$relative|$($file.Length)|$(Get-AvidScriptPluginReleaseSha256 $file.FullName)")
    }
    $contentHash = Get-AvidScriptPluginReleaseBytesSha256 ([Text.UTF8Encoding]::new($false).GetBytes(
        [string]::Join("`n", $lines) + "`n"))
    $dllHash = Get-AvidScriptPluginReleaseSha256 (Join-Path $RuntimeRoot 'lib/wasmtime.dll')
    if ($contentHash -cne $marker.installed_content_sha256 -or $dllHash -cne $marker.dll_sha256) {
        throw 'ASWB1004 bundled runtime bytes differ from managed marker'
    }
    $noticesPath = Join-Path $RuntimeRoot 'notices/manifest.json'
    $noticesHash = Get-AvidScriptPluginReleaseSha256 $noticesPath
    $null = Read-AvidScriptPluginReleaseJsonObject $noticesPath 'bundled runtime notices'
    $notices = Test-AvidScriptWasmtimeNotices -BundleRoot (Join-Path $RuntimeRoot 'notices')
    if ($noticesHash -cne (Get-AvidScriptPluginReleaseSha256 $noticesPath) -or
        $notices.toolchain_id -cne $lock.toolchain_id -or
        $notices.source_archive_sha256 -cne $lock.upstream.source_archive.sha256 -or
        $notices.patch_sha256 -cne $patchHash) { throw 'ASWB1005 bundled runtime notice provenance differs' }
    foreach ($name in @('wasmtime.dll', 'wasmtime.dll.lib', 'wasmtime.lib')) {
        if ($notices.runtime.$name -cne (Get-AvidScriptPluginReleaseSha256 (Join-Path $RuntimeRoot "lib/$name"))) {
            throw 'ASWB1005 bundled runtime notice binary identity differs'
        }
    }
    foreach ($reference in @($notices.packages | ForEach-Object notices) + @($notices.rust.notices)) {
        if ([IO.Path]::GetFileNameWithoutExtension($reference.path) -cne $reference.sha256) {
            throw 'ASWB1005 bundled notice filename differs from content hash'
        }
    }
    return [pscustomobject][ordered]@{
        root = "AvidScript/$runtimeRelative"
        marker_sha256 = Get-AvidScriptPluginReleaseSha256 $markerPath
        installed_content_sha256 = $contentHash
        notices_sha256 = $noticesHash
    }
}

function Assert-AvidScriptWin64BundledDependency {
    param([Parameter(Mandatory)][string]$PayloadRoot, [Parameter(Mandatory)]$Dependency)
    $expectedLock = 'AvidScript/Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.lock.json'
    if ($Dependency.id -cne 'wasmtime-win64' -or $Dependency.mode -cne 'bundled' -or
        $Dependency.identity_path -cne $expectedLock) {
        throw 'ASWB1001 unsupported bundled dependency'
    }
    $lock = Read-AvidScriptPluginReleaseJsonObject (Join-Path $PayloadRoot $expectedLock) 'bundled dependency lock'
    $expectedRoot = 'AvidScript/' + (Get-AvidScriptWin64RuntimeRelativePath $lock.toolchain_id)
    if ($Dependency.bundle.root -cne $expectedRoot) { throw 'ASWB1001 bundled root differs from its toolchain' }
    if ($Dependency.version -cne $lock.toolchain_id) { throw 'ASWB1002 bundled dependency version differs' }
    $actual = Get-AvidScriptWin64BundleIdentity -PluginRoot (Join-Path $PayloadRoot 'AvidScript') `
        -RuntimeRoot (Join-Path $PayloadRoot $expectedRoot)
    if ((Get-AvidScriptPluginReleaseBytesSha256 (ConvertTo-AvidScriptPluginReleaseCanonicalJsonBytes $actual)) -cne
        (Get-AvidScriptPluginReleaseBytesSha256 (ConvertTo-AvidScriptPluginReleaseCanonicalJsonBytes $Dependency.bundle))) {
        throw 'ASWB1004 bundled dependency identity differs from release manifest'
    }
    # WAMR is tracked source plus a tracked Win64 library; the outer payload inventory pins both.
    foreach ($required in @('Source/ThirdParty/WAMR/lib/Win64/Release/libiwasm.lib',
            'Source/ThirdParty/WAMR/upstream/LICENSE', 'Source/ThirdParty/WAMR/upstream/core/iwasm/include/wasm_export.h')) {
        if (-not (Test-Path -LiteralPath (Join-Path $PayloadRoot "AvidScript/$required") -PathType Leaf)) {
            throw "ASWB1003 offline profile requires WAMR source, license and library: $required"
        }
    }
}
