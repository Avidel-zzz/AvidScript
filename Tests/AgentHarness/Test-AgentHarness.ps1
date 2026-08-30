[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$HarnessPath = Join-Path $RepositoryRoot 'Build/InvokeAgentHarness.ps1'
$ManifestPath = Join-Path $RepositoryRoot 'AgentHarness/manifest.json'
$SchemaPath = Join-Path $RepositoryRoot 'AgentHarness/schemas/manifest.schema.json'
$PowerShellHost = Join-Path $PSHOME 'pwsh.exe'
$Passed = 0
$Total = 7

function Assert-Contract {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "AgentHarness contract failed: $Message"
    }
}

function Invoke-HarnessJson {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $Output = & $PowerShellHost -NoProfile -File $HarnessPath @Arguments -Json 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Harness command failed: $($Output -join ' ')"
    }

    return ($Output -join [Environment]::NewLine) | ConvertFrom-Json -Depth 64
}

$ManifestText = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8
Assert-Contract ($ManifestText | Test-Json -SchemaFile $SchemaPath) 'manifest must satisfy its local schema'
$Passed++

$Audit = Invoke-HarnessJson @('audit')
Assert-Contract ([bool]$Audit.Passed) 'audit must pass'
Assert-Contract (@($Audit.Checks).Count -ge 10) 'audit must execute the core integrity checks'
$Passed++

$Context = Invoke-HarnessJson @(
    'context',
    '-Intent', '优化 Semantic 与 Puerts reflection binding',
    '-Paths', 'Source/AvidScriptBindings/Public/AvidScriptBindingDescriptor.h'
)
$DomainIds = @($Context.DomainPolicies.Id)
Assert-Contract ($DomainIds -contains 'csharp-frontend') 'semantic intent must route csharp-frontend'
Assert-Contract ($DomainIds -contains 'bindings') 'binding path must route bindings'
Assert-Contract ($DomainIds -contains 'performance') 'Puerts intent must route performance'
$Passed++

$Lessons = @(Invoke-HarnessJson @('lesson-query', '-Tags', 'path,bindings,toolchain', '-Limit', '5'))
Assert-Contract ($Lessons.Count -le 5) 'lesson-query must enforce the five-result ceiling'
Assert-Contract (@($Lessons.Id) -contains 'index-paths-before-read') 'path lesson must be discoverable'
Assert-Contract (@($Lessons.Id) -contains 'generated-ue-api-not-handwritten') 'binding architecture lesson must be discoverable'
$Passed++

$Verification = Invoke-HarnessJson @(
    'verify',
    '-Profile', 'Auto',
    '-Intent', 'Agent Harness 重构',
    '-Paths', 'AGENTS.md,AgentHarness/manifest.json,Build/InvokeAgentHarness.ps1'
)
Assert-Contract ($Verification.SelectedProfile -eq 'DocsOnly') 'Harness-only changes must select DocsOnly'
Assert-Contract ($Verification.Execution -eq 'impact-plan-only') 'H1 verify must not pretend to execute the H3 Gate runner'
$Passed++

$Bootstrap = Invoke-HarnessJson @('bootstrap', '-Intent', '恢复当前 AvidScript Phase')
Assert-Contract ($null -ne $Bootstrap.Phase) 'bootstrap must discover a Phase'
Assert-Contract ($Bootstrap.Phase.phase -gt 0) 'bootstrap Phase id must be positive'
$ExpectedLatestPhase = @(Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'Docs') -Filter 'Phase*_State.json' -File -Recurse |
    ForEach-Object { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).phase.id } |
    Measure-Object -Maximum)[0].Maximum
Assert-Contract ($Bootstrap.Phase.phase -eq $ExpectedLatestPhase) 'bootstrap must select the highest Phase id'
Assert-Contract ($Bootstrap.ProtectedDirty.Drift.Count -eq 0) 'protected dirty content must not drift'
Assert-Contract ($Bootstrap.ElapsedMs -le 2000) "bootstrap exceeded 2000 ms: $($Bootstrap.ElapsedMs)"
$Passed++

$BootstrapText = @(& $PowerShellHost -NoProfile -File $HarnessPath bootstrap -Intent '检查输出预算' 2>&1)
Assert-Contract ($LASTEXITCODE -eq 0) 'human bootstrap must exit successfully'
Assert-Contract ($BootstrapText.Count -le 100) "bootstrap exceeded 100 lines: $($BootstrapText.Count)"
$Passed++

Write-Output "AgentHarness tests: $Passed/$Total passed"
