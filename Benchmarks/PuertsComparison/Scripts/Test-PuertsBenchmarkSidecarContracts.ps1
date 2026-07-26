[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BenchmarkRoot = Split-Path -Parent $ScriptRoot
$RunnerPath = Join-Path $ScriptRoot 'Invoke-PuertsBenchmarkProcesses.ps1'
$AggregatorPath = Join-Path $ScriptRoot 'Merge-PuertsBenchmarkResults.ps1'
$CommonPath = Join-Path $ScriptRoot 'PuertsBenchmarkSidecar.Common.ps1'
$RequestSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkProcessRequest.schema.json'
$CalibrationSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkCalibration.schema.json'
$RawSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkProcessResult.schema.json'
$AggregateSchemaPath = Join-Path $BenchmarkRoot 'Schema/BenchmarkAggregate.schema.json'
$FixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('AvidScriptP53SidecarTest-' + [Guid]::NewGuid().ToString('N'))

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "ASP53ST1000 $Message"
    }
}

function Assert-Near {
    param(
        [Parameter(Mandatory = $true)][double]$Actual,
        [Parameter(Mandatory = $true)][double]$Expected,
        [double]$Tolerance = 0.000001,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if ([Math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "ASP53ST1001 $Message，实际值=$Actual，期望值=$Expected"
    }
}

function Write-NewJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )

    $Json = (($Value | ConvertTo-Json -Depth 64) -replace "`r`n", "`n") + "`n"
    [System.IO.File]::WriteAllText($Path, $Json, [System.Text.UTF8Encoding]::new($false))
}

function Invoke-ExpectedFailure {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$ExpectedCode
    )

    try {
        & $Action
        throw "ASP53ST1002 预期失败但命令成功：$ExpectedCode"
    }
    catch {
        Assert-True ($_.Exception.Message.Contains($ExpectedCode)) "失败信息未包含 $ExpectedCode：$($_.Exception.Message)"
    }
}

function Copy-AndMutateRawResult {
    param(
        [Parameter(Mandatory = $true)][string]$SourceAttempt,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Mutation
    )

    $Candidate = Join-Path $FixtureRoot $Name
    Copy-Item -LiteralPath $SourceAttempt -Destination $Candidate -Recurse
    $RawPath = Join-Path $Candidate 'runs/02/raw-result.json'
    $Raw = Get-Content -LiteralPath $RawPath -Raw | ConvertFrom-Json
    & $Mutation $Raw
    Write-NewJson $RawPath $Raw
    return $Candidate
}

foreach ($RequiredPath in @(
    $RunnerPath,
    $AggregatorPath,
    $CommonPath,
    $RequestSchemaPath,
    $CalibrationSchemaPath,
    $RawSchemaPath,
    $AggregateSchemaPath)) {
    Assert-True (Test-Path -LiteralPath $RequiredPath -PathType Leaf) "缺少 P53.3 文件：$RequiredPath"
}

$ParserErrors = $null
$Tokens = $null
foreach ($ScriptPath in @($RunnerPath, $AggregatorPath, $CommonPath)) {
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $ScriptPath,
        [ref]$Tokens,
        [ref]$ParserErrors)
    Assert-True (@($ParserErrors).Count -eq 0) "PowerShell 解析失败：$ScriptPath"
}

$TrackedProfile = Get-Content -LiteralPath (Join-Path $BenchmarkRoot 'Config/BenchmarkProfile.json') -Raw | ConvertFrom-Json
Assert-True ([int]$TrackedProfile.process_runs -eq 5) '正式 profile 必须固定执行 5 个独立进程'
Assert-True ([int]$TrackedProfile.warmup_samples -eq 5) '正式 profile 必须固定 5 次预热'
Assert-True ([int]$TrackedProfile.timed_samples -eq 30) '正式 profile 必须固定 30 个计时样本'

