[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageDescriptorPath,
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [ValidateSet('Development', 'Shipping')][string]$Configuration = 'Development',
    [ValidateSet('Win64', 'Android')][string]$TargetPlatform = 'Win64'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AvidScriptGeneratedTypeCookPackage.ps1')

try {
    $Package = Publish-AvidScriptGeneratedTypeCookPackage `
        -PackageDescriptorPath $PackageDescriptorPath `
        -ProjectRoot $ProjectRoot `
        -OutputRoot $OutputRoot `
        -Configuration $Configuration `
        -TargetPlatform $TargetPlatform
    $Result = [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_generated_type_cook_package_published'
        status = 'ok'
        module_id = [string]$Package.ModuleId
        package_id = [string]$Package.PackageId
        configuration = [string]$Package.Configuration
        platform = [string]$Package.Platform
        generated_type_package_id = [string]$Package.GeneratedTypePackageId
        output_root = [System.IO.Path]::GetFullPath($OutputRoot)
        descriptor_path = [string]$Package.DescriptorPath
    }
    [Console]::Out.WriteLine(($Result | ConvertTo-Json -Depth 8 -Compress))
    exit 0
}
catch {
    $Failure = [pscustomobject][ordered]@{
        schema_version = 1
        result = 'avidscript_generated_type_cook_package_failed'
        status = 'error'
        message = $_.Exception.Message
    }
    [Console]::Out.WriteLine(($Failure | ConvertTo-Json -Depth 4 -Compress))
    exit 1
}
