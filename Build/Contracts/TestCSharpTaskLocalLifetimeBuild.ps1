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
$runRoot = Join-Path $projectRoot ('Saved/AvidScript/TaskLocalLifetimeBuild/' +
    [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-build')
$null = New-Item -ItemType Directory -Path $runRoot -Force
$sourcePath = Join-Path $runRoot 'TaskLifetime.cs'
$source = @'
using System.Runtime.InteropServices;
using System.Threading.Tasks;
public static class TaskLifetimeScript
{
    public static int Result;
    public static int Main() => 0;
    public static async Task<int> Child(int value) { return value; }
    public static async Task<int> Run()
    {
        Task<int> pending = Child(1);
        Task<int> saved = pending;
        for (int i = 0; i < 3; i++)
        {
            Task<int> iteration = Child(i);
            pending = iteration;
            int value = await iteration;
            if (value == 1) continue;
        }
        int first = await saved;
        int last = await pending;
        return first + last;
    }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay() { Result = await Run(); }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds) { }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay() { }
}
'@
[IO.File]::WriteAllText($sourcePath, $source)
$previousCliHome = $env:DOTNET_CLI_HOME
Push-Location $pluginRoot
try {
    $env:DOTNET_CLI_HOME = Join-Path ([IO.Path]::GetTempPath()) 'AvidScriptTaskLifetimeCliHome'
    $sdk = & $DotNetPath --version
    if ($LASTEXITCODE -ne 0 -or $sdk -cne '8.0.416') { throw "Expected SDK 8.0.416, got $sdk" }
    Write-Host "Task lifetime build evidence: $runRoot"
    $results = foreach ($case in @('cold', 'warm', 'semantic-only', 'prepared')) {
        Write-Host "Checking Task lifetime build: $case"
        $outputRoot = Join-Path $runRoot $case
        $arguments = @('-NoProfile', '-File', (Join-Path $pluginRoot 'Build/BuildCSharpActorLifecycle.ps1'),
            '-DotNetPath', $DotNetPath, '-SourcePath', $sourcePath, '-ProjectRoot', $projectRoot,
            '-OutputRoot', $outputRoot, '-ArtifactStem', 'task_lifetime',
            '-BindingPackagePath', $BindingPackagePath,
            '-ModuleId', 'avidscript.fixture.task_lifetime_build', '-CompilerWorkerMode', 'disabled',
            '-SemanticCacheRoot', (Join-Path $runRoot 'SemanticCache'),
            '-CompilationCacheRoot', (Join-Path $runRoot 'CompilationCache'))
        if ($case -ceq 'semantic-only') { $arguments += '-DisableCompilationCache' }
        if ($case -ceq 'prepared') {
            $arguments += @('-PreparedBuildReportPath', (Join-Path $runRoot 'cold/task_lifetime.csharp.report.json'),
                '-DisableSemanticCache', '-DisableCompilationCache')
        }
        & (Join-Path $PSHOME 'pwsh.exe') @arguments *> (Join-Path $runRoot ($case + '.log'))
        $exitCode = $LASTEXITCODE
        $report = Get-Content -LiteralPath (Join-Path $outputRoot 'task_lifetime.csharp.report.json') -Raw | ConvertFrom-Json
        if ($exitCode -ne 0 -or -not $report.succeeded) {
            throw "Task lifetime build failed in ${case}: $($report.result). See $runRoot"
        }
        $ir = Get-Content -LiteralPath (Join-Path $outputRoot 'task_lifetime.guestir.json') -Raw | ConvertFrom-Json
        if ($exitCode -ne 0 -or -not $report.succeeded -or $report.result -cne 'direct_abi_built' -or
            $report.semantic.schema_version -ne 45 -or $report.semantic.version -cne '1.54' -or
            $ir.schema_version -ne 25 -or $ir.ir_version -cne '1.24' -or
            $ir.task_local_lifetimes.exception_model -cne 'none' -or
            $ir.task_local_lifetimes.functions.Count -eq 0) {
            throw "Task lifetime contract failed in $case. See $runRoot"
        }
        if (($case -ceq 'cold' -and ($report.semantic_cache.lookup -cne 'miss' -or $report.compilation_cache.lookup -cne 'miss')) -or
            ($case -ceq 'warm' -and ($report.semantic_cache.lookup -cne 'hit' -or $report.compilation_cache.lookup -cne 'hit')) -or
            ($case -ceq 'semantic-only' -and ($report.semantic_cache.lookup -cne 'hit' -or $report.tool_invocations.guest_ir -ne 1)) -or
            ($case -ceq 'prepared' -and (-not $report.build_reuse.frontend_reused -or
                -not $report.build_reuse.semantic_reused -or $report.tool_invocations.frontend -ne 0 -or
                $report.tool_invocations.semantic -ne 0 -or $report.tool_invocations.guest_ir -ne 1 -or
                $report.tool_invocations.wasm_backend -ne 1))) {
            throw "Expected cache or prepared path was not exercised in $case. See $runRoot"
        }
        $hashes = [ordered]@{}
        foreach ($extension in @('csharp.semantic.json', 'guestir.json', 'wasm')) {
            $hashes[$extension] = (Get-FileHash -LiteralPath (Join-Path $outputRoot "task_lifetime.$extension") -Algorithm SHA256).Hash
            if ($case -cne 'cold' -and $hashes[$extension] -cne
                (Get-FileHash -LiteralPath (Join-Path $runRoot "cold/task_lifetime.$extension") -Algorithm SHA256).Hash) {
                throw "Task lifetime $extension changed between cold and $case. See $runRoot"
            }
        }
        [ordered]@{ case = $case; passed = $true; hashes = $hashes }
    }
    # Exercise the actual authorization guard against the compiled imports; a
    # binding package entry must not turn a forged Task import into a valid one.
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $pluginRoot 'Build/BuildCSharpActorLifecycle.ps1'), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -ne 0) { throw ($parseErrors | Out-String) }
    foreach ($name in @('Test-CompilerInjectedBindingImport', 'Test-BindingPackageImports')) {
        $definitions = @($ast.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
        }, $false))
        if ($definitions.Count -ne 1) { throw "Missing unique import guard: $name" }
        . ([scriptblock]::Create($definitions[0].Extent.Text))
    }
    $importChecks = 0
    $options = @{ AllowDataLaneImports = $false; AllowGeneratedTypeImports = $false;
        AllowDebugImports = $false; AllowBoundedLanguageErrors = $false; AllowDirectAwaitCleanup = $false }
    foreach ($name in @('avid_task_i32_v1', 'avid_task_bind_producer_v1',
        'avid_task_propagate_failure_v1', 'avid_task_retain_for_continuation_v1')) {
        $imports = @($ir.imports | Where-Object name -CEQ $name)
        if ($imports.Count -ne 1) { throw "Expected one compiled Task import: $name" }
        $original = $imports[0]
        if (-not (Test-CompilerInjectedBindingImport -Import $original @options)) { throw "Task import rejected: $name" }
        $importChecks++
        foreach ($field in @('id', 'name', 'module', 'dispatch_class', 'optimization_class',
            'binding_ordinal', 'parameter_type_ids', 'return_type_id')) {
            $mutated = $original | ConvertTo-Json -Depth 20 | ConvertFrom-Json
            switch ($field) {
                'id' { $mutated.id = 'import:symbol:method:global::User.Native.Call():int32' }
                'name' { $mutated.name += '_spoof' }
                'module' { $mutated.module = 'env' }
                'dispatch_class' { $mutated.dispatch_class = 'dynamic' }
                'optimization_class' { $mutated.optimization_class = 'snapshot_read' }
                'binding_ordinal' { $mutated.binding_ordinal = 0 }
                'parameter_type_ids' { $mutated.parameter_type_ids[0] = 'type:float32' }
                'return_type_id' { $mutated.return_type_id = 'type:void' }
            }
            if (Test-CompilerInjectedBindingImport -Import $mutated @options) { throw "Malformed Task import accepted: $name field=$field" }
            $importChecks++
        }
        $forged = $original | ConvertTo-Json -Depth 20 | ConvertFrom-Json
        $forged.id = 'import:symbol:method:global::User.Native.Call():int32'
        $package = [pscustomobject]@{ RequiredImports = @([pscustomobject]@{
            Module = 'avidscript'; Name = $name; StableId = 'test'; Ordinal = 0; Signature = 'test'
        }) }
        $validation = Test-BindingPackageImports -PackageInfo $package -GuestImports @($forged)
        if (@($validation.UnexpectedImports).Count -ne 1) { throw "Binding package authorized a forged Task import: $name" }
        $importChecks++
    }
    if ($importChecks -ne 40) { throw "Task import coverage changed: $importChecks" }
    [ordered]@{ schema_version = 1; passed = @($results).Count; total = 4;
        import_checks_passed = $importChecks; results = @($results) } |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8NoBOM
    Write-Host "CSharp Task local lifetime builds: $(@($results).Count)/4 passed"
    Write-Host "Task import authorization: $importChecks/40 passed"
}
finally {
    Pop-Location
    $env:DOTNET_CLI_HOME = $previousCliHome
}
