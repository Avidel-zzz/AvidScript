[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BindingPackageManifestPath,
    [string]$DotNetPath = "$env:USERPROFILE/.dotnet/dotnet.exe"
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$source = Join-Path $pluginRoot 'Fixtures/Phase66/BoundedGeneratedType.cs'
$project = Join-Path $pluginRoot 'Fixtures/Phase66/BoundedGeneratedType.csproj'
$build = Join-Path $pluginRoot 'Build/BuildCSharpScriptTypes.ps1'
$runner = Join-Path $pluginRoot 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs'
$package = (Resolve-Path -LiteralPath $BindingPackageManifestPath -ErrorAction Stop).Path
$runRoot = Join-Path $projectRoot ('Saved/AvidScript/BoundedGeneratedTypeContracts/' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($runRoot)

function Invoke-Case {
    param([string]$Name, [string]$Source, [bool]$Bounded, [string[]]$ExtraArguments = @())
    $caseRoot = Join-Path $runRoot $Name
    $native = Join-Path $caseRoot 'Native'
    $arguments = @('-NoProfile', '-File', $build,
        '-DotNetPath', $DotNetPath,
        '-SourcePath', $Source,
        '-SourceId', 'Fixtures/Phase66/BoundedGeneratedType.cs',
        '-ProjectPath', $project,
        '-BindingPackageManifestPath', $package,
        '-OutputRoot', $native,
        '-ArtifactRoot', (Join-Path $caseRoot 'Artifacts'),
        '-CookOutputRoot', (Join-Path $caseRoot 'Cook'),
        '-RuntimeModuleId', 'bounded_generated_type_contract')
    if ($Bounded) { $arguments += @('-LanguageErrors', 'bounded') }
    $arguments += $ExtraArguments
    & (Join-Path $PSHOME 'pwsh.exe') @arguments *> (Join-Path $runRoot "$Name.log")
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Native = $native
        Descriptor = Join-Path $native 'AvidScriptGeneratedPackage.json'
        TypeManifest = Join-Path $native 'AvidScriptGeneratedManifest.json'
    }
}

$positive = Invoke-Case -Name 'bounded' -Source $source -Bounded $true
if ($positive.ExitCode -ne 0 -or
    -not (Test-Path -LiteralPath $positive.Descriptor -PathType Leaf)) {
    throw "Bounded generated type build failed: $runRoot"
}
$descriptor = Get-Content -LiteralPath $positive.Descriptor -Raw | ConvertFrom-Json
$types = Get-Content -LiteralPath $positive.TypeManifest -Raw | ConvertFrom-Json
$runtimePath = [IO.Path]::GetFullPath((Join-Path $positive.Native ([string]$descriptor.runtime_manifest.file)))
$runtime = Get-Content -LiteralPath $runtimePath -Raw | ConvertFrom-Json
$wasmPath = Join-Path (Split-Path -Parent $runtimePath) 'generated_types.wasm'
$semanticPath = Join-Path (Split-Path -Parent (Split-Path -Parent $runtimePath)) 'script-types.semantic.json'
if ($descriptor.runtime_module_id -cne 'bounded_generated_type_contract' -or
    $runtime.module_id -cne $descriptor.runtime_module_id -or
    $types.semantic_schema_version -ne 34 -or
    $types.semantic_version -cne '1.43' -or
    (Get-FileHash -LiteralPath $semanticPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne
        [string]$types.semantic_artifact_sha256 -or
    @($types.types).Count -ne 1 -or
    @($types.types[0].functions).Count -ne 2 -or
    -not (Test-Path -LiteralPath $wasmPath -PathType Leaf)) {
    throw "Generated type shell and Runtime identities diverged: $runRoot"
}
& node $runner $wasmPath --bounded-generated-type $positive.TypeManifest
if ($LASTEXITCODE -ne 0) { throw "Generated UFunction WASM execution failed: $runRoot" }
Write-Host 'PASS bounded generated type shell, Runtime package and UFunction WASM'

$default = Invoke-Case -Name 'default_rejects' -Source $source -Bounded $false
if ($default.ExitCode -eq 0 -or (Test-Path -LiteralPath $default.Descriptor)) {
    throw "Default generated type build accepted exception syntax: $runRoot"
}
Write-Host 'PASS default rejects exception syntax'

$unsafeSource = Join-Path $runRoot 'UnsupportedExceptionConstructor.cs'
$sourceText = [IO.File]::ReadAllText($source)
$unsafeText = $sourceText.Replace('throw new Exception();', 'throw new Exception("unsupported");')
if ($unsafeText -ceq $sourceText) { throw 'Fixture no longer contains the expected exception constructor.' }
[IO.File]::WriteAllText($unsafeSource, $unsafeText, [Text.UTF8Encoding]::new($false))
$unsafe = Invoke-Case -Name 'bounded_rejects' -Source $unsafeSource -Bounded $true
if ($unsafe.ExitCode -eq 0 -or (Test-Path -LiteralPath $unsafe.Descriptor)) {
    throw "Bounded generated type build accepted an unsupported constructor: $runRoot"
}
Write-Host 'PASS bounded rejects unsupported exception constructor'

foreach ($case in @(
        @{ Name = 'skip_runtime_rejects'; Arguments = @('-SkipRuntimePackage') },
        @{ Name = 'headless_rejects'; Arguments = @('-HeadlessRelease') })) {
    $rejected = Invoke-Case -Name $case.Name -Source $source -Bounded $true `
        -ExtraArguments $case.Arguments
    if ($rejected.ExitCode -eq 0 -or (Test-Path -LiteralPath $rejected.Descriptor)) {
        throw "Bounded generated type build accepted $($case.Name): $runRoot"
    }
    Write-Host "PASS $($case.Name)"
}
Write-Host "Bounded generated type build contracts: 5/5 passed; evidence=$runRoot"
