Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-As36Integer {
    param($Value, [long]$Minimum, [long]$Maximum, [string]$Name)
    if ($Value -isnot [long] -and $Value -isnot [int] -and $Value -isnot [double]) { throw "Non-numeric $Name" }
    if ([double]::IsNaN($Value) -or [double]::IsInfinity($Value) -or
        $Value -lt $Minimum -or $Value -gt $Maximum -or [math]::Truncate($Value) -ne $Value) { throw "Invalid integer $Name" }
}

function Assert-As36CorrectnessReport {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Report)
    if ($Report.passed -isnot [bool] -or -not $Report.passed -or $Report.schema_version -ne 1 -or
        $Report.mode -cne 'UE Editor AngelScript VM' -or $Report.expected_case_count -ne 264 -or
        $Report.completed_case_count -ne 264 -or @($Report.cases).Count -ne 264 -or
        $Report.expected_rejection_controls -ne 17 -or @($Report.rejection_controls).Count -ne 17 -or
        $Report.float64_ref_out_object_identity_passed -isnot [bool] -or -not $Report.float64_ref_out_object_identity_passed -or
        $Report.per_actor_callback_state_passed -isnot [bool] -or -not $Report.per_actor_callback_state_passed -or
        -not [string]::IsNullOrEmpty($Report.error)) { throw 'Incomplete or failed correctness report.' }
    $Expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    for ($Id = 0; $Id -lt 12; ++$Id) {
        $Iterations = if ($Id -ge 10) { @(1, 2) } else { @(1, 2, 17, 257) }
        foreach ($Seed in @(0L, 1L, 1397313L, 2147483647L, 2147483648L, 4294967295L)) {
            foreach ($Count in $Iterations) { $null = $Expected.Add("$Id/$Seed/$Count") }
        }
    }
    foreach ($Case in $Report.cases) {
        Assert-As36Integer $Case.workload_id 0 11 'workload'
        Assert-As36Integer $Case.iterations 1 257 'iterations'
        Assert-As36Integer $Case.seed_u32 0 4294967295L 'seed'
        $Id = [int]$Case.workload_id
        $Count = [int]$Case.iterations
        if (-not $Expected.Remove("$Id/$($Case.seed_u32)/$Count")) { throw 'Duplicate or unexpected correctness tuple.' }
        foreach ($Name in @('actual_checksum_u32', 'expected_checksum_u32')) { Assert-As36Integer $Case.$Name 0 4294967295L $Name }
        foreach ($Name in @('actual_scalar', 'expected_scalar')) { Assert-As36Integer $Case.$Name -2147483648L 2147483647L $Name }
        if ($Case.matched -isnot [bool] -or -not $Case.matched -or
            $Case.actual_checksum_u32 -ne $Case.expected_checksum_u32 -or $Case.actual_scalar -ne $Case.expected_scalar -or
            @($Case.actual_native_calls).Count -ne 13 -or @($Case.expected_native_calls).Count -ne 13) { throw 'Case mismatch.' }
        $Counts = [long[]]::new(13)
        if ($Id -in @(1, 2, 4, 5, 6, 9)) { $Counts[$Id] = $Count }
        elseif ($Id -eq 10) { $Counts[2] = 32L * $Count; $Counts[4] = 8L * $Count; $Counts[5] = 4L * $Count; $Counts[12] = 2L * $Count }
        elseif ($Id -eq 11) { $Counts[2] = 4096L * $Count; $Counts[4] = 2048L * $Count; $Counts[5] = 512L * $Count; $Counts[12] = 512L * $Count }
        for ($Index = 0; $Index -lt 13; ++$Index) {
            Assert-As36Integer $Case.actual_native_calls[$Index] 0 ([long]::MaxValue) 'actual calls'
            Assert-As36Integer $Case.expected_native_calls[$Index] 0 ([long]::MaxValue) 'oracle calls'
            if ($Case.actual_native_calls[$Index] -ne $Counts[$Index] -or $Case.expected_native_calls[$Index] -ne $Counts[$Index]) { throw "Native call schedule mismatch: $Id/$Index" }
        }
    }
    if ($Expected.Count -ne 0) { throw 'Missing correctness tuples.' }
    $Names = @('unbound sample', 'execute before prepare', 'read before execute', 'negative workload', 'unknown workload',
        'zero iterations', 'negative iterations', 'micro iteration limit', 'gameplay iteration limit', 'duplicate execute',
        'duplicate read', 'native actor substituted for script', 'cross World fixture', 'non Game Thread sample',
        'World tearing down', 'destroyed fixture', 'destroyed actor')
    $Controls = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($Name in $Names) { $null = $Controls.Add($Name) }
    foreach ($Control in $Report.rejection_controls) {
        if (-not $Controls.Remove($Control.name) -or $Control.rejected -isnot [bool] -or -not $Control.rejected) { throw 'Invalid or duplicate rejection control.' }
    }
    if ($Controls.Count -ne 0) { throw 'Missing rejection controls.' }
}

