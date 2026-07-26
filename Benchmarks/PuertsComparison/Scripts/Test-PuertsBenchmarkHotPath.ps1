$ErrorActionPreference = 'Stop'

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "ASP53H1001 $Message"
    }
}

$PluginRoot = Split-Path -Parent (
    Split-Path -Parent (
        Split-Path -Parent $PSScriptRoot))
$InvocationPath = Join-Path $PluginRoot 'Source/AvidScriptBindings/Private/AvidScriptBindingInvocation.cpp'
Assert-True (Test-Path -LiteralPath $InvocationPath -PathType Leaf) `
    "missing production binding invocation source: $InvocationPath"

$InvocationSource = Get-Content -LiteralPath $InvocationPath -Raw
$ResolveStart = $InvocationSource.IndexOf('bool ResolveAvidScriptRuntimeHandle(')
$ResolveEnd = $InvocationSource.IndexOf('bool SetAvidScriptRuntimeNumericValue(', $ResolveStart)
Assert-True ($ResolveStart -ge 0 -and $ResolveEnd -gt $ResolveStart) `
    'unable to isolate ResolveAvidScriptRuntimeHandle'
$ResolveSource = $InvocationSource.Substring($ResolveStart, $ResolveEnd - $ResolveStart)

$NoPathResolvePattern = 'ResolveObject\(\s*\{\s*Slot\s*,\s*Generation\s*\}\s*,\s*ResolveResult\s*,\s*false\s*\)'
Assert-True ([regex]::IsMatch($ResolveSource, $NoPathResolvePattern)) `
    'successful dynamic handle resolution must suppress UObject path materialization'
Assert-True ($ResolveSource.Contains('Context.ObjectRegistry == nullptr')) `
    'dynamic handle resolution must retain the missing-registry guard'
Assert-True ($ResolveSource.Contains('!OutObject->IsA(ExpectedClass)')) `
    'dynamic handle resolution must retain runtime owner type validation'
Assert-True ($ResolveSource.Contains('ResolveResult.ErrorMessage')) `
    'dynamic handle resolution must retain registry failure diagnostics'

foreach ($FixtureToken in @(
    'AvidScriptPerf',
    'AAvidScriptPerfFixture',
    'scalar_noop',
    'vector_value',
    'object_roundtrip')) {
    Assert-True (-not $InvocationSource.Contains($FixtureToken)) `
        "production binding hot path must not reference benchmark fixture token: $FixtureToken"
}

Write-Output 'AvidScript production hot-path contracts passed: path_materialization=0 registry_guard=1 type_guard=1 fixture_fast_path=0'
