param(
    [string]$DotNetPath = (Join-Path $env:USERPROFILE ".dotnet\dotnet.exe"),
    [string]$GeneratedBindingRoot = "",
    [string]$OutputReportPath = "",
    [ValidateRange(1, 20)]
    [int]$Iterations = 3,
    [switch]$EnforceBudgets,
    [double]$NoOpMedianBudgetMs = 1000.0,
    [double]$NoOpP95BudgetMs = 1500.0,
    [double]$MethodBodyMedianBudgetMs = 3000.0,
    [double]$BindingMedianBudgetMs = 3000.0,
    [double]$ToolchainMedianBudgetMs = 10000.0
)

$ErrorActionPreference = "Stop"
$BuildRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $BuildRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$BuildScript = Join-Path $BuildRoot "BuildCSharpActorLifecycle.ps1"
$DefaultGuestCompiler = Join-Path $BuildRoot "InvokeCSharpGuestCompiler.ps1"
$RunId = [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssZ") + "-" + [Guid]::NewGuid().ToString("N").Substring(0, 8)
$RunRoot = Join-Path $ProjectRoot "Saved\AvidScript\P61A3b\$RunId"
$BuildOutputRoot = Join-Path $RunRoot "BuildOutput"
$SemanticCacheRoot = Join-Path $RunRoot "SemanticCache"
$CompilationCacheRoot = Join-Path $RunRoot "CompilationCache"
$SourcePath = Join-Path $RunRoot "IncrementalMatrixScript.cs"
$BuildReportPath = Join-Path $BuildOutputRoot "incremental.csharp.report.json"
$ManifestPath = Join-Path $BuildOutputRoot "incremental.avidscript.json"
$Utf8 = [System.Text.UTF8Encoding]::new($false)

if ([string]::IsNullOrWhiteSpace($GeneratedBindingRoot)) {
    $GeneratedBindingRoot = Join-Path $ProjectRoot "Saved\AvidScriptGeneratedBindings"
}
if ([string]::IsNullOrWhiteSpace($OutputReportPath)) {
    $OutputReportPath = Join-Path $RunRoot "incremental-build-matrix.json"
}
$GeneratedBindingRoot = [System.IO.Path]::GetFullPath($GeneratedBindingRoot)
$OutputReportPath = [System.IO.Path]::GetFullPath($OutputReportPath)

. (Join-Path $BuildRoot "AvidScriptCSharpBindingPackage.ps1")

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        $Sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $Hash = $Sha256.ComputeHash($Stream)
        }
        finally {
            $Sha256.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
    }
    return [Convert]::ToHexString($Hash).ToLowerInvariant()
}

function Get-Utf8Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$Value)

    $Hash = [System.Security.Cryptography.SHA256]::HashData(
        [System.Text.Encoding]::UTF8.GetBytes($Value))
    return [Convert]::ToHexString($Hash).ToLowerInvariant()
}

function Find-BenchmarkBindingPackage {
    Assert-Condition (Test-Path -LiteralPath $GeneratedBindingRoot -PathType Container) `
        "Generated binding root is missing: $GeneratedBindingRoot"

    $Candidates = foreach ($Candidate in Get-ChildItem `
            -LiteralPath $GeneratedBindingRoot `
            -Filter "package.json" `
            -File `
            -Recurse `
            -ErrorAction SilentlyContinue) {
        try {
            $Package = Resolve-AvidScriptCSharpBindingPackage -ManifestPath $Candidate.FullName
            [pscustomobject]@{
                Path = $Candidate.FullName
                ImportCount = @($Package.RequiredImports).Count
                LastWriteTime = $Candidate.LastWriteTimeUtc
            }
        }
        catch {
            continue
        }
    }
    $Selected = $Candidates |
        Sort-Object `
            -Property @{ Expression = { $_.ImportCount }; Descending = $false },
                @{ Expression = { $_.LastWriteTime }; Descending = $true } |
        Select-Object -First 1
    Assert-Condition ($null -ne $Selected) `
        "No valid generated binding package is available for the incremental benchmark."
    return [string]$Selected.Path
}

