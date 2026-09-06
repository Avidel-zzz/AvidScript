$script:AvidScriptCompatibilityDoctorRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $script:AvidScriptCompatibilityDoctorRoot 'AvidScriptPluginReleasePackage.ps1')
. (Join-Path $script:AvidScriptCompatibilityDoctorRoot 'AvidScriptPluginInstaller.ps1')
. (Join-Path (Split-Path -Parent $script:AvidScriptCompatibilityDoctorRoot) 'AvidScriptCSharpBindingPackage.ps1')

function Add-AvidScriptCompatibilityCheck {
    param(
        [Parameter(Mandatory = $true)]$Checks,
        [Parameter(Mandatory = $true)][string]$Id,
        [Parameter(Mandatory = $true)][ValidateSet('host', 'plugin', 'project', 'platform')][string]$Area,
        [Parameter(Mandatory = $true)][ValidateSet('passed', 'warning', 'blocked', 'not_run')][string]$Status,
        [Parameter(Mandatory = $true)][string]$Code,
        [string]$Expected = '',
        [string]$Actual = '',
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$Remediation = ''
    )

    $Severity = switch ($Status) {
        'blocked' { 'error' }
        'warning' { 'warning' }
        default { 'info' }
    }
    $Category = switch ($Status) {
        'blocked' { 'incompatible' }
        'warning' { 'degraded' }
        'not_run' { 'not_run' }
        default { 'compatible' }
    }
    $Checks.Add([pscustomobject][ordered]@{
            id = $Id
            area = $Area
            status = $Status
            severity = $Severity
            code = $Code
            category = $Category
            expected = $Expected
            actual = $Actual
            message = $Message
            remediation = $Remediation
        })
}

function Invoke-AvidScriptCompatibilityProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [string[]]$Arguments = @(),
        [ValidateRange(1, 300)][int]$TimeoutSeconds = 30
    )

    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $Executable
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    $StartInfo.Environment['DOTNET_CLI_TELEMETRY_OPTOUT'] = '1'
    $StartInfo.Environment['DOTNET_SKIP_FIRST_TIME_EXPERIENCE'] = '1'
    foreach ($Argument in $Arguments) {
        [void]$StartInfo.ArgumentList.Add($Argument)
    }
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    try {
        if (-not $Process.Start()) {
            throw "process could not be started: $Executable"
        }
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        $TimedOut = -not $Process.WaitForExit($TimeoutSeconds * 1000)
        if ($TimedOut) {
            $Process.Kill($true)
            $Process.WaitForExit()
        }
        $Stderr = $StderrTask.GetAwaiter().GetResult().Trim()
        if ($TimedOut) {
            $Stderr = "probe timed out after $TimeoutSeconds seconds. $Stderr".Trim()
        }
        return [pscustomobject][ordered]@{
            exit_code = $(if ($TimedOut) { -2 } else { $Process.ExitCode })
            stdout = $StdoutTask.GetAwaiter().GetResult().Trim()
            stderr = $Stderr
        }
    }
    catch {
        return [pscustomobject][ordered]@{
            exit_code = -1
            stdout = ''
            stderr = $_.Exception.Message
        }
    }
    finally {
        $Process.Dispose()
    }
}

function Test-AvidScriptCompatibilityOrdinaryRoot {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return $false
    }
    return (((Get-Item -LiteralPath $Path -Force).Attributes -band
            [System.IO.FileAttributes]::ReparsePoint) -eq 0)
}

function Get-AvidScriptCompatibilityJson {
    param([Parameter(Mandatory = $true)][string]$Path)

    try {
        return Read-AvidScriptPluginReleaseJsonObject $Path 'compatibility input'
    }
    catch {
        return $null
    }
}

