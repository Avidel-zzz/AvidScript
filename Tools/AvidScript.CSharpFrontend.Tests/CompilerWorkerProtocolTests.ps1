param(
    [string]$DotNetPath = "",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$WorkerDll = Join-Path $PluginRoot "Tools\AvidScript.CSharpCompilerWorker\bin\$Configuration\net8.0\AvidScript.CSharpCompilerWorker.dll"
$SourcePath = Join-Path $PluginRoot "Samples\CSharp\ActorLifecycle\ActorLifecycleScript.cs"
$RunRoot = Join-Path $PluginRoot "Saved\AvidScriptFrontendDotNet\CompilerWorkerProtocolTests"
$PipeName = "AvidScript.CompilerWorker.Tests." + [Guid]::NewGuid().ToString("N")
$Fingerprint = "a" * 64

function Assert-Condition {
    param([bool]$Condition, [string]$Message)

    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-WorkerRequest {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Request,
        [int]$DelayBeforeWriteMilliseconds = 0
    )

    $Pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        ".",
        $PipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $Pipe.Connect(5000)
        $Writer = [System.IO.StreamWriter]::new(
            $Pipe,
            [System.Text.UTF8Encoding]::new($false),
            4096,
            $true)
        $Reader = [System.IO.StreamReader]::new(
            $Pipe,
            [System.Text.UTF8Encoding]::new($false, $true),
            $false,
            4096,
            $true)
        try {
            $Writer.NewLine = "`n"
            if ($DelayBeforeWriteMilliseconds -gt 0) {
                Start-Sleep -Milliseconds $DelayBeforeWriteMilliseconds
            }
            $Writer.WriteLine(($Request | ConvertTo-Json -Compress -Depth 8))
            $Writer.Flush()
            $ReadTask = $Reader.ReadLineAsync()
            Assert-Condition ($ReadTask.Wait(10000)) "compiler worker response timed out"
            $Line = $ReadTask.Result
            Assert-Condition (-not [string]::IsNullOrWhiteSpace($Line)) "compiler worker response is empty"
            return $Line | ConvertFrom-Json
        }
        finally {
            $Reader.Dispose()
            $Writer.Dispose()
        }
    }
    finally {
        $Pipe.Dispose()
    }
}