function New-BindingVariant {
    param(
        [Parameter(Mandatory = $true)][string]$SourceManifestPath,
        [Parameter(Mandatory = $true)][string]$DestinationDirectory,
        [Parameter(Mandatory = $true)][string]$VariantId
    )

    $Manifest = Get-Content -Raw -LiteralPath $SourceManifestPath | ConvertFrom-Json
    $SourceDirectory = Split-Path -Parent $SourceManifestPath
    $SourceDescriptor = Join-Path $SourceDirectory ([string]$Manifest.files.descriptor)
    $SourceReference = Join-Path $SourceDirectory ([string]$Manifest.files.reference_source)
    $Descriptor = Get-Content -Raw -LiteralPath $SourceDescriptor | ConvertFrom-Json
    $VariantHash = Get-Utf8Sha256Hex "$([string]$Manifest.package_hash)|$VariantId"
    $Manifest.package_hash = $VariantHash
    $Descriptor.package_hash = $VariantHash

    $DescriptorPath = Join-Path $DestinationDirectory ([string]$Manifest.files.descriptor)
    $ReferencePath = Join-Path $DestinationDirectory ([string]$Manifest.files.reference_source)
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $DescriptorPath)) | Out-Null
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $ReferencePath)) | Out-Null
    [System.IO.File]::WriteAllText(
        $DescriptorPath,
        ($Descriptor | ConvertTo-Json -Depth 100),
        $Utf8)
    Copy-Item -LiteralPath $SourceReference -Destination $ReferencePath -Force
    $Manifest.descriptor_sha256 = Get-Sha256Hex $DescriptorPath
    $Manifest.reference_source_sha256 = Get-Sha256Hex $ReferencePath
    $VariantManifestPath = Join-Path $DestinationDirectory "package.json"
    [System.IO.File]::WriteAllText(
        $VariantManifestPath,
        ($Manifest | ConvertTo-Json -Depth 32),
        $Utf8)
    [void](Resolve-AvidScriptCSharpBindingPackage -ManifestPath $VariantManifestPath)
    return $VariantManifestPath
}

function Write-LifecycleSource {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$BodyValue
    )

    $Source = @"
using System.Runtime.InteropServices;

namespace AvidScript;

public static class IncrementalMatrixScript
{
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        int bodyValue = $BodyValue;
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds) {}

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay() {}

    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
    public static void OnTimer(int callbackId, int timerHandle) {}

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int eventId, float value) {}

    [UnmanagedCallersOnly(EntryPoint = "avid_on_gameplay_event")]
    public static void OnGameplayEvent(
        int eventType, int primaryId, int secondaryId, int objectSlot,
        int objectGeneration, float x, float y, float z) {}
}
"@
    [System.IO.File]::WriteAllText($Path, $Source, $Utf8)
}

function Write-GuestCompilerWrapper {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$VariantId
    )

    $EscapedCompilerPath = $DefaultGuestCompiler.Replace("'", "''")
    $Wrapper = @'
param(
    [Parameter(Mandatory = $true)][string]$DotNetPath,
    [Parameter(Mandatory = $true)][string]$SemanticPath,
    [Parameter(Mandatory = $true)][string]$FrontendArtifactSha256,
    [Parameter(Mandatory = $true)][string]$GuestIrPath,
    [Parameter(Mandatory = $true)][string]$DebugMapPath,
    [Parameter(Mandatory = $true)][string]$StateSchemaPath,
    [Parameter(Mandatory = $true)][string]$WasmPath,
    [Parameter(Mandatory = $true)][string]$InspectionPath,
    [string]$Configuration = "Release",
    [ValidateSet("enabled", "disabled")]
    [string]$DataLaneFusion = "enabled"
)

& '__REAL_COMPILER__' @PSBoundParameters
exit $LASTEXITCODE
# fingerprint: __VARIANT_ID__
'@
    $Wrapper = $Wrapper.Replace("__REAL_COMPILER__", $EscapedCompilerPath)
    $Wrapper = $Wrapper.Replace("__VARIANT_ID__", $VariantId)
    [System.IO.File]::WriteAllText($Path, $Wrapper, $Utf8)
}