function Get-AvidScriptCompatibilityDoctorReport {
    param(
        [Parameter(Mandatory = $true)][string]$PluginRoot,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$EngineRoot,
        [Parameter(Mandatory = $true)][string]$DotNetPath,
        [switch]$SkipExternalTools
    )

    $PluginRoot = [System.IO.Path]::GetFullPath($PluginRoot)
    $ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
    $EngineRoot = [System.IO.Path]::GetFullPath($EngineRoot)
    $DotNetPath = [System.IO.Path]::GetFullPath($DotNetPath)
    $Checks = [System.Collections.Generic.List[object]]::new()
    $Identity = [ordered]@{
        plugin_version = ''
        engine_version = ''
        dotnet_sdk = ''
        powershell = [string]$PSVersionTable.PSVersion
        install_release_id = ''
        binding_package_sha256 = ''
        generated_type_package_id = ''
        wasmtime_toolchain_id = ''
    }

    Add-AvidScriptCompatibilityCheck $Checks 'host.os.windows' 'host' `
        $(if ($IsWindows) { 'passed' } else { 'blocked' }) 'ASCD1001' `
        'Windows 10/11 x64' ([System.Runtime.InteropServices.RuntimeInformation]::OSDescription) `
        'AvidScript Developer Preview host platform check.' `
        'Use the current Windows Developer Preview platform.'

    $Architecture = [string][System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
    Add-AvidScriptCompatibilityCheck $Checks 'host.os.architecture' 'host' `
        $(if ($Architecture -ceq 'X64') { 'passed' } else { 'blocked' }) 'ASCD1007' `
        'X64' $Architecture 'Host architecture check.' 'Use the Win64 x64 Developer Preview host.'

    $PowerShellReady = $PSVersionTable.PSVersion -ge [version]'7.4.0'
    Add-AvidScriptCompatibilityCheck $Checks 'host.powershell.version' 'host' `
        $(if ($PowerShellReady) { 'passed' } else { 'blocked' }) 'ASCD1002' `
        '>= 7.4.0' ([string]$PSVersionTable.PSVersion) `
        'PowerShell runtime version check.' 'Install PowerShell 7.4 or newer.'

    $EngineVersionPath = Join-Path $EngineRoot 'Engine/Build/Build.version'
    $EngineVersion = Get-AvidScriptCompatibilityJson $EngineVersionPath
    $EngineReady = $null -ne $EngineVersion -and
        [int]$EngineVersion.MajorVersion -eq 5 -and [int]$EngineVersion.MinorVersion -eq 8
    if ($null -ne $EngineVersion) {
        $Identity.engine_version = "$($EngineVersion.MajorVersion).$($EngineVersion.MinorVersion).$($EngineVersion.PatchVersion)"
    }
    Add-AvidScriptCompatibilityCheck $Checks 'host.engine.version' 'host' `
        $(if ($EngineReady) { 'passed' } else { 'blocked' }) 'ASCD1003' `
        'UE 5.8 source tree' $Identity.engine_version 'Unreal Engine source version check.' `
        'Pass the UE5.8 source root with -EngineRoot.'

    $EngineSourceReady = Test-Path -LiteralPath (
        Join-Path $EngineRoot 'Engine/Source/Programs/UnrealBuildTool') -PathType Container
    Add-AvidScriptCompatibilityCheck $Checks 'host.engine.source' 'host' `
        $(if ($EngineSourceReady) { 'passed' } else { 'blocked' }) 'ASCD1004' `
        'UnrealBuildTool source directory' $(if ($EngineSourceReady) { 'present' } else { 'missing' }) `
        'Unreal Engine source distribution check.' 'Use a complete source build of Unreal Engine 5.8.'

    if ($SkipExternalTools) {
        Add-AvidScriptCompatibilityCheck $Checks 'host.dotnet.version' 'host' 'not_run' 'ASCD1005' `
            '8.0.416' '' 'External .NET version execution was skipped.' 'Run again without -SkipExternalTools.'
    }
    elseif (-not (Test-Path -LiteralPath $DotNetPath -PathType Leaf)) {
        Add-AvidScriptCompatibilityCheck $Checks 'host.dotnet.version' 'host' 'blocked' 'ASCD1005' `
            '8.0.416' 'missing' 'Pinned .NET host is unavailable.' 'Install .NET SDK 8.0.416 or pass -DotNetPath.'
    }
    else {
        $DotNet = Invoke-AvidScriptCompatibilityProcess $DotNetPath @('--version')
        $Identity.dotnet_sdk = $DotNet.stdout.Split([Environment]::NewLine)[0].Trim()
        Add-AvidScriptCompatibilityCheck $Checks 'host.dotnet.version' 'host' `
            $(if ($DotNet.exit_code -eq 0 -and $Identity.dotnet_sdk -ceq '8.0.416') { 'passed' } else { 'blocked' }) `
            'ASCD1005' '8.0.416' $Identity.dotnet_sdk 'Pinned .NET SDK version check.' `
            'Install or select the repository-pinned .NET SDK 8.0.416.'
    }

    if ($SkipExternalTools) {
        Add-AvidScriptCompatibilityCheck $Checks 'host.visual_studio' 'host' 'not_run' 'ASCD1006' `
            'Visual Studio 2022 with MSBuild' '' 'Visual Studio discovery was skipped.' `
            'Run again without -SkipExternalTools.'
    }
    else {
        $VsWhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) `
            'Microsoft Visual Studio/Installer/vswhere.exe'
        $VsVersion = ''
        if (Test-Path -LiteralPath $VsWhere -PathType Leaf) {
            $Vs = Invoke-AvidScriptCompatibilityProcess $VsWhere @(
                '-latest', '-products', '*', '-requires', 'Microsoft.Component.MSBuild',
                '-property', 'installationVersion')
            if ($Vs.exit_code -eq 0) {
                $VsVersion = $Vs.stdout.Trim()
            }
        }
        Add-AvidScriptCompatibilityCheck $Checks 'host.visual_studio' 'host' `
            $(if ($VsVersion -match '^17\.') { 'passed' } else { 'warning' }) 'ASCD1006' `
            'Visual Studio 2022 with MSBuild' $VsVersion 'Visual Studio toolchain discovery.' `
            'Install Visual Studio 2022 C++ tools if UBT cannot locate a compiler.'
    }

    $PluginRootReady = Test-AvidScriptCompatibilityOrdinaryRoot $PluginRoot
    Add-AvidScriptCompatibilityCheck $Checks 'plugin.root' 'plugin' `
        $(if ($PluginRootReady) { 'passed' } else { 'blocked' }) 'ASCD2001' `
        'existing ordinary directory' $(if ($PluginRootReady) { 'ordinary' } else { 'missing_or_reparse' }) `
        'Plugin root boundary check.' 'Use a local non-reparse AvidScript plugin directory.'

    $DescriptorPath = Join-Path $PluginRoot 'AvidScript.uplugin'
    $Descriptor = Get-AvidScriptCompatibilityJson $DescriptorPath
    $RequiredModules = @(
        'AvidScriptBindings', 'AvidScriptCore', 'AvidScriptEditor',
        'AvidScriptGenerated', 'AvidScriptRuntime', 'AvidScriptVM')
    $ActualModules = if ($null -ne $Descriptor) {
        @($Descriptor.Modules | ForEach-Object { [string]$_.Name } | Sort-Object -CaseSensitive)
    }
    else { @() }
    $DescriptorReady = $null -ne $Descriptor -and
        [string]$Descriptor.VersionName -match '^[0-9]+\.[0-9]+\.[0-9]+$' -and
        [string]::Join('|', $ActualModules) -ceq [string]::Join('|', $RequiredModules)
    if ($null -ne $Descriptor) {
        $Identity.plugin_version = [string]$Descriptor.VersionName
    }
    Add-AvidScriptCompatibilityCheck $Checks 'plugin.descriptor' 'plugin' `
        $(if ($DescriptorReady) { 'passed' } else { 'blocked' }) 'ASCD2002' `
        ([string]::Join(',', $RequiredModules)) ([string]::Join(',', $ActualModules)) `
        'Plugin descriptor and module identity check.' 'Restore the matching AvidScript.uplugin and modules.'

    $GlobalJson = Get-AvidScriptCompatibilityJson (Join-Path $PluginRoot 'global.json')
    $GlobalSdk = if ($null -ne $GlobalJson) { [string]$GlobalJson.sdk.version } else { '' }
    Add-AvidScriptCompatibilityCheck $Checks 'plugin.dotnet.lock' 'plugin' `
        $(if ($GlobalSdk -ceq '8.0.416') { 'passed' } else { 'blocked' }) 'ASCD2003' `
        '8.0.416' $GlobalSdk 'Repository .NET SDK lock check.' 'Restore global.json from the release package.'

    $ReceiptPath = Join-Path $PluginRoot '.avidscript-install.json'
    if (Test-Path -LiteralPath $ReceiptPath -PathType Leaf) {
        try {
            $Receipt = Read-AvidScriptPluginInstallReceipt $PluginRoot
            Assert-AvidScriptInstalledPluginInventory `
                -PluginRoot $PluginRoot `
                -ExpectedFiles @($Receipt.files) `
                -ExpectedInventorySha256 ([string]$Receipt.inventory_sha256)
            $Identity.install_release_id = [string]$Receipt.release_id
            Add-AvidScriptCompatibilityCheck $Checks 'plugin.install_receipt' 'plugin' 'passed' 'ASCD2004' `
                'valid managed install receipt' $Identity.install_release_id 'Managed plugin install receipt check.' ''
        }
        catch {
            Add-AvidScriptCompatibilityCheck $Checks 'plugin.install_receipt' 'plugin' 'blocked' 'ASCD2004' `
                'valid managed install receipt' 'invalid' 'Managed plugin install receipt is invalid.' `
                'Repair the plugin from its original release package.'
        }
    }
    else {
        Add-AvidScriptCompatibilityCheck $Checks 'plugin.install_receipt' 'plugin' 'warning' 'ASCD2004' `
            'managed receipt or source checkout' 'source_checkout' `
            'No install receipt is present; the plugin is treated as a source checkout.' `
            'Use the release installer when validating a distributed package.'
    }

    $WamrLibrary = Join-Path $PluginRoot 'Source/ThirdParty/WAMR/lib/Win64/Release/libiwasm.lib'
    Add-AvidScriptCompatibilityCheck $Checks 'plugin.wamr.library' 'plugin' `
        $(if (Test-Path -LiteralPath $WamrLibrary -PathType Leaf) { 'passed' } else { 'warning' }) 'ASCD2005' `
        'tracked Win64 WAMR import library' $(if (Test-Path -LiteralPath $WamrLibrary -PathType Leaf) { 'present' } else { 'missing' }) `
        'Fallback WAMR library presence check.' 'Build the tracked WAMR dependency when the fallback backend is required.'

    if ($SkipExternalTools) {
        Add-AvidScriptCompatibilityCheck $Checks 'plugin.wasmtime.performance' 'plugin' 'not_run' 'ASCD2006' `
            'verified managed performance toolchain' '' 'Wasmtime toolchain verification was skipped.' `
            'Run again without -SkipExternalTools.'
    }
    else {
        $PowerShellPath = (Get-Process -Id $PID).Path
        $WasmtimeScript = Join-Path $PluginRoot 'Build/BuildAvidScriptWasmtimePerformanceToolchain.ps1'
        $Wasmtime = Invoke-AvidScriptCompatibilityProcess $PowerShellPath @(
            '-NoProfile', '-File', $WasmtimeScript, '-Mode', 'Verify',
            '-RepositoryRoot', $PluginRoot)
        $WasmtimeJson = if ($Wasmtime.exit_code -eq 0 -and -not [string]::IsNullOrWhiteSpace($Wasmtime.stdout)) {
            try { $Wasmtime.stdout | ConvertFrom-Json -Depth 16 -DateKind String } catch { $null }
        }
        else { $null }
        $WasmtimeReady = $null -ne $WasmtimeJson -and
            [string]$WasmtimeJson.result -ceq 'wasmtime_performance_toolchain_verified'
        if ($WasmtimeReady) {
            $Identity.wasmtime_toolchain_id = [string]$WasmtimeJson.evidence.compiler_profile
        }
        Add-AvidScriptCompatibilityCheck $Checks 'plugin.wasmtime.performance' 'plugin' `
            $(if ($WasmtimeReady) { 'passed' } else { 'blocked' }) 'ASCD2006' `
            'verified managed performance toolchain' $Identity.wasmtime_toolchain_id `
            'Win64 Wasmtime performance toolchain check.' `
            'Run Build/BuildAvidScriptWasmtimePerformanceToolchain.ps1 -Mode Verify or Build.'
    }

    $GeneratedManifestPath = Join-Path $PluginRoot 'Source/AvidScriptGenerated/AvidScriptGeneratedManifest.json'
    $GeneratedPackagePath = Join-Path $PluginRoot 'Source/AvidScriptGenerated/AvidScriptGeneratedPackage.json'
    $GeneratedManifest = Get-AvidScriptCompatibilityJson $GeneratedManifestPath
    $GeneratedPackage = Get-AvidScriptCompatibilityJson $GeneratedPackagePath
    $GeneratedReady = $null -ne $GeneratedManifest -and $null -ne $GeneratedPackage -and
        [int]$GeneratedManifest.schema_version -eq 6 -and
        [int]$GeneratedPackage.schema_version -eq 1 -and
        [string]$GeneratedPackage.type_manifest.file -ceq 'AvidScriptGeneratedManifest.json' -and
        (Get-AvidScriptPluginReleaseSha256 $GeneratedManifestPath) -ceq
        [string]$GeneratedPackage.type_manifest.sha256
    if ($null -ne $GeneratedPackage) {
        $Identity.generated_type_package_id = [string]$GeneratedPackage.package_id
    }
    Add-AvidScriptCompatibilityCheck $Checks 'plugin.generated_types' 'plugin' `
        $(if ($GeneratedReady) { 'passed' } else { 'warning' }) 'ASCD2007' `
        'schema 6 manifest with matching package hash' $Identity.generated_type_package_id `
        'Generated Type source package identity check.' `
        'Regenerate project C# script types before using generated Actor or Subsystem types.'

    $CookCurrentPath = Join-Path $PluginRoot 'Content/AvidScriptGenerated/current.json'
    $CookCurrent = Get-AvidScriptCompatibilityJson $CookCurrentPath
    $CookReady = $false
    if ($null -ne $CookCurrent -and [int]$CookCurrent.schema_version -eq 2) {
        try {
            $CookRelative = Normalize-AvidScriptPluginReleaseRelativePath `
                ([string]$CookCurrent.type_manifest.file) `
                'generated type cooked manifest'
            $CookManifestPath = Join-Path (Split-Path -Parent $CookCurrentPath) $CookRelative
            $CookReady = Test-Path -LiteralPath $CookManifestPath -PathType Leaf
            if ($CookReady) {
                $CookReady = (Get-AvidScriptPluginReleaseSha256 $CookManifestPath) -ceq
                    [string]$CookCurrent.type_manifest.sha256
            }
        }
        catch {
            $CookReady = $false
        }
    }
    Add-AvidScriptCompatibilityCheck $Checks 'plugin.generated_types.cooked' 'plugin' `
        $(if ($CookReady) { 'passed' } else { 'warning' }) 'ASCD2008' `
        'schema 2 current.json with matching type manifest' $(if ($CookReady) { 'verified' } else { 'missing_or_invalid' }) `
        'Cooked Generated Type package identity check.' 'Publish Generated Types before packaging a game.'

    $ProjectRootReady = Test-AvidScriptCompatibilityOrdinaryRoot $ProjectRoot
    Add-AvidScriptCompatibilityCheck $Checks 'project.root' 'project' `
        $(if ($ProjectRootReady) { 'passed' } else { 'blocked' }) 'ASCD3001' `
        'existing ordinary directory' $(if ($ProjectRootReady) { 'ordinary' } else { 'missing_or_reparse' }) `
        'Project root boundary check.' 'Use a local non-reparse Unreal project directory.'

    $ProjectFiles = if ($ProjectRootReady) {
        @(Get-ChildItem -LiteralPath $ProjectRoot -Filter '*.uproject' -File -ErrorAction SilentlyContinue)
    }
    else { @() }
    Add-AvidScriptCompatibilityCheck $Checks 'project.descriptor' 'project' `
        $(if ($ProjectFiles.Count -eq 1) { 'passed' } else { 'blocked' }) 'ASCD3002' `
        'exactly one .uproject' ([string]$ProjectFiles.Count) 'Unreal project descriptor check.' `
        'Pass a project root containing exactly one .uproject file.'

    $ExpectedPluginRoot = Join-Path $ProjectRoot 'Plugins/AvidScript'
    $PluginPlacementReady = $ExpectedPluginRoot.Equals(
        $PluginRoot,
        [System.StringComparison]::OrdinalIgnoreCase)
    Add-AvidScriptCompatibilityCheck $Checks 'project.plugin_placement' 'project' `
        $(if ($PluginPlacementReady) { 'passed' } else { 'blocked' }) 'ASCD3003' `
        '<Project>/Plugins/AvidScript' $(if ($PluginPlacementReady) { 'matched' } else { 'different_path' }) `
        'Project plugin placement check.' 'Run the doctor from the project-installed AvidScript plugin.'

    $ProjectPluginsRoot = Join-Path $ProjectRoot 'Plugins'
    $PluginsBoundaryReady = -not (Test-Path -LiteralPath $ProjectPluginsRoot) -or
        (Test-AvidScriptCompatibilityOrdinaryRoot $ProjectPluginsRoot)
    $SameVolume = [System.IO.Path]::GetPathRoot($ProjectRoot).Equals(
        [System.IO.Path]::GetPathRoot($ExpectedPluginRoot),
        [System.StringComparison]::OrdinalIgnoreCase)
    Add-AvidScriptCompatibilityCheck $Checks 'project.staging_boundary' 'project' `
        $(if ($PluginsBoundaryReady -and $SameVolume) { 'passed' } else { 'blocked' }) 'ASCD3006' `
        'ordinary same-volume Plugins staging boundary' `
        "plugins=$(if (Test-Path -LiteralPath $ProjectPluginsRoot) { 'present' } else { 'creatable' }),same_volume=$SameVolume" `
        'Installer staging boundary check without mutating the project.' `
        'Remove reparse points or move the project to a local volume before installation.'

    $TargetRoot = Join-Path $ProjectRoot 'Source'
    $Targets = if (Test-Path -LiteralPath $TargetRoot -PathType Container) {
        @(Get-ChildItem -LiteralPath $TargetRoot -Filter '*Target.cs' -File -ErrorAction SilentlyContinue)
    }
    else { @() }
    $EditorTargets = @($Targets | Where-Object { $_.BaseName.EndsWith('Editor.Target') })
    $GameTargets = @($Targets | Where-Object { -not $_.BaseName.EndsWith('Editor.Target') })
    Add-AvidScriptCompatibilityCheck $Checks 'project.targets' 'project' `
        $(if ($EditorTargets.Count -gt 0 -and $GameTargets.Count -gt 0) { 'passed' } else { 'warning' }) 'ASCD3004' `
        'at least one Game and one Editor Target' "game=$($GameTargets.Count),editor=$($EditorTargets.Count)" `
        'Unreal Target discovery.' 'Add conventional Game and Editor Target files when building from source.'

    $BindingRoot = Join-Path $ProjectRoot 'Saved/AvidScriptGeneratedBindings'
    $BindingCandidates = if (Test-Path -LiteralPath $BindingRoot -PathType Container) {
        @(Get-ChildItem -LiteralPath $BindingRoot -Filter 'package.json' -File -Recurse -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 64)
    }
    else { @() }
    $ResolvedBinding = $null
    foreach ($Candidate in $BindingCandidates) {
        try {
            $ResolvedBinding = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $Candidate.FullName
            break
        }
        catch {
            continue
        }
    }
    if ($null -ne $ResolvedBinding) {
        $Identity.binding_package_sha256 = [string]$ResolvedBinding.ManifestSha256
    }
    Add-AvidScriptCompatibilityCheck $Checks 'project.binding_package' 'project' `
        $(if ($null -ne $ResolvedBinding) { 'passed' } else { 'warning' }) 'ASCD3005' `
        'at least one verified generated binding package' $Identity.binding_package_sha256 `
        'Generated C# binding package discovery.' 'Generate project bindings before compiling gameplay scripts.'

    if ($SkipExternalTools) {
        Add-AvidScriptCompatibilityCheck $Checks 'platform.android.toolchain' 'platform' 'not_run' 'ASCD4001' `
            'UE5.8 pinned Android toolchain' '' 'Android toolchain preflight was skipped.' `
            'Run again without -SkipExternalTools when Android evidence is required.'
    }
    else {
        $PowerShellPath = (Get-Process -Id $PID).Path
        $AndroidScript = Join-Path $PluginRoot 'Build/TestAvidScriptAndroidToolchain.ps1'
        $Android = Invoke-AvidScriptCompatibilityProcess $PowerShellPath @(
            '-NoProfile', '-File', $AndroidScript, '-EngineRoot', $EngineRoot)
        $AndroidJson = if (-not [string]::IsNullOrWhiteSpace($Android.stdout)) {
            try { $Android.stdout | ConvertFrom-Json -Depth 32 -DateKind String } catch { $null }
        }
        else { $null }
        $AndroidReady = $null -ne $AndroidJson -and [bool]$AndroidJson.ready
        $AndroidActual = if ($null -ne $AndroidJson) {
            "passed=$(@($AndroidJson.checks | Where-Object status -eq 'ok').Count)/$(@($AndroidJson.checks).Count)"
        }
        else { 'preflight_unreadable' }
        Add-AvidScriptCompatibilityCheck $Checks 'platform.android.toolchain' 'platform' `
            $(if ($AndroidReady) { 'passed' } else { 'warning' }) 'ASCD4001' `
            'UE5.8 pinned Android toolchain' $AndroidActual 'Android toolchain read-only preflight.' `
            'Install the exact SDK/NDK/JDK packages reported by TestAvidScriptAndroidToolchain.ps1.'
        if ($null -ne $AndroidJson) {
            foreach ($AndroidCheck in @($AndroidJson.checks)) {
                $Expected = $AndroidCheck.expected | ConvertTo-Json -Depth 8 -Compress
                $Actual = $AndroidCheck.actual | ConvertTo-Json -Depth 8 -Compress
                Add-AvidScriptCompatibilityCheck $Checks `
                    "platform.android.$([string]$AndroidCheck.id)" `
                    'platform' `
                    $(if ([string]$AndroidCheck.status -ceq 'ok') { 'passed' } else { 'warning' }) `
                    'ASCD4002' `
                    $Expected `
                    $Actual `
                    "Android preflight detail: $([string]$AndroidCheck.id)." `
                    'Install or repair the exact Android component reported by the preflight.'
            }
        }
    }

    $StatusCounts = [ordered]@{}
    foreach ($Status in @('passed', 'warning', 'blocked', 'not_run')) {
        $StatusCounts[$Status] = @($Checks | Where-Object { $_.status -ceq $Status }).Count
    }
    $Result = if ($StatusCounts.blocked -gt 0) {
        'blocked'
    }
    elseif ($StatusCounts.warning -gt 0 -or $StatusCounts.not_run -gt 0) {
        'degraded'
    }
    else { 'ready' }
    return [pscustomobject][ordered]@{
        schema_version = 1
        format = 'avidscript.compatibility.report'
        result = $Result
        generated_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
        context = [pscustomobject][ordered]@{
            plugin_root = $PluginRoot
            project_root = $ProjectRoot
            engine_root = $EngineRoot
        }
        identity = [pscustomobject]$Identity
        summary = [pscustomobject][ordered]@{
            passed = $StatusCounts.passed
            warning = $StatusCounts.warning
            blocked = $StatusCounts.blocked
            not_run = $StatusCounts.not_run
            total = $Checks.Count
        }
        checks = @($Checks)
    }
}

function Assert-AvidScriptCompatibilityDoctorReport {
    param([Parameter(Mandatory = $true)]$Report)

    $Ids = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Check in @($Report.checks)) {
        if (-not $Ids.Add([string]$Check.id)) {
            Throw-AvidScriptPluginReleaseError 'ASCD9001' 'schema_invalid' "duplicate compatibility check id: $($Check.id)"
        }
    }
    $Total = [int]$Report.summary.passed + [int]$Report.summary.warning +
        [int]$Report.summary.blocked + [int]$Report.summary.not_run
    if ($Total -ne [int]$Report.summary.total -or $Total -ne @($Report.checks).Count) {
        Throw-AvidScriptPluginReleaseError 'ASCD9002' 'schema_invalid' 'compatibility summary does not match checks.'
    }
    $ExpectedResult = if ([int]$Report.summary.blocked -gt 0) {
        'blocked'
    }
    elseif ([int]$Report.summary.warning -gt 0 -or [int]$Report.summary.not_run -gt 0) {
        'degraded'
    }
    else { 'ready' }
    if ([string]$Report.result -cne $ExpectedResult) {
        Throw-AvidScriptPluginReleaseError 'ASCD9003' 'schema_invalid' 'compatibility result does not match summary.'
    }
}
