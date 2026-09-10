[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputRoot)

$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $pluginRoot 'Build/ReleaseEngineering/AvidScriptNeutralWasmtimeBuild.ps1')
$testRoot = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('neutral-contracts-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($testRoot)
$repo = Join-Path $testRoot 'repository with spaces'
$cache = Join-Path $testRoot 'cache with spaces'
$install = Join-Path $repo 'installed/runtime'
[void][IO.Directory]::CreateDirectory($repo)
$script:NeutralAssertions = 0
function Assert-NeutralContract([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASNBT1000 $Message" }
    $script:NeutralAssertions++
}
function Assert-NeutralReject([scriptblock]$Action, [string]$Code) {
    $actual = ''
    try { & $Action }
    catch { $actual = $_.Exception.GetBaseException().Message.Split(' ')[0] }
    Assert-NeutralContract ($actual -ceq $Code) "expected rejection $Code, got $actual"
}

Assert-AvidScriptNeutralBuildInputs -RepositoryRoot $repo -CacheRoot $cache -InstallPath $install
Assert-NeutralContract (-not (Test-Path -LiteralPath $cache)) 'input preflight must not write cache'
Assert-NeutralReject { Assert-AvidScriptNeutralBuildInputs $repo $repo $install } 'ASNB1001'
Assert-NeutralReject { Assert-AvidScriptNeutralBuildInputs $repo (Join-Path $repo 'nested') $install } 'ASNB1001'
Assert-NeutralReject { Assert-AvidScriptNeutralBuildInputs ([Environment]::GetFolderPath('UserProfile')) $cache $install } 'ASNB1001'
[void][IO.Directory]::CreateDirectory($install)
[IO.File]::WriteAllText((Join-Path $install 'keep.txt'), 'existing runtime')
Assert-NeutralReject { Assert-AvidScriptNeutralBuildInputs $repo $cache $install } 'ASNB1003'
Assert-NeutralContract (([IO.File]::ReadAllText((Join-Path $install 'keep.txt'))) -ceq 'existing runtime') 'existing install must remain intact'
$install = Join-Path $repo 'installed/new-runtime'

foreach ($name in @('RUSTFLAGS','CARGO_ENCODED_RUSTFLAGS','CARGO_BUILD_RUSTFLAGS','CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS',
    'CARGO_PROFILE_RELEASE_OPT_LEVEL','RUSTC_WRAPPER','CC','CL','LDFLAGS','WASMTIME_USER_CARGO_BUILD_OPTIONS')) {
    $old = [Environment]::GetEnvironmentVariable($name, 'Process')
    try {
        [Environment]::SetEnvironmentVariable($name, 'external override', 'Process')
        Assert-NeutralReject { Assert-AvidScriptNeutralBuildInputs $repo $cache $install } 'ASNB1002'
    }
    finally { Restore-AvidScriptWasmtimeEnvironment -Values @{$name=$old} }
}
$configRepo = Join-Path $testRoot 'configured-repository'
[void][IO.Directory]::CreateDirectory((Join-Path $configRepo '.cargo'))
[IO.File]::WriteAllText((Join-Path $configRepo '.cargo/config.toml'), '[build]')
Assert-NeutralReject { Assert-AvidScriptNeutralBuildInputs $configRepo $cache $install } 'ASNB1002'
$link = Join-Path $testRoot 'linked-repository'
[void](New-Item -ItemType Junction -Path $link -Target $repo)
Assert-NeutralReject { Assert-AvidScriptNeutralBuildInputs $link $cache $install } 'ASBP1002'
$ordinaryCache = Join-Path $testRoot 'ordinary-cache'
[void][IO.Directory]::CreateDirectory($ordinaryCache)
[IO.File]::WriteAllText((Join-Path $ordinaryCache 'keep.txt'), 'ordinary cache')
Assert-NeutralReject { Assert-AvidScriptNeutralBuildInputs $repo $ordinaryCache $install } 'ASNB1003'

$mappings = [ordered]@{}
$mappings[(Join-Path $cache 'specific source')] = '/avidscript/source'
$mappings[$cache] = '/avidscript/build'
$encoded = New-AvidScriptNeutralRustFlags -Mappings $mappings
$flags = @($encoded.Split([char]31))
Assert-NeutralContract ($flags.Count -eq 4) 'two paths must produce four independent slash variants'
Assert-NeutralContract ($flags[0].EndsWith('=/avidscript/build') -and $flags[3].EndsWith('=/avidscript/source')) 'specific mappings must follow broad mappings'
Assert-NeutralContract ($flags[0].Contains('cache with spaces')) 'encoded argument must preserve spaces'
Assert-NeutralReject { [void](New-AvidScriptNeutralRustFlags -Mappings @{(Join-Path $cache 'ambiguous=path')='/avidscript/source'}) } 'ASNB1001'

$saved = Save-AvidScriptWasmtimeEnvironment
try {
    $env:RUSTUP_TOOLCHAIN = '1.93.0-x86_64-pc-windows-msvc'
    $source = Join-Path $cache 'source'
    $target = Join-Path $cache 'cargo-target'
    $privatePaths = @(Set-AvidScriptNeutralWasmtimeEnvironment $repo $cache $source $target)
    Assert-NeutralContract ($env:CARGO_HOME -ceq (Join-Path $cache 'cargo-home')) 'Cargo home must be isolated'
    Assert-NeutralContract ($env:TEMP -ceq $env:TMP -and $env:TEMP -ceq (Join-Path $cache 'temp')) 'both temporary directories must be isolated'
    Assert-NeutralContract ($privatePaths.Count -ge 8) 'scan must receive all private build roots'
    Test-AvidScriptNeutralRustFlags -CacheRoot $cache
    Assert-NeutralContract $true 'pinned Rust metadata must contain remapped file!() path'
    throw 'expected fixture failure'
}
catch { if ($_.Exception.Message -cne 'expected fixture failure') { throw } }
finally { Restore-AvidScriptWasmtimeEnvironment -Values $saved }
foreach ($name in $saved.Keys) {
    Assert-NeutralContract ([Environment]::GetEnvironmentVariable($name, 'Process') -ceq $saved[$name]) "environment restore failed for $name"
}

# Exercise the real builder's finally block after it has selected the pinned Rust toolchain.
$builder = Join-Path $pluginRoot 'Build/BuildAvidScriptWasmtimePerformanceToolchain.ps1'
$caught = $false
try { & $builder -Mode Build -SourceArchiveOverride (Join-Path $testRoot 'missing-source.zip') | Out-Null }
catch { $caught = $_.Exception.Message.StartsWith('ASP57W1201') }
Assert-NeutralContract $caught 'real builder must fail before source preparation on missing archive'
foreach ($name in $saved.Keys) {
    Assert-NeutralContract ([Environment]::GetEnvironmentVariable($name, 'Process') -ceq $saved[$name]) "real builder leaked environment $name"
}
[ordered]@{
    result='neutral_wasmtime_build_contracts_passed'; assertions_passed=$script:NeutralAssertions
    assertions_total=$script:NeutralAssertions; evidence_root=$testRoot
} | ConvertTo-Json
