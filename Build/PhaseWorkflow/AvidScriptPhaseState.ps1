$script:AvidScriptPhaseStateSchemaVersion = 1
$script:AvidScriptPhaseUtf8 = [System.Text.UTF8Encoding]::new($false)

function ConvertFrom-AvidScriptJson {
    param([Parameter(Mandatory = $true)][string]$Json)

    $ConvertCommand = Get-Command ConvertFrom-Json -CommandType Cmdlet
    if ($ConvertCommand.Parameters.ContainsKey('DateKind')) {
        return $Json | ConvertFrom-Json -DateKind String
    }
    return $Json | ConvertFrom-Json
}

function Throw-AvidScriptPhaseError {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message
    )

    throw "${Code}: $Message"
}

function Get-AvidScriptRepositoryRoot {
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

    $FullRoot = [System.IO.Path]::GetFullPath($RepositoryRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    if (-not [System.IO.Directory]::Exists($FullRoot)) {
        Throw-AvidScriptPhaseError 'ASPW4001' "repository root does not exist: $RepositoryRoot"
    }

    return $FullRoot
}

function ConvertTo-AvidScriptRepositoryPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return $Path.Replace([System.IO.Path]::DirectorySeparatorChar, '/').Replace(
        [System.IO.Path]::AltDirectorySeparatorChar,
        '/')
}

function Resolve-AvidScriptRepositoryPath {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [switch]$AllowMissing
    )

    $Root = Get-AvidScriptRepositoryRoot $RepositoryRoot
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or [System.IO.Path]::IsPathRooted($RelativePath)) {
        Throw-AvidScriptPhaseError 'ASPW4002' "path must be repository-relative: $RelativePath"
    }

    $NormalizedRelative = ConvertTo-AvidScriptRepositoryPath $RelativePath
    foreach ($Segment in $NormalizedRelative.Split('/')) {
        if ([string]::IsNullOrWhiteSpace($Segment) -or $Segment -eq '.' -or $Segment -eq '..') {
            Throw-AvidScriptPhaseError 'ASPW4003' "path contains an unsafe segment: $RelativePath"
        }
    }

    $FullPath = [System.IO.Path]::GetFullPath((Join-Path $Root $NormalizedRelative))
    $RootPrefix = $Root + [System.IO.Path]::DirectorySeparatorChar
    if (-not $FullPath.StartsWith($RootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-AvidScriptPhaseError 'ASPW4004' "path escapes repository root: $RelativePath"
    }

    $Cursor = $Root
    foreach ($Segment in $NormalizedRelative.Split('/')) {
        $Cursor = Join-Path $Cursor $Segment
        if (-not [System.IO.File]::Exists($Cursor) -and -not [System.IO.Directory]::Exists($Cursor)) {
            break
        }

        $Item = Get-Item -LiteralPath $Cursor -Force
        if (($Item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            Throw-AvidScriptPhaseError 'ASPW4005' "path crosses a reparse point: $RelativePath"
        }
    }

    if (-not $AllowMissing -and
        -not [System.IO.File]::Exists($FullPath) -and
        -not [System.IO.Directory]::Exists($FullPath)) {
        Throw-AvidScriptPhaseError 'ASPW4006' "repository path does not exist: $RelativePath"
    }

    return $FullPath
}

function Get-AvidScriptPhaseStateRelativePath {
    param([Parameter(Mandatory = $true)][int]$Phase)

    if ($Phase -lt 1 -or $Phase -gt 9999) {
        Throw-AvidScriptPhaseError 'ASPW1001' "phase must be between 1 and 9999: $Phase"
    }

    return "Docs/Phase$Phase/Phase${Phase}_State.json"
}

function Get-AvidScriptPhaseStatePath {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][int]$Phase
    )

    return Resolve-AvidScriptRepositoryPath `
        -RepositoryRoot $RepositoryRoot `
        -RelativePath (Get-AvidScriptPhaseStateRelativePath $Phase) `
        -AllowMissing
}

function Get-AvidScriptSha256HexFromBytes {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    $Hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($Hasher.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $Hasher.Dispose()
    }
}

function Get-AvidScriptSha256HexFromText {
    param([AllowEmptyString()][string]$Text)

    return Get-AvidScriptSha256HexFromBytes $script:AvidScriptPhaseUtf8.GetBytes($Text)
}

function Get-AvidScriptFileSha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not [System.IO.File]::Exists($Path)) {
        Throw-AvidScriptPhaseError 'ASPW4007' "file does not exist for hashing: $Path"
    }

    return Get-AvidScriptSha256HexFromBytes ([System.IO.File]::ReadAllBytes($Path))
}

function Invoke-AvidScriptGit {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )

    $Root = Get-AvidScriptRepositoryRoot $RepositoryRoot
    $PreviousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $Output = @(& git -C $Root @Arguments 2>&1)
        $ExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $PreviousErrorActionPreference
    }
    $Text = (($Output | ForEach-Object { [string]$_ }) -join "`n")
    if ($ExitCode -ne 0 -and -not $AllowFailure) {
        Throw-AvidScriptPhaseError 'ASPW4008' "git command failed with exit $ExitCode"
    }

    return [pscustomobject]@{
        ExitCode = $ExitCode
        Text = $Text
    }
}