function Assert-As36CompletionLog {
    param([Parameter(Mandatory)][string]$Text, [Parameter(Mandatory)][int]$ExitCode)
    if ($ExitCode -ne 0) { throw "Native validation exited $ExitCode" }
    if ($Text -match '(?im)^.*(?:Fatal error:|Assertion failed:|Unhandled Exception:|\b\w+: Error:)') { throw 'Error marker in validation log.' }
    if ([regex]::Matches($Text, 'AS36_VALIDATED cases=264 controls=17 mode=editor_vm').Count -ne 1 -or
        [regex]::Matches($Text, 'Commandlet .*As36LeadershipValidateCommandlet.*finished execution \(result 0\)').Count -ne 1) { throw 'Missing or duplicate completion marker.' }
}

function Assert-As36PhysicalPath {
    param([Parameter(Mandatory)][string]$Path)
    $Cursor = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($Cursor)) {
        if (Test-Path -LiteralPath $Cursor) {
            if ((Get-Item -LiteralPath $Cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse path is not an isolated physical destination: $Cursor" }
        }
        $Cursor = [IO.Path]::GetDirectoryName($Cursor)
    }
}

function Get-As36Sha256 {
    param([Parameter(Mandatory)][string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-As36ProtectedRoots {
    param([Parameter(Mandatory)][string]$AdapterRoot)
    $Repository = [IO.Path]::GetFullPath((Join-Path $AdapterRoot '../../../..'))
    $Repositories = @($Repository)
    # A detached worktree need not be inside a UE project. Resolve its common
    # Git directory rather than treating ../../ (potentially a drive root)
    # as the main project. Source-only copies still protect their own root.
    if ((Test-Path -LiteralPath (Join-Path $Repository '.git')) -and (Get-Command git -ErrorAction SilentlyContinue)) {
        $Common = @(& git -C $Repository rev-parse --path-format=absolute --git-common-dir 2>$null)
        if ($LASTEXITCODE -ne 0 -or $Common.Count -ne 1) { throw 'Cannot determine the common repository root.' }
        $CommonPath = [IO.Path]::GetFullPath($Common[0])
        if ([IO.Path]::GetFileName($CommonPath) -eq '.git') { $Repositories += [IO.Path]::GetDirectoryName($CommonPath) }
    }
    $Roots = @($Repositories)
    foreach ($Repo in $Repositories) {
        $Parent = [IO.Path]::GetDirectoryName($Repo)
        if ([IO.Path]::GetFileName($Parent) -ieq 'Plugins') {
            $Project = [IO.Path]::GetDirectoryName($Parent)
            if (@(Get-ChildItem -LiteralPath $Project -File -Filter '*.uproject').Count -gt 0) { $Roots += $Project }
        }
    }
    $Roots | Sort-Object -Unique
}

Export-ModuleMember -Function Assert-As36CorrectnessReport, Assert-As36CompletionLog, Assert-As36PhysicalPath, Get-As36Sha256, Get-As36ProtectedRoots