function New-WorkerRequest {
    param(
        [Parameter(Mandatory = $true)][string]$RequestId,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    return @{
        protocol_version = 1
        request_id = $RequestId
        toolchain_fingerprint = $Fingerprint
        stage = $Stage
    }
}

if ([string]::IsNullOrWhiteSpace($DotNetPath)) {
    $DotNetPath = Join-Path $env:USERPROFILE ".dotnet\dotnet.exe"
}
foreach ($RequiredFile in @($DotNetPath, $WorkerDll, $SourcePath)) {
    Assert-Condition (Test-Path -LiteralPath $RequiredFile -PathType Leaf) "required worker test file is missing: $RequiredFile"
}
Remove-Item -LiteralPath $RunRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $RunRoot | Out-Null
$StdoutPath = Join-Path $RunRoot "worker.stdout.log"
$StderrPath = Join-Path $RunRoot "worker.stderr.log"
$WorkerProcess = Start-Process `
    -FilePath $DotNetPath `
    -ArgumentList @(
        "`"$WorkerDll`"",
        "--pipe", $PipeName,
        "--toolchain-fingerprint", $Fingerprint,
        "--idle-timeout-seconds", "2") `
    -WindowStyle Hidden `
    -RedirectStandardOutput $StdoutPath `
    -RedirectStandardError $StderrPath `
    -PassThru

try {
    $Ping = Invoke-WorkerRequest `
        (New-WorkerRequest -RequestId "ping-1" -Stage "ping") `
        -DelayBeforeWriteMilliseconds 2500
    Assert-Condition ($Ping.succeeded -and [int]$Ping.exit_code -eq 0) "compiler worker ping failed"
    $WorkerInstanceId = [string]$Ping.worker_instance_id
    Assert-Condition ($WorkerInstanceId -cmatch '^[0-9a-f]{32}$') "compiler worker instance id is invalid"
    Assert-Condition ([int]$Ping.worker_process_id -eq $WorkerProcess.Id) "compiler worker process id is invalid"

    $FrontendPath = Join-Path $RunRoot "module.frontend.json"
    $FrontendRequest = New-WorkerRequest -RequestId "frontend-1" -Stage "frontend"
    $FrontendRequest.source_path = [System.IO.Path]::GetFullPath($SourcePath)
    $FrontendRequest.source_id = "Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs"
    $FrontendRequest.output_path = [System.IO.Path]::GetFullPath($FrontendPath)
    $Frontend = Invoke-WorkerRequest $FrontendRequest
    Assert-Condition ($Frontend.succeeded -and (Test-Path -LiteralPath $FrontendPath -PathType Leaf)) "worker Frontend stage failed"
    Assert-Condition ([string]$Frontend.worker_instance_id -ceq $WorkerInstanceId) "worker restarted before Frontend stage"

    $SemanticPath = Join-Path $RunRoot "module.semantic.json"
    $SemanticRequest = New-WorkerRequest -RequestId "semantic-1" -Stage "semantic"
    $SemanticRequest.source_path = [System.IO.Path]::GetFullPath($SourcePath)
    $SemanticRequest.source_id = "Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs"
    $SemanticRequest.frontend_path = [System.IO.Path]::GetFullPath($FrontendPath)
    $SemanticRequest.output_path = [System.IO.Path]::GetFullPath($SemanticPath)
    $Semantic = Invoke-WorkerRequest $SemanticRequest
    Assert-Condition ($Semantic.succeeded -and (Test-Path -LiteralPath $SemanticPath -PathType Leaf)) "worker Semantic stage failed"
    Assert-Condition ([string]$Semantic.worker_instance_id -ceq $WorkerInstanceId) "worker restarted before Semantic stage"
    Assert-Condition ([int]$Semantic.workspace.metadata_reference_set_builds -eq 1) "worker did not initialize one Roslyn metadata-reference set"
    Assert-Condition ([int]$Semantic.workspace.syntax_tree_cache_misses -ge 1) "worker did not report the initial Roslyn syntax-tree miss"

    $SecondSemanticPath = Join-Path $RunRoot "module.second.semantic.json"
    $SecondSemanticRequest = New-WorkerRequest -RequestId "semantic-2" -Stage "semantic"
    $SecondSemanticRequest.source_path = [System.IO.Path]::GetFullPath($SourcePath)
    $SecondSemanticRequest.source_id = "Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs"
    $SecondSemanticRequest.frontend_path = [System.IO.Path]::GetFullPath($FrontendPath)
    $SecondSemanticRequest.output_path = [System.IO.Path]::GetFullPath($SecondSemanticPath)
    $SecondSemantic = Invoke-WorkerRequest $SecondSemanticRequest
    Assert-Condition ($SecondSemantic.succeeded -and (Test-Path -LiteralPath $SecondSemanticPath -PathType Leaf)) "second worker Semantic stage failed"
    Assert-Condition ([string]$SecondSemantic.worker_instance_id -ceq $WorkerInstanceId) "worker restarted before the second Semantic stage"
    Assert-Condition ([int]$SecondSemantic.workspace.metadata_reference_set_builds -eq 1) "worker rebuilt Roslyn metadata references"
    Assert-Condition ([int]$SecondSemantic.workspace.syntax_tree_cache_hits -gt [int]$Semantic.workspace.syntax_tree_cache_hits) "worker did not reuse the Roslyn syntax tree"

    $GuestIrPath = Join-Path $RunRoot "module.guestir.json"
    $DebugMapPath = Join-Path $RunRoot "module.debug.json"
    $StateSchemaPath = Join-Path $RunRoot "module.state.json"
    $WasmPath = Join-Path $RunRoot "module.wasm"
    $InspectionPath = Join-Path $RunRoot "module.wasm.inspect.json"
    $GuestRequest = New-WorkerRequest -RequestId "guest-1" -Stage "guest"
    $GuestRequest.semantic_path = [System.IO.Path]::GetFullPath($SemanticPath)
    $GuestRequest.guest_ir_path = [System.IO.Path]::GetFullPath($GuestIrPath)
    $GuestRequest.debug_map_path = [System.IO.Path]::GetFullPath($DebugMapPath)
    $GuestRequest.state_schema_path = [System.IO.Path]::GetFullPath($StateSchemaPath)
    $GuestRequest.wasm_path = [System.IO.Path]::GetFullPath($WasmPath)
    $GuestRequest.inspection_path = [System.IO.Path]::GetFullPath($InspectionPath)
    $GuestRequest.frontend_artifact_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $FrontendPath).Hash.ToLowerInvariant()
    $GuestRequest.data_lane_fusion = "enabled"
    $GuestRequest.debug_instrumentation = "enabled"
    $Guest = Invoke-WorkerRequest $GuestRequest
    Assert-Condition ($Guest.succeeded) "worker Guest/WASM stage failed: $(@($Guest.diagnostics) -join '; ')"
    foreach ($Artifact in @($GuestIrPath, $DebugMapPath, $StateSchemaPath, $WasmPath, $InspectionPath)) {
        Assert-Condition (Test-Path -LiteralPath $Artifact -PathType Leaf) "worker artifact is missing: $Artifact"
    }
    $GuestIr = Get-Content -Raw -LiteralPath $GuestIrPath | ConvertFrom-Json
    Assert-Condition (@($GuestIr.imports | Where-Object {
        [string]$_.module -ceq "avidscript" -and
        [string]$_.name -ceq "avid_debug_probe" -and
        [string]$_.dispatch_class -ceq "debug"
    }).Count -eq 1) "worker did not forward enabled debug instrumentation"
    Assert-Condition ([string]$Guest.worker_instance_id -ceq $WorkerInstanceId) "worker restarted before Guest/WASM stage"

    $InvalidRequest = New-WorkerRequest -RequestId "invalid-version" -Stage "ping"
    $InvalidRequest.protocol_version = 2
    $Invalid = Invoke-WorkerRequest $InvalidRequest
    Assert-Condition (-not $Invalid.succeeded -and [int]$Invalid.exit_code -eq 2) "worker accepted an invalid protocol version"
    Assert-Condition (@($Invalid.diagnostics)[0] -clike '*ASCW1001*') "worker returned the wrong protocol diagnostic"
    Assert-Condition ([string]$Invalid.worker_instance_id -ceq $WorkerInstanceId) "worker restarted after a rejected request"

    $Shutdown = Invoke-WorkerRequest (New-WorkerRequest -RequestId "shutdown-1" -Stage "shutdown")
    Assert-Condition ($Shutdown.succeeded) "compiler worker shutdown request failed"
    Assert-Condition ([string]$Shutdown.worker_instance_id -ceq $WorkerInstanceId) "shutdown reached another worker instance"
    Assert-Condition ($WorkerProcess.WaitForExit(5000)) "compiler worker did not exit after shutdown"
    Assert-Condition ($WorkerProcess.ExitCode -eq 0) "compiler worker exited with code $($WorkerProcess.ExitCode)"
}
finally {
    if (-not $WorkerProcess.HasExited) {
        Stop-Process -Id $WorkerProcess.Id -Force
        [void]$WorkerProcess.WaitForExit(5000)
    }
    $WorkerProcess.Dispose()
}

Write-Output "AvidScript C# compiler worker protocol tests passed."
