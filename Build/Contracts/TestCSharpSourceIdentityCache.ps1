param(
    [Parameter(Mandatory = $true)][string]$BindingPackagePath,
    [Parameter(Mandatory = $true)][string]$GeneratedTypeManifestPath,
    [string]$BaselineBuildReportPath = '',
    [string]$DotNetPath = ''
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$RunId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$RunRoot = Join-Path $ProjectRoot "Saved/AvidScript/SourceIdentityContracts/$RunId"
$EvidenceRoot = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) "AvidScript/Verification/SourceIdentityCache/$RunId"
$Utf8 = [Text.UTF8Encoding]::new($false)
$Cases = [Collections.Generic.List[string]]::new()
$Builds = [Collections.Generic.List[object]]::new()
$PersistentPathBefore = @([Environment]::GetEnvironmentVariable('PATH', 'User'), [Environment]::GetEnvironmentVariable('PATH', 'Machine'))
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
$env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
$env:DOTNET_NOLOGO = '1'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
$env:MSBUILDDISABLENODEREUSE = '1'
$env:DOTNET_CLI_USE_MSBUILD_SERVER = '0'
$env:UseSharedCompilation = 'false'
$env:DOTNET_CLI_HOME = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Toolchain/SemanticTests'
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
if ([string]::IsNullOrWhiteSpace($DotNetPath)) { $DotNetPath = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.dotnet/dotnet.exe' }
$Version = & $DotNetPath --version
if ($LASTEXITCODE -ne 0 -or $Version -cne '8.0.416') { throw 'Installed SDK must be 8.0.416.' }
New-Item -ItemType Directory -Force -Path $RunRoot, $EvidenceRoot | Out-Null
. (Join-Path $PluginRoot 'Build/AvidScriptCSharpSemanticCache.ps1')
$SourcePath = Join-Path $PluginRoot 'Fixtures/Phase66/GeneratedNaturalReload.cs'
$ProjectPath = Join-Path $PluginRoot 'Fixtures/Phase66/GeneratedNaturalReload.csproj'
$SourceId = 'Fixtures/Phase66/GeneratedNaturalReload.cs'
$DefaultSourceId = Resolve-AvidScriptCSharpSourceId -ProjectRoot $ProjectRoot -SourcePath $SourcePath
$ModuleId = 'avidscript.fixture.generated_natural_reload'
$SemanticCacheRoot = Join-Path $RunRoot 'SemanticCache/v1'
$CompilationCacheRoot = Join-Path $RunRoot 'CompilationCache/v1'
$ArtifactFields = @('frontend_file', 'semantic_file', 'guest_ir_file', 'debug_map_file', 'state_schema_file', 'wasm_file')
$Authorization = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $BindingPackagePath
$Profile = Resolve-AvidScriptCSharpLanguageProfile -Name gameplay-v1 -PluginRoot $PluginRoot -DotNetPath $DotNetPath -Configuration Release

function Assert-SourceIdentity([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Pass-SourceIdentity([string]$Name) {
    $Cases.Add($Name)
    Write-Output "PASS $Name"
}
function Write-SourceIdentityJson([string]$Path, $Value) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 100), $Utf8)
}
function Get-SourceIdentityArtifactHashes($Report) {
    $Hashes = [ordered]@{}
    foreach ($Field in $ArtifactFields) {
        $Path = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Report.artifacts.$Field
        $Hashes[$Field] = Get-AvidScriptBindingSha256Hex $Path
    }
    return $Hashes
}
function Assert-SourceIdentityHashes($Actual, $Expected, [string]$Label) {
    foreach ($Field in $ArtifactFields) {
        Assert-SourceIdentity ($Actual[$Field] -ceq $Expected[$Field]) "$Label changed $Field bytes."
    }
}
function Invoke-SourceIdentityBuild([string]$Name, [string]$Identity, [bool]$Explicit = $true, [string]$PreparedReport = '') {
    $Output = Join-Path $RunRoot $Name
    $Log = Join-Path $EvidenceRoot "$Name.log"
    $Arguments = @('-NoProfile', '-File', (Join-Path $PluginRoot 'Build/BuildCSharpActorLifecycle.ps1'),
        '-DotNetPath', $DotNetPath, '-ProjectRoot', $ProjectRoot, '-SourcePath', $SourcePath, '-ProjectPath', $ProjectPath,
        '-BindingPackagePath', $BindingPackagePath, '-GeneratedTypeManifestPath', $GeneratedTypeManifestPath,
        '-AllowGeneratedTypeImports', '-ModuleId', $ModuleId, '-ArtifactStem', 'generated_types',
        '-LanguageProfile', 'gameplay-v1', '-CompilerWorkerMode', 'disabled', '-OutputRoot', $Output,
        '-SemanticCacheRoot', $SemanticCacheRoot, '-CompilationCacheRoot', $CompilationCacheRoot)
    if ($Explicit) { $Arguments += @('-SourceId', $Identity) }
    if ($PreparedReport) { $Arguments += @('-PreparedBuildReportPath', $PreparedReport) }
    $Watch = [Diagnostics.Stopwatch]::StartNew()
    & (Join-Path $PSHOME 'pwsh.exe') @Arguments *> $Log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $Log -Tail 25; throw "Build failed: $Name" }
    $ReportPath = Join-Path $Output 'generated_types.csharp.report.json'
    $Report = Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json
    Assert-SourceIdentity ($Report.result -ceq 'direct_abi_built' -and $Report.succeeded) "$Name did not publish a valid build."
    $ExpectedId = if ($Explicit) { $Identity } else { $DefaultSourceId }
    Assert-SourceIdentity ($Report.source.source_id -ceq $ExpectedId) "$Name report lost SourceId."
    Assert-SourceIdentity ($Report.source.file -ceq $DefaultSourceId) "$Name report lost physical source path."
    foreach ($Field in @('frontend_file', 'semantic_file')) {
        $Model = Get-Content -Raw -LiteralPath (Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Report.artifacts.$Field) | ConvertFrom-Json
        Assert-SourceIdentity ($Model.source.source_id -ceq $ExpectedId) "$Name $Field has the wrong SourceId."
    }
    $Debug = Get-Content -Raw -LiteralPath (Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Report.artifacts.debug_map_file) | ConvertFrom-Json
    $Manifest = Get-Content -Raw -LiteralPath (Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Report.artifacts.manifest_file) | ConvertFrom-Json
    Assert-SourceIdentity ($Debug.source.id -ceq $ExpectedId -and $Manifest.source.file -ceq $ExpectedId) "$Name debug or Runtime identity differs."
    Assert-SourceIdentity ([string]::IsNullOrWhiteSpace($Report.semantic_cache.diagnostic_code) -and
        [string]::IsNullOrWhiteSpace($Report.compilation_cache.diagnostic_code)) "$Name cache admission reported a diagnostic."
    $Builds.Add([pscustomobject]@{ name=$Name; elapsed_ms=$Watch.ElapsedMilliseconds; source_id=$ExpectedId;
        semantic_lookup=$Report.semantic_cache.lookup; compilation_lookup=$Report.compilation_cache.lookup; tool_invocations=$Report.tool_invocations })
    return [pscustomobject]@{ Report=$Report; Path=$ReportPath; Hashes=(Get-SourceIdentityArtifactHashes $Report) }
}
function Assert-SourceIdentityWarm($Build, [string]$Label) {
    Assert-SourceIdentity ($Build.Report.semantic_cache.lookup -ceq 'hit' -and $Build.Report.compilation_cache.lookup -ceq 'hit') "$Label did not hit both caches."
    Assert-SourceIdentity ($Build.Report.tool_invocations.frontend -eq 0 -and $Build.Report.tool_invocations.semantic -eq 0 -and
        $Build.Report.tool_invocations.guest_ir -eq 0 -and $Build.Report.tool_invocations.wasm_backend -eq 0) "$Label invoked a compiler stage."
}
function New-SourceIdentityReport($Original, [string]$Name, [scriptblock]$Mutation) {
    $Report = $Original | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    & $Mutation $Report
    $Path = Join-Path $RunRoot "Mutations/$Name.csharp.report.json"
    Write-SourceIdentityJson $Path $Report
    return $Path
}
function Assert-SourceIdentityRejected([string]$Name, [string]$ReportPath, [string]$Identity, [string]$Code) {
    $Root = Join-Path $RunRoot "Rejected/$Name"
    New-Item -ItemType Directory -Force -Path $Root | Out-Null
    $Frontend = Join-Path $Root 'frontend.json'
    $Semantic = Join-Path $Root 'semantic.json'
    [IO.File]::WriteAllText($Frontend, 'previous frontend', $Utf8)
    [IO.File]::WriteAllText($Semantic, 'previous semantic', $Utf8)
    $Before = @(Get-AvidScriptBindingSha256Hex $Frontend; Get-AvidScriptBindingSha256Hex $Semantic)
    $ObservedCode = ''
    try {
        Import-AvidScriptCSharpPreparedSemantic -PreparedReportPath $ReportPath -ProjectRoot $ProjectRoot `
            -ExpectedSourcePath $SourcePath -ExpectedSourceId $Identity -ExpectedAuthorizationPackage $Authorization `
            -ExpectedLanguageProfile $Profile -FrontendDestinationPath $Frontend -SemanticDestinationPath $Semantic | Out-Null
    }
    catch { $ObservedCode = [string]$_.Exception.Data['AvidScriptCode'] }
    Assert-SourceIdentity ($ObservedCode -ceq $Code) "$Name rejection differs: $ObservedCode"
    Assert-SourceIdentity ((Get-AvidScriptBindingSha256Hex $Frontend) -ceq $Before[0] -and
        (Get-AvidScriptBindingSha256Hex $Semantic) -ceq $Before[1]) "$Name changed previous destination files."
    Pass-SourceIdentity $Name
}

