[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('bootstrap', 'context', 'verify', 'lesson-query', 'audit')]
    [string]$Command,

    [string]$Intent = '',
    [string[]]$Paths = @(),
    [string[]]$Tags = @(),

    [ValidateSet('Auto', 'DocsOnly', 'Managed', 'Native', 'Runtime', 'Performance', 'Full')]
    [string]$Profile = 'Auto',

    [ValidateRange(1, 5)]
    [int]$Limit = 5,

    [switch]$Json
)

$ErrorActionPreference = 'Stop'
$StartedAt = [System.Diagnostics.Stopwatch]::StartNew()
$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepositoryRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $ScriptDirectory))
$ManifestPath = Join-Path $RepositoryRoot 'AgentHarness/manifest.json'

function Throw-HarnessError {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message
    )

    throw "$Code $Message"
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not [System.IO.File]::Exists($Path)) {
        Throw-HarnessError 'ASAH1001' "required JSON file does not exist: $Path"
    }

    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 64
}

function ConvertTo-RepositoryPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Value = $Path.Trim()
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ''
    }

    if ([System.IO.Path]::IsPathRooted($Value)) {
        $FullPath = [System.IO.Path]::GetFullPath($Value)
        $RootWithSeparator = $RepositoryRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
        if (-not $FullPath.StartsWith($RootWithSeparator, [System.StringComparison]::OrdinalIgnoreCase) -and
            -not $FullPath.Equals($RepositoryRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            Throw-HarnessError 'ASAH1002' "path is outside the plugin repository: $Path"
        }

        $Value = [System.IO.Path]::GetRelativePath($RepositoryRoot, $FullPath)
    }

    return $Value.Replace('\', '/').TrimStart('.', '/')
}

function Get-NormalizedInputs {
    param([string[]]$Values)

    $Result = @()
    foreach ($Value in $Values) {
        foreach ($Part in @($Value -split ',')) {
            if (-not [string]::IsNullOrWhiteSpace($Part)) {
                $Result += ConvertTo-RepositoryPath $Part
            }
        }
    }

    return @($Result | Sort-Object -Unique)
}

function Get-NormalizedTags {
    param([string[]]$Values)

    $Result = @()
    foreach ($Value in $Values) {
        foreach ($Part in @($Value -split ',')) {
            $Normalized = $Part.Trim().ToLowerInvariant()
            if (-not [string]::IsNullOrWhiteSpace($Normalized)) {
                $Result += $Normalized
            }
        }
    }

    return @($Result | Sort-Object -Unique)
}

function Assert-HostAndRepository {
    param([Parameter(Mandatory = $true)]$Manifest)

    if ($PSVersionTable.PSVersion.Major -ne [int]$Manifest.toolchain.powershell_major) {
        Throw-HarnessError 'ASAH1100' "PowerShell $($Manifest.toolchain.powershell_major) is required; current host is $($PSVersionTable.PSVersion)"
    }

    $GitRootOutput = & git -C $RepositoryRoot rev-parse --show-toplevel 2>&1
    if ($LASTEXITCODE -ne 0) {
        Throw-HarnessError 'ASAH1101' "plugin root is not a readable Git repository: $($GitRootOutput -join ' ')"
    }

    $GitRoot = [System.IO.Path]::GetFullPath(([string]($GitRootOutput | Select-Object -Last 1)).Trim())
    if (-not $GitRoot.Equals($RepositoryRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        Throw-HarnessError 'ASAH1102' "Git root mismatch: expected $RepositoryRoot, resolved $GitRoot"
    }
}

function Read-PhaseWorkflowStatus {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [Parameter(Mandatory = $true)][int]$PhaseId
    )

    $WorkflowPath = Join-Path $RepositoryRoot ([string]$Manifest.phase_workflow.script)
    if (-not [System.IO.File]::Exists($WorkflowPath)) {
        Throw-HarnessError 'ASAH1201' "Phase workflow script does not exist: $WorkflowPath"
    }

    $PowerShellHost = Join-Path $PSHOME 'pwsh.exe'
    $StatusOutput = & $PowerShellHost -NoProfile -File $WorkflowPath status -Phase $PhaseId -RepositoryRoot $RepositoryRoot -Json 2>&1
    if ($LASTEXITCODE -ne 0) {
        Throw-HarnessError 'ASAH1202' "Phase $PhaseId status failed: $($StatusOutput -join ' ')"
    }

    try {
        return ($StatusOutput -join [Environment]::NewLine) | ConvertFrom-Json -Depth 32
    }
    catch {
        Throw-HarnessError 'ASAH1203' "Phase $PhaseId status did not return valid JSON: $($StatusOutput -join ' ')"
    }
}

function Get-PhaseContext {
    param([Parameter(Mandatory = $true)]$Manifest)

    $DocsRoot = Join-Path $RepositoryRoot 'Docs'
    $Candidates = @()
    foreach ($StateFile in @(Get-ChildItem -LiteralPath $DocsRoot -Filter 'Phase*_State.json' -File -Recurse)) {
        try {
            $State = Read-JsonFile $StateFile.FullName
            if ($null -ne $State.phase.id) {
                $Candidates += [pscustomobject]@{
                    Id = [int]$State.phase.id
                    State = $State
                    Path = $StateFile.FullName
                    RelativePath = ConvertTo-RepositoryPath $StateFile.FullName
                }
            }
        }
        catch {
            Throw-HarnessError 'ASAH1200' "invalid Phase state $($StateFile.FullName): $($_.Exception.Message)"
        }
    }

    if ($Candidates.Count -eq 0) {
        return $null
    }

    $ClosedStages = @($Manifest.phase_workflow.closed_stages | ForEach-Object { ([string]$_).ToLowerInvariant() })
    $OrderedCandidates = @($Candidates | Sort-Object Id -Descending)
    $DeclaredOpenCandidates = @($OrderedCandidates | Where-Object { $ClosedStages -notcontains ([string]$_.State.declared_stage).ToLowerInvariant() })
    $Selected = $null
    $Status = $null
    foreach ($Candidate in $DeclaredOpenCandidates) {
        $CandidateStatus = Read-PhaseWorkflowStatus $Manifest $Candidate.Id
        if ($ClosedStages -notcontains ([string]$CandidateStatus.effective_stage).ToLowerInvariant()) {
            $Selected = $Candidate
            $Status = $CandidateStatus
            break
        }
    }

    if ($null -eq $Selected) {
        $Selected = $OrderedCandidates[0]
        $Status = Read-PhaseWorkflowStatus $Manifest $Selected.Id
    }

    return [pscustomobject]@{
        Id = $Selected.Id
        State = $Selected.State
        StatePath = $Selected.Path
        StateRelativePath = $Selected.RelativePath
        Status = $Status
    }
}

function Get-GitContext {
    $StatusLines = @(& git -C $RepositoryRoot status --short --untracked-files=all 2>&1)
    if ($LASTEXITCODE -ne 0) {
        Throw-HarnessError 'ASAH1300' "git status failed: $($StatusLines -join ' ')"
    }

    $BranchOutput = @(& git -C $RepositoryRoot branch --show-current 2>&1)
    if ($LASTEXITCODE -ne 0) {
        Throw-HarnessError 'ASAH1301' "git branch discovery failed: $($BranchOutput -join ' ')"
    }

    $Entries = @()
    foreach ($LineValue in $StatusLines) {
        $Line = [string]$LineValue
        if ($Line.Length -lt 3) {
            continue
        }

        $Path = $Line.Substring(3).Trim()
        if ($Path.Contains(' -> ')) {
            $Path = ($Path -split ' -> ')[-1]
        }

        $Entries += [pscustomobject]@{
            Status = $Line.Substring(0, 2)
            Path = $Path.Trim('"').Replace('\', '/')
        }
    }

    $BranchValue = $BranchOutput | Select-Object -Last 1
    return [pscustomobject]@{
        Branch = if ($null -eq $BranchValue) { '(detached)' } else { ([string]$BranchValue).Trim() }
        Entries = $Entries
        Lines = $StatusLines
    }
}

function Get-ProtectedDirtyContext {
    param(
        [AllowNull()]$PhaseContext,
        [Parameter(Mandatory = $true)]$GitContext
    )

    if ($null -eq $PhaseContext) {
        return @()
    }

    $PhaseOwned = @(
        [string]$PhaseContext.State.architecture.path,
        [string]$PhaseContext.State.documents.plan,
        [string]$PhaseContext.State.documents.closeout,
        [string]$PhaseContext.StateRelativePath
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object { $_.Replace('\', '/') }

    $StatusByPath = @{}
    foreach ($Entry in $GitContext.Entries) {
        $StatusByPath[$Entry.Path] = $Entry.Status
    }
    $IsCleanDetachedCandidate =
        $GitContext.Branch -ceq '(detached)' -and
        $GitContext.Entries.Count -eq 0 -and
        [string]$PhaseContext.Status.effective_stage -in @('gate_ready', 'gate_attested')

    $Results = @()
    foreach ($Protected in @($PhaseContext.State.protected_dirty)) {
        $RelativePath = ([string]$Protected.path).Replace('\', '/')
        if ($PhaseOwned -contains $RelativePath) {
            continue
        }

        $FullPath = Join-Path $RepositoryRoot $RelativePath
        $FileExists = [System.IO.File]::Exists($FullPath)
        $CurrentHash = ''
        if ($FileExists) {
            $CurrentHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $FullPath).Hash.ToLowerInvariant()
        }

        $ExpectedHash = ([string]$Protected.worktree_sha256).ToLowerInvariant()
        $CurrentStatus = if ($StatusByPath.ContainsKey($RelativePath)) { [string]$StatusByPath[$RelativePath] } else { '  ' }
        $ContentStable = -not [string]::IsNullOrWhiteSpace($ExpectedHash) -and $ExpectedHash -eq $CurrentHash
        $StatusStable = ([string]$Protected.status) -eq $CurrentStatus
        $RepresentedByCleanCandidate = $IsCleanDetachedCandidate
        $Results += [pscustomobject]@{
            Path = $RelativePath
            ExpectedStatus = [string]$Protected.status
            CurrentStatus = $CurrentStatus
            ExpectedSha256 = $ExpectedHash
            CurrentSha256 = $CurrentHash
            ContentStable = $ContentStable
            StatusStable = $StatusStable
            RepresentedByCleanCandidate = $RepresentedByCleanCandidate
            BaselineSatisfied = ($ContentStable -and $StatusStable) -or $RepresentedByCleanCandidate
        }
    }

    return $Results
}

function Get-RoutingContext {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [string]$TaskIntent,
        [string[]]$TaskPaths
    )

    $NormalizedIntent = $TaskIntent.ToLowerInvariant()
    $Core = @($Manifest.routing.core_policies | ForEach-Object {
        [pscustomobject]@{
            Id = [string]$_.id
            Path = [string]$_.path
            Reasons = @('core')
        }
    })

    $Domains = @()
    foreach ($Domain in @($Manifest.routing.domains)) {
        $Reasons = @()
        foreach ($Keyword in @($Domain.keywords)) {
            $KeywordText = ([string]$Keyword).ToLowerInvariant()
            if (-not [string]::IsNullOrWhiteSpace($KeywordText) -and $NormalizedIntent.Contains($KeywordText)) {
                $Reasons += "intent:$Keyword"
            }
        }

        foreach ($TaskPath in $TaskPaths) {
            foreach ($Glob in @($Domain.globs)) {
                if ($TaskPath -like [string]$Glob) {
                    $Reasons += "path:$TaskPath"
                    break
                }
            }
        }

        if ($Reasons.Count -gt 0) {
            $Domains += [pscustomobject]@{
                Id = [string]$Domain.id
                Path = [string]$Domain.path
                Reasons = @($Reasons | Sort-Object -Unique)
            }
        }
    }

    $MaxDomains = [int]$Manifest.routing.max_domain_policies
    if ($Domains.Count -gt $MaxDomains) {
        $Domains = @($Domains | Select-Object -First $MaxDomains)
    }

    return [pscustomobject]@{
        Intent = $TaskIntent
        Paths = $TaskPaths
        CorePolicies = $Core
        DomainPolicies = $Domains
    }
}

function Get-LessonMatches {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [string[]]$QueryTags,
        [int]$ResultLimit
    )

    $LessonIndex = Read-JsonFile (Join-Path $RepositoryRoot ([string]$Manifest.lessons.index))
    $NormalizedTags = Get-NormalizedTags $QueryTags
    $Matches = @()
    foreach ($Rule in @($LessonIndex.rules)) {
        $RuleTags = @($Rule.tags | ForEach-Object { ([string]$_).ToLowerInvariant() })
        $Score = @($NormalizedTags | Where-Object { $RuleTags -contains $_ }).Count
        if ($NormalizedTags.Count -eq 0 -or $Score -gt 0) {
            $Matches += [pscustomobject]@{
                Id = [string]$Rule.id
                Tags = $RuleTags
                Summary = [string]$Rule.summary
                Prevention = [string]$Rule.prevention
                Source = [string]$Rule.source
                Score = $Score
            }
        }
    }

    $EffectiveLimit = [Math]::Min($ResultLimit, [int]$Manifest.routing.max_lessons)
    return @($Matches | Sort-Object @{ Expression = 'Score'; Descending = $true }, Id | Select-Object -First $EffectiveLimit)
}

function Get-VerificationPlan {
    param(
        [Parameter(Mandatory = $true)]$Manifest,
        [string]$RequestedProfile,
        [string]$TaskIntent,
        [string[]]$TaskPaths
    )

    $SelectedProfile = $RequestedProfile
    $Combined = (($TaskPaths -join ' ') + ' ' + $TaskIntent).ToLowerInvariant()
    if ($RequestedProfile -eq 'Auto') {
        if ($Combined -match 'benchmark|performance|puerts|angelscript|性能|benchmarks/') {
            $SelectedProfile = 'Performance'
        }
        elseif ($Combined -match 'source/avidscript(runtime|bindings|vm|core)|beginplay|tick|lifecycle|objecthandle|ufunction|descriptor') {
            $SelectedProfile = 'Runtime'
        }
        elseif ($Combined -match 'source/') {
            $SelectedProfile = 'Native'
        }
        elseif ($Combined -match 'tools/|csharp|roslyn|semantic|guest ir') {
            $SelectedProfile = 'Managed'
        }
        else {
            $SelectedProfile = 'DocsOnly'
        }
    }

    $Checks = switch ($SelectedProfile) {
        'DocsOnly' { @('agent-harness-audit', 'json-and-link-integrity') }
        'Managed' { @('agent-harness-audit', 'pinned-dotnet-sdk', 'affected-managed-runners') }
        'Native' { @('agent-harness-audit', 'architecture-check', 'no-clean-affected-ubt') }
        'Runtime' { @('agent-harness-audit', 'affected-managed-runners', 'architecture-check', 'no-clean-affected-ubt', 'focused-automation') }
        'Performance' { @('agent-harness-audit', 'correctness-prerequisites', 'controlled-benchmark-profile', 'performance-evaluator') }
        'Full' { @('agent-harness-audit', 'all-managed-runners', 'clean-architecture-candidate', 'canonical-no-clean-editor-build', 'full-avidscript-automation', 'required-benchmarks') }
    }

    return [pscustomobject]@{
        RequestedProfile = $RequestedProfile
        SelectedProfile = $SelectedProfile
        Paths = $TaskPaths
        Checks = $Checks
        Execution = 'impact-plan-only'
        Note = 'H1 only plans verification. Cached execution and machine-readable Gate reports are implemented in H3.'
    }
}

function Invoke-HarnessAudit {
    param([Parameter(Mandatory = $true)]$Manifest)

    $Checks = @()
    function Add-Check {
        param([string]$Id, [bool]$Passed, [string]$Detail)
        $script:AuditChecks += [pscustomobject]@{ Id = $Id; Passed = $Passed; Detail = $Detail }
    }

    $script:AuditChecks = @()
    $EntryPath = Join-Path $RepositoryRoot ([string]$Manifest.entry.path)
    $EntryItem = Get-Item -LiteralPath $EntryPath
    Add-Check 'entry-hard-limit' ($EntryItem.Length -le [int64]$Manifest.entry.hard_limit_bytes) "$($EntryItem.Length)/$($Manifest.entry.hard_limit_bytes) bytes"
    Add-Check 'entry-soft-limit' ($EntryItem.Length -le [int64]$Manifest.entry.soft_limit_bytes) "$($EntryItem.Length)/$($Manifest.entry.soft_limit_bytes) bytes"

    $EntryText = Get-Content -LiteralPath $EntryPath -Raw -Encoding UTF8
    Add-Check 'entry-bootstrap-link' $EntryText.Contains([string]$Manifest.entry.required_bootstrap_text) ([string]$Manifest.entry.required_bootstrap_text)

    $SchemaPath = Join-Path $RepositoryRoot ([string]$Manifest.'$schema')
    $SchemaExists = [System.IO.File]::Exists($SchemaPath)
    Add-Check 'manifest-schema-present' $SchemaExists ([string]$Manifest.'$schema')
    if ($SchemaExists) {
        $ManifestText = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8
        $SchemaValid = $false
        try {
            $SchemaValid = [bool]($ManifestText | Test-Json -SchemaFile $SchemaPath -ErrorAction Stop)
        }
        catch {
            $SchemaValid = $false
        }
        Add-Check 'manifest-schema-valid' $SchemaValid ([string]$Manifest.'$schema')
    }

    $RegisteredPolicyPaths = @($Manifest.routing.core_policies.path) + @($Manifest.routing.domains.path)
    $MissingPolicies = @($RegisteredPolicyPaths | Where-Object { -not [System.IO.File]::Exists((Join-Path $RepositoryRoot ([string]$_))) })
    $RegisteredPolicyDetail = if ($MissingPolicies.Count -eq 0) { "$($RegisteredPolicyPaths.Count) files" } else { $MissingPolicies -join ', ' }
    Add-Check 'registered-policy-files' ($MissingPolicies.Count -eq 0) $RegisteredPolicyDetail

    $RegisteredIds = @($Manifest.routing.core_policies.id) + @($Manifest.routing.domains.id)
    $DuplicatePolicyIds = @($RegisteredIds | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name)
    $PolicyIdDetail = if ($DuplicatePolicyIds.Count -eq 0) { "$($RegisteredIds.Count) ids" } else { $DuplicatePolicyIds -join ', ' }
    Add-Check 'unique-policy-ids' ($DuplicatePolicyIds.Count -eq 0) $PolicyIdDetail

    $DiscoveredPolicies = @(
        Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'AgentHarness/policies') -Filter '*.md' -File
        Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'AgentHarness/domains') -Filter '*.md' -File
    ) | ForEach-Object { ConvertTo-RepositoryPath $_.FullName }
    $UnregisteredPolicies = @($DiscoveredPolicies | Where-Object { $RegisteredPolicyPaths -notcontains $_ })
    $UnregisteredPolicyDetail = if ($UnregisteredPolicies.Count -eq 0) { "$($DiscoveredPolicies.Count) files" } else { $UnregisteredPolicies -join ', ' }
    Add-Check 'no-unregistered-policies' ($UnregisteredPolicies.Count -eq 0) $UnregisteredPolicyDetail

    $LegacyPath = Join-Path $RepositoryRoot ([string]$Manifest.lessons.legacy_path)
    $LegacyItem = Get-Item -LiteralPath $LegacyPath
    $LegacyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $LegacyPath).Hash.ToLowerInvariant()
    Add-Check 'legacy-byte-count' ($LegacyItem.Length -eq [int64]$Manifest.lessons.legacy_bytes) "$($LegacyItem.Length)/$($Manifest.lessons.legacy_bytes) bytes"
    Add-Check 'legacy-sha256' ($LegacyHash -eq ([string]$Manifest.lessons.legacy_sha256).ToLowerInvariant()) $LegacyHash

    $LegacyLines = (Get-Content -LiteralPath $LegacyPath).Count
    Add-Check 'legacy-line-count' ($LegacyLines -eq [int]$Manifest.lessons.legacy_lines) "$LegacyLines/$($Manifest.lessons.legacy_lines) lines"

    $LessonIndex = Read-JsonFile (Join-Path $RepositoryRoot ([string]$Manifest.lessons.index))
    $LessonIds = @($LessonIndex.rules.id)
    $DuplicateLessonIds = @($LessonIds | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name)
    $LessonIdDetail = if ($DuplicateLessonIds.Count -eq 0) { "$($LessonIds.Count) ids" } else { $DuplicateLessonIds -join ', ' }
    Add-Check 'unique-lesson-ids' ($DuplicateLessonIds.Count -eq 0) $LessonIdDetail
    Add-Check 'lesson-legacy-identity' (([string]$LessonIndex.legacy_source.sha256).ToLowerInvariant() -eq $LegacyHash) ([string]$LessonIndex.legacy_source.sha256)

    $RepositoryFiles = @(& git -C $RepositoryRoot ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) {
        Throw-HarnessError 'ASAH1400' 'failed to enumerate repository-visible files for AGENTS.md audit'
    }
    $AgentEntries = @($RepositoryFiles | Where-Object { ([string]$_).Replace('\', '/') -match '(^|/)AGENTS\.md$' })
    $SingleEntryPassed = $AgentEntries.Count -eq 1 -and ([string]$AgentEntries[0]).Replace('\', '/') -eq 'AGENTS.md'
    Add-Check 'single-agents-entry' $SingleEntryPassed "$($AgentEntries.Count) repository-visible AGENTS.md"

    Add-Check 'phase-workflow-present' ([System.IO.File]::Exists((Join-Path $RepositoryRoot ([string]$Manifest.phase_workflow.script)))) ([string]$Manifest.phase_workflow.script)

    return [pscustomobject]@{
        Passed = @($script:AuditChecks | Where-Object { -not $_.Passed }).Count -eq 0
        Checks = $script:AuditChecks
    }
}

function Write-ContextText {
    param([Parameter(Mandatory = $true)]$Routing)

    Write-Output 'Core policies:'
    foreach ($Policy in $Routing.CorePolicies) {
        Write-Output "  - $($Policy.Path)"
    }

    Write-Output 'Domain policies:'
    if ($Routing.DomainPolicies.Count -eq 0) {
        Write-Output '  - none'
    }
    else {
        foreach ($Policy in $Routing.DomainPolicies) {
            Write-Output "  - $($Policy.Path) [$($Policy.Reasons -join ', ')]"
        }
    }
}

try {
    $Manifest = Read-JsonFile $ManifestPath
    Assert-HostAndRepository $Manifest
    $NormalizedPaths = Get-NormalizedInputs $Paths

    switch ($Command) {
        'bootstrap' {
            $PhaseContext = Get-PhaseContext $Manifest
            $GitContext = Get-GitContext
            $ProtectedContext = @(Get-ProtectedDirtyContext $PhaseContext $GitContext)
            $Routing = Get-RoutingContext $Manifest $Intent $NormalizedPaths
            $BootstrapTags = @('phase', 'resume', 'workflow') + @($Routing.DomainPolicies.Id)
            $Lessons = @(Get-LessonMatches $Manifest $BootstrapTags 3)
            $StartedAt.Stop()

            $Result = [pscustomobject]@{
                HarnessVersion = [string]$Manifest.harness_version
                Repository = $RepositoryRoot
                PowerShell = [string]$PSVersionTable.PSVersion
                Phase = if ($null -eq $PhaseContext) { $null } else { $PhaseContext.Status }
                Git = [pscustomobject]@{
                    Branch = $GitContext.Branch
                    DirtyCount = $GitContext.Entries.Count
                }
                ProtectedDirty = [pscustomobject]@{
                    Count = $ProtectedContext.Count
                    StableCount = @($ProtectedContext | Where-Object BaselineSatisfied).Count
                    Drift = @($ProtectedContext | Where-Object { -not $_.BaselineSatisfied })
                    Items = $ProtectedContext
                }
                Context = $Routing
                Lessons = $Lessons
                NextAction = if ($null -eq $PhaseContext) { 'No Phase state found; define or inspect the requested task.' } else { [string]$PhaseContext.Status.next_action }
                ElapsedMs = $StartedAt.ElapsedMilliseconds
            }

            if ($Json) {
                Write-Output ($Result | ConvertTo-Json -Depth 32)
            }
            else {
                Write-Output "AvidScript Agent Harness $($Result.HarnessVersion)"
                Write-Output "Repository: $RepositoryRoot"
                Write-Output "PowerShell: $($Result.PowerShell)"
                if ($null -ne $Result.Phase) {
                    Write-Output "Phase: $($Result.Phase.phase) / $($Result.Phase.effective_stage) / revision $($Result.Phase.revision)"
                    Write-Output "Completed batches: $(@($Result.Phase.completed_batches) -join ', ')"
                    Write-Output "Next: $($Result.Phase.next_action)"
                }
                Write-Output "Git: branch $($Result.Git.Branch), dirty $($Result.Git.DirtyCount)"
                Write-Output "Protected dirty: $($Result.ProtectedDirty.StableCount)/$($Result.ProtectedDirty.Count) baseline-satisfied"
                foreach ($Drift in @($Result.ProtectedDirty.Drift)) {
                    Write-Output "  DRIFT $($Drift.Path) expected=$($Drift.ExpectedSha256) current=$($Drift.CurrentSha256)"
                }
                Write-ContextText $Routing
                Write-Output 'Relevant lessons:'
                foreach ($Lesson in $Lessons) {
                    Write-Output "  - $($Lesson.Id): $($Lesson.Prevention)"
                }
                Write-Output "Elapsed: $($Result.ElapsedMs) ms"
            }

            if ($Result.ProtectedDirty.Drift.Count -gt 0) {
                exit 2
            }
        }
        'context' {
            $Routing = Get-RoutingContext $Manifest $Intent $NormalizedPaths
            if ($Json) {
                Write-Output ($Routing | ConvertTo-Json -Depth 16)
            }
            else {
                Write-Output "Intent: $Intent"
                Write-Output "Paths: $($NormalizedPaths -join ', ')"
                Write-ContextText $Routing
            }
        }
        'verify' {
            $Plan = Get-VerificationPlan $Manifest $Profile $Intent $NormalizedPaths
            if ($Json) {
                Write-Output ($Plan | ConvertTo-Json -Depth 16)
            }
            else {
                Write-Output "Verification profile: $($Plan.SelectedProfile) (requested $($Plan.RequestedProfile))"
                Write-Output "Execution: $($Plan.Execution)"
                Write-Output 'Checks:'
                foreach ($Check in $Plan.Checks) {
                    Write-Output "  - $Check"
                }
                Write-Output "Note: $($Plan.Note)"
            }
        }
        'lesson-query' {
            $Lessons = @(Get-LessonMatches $Manifest $Tags $Limit)
            if ($Json) {
                Write-Output ($Lessons | ConvertTo-Json -Depth 16)
            }
            else {
                Write-Output "Lessons: $($Lessons.Count)"
                foreach ($Lesson in $Lessons) {
                    Write-Output "[$($Lesson.Id)] tags=$($Lesson.Tags -join ',') source=$($Lesson.Source)"
                    Write-Output "  $($Lesson.Summary)"
                    Write-Output "  Prevention: $($Lesson.Prevention)"
                }
            }
        }
        'audit' {
            $Audit = Invoke-HarnessAudit $Manifest
            if ($Json) {
                Write-Output ($Audit | ConvertTo-Json -Depth 16)
            }
            else {
                foreach ($Check in $Audit.Checks) {
                    $State = if ($Check.Passed) { 'PASS' } else { 'FAIL' }
                    Write-Output "$State $($Check.Id): $($Check.Detail)"
                }
                Write-Output "Audit: $(if ($Audit.Passed) { 'PASS' } else { 'FAIL' })"
            }

            if (-not $Audit.Passed) {
                exit 1
            }
        }
    }
}
catch {
    $Message = ([string]$_.Exception.Message -replace '[\r\n]+', ' ').Trim()
    if ($Json) {
        Write-Output ([pscustomobject]@{ error = $Message } | ConvertTo-Json -Compress)
    }
    else {
        Write-Output "ERROR $Message"
    }
    exit 1
}
