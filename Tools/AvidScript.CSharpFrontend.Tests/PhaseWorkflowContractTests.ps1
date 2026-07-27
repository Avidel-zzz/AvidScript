param(
    [string]$Filter = 'All'
)

$ErrorActionPreference = 'Stop'
$TestDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$ToolsRoot = Split-Path -Parent $TestDirectory
$PluginRoot = Split-Path -Parent $ToolsRoot
$CliPath = Join-Path $PluginRoot 'Build\InvokePhaseWorkflow.ps1'
$StateHelperPath = Join-Path $PluginRoot 'Build\PhaseWorkflow\AvidScriptPhaseState.ps1'
$EvidenceHelperPath = Join-Path $PluginRoot 'Build\PhaseWorkflow\AvidScriptPhaseEvidence.ps1'
$RunRoot = Join-Path $PluginRoot 'Saved\AvidScriptPhaseWorkflowTests'
$ExternalEvidenceRoot = 'C:\tmp\AvidScriptPhaseWorkflowContracts'
$Utf8 = [System.Text.UTF8Encoding]::new($false)
$Passed = 0
$Executed = 0
$Failures = [System.Collections.Generic.List[string]]::new()

. $StateHelperPath
. $EvidenceHelperPath

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return Get-AvidScriptFileSha256Hex $Path
}

function Invoke-FixtureGit {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $Output = @(& git -C $RepositoryRoot @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "fixture git failed: git $($Arguments -join ' ') :: $($Output -join ' ')"
    }
    return (($Output | ForEach-Object { [string]$_ }) -join "`n").Trim()
}

function Write-Utf8Text {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [AllowEmptyString()][string]$Text
    )

    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Text, $Utf8)
}

function Write-TestJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $Json = ($Value | ConvertTo-Json -Depth 64) -replace "`r`n?", "`n"
    Write-Utf8Text $Path ($Json + "`n")
}

function New-FixtureRepository {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$Phase = 91
    )

    $Root = Join-Path $RunRoot $Name
    if ([System.IO.Directory]::Exists($Root)) {
        Remove-Item -LiteralPath $Root -Recurse -Force
    }
    [System.IO.Directory]::CreateDirectory($Root) | Out-Null
    Invoke-FixtureGit $Root @('init', '--initial-branch=main') | Out-Null
    Invoke-FixtureGit $Root @('config', 'user.email', 'phase-workflow@example.invalid') | Out-Null
    Invoke-FixtureGit $Root @('config', 'user.name', 'AvidScript Phase Workflow') | Out-Null
    Invoke-FixtureGit $Root @('config', 'core.autocrlf', 'false') | Out-Null

    Write-Utf8Text (Join-Path $Root "Docs\Phase$Phase\Architecture.md") "# Architecture`n"
    Write-Utf8Text (Join-Path $Root "Docs\Phase$Phase\Plan.md") "# Plan`n"
    Write-Utf8Text (Join-Path $Root 'README.md') "# Fixture`n"
    Invoke-FixtureGit $Root @('add', '--', 'Docs', 'README.md') | Out-Null
    Invoke-FixtureGit $Root @('commit', '-m', 'fixture baseline') | Out-Null
    return $Root
}

function Invoke-WorkflowCli {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$PowerShellHost = 'powershell.exe'
    )

    $Output = @(& $PowerShellHost `
        -NoProfile `
        -ExecutionPolicy Bypass `
        -File $CliPath `
        @Arguments `
        -RepositoryRoot $RepositoryRoot 2>&1)
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = (($Output | ForEach-Object { [string]$_ }) -join "`n")
    }
}

function Start-FixturePhase {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [int]$Phase = 91,
        [string[]]$Batches = @('P91.1', 'P91.2')
    )

    $Arguments = [System.Collections.Generic.List[string]]::new()
    foreach ($Value in @(
        'start', '-Phase', [string]$Phase,
        '-Goal', 'Exercise the phase workflow contract',
        '-ArchitecturePath', "Docs/Phase$Phase/Architecture.md",
        '-PlanPath', "Docs/Phase$Phase/Plan.md",
        '-CloseoutPath', "Docs/Phase$Phase/Closeout.md",
        '-BatchId')) {
        $Arguments.Add($Value)
    }
    $Arguments.Add(($Batches -join ','))
    $Result = Invoke-WorkflowCli $RepositoryRoot @($Arguments)
    Assert-Condition ($Result.ExitCode -eq 0) "start failed: $($Result.Output)"
    return Join-Path $RepositoryRoot "Docs\Phase$Phase\Phase${Phase}_State.json"
}

