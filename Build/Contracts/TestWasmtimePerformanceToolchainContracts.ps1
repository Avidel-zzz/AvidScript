[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PluginRoot = [System.IO.Path]::GetFullPath(
    (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$BuilderPath = Join-Path $PluginRoot 'Build/BuildAvidScriptWasmtimePerformanceToolchain.ps1'
$ToolchainRoot = Join-Path $PluginRoot 'Source/ThirdParty/Wasmtime/PerformanceToolchain'
$LockPath = Join-Path $ToolchainRoot 'WasmtimePerformanceToolchain.lock.json'
$SchemaPath = Join-Path $ToolchainRoot 'WasmtimePerformanceToolchain.schema.json'
$PatchPath = Join-Path $ToolchainRoot 'avidscript-wasmtime-v45-inlining.patch'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "ASP57WT1000 $Message"
    }
}

function Get-CanonicalTextSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Text = [System.IO.File]::ReadAllText($Path)
    if ($Text.Length -gt 0 -and $Text[0] -eq [char]0xFEFF) {
        $Text = $Text.Substring(1)
    }
    $Canonical = $Text.Replace("`r`n", "`n").Replace("`r", "`n")
    $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Canonical)
    return [Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

foreach ($Path in @($BuilderPath, $LockPath, $SchemaPath, $PatchPath)) {
    Assert-True (Test-Path -LiteralPath $Path -PathType Leaf) "required file is missing: $Path"
}
$Tokens = $null
$Errors = $null
[System.Management.Automation.Language.Parser]::ParseFile(
    $BuilderPath,
    [ref]$Tokens,
    [ref]$Errors) | Out-Null
Assert-True ($Errors.Count -eq 0) 'performance toolchain builder has parser errors'

$RawLock = Get-Content -LiteralPath $LockPath -Raw
Assert-True ($RawLock | Test-Json -SchemaFile $SchemaPath -ErrorAction SilentlyContinue) `
    'performance toolchain lock does not satisfy its schema'
$Lock = $RawLock | ConvertFrom-Json
Assert-True ([string]$Lock.upstream.commit -ceq '377cd917af258d932d55b201a646917ecf193639') `
    'upstream commit drifted'
Assert-True ([string]$Lock.rust.toolchain -ceq '1.93.0-x86_64-pc-windows-msvc') `
    'Rust toolchain drifted'
Assert-True ([string]$Lock.rust.build_profile -ceq 'fastest-runtime') `
    'runtime build profile drifted'
Assert-True (@($Lock.rust.features) -join ',' -ceq 'cranelift,parallel-compilation,disable-logging') `
    'Cargo feature set drifted'
Assert-True ([string]$Lock.compiler_profile.optimization -ceq 'speed_and_size') `
    'Cranelift optimization drifted'
Assert-True ([string]$Lock.compiler_profile.register_allocator -ceq 'backtracking') `
    'Cranelift register allocator drifted'
Assert-True ([string]$Lock.compiler_profile.inlining -ceq 'all') `
    'compiler inlining drifted'
Assert-True ([string]$Lock.compiler_profile.cpu -ceq 'x86-64-v3') `
    'CPU profile drifted'
Assert-True ([bool]$Lock.compiler_profile.spectre_mitigation) `
    'Spectre mitigation must remain enabled'
Assert-True (-not [bool]$Lock.compiler_profile.nan_canonicalization) `
    'NaN canonicalization contract drifted'

$PatchSha256 = Get-CanonicalTextSha256 -Path $PatchPath
Assert-True ($PatchSha256 -ceq [string]$Lock.patch.canonical_sha256) `
    'patch digest differs from the lock'
$PatchText = Get-Content -LiteralPath $PatchPath -Raw
$PatchedFiles = @(
    [regex]::Matches($PatchText, '(?m)^diff --git a/(?<path>\S+) b/\S+$') |
        ForEach-Object { $_.Groups['path'].Value })
Assert-True ($PatchedFiles.Count -eq 2) 'patch must touch exactly two upstream C API files'
Assert-True ($PatchedFiles -contains 'crates/c-api/src/config.rs') `
    'patch must own the Rust C API implementation'
Assert-True ($PatchedFiles -contains 'crates/c-api/include/wasmtime/config.h') `
    'patch must own the public C API declaration'
$ExportCount = ([regex]::Matches(
    $PatchText,
    'avidscript_wasmtime_config_compiler_inlining_set')).Count
Assert-True ($ExportCount -eq 2) 'extension symbol must occur once in each patched file'
Assert-True (-not $PatchText.Contains('enable_heap_access_spectre_mitigation')) `
    'patch must not weaken heap Spectre mitigation'
Assert-True (-not $PatchText.Contains('enable_table_access_spectre_mitigation')) `
    'patch must not weaken table Spectre mitigation'

$BuilderText = Get-Content -LiteralPath $BuilderPath -Raw
foreach ($RequiredLiteral in @(
    "'git'",
    "'apply', '--check', '--unidiff-zero'",
    'Assert-PathWithin',
    'SOURCE_DATE_EPOCH',
    'RUSTUP_TOOLCHAIN',
    'TryGetExport',
    'installed_content_sha256')) {
    Assert-True $BuilderText.Contains($RequiredLiteral) `
        "builder lacks required contract literal: $RequiredLiteral"
}
$GitIgnore = Get-Content -LiteralPath (Join-Path $PluginRoot '.gitignore') -Raw
Assert-True $GitIgnore.Contains('Source/ThirdParty/Wasmtime/installed/') `
    'generated Wasmtime managed layouts must remain ignored'

$ValidationOutput = & pwsh -NoProfile -ExecutionPolicy Bypass `
    -File $BuilderPath -Mode ValidateLock
Assert-True ($LASTEXITCODE -eq 0) 'ValidateLock child process failed'
$Validation = ($ValidationOutput -join [System.Environment]::NewLine) | ConvertFrom-Json
Assert-True ([string]$Validation.result -ceq 'wasmtime_performance_toolchain_lock_valid') `
    'ValidateLock result identity is invalid'
Assert-True ([string]$Validation.patch_sha256 -ceq $PatchSha256) `
    'ValidateLock did not report the locked patch digest'

[pscustomobject]@{
    result = 'wasmtime_performance_toolchain_contracts_passed'
    assertion_count = 30
    toolchain_id = [string]$Lock.toolchain_id
    source_sha256 = [string]$Lock.upstream.source_archive.sha256
    patch_sha256 = $PatchSha256
} | ConvertTo-Json -Depth 4
