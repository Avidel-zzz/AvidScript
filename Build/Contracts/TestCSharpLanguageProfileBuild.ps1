[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$BindingPackagePath,
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [switch]$StandardCancellationTokens,
    [switch]$ObjectAwaitCancellation)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($ObjectAwaitCancellation -and -not $StandardCancellationTokens) { throw 'Object await contract requires the standard token fixture.' }
$PluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ProjectOwner = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N')
$RunRoot = Join-Path $ProjectOwner "Saved/AvidScriptGameplayBuild/$RunId"
$ProjectRoot = Join-Path $RunRoot 'Project'
$null = New-Item -ItemType Directory -Path $ProjectRoot
$Results = [Collections.Generic.List[object]]::new()
$WorkerContext = $null
$HeldPipe = $null
$Utf8 = [Text.UTF8Encoding]::new($false)
$UserPathBefore = [Environment]::GetEnvironmentVariable('Path', 'User')
$MachinePathBefore = [Environment]::GetEnvironmentVariable('Path', 'Machine')
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
$env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
$env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
$env:DOTNET_NOLOGO = '1'
$env:MSBUILDDISABLENODEREUSE = '1'
$env:DOTNET_CLI_USE_MSBUILD_SERVER = '0'
$env:UseSharedCompilation = 'false'
$env:DOTNET_CLI_HOME = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Toolchain/GameplayBuildTests'
. (Join-Path $PluginRoot 'Build/AvidScriptCSharpSemanticCache.ps1')
. (Join-Path $PluginRoot 'Build/AvidScriptCSharpCompilationCache.ps1')
. (Join-Path $PluginRoot 'Build/AvidScriptCSharpCompilerWorker.ps1')

function Check([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "Gameplay build contract failed: $Name; evidence=$RunRoot" }
    $Results.Add([ordered]@{ name = $Name; passed = $true })
}
function WriteJson([string]$Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 100), $Utf8) }
function ReadJson([string]$Path) { Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json }
function BuildCase([string]$Name, [string]$Mode = 'required', [string[]]$Extra = @(), [bool]$Success = $true, [bool]$Profile = $true,
    [string]$RequestedModuleId = 'avidscript.fixture.gameplay_build') {
    $Output = Join-Path $ProjectRoot $Name
    $Arguments = @('-NoProfile', '-File', (Join-Path $PluginRoot 'Build/BuildCSharpActorLifecycle.ps1'),
        '-DotNetPath', $DotNetPath, '-SourcePath', $SourcePath, '-ProjectPath', $ProjectPath,
        '-ProjectRoot', $ProjectRoot, '-OutputRoot', $Output, '-ArtifactStem', 'script',
        '-BindingPackagePath', $LocalPackage, '-ModuleId', $RequestedModuleId,
        '-CompilerWorkerMode', $Mode, '-CompilerWorkerTimeoutSeconds', '2', '-CompilerWorkerIdleTimeoutSeconds', '60')
    if ($Profile) { $Arguments += @('-LanguageProfile', 'gameplay-v1') }
    $Arguments += $Extra
    & (Join-Path $PSHOME 'pwsh.exe') @Arguments *> (Join-Path $RunRoot "$Name.log")
    $Exit = $LASTEXITCODE
    $Report = ReadJson (Join-Path $Output 'script.csharp.report.json')
    WriteJson (Join-Path $RunRoot "$Name.report.evidence.json") $Report
    if ($Report.compiler_worker.used) {
        $script:WorkerContext = Get-AvidScriptCompilerWorkerContext -PluginRoot $PluginRoot -DotNetPath $DotNetPath -Configuration Release -ProjectRoot $ProjectRoot
        $script:WorkerContext.WorkerInstanceId = [string]$Report.compiler_worker.worker_instance_id
        $script:WorkerContext.WorkerProcessId = [int]$Report.compiler_worker.worker_process_id
    }
    if (($Exit -eq 0) -ne $Success -or [bool]$Report.succeeded -ne $Success) {
        Write-Output ($Report.diagnostics | ConvertTo-Json -Depth 8)
    }
    Check (($Exit -eq 0) -eq $Success -and [bool]$Report.succeeded -eq $Success) "$Name result"
    if (-not $Success) {
        Check (-not (Test-Path -LiteralPath (Join-Path $Output 'script.wasm')) -and
            -not (Test-Path -LiteralPath (Join-Path $Output 'script.avidscript.json'))) "$Name publishes no loadable module"
    }
    return $Report
}
function StopOwnedWorker {
    if ($null -eq $script:WorkerContext) { return }
    $Worker = Get-Process -Id $script:WorkerContext.WorkerProcessId -ErrorAction SilentlyContinue
    if ($null -ne $Worker) {
        $Request = New-AvidScriptCompilerWorkerRequest -Context $script:WorkerContext -Stage shutdown
        $Response = Invoke-AvidScriptCompilerWorkerRaw -Context $script:WorkerContext -Request $Request
        Check ($Response.succeeded -and [string]$Response.worker_instance_id -ceq $script:WorkerContext.WorkerInstanceId) 'owned worker shutdown identity'
        Check ($Worker.WaitForExit(10000)) 'owned worker exits'
    }
    $script:WorkerContext = $null
}

