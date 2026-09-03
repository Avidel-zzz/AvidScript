[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$AndroidSdkRoot = '',
    [string]$NdkRoot = '',
    [string]$JavaHome = '',
    [string]$OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Checks = [System.Collections.Generic.List[object]]::new()
$Toolchain = [ordered]@{ sdk_root = ''; ndk_root = ''; java_home = ''; adb_path = '' }
$Requirements = [ordered]@{}
$Discovery = [ordered]@{}

function Add-Check {
    param([string]$Id, [bool]$Passed, $Expected, $Actual)
    $Checks.Add([ordered]@{
        id = $Id
        status = $(if ($Passed) { 'ok' } else { 'blocked' })
        expected = $Expected
        actual = $Actual
    })
}

function Join-ToolPath {
    param([string]$Root, [string]$Child)
    if ([string]::IsNullOrWhiteSpace($Root)) { return '' }
    return [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($Root, $Child))
}

function Get-LocalPath {
    param([string]$Path)
    $Value = [Environment]::ExpandEnvironmentVariables($Path.Trim().Trim('"'))
    if ([string]::IsNullOrWhiteSpace($Value)) { return '' }
    $FullPath = [System.IO.Path]::GetFullPath($Value)
    if ($FullPath.StartsWith('\\') -or $FullPath.StartsWith('//')) {
        throw 'Only local filesystem paths are supported.'
    }
    return $FullPath
}

function Find-ToolRoot {
    param([string]$Explicit, [string[]]$EnvironmentNames, [string[]]$Defaults)
    # A configured but invalid path must not silently fall back to another installation.
    if (-not [string]::IsNullOrWhiteSpace($Explicit)) {
        return @{ path = (Get-LocalPath $Explicit); source = 'parameter' }
    }
    foreach ($Name in $EnvironmentNames) {
        $Value = [Environment]::GetEnvironmentVariable($Name)
        if (-not [string]::IsNullOrWhiteSpace($Value)) {
            return @{ path = (Get-LocalPath $Value); source = "environment:$Name" }
        }
    }
    $Candidates = @($Defaults | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    foreach ($Candidate in $Candidates) {
        $Path = Get-LocalPath $Candidate
        if ([System.IO.Directory]::Exists($Path)) {
            return @{ path = $Path; source = 'ue_default' }
        }
    }
    return @{
        path = $(if ($Candidates.Count -gt 0) { Get-LocalPath $Candidates[0] } else { '' })
        source = 'ue_default'
    }
}

function Get-PropertyValue {
    param([string]$Path, [string]$Name)
    if (-not [System.IO.File]::Exists($Path)) { return '' }
    try {
        $Properties = ConvertFrom-StringData -StringData ([System.IO.File]::ReadAllText($Path))
        if ($Properties.ContainsKey($Name)) { return ([string]$Properties[$Name]).Trim().Trim('"') }
    }
    catch { return '' }
    return ''
}

function Test-ToolFiles {
    param([string]$Id, [string]$Root, [string[]]$Files)
    $Missing = @($Files | Where-Object {
        -not [System.IO.File]::Exists((Join-ToolPath $Root $_))
    })
    Add-Check $Id ($Missing.Count -eq 0) $Files @{ root = $Root; missing = $Missing }
}

function Test-PackageVersion {
    param([string]$Id, [string]$Root, [string]$Expected, [string]$Property = 'Pkg.Revision')
    $Actual = Get-PropertyValue (Join-ToolPath $Root 'source.properties') $Property
    Add-Check $Id ($Actual -ceq $Expected -and $Expected -ne '') $Expected $Actual
}

try {
    $EngineRoot = Get-LocalPath $EngineRoot
    $VersionPath = Join-ToolPath $EngineRoot 'Engine/Build/Build.version'
    $Version = Get-Content -Raw -LiteralPath $VersionPath | ConvertFrom-Json -AsHashtable
    $IsUE58 = $Version['MajorVersion'] -eq 5 -and $Version['MinorVersion'] -eq 8
    Add-Check 'engine_version' $IsUE58 '5.8' $Version

    $ConfigPath = Join-ToolPath $EngineRoot 'Engine/Config/Android/Android_SDK.json'
    $Config = Get-Content -Raw -LiteralPath $ConfigPath | ConvertFrom-Json -AsHashtable
    $ConfigValid = $Config['platforms'] -cmatch '^android-[1-9][0-9]*$' -and
        $Config['build-tools'] -cmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -and
        $Config['cmake'] -cmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -and
        $Config['ndk'] -cmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -and
        $Config['MainVersion'] -cmatch '^r[0-9]+[a-z]?$'
    Add-Check 'engine_android_config' $ConfigValid 'Exact package versions from Android_SDK.json' $Config
    if (-not $ConfigValid) { throw 'Android_SDK.json is missing or has invalid package versions.' }
    $Requirements = [ordered]@{
        platform = [string]$Config['platforms']
        build_tools = [string]$Config['build-tools']
        cmake = [string]$Config['cmake']
        ndk = [string]$Config['ndk']
        ndk_release = [string]$Config['MainVersion']
        architecture = 'arm64-v8a'
        java_minimum = $null
    }

    $JavaPolicyPath = Join-ToolPath $EngineRoot 'Engine/Source/Programs/UnrealBuildTool/Platform/Android/AndroidPlatformSDK.cs'
    $JavaPolicy = Get-Content -Raw -LiteralPath $JavaPolicyPath
    $JavaMatches = [regex]::Matches($JavaPolicy, 'if\s*\(\s*JavaVersion\s*<\s*(\d+)\s*\)')
    $JavaPolicyValid = $JavaMatches.Count -eq 1 -and [int]$JavaMatches[0].Groups[1].Value -ge 17
    Add-Check 'engine_java_requirement' $JavaPolicyValid 'One JavaVersion minimum >= 17 in AndroidPlatformSDK.cs' @($JavaMatches | ForEach-Object { $_.Value })
    if (-not $JavaPolicyValid) { throw 'Cannot determine a supported Java minimum from this engine.' }
    $Requirements.java_minimum = [int]$JavaMatches[0].Groups[1].Value

    $StudioPath = ''
    $StudioSdk = ''
    if ($IsWindows) {
        $StudioPath = [string][Microsoft.Win32.Registry]::GetValue('HKEY_LOCAL_MACHINE\SOFTWARE\Android Studio', 'Path', '')
        $StudioSdk = [string][Microsoft.Win32.Registry]::GetValue('HKEY_LOCAL_MACHINE\SOFTWARE\Android Studio', 'SdkPath', '')
    }
    $LocalAppData = [Environment]::GetEnvironmentVariable('LOCALAPPDATA')
    $ProgramFiles = [Environment]::GetEnvironmentVariable('ProgramFiles')
    $Sdk = Find-ToolRoot $AndroidSdkRoot @('ANDROID_HOME', 'ANDROID_SDK_ROOT') @(
        $StudioSdk, (Join-ToolPath $LocalAppData 'Android/Sdk'))
    $Ndk = Find-ToolRoot $NdkRoot @('NDKROOT', 'NDK_ROOT', 'ANDROID_NDK_ROOT') @(
        (Join-ToolPath $Sdk.path "ndk/$($Requirements.ndk)"))
    $JavaDefaults = @(
        (Join-ToolPath $StudioPath 'jbr'),
        (Join-ToolPath $LocalAppData 'Programs/Android Studio/jbr'),
        (Join-ToolPath $ProgramFiles 'Android/Android Studio/jbr'),
        (Join-ToolPath $StudioPath 'jre'))
    $Java = Find-ToolRoot $JavaHome @('JAVA_HOME') $JavaDefaults
    $Toolchain.sdk_root = $Sdk.path
    $Toolchain.ndk_root = $Ndk.path
    $Toolchain.java_home = $Java.path
    $Toolchain.adb_path = Join-ToolPath $Sdk.path 'platform-tools/adb.exe'
    $Discovery = [ordered]@{ sdk_root = $Sdk.source; ndk_root = $Ndk.source; java_home = $Java.source }
    foreach ($Entry in @(@('sdk_root', $Sdk.path), @('ndk_root', $Ndk.path), @('java_home', $Java.path))) {
        Add-Check $Entry[0] ([System.IO.Directory]::Exists($Entry[1])) 'Existing directory' $Entry[1]
    }

    $PlatformRoot = Join-ToolPath $Sdk.path "platforms/$($Requirements.platform)"
    Test-PackageVersion 'sdk_platform_version' $PlatformRoot $Requirements.platform.Substring(8) 'AndroidVersion.ApiLevel'
    Test-ToolFiles 'sdk_platform_files' $PlatformRoot @('android.jar')
    $BuildToolsRoot = Join-ToolPath $Sdk.path "build-tools/$($Requirements.build_tools)"
    Test-PackageVersion 'build_tools_version' $BuildToolsRoot $Requirements.build_tools
    Test-ToolFiles 'build_tools_files' $BuildToolsRoot @('aapt.exe', 'aapt2.exe', 'zipalign.exe', 'apksigner.bat')
    $CMakeRoot = Join-ToolPath $Sdk.path "cmake/$($Requirements.cmake)"
    Test-PackageVersion 'cmake_version' $CMakeRoot $Requirements.cmake
    Test-ToolFiles 'cmake_files' $CMakeRoot @('bin/cmake.exe', 'bin/ninja.exe')
    Test-ToolFiles 'adb' $Sdk.path @('platform-tools/adb.exe')

    $CommandToolsRoot = Join-ToolPath $Sdk.path 'cmdline-tools'
    $SdkManager = Join-ToolPath $CommandToolsRoot 'latest/bin/sdkmanager.bat'
    if (-not [System.IO.File]::Exists($SdkManager) -and [System.IO.Directory]::Exists($CommandToolsRoot)) {
        foreach ($Directory in (Get-ChildItem -LiteralPath $CommandToolsRoot -Directory | Sort-Object Name -Descending)) {
            $Candidate = Join-ToolPath $Directory.FullName 'bin/sdkmanager.bat'
            if ([System.IO.File]::Exists($Candidate)) { $SdkManager = $Candidate; break }
        }
    }
    Add-Check 'sdkmanager' ([System.IO.File]::Exists($SdkManager)) 'cmdline-tools/*/bin/sdkmanager.bat' $SdkManager
    Test-PackageVersion 'ndk_version' $Ndk.path $Requirements.ndk
    $LlvmRoot = Join-ToolPath $Ndk.path 'toolchains/llvm/prebuilt/windows-x86_64'
    Test-ToolFiles 'ndk_tools' $LlvmRoot @('bin/clang++.exe', 'bin/llvm-ar.exe', 'bin/ld.lld.exe')
    $Arm64Root = Join-ToolPath $LlvmRoot 'sysroot/usr/lib/aarch64-linux-android'
    Add-Check 'ndk_arm64' ([System.IO.Directory]::Exists($Arm64Root)) 'AArch64 Android sysroot library directory' $Arm64Root
    $JavaVersion = Get-PropertyValue (Join-ToolPath $Java.path 'release') 'JAVA_VERSION'
    $JavaMajor = 0
    if ($JavaVersion -cmatch '^(?<major>[1-9][0-9]*)(?:[.][0-9]+)*(?:[+_-][A-Za-z0-9.+_-]+)?$') {
        [void][int]::TryParse($Matches.major, [ref]$JavaMajor)
    }
    Add-Check 'java_version' ($JavaMajor -ge $Requirements.java_minimum) ">= $($Requirements.java_minimum)" $JavaVersion
    Test-ToolFiles 'java_tools' $Java.path @('bin/java.exe', 'bin/javac.exe', 'bin/jar.exe')
}
catch {
    Add-Check 'preflight_error' $false 'Readable UE5.8 configuration and local toolchain metadata' $_.Exception.Message
}

$ReportPath = ''
if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    try {
        $ReportPath = Get-LocalPath $OutputPath
        $Parent = [System.IO.Path]::GetDirectoryName($ReportPath)
        if (-not [System.IO.Directory]::Exists($Parent) -or (Test-Path -LiteralPath $ReportPath)) {
            throw 'OutputPath must be a new file in an existing local directory.'
        }
        Add-Check 'output_report' $true 'New local JSON report; no existing file overwritten' $ReportPath
    }
    catch {
        $ReportPath = ''
        Add-Check 'output_report' $false 'New file in an existing local directory' $_.Exception.Message
    }
}

function Get-ResultJson {
    $Ready = @($Checks | Where-Object { $_.status -ne 'ok' }).Count -eq 0
    return ([ordered]@{
        schema_version = 1
        status = $(if ($Ready) { 'ok' } else { 'blocked' })
        ready = $Ready
        result = $(if ($Ready) { 'avidscript_android_toolchain_ready' } else { 'avidscript_android_toolchain_blocked' })
        checks = @($Checks.ToArray())
        toolchain = $Toolchain
        requirements = $Requirements
        discovery = $Discovery
    } | ConvertTo-Json -Depth 12 -Compress)
}

$Json = Get-ResultJson
if ($ReportPath -ne '') {
    $Stream = $null
    try {
        # CreateNew also prevents a race from overwriting a file after the path check.
        $Stream = [System.IO.File]::Open($ReportPath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write)
        $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Json + [Environment]::NewLine)
        $Stream.Write($Bytes, 0, $Bytes.Length)
    }
    catch {
        $Checks[$Checks.Count - 1].status = 'blocked'
        $Checks[$Checks.Count - 1].actual = $_.Exception.Message
        $Json = Get-ResultJson
    }
    finally { if ($null -ne $Stream) { $Stream.Dispose() } }
}
[Console]::Out.WriteLine($Json)
if (@($Checks | Where-Object { $_.status -ne 'ok' }).Count -gt 0) { exit 2 }
exit 0
