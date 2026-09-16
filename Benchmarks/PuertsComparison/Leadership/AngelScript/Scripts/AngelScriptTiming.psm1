Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-As38Number {
    param($Value, [double]$Minimum, [double]$Maximum, [switch]$Integer)
    if ($Value -isnot [int] -and $Value -isnot [long] -and $Value -isnot [double]) { throw 'Expected a JSON number.' }
    if (-not [double]::IsFinite($Value) -or $Value -lt $Minimum -or $Value -gt $Maximum -or
        ($Integer -and [math]::Truncate($Value) -ne $Value)) { throw 'Invalid numeric value.' }
}

function Get-As38Workloads {
    param([ValidateSet('micro', 'gameplay')][string]$Suite)
    $Names = @('pure_integer', 'scalar_noop', 'scalar_add_int32', 'property_get_set', 'vector_value', 'object_roundtrip',
        'batch_scalar', 'callback_empty', 'callback_tick', 'vector_ref_out', 'gameplay_frame_small', 'gameplay_frame_dense')
    $Ids = if ($Suite -ceq 'micro') { @(7, 8, 0, 1, 2, 3, 4, 9, 5, 6) } else { @(10, 11) }
    for ($Index = 0; $Index -lt $Ids.Count; ++$Index) { [pscustomobject]@{ id = $Ids[$Index]; name = $Names[$Ids[$Index]]; index = $Index } }
}

function Get-As38Seed {
    param([int]$WorkloadIndex, [int]$SampleIndex)
    $Bits = 1397313L -bxor ((($WorkloadIndex + 1L) * 2654435769L) -band 4294967295L) -bxor ($SampleIndex + 1L)
    (($Bits * 1664525L + 1013904223L) -band 8388607L)
}