function Commit-Paths {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string[]]$Paths,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $AddArguments = [System.Collections.Generic.List[string]]::new()
    $AddArguments.Add('add')
    $AddArguments.Add('--')
    foreach ($Path in $Paths) {
        $AddArguments.Add($Path)
    }
    Invoke-FixtureGit $RepositoryRoot @($AddArguments) | Out-Null
    Invoke-FixtureGit $RepositoryRoot @('commit', '-m', $Message) | Out-Null
}

function Complete-FixtureBatch {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$Batch,
        [int]$Phase = 91
    )

    $Result = Invoke-WorkflowCli $RepositoryRoot @(
        'batch-complete', '-Phase', [string]$Phase, '-BatchId', $Batch, '-Evidence', "$Batch implementation complete")
    Assert-Condition ($Result.ExitCode -eq 0) "batch completion failed: $($Result.Output)"
}

function Prepare-GateReadyFixture {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$Phase = 91
    )

    $Root = New-FixtureRepository $Name $Phase
    $StatePath = Start-FixturePhase $Root $Phase @("P$Phase.1")
    Complete-FixtureBatch $Root "P$Phase.1" $Phase
    Commit-Paths $Root @("Docs/Phase$Phase/Phase${Phase}_State.json") 'complete phase batch'
    $Freeze = Invoke-WorkflowCli $Root @(
        'freeze', '-Phase', [string]$Phase, '-ReviewEvidence', 'independent review completed')
    Assert-Condition ($Freeze.ExitCode -eq 0) "freeze failed: $($Freeze.Output)"
    Commit-Paths $Root @("Docs/Phase$Phase/Phase${Phase}_State.json") 'freeze gate candidate'
    return [pscustomobject]@{
        Root = $Root
        StatePath = $StatePath
        Phase = $Phase
    }
}

function New-ValidGateReport {
    param(
        [Parameter(Mandatory = $true)]$Fixture,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $EvidenceDirectory = Join-Path $ExternalEvidenceRoot $Name
    if ([System.IO.Directory]::Exists($EvidenceDirectory)) {
        Remove-Item -LiteralPath $EvidenceDirectory -Recurse -Force
    }
    [System.IO.Directory]::CreateDirectory($EvidenceDirectory) | Out-Null
    $LogPath = Join-Path $EvidenceDirectory 'automation.log'
    Write-Utf8Text $LogPath "Automation Test Queue Empty`nTEST COMPLETE`nRequestExitWithStatus(0)`n"
    $Commit = Invoke-FixtureGit $Fixture.Root @('rev-parse', 'HEAD')
    $Tree = Invoke-FixtureGit $Fixture.Root @('rev-parse', 'HEAD^{tree}')
    $Report = [ordered]@{
        schema_version = 1
        producer = [ordered]@{
            name = 'AvidScript.PhaseGate'
            version = '1.0'
        }
        phase_id = $Fixture.Phase
        run_id = "$Name-run"
        verified = [ordered]@{
            commit = $Commit
            tree = $Tree
            state_path = "Docs/Phase$($Fixture.Phase)/Phase$($Fixture.Phase)_State.json"
            state_sha256 = Get-FileSha256 $Fixture.StatePath
        }
        required_check_ids = @('automation')
        checks = @(
            [ordered]@{
                id = 'automation'
                category = 'Automation'
                passed = $true
                exit_code = 0
                completion_marker = 'TEST COMPLETE'
                log_path = $LogPath
                log_sha256 = Get-FileSha256 $LogPath
                counts = [ordered]@{
                    found = 3
                    completed = 3
                    succeeded = 3
                    failed = 0
                    not_run = 0
                    queue_empty = $true
                    test_exit = $true
                    request_exit_status = 0
                    process_exit_code = 0
                }
            }
        )
        invocations = [ordered]@{
            ubt = 0
            automation = 1
            full_gate = 1
        }
        budget_exception_reason = ''
        completed_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
    }
    $ReportPath = Join-Path $EvidenceDirectory 'PhaseGate.json'
    Write-TestJson $ReportPath $Report
    return [pscustomobject]@{
        Path = $ReportPath
        Value = $Report
    }
}

function Assert-CliFailurePreservesState {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$StatePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$ExpectedCode
    )

    $Before = Get-FileSha256 $StatePath
    $Result = Invoke-WorkflowCli $RepositoryRoot $Arguments
    Assert-Condition ($Result.ExitCode -eq 1) "command unexpectedly succeeded: $($Arguments -join ' ')"
    Assert-Condition ($Result.Output.Contains($ExpectedCode)) "missing diagnostic ${ExpectedCode}: $($Result.Output)"
    Assert-Condition ((Get-FileSha256 $StatePath) -ceq $Before) 'rejected command changed phase state bytes'
}