$Cold = Invoke-SourceIdentityBuild 'AliasCold' $SourceId
Assert-SourceIdentity ($Cold.Report.semantic_cache.published -and $Cold.Report.compilation_cache.published) 'Cold build did not publish both caches.'
Pass-SourceIdentity 'alias cold publication'
if ($BaselineBuildReportPath) {
    $Baseline = Get-Content -Raw -LiteralPath $BaselineBuildReportPath | ConvertFrom-Json
    $BaselineHashes = Get-SourceIdentityArtifactHashes $Baseline
    Assert-SourceIdentityHashes $Cold.Hashes $BaselineHashes 'C20 runtime baseline'
    Pass-SourceIdentity 'unchanged six C20 runtime artifacts'
}
$Warm = Invoke-SourceIdentityBuild 'AliasWarm' $SourceId
Assert-SourceIdentityWarm $Warm 'AliasWarm'
Assert-SourceIdentityHashes $Warm.Hashes $Cold.Hashes 'AliasWarm'
Pass-SourceIdentity 'alias warm six-byte parity and zero compiler invocations'
$Prepared = Invoke-SourceIdentityBuild 'AliasPrepared' $SourceId $true $Cold.Path
Assert-SourceIdentity ($Prepared.Report.build_reuse.frontend_reused -and $Prepared.Report.build_reuse.semantic_reused -and
    $Prepared.Report.compilation_cache.lookup -ceq 'hit' -and $Prepared.Report.tool_invocations.frontend -eq 0 -and
    $Prepared.Report.tool_invocations.semantic -eq 0 -and $Prepared.Report.tool_invocations.guest_ir -eq 0 -and
    $Prepared.Report.tool_invocations.wasm_backend -eq 0) 'Prepared alias was not reused.'
