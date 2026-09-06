[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Win64Build', 'AndroidBuild', 'AndroidScenario')]
    [string]$Step,
    [Parameter(Mandatory = $true)][string]$InputPath
)

$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Input = Get-Content -LiteralPath $InputPath -Raw |
    ConvertFrom-Json -Depth 100 -DateKind String

switch ($Step) {
    'Win64Build' {
        $Parameters = @{
            SourcePath = [string]$Input.source_path
            CSharpProjectPath = [string]$Input.csharp_project_path
            ModuleId = [string]$Input.module_id
            ArtifactStem = [string]$Input.artifact_stem
            OutputRoot = [string]$Input.output_root
            DotNetPath = [string]$Input.dotnet_path
            Configuration = [string]$Input.configuration
            ArchiveRoot = [string]$Input.archive_root
            PackagedOracleMode = [string]$Input.packaged_oracle_mode
            CookMaps = @($Input.cook_maps)
            EnablePlugins = @($Input.enable_plugins)
            DisablePlugins = @($Input.disable_plugins)
            EngineRoot = [string]$Input.engine_root
        }
        foreach ($OptionalPath in @(
                @{ Name = 'BindingPackagePath'; Value = [string]$Input.binding_package_path },
                @{ Name = 'RuntimeBindingPackagePath'; Value = [string]$Input.runtime_binding_package_path },
                @{ Name = 'GeneratedTypeManifestPath'; Value = [string]$Input.generated_type_manifest_path })) {
            if (-not [string]::IsNullOrWhiteSpace($OptionalPath.Value)) {
                $Parameters[$OptionalPath.Name] = $OptionalPath.Value
            }
        }
        & (Join-Path $BuildRoot 'InvokeAvidScriptBuildCookRun.ps1') @Parameters
    }
    'AndroidBuild' {
        $Parameters = @{
            Mode = 'BuildCookRun'
            Configuration = [string]$Input.configuration
            ArchiveRoot = [string]$Input.archive_root
            EngineRoot = [string]$Input.engine_root
            AndroidSdkRoot = [string]$Input.sdk_root
            NdkRoot = [string]$Input.ndk_root
            JavaHome = [string]$Input.java_home
            TimeoutSeconds = [int]$Input.timeout_seconds
        }
        & (Join-Path $BuildRoot 'InvokeAvidScriptAndroidBuildCookRun.ps1') @Parameters
    }
    'AndroidScenario' {
        $Parameters = @{
            Mode = [string]$Input.mode
            Configuration = [string]$Input.configuration
            AdbPath = [string]$Input.adb_path
            DeviceId = [string]$Input.device_id
            ApkPath = [string]$Input.apk_path
            ObbPaths = @($Input.obb_paths)
            ScenarioId = [string]$Input.scenario_id
            ModuleId = [string]$Input.module_id
            Map = [string]$Input.map
            EventIds = [string]$Input.event_ids
            OutputRoot = [string]$Input.output_root
            TimeoutSeconds = [int]$Input.timeout_seconds
            ExpectedPackageId = [string]$Input.expected_package_id
        }
        & (Join-Path $BuildRoot 'InvokeAvidScriptAndroidScenario.ps1') @Parameters
    }
}