function Get-AvidScriptGitCommit {
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

    return (Invoke-AvidScriptGit $RepositoryRoot @('rev-parse', 'HEAD')).Text.ToLowerInvariant()
}

function Get-AvidScriptGitTree {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [string]$Commit = 'HEAD'
    )

    return (Invoke-AvidScriptGit $RepositoryRoot @('rev-parse', "$Commit^{tree}")).Text.ToLowerInvariant()
}

function Get-AvidScriptHeadContentSha256 {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $Result = Invoke-AvidScriptGit $RepositoryRoot @('show', "HEAD:$RelativePath") -AllowFailure
    if ($Result.ExitCode -ne 0) {
        return ''
    }

    return Get-AvidScriptSha256HexFromText ($Result.Text + "`n")
}

function Get-AvidScriptProtectedDirtyBaseline {
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

    $Status = Invoke-AvidScriptGit $RepositoryRoot @('status', '--porcelain=v1', '--untracked-files=all')
    $Entries = [System.Collections.Generic.List[object]]::new()
    foreach ($Line in @($Status.Text -split "`n")) {
        if ([string]::IsNullOrWhiteSpace($Line)) {
            continue
        }
        if ($Line.Length -lt 4 -or $Line.Substring(2, 1) -ne ' ') {
            Throw-AvidScriptPhaseError 'ASPW4009' 'unsupported git porcelain entry in protected dirty baseline'
        }

        $RelativePath = ConvertTo-AvidScriptRepositoryPath $Line.Substring(3)
        if ($RelativePath.Contains(' -> ')) {
            Throw-AvidScriptPhaseError 'ASPW4010' 'rename entries are not supported in protected dirty baseline'
        }

        $FullPath = Resolve-AvidScriptRepositoryPath $RepositoryRoot $RelativePath
        if (-not [System.IO.File]::Exists($FullPath)) {
            Throw-AvidScriptPhaseError 'ASPW4011' "protected dirty path must be a file: $RelativePath"
        }

        $Entries.Add([ordered]@{
            path = $RelativePath
            status = $Line.Substring(0, 2)
            worktree_sha256 = Get-AvidScriptFileSha256Hex $FullPath
            head_blob_sha256 = Get-AvidScriptHeadContentSha256 $RepositoryRoot $RelativePath
        })
    }

    return @($Entries | Sort-Object { $_.path })
}

function Assert-AvidScriptStateProperty {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Code
    )

    if ($null -eq $Object -or $Object.PSObject.Properties.Name -notcontains $Name) {
        Throw-AvidScriptPhaseError $Code "phase state is missing property '$Name'"
    }
}

