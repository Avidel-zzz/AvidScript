Set-StrictMode -Version Latest

$script:AvidScriptCompilerWorkerProtocolVersion = 1
$script:AvidScriptCompilerWorkerMaximumMessageCharacters = 1024 * 1024

function Get-AvidScriptCompilerWorkerFileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        $Sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $Bytes = $Sha256.ComputeHash($Stream)
        }
        finally {
            $Sha256.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
    }
    return [System.BitConverter]::ToString($Bytes).Replace("-", "").ToLowerInvariant()
}

function Get-AvidScriptCompilerWorkerTextSha256 {
    param([Parameter(Mandatory = $true)][string]$Text)

    $Bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
    $Hash = [System.Security.Cryptography.SHA256]::HashData($Bytes)
    return [System.BitConverter]::ToString($Hash).Replace("-", "").ToLowerInvariant()
}

function Get-AvidScriptCompilerWorkerToolchainFingerprint {
    param(
        [Parameter(Mandatory = $true)][string]$PluginRoot,
        [Parameter(Mandatory = $true)][string]$DotNetPath
    )

    $PluginRoot = [System.IO.Path]::GetFullPath($PluginRoot)
    $DotNetPath = [System.IO.Path]::GetFullPath($DotNetPath)
    $RequiredFiles = @(
        (Join-Path $PluginRoot "global.json"),
        (Join-Path $PluginRoot "Build\AvidScriptCSharpCompilerWorker.ps1"))
    foreach ($RequiredFile in $RequiredFiles) {
        if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
            throw "ASCW3001: Required compiler worker toolchain file is missing: $RequiredFile"
        }
    }
    if (-not (Test-Path -LiteralPath $DotNetPath -PathType Leaf)) {
        throw "ASCW3001: Compiler worker .NET host is missing: $DotNetPath"
    }

    $Files = @($RequiredFiles | ForEach-Object { Get-Item -LiteralPath $_ })
    foreach ($SourceRootName in @(
        "AvidScript.CSharpFrontend",
        "AvidScript.CSharpSemantic",
        "AvidScript.GuestIr",
        "AvidScript.CSharpGuest",
        "AvidScript.WasmBackend",
        "AvidScript.CSharpCompilerWorker")) {
        $SourceRoot = Join-Path $PluginRoot ("Tools\" + $SourceRootName)
        if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
            throw "ASCW3001: Compiler worker source root is missing: $SourceRoot"
        }
        $Files += @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File | Where-Object {
            ($_.Extension -eq ".cs" -or $_.Extension -eq ".csproj") -and
            $_.FullName -notmatch '[\\/](?:bin|obj)[\\/]'
        })
    }

    $PluginPrefix = $PluginRoot.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    $Records = @($Files | ForEach-Object {
        $FullPath = [System.IO.Path]::GetFullPath($_.FullName)
        if (-not $FullPath.StartsWith($PluginPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "ASCW3001: Compiler worker source escapes the plugin root: $FullPath"
        }
        [ordered]@{
            path = $FullPath.Substring($PluginPrefix.Length).Replace("\", "/")
            length = [long]$_.Length
            sha256 = Get-AvidScriptCompilerWorkerFileSha256 $FullPath
        }
    } | Sort-Object -Property path)
    $Identity = [ordered]@{
        schema_version = 1
        files = @($Records)
        dotnet_sha256 = Get-AvidScriptCompilerWorkerFileSha256 $DotNetPath
    }
    $Json = $Identity | ConvertTo-Json -Compress -Depth 8
    return Get-AvidScriptCompilerWorkerTextSha256 $Json
}

function Get-AvidScriptCompilerWorkerContext {
    param(
        [Parameter(Mandatory = $true)][string]$PluginRoot,
        [Parameter(Mandatory = $true)][string]$DotNetPath,
        [Parameter(Mandatory = $true)][string]$Configuration,
        [ValidateRange(1, 300)][int]$RequestTimeoutSeconds = 30,
        [ValidateRange(5, 3600)][int]$IdleTimeoutSeconds = 300
    )

    $PluginRoot = [System.IO.Path]::GetFullPath($PluginRoot)
    $DotNetPath = [System.IO.Path]::GetFullPath($DotNetPath)
    $Fingerprint = Get-AvidScriptCompilerWorkerToolchainFingerprint `
        -PluginRoot $PluginRoot `
        -DotNetPath $DotNetPath
    $RepositoryIdentity = Get-AvidScriptCompilerWorkerTextSha256 (
        $PluginRoot.ToLowerInvariant() + "`n" + $Configuration + "`n" + $Fingerprint)
    $ToolRoot = Join-Path $PluginRoot "Saved\AvidScriptCompilerWorkerTool\$Configuration"
    return [pscustomobject]@{
        ProtocolVersion = $script:AvidScriptCompilerWorkerProtocolVersion
        PluginRoot = $PluginRoot
        DotNetPath = $DotNetPath
        Configuration = $Configuration
        ToolchainFingerprint = $Fingerprint
        PipeName = "AvidScript.CompilerWorker." + $RepositoryIdentity.Substring(0, 32)
        WorkerProjectPath = Join-Path $PluginRoot "Tools\AvidScript.CSharpCompilerWorker\AvidScript.CSharpCompilerWorker.csproj"
        WorkerDllPath = Join-Path $PluginRoot "Tools\AvidScript.CSharpCompilerWorker\bin\$Configuration\net8.0\AvidScript.CSharpCompilerWorker.dll"
        ToolRoot = $ToolRoot
        BuildMarkerPath = Join-Path $ToolRoot "build.json"
        RequestTimeoutMilliseconds = $RequestTimeoutSeconds * 1000
        IdleTimeoutSeconds = $IdleTimeoutSeconds
        WorkerBuilt = $false
        WorkerStarted = $false
        WorkerInstanceId = ""
        WorkerProcessId = 0
    }
}

function Test-AvidScriptCompilerWorkerBuildMarker {
    param([Parameter(Mandatory = $true)]$Context)

    if (-not (Test-Path -LiteralPath $Context.WorkerDllPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $Context.BuildMarkerPath -PathType Leaf)) {
        return $false
    }
    try {
        $Marker = Get-Content -Raw -LiteralPath $Context.BuildMarkerPath | ConvertFrom-Json
        return [int]$Marker.schema_version -eq 1 -and
            [string]$Marker.toolchain_fingerprint -ceq [string]$Context.ToolchainFingerprint -and
            [string]$Marker.worker_dll_sha256 -ceq
                (Get-AvidScriptCompilerWorkerFileSha256 $Context.WorkerDllPath)
    }
    catch {
        return $false
    }
}

function Build-AvidScriptCompilerWorker {
    param([Parameter(Mandatory = $true)]$Context)

    if (Test-AvidScriptCompilerWorkerBuildMarker -Context $Context) {
        return $false
    }
    New-Item -ItemType Directory -Force -Path $Context.ToolRoot | Out-Null
    $RawOutput = @(& $Context.DotNetPath build $Context.WorkerProjectPath `
        --configuration $Context.Configuration `
        --nologo `
        --verbosity quiet 2>&1)
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0 -or -not (Test-Path -LiteralPath $Context.WorkerDllPath -PathType Leaf)) {
        $Output = @($RawOutput | ForEach-Object { $_.ToString() }) -join [System.Environment]::NewLine
        throw "ASCW3002: Compiler worker build failed with exit code ${ExitCode}: $Output"
    }

    $Marker = [ordered]@{
        schema_version = 1
        toolchain_fingerprint = $Context.ToolchainFingerprint
        worker_dll_sha256 = Get-AvidScriptCompilerWorkerFileSha256 $Context.WorkerDllPath
    }
    $TemporaryPath = "$($Context.BuildMarkerPath).tmp.$PID"
    try {
        [System.IO.File]::WriteAllText(
            $TemporaryPath,
            ($Marker | ConvertTo-Json -Depth 4) + [System.Environment]::NewLine,
            [System.Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $TemporaryPath -Destination $Context.BuildMarkerPath -Force
    }
    finally {
        if (Test-Path -LiteralPath $TemporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
    }
    return $true
}

function New-AvidScriptCompilerWorkerRequest {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [Parameter(Mandatory = $true)][string]$Stage,
        [hashtable]$Fields = @{}
    )

    $Request = [ordered]@{
        protocol_version = [int]$Context.ProtocolVersion
        request_id = "$Stage-$([Guid]::NewGuid().ToString('N'))"
        toolchain_fingerprint = [string]$Context.ToolchainFingerprint
        stage = $Stage
    }
    foreach ($Entry in $Fields.GetEnumerator()) {
        if ($Request.Contains($Entry.Key)) {
            throw "ASCW3003: Compiler worker request field is reserved: $($Entry.Key)"
        }
        $Request[$Entry.Key] = $Entry.Value
    }
    return $Request
}

function Invoke-AvidScriptCompilerWorkerRaw {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Request,
        [int]$ConnectTimeoutMilliseconds = 1000,
        [int]$ResponseTimeoutMilliseconds = 0
    )

    if ($ResponseTimeoutMilliseconds -le 0) {
        $ResponseTimeoutMilliseconds = [int]$Context.RequestTimeoutMilliseconds
    }
    $Pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        ".",
        [string]$Context.PipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $Pipe.Connect($ConnectTimeoutMilliseconds)
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
            $Writer.WriteLine(($Request | ConvertTo-Json -Compress -Depth 12))
            $Writer.Flush()
            $ResponseCancellation = [System.Threading.CancellationTokenSource]::new()
            $ResponseCancellation.CancelAfter($ResponseTimeoutMilliseconds)
            try {
                $Line = $Reader.ReadLineAsync($ResponseCancellation.Token).AsTask().GetAwaiter().GetResult()
            }
            catch [System.OperationCanceledException] {
                throw [System.TimeoutException]::new(
                    "ASCW3004: Compiler worker response timed out after $ResponseTimeoutMilliseconds ms.")
            }
            finally {
                $ResponseCancellation.Dispose()
            }
            if ([string]::IsNullOrWhiteSpace($Line) -or
                $Line.Length -gt $script:AvidScriptCompilerWorkerMaximumMessageCharacters) {
                throw "ASCW3005: Compiler worker response size is invalid."
            }
            $Response = $Line | ConvertFrom-Json
        }
        finally {
            $Reader.Dispose()
            $Writer.Dispose()
        }
    }
    finally {
        $Pipe.Dispose()
    }

    foreach ($FieldName in @(
        "protocol_version",
        "request_id",
        "worker_instance_id",
        "worker_process_id",
        "toolchain_fingerprint",
        "stage",
        "succeeded",
        "exit_code",
        "duration_ms",
        "diagnostics",
        "workspace")) {
        if ($Response.PSObject.Properties.Name -notcontains $FieldName) {
            throw "ASCW3006: Compiler worker response is missing field: $FieldName"
        }
    }
    if ([int]$Response.protocol_version -ne [int]$Context.ProtocolVersion -or
        [string]$Response.request_id -cne [string]$Request.request_id -or
        [string]$Response.toolchain_fingerprint -cne [string]$Context.ToolchainFingerprint -or
        [string]$Response.stage -cne [string]$Request.stage -or
        [string]$Response.worker_instance_id -cnotmatch '^[0-9a-f]{32}$' -or
        [int]$Response.worker_process_id -le 0) {
        throw "ASCW3006: Compiler worker response identity is invalid."
    }
    return $Response
}

function Start-AvidScriptCompilerWorkerProcess {
    param([Parameter(Mandatory = $true)]$Context)

    New-Item -ItemType Directory -Force -Path $Context.ToolRoot | Out-Null
    $LogIdentity = [Guid]::NewGuid().ToString("N")
    $StdoutPath = Join-Path $Context.ToolRoot "worker.$LogIdentity.stdout.log"
    $StderrPath = Join-Path $Context.ToolRoot "worker.$LogIdentity.stderr.log"
    $Process = Start-Process `
        -FilePath $Context.DotNetPath `
        -ArgumentList @(
            "`"$($Context.WorkerDllPath)`"",
            "--pipe", $Context.PipeName,
            "--toolchain-fingerprint", $Context.ToolchainFingerprint,
            "--idle-timeout-seconds", [string]$Context.IdleTimeoutSeconds) `
        -WindowStyle Hidden `
        -RedirectStandardOutput $StdoutPath `
        -RedirectStandardError $StderrPath `
        -PassThru
    return $Process
}

function Initialize-AvidScriptCompilerWorker {
    param([Parameter(Mandatory = $true)]$Context)

    $MutexName = "Local\AvidScript.CompilerWorker.Provision." +
        ([string]$Context.ToolchainFingerprint).Substring(0, 24)
    $Mutex = [System.Threading.Mutex]::new($false, $MutexName)
    $Acquired = $false
    try {
        try {
            $Acquired = $Mutex.WaitOne([int]$Context.RequestTimeoutMilliseconds)
        }
        catch [System.Threading.AbandonedMutexException] {
            $Acquired = $true
        }
        if (-not $Acquired) {
            throw "ASCW3007: Timed out waiting for compiler worker provisioning lock."
        }

        $Context.WorkerBuilt = Build-AvidScriptCompilerWorker -Context $Context
        $PingRequest = New-AvidScriptCompilerWorkerRequest -Context $Context -Stage "ping"
        try {
            $Ping = Invoke-AvidScriptCompilerWorkerRaw `
                -Context $Context `
                -Request $PingRequest `
                -ConnectTimeoutMilliseconds 200 `
                -ResponseTimeoutMilliseconds 1000
        }
        catch {
            $Process = Start-AvidScriptCompilerWorkerProcess -Context $Context
            $Context.WorkerStarted = $true
            $Deadline = [System.DateTime]::UtcNow.AddMilliseconds(
                [System.Math]::Min(10000, [int]$Context.RequestTimeoutMilliseconds))
            $LastError = $_.Exception.Message
            $Ping = $null
            while ([System.DateTime]::UtcNow -lt $Deadline -and $null -eq $Ping) {
                Start-Sleep -Milliseconds 100
                try {
                    $PingRequest = New-AvidScriptCompilerWorkerRequest -Context $Context -Stage "ping"
                    $Ping = Invoke-AvidScriptCompilerWorkerRaw `
                        -Context $Context `
                        -Request $PingRequest `
                        -ConnectTimeoutMilliseconds 250 `
                        -ResponseTimeoutMilliseconds 1000
                }
                catch {
                    $LastError = $_.Exception.Message
                    if ($Process.HasExited) {
                        break
                    }
                }
            }
            if ($null -eq $Ping) {
                throw "ASCW3008: Compiler worker did not become ready: $LastError"
            }
        }
        if (-not [bool]$Ping.succeeded -or [int]$Ping.exit_code -ne 0) {
            throw "ASCW3008: Compiler worker ping was rejected."
        }
        $Context.WorkerInstanceId = [string]$Ping.worker_instance_id
        $Context.WorkerProcessId = [int]$Ping.worker_process_id
        return $Context
    }
    finally {
        if ($Acquired) {
            $Mutex.ReleaseMutex()
        }
        $Mutex.Dispose()
    }
}

function Invoke-AvidScriptCompilerWorkerWithPolicy {
    param(
        [Parameter(Mandatory = $true)]$Context,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Request,
        [ValidateSet("auto", "required")][string]$Mode = "auto",
        [int]$ConnectTimeoutMilliseconds = 1000,
        [int]$ResponseTimeoutMilliseconds = 0
    )

    try {
        $Response = Invoke-AvidScriptCompilerWorkerRaw `
            -Context $Context `
            -Request $Request `
            -ConnectTimeoutMilliseconds $ConnectTimeoutMilliseconds `
            -ResponseTimeoutMilliseconds $ResponseTimeoutMilliseconds
        return [pscustomobject]@{
            UseWorker = $true
            Fallback = $false
            Response = $Response
            DiagnosticCode = ""
            DiagnosticMessage = ""
        }
    }
    catch {
        if ($Mode -ceq "required") {
            throw
        }
        return [pscustomobject]@{
            UseWorker = $false
            Fallback = $true
            Response = $null
            DiagnosticCode = "ASCW3009"
            DiagnosticMessage = $_.Exception.Message
        }
    }
}
