param(
    [string]$DotNetPath = (Join-Path $env:USERPROFILE ".dotnet\dotnet.exe")
)

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$BuildScript = Join-Path $PluginRoot "Build\BuildCSharpActorLifecycle.ps1"
$SourcePath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\ActorLifecycleScript.cs"
$ProjectPath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\AvidScript.ActorLifecycle.csproj"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\BuildPublicationContracts"
$CacheParent = Join-Path $ProjectRoot "Saved\AvidScript\BuildPublicationContracts"
$CacheRoot = Join-Path $CacheParent "v1"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Write-FakeCompiler {
    param([string]$Path, [string]$Body)
    $Preamble = @'
param(
    [Parameter(Mandatory = $true)][string]$DotNetPath,
    [Parameter(Mandatory = $true)][string]$SemanticPath,
    [Parameter(Mandatory = $true)][string]$GuestIrPath,
    [Parameter(Mandatory = $true)][string]$StateSchemaPath,
    [Parameter(Mandatory = $true)][string]$WasmPath,
    [Parameter(Mandatory = $true)][string]$InspectionPath,
    [string]$Configuration = "Release"
)
$ErrorActionPreference = "Stop"
'@
    [System.IO.File]::WriteAllText($Path, $Preamble + [System.Environment]::NewLine + $Body, $Utf8)
}