function Assert-AvidScriptPropertySet {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string[]]$Properties,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($null -eq $Object) {
        Throw-AvidScriptPhaseError $Code "$Label must be an object"
    }
    $Observed = if ($Object -is [System.Collections.IDictionary]) {
        @($Object.Keys | ForEach-Object { [string]$_ })
    }
    else {
        @($Object.PSObject.Properties.Name)
    }
    foreach ($Property in $Properties) {
        if ($Observed -notcontains $Property) {
            Throw-AvidScriptPhaseError $Code "$Label is missing property '$Property'"
        }
    }
    foreach ($Property in $Observed) {
        if ($Properties -notcontains $Property) {
            Throw-AvidScriptPhaseError $Code "$Label contains unsupported property '$Property'"
        }
    }
}

function Assert-AvidScriptIdentifier {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Value -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
        Throw-AvidScriptPhaseError 'ASPW1010' "$Label has an invalid identifier: $Value"
    }
}

function Assert-AvidScriptSha256 {
    param(
        [AllowEmptyString()][string]$Value,
        [Parameter(Mandatory = $true)][string]$Label,
        [switch]$AllowEmpty
    )

    if ($AllowEmpty -and [string]::IsNullOrEmpty($Value)) {
        return
    }
    if ($Value -notmatch '^[0-9a-f]{64}$') {
        Throw-AvidScriptPhaseError 'ASPW1011' "$Label is not a lowercase SHA-256"
    }
}

function Assert-AvidScriptGitHash {
    param(
        [AllowEmptyString()][string]$Value,
        [Parameter(Mandatory = $true)][string]$Label,
        [switch]$AllowEmpty
    )

    if ($AllowEmpty -and [string]::IsNullOrEmpty($Value)) {
        return
    }
    if ($Value -notmatch '^[0-9a-f]{40}$') {
        Throw-AvidScriptPhaseError 'ASPW1012' "$Label is not a lowercase Git hash"
    }
}

function Get-AvidScriptPhaseNextAction {
    param([Parameter(Mandatory = $true)]$State)

    foreach ($Debt in @($State.debt)) {
        if ($Debt.status -in @('Open', 'Fixing') -and $Debt.severity -in @('Blocker', 'Critical')) {
            return "debt-update -Phase $($State.phase.id) -DebtId $($Debt.id) -Status Verified -Evidence <text>"
        }
    }

    if ($State.declared_stage -eq 'implementing') {
        $Pending = @($State.batches | Where-Object { $_.status -eq 'Pending' } | Select-Object -First 1)
        if ($Pending.Count -gt 0) {
            return "batch-complete -Phase $($State.phase.id) -BatchId $($Pending[0].id) -Evidence <text>"
        }
        return "freeze -Phase $($State.phase.id) -ReviewEvidence <text>"
    }
    if ($State.declared_stage -eq 'gate_ready') {
        return "attest -Phase $($State.phase.id) -GateReportPath <path>"
    }
    if ($State.declared_stage -eq 'gate_attested') {
        return "close -Phase $($State.phase.id)"
    }

    Throw-AvidScriptPhaseError 'ASPW1013' "unknown declared stage: $($State.declared_stage)"
}

