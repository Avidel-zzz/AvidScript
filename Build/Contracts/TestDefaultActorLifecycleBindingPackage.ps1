[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BindingPackagePath,
    [string]$DotNetPath = "$env:USERPROFILE/.dotnet/dotnet.exe"
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$runRoot = Join-Path $projectRoot ('Saved/AvidScript/DefaultBindingContracts/' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($runRoot)
$package = Get-Content -LiteralPath $BindingPackagePath -Raw | ConvertFrom-Json
$cases = @(
    @{ Name = 'legacy_default_without_package'; Arguments = @(); Success = $true; HasPackage = $false }
    @{ Name = 'explicit_default_binding_preserved'; Arguments = @('-BindingPackagePath', $BindingPackagePath); Success = $true; HasPackage = $true }
    @{ Name = 'explicit_default_binding_cache_reuse'; Arguments = @('-BindingPackagePath', $BindingPackagePath); Success = $true; HasPackage = $true }
    @{ Name = 'invalid_default_binding_rejected'; Arguments = @('-BindingPackagePath', (Join-Path $runRoot 'missing.json')); Success = $false; Category = 'binding_package_invalid' }
    @{ Name = 'runtime_without_authorization_rejected'; Arguments = @('-RuntimeBindingPackagePath', $BindingPackagePath); Success = $false; Category = 'phase42_binding_required' }
)
$results = foreach ($case in $cases) {
    $outputRoot = Join-Path $runRoot $case.Name
    $arguments = @('-NoProfile', '-File', (Join-Path $pluginRoot 'Build/BuildCSharpActorLifecycle.ps1'),
        '-DotNetPath', $DotNetPath, '-OutputRoot', $outputRoot, '-ProjectRoot', $projectRoot,
        '-CompilerWorkerMode', 'disabled', '-SemanticCacheRoot', (Join-Path $runRoot 'semantic-cache'),
        '-CompilationCacheRoot', (Join-Path $runRoot 'compilation-cache')) + $case.Arguments
    & (Join-Path $PSHOME 'pwsh.exe') @arguments *> (Join-Path $runRoot ($case.Name + '.log'))
    $exitCode = $LASTEXITCODE
    $report = Get-Content -LiteralPath (Join-Path $outputRoot 'actor_lifecycle.csharp.report.json') -Raw | ConvertFrom-Json
    if (($exitCode -eq 0) -ne $case.Success) { throw "Unexpected build outcome: $($case.Name); see $runRoot" }
    if ($case.Success) {
        if ($report.result -cne 'direct_abi_built' -or -not $report.succeeded) { throw "Build report did not succeed: $($case.Name)" }
        $manifest = Get-Content -LiteralPath (Join-Path $outputRoot 'actor_lifecycle.avidscript.json') -Raw | ConvertFrom-Json
        $hasPackage = $null -ne $manifest.PSObject.Properties['binding_package']
        if ($hasPackage -ne $case.HasPackage) { throw "Binding package presence differs: $($case.Name)" }
        if ($hasPackage -and $manifest.binding_package.package_hash -cne $package.package_hash) {
            throw 'Explicit binding identity was not preserved.'
        }
        if ($case.Name -ceq 'explicit_default_binding_cache_reuse' -and
            ($report.semantic_cache.lookup -cne 'hit' -or $report.compilation_cache.lookup -cne 'hit')) {
            throw 'Explicit binding build did not reuse both verified caches.'
        }
    } elseif ($report.result -cne $case.Category) {
        throw "Wrong rejection category: $($case.Name): $($report.result)"
    }
    Write-Host "PASS $($case.Name)"
    [ordered]@{ name = $case.Name; passed = $true; exit_code = $exitCode }
}
[ordered]@{ passed = @($results).Count; total = $cases.Count; results = @($results) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json')
Write-Host "Default ActorLifecycle binding contracts: $(@($results).Count)/$($cases.Count)"
Write-Host "Evidence: $runRoot"
