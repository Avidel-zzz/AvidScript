function Read-AvidScriptGateEvidence {
    param([Parameter(Mandatory = $true)][string]$Path)

    $FullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not [System.IO.File]::Exists($FullPath)) {
        Throw-AvidScriptPhaseError 'ASPW3001' "gate report does not exist: $Path"
    }
    try {
        $Evidence = [System.IO.File]::ReadAllText($FullPath) | ConvertFrom-Json
    }
    catch {
        Throw-AvidScriptPhaseError 'ASPW3002' 'gate report is not valid JSON'
    }

    return [pscustomobject]@{
        Path = $FullPath
        Sha256 = Get-AvidScriptFileSha256Hex $FullPath
        Value = $Evidence
    }
}

function Assert-AvidScriptExternalEvidencePath {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $Root = Get-AvidScriptRepositoryRoot $RepositoryRoot
    $FullPath = [System.IO.Path]::GetFullPath($Path)
    $RootPrefix = $Root + [System.IO.Path]::DirectorySeparatorChar
    if ($FullPath.StartsWith($RootPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
        $FullPath.Equals($Root, [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-AvidScriptPhaseError 'ASPW3040' "$Label must be stored outside the repository"
    }
    return $FullPath
}

function Get-AvidScriptCommittedText {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$Commit,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $Result = Invoke-AvidScriptGit $RepositoryRoot @('show', "${Commit}:$RelativePath") -AllowFailure
    if ($Result.ExitCode -ne 0) {
        Throw-AvidScriptPhaseError 'ASPW3003' "verified commit does not contain $RelativePath"
    }
    return $Result.Text + "`n"
}

function Test-AvidScriptGateEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][int]$Phase,
        [Parameter(Mandatory = $true)][string]$GateReportPath
    )

    Assert-AvidScriptExternalEvidencePath $RepositoryRoot $GateReportPath 'gate report' | Out-Null
    $ReportFile = Read-AvidScriptGateEvidence $GateReportPath
    $Report = $ReportFile.Value
    $ReportProperties = @(
        'schema_version', 'producer', 'phase_id', 'run_id', 'verified', 'required_check_ids',
        'checks', 'invocations', 'budget_exception_reason', 'completed_at_utc')
    Assert-AvidScriptPropertySet $Report $ReportProperties 'ASPW3004' 'gate report'
    Assert-AvidScriptPropertySet $Report.producer @('name', 'version') 'ASPW3004' 'gate producer'
    Assert-AvidScriptPropertySet $Report.verified @(
        'commit', 'tree', 'state_path', 'state_sha256') 'ASPW3004' 'gate verified identity'
    Assert-AvidScriptPropertySet $Report.invocations @(
        'ubt', 'automation', 'full_gate') 'ASPW3004' 'gate invocations'
    if ([int]$Report.schema_version -ne 1 -or
        [string]$Report.producer.name -cne 'AvidScript.PhaseGate' -or
        [string]$Report.producer.version -cne '1.0') {
        Throw-AvidScriptPhaseError 'ASPW3005' 'unsupported gate evidence producer or schema'
    }
    if ([int]$Report.phase_id -ne $Phase) {
        Throw-AvidScriptPhaseError 'ASPW3006' "gate evidence phase mismatch: $($Report.phase_id)"
    }
    Assert-AvidScriptIdentifier ([string]$Report.run_id) 'gate run id'
    Assert-AvidScriptGitHash ([string]$Report.verified.commit) 'gate verified commit'
    Assert-AvidScriptGitHash ([string]$Report.verified.tree) 'gate verified tree'
    Assert-AvidScriptSha256 ([string]$Report.verified.state_sha256) 'gate verified state hash'

    $ExpectedStatePath = Get-AvidScriptPhaseStateRelativePath $Phase
    if ([string]$Report.verified.state_path -cne $ExpectedStatePath) {
        Throw-AvidScriptPhaseError 'ASPW3007' 'gate evidence references the wrong phase state path'
    }
    $ActualTree = Get-AvidScriptGitTree $RepositoryRoot ([string]$Report.verified.commit)
    if ($ActualTree -cne [string]$Report.verified.tree) {
        Throw-AvidScriptPhaseError 'ASPW3008' 'gate evidence tree does not match the verified commit'
    }

    $CommittedStateText = Get-AvidScriptCommittedText `
        -RepositoryRoot $RepositoryRoot `
        -Commit ([string]$Report.verified.commit) `
        -RelativePath $ExpectedStatePath
    $CommittedStateHash = Get-AvidScriptSha256HexFromText $CommittedStateText
    if ($CommittedStateHash -cne [string]$Report.verified.state_sha256) {
        Throw-AvidScriptPhaseError 'ASPW3009' 'gate evidence state hash does not match the verified commit'
    }
    try {
        $VerifiedState = $CommittedStateText | ConvertFrom-Json
    }
    catch {
        Throw-AvidScriptPhaseError 'ASPW3010' 'verified phase state is not valid JSON'
    }
    Test-AvidScriptPhaseState $VerifiedState $RepositoryRoot $Phase | Out-Null
    if ($VerifiedState.declared_stage -cne 'gate_ready') {
        Throw-AvidScriptPhaseError 'ASPW3011' 'verified phase state is not gate_ready'
    }

    $RequiredIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($RequiredId in @($Report.required_check_ids)) {
        Assert-AvidScriptIdentifier ([string]$RequiredId) 'required check id'
        if (-not $RequiredIds.Add([string]$RequiredId)) {
            Throw-AvidScriptPhaseError 'ASPW3012' "duplicate required check id: $RequiredId"
        }
    }
    if ($RequiredIds.Count -eq 0) {
        Throw-AvidScriptPhaseError 'ASPW3013' 'gate evidence has no required checks'
    }

    $ObservedIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($Check in @($Report.checks)) {
        Assert-AvidScriptPropertySet $Check @(
            'id', 'category', 'passed', 'exit_code', 'completion_marker', 'log_path',
            'log_sha256', 'counts') 'ASPW3004' 'gate check'
        Assert-AvidScriptIdentifier ([string]$Check.id) 'gate check id'
        if (-not $ObservedIds.Add([string]$Check.id)) {
            Throw-AvidScriptPhaseError 'ASPW3014' "duplicate gate check id: $($Check.id)"
        }
        if (-not $RequiredIds.Contains([string]$Check.id)) {
            Throw-AvidScriptPhaseError 'ASPW3015' "gate evidence contains undeclared check: $($Check.id)"
        }
        if ($Check.category -notin @('Static', 'DotNet', 'PowerShell', 'Build', 'Automation', 'Performance')) {
            Throw-AvidScriptPhaseError 'ASPW3016' "gate check has invalid category: $($Check.id)"
        }
        if (-not [bool]$Check.passed -or [int]$Check.exit_code -ne 0) {
            Throw-AvidScriptPhaseError 'ASPW3017' "gate check did not pass: $($Check.id)"
        }
        if ([string]::IsNullOrWhiteSpace([string]$Check.completion_marker)) {
            Throw-AvidScriptPhaseError 'ASPW3018' "gate check lacks a completion marker: $($Check.id)"
        }

        $LogPath = Assert-AvidScriptExternalEvidencePath $RepositoryRoot ([string]$Check.log_path) 'gate check log'
        if (-not [System.IO.File]::Exists($LogPath)) {
            Throw-AvidScriptPhaseError 'ASPW3019' "gate check log is missing: $($Check.id)"
        }
        Assert-AvidScriptSha256 ([string]$Check.log_sha256) "gate check $($Check.id) log hash"
        if ((Get-AvidScriptFileSha256Hex $LogPath) -cne [string]$Check.log_sha256) {
            Throw-AvidScriptPhaseError 'ASPW3020' "gate check log hash differs: $($Check.id)"
        }

        if ($Check.category -eq 'Automation') {
            if ($null -eq $Check.counts) {
                Throw-AvidScriptPhaseError 'ASPW3021' "Automation check lacks counts: $($Check.id)"
            }
            $Counts = $Check.counts
            Assert-AvidScriptPropertySet $Counts @(
                'found', 'completed', 'succeeded', 'failed', 'not_run', 'queue_empty',
                'test_exit', 'request_exit_status', 'process_exit_code') 'ASPW3004' 'Automation counts'
            if ([int]$Counts.found -le 0 -or
                [int]$Counts.found -ne [int]$Counts.completed -or
                [int]$Counts.completed -ne [int]$Counts.succeeded -or
                [int]$Counts.failed -ne 0 -or
                [int]$Counts.not_run -ne 0 -or
                -not [bool]$Counts.queue_empty -or
                -not [bool]$Counts.test_exit -or
                [int]$Counts.request_exit_status -ne 0 -or
                [int]$Counts.process_exit_code -ne 0) {
                Throw-AvidScriptPhaseError 'ASPW3022' "Automation completion contract failed: $($Check.id)"
            }
        }
        elseif ($null -ne $Check.counts) {
            Throw-AvidScriptPhaseError 'ASPW3023' "non-Automation check must not publish Automation counts: $($Check.id)"
        }
    }

    if ($ObservedIds.Count -ne $RequiredIds.Count) {
        Throw-AvidScriptPhaseError 'ASPW3024' 'gate evidence is missing one or more required checks'
    }
    foreach ($RequiredId in $RequiredIds) {
        if (-not $ObservedIds.Contains($RequiredId)) {
            Throw-AvidScriptPhaseError 'ASPW3025' "gate evidence is missing required check: $RequiredId"
        }
    }

    $BudgetExceeded = $false
    foreach ($CounterName in @('ubt', 'automation', 'full_gate')) {
        if ([int]$Report.invocations.$CounterName -lt 0) {
            Throw-AvidScriptPhaseError 'ASPW3026' "gate invocation count is negative: $CounterName"
        }
        if ([int]$Report.invocations.$CounterName -gt [int]$VerifiedState.budgets.$CounterName) {
            $BudgetExceeded = $true
        }
    }
    if ($BudgetExceeded -and [string]::IsNullOrWhiteSpace([string]$Report.budget_exception_reason)) {
        Throw-AvidScriptPhaseError 'ASPW3027' 'gate invocation budget was exceeded without a risk reason'
    }

    return [pscustomobject]@{
        Path = $ReportFile.Path
        Sha256 = $ReportFile.Sha256
        Report = $Report
        VerifiedState = $VerifiedState
    }
}

function Test-AvidScriptProtectedDirtyBaseline {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]$State
    )

    $ActualStatus = Invoke-AvidScriptGit $RepositoryRoot @('status', '--porcelain=v1', '--untracked-files=all')
    $ActualByPath = @{}
    foreach ($Line in @($ActualStatus.Text -split "`n")) {
        if ([string]::IsNullOrWhiteSpace($Line)) {
            continue
        }
        if ($Line.Length -lt 4 -or $Line.Substring(2, 1) -ne ' ') {
            Throw-AvidScriptPhaseError 'ASPW4020' 'unsupported git porcelain entry during close'
        }
        $ActualByPath[(ConvertTo-AvidScriptRepositoryPath $Line.Substring(3))] = $Line.Substring(0, 2)
    }

    if ($ActualByPath.Count -ne @($State.protected_dirty).Count) {
        Throw-AvidScriptPhaseError 'ASPW4021' 'worktree contains changes outside the protected dirty baseline'
    }
    foreach ($Entry in @($State.protected_dirty)) {
        if (-not $ActualByPath.ContainsKey([string]$Entry.path) -or
            [string]$ActualByPath[[string]$Entry.path] -cne [string]$Entry.status) {
            Throw-AvidScriptPhaseError 'ASPW4022' "protected dirty status changed: $($Entry.path)"
        }
        $FullPath = Resolve-AvidScriptRepositoryPath $RepositoryRoot ([string]$Entry.path)
        if ((Get-AvidScriptFileSha256Hex $FullPath) -cne [string]$Entry.worktree_sha256) {
            Throw-AvidScriptPhaseError 'ASPW4023' "protected dirty content changed: $($Entry.path)"
        }
        if ((Get-AvidScriptHeadContentSha256 $RepositoryRoot ([string]$Entry.path)) -cne [string]$Entry.head_blob_sha256) {
            Throw-AvidScriptPhaseError 'ASPW4024' "protected dirty HEAD baseline changed: $($Entry.path)"
        }
        $CommittedDiff = Invoke-AvidScriptGit $RepositoryRoot @(
            'diff', '--name-only', "$($State.phase.start_commit)..HEAD", '--', [string]$Entry.path)
        if (-not [string]::IsNullOrWhiteSpace($CommittedDiff.Text)) {
            Throw-AvidScriptPhaseError 'ASPW4025' "protected dirty path entered the phase commit range: $($Entry.path)"
        }
    }

    $IndexDiff = Invoke-AvidScriptGit $RepositoryRoot @('diff', '--cached', '--name-only')
    if (-not [string]::IsNullOrWhiteSpace($IndexDiff.Text)) {
        Throw-AvidScriptPhaseError 'ASPW4026' 'Git index must be empty during close'
    }

    return $true
}