function Assert-As38TimingReport {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Report, [Parameter(Mandatory)]$Request)
    $Lanes = @('native_cpp', 'angelscript_editor_vm')
    if ($Request.suite -cnotin @('micro', 'gameplay') -or $Request.mode -cnotin @('calibration', 'measure') -or
        $Request.execution_mode -cne 'editor_vm' -or $Request.request_id -cnotmatch '^[0-9a-f]{32}$' -or
        $Request.schema_version -ne 1 -or $Request.seed -ne 1397313 -or $Request.process_runs -notin @(1, 5) -or
        $Request.warmup_samples -ne 5 -or $Request.timed_samples -ne 30) { throw 'Unexpected timing request.' }
    foreach ($Name in @('schema_version', 'seed', 'process_runs', 'process_run', 'warmup_samples', 'timed_samples')) {
        Assert-As38Number $Request.$Name -1 1397313 -Integer
    }
    $Calibration = $Request.mode -ceq 'calibration'
    if (($Calibration -and $Request.process_run -ne -1) -or
        (-not $Calibration -and ($Request.process_run -lt 0 -or $Request.process_run -ge $Request.process_runs))) { throw 'Invalid process index.' }
    foreach ($Name in @('schema_version', 'process_runs', 'process_run', 'warmup_samples', 'timed_samples')) {
        Assert-As38Number $Report.$Name -1 30 -Integer
        if ($Report.$Name -ne $Request.$Name) { throw "Report/request mismatch: $Name" }
    }
    foreach ($Name in @('request_id', 'mode', 'suite', 'execution_mode')) {
        if ($Report.$Name -isnot [string] -or $Report.$Name -cne $Request.$Name) { throw "Report/request mismatch: $Name" }
    }
    if ($Report.passed -isnot [bool] -or -not $Report.passed -or -not [string]::IsNullOrEmpty($Report.error) -or
        $Report.static_jit_transpiled_code_loaded -isnot [bool] -or $Report.static_jit_transpiled_code_loaded) { throw 'Failed report or wrong execution mode.' }
    Assert-As38Number $Report.seconds_per_cycle ([double]::Epsilon) 1
    Assert-As38Number $Report.minimum_sample_milliseconds 5 5
    Assert-As38Number $Report.calibration_confirmation_samples 3 3 -Integer
    $Workloads = @(Get-As38Workloads $Request.suite)
    $Minimum = if ($Request.suite -ceq 'micro') { 1000 } else { 1 }
    $Maximum = if ($Request.suite -ceq 'micro') { 10000000 } else { 1048576 }
    if (@($Report.iteration_counts.PSObject.Properties).Count -ne $Workloads.Count) { throw 'Wrong iteration workload set.' }
    if (-not $Calibration -and @($Request.iteration_counts.PSObject.Properties).Count -ne $Workloads.Count) { throw 'Wrong frozen workload set.' }
    foreach ($Workload in $Workloads) {
        $Counts = $Report.iteration_counts.($Workload.name)
        if (@($Counts.PSObject.Properties).Count -ne 2) { throw 'Wrong lane set.' }
        foreach ($Lane in $Lanes) {
            Assert-As38Number $Counts.$Lane $Minimum $Maximum -Integer
            if (-not $Calibration) {
                $Frozen = $Request.iteration_counts.($Workload.name)
                if (@($Frozen.PSObject.Properties).Count -ne 2) { throw 'Wrong frozen lane set.' }
                Assert-As38Number $Frozen.$Lane $Minimum $Maximum -Integer
                if ($Counts.$Lane -ne $Frozen.$Lane) { throw 'Iteration count changed after calibration.' }
            }
        }
    }
    if (@($Report.samples).Count -eq 0) { throw 'Missing timing samples.' }
    $LastGroup = -1
    $Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($Row in $Report.samples) {
        Assert-As38Number $Row.workload_index 0 ($Workloads.Count - 1) -Integer
        $Workload = $Workloads[$Row.workload_index]
        Assert-As38Number $Row.workload_id $Workload.id $Workload.id -Integer
        if ($Row.workload -cne $Workload.name -or $Row.lane -cnotin $Lanes) { throw 'Unexpected workload or lane.' }
        Assert-As38Number $Row.sample_index 0 256 -Integer
        Assert-As38Number $Row.lane_position 0 1 -Integer
        Assert-As38Number $Row.seed_u32 0 8388607 -Integer
        if ($Row.seed_u32 -ne (Get-As38Seed $Workload.index $Row.sample_index)) { throw 'Sample seed mismatch.' }
        Assert-As38Number $Row.iterations $Minimum $Maximum -Integer
        Assert-As38Number $Row.elapsed_cycles 1 9007199254740991 -Integer
        Assert-As38Number $Row.milliseconds ([double]::Epsilon) 1800000
        Assert-As38Number $Row.working_set_delta_bytes -9007199254740991 9007199254740991 -Integer
        $Milliseconds = $Row.elapsed_cycles * $Report.seconds_per_cycle * 1000
        if ([math]::Abs($Milliseconds - $Row.milliseconds) -gt [math]::Max(0.000001, $Milliseconds * 0.000000001)) { throw 'Cycle/time mismatch.' }
        foreach ($Name in @('actual_checksum_u32', 'expected_checksum_u32')) { Assert-As38Number $Row.$Name 0 4294967295 -Integer }
        foreach ($Name in @('actual_scalar', 'expected_scalar')) { Assert-As38Number $Row.$Name -2147483648 2147483647 -Integer }
        if ($Row.matched -isnot [bool] -or -not $Row.matched -or $Row.actual_checksum_u32 -ne $Row.expected_checksum_u32 -or
            $Row.actual_scalar -ne $Row.expected_scalar -or @($Row.actual_native_calls).Count -ne 13 -or @($Row.expected_native_calls).Count -ne 13) { throw 'Incorrect sample result.' }
        $Expected = [long[]]::new(13)
        if ($Workload.id -in @(1, 2, 4, 5, 6, 9)) { $Expected[$Workload.id] = $Row.iterations }
        elseif ($Workload.id -eq 10) { $Expected[2] = 32L * $Row.iterations; $Expected[4] = 8L * $Row.iterations; $Expected[5] = 4L * $Row.iterations; $Expected[12] = 2L * $Row.iterations }
        elseif ($Workload.id -eq 11) { $Expected[2] = 4096L * $Row.iterations; $Expected[4] = 2048L * $Row.iterations; $Expected[5] = 512L * $Row.iterations; $Expected[12] = 512L * $Row.iterations }
        for ($Counter = 0; $Counter -lt 13; ++$Counter) {
            Assert-As38Number $Row.actual_native_calls[$Counter] 0 9007199254740991 -Integer
            Assert-As38Number $Row.expected_native_calls[$Counter] 0 9007199254740991 -Integer
            if ($Row.actual_native_calls[$Counter] -ne $Expected[$Counter] -or $Row.expected_native_calls[$Counter] -ne $Expected[$Counter]) { throw 'Native call schedule mismatch.' }
        }
        $Key = "$($Row.workload)/$($Row.lane)/$($Row.phase)/$($Row.sample_index)"
        if (-not $Seen.Add($Key)) { throw 'Duplicate sample.' }
        if ($Calibration) {
            $LaneIndex = [array]::IndexOf($Lanes, $Row.lane)
            $GroupIndex = $Workload.index * 2 + $LaneIndex
            if ($GroupIndex -lt $LastGroup -or $Row.lane_position -ne $LaneIndex) { throw 'Calibration order changed.' }
            $LastGroup = $GroupIndex
        }
    }
    if ($Calibration) {
        foreach ($Workload in $Workloads) {
            foreach ($Lane in $Lanes) {
                $Rows = @($Report.samples | Where-Object { $_.workload -ceq $Workload.name -and $_.lane -ceq $Lane })
                $Cursor = 0
                $Iterations = $Minimum
                $Confirmed = $false
                while ($Cursor -lt $Rows.Count) {
                    $Probe = $Rows[$Cursor]
                    if ($Probe.phase -cne 'calibration_probe' -or $Probe.sample_index -ne $Cursor -or $Probe.iterations -ne $Iterations) { throw 'Invalid calibration probe progression.' }
                    $Cursor++
                    if ($Probe.milliseconds -ge 5) {
                        if ($Cursor + 3 -gt $Rows.Count) { throw 'Missing calibration confirmations.' }
                        $Times = @()
                        for ($Index = 0; $Index -lt 3; ++$Index) {
                            $Row = $Rows[$Cursor]
                            if ($Row.phase -cne 'calibration_confirm' -or $Row.sample_index -ne $Cursor -or $Row.iterations -ne $Iterations) { throw 'Invalid confirmation sample.' }
                            $Times += $Row.milliseconds
                            $Cursor++
                        }
                        if (@($Times | Sort-Object)[1] -ge 5) { $Confirmed = $true; break }
                    }
                    if ($Iterations -ge $Maximum) { throw 'Unconfirmed calibration at maximum iterations.' }
                    $Iterations = [math]::Min($Maximum, $Iterations * 2)
                }
                if (-not $Confirmed -or $Cursor -ne $Rows.Count -or $Iterations -ne $Report.iteration_counts.($Workload.name).$Lane) { throw 'Invalid final calibrated iteration count.' }
            }
        }
    } else {
        if (@($Report.samples).Count -ne $Workloads.Count * 70) { throw 'Incomplete measured sample set.' }
        $Cursor = 0
        foreach ($Workload in $Workloads) {
            foreach ($Phase in @('warmup', 'timed')) {
                $Count = if ($Phase -ceq 'warmup') { 5 } else { 30 }
                for ($Index = 0; $Index -lt $Count; ++$Index) {
                    for ($Position = 0; $Position -lt 2; ++$Position) {
                        $Row = $Report.samples[$Cursor++]
                        $Lane = $Lanes[($Request.process_run + $Workload.index + $Index + $Position) % 2]
                        if ($Row.workload -cne $Workload.name -or $Row.phase -cne $Phase -or $Row.sample_index -ne $Index -or
                            $Row.lane_position -ne $Position -or $Row.lane -cne $Lane -or
                            $Row.iterations -ne $Request.iteration_counts.($Workload.name).$Lane) { throw 'Measured sample order or frozen count mismatch.' }
                    }
                }
            }
        }
    }
}

function Assert-As38TimingLog {
    param([string]$Text, [int]$ExitCode, $Report)
    if ($ExitCode -ne 0 -or $Text -match '(?im)^.*(?:Fatal error:|Assertion failed:|Unhandled Exception:|\b\w+: Error:)') { throw 'Failed timing process or error log.' }
    $Marker = 'AS38_TIMING_VALIDATED mode=' + $Report.mode + ' suite=' + $Report.suite + ' samples=' + @($Report.samples).Count
    if ([regex]::Matches($Text, [regex]::Escape($Marker) + '(?=\s|$)').Count -ne 1 -or
        [regex]::Matches($Text, 'Commandlet .*As38LeadershipTimingCommandlet.*finished execution \(result 0\)').Count -ne 1) { throw 'Missing or duplicate timing completion.' }
}

Export-ModuleMember -Function Get-As38Workloads, Get-As38Seed, Assert-As38TimingReport, Assert-As38TimingLog
