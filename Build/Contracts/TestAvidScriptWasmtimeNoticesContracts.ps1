[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $pluginRoot 'Build/ReleaseEngineering/AvidScriptWasmtimeNotices.ps1')
Initialize-AvidScriptBinaryPrivacy
$testRoot = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('notice-contracts-' + [Guid]::NewGuid().ToString('N'))
$source = Join-Path $testRoot 'source'
$cargoHome = Join-Path $testRoot 'cargo'
$sysroot = Join-Path $testRoot 'rust'
$built = Join-Path $testRoot 'built'
foreach ($path in @($source, (Join-Path $source 'dep'), $cargoHome, (Join-Path $sysroot 'share/doc/rust'), $built)) {
    [void][IO.Directory]::CreateDirectory($path)
}
$lock = Get-Content (Join-Path $pluginRoot 'Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.lock.json') -Raw | ConvertFrom-Json
$rootToml = @'
[workspace]
members = ["dep"]
resolver = "2"
[package]
name = "wasmtime-c-api"
version = "45.0.0"
edition = "2021"
license = "MIT"
[lib]
name = "wasmtime"
path = "lib.rs"
crate-type = ["cdylib", "staticlib"]
[dependencies]
notice-dep = { path = "dep" }
[features]
all-arch = []
cranelift = []
disable-logging = []
gc = []
gc-drc = []
parallel-compilation = []
'@
[IO.File]::WriteAllText((Join-Path $source 'Cargo.toml'), $rootToml)
[IO.File]::WriteAllText((Join-Path $source 'lib.rs'), '')
[IO.File]::WriteAllText((Join-Path $source 'LICENSE'), 'Fixture license text, preserved byte for byte.')
[IO.File]::WriteAllText((Join-Path $source 'dep/Cargo.toml'), "[package]`nname = `"notice-dep`"`nversion = `"0.1.0`"`nedition = `"2021`"`nlicense = `"MIT`"`n[lib]`npath = `"lib.rs`"`n")
[IO.File]::WriteAllText((Join-Path $source 'dep/lib.rs'), '')
[IO.File]::WriteAllText((Join-Path $sysroot 'share/doc/rust/COPYRIGHT-library.html'), '<html>Fixture Rust standard library notice.</html>')
foreach ($name in @('wasmtime.dll','wasmtime.lib','wasmtime.dll.lib')) { [IO.File]::WriteAllText((Join-Path $built $name), "fixture bytes: $name") }
$script:NoticeAssertions = 0
function Assert-Notice([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASWNT1000 $Message" }
    $script:NoticeAssertions++
}
function Assert-NoticeReject([scriptblock]$Action, [string]$Code) {
    $actual = ''
    try { & $Action | Out-Null }
    catch { $actual = $_.Exception.GetBaseException().Message.Split(' ')[0] }
    Assert-Notice ($actual -ceq $Code) "expected $Code, got $actual"
}
function New-NoticeRun([string]$Name) {
    $stage = Join-Path $testRoot $Name
    [void][IO.Directory]::CreateDirectory((Join-Path $stage 'lib'))
    foreach ($file in Get-ChildItem -LiteralPath $built -File) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $stage 'lib') }
    return @{stage=$stage;log=(Join-Path $testRoot "$Name.jsonl");messages=($script:BaseMessages | ConvertTo-Json -Depth 20 | ConvertFrom-Json -Depth 20)}
}
function Invoke-NoticeRun($Run) {
    [IO.File]::WriteAllLines($Run.log, @($Run.messages | ForEach-Object { $_ | ConvertTo-Json -Depth 20 -Compress }), [Text.UTF8Encoding]::new($false))
    Export-AvidScriptWasmtimeNotices -ArtifactLog $Run.log -SourceRoot $source -CargoHome $cargoHome -RustSysroot $sysroot -StagingRoot $Run.stage -Lock $lock
}

