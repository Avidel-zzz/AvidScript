param(
    [string]$DotNetPath = ""
)

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$BuildScript = Join-Path $PluginRoot "Build\BuildCSharpActorLifecycle.ps1"
$SourcePath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\ActorLifecycleScript.cs"
$ProjectPath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\AvidScript.ActorLifecycle.csproj"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\P61CompilationCacheBuildIntegration"
$CacheParent = Join-Path $ProjectRoot "Saved\AvidScript\Tests\P61\CompilationCacheBuildIntegration"
$SemanticCacheRoot = Join-Path $CacheParent "SemanticCache\v1"
$CompilationCacheRoot = Join-Path $CacheParent "CompilationCache\v1"

function Assert-Condition {
    param([bool]$Condition, [string]$Message)

    if (-not $Condition) {
        throw $Message
    }
}

function Resolve-ProjectPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $Path))
}

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Invoke-CompilationCacheBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [ValidateSet("enabled", "disabled")][string]$DataLaneFusion = "enabled",
        [ValidateSet("enabled", "disabled")][string]$DebugInstrumentation = "disabled",
        [switch]$DisableCompilationCache
    )

    $OutputRoot = Join-Path $RunRoot $Name
    $ReportPath = Join-Path $OutputRoot "module.csharp.report.json"
    $ManifestPath = Join-Path $OutputRoot "module.avidscript.json"
    $Arguments = @{
        DotNetPath = $DotNetPath
        OutputRoot = $OutputRoot
        SourcePath = $SourcePath
        ProjectPath = $ProjectPath
        ModuleId = "p61_compilation_cache"
        ArtifactStem = "module"
        ReportPath = $ReportPath
        ManifestPath = $ManifestPath
        SemanticCacheRoot = $SemanticCacheRoot
        CompilationCacheRoot = $CompilationCacheRoot
        DataLaneFusion = $DataLaneFusion
        DebugInstrumentation = $DebugInstrumentation
    }
    if ($DisableCompilationCache) {
        $Arguments.DisableCompilationCache = $true
    }

    & $BuildScript @Arguments | Out-Null
    $ExitCode = $LASTEXITCODE
    $Report = if (Test-Path -LiteralPath $ReportPath -PathType Leaf) {
        Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json
    }
    else {
        $null
    }
    return [pscustomobject]@{
        ExitCode = $ExitCode
        ReportPath = $ReportPath
        ManifestPath = $ManifestPath
        Report = $Report
    }
}

if ([string]::IsNullOrWhiteSpace($DotNetPath)) {
    $DotNetPath = Join-Path $env:USERPROFILE ".dotnet\dotnet.exe"
}
Assert-Condition (Test-Path -LiteralPath $DotNetPath -PathType Leaf) ".NET executable is missing: $DotNetPath"
Remove-Item -LiteralPath $RunRoot -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $CacheParent -Recurse -Force -ErrorAction SilentlyContinue

$Cold = Invoke-CompilationCacheBuild -Name "Cold"
Assert-Condition ($Cold.ExitCode -eq 0 -and $Cold.Report.succeeded) "cold compilation-cache build failed"
Assert-Condition ($Cold.Report.semantic_cache.lookup -ceq "miss") "cold semantic cache did not miss"
Assert-Condition ($Cold.Report.compilation_cache.lookup -ceq "miss") "cold compilation cache did not miss"
Assert-Condition ([bool]$Cold.Report.compilation_cache.published) "cold compilation cache was not published"
Assert-Condition (
    [int]$Cold.Report.tool_invocations.frontend -eq 1 -and
    [int]$Cold.Report.tool_invocations.semantic -eq 1 -and
    [int]$Cold.Report.tool_invocations.guest_ir -eq 1 -and
    [int]$Cold.Report.tool_invocations.wasm_backend -eq 1) `
    "cold compilation-cache invocation counts differ"

$Warm = Invoke-CompilationCacheBuild -Name "Warm"
Assert-Condition ($Warm.ExitCode -eq 0 -and $Warm.Report.succeeded) "warm compilation-cache build failed"
Assert-Condition ($Warm.Report.semantic_cache.lookup -ceq "hit") "warm semantic cache did not hit"
Assert-Condition ($Warm.Report.compilation_cache.lookup -ceq "hit") "warm compilation cache did not hit"
Assert-Condition (-not [bool]$Warm.Report.compilation_cache.published) "warm compilation cache republished its entry"
Assert-Condition (
    [int]$Warm.Report.tool_invocations.frontend -eq 0 -and
    [int]$Warm.Report.tool_invocations.semantic -eq 0 -and
    [int]$Warm.Report.tool_invocations.guest_ir -eq 0 -and
    [int]$Warm.Report.tool_invocations.wasm_backend -eq 0) `
    "warm compilation-cache build invoked a compiler stage"
