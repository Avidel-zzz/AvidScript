param(
    [ValidateSet('All', 'Dedicated', 'Listen')]
    [string]$Topology = 'All',
    [string]$EngineRoot = 'C:\UnrealEngine',
    [string]$ProjectPath = '',
    [int]$TimeoutSeconds = 75,
    [switch]$SkipProfileBuild
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PluginRoot)
if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
    $ProjectPath = Join-Path $ProjectRoot 'AvidTPSTemplate.uproject'
}
$ProjectPath = [System.IO.Path]::GetFullPath($ProjectPath)
$EditorExe = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if (-not [System.IO.File]::Exists($EditorExe)) {
    throw "UE5.8 UnrealEditor-Cmd.exe is missing: $EditorExe"
}
if (-not [System.IO.File]::Exists($ProjectPath)) {
    throw "AvidScript topology project is missing: $ProjectPath"
}
if ($TimeoutSeconds -lt 15) {
    throw 'TimeoutSeconds must be at least 15.'
}

$RunId = '{0}_{1}_{2}' -f (
    [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')),
    $PID,
    ([Guid]::NewGuid().ToString('N').Substring(0, 8))
$RunRoot = Join-Path 'C:\tmp' ("AvidScriptNetworkTopology_$RunId")
[void][System.IO.Directory]::CreateDirectory($RunRoot)

function Get-FreeTcpPort {
    $Listener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Loopback,
        0)
    try {
        $Listener.Start()
        return [int]$Listener.LocalEndpoint.Port
    }
    finally {
        $Listener.Stop()
    }
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Invoke-ProfileBuild {
    $LogPath = Join-Path $RunRoot 'profile_build.log'
    $Arguments = @(
        $ProjectPath,
        '-unattended',
        '-nop4',
        '-nosplash',
        '-nullrhi',
        '-ExecCmds=Automation RunTests AvidScript.Editor.NetworkTopology.BuildProfile',
        '-TestExit=Automation Test Queue Empty',
        "-abslog=$LogPath"
    )
    & $EditorExe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Network topology profile build process failed with exit code $LASTEXITCODE. Log: $LogPath"
    }
    $Succeeded = Select-String -LiteralPath $LogPath -SimpleMatch (
        'Test Completed. Result={Success} Name={BuildProfile}')
    $Failed = Select-String -LiteralPath $LogPath -SimpleMatch (
        'Test Completed. Result={Fail}')
    if ($null -eq $Succeeded -or $null -ne $Failed) {
        throw "Network topology production profile did not pass Automation. Log: $LogPath"
    }
    return [ordered]@{
        log_path = $LogPath
        log_sha256 = Get-FileSha256 $LogPath
    }
}

function Start-TopologyProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogPath
    )
    $QuotedProject = '"' + $ProjectPath + '"'
    $ProcessArguments = @($QuotedProject) + $Arguments + @("-abslog=$LogPath")
    $Process = Start-Process `
        -FilePath $EditorExe `
        -ArgumentList $ProcessArguments `
        -PassThru `
        -WindowStyle Hidden
    return [pscustomobject][ordered]@{
        name = $Name
        process = $Process
        arguments = $ProcessArguments
        log_path = $LogPath
    }
}