function Test-AvidScriptPhasePrivacy {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]$State
    )

    $Range = "$($State.phase.start_commit)..HEAD"
    $Names = Invoke-AvidScriptGit $RepositoryRoot @('diff', '--name-only', $Range)
    foreach ($Name in @($Names.Text -split "`n")) {
        if ([string]::IsNullOrWhiteSpace($Name)) {
            continue
        }
        $Normalized = ConvertTo-AvidScriptRepositoryPath $Name
        if ($Normalized -match '(^|/)(Saved|Binaries|Intermediate|DerivedDataCache|TestResults|artifacts|coverage)/' -or
            $Normalized -match '(?i)\.(pem|key|pfx|p12|snk)$' -or
            $Normalized -match '(^|/)\.env($|\.)') {
            Throw-AvidScriptPhaseError 'ASPW4030' "phase commit range contains a forbidden path: $Normalized"
        }
    }

    $Patch = (Invoke-AvidScriptGit $RepositoryRoot @('diff', '--no-ext-diff', '--unified=0', $Range)).Text
    foreach ($Pattern in @(
        '(?i)[A-Z]:[\\/]+Users[\\/]+',
        '-----BEGIN [A-Z ]*PRIVATE KEY-----',
        '(?i)(password|secret|token)\s*[:=]\s*["''][^"'']{8,}["'']',
        'gh[pousr]_[A-Za-z0-9_]{20,}'
    )) {
        if ($Patch -match $Pattern) {
            Throw-AvidScriptPhaseError 'ASPW4031' 'phase commit range failed privacy scanning'
        }
    }

    $Whitespace = Invoke-AvidScriptGit $RepositoryRoot @('diff', '--check', $Range) -AllowFailure
    if ($Whitespace.ExitCode -ne 0) {
        Throw-AvidScriptPhaseError 'ASPW4032' 'phase commit range failed whitespace validation'
    }

    return $true
}

