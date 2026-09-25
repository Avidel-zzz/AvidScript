[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BindingPackagePath,
    [string]$DotNetPath = "$env:USERPROFILE/.dotnet/dotnet.exe"
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$source = Join-Path $pluginRoot 'Fixtures/Phase66/BoundedLanguageErrorsLifecycle.cs'
$project = Join-Path $pluginRoot 'Fixtures/Phase66/BoundedLanguageErrorsLifecycle.csproj'
$build = Join-Path $pluginRoot 'Build/BuildCSharpActorLifecycle.ps1'
$runner = Join-Path $pluginRoot 'Tools/AvidScript.CSharpGuest.Tests/RunThrowProducerWasm.cjs'
$package = (Resolve-Path -LiteralPath $BindingPackagePath -ErrorAction Stop).Path
$runRoot = Join-Path $projectRoot ('Saved/AvidScript/BoundedLanguageErrorBuildContracts/' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($runRoot)
$stem = 'bounded_language_errors'

function Invoke-Case {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][bool]$Bounded,
        [string]$ModuleId = ''
    )
    $output = Join-Path $runRoot $Name
    $arguments = @('-NoProfile', '-File', $build,
        '-DotNetPath', $DotNetPath, '-SourcePath', $Source, '-ProjectPath', $project,
        '-ProjectRoot', $projectRoot, '-OutputRoot', $output,
        '-BindingPackagePath', $package, '-ArtifactStem', $stem,
        '-CompilerWorkerMode', 'disabled')
    if ($Bounded) { $arguments += @('-LanguageErrors', 'bounded') }
    if (-not [string]::IsNullOrWhiteSpace($ModuleId)) {
        $arguments += @('-ModuleId', $ModuleId)
    }
    & (Join-Path $PSHOME 'pwsh.exe') @arguments *> (Join-Path $runRoot "$Name.log")
    $exitCode = $LASTEXITCODE
    $reportFile = Join-Path $output "$stem.csharp.report.json"
    if (-not (Test-Path -LiteralPath $reportFile -PathType Leaf)) {
        throw "$Name produced no build report: $runRoot"
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Report = Get-Content -LiteralPath $reportFile -Raw | ConvertFrom-Json
        Wasm = Join-Path $output "$stem.wasm"
        Manifest = Join-Path $output "$stem.avidscript.json"
        GuestIr = Join-Path $output "$stem.guestir.json"
        DebugMap = Join-Path $output "$stem.csharp.debug.json"
    }
}

$positive = Invoke-Case -Name 'bounded' -Source $source -Bounded $true
if ($positive.ExitCode -ne 0 -or $positive.Report.result -cne 'direct_abi_built' -or
    -not $positive.Report.succeeded -or
    $positive.Report.compilation.language_errors -cne 'bounded' -or
    $positive.Report.semantic.succeeded -or
    $positive.Report.semantic.version -cne '1.43' -or
    $positive.Report.compilation_cache.lookup -cne 'disabled' -or
    $positive.Report.semantic_cache.lookup -cne 'disabled' -or
    $positive.Report.compilation_cache.enabled -or $positive.Report.semantic_cache.enabled -or
    -not (Test-Path -LiteralPath $positive.Wasm -PathType Leaf) -or
    -not (Test-Path -LiteralPath $positive.Manifest -PathType Leaf)) {
    throw "Bounded lifecycle did not publish a complete artifact: $runRoot"
}
$ir = Get-Content -LiteralPath $positive.GuestIr -Raw | ConvertFrom-Json
if ($ir.schema_version -ne 17 -or $ir.ir_version -cne '1.16' -or
    $null -eq $ir.language_error_catalog) {
    throw "Bounded lifecycle published the wrong Guest IR contract: $runRoot"
}
& node $runner $positive.Wasm --bounded-lifecycle
if ($LASTEXITCODE -ne 0) { throw "Bounded lifecycle WASM execution failed: $runRoot" }
Write-Host 'PASS bounded lifecycle and WASM'

$default = Invoke-Case -Name 'default_rejects' -Source $source -Bounded $false
if ($default.ExitCode -eq 0 -or $default.Report.succeeded -or
    $default.Report.result -cne 'semantic_failed' -or
    @($default.Report.diagnostics | Where-Object code -CEQ 'ASCS3001').Count -eq 0 -or
    (Test-Path -LiteralPath $default.Wasm) -or (Test-Path -LiteralPath $default.Manifest)) {
    throw "Default build accepted exception syntax: $runRoot"
}
Write-Host 'PASS default rejects exception syntax'