function Invoke-BenchmarkBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Scenario,
        [Parameter(Mandatory = $true)][int]$Iteration,
        [Parameter(Mandatory = $true)][string]$BindingManifestPath,
        [string]$GuestCompilerPath = ""
    )

    $Arguments = @{
        DotNetPath = $DotNetPath
        OutputRoot = $BuildOutputRoot
        SourcePath = $SourcePath
        BindingPackagePath = $BindingManifestPath
        RuntimeBindingPackagePath = $BindingManifestPath
        ModuleId = "p61_incremental_matrix"
        ArtifactStem = "incremental_matrix"
        ReportPath = $BuildReportPath
        ManifestPath = $ManifestPath
        SemanticCacheRoot = $SemanticCacheRoot
        CompilationCacheRoot = $CompilationCacheRoot
        CompilerWorkerMode = "required"
        CompilerWorkerTimeoutSeconds = 60
        CompilerWorkerIdleTimeoutSeconds = 600
    }
    if (-not [string]::IsNullOrWhiteSpace($GuestCompilerPath)) {
        $Arguments.GuestCompilerPath = $GuestCompilerPath
    }

    $Stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    & $BuildScript @Arguments | Out-Null
    $ExitCode = $LASTEXITCODE
    $Stopwatch.Stop()
    Assert-Condition ($ExitCode -eq 0) `
        "$Scenario iteration $Iteration failed with exit code $ExitCode."
    Assert-Condition (Test-Path -LiteralPath $BuildReportPath -PathType Leaf) `
        "$Scenario iteration $Iteration did not publish a build report."
    $BuildReport = Get-Content -Raw -LiteralPath $BuildReportPath | ConvertFrom-Json
    Assert-Condition ([bool]$BuildReport.succeeded) `
        "$Scenario iteration $Iteration published a failed build report."

    return [ordered]@{
        scenario = $Scenario
        iteration = $Iteration
        duration_ms = [Math]::Round($Stopwatch.Elapsed.TotalMilliseconds, 3)
        semantic_cache_lookup = [string]$BuildReport.semantic_cache.lookup
        compilation_cache_lookup = [string]$BuildReport.compilation_cache.lookup
        tool_invocations = [ordered]@{
            frontend = [int]$BuildReport.tool_invocations.frontend
            semantic = [int]$BuildReport.tool_invocations.semantic
            guest_ir = [int]$BuildReport.tool_invocations.guest_ir
            wasm_backend = [int]$BuildReport.tool_invocations.wasm_backend
        }
        worker = [ordered]@{
            used = [bool]$BuildReport.compiler_worker.used
            started = [bool]$BuildReport.compiler_worker.worker_started
            request_count = [int]$BuildReport.compiler_worker.request_count
            duration_ms = [Math]::Round([double]$BuildReport.compiler_worker.total_duration_ms, 3)
        }
    }
}

function Assert-InvocationCounts {
    param(
        [Parameter(Mandatory = $true)][object]$Measurement,
        [Parameter(Mandatory = $true)][int]$Frontend,
        [Parameter(Mandatory = $true)][int]$Semantic,
        [Parameter(Mandatory = $true)][int]$GuestIr,
        [Parameter(Mandatory = $true)][int]$WasmBackend
    )

    $ExpectedCounts = @{
        frontend = $Frontend
        semantic = $Semantic
        guest_ir = $GuestIr
        wasm_backend = $WasmBackend
    }
    foreach ($Stage in @("frontend", "semantic", "guest_ir", "wasm_backend")) {
        $Expected = [int]$ExpectedCounts[$Stage]
        Assert-Condition (
            [int]$Measurement.tool_invocations[$Stage] -eq $Expected) `
            "$($Measurement.scenario) iteration $($Measurement.iteration) expected $Stage=$Expected, actual=$($Measurement.tool_invocations[$Stage])."
    }
}

function Get-ScenarioSummary {
    param(
        [Parameter(Mandatory = $true)][string]$Scenario,
        [Parameter(Mandatory = $true)][object[]]$Measurements
    )

    $Durations = @($Measurements | ForEach-Object { [double]$_.duration_ms } | Sort-Object)
    $Middle = [int][Math]::Floor($Durations.Count / 2)
    $Median = if (($Durations.Count % 2) -eq 0) {
        ($Durations[$Middle - 1] + $Durations[$Middle]) / 2.0
    }
    else {
        $Durations[$Middle]
    }
    $P95Index = [Math]::Max(0, [int][Math]::Ceiling($Durations.Count * 0.95) - 1)
    return [ordered]@{
        scenario = $Scenario
        count = $Durations.Count
        min_ms = [Math]::Round($Durations[0], 3)
        median_ms = [Math]::Round($Median, 3)
        p95_ms = [Math]::Round($Durations[$P95Index], 3)
        max_ms = [Math]::Round($Durations[-1], 3)
    }
}