function Test-AvidScriptAttestationDiff {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]$State,
        [Parameter(Mandatory = $true)]$GateReport
    )

    $Head = Get-AvidScriptGitCommit $RepositoryRoot
    $Parents = (Invoke-AvidScriptGit $RepositoryRoot @('rev-list', '--parents', '-n', '1', $Head)).Text.Split(' ')
    if ($Parents.Count -ne 2 -or $Parents[1] -cne [string]$GateReport.verified.commit) {
        Throw-AvidScriptPhaseError 'ASPW3030' 'attestation commit parent is not the Gate verified commit'
    }
    if ([string]$State.gate.attestation_parent -cne [string]$GateReport.verified.commit) {
        Throw-AvidScriptPhaseError 'ASPW3031' 'phase state attestation parent differs from Gate evidence'
    }

    $AllowedPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $AllowedPaths.Add((Get-AvidScriptPhaseStateRelativePath ([int]$State.phase.id))) | Out-Null
    $AllowedPaths.Add([string]$State.documents.closeout) | Out-Null
    $AllowedPaths.Add("Docs/Phase$($State.phase.id)/P$($State.phase.id)_Gate_Summary.json") | Out-Null

    $DiffNames = Invoke-AvidScriptGit $RepositoryRoot @(
        'diff', '--name-only', "$($GateReport.verified.commit)..$Head")
    foreach ($Name in @($DiffNames.Text -split "`n")) {
        if ([string]::IsNullOrWhiteSpace($Name)) {
            continue
        }
        $Normalized = ConvertTo-AvidScriptRepositoryPath $Name
        if (-not $AllowedPaths.Contains($Normalized)) {
            Throw-AvidScriptPhaseError 'ASPW3032' "attestation commit changed a forbidden path: $Normalized"
        }
    }

    return @($DiffNames.Text -split "`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
}

