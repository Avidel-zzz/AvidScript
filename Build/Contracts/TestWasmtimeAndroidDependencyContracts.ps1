[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PluginRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$InstallerPath = Join-Path $PluginRoot 'Build/InstallWasmtimeDependency.ps1'
$ThirdPartyRoot = Join-Path $PluginRoot 'Source/ThirdParty/Wasmtime'
$LockPath = Join-Path $ThirdPartyRoot 'WasmtimeAndroidDependency.lock.json'
$SchemaPath = Join-Path $ThirdPartyRoot 'WasmtimeAndroidDependency.schema.json'
$Passed = 0
$Total = 16

function Assert-Contract {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
    ++$script:Passed
}

function Assert-ThrowsCode {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Body,
        [Parameter(Mandatory = $true)][string]$Code
    )

    try {
        & $Body
    }
    catch {
        if (-not $_.Exception.Message.Contains($Code)) {
            throw "Expected $Code but received: $($_.Exception.Message)"
        }
        return
    }
    throw "Expected failure containing $Code"
}

function Copy-LockObject {
    param([Parameter(Mandatory = $true)]$Value)

    return $Value | ConvertTo-Json -Depth 32 | ConvertFrom-Json
}

function New-SyntheticElfArchive {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][uint16]$Machine
    )

    $Elf = [byte[]]::new(64)
    $Elf[0] = 0x7f
    $Elf[1] = [byte][char]'E'
    $Elf[2] = [byte][char]'L'
    $Elf[3] = [byte][char]'F'
    $Elf[4] = 2
    $Elf[5] = 1
    $MachineBytes = [System.BitConverter]::GetBytes($Machine)
    $Elf[18] = $MachineBytes[0]
    $Elf[19] = $MachineBytes[1]
    $HeaderText = 'fixture.o/'.PadRight(16) +
        '0'.PadRight(12) +
        '0'.PadRight(6) +
        '0'.PadRight(6) +
        '100644'.PadRight(8) +
        ([string]$Elf.Length).PadRight(10) +
        "```n"
    if ($HeaderText.Length -ne 60) {
        throw 'Synthetic ar header is not 60 bytes.'
    }
    $Signature = [System.Text.Encoding]::ASCII.GetBytes("!<arch>`n")
    $Header = [System.Text.Encoding]::ASCII.GetBytes($HeaderText)
    $Bytes = [byte[]]::new($Signature.Length + $Header.Length + $Elf.Length)
    [Array]::Copy($Signature, 0, $Bytes, 0, $Signature.Length)
    [Array]::Copy($Header, 0, $Bytes, $Signature.Length, $Header.Length)
    [Array]::Copy($Elf, 0, $Bytes, $Signature.Length + $Header.Length, $Elf.Length)
    [System.IO.File]::WriteAllBytes($Path, $Bytes)
}

function New-AndroidArchive {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)]$Lock,
        [uint16]$Machine = 183,
        [switch]$MissingStaticLibrary
    )

    $SourceRoot = Join-Path (Split-Path -Parent $ArchivePath) 'source'
    $ArchiveRoot = Join-Path $SourceRoot ([string]$Lock.archive.root)
    New-Item -ItemType Directory -Path (Join-Path $ArchiveRoot 'include/wasmtime') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $ArchiveRoot 'lib') -Force | Out-Null
    [System.IO.File]::WriteAllText(
        (Join-Path $ArchiveRoot 'include/wasmtime.h'),
        'synthetic wasmtime header')
    [System.IO.File]::WriteAllText(
        (Join-Path $ArchiveRoot 'include/wasmtime/conf.h'),
        '#define WASMTIME_FEATURE_CRANELIFT')
    [System.IO.File]::WriteAllText(
        (Join-Path $ArchiveRoot 'LICENSE'),
        'synthetic license')
    if (-not $MissingStaticLibrary) {
        New-SyntheticElfArchive `
            -Path (Join-Path $ArchiveRoot 'lib/libwasmtime.a') `
            -Machine $Machine
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $ArchivePath) -Force | Out-Null
    & tar.exe -cf $ArchivePath -C $SourceRoot ([string]$Lock.archive.root)
    if ($LASTEXITCODE -ne 0) {
        throw 'Synthetic Android archive creation failed.'
    }
    return $ArchiveRoot
}

