[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'AngelScriptValidation.psm1') -Force
$Adapter = Split-Path -Parent $PSScriptRoot
$Repository = [IO.Path]::GetFullPath((Join-Path $Adapter '../../../..'))
$HarnessSource = Join-Path $Repository 'Benchmarks/PuertsComparison/AvidScriptPerfHarness'
$Contract = Get-Content -LiteralPath (Join-Path $Adapter 'contract.json') -Raw | ConvertFrom-Json
$Installer = Join-Path $PSScriptRoot 'Install-AngelScriptAdapter.ps1'
$Root = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('install-contracts-' + [guid]::NewGuid().ToString('N'))
Assert-As36PhysicalPath $Root
$null = New-Item -ItemType Directory -Path $Root
$ProjectFile = Join-Path $Root 'Contract.uproject'
$Harness = Join-Path $Root 'Plugins/AvidScriptPerfHarness'
$ProjectText = '{"FileVersion":3,"Description":"preserved sentinel","Plugins":[{"Name":"Puerts","Enabled":false},{"Name":"Unrelated","Enabled":false}]}'
[IO.File]::WriteAllText($ProjectFile, $ProjectText)
foreach ($InputFile in $Contract.inputs) {
    $Destination = Join-Path $Harness $InputFile.path
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force
    # The frozen Windows candidate uses UTF-8 without BOM and CRLF. Local Git
    # checkouts can contain mixed newlines despite an identical Git blob.
    $Text = [IO.File]::ReadAllText((Join-Path $HarnessSource $InputFile.path))
    [IO.File]::WriteAllText($Destination, ($Text -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($false))
}
foreach ($Config in $Contract.integration_files) {
    $Destination = Join-Path $Harness $Config.destination
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force
    $Text = [IO.File]::ReadAllText((Join-Path $HarnessSource $Config.destination))
    [IO.File]::WriteAllText($Destination, ($Text -replace '\r?\n', "`r`n"), [Text.UTF8Encoding]::new($false))
}
foreach ($Dependency in @('Puerts', 'AvidScript')) {
    $Directory = Join-Path $Root "Plugins/$Dependency"
    $null = New-Item -ItemType Directory -Path $Directory -Force
    [IO.File]::WriteAllText((Join-Path $Directory "$Dependency.uplugin"), '{}')
}
function Get-TestSnapshot {
    @(Get-ChildItem -LiteralPath $Root -Recurse -File | Sort-Object FullName | ForEach-Object {
        $_.FullName + ':' + (Get-As36Sha256 $_.FullName)
    }) -join "`n"
}
$Passed = 0
$Before = Get-TestSnapshot
$null = & $Installer -ProjectFile $ProjectFile -Plan
if ((Get-TestSnapshot) -cne $Before) { throw 'Plan wrote files.' }
$Passed++
$null = & $Installer -ProjectFile $ProjectFile
$Readback = Get-Content -LiteralPath $ProjectFile -Raw | ConvertFrom-Json
if ($Readback.Description -cne 'preserved sentinel' -or @($Readback.Plugins).Count -ne 5 -or
    @($Readback.Plugins | Where-Object { $_.Name -ceq 'Unrelated' -and -not $_.Enabled }).Count -ne 1) { throw 'Project fields or unrelated plugins changed.' }
$Passed++
foreach ($Name in @('Angelscript', 'AvidScript', 'Puerts', 'AvidScriptPerfHarness')) {
    if (@($Readback.Plugins | Where-Object { $_.Name -ceq $Name -and $_.Enabled }).Count -ne 1) { throw "Plugin not enabled exactly once: $Name" }
}
$Passed++
$Before = Get-TestSnapshot
$null = & $Installer -ProjectFile $ProjectFile
if ((Get-TestSnapshot) -cne $Before) { throw 'Second install changed content.' }
$Passed++
function Test-NoWriteRejection {
    param([string]$Name)
    $Before = Get-TestSnapshot
    $Rejected = $false
    try { $null = & $Installer -ProjectFile $ProjectFile } catch { $Rejected = $true }
    if (-not $Rejected -or (Get-TestSnapshot) -cne $Before) { throw "Invalid input accepted or changed files: $Name" }
    $script:Passed++
}
$Fixture = Join-Path $Harness $Contract.inputs[0].path
$Saved = [IO.File]::ReadAllBytes($Fixture)
[IO.File]::AppendAllText($Fixture, "`n// divergent fixture")
Test-NoWriteRejection 'fixture mismatch'
[IO.File]::WriteAllBytes($Fixture, $Saved)
$Bridge = Join-Path $Harness 'Source/AvidScriptPerfHarness/Private/As36LeadershipBridge.cpp'
$Saved = [IO.File]::ReadAllBytes($Bridge)
[IO.File]::AppendAllText($Bridge, "`n// user-owned change")
Test-NoWriteRejection 'divergent adapter'
[IO.File]::WriteAllBytes($Bridge, $Saved)
$Saved = [IO.File]::ReadAllBytes($ProjectFile)
$Project = Get-Content -LiteralPath $ProjectFile -Raw | ConvertFrom-Json -AsHashtable
$Project.Plugins += @{ Name = 'Puerts'; Enabled = $true }
$Project | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $ProjectFile -Encoding utf8
Test-NoWriteRejection 'duplicate plugin'
[IO.File]::WriteAllBytes($ProjectFile, $Saved)
$Link = Join-Path $Root 'linked-destination'
$null = New-Item -ItemType Junction -Path $Link -Target (Join-Path $Root 'Plugins')
$Rejected = $false
try { Assert-As36PhysicalPath (Join-Path $Link 'AvidScript/AvidScript.uplugin') } catch { $Rejected = $true }
if (-not $Rejected) { throw 'Junction destination accepted.' }
$Passed++
$ActualProject = @(Get-As36ProtectedRoots $Adapter | ForEach-Object { Get-ChildItem -LiteralPath $_ -File -Filter '*.uproject' })
if ($ActualProject.Count -ne 1) { throw 'Expected exactly one main project for refusal control.' }
$Rejected = $false
try { $null = & $Installer -ProjectFile $ActualProject[0].FullName -Plan } catch { $Rejected = $true }
if (-not $Rejected) { throw 'Main project installation accepted.' }
$Passed++
Write-Output "AngelScript installation contracts: $Passed/9 passed; retained evidence=$Root"
if ($Passed -ne 9) { throw 'Unexpected contract count.' }