function Invoke-ContractCase {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    if ($Filter -cne 'All' -and $Name.IndexOf($Filter, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        return
    }
    $script:Executed++
    try {
        & $Body
        $script:Passed++
        Write-Output "PASS $Name"
    }
    catch {
        $script:Failures.Add("$Name :: $($_.Exception.Message)")
        Write-Output "FAIL $Name :: $($_.Exception.Message)"
    }
}

if ([System.IO.Directory]::Exists($RunRoot)) {
    Remove-Item -LiteralPath $RunRoot -Recurse -Force
}
[System.IO.Directory]::CreateDirectory($RunRoot) | Out-Null
[System.IO.Directory]::CreateDirectory($ExternalEvidenceRoot) | Out-Null

Invoke-ContractCase 'State.StartAndStatus' {
    $Root = New-FixtureRepository 'StateStart'
    $StatePath = Start-FixturePhase $Root
    $State = Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json
    Assert-Condition ($State.schema_version -eq 1) 'state schema version differs'
    Assert-Condition ($State.declared_stage -ceq 'implementing') 'new phase is not implementing'
    Assert-Condition (@($State.batches).Count -eq 2) 'new phase batch count differs'
    $Status = Invoke-WorkflowCli $Root @('status', '-Phase', '91', '-Json')
    Assert-Condition ($Status.ExitCode -eq 0) "status failed: $($Status.Output)"
    Assert-Condition ($Status.Output.Contains('P91.1')) 'status did not expose next batch'
}

Invoke-ContractCase 'State.PwshPreservesTimestampStrings' {
    $Pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
    if ($null -eq $Pwsh) {
        return
    }

    $Root = New-FixtureRepository 'StatePwshTimestamp'
    $StatePath = Start-FixturePhase $Root
    Complete-FixtureBatch $Root 'P91.1'
    $Before = Get-Content -Raw -LiteralPath $StatePath
    $Match = [regex]::Match(
        $Before,
        '"id":\s*"P91\.1"[\s\S]*?"completed_at_utc":\s*"([^"]+)"')
    Assert-Condition $Match.Success 'first batch timestamp was not serialized'
    $FirstTimestamp = $Match.Groups[1].Value

    $Result = Invoke-WorkflowCli `
        $Root `
        @('batch-complete', '-Phase', '91', '-BatchId', 'P91.2', '-Evidence', 'P91.2 implementation complete') `
        -PowerShellHost $Pwsh.Source
    Assert-Condition ($Result.ExitCode -eq 0) "pwsh batch completion failed: $($Result.Output)"
    $After = Get-Content -Raw -LiteralPath $StatePath
    Assert-Condition `
        ($After.Contains("`"completed_at_utc`": `"$FirstTimestamp`"")) `
        'pwsh changed an existing ISO timestamp while updating phase state'
}

Invoke-ContractCase 'State.DuplicateStartPreservesBytes' {
    $Root = New-FixtureRepository 'StateDuplicateStart'
    $StatePath = Start-FixturePhase $Root
    Assert-CliFailurePreservesState $Root $StatePath @(
        'start', '-Phase', '91', '-Goal', 'duplicate',
        '-ArchitecturePath', 'Docs/Phase91/Architecture.md',
        '-PlanPath', 'Docs/Phase91/Plan.md',
        '-CloseoutPath', 'Docs/Phase91/Closeout.md',
        '-BatchId', 'P91.1') 'ASPW1028'
}

Invoke-ContractCase 'State.MalformedJsonPreservesBytes' {
    $Root = New-FixtureRepository 'StateMalformed'
    $StatePath = Start-FixturePhase $Root
    Write-Utf8Text $StatePath '{ malformed'
    Assert-CliFailurePreservesState $Root $StatePath @('status', '-Phase', '91') 'ASPW1025'
}

Invoke-ContractCase 'State.SchemaVersionPreservesBytes' {
    $Root = New-FixtureRepository 'StateSchema'
    $StatePath = Start-FixturePhase $Root
    $State = Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json
    $State.schema_version = 99
    Write-TestJson $StatePath $State
    Assert-CliFailurePreservesState $Root $StatePath @('status', '-Phase', '91') 'ASPW1003'
}

Invoke-ContractCase 'State.InvalidStagePreservesBytes' {
    $Root = New-FixtureRepository 'StateStage'
    $StatePath = Start-FixturePhase $Root
    $State = Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json
    $State.declared_stage = 'finished'
    Write-TestJson $StatePath $State
    Assert-CliFailurePreservesState $Root $StatePath @('status', '-Phase', '91') 'ASPW1007'
}

Invoke-ContractCase 'State.MissingNextActionPreservesBytes' {
    $Root = New-FixtureRepository 'StateMissingNextAction'
    $StatePath = Start-FixturePhase $Root
    $State = Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json
    $State.PSObject.Properties.Remove('next_action')
    Write-TestJson $StatePath $State
    Assert-CliFailurePreservesState $Root $StatePath @('status', '-Phase', '91') 'ASPW1002'
}

Invoke-ContractCase 'State.PhaseBoundsPreserveBytes' {
    $Root = New-FixtureRepository 'StatePhaseBounds'
    $StatePath = Start-FixturePhase $Root
    $State = Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json
    $State.phase.id = 0
    Write-TestJson $StatePath $State
    Assert-CliFailurePreservesState $Root $StatePath @('status', '-Phase', '91') 'ASPW1004'
}

Invoke-ContractCase 'State.DuplicateDebtPreservesBytes' {
    $Root = New-FixtureRepository 'StateDuplicateDebt'
    $StatePath = Start-FixturePhase $Root
    $Add = Invoke-WorkflowCli $Root @(
        'debt-add', '-Phase', '91', '-DebtId', 'P91-D001', '-Severity', 'Normal',
        '-FoundBatch', 'P91.1', '-Scope', 'workflow', '-Evidence', 'edge case',
        '-DeferralReason', 'aggregate', '-Remediation', 'resolve before close')
    Assert-Condition ($Add.ExitCode -eq 0) "debt add failed: $($Add.Output)"
    $State = Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json
    $State.debt = @($State.debt[0], $State.debt[0])
    Write-TestJson $StatePath $State
    Assert-CliFailurePreservesState $Root $StatePath @('status', '-Phase', '91') 'ASPW2001'
}

Invoke-ContractCase 'State.DuplicateBatchRejected' {
    $Root = New-FixtureRepository 'StateDuplicateBatch'
    $Result = Invoke-WorkflowCli $Root @(
        'start', '-Phase', '91', '-Goal', 'duplicate batches',
        '-ArchitecturePath', 'Docs/Phase91/Architecture.md',
        '-PlanPath', 'Docs/Phase91/Plan.md',
        '-CloseoutPath', 'Docs/Phase91/Closeout.md',
        '-BatchId', 'P91.1,P91.1')
    Assert-Condition ($Result.ExitCode -eq 1) 'duplicate batch start unexpectedly succeeded'
    Assert-Condition ($Result.Output.Contains('ASPW1029')) 'duplicate batch diagnostic differs'
}

Invoke-ContractCase 'State.RepositoryEscapeRejected' {
    $Root = New-FixtureRepository 'StateEscape'
    $Result = Invoke-WorkflowCli $Root @(
        'start', '-Phase', '91', '-Goal', 'escape',
        '-ArchitecturePath', '../Architecture.md',
        '-PlanPath', 'Docs/Phase91/Plan.md',
        '-CloseoutPath', 'Docs/Phase91/Closeout.md',
        '-BatchId', 'P91.1')
    Assert-Condition ($Result.ExitCode -eq 1) 'repository escape unexpectedly succeeded'
    Assert-Condition ($Result.Output.Contains('ASPW4003')) 'repository escape diagnostic differs'
}

Invoke-ContractCase 'Transitions.RevisionAndRejectedFreeze' {
    $Root = New-FixtureRepository 'TransitionRevision'
    $StatePath = Start-FixturePhase $Root
    Assert-CliFailurePreservesState $Root $StatePath @(
        'freeze', '-Phase', '91', '-ReviewEvidence', 'reviewed') 'ASPW1108'
    $Revision1 = (Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json).revision
    Complete-FixtureBatch $Root 'P91.1'
    $Revision2 = (Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json).revision
    Assert-Condition ($Revision2 -eq $Revision1 + 1) 'revision did not increase by one'
}

Invoke-ContractCase 'Transitions.DebtBlocksFreezeUntilVerified' {
    $Root = New-FixtureRepository 'TransitionDebt'
    $StatePath = Start-FixturePhase $Root 91 @('P91.1')
    Complete-FixtureBatch $Root 'P91.1'
    $Add = Invoke-WorkflowCli $Root @(
        'debt-add', '-Phase', '91', '-DebtId', 'P91-D001', '-Severity', 'Important',
        '-FoundBatch', 'P91.1', '-Scope', 'workflow', '-Evidence', 'failure',
        '-DeferralReason', 'batch aggregation', '-Remediation', 'fix before freeze')
    Assert-Condition ($Add.ExitCode -eq 0) "debt add failed: $($Add.Output)"
    $AfterAdd = Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json
    Assert-Condition ($AfterAdd.next_action -ceq
        'debt-update -Phase 91 -DebtId P91-D001 -Status Verified -Evidence <text>') `
        'Important debt must be the next action when it blocks freeze'
    Commit-Paths $Root @('Docs/Phase91/Phase91_State.json') 'record phase debt'
    Assert-CliFailurePreservesState $Root $StatePath @(
        'freeze', '-Phase', '91', '-ReviewEvidence', 'reviewed') 'ASPW2108'
    $Update = Invoke-WorkflowCli $Root @(
        'debt-update', '-Phase', '91', '-DebtId', 'P91-D001', '-Status', 'Verified', '-Evidence', 'fixed')
    Assert-Condition ($Update.ExitCode -eq 0) "debt update failed: $($Update.Output)"
    $Debt = (Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json).debt[0]
    Assert-Condition ($Debt.status -ceq 'Verified') 'debt was not verified'
}

Invoke-ContractCase 'Transitions.CriticalDebtBlocksBatchCompletion' {
    $Root = New-FixtureRepository 'TransitionCriticalDebt'
    $StatePath = Start-FixturePhase $Root 91 @('P91.1')
    $Add = Invoke-WorkflowCli $Root @(
        'debt-add', '-Phase', '91', '-DebtId', 'P91-D001', '-Severity', 'Critical',
        '-FoundBatch', 'P91.1', '-Scope', 'state mutation', '-Evidence', 'unsafe condition',
        '-DeferralReason', 'record before immediate repair', '-Remediation', 'repair contract')
    Assert-Condition ($Add.ExitCode -eq 0) "critical debt add failed: $($Add.Output)"
    Assert-CliFailurePreservesState $Root $StatePath @(
        'batch-complete', '-Phase', '91', '-BatchId', 'P91.1', '-Evidence', 'must reject') 'ASPW2109'
}

Invoke-ContractCase 'Transitions.ProtectedDirtyBaseline' {
    $Root = New-FixtureRepository 'TransitionProtected'
    Write-Utf8Text (Join-Path $Root 'Protected.md') "baseline`n"
    Commit-Paths $Root @('Protected.md') 'add protected file'
    Write-Utf8Text (Join-Path $Root 'Protected.md') "user edit`n"
    Write-Utf8Text (Join-Path $Root 'Private.md') "untracked user edit`n"
    $StatePath = Start-FixturePhase $Root 91 @('P91.1')
    $State = Get-Content -Raw -LiteralPath $StatePath | ConvertFrom-Json
    Assert-Condition (@($State.protected_dirty).Count -eq 2) 'protected dirty baseline count differs'
    Complete-FixtureBatch $Root 'P91.1'
    Commit-Paths $Root @('Docs/Phase91/Phase91_State.json') 'complete with protected baseline'
    Write-Utf8Text (Join-Path $Root 'Protected.md') "changed again`n"
    Assert-CliFailurePreservesState $Root $StatePath @(
        'freeze', '-Phase', '91', '-ReviewEvidence', 'reviewed') 'ASPW4023'
}

Invoke-ContractCase 'Transitions.FreezeAndReopen' {
    $Fixture = Prepare-GateReadyFixture 'TransitionReopen'
    $State = Get-Content -Raw -LiteralPath $Fixture.StatePath | ConvertFrom-Json
    Assert-Condition ($State.declared_stage -ceq 'gate_ready') 'freeze did not reach gate_ready'
    Assert-Condition ($State.review.completed) 'freeze did not record review completion'
    $Reopen = Invoke-WorkflowCli $Fixture.Root @(
        'reopen', '-Phase', '91', '-Reason', 'new implementation issue')
    Assert-Condition ($Reopen.ExitCode -eq 0) "reopen failed: $($Reopen.Output)"
    $Reopened = Get-Content -Raw -LiteralPath $Fixture.StatePath | ConvertFrom-Json
    Assert-Condition ($Reopened.declared_stage -ceq 'implementing') 'reopen did not restore implementing'
    Assert-Condition ($Reopened.batches[0].status -ceq 'Pending') 'reopen did not reopen the last batch'
}

Invoke-ContractCase 'Evidence.HostileReportsPreserveState' {
    $Fixture = Prepare-GateReadyFixture 'EvidenceHostile'
    $Gate = New-ValidGateReport $Fixture 'EvidenceHostile'
    $ValidJson = Get-Content -Raw -LiteralPath $Gate.Path
    $Cases = @(
        @{ Name = 'Tree'; Code = 'ASPW3008'; Mutate = { param($R) $R.verified.tree = ('0' * 40) } },
        @{ Name = 'ExtraField'; Code = 'ASPW3004'; Mutate = { param($R) Add-Member -InputObject $R -NotePropertyName extra -NotePropertyValue 'bad' } },
        @{ Name = 'StateHash'; Code = 'ASPW3009'; Mutate = { param($R) $R.verified.state_sha256 = ('0' * 64) } },
        @{ Name = 'Marker'; Code = 'ASPW3018'; Mutate = { param($R) $R.checks[0].completion_marker = '' } },
        @{ Name = 'Exit'; Code = 'ASPW3017'; Mutate = { param($R) $R.checks[0].exit_code = 7 } },
        @{ Name = 'ZeroTests'; Code = 'ASPW3022'; Mutate = { param($R) $R.checks[0].counts.found = 0; $R.checks[0].counts.completed = 0; $R.checks[0].counts.succeeded = 0 } },
        @{ Name = 'CountMismatch'; Code = 'ASPW3022'; Mutate = { param($R) $R.checks[0].counts.completed = 2 } },
        @{ Name = 'Queue'; Code = 'ASPW3022'; Mutate = { param($R) $R.checks[0].counts.queue_empty = $false } },
        @{ Name = 'TestExit'; Code = 'ASPW3022'; Mutate = { param($R) $R.checks[0].counts.test_exit = $false } },
        @{ Name = 'RequestExit'; Code = 'ASPW3022'; Mutate = { param($R) $R.checks[0].counts.request_exit_status = 1 } },
        @{ Name = 'LogHash'; Code = 'ASPW3020'; Mutate = { param($R) $R.checks[0].log_sha256 = ('0' * 64) } },
        @{ Name = 'Budget'; Code = 'ASPW3027'; Mutate = { param($R) $R.invocations.automation = 3 } }
    )
    foreach ($Case in $Cases) {
        $Report = $ValidJson | ConvertFrom-Json
        & $Case.Mutate $Report
        Write-TestJson $Gate.Path $Report
        Assert-CliFailurePreservesState $Fixture.Root $Fixture.StatePath @(
            'attest', '-Phase', '91', '-GateReportPath', $Gate.Path) $Case.Code
    }
}

Invoke-ContractCase 'Evidence.ValidAttestAndClose' {
    $Fixture = Prepare-GateReadyFixture 'EvidenceClose'
    $Gate = New-ValidGateReport $Fixture 'EvidenceClose'
    $Attest = Invoke-WorkflowCli $Fixture.Root @(
        'attest', '-Phase', '91', '-GateReportPath', $Gate.Path)
    Assert-Condition ($Attest.ExitCode -eq 0) "attest failed: $($Attest.Output)"
    Write-Utf8Text (Join-Path $Fixture.Root 'Docs\Phase91\Closeout.md') "# Closeout`n"
    Commit-Paths $Fixture.Root @(
        'Docs/Phase91/Phase91_State.json',
        'Docs/Phase91/Closeout.md') 'attest phase gate'
    $Close = Invoke-WorkflowCli $Fixture.Root @('close', '-Phase', '91')
    Assert-Condition ($Close.ExitCode -eq 0) "close failed: $($Close.Output)"
    $ClosePath = Get-AvidScriptPhaseCloseEvidencePath $Gate.Path 91
    Assert-Condition ([System.IO.File]::Exists($ClosePath)) 'close evidence was not written'
    $Status = Invoke-WorkflowCli $Fixture.Root @('status', '-Phase', '91', '-Json')
    Assert-Condition ($Status.Output.Contains('closed')) 'status did not derive closed stage'
    $CloseJson = Get-Content -Raw -LiteralPath $ClosePath | ConvertFrom-Json
    $CloseJson.phase_id = 92
    Write-TestJson $ClosePath $CloseJson
    $TamperedStatus = Invoke-WorkflowCli $Fixture.Root @('status', '-Phase', '91', '-Json')
    Assert-Condition ($TamperedStatus.ExitCode -eq 1) 'status accepted tampered close evidence'
    Assert-Condition ($TamperedStatus.Output.Contains('ASPW3044')) 'tampered close diagnostic differs'
}

Invoke-ContractCase 'Evidence.AttestationSourceChangeRejected' {
    $Fixture = Prepare-GateReadyFixture 'EvidenceSourceChange'
    $Gate = New-ValidGateReport $Fixture 'EvidenceSourceChange'
    $Attest = Invoke-WorkflowCli $Fixture.Root @(
        'attest', '-Phase', '91', '-GateReportPath', $Gate.Path)
    Assert-Condition ($Attest.ExitCode -eq 0) "attest failed: $($Attest.Output)"
    Write-Utf8Text (Join-Path $Fixture.Root 'Source\Forbidden.cpp') "int Forbidden = 1;`n"
    Commit-Paths $Fixture.Root @(
        'Docs/Phase91/Phase91_State.json',
        'Source/Forbidden.cpp') 'invalid source attestation'
    $Close = Invoke-WorkflowCli $Fixture.Root @('close', '-Phase', '91')
    Assert-Condition ($Close.ExitCode -eq 1) 'close accepted source changes after Gate'
    Assert-Condition ($Close.Output.Contains('ASPW3032')) 'source attestation diagnostic differs'
}

Invoke-ContractCase 'Evidence.PrivacyDeletedAccountPathIgnored' {
    $Root = New-FixtureRepository 'EvidencePrivacyDeletedPath'
    $PrivatePath = 'C:' + '\Users\Example\file'
    Write-Utf8Text (Join-Path $Root 'Docs\Legacy.md') "legacy path $PrivatePath`n"
    Commit-Paths $Root @('Docs/Legacy.md') 'add legacy local path'
    $StatePath = Start-FixturePhase $Root 91 @('P91.1')
    Write-Utf8Text (Join-Path $Root 'Docs\Legacy.md') "legacy path removed`n"
    Commit-Paths $Root @(
        'Docs/Phase91/Phase91_State.json',
        'Docs/Legacy.md') 'remove legacy local path'
    $State = Read-AvidScriptPhaseState $Root 91
    Assert-Condition (Test-AvidScriptPhasePrivacy $Root $State) 'privacy scan rejected a deleted account path'
}

Invoke-ContractCase 'Evidence.PrivacySchemaTokenIdentifierAllowed' {
    $Root = New-FixtureRepository 'EvidencePrivacySchemaToken'
    $StatePath = Start-FixturePhase $Root 91 @('P91.1')
    Write-Utf8Text `
        (Join-Path $Root 'Tools\SchemaCheck.ps1') `
        ('$RequiredDescriptorSchemaToken = ''$DescriptorSchemaVersion -ne '' + $RequiredDescriptorSchemaVersion' + "`n")
    Commit-Paths $Root @(
        'Docs/Phase91/Phase91_State.json',
        'Tools/SchemaCheck.ps1') 'add schema token identifier'
    $State = Read-AvidScriptPhaseState $Root 91
    Assert-Condition (Test-AvidScriptPhasePrivacy $Root $State) 'privacy scan rejected a non-secret token identifier'
}

Invoke-ContractCase 'Evidence.PrivacyDoublePlusContentRejected' {
    $Root = New-FixtureRepository 'EvidencePrivacyDoublePlus'
    $StatePath = Start-FixturePhase $Root 91 @('P91.1')
    $PrivatePath = 'C:' + '\Users\Example\file'
    Write-Utf8Text (Join-Path $Root 'Docs\Prefixed.md') "++$PrivatePath`n"
    Commit-Paths $Root @(
        'Docs/Phase91/Phase91_State.json',
        'Docs/Prefixed.md') 'add prefixed private path'
    $State = Read-AvidScriptPhaseState $Root 91
    $Diagnostic = ''
    try {
        Test-AvidScriptPhasePrivacy $Root $State | Out-Null
    }
    catch {
        $Diagnostic = $_.Exception.Message
    }
    Assert-Condition ($Diagnostic.Contains('ASPW4031')) 'privacy scan accepted private content beginning with two plus signs'
}

Invoke-ContractCase 'Evidence.PrivacyColoredDiffRejected' {
    $Root = New-FixtureRepository 'EvidencePrivacyColoredDiff'
    $StatePath = Start-FixturePhase $Root 91 @('P91.1')
    $PrivatePath = 'C:' + '\Users\Example\file'
    Write-Utf8Text (Join-Path $Root 'Docs\Colored.md') "private path $PrivatePath`n"
    Commit-Paths $Root @(
        'Docs/Phase91/Phase91_State.json',
        'Docs/Colored.md') 'add private path under colored diff'
    Invoke-FixtureGit $Root @('config', 'color.ui', 'always') | Out-Null
    $State = Read-AvidScriptPhaseState $Root 91
    $Diagnostic = ''
    try {
        Test-AvidScriptPhasePrivacy $Root $State | Out-Null
    }
    catch {
        $Diagnostic = $_.Exception.Message
    }
    Assert-Condition ($Diagnostic.Contains('ASPW4031')) 'privacy scan accepted a private path when Git forced colored output'
}

Invoke-ContractCase 'Evidence.PrivacyBinaryClassifiedFileRejected' {
    $Root = New-FixtureRepository 'EvidencePrivacyBinaryClassified'
    $StatePath = Start-FixturePhase $Root 91 @('P91.1')
    $PrivatePath = 'C:' + '\Users\Example\file'
    Write-Utf8Text (Join-Path $Root '.gitattributes') "Docs/Opaque.md -diff`n"
    Write-Utf8Text (Join-Path $Root 'Docs\Opaque.md') "private path $PrivatePath`n"
    Commit-Paths $Root @(
        '.gitattributes',
        'Docs/Phase91/Phase91_State.json',
        'Docs/Opaque.md') 'add binary-classified private path'
    $State = Read-AvidScriptPhaseState $Root 91
    $Diagnostic = ''
    try {
        Test-AvidScriptPhasePrivacy $Root $State | Out-Null
    }
    catch {
        $Diagnostic = $_.Exception.Message
    }
    Assert-Condition ($Diagnostic.Contains('ASPW4031')) 'privacy scan accepted a private path hidden by a diff attribute'
}

Invoke-ContractCase 'Evidence.PrivacyChangeRejected' {
    $Fixture = Prepare-GateReadyFixture 'EvidencePrivacy'
    $Gate = New-ValidGateReport $Fixture 'EvidencePrivacy'
    $Attest = Invoke-WorkflowCli $Fixture.Root @(
        'attest', '-Phase', '91', '-GateReportPath', $Gate.Path)
    Assert-Condition ($Attest.ExitCode -eq 0) "attest failed: $($Attest.Output)"
    Write-Utf8Text (Join-Path $Fixture.Root 'Docs\Phase91\Closeout.md') "private path C:\Users\Example\file`n"
    Commit-Paths $Fixture.Root @(
        'Docs/Phase91/Phase91_State.json',
        'Docs/Phase91/Closeout.md') 'invalid private attestation'
    $Close = Invoke-WorkflowCli $Fixture.Root @('close', '-Phase', '91')
    Assert-Condition ($Close.ExitCode -eq 1) 'close accepted a private account path'
    Assert-Condition ($Close.Output.Contains('ASPW4031')) 'privacy diagnostic differs'
}

Invoke-ContractCase 'Evidence.AttestationParentRejected' {
    $Fixture = Prepare-GateReadyFixture 'EvidenceParent'
    $Gate = New-ValidGateReport $Fixture 'EvidenceParent'
    $Attest = Invoke-WorkflowCli $Fixture.Root @(
        'attest', '-Phase', '91', '-GateReportPath', $Gate.Path)
    Assert-Condition ($Attest.ExitCode -eq 0) "attest failed: $($Attest.Output)"
    Commit-Paths $Fixture.Root @('Docs/Phase91/Phase91_State.json') 'attest phase gate'
    Write-Utf8Text (Join-Path $Fixture.Root 'Docs\Phase91\Closeout.md') "# Later closeout`n"
    Commit-Paths $Fixture.Root @('Docs/Phase91/Closeout.md') 'extra post-attestation commit'
    $Close = Invoke-WorkflowCli $Fixture.Root @('close', '-Phase', '91')
    Assert-Condition ($Close.ExitCode -eq 1) 'close accepted a non-direct attestation parent'
    Assert-Condition ($Close.Output.Contains('ASPW3030')) 'attestation parent diagnostic differs'
}

if ($Executed -eq 0) {
    Write-Error "No PhaseWorkflow contract cases matched Filter '$Filter'."
    exit 1
}
if ($Failures.Count -gt 0) {
    Write-Output "AvidScript.PhaseWorkflow: $Passed/$Executed passed"
    foreach ($Failure in $Failures) {
        Write-Output " - $Failure"
    }
    exit 1
}

Write-Output "AvidScript.PhaseWorkflow: $Passed/$Executed passed"
exit 0