Assert-Condition (
    [bool]$Warm.Report.build_reuse.frontend_reused -and
    [bool]$Warm.Report.build_reuse.semantic_reused -and
    [bool]$Warm.Report.build_reuse.guest_ir_reused -and
    [bool]$Warm.Report.build_reuse.debug_map_reused -and
    [bool]$Warm.Report.build_reuse.state_schema_reused -and
    [bool]$Warm.Report.build_reuse.wasm_reused) `
    "warm compilation-cache build did not report complete artifact reuse"
Assert-Condition (
    [string]$Warm.Report.guest_ir.sha256 -ceq [string]$Cold.Report.guest_ir.sha256 -and
    [string]$Warm.Report.debug_map.sha256 -ceq [string]$Cold.Report.debug_map.sha256 -and
    [string]$Warm.Report.wasm.sha256 -ceq [string]$Cold.Report.wasm.sha256) `
    "warm compilation-cache artifacts differ from the cold build"

$EntryReportPath = Resolve-ProjectPath ([string]$Warm.Report.compilation_cache.entry_report_file)
$Entry = Get-Content -Raw -LiteralPath $EntryReportPath | ConvertFrom-Json
$EntryWasmPath = Join-Path (Split-Path -Parent $EntryReportPath) ([string]$Entry.artifacts.wasm.file)
[System.IO.File]::WriteAllBytes($EntryWasmPath, [byte[]]@(0, 97, 115, 109))
$Recovered = Invoke-CompilationCacheBuild -Name "Recovered"
Assert-Condition ($Recovered.ExitCode -eq 0 -and $Recovered.Report.succeeded) "corrupt compilation-cache recovery failed"
Assert-Condition ($Recovered.Report.compilation_cache.lookup -ceq "rejected") "corrupt compilation cache was not rejected"
Assert-Condition ($Recovered.Report.compilation_cache.diagnostic_code -ceq "ASBI4602") "corrupt cache diagnostic code differs"
Assert-Condition ([bool]$Recovered.Report.compilation_cache.published) "recovered build did not replace the rejected entry"
Assert-Condition (
    [int]$Recovered.Report.tool_invocations.frontend -eq 0 -and
    [int]$Recovered.Report.tool_invocations.semantic -eq 0 -and
    [int]$Recovered.Report.tool_invocations.guest_ir -eq 1 -and
    [int]$Recovered.Report.tool_invocations.wasm_backend -eq 1) `
    "corrupt cache recovery invocation counts differ"
Assert-Condition (
    (Get-Sha256Hex (Resolve-ProjectPath ([string]$Recovered.Report.artifacts.wasm_file))) -ceq
        [string]$Recovered.Report.wasm.sha256) `
    "recovered WASM hash differs from the report"
Assert-Condition (
    @(Get-ChildItem -LiteralPath (Join-Path $CompilationCacheRoot ".corrupt") -Directory).Count -eq 1) `
    "rejected compilation-cache entry was not isolated"

$Invalidated = Invoke-CompilationCacheBuild -Name "Invalidated" -DataLaneFusion "disabled"
Assert-Condition ($Invalidated.ExitCode -eq 0 -and $Invalidated.Report.succeeded) "cache invalidation build failed"
Assert-Condition ($Invalidated.Report.compilation_cache.lookup -ceq "miss") "data-lane change did not invalidate compilation cache"
Assert-Condition (
    [string]$Invalidated.Report.compilation_cache.key -cne [string]$Recovered.Report.compilation_cache.key) `
    "data-lane change retained the previous compilation cache key"

$DebugInvalidated = Invoke-CompilationCacheBuild `
    -Name "DebugInvalidated" `
    -DataLaneFusion "disabled" `
    -DebugInstrumentation "enabled"
Assert-Condition ($DebugInvalidated.ExitCode -eq 0 -and $DebugInvalidated.Report.succeeded) `
    "debug instrumentation cache invalidation build failed"
Assert-Condition ($DebugInvalidated.Report.compilation_cache.lookup -ceq "miss") `
    "debug instrumentation change did not invalidate compilation cache"
Assert-Condition (
    [string]$DebugInvalidated.Report.compilation_cache.key -cne
        [string]$Invalidated.Report.compilation_cache.key) `
    "debug instrumentation change retained the previous compilation cache key"
Assert-Condition (
    [string]$DebugInvalidated.Report.compilation.debug_instrumentation -ceq "enabled") `
    "debug instrumentation mode was not reported"
$DebugGuestIr = Get-Content -Raw -LiteralPath (
    Resolve-ProjectPath ([string]$DebugInvalidated.Report.artifacts.guest_ir_file)) | ConvertFrom-Json
Assert-Condition (@($DebugGuestIr.imports | Where-Object {
    [string]$_.module -ceq "avidscript" -and
    [string]$_.name -ceq "avid_debug_probe"
}).Count -eq 1) "debug instrumentation build did not publish the probe import"

$Disabled = Invoke-CompilationCacheBuild -Name "Disabled" -DisableCompilationCache
Assert-Condition ($Disabled.ExitCode -eq 0 -and $Disabled.Report.succeeded) "disabled compilation-cache build failed"
Assert-Condition ($Disabled.Report.compilation_cache.lookup -ceq "disabled") "disabled compilation cache reported another state"
Assert-Condition (
    [int]$Disabled.Report.tool_invocations.guest_ir -eq 1 -and
    [int]$Disabled.Report.tool_invocations.wasm_backend -eq 1) `
    "disabled compilation cache skipped compiler stages"

Write-Output "AvidScript C# compilation cache build integration tests passed."