Assert-SourceIdentityHashes $Prepared.Hashes $Cold.Hashes 'AliasPrepared'
Pass-SourceIdentity 'prepared alias six-byte parity and zero compiler invocations'

$OtherId = 'Scripts/Skills/GeneratedFlow.cs'
$OtherCold = Invoke-SourceIdentityBuild 'OtherCold' $OtherId
Assert-SourceIdentity ($OtherCold.Report.semantic_cache.published -and $OtherCold.Report.compilation_cache.published -and
    $OtherCold.Report.semantic_cache.key -cne $Cold.Report.semantic_cache.key -and
    $OtherCold.Report.compilation_cache.key -cne $Cold.Report.compilation_cache.key) 'Other SourceId reused the original cache identity.'
foreach ($Field in @('frontend_file', 'semantic_file', 'debug_map_file')) {
    Assert-SourceIdentity ($OtherCold.Hashes[$Field] -cne $Cold.Hashes[$Field]) "Other SourceId retained old $Field."
}
Pass-SourceIdentity 'different alias has independent caches and source maps'
$OtherWarm = Invoke-SourceIdentityBuild 'OtherWarm' $OtherId
Assert-SourceIdentityWarm $OtherWarm 'OtherWarm'
Assert-SourceIdentityHashes $OtherWarm.Hashes $OtherCold.Hashes 'OtherWarm'
Pass-SourceIdentity 'second alias warm six-byte parity'

