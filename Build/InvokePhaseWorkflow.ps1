param(
    [Parameter(Position = 0, Mandatory = $true)][string]$Command,
    [int]$Phase = 0,
    [string]$Goal = '',
    [string]$ArchitecturePath = '',
    [string]$PlanPath = '',
    [string]$CloseoutPath = '',
    [string[]]$BatchId = @(),
    [string]$Evidence = '',
    [string]$DebtId = '',
    [string]$Severity = '',
    [string]$FoundBatch = '',
    [string]$Scope = '',
    [string]$DeferralReason = '',
    [string]$Remediation = '',
    [string]$Status = '',
    [int]$TargetPhase = 0,
    [string]$TransferReason = '',
    [int]$Version = 0,
    [string]$Reason = '',
    [string]$ReviewEvidence = '',
    [string]$GateReportPath = '',
    [int]$UbtBudget = 3,
    [int]$AutomationBudget = 2,
    [int]$FullGateBudget = 2,
    [switch]$Json,
    [string]$RepositoryRoot = ''
)

$ErrorActionPreference = 'Stop'
$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent $ScriptDirectory
}
$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
. (Join-Path $ScriptDirectory 'PhaseWorkflow\AvidScriptPhaseState.ps1')
. (Join-Path $ScriptDirectory 'PhaseWorkflow\AvidScriptPhaseEvidence.ps1')

function Assert-AvidScriptCliText {
    param(
        [AllowEmptyString()][string]$Value,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        Throw-AvidScriptPhaseError 'ASPW1100' "missing required parameter: $Name"
    }
}

function Write-AvidScriptPhaseStatus {
    param(
        [Parameter(Mandatory = $true)]$State,
        [Parameter(Mandatory = $true)][string]$RepositoryRoot
    )

    $EffectiveStage = [string]$State.declared_stage
    $CloseEvidencePath = ''
    if ($State.declared_stage -eq 'gate_attested' -and
        -not [string]::IsNullOrWhiteSpace([string]$State.gate.report_path)) {
        $CloseEvidencePath = Get-AvidScriptPhaseCloseEvidencePath ([string]$State.gate.report_path) ([int]$State.phase.id)
        if ([System.IO.File]::Exists($CloseEvidencePath)) {
            Test-AvidScriptPhaseCloseEvidence $RepositoryRoot $State $CloseEvidencePath | Out-Null
            $EffectiveStage = 'closed'
        }
    }

    $CompletedBatches = @($State.batches | Where-Object { $_.status -eq 'Completed' } | ForEach-Object { $_.id })
    $OpenDebt = @($State.debt | Where-Object { $_.status -in @('Open', 'Fixing') } | ForEach-Object { $_.id })
    $StatusValue = [ordered]@{
        phase = [int]$State.phase.id
        declared_stage = [string]$State.declared_stage
        effective_stage = $EffectiveStage
        completed_batches = $CompletedBatches
        open_debt = $OpenDebt
        revision = [int]$State.revision
        freeze_source_commit = [string]$State.freeze.source_commit
        gate_run_id = [string]$State.gate.run_id
        gate_verified_commit = [string]$State.gate.verified_commit
        close_evidence_path = $CloseEvidencePath
        next_action = if ($EffectiveStage -eq 'closed') { 'none' } else { [string]$State.next_action }
    }

    if ($Json) {
        Write-Output ($StatusValue | ConvertTo-Json -Depth 16)
        return
    }

    Write-Output "Phase: $($StatusValue.phase)"
    Write-Output "Stage: $($StatusValue.effective_stage)"
    Write-Output "Completed batches: $($CompletedBatches -join ', ')"
    Write-Output "Open debt: $($OpenDebt -join ', ')"
    Write-Output "Revision: $($StatusValue.revision)"
    Write-Output "Next: $($StatusValue.next_action)"
}