Assert-Condition (Test-Path -LiteralPath $DotNetPath -PathType Leaf) `
    "dotnet executable is missing: $DotNetPath"
Assert-Condition (Test-Path -LiteralPath $BuildScript -PathType Leaf) `
    "C# build script is missing: $BuildScript"
[System.IO.Directory]::CreateDirectory($RunRoot) | Out-Null
[System.IO.Directory]::CreateDirectory($BuildOutputRoot) | Out-Null
$BindingPackagePath = Find-BenchmarkBindingPackage
$Measurements = [System.Collections.Generic.List[object]]::new()

Write-LifecycleSource -Path $SourcePath -BodyValue 100
[void](Invoke-BenchmarkBuild `
    -Scenario "cold_prime" `
    -Iteration 0 `
    -BindingManifestPath $BindingPackagePath)

for ($Iteration = 1; $Iteration -le $Iterations; ++$Iteration) {
    $Measurement = Invoke-BenchmarkBuild `
        -Scenario "no_op" `
        -Iteration $Iteration `
        -BindingManifestPath $BindingPackagePath
    Assert-Condition ($Measurement.compilation_cache_lookup -ceq "hit") `
        "no_op must restore the complete compilation cache."
    Assert-InvocationCounts $Measurement 0 0 0 0
    $Measurements.Add($Measurement)
}

for ($Iteration = 1; $Iteration -le $Iterations; ++$Iteration) {
    Write-LifecycleSource -Path $SourcePath -BodyValue (200 + $Iteration)
    $Measurement = Invoke-BenchmarkBuild `
        -Scenario "method_body_change" `
        -Iteration $Iteration `
        -BindingManifestPath $BindingPackagePath
    Assert-Condition ($Measurement.semantic_cache_lookup -ceq "miss") `
        "method_body_change must invalidate semantic cache."
    Assert-Condition ($Measurement.compilation_cache_lookup -ceq "miss") `
        "method_body_change must invalidate compilation cache."
    Assert-InvocationCounts $Measurement 1 1 1 1
    $Measurements.Add($Measurement)
}

Write-LifecycleSource -Path $SourcePath -BodyValue 300
[void](Invoke-BenchmarkBuild `
    -Scenario "binding_prime" `
    -Iteration 0 `
    -BindingManifestPath $BindingPackagePath)
for ($Iteration = 1; $Iteration -le $Iterations; ++$Iteration) {
    $VariantDirectory = Join-Path $RunRoot "BindingVariants\$Iteration"
    $BindingVariantPath = New-BindingVariant `
        -SourceManifestPath $BindingPackagePath `
        -DestinationDirectory $VariantDirectory `
        -VariantId "binding-$Iteration"
    $Measurement = Invoke-BenchmarkBuild `
        -Scenario "binding_change" `
        -Iteration $Iteration `
        -BindingManifestPath $BindingVariantPath
    Assert-Condition ($Measurement.semantic_cache_lookup -ceq "miss") `
        "binding_change must invalidate semantic cache."
    Assert-Condition ($Measurement.compilation_cache_lookup -ceq "miss") `
        "binding_change must invalidate compilation cache."
    Assert-InvocationCounts $Measurement 1 1 1 1
    $Measurements.Add($Measurement)
}

Write-LifecycleSource -Path $SourcePath -BodyValue 400
[void](Invoke-BenchmarkBuild `
    -Scenario "toolchain_prime" `
    -Iteration 0 `
    -BindingManifestPath $BindingPackagePath)