$Default = Invoke-SourceIdentityBuild 'DefaultCold' '' $false
$ExplicitDefault = Invoke-SourceIdentityBuild 'ExplicitDefaultWarm' $DefaultSourceId
Assert-SourceIdentityWarm $ExplicitDefault 'ExplicitDefaultWarm'
Assert-SourceIdentity ($ExplicitDefault.Report.semantic_cache.key -ceq $Default.Report.semantic_cache.key) 'Explicit default changed the cache key.'
Assert-SourceIdentityHashes $ExplicitDefault.Hashes $Default.Hashes 'ExplicitDefaultWarm'
Pass-SourceIdentity 'implicit and explicit default share the same identity'
$LegacyPath = New-SourceIdentityReport $Default.Report 'LegacyDefault' { param($Report) $Report.source.PSObject.Properties.Remove('source_id') }
$Legacy = Invoke-SourceIdentityBuild 'LegacyDefaultPrepared' $DefaultSourceId $true $LegacyPath
Assert-SourceIdentityHashes $Legacy.Hashes $Default.Hashes 'LegacyDefaultPrepared'
Pass-SourceIdentity 'legacy default report compatibility'
$LegacyAliasPath = New-SourceIdentityReport $Cold.Report 'LegacyAlias' { param($Report) $Report.source.PSObject.Properties.Remove('source_id') }
Assert-SourceIdentityRejected 'legacy alias cannot infer identity' $LegacyAliasPath $SourceId 'ASBI4401'
Assert-SourceIdentityRejected 'caller identity mismatch' $Cold.Path $OtherId 'ASBI4401'
foreach ($Value in @($OtherId, '', 123, $null)) {
    $Name = 'report identity ' + $(if ($null -eq $Value) { 'null' } elseif ($Value -ceq '') { 'empty' } else { [string]$Value })
    $ReportPath = New-SourceIdentityReport $Cold.Report ([Guid]::NewGuid().ToString('N')) { param($Report) $Report.source.source_id = $Value }
    Assert-SourceIdentityRejected $Name $ReportPath $SourceId 'ASBI4401'
}
foreach ($Field in @('frontend_file', 'semantic_file')) {
    $ReportPath = New-SourceIdentityReport $Cold.Report "Forged-$Field" {
        param($Report)
        $Path = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Report.artifacts.$Field
        $Model = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
        $Model.source.source_id = $OtherId
        $Forged = Join-Path $RunRoot "Mutations/$Field.json"
        Write-SourceIdentityJson $Forged $Model
        $Report.output_root = $RunRoot
        $Report.artifacts.$Field = $Forged
        $HashField = if ($Field -ceq 'frontend_file') { 'frontend' } else { 'semantic' }
        $Report.$HashField.artifact_sha256 = Get-AvidScriptBindingSha256Hex $Forged
    }
    Assert-SourceIdentityRejected "rehashed $Field identity forgery" $ReportPath $SourceId 'ASBI4403'
}
$BadPhysical = New-SourceIdentityReport $Cold.Report 'PhysicalMismatch' { param($Report) $Report.source.file = $ProjectPath }
Assert-SourceIdentityRejected 'physical path still verified' $BadPhysical $SourceId 'ASBI4401'
$BadHash = New-SourceIdentityReport $Cold.Report 'HashMismatch' { param($Report) $Report.source.sha256 = '0' * 64 }
Assert-SourceIdentityRejected 'physical source hash still verified' $BadHash $SourceId 'ASBI4401'

