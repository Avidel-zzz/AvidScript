param(
    [string]$DotNetPath = ""
)

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$BuildScript = Join-Path $PluginRoot "Build\BuildCSharpActorLifecycle.ps1"
$DefaultGuestCompiler = Join-Path $PluginRoot "Build\InvokeCSharpGuestCompiler.ps1"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\CompilerWorkerBuildIntegrationTests"
$CacheRoot = Join-Path $ProjectRoot "Saved\AvidScript\Tests\P61A2b2"
$PowerShellHost = Join-Path $PSHOME "pwsh.exe"

function Assert-Condition {
    param([bool]$Condition, [string]$Message)

    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-TestBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [ValidateSet("auto", "required", "disabled")][string]$Mode,
        [switch]$DisableCaches,
        [string]$GuestCompilerPath = "",
        [string]$SemanticCacheRoot = "",
        [string]$CompilationCacheRoot = ""
    )

    $OutputRoot = Join-Path $RunRoot $Name
    $Arguments = @(
        "-NoProfile",
        "-File", $BuildScript,
        "-DotNetPath", $DotNetPath,
        "-OutputRoot", $OutputRoot,
        "-CompilerWorkerMode", $Mode)
    if ($DisableCaches) {
        $Arguments += @("-DisableSemanticCache", "-DisableCompilationCache")
    }
    if (-not [string]::IsNullOrWhiteSpace($GuestCompilerPath)) {
        $Arguments += @("-GuestCompilerPath", $GuestCompilerPath)
    }
    if (-not [string]::IsNullOrWhiteSpace($SemanticCacheRoot)) {
        $Arguments += @("-SemanticCacheRoot", $SemanticCacheRoot)
    }
    if (-not [string]::IsNullOrWhiteSpace($CompilationCacheRoot)) {
        $Arguments += @("-CompilationCacheRoot", $CompilationCacheRoot)
    }

    $QuotedArguments = @($Arguments | ForEach-Object {
        $Value = [string]$_
        if ($Value.Contains(' ') -or $Value.Contains('"')) {
            '"' + $Value.Replace('"', '\"') + '"'
        }
        else {
            $Value
        }
    })
    $StdoutPath = Join-Path $RunRoot "$Name.stdout.log"
    $StderrPath = Join-Path $RunRoot "$Name.stderr.log"
    $Process = Start-Process `
        -FilePath $PowerShellHost `
        -ArgumentList $QuotedArguments `
        -WindowStyle Hidden `
        -RedirectStandardOutput $StdoutPath `
        -RedirectStandardError $StderrPath `
        -PassThru
    try {
        Assert-Condition ($Process.WaitForExit(120000)) `
            "compiler worker build '$Name' timed out"
        $ExitCode = $Process.ExitCode
    }
    finally {
        if (-not $Process.HasExited) {
            Stop-Process -Id $Process.Id -Force
            [void]$Process.WaitForExit(5000)
        }
        $Process.Dispose()
    }
    $Output = @()
    if (Test-Path -LiteralPath $StdoutPath -PathType Leaf) {
        $Output += @(Get-Content -LiteralPath $StdoutPath)
    }
    if (Test-Path -LiteralPath $StderrPath -PathType Leaf) {
        $Output += @(Get-Content -LiteralPath $StderrPath)
    }
    Assert-Condition ($ExitCode -eq 0) `
        "compiler worker build '$Name' failed with exit code ${ExitCode}: $(@($Output) -join [System.Environment]::NewLine)"
    $ReportPath = Join-Path $OutputRoot "actor_lifecycle.csharp.report.json"
    Assert-Condition (Test-Path -LiteralPath $ReportPath -PathType Leaf) `
        "compiler worker build report is missing: $ReportPath"
    return Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json
}

if ([string]::IsNullOrWhiteSpace($DotNetPath)) {
    $DotNetPath = Join-Path $env:USERPROFILE ".dotnet\dotnet.exe"
}
foreach ($RequiredFile in @($DotNetPath, $PowerShellHost, $BuildScript, $DefaultGuestCompiler)) {
    Assert-Condition (Test-Path -LiteralPath $RequiredFile -PathType Leaf) `
        "required compiler worker integration file is missing: $RequiredFile"
}
Remove-Item -LiteralPath $RunRoot -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $CacheRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null

$ColdRequired = Invoke-TestBuild -Name "Required" -Mode required -DisableCaches
$WarmRequired = Invoke-TestBuild -Name "Required" -Mode required -DisableCaches
Assert-Condition ($ColdRequired.succeeded -and $WarmRequired.succeeded) `
    "required worker builds should succeed"
Assert-Condition ($ColdRequired.compiler_worker.used -and $WarmRequired.compiler_worker.used) `
    "required builds should use the compiler worker"
