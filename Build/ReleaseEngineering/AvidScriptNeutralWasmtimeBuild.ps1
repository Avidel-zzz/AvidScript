# Explicit, opt-in build policy. This helper never changes the VM's runtime safety or optimization profile.
. (Join-Path $PSScriptRoot 'AvidScriptBinaryPrivacy.ps1')

function Assert-AvidScriptNeutralBuildInputs {
    param([string]$RepositoryRoot, [string]$CacheRoot, [string]$InstallPath)
    if (-not $IsWindows) { throw 'ASNB1001 neutral Wasmtime builds require Windows' }
    $profile = [Environment]::GetFolderPath('UserProfile').TrimEnd('\', '/')
    foreach ($path in @($RepositoryRoot, $CacheRoot)) {
        if (-not [IO.Path]::IsPathFullyQualified($path)) { throw 'ASNB1001 neutral roots must be absolute' }
        $full = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($path))
        if ($full -eq [IO.Path]::GetPathRoot($full) -or $full -eq $profile -or
            $full.StartsWith($profile + '\', [StringComparison]::OrdinalIgnoreCase) -or
            $full -match '(?i)^[a-z]:[\\/]Users[\\/]') {
            throw 'ASNB1001 neutral roots must be outside user homes and cannot be drive roots'
        }
        Assert-AvidScriptBinaryPrivacyAncestors -Path $full
        $cursor = $full
        while ($cursor) {
            foreach ($name in @('.cargo/config', '.cargo/config.toml')) {
                if (Test-Path -LiteralPath (Join-Path $cursor $name)) {
                    throw 'ASNB1002 inherited Cargo configuration is not allowed'
                }
            }
            $cursor = [IO.Path]::GetDirectoryName($cursor)
        }
    }
    $repo = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($RepositoryRoot))
    $cache = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($CacheRoot))
    if ($repo.Equals($cache, [StringComparison]::OrdinalIgnoreCase) -or
        $repo.StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $cache.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ASNB1001 repository and neutral cache must be disjoint'
    }
    # This mode only publishes a new isolated install. An existing runtime is never removed or replaced.
    if (Test-Path -LiteralPath $InstallPath) { throw 'ASNB1003 neutral install destination must not already exist' }
    Assert-AvidScriptBinaryPrivacyAncestors -Path $InstallPath
    foreach ($variable in Get-ChildItem Env:) {
        if ($variable.Name -match '^(RUSTFLAGS|RUSTDOCFLAGS|RUSTC|RUSTDOC|RUSTC_WRAPPER|RUSTC_WORKSPACE_WRAPPER|CARGO_ENCODED_RUSTFLAGS|CARGO_ENCODED_RUSTDOCFLAGS|CARGO_BUILD_.*|CARGO_TARGET_.*|CARGO_PROFILE_.*|CC|CXX|AR|CFLAGS|CXXFLAGS|CPPFLAGS|LDFLAGS|CL|_CL_|LINK|_LINK_|CMAKE_ARGS|CMAKE_TOOLCHAIN_FILE|WASMTIME_.*)$' -and
            -not [string]::IsNullOrEmpty($variable.Value)) {
            throw 'ASNB1002 inherited compiler or linker configuration is not allowed'
        }
    }
    if (Test-Path -LiteralPath $cache) {
        if (@(Get-ChildItem -LiteralPath $cache -Force).Count -gt 0) {
            throw 'ASNB1003 neutral build requires a fresh empty cache'
        }
    }
}

