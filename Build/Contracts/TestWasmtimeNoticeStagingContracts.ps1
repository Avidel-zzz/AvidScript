[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputRoot,
    [string]$DotNetPath = "$env:USERPROFILE/.dotnet/dotnet.exe",
    [string]$RealWasmtimeRoot = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$testRoot = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('notice-staging-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($testRoot)
$buildRules = Join-Path $pluginRoot 'Source/ThirdParty/Wasmtime/Wasmtime.Build.cs'
$contract = Join-Path $PSScriptRoot 'WasmtimeNoticeStagingContract.cs'
$project = @"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework><EnableDefaultCompileItems>false</EnableDefaultCompileItems><ImplicitUsings>disable</ImplicitUsings><Nullable>disable</Nullable></PropertyGroup>
  <ItemGroup>
    <Compile Include="$([Security.SecurityElement]::Escape($buildRules))" />
    <Compile Include="$([Security.SecurityElement]::Escape($contract))" />
  </ItemGroup>
</Project>
"@
$projectPath = Join-Path $testRoot 'NoticeStaging.csproj'
[IO.File]::WriteAllText($projectPath, $project, [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $pluginRoot 'global.json') -Destination (Join-Path $testRoot 'global.json')
$previousCliHome = $env:DOTNET_CLI_HOME
try {
    $env:DOTNET_CLI_HOME = Join-Path $testRoot 'cli-home'
    $version = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $version.Trim() -cne '8.0.416') { throw 'Notice staging contracts require SDK 8.0.416.' }
    $arguments = @('run','--project',$projectPath,'--configuration','Release','--',$testRoot)
    if (-not [string]::IsNullOrWhiteSpace($RealWasmtimeRoot)) { $arguments += [IO.Path]::GetFullPath($RealWasmtimeRoot) }
    & $DotNetPath @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Wasmtime notice staging contracts failed.' }
} finally {
    $env:DOTNET_CLI_HOME = $previousCliHome
}
