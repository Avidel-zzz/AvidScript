[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BindingPackagePath,
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$projectRoot = Split-Path -Parent (Split-Path -Parent $pluginRoot)
$BindingPackagePath = (Resolve-Path -LiteralPath $BindingPackagePath).Path
$runRoot = Join-Path $projectRoot ('Saved/AvidScriptAsyncThrowRouting/' + [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-build')
$null = New-Item -ItemType Directory -Path $runRoot -Force
$oldCliHome = $env:DOTNET_CLI_HOME
Push-Location $pluginRoot
try {
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'AvidScriptAsyncThrowCliHome'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }
    $results = foreach ($case in @('ready', 'deferred', 'disabled')) {
        $wait = if ($case -ceq 'ready') { '' } else { 'await AvidContinuations.NextTickAsync();' }
        $sourcePath = Join-Path $runRoot "$case.cs"
        $source = @"
using AvidScript;
using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
public static class AsyncThrowScript
{
    public static int Result, Trace;
    public static int Main() => 0;
    public static async Task<int> Read() { $wait return 3; }
    public static async Task<int> Run()
    {
        try { int value = await Read(); throw new ArgumentException(); }
        catch (ArgumentException) { return 39; }
        finally { Trace++; }
    }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay() { Result = await Run(); }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds) { }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay() { }
}
"@
        [IO.File]::WriteAllText($sourcePath, $source)
        $outputRoot = Join-Path $runRoot $case
        $arguments = @('-NoProfile', '-File', (Join-Path $pluginRoot 'Build/BuildCSharpActorLifecycle.ps1'),
            '-DotNetPath', $DotNetPath, '-SourcePath', $sourcePath, '-ProjectRoot', $projectRoot,
            '-OutputRoot', $outputRoot, '-ArtifactStem', 'async_throw',
            '-BindingPackagePath', $BindingPackagePath, '-ModuleId', 'avidscript.fixture.async_throw_build',
            '-CompilerWorkerMode', 'disabled', '-LanguageErrors', 'bounded', '-AsyncExceptionFlow', '-DirectAwaitCleanup')
        if ($case -cne 'disabled') { $arguments += '-AsyncCancellationFlow' }
        & (Join-Path $PSHOME 'pwsh.exe') @arguments *> (Join-Path $runRoot "$case.log")
        $exitCode = $LASTEXITCODE
        $report = Get-Content -LiteralPath (Join-Path $outputRoot 'async_throw.csharp.report.json') -Raw | ConvertFrom-Json
        $wasmPath = Join-Path $outputRoot 'async_throw.wasm'
        if ($case -ceq 'disabled') {
            if ($exitCode -eq 0 -or $report.succeeded -or (Test-Path -LiteralPath $wasmPath)) {
                throw "Routed throws compiled without the required preview: $runRoot"
            }
            [ordered]@{ case = $case; passed = $true; rejected = $report.result }
            continue
        }
        $ir = Get-Content -LiteralPath (Join-Path $outputRoot 'async_throw.guestir.json') -Raw | ConvertFrom-Json
        if ($exitCode -ne 0 -or -not $report.succeeded -or $report.result -cne 'direct_abi_built' -or
            $report.semantic.schema_version -ne 46 -or $report.semantic.version -cne '1.55' -or
            $ir.schema_version -ne 26 -or $ir.ir_version -cne '1.25' -or
            $ir.task_local_lifetimes.exception_model -cne 'cancellation' -or
            @($ir.async_exception_transfers | Where-Object { $_.kind -ceq 'raise_exception' }).Count -eq 0 -or
            -not (Test-Path -LiteralPath $wasmPath)) {
            throw "Formal async throw build failed in ${case}: $($report.result). See $runRoot"
        }
        [ordered]@{ case = $case; passed = $true; wasm_sha256 = (Get-FileHash -LiteralPath $wasmPath -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    Write-Output "Async throw formal build: 3/3 passed; evidence=$runRoot"
}
finally {
    Pop-Location
    $env:DOTNET_CLI_HOME = $oldCliHome
}