function Test-AvidScriptPhaseState {
    param(
        [Parameter(Mandatory = $true)]$State,
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [int]$ExpectedPhase = 0
    )

    $TopLevelProperties = @(
        'schema_version', 'phase', 'declared_stage', 'architecture', 'documents', 'batches', 'debt',
        'review', 'budgets', 'invocations', 'freeze', 'gate', 'protected_dirty', 'next_action',
        'revision', 'updated_at_utc')
    Assert-AvidScriptPropertySet $State $TopLevelProperties 'ASPW1002' 'phase state'

    if ([int]$State.schema_version -ne $script:AvidScriptPhaseStateSchemaVersion) {
        Throw-AvidScriptPhaseError 'ASPW1003' "unsupported phase state schema: $($State.schema_version)"
    }
    if ([int]$State.phase.id -lt 1 -or [int]$State.phase.id -gt 9999) {
        Throw-AvidScriptPhaseError 'ASPW1004' "invalid phase id: $($State.phase.id)"
    }
    if ($ExpectedPhase -gt 0 -and [int]$State.phase.id -ne $ExpectedPhase) {
        Throw-AvidScriptPhaseError 'ASPW1005' "phase state identity mismatch: expected $ExpectedPhase"
    }
    if ([string]::IsNullOrWhiteSpace([string]$State.phase.goal)) {
        Throw-AvidScriptPhaseError 'ASPW1006' 'phase goal must not be empty'
    }
    Assert-AvidScriptPropertySet $State.phase @('id', 'goal', 'start_commit') 'ASPW1002' 'phase state phase'
    Assert-AvidScriptGitHash ([string]$State.phase.start_commit) 'phase.start_commit'

    if ($State.declared_stage -notin @('implementing', 'gate_ready', 'gate_attested')) {
        Throw-AvidScriptPhaseError 'ASPW1007' "invalid declared stage: $($State.declared_stage)"
    }

    Assert-AvidScriptPropertySet $State.architecture @('path', 'version', 'sha256', 'revision_reason') 'ASPW1002' 'phase state architecture'
    Assert-AvidScriptPropertySet $State.documents @('plan', 'closeout') 'ASPW1002' 'phase state documents'
    foreach ($Path in @($State.architecture.path, $State.documents.plan, $State.documents.closeout)) {
        Resolve-AvidScriptRepositoryPath $RepositoryRoot ([string]$Path) -AllowMissing | Out-Null
    }
    if ([int]$State.architecture.version -lt 1) {
        Throw-AvidScriptPhaseError 'ASPW1008' 'architecture version must be positive'
    }
    Assert-AvidScriptSha256 ([string]$State.architecture.sha256) 'architecture.sha256'

    $BatchIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    if (@($State.batches).Count -lt 1) {
        Throw-AvidScriptPhaseError 'ASPW1014' 'phase state must declare at least one batch'
    }
    foreach ($Batch in @($State.batches)) {
        Assert-AvidScriptPropertySet $Batch @('id', 'status', 'evidence', 'completed_at_utc') 'ASPW1002' 'phase state batch'
        Assert-AvidScriptIdentifier ([string]$Batch.id) 'batch id'
        if (-not $BatchIds.Add([string]$Batch.id)) {
            Throw-AvidScriptPhaseError 'ASPW1015' "duplicate batch id: $($Batch.id)"
        }
        if ($Batch.status -notin @('Pending', 'Completed')) {
            Throw-AvidScriptPhaseError 'ASPW1016' "invalid batch status: $($Batch.status)"
        }
        if ($Batch.status -eq 'Completed' -and [string]::IsNullOrWhiteSpace([string]$Batch.evidence)) {
            Throw-AvidScriptPhaseError 'ASPW1017' "completed batch lacks evidence: $($Batch.id)"
        }
    }

    $DebtIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($Debt in @($State.debt)) {
        Assert-AvidScriptPropertySet $Debt @(
            'id', 'severity', 'found_batch', 'scope', 'evidence', 'deferral_reason', 'remediation',
            'status', 'resolution_evidence', 'target_phase', 'transfer_reason') 'ASPW2000' 'phase state debt'
        Assert-AvidScriptIdentifier ([string]$Debt.id) 'debt id'
        if (-not $DebtIds.Add([string]$Debt.id)) {
            Throw-AvidScriptPhaseError 'ASPW2001' "duplicate debt id: $($Debt.id)"
        }
        if ($Debt.severity -notin @('Blocker', 'Critical', 'Important', 'Normal')) {
            Throw-AvidScriptPhaseError 'ASPW2002' "invalid debt severity: $($Debt.severity)"
        }
        if ($Debt.status -notin @('Open', 'Fixing', 'Verified', 'Transferred')) {
            Throw-AvidScriptPhaseError 'ASPW2003' "invalid debt status: $($Debt.status)"
        }
        if (-not $BatchIds.Contains([string]$Debt.found_batch)) {
            Throw-AvidScriptPhaseError 'ASPW2004' "debt references an unknown batch: $($Debt.found_batch)"
        }
        foreach ($RequiredText in @('scope', 'evidence', 'deferral_reason', 'remediation')) {
            if ([string]::IsNullOrWhiteSpace([string]$Debt.$RequiredText)) {
                Throw-AvidScriptPhaseError 'ASPW2005' "debt $($Debt.id) lacks $RequiredText"
            }
        }
        if ($Debt.status -eq 'Verified' -and [string]::IsNullOrWhiteSpace([string]$Debt.resolution_evidence)) {
            Throw-AvidScriptPhaseError 'ASPW2006' "verified debt lacks resolution evidence: $($Debt.id)"
        }
        if ($Debt.status -eq 'Transferred' -and
            ($null -eq $Debt.target_phase -or [string]::IsNullOrWhiteSpace([string]$Debt.transfer_reason))) {
            Throw-AvidScriptPhaseError 'ASPW2007' "transferred debt lacks target phase or reason: $($Debt.id)"
        }
    }

    Assert-AvidScriptPropertySet $State.review @('completed', 'evidence', 'completed_at_utc') 'ASPW1002' 'phase state review'
    foreach ($CounterGroupName in @('budgets', 'invocations')) {
        $CounterGroup = $State.$CounterGroupName
        Assert-AvidScriptPropertySet $CounterGroup @('ubt', 'automation', 'full_gate') 'ASPW1002' "phase state $CounterGroupName"
        foreach ($CounterName in @('ubt', 'automation', 'full_gate')) {
            Assert-AvidScriptStateProperty $CounterGroup $CounterName 'ASPW1018'
            if ([int]$CounterGroup.$CounterName -lt 0) {
                Throw-AvidScriptPhaseError 'ASPW1019' "$CounterGroupName.$CounterName must not be negative"
            }
        }
    }

    Assert-AvidScriptPropertySet $State.freeze @(
        'source_commit', 'source_tree', 'state_sha256', 'review_evidence', 'at_utc') 'ASPW1002' 'phase state freeze'
    Assert-AvidScriptPropertySet $State.gate @(
        'report_path', 'report_sha256', 'run_id', 'verified_commit', 'verified_tree',
        'attestation_parent', 'attested_at_utc') 'ASPW1002' 'phase state gate'
    Assert-AvidScriptGitHash ([string]$State.freeze.source_commit) 'freeze.source_commit' -AllowEmpty
    Assert-AvidScriptGitHash ([string]$State.freeze.source_tree) 'freeze.source_tree' -AllowEmpty
    Assert-AvidScriptSha256 ([string]$State.freeze.state_sha256) 'freeze.state_sha256' -AllowEmpty
    Assert-AvidScriptSha256 ([string]$State.gate.report_sha256) 'gate.report_sha256' -AllowEmpty
    Assert-AvidScriptGitHash ([string]$State.gate.verified_commit) 'gate.verified_commit' -AllowEmpty
    Assert-AvidScriptGitHash ([string]$State.gate.verified_tree) 'gate.verified_tree' -AllowEmpty
    Assert-AvidScriptGitHash ([string]$State.gate.attestation_parent) 'gate.attestation_parent' -AllowEmpty

    $ProtectedPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($Entry in @($State.protected_dirty)) {
        Assert-AvidScriptPropertySet $Entry @(
            'path', 'status', 'worktree_sha256', 'head_blob_sha256') 'ASPW1002' 'phase state protected dirty entry'
        Resolve-AvidScriptRepositoryPath $RepositoryRoot ([string]$Entry.path) | Out-Null
        if (-not $ProtectedPaths.Add([string]$Entry.path)) {
            Throw-AvidScriptPhaseError 'ASPW1020' "duplicate protected dirty path: $($Entry.path)"
        }
        if ([string]$Entry.status -notmatch '^.{2}$') {
            Throw-AvidScriptPhaseError 'ASPW1021' "invalid protected dirty status: $($Entry.status)"
        }
        Assert-AvidScriptSha256 ([string]$Entry.worktree_sha256) "protected_dirty[$($Entry.path)].worktree_sha256"
        Assert-AvidScriptSha256 ([string]$Entry.head_blob_sha256) "protected_dirty[$($Entry.path)].head_blob_sha256" -AllowEmpty
    }

    if ([int]$State.revision -lt 1) {
        Throw-AvidScriptPhaseError 'ASPW1022' 'phase state revision must be positive'
    }
    $ExpectedNextAction = Get-AvidScriptPhaseNextAction $State
    if ([string]$State.next_action -cne $ExpectedNextAction) {
        Throw-AvidScriptPhaseError 'ASPW1023' 'phase state next_action is stale or invalid'
    }

    return $true
}