foreach ($Directory in @($RunRoot, $CacheParent)) {
    if (Test-Path -LiteralPath $Directory) {
        Remove-Item -LiteralPath $Directory -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null

$SeedRoot = Join-Path $RunRoot "Seed"
$SeedReport = Join-Path $SeedRoot "actor_lifecycle.csharp.report.json"
$SeedManifest = Join-Path $SeedRoot "actor_lifecycle.avidscript.json"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $SeedRoot `
    -SourcePath $SourcePath `
    -ProjectPath $ProjectPath `
    -SemanticCacheRoot $CacheRoot `
    -ModuleId "csharp_actor_lifecycle" `
    -ArtifactStem "actor_lifecycle" `
    -ReportPath $SeedReport `
    -ManifestPath $SeedManifest | Out-Null
Assert-Condition ($LASTEXITCODE -eq 0) "seed ActorLifecycle build failed"
$SeedReportJson = Get-Content -Raw -LiteralPath $SeedReport | ConvertFrom-Json
Assert-Condition ($SeedReportJson.PSObject.Properties.Name -contains "build_reuse") "build report is missing build_reuse"
Assert-Condition (-not $SeedReportJson.build_reuse.frontend_reused) "ordinary seed build unexpectedly reused frontend"
Assert-Condition (-not $SeedReportJson.build_reuse.semantic_reused) "ordinary seed build unexpectedly reused semantic"
Assert-Condition ([string]::IsNullOrWhiteSpace([string]$SeedReportJson.build_reuse.prepared_report_file)) "ordinary seed build retained a prepared report path"
Assert-Condition ([string]::IsNullOrWhiteSpace([string]$SeedReportJson.build_reuse.prepared_report_sha256)) "ordinary seed build retained a prepared report hash"
Assert-Condition ($SeedReportJson.PSObject.Properties.Name -contains "binding_authorization") "build report is missing binding_authorization"
Assert-Condition (
    -not [bool]$SeedReportJson.binding_authorization.required -and
    [string]::IsNullOrWhiteSpace([string]$SeedReportJson.binding_authorization.package_name) -and
    [int]$SeedReportJson.binding_authorization.profile_import_count -eq 0 -and
    [int]$SeedReportJson.binding_authorization.used_import_count -eq 0) `
    "default source did not publish explicit empty binding authorization"
Assert-Condition ($null -eq $SeedReportJson.binding_package) "default source unexpectedly published a runtime binding package"
Assert-Condition ($SeedReportJson.semantic_cache.lookup -ceq "miss" -and $SeedReportJson.semantic_cache.published) `
    "seed build did not publish its semantic cache entry"
Assert-Condition (
    [int]$SeedReportJson.tool_invocations.frontend -eq 1 -and
    [int]$SeedReportJson.tool_invocations.semantic -eq 1 -and
    [int]$SeedReportJson.tool_invocations.guest_ir -eq 1 -and
    [int]$SeedReportJson.tool_invocations.wasm_backend -eq 1) `
    "seed build invocation counts differ"
Assert-Condition ((Get-Content -Raw -LiteralPath $SeedManifest).IndexOf("build_reuse", [System.StringComparison]::Ordinal) -lt 0) "seed manifest leaked build_reuse"
$SeedGuestIr = Join-Path $SeedRoot "actor_lifecycle.guestir.json"
$SeedStateSchema = Join-Path $SeedRoot "actor_lifecycle.state.json"
$SeedWasm = Join-Path $SeedRoot "actor_lifecycle.wasm"
Assert-Condition (Test-Path -LiteralPath $SeedGuestIr -PathType Leaf) "seed Guest IR is missing"
Assert-Condition (Test-Path -LiteralPath $SeedStateSchema -PathType Leaf) "seed state schema is missing"
Assert-Condition (Test-Path -LiteralPath $SeedWasm -PathType Leaf) "seed WASM is missing"
$SeedStateSchemaJson = Get-Content -Raw -LiteralPath $SeedStateSchema | ConvertFrom-Json
Assert-Condition ($SeedStateSchemaJson.schema_version -eq 2 -and
    $SeedStateSchemaJson.policy -eq "compatible" -and
    $SeedStateSchemaJson.contract_version -eq 1 -and
    @($SeedStateSchemaJson.slots | Where-Object { $_.PSObject.Properties.Name -contains "aliases" }).Count -eq @($SeedStateSchemaJson.slots).Count) `
    "seed state schema must use the structured v2 contract"

$GuestFailureCompiler = Join-Path $RunRoot "GuestFailureCompiler.ps1"
$GuestFailureBody = @"
`$Model = Get-Content -Raw -LiteralPath '$SeedGuestIr' | ConvertFrom-Json
`$Model.succeeded = `$false
`$Model | ConvertTo-Json -Depth 64 | Set-Content -LiteralPath `$GuestIrPath -Encoding utf8
Copy-Item -LiteralPath '$SeedStateSchema' -Destination `$StateSchemaPath -Force
[Console]::Error.WriteLine('guest lowering failed')
exit 1
"@
Write-FakeCompiler -Path $GuestFailureCompiler -Body $GuestFailureBody
$GuestFailureRoot = Join-Path $RunRoot "GuestFailure"
$GuestFailureReport = Join-Path $GuestFailureRoot "actor_lifecycle.csharp.report.json"
$GuestFailureManifest = Join-Path $GuestFailureRoot "actor_lifecycle.avidscript.json"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $GuestFailureRoot `
    -SourcePath $SourcePath `
    -ProjectPath $ProjectPath `
    -SemanticCacheRoot $CacheRoot `
    -ModuleId "csharp_actor_lifecycle" `
    -ArtifactStem "actor_lifecycle" `
    -ReportPath $GuestFailureReport `
    -ManifestPath $GuestFailureManifest `
    -GuestCompilerPath $GuestFailureCompiler | Out-Null
$GuestFailureExit = $LASTEXITCODE
Assert-Condition ($GuestFailureExit -eq 1) "failed Guest lowering must return exit 1; actual=$GuestFailureExit"
$GuestFailureJson = Get-Content -Raw -LiteralPath $GuestFailureReport | ConvertFrom-Json
Assert-Condition ($GuestFailureJson.result -eq "guest_ir_failed") "succeeded=false Guest IR was misclassified as backend failure"
Assert-Condition (-not (Test-Path -LiteralPath $GuestFailureManifest -PathType Leaf)) "Guest failure left a manifest"
Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $GuestFailureRoot "actor_lifecycle.guestir.json") -PathType Leaf)) "Guest failure left Guest IR"
Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $GuestFailureRoot "actor_lifecycle.state.json") -PathType Leaf)) "Guest failure left state schema"
Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $GuestFailureRoot "actor_lifecycle.wasm") -PathType Leaf)) "Guest failure left WASM"

$MissingExportCompiler = Join-Path $RunRoot "MissingExportCompiler.ps1"
$MissingExportBody = @"
Copy-Item -LiteralPath '$SeedGuestIr' -Destination `$GuestIrPath -Force
Copy-Item -LiteralPath '$SeedStateSchema' -Destination `$StateSchemaPath -Force
`$Bytes = [System.IO.File]::ReadAllBytes('$SeedWasm')
`$From = [System.Text.Encoding]::ASCII.GetBytes('avid_on_tick')
`$To = [System.Text.Encoding]::ASCII.GetBytes('avid_on_tock')
`$Found = `$false
for (`$Offset = 0; `$Offset -le `$Bytes.Length - `$From.Length; `$Offset++) {
    `$Match = `$true
    for (`$Index = 0; `$Index -lt `$From.Length; `$Index++) {
        if (`$Bytes[`$Offset + `$Index] -ne `$From[`$Index]) { `$Match = `$false; break }
    }
    if (`$Match) {
        [System.Array]::Copy(`$To, 0, `$Bytes, `$Offset, `$To.Length)
        `$Found = `$true
        break
    }
}
if (-not `$Found) { throw 'seed WASM tick export was not found' }
[System.IO.File]::WriteAllBytes(`$WasmPath, `$Bytes)
`$BackendDll = Join-Path '$PluginRoot' 'Tools\AvidScript.WasmBackend\bin\Release\net8.0\AvidScript.WasmBackend.dll'
& `$DotNetPath `$BackendDll --inspect `$WasmPath `$InspectionPath
exit `$LASTEXITCODE
"@
Write-FakeCompiler -Path $MissingExportCompiler -Body $MissingExportBody
$MissingExportRoot = Join-Path $RunRoot "MissingExport"
$MissingExportReport = Join-Path $MissingExportRoot "actor_lifecycle.csharp.report.json"
$MissingExportManifest = Join-Path $MissingExportRoot "actor_lifecycle.avidscript.json"
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $MissingExportRoot `
    -SourcePath $SourcePath `
    -ProjectPath $ProjectPath `
    -SemanticCacheRoot $CacheRoot `
    -ModuleId "csharp_actor_lifecycle" `
    -ArtifactStem "actor_lifecycle" `
    -ReportPath $MissingExportReport `
    -ManifestPath $MissingExportManifest `
    -GuestCompilerPath $MissingExportCompiler | Out-Null
