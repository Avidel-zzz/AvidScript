[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Preflight = Join-Path (Split-Path -Parent $PSScriptRoot) 'TestAvidScriptAndroidToolchain.ps1'
$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('AvidScriptAndroidToolchainContracts-' + [Guid]::NewGuid().ToString('N'))
$Passed = 0
$Total = 0
$Failures = [System.Collections.Generic.List[string]]::new()
$EnvironmentNames = @('ANDROID_HOME', 'ANDROID_SDK_ROOT', 'NDKROOT', 'NDK_ROOT', 'ANDROID_NDK_ROOT', 'JAVA_HOME')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-Contract {
    param([string]$Name, [scriptblock]$Body)
    ++$script:Total
    try { & $Body; ++$script:Passed }
    catch { $Failures.Add("${Name}: $($_.Exception.Message)") }
}

function Write-FixtureFile {
    param([string]$Path, [string]$Content = 'fixture; never execute')
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Path))
    [System.IO.File]::WriteAllText($Path, $Content)
}

function New-Fixture {
    $Root = Join-Path $FixtureRoot ([Guid]::NewGuid().ToString('N'))
    $Fixture = @{
        Engine = (Join-Path $Root 'EngineRoot')
        Sdk = (Join-Path $Root 'SDK with spaces')
        Java = (Join-Path $Root 'Java with spaces')
        Root = $Root
    }
    $Fixture.Ndk = Join-Path $Fixture.Sdk 'ndk/27.2.12479018'
    Write-FixtureFile (Join-Path $Fixture.Engine 'Engine/Build/Build.version') '{"MajorVersion":5,"MinorVersion":8,"PatchVersion":0}'
    Write-FixtureFile (Join-Path $Fixture.Engine 'Engine/Config/Android/Android_SDK.json') '{"MainVersion":"r27c","MinVersion":"r27c","MaxVersion":"r29","platforms":"android-34","build-tools":"35.0.1","cmake":"3.22.1","ndk":"27.2.12479018"}'
    Write-FixtureFile (Join-Path $Fixture.Engine 'Engine/Source/Programs/UnrealBuildTool/Platform/Android/AndroidPlatformSDK.cs') 'if (JavaVersion < 17) { return false; }'
    Write-FixtureFile (Join-Path $Fixture.Sdk 'platforms/android-34/source.properties') 'AndroidVersion.ApiLevel = 34'
    Write-FixtureFile (Join-Path $Fixture.Sdk 'build-tools/35.0.1/source.properties') 'Pkg.Revision = 35.0.1'
    Write-FixtureFile (Join-Path $Fixture.Sdk 'cmake/3.22.1/source.properties') 'Pkg.Revision = 3.22.1'
    Write-FixtureFile (Join-Path $Fixture.Ndk 'source.properties') 'Pkg.Revision = 27.2.12479018'
    Write-FixtureFile (Join-Path $Fixture.Java 'release') 'JAVA_VERSION="17.0.12"'
    foreach ($File in @('platforms/android-34/android.jar', 'build-tools/35.0.1/aapt.exe', 'build-tools/35.0.1/aapt2.exe',
        'build-tools/35.0.1/zipalign.exe', 'build-tools/35.0.1/apksigner.bat',
        'cmake/3.22.1/bin/cmake.exe', 'cmake/3.22.1/bin/ninja.exe',
        'platform-tools/adb.exe', 'cmdline-tools/latest/bin/sdkmanager.bat')) {
        Write-FixtureFile (Join-Path $Fixture.Sdk $File)
    }
    foreach ($File in @('clang++.exe', 'llvm-ar.exe', 'ld.lld.exe')) {
        Write-FixtureFile (Join-Path $Fixture.Ndk "toolchains/llvm/prebuilt/windows-x86_64/bin/$File")
    }
    [void][System.IO.Directory]::CreateDirectory((Join-Path $Fixture.Ndk 'toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android'))
    foreach ($File in @('java.exe', 'javac.exe', 'jar.exe')) {
        Write-FixtureFile (Join-Path $Fixture.Java "bin/$File")
    }
    return $Fixture
}