try {
    if ($Phase -lt 1 -or $Phase -gt 9999) {
        Throw-AvidScriptPhaseError 'ASPW1101' 'Phase must be between 1 and 9999'
    }

    switch ($Command.ToLowerInvariant()) {
        'start' {
            Assert-AvidScriptCliText $Goal 'Goal'
            Assert-AvidScriptCliText $ArchitecturePath 'ArchitecturePath'
            Assert-AvidScriptCliText $PlanPath 'PlanPath'
            Assert-AvidScriptCliText $CloseoutPath 'CloseoutPath'
            $NormalizedBatchIds = @($BatchId | ForEach-Object { $_.Split(',') } | ForEach-Object { $_.Trim() })
            $State = New-AvidScriptInitialPhaseState `
                -RepositoryRoot $RepositoryRoot `
                -Phase $Phase `
                -Goal $Goal `
                -ArchitecturePath $ArchitecturePath `
                -PlanPath $PlanPath `
                -CloseoutPath $CloseoutPath `
                -BatchIds $NormalizedBatchIds `
                -UbtBudget $UbtBudget `
                -AutomationBudget $AutomationBudget `
                -FullGateBudget $FullGateBudget
            Write-AvidScriptPhaseStateAtomic (Get-AvidScriptPhaseStatePath $RepositoryRoot $Phase) $State
            Write-AvidScriptPhaseStatus $State $RepositoryRoot
        }
        'status' {
            Write-AvidScriptPhaseStatus (Read-AvidScriptPhaseState $RepositoryRoot $Phase) $RepositoryRoot
        }
        'batch-complete' {
            Assert-AvidScriptCliText $BatchId[0] 'BatchId'
            Assert-AvidScriptCliText $Evidence 'Evidence'
            $RequestedBatchId = $BatchId[0]
            Update-AvidScriptPhaseState $RepositoryRoot $Phase {
                param($State)
                if ($State.declared_stage -cne 'implementing') {
                    Throw-AvidScriptPhaseError 'ASPW1102' 'batch completion is only valid while implementing'
                }
                foreach ($OpenDebt in @($State.debt)) {
                    if ($OpenDebt.status -in @('Open', 'Fixing') -and
                        $OpenDebt.severity -in @('Blocker', 'Critical')) {
                        Throw-AvidScriptPhaseError 'ASPW2109' "batch completion is blocked by debt: $($OpenDebt.id)"
                    }
                }
                $Batch = @($State.batches | Where-Object { $_.id -ceq $RequestedBatchId })
                if ($Batch.Count -ne 1) {
                    Throw-AvidScriptPhaseError 'ASPW1103' "unknown batch id: $RequestedBatchId"
                }
                if ($Batch[0].status -cne 'Pending') {
                    Throw-AvidScriptPhaseError 'ASPW1104' "batch is already completed: $RequestedBatchId"
                }
                $Batch[0].status = 'Completed'
                $Batch[0].evidence = $Evidence.Trim()
                $Batch[0].completed_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
            } | Out-Null
            Write-AvidScriptPhaseStatus (Read-AvidScriptPhaseState $RepositoryRoot $Phase) $RepositoryRoot
        }
        'debt-add' {
            foreach ($Required in @(
                @{ Value = $DebtId; Name = 'DebtId' },
                @{ Value = $Severity; Name = 'Severity' },
                @{ Value = $FoundBatch; Name = 'FoundBatch' },
                @{ Value = $Scope; Name = 'Scope' },
                @{ Value = $Evidence; Name = 'Evidence' },
                @{ Value = $DeferralReason; Name = 'DeferralReason' },
                @{ Value = $Remediation; Name = 'Remediation' })) {
                Assert-AvidScriptCliText $Required.Value $Required.Name
            }
            if ($Severity -notin @('Blocker', 'Critical', 'Important', 'Normal')) {
                Throw-AvidScriptPhaseError 'ASPW2100' "invalid debt severity: $Severity"
            }
            Update-AvidScriptPhaseState $RepositoryRoot $Phase {
                param($State)
                if ($State.declared_stage -cne 'implementing') {
                    Throw-AvidScriptPhaseError 'ASPW2101' 'debt can only be added while implementing'
                }
                if (@($State.debt | Where-Object { $_.id -ceq $DebtId }).Count -gt 0) {
                    Throw-AvidScriptPhaseError 'ASPW2102' "duplicate debt id: $DebtId"
                }
                if (@($State.batches | Where-Object { $_.id -ceq $FoundBatch }).Count -ne 1) {
                    Throw-AvidScriptPhaseError 'ASPW2103' "unknown debt batch: $FoundBatch"
                }
                $State.debt += [pscustomobject][ordered]@{
                    id = $DebtId
                    severity = $Severity
                    found_batch = $FoundBatch
                    scope = $Scope.Trim()
                    evidence = $Evidence.Trim()
                    deferral_reason = $DeferralReason.Trim()
                    remediation = $Remediation.Trim()
                    status = 'Open'
                    resolution_evidence = ''
                    target_phase = $null
                    transfer_reason = ''
                }
            } | Out-Null
            Write-AvidScriptPhaseStatus (Read-AvidScriptPhaseState $RepositoryRoot $Phase) $RepositoryRoot
        }
        'debt-update' {
            Assert-AvidScriptCliText $DebtId 'DebtId'
            Assert-AvidScriptCliText $Status 'Status'
            if ($Status -notin @('Open', 'Fixing', 'Verified', 'Transferred')) {
                Throw-AvidScriptPhaseError 'ASPW2104' "invalid debt status: $Status"
            }
            if ($Status -eq 'Verified') {
                Assert-AvidScriptCliText $Evidence 'Evidence'
            }
            if ($Status -eq 'Transferred') {
                if ($TargetPhase -lt 1) {
                    Throw-AvidScriptPhaseError 'ASPW2105' 'Transferred debt requires TargetPhase'
                }
                Assert-AvidScriptCliText $TransferReason 'TransferReason'
            }
            Update-AvidScriptPhaseState $RepositoryRoot $Phase {
                param($State)
                if ($State.declared_stage -cne 'implementing') {
                    Throw-AvidScriptPhaseError 'ASPW2106' 'debt updates are only valid while implementing'
                }
                $Debt = @($State.debt | Where-Object { $_.id -ceq $DebtId })
                if ($Debt.Count -ne 1) {
                    Throw-AvidScriptPhaseError 'ASPW2107' "unknown debt id: $DebtId"
                }
                $Debt[0].status = $Status
                if ($Status -eq 'Verified') {
                    $Debt[0].resolution_evidence = $Evidence.Trim()
                    $Debt[0].target_phase = $null
                    $Debt[0].transfer_reason = ''
                }
                elseif ($Status -eq 'Transferred') {
                    $Debt[0].resolution_evidence = $Evidence.Trim()
                    $Debt[0].target_phase = $TargetPhase
                    $Debt[0].transfer_reason = $TransferReason.Trim()
                }
            } | Out-Null
            Write-AvidScriptPhaseStatus (Read-AvidScriptPhaseState $RepositoryRoot $Phase) $RepositoryRoot
        }
        'architecture-revise' {
            Assert-AvidScriptCliText $Reason 'Reason'
            Update-AvidScriptPhaseState $RepositoryRoot $Phase {
                param($State)
                if ($State.declared_stage -cne 'implementing') {
                    Throw-AvidScriptPhaseError 'ASPW1105' 'architecture can only be revised while implementing'
                }
                if ($Version -le [int]$State.architecture.version) {
                    Throw-AvidScriptPhaseError 'ASPW1106' 'architecture version must increase'
                }
                $ArchitectureFull = Resolve-AvidScriptRepositoryPath $RepositoryRoot ([string]$State.architecture.path)
                $State.architecture.version = $Version
                $State.architecture.sha256 = Get-AvidScriptFileSha256Hex $ArchitectureFull
                $State.architecture.revision_reason = $Reason.Trim()
            } | Out-Null
            Write-AvidScriptPhaseStatus (Read-AvidScriptPhaseState $RepositoryRoot $Phase) $RepositoryRoot
        }
        'freeze' {
            Assert-AvidScriptCliText $ReviewEvidence 'ReviewEvidence'
            $Before = Read-AvidScriptPhaseState $RepositoryRoot $Phase
            if ($Before.declared_stage -cne 'implementing') {
                Throw-AvidScriptPhaseError 'ASPW1107' 'freeze is only valid while implementing'
            }
            if (@($Before.batches | Where-Object { $_.status -ne 'Completed' }).Count -gt 0) {
                Throw-AvidScriptPhaseError 'ASPW1108' 'all batches must be completed before freeze'
            }
            foreach ($Debt in @($Before.debt)) {
                if ($Debt.status -in @('Open', 'Fixing') -and $Debt.severity -in @('Blocker', 'Critical', 'Important')) {
                    Throw-AvidScriptPhaseError 'ASPW2108' "freeze is blocked by debt: $($Debt.id)"
                }
            }
            Test-AvidScriptProtectedDirtyBaseline $RepositoryRoot $Before | Out-Null
            $StatePath = Get-AvidScriptPhaseStatePath $RepositoryRoot $Phase
            $InputStateHash = Get-AvidScriptFileSha256Hex $StatePath
            $SourceCommit = Get-AvidScriptGitCommit $RepositoryRoot
            $SourceTree = Get-AvidScriptGitTree $RepositoryRoot $SourceCommit
            Update-AvidScriptPhaseState $RepositoryRoot $Phase {
                param($State)
                $Now = [DateTimeOffset]::UtcNow.ToString('o')
                $State.declared_stage = 'gate_ready'
                $State.review.completed = $true
                $State.review.evidence = $ReviewEvidence.Trim()
                $State.review.completed_at_utc = $Now
                $State.freeze.source_commit = $SourceCommit
                $State.freeze.source_tree = $SourceTree
                $State.freeze.state_sha256 = $InputStateHash
                $State.freeze.review_evidence = $ReviewEvidence.Trim()
                $State.freeze.at_utc = $Now
            } | Out-Null
            Write-AvidScriptPhaseStatus (Read-AvidScriptPhaseState $RepositoryRoot $Phase) $RepositoryRoot
        }
        'attest' {
            Assert-AvidScriptCliText $GateReportPath 'GateReportPath'
            $Before = Read-AvidScriptPhaseState $RepositoryRoot $Phase
            if ($Before.declared_stage -cne 'gate_ready') {
                Throw-AvidScriptPhaseError 'ASPW3100' 'attest requires a gate_ready phase'
            }
            Test-AvidScriptProtectedDirtyBaseline $RepositoryRoot $Before | Out-Null
            $GateEvidence = Test-AvidScriptGateEvidence $RepositoryRoot $Phase $GateReportPath
            $Head = Get-AvidScriptGitCommit $RepositoryRoot
            if ($Head -cne [string]$GateEvidence.Report.verified.commit) {
                Throw-AvidScriptPhaseError 'ASPW3101' 'current HEAD is not the Gate verified commit'
            }
            $CurrentStateHash = Get-AvidScriptFileSha256Hex (Get-AvidScriptPhaseStatePath $RepositoryRoot $Phase)
            if ($CurrentStateHash -cne [string]$GateEvidence.Report.verified.state_sha256) {
                Throw-AvidScriptPhaseError 'ASPW3102' 'current phase state differs from Gate verified state'
            }
            Update-AvidScriptPhaseState $RepositoryRoot $Phase {
                param($State)
                $State.declared_stage = 'gate_attested'
                $State.invocations.ubt = [int]$GateEvidence.Report.invocations.ubt
                $State.invocations.automation = [int]$GateEvidence.Report.invocations.automation
                $State.invocations.full_gate = [int]$GateEvidence.Report.invocations.full_gate
                $State.gate.report_path = $GateEvidence.Path
                $State.gate.report_sha256 = $GateEvidence.Sha256
                $State.gate.run_id = [string]$GateEvidence.Report.run_id
                $State.gate.verified_commit = [string]$GateEvidence.Report.verified.commit
                $State.gate.verified_tree = [string]$GateEvidence.Report.verified.tree
                $State.gate.attestation_parent = [string]$GateEvidence.Report.verified.commit
                $State.gate.attested_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
            } | Out-Null
            Write-AvidScriptPhaseStatus (Read-AvidScriptPhaseState $RepositoryRoot $Phase) $RepositoryRoot
        }
        'close' {
            $Result = Invoke-AvidScriptPhaseClose $RepositoryRoot $Phase
            Write-Output "Phase $Phase closed."
            Write-Output "Close evidence: $($Result.CloseEvidencePath)"
            Write-Output "Attestation commit: $($Result.AttestationCommit)"
        }
        'reopen' {
            Assert-AvidScriptCliText $Reason 'Reason'
            Update-AvidScriptPhaseState $RepositoryRoot $Phase {
                param($State)
                if ($State.declared_stage -notin @('gate_ready', 'gate_attested')) {
                    Throw-AvidScriptPhaseError 'ASPW1109' 'reopen requires gate_ready or gate_attested'
                }
                $LastBatch = @($State.batches | Sort-Object { $_.id } | Select-Object -Last 1)[0]
                $LastBatch.status = 'Pending'
                $LastBatch.evidence = ''
                $LastBatch.completed_at_utc = $null
                $State.declared_stage = 'implementing'
                $State.review.completed = $false
                $State.review.evidence = "reopened: $($Reason.Trim())"
                $State.review.completed_at_utc = $null
                $State.freeze.source_commit = ''
                $State.freeze.source_tree = ''
                $State.freeze.state_sha256 = ''
                $State.freeze.review_evidence = ''
                $State.freeze.at_utc = $null
                $State.gate.report_path = ''
                $State.gate.report_sha256 = ''
                $State.gate.run_id = ''
                $State.gate.verified_commit = ''
                $State.gate.verified_tree = ''
                $State.gate.attestation_parent = ''
                $State.gate.attested_at_utc = $null
            } | Out-Null
            Write-AvidScriptPhaseStatus (Read-AvidScriptPhaseState $RepositoryRoot $Phase) $RepositoryRoot
        }
        default {
            Throw-AvidScriptPhaseError 'ASPW1109' "unknown phase workflow command: $Command"
        }
    }
    exit 0
}
catch {
    $Message = [string]$_.Exception.Message
    $Message = ($Message -replace '[\r\n]+', ' ').Trim()
    Write-Output "ERROR $Message"
    exit 1
}