function Save-AvidScriptWasmtimeEnvironment {
    $values = @{}
    foreach ($name in @('RUSTUP_TOOLCHAIN', 'CARGO_HOME', 'CARGO_TARGET_DIR', 'CARGO_ENCODED_RUSTFLAGS', 'SOURCE_DATE_EPOCH', 'TEMP', 'TMP')) {
        $values[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    return $values
}

function Restore-AvidScriptWasmtimeEnvironment {
    param([hashtable]$Values)
    foreach ($name in $Values.Keys) {
        $value = if ($null -eq $Values[$name]) { [NullString]::Value } else { $Values[$name] }
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}

function New-AvidScriptNeutralRustFlags {
    param([System.Collections.IDictionary]$Mappings)
    $flags = [Collections.Generic.List[string]]::new()
    foreach ($mapping in @($Mappings.GetEnumerator() | Sort-Object { $_.Key.Length })) {
        $source = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath([string]$mapping.Key))
        $target = [string]$mapping.Value
        if ($source.Contains('=') -or $source.Contains([string][char]31) -or $target -notmatch '^/avidscript/[a-z-]+$') {
            throw 'ASNB1001 invalid path mapping'
        }
        foreach ($variant in @($source.Replace('\', '/'), $source.Replace('/', '\')) | Select-Object -Unique) {
            $flags.Add("--remap-path-prefix=$variant=$target")
        }
    }
    return [string]::Join([char]31, $flags)
}

function Set-AvidScriptNeutralWasmtimeEnvironment {
    param([string]$RepositoryRoot, [string]$CacheRoot, [string]$SourceRoot, [string]$CargoTargetRoot)
    $cargoHome = Join-Path $CacheRoot 'cargo-home'
    $temporary = Join-Path $CacheRoot 'temp'
    $sysroot = (& rustc --print sysroot | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not [IO.Path]::IsPathFullyQualified($sysroot)) { throw 'ASNB1004 pinned Rust sysroot is unavailable' }
    foreach ($directory in @($cargoHome, $temporary)) {
        Assert-AvidScriptBinaryPrivacyAncestors -Path $directory
        [void][IO.Directory]::CreateDirectory($directory)
    }
    foreach ($name in @('config', 'config.toml')) {
        if (Test-Path -LiteralPath (Join-Path $cargoHome $name)) { throw 'ASNB1002 neutral Cargo home contains configuration overrides' }
    }
    $mappings = [ordered]@{}
    $mappings[[Environment]::GetFolderPath('UserProfile')] = '/avidscript/host'
    $mappings[$RepositoryRoot] = '/avidscript/repository'
    $mappings[$CacheRoot] = '/avidscript/build'
    $mappings[$SourceRoot] = '/avidscript/source'
    $mappings[$cargoHome] = '/avidscript/cargo'
    $mappings[$CargoTargetRoot] = '/avidscript/target'
    $mappings[$temporary] = '/avidscript/temp'
    $mappings[$sysroot] = '/avidscript/rust'
    $env:CARGO_ENCODED_RUSTFLAGS = New-AvidScriptNeutralRustFlags -Mappings $mappings
    $env:CARGO_HOME = $cargoHome
    $env:TEMP = $temporary
    $env:TMP = $temporary
    return @($mappings.Keys)
}

function Test-AvidScriptNeutralRustFlags {
    param([string]$CacheRoot)
    $probeRoot = Join-Path $CacheRoot ('probes/' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($probeRoot)
    $source = Join-Path $probeRoot 'path_probe.rs'
    $output = Join-Path $probeRoot 'path_probe.rmeta'
    [IO.File]::WriteAllText($source, 'pub const BUILD_SOURCE: &str = file!();', [Text.UTF8Encoding]::new($false))
    $arguments = @('--crate-name', 'path_probe', '--crate-type', 'lib', '--emit=metadata', $source, '-o', $output) +
        @($env:CARGO_ENCODED_RUSTFLAGS.Split([char]31))
    & rustc @arguments
    if ($LASTEXITCODE -ne 0) { throw 'ASNB1004 pinned Rust rejected path mapping arguments' }
    $bytes = [IO.File]::ReadAllBytes($output)
    $text = [Text.Encoding]::UTF8.GetString($bytes)
    if (-not $text.Replace('\', '/').Contains('/avidscript/build/probes/') -or $text.Contains($CacheRoot) -or $text.Contains($CacheRoot.Replace('\', '/'))) {
        throw 'ASNB1004 pinned Rust path mapping probe failed'
    }
}