function Invoke-Preflight {
    param($Fixture, [hashtable]$Overrides = @{}, [hashtable]$ChildEnvironment = @{}, [string[]]$Omit = @())
    $Parameters = [ordered]@{
        EngineRoot = $Fixture.Engine; AndroidSdkRoot = $Fixture.Sdk
        NdkRoot = $Fixture.Ndk; JavaHome = $Fixture.Java
    }
    foreach ($Name in $Omit) { $Parameters.Remove($Name) }
    foreach ($Name in $Overrides.Keys) { $Parameters[$Name] = $Overrides[$Name] }
    $Start = [System.Diagnostics.ProcessStartInfo]::new()
    $Start.FileName = Join-Path $PSHOME 'pwsh.exe'
    $Start.UseShellExecute = $false
    $Start.CreateNoWindow = $true
    $Start.RedirectStandardOutput = $true
    $Start.RedirectStandardError = $true
    foreach ($Argument in @('-NoProfile', '-File', $Preflight)) { $Start.ArgumentList.Add($Argument) }
    foreach ($Name in $Parameters.Keys) {
        $Start.ArgumentList.Add("-$Name")
        $Start.ArgumentList.Add([string]$Parameters[$Name])
    }
    foreach ($Name in $EnvironmentNames) { [void]$Start.Environment.Remove($Name) }
    foreach ($Name in $ChildEnvironment.Keys) { $Start.Environment[$Name] = $ChildEnvironment[$Name] }
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $Start
    try {
        [void]$Process.Start()
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit(20000)) {
            $Process.Kill($true)
            $Process.WaitForExit()
            throw 'Preflight timed out.'
        }
        $Stdout = $StdoutTask.GetAwaiter().GetResult().Trim()
        $Stderr = $StderrTask.GetAwaiter().GetResult()
        Assert-True ([string]::IsNullOrWhiteSpace($Stderr)) "Unexpected stderr: $Stderr"
        Assert-True (-not $Stdout.Contains("`n")) 'Expected exactly one JSON line.'
        $Json = $Stdout | ConvertFrom-Json
        Assert-True ($Json.schema_version -eq 1 -and $Json.ready -is [bool]) 'Invalid schema/ready type.'
        Assert-True ($Json.checks -is [array] -and $Json.checks.Count -gt 0) 'Checks must be a nonempty array.'
        foreach ($Check in $Json.checks) {
            Assert-True ($Check.status -cin @('ok', 'blocked') -and $null -ne $Check.expected -and $null -ne $Check.actual) 'Malformed check.'
        }
        $ExpectedExit = if ($Json.ready) { 0 } else { 2 }
        $ExpectedStatus = if ($Json.ready) { 'ok' } else { 'blocked' }
        $ExpectedResult = if ($Json.ready) { 'avidscript_android_toolchain_ready' } else { 'avidscript_android_toolchain_blocked' }
        Assert-True ($Process.ExitCode -eq $ExpectedExit -and $Json.status -ceq $ExpectedStatus -and $Json.result -ceq $ExpectedResult) 'Exit/status/result mismatch.'
        return @{ json = $Json; stdout = $Stdout }
    }
    finally { $Process.Dispose() }
}

function Assert-Blocked {
    param($Result, [string]$Id)
    Assert-True (-not $Result.json.ready) 'Unexpected ready result.'
    Assert-True (@($Result.json.checks | Where-Object { $_.id -ceq $Id -and $_.status -ceq 'blocked' }).Count -eq 1) "Missing blocked check: $Id"
}