$unsafeSource = Join-Path $runRoot 'UnsupportedExceptionConstructor.cs'
$sourceText = [IO.File]::ReadAllText($source)
$unsafeText = $sourceText.Replace('throw new Exception();', 'throw new Exception("unsupported");')
if ($unsafeText -ceq $sourceText) { throw 'Fixture no longer contains the expected exception constructor.' }
[IO.File]::WriteAllText($unsafeSource, $unsafeText, [Text.UTF8Encoding]::new($false))
$unsafe = Invoke-Case -Name 'bounded_rejects' -Source $unsafeSource -Bounded $true
if ($unsafe.ExitCode -eq 0 -or $unsafe.Report.succeeded -or
    $unsafe.Report.result -cne 'guest_ir_failed' -or
    @($unsafe.Report.diagnostics | Where-Object code -CEQ 'guest_ir_compile_failed').Count -ne 1 -or
    -not (@($unsafe.Report.diagnostics[0].output) -join "`n").Contains('ASCG1004') -or
    (Test-Path -LiteralPath $unsafe.Wasm) -or (Test-Path -LiteralPath $unsafe.Manifest)) {
    throw "Bounded build accepted an unsupported constructor: $runRoot"
}
Write-Host 'PASS bounded rejects unsupported constructor'

$customId = 'bounded_language_errors_custom'
$custom = Invoke-Case -Name 'custom_module_id' -Source $source -Bounded $true -ModuleId $customId
if ($custom.ExitCode -ne 0 -or $custom.Report.result -cne 'direct_abi_built' -or
    $custom.Report.module_id -cne $customId -or
    -not (Test-Path -LiteralPath $custom.Wasm -PathType Leaf) -or
    -not (Test-Path -LiteralPath $custom.Manifest -PathType Leaf)) {
    throw "Bounded build could not publish a custom module identity: $runRoot"
}
$customManifest = Get-Content -LiteralPath $custom.Manifest -Raw | ConvertFrom-Json
$customIr = Get-Content -LiteralPath $custom.GuestIr -Raw | ConvertFrom-Json
$customDebugMap = Get-Content -LiteralPath $custom.DebugMap -Raw | ConvertFrom-Json
if ($customManifest.module_id -cne $customId -or
    $customIr.module_id -cne $customId -or
    $customDebugMap.module_id -cne $customId -or
    (Get-FileHash -LiteralPath $custom.Wasm -Algorithm SHA256).Hash -ceq
        (Get-FileHash -LiteralPath $positive.Wasm -Algorithm SHA256).Hash) {
    throw "Custom module identity did not reach every formal artifact: $runRoot"
}
& node $runner $custom.Wasm --bounded-lifecycle
if ($LASTEXITCODE -ne 0) { throw "Custom module identity WASM execution failed: $runRoot" }
Write-Host 'PASS bounded custom module identity'

$invalid = Invoke-Case -Name 'invalid_module_id' -Source $source -Bounded $true `
    -ModuleId "bad`nmodule"
if ($invalid.ExitCode -eq 0 -or $invalid.Report.result -cne 'guest_ir_failed' -or
    (Test-Path -LiteralPath $invalid.Wasm) -or
    (Test-Path -LiteralPath $invalid.Manifest) -or
    -not (@($invalid.Report.diagnostics[0].output) -join "`n").Contains(
        '--module-id must be at most 1024 characters')) {
    throw "Bounded build accepted an invalid module identity: $runRoot"
}
Write-Host 'PASS bounded rejects control characters in module identity'

[ordered]@{
    passed = 5
    total = 5
    package = $package
    bounded_wasm_sha256 = (Get-FileHash -LiteralPath $positive.Wasm -Algorithm SHA256).Hash.ToLowerInvariant()
    custom_module_id = $customId
    default_result = [string]$default.Report.result
    unsupported_result = [string]$unsafe.Report.result
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8NoBOM
Write-Host 'Bounded language-error build contracts: 5/5'
Write-Host "Evidence: $runRoot"
