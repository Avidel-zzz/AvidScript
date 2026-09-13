[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BaselineSourceRoot,
    [Parameter(Mandatory)][string]$WorkRoot,
    [Parameter(Mandatory)][string]$CargoHome
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture -ne 'X64') {
    throw 'This diagnostic requires a native Win64 host.'
}
$pluginRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../../..'))
$BaselineSourceRoot = [IO.Path]::GetFullPath($BaselineSourceRoot)
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$CargoHome = [IO.Path]::GetFullPath($CargoHome)
if (Test-Path -LiteralPath $WorkRoot) { throw 'WorkRoot must not exist; evidence is never overwritten.' }
foreach ($root in @($BaselineSourceRoot, $pluginRoot, $CargoHome)) {
    if ($WorkRoot.StartsWith($root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'WorkRoot must be outside the source, repository and Cargo cache.'
    }
}

function Get-Sha([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Write-Text([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

# Input is the existing archive plus the locked patchset. Reject a changed
# baseline before making an isolated copy. This is a diagnostic, not a managed
# install or a new production runtime identity.
$expected = [ordered]@{
    'crates/cranelift/src/lib.rs' = 'b73dd582b4110e114d4ce799ea19b9a6638f2eeeca3ff91b91b48960e5a068af'
    'crates/cranelift/src/func_environ.rs' = 'd5eef71c1ca4cc51cbf9a666c9f3edbda3f8c5927970de9a64cd1d01302342da'
    'crates/cranelift/src/compiler.rs' = '470c5a3766ac469ac997f1b735f57d62994efd76e2370fa8662a9bfbf3a523d2'
    'Cargo.lock' = 'bd11853bef07fa76f9ba1997c5a9844f8036b8819785601b9b8637d76fd173ee'
}
foreach ($relative in $expected.Keys) {
    if ((Get-Sha (Join-Path $BaselineSourceRoot $relative)) -cne $expected[$relative]) {
        throw "Unexpected baseline input: $relative"
    }
}
$toolchain = '1.93.0-x86_64-pc-windows-msvc'
$rustVersion = (& rustc "+$toolchain" --version) -join ' '
if ($LASTEXITCODE -ne 0 -or $rustVersion -notmatch '^rustc 1\.93\.0 ') { throw 'Locked Rust toolchain is unavailable.' }

New-Item -ItemType Directory -Path $WorkRoot | Out-Null
$source = Join-Path $WorkRoot 'source'
Copy-Item -LiteralPath $BaselineSourceRoot -Destination $source -Recurse
$localPatch = Join-Path $WorkRoot 'epoch-preserve-win64.patch'
Write-Text $localPatch ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'epoch-preserve-win64.patch')).Replace("`r`n", "`n"))
& git -C $source apply --check $localPatch
if ($LASTEXITCODE -ne 0) { throw 'Epoch patch precondition failed.' }
& git -C $source apply $localPatch
if ($LASTEXITCODE -ne 0) { throw 'Epoch patch application failed.' }

# Bind every copied source file, not just the three touched compiler files.
$sourceInventory = @(Get-ChildItem -LiteralPath $source -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path = [IO.Path]::GetRelativePath($source, $_.FullName).Replace('\', '/'); sha256 = Get-Sha $_.FullName }
})
$inventoryPath = Join-Path $WorkRoot 'source-inventory.json'
Write-Text $inventoryPath ($sourceInventory | ConvertTo-Json -Depth 5)
$probe = Join-Path $WorkRoot 'probe'
New-Item -ItemType Directory -Path (Join-Path $probe 'src') | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'EpochPreserveProbe.rs') -Destination (Join-Path $probe 'src/main.rs')
Copy-Item -LiteralPath (Join-Path $source 'Cargo.lock') -Destination (Join-Path $probe 'Cargo.lock')
$dependencyPath = (Join-Path $source 'crates/wasmtime').Replace('\', '/') | ConvertTo-Json -Compress
Write-Text (Join-Path $probe 'Cargo.toml') @"
[package]
name = "avidscript-epoch-preserve-probe"
version = "0.1.0"
edition = "2024"
[dependencies]
wasmtime = { path = $dependencyPath, default-features = false, features = ["std", "runtime", "cranelift", "gc", "gc-drc"] }
[profile.release]
opt-level = 3
debug = false
"@

$previousCargoHome = $env:CARGO_HOME
$previousTargetDir = $env:CARGO_TARGET_DIR
try {
    $env:CARGO_HOME = $CargoHome
    $env:CARGO_TARGET_DIR = Join-Path $WorkRoot 'target'
    $buildLog = Join-Path $WorkRoot 'build.log'
    # The upstream lock seeds exact dependencies (including pinned yanked
    # versions); Cargo prunes unrelated workspace packages for this probe.
    & cargo "+$toolchain" build --offline --release --manifest-path (Join-Path $probe 'Cargo.toml') *> $buildLog
    if ($LASTEXITCODE -ne 0) { throw "Diagnostic build failed; see $buildLog" }
    $executable = Join-Path $env:CARGO_TARGET_DIR 'release/avidscript-epoch-preserve-probe.exe'
    $exeHash = Get-Sha $executable
    $kernelRoot = Join-Path $pluginRoot 'Benchmarks/PuertsComparison/ControlledRuntime/Kernel'
    $kernels = @('scalar_float', 'simd128', 'mixed_gameplay', 'data_branch')
    $kernelHashes = [ordered]@{}
    foreach ($kernel in $kernels) { $kernelHashes[$kernel] = Get-Sha (Join-Path $kernelRoot "suite_$kernel.wasm") }
    $artifactRoot = Join-Path $WorkRoot 'artifacts'
    $runLog = Join-Path $WorkRoot 'probe.log'
    # The child owns a 60-second watchdog, including its infinite-loop test.
    & $executable $kernelRoot $artifactRoot *> $runLog
    if ($LASTEXITCODE -ne 0) { throw "Diagnostic execution failed; see $runLog" }
    $runText = [IO.File]::ReadAllText($runLog)
    if ($runText -notmatch '(?m)^Epoch preserve probe: passed=17/17 failed=0\r?$' -or
        ([regex]::Matches($runText, '(?m)^PASS ')).Count -ne 17) { throw 'Incomplete probe results.' }
    if ((Get-Sha $executable) -cne $exeHash) { throw 'Probe executable changed during execution.' }
    foreach ($kernel in $kernels) {
        if ((Get-Sha (Join-Path $kernelRoot "suite_$kernel.wasm")) -cne $kernelHashes[$kernel]) { throw 'Kernel changed during execution.' }
    }
    $artifacts = @(Get-ChildItem -LiteralPath $artifactRoot -File | Sort-Object Name | ForEach-Object {
        [ordered]@{ path = $_.Name; sha256 = Get-Sha $_.FullName; length = $_.Length }
    })
    if ($artifacts.Count -ne 8) { throw 'Expected eight compiled artifacts.' }
    $report = [ordered]@{
        schema_version = 1
        evidence_class = 'diagnostic_epoch_semantics'
        repository_commit = ((& git -C $pluginRoot rev-parse HEAD) -join '').Trim()
        repository_tree = ((& git -C $pluginRoot rev-parse 'HEAD^{tree}') -join '').Trim()
        repository_clean = @(& git -C $pluginRoot status --porcelain=v1).Count -eq 0
        passed = 17
        total = 17
        rustc = $rustVersion
        source_inventory_sha256 = Get-Sha $inventoryPath
        baseline_inputs = $expected
        patch_canonical_sha256 = Get-Sha $localPatch
        probe_source_sha256 = Get-Sha (Join-Path $probe 'src/main.rs')
        probe_lock_sha256 = Get-Sha (Join-Path $probe 'Cargo.lock')
        executable_sha256 = $exeHash
        kernel_sha256 = $kernelHashes
        build_log_sha256 = Get-Sha $buildLog
        run_log_sha256 = Get-Sha $runLog
        artifacts = $artifacts
        production_runtime_modified = $false
        formal_performance_evidence = $false
    }
    Write-Text (Join-Path $WorkRoot 'report.json') ($report | ConvertTo-Json -Depth 10)
    Write-Output 'Epoch preserve diagnostic: 17/17 passed; production runtime unchanged.'
}
finally {
    $env:CARGO_HOME = $previousCargoHome
    $env:CARGO_TARGET_DIR = $previousTargetDir
}