try {
    Invoke-Contract 'ready fixture and no filesystem writes or tool execution' {
        $F = New-Fixture
        $Before = @(Get-ChildItem -LiteralPath $F.Root -Recurse -File | Get-FileHash | Select-Object Path, Hash) | ConvertTo-Json -Compress
        $R = Invoke-Preflight $F
        Assert-True $R.json.ready 'Ready fixture blocked.'
        Assert-True ($R.json.toolchain.sdk_root -ceq $F.Sdk -and $R.json.toolchain.ndk_root -ceq $F.Ndk -and $R.json.toolchain.java_home -ceq $F.Java) 'Resolved paths differ.'
        Assert-True ($R.json.toolchain.adb_path -ceq (Join-Path $F.Sdk 'platform-tools/adb.exe')) 'ADB path differs.'
        $After = @(Get-ChildItem -LiteralPath $F.Root -Recurse -File | Get-FileHash | Select-Object Path, Hash) | ConvertTo-Json -Compress
        Assert-True ($Before -ceq $After) 'Preflight changed the fixture.'
    }
    Invoke-Contract 'missing roots' {
        $F = New-Fixture
        $R = Invoke-Preflight $F @{ AndroidSdkRoot = "$($F.Root)/absent-sdk"; NdkRoot = "$($F.Root)/absent-ndk"; JavaHome = "$($F.Root)/absent-java" }
        foreach ($Id in @('sdk_root', 'ndk_root', 'java_home', 'ndk_version', 'java_version', 'adb')) { Assert-Blocked $R $Id }
    }
    foreach ($Case in @(
        @('Sdk', 'platforms/android-34/android.jar', 'sdk_platform_files'),
        @('Sdk', 'platform-tools/adb.exe', 'adb'),
        @('Sdk', 'cmdline-tools/latest/bin/sdkmanager.bat', 'sdkmanager'),
        @('Sdk', 'build-tools/35.0.1/aapt2.exe', 'build_tools_files'),
        @('Sdk', 'cmake/3.22.1/bin/ninja.exe', 'cmake_files'),
        @('Ndk', 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe', 'ndk_tools'),
        @('Java', 'bin/javac.exe', 'java_tools'),
        @('Ndk', 'source.properties', 'ndk_version'),
        @('Java', 'release', 'java_version'))) {
        Invoke-Contract "missing $($Case[1])" {
            $F = New-Fixture
            [System.IO.File]::Delete((Join-Path $F[$Case[0]] $Case[1]))
            Assert-Blocked (Invoke-Preflight $F) $Case[2]
        }
    }
    foreach ($Case in @(
        @('Sdk', 'platforms/android-34/source.properties', 'AndroidVersion.ApiLevel=35', 'sdk_platform_version'),
        @('Sdk', 'build-tools/35.0.1/source.properties', 'Pkg.Revision=35.0.0', 'build_tools_version'),
        @('Sdk', 'cmake/3.22.1/source.properties', 'Pkg.Revision=3.31.0', 'cmake_version'),
        @('Ndk', 'source.properties', 'Pkg.Revision=29.0.0', 'ndk_version'),
        @('Java', 'release', 'JAVA_VERSION="11.0.20"', 'java_version'),
        @('Java', 'release', 'JAVA_VERSION="not-a-version"', 'java_version'),
        @('Ndk', 'source.properties', "Pkg.Revision=27.2.12479018`nPkg.Revision=29.0.0", 'ndk_version'))) {
        Invoke-Contract "wrong metadata $($Case[2])" {
            $F = New-Fixture
            Write-FixtureFile (Join-Path $F[$Case[0]] $Case[1]) $Case[2]
            Assert-Blocked (Invoke-Preflight $F) $Case[3]
        }
    }
    Invoke-Contract 'wrong engine version' {
        $F = New-Fixture
        Write-FixtureFile (Join-Path $F.Engine 'Engine/Build/Build.version') '{"MajorVersion":5,"MinorVersion":7}'
        Assert-Blocked (Invoke-Preflight $F) 'engine_version'
    }
    Invoke-Contract 'arm64 sysroot is required' {
        $F = New-Fixture
        [System.IO.Directory]::Delete((Join-Path $F.Ndk 'toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android'))
        Assert-Blocked (Invoke-Preflight $F) 'ndk_arm64'
    }
    Invoke-Contract 'engine package pins are authoritative' {
        $F = New-Fixture
        $Path = Join-Path $F.Engine 'Engine/Config/Android/Android_SDK.json'
        $Config = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json -AsHashtable
        $Config['build-tools'] = '35.0.2'
        Write-FixtureFile $Path ($Config | ConvertTo-Json -Compress)
        $R = Invoke-Preflight $F
        Assert-Blocked $R 'build_tools_version'
        Assert-True ($R.json.requirements.build_tools -ceq '35.0.2') 'Ignored engine package pin.'
    }
    Invoke-Contract 'malformed engine configuration' {
        $F = New-Fixture
        Write-FixtureFile (Join-Path $F.Engine 'Engine/Config/Android/Android_SDK.json') '{"platforms":"../../other"}'
        Assert-Blocked (Invoke-Preflight $F) 'engine_android_config'
    }
    Invoke-Contract 'missing engine configuration' {
        $F = New-Fixture
        [System.IO.File]::Delete((Join-Path $F.Engine 'Engine/Config/Android/Android_SDK.json'))
        Assert-Blocked (Invoke-Preflight $F) 'preflight_error'
    }
    Invoke-Contract 'engine Java minimum is authoritative' {
        $F = New-Fixture
        Write-FixtureFile (Join-Path $F.Engine 'Engine/Source/Programs/UnrealBuildTool/Platform/Android/AndroidPlatformSDK.cs') 'if (JavaVersion < 21) { return false; }'
        Assert-Blocked (Invoke-Preflight $F) 'java_version'
        Write-FixtureFile (Join-Path $F.Java 'release') 'JAVA_VERSION="21.0.4+7"'
        Assert-True (Invoke-Preflight $F).json.ready 'JDK21 should satisfy engine minimum21.'
    }
    Invoke-Contract 'unknown Java policy fails closed' {
        $F = New-Fixture
        Write-FixtureFile (Join-Path $F.Engine 'Engine/Source/Programs/UnrealBuildTool/Platform/Android/AndroidPlatformSDK.cs') 'changed engine policy'
        Assert-Blocked (Invoke-Preflight $F) 'engine_java_requirement'
    }
    Invoke-Contract 'environment aliases and pinned NDK default' {
        $F = New-Fixture
        $R = Invoke-Preflight $F -Omit @('AndroidSdkRoot', 'NdkRoot', 'JavaHome') -ChildEnvironment @{ ANDROID_SDK_ROOT = $F.Sdk; NDK_ROOT = $F.Ndk; JAVA_HOME = $F.Java }
        Assert-True ($R.json.ready -and $R.json.discovery.sdk_root -ceq 'environment:ANDROID_SDK_ROOT') 'Environment discovery failed.'
        $R = Invoke-Preflight $F -Omit @('NdkRoot')
        Assert-True ($R.json.ready -and $R.json.discovery.ndk_root -ceq 'ue_default') 'Pinned SDK/ndk default failed.'
    }
    Invoke-Contract 'explicit parameters override environment' {
        $F = New-Fixture
        $R = Invoke-Preflight $F -ChildEnvironment @{ ANDROID_HOME = "$($F.Root)/bad"; NDKROOT = "$($F.Root)/bad"; JAVA_HOME = "$($F.Root)/bad" }
        Assert-True $R.json.ready 'Explicit paths did not win.'
        $R = Invoke-Preflight $F @{ AndroidSdkRoot = "$($F.Root)/bad" } -ChildEnvironment @{ ANDROID_HOME = $F.Sdk }
        Assert-Blocked $R 'sdk_root'
    }
    Invoke-Contract 'invalid primary environment does not fall back' {
        $F = New-Fixture
        $R = Invoke-Preflight $F -Omit @('AndroidSdkRoot') -ChildEnvironment @{ ANDROID_HOME = "$($F.Root)/bad"; ANDROID_SDK_ROOT = $F.Sdk }
        Assert-Blocked $R 'sdk_root'
        Assert-True ($R.json.discovery.sdk_root -ceq 'environment:ANDROID_HOME') 'Environment precedence changed.'
    }
    Invoke-Contract 'optional JSON report and overwrite refusal' {
        $F = New-Fixture
        $Report = Join-Path $F.Root 'report.json'
        $R = Invoke-Preflight $F @{ OutputPath = $Report }
        Assert-True ($R.json.ready -and [System.IO.File]::ReadAllText($Report).Trim() -ceq $R.stdout) 'Report differs from stdout.'
        $Original = [System.IO.File]::ReadAllText($Report)
        Assert-Blocked (Invoke-Preflight $F @{ OutputPath = $Report }) 'output_report'
        Assert-True ([System.IO.File]::ReadAllText($Report) -ceq $Original) 'Existing report was overwritten.'
    }
    Invoke-Contract 'missing output parent is not created' {
        $F = New-Fixture
        $Parent = Join-Path $F.Root 'absent'
        Assert-Blocked (Invoke-Preflight $F @{ OutputPath = "$Parent/report.json" }) 'output_report'
        Assert-True (-not [System.IO.Directory]::Exists($Parent)) 'Created output parent.'
    }
}
finally {
    $Resolved = [System.IO.Path]::GetFullPath($FixtureRoot)
    $Temp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $Resolved.StartsWith($Temp, [StringComparison]::OrdinalIgnoreCase) -or
        -not [System.IO.Path]::GetFileName($Resolved).StartsWith('AvidScriptAndroidToolchainContracts-')) {
        throw 'Refusing cleanup outside the owned temporary fixture root.'
    }
    if ([System.IO.Directory]::Exists($Resolved)) { Remove-Item -LiteralPath $Resolved -Recurse -Force }
}

[ordered]@{ status = $(if ($Passed -eq $Total) { 'ok' } else { 'failed' }); passed = $Passed; total = $Total; failures = @($Failures.ToArray()) } |
    ConvertTo-Json -Depth 5 -Compress
if ($Passed -ne $Total) { exit 1 }
exit 0