try {
    New-Item -ItemType Directory -Path $FixtureRoot | Out-Null
    $ProjectPath = Join-Path $FixtureRoot 'SidecarFixture.uproject'
    [System.IO.File]::WriteAllText($ProjectPath, "{}`n", [System.Text.UTF8Encoding]::new($false))

    $ProfilePath = Join-Path $FixtureRoot 'profile.json'
    $Profile = [ordered]@{
        schema_version = 1
        profile_id = 'sidecar-contract'
        platform = 'Win64'
        target = 'SidecarFixtureEditor'
        configuration = 'Development'
        null_rhi = $true
        process_runs = 5
        warmup_samples = 5
        timed_samples = 30
        minimum_sample_milliseconds = 5.0
        minimum_iterations = 100
        maximum_iterations = 100000
        seed = 1397313
        lanes = @(
            'native_cpp',
            'puerts_v8_reflection',
            'puerts_v8_static',
            'avidscript_wamr'
        )
        workloads = @('scalar_noop', 'property_get_set')
    }
    Write-NewJson $ProfilePath $Profile

    $FakeEditorPath = Join-Path $FixtureRoot 'Fake-UnrealEditor-Cmd.ps1'
    $FakeEditor = @'
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$EditorArguments
)

$ErrorActionPreference = 'Stop'
function Get-NativeSwitchValue {
    param([Parameter(Mandatory = $true)][string]$Prefix)

    for ($Index = 0; $Index -lt $EditorArguments.Count; ++$Index) {
        if (-not $EditorArguments[$Index].StartsWith($Prefix)) {
            continue
        }
        $Value = $EditorArguments[$Index].Substring($Prefix.Length)
        # pwsh 的脚本参数绑定器会把 -Key=C:\Path 拆成 C 与 \Path；真实 native Editor 不会。
        if ($Value -cmatch '^[A-Za-z]$' -and
            $Index + 1 -lt $EditorArguments.Count -and
            $EditorArguments[$Index + 1].StartsWith('\')) {
            return $Value + ':' + $EditorArguments[$Index + 1]
        }
        return $Value
    }
    throw "fake editor did not receive switch: $Prefix"
}

foreach ($RequiredArgument in @('-run=AvidScriptPerfRun', '-Multiprocess', '-NoCompile')) {
    if ($EditorArguments -cnotcontains $RequiredArgument) {
        throw "fake editor missing commandlet argument: $RequiredArgument"
    }
}
if (@($EditorArguments | Where-Object { $_.StartsWith('-ExecCmds=') }).Count -ne 0) {
    throw 'fake editor must not receive -ExecCmds'
}

$RequestPath = Get-NativeSwitchValue '-AvidScriptPerfRequest='
$ResultPath = Get-NativeSwitchValue '-AvidScriptPerfResult='
if (-not (Test-Path -LiteralPath $RequestPath -PathType Leaf)) {
    throw "fake editor request missing: path=[$RequestPath] args=[$($EditorArguments -join '][')]"
}
$Request = Get-Content -LiteralPath $RequestPath -Raw | ConvertFrom-Json
$Result = $null
if ([string]$Request.mode -ceq 'calibration') {
    $IterationCounts = [ordered]@{}
    for ($WorkloadIndex = 0; $WorkloadIndex -lt $Request.workloads.Count; ++$WorkloadIndex) {
        $IterationCounts[[string]$Request.workloads[$WorkloadIndex]] = [int64](100 * ($WorkloadIndex + 1))
    }
    $Result = [ordered]@{
        schema_version = [int]$Request.result_schema.version
        calibration_id = [string]$Request.attempt_id
        timer_frequency_hz = 1000000000
        provenance = $Request.provenance
        iteration_counts = $IterationCounts
    }
}
elseif ([string]$Request.mode -ceq 'timed') {
    function Get-FakeSampleSeed {
        param([int]$Seed, [int]$WorkloadIndex, [int]$SampleIndex)

        [uint64]$SeedBits = [int64]$Seed -band 0xffffffffL
        [uint64]$WorkloadBits = (
            [uint64]($WorkloadIndex + 1) * [uint64]2654435769) -band 0xffffffffL
        [uint64]$Value = ($SeedBits -bxor $WorkloadBits -bxor [uint64]($SampleIndex + 1)) -band 0xffffffffL
        [uint64]$Mixed = (
            ($Value * [uint64]1664525) + [uint64]1013904223) -band 0xffffffffL
        return [int]($Mixed -band 0x007fffffL)
    }

    $Samples = [System.Collections.Generic.List[object]]::new()
    for ($WorkloadIndex = 0; $WorkloadIndex -lt $Request.workloads.Count; ++$WorkloadIndex) {
        $Workload = [string]$Request.workloads[$WorkloadIndex]
        for ($SampleIndex = 0; $SampleIndex -lt [int]$Request.timed_samples; ++$SampleIndex) {
            $SampleSeed = Get-FakeSampleSeed `
                -Seed ([int]$Request.seed) `
                -WorkloadIndex $WorkloadIndex `
                -SampleIndex $SampleIndex
            $Checksum = 1000 + ($WorkloadIndex * 100) + $SampleIndex
            for ($LaneIndex = 0; $LaneIndex -lt $Request.lanes.Count; ++$LaneIndex) {
                $Lane = [string]$Request.lanes[$LaneIndex]
                $LanePosition = [Array]::IndexOf([object[]]@($Request.lane_order), $Lane)
                $Iterations = [int64]$Request.iteration_counts.$Workload
                $CyclesPerOperation = 1 + $SampleIndex + ($LaneIndex * 100) + ($WorkloadIndex * 1000)
                $Samples.Add([ordered]@{
                    process_run = [int]$Request.process_run
                    lane = $Lane
                    lane_position = $LanePosition
                    workload = $Workload
                    sample_index = $SampleIndex
                    seed = $SampleSeed
                    iterations = $Iterations
                    elapsed_cycles = [int64]($Iterations * $CyclesPerOperation)
                    checksum = [int64]$Checksum
                    expected_checksum = [int64]$Checksum
                    final_scalar = [double](10 + $WorkloadIndex + $SampleIndex)
                    expected_final_scalar = [double](10 + $WorkloadIndex + $SampleIndex)
                    operation_call_count = $Iterations
                    expected_operation_call_count = $Iterations
                    host_import_call_count = [int64]($LaneIndex * $Iterations)
                    expected_host_import_call_count = [int64]($LaneIndex * $Iterations)
                    correct = $true
                })
            }
        }
    }
    $Result = [ordered]@{
        schema_version = [int]$Request.result_schema.version
        run_id = [string]$Request.attempt_id
        process_run = [int]$Request.process_run
        lane_order = @($Request.lane_order)
        timer_frequency_hz = 1000000000
        provenance = $Request.provenance
        samples = $Samples
    }
}
else {
    throw "fake editor received unknown mode: $($Request.mode)"
}

$Json = (($Result | ConvertTo-Json -Depth 64) -replace "`r`n", "`n") + "`n"
$Encoding = [System.Text.UTF8Encoding]::new($false)
$Stream = [System.IO.FileStream]::new(
    [string]$Request.result_write.temporary_path,
    [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::Write,
    [System.IO.FileShare]::Read)
try {
    $Writer = [System.IO.StreamWriter]::new($Stream, $Encoding, 4096, $true)
    try {
        $Writer.Write($Json)
        $Writer.Flush()
        $Stream.Flush($true)
    }
    finally {
        $Writer.Dispose()
    }
}
finally {
    $Stream.Dispose()
}
[System.IO.File]::Move([string]$Request.result_write.temporary_path, $ResultPath)

Write-Output "fake-editor-pid=$PID"
'@
    [System.IO.File]::WriteAllText($FakeEditorPath, $FakeEditor, [System.Text.UTF8Encoding]::new($false))

    $OutputRoot = Join-Path $FixtureRoot 'attempts'
    $PowerShellExecutable = (Get-Command pwsh -ErrorAction Stop).Source
    $RunnerArguments = @{
        ProjectPath = $ProjectPath
        EditorExecutable = $PowerShellExecutable
        EditorPrefixArguments = @('-NoProfile', '-File', $FakeEditorPath)
        ProfilePath = $ProfilePath
        OutputRoot = $OutputRoot
        UeVersion = '5.8.0'
        UeBuildId = 'sidecar-engine-build'
        AvidScriptCommit = ('a' * 40)
        AvidScriptTreeSha = ('d' * 40)
        AvidScriptDirty = $false
        PuertsCommit = ('b' * 40)
        PuertsBackendSha256 = ('c' * 64)
        CpuModel = 'Contract CPU'
        OperatingSystem = 'Contract OS'
        WamrMode = 'interpreter'
        V8Mode = 'jit'
        WasmSha256 = ('e' * 64)
        ManifestSha256 = ('f' * 64)
    }

    $FirstRunOutput = & $RunnerPath @RunnerArguments
    $FirstRun = $FirstRunOutput | ConvertFrom-Json
    Assert-True ($FirstRun.succeeded -eq $true) '5 进程 sidecar 未成功完成'
    Assert-True ([int]$FirstRun.process_run_count -eq 5) 'sidecar 未执行 5 个进程'
    $FirstAttempt = [string]$FirstRun.attempt_path
    Assert-True (Test-Path -LiteralPath $FirstAttempt -PathType Container) 'attempt 目录不存在'

    $Manifest = Get-Content -LiteralPath (Join-Path $FirstAttempt 'attempt.json') -Raw | ConvertFrom-Json
    Assert-True ($Manifest.profile.sha256 -cmatch '^[0-9a-f]{64}$') 'attempt 未固定 profile 哈希'
    Assert-True ($Manifest.result_schema.sha256 -cmatch '^[0-9a-f]{64}$') 'attempt 未固定 raw schema 哈希'
    Assert-True ([int]$Manifest.process_runs.Count -eq 5) 'attempt 清单没有 5 个进程'
    Assert-True ($Manifest.calibration.sha256 -cmatch '^[0-9a-f]{64}$') 'attempt 未固定 calibration raw 哈希'
    Assert-True (Test-Path -LiteralPath (Join-Path $FirstAttempt $Manifest.calibration.raw_path) -PathType Leaf) 'attempt 未保留 calibration.json'

    $ExpectedLaneOrders = @(
        'native_cpp,puerts_v8_reflection,puerts_v8_static,avidscript_wamr',
        'puerts_v8_reflection,puerts_v8_static,avidscript_wamr,native_cpp',
        'puerts_v8_static,avidscript_wamr,native_cpp,puerts_v8_reflection',
        'avidscript_wamr,native_cpp,puerts_v8_reflection,puerts_v8_static',
        'native_cpp,puerts_v8_reflection,puerts_v8_static,avidscript_wamr'
    )
    $ProcessIds = [System.Collections.Generic.HashSet[int]]::new()
    $CalibrationProcess = Get-Content -LiteralPath (Join-Path $FirstAttempt $Manifest.calibration.process_metadata_path) -Raw | ConvertFrom-Json
    Assert-True ($ProcessIds.Add([int]$CalibrationProcess.pid)) 'calibration 未使用独立 Editor 进程'
    $ExpectedIterationCounts = ($Manifest.calibration.iteration_counts | ConvertTo-Json -Compress)
    for ($ProcessRun = 0; $ProcessRun -lt 5; ++$ProcessRun) {
        $RunDirectory = Join-Path $FirstAttempt ('runs/{0:D2}' -f $ProcessRun)
        $Request = Get-Content -LiteralPath (Join-Path $RunDirectory 'request.json') -Raw | ConvertFrom-Json
        $Process = Get-Content -LiteralPath (Join-Path $RunDirectory 'process.json') -Raw | ConvertFrom-Json
        Assert-True ([int]$Request.schema_version -eq 1) "进程 $ProcessRun request 缺少 schema_version"
        Assert-True ([string]$Request.mode -ceq 'timed') "进程 $ProcessRun request 不是 timed 模式"
        Assert-True ((@($Request.lane_order) -join ',') -ceq $ExpectedLaneOrders[$ProcessRun]) "lane 顺序未按进程轮转：$ProcessRun"
        Assert-True ([int]$Request.warmup_samples -eq 5) "进程 $ProcessRun 未使用 profile 预热次数"
        Assert-True ([int]$Request.timed_samples -eq 30) "进程 $ProcessRun 未使用 profile 样本数"
        Assert-True ([string]$Request.result_write.strategy -ceq 'same_directory_temporary_then_atomic_rename') "进程 $ProcessRun 未声明原子发布策略"
        Assert-True ($Request.provenance.null_rhi -eq $true) "进程 $ProcessRun 未固定 NullRHI"
        Assert-True ($Request.provenance.avidscript_dirty -eq $false) "进程 $ProcessRun 允许 dirty AvidScript"
        Assert-True (($Request.iteration_counts | ConvertTo-Json -Compress) -ceq $ExpectedIterationCounts) "进程 $ProcessRun 未复用 calibration iteration map"
        Assert-True ($ProcessIds.Add([int]$Process.pid)) "进程 $ProcessRun 未使用新的 Editor 进程"
        Assert-True (Test-Path -LiteralPath (Join-Path $RunDirectory 'raw-result.json') -PathType Leaf) "进程 $ProcessRun 未保留 raw JSON"
    }
    Assert-True ($ProcessIds.Count -eq 6) '一次 attempt 必须使用 1 calibration + 5 timed 独立 PID'
    Assert-True (@(Get-ChildItem -LiteralPath $FirstAttempt -Recurse -File -Filter '*.tmp').Count -eq 0) '成功 attempt 遗留未发布临时文件'

    $AggregatePath = Join-Path $FirstAttempt 'aggregate.json'
    $AggregateRaw = Get-Content -LiteralPath $AggregatePath -Raw
    Assert-True ($AggregateRaw | Test-Json -SchemaFile $AggregateSchemaPath) '聚合结果不符合 Schema'
    $Aggregate = $AggregateRaw | ConvertFrom-Json
    Assert-True ([int]$Aggregate.samples.Count -eq 1200) '聚合结果未保留全部 raw samples'
    Assert-True ([int]$Aggregate.process_statistics.Count -eq 40) '每进程统计矩阵不完整'
    Assert-True ([int]$Aggregate.cross_process_statistics.Count -eq 8) '跨进程统计矩阵不完整'
    Assert-True ([int]$Aggregate.paired_comparisons.Count -eq 6) '同进程配对比较矩阵不完整'
    Assert-True ([int]$Aggregate.descriptive_pooled_statistics.Count -eq 8) '池化描述统计矩阵不完整'
    $ScalarNativeProcess = @($Aggregate.process_statistics | Where-Object {
        [int]$_.process_run -eq 0 -and
        $_.lane -ceq 'native_cpp' -and $_.workload -ceq 'scalar_noop'
    })
    Assert-True ($ScalarNativeProcess.Count -eq 1) '缺少 process/native_cpp/scalar_noop 统计'
    Assert-True ([int]$ScalarNativeProcess[0].sample_count -eq 30) '每个 process/lane/workload 应有 30 个样本'
    Assert-Near ([double]$ScalarNativeProcess[0].p50_ns_per_operation) 15.0 -Message '每进程 P50 计算错误'
    Assert-Near ([double]$ScalarNativeProcess[0].p95_ns_per_operation) 29.0 -Message '每进程 P95 计算错误'
    Assert-Near ([double]$ScalarNativeProcess[0].mad_ns_per_operation) 7.0 -Message '每进程 MAD 计算错误'
    $ScalarNativeCrossProcess = @($Aggregate.cross_process_statistics | Where-Object {
        $_.lane -ceq 'native_cpp' -and $_.workload -ceq 'scalar_noop'
    })
    Assert-True ($ScalarNativeCrossProcess.Count -eq 1) '缺少跨进程 native_cpp/scalar_noop 统计'
    Assert-True ([int]$ScalarNativeCrossProcess[0].process_count -eq 5) '跨进程统计必须基于 5 个 process summary'
    Assert-Near ([double]$ScalarNativeCrossProcess[0].p50_of_process_p50_ns_per_operation) 15.0 -Message '跨进程 P50 计算错误'
    Assert-Near ([double]$ScalarNativeCrossProcess[0].p95_of_process_p50_ns_per_operation) 15.0 -Message '跨进程 P95 计算错误'
    Assert-Near ([double]$ScalarNativeCrossProcess[0].mad_of_process_p50_ns_per_operation) 0.0 -Message '跨进程 MAD 计算错误'
    $ReflectionPair = @($Aggregate.paired_comparisons | Where-Object {
        $_.lane -ceq 'puerts_v8_reflection' -and $_.workload -ceq 'scalar_noop'
    })
    Assert-True ($ReflectionPair.Count -eq 1 -and [int]$ReflectionPair[0].per_process_ratios.Count -eq 5) '配对比较未保留 5 个 process ratio'
    Assert-Near ([double]$ReflectionPair[0].p50_ratio) (115.0 / 15.0) -Message '同进程配对 ratio 计算错误'
    $ScalarNativePooled = @($Aggregate.descriptive_pooled_statistics | Where-Object {
        $_.lane -ceq 'native_cpp' -and $_.workload -ceq 'scalar_noop'
    })
    Assert-True ($ScalarNativePooled.Count -eq 1 -and $ScalarNativePooled[0].descriptive_only -eq $true) '池化统计必须明确标记 descriptive_only'
    Assert-True ([int]$ScalarNativePooled[0].sample_count -eq 150) '池化描述统计应保留 150 个样本'
    $ExpectedGeometricMean = [Math]::Exp((1..30 | ForEach-Object { [Math]::Log([double]$_) } | Measure-Object -Average).Average)
    Assert-Near ([double]$ScalarNativePooled[0].geometric_mean_ns_per_operation) $ExpectedGeometricMean 0.000001 -Message '池化几何平均数计算错误'

    $RawAuditSample = (Get-Content -LiteralPath (Join-Path $FirstAttempt 'runs/00/raw-result.json') -Raw | ConvertFrom-Json).samples[0]
    foreach ($Field in @(
        'lane_position',
        'seed',
        'expected_checksum',
        'final_scalar',
        'expected_final_scalar',
        'operation_call_count',
        'expected_operation_call_count',
        'host_import_call_count',
        'expected_host_import_call_count')) {
        Assert-True ($null -ne $RawAuditSample.PSObject.Properties[$Field]) "raw sample 缺少公平性字段：$Field"
    }

    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $FirstAttempt -Mode Aggregate | Out-Null
    } 'ASP53S2026'

    $SecondRunOutput = & $RunnerPath @RunnerArguments
    $SecondRun = $SecondRunOutput | ConvertFrom-Json
    Assert-True ([string]$SecondRun.attempt_path -cne $FirstAttempt) '重复执行覆盖了已有 attempt'
    Assert-True (Test-Path -LiteralPath $AggregatePath -PathType Leaf) '第二次执行破坏了第一次 raw/aggregate 证据'

    $MixedCommit = Copy-AndMutateRawResult $FirstAttempt 'mixed-commit' {
        param($Raw)
        $Raw.provenance.avidscript_commit = ('d' * 40)
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $MixedCommit -Mode Validate | Out-Null
    } 'ASP53S2014'

    $MixedConfig = Copy-AndMutateRawResult $FirstAttempt 'mixed-config' {
        param($Raw)
        $Raw.provenance.configuration = 'Shipping'
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $MixedConfig -Mode Validate | Out-Null
    } 'ASP53S2014'

    $MixedEngine = Copy-AndMutateRawResult $FirstAttempt 'mixed-engine' {
        param($Raw)
        $Raw.provenance.ue_build_id = 'other-engine-build'
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $MixedEngine -Mode Validate | Out-Null
    } 'ASP53S2014'

    $MixedSchema = Copy-AndMutateRawResult $FirstAttempt 'mixed-schema' {
        param($Raw)
        $Raw.provenance.result_schema_sha256 = ('e' * 64)
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $MixedSchema -Mode Validate | Out-Null
    } 'ASP53S2014'

    $MixedIterationRequest = Join-Path $FixtureRoot 'mixed-iteration-request'
    Copy-Item -LiteralPath $FirstAttempt -Destination $MixedIterationRequest -Recurse
    $TimedRequestPath = Join-Path $MixedIterationRequest 'runs/02/request.json'
    $TimedRequest = Get-Content -LiteralPath $TimedRequestPath -Raw | ConvertFrom-Json
    $TimedRequest.iteration_counts.scalar_noop = [int64]$TimedRequest.iteration_counts.scalar_noop + 1
    Write-NewJson $TimedRequestPath $TimedRequest
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $MixedIterationRequest -Mode Validate | Out-Null
    } 'ASP53S2046'

    $BadSeed = Copy-AndMutateRawResult $FirstAttempt 'bad-seed' {
        param($Raw)
        $Raw.samples[0].seed = [int]$Raw.samples[0].seed + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadSeed -Mode Validate | Out-Null
    } 'ASP53S2035'

    $BadExpectedChecksum = Copy-AndMutateRawResult $FirstAttempt 'bad-expected-checksum' {
        param($Raw)
        $Raw.samples[0].expected_checksum = [int64]$Raw.samples[0].checksum + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadExpectedChecksum -Mode Validate | Out-Null
    } 'ASP53S2036'

    $BadFinalScalar = Copy-AndMutateRawResult $FirstAttempt 'bad-final-scalar' {
        param($Raw)
        $Raw.samples[0].expected_final_scalar = [double]$Raw.samples[0].final_scalar + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadFinalScalar -Mode Validate | Out-Null
    } 'ASP53S2037'

    $BadOperationCount = Copy-AndMutateRawResult $FirstAttempt 'bad-operation-count' {
        param($Raw)
        $Raw.samples[0].expected_operation_call_count = [int64]$Raw.samples[0].operation_call_count + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadOperationCount -Mode Validate | Out-Null
    } 'ASP53S2038'

    $BadHostImportCount = Copy-AndMutateRawResult $FirstAttempt 'bad-host-import-count' {
        param($Raw)
        $Raw.samples[0].expected_host_import_call_count = [int64]$Raw.samples[0].host_import_call_count + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadHostImportCount -Mode Validate | Out-Null
    } 'ASP53S2039'

    $BadLanePosition = Copy-AndMutateRawResult $FirstAttempt 'bad-lane-position' {
        param($Raw)
        $Raw.samples[0].lane_position = [int]$Raw.samples[1].lane_position
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadLanePosition -Mode Validate | Out-Null
    } 'ASP53S2040'

    $BadIterations = Copy-AndMutateRawResult $FirstAttempt 'bad-iterations' {
        param($Raw)
        $Raw.samples[0].iterations = [int64]$Raw.samples[0].iterations + 1
    }
    Invoke-ExpectedFailure {
        & $AggregatorPath -AttemptPath $BadIterations -Mode Validate | Out-Null
    } 'ASP53S2044'
}
finally {
    if (Test-Path -LiteralPath $FixtureRoot) {
        Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    }
}

Write-Output 'Puerts benchmark sidecar contracts passed: parser=1 commandlet=1 calibration_processes=1 timed_processes=5 fresh_pids=6 rotation=1 immutable=1 raw_samples=1200 process_stats=40 cross_process_stats=8 paired=6 descriptive_pool=8 mixed_rejections=4 fixed_iteration_rejections=2 fairness_rejections=6'