function Set-FixtureIdentity {
    param(
        [Parameter(Mandatory = $true)]$Lock,
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [string]$StaticLibraryPath = ''
    )

    $Lock.archive.size_bytes = [int64](Get-Item -LiteralPath $ArchivePath).Length
    $Lock.archive.sha256 = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).
        Hash.ToLowerInvariant()
    if (-not [string]::IsNullOrWhiteSpace($StaticLibraryPath)) {
        $Lock.layout.static_library_sha256 = (
            Get-FileHash -LiteralPath $StaticLibraryPath -Algorithm SHA256).
            Hash.ToLowerInvariant()
    }
}

$RawLock = Get-Content -Raw -LiteralPath $LockPath
Assert-Contract ($RawLock | Test-Json -SchemaFile $SchemaPath) 'Android lock schema failed.'
$TrackedLock = $RawLock | ConvertFrom-Json
Assert-Contract (
    [string]$TrackedLock.archive.sha256 -ceq
        'af5160bb3975686aec2f2ae1494bcd7ed4adcd3d4922af23e16bea3b687bfc1b') `
    'Android archive SHA-256 drifted.'
Assert-Contract (
    [string]$TrackedLock.layout.static_library_sha256 -ceq
        'd53dd5a687cc555b1a8b77c4639bfc355812000b7987c4a0f559982e61e75b28') `
    'Android static library SHA-256 drifted.'
Assert-Contract (
    [string]$TrackedLock.install.relative_path -ceq
        'Source/ThirdParty/Wasmtime/installed/Android/arm64/v45.0.0') `
    'Android install boundary drifted.'

$ValidateOutput = & pwsh `
    -NoProfile `
    -File $InstallerPath `
    -Mode ValidateLock `
    -Platform AndroidArm64 `
    -RepositoryRoot $PluginRoot
Assert-Contract ($LASTEXITCODE -eq 0) 'Android lock CLI validation failed.'
$ValidateResult = $ValidateOutput | ConvertFrom-Json
Assert-Contract ($ValidateResult.status -ceq 'valid') 'Android lock CLI result is invalid.'

. $InstallerPath
$BuildSource = Get-Content -Raw -LiteralPath (Join-Path $ThirdPartyRoot 'Wasmtime.Build.cs')
Assert-Contract ($BuildSource -match '(?s)"installed".*?"Android".*?"arm64".*?"v45\.0\.0"') `
    'Wasmtime.Build.cs has no Android arm64 root.'
Assert-Contract ($BuildSource.Contains('"libwasmtime.a"')) `
    'Wasmtime.Build.cs does not link the Android static library.'
Assert-Contract (
    $BuildSource.Contains('bHasAndroidCrossTargetLayout') -and
    $BuildSource.Contains('AVIDSCRIPT_WASMTIME_ANDROID_STATIC_SHA256')) `
    'Wasmtime.Build.cs does not expose the Android runtime identity to the host cross-compiler.'
Assert-Contract (
    $BuildSource.Contains('Target.Architecture != UnrealArch.Arm64') -and
    -not $BuildSource.Contains('Binaries/Android/wasmtime.so')) `
    'Wasmtime.Build.cs does not fail closed on Android architecture or stages a shared library.'