$Context = Get-AvidScriptCSharpSemanticCacheContext -PluginRoot $PluginRoot -ProjectRoot $ProjectRoot -CacheRoot $SemanticCacheRoot `
    -Configuration Release -SourcePath $SourcePath -SourceId $SourceId -ProjectPath $ProjectPath -AuthorizationPackage $Authorization -LanguageProfile $Profile
$EntryBefore = Get-AvidScriptBindingSha256Hex $Context.EntryReportPath
$Mismatch = Import-AvidScriptCSharpSemanticCacheEntry -Context $Context -ProjectRoot $ProjectRoot -ExpectedSourcePath $SourcePath `
    -ExpectedSourceId $OtherId -ExpectedAuthorizationPackage $Authorization -FrontendDestinationPath (Join-Path $RunRoot 'BadContext/frontend.json') `
    -SemanticDestinationPath (Join-Path $RunRoot 'BadContext/semantic.json')
Assert-SourceIdentity ($Mismatch.Status -ceq 'rejected' -and $Mismatch.DiagnosticCode -ceq 'ASBI4502' -and
    [string]::IsNullOrWhiteSpace($Mismatch.CorruptEntryPath) -and (Get-AvidScriptBindingSha256Hex $Context.EntryReportPath) -ceq $EntryBefore -and
    -not (Test-Path -LiteralPath (Join-Path $RunRoot 'BadContext'))) 'Caller mismatch consumed a valid cache or copied outputs.'
Pass-SourceIdentity 'cache caller mismatch preserves valid entry'
$ObservedCode = ''
try {
    Publish-AvidScriptCSharpSemanticCacheEntry -Context $Context -ProjectRoot $ProjectRoot -ExpectedSourcePath $SourcePath `
        -ExpectedSourceId $OtherId -ExpectedAuthorizationPackage $Authorization -SourceReportPath $Cold.Path | Out-Null
}
catch { $ObservedCode = [string]$_.Exception.Data['AvidScriptCode'] }
Assert-SourceIdentity ($ObservedCode -ceq 'ASBI4502' -and (Get-AvidScriptBindingSha256Hex $Context.EntryReportPath) -ceq $EntryBefore) 'Publication accepted a mismatched caller identity.'
Pass-SourceIdentity 'cache publisher mismatch preserves valid entry'
$CaseContext = Get-AvidScriptCSharpSemanticCacheContext -PluginRoot $PluginRoot -ProjectRoot $ProjectRoot -CacheRoot $SemanticCacheRoot `
    -Configuration Release -SourcePath $SourcePath -SourceId $SourceId.ToUpperInvariant() -ProjectPath $ProjectPath -AuthorizationPackage $Authorization -LanguageProfile $Profile
Assert-SourceIdentity ($CaseContext.CacheKey -cne $Context.CacheKey) 'Logical identity was folded to lowercase.'
Pass-SourceIdentity 'logical identity is case sensitive'

$PublishedHashes = Get-SourceIdentityArtifactHashes $Cold.Report
$ReportHash = Get-AvidScriptBindingSha256Hex $Cold.Path
foreach ($Invalid in @('', '../bad.cs', 'Bad\Source.cs', '/rooted.cs', "Bad`nSource.cs")) {
    $Log = Join-Path $EvidenceRoot ('Invalid-' + [Guid]::NewGuid().ToString('N') + '.log')
    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File (Join-Path $PluginRoot 'Build/BuildCSharpActorLifecycle.ps1') `
        -SourceId $Invalid -OutputRoot (Split-Path -Parent $Cold.Path) *> $Log
    Assert-SourceIdentity ($LASTEXITCODE -ne 0 -and (Get-Content -Raw -LiteralPath $Log).Contains('SourceId must be a stable')) 'Invalid SourceId was not rejected at entry.'
    Assert-SourceIdentityHashes (Get-SourceIdentityArtifactHashes $Cold.Report) $PublishedHashes 'Early rejection'
    Assert-SourceIdentity ((Get-AvidScriptBindingSha256Hex $Cold.Path) -ceq $ReportHash) 'Early rejection changed the build report.'
}
Pass-SourceIdentity 'five invalid public identities preserve published artifacts'
if ($BaselineBuildReportPath) {
    Assert-SourceIdentityHashes (Get-SourceIdentityArtifactHashes $Baseline) $BaselineHashes 'Baseline preservation'
    Pass-SourceIdentity 'original runtime artifacts preserved'
}
$PersistentPathAfter = @([Environment]::GetEnvironmentVariable('PATH', 'User'), [Environment]::GetEnvironmentVariable('PATH', 'Machine'))
Assert-SourceIdentity ($PersistentPathAfter[0] -ceq $PersistentPathBefore[0] -and $PersistentPathAfter[1] -ceq $PersistentPathBefore[1]) 'Persistent PATH changed.'
Pass-SourceIdentity 'persistent PATH unchanged'
Write-SourceIdentityJson (Join-Path $EvidenceRoot 'results.json') ([ordered]@{
    passed=$Cases.Count; total=$Cases.Count; cases=$Cases.ToArray(); builds=$Builds.ToArray(); baseline_compared=[bool]$BaselineBuildReportPath;
    cold_artifact_hashes=$Cold.Hashes; process_priority='BelowNormal'; persistent_path_unchanged=$true; runtime_executed=$false
})
Write-Output "AvidScript.CSharp.SourceIdentityCache: $($Cases.Count)/$($Cases.Count) passed; evidence=$EvidenceRoot"
