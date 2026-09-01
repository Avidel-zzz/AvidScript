param(
    [Parameter(Mandatory = $true)][string]$PipeName,
    [ValidateRange(1, 30)][int]$HoldSeconds = 10
)

$ErrorActionPreference = "Stop"
$Server = [System.IO.Pipes.NamedPipeServerStream]::new(
    $PipeName,
    [System.IO.Pipes.PipeDirection]::InOut,
    1,
    [System.IO.Pipes.PipeTransmissionMode]::Byte,
    [System.IO.Pipes.PipeOptions]::Asynchronous -bor
        [System.IO.Pipes.PipeOptions]::CurrentUserOnly)
try {
    $Server.WaitForConnection()
    $Reader = [System.IO.StreamReader]::new(
        $Server,
        [System.Text.UTF8Encoding]::new($false, $true),
        $false,
        4096,
        $true)
    try {
        [void]$Reader.ReadLine()
        Start-Sleep -Seconds $HoldSeconds
    }
    finally {
        $Reader.Dispose()
    }
}
finally {
    $Server.Dispose()
}