$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'AvidScriptWasmtimeAndroidContracts-' + [Guid]::NewGuid().ToString('N'))
try {
    $CacheRoot = Join-Path $FixtureRoot 'cache'
    $ArchivePath = Get-WasmtimeArchiveCachePath -CacheRoot $CacheRoot -Lock $TrackedLock
    $FixtureLock = Copy-LockObject $TrackedLock
    $ArchiveRoot = New-AndroidArchive -ArchivePath $ArchivePath -Lock $FixtureLock
    Set-FixtureIdentity `
        -Lock $FixtureLock `
        -ArchivePath $ArchivePath `
        -StaticLibraryPath (Join-Path $ArchiveRoot 'lib/libwasmtime.a')
    $FixtureLockPath = Join-Path $FixtureRoot 'fixture-lock.json'
    $FixtureLock | ConvertTo-Json -Depth 32 |
        Set-Content -LiteralPath $FixtureLockPath -Encoding utf8NoBOM
    $FixtureLockSha256 = Get-CanonicalFileSha256 -Path $FixtureLockPath
    $RepositoryRoot = Join-Path $FixtureRoot 'repository'
    New-Item -ItemType Directory -Path (
        Join-Path $RepositoryRoot 'Source/ThirdParty/Wasmtime') -Force | Out-Null

    $InstallResult = Install-WasmtimeDependency `
        -RepositoryRoot $RepositoryRoot `
        -CacheRoot $CacheRoot `
        -Lock $FixtureLock `
        -LockSha256 $FixtureLockSha256
    Assert-Contract ($InstallResult.status -ceq 'installed') 'Android install failed.'
    $InstallPath = Join-Path $RepositoryRoot $FixtureLock.install.relative_path
    Assert-Contract (
        (Test-Path -LiteralPath (Join-Path $InstallPath 'lib/libwasmtime.a') -PathType Leaf) -and
        -not (Test-Path -LiteralPath (Join-Path $InstallPath 'lib/libwasmtime.so'))) `
        'Android install file set is invalid.'
    $VerifyResult = Test-WasmtimeDependency `
        -RepositoryRoot $RepositoryRoot `
        -Lock $FixtureLock `
        -LockSha256 $FixtureLockSha256
    Assert-Contract ($VerifyResult.status -ceq 'verified') 'Android verify failed.'

    [System.IO.File]::WriteAllText((Join-Path $InstallPath 'extra.txt'), 'extra')
    Assert-ThrowsCode {
        Test-WasmtimeDependency $RepositoryRoot $FixtureLock $FixtureLockSha256
    } 'ASP54W1605'
    ++$Passed

    $WrongCache = Join-Path $FixtureRoot 'wrong-cache'
    $WrongArchivePath = Get-WasmtimeArchiveCachePath `
        -CacheRoot $WrongCache `
        -Lock $TrackedLock
    $WrongLock = Copy-LockObject $TrackedLock
    $WrongArchiveRoot = New-AndroidArchive `
        -ArchivePath $WrongArchivePath `
        -Lock $WrongLock `
        -Machine 62
    Set-FixtureIdentity `
        -Lock $WrongLock `
        -ArchivePath $WrongArchivePath `
        -StaticLibraryPath (Join-Path $WrongArchiveRoot 'lib/libwasmtime.a')
    $WrongRepository = Join-Path $FixtureRoot 'wrong-repository'
    New-Item -ItemType Directory -Path (
        Join-Path $WrongRepository 'Source/ThirdParty/Wasmtime') -Force | Out-Null
    Assert-ThrowsCode {
        Install-WasmtimeDependency `
            -RepositoryRoot $WrongRepository `
            -CacheRoot $WrongCache `
            -Lock $WrongLock `
            -LockSha256 ('0' * 64)
    } 'ASP54W1407'
    ++$Passed

    $HashLock = Copy-LockObject $FixtureLock
    $HashLock.archive.sha256 = '0' * 64
    Assert-ThrowsCode {
        Assert-WasmtimeArchive -Path $ArchivePath -Lock $HashLock
    } 'ASP54W1302'
    ++$Passed
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot -PathType Container) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

if ($Passed -ne $Total) {
    throw "Wasmtime Android dependency contracts failed: $Passed/$Total"
}
Write-Output "Wasmtime Android dependency contracts: PASS ($Passed/$Total)"