function Get-AvidScriptPhaseCloseEvidencePath {
    param(
        [Parameter(Mandatory = $true)][string]$GateReportPath,
        [Parameter(Mandatory = $true)][int]$Phase
    )

    return Join-Path (Split-Path -Parent ([System.IO.Path]::GetFullPath($GateReportPath))) "Phase${Phase}_Close.json"
}

function Test-AvidScriptPhaseCloseEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)]$State,
        [Parameter(Mandatory = $true)][string]$Path
    )

    Assert-AvidScriptExternalEvidencePath $RepositoryRoot $Path 'close evidence' | Out-Null
    if (-not [System.IO.File]::Exists($Path)) {
        Throw-AvidScriptPhaseError 'ASPW3041' "close evidence does not exist: $Path"
    }
    try {
        $Close = [System.IO.File]::ReadAllText($Path) | ConvertFrom-Json
    }
    catch {
        Throw-AvidScriptPhaseError 'ASPW3042' 'close evidence is not valid JSON'
    }
    Assert-AvidScriptPropertySet $Close @(
        'schema_version', 'phase_id', 'run_id', 'gate_report_sha256', 'verified_commit',
        'attestation_commit', 'attestation_tree', 'attestation_paths', 'protected_dirty_count',
        'privacy_passed', 'closed_at_utc') 'ASPW3043' 'close evidence'
    if ([int]$Close.schema_version -ne 1 -or
        [int]$Close.phase_id -ne [int]$State.phase.id -or
        [string]$Close.run_id -cne [string]$State.gate.run_id -or
        [string]$Close.gate_report_sha256 -cne [string]$State.gate.report_sha256 -or
        [string]$Close.verified_commit -cne [string]$State.gate.verified_commit -or
        -not [bool]$Close.privacy_passed) {
        Throw-AvidScriptPhaseError 'ASPW3044' 'close evidence identity differs from phase state'
    }
    Assert-AvidScriptGitHash ([string]$Close.attestation_commit) 'close attestation commit'
    Assert-AvidScriptGitHash ([string]$Close.attestation_tree) 'close attestation tree'
    $Head = Get-AvidScriptGitCommit $RepositoryRoot
    if ([string]$Close.attestation_commit -cne $Head -or
        [string]$Close.attestation_tree -cne (Get-AvidScriptGitTree $RepositoryRoot $Head)) {
        Throw-AvidScriptPhaseError 'ASPW3045' 'close evidence does not identify the current attestation commit'
    }
    if ([int]$Close.protected_dirty_count -ne @($State.protected_dirty).Count) {
        Throw-AvidScriptPhaseError 'ASPW3046' 'close evidence protected dirty count differs'
    }
    return $Close
}

