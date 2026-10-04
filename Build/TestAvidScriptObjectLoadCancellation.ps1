[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$DotNetPath = (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'),
    [switch]$SkipBuild,
    [switch]$ExecutionOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectOwner = Split-Path -Parent (Split-Path -Parent $PluginRoot)
$ProjectPath = Join-Path $ProjectOwner 'AvidTPSTemplate.uproject'
$RunId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N')
$RunRoot = Join-Path $ProjectOwner "Saved/AvidScriptObjectCancellation/$RunId"
$Workspace = Join-Path $RunRoot 'Workspace'
$null = New-Item -ItemType Directory -Path $Workspace
$Utf8 = [Text.UTF8Encoding]::new($false)
$Checks = [Collections.Generic.List[object]]::new()
$WorkerContext = $null
$UserPathBefore = [Environment]::GetEnvironmentVariable('Path', 'User')
$MachinePathBefore = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$PriorEnvironment = @{}
foreach ($Name in @('DOTNET_CLI_HOME', 'DOTNET_ADD_GLOBAL_TOOLS_TO_PATH', 'DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK',
    'DOTNET_GENERATE_ASPNET_CERTIFICATE', 'DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE', 'DOTNET_NOLOGO',
    'MSBUILDDISABLENODEREUSE', 'DOTNET_CLI_USE_MSBUILD_SERVER', 'UseSharedCompilation', 'AVIDSCRIPT_OBJECT_CANCELLATION_DIR')) {
    $PriorEnvironment[$Name] = [Environment]::GetEnvironmentVariable($Name, 'Process')
}
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
$env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
$env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
$env:DOTNET_NOLOGO = '1'
$env:MSBUILDDISABLENODEREUSE = '1'
$env:DOTNET_CLI_USE_MSBUILD_SERVER = '0'
$env:UseSharedCompilation = 'false'
$env:DOTNET_CLI_HOME = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Toolchain/ObjectCancellationTests'
. (Join-Path $PluginRoot 'Build/AvidScriptCSharpCompilerWorker.ps1')

function Check([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "Object cancellation verification failed: $Name; evidence=$RunRoot" }
    $Checks.Add([ordered]@{ name = $Name; passed = $true })
}
function WriteJson([string]$Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 100), $Utf8) }
function ReadJson([string]$Path) { Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json }
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function InvokeEditor([string[]]$Arguments, [string]$Log) {
    $Info = [Diagnostics.ProcessStartInfo]::new((Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'))
    $Info.UseShellExecute = $false
    $Info.CreateNoWindow = $true
    $Info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $Info.RedirectStandardOutput = $true
    $Info.RedirectStandardError = $true
    foreach ($Argument in @($ProjectPath, '-unattended', '-nop4', '-NullRHI', '-nosplash', '-Multiprocess', "-abslog=$Log") + $Arguments) {
        $Info.ArgumentList.Add($Argument)
    }
    $Process = [Diagnostics.Process]::Start($Info)
    try {
        $Process.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
        $Stdout = $Process.StandardOutput.ReadToEndAsync()
        $Stderr = $Process.StandardError.ReadToEndAsync()
        $Process.WaitForExit()
        [IO.File]::WriteAllText("$Log.console", $Stdout.GetAwaiter().GetResult() + $Stderr.GetAwaiter().GetResult(), $Utf8)
        WriteJson "$Log.process.json" ([ordered]@{ process_id = $Process.Id; exit_code = $Process.ExitCode; exited = $Process.HasExited })
        Check ($Process.ExitCode -eq 0) "owned EditorCmd exits: $([IO.Path]::GetFileName($Log))"
    }
    finally { $Process.Dispose() }
}
function BuildCase([string]$Name, [string]$Mode, [string[]]$Extra = @(), [bool]$Profile = $true, [bool]$Success = $true) {
    $Output = Join-Path $Workspace $Name
    # Export the manifest at the bundle root so its project-relative artifact
    # paths are also relative to the manifest when loaded by the real Runtime.
    $ManifestPath = Join-Path $Workspace "$Name.avidscript.json"
    $Arguments = @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'BuildCSharpActorLifecycle.ps1'),
        '-DotNetPath', $DotNetPath, '-SourcePath', $SourcePath, '-ProjectPath', $SourceProject,
        '-ProjectRoot', $Workspace, '-OutputRoot', $Output, '-ArtifactStem', 'flow',
        '-ManifestPath', $ManifestPath,
        '-BindingPackagePath', $BindingPackage, '-ModuleId', 'avidscript.fixture.object_cancellation',
        '-CompilerWorkerMode', $Mode, '-CompilerWorkerTimeoutSeconds', '5', '-CompilerWorkerIdleTimeoutSeconds', '60')
    if ($Profile) { $Arguments += @('-LanguageProfile', 'gameplay-v1') }
    $Arguments += $Extra
    & (Join-Path $PSHOME 'pwsh.exe') @Arguments *> (Join-Path $RunRoot "$Name.log")
    $ExitCode = $LASTEXITCODE
    $Report = ReadJson (Join-Path $Output 'flow.csharp.report.json')
    WriteJson (Join-Path $RunRoot "$Name.report.evidence.json") $Report
    if ($Report.compiler_worker.used) {
        $script:WorkerContext = Get-AvidScriptCompilerWorkerContext -PluginRoot $PluginRoot -DotNetPath $DotNetPath -Configuration Release -ProjectRoot $Workspace
        $script:WorkerContext.WorkerInstanceId = [string]$Report.compiler_worker.worker_instance_id
        $script:WorkerContext.WorkerProcessId = [int]$Report.compiler_worker.worker_process_id
    }
    if (($ExitCode -eq 0) -ne $Success) { Write-Output ($Report.diagnostics | ConvertTo-Json -Depth 8) }
    Check (($ExitCode -eq 0) -eq $Success -and [bool]$Report.succeeded -eq $Success) "$Name formal result"
    if ($Success) {
        $Ir = ReadJson (Join-Path $Output 'flow.guestir.json')
        $Semantic = ReadJson (Join-Path $Output 'flow.csharp.semantic.json')
        $Manifest = ReadJson $ManifestPath
        Check ($Ir.schema_version -eq 39 -and $Ir.ir_version -ceq '1.38' -and
            $Semantic.schema_version -eq 58 -and $Semantic.semantic_version -ceq '1.67' -and
            $null -ne $Ir.object_await_cancellation -and $null -ne $Ir.static_storage -and $null -ne $Ir.cancellation_tokens) "$Name exact combined source and execution plans"
        Check ($Manifest.module_id -ceq $Ir.module_id -and $Manifest.module_id -ceq $Report.module_id -and
            $Manifest.wasm.sha256 -ceq (Hash (Join-Path $Output 'flow.wasm')) -and
            $Manifest.guest_ir.sha256 -ceq (Hash (Join-Path $Output 'flow.guestir.json')) -and
            $Manifest.source.semantic_sha256 -ceq (Hash (Join-Path $Output 'flow.csharp.semantic.json'))) "$Name executed bytes match formal manifest"
    }
    else {
        Check (-not (Test-Path -LiteralPath (Join-Path $Output 'flow.wasm')) -and
            -not (Test-Path -LiteralPath $ManifestPath)) "$Name publishes no module"
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
    $SourcePath = Join-Path $Workspace 'Flow.cs'
    $SourceProject = Join-Path $Workspace 'Flow.csproj'
    $SharedSource = [IO.File]::ReadAllText((Join-Path $PluginRoot 'Fixtures/Phase66/ObjectLoadCancellationFlow.cs'))
    $GuestEntry = [IO.File]::ReadAllText((Join-Path $PluginRoot 'Fixtures/Phase66/ObjectLoadCancellationFlow.Guest.cs'))
    [IO.File]::WriteAllText($SourcePath, $SharedSource + "`n" + $GuestEntry, $Utf8)
    [IO.File]::WriteAllText($SourceProject, '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>', $Utf8)
    & $DotNetPath build Fixtures/Phase66/ObjectLoadCancellationFlow.Reference.csproj -c Release --disable-build-servers -m:1 -nodeReuse:false -p:UseSharedCompilation=false *> (Join-Path $RunRoot 'reference-build.log')
    Check ($LASTEXITCODE -eq 0) 'same-source reference build'
    $ReferenceCases = Join-Path $Workspace 'reference-cases.json'
    & $DotNetPath run --project Fixtures/Phase66/ObjectLoadCancellationFlow.Reference.csproj -c Release --no-build --no-restore -- $ReferenceCases *> (Join-Path $RunRoot 'reference.log')
    $ReferenceExit = $LASTEXITCODE
    $ReferenceLog = Get-Content -Raw -LiteralPath (Join-Path $RunRoot 'reference.log')
    Check ($ReferenceExit -eq 0 -and $ReferenceLog -match 'ObjectLoadCancellationFlow.Reference: (\d+)/\1 passed; cases=10') 'same-source reference runner completes all cases'
    Copy-Item -LiteralPath $ReferenceCases -Destination (Join-Path $RunRoot 'reference-cases.evidence.json')
    if (-not $SkipBuild) {
        & (Join-Path $EngineRoot 'Engine/Binaries/ThirdParty/DotNet/10.0/win-x64/dotnet.exe') (Join-Path $EngineRoot 'Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll') AvidTPSTemplateEditor Win64 Development "-Project=$ProjectPath" -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=1 -NoUBA *> (Join-Path $RunRoot 'native-build.log')
        Check ($LASTEXITCODE -eq 0) 'single-action no-clean native build'
    }
    $Profile = ReadJson (Join-Path $PluginRoot 'Fixtures/Phase66/IntegratedLanguageFlow.csharp-profile.json')
    $Profile.source_path = [IO.Path]::GetRelativePath($ProjectOwner, $SourcePath)
    $Profile.project_path = [IO.Path]::GetRelativePath($ProjectOwner, $SourceProject)
    $Profile.binding_profile.package_name = 'avidscript.fixture.object_cancellation'
    $ProfilePath = Join-Path $Workspace 'profile.json'
    WriteJson $ProfilePath $Profile
    $PublishReport = Join-Path $RunRoot 'publish.json'
    InvokeEditor @('-run=AvidScriptPublishProfileBindings', "-Profile=$ProfilePath", "-Report=$PublishReport", "-OutputRoot=$(Join-Path $Workspace 'Bindings')") (Join-Path $RunRoot 'publish.log')
    $Published = ReadJson $PublishReport
    Check ($Published.status -ceq 'ok') 'actual profile commandlet publishes binding package'
    $BindingPackage = [string]$Published.manifest_path
    $Package = ReadJson $BindingPackage
    Check ($Package.emitter_version -ceq '49.6.0') 'real generated object cancellation declarations'
    $Facade = Join-Path (Split-Path -Parent $BindingPackage) ([string]$Package.files.reference_source)
    $FacadeHash = Hash $Facade
    if ($ExecutionOnly) {
        # Focus a native test repair on freshly generated, formally verified
        # bytes; the default invocation still runs all build contracts.
        $null = BuildCase cold disabled @('-DisableSemanticCache', '-DisableCompilationCache')
    }
    else {
    $Cold = BuildCase cold required
    Check ($Cold.compiler_worker.used -and $Cold.semantic_cache.published -and $Cold.compilation_cache.published) 'cold uses production worker and publishes both caches'
    $Warm = BuildCase warm required
    Check ($Warm.semantic_cache.lookup -ceq 'hit' -and $Warm.compilation_cache.lookup -ceq 'hit' -and
        $Warm.tool_invocations.frontend -eq 0 -and $Warm.tool_invocations.semantic -eq 0 -and
        $Warm.tool_invocations.guest_ir -eq 0 -and $Warm.tool_invocations.wasm_backend -eq 0) 'warm avoids all compiler stages'
    $NoWorker = BuildCase worker-disabled disabled @('-DisableSemanticCache', '-DisableCompilationCache')
    $Prepared = BuildCase prepared disabled @('-PreparedBuildReportPath', (Join-Path $Workspace 'cold/flow.csharp.report.json'))
    Check ($Prepared.build_reuse.frontend_reused -and $Prepared.build_reuse.semantic_reused) 'prepared reuses validated source artifacts'
    foreach ($Report in @($Warm, $NoWorker, $Prepared)) {
        Check ($Report.semantic.artifact_sha256 -ceq $Cold.semantic.artifact_sha256 -and
            $Report.guest_ir.sha256 -ceq $Cold.guest_ir.sha256 -and $Report.wasm.sha256 -ceq $Cold.wasm.sha256) 'all production routes preserve identical canonical bytes'
    }
    $Rejected = BuildCase legacy-reject disabled @() $false $false
    Check ($Rejected.result -ceq 'semantic_failed') 'default language mode retains rejection'
    $CompilerAst = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'InvokeCSharpGuestCompiler.ps1'), [ref]$null, [ref]$null)
    $FailingCompiler = Join-Path $Workspace 'FailingCompiler.ps1'
    [IO.File]::WriteAllText($FailingCompiler, $CompilerAst.ParamBlock.Extent.Text + "`nWrite-Output 'deliberate compiler failure'`nexit 2`n", $Utf8)
    $FailedCompiler = BuildCase compiler-failure disabled @('-DisableSemanticCache', '-DisableCompilationCache', '-GuestCompilerPath', $FailingCompiler) $true $false
    Check ($FailedCompiler.result -ceq 'guest_ir_failed' -and
        @($FailedCompiler.diagnostics | Where-Object code -eq 'guest_ir_compile_failed').Count -eq 1 -and
        @($FailedCompiler.diagnostics | Where-Object code -eq 'ASBI4703').Count -eq 0) 'compiler failure retains its diagnosis instead of claiming a byte mismatch'
    Check ((Hash $Facade) -ceq $FacadeHash -and [IO.File]::ReadAllText($SourcePath) -ceq ($SharedSource + "`n" + $GuestEntry)) 'generated facade and shared method bodies remain unchanged'
    StopOwnedWorker
    $ContractLog = Join-Path $RunRoot 'language-profile-contract.log'
    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -File (Join-Path $PSScriptRoot 'Contracts/TestCSharpLanguageProfileBuild.ps1') -BindingPackagePath $BindingPackage -DotNetPath $DotNetPath -StandardCancellationTokens -ObjectAwaitCancellation *> $ContractLog
    $ContractExit = $LASTEXITCODE
    Check ($ContractExit -eq 0 -and (Get-Content -Raw -LiteralPath $ContractLog) -match 'Gameplay profile build: (\d+)/\1 passed;') 'real generated facade passes language profile build contract'
    }
    Check ((Hash $Facade) -ceq $FacadeHash -and [IO.File]::ReadAllText($SourcePath) -ceq ($SharedSource + "`n" + $GuestEntry)) 'execution uses unchanged generated facade and shared method bodies'
    $env:AVIDSCRIPT_OBJECT_CANCELLATION_DIR = $Workspace
    $TestName = 'AvidScript.Runtime.Continuation.CompiledObjectLoadCancellation'
    $NativeLog = Join-Path $RunRoot 'native-automation.log'
    InvokeEditor @("-ExecCmds=Automation RunTests $TestName;Quit", '-TestExit=Automation Test Queue Empty') $NativeLog
    $Log = Get-Content -Raw -LiteralPath $NativeLog
    Check ([regex]::Matches($Log, 'Test Completed\. Result=\{Success\} Name=\{CompiledObjectLoadCancellation\}').Count -eq 1 -and
        [regex]::Matches($Log, 'Test Completed\. Result=\{Fail\}').Count -eq 0 -and
        [regex]::Matches($Log, '\*\*\*\* TEST COMPLETE\. EXIT CODE: 0 \*\*\*\*').Count -eq 1 -and
        $Log.Contains('CompiledObjectLoadCancellation: 48/48 passed')) 'dual VM execution matrix and queue complete'
    Check ([regex]::Matches($Log, 'object-cancellation backend=[01] case=\d+ lane=[01] result=').Count -eq 40 -and
        [regex]::Matches($Log, 'object-cancellation-retire backend=[01] case=\d+ lane=0').Count -eq 8) 'complete normal and teardown case inventory'
    Check ($UserPathBefore -ceq [Environment]::GetEnvironmentVariable('Path', 'User') -and
        $MachinePathBefore -ceq [Environment]::GetEnvironmentVariable('Path', 'Machine')) 'persistent PATH unchanged'
    Write-Output "Object load cancellation: $($Checks.Count)/$($Checks.Count) build checks, VM=48/48; evidence=$RunRoot"
}
finally {
    try {
    StopOwnedWorker
    WriteJson (Join-Path $RunRoot 'results.json') @($Checks)
    # Only this newly-created workspace is owned here. Preserve logs and hashes.
    $Resolved = [IO.Path]::GetFullPath($Workspace).TrimEnd('\', '/')
    $Expected = [IO.Path]::GetFullPath((Join-Path $RunRoot 'Workspace')).TrimEnd('\', '/')
    if ($Resolved -cne $Expected -or (Get-Item -LiteralPath $Resolved).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Cleanup root containment failed.' }
    $Prefix = $Resolved + [IO.Path]::DirectorySeparatorChar
    $Items = @(Get-ChildItem -LiteralPath $Resolved -Recurse -Force)
    if (@($Items | Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        -not $_.FullName.StartsWith($Prefix, [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0) { throw 'Cleanup item containment failed.' }
    $Files = @($Items | Where-Object { -not $_.PSIsContainer } | ForEach-Object {
        [ordered]@{ path = $_.FullName; length = $_.Length; sha256 = Hash $_.FullName }
    })
    WriteJson (Join-Path $RunRoot 'generated-files.json') $Files
    foreach ($File in $Files) {
        if ((Hash $File.path) -cne $File.sha256) { throw 'Owned cleanup bytes changed.' }
        Remove-Item -LiteralPath $File.path -Force
    }
    foreach ($Directory in @($Items | Where-Object PSIsContainer | Sort-Object { $_.FullName.Length } -Descending)) {
        if (@(Get-ChildItem -LiteralPath $Directory.FullName -Force).Count -eq 0) { Remove-Item -LiteralPath $Directory.FullName }
    }
    if (@(Get-ChildItem -LiteralPath $Resolved -Force).Count -eq 0) { Remove-Item -LiteralPath $Resolved }
    }
    finally {
        foreach ($Name in $PriorEnvironment.Keys) { [Environment]::SetEnvironmentVariable($Name, $PriorEnvironment[$Name], 'Process') }
        Pop-Location
    }
}
