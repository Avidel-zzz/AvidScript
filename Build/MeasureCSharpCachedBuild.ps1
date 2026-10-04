param(
    [Parameter(Mandatory = $true)][string]$ProducerBuildReportPath,
    [Parameter(Mandatory = $true)][string]$GeneratedTypeManifestPath,
    [Parameter(Mandatory = $true)][string]$BindingPackagePath,
    [ValidateRange(1, 10)][int]$SampleCount = 3,
    [string]$ExistingProbeOutputRoot = '',
    [string]$DotNetPath = ''
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$RunId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$OutputRoot = Join-Path $ProjectRoot "Saved/AvidScript/CachedBuildProbe/$RunId"
$EvidenceRoot = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) "AvidScript/Verification/CachedBuildLatency/$RunId"
$Utf8 = [Text.UTF8Encoding]::new($false)
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
$env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
$env:DOTNET_NOLOGO = '1'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
$env:MSBUILDDISABLENODEREUSE = '1'
$env:DOTNET_CLI_USE_MSBUILD_SERVER = '0'
$env:UseSharedCompilation = 'false'
$env:DOTNET_CLI_HOME = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Toolchain/GameplayBuild'
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
if ([string]::IsNullOrWhiteSpace($DotNetPath)) { $DotNetPath = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.dotnet/dotnet.exe' }
$Version = & $DotNetPath --version
if ($LASTEXITCODE -ne 0 -or $Version -cne '8.0.416') { throw 'Installed SDK must be 8.0.416.' }
$PersistentPathBefore = @([Environment]::GetEnvironmentVariable('PATH', 'User'), [Environment]::GetEnvironmentVariable('PATH', 'Machine'))
. (Join-Path $PSScriptRoot 'AvidScriptCSharpBindingPackage.ps1')
$Producer = Get-Content -Raw -LiteralPath $ProducerBuildReportPath | ConvertFrom-Json
if (-not $Producer.succeeded -or $Producer.result -cne 'direct_abi_built' -or
    $Producer.source.source_id -isnot [string] -or $Producer.language_profile.name -cne 'gameplay-v1') { throw 'A verified gameplay producer with explicit source identity is required.' }
$SourcePath = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Producer.source.file
$ProjectPath = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Producer.source.project
if ((Get-AvidScriptBindingSha256Hex $SourcePath) -cne $Producer.source.sha256) { throw 'Producer source bytes are no longer current.' }
$SemanticEntry = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Producer.semantic_cache.entry_report_file
$CompilationEntry = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Producer.compilation_cache.entry_report_file
$SemanticCacheRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $SemanticEntry))
$CompilationCacheRoot = Split-Path -Parent (Split-Path -Parent $CompilationEntry)
foreach ($Entry in @($SemanticEntry, $CompilationEntry)) {
    if (-not (Test-Path -LiteralPath $Entry -PathType Leaf) -or
        -not (Test-AvidScriptBindingPathContained -RootPath (Join-Path $ProjectRoot 'Saved/AvidScript') -CandidatePath $Entry)) { throw 'Producer cache entry is missing or escapes its namespace.' }
}
$ArtifactFields = @('frontend_file', 'semantic_file', 'guest_ir_file', 'debug_map_file', 'state_schema_file', 'wasm_file')
$ExpectedHashes = [ordered]@{}
foreach ($Field in $ArtifactFields) {
    $ExpectedHashes[$Field] = Get-AvidScriptBindingSha256Hex (Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Producer.artifacts.$Field)
}
if ($ExistingProbeOutputRoot) {
    $ExistingRoot = [IO.Path]::GetFullPath($ExistingProbeOutputRoot)
    if (-not (Test-AvidScriptBindingPathContained -RootPath (Join-Path $ProjectRoot 'Saved/AvidScript/CachedBuildProbe') -CandidatePath $ExistingRoot) -or
        (Split-Path -Leaf $ExistingRoot) -cnotmatch '^\d{8}T\d{9}Z-[0-9a-f]{8}$') { throw 'Existing output must be an owned CachedBuildProbe directory.' }
    $Prior = Get-Content -Raw -LiteralPath (Join-Path $ExistingRoot 'generated_types.csharp.report.json') | ConvertFrom-Json
    $PriorRoot = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Prior.output_root
    if (-not $Prior.succeeded -or -not $PriorRoot.Equals($ExistingRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $Prior.module_id -cne $Producer.module_id -or $Prior.source.source_id -cne $Producer.source.source_id -or
        $Prior.source.sha256 -cne $Producer.source.sha256 -or $Prior.language_profile.contract_sha256 -cne $Producer.language_profile.contract_sha256) {
        throw 'Existing probe ownership or source identity differs from the producer.'
    }
    foreach ($Field in $ArtifactFields) {
        $Path = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Prior.artifacts.$Field
        if (-not (Test-AvidScriptBindingPathContained -RootPath $ExistingRoot -CandidatePath $Path) -or
            (Get-AvidScriptBindingSha256Hex $Path) -cne $ExpectedHashes[$Field]) { throw 'Existing output is not the verified producer artifact set.' }
    }
    $OutputRoot = $ExistingRoot
}
New-Item -ItemType Directory -Force -Path $OutputRoot, $EvidenceRoot | Out-Null
$InputIdentity = [ordered]@{
    producer_sha256 = Get-AvidScriptBindingSha256Hex $ProducerBuildReportPath
    source_sha256 = Get-AvidScriptBindingSha256Hex $SourcePath
    project_sha256 = Get-AvidScriptBindingSha256Hex $ProjectPath
    generated_types_sha256 = Get-AvidScriptBindingSha256Hex $GeneratedTypeManifestPath
    binding_package_sha256 = Get-AvidScriptBindingSha256Hex $BindingPackagePath
    source_id = $Producer.source.source_id
    module_id = $Producer.module_id
    language_profile = $Producer.language_profile
    artifact_hashes = $ExpectedHashes
}
$OwnedBreakpoints = [Collections.Generic.List[object]]::new()
$Samples = [Collections.Generic.List[object]]::new()
$global:AvidCachedBuildProbe = $null

function Install-AvidCachedBuildTiming {
    $Owners = [ordered]@{
        'BuildCSharpActorLifecycle.ps1' = @('Resolve-DotNetTool')
        'AvidScriptCSharpLanguageProfile.ps1' = @('Resolve-AvidScriptCSharpLanguageProfile', 'Invoke-AvidScriptCSharpProfileTool', 'Get-AvidScriptCSharpProfileToolIdentity', 'Get-AvidScriptCSharpProfileAdmission')
        'AvidScriptCSharpBindingPackage.ps1' = @('Resolve-AvidScriptCSharpBindingPackage')
        'AvidScriptCSharpSemanticCache.ps1' = @('Get-AvidScriptCSharpToolchainFingerprint', 'Get-AvidScriptCSharpSemanticCacheContext', 'Import-AvidScriptCSharpSemanticCacheEntry')
        'AvidScriptCSharpPreparedSemantic.ps1' = @('Import-AvidScriptCSharpPreparedSemantic')
        'AvidScriptCSharpCompilationCache.ps1' = @('Get-AvidScriptCompilationCacheToolchainFingerprint', 'Get-AvidScriptCSharpCompilationCacheContext', 'Import-AvidScriptCSharpCompilationCacheEntry')
    }
    foreach ($Owner in $Owners.GetEnumerator()) {
        $Path = Join-Path $PSScriptRoot $Owner.Key
        $Tokens = $null; $ParseErrors = $null
        $Ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$Tokens, [ref]$ParseErrors)
        if ($ParseErrors.Count) { throw "Cannot instrument invalid owner: $($Owner.Key)" }
        foreach ($Name in $Owner.Value) {
            $Definitions = @($Ast.FindAll({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -ceq $Name }, $true))
            if ($Definitions.Count -ne 1) { throw "Function identity is ambiguous: $Name" }
            $Definition = $Definitions[0]
            $StartLine = $Definition.Body.EndBlock.Statements[0].Extent.StartLineNumber
            $Returns = @($Definition.Body.FindAll({ param($Node)
                if ($Node -isnot [Management.Automation.Language.ReturnStatementAst]) { return $false }
                $Parent = $Node.Parent
                while ($null -ne $Parent -and $Parent -isnot [Management.Automation.Language.ScriptBlockAst]) { $Parent = $Parent.Parent }
                return [object]::ReferenceEquals($Parent, $Definition.Body)
            }, $true))
            if (-not $Returns.Count) { throw "No return boundary: $Name" }
            $BeginAction = {
                $State = $global:AvidCachedBuildProbe
                $State.NextId++
                $ParentId = if ($State.Active.Count) { $State.Active.Peek().id } else { 0 }
                $State.Active.Push([ordered]@{ id=$State.NextId; parent_id=$ParentId; name=$Name; started=[Diagnostics.Stopwatch]::GetTimestamp() })
            }.GetNewClosure()
            $EndAction = {
                $State = $global:AvidCachedBuildProbe
                if (-not $State.Active.Count -or $State.Active.Peek().name -cne $Name) { throw "Incomplete timing stack: $Name" }
                $Event = $State.Active.Pop()
                $Event.elapsed_ms = ([Diagnostics.Stopwatch]::GetTimestamp() - $Event.started) * 1000.0 / [Diagnostics.Stopwatch]::Frequency
                $Event.Remove('started')
                $State.Events.Add([pscustomobject]$Event)
            }.GetNewClosure()
            $OwnedBreakpoints.Add((Set-PSBreakpoint -Script $Path -Line $StartLine -Action $BeginAction))
            foreach ($Return in $Returns) { $OwnedBreakpoints.Add((Set-PSBreakpoint -Script $Path -Line $Return.Extent.StartLineNumber -Action $EndAction)) }
        }
    }
}
function Invoke-AvidCachedBuildSample([string]$Group, [int]$Index, [bool]$Warmup) {
    $global:AvidCachedBuildProbe = [pscustomobject]@{ NextId=0; Active=[Collections.Generic.Stack[object]]::new(); Events=[Collections.Generic.List[object]]::new() }
    $Arguments = @{
        DotNetPath=$DotNetPath; ProjectRoot=$ProjectRoot; SourcePath=$SourcePath; SourceId=[string]$Producer.source.source_id
        ProjectPath=$ProjectPath; BindingPackagePath=$BindingPackagePath; GeneratedTypeManifestPath=$GeneratedTypeManifestPath
        AllowGeneratedTypeImports=$true; ModuleId=[string]$Producer.module_id; ArtifactStem='generated_types'; LanguageProfile='gameplay-v1'
        CompilerWorkerMode='required'; CompilerWorkerIdleTimeoutSeconds=5; OutputRoot=$OutputRoot
        SemanticCacheRoot=$SemanticCacheRoot; CompilationCacheRoot=$CompilationCacheRoot
    }
    $Log = Join-Path $EvidenceRoot "$Group-$Index.log"
    $Watch = [Diagnostics.Stopwatch]::StartNew()
    & (Join-Path $PSScriptRoot 'BuildCSharpActorLifecycle.ps1') @Arguments *> $Log
    $Elapsed = $Watch.Elapsed.TotalMilliseconds
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $Log" }
    $Report = Get-Content -Raw -LiteralPath (Join-Path $OutputRoot 'generated_types.csharp.report.json') | ConvertFrom-Json
    if (-not $Report.succeeded -or $Report.semantic_cache.lookup -cne 'hit' -or $Report.compilation_cache.lookup -cne 'hit' -or
        @($Report.tool_invocations.PSObject.Properties | Where-Object Value -ne 0).Count -or
        $Report.compiler_worker.worker_started -or $Report.compiler_worker.fallback_used -or $Report.compiler_worker.request_count -ne 0) { throw 'Observed build was not a fully cached build.' }
    foreach ($Field in $ArtifactFields) {
        $Path = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Report.artifacts.$Field
        if ((Get-AvidScriptBindingSha256Hex $Path) -cne $ExpectedHashes[$Field]) { throw "Observed build changed $Field bytes." }
    }
    if ($global:AvidCachedBuildProbe.Active.Count) { throw 'Timing scopes were not completed.' }
    if ($Group -ceq 'observed' -and -not $global:AvidCachedBuildProbe.Events.Count) { throw 'No timing events were captured.' }
    $Sample = [pscustomobject]@{ group=$Group; index=$Index; warmup=$Warmup; elapsed_ms=$Elapsed;
        matched_artifacts=6; tool_invocations=$Report.tool_invocations; events=$global:AvidCachedBuildProbe.Events.ToArray() }
    $Samples.Add($Sample)
    [IO.File]::WriteAllText((Join-Path $EvidenceRoot "$Group-$Index.json"),($Sample | ConvertTo-Json -Depth 100),$Utf8)
    Write-Output "PASS $Group sample $Index warmup=$Warmup elapsed_ms=$([Math]::Round($Elapsed, 2)); six artifacts unchanged"
}
function Get-AvidCachedBuildSummary([string]$Group) {
    $Values = @($Samples | Where-Object { $_.group -ceq $Group -and -not $_.warmup } | ForEach-Object elapsed_ms | Sort-Object)
    $Middle = [int][Math]::Floor($Values.Count / 2)
    $Median = if ($Values.Count % 2) { $Values[$Middle] } else { ($Values[$Middle - 1] + $Values[$Middle]) / 2 }
    return [pscustomobject]@{ group=$Group; count=$Values.Count; median_ms=$Median; p95_ms=$Values[[int][Math]::Ceiling(0.95 * $Values.Count) - 1] }
}
try {
    foreach ($Index in 0..$SampleCount) { Invoke-AvidCachedBuildSample 'baseline' $Index ($Index -eq 0) }
    Install-AvidCachedBuildTiming
    foreach ($Index in 0..$SampleCount) { Invoke-AvidCachedBuildSample 'observed' $Index ($Index -eq 0) }
    $PersistentPathAfter = @([Environment]::GetEnvironmentVariable('PATH', 'User'), [Environment]::GetEnvironmentVariable('PATH', 'Machine'))
    if ($PersistentPathAfter[0] -cne $PersistentPathBefore[0] -or $PersistentPathAfter[1] -cne $PersistentPathBefore[1]) { throw 'Persistent PATH changed.' }
    $Result = [ordered]@{
        schema_version=1; git_commit=(git -C $PluginRoot rev-parse HEAD).Trim(); git_tree=(git -C $PluginRoot rev-parse 'HEAD^{tree}').Trim()
        diagnostic_script_sha256=Get-AvidScriptBindingSha256Hex $MyInvocation.MyCommand.Path
        sdk_version=$Version; powershell_version=[string]$PSVersionTable.PSVersion; cpu=(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfLogicalProcessors)
        process_priority='BelowNormal'; max_parallel_build_actions=1; input=$InputIdentity; samples=$Samples.ToArray()
        summaries=@((Get-AvidCachedBuildSummary 'baseline'),(Get-AvidCachedBuildSummary 'observed'))
        timing_scope='inclusive entry-to-return; nested durations overlap'; persistent_path_unchanged=$true; runtime_executed=$false
        output_reused_for_all_samples=$true; observation_diagnostic_only=$true
        output_reused_across_probe_runs=[bool]$ExistingProbeOutputRoot
        peak_working_set_mib=[Math]::Round([Diagnostics.Process]::GetCurrentProcess().PeakWorkingSet64 / 1MB, 2)
    }
    [IO.File]::WriteAllText((Join-Path $EvidenceRoot 'results.json'),($Result | ConvertTo-Json -Depth 100),$Utf8)
    Write-Output "AvidScript.CachedBuildLatency: $($Samples.Count)/$($Samples.Count) passed; evidence=$EvidenceRoot"
}
finally {
    foreach ($Breakpoint in $OwnedBreakpoints) { Remove-PSBreakpoint -Breakpoint $Breakpoint }
    $global:AvidCachedBuildProbe = $null
}