for ($Iteration = 1; $Iteration -le $Iterations; ++$Iteration) {
    $GuestWrapperPath = Join-Path $RunRoot "GuestCompiler-$Iteration.ps1"
    Write-GuestCompilerWrapper -Path $GuestWrapperPath -VariantId "toolchain-$Iteration"
    $Measurement = Invoke-BenchmarkBuild `
        -Scenario "toolchain_change" `
        -Iteration $Iteration `
        -BindingManifestPath $BindingPackagePath `
        -GuestCompilerPath $GuestWrapperPath
    Assert-Condition ($Measurement.semantic_cache_lookup -ceq "hit") `
        "toolchain_change must preserve the semantic cache."
    Assert-Condition ($Measurement.compilation_cache_lookup -ceq "miss") `
        "toolchain_change must invalidate the compilation cache."
    Assert-InvocationCounts $Measurement 0 0 1 1
    $Measurements.Add($Measurement)
}

$Summaries = @(
    Get-ScenarioSummary "no_op" @($Measurements | Where-Object { $_.scenario -ceq "no_op" })
    Get-ScenarioSummary "method_body_change" @($Measurements | Where-Object { $_.scenario -ceq "method_body_change" })
    Get-ScenarioSummary "binding_change" @($Measurements | Where-Object { $_.scenario -ceq "binding_change" })
    Get-ScenarioSummary "toolchain_change" @($Measurements | Where-Object { $_.scenario -ceq "toolchain_change" })
)
$SummaryByScenario = @{}
foreach ($Summary in $Summaries) {
    $SummaryByScenario[$Summary.scenario] = $Summary
}
$BudgetViolations = [System.Collections.Generic.List[string]]::new()
if ([double]$SummaryByScenario.no_op.median_ms -gt $NoOpMedianBudgetMs) {
    $BudgetViolations.Add("no_op median exceeds $NoOpMedianBudgetMs ms")
}
if ([double]$SummaryByScenario.no_op.p95_ms -gt $NoOpP95BudgetMs) {
    $BudgetViolations.Add("no_op p95 exceeds $NoOpP95BudgetMs ms")
}
if ([double]$SummaryByScenario.method_body_change.median_ms -gt $MethodBodyMedianBudgetMs) {
    $BudgetViolations.Add("method_body_change median exceeds $MethodBodyMedianBudgetMs ms")
}
if ([double]$SummaryByScenario.binding_change.median_ms -gt $BindingMedianBudgetMs) {
    $BudgetViolations.Add("binding_change median exceeds $BindingMedianBudgetMs ms")
}
if ([double]$SummaryByScenario.toolchain_change.median_ms -gt $ToolchainMedianBudgetMs) {
    $BudgetViolations.Add("toolchain_change median exceeds $ToolchainMedianBudgetMs ms")
}

$GitCommit = (& git -C $PluginRoot rev-parse HEAD).Trim()
$Result = [ordered]@{
    schema_version = 1
    phase = "P61.A3b"
    generated_utc = [DateTime]::UtcNow.ToString("O")
    git_commit = $GitCommit
    configuration = "Release"
    platform = [System.Runtime.InteropServices.RuntimeInformation]::OSDescription
    architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    iterations = $Iterations
    correctness_gate = "passed"
    budgets = [ordered]@{
        enforced = [bool]$EnforceBudgets
        no_op_median_ms = $NoOpMedianBudgetMs
        no_op_p95_ms = $NoOpP95BudgetMs
        method_body_median_ms = $MethodBodyMedianBudgetMs
        binding_median_ms = $BindingMedianBudgetMs
        toolchain_median_ms = $ToolchainMedianBudgetMs
        passed = $BudgetViolations.Count -eq 0
        violations = @($BudgetViolations)
    }
    summaries = $Summaries
    measurements = @($Measurements)
}
[System.IO.Directory]::CreateDirectory((Split-Path -Parent $OutputReportPath)) | Out-Null
[System.IO.File]::WriteAllText(
    $OutputReportPath,
    ($Result | ConvertTo-Json -Depth 32),
    $Utf8)

Write-Output "AvidScript P61.A3b incremental matrix: report=$OutputReportPath"
foreach ($Summary in $Summaries) {
    Write-Output ("{0}: median={1} ms p95={2} ms" -f `
        $Summary.scenario, $Summary.median_ms, $Summary.p95_ms)
}
if ($EnforceBudgets -and $BudgetViolations.Count -gt 0) {
    foreach ($Violation in $BudgetViolations) {
        Write-Error $Violation
    }
    exit 1
}
exit 0