function Wait-ForServerListen {
    param(
        [Parameter(Mandatory = $true)]$ProcessRecord,
        [Parameter(Mandatory = $true)][datetime]$Deadline
    )
    while ([DateTime]::UtcNow -lt $Deadline) {
        $ProcessRecord.process.Refresh()
        if ($ProcessRecord.process.HasExited) {
            throw "$($ProcessRecord.name) exited before NetDriver listen; exit=$($ProcessRecord.process.ExitCode), log=$($ProcessRecord.log_path)"
        }
        if ([System.IO.File]::Exists($ProcessRecord.log_path)) {
            $Signal = Select-String `
                -LiteralPath $ProcessRecord.log_path `
                -Pattern 'GameNetDriver.*listening|IpNetDriver.*listening' `
                -Quiet
            if ($Signal) {
                return
            }
        }
        Start-Sleep -Milliseconds 200
    }
    throw "$($ProcessRecord.name) did not open its NetDriver before timeout; log=$($ProcessRecord.log_path)"
}

function Stop-TopologyProcesses {
    param([object[]]$ProcessRecords)
    foreach ($Record in @($ProcessRecords)) {
        if ($null -eq $Record -or $null -eq $Record.process) {
            continue
        }
        $Record.process.Refresh()
        if (-not $Record.process.HasExited) {
            Stop-Process -Id $Record.process.Id -Force
            [void]$Record.process.WaitForExit(10000)
        }
    }
}

function Assert-RoleResult {
    param(
        [Parameter(Mandatory = $true)]$Result,
        [Parameter(Mandatory = $true)][string]$ExpectedTopology,
        [Parameter(Mandatory = $true)][string]$ExpectedRole,
        [Parameter(Mandatory = $true)][int]$ExpectedClientId,
        [Parameter(Mandatory = $true)][int]$ExpectedClients,
        [Parameter(Mandatory = $true)][int]$ExpectedProcessId
    )
    if ([int]$Result.schema_version -ne 1 -or
        [string]$Result.topology -cne $ExpectedTopology -or
        [string]$Result.role -cne $ExpectedRole -or
        [int]$Result.client_id -ne $ExpectedClientId -or
        [int]$Result.expected_clients -ne $ExpectedClients -or
        [int]$Result.process_id -ne $ExpectedProcessId -or
        -not [bool]$Result.succeeded) {
        throw "Invalid $ExpectedTopology/$ExpectedRole result: $($Result | ConvertTo-Json -Depth 16 -Compress)"
    }
    $Actors = @($Result.actors)
    if ($ExpectedRole -eq 'server') {
        $CompletedActors = @($Actors | Where-Object {
            [int]$_.replicated_score -eq 41 -and
            [int]$_.native_server_rpc_count -eq 1 -and
            [int]$_.script_server_rpc_count -eq 1 -and
            [int]$_.client_ack_count -eq 1 -and
            [bool]$_.runtime_loaded -and
            [bool]$_.begin_play_called
        })
        if ($CompletedActors.Count -ne $ExpectedClients) {
            throw "$ExpectedTopology server completed $($CompletedActors.Count)/$ExpectedClients remote actors."
        }
    }
    else {
        $CompletedActors = @($Actors | Where-Object {
            [int]$_.replicated_score -eq 41 -and
            [int]$_.native_server_rpc_count -eq 0 -and
            [int]$_.script_server_rpc_count -eq 0 -and
            [int]$_.native_rep_notify_count -eq 1 -and
            [int]$_.script_rep_notify_count -eq 1 -and
            [bool]$_.runtime_loaded -and
            [bool]$_.begin_play_called
        })
        if ($CompletedActors.Count -ne 1) {
            throw "$ExpectedTopology client $ExpectedClientId did not complete exactly one owner actor."
        }
    }
}

function Invoke-Topology {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$Dedicated,
        [Parameter(Mandatory = $true)][int]$ClientCount
    )
    $TopologyRoot = Join-Path $RunRoot $Name
    [void][System.IO.Directory]::CreateDirectory($TopologyRoot)
    $Port = Get-FreeTcpPort
    $ProcessRecords = [System.Collections.Generic.List[object]]::new()
    $ResultRecords = [System.Collections.Generic.List[object]]::new()
    try {
        $ServerResultPath = Join-Path $TopologyRoot 'server.json'
        $ServerLogPath = Join-Path $TopologyRoot 'server.log'
        $ServerMap = if ($Dedicated) {
            '/Game/TopDown/Lvl_TopDown'
        }
        else {
            '/Game/TopDown/Lvl_TopDown?listen'
        }
        $ServerArguments = @(
            $ServerMap,
            $(if ($Dedicated) { '-server' } else { '-game' }),
            '-unattended',
            '-nop4',
            '-nosplash',
            '-nullrhi',
            '-nosound',
            "-port=$Port",
            "-AvidScriptNetworkTopology=$Name",
            '-AvidScriptNetworkTopologyRole=server',
            '-AvidScriptNetworkTopologyClientId=0',
            "-AvidScriptNetworkTopologyExpectedClients=$ClientCount",
            "-AvidScriptNetworkTopologyTimeout=$TimeoutSeconds",
            "-AvidScriptNetworkTopologyResult=$ServerResultPath"
        )
        $Server = Start-TopologyProcess `
            -Name "$Name-server" `
            -Arguments $ServerArguments `
            -LogPath $ServerLogPath
        $ProcessRecords.Add($Server)
        $Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        Wait-ForServerListen -ProcessRecord $Server -Deadline $Deadline

        $ExpectedResults = [System.Collections.Generic.List[object]]::new()
        $ExpectedResults.Add([pscustomobject]@{
            path = $ServerResultPath
            role = 'server'
            client_id = 0
            process = $Server.process
        })
        for ($ClientId = 1; $ClientId -le $ClientCount; ++$ClientId) {
            $ClientResultPath = Join-Path $TopologyRoot ("client_$ClientId.json")
            $ClientLogPath = Join-Path $TopologyRoot ("client_$ClientId.log")
            $ClientArguments = @(
                "127.0.0.1:$Port",
                '-game',
                '-unattended',
                '-nop4',
                '-nosplash',
                '-nullrhi',
                '-nosound',
                "-AvidScriptNetworkTopology=$Name",
                '-AvidScriptNetworkTopologyRole=client',
                "-AvidScriptNetworkTopologyClientId=$ClientId",
                "-AvidScriptNetworkTopologyExpectedClients=$ClientCount",
                "-AvidScriptNetworkTopologyTimeout=$TimeoutSeconds",
                "-AvidScriptNetworkTopologyResult=$ClientResultPath"
            )
            $Client = Start-TopologyProcess `
                -Name "$Name-client-$ClientId" `
                -Arguments $ClientArguments `
                -LogPath $ClientLogPath
            $ProcessRecords.Add($Client)
            $ExpectedResults.Add([pscustomobject]@{
                path = $ClientResultPath
                role = 'client'
                client_id = $ClientId
                process = $Client.process
            })
        }

        $Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        while ([DateTime]::UtcNow -lt $Deadline) {
            $AllPresent = $true
            foreach ($Expected in $ExpectedResults) {
                if (-not [System.IO.File]::Exists($Expected.path)) {
                    $AllPresent = $false
                    $Expected.process.Refresh()
                    if ($Expected.process.HasExited) {
                        throw "$Name/$($Expected.role)/$($Expected.client_id) exited before writing a result; exit=$($Expected.process.ExitCode)"
                    }
                }
            }
            if ($AllPresent) {
                break
            }
            Start-Sleep -Milliseconds 200
        }
        foreach ($Expected in $ExpectedResults) {
            if (-not [System.IO.File]::Exists($Expected.path)) {
                throw "$Name/$($Expected.role)/$($Expected.client_id) result timed out: $($Expected.path)"
            }
            $Result = Get-Content -Raw -LiteralPath $Expected.path |
                ConvertFrom-Json -Depth 32
            Assert-RoleResult `
                -Result $Result `
                -ExpectedTopology $Name `
                -ExpectedRole $Expected.role `
                -ExpectedClientId $Expected.client_id `
                -ExpectedClients $ClientCount `
                -ExpectedProcessId $Expected.process.Id
            $ResultRecords.Add([ordered]@{
                role = $Expected.role
                client_id = $Expected.client_id
                process_id = $Expected.process.Id
                result_path = $Expected.path
                result_sha256 = Get-FileSha256 $Expected.path
                result = $Result
            })
        }
    }
    finally {
        $ProcessRecordArray = @($ProcessRecords)
        Stop-TopologyProcesses -ProcessRecords $ProcessRecordArray
    }

    $Processes = [System.Collections.Generic.List[object]]::new()
    foreach ($Record in $ProcessRecords) {
        $Processes.Add([ordered]@{
            name = $Record.name
            process_id = $Record.process.Id
            arguments = $Record.arguments
            log_path = $Record.log_path
            log_sha256 = if ([System.IO.File]::Exists($Record.log_path)) {
                Get-FileSha256 $Record.log_path
            }
            else {
                ''
            }
        })
    }
    return [ordered]@{
        topology = $Name
        dedicated = $Dedicated
        port = $Port
        client_count = $ClientCount
        succeeded = $true
        processes = @($Processes)
        results = @($ResultRecords)
    }
}

$ProfileBuild = $null
$TopologyResults = [System.Collections.Generic.List[object]]::new()
try {
    if (-not $SkipProfileBuild) {
        $ProfileBuild = Invoke-ProfileBuild
    }
    if ($Topology -in @('All', 'Dedicated')) {
        $TopologyResults.Add((Invoke-Topology `
            -Name 'dedicated' `
            -Dedicated $true `
            -ClientCount 2))
    }
    if ($Topology -in @('All', 'Listen')) {
        $TopologyResults.Add((Invoke-Topology `
            -Name 'listen' `
            -Dedicated $false `
            -ClientCount 1))
    }
}
catch {
    $FailurePath = Join-Path $RunRoot 'failure.txt'
    [System.IO.File]::WriteAllText($FailurePath, $_.Exception.ToString())
    throw
}

$Aggregate = [ordered]@{
    schema_version = 1
    run_id = $RunId
    succeeded = $true
    engine_root = [System.IO.Path]::GetFullPath($EngineRoot)
    project_path = $ProjectPath
    timeout_seconds = $TimeoutSeconds
    profile_build = $ProfileBuild
    topologies = @($TopologyResults)
}
$AggregatePath = Join-Path $RunRoot 'aggregate.json'
$AggregateJson = $Aggregate | ConvertTo-Json -Depth 64
[System.IO.File]::WriteAllText(
    $AggregatePath,
    $AggregateJson,
    [System.Text.UTF8Encoding]::new($false))
Write-Output "AvidScript network topology: $($TopologyResults.Count)/$($TopologyResults.Count) passed"
Write-Output "Aggregate: $AggregatePath"
Write-Output "Aggregate SHA-256: $(Get-FileSha256 $AggregatePath)"