Push-Location $PluginRoot
try {
    $Sdk = & $DotNetPath --version
    Check ($LASTEXITCODE -eq 0 -and $Sdk -ceq '8.0.416') 'installed SDK'
    $Package = ReadJson $BindingPackagePath
    $PackageRoot = Split-Path -Parent (Resolve-Path -LiteralPath $BindingPackagePath).Path
    $LocalRoot = Join-Path $ProjectRoot 'Bindings'
    $null = New-Item -ItemType Directory -Path $LocalRoot
    foreach ($Name in @('package.json', [string]$Package.files.descriptor, [string]$Package.files.reference_source)) {
        Check ([IO.Path]::GetFileName($Name) -ceq $Name) "binding file containment $Name"
        Copy-Item -LiteralPath (Join-Path $PackageRoot $Name) -Destination (Join-Path $LocalRoot $Name)
    }
    $LocalPackage = Join-Path $LocalRoot 'package.json'
    if ($StandardCancellationTokens) {
        $ExpectedEmitter = if ($ObjectAwaitCancellation) { '49.6.0' } else { '49.5.0' }
        Check ($Package.emitter_version -ceq $ExpectedEmitter) 'standard token test uses its exact production emitter contract'
        $ConsumerRoot = Join-Path $ProjectRoot 'FacadeConsumer'
        $null = New-Item -ItemType Directory -Path $ConsumerRoot
        $ConsumerProject = Join-Path $ConsumerRoot 'FacadeConsumer.csproj'
        $ProjectXml = [xml]'<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework><EnableDefaultCompileItems>false</EnableDefaultCompileItems><TreatWarningsAsErrors>true</TreatWarningsAsErrors></PropertyGroup><ItemGroup><Compile Include="Consumer.cs" /></ItemGroup></Project>'
        $ReferenceItem = $ProjectXml.CreateElement('Compile')
        $ReferenceItem.SetAttribute('Include', (Join-Path $LocalRoot ([string]$Package.files.reference_source)))
        $null = $ProjectXml.Project.ItemGroup.AppendChild($ReferenceItem)
        $ProjectXml.Save($ConsumerProject)
        [IO.File]::WriteAllText((Join-Path $ConsumerRoot 'Consumer.cs'), @'
using AvidScript;
using System.Threading;
public static class Consumer {
    public static CancellationToken Convert(AvidCancellationToken value) => value;
    public static void Overloads(AvidDelayAwaitable delay, AvidObjectAwaitable load,
        AvidOutcomeAwaitable<int> outcome, AvidCancellationToken legacy, CancellationToken token) {
        _ = delay.WithCancellation(legacy); _ = delay.WithCancellation(token);
        _ = load.WithCancellation(legacy); _ = load.WithCancellation(token);
        _ = outcome.WithCancellation(legacy); _ = outcome.WithCancellation(token);
        _ = delay.WithCancellation(default); _ = load.WithCancellation(default);
        _ = outcome.WithCancellation(default);
    }
}
'@, $Utf8)
        # SDK reference packs only: no network packages, SDK installation or CLR execution.
        $EmptySource = Join-Path $ConsumerRoot 'EmptyNuGetSource'
        $null = New-Item -ItemType Directory -Path $EmptySource
        & $DotNetPath restore $ConsumerProject --source $EmptySource --disable-parallel -p:NuGetAudit=false *> (Join-Path $RunRoot 'facade-restore.log')
        Check ($LASTEXITCODE -eq 0) 'generated facade consumer restores from installed reference packs'
        & $DotNetPath build $ConsumerProject -c Release --no-restore --disable-build-servers -m:1 -nodeReuse:false -p:UseSharedCompilation=false *> (Join-Path $RunRoot 'facade-build.log')
        Check ($LASTEXITCODE -eq 0) 'real generated facade compiles conversion and all six cancellation overloads'
    }
    $SourcePath = Join-Path $ProjectRoot 'Gameplay.cs'
    $ProjectPath = Join-Path $ProjectRoot 'Gameplay.csproj'
    [IO.File]::WriteAllText($ProjectPath, '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>', $Utf8)
    $Source = @'
using AvidScript;
using System;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.InteropServices;
public static class Script {
    public static int Result;
    public static int Main() => 0;
    public static int Sync(int value) { if (value < 0) throw new ArgumentException(); return value; }
    public static async Task<int> Run() {
        var source = AvidCancellationSource.Create(); AvidCancellationToken token = source.Token;
        try { await AvidContinuations.NextTickAsync().WithCancellation(token); return Cache.Value; }
        catch (ArgumentException error) { return error == null ? 9 : 3; }
        finally { source.Release(); }
    }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")] public static void BeginPlay() { Run(); }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")] public static void Tick(float deltaSeconds) { }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")] public static void EndPlay() { }
}
public static class Cache { public static int Value; static Cache() { Value = Script.Sync(7); } }
'@
    [IO.File]::WriteAllText($SourcePath, $Source, $Utf8)
    $Cold = BuildCase cold
    Check ($Cold.semantic_cache.published -and $Cold.compilation_cache.published -and
        $Cold.compiler_worker.stages.semantic.count -eq 1 -and $Cold.compiler_worker.stages.guest.count -eq 1) 'cold build uses v2 worker and publishes both caches'
    Check ($Cold.language_profile.name -ceq 'gameplay-v1' -and -not $Cold.semantic.succeeded -and
        $Cold.language_profile_admission.semantic_succeeded -eq $Cold.semantic.succeeded) 'profile admission retains honest preview diagnostics'
    $Warm = BuildCase warm
    Check ($Warm.semantic_cache.lookup -ceq 'hit' -and $Warm.compilation_cache.lookup -ceq 'hit' -and
        $Warm.tool_invocations.frontend -eq 0 -and $Warm.tool_invocations.semantic -eq 0 -and
        $Warm.tool_invocations.guest_ir -eq 0 -and $Warm.tool_invocations.wasm_backend -eq 0 -and
        $Warm.wasm.sha256 -ceq $Cold.wasm.sha256) 'warm build reuses exact artifacts without Roslyn or backend'
    $PreparedReport = Join-Path $ProjectRoot 'cold/script.csharp.report.json'
    $Prepared = BuildCase prepared disabled @('-PreparedBuildReportPath', $PreparedReport)
    Check ($Prepared.build_reuse.frontend_reused -and $Prepared.build_reuse.semantic_reused -and
        $Prepared.wasm.sha256 -ceq $Cold.wasm.sha256) 'prepared input compiles under its exact profile'
    foreach ($Mutation in @('missing', 'name', 'hash', 'extra')) {
        $Changed = ReadJson $PreparedReport
        switch ($Mutation) {
            missing { $Changed.PSObject.Properties.Remove('language_profile') }
            name { $Changed.language_profile.name = 'gameplay-v2' }
            hash { $Changed.language_profile.contract_sha256 = '0' * 64 }
            extra { $Changed.language_profile | Add-Member -NotePropertyName unchecked -NotePropertyValue $true }
        }
        $MutantPath = Join-Path $ProjectRoot "$Mutation.report.json"
        WriteJson $MutantPath $Changed
        $Rejected = BuildCase "reject-$Mutation" disabled @('-PreparedBuildReportPath', $MutantPath) $false
        Check ($Rejected.result -ceq 'prepared_semantic_invalid') "prepared rejects $Mutation profile identity"
    }
    $LegacyReject = BuildCase legacy-reject disabled @('-PreparedBuildReportPath', $PreparedReport) $false $false
    Check ($LegacyReject.result -ceq 'prepared_semantic_invalid') 'legacy build rejects profile prepared input'
    StopOwnedWorker
    $HeldContext = Get-AvidScriptCompilerWorkerContext -PluginRoot $PluginRoot -DotNetPath $DotNetPath -Configuration Release -ProjectRoot $ProjectRoot
    $HeldPipe = [IO.Pipes.NamedPipeServerStream]::new($HeldContext.PipeName, [IO.Pipes.PipeDirection]::InOut, 1,
        [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous)
    $Timer = [Diagnostics.Stopwatch]::StartNew()
    $TimedOut = $false
    try {
        $Request = New-AvidScriptCompilerWorkerRequest -Context $HeldContext -Stage ping
        Invoke-AvidScriptCompilerWorkerRaw -Context $HeldContext -Request $Request -ResponseTimeoutMilliseconds 300 | Out-Null
    }
    catch { $TimedOut = $_.Exception.Message -like '*timed out*' }
    Check ($TimedOut -and $Timer.ElapsedMilliseconds -lt 5000) 'unresponsive IPC peer cannot block writing or reading'
    $Fallback = BuildCase fallback auto @('-DisableSemanticCache', '-DisableCompilationCache')
    $HeldPipe.Dispose(); $HeldPipe = $null
    Check ($Fallback.compiler_worker.fallback_used -and -not $Fallback.compiler_worker.used -and
        $Fallback.wasm.sha256 -ceq $Cold.wasm.sha256 -and $Fallback.guest_ir.sha256 -ceq $Cold.guest_ir.sha256) 'unavailable worker auto fallback preserves exact output'
    [IO.File]::WriteAllText($SourcePath, $Source.Replace('Script.Sync(7)', 'Script.Sync(8)'), $Utf8)
    $Modified = BuildCase modified disabled
    Check ($Modified.semantic_cache.lookup -ceq 'miss' -and $Modified.compilation_cache.lookup -ceq 'miss' -and
        $Modified.semantic_cache.key -cne $Cold.semantic_cache.key -and
        $Modified.compilation_cache.key -cne $Cold.compilation_cache.key -and $Modified.wasm.sha256 -cne $Cold.wasm.sha256) 'source edit invalidates both caches and changes executable bytes'
    $Stale = BuildCase stale disabled @('-PreparedBuildReportPath', $PreparedReport) $false
    Check ($Stale.result -ceq 'prepared_semantic_invalid') 'prepared rejects stale source bytes'
    [IO.File]::WriteAllText($SourcePath, $Source, $Utf8)
    foreach ($CacheName in @('semantic_cache', 'compilation_cache')) {
        $EntryPath = Join-Path $ProjectRoot ([string]$Cold.$CacheName.entry_report_file)
        $Entry = ReadJson $EntryPath
        $Entry.language_profile.contract_sha256 = '0' * 64
        WriteJson $EntryPath $Entry
    }
    $Repair = BuildCase repaired disabled
    Check ($Repair.semantic_cache.lookup -ceq 'rejected' -and $Repair.compilation_cache.lookup -ceq 'rejected' -and
        $Repair.wasm.sha256 -ceq $Cold.wasm.sha256) 'tampered cache profile metadata is rejected and rebuilt'
    $StandardSource = $Source.Replace('AvidCancellationToken token', 'CancellationToken token')
    [IO.File]::WriteAllText($SourcePath, $StandardSource, $Utf8)
    if ($StandardCancellationTokens) {
        $Standard = BuildCase standard-cold
        $StandardIr = ReadJson (Join-Path $ProjectRoot ([string]$Standard.artifacts.guest_ir_file))
        Check ($null -ne $StandardIr.cancellation_tokens -and
            @($StandardIr.types | Where-Object id -eq 'type:global::System.Threading.CancellationToken').Count -eq 1 -and
            $Standard.compiler_worker.stages.semantic.count -eq 1 -and $Standard.compiler_worker.stages.guest.count -eq 1) 'standard token uses typed lowering and the production worker'
        $StandardWarm = BuildCase standard-warm
        Check ($StandardWarm.semantic_cache.lookup -ceq 'hit' -and $StandardWarm.compilation_cache.lookup -ceq 'hit' -and
            $StandardWarm.tool_invocations.frontend -eq 0 -and $StandardWarm.tool_invocations.semantic -eq 0 -and
            $StandardWarm.tool_invocations.guest_ir -eq 0 -and $StandardWarm.tool_invocations.wasm_backend -eq 0 -and
            $StandardWarm.wasm.sha256 -ceq $Standard.wasm.sha256) 'standard token caches preserve executable bytes'
        $StandardPrepared = BuildCase standard-prepared disabled @('-PreparedBuildReportPath', (Join-Path $ProjectRoot 'standard-cold/script.csharp.report.json'))
        Check ($StandardPrepared.build_reuse.frontend_reused -and $StandardPrepared.build_reuse.semantic_reused -and
            $StandardPrepared.wasm.sha256 -ceq $Standard.wasm.sha256) 'standard token prepared input retains its exact profile'
        $null = BuildCase standard-legacy-reject disabled @() $false $false
        [IO.File]::WriteAllText($SourcePath, $StandardSource.Replace('AvidContinuations.NextTickAsync()',
            'AvidAssets.LoadObjectAsync("/Engine/EngineResources/DefaultTexture.DefaultTexture")'), $Utf8)
        if ($ObjectAwaitCancellation) {
            $ObjectLoad = BuildCase standard-object-composition disabled
            $ObjectIr = ReadJson (Join-Path $ProjectRoot ([string]$ObjectLoad.artifacts.guest_ir_file))
            Check ($ObjectIr.schema_version -eq 39 -and $ObjectIr.ir_version -ceq '1.38' -and
                $null -ne $ObjectIr.object_await_cancellation -and $null -ne $ObjectIr.static_storage -and
                $null -ne $ObjectIr.cancellation_tokens) 'new object contract admits the full static catch token combination'
        }
        else {
            $ObjectLoad = BuildCase standard-object-composition-reject disabled @() $false
            Check ($ObjectLoad.result -ceq 'semantic_failed' -and
                @($ObjectLoad.diagnostics | Where-Object { $_.code -ceq 'ASBI4701' -and $_.message -like '*ASCG1004*' }).Count -eq 1) 'old facade lacks the paired object import and retains owner rejection'
        }
    }
    else {
        $FacadeGap = BuildCase unsupported-facade disabled @() $false
        Check ($FacadeGap.result -ceq 'semantic_failed' -and
            @($FacadeGap.diagnostics | Where-Object code -eq 'CS0029').Count -gt 0) 'old generated facade retains compiler diagnostics and rejects publication'
    }
    $OrdinarySource = @'
using System.Runtime.InteropServices;
public static class Script {
    public static int Main() => 0;
    public static int Next() => 7;
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")] public static void BeginPlay() { int value = Next(); }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")] public static void Tick(float deltaSeconds) { }
    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")] public static void EndPlay() { }
}
'@
    [IO.File]::WriteAllText($SourcePath, $OrdinarySource, $Utf8)
    $Legacy = BuildCase ordinary-legacy disabled @() $true $false 'csharp:Gameplay.cs'
    $Profiled = BuildCase ordinary-profile disabled @() $true $true 'csharp:Gameplay.cs'
    Check ($Legacy.semantic.schema_version -eq 31 -and $Legacy.semantic.succeeded -and $Profiled.semantic.succeeded -and
        $Legacy.semantic.artifact_sha256 -ceq $Profiled.semantic.artifact_sha256 -and
        $Legacy.guest_ir.sha256 -ceq $Profiled.guest_ir.sha256 -and $Legacy.wasm.sha256 -ceq $Profiled.wasm.sha256) 'ordinary source preserves legacy Semantic Guest and WASM bytes'
    Check ($Legacy.semantic_cache.key -cne $Profiled.semantic_cache.key -and
        $Legacy.compilation_cache.key -cne $Profiled.compilation_cache.key) 'profile identities separate cache keys even when artifacts are identical'
    Check ($UserPathBefore -ceq [Environment]::GetEnvironmentVariable('Path', 'User') -and
        $MachinePathBefore -ceq [Environment]::GetEnvironmentVariable('Path', 'Machine')) 'persistent PATH unchanged'
    Write-Output "Gameplay profile build: $($Results.Count)/$($Results.Count) passed; evidence=$RunRoot"
}
finally {
    if ($null -ne $HeldPipe) { $HeldPipe.Dispose() }
    StopOwnedWorker
    WriteJson (Join-Path $RunRoot 'results.json') @($Results)
    # Only this newly created project tree belongs to this test. Keep logs and hashes.
    $Prefix = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $Items = @(Get-ChildItem -LiteralPath $ProjectRoot -Recurse -Force)
    if (@($Items | Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        -not $_.FullName.StartsWith($Prefix, [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0) { throw 'Test cleanup containment failed.' }
    $Files = @($Items | Where-Object { -not $_.PSIsContainer } | ForEach-Object {
        [ordered]@{ path = $_.FullName; length = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    WriteJson (Join-Path $RunRoot 'generated-files.json') $Files
    foreach ($File in $Files) {
        if ((Get-FileHash -LiteralPath $File.path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $File.sha256) { throw 'Test cleanup bytes changed.' }
        Remove-Item -LiteralPath $File.path -Force
    }
    foreach ($Directory in @($Items | Where-Object PSIsContainer | Sort-Object { $_.FullName.Length } -Descending)) {
        if (@(Get-ChildItem -LiteralPath $Directory.FullName -Force).Count -eq 0) { Remove-Item -LiteralPath $Directory.FullName }
    }
    if (@(Get-ChildItem -LiteralPath $ProjectRoot -Force).Count -eq 0) { Remove-Item -LiteralPath $ProjectRoot }
    Pop-Location
}
