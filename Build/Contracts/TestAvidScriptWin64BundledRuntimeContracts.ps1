[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PluginRoot 'Build/ReleaseEngineering/AvidScriptPluginReleasePackage.ps1')
. (Join-Path $PluginRoot 'Build/ReleaseEngineering/AvidScriptPluginInstaller.ps1')
. (Join-Path $PluginRoot 'Build/ReleaseEngineering/AvidScriptWin64BundledRuntime.ps1')
$FixtureBase = Join-Path ([IO.Path]::GetPathRoot([IO.Path]::GetTempPath())) 'tmp'
$FixtureRoot = Join-Path $FixtureBase ('awb-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$RuntimeRelative = 'Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0-avidscript.2'
$LockRelative = 'Source/ThirdParty/Wasmtime/PerformanceToolchain/WasmtimePerformanceToolchain.lock.json'
$MarkerName = '.avidscript-wasmtime-performance-managed.json'
$Passed = 0

function Write-FixtureText($Path, $Text) {
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Update-FixtureMarker($Runtime) {
    $markerPath = Join-Path $Runtime $MarkerName
    $marker = Read-AvidScriptPluginReleaseJsonObject $markerPath 'fixture marker'
    $lines = @(Get-ChildItem -LiteralPath $Runtime -File -Recurse -Force | Sort-Object FullName | Where-Object Name -CNE $MarkerName | ForEach-Object {
        "$([IO.Path]::GetRelativePath($Runtime, $_.FullName).Replace('\', '/'))|$($_.Length)|$(Get-AvidScriptPluginReleaseSha256 $_.FullName)"
    })
    $marker.installed_content_sha256 = Get-AvidScriptPluginReleaseBytesSha256 ([Text.Encoding]::UTF8.GetBytes(($lines -join "`n") + "`n"))
    $marker.dll_sha256 = Get-AvidScriptPluginReleaseSha256 (Join-Path $Runtime 'lib/wasmtime.dll')
    Write-AvidScriptPluginReleaseJson $markerPath $marker
}
function Test-Case($Name, [scriptblock]$Body) {
    try { & $Body; $script:Passed++; Write-Output "PASS $Name" }
    catch { throw "$Name failed: $($_.Exception.Message)" }
}
function Expect-Rejected([scriptblock]$Body, [string]$Pattern) {
    $rejected = $false
    try { & $Body | Out-Null } catch { if ($_.Exception.Message -match $Pattern) { $rejected = $true } else { throw } }
    if (-not $rejected) { throw "expected rejection matching $Pattern" }
}
function Copy-Fixture($Name) {
    $destination = Join-Path $FixtureRoot $Name
    Copy-AvidScriptPluginReleasePayload (Join-Path $FixtureRoot 'base') $destination
    return $destination
}
function Get-FixtureDependency($Payload) {
    $plugin = Join-Path $Payload 'AvidScript'
    return [pscustomobject][ordered]@{
        id = 'wasmtime-win64'; version = $script:FixtureLock.toolchain_id; mode = 'bundled'
        identity_path = "AvidScript/$LockRelative"
        identity_sha256 = Get-AvidScriptPluginReleaseSha256 (Join-Path $plugin $LockRelative)
        bundle = Get-AvidScriptWin64BundleIdentity -PluginRoot $plugin -RuntimeRoot (Join-Path $plugin $RuntimeRelative)
    }
}
function Publish-Fixture($Payload, $Output) {
    $contracts = @('generated-type-package', 'module-release-package')
    $producers = @('AvidScriptGeneratedTypeCookPackage.ps1', 'AvidScriptModuleReleasePackage.ps1')
    $artifacts = @(for ($i = 0; $i -lt 2; $i++) {
        [pscustomobject]@{ id = $contracts[$i]; producer_path = "AvidScript/Build/$($producers[$i])"
            producer_sha256 = Get-AvidScriptPluginReleaseSha256 (Join-Path $Payload "AvidScript/Build/$($producers[$i])") }
    })
    Publish-AvidScriptPluginReleasePackage -PayloadSourceRoot $Payload -OutputRoot $Output -Version '0.1.0' `
        -Profile win64-offline -Targets @('Win64') -Commit ('a' * 40) -Tree ('b' * 40) `
        -CommittedAtUtc '2026-09-13T00:00:00.0000000+00:00' -ArtifactContracts $artifacts `
        -Dependencies @((Get-FixtureDependency $Payload))
}

try {
    $basePlugin = Join-Path $FixtureRoot 'base/AvidScript'
    $runtime = Join-Path $basePlugin $RuntimeRelative
    foreach ($relative in @($LockRelative,
            'Source/ThirdParty/Wasmtime/PerformanceToolchain/avidscript-wasmtime-v45-inlining.patch')) {
        $dest = Join-Path $basePlugin $relative
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $dest))
        [IO.File]::Copy((Join-Path $PluginRoot $relative), $dest)
    }
    $script:FixtureLock = Read-AvidScriptPluginReleaseJsonObject (Join-Path $basePlugin $LockRelative) 'fixture lock'
    Write-FixtureText (Join-Path $basePlugin 'AvidScript.uplugin') '{"FileVersion":3,"VersionName":"0.1.0"}'
    foreach ($relative in @('LICENSE', 'Build/AvidScriptGeneratedTypeCookPackage.ps1', 'Build/AvidScriptModuleReleasePackage.ps1',
            'Source/ThirdParty/WAMR/lib/Win64/Release/libiwasm.lib', 'Source/ThirdParty/WAMR/upstream/LICENSE',
            'Source/ThirdParty/WAMR/upstream/core/iwasm/include/wasm_export.h')) {
        Write-FixtureText (Join-Path $basePlugin $relative) 'fixture only; never executed'
    }
    foreach ($relative in @('include/wasmtime.h', 'include/wasmtime/config.h', 'LICENSE', 'lib/wasmtime.dll', 'lib/wasmtime.dll.lib', 'lib/wasmtime.lib')) {
        Write-FixtureText (Join-Path $runtime $relative) "fixture $relative"
    }
    $noticeText = 'fixture notice'
    $noticeHash = Get-AvidScriptPluginReleaseBytesSha256 ([Text.Encoding]::UTF8.GetBytes($noticeText))
    $notice = @{origin_path = 'LICENSE'; path = "texts/$noticeHash.txt"; sha256 = $noticeHash; length = $noticeText.Length}
    Write-FixtureText (Join-Path $runtime "notices/$($notice.path)") $noticeText
    $runtimeHashes = @{}
    foreach ($name in @('wasmtime.dll', 'wasmtime.dll.lib', 'wasmtime.lib')) { $runtimeHashes[$name] = Get-AvidScriptPluginReleaseSha256 (Join-Path $runtime "lib/$name") }
    Write-AvidScriptPluginReleaseJson (Join-Path $runtime 'notices/manifest.json') @{
        schema_version = 1; scope = 'win64-wasmtime-build-notices'; package_count = 1; toolchain_id = $FixtureLock.toolchain_id
        source_archive_sha256 = $FixtureLock.upstream.source_archive.sha256; patch_sha256 = $FixtureLock.patch.canonical_sha256
        cargo_lock_sha256 = 'c' * 64; artifact_log_sha256 = 'd' * 64; runtime = $runtimeHashes
        packages = @(@{id = 'wasmtime:fixture@1'; name = 'fixture'; version = '1'; license_expression = 'MIT'
            source = @{kind = 'wasmtime'; identity = $FixtureLock.upstream.commit; checksum = $null}; features = @(); notices = @($notice)})
        rust = @{toolchain = $FixtureLock.rust.toolchain; notices = @($notice)}
    }
    Write-AvidScriptPluginReleaseJson (Join-Path $runtime $MarkerName) @{
        schema_version = 1; toolchain_id = $FixtureLock.toolchain_id; source_sha256 = $FixtureLock.upstream.source_archive.sha256
        patch_sha256 = $FixtureLock.patch.canonical_sha256; rust_toolchain = $FixtureLock.rust.toolchain
        build_profile = $FixtureLock.rust.build_profile; compiler_profile = $FixtureLock.compiler_profile.id
        dll_sha256 = ''; installed_content_sha256 = ''
    }
    Update-FixtureMarker $runtime

    Test-Case 'deterministic offline package and schema v2' {
        $script:Package = Publish-Fixture (Join-Path $FixtureRoot 'base') (Join-Path $FixtureRoot 'out')
        $repeat = Publish-Fixture (Join-Path $FixtureRoot 'base') (Join-Path $FixtureRoot 'out2')
        if ($Package.Manifest.schema_version -ne 2 -or $Package.Manifest.targets.Count -ne 1 -or
            $Package.ManifestSha256 -cne $repeat.ManifestSha256) { throw 'offline identity is not deterministic' }
    }
    Test-Case 'offline plan apply verify and no-op' {
        $script:Project = Join-Path $FixtureRoot 'project'
        Write-FixtureText (Join-Path $Project 'fixture.uproject') '{}'
        $plan = Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Plan
        if ($plan.action -cne 'Install' -or (Test-Path (Join-Path $Project 'Plugins/AvidScript'))) { throw 'Plan wrote target' }
        $apply = Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Apply
        $verify = Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Verify
        $repeat = Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Apply
        if ($apply.action -cne 'Install' -or $verify.result -cne 'passed' -or $repeat.action -cne 'NoOp') { throw 'installation contract failed' }
    }
    Test-Case 'installed DLL tampering rejected' {
        [IO.File]::AppendAllText((Join-Path $Project "Plugins/AvidScript/$RuntimeRelative/lib/wasmtime.dll"), 'tampered')
        Expect-Rejected { Invoke-AvidScriptPluginInstallTransaction $Package.Root $Project Verify } 'ASRI'
    }
    foreach ($item in @(
        @{name='DLL'; path='lib/wasmtime.dll'}, @{name='import library'; path='lib/wasmtime.dll.lib'},
        @{name='header'; path='include/wasmtime.h'}, @{name='license'; path='LICENSE'})) {
        Test-Case "$($item.name) tampering rejected" {
            $payload = Copy-Fixture ('mut' + $Passed)
            [IO.File]::AppendAllText((Join-Path $payload "AvidScript/$RuntimeRelative/$($item.path)"), 'tampered')
            Expect-Rejected { Get-FixtureDependency $payload } 'ASWB1004'
        }
    }
    Test-Case 'missing static library rejected' {
        $payload = Copy-Fixture 'missing'
        Remove-Item -LiteralPath (Join-Path $payload "AvidScript/$RuntimeRelative/lib/wasmtime.lib")
        Expect-Rejected { Get-FixtureDependency $payload } 'ASWB1003'
    }
    Test-Case 'marker source identity rejected' {
        $payload = Copy-Fixture 'marker'
        $path = Join-Path $payload "AvidScript/$RuntimeRelative/$MarkerName"
        $marker = Read-AvidScriptPluginReleaseJsonObject $path 'fixture marker'
        $marker.source_sha256 = 'f' * 64
        Write-AvidScriptPluginReleaseJson $path $marker
        Expect-Rejected { Get-FixtureDependency $payload } 'ASWB1002'
    }
    Test-Case 'patch drift rejected' {
        $payload = Copy-Fixture 'patch'
        [IO.File]::AppendAllText((Join-Path $payload 'AvidScript/Source/ThirdParty/Wasmtime/PerformanceToolchain/avidscript-wasmtime-v45-inlining.patch'), 'tampered')
        Expect-Rejected { Get-FixtureDependency $payload } 'ASWB1002'
    }
    Test-Case 'notice binary mismatch rejected after marker recomputed' {
        $payload = Copy-Fixture 'notice'
        $runtime = Join-Path $payload "AvidScript/$RuntimeRelative"
        [IO.File]::AppendAllText((Join-Path $runtime 'lib/wasmtime.dll'), 'tampered')
        Update-FixtureMarker $runtime
        Expect-Rejected { Get-FixtureDependency $payload } 'ASWB1005'
    }
    Test-Case 'unreferenced notice rejected after marker recomputed' {
        $payload = Copy-Fixture 'extra'
        $runtime = Join-Path $payload "AvidScript/$RuntimeRelative"
        Write-FixtureText (Join-Path $runtime 'notices/extra.txt') 'extra'
        Update-FixtureMarker $runtime
        Expect-Rejected { Get-FixtureDependency $payload } 'ASWN1003'
    }
    Test-Case 'missing WAMR blocks offline publish' {
        $payload = Copy-Fixture 'wamr'
        Remove-Item -LiteralPath (Join-Path $payload 'AvidScript/Source/ThirdParty/WAMR/lib/Win64/Release/libiwasm.lib')
        Expect-Rejected { Publish-Fixture $payload (Join-Path $FixtureRoot 'badout') } 'ASWB1003'
    }
    foreach ($mutation in @('schema', 'targets', 'mode', 'bundle-root', 'empty-dependencies')) {
        Test-Case "invalid offline $mutation rejected" {
            $manifest = $Package.Manifest | ConvertTo-Json -Depth 40 | ConvertFrom-Json -Depth 40
            switch ($mutation) {
                'schema' { $manifest.schema_version = 1 }
                'targets' { $manifest.targets = @('Android-arm64', 'Win64') }
                'mode' { $manifest.dependencies[0].mode = 'external' }
                'bundle-root' { $manifest.dependencies[0].bundle.root = '../escape' }
                'empty-dependencies' { $manifest.dependencies = @() }
            }
            $manifest.release_id = Get-AvidScriptPluginReleaseId $manifest
            $path = Join-Path $FixtureRoot 'bad-manifest.json'
            Write-AvidScriptPluginReleaseJson $path $manifest
            Expect-Rejected { Assert-AvidScriptPluginReleaseJsonSchema $path (Join-Path $PluginRoot 'Build/ReleaseEngineering/AvidScriptPluginRelease.schema.json') 'fixture manifest' } 'ASRE'
        }
    }
    if ($Passed -ne 18) { throw "unexpected contract count $Passed/18" }
    Write-Output "Win64 bundled runtime contracts: $Passed/18 passed"
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        $resolved = (Resolve-Path -LiteralPath $FixtureRoot).Path
        if (-not (Test-AvidScriptPluginReleasePathUnderRoot $resolved $FixtureBase) -or
            [IO.Path]::GetFileName($resolved) -notmatch '^awb-[0-9a-f]{8}$') { throw 'fixture cleanup escaped its root' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
