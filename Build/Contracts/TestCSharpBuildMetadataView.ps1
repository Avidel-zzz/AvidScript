param(
    [Parameter(Mandatory = $true)][string]$ProducerBuildReportPath,
    [Parameter(Mandatory = $true)][string]$BindingPackagePath,
    [string]$DotNetPath = ''
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
. (Join-Path $PluginRoot 'Build/AvidScriptCSharpPreparedSemantic.ps1')
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
$env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
$env:DOTNET_NOLOGO = '1'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
if (-not $DotNetPath) { $DotNetPath = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.dotnet/dotnet.exe' }
if ((& $DotNetPath --version) -cne '8.0.416') { throw 'Pinned SDK mismatch.' }
$PersistentPathBefore = @([Environment]::GetEnvironmentVariable('PATH','User'),[Environment]::GetEnvironmentVariable('PATH','Machine'))
$Profile = Resolve-AvidScriptCSharpLanguageProfile -Name gameplay-v1 -PluginRoot $PluginRoot -DotNetPath $DotNetPath -Configuration Release
$Authorization = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $BindingPackagePath
$Producer = Get-Content -LiteralPath $ProducerBuildReportPath -Raw | ConvertFrom-Json
if (-not $Producer.succeeded -or $Producer.language_profile.name -cne 'gameplay-v1') { throw 'A verified gameplay producer is required.' }
$OriginalSemanticPath = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Producer.artifacts.semantic_file
$OriginalFrontendPath = Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Producer.artifacts.frontend_file
$RunId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N')
$EvidenceBase = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Verification/BuildMetadataView'
$EvidenceRoot = Join-Path $EvidenceBase $RunId
$Scratch = Join-Path $EvidenceRoot 'Scratch'
$InputRoot = Join-Path $Scratch 'Input'
$OutputRoot = Join-Path $Scratch 'Output'
$ReportPath = Join-Path $InputRoot 'producer.json'
$SemanticPath = Join-Path $InputRoot 'semantic.json'
$FrontendPath = Join-Path $InputRoot 'frontend.json'
$DestSemantic = Join-Path $OutputRoot 'semantic.json'
$DestFrontend = Join-Path $OutputRoot 'frontend.json'
$Utf8 = [Text.UTF8Encoding]::new($false)
$Cases = [Collections.Generic.List[string]]::new()
function Check([bool]$Condition,[string]$Name) { if (-not $Condition) { throw $Name }; $Cases.Add($Name) }
function Write-Report($Value) { [IO.File]::WriteAllText($ReportPath,($Value | ConvertTo-Json -Depth 100),$Utf8) }
function Restore-Input {
    Copy-Item -LiteralPath $OriginalSemanticPath -Destination $SemanticPath -Force
    $script:Report = ($Producer | ConvertTo-Json -Depth 100 | ConvertFrom-Json)
    $script:Report.output_root = $InputRoot
    $script:Report.artifacts.frontend_file = $FrontendPath
    $script:Report.artifacts.semantic_file = $SemanticPath
    Write-Report $script:Report
}
function Reject-Import([string]$Name) {
    $code = ''
    try { Import-AvidScriptCSharpPreparedSemantic @Arguments -BuildMetadataView | Out-Null }
    catch { $code = [string]$_.Exception.Data['AvidScriptCode'] }
    Check ($code -cmatch '^ASBI44') "$Name rejected before publication"
    Check ((Get-AvidScriptBindingSha256Hex $DestFrontend) -ceq $OriginalFrontendHash -and
        (Get-AvidScriptBindingSha256Hex $DestSemantic) -ceq $OriginalSemanticHash) "$Name preserved both destinations"
}

try {
    [IO.Directory]::CreateDirectory($InputRoot) | Out-Null
    [IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
    Copy-Item -LiteralPath $OriginalFrontendPath -Destination $FrontendPath
    Restore-Input
    $Arguments = @{
        PreparedReportPath=$ReportPath; ProjectRoot=$ProjectRoot
        ExpectedSourcePath=(Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Producer.source.file)
        ExpectedSourceId=$Producer.source.source_id; ExpectedAuthorizationPackage=$Authorization; ExpectedLanguageProfile=$Profile
        FrontendDestinationPath=$DestFrontend; SemanticDestinationPath=$DestSemantic
    }
    $Full = Import-AvidScriptCSharpPreparedSemantic @Arguments
    Check (-not $Full.BuildMetadataViewUsed -and $null -ne $Full.SemanticModel.PSObject.Properties['methods']) 'default reader returns full method trees'
    $OriginalFrontendHash = Get-AvidScriptBindingSha256Hex $DestFrontend
    $OriginalSemanticHash = Get-AvidScriptBindingSha256Hex $DestSemantic
    $Full = $null
    [GC]::Collect()
    $View = Import-AvidScriptCSharpPreparedSemantic @Arguments -BuildMetadataView
    Check ($View.BuildMetadataViewUsed -and $null -eq $View.SemanticModel.PSObject.Properties['methods']) 'requested view skips unused method trees'
    Check ($View.SemanticModel.source.source_id -ceq $Producer.source.source_id) 'view preserves logical source identity'
    Check ($View.ProfileAdmission.semantic_sha256 -ceq $OriginalSemanticHash) 'view identifies the complete raw semantic bytes'
    Check ((Get-AvidScriptBindingSha256Hex $DestFrontend) -ceq $OriginalFrontendHash -and
        (Get-AvidScriptBindingSha256Hex $DestSemantic) -ceq $OriginalSemanticHash) 'view publishes original byte pair'
    $View = $null
    [GC]::Collect()

    [IO.File]::WriteAllText($SemanticPath,'{broken',$Utf8)
    $Report.semantic.artifact_sha256 = Get-AvidScriptBindingSha256Hex $SemanticPath
    Write-Report $Report
    Reject-Import 'malformed semantic'
    foreach ($Case in @('future_version','forged_source_id','forged_source_hash','case_conflict','empty_key')) {
        Restore-Input
        $Node = [Text.Json.Nodes.JsonNode]::Parse([IO.File]::ReadAllText($SemanticPath))
        switch ($Case) {
            'future_version' { $Node['semantic_version'] = [Text.Json.Nodes.JsonValue]::Create('future') }
            'forged_source_id' { $Node['source']['source_id'] = [Text.Json.Nodes.JsonValue]::Create('Scripts/Forged.cs') }
            'forged_source_hash' { $Node['source']['sha256'] = [Text.Json.Nodes.JsonValue]::Create(('0' * 64)) }
            'case_conflict' { $Node['ignored_payload'] = [Text.Json.Nodes.JsonNode]::Parse('{"name":1,"NAME":2}') }
            'empty_key' { $Node['ignored_payload'] = [Text.Json.Nodes.JsonNode]::Parse('{"":1}') }
        }
        $Options = [Text.Json.JsonSerializerOptions]::new(); $Options.MaxDepth = 256
        [IO.File]::WriteAllText($SemanticPath,$Node.ToJsonString($Options),$Utf8)
        $Node = $null
        $Report.semantic.artifact_sha256 = Get-AvidScriptBindingSha256Hex $SemanticPath
        Write-Report $Report
        Reject-Import $Case
        [GC]::Collect()
    }
    Restore-Input
    $Report.source.source_id = 'Scripts/Other.cs'; Write-Report $Report
    Reject-Import 'report source identity'
    Restore-Input
    $Report.binding_authorization.package_hash = '0' * 64; Write-Report $Report
    Reject-Import 'binding authorization'
    Restore-Input
    $Helper = Join-Path $PluginRoot 'Build/AvidScriptCSharpBindingPackage.ps1'
    $Ast = [Management.Automation.Language.Parser]::ParseFile($Helper,[ref]$null,[ref]$null)
    $Definition = $Ast.Find({param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $Node.Name -ceq 'Publish-AvidScriptBindingFilePairAtomic'},$false)
    $Action = { [IO.File]::WriteAllText($SemanticPath,'{changed_after_admission}',$Utf8) }.GetNewClosure()
    $Breakpoint = Set-PSBreakpoint -Script $Helper -Line $Definition.Body.EndBlock.Statements[0].Extent.StartLineNumber -Action $Action
    try {
        $Message = ''
        try { Import-AvidScriptCSharpPreparedSemantic @Arguments -BuildMetadataView | Out-Null }
        catch { $Message = $_.Exception.Message }
        Check ($Message -clike 'Atomic pair staging differs from verified bytes:*') 'post-admission mutation rejected before commit'
        Check ((Get-AvidScriptBindingSha256Hex $DestFrontend) -ceq $OriginalFrontendHash -and
            (Get-AvidScriptBindingSha256Hex $DestSemantic) -ceq $OriginalSemanticHash) 'post-admission mutation preserved both destinations'
    }
    finally { Remove-PSBreakpoint -Breakpoint $Breakpoint }
    $PersistentPathAfter = @([Environment]::GetEnvironmentVariable('PATH','User'),[Environment]::GetEnvironmentVariable('PATH','Machine'))
    Check ($PersistentPathAfter[0] -ceq $PersistentPathBefore[0] -and $PersistentPathAfter[1] -ceq $PersistentPathBefore[1]) 'persistent PATH unchanged'
}
finally {
    $AbsoluteScratch = [IO.Path]::GetFullPath($Scratch)
    if ($AbsoluteScratch -cne [IO.Path]::GetFullPath((Join-Path (Join-Path $EvidenceBase $RunId) 'Scratch')) -or
        $RunId -cnotmatch '^\d{8}T\d{9}Z-[0-9a-f]{32}$') { throw 'Unexpected scratch cleanup target.' }
    if (Test-Path -LiteralPath $AbsoluteScratch) {
        if ((Get-Item -LiteralPath $AbsoluteScratch -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Scratch is a reparse point.' }
        $Entries = @(Get-ChildItem -LiteralPath $AbsoluteScratch -Recurse -Force)
        if (@($Entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Scratch contains an unexpected reparse point.' }
        foreach ($Entry in @($Entries | Sort-Object { $_.FullName.Length } -Descending)) {
            $AbsolutePath = [IO.Path]::GetFullPath($Entry.FullName)
            if (-not $AbsolutePath.StartsWith($AbsoluteScratch + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Scratch cleanup escaped namespace.' }
            Remove-Item -LiteralPath $AbsolutePath -Force
        }
        Remove-Item -LiteralPath $AbsoluteScratch -Force
    }
}
$Result = [ordered]@{schema_version=1;passed=$Cases.Count;total=$Cases.Count;cases=$Cases.ToArray();scratch_removed=(-not (Test-Path -LiteralPath $Scratch));producer_sha256=Get-AvidScriptBindingSha256Hex $ProducerBuildReportPath}
[IO.File]::WriteAllText((Join-Path $EvidenceRoot 'results.json'),($Result | ConvertTo-Json -Depth 8),$Utf8)
Write-Output "AvidScript.CSharpBuildMetadataView: $($Cases.Count)/$($Cases.Count) passed; evidence=$EvidenceRoot"
