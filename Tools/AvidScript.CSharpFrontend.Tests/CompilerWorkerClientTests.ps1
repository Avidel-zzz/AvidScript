param()

$ErrorActionPreference = "Stop"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDir
$PluginRoot = Split-Path -Parent $ToolsRoot
$PowerShellHost = Join-Path $PSHOME "pwsh.exe"
$BlackholeServerScript = Join-Path $TestDir "CompilerWorkerBlackholeServer.ps1"
. (Join-Path $PluginRoot "Build\AvidScriptCSharpCompilerWorker.ps1")

function Assert-Condition {
    param([bool]$Condition, [string]$Message)

    if (-not $Condition) {
        throw $Message
    }
}

function New-TestContext {
    param([Parameter(Mandatory = $true)][string]$PipeName)

    return [pscustomobject]@{
        ProtocolVersion = 1
        PipeName = $PipeName
        ToolchainFingerprint = "b" * 64
        RequestTimeoutMilliseconds = 100
    }
}

function Start-BlackholeServer {
    param([Parameter(Mandatory = $true)][string]$PipeName)

    return Start-Process `
        -FilePath $PowerShellHost `
        -ArgumentList @(
            "-NoProfile",
            "-File", "`"$BlackholeServerScript`"",
            "-PipeName", $PipeName,
            "-HoldSeconds", "10") `
        -WindowStyle Hidden `
        -PassThru
}

foreach ($RequiredFile in @($PowerShellHost, $BlackholeServerScript)) {
    Assert-Condition (Test-Path -LiteralPath $RequiredFile -PathType Leaf) `
        "required compiler worker client test file is missing: $RequiredFile"
}

$AutoPipeName = "AvidScript.CompilerWorker.Client.Auto." + [Guid]::NewGuid().ToString("N")
$AutoContext = New-TestContext -PipeName $AutoPipeName
$AutoServer = Start-BlackholeServer -PipeName $AutoPipeName
try {
    $AutoRequest = New-AvidScriptCompilerWorkerRequest -Context $AutoContext -Stage "ping"
    $AutoResult = Invoke-AvidScriptCompilerWorkerWithPolicy `
        -Context $AutoContext `
        -Request $AutoRequest `
        -Mode auto `
        -ConnectTimeoutMilliseconds 5000 `
        -ResponseTimeoutMilliseconds 100
    Assert-Condition (-not $AutoResult.UseWorker -and $AutoResult.Fallback) `
        "auto mode should fall back after a worker response timeout"
    Assert-Condition ([string]$AutoResult.DiagnosticCode -ceq "ASCW3009") `
        "auto timeout should publish the stable fallback diagnostic"
}
finally {
    if (-not $AutoServer.HasExited) {
        Stop-Process -Id $AutoServer.Id -Force
        [void]$AutoServer.WaitForExit(5000)
    }
    $AutoServer.Dispose()
}

$RequiredPipeName = "AvidScript.CompilerWorker.Client.Required." + [Guid]::NewGuid().ToString("N")
$RequiredContext = New-TestContext -PipeName $RequiredPipeName
$RequiredServer = Start-BlackholeServer -PipeName $RequiredPipeName
try {
    $RequiredRequest = New-AvidScriptCompilerWorkerRequest -Context $RequiredContext -Stage "ping"
    $RequiredRejected = $false
    try {
        Invoke-AvidScriptCompilerWorkerWithPolicy `
            -Context $RequiredContext `
            -Request $RequiredRequest `
            -Mode required `
            -ConnectTimeoutMilliseconds 5000 `
            -ResponseTimeoutMilliseconds 100 | Out-Null
    }
    catch {
        $RequiredRejected = $_.Exception.ToString().Contains("ASCW3004")
    }
    Assert-Condition $RequiredRejected "required mode should fail closed after a worker timeout"
}
finally {
    if (-not $RequiredServer.HasExited) {
        Stop-Process -Id $RequiredServer.Id -Force
        [void]$RequiredServer.WaitForExit(5000)
    }
    $RequiredServer.Dispose()
}

Write-Output "AvidScript compiler worker client tests: 2/2 passed."
