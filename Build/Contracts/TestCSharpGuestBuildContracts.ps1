[CmdletBinding()]
param([string]$DotNetPath = "$env:USERPROFILE/.dotnet/dotnet.exe")

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$runRoot = Join-Path $projectRoot ('Saved/AvidScript/GuestBuildContracts/' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($runRoot)
$compilerPath = Join-Path $pluginRoot 'Build/InvokeCSharpGuestCompiler.ps1'
# Preserve the real compiler parameters, then alter only its final IR artifact.
# The build entry must reject inconsistent artifacts even from a custom compiler.
$parseErrors = $null
$tokens = $null
$compilerAst = [Management.Automation.Language.Parser]::ParseFile($compilerPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Cannot parse the formal compiler entry.' }
$wrapperBody = @'
$arguments = @('-NoProfile', '-File', $env:AVIDSCRIPT_CONTRACT_COMPILER)
foreach ($entry in $PSBoundParameters.GetEnumerator()) {
    $arguments += '-' + $entry.Key
    if ($entry.Value -isnot [Management.Automation.SwitchParameter]) { $arguments += [string]$entry.Value }
}
& (Join-Path $PSHOME 'pwsh.exe') @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$model = Get-Content -LiteralPath $GuestIrPath -Raw | ConvertFrom-Json
switch ($env:AVIDSCRIPT_CONTRACT_MUTATION) {
    'schema' { $model.schema_version = 3 }
    'version' { $model.ir_version = '1.2' }
    'future' { $model.schema_version = 99; $model.ir_version = '99.0' }
    'provenance' { $model.provenance.semantic_sha256 = '0' * 64 }
    default { throw 'Unknown contract mutation.' }
}
$model | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $GuestIrPath -Encoding utf8NoBOM
# Preserve the debug map's byte provenance so rejection tests the IR contract.
$debugMap = Get-Content -LiteralPath $DebugMapPath -Raw | ConvertFrom-Json
$debugMap.provenance.guest_ir_sha256 = (Get-FileHash -LiteralPath $GuestIrPath -Algorithm SHA256).Hash.ToLowerInvariant()
$debugMap | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $DebugMapPath -Encoding utf8NoBOM
exit 0
'@
$wrapperPath = Join-Path $runRoot 'MutateGuestCompiler.ps1'
[IO.File]::WriteAllText($wrapperPath, $compilerAst.ParamBlock.Extent.Text + "`n" + $wrapperBody)
$previousCompiler = $env:AVIDSCRIPT_CONTRACT_COMPILER
$previousMutation = $env:AVIDSCRIPT_CONTRACT_MUTATION
try {
    $env:AVIDSCRIPT_CONTRACT_COMPILER = $compilerPath
    $results = foreach ($case in @('current', 'schema', 'version', 'future', 'provenance')) {
        $outputRoot = Join-Path $runRoot $case
        $arguments = @('-NoProfile', '-File', (Join-Path $pluginRoot 'Build/BuildCSharpActorLifecycle.ps1'),
            '-DotNetPath', $DotNetPath, '-OutputRoot', $outputRoot, '-ProjectRoot', $projectRoot,
            '-CompilerWorkerMode', 'disabled', '-DisableCompilationCache', '-DisableSemanticCache')
        if ($case -cne 'current') {
            $env:AVIDSCRIPT_CONTRACT_MUTATION = $case
            $arguments += @('-GuestCompilerPath', $wrapperPath)
        }
        & (Join-Path $PSHOME 'pwsh.exe') @arguments *> (Join-Path $runRoot ($case + '.log'))
        $exitCode = $LASTEXITCODE
        $report = Get-Content -LiteralPath (Join-Path $outputRoot 'actor_lifecycle.csharp.report.json') -Raw | ConvertFrom-Json
        $manifestExists = Test-Path -LiteralPath (Join-Path $outputRoot 'actor_lifecycle.avidscript.json')
        $wasmExists = Test-Path -LiteralPath (Join-Path $outputRoot 'actor_lifecycle.wasm')
        if ($case -ceq 'current') {
            if ($exitCode -ne 0 -or $report.result -cne 'direct_abi_built' -or
                -not $report.succeeded -or -not $manifestExists -or -not $wasmExists) {
                throw "Current compiler cannot publish through the formal build: $runRoot"
            }
        } else {
            $contractErrors = @($report.diagnostics | Where-Object code -CEQ 'direct_abi_contract_invalid')
            if ($exitCode -eq 0 -or $report.result -cne 'direct_abi_unsupported' -or
                $contractErrors.Count -ne 1 -or $contractErrors[0].guest_contract_valid -or
                -not $contractErrors[0].debug_map_contract_valid -or
                -not $contractErrors[0].state_schema_contract_valid -or
                -not $contractErrors[0].wasm_inspection_valid -or $manifestExists -or $wasmExists) {
                throw "Expected precise IR rejection and no loadable output for ${case}: $runRoot"
            }
        }
        Write-Host "PASS $case"
        [ordered]@{ name = $case; passed = $true; exit_code = $exitCode }
    }
    [ordered]@{ passed = @($results).Count; total = 5; results = @($results) } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json')
    Write-Host "CSharp Guest build contracts: $(@($results).Count)/5"
    Write-Host "Evidence: $runRoot"
} finally {
    $env:AVIDSCRIPT_CONTRACT_COMPILER = $previousCompiler
    $env:AVIDSCRIPT_CONTRACT_MUTATION = $previousMutation
}