function Write-AvidScriptJsonAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $Directory = Split-Path -Parent $Path
    [System.IO.Directory]::CreateDirectory($Directory) | Out-Null
    $TemporaryPath = Join-Path $Directory ('.' + [System.IO.Path]::GetFileName($Path) + '.' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        $Json = $Value | ConvertTo-Json -Depth 64
        $Json = $Json -replace "`r`n?", "`n"
        [System.IO.File]::WriteAllText($TemporaryPath, $Json + "`n", $script:AvidScriptPhaseUtf8)
        if ([System.IO.File]::Exists($Path)) {
            Throw-AvidScriptPhaseError 'ASPW3033' "immutable evidence already exists: $Path"
        }
        [System.IO.File]::Move($TemporaryPath, $Path)
    }
    finally {
        if ([System.IO.File]::Exists($TemporaryPath)) {
            [System.IO.File]::Delete($TemporaryPath)
        }
    }
}

function Invoke-AvidScriptPhaseClose {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][int]$Phase
    )

    $State = Read-AvidScriptPhaseState $RepositoryRoot $Phase
    if ($State.declared_stage -cne 'gate_attested') {
        Throw-AvidScriptPhaseError 'ASPW3034' 'phase must be gate_attested before close'
    }
    foreach ($Debt in @($State.debt)) {
        if ($Debt.status -in @('Open', 'Fixing') -and $Debt.severity -in @('Blocker', 'Critical', 'Important')) {
            Throw-AvidScriptPhaseError 'ASPW3035' "phase has unresolved closing debt: $($Debt.id)"
        }
        if ($Debt.severity -eq 'Normal' -and $Debt.status -notin @('Verified', 'Transferred')) {
            Throw-AvidScriptPhaseError 'ASPW3036' "Normal debt must be verified or transferred: $($Debt.id)"
        }
    }

    $Evidence = Test-AvidScriptGateEvidence $RepositoryRoot $Phase ([string]$State.gate.report_path)
    if ($Evidence.Sha256 -cne [string]$State.gate.report_sha256 -or
        [string]$Evidence.Report.run_id -cne [string]$State.gate.run_id -or
        [string]$Evidence.Report.verified.commit -cne [string]$State.gate.verified_commit -or
        [string]$Evidence.Report.verified.tree -cne [string]$State.gate.verified_tree) {
        Throw-AvidScriptPhaseError 'ASPW3037' 'attested Gate identity differs from immutable evidence'
    }

    $AttestationPaths = @(Test-AvidScriptAttestationDiff $RepositoryRoot $State $Evidence.Report)
    Test-AvidScriptProtectedDirtyBaseline $RepositoryRoot $State | Out-Null
    Test-AvidScriptPhasePrivacy $RepositoryRoot $State | Out-Null

    $Head = Get-AvidScriptGitCommit $RepositoryRoot
    $ClosePath = Get-AvidScriptPhaseCloseEvidencePath $Evidence.Path $Phase
    $CloseEvidence = [ordered]@{
        schema_version = 1
        phase_id = $Phase
        run_id = [string]$Evidence.Report.run_id
        gate_report_sha256 = [string]$State.gate.report_sha256
        verified_commit = [string]$Evidence.Report.verified.commit
        attestation_commit = $Head
        attestation_tree = Get-AvidScriptGitTree $RepositoryRoot $Head
        attestation_paths = $AttestationPaths
        protected_dirty_count = @($State.protected_dirty).Count
        privacy_passed = $true
        closed_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
    }
    Write-AvidScriptJsonAtomic $ClosePath $CloseEvidence
    Test-AvidScriptPhaseCloseEvidence $RepositoryRoot $State $ClosePath | Out-Null
    return [pscustomobject]@{
        Stage = 'closed'
        CloseEvidencePath = $ClosePath
        AttestationCommit = $Head
    }
}
