[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Win64Build', 'AndroidBuild', 'AndroidScenario')]
    [string]$Step,
    [Parameter(Mandatory = $true)][string]$InputPath
)

$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$StepInput = Get-Content -LiteralPath $InputPath -Raw |
    ConvertFrom-Json -Depth 100 -DateKind String

switch ($Step) {
    'Win64Build' {
        $Parameters = @{
            SourcePath = [string]$StepInput.source_path
            CSharpProjectPath = [string]$StepInput.csharp_project_path
            ModuleId = [string]$StepInput.module_id
            ArtifactStem = [string]$StepInput.artifact_stem
            OutputRoot = [string]$StepInput.output_root
            DotNetPath = [string]$StepInput.dotnet_path
            Configuration = [string]$StepInput.configuration
            ArchiveRoot = [string]$StepInput.archive_root
            PackagedOracleMode = [string]$StepInput.packaged_oracle_mode
            CookMaps = @($StepInput.cook_maps)
            EnablePlugins = @($StepInput.enable_plugins)
            DisablePlugins = @($StepInput.disable_plugins)
            EngineRoot = [string]$StepInput.engine_root
        }
        foreach ($OptionalPath in @(
                @{ Name = 'BindingPackagePath'; Value = [string]$StepInput.binding_package_path },
                @{ Name = 'RuntimeBindingPackagePath'; Value = [string]$StepInput.runtime_binding_package_path },
                @{ Name = 'GeneratedTypeManifestPath'; Value = [string]$StepInput.generated_type_manifest_path })) {
            if (-not [string]::IsNullOrWhiteSpace($OptionalPath.Value)) {
                $Parameters[$OptionalPath.Name] = $OptionalPath.Value
            }
        }
        & (Join-Path $BuildRoot 'InvokeAvidScriptBuildCookRun.ps1') @Parameters
    }
    'AndroidBuild' {
        $Parameters = @{
            Mode = 'BuildCookRun'
            Configuration = [string]$StepInput.configuration
            ArchiveRoot = [string]$StepInput.archive_root
            EngineRoot = [string]$StepInput.engine_root
            AndroidSdkRoot = [string]$StepInput.sdk_root
            NdkRoot = [string]$StepInput.ndk_root
            JavaHome = [string]$StepInput.java_home
            TimeoutSeconds = [int]$StepInput.timeout_seconds
        }
        & (Join-Path $BuildRoot 'InvokeAvidScriptAndroidBuildCookRun.ps1') @Parameters
    }
    'AndroidScenario' {
        $Parameters = @{
            Mode = [string]$StepInput.mode
            Configuration = [string]$StepInput.configuration
            AdbPath = [string]$StepInput.adb_path
            DeviceId = [string]$StepInput.device_id
            ApkPath = [string]$StepInput.apk_path
            ObbPaths = @($StepInput.obb_paths)
            ScenarioId = [string]$StepInput.scenario_id
            ModuleId = [string]$StepInput.module_id
            Map = [string]$StepInput.map
            EventIds = [string]$StepInput.event_ids
            OutputRoot = [string]$StepInput.output_root
            TimeoutSeconds = [int]$StepInput.timeout_seconds
            ExpectedPackageId = [string]$StepInput.expected_package_id
        }
        & (Join-Path $BuildRoot 'InvokeAvidScriptAndroidScenario.ps1') @Parameters
    }
}
