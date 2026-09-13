[CmdletBinding()]
param([Parameter(Mandatory)][string]$ProjectFile, [switch]$Plan)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'AngelScriptValidation.psm1') -Force
$Adapter = Split-Path -Parent $PSScriptRoot
$ProjectFile = [IO.Path]::GetFullPath($ProjectFile)
$ProjectRoot = Split-Path -Parent $ProjectFile
if ([IO.Path]::GetExtension($ProjectFile) -ne '.uproject' -or -not (Test-Path -LiteralPath $ProjectFile -PathType Leaf)) { throw 'An existing isolated .uproject is required.' }
foreach ($ProtectedRoot in (Get-As36ProtectedRoots $Adapter)) {
    if ($ProjectRoot.Equals($ProtectedRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $ProjectRoot.StartsWith($ProtectedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Do not install the experimental adapter into the main project or repository.' }
}
Assert-As36PhysicalPath $ProjectFile
$Harness = Join-Path $ProjectRoot 'Plugins/AvidScriptPerfHarness'
$Contract = Get-Content -LiteralPath (Join-Path $Adapter 'contract.json') -Raw | ConvertFrom-Json
foreach ($InputFile in $Contract.inputs) {
    $Path = Join-Path $Harness $InputFile.path
    Assert-As36PhysicalPath $Path
    if ((Get-As36Sha256 $Path) -cne $InputFile.sha256) { throw "Frozen workload input changed: $($InputFile.path)" }
}
foreach ($Dependency in @('AvidScript', 'Puerts')) {
    $Path = Join-Path $ProjectRoot "Plugins/$Dependency/$Dependency.uplugin"
    Assert-As36PhysicalPath $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Physically installed $Dependency is required." }
}
$Entries = @($Contract.native_bridge.files | ForEach-Object {
    [pscustomobject]@{ source = Join-Path $Adapter $_.path; destination = Join-Path $Harness ('Source/AvidScriptPerfHarness/Private/' + [IO.Path]::GetFileName($_.path)); sha256 = $_.sha256; base_sha256 = $_.base_sha256 }
})
$Entries += [pscustomobject]@{ source = Join-Path $Adapter 'Script/AvidScriptLeadership.as'; destination = Join-Path $ProjectRoot 'Script/AvidScriptLeadership.as'; sha256 = $Contract.script_sha256; base_sha256 = $Contract.script_base_sha256 }
foreach ($Config in $Contract.integration_files) {
    $Entries += [pscustomobject]@{ source = Join-Path $Adapter $Config.path; destination = Join-Path $Harness $Config.destination; sha256 = $Config.sha256; base_sha256 = $Config.base_sha256 }
}
# Complete preflight before any write. Refuse divergent existing files.
foreach ($Entry in $Entries) {
    if ((Get-As36Sha256 $Entry.source) -cne $Entry.sha256) { throw "Adapter source hash mismatch: $($Entry.source)" }
    Assert-As36PhysicalPath $Entry.destination
    if (Test-Path -LiteralPath $Entry.destination) {
        $Current = Get-As36Sha256 $Entry.destination
        if ($Current -cne $Entry.sha256 -and $Current -cne $Entry.base_sha256) { throw "Refusing to overwrite divergent file: $($Entry.destination)" }
    }
}
$Project = Get-Content -LiteralPath $ProjectFile -Raw | ConvertFrom-Json -AsHashtable
if (-not $Project.ContainsKey('Plugins')) { $Project.Plugins = @() }
foreach ($Name in @('Angelscript', 'AvidScript', 'Puerts', 'AvidScriptPerfHarness')) {
    $Existing = @($Project.Plugins | Where-Object { $_['Name'] -ceq $Name })
    if ($Existing.Count -gt 1) { throw "Duplicate plugin entry: $Name" }
    if ($Existing.Count -eq 1) { $Existing[0].Enabled = $true }
    else { $Project.Plugins += @{ Name = $Name; Enabled = $true } }
}
if ($Plan) {
    $Entries | Select-Object destination, sha256 | ConvertTo-Json -Depth 4
    Write-Output "Plan: enable four plugins in $ProjectFile; no files written."
    exit 0
}
foreach ($Entry in $Entries) {
    if (-not (Test-Path -LiteralPath $Entry.destination) -or (Get-As36Sha256 $Entry.destination) -cne $Entry.sha256) {
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $Entry.destination) -Force
        [IO.File]::WriteAllBytes($Entry.destination, [IO.File]::ReadAllBytes($Entry.source))
    }
    if ((Get-As36Sha256 $Entry.destination) -cne $Entry.sha256) { throw 'Installed adapter readback failed.' }
}
$OriginalProject = Get-Content -LiteralPath $ProjectFile -Raw | ConvertFrom-Json -AsHashtable
if (($OriginalProject | ConvertTo-Json -Depth 32 -Compress) -cne ($Project | ConvertTo-Json -Depth 32 -Compress)) {
    [IO.File]::WriteAllText($ProjectFile, ($Project | ConvertTo-Json -Depth 32) + "`r`n", [Text.UTF8Encoding]::new($false))
}
Write-Output "Installed and hash-verified $($Entries.Count) adapter files. Build the isolated Editor before validation."