$MissingExportExit = $LASTEXITCODE
Assert-Condition ($MissingExportExit -eq 1) "missing final WASM export must return exit 1; actual=$MissingExportExit"
$MissingExportJson = Get-Content -Raw -LiteralPath $MissingExportReport | ConvertFrom-Json
Assert-Condition ($MissingExportJson.result -eq "direct_abi_unsupported") "missing final WASM export was not rejected"
Assert-Condition ($MissingExportJson.observed_exports -contains "avid_on_tock") "report did not inspect the mutated final WASM"
Assert-Condition (-not (Test-Path -LiteralPath $MissingExportManifest -PathType Leaf)) "missing export failure left a manifest"
Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $MissingExportRoot "actor_lifecycle.wasm") -PathType Leaf)) "missing export failure left WASM"

$PublicationRoot = Join-Path $RunRoot "PublicationFailure"
$PublicationReportDirectory = Join-Path $PublicationRoot "ReportIsDirectory"
$PublicationManifest = Join-Path $PublicationRoot "actor_lifecycle.avidscript.json"
New-Item -ItemType Directory -Force -Path $PublicationReportDirectory | Out-Null
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $PublicationRoot `
    -SourcePath $SourcePath `
    -ProjectPath $ProjectPath `
    -SemanticCacheRoot $CacheRoot `
    -ModuleId "csharp_actor_lifecycle" `
    -ArtifactStem "actor_lifecycle" `
    -ReportPath $PublicationReportDirectory `
    -ManifestPath $PublicationManifest | Out-Null
$PublicationExit = $LASTEXITCODE
Assert-Condition ($PublicationExit -eq 1) "report publication failure must return exit 1; actual=$PublicationExit"
Assert-Condition (-not (Test-Path -LiteralPath $PublicationManifest -PathType Leaf)) "report publication failure left a manifest"
Assert-Condition (-not (Test-Path -LiteralPath (Join-Path $PublicationRoot "actor_lifecycle.wasm") -PathType Leaf)) "report publication failure left WASM"

$PreparedFailureRoot = Join-Path $RunRoot "PreparedFailure"
$PreparedFailureReport = Join-Path $PreparedFailureRoot "actor_lifecycle.csharp.report.json"
$PreparedFailureManifest = Join-Path $PreparedFailureRoot "actor_lifecycle.avidscript.json"
$PreparedFailureWasm = Join-Path $PreparedFailureRoot "actor_lifecycle.wasm"
$MissingPreparedReport = Join-Path $RunRoot "MissingPrepared\missing.csharp.report.json"
New-Item -ItemType Directory -Force -Path $PreparedFailureRoot | Out-Null
[System.IO.File]::WriteAllText($PreparedFailureManifest, "stale-manifest", $Utf8)
[System.IO.File]::WriteAllText($PreparedFailureWasm, "stale-wasm", $Utf8)
& $BuildScript `
    -DotNetPath $DotNetPath `
    -OutputRoot $PreparedFailureRoot `
    -SourcePath $SourcePath `
    -ProjectPath $ProjectPath `
    -SemanticCacheRoot $CacheRoot `
    -ModuleId "csharp_actor_lifecycle" `
    -ArtifactStem "actor_lifecycle" `
    -PreparedBuildReportPath $MissingPreparedReport `
    -ReportPath $PreparedFailureReport `
    -ManifestPath $PreparedFailureManifest | Out-Null
$PreparedFailureExit = $LASTEXITCODE
Assert-Condition ($PreparedFailureExit -eq 1) "invalid prepared report must return exit 1; actual=$PreparedFailureExit"
Assert-Condition (Test-Path -LiteralPath $PreparedFailureReport -PathType Leaf) "invalid prepared import did not publish a failure report"
$PreparedFailureJson = Get-Content -Raw -LiteralPath $PreparedFailureReport | ConvertFrom-Json
Assert-Condition ($PreparedFailureJson.result -eq "prepared_semantic_invalid") "invalid prepared import has the wrong result"
Assert-Condition (@($PreparedFailureJson.diagnostics | Where-Object code -eq "ASBI4403").Count -eq 1) "invalid prepared import diagnostic is missing"
Assert-Condition ($PreparedFailureJson.semantic_cache.lookup -ceq "disabled") `
    "invalid prepared import unexpectedly performed cache lookup"
Assert-Condition (
    [int]$PreparedFailureJson.tool_invocations.frontend -eq 0 -and
    [int]$PreparedFailureJson.tool_invocations.semantic -eq 0) `
    "invalid prepared import invocation counts differ"
Assert-Condition (-not (Test-Path -LiteralPath $PreparedFailureManifest -PathType Leaf)) "invalid prepared import left a manifest"
Assert-Condition (-not (Test-Path -LiteralPath $PreparedFailureWasm -PathType Leaf)) "invalid prepared import left WASM"

Write-Output "AvidScript.CSharpFrontend.BuildPublicationContracts: 4/4 passed"
exit 0