# Reuse the production environment restoration helpers without inheriting user Cargo configuration.
. (Join-Path $pluginRoot 'Build/ReleaseEngineering/AvidScriptNeutralWasmtimeBuild.ps1')
$saved = Save-AvidScriptWasmtimeEnvironment
try {
    $env:RUSTUP_TOOLCHAIN = $lock.rust.toolchain
    $env:CARGO_HOME = $cargoHome
    & cargo generate-lockfile --offline --manifest-path (Join-Path $source 'Cargo.toml')
    if ($LASTEXITCODE -ne 0) { throw 'ASWNT1000 fixture Cargo lock failed' }
    $meta = (& cargo metadata --no-deps --offline --format-version 1 --manifest-path (Join-Path $source 'Cargo.toml')) | ConvertFrom-Json -Depth 30
    if ($LASTEXITCODE -ne 0) { throw 'ASWNT1000 fixture metadata failed' }
    $root = $meta.packages | Where-Object name -CEQ 'wasmtime-c-api'
    $dep = $meta.packages | Where-Object name -CEQ 'notice-dep'
    $profile = @{opt_level='3';debuginfo=0;debug_assertions=$false;overflow_checks=$false;test=$false}
    $script:BaseMessages = @(
        @{reason='compiler-artifact';package_id=$root.id;manifest_path=$root.manifest_path;target=@{name='wasmtime';crate_types=@('cdylib','staticlib')};profile=$profile;features=@($lock.rust.features);filenames=@(Get-ChildItem -LiteralPath $built -File | ForEach-Object FullName);fresh=$true},
        @{reason='compiler-artifact';package_id=$dep.id;manifest_path=$dep.manifest_path;target=@{name='notice_dep';crate_types=@('lib')};profile=$profile;features=@();filenames=@();fresh=$true},
        @{reason='build-finished';success=$true}
    )
    $valid = New-NoticeRun 'valid'
    $result = Invoke-NoticeRun $valid
    Assert-Notice ($result.package_count -eq 2) 'both real graph packages must be covered'
    $bundle = Join-Path $valid.stage 'notices'
    $manifest = Test-AvidScriptWasmtimeNotices $bundle
    Assert-Notice ($manifest.rust.notices.Count -eq 1) 'Rust standard library needs a separate notice'
    Assert-Notice ($manifest.packages[0].notices[0].sha256 -ceq (Get-FileHash (Join-Path $source 'LICENSE')).Hash.ToLowerInvariant()) 'workspace fallback must preserve original license bytes'
    Assert-Notice (-not ([IO.File]::ReadAllText((Join-Path $bundle 'manifest.json'))).Contains($testRoot)) 'manifest must not expose build root'
    $cases = [ordered]@{
        'missing-finish' = {param($r) $r.messages=@($r.messages | Where-Object reason -ne 'build-finished')}
        'failed-build' = {param($r) $r.messages[-1].success=$false}
        'duplicate-finish' = {param($r) $r.messages += $r.messages[-1]}
        'missing-runtime' = {param($r) $r.messages=@($r.messages | Where-Object {$_.reason -ne 'compiler-artifact' -or $_.package_id -ne $root.id})}
        'duplicate-runtime' = {param($r) $r.messages=@($r.messages[0],$r.messages[0],$r.messages[1],$r.messages[2])}
        'wrong-features' = {param($r) $r.messages[0].features=@('cranelift')}
        'wrong-profile' = {param($r) $r.messages[0].profile.opt_level='0'}
        'wrong-runtime-bytes' = {param($r) [IO.File]::WriteAllText((Join-Path $r.stage 'lib/wasmtime.dll'),'tampered')}
        'missing-dependency' = {param($r) $r.messages=@($r.messages[0],$r.messages[2])}
    }
    foreach ($name in $cases.Keys) {
        $run=New-NoticeRun $name
        & $cases[$name] $run
        Assert-NoticeReject {Invoke-NoticeRun $run} 'ASWN1004'
    }
    $run=New-NoticeRun 'source-escape'
    $run.messages[1].manifest_path=Join-Path $testRoot 'outside/Cargo.toml'
    Assert-NoticeReject {Invoke-NoticeRun $run} 'ASWN1001'
    $licenseBytes=[IO.File]::ReadAllBytes((Join-Path $source 'LICENSE'))
    try {
        [IO.File]::WriteAllBytes((Join-Path $source 'LICENSE'), [byte[]]@())
        $run=New-NoticeRun 'empty-license'
        Assert-NoticeReject {Invoke-NoticeRun $run} 'ASWN1001'
    } finally { [IO.File]::WriteAllBytes((Join-Path $source 'LICENSE'),$licenseBytes) }
    foreach ($name in @('tampered-text','extra-file','wrong-count','duplicate-package','path-escape')) {
        $copy=Join-Path $testRoot $name
        Copy-Item -LiteralPath $bundle -Destination $copy -Recurse
        $path=Join-Path $copy 'manifest.json'
        $data=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable -Depth 30
        switch ($name) {
            'tampered-text' { [IO.File]::AppendAllText((Join-Path $copy $data.packages[0].notices[0].path),'changed') }
            'extra-file' { [IO.File]::WriteAllText((Join-Path $copy 'extra.txt'),'extra') }
            'wrong-count' { $data.package_count=1 }
            'duplicate-package' { $data.packages[1].id=$data.packages[0].id }
            'path-escape' { $data.packages[0].notices[0].path='../outside.txt' }
        }
        [IO.File]::WriteAllText($path,($data | ConvertTo-Json -Depth 30))
        Assert-NoticeReject {Test-AvidScriptWasmtimeNotices $copy} 'ASWN1003'
    }
    Assert-NoticeReject {Invoke-NoticeRun $valid} 'ASWN1001'
    function New-NoticeArchive([string]$Path, [string[]]$Names) {
        $stream=[IO.File]::Create($Path)
        try {
            $gzip=[IO.Compression.GZipStream]::new($stream,[IO.Compression.CompressionLevel]::Optimal,$true)
            try {
                $writer=[System.Formats.Tar.TarWriter]::new($gzip,$true)
                try {
                    foreach ($name in $Names) {
                        $entry=[System.Formats.Tar.PaxTarEntry]::new([System.Formats.Tar.TarEntryType]::RegularFile,$name)
                        $memory=[IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes($(if ($name.EndsWith('/empty')) {''} else {'original archive bytes'})))
                        try { $entry.DataStream=$memory; $writer.WriteEntry($entry) }
                        finally { $memory.Dispose() }
                    }
                } finally { $writer.Dispose() }
            } finally { $gzip.Dispose() }
        } finally { $stream.Dispose() }
    }
    $archive=Join-Path $testRoot 'valid.crate'
    New-NoticeArchive $archive @('fixture-1.0.0/LICENSE','fixture-1.0.0/empty')
    $checksums=Get-AvidNoticeArchiveChecksums $archive 'fixture-1.0.0'
    Assert-Notice ($checksums.package -ceq (Get-FileHash $archive).Hash.ToLowerInvariant()) 'crate identity must cover complete archive bytes'
    Assert-Notice ($checksums.files.empty -ceq 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855') 'empty tar files must hash as empty bytes'
    $tampered=Join-Path $testRoot 'LICENSE'
    [IO.File]::WriteAllText($tampered,'tampered notice')
    Assert-NoticeReject {Add-AvidNoticeText $tampered $testRoot $bundle $checksums.files} 'ASWN1002'
    foreach ($name in @('duplicate','escape','wrong-prefix')) {
        $path=Join-Path $testRoot "$name.crate"
        $entries=switch ($name) {
            'duplicate' {@('fixture-1.0.0/LICENSE','fixture-1.0.0/LICENSE')}
            'escape' {@('fixture-1.0.0/../outside')}
            'wrong-prefix' {@('another-1.0.0/LICENSE')}
        }
        New-NoticeArchive $path $entries
        Assert-NoticeReject {Get-AvidNoticeArchiveChecksums $path 'fixture-1.0.0'} 'ASWN1002'
    }
}
finally { Restore-AvidScriptWasmtimeEnvironment $saved }
[ordered]@{result='wasmtime_notices_contracts_passed';assertions_passed=$script:NoticeAssertions;assertions_total=$script:NoticeAssertions;evidence_root=$testRoot} | ConvertTo-Json