Assert-Condition ([int]$ColdRequired.compiler_worker.request_count -eq 3 -and
    [int]$WarmRequired.compiler_worker.request_count -eq 3) `
    "required builds should route all three logical stages through the worker"
Assert-Condition ([string]$ColdRequired.compiler_worker.worker_instance_id -ceq
    [string]$WarmRequired.compiler_worker.worker_instance_id) `
    "consecutive builds should reuse the same worker instance"
Assert-Condition ([int]$ColdRequired.compiler_worker.worker_process_id -eq
    [int]$WarmRequired.compiler_worker.worker_process_id) `
    "consecutive builds should reuse the same worker process"
Assert-Condition ([int]$WarmRequired.compiler_worker.workspace.syntax_tree_cache_hits -gt
    [int]$ColdRequired.compiler_worker.workspace.syntax_tree_cache_hits) `
    "the warm build should record a new Roslyn syntax-tree hit"

$Disabled = Invoke-TestBuild -Name "Disabled" -Mode disabled -DisableCaches
Assert-Condition (-not $Disabled.compiler_worker.used -and
    [string]$Disabled.compiler_worker.status -ceq "disabled" -and
    [int]$Disabled.compiler_worker.request_count -eq 0) `
    "disabled mode should retain the one-shot toolchain"

$SemanticCacheRoot = Join-Path $CacheRoot "Semantic\v1"
$CompilationCacheRoot = Join-Path $CacheRoot "Compilation\v1"
$ColdCached = Invoke-TestBuild `
    -Name "Cached" `
    -Mode required `
    -SemanticCacheRoot $SemanticCacheRoot `
    -CompilationCacheRoot $CompilationCacheRoot
$WarmCached = Invoke-TestBuild `
    -Name "Cached" `
    -Mode required `
    -SemanticCacheRoot $SemanticCacheRoot `
    -CompilationCacheRoot $CompilationCacheRoot
Assert-Condition ($ColdCached.compiler_worker.used) "cold cached build should compile through the worker"
Assert-Condition (-not $WarmCached.compiler_worker.used -and
    [string]$WarmCached.compiler_worker.status -ceq "not_needed") `
    "complete cache hit should not initialize the compiler worker"
Assert-Condition ([string]$WarmCached.semantic_cache.lookup -ceq "hit" -and
    [string]$WarmCached.compilation_cache.lookup -ceq "hit") `
    "warm cached build should restore both cache levels"
Assert-Condition ([int]$WarmCached.tool_invocations.frontend -eq 0 -and
    [int]$WarmCached.tool_invocations.semantic -eq 0 -and
    [int]$WarmCached.tool_invocations.guest_ir -eq 0 -and
    [int]$WarmCached.tool_invocations.wasm_backend -eq 0) `
    "complete cache hit should invoke no compiler stages"

$CustomMarker = Join-Path $RunRoot "custom-guest.marker"
$CustomGuestCompiler = Join-Path $RunRoot "InvokeCustomGuestCompiler.ps1"
$EscapedDefaultGuestCompiler = $DefaultGuestCompiler.Replace("'", "''")
$EscapedCustomMarker = $CustomMarker.Replace("'", "''")
$CustomGuestSource = @"
param(
    [string]`$DotNetPath,
    [string]`$SemanticPath,
    [string]`$FrontendArtifactSha256,
    [string]`$GuestIrPath,
    [string]`$DebugMapPath,
    [string]`$StateSchemaPath,
    [string]`$WasmPath,
    [string]`$InspectionPath,
    [string]`$Configuration = "Release",
    [string]`$DataLaneFusion = "enabled")
[System.IO.File]::WriteAllText('$EscapedCustomMarker', 'called')
& '$EscapedDefaultGuestCompiler' @PSBoundParameters
exit `$LASTEXITCODE
"@
[System.IO.File]::WriteAllText(
    $CustomGuestCompiler,
    $CustomGuestSource,
    [System.Text.UTF8Encoding]::new($false))
$CustomGuest = Invoke-TestBuild `
    -Name "CustomGuest" `
    -Mode required `
    -DisableCaches `
    -GuestCompilerPath $CustomGuestCompiler
Assert-Condition (Test-Path -LiteralPath $CustomMarker -PathType Leaf) `
    "custom Guest compiler was bypassed"
Assert-Condition (-not $CustomGuest.compiler_worker.guest_stage_enabled -and
    [int]$CustomGuest.compiler_worker.request_count -eq 2 -and
    [int]$CustomGuest.compiler_worker.stages.guest.count -eq 0) `
    "custom Guest compiler should leave only Frontend and Semantic in the worker"

Write-Output "AvidScript compiler worker build integration tests: 4/4 passed."
