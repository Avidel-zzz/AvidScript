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
$guardName = 'Test-AvidScriptWasmtimeStaticLinkageViolation'
$parseErrors = $null
$tokens = $null
$checkerAst = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $pluginRoot 'Build/CheckAvidScriptArchitecture.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Architecture checker parse failed.' }
$guard = $checkerAst.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $guardName
}, $false)
if ($null -eq $guard) { throw 'Static linkage guard missing.' }
. ([scriptblock]::Create($guard.Extent.Text))
$rulesSource = [IO.File]::ReadAllText($buildRules)
$guardCases = @(
    @{ Name = 'notice_hashing_allowed'; Source = $rulesSource; Rejected = $false }
    @{ Name = 'constructor_static_link_rejected'; Source = $rulesSource.Replace('PublicAdditionalLibraries.Add(ImportLibraryPath);', 'PublicAdditionalLibraries.Add("wasmtime.lib");'); Rejected = $true }
    @{ Name = 'notice_verifier_link_access_rejected'; Source = $rulesSource.Replace('string Root = Path.Combine(InstallRoot, "notices");', 'PublicAdditionalLibraries.Add("wasmtime.lib"); string Root = Path.Combine(InstallRoot, "notices");'); Rejected = $true }
    @{ Name = 'other_helper_static_link_rejected'; Source = $rulesSource + ' private void Link() { PublicAdditionalLibraries.Add("wasmtime.lib"); }'; Rejected = $true }
    @{ Name = 'legacy_static_link_rejected'; Source = 'PublicAdditionalLibraries.Add("wasmtime.lib");'; Rejected = $true }
    @{ Name = 'unrecognized_verifier_bounds_rejected'; Source = $rulesSource.Replace('private static void AssertUniqueNoticeJsonProperties(', 'private static void ChangedVerifierBoundary('); Rejected = $true }
)
foreach ($case in $guardCases) {
    if ((Test-AvidScriptWasmtimeStaticLinkageViolation $case.Source) -ne $case.Rejected) {
        throw "Static linkage guard contract failed: $($case.Name)"
    }
    Write-Host "PASS $($case.Name)"
}
Write-Host "Wasmtime static linkage guard contracts: $($guardCases.Count)/$($guardCases.Count)"
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