function ConvertTo-AvidScriptPhaseStateDocument {
    param([Parameter(Mandatory = $true)]$State)

    $Batches = @($State.batches | Sort-Object { $_.id } | ForEach-Object {
        [ordered]@{
            id = [string]$_.id
            status = [string]$_.status
            evidence = [string]$_.evidence
            completed_at_utc = $_.completed_at_utc
        }
    })
    $Debt = @($State.debt | Sort-Object { $_.id } | ForEach-Object {
        [ordered]@{
            id = [string]$_.id
            severity = [string]$_.severity
            found_batch = [string]$_.found_batch
            scope = [string]$_.scope
            evidence = [string]$_.evidence
            deferral_reason = [string]$_.deferral_reason
            remediation = [string]$_.remediation
            status = [string]$_.status
            resolution_evidence = [string]$_.resolution_evidence
            target_phase = $_.target_phase
            transfer_reason = [string]$_.transfer_reason
        }
    })
    $ProtectedDirty = @($State.protected_dirty | Sort-Object { $_.path } | ForEach-Object {
        [ordered]@{
            path = [string]$_.path
            status = [string]$_.status
            worktree_sha256 = [string]$_.worktree_sha256
            head_blob_sha256 = [string]$_.head_blob_sha256
        }
    })

    return [ordered]@{
        schema_version = [int]$State.schema_version
        phase = [ordered]@{
            id = [int]$State.phase.id
            goal = [string]$State.phase.goal
            start_commit = [string]$State.phase.start_commit
        }
        declared_stage = [string]$State.declared_stage
        architecture = [ordered]@{
            path = [string]$State.architecture.path
            version = [int]$State.architecture.version
            sha256 = [string]$State.architecture.sha256
            revision_reason = [string]$State.architecture.revision_reason
        }
        documents = [ordered]@{
            plan = [string]$State.documents.plan
            closeout = [string]$State.documents.closeout
        }
        batches = $Batches
        debt = $Debt
        review = [ordered]@{
            completed = [bool]$State.review.completed
            evidence = [string]$State.review.evidence
            completed_at_utc = $State.review.completed_at_utc
        }
        budgets = [ordered]@{
            ubt = [int]$State.budgets.ubt
            automation = [int]$State.budgets.automation
            full_gate = [int]$State.budgets.full_gate
        }
        invocations = [ordered]@{
            ubt = [int]$State.invocations.ubt
            automation = [int]$State.invocations.automation
            full_gate = [int]$State.invocations.full_gate
        }
        freeze = [ordered]@{
            source_commit = [string]$State.freeze.source_commit
            source_tree = [string]$State.freeze.source_tree
            state_sha256 = [string]$State.freeze.state_sha256
            review_evidence = [string]$State.freeze.review_evidence
            at_utc = $State.freeze.at_utc
        }
        gate = [ordered]@{
            report_path = [string]$State.gate.report_path
            report_sha256 = [string]$State.gate.report_sha256
            run_id = [string]$State.gate.run_id
            verified_commit = [string]$State.gate.verified_commit
            verified_tree = [string]$State.gate.verified_tree
            attestation_parent = [string]$State.gate.attestation_parent
            attested_at_utc = $State.gate.attested_at_utc
        }
        protected_dirty = $ProtectedDirty
        next_action = [string]$State.next_action
        revision = [int]$State.revision
        updated_at_utc = [string]$State.updated_at_utc
    }
}

function Write-AvidScriptPhaseStateAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$State
    )

    $Directory = Split-Path -Parent $Path
    [System.IO.Directory]::CreateDirectory($Directory) | Out-Null
    $OperationId = [guid]::NewGuid().ToString('N')
    $TemporaryPath = Join-Path $Directory ('.' + [System.IO.Path]::GetFileName($Path) + '.' + $OperationId + '.tmp')
    $BackupPath = Join-Path $Directory ('.' + [System.IO.Path]::GetFileName($Path) + '.' + $OperationId + '.bak')
    $Json = (ConvertTo-AvidScriptPhaseStateDocument $State) | ConvertTo-Json -Depth 64
    $Json = $Json -replace "`r`n?", "`n"
    try {
        [System.IO.File]::WriteAllText($TemporaryPath, $Json + "`n", $script:AvidScriptPhaseUtf8)
        if ([System.IO.File]::Exists($Path)) {
            [System.IO.File]::Replace($TemporaryPath, $Path, $BackupPath, $true)
            [System.IO.File]::Delete($BackupPath)
        }
        else {
            [System.IO.File]::Move($TemporaryPath, $Path)
        }
    }
    finally {
        if ([System.IO.File]::Exists($TemporaryPath)) {
            [System.IO.File]::Delete($TemporaryPath)
        }
        if ([System.IO.File]::Exists($BackupPath)) {
            [System.IO.File]::Delete($BackupPath)
        }
    }
}

function Read-AvidScriptPhaseState {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][int]$Phase
    )

    $Path = Get-AvidScriptPhaseStatePath $RepositoryRoot $Phase
    if (-not [System.IO.File]::Exists($Path)) {
        Throw-AvidScriptPhaseError 'ASPW1024' "phase state does not exist: $(Get-AvidScriptPhaseStateRelativePath $Phase)"
    }

    try {
        $State = ConvertFrom-AvidScriptJson ([System.IO.File]::ReadAllText($Path))
    }
    catch {
        Throw-AvidScriptPhaseError 'ASPW1025' 'phase state is not valid JSON'
    }
    Test-AvidScriptPhaseState $State $RepositoryRoot $Phase | Out-Null
    return $State
}

function New-AvidScriptInitialPhaseState {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][int]$Phase,
        [Parameter(Mandatory = $true)][string]$Goal,
        [Parameter(Mandatory = $true)][string]$ArchitecturePath,
        [Parameter(Mandatory = $true)][string]$PlanPath,
        [Parameter(Mandatory = $true)][string]$CloseoutPath,
        [Parameter(Mandatory = $true)][string[]]$BatchIds,
        [int]$UbtBudget = 3,
        [int]$AutomationBudget = 2,
        [int]$FullGateBudget = 2
    )

    if ([string]::IsNullOrWhiteSpace($Goal)) {
        Throw-AvidScriptPhaseError 'ASPW1026' 'phase goal must not be empty'
    }
    if ($UbtBudget -lt 0 -or $AutomationBudget -lt 0 -or $FullGateBudget -lt 0) {
        Throw-AvidScriptPhaseError 'ASPW1027' 'phase budgets must not be negative'
    }

    $StatePath = Get-AvidScriptPhaseStatePath $RepositoryRoot $Phase
    if ([System.IO.File]::Exists($StatePath)) {
        Throw-AvidScriptPhaseError 'ASPW1028' "phase state already exists: $(Get-AvidScriptPhaseStateRelativePath $Phase)"
    }

    $ArchitectureRelative = ConvertTo-AvidScriptRepositoryPath $ArchitecturePath
    $PlanRelative = ConvertTo-AvidScriptRepositoryPath $PlanPath
    $CloseoutRelative = ConvertTo-AvidScriptRepositoryPath $CloseoutPath
    $ArchitectureFull = Resolve-AvidScriptRepositoryPath $RepositoryRoot $ArchitectureRelative
    Resolve-AvidScriptRepositoryPath $RepositoryRoot $PlanRelative | Out-Null
    Resolve-AvidScriptRepositoryPath $RepositoryRoot $CloseoutRelative -AllowMissing | Out-Null

    $UniqueBatchIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $Batches = [System.Collections.Generic.List[object]]::new()
    foreach ($BatchId in @($BatchIds)) {
        Assert-AvidScriptIdentifier $BatchId 'batch id'
        if (-not $UniqueBatchIds.Add($BatchId)) {
            Throw-AvidScriptPhaseError 'ASPW1029' "duplicate batch id: $BatchId"
        }
        $Batches.Add([ordered]@{
            id = $BatchId
            status = 'Pending'
            evidence = ''
            completed_at_utc = $null
        })
    }
    if ($Batches.Count -eq 0) {
        Throw-AvidScriptPhaseError 'ASPW1030' 'phase must declare at least one batch'
    }

    $Now = [DateTimeOffset]::UtcNow.ToString('o')
    $State = [pscustomobject][ordered]@{
        schema_version = $script:AvidScriptPhaseStateSchemaVersion
        phase = [pscustomobject][ordered]@{
            id = $Phase
            goal = $Goal.Trim()
            start_commit = Get-AvidScriptGitCommit $RepositoryRoot
        }
        declared_stage = 'implementing'
        architecture = [pscustomobject][ordered]@{
            path = $ArchitectureRelative
            version = 1
            sha256 = Get-AvidScriptFileSha256Hex $ArchitectureFull
            revision_reason = 'initial architecture freeze'
        }
        documents = [pscustomobject][ordered]@{
            plan = $PlanRelative
            closeout = $CloseoutRelative
        }
        batches = @($Batches)
        debt = @()
        review = [pscustomobject][ordered]@{
            completed = $false
            evidence = ''
            completed_at_utc = $null
        }
        budgets = [pscustomobject][ordered]@{
            ubt = $UbtBudget
            automation = $AutomationBudget
            full_gate = $FullGateBudget
        }
        invocations = [pscustomobject][ordered]@{
            ubt = 0
            automation = 0
            full_gate = 0
        }
        freeze = [pscustomobject][ordered]@{
            source_commit = ''
            source_tree = ''
            state_sha256 = ''
            review_evidence = ''
            at_utc = $null
        }
        gate = [pscustomobject][ordered]@{
            report_path = ''
            report_sha256 = ''
            run_id = ''
            verified_commit = ''
            verified_tree = ''
            attestation_parent = ''
            attested_at_utc = $null
        }
        protected_dirty = @(Get-AvidScriptProtectedDirtyBaseline $RepositoryRoot)
        next_action = ''
        revision = 1
        updated_at_utc = $Now
    }
    $State.next_action = Get-AvidScriptPhaseNextAction $State
    Test-AvidScriptPhaseState $State $RepositoryRoot $Phase | Out-Null
    return $State
}

function Update-AvidScriptPhaseState {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][int]$Phase,
        [Parameter(Mandatory = $true)][scriptblock]$Mutation
    )

    $StatePath = Get-AvidScriptPhaseStatePath $RepositoryRoot $Phase
    $State = Read-AvidScriptPhaseState $RepositoryRoot $Phase
    & $Mutation $State | Out-Null
    $State.revision = [int]$State.revision + 1
    $State.updated_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
    $State.next_action = Get-AvidScriptPhaseNextAction $State
    Test-AvidScriptPhaseState $State $RepositoryRoot $Phase | Out-Null
    Write-AvidScriptPhaseStateAtomic $StatePath $State
    return $State
}
